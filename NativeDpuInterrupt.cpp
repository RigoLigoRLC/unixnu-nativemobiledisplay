// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NativeDpuInterrupt.h"

#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/clock.h>

extern "C" uint64_t mach_continuous_time(void);

namespace {

// NativeDpuInterruptState keeps the registered GIC callback and its mapping
// alive for the boot lifetime. Start and stop attach the display's wake
// callback under a spin lock; the interrupt never enters the display gate.
struct NativeDpuInterruptState {
    NativeDpuInterruptResources resources = {};
    IOMemoryMap *mapping = nullptr;
    IOSimpleLock *lock = nullptr;
    void (*wake)(void *) = nullptr;
    void *context = nullptr;
    NativeDpuVblankSample sample = {};
    uint64_t consumed = 0;
    bool registered = false;
};

NativeDpuInterruptState state;

uint32_t readRegister(uint32_t offset) {
    return *reinterpret_cast<volatile uint32_t *>(state.mapping->getVirtualAddress() + offset);
}

void writeRegister(uint32_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(state.mapping->getVirtualAddress() + offset) = value;
}

void interruptOccurred(void *) {
    const IOInterruptState flags = IOSimpleLockLockDisableInterrupt(state.lock);
    const auto &resources = state.resources;
    const uint32_t pending = readRegister(resources.interruptBase + 0x14);
    if (pending & resources.vsyncMask) {
        NativeDpuVblankSample sample = {};
        sample.sequence = state.sample.sequence + 1;
        sample.observationBegan = mach_continuous_time();
        sample.flush = readRegister(resources.controlBase + 0x18);
        sample.frameCount = readRegister(resources.interfaceBase + 0xac);
        sample.absoluteTime = mach_absolute_time();
        sample.continuousTime = mach_continuous_time();
        writeRegister(resources.interruptBase + 0x18, resources.vsyncMask);
        __asm__ volatile("dsb sy" ::: "memory");
        state.sample = sample;
        if (state.wake) {
            state.wake(state.context);
        }
    }
    IOSimpleLockUnlockEnableInterrupt(state.lock, flags);
}

}

extern "C" IOReturn native_dpu_vblank_start(void (*wake)(void *), void *context) {
    if (!wake) {
        return kIOReturnBadArgument;
    }
    if (!state.lock) {
        state.lock = IOSimpleLockAlloc();
        if (!state.lock) {
            return kIOReturnNoMemory;
        }
    }
    if (!state.mapping) {
        NativeDpuInterruptResources resources = {};
        if (!native_dpu_interrupt_resources(&resources) || resources.bytes < 0xb0 ||
            resources.interruptBase > resources.bytes - 0x1c ||
            resources.interfaceBase > resources.bytes - 0xb0 ||
            resources.controlBase > resources.bytes - 0x1c || !resources.vsyncMask ||
            ((resources.interruptBase | resources.interfaceBase | resources.controlBase) & 3)) {
            return kIOReturnUnsupported;
        }
        IODeviceMemory *memory = IODeviceMemory::withRange(resources.physical, resources.bytes);
        if (!memory) {
            return kIOReturnNoMemory;
        }
        state.mapping = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
        memory->release();
        if (!state.mapping) {
            return kIOReturnNoMemory;
        }
        state.resources = resources;
    }

    const auto &resources = state.resources;
    if (!state.registered) {
        const IOReturn result = native_dpu_interrupt_register(resources.interrupt,
            &interruptOccurred, nullptr);
        if (result != kIOReturnSuccess) {
            return result;
        }
        state.registered = true;
    }

    const IOInterruptState flags = IOSimpleLockLockDisableInterrupt(state.lock);
    if (state.wake) {
        IOSimpleLockUnlockEnableInterrupt(state.lock, flags);
        return kIOReturnExclusiveAccess;
    }
    const uint32_t enabled = readRegister(resources.interruptBase + 0x10);
    if ((enabled & ~resources.vsyncMask) || readRegister(resources.interruptBase + 8)) {
        IOSimpleLockUnlockEnableInterrupt(state.lock, flags);
        return kIOReturnUnsupported;
    }
    state.context = context;
    state.wake = wake;
    state.consumed = state.sample.sequence;
    writeRegister(resources.interruptBase + 0x18, resources.vsyncMask);
    writeRegister(resources.interruptBase + 0x10, enabled | resources.vsyncMask);
    __asm__ volatile("dsb sy" ::: "memory");
    IOSimpleLockUnlockEnableInterrupt(state.lock, flags);

    const IOReturn result = native_dpu_interrupt_enable(resources.interrupt);
    if (result != kIOReturnSuccess) {
        native_dpu_vblank_stop();
    }
    return result;
}

extern "C" void native_dpu_vblank_stop(void) {
    if (!state.registered || !state.lock) {
        return;
    }
    native_dpu_interrupt_disable(state.resources.interrupt);
    const IOInterruptState flags = IOSimpleLockLockDisableInterrupt(state.lock);
    state.wake = nullptr;
    state.context = nullptr;
    const auto &resources = state.resources;
    const uint32_t enabled = readRegister(resources.interruptBase + 0x10);
    writeRegister(resources.interruptBase + 0x10, enabled & ~resources.vsyncMask);
    writeRegister(resources.interruptBase + 0x18, resources.vsyncMask);
    __asm__ volatile("dsb sy" ::: "memory");
    IOSimpleLockUnlockEnableInterrupt(state.lock, flags);
}

extern "C" bool native_dpu_vblank_take(NativeDpuVblankSample *sample) {
    if (!sample || !state.lock) {
        return false;
    }
    const IOInterruptState flags = IOSimpleLockLockDisableInterrupt(state.lock);
    const bool available = state.sample.sequence != state.consumed;
    if (available) {
        *sample = state.sample;
        state.consumed = state.sample.sequence;
    }
    IOSimpleLockUnlockEnableInterrupt(state.lock, flags);

    return available;
}
