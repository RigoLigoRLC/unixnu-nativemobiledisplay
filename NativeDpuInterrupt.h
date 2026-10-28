// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <IOKit/IOReturn.h>
#include <stdint.h>

// NativeDpuInterruptResources describes the selected panel's DPU register
// window, GIC interrupt and active interface. The device supplies these values
// from its DT; NativeDpuInterrupt owns registration and MDP acknowledgement.
struct NativeDpuInterruptResources {
    uint64_t physical;
    uint64_t bytes;
    uint32_t interrupt;
    uint32_t interruptBase;
    uint32_t interfaceBase;
    uint32_t controlBase;
    uint32_t vsyncMask;
};

// NativeDpuVblankSample contains one acknowledged interface vblank and the
// accompanying CTL flush observation. The hard interrupt records values;
// the display work loop owns presentation and buffer retirement.
struct NativeDpuVblankSample {
    uint64_t sequence;
    uint64_t absoluteTime;
    uint64_t observationBegan;
    uint64_t continuousTime;
    uint32_t frameCount;
    uint32_t flush;
};

extern "C" bool native_dpu_interrupt_resources(NativeDpuInterruptResources *resources);
extern "C" IOReturn native_dpu_interrupt_register(uint32_t interrupt,
    void (*callback)(void *), void *context);
extern "C" IOReturn native_dpu_interrupt_enable(uint32_t interrupt);
extern "C" IOReturn native_dpu_interrupt_disable(uint32_t interrupt);

// The wake callback runs in interrupt context and may only signal a work loop.
// Stop removes the callback under the interrupt lock before returning.
extern "C" IOReturn native_dpu_vblank_start(void (*wake)(void *), void *context);
extern "C" void native_dpu_vblank_stop(void);
extern "C" bool native_dpu_vblank_take(NativeDpuVblankSample *sample);
