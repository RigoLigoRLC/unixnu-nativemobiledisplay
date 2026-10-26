// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <IOKit/IOReturn.h>
#include <stdint.h>

enum {
    NativeDpuPipeCount = 4,
    NativeDpuControlCount = 4,
    NativeDpuMixerCount = 3,
    NativeDpuBlockWords = 80,
    NativeDpuMaxStreamRoutes = 256,
    NativeDpuMaxContextBanks = 256,
    NativeDpuFrameSamples = 4
};

// NativeDpuRegisterValue retains one 32-bit register and its byte offset from
// the containing DPU block. The register lists exclude clear and trigger ports.
struct NativeDpuRegisterValue {
    uint32_t offset;
    uint32_t value;
};

// NativeDpuRegisterBlock contains the named registers read from one hardware
// block. base is relative to the MDP physical window at 0x0ae00000.
struct NativeDpuRegisterBlock {
    uint32_t base;
    uint32_t count;
    NativeDpuRegisterValue words[NativeDpuBlockWords];
};

// NativeDpuStreamRoute contains the unchanged apps SMMU stream-match pair.
// The four bits in displayStreams identify SID 0x900, 0x902, 0xd00 and 0xd02.
struct NativeDpuStreamRoute {
    uint32_t smr;
    uint32_t s2cr;
    uint32_t valid;
    uint32_t displayStreams;
};

// NativeDpuSmmuContext contains the translation registers of a bank reached
// by a non-secure display SID. No bank is selected from its numerical position.
struct NativeDpuSmmuContext {
    uint32_t bank;
    uint32_t offset;
    uint32_t cbar;
    uint32_t cba2r;
    uint32_t sctlr;
    uint32_t actlr;
    uint32_t tcr;
    uint32_t tcr2;
    uint32_t mair0;
    uint32_t mair1;
    uint32_t fsr;
    uint32_t fsynr0;
    uint32_t cbfrsynra;
    uint64_t ttbr0;
    uint64_t ttbr1;
    uint64_t far;
};

// NativeDpuFrameSample records the running DSI interface and CTL flush words
// at one counter timestamp. Four samples, eight milliseconds apart, retain
// actual frame-count changes without clearing or enabling an interrupt.
struct NativeDpuFrameSample {
    uint64_t ticks;
    uint32_t frameCount;
    uint32_t lineCount;
    uint32_t interruptStatus;
    uint32_t controlFlush[NativeDpuControlCount];
};

// NativeDpuSnapshot contains the firmware-owned SM7325 display state captured
// by t733_dpu_capture_start. The capture maps the DPU and apps SMMU register
// windows as Device memory, reads the yupik SDE 7.2 block configuration, then
// records every SMMU route and the banks reached by non-secure display SIDs.
//
// The immutable snapshot remains owned by this module for the boot lifetime.
// NativeLegacyDisplay owns swap requests and their IOSurface descriptors; this
// capture supplies register state and does not acquire those descriptors.
struct NativeDpuSnapshot {
    uint32_t version;
    uint32_t hardwareVersion;
    uint32_t mdssInterruptStatus;
    uint32_t mdpInterruptEnable;
    uint32_t mdpInterruptStatus;
    uint32_t mdpInterrupt2Enable;
    uint32_t mdpInterrupt2Status;
    uint32_t firmwarePipeMask;
    uint64_t firmwarePhysical;
    uint64_t firmwareBytes;
    uint32_t firmwareWidth;
    uint32_t firmwareHeight;
    uint32_t firmwareStride;
    NativeDpuRegisterBlock pipes[NativeDpuPipeCount];
    NativeDpuRegisterBlock controls[NativeDpuControlCount];
    NativeDpuRegisterBlock mixers[NativeDpuMixerCount];
    NativeDpuRegisterBlock interface;
    NativeDpuFrameSample samples[NativeDpuFrameSamples];
    uint32_t smmuScr0;
    uint32_t smmuId0;
    uint32_t smmuId1;
    uint32_t smmuId2;
    uint32_t smmuGlobalFault;
    uint32_t smmuGlobalSyndrome0;
    uint32_t smmuGlobalSyndrome1;
    uint32_t streamCount;
    uint32_t contextCount;
    uint32_t contextStride;
    uint32_t contextBase;
    NativeDpuStreamRoute streams[NativeDpuMaxStreamRoutes];
    NativeDpuSmmuContext contexts[NativeDpuMaxContextBanks];
};

// The caller invokes the capture from a sleepable initialization thread while
// the inherited DPU video engine is running. The first invocation performs the
// capture and logs its contents; later invocations return its saved result.
extern "C" IOReturn t733_dpu_capture_start(void);

// The returned pointer is available after a successful capture. Its contents
// remain immutable and the caller must not release or modify the storage.
extern "C" const NativeDpuSnapshot *t733_dpu_capture_snapshot(void);
