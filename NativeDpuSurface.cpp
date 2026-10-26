// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NativeDpuSurface.h"
#include "NativeDpuScanout.h"

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/clock.h>
#include <kern/cpu_data.h>
#include <kern/task.h>
#include <pexpert/pexpert.h>

extern "C" vm_offset_t phystokv(uint64_t physical);
extern "C" uint64_t gPhysBase;
extern "C" uint64_t gPhysSize;
extern "C" void native_legacy_surface_increment_use(OSObject *surface);
extern "C" void native_legacy_surface_decrement_use(OSObject *surface);
extern "C" IOReturn native_legacy_surface_fence_complete(OSObject *fence, IOReturn status);

namespace {

constexpr uint64_t PageBytes = 4096;
constexpr uint64_t SectionBytes = 0x200000;
constexpr uint32_t SurfaceIova = 0x40000000;
constexpr uint32_t SurfaceBytes = 0x02000000;
constexpr unsigned SurfaceCount = 2;
constexpr unsigned SurfaceTablePages = SurfaceBytes / SectionBytes;
constexpr uint64_t PhysicalLimit = UINT64_C(1) << 36;
constexpr uint32_t MdpPhysical = 0x0ae00000;
constexpr uint32_t MdpBytes = 0x84000;
constexpr uint32_t SmmuPhysical = 0x15000000;
constexpr uint32_t SmmuBytes = 0x100000;
constexpr uint32_t Vig0 = 0x5000;
constexpr uint32_t Control = 0x16000;
constexpr uint32_t Interface = 0x36000;
constexpr uint32_t DisplayBank = 4;
constexpr uint32_t DisplayAsid = DisplayBank + 1;
constexpr uint32_t StreamSlot = 4;
constexpr uint32_t StreamMatch = 0x84020900;
constexpr uint32_t Context = 0x84000;
constexpr uint32_t GlobalPage = 0x1000;
constexpr uint32_t FaultMask = 0xc00001fe;
constexpr uint32_t ContextControl = 0x00c01127;
constexpr uint32_t TranslationControl = 0x80802020;
constexpr uint32_t TranslationControl2 = 0x00038011;
constexpr unsigned FlipMilliseconds = 80;
enum : unsigned { NoOperation, SurfaceOperation, FirmwareOperation };

// NativeDpuSurfaceBuffer owns the IOSurface and its wired descriptor for one
// scanout IOVA independently of the display family's completed swap request.
struct NativeDpuSurfaceBuffer {
    OSObject *surface = nullptr;
    OSObject *fence = nullptr; // An active read fence holds queued writers until this surface is replaced.
    IOMemoryDescriptor *memory = nullptr;
    uint64_t bytes = 0;
    uint32_t stride = 0;
    bool prepared = false;
    bool inUse = false; // The global IOSurface use count prevents compositor reuse during scanout.
};

// NativeDpuSurfaceState owns the SM7325 display context and its page tables.
// Initialization retains the firmware framebuffer's identity mapping, prepares
// two empty 32 MiB IOVA ranges, then enables only the inherited display CB4.
// The stream-match pair, display clocks, DSI timing, mixer and backlight remain
// owned by their existing components.
//
// A present wires the next IOSurface's pages, publishes read-only Normal NC
// leaves, and switches the inherited VIG0 address through CTL0. INTF1's frame
// counter and the cleared CTL flush word determine when the old buffer can be
// unmapped. The table allocation uses its normal kernel mapping and explicit
// cache cleaning; no CPU cache attribute of an IOSurface is changed.
struct NativeDpuSurfaceState {
    IOMemoryMap *mdp = nullptr;
    IOMemoryMap *smmu = nullptr;
    IOBufferMemoryDescriptor *tableMemory = nullptr;
    uint64_t *tables = nullptr;
    uint64_t tablePhysical = 0;
    uint64_t tableBytes = 0;
    NativeDpuSmmuContext savedContext = {};
    NativeDpuSurfaceBuffer buffers[SurfaceCount];
    uint32_t firmwareAddress = 0;
    uint32_t firmwareStride = 0;
    uint64_t firmwareBytes = 0;
    uint64_t sequence = 0;
    int scanning = -1;
    unsigned operation = NoOperation;
    unsigned pendingSlot = 0;
    uint32_t pendingAddress = 0;
    uint32_t pendingFrame = 0;
    uint64_t presentBegan = 0;
    uint64_t waitBegan = 0;
    uint64_t deadline = 0;
    bool disableOnRestore = false;
    bool initialized = false;
    bool failed = false;
    bool tablesPrepared = false;
    bool contextEnabled = false;
};

NativeDpuSurfaceState state;

// NativeDpuSurfaceTiming records the CPU stages and hardware frame transition
// of one present. Durations use mach_absolute_time and are logged in microseconds.
struct NativeDpuSurfaceTiming {
    uint64_t initialize = 0;
    uint64_t prepare = 0;
    uint64_t pages = 0;
    uint64_t clean = 0;
    uint64_t tables = 0;
    uint64_t wait = 0;
    uint64_t release = 0;
    uint64_t submit = 0;
    uint64_t poll = 0;
    uint64_t total = 0;
    uint32_t frameBefore = 0;
    uint32_t frameAfter = 0;
};

NativeDpuSurfaceTiming pendingTiming;

uint64_t microseconds(uint64_t ticks) {
    uint64_t nanoseconds = 0;
    absolutetime_to_nanoseconds(ticks, &nanoseconds);

    return nanoseconds / 1000;
}

bool logPresentTiming(uint64_t sequence) {
    static unsigned period = 120;
    static bool configured = false;
    if (!configured) {
        PE_parse_boot_argn("t733-dpu-timing", &period, sizeof(period));
        configured = true;
    }

    return sequence <= 4 || (period && !(sequence % period));
}

IOMemoryMap *mapRegisters(uint64_t physical, uint64_t bytes) {
    IODeviceMemory *memory = IODeviceMemory::withRange(physical, bytes);
    if (!memory) {
        return nullptr;
    }

    IOMemoryMap *mapping = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
    memory->release();

    return mapping;
}

uint32_t read32(IOMemoryMap *mapping, uint32_t offset) {
    const uint32_t value = *reinterpret_cast<volatile uint32_t *>(
        mapping->getVirtualAddress() + offset);
    __asm__ volatile("dsb ld" ::: "memory");

    return value;
}

uint64_t read64(IOMemoryMap *mapping, uint32_t offset) {
    const uint64_t value = *reinterpret_cast<volatile uint64_t *>(
        mapping->getVirtualAddress() + offset);
    __asm__ volatile("dsb ld" ::: "memory");

    return value;
}

void write32(IOMemoryMap *mapping, uint32_t offset, uint32_t value) {
    __asm__ volatile("dmb oshst" ::: "memory");
    *reinterpret_cast<volatile uint32_t *>(mapping->getVirtualAddress() + offset) = value;
}

void write64(IOMemoryMap *mapping, uint32_t offset, uint64_t value) {
    __asm__ volatile("dmb oshst" ::: "memory");
    *reinterpret_cast<volatile uint64_t *>(mapping->getVirtualAddress() + offset) = value;
}

// The caller supplies a resident, writable Normal kernel mapping. Page-table
// allocations are touched before this call; IOSurface pages use phystokv.
IOReturn cleanNormalMemory(void *mapping, uint64_t bytes) {
    if (!mapping || !bytes || bytes > UINTPTR_MAX - reinterpret_cast<uintptr_t>(mapping)) {
        return kIOReturnBadArgument;
    }

    uintptr_t address = reinterpret_cast<uintptr_t>(mapping);
    const uintptr_t end = address + bytes;
    while (address < end) {
        disable_preemption();
        uint64_t ctr = 0;
        __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
        const uintptr_t line = static_cast<uintptr_t>(4) << ((ctr >> 16) & 15);
        if (line > PageBytes) {
            enable_preemption();
            return kIOReturnUnsupported;
        }

        uintptr_t limit = address + 64 * PageBytes;
        if (limit > end) {
            limit = end;
        }
        address &= ~(line - 1);
        __asm__ volatile("dsb sy" ::: "memory");
        while (address < limit) {
            __asm__ volatile("dc cvac, %0" :: "r"(address) : "memory");
            address += line;
        }
        __asm__ volatile("dsb sy" ::: "memory");
        enable_preemption();
    }

    return kIOReturnSuccess;
}

IOReturn cleanSurfacePages(IOMemoryDescriptor *memory, uint64_t bytes) {
    uint64_t offset = 0;
    uint64_t physical = 0;
    uint64_t segmentBytes = 0;
    while (offset < bytes) {
        uint64_t pages[64];
        unsigned count = 0;
        while (count < 64 && offset < bytes) {
            if (!segmentBytes) {
                IOByteCount length = 0;
                physical = memory->getPhysicalSegment(offset, &length, kIOMemoryMapperNone);
                segmentBytes = length;
                if (segmentBytes > bytes - offset) {
                    segmentBytes = bytes - offset;
                }
                segmentBytes &= ~(PageBytes - 1);
                if (!segmentBytes || (physical & (PageBytes - 1)) || physical < gPhysBase ||
                    physical - gPhysBase >= gPhysSize ||
                    segmentBytes > gPhysSize - (physical - gPhysBase)) {
                    return kIOReturnUnsupported;
                }
            }

            pages[count++] = physical;
            physical += PageBytes;
            segmentBytes -= PageBytes;
            offset += PageBytes;
        }

        disable_preemption();
        uint64_t ctr = 0;
        __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
        const uintptr_t line = static_cast<uintptr_t>(4) << ((ctr >> 16) & 15);
        if (line > PageBytes) {
            enable_preemption();
            return kIOReturnUnsupported;
        }

        // The prepared descriptor owns these RAM pages. The permanent kernel
        // aliases retain writable Normal PTEs across scattered physical spans.
        __asm__ volatile("dsb sy" ::: "memory");
        for (unsigned page = 0; page < count; ++page) {
            const vm_offset_t alias = phystokv(pages[page]);
            if (!alias) {
                __asm__ volatile("dsb sy" ::: "memory");
                enable_preemption();
                return kIOReturnUnsupported;
            }

            for (uintptr_t address = alias, end = alias + PageBytes; address < end; address += line) {
                __asm__ volatile("dc cvac, %0" :: "r"(address) : "memory");
            }
        }
        __asm__ volatile("dsb sy" ::: "memory");
        enable_preemption();
    }

    return kIOReturnSuccess;
}

uint64_t *tablePage(unsigned index) {
    return state.tables + index * (PageBytes / sizeof(uint64_t));
}

uint64_t tableDescriptor(unsigned index) {
    return state.tablePhysical + index * PageBytes + 3;
}

uint64_t pageDescriptor(uint64_t physical) {
    // AF, outer shareability, read-only access, PXN and UXN; MAIR index 0 is NC.
    return physical | UINT64_C(0x00600000000006c3);
}

uint64_t *surfaceLeaves(unsigned slot) {
    return tablePage(3 + slot * SurfaceTablePages);
}

IOReturn synchronizeContext() {
    __asm__ volatile("dsb sy" ::: "memory");
    write32(state.smmu, Context + 0x618, 0);
    write32(state.smmu, Context + 0x7f0, UINT32_MAX);
    __asm__ volatile("dsb sy" ::: "memory");

    for (unsigned elapsed = 0; elapsed < 2000; ++elapsed) {
        if (!(read32(state.smmu, Context + 0x7f4) & 1)) {
            __asm__ volatile("dsb sy" ::: "memory");
            return kIOReturnSuccess;
        }
        IODelay(1);
    }

    return kIOReturnTimeout;
}

IOReturn releaseBuffer(NativeDpuSurfaceBuffer &buffer) {
    if (buffer.prepared) {
        const IOReturn result = buffer.memory->complete(kIODirectionOut);
        if (result != kIOReturnSuccess) {
            return result;
        }
        buffer.prepared = false;
    }
    if (buffer.fence) {
        const IOReturn result = native_legacy_surface_fence_complete(buffer.fence, kIOReturnSuccess);
        if (result != kIOReturnSuccess) {
            return result;
        }
        OSSafeReleaseNULL(buffer.fence);
    }
    if (buffer.inUse) {
        native_legacy_surface_decrement_use(buffer.surface);
        buffer.inUse = false;
    }

    OSSafeReleaseNULL(buffer.memory);
    OSSafeReleaseNULL(buffer.surface);
    buffer = {};

    return kIOReturnSuccess;
}

void releaseInactiveTables() {
    if (state.contextEnabled) {
        return;
    }

    if (state.tablesPrepared) {
        const IOReturn result = state.tableMemory->complete();
        if (result != kIOReturnSuccess) {
            state.failed = true;
            IOLog("T733 DPU surface: table completion failed status=%x\n", result);
            return;
        }
    }
    OSSafeReleaseNULL(state.tableMemory);
    OSSafeReleaseNULL(state.smmu);
    OSSafeReleaseNULL(state.mdp);
    state.tables = nullptr;
    state.tablesPrepared = false;
}

IOReturn restoreBypassContext() {
    if (!state.smmu || !state.tableMemory) {
        return kIOReturnSuccess;
    }

    write32(state.smmu, Context, state.savedContext.sctlr);
    __asm__ volatile("dsb sy" ::: "memory");
    if (read32(state.smmu, Context) & 1) {
        return kIOReturnIOError;
    }

    const IOReturn result = synchronizeContext();
    if (result != kIOReturnSuccess) {
        return result;
    }

    write32(state.smmu, GlobalPage + 4 * DisplayBank, state.savedContext.cbar);
    write32(state.smmu, GlobalPage + 0x800 + 4 * DisplayBank, state.savedContext.cba2r);
    write32(state.smmu, Context + 0x10, state.savedContext.tcr2);
    write32(state.smmu, Context + 0x30, state.savedContext.tcr);
    write64(state.smmu, Context + 0x20, state.savedContext.ttbr0);
    write64(state.smmu, Context + 0x28, state.savedContext.ttbr1);
    write32(state.smmu, Context + 0x38, state.savedContext.mair0);
    write32(state.smmu, Context + 0x3c, state.savedContext.mair1);
    write32(state.smmu, Context + 0x04, state.savedContext.actlr);
    write32(state.smmu, Context + 0x58, FaultMask);
    __asm__ volatile("dsb sy" ::: "memory");
    state.contextEnabled = false;

    return kIOReturnSuccess;
}

IOReturn allocateTables() {
    const uint64_t firmwareTablePages =
        (state.firmwareBytes + SectionBytes - 1) / SectionBytes;
    const uint64_t tablePages = 3 + SurfaceCount * SurfaceTablePages + firmwareTablePages;
    state.tableBytes = tablePages * PageBytes;
    state.tableMemory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        static_cast<IOOptionBits>(kIODirectionInOut) |
        static_cast<IOOptionBits>(kIOMemoryMapperNone) | kIOMemoryPhysicallyContiguous,
        state.tableBytes, (PhysicalLimit - 1) & ~(PageBytes - 1));
    if (!state.tableMemory) {
        return kIOReturnNoMemory;
    }

    IOReturn result = state.tableMemory->prepare();
    if (result != kIOReturnSuccess) {
        return result;
    }
    state.tablesPrepared = true;
    state.tables = static_cast<uint64_t *>(state.tableMemory->getBytesNoCopy());
    IOByteCount length = 0;
    state.tablePhysical = state.tableMemory->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
    if (!state.tables || !state.tablePhysical || length < state.tableBytes ||
        (state.tablePhysical & (PageBytes - 1)) ||
        state.tablePhysical >= PhysicalLimit || state.tableBytes > PhysicalLimit - state.tablePhysical) {
        return kIOReturnUnsupported;
    }

    bzero(state.tables, state.tableBytes);
    tablePage(0)[SurfaceIova >> 30] = tableDescriptor(1);
    tablePage(0)[state.firmwareAddress >> 30] = tableDescriptor(2);

    for (unsigned slot = 0; slot < SurfaceCount; ++slot) {
        const uint32_t iova = SurfaceIova + slot * SurfaceBytes;
        for (unsigned section = 0; section < SurfaceTablePages; ++section) {
            tablePage(1)[((iova >> 21) & 511) + section] =
                tableDescriptor(3 + slot * SurfaceTablePages + section);
        }
    }

    const unsigned firmwareTable = 3 + SurfaceCount * SurfaceTablePages;
    for (unsigned section = 0; section < firmwareTablePages; ++section) {
        tablePage(2)[((state.firmwareAddress >> 21) & 511) + section] =
            tableDescriptor(firmwareTable + section);
    }

    uint64_t *firmwareLeaves = tablePage(firmwareTable);
    for (uint64_t offset = 0; offset < state.firmwareBytes; offset += PageBytes) {
        firmwareLeaves[offset / PageBytes] = pageDescriptor(state.firmwareAddress + offset);
    }

    return cleanNormalMemory(state.tables, state.tableBytes);
}

IOReturn initialize() {
    if (state.initialized) {
        return kIOReturnSuccess;
    }
    if (state.failed) {
        return kIOReturnNotResponding;
    }

    const NativeDpuSnapshot *captured = t733_dpu_capture_snapshot();
    if (!captured || captured->hardwareVersion != 0x70020000 || captured->firmwarePipeMask != 1 ||
        captured->firmwareWidth != 1600 || captured->firmwareHeight != 2560 ||
        captured->firmwarePhysical != 0xe1000000 || captured->firmwareStride != 6400 ||
        captured->contextCount != 1 || captured->contexts[0].bank != DisplayBank ||
        captured->contexts[0].offset != Context || (captured->contexts[0].sctlr & 1) ||
        captured->contexts[0].ttbr0 || captured->contexts[0].ttbr1 ||
        captured->streams[StreamSlot].smr != StreamMatch ||
        captured->streams[StreamSlot].s2cr != DisplayBank) {
        return kIOReturnUnsupported;
    }

    state.firmwareAddress = static_cast<uint32_t>(captured->firmwarePhysical);
    state.firmwareStride = captured->firmwareStride;
    state.firmwareBytes = captured->firmwareBytes;
    state.savedContext = captured->contexts[0];
    state.mdp = mapRegisters(MdpPhysical, MdpBytes);
    state.smmu = mapRegisters(SmmuPhysical, SmmuBytes);
    if (!state.mdp || !state.smmu) {
        releaseInactiveTables();
        return kIOReturnNoMemory;
    }

    if (read32(state.smmu, 0x800 + 4 * StreamSlot) != StreamMatch ||
        read32(state.smmu, 0xc00 + 4 * StreamSlot) != DisplayBank ||
        (read32(state.smmu, Context) & 1) ||
        read32(state.mdp, Vig0 + 0x14) != state.firmwareAddress ||
        read32(state.mdp, Vig0 + 0x30) != 0x000237ff ||
        read32(state.mdp, Vig0 + 0x34) != 0x03020001 ||
        read32(state.mdp, Control + 0xfc) != 0x00010000 ||
        read32(state.mdp, Control + 0xf4) != 2) {
        releaseInactiveTables();
        return kIOReturnUnsupported;
    }

    IOReturn result = allocateTables();
    if (result != kIOReturnSuccess) {
        releaseInactiveTables();
        return result;
    }

    // The early-map handoff enables translation while the identity-mapped
    // firmware framebuffer remains the current VIG0 source.
    IOLog("T733 DPU surface: enabling CB4 tables=%llx bytes=%llu firmware=%x\n",
        state.tablePhysical, state.tableBytes, state.firmwareAddress);
    write32(state.smmu, GlobalPage + 4 * DisplayBank, 0x0001f300);
    write32(state.smmu, GlobalPage + 0x800 + 4 * DisplayBank, 1);
    write32(state.smmu, Context + 0x10, TranslationControl2);
    write32(state.smmu, Context + 0x30, TranslationControl);
    write64(state.smmu, Context + 0x20,
        state.tablePhysical | (static_cast<uint64_t>(DisplayAsid) << 48));
    write64(state.smmu, Context + 0x28, static_cast<uint64_t>(DisplayAsid) << 48);
    write32(state.smmu, Context + 0x38, 0x0000ff44);
    write32(state.smmu, Context + 0x3c, 0);
    write32(state.smmu, Context + 0x04, 0x00000103);
    write32(state.smmu, Context + 0x58, FaultMask);

    result = synchronizeContext();
    if (result != kIOReturnSuccess) {
        state.failed = true;
        return result;
    }

    write32(state.smmu, Context, ContextControl);
    __asm__ volatile("dsb sy" ::: "memory");
    state.contextEnabled = true;
    const uint32_t control = read32(state.smmu, Context);
    if ((control & ~0x1000U) != (ContextControl & ~0x1000U) ||
        read64(state.smmu, Context + 0x20) !=
            (state.tablePhysical | (static_cast<uint64_t>(DisplayAsid) << 48))) {
        state.failed = true;
        return kIOReturnIOError;
    }

    state.initialized = true;
    IOLog("T733 DPU surface: ready CB4 sctlr=%08x tcr=%08x ttbr0=%016llx actlr=%08x\n",
        control, read32(state.smmu, Context + 0x30), read64(state.smmu, Context + 0x20),
        read32(state.smmu, Context + 4));

    return kIOReturnSuccess;
}

IOReturn mapBuffer(unsigned slot, OSObject *surface, IOMemoryDescriptor *memory,
    uint64_t stride, uint32_t height, NativeDpuSurfaceTiming &timing) {
    const uint64_t began = mach_absolute_time();
    const uint64_t bytes = stride * height;
    const uint64_t mappedBytes = (bytes + PageBytes - 1) & ~(PageBytes - 1);
    if (state.buffers[slot].memory || !surface || !memory || !bytes || mappedBytes > SurfaceBytes ||
        memory->getLength() < mappedBytes || stride > UINT16_MAX) {
        return kIOReturnBadArgument;
    }

    NativeDpuSurfaceBuffer &buffer = state.buffers[slot];
    surface->retain();
    memory->retain();
    buffer.surface = surface;
    buffer.memory = memory;
    buffer.bytes = mappedBytes;
    buffer.stride = static_cast<uint32_t>(stride);

    IOReturn result = memory->prepare(kIODirectionOut);
    if (result != kIOReturnSuccess) {
        releaseBuffer(buffer);
        return result;
    }
    buffer.prepared = true;
    native_legacy_surface_increment_use(surface);
    buffer.inUse = true;
    const uint64_t prepared = mach_absolute_time();
    timing.prepare = prepared - began;

    uint64_t *leaves = surfaceLeaves(slot);
    for (uint64_t offset = 0; offset < mappedBytes; offset += PageBytes) {
        IOByteCount length = 0;
        const uint64_t physical = memory->getPhysicalSegment(offset, &length, kIOMemoryMapperNone);
        if (!physical || !length || (physical & (PageBytes - 1)) || physical >= PhysicalLimit ||
            leaves[offset / PageBytes]) {
            bzero(leaves, SurfaceTablePages * PageBytes);
            releaseBuffer(buffer);
            return kIOReturnUnsupported;
        }
        leaves[offset / PageBytes] = pageDescriptor(physical);
    }
    const uint64_t pages = mach_absolute_time();
    timing.pages = pages - prepared;

    result = cleanSurfacePages(memory, mappedBytes);
    if (result != kIOReturnSuccess) {
        bzero(leaves, SurfaceTablePages * PageBytes);
        releaseBuffer(buffer);
        return result;
    }
    const uint64_t cleaned = mach_absolute_time();
    timing.clean = cleaned - pages;

    result = cleanNormalMemory(leaves, SurfaceTablePages * PageBytes);
    if (result == kIOReturnSuccess) {
        result = synchronizeContext();
    }
    timing.tables = mach_absolute_time() - cleaned;

    return result;
}

IOReturn unmapBuffer(unsigned slot) {
    NativeDpuSurfaceBuffer &buffer = state.buffers[slot];
    if (!buffer.memory) {
        return kIOReturnSuccess;
    }

    uint64_t *leaves = surfaceLeaves(slot);
    bzero(leaves, SurfaceTablePages * PageBytes);
    IOReturn result = cleanNormalMemory(leaves, SurfaceTablePages * PageBytes);
    if (result == kIOReturnSuccess) {
        result = synchronizeContext();
    }
    if (result != kIOReturnSuccess) {
        return result;
    }

    return releaseBuffer(buffer);
}

void submitAddress(uint32_t address, uint32_t stride) {
    write32(state.mdp, Vig0 + 0x24, stride);
    write32(state.mdp, Vig0 + 0x14, address);
    __asm__ volatile("dsb sy" ::: "memory");
    write32(state.mdp, Control + 0x18, 1);
    __asm__ volatile("dsb sy" ::: "memory");
    state.pendingAddress = address;
    state.pendingFrame = read32(state.mdp, Interface + 0xac);
    state.waitBegan = mach_absolute_time();
    clock_interval_to_deadline(FlipMilliseconds, kMillisecondScale, &state.deadline);
}

}

extern "C" bool t733_dpu_surface_enabled(void) {
    unsigned enabled = 0;
    PE_parse_boot_argn("t733-dpu-scanout", &enabled, sizeof(enabled));

    return enabled == 1;
}

extern "C" bool t733_dpu_surface_active(void) {
    return state.scanning >= 0;
}

extern "C" bool t733_dpu_surface_pending(void) {
    return state.operation != NoOperation;
}

extern "C" bool t733_dpu_surface_timing_enabled(void) {
    if (state.mdp) {
        return (read32(state.mdp, Interface) & 1) != 0;
    }

    IOMemoryMap *mapping = mapRegisters(MdpPhysical + Interface, PageBytes);
    if (!mapping) {
        return false;
    }
    const bool enabled = (read32(mapping, 0) & 1) != 0;
    mapping->release();

    return enabled;
}

extern "C" IOReturn t733_dpu_surface_begin(OSObject *surface,
    IOMemoryDescriptor *memory, uint64_t stride, uint32_t width, uint32_t height,
    OSObject *fence) {
    if (!t733_dpu_surface_enabled()) {
        return kIOReturnUnsupported;
    }
    if (state.operation != NoOperation) {
        return kIOReturnBusy;
    }
    if (width != 1600 || height != 2560 || stride < static_cast<uint64_t>(width) * 4 ||
        stride > SurfaceBytes / height || state.failed) {
        return state.failed ? kIOReturnNotResponding : kIOReturnBadArgument;
    }

    pendingTiming = {};
    state.presentBegan = mach_absolute_time();
    IOReturn result = initialize();
    if (result != kIOReturnSuccess) {
        return result;
    }
    pendingTiming.initialize = mach_absolute_time() - state.presentBegan;

    const unsigned next = state.scanning == 0 ? 1 : 0;
    result = mapBuffer(next, surface, memory, stride, height, pendingTiming);
    if (result != kIOReturnSuccess) {
        if (state.buffers[next].memory) {
            state.failed = true;
        }
        return result;
    }

    NativeDpuSurfaceBuffer &buffer = state.buffers[next];
    buffer.fence = fence;
    if (fence && buffer.inUse) {
        native_legacy_surface_decrement_use(buffer.surface);
        buffer.inUse = false;
    }

    state.pendingSlot = next;
    state.operation = SurfaceOperation;
    submitAddress(SurfaceIova + next * SurfaceBytes, static_cast<uint32_t>(stride));
    pendingTiming.frameBefore = state.pendingFrame;
    pendingTiming.submit = mach_absolute_time() - state.presentBegan;

    return kIOReturnSuccess;
}

extern "C" IOReturn t733_dpu_surface_begin_restore(bool disableContext) {
    if (!state.initialized) {
        const IOReturn result = restoreBypassContext();
        if (result == kIOReturnSuccess) {
            releaseInactiveTables();
        }

        return result;
    }

    state.operation = FirmwareOperation;
    state.disableOnRestore = disableContext;
    submitAddress(state.firmwareAddress, state.firmwareStride);

    return kIOReturnSuccess;
}

extern "C" IOReturn t733_dpu_surface_poll(void) {
    if (state.operation == NoOperation) {
        return kIOReturnSuccess;
    }

    const uint64_t began = mach_absolute_time();
    const uint32_t flush = read32(state.mdp, Control + 0x18);
    const uint32_t frame = read32(state.mdp, Interface + 0xac);
    const uint32_t fault = read32(state.smmu, Context + 0x58) & FaultMask;
    if (state.operation == SurfaceOperation && fault) {
        IOLog("T733 DPU surface: fault=%08x far=%016llx syn=%08x addr=%x\n",
            fault, read64(state.smmu, Context + 0x60),
            read32(state.smmu, Context + 0x68), state.pendingAddress);
        state.failed = true;
        return kIOReturnVMError;
    }
    if (flush || frame == state.pendingFrame) {
        pendingTiming.poll += mach_absolute_time() - began;
        if (mach_absolute_time() < state.deadline) {
            return kIOReturnNotReady;
        }

        IOLog("T733 DPU surface: flip timeout addr=%x flush=%08x before=%u frame=%u line=%u\n",
            state.pendingAddress, flush, state.pendingFrame, frame,
            read32(state.mdp, Interface + 0xb0));
        state.failed = true;
        return kIOReturnTimeout;
    }

    if (state.operation == SurfaceOperation) {
        pendingTiming.wait = began - state.waitBegan;
        pendingTiming.frameAfter = frame;
        const int previous = state.scanning;
        state.scanning = static_cast<int>(state.pendingSlot);
        state.operation = NoOperation;
        if (previous >= 0) {
            const IOReturn result = unmapBuffer(static_cast<unsigned>(previous));
            if (result != kIOReturnSuccess) {
                state.failed = true;
                return result;
            }
        }
        pendingTiming.release = mach_absolute_time() - began;
        pendingTiming.total = mach_absolute_time() - state.presentBegan;
        pendingTiming.poll += mach_absolute_time() - began;

        ++state.sequence;
        if (logPresentTiming(state.sequence)) {
            IOLog("T733 DPU surface: flip=%llu iova=%x stride=%u frame=%u previous=%d asynchronous=1\n",
                state.sequence, state.pendingAddress, state.buffers[state.scanning].stride, frame, previous);
            IOLog("T733 DPU surface: us total=%llu submit=%llu poll=%llu init=%llu prepare=%llu pages=%llu clean=%llu tables=%llu wait=%llu release=%llu frames=%u,%u\n",
                microseconds(pendingTiming.total), microseconds(pendingTiming.submit), microseconds(pendingTiming.poll),
                microseconds(pendingTiming.initialize), microseconds(pendingTiming.prepare), microseconds(pendingTiming.pages),
                microseconds(pendingTiming.clean), microseconds(pendingTiming.tables), microseconds(pendingTiming.wait),
                microseconds(pendingTiming.release), pendingTiming.frameBefore, pendingTiming.frameAfter);
        }

        return kIOReturnSuccess;
    }

    if (state.disableOnRestore) {
        const IOReturn result = restoreBypassContext();
        if (result != kIOReturnSuccess) {
            state.failed = true;
            return result;
        }
    }

    state.scanning = -1;
    state.operation = NoOperation;
    for (unsigned slot = 0; slot < SurfaceCount; ++slot) {
        const IOReturn result = state.disableOnRestore ? releaseBuffer(state.buffers[slot]) : unmapBuffer(slot);
        if (result != kIOReturnSuccess) {
            state.failed = true;
            return result;
        }
    }
    IOLog("T733 DPU surface: firmware restored addr=%x frame=%u\n",
        state.firmwareAddress, read32(state.mdp, Interface + 0xac));

    if (state.disableOnRestore) {
        state.initialized = false;
        releaseInactiveTables();
    }

    return kIOReturnSuccess;
}
