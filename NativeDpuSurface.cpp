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
constexpr unsigned SurfaceCount = 4;
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

// NativeDpuSurfaceBuffer retains an IOSurface, its prepared descriptor and its
// page-table entries while that surface occupies one cached scanout IOVA.
// The active read fence or use count covers only its current presentation;
// replacement ends that read while the mapping remains available for reuse.
struct NativeDpuSurfaceBuffer {
    OSObject *surface = nullptr;
    OSObject *fence = nullptr; // An active read fence holds queued writers until this surface is replaced.
    IOMemoryDescriptor *memory = nullptr;
    uint64_t bytes = 0;
    uint64_t lastUsed = 0;
    uint32_t stride = 0;
    bool prepared = false;
    bool inUse = false; // The global IOSurface use count prevents compositor reuse during scanout.
};

// NativeDpuSurfaceState owns the SM7325 display context and its page tables.
// Initialization retains the firmware framebuffer's identity mapping, prepares
// four empty 32 MiB IOVA ranges, then enables only the inherited display CB4.
// The stream-match pair, display clocks, DSI timing, mixer and backlight remain
// owned by their existing components.
//
// A present reuses an inactive surface mapping or replaces the least recently
// used inactive entry. New entries wire the descriptor and publish read-only
// Normal NC leaves. Every present cleans the surface pages before switching
// VIG0 through CTL0. INTF1's frame counter and the cleared CTL flush word end
// the previous surface's read fence. Mappings survive until cache eviction or
// firmware restoration. The display family owns producer waiting and request
// completion; this state owns the retained mappings and scanout read fences.
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
    uint64_t cacheClock = 0;
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
    uint64_t producerWait = 0;
    uint32_t producerFence = 0;
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

enum DisplayProfileStage : unsigned {
    DisplayMap,
    DisplayUnmap,
    DisplayCache,
    DisplayProducer,
    DisplaySubmit,
    DisplayWait,
    DisplayRelease,
    DisplayTotal,
    DisplayStageCount
};

// DisplayStageTotals accumulates elapsed mach time for one display operation.
// Explicit profile reads convert totals and maxima to microseconds.
struct DisplayStageTotals {
    uint64_t count = 0;
    uint64_t total = 0;
    uint64_t maximum = 0;

    void add(uint64_t ticks) {
        ++count;
        total += ticks;
        if (ticks > maximum) {
            maximum = ticks;
        }
    }
};

// DisplayProfileTotals records completed presentations and mapping operations.
// The display work loop updates these counters under displayProfileLock; an
// explicit USB request copies them once and prints a fixed number of rows.
struct DisplayProfileTotals {
    uint64_t records = 0;
    uint64_t sequence = 0;
    uint32_t producerFence = 0;
    uint64_t cacheHits = 0;
    uint64_t cacheMisses = 0;
    uint64_t evictions = 0;
    DisplayStageTotals stages[DisplayStageCount];
};

DisplayProfileTotals displayProfile;
IOLock *displayProfileLock = nullptr;
uint64_t displayProfileSnapshot = 0;

void initializeDisplayProfile() {
    if (__atomic_load_n(&displayProfileLock, __ATOMIC_ACQUIRE)) {
        return;
    }

    IOLock *lock = IOLockAlloc();
    __atomic_store_n(&displayProfileLock, lock, __ATOMIC_RELEASE);
}

void recordDisplayProfile(uint64_t sequence, const NativeDpuSurfaceTiming &timing) {
    IOLock *lock = __atomic_load_n(&displayProfileLock, __ATOMIC_ACQUIRE);
    if (!lock) {
        return;
    }

    IOLockLock(lock);
    ++displayProfile.records;
    displayProfile.sequence = sequence;
    displayProfile.producerFence = timing.producerFence;
    displayProfile.stages[DisplayProducer].add(timing.producerWait);
    displayProfile.stages[DisplaySubmit].add(timing.submit);
    displayProfile.stages[DisplayWait].add(timing.wait);
    displayProfile.stages[DisplayRelease].add(timing.release);
    displayProfile.stages[DisplayTotal].add(timing.total);
    IOLockUnlock(lock);
}

void recordCacheChoice(bool hit, bool eviction) {
    IOLock *lock = __atomic_load_n(&displayProfileLock, __ATOMIC_ACQUIRE);
    if (!lock) {
        return;
    }

    IOLockLock(lock);
    if (hit) {
        ++displayProfile.cacheHits;
    } else {
        ++displayProfile.cacheMisses;
    }
    if (eviction) {
        ++displayProfile.evictions;
    }
    IOLockUnlock(lock);
}

// DisplayStageDuration records every attempted map, unmap or surface clean,
// including an operation that returns an error before completing its work.
class DisplayStageDuration {
    DisplayProfileStage stage;
    uint64_t began;

public:
    explicit DisplayStageDuration(DisplayProfileStage value)
        : stage(value), began(mach_absolute_time()) {
    }

    ~DisplayStageDuration() {
        const uint64_t elapsed = mach_absolute_time() - began;
        IOLock *lock = __atomic_load_n(&displayProfileLock, __ATOMIC_ACQUIRE);
        if (!lock) {
            return;
        }

        IOLockLock(lock);
        displayProfile.stages[stage].add(elapsed);
        IOLockUnlock(lock);
    }
};

uint64_t microseconds(uint64_t ticks) {
    uint64_t nanoseconds = 0;
    absolutetime_to_nanoseconds(ticks, &nanoseconds);

    return nanoseconds / 1000;
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
    DisplayStageDuration duration(DisplayCache);
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

IOReturn releaseScanoutRead(NativeDpuSurfaceBuffer &buffer) {
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

    return kIOReturnSuccess;
}

IOReturn releaseBuffer(NativeDpuSurfaceBuffer &buffer) {
    if (buffer.prepared) {
        const IOReturn result = buffer.memory->complete(kIODirectionOut);
        if (result != kIOReturnSuccess) {
            return result;
        }
        buffer.prepared = false;
    }

    const IOReturn result = releaseScanoutRead(buffer);
    if (result != kIOReturnSuccess) {
        return result;
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
    DisplayStageDuration duration(DisplayMap);
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

    result = cleanNormalMemory(leaves, SurfaceTablePages * PageBytes);
    if (result == kIOReturnSuccess) {
        result = synchronizeContext();
    }
    timing.tables = mach_absolute_time() - pages;

    return result;
}

IOReturn unmapBuffer(unsigned slot) {
    NativeDpuSurfaceBuffer &buffer = state.buffers[slot];
    if (!buffer.memory) {
        return kIOReturnSuccess;
    }

    DisplayStageDuration duration(DisplayUnmap);
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

// An inactive mapping can be reused without preparing the descriptor again.
// Eviction keeps the current and pending scanout addresses mapped until their
// hardware switch completes; a retained read fence also prevents eviction.
IOReturn selectBuffer(OSObject *surface, IOMemoryDescriptor *memory,
    uint64_t stride, uint32_t height, NativeDpuSurfaceTiming &timing, unsigned *selected) {
    const uint64_t bytes = stride * height;
    const uint64_t mappedBytes = (bytes + PageBytes - 1) & ~(PageBytes - 1);
    int candidate = -1;
    for (unsigned slot = 0; slot < SurfaceCount; ++slot) {
        if (static_cast<int>(slot) == state.scanning ||
            (state.operation == SurfaceOperation && slot == state.pendingSlot)) {
            continue;
        }

        NativeDpuSurfaceBuffer &buffer = state.buffers[slot];
        if (buffer.fence || buffer.inUse) {
            continue;
        }
        if (buffer.prepared && buffer.surface == surface && buffer.memory == memory &&
            buffer.bytes == mappedBytes && buffer.stride == stride) {
            buffer.lastUsed = ++state.cacheClock;
            *selected = slot;
            recordCacheChoice(true, false);

            return kIOReturnSuccess;
        }
        if (candidate < 0 || !buffer.memory ||
            (state.buffers[candidate].memory && buffer.lastUsed < state.buffers[candidate].lastUsed)) {
            candidate = static_cast<int>(slot);
        }
    }

    if (candidate < 0) {
        recordCacheChoice(false, false);
        return kIOReturnNoResources;
    }

    const unsigned slot = static_cast<unsigned>(candidate);
    const bool eviction = state.buffers[slot].memory != nullptr;
    recordCacheChoice(false, eviction);
    if (eviction) {
        const IOReturn result = unmapBuffer(slot);
        if (result != kIOReturnSuccess) {
            state.failed = true;
            return result;
        }
    }

    const IOReturn result = mapBuffer(slot, surface, memory, stride, height, timing);
    if (result != kIOReturnSuccess) {
        if (state.buffers[slot].memory) {
            state.failed = true;
        }
        return result;
    }

    state.buffers[slot].lastUsed = ++state.cacheClock;
    *selected = slot;

    return kIOReturnSuccess;
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

extern "C" void native_display_profile_dump(void) {
    IOLock *lock = __atomic_load_n(&displayProfileLock, __ATOMIC_ACQUIRE);
    if (!lock) {
        return;
    }

    IOLockLock(lock);
    const uint64_t snapshot = ++displayProfileSnapshot;
    const DisplayProfileTotals totals = displayProfile;
    IOLockUnlock(lock);

    IOLog("NativeAdrenoProfile: kind=display-summary snapshot=%llu records=%llu lost=0 cache_hits=%llu cache_misses=%llu evictions=%llu seq=%llu fence=%u\n",
        snapshot, totals.records, totals.cacheHits, totals.cacheMisses, totals.evictions,
        totals.sequence, totals.producerFence);

    static const char *const names[DisplayStageCount] = {
        "map", "unmap", "cache", "producer", "submit", "wait", "release", "total"
    };
    for (unsigned stage = 0; stage < DisplayStageCount; ++stage) {
        const DisplayStageTotals &duration = totals.stages[stage];
        IOLog("NativeAdrenoProfile: kind=display-stage snapshot=%llu stage=%s count=%llu total_us=%llu max_us=%llu\n",
            snapshot, names[stage], duration.count, microseconds(duration.total),
            microseconds(duration.maximum));
    }
}

extern "C" bool t733_dpu_surface_pending(void) {
    return state.operation != NoOperation;
}

extern "C" IOReturn t733_dpu_surface_frame(uint32_t *frame) {
    if (!frame) {
        return kIOReturnBadArgument;
    }
    *frame = 0;
    if (!state.mdp) {
        return kIOReturnNotReady;
    }
    if (!(read32(state.mdp, Interface) & 1)) {
        return kIOReturnOffline;
    }
    *frame = read32(state.mdp, Interface + 0xac);
    return kIOReturnSuccess;
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
    OSObject *fence, uint32_t producerFence, uint64_t producerWaitTicks) {
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

    initializeDisplayProfile();
    pendingTiming = {};
    pendingTiming.producerFence = producerFence;
    pendingTiming.producerWait = producerWaitTicks;
    state.presentBegan = mach_absolute_time();
    IOReturn result = initialize();
    if (result != kIOReturnSuccess) {
        return result;
    }
    pendingTiming.initialize = mach_absolute_time() - state.presentBegan;

    unsigned next = 0;
    result = selectBuffer(surface, memory, stride, height, pendingTiming, &next);
    if (result != kIOReturnSuccess) {
        return result;
    }

    NativeDpuSurfaceBuffer &buffer = state.buffers[next];
    if (!fence) {
        native_legacy_surface_increment_use(buffer.surface);
        buffer.inUse = true;
    }

    const uint64_t cleanBegan = mach_absolute_time();
    result = cleanSurfacePages(memory, buffer.bytes);
    pendingTiming.clean = mach_absolute_time() - cleanBegan;
    if (result != kIOReturnSuccess) {
        return result;
    }

    buffer.fence = fence;
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
            const IOReturn result = releaseScanoutRead(state.buffers[previous]);
            if (result != kIOReturnSuccess) {
                state.failed = true;
                return result;
            }
        }
        pendingTiming.release = mach_absolute_time() - began;
        pendingTiming.total = mach_absolute_time() - state.presentBegan;
        pendingTiming.poll += mach_absolute_time() - began;

        ++state.sequence;
        recordDisplayProfile(state.sequence, pendingTiming);

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
