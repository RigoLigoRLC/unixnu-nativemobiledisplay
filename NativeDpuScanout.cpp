// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NativeDpuScanout.h"

#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>

namespace {

constexpr uint64_t MdpPhysical = 0x0ae00000;
constexpr uint64_t MdpBytes = 0x84000;
constexpr uint64_t AppsSmmuPhysical = 0x15000000;
constexpr uint64_t AppsSmmuBytes = 0x100000;
constexpr uint32_t InterfaceBase = 0x36000;
constexpr uint32_t PipeBases[NativeDpuPipeCount] = {
    0x5000, 0x25000, 0x27000, 0x29000
};
constexpr uint32_t ControlBases[NativeDpuControlCount] = {
    0x16000, 0x17000, 0x18000, 0x19000
};
constexpr uint32_t MixerBases[NativeDpuMixerCount] = {
    0x45000, 0x47000, 0x48000
};
constexpr uint16_t DisplayStreams[] = {0x900, 0x902, 0xd00, 0xd02};

// These offsets follow the SM7325 stock sde_hw_sspp, sde_hw_ctl,
// sde_hw_lm and sde_hw_intf definitions and yupik-sde-common.dtsi.
constexpr uint32_t PipeOffsets[] = {
    0x000, 0x008, 0x00c, 0x010, 0x014, 0x018, 0x01c, 0x020,
    0x024, 0x028, 0x030, 0x034, 0x038, 0x03c, 0x040, 0x044,
    0x048, 0x050, 0x060, 0x064, 0x068, 0x06c, 0x070, 0x074,
    0x078, 0x0b4, 0x100, 0x104, 0x108, 0x110, 0x114, 0x118,
    0x120, 0x124, 0x128, 0x130, 0x134, 0x138, 0x13c, 0x150,
    0x154, 0x158, 0x160, 0x164, 0x168, 0x16c, 0x170, 0x174,
    0x178, 0x17c, 0x180
};
constexpr uint32_t ControlOffsets[] = {
    0x000, 0x004, 0x008, 0x00c, 0x014, 0x018, 0x030, 0x040,
    0x044, 0x048, 0x04c, 0x060, 0x064, 0x070, 0x074, 0x078,
    0x07c, 0x0a0, 0x0a4, 0x0a8, 0x0ac, 0x0c0, 0x0c4, 0x0e4,
    0x0e8, 0x0ec, 0x0f0, 0x0f4, 0x0f8, 0x0fc, 0x100, 0x104,
    0x108, 0x10c, 0x110, 0x114, 0x128, 0x134, 0x138, 0x13c
};
constexpr uint32_t MixerOffsets[] = {
    0x000, 0x004, 0x008, 0x010,
    0x020, 0x024, 0x028, 0x02c, 0x030, 0x034,
    0x038, 0x03c, 0x040, 0x044, 0x048, 0x04c,
    0x050, 0x054, 0x058, 0x05c, 0x060, 0x064,
    0x068, 0x06c, 0x070, 0x074, 0x078, 0x07c,
    0x080, 0x084, 0x088, 0x08c, 0x090, 0x094,
    0x098, 0x09c, 0x0a0, 0x0a4, 0x0a8, 0x0ac,
    0x0b0, 0x0b4, 0x0b8, 0x0bc, 0x0c0, 0x0c4,
    0x0c8, 0x0cc, 0x0d0, 0x0d4, 0x0d8, 0x0dc,
    0x0e0, 0x0e4, 0x0e8, 0x0ec, 0x0f0, 0x0f4,
    0x310, 0x314
};
constexpr uint32_t InterfaceOffsets[] = {
    0x000, 0x004, 0x008, 0x00c, 0x010, 0x014, 0x018, 0x01c,
    0x020, 0x024, 0x028, 0x02c, 0x030, 0x034, 0x038, 0x03c,
    0x040, 0x044, 0x048, 0x04c, 0x050, 0x054, 0x058, 0x05c,
    0x060, 0x064, 0x068, 0x080, 0x084, 0x090, 0x0a8, 0x0ac,
    0x0b0, 0x170, 0x174, 0x1c0, 0x1c4, 0x25c, 0x268, 0x26c
};

NativeDpuSnapshot snapshot = {};
unsigned captureState = 0;
IOReturn captureResult = kIOReturnNotReady;

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

template <size_t Count>
void captureBlock(IOMemoryMap *mapping, uint32_t base,
    const uint32_t (&offsets)[Count], NativeDpuRegisterBlock &block) {
    static_assert(Count <= NativeDpuBlockWords, "DPU register block capacity");

    block.base = base;
    block.count = Count;
    for (size_t index = 0; index < Count; ++index) {
        block.words[index] = {offsets[index], read32(mapping, base + offsets[index])};
    }
}

void logBlock(const char *name, unsigned instance, const NativeDpuRegisterBlock &block) {
    IOLog("T733 DPU: %s%u base=%05x words=%u\n", name, instance, block.base, block.count);

    uint32_t index = 0;
    while (block.count - index >= 4) {
        const NativeDpuRegisterValue *word = block.words + index;
        IOLog("T733 DPU: %s%u +%03x=%08x +%03x=%08x +%03x=%08x +%03x=%08x\n",
            name, instance, word[0].offset, word[0].value, word[1].offset, word[1].value,
            word[2].offset, word[2].value, word[3].offset, word[3].value);
        index += 4;
    }

    while (index < block.count) {
        const NativeDpuRegisterValue &word = block.words[index];
        IOLog("T733 DPU: %s%u +%03x=%08x\n", name, instance, word.offset, word.value);
        ++index;
    }
}

void captureDpu(IOMemoryMap *mapping) {
    PE_Video video = {};
    if (!PE_current_console(&video)) {
        snapshot.firmwarePhysical = video.v_baseAddr;
        snapshot.firmwareBytes = video.v_rowBytes * video.v_height;
        snapshot.firmwareWidth = video.v_width;
        snapshot.firmwareHeight = video.v_height;
        snapshot.firmwareStride = video.v_rowBytes;
    }

    snapshot.mdssInterruptStatus = read32(mapping, 0x10);
    snapshot.mdpInterruptEnable = read32(mapping, 0x1010);
    snapshot.mdpInterruptStatus = read32(mapping, 0x1014);
    snapshot.mdpInterrupt2Enable = read32(mapping, 0x1008);
    snapshot.mdpInterrupt2Status = read32(mapping, 0x100c);
    IOLog("T733 DPU: hw=%08x mdss-irq=%08x intr-en=%08x intr=%08x intr2-en=%08x intr2=%08x\n",
        snapshot.hardwareVersion, snapshot.mdssInterruptStatus, snapshot.mdpInterruptEnable,
        snapshot.mdpInterruptStatus, snapshot.mdpInterrupt2Enable, snapshot.mdpInterrupt2Status);
    IOLog("T733 DPU: firmware pa=%llx bytes=%llu width=%u height=%u stride=%u\n",
        snapshot.firmwarePhysical, snapshot.firmwareBytes, snapshot.firmwareWidth,
        snapshot.firmwareHeight, snapshot.firmwareStride);

    for (unsigned index = 0; index < NativeDpuPipeCount; ++index) {
        captureBlock(mapping, PipeBases[index], PipeOffsets, snapshot.pipes[index]);
        logBlock("SSPP", index, snapshot.pipes[index]);

        const uint32_t address = read32(mapping, PipeBases[index] + 0x14);
        if (snapshot.firmwarePhysical && address == snapshot.firmwarePhysical) {
            snapshot.firmwarePipeMask |= 1U << index;
        }
    }

    for (unsigned index = 0; index < NativeDpuControlCount; ++index) {
        captureBlock(mapping, ControlBases[index], ControlOffsets, snapshot.controls[index]);
        logBlock("CTL", index, snapshot.controls[index]);
    }

    for (unsigned index = 0; index < NativeDpuMixerCount; ++index) {
        captureBlock(mapping, MixerBases[index], MixerOffsets, snapshot.mixers[index]);
        logBlock("LM", index == 0 ? 0 : index + 1, snapshot.mixers[index]);
    }

    captureBlock(mapping, InterfaceBase, InterfaceOffsets, snapshot.interface);
    logBlock("INTF", 1, snapshot.interface);

    for (unsigned index = 0; index < NativeDpuFrameSamples; ++index) {
        if (index) {
            IOSleep(8);
        }

        NativeDpuFrameSample &sample = snapshot.samples[index];
        sample.ticks = mach_absolute_time();
        sample.frameCount = read32(mapping, InterfaceBase + 0xac);
        sample.lineCount = read32(mapping, InterfaceBase + 0xb0);
        sample.interruptStatus = read32(mapping, 0x1014);
        for (unsigned control = 0; control < NativeDpuControlCount; ++control) {
            sample.controlFlush[control] = read32(mapping, ControlBases[control] + 0x18);
        }
        IOLog("T733 DPU: sample=%u ticks=%llu frame=%u line=%u intr=%08x flush=%08x,%08x,%08x,%08x\n",
            index, sample.ticks, sample.frameCount, sample.lineCount, sample.interruptStatus,
            sample.controlFlush[0], sample.controlFlush[1], sample.controlFlush[2], sample.controlFlush[3]);
    }

    IOLog("T733 DPU: firmware-pipe-mask=%x intf1-vsync-mask=08000000\n",
        snapshot.firmwarePipeMask);
}

void captureContext(IOMemoryMap *mapping, uint32_t bank) {
    for (unsigned index = 0; index < snapshot.contextCount; ++index) {
        if (snapshot.contexts[index].bank == bank) {
            return;
        }
    }

    if (bank >= (snapshot.smmuId1 & 255) || snapshot.contextCount >= NativeDpuMaxContextBanks) {
        return;
    }

    const uint32_t offset = snapshot.contextBase + bank * snapshot.contextStride;
    if (offset > AppsSmmuBytes || AppsSmmuBytes - offset < 0x70) {
        return;
    }

    NativeDpuSmmuContext &context = snapshot.contexts[snapshot.contextCount++];
    context.bank = bank;
    context.offset = offset;
    IOLog("T733 DPU: SMMU-CB%u capture begin off=%x\n", bank, offset);
    context.cbar = read32(mapping, snapshot.contextStride + 4 * bank);
    context.cba2r = read32(mapping, snapshot.contextStride + 0x800 + 4 * bank);
    context.cbfrsynra = read32(mapping, snapshot.contextStride + 0x400 + 4 * bank);
    context.sctlr = read32(mapping, offset);
    context.actlr = read32(mapping, offset + 4);
    context.tcr2 = read32(mapping, offset + 0x10);
    context.ttbr0 = read64(mapping, offset + 0x20);
    context.ttbr1 = read64(mapping, offset + 0x28);
    context.tcr = read32(mapping, offset + 0x30);
    context.mair0 = read32(mapping, offset + 0x38);
    context.mair1 = read32(mapping, offset + 0x3c);
    context.fsr = read32(mapping, offset + 0x58);
    context.far = read64(mapping, offset + 0x60);
    context.fsynr0 = read32(mapping, offset + 0x68);

    IOLog("T733 DPU: SMMU-CB%u off=%x cbar=%08x cba2r=%08x sctlr=%08x actlr=%08x\n",
        bank, offset, context.cbar, context.cba2r, context.sctlr, context.actlr);
    IOLog("T733 DPU: SMMU-CB%u tcr=%08x tcr2=%08x ttbr0=%016llx ttbr1=%016llx mair=%08x,%08x\n",
        bank, context.tcr, context.tcr2, context.ttbr0, context.ttbr1, context.mair0, context.mair1);
    IOLog("T733 DPU: SMMU-CB%u fsr=%08x fsynr0=%08x far=%016llx cbfrsynra=%08x\n",
        bank, context.fsr, context.fsynr0, context.far, context.cbfrsynra);
}

IOReturn captureSmmu(IOMemoryMap *mapping) {
    IOLog("T733 DPU: SMMU capture begin base=%llx bytes=%llx\n",
        AppsSmmuPhysical, AppsSmmuBytes);
    snapshot.smmuScr0 = read32(mapping, 0x00);
    snapshot.smmuId0 = read32(mapping, 0x20);
    snapshot.smmuId1 = read32(mapping, 0x24);
    snapshot.smmuId2 = read32(mapping, 0x28);
    snapshot.smmuGlobalFault = read32(mapping, 0x48);
    snapshot.smmuGlobalSyndrome0 = read32(mapping, 0x50);
    snapshot.smmuGlobalSyndrome1 = read32(mapping, 0x54);
    IOLog("T733 DPU: SMMU scr0=%08x id0=%08x id1=%08x id2=%08x gfsr=%08x gfsynr=%08x,%08x\n",
        snapshot.smmuScr0, snapshot.smmuId0, snapshot.smmuId1, snapshot.smmuId2,
        snapshot.smmuGlobalFault, snapshot.smmuGlobalSyndrome0, snapshot.smmuGlobalSyndrome1);

    const uint32_t count = snapshot.smmuId0 & 255;
    const uint32_t sidBits = (snapshot.smmuId0 >> 9) & 15;
    const bool extendedIds = (snapshot.smmuId0 & (1U << 8)) != 0;
    if (!(snapshot.smmuId0 & (1U << 27)) || !count || (!extendedIds && !sidBits)) {
        return kIOReturnUnsupported;
    }

    snapshot.streamCount = count;
    snapshot.contextStride = 1U << ((snapshot.smmuId1 >> 31) ? 16 : 12);
    snapshot.contextBase = snapshot.contextStride *
        (1U << (((snapshot.smmuId1 >> 28) & 7) + 1));

    const uint32_t sidMask = extendedIds ? 0xffff : (1U << sidBits) - 1;
    for (unsigned index = 0; index < snapshot.streamCount; ++index) {
        NativeDpuStreamRoute &route = snapshot.streams[index];
        route.smr = read32(mapping, 0x800 + 4 * index);
        route.s2cr = read32(mapping, 0xc00 + 4 * index);
        route.valid = extendedIds ? (route.s2cr >> 10) & 1 : route.smr >> 31;

        const uint32_t sid = route.smr & sidMask;
        const uint32_t mask = (route.smr >> 16) & sidMask;
        if (route.valid) {
            for (unsigned stream = 0; stream < sizeof(DisplayStreams) / sizeof(DisplayStreams[0]); ++stream) {
                if (!((DisplayStreams[stream] ^ sid) & ~mask & sidMask)) {
                    route.displayStreams |= 1U << stream;
                }
            }
        }

        IOLog("T733 DPU: SMMU-route%u smr=%08x s2cr=%08x valid=%u sid=%04x mask=%04x display=%x type=%u cb=%u\n",
            index, route.smr, route.s2cr, route.valid, sid, mask, route.displayStreams,
            (route.s2cr >> 16) & 3, route.s2cr & 255);
    }

    // Only banks reached by a valid translating display route are accessed.
    for (unsigned index = 0; index < snapshot.streamCount; ++index) {
        const NativeDpuStreamRoute &route = snapshot.streams[index];
        if (route.displayStreams && ((route.s2cr >> 16) & 3) == 0) {
            captureContext(mapping, route.s2cr & 255);
        }
    }

    return kIOReturnSuccess;
}

IOReturn captureHardware() {
    IOMemoryMap *mdp = mapRegisters(MdpPhysical, MdpBytes);
    if (!mdp || !mdp->getVirtualAddress()) {
        OSSafeReleaseNULL(mdp);
        return kIOReturnNoMemory;
    }

    snapshot.version = 1;
    IOLog("T733 DPU: capture begin mdp=%llx bytes=%llx\n", MdpPhysical, MdpBytes);
    snapshot.hardwareVersion = read32(mdp, 0);
    IOLog("T733 DPU: hardware version=%08x\n", snapshot.hardwareVersion);
    if ((snapshot.hardwareVersion & 0xffff0000U) != 0x70020000U) {
        mdp->release();
        return kIOReturnUnsupported;
    }

    captureDpu(mdp);
    mdp->release();

    IOMemoryMap *smmu = mapRegisters(AppsSmmuPhysical, AppsSmmuBytes);
    if (!smmu || !smmu->getVirtualAddress()) {
        OSSafeReleaseNULL(smmu);
        return kIOReturnNoMemory;
    }

    const IOReturn result = captureSmmu(smmu);
    smmu->release();

    return result;
}

}

extern "C" IOReturn t733_dpu_capture_start(void) {
    unsigned expected = 0;
    if (!__atomic_compare_exchange_n(&captureState, &expected, 1, false,
        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        if (expected == 1) {
            return kIOReturnBusy;
        }

        return captureResult;
    }

    captureResult = captureHardware();
    IOLog("T733 DPU: capture complete result=%x pipes=%x routes=%u contexts=%u\n",
        captureResult, snapshot.firmwarePipeMask, snapshot.streamCount, snapshot.contextCount);
    __atomic_store_n(&captureState, captureResult == kIOReturnSuccess ? 2 : 3, __ATOMIC_RELEASE);

    return captureResult;
}

extern "C" const NativeDpuSnapshot *t733_dpu_capture_snapshot(void) {
    if (__atomic_load_n(&captureState, __ATOMIC_ACQUIRE) != 2) {
        return nullptr;
    }

    return &snapshot;
}
