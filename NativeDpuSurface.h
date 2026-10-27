// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <IOKit/IOReturn.h>
#include <stdint.h>

class IOMemoryDescriptor;
class OSObject;

enum : uint32_t {
    NativeDpuPresentationPolled = 1
};

// NativeDpuPresentation describes the CTL flush observed for one IOSurface.
// Both endpoints use mach_continuous_time. The upper endpoint is the first
// observation of a completed flush and advancing frame counter, before buffer
// reclamation. The display family pairs this value with its retained swap ID.
struct NativeDpuPresentation {
    uint64_t lowerBound; // A time before submission or a read of pending flush.
    uint64_t upperBound; // The software presentation estimate, not a hardware latch.
    uint64_t submittedTime; // A sample immediately before the CTL0 flush write.
    uint32_t frameCount;
    uint32_t source;
};

// The t733-dpu-scanout=1 boot argument enables the IOSurface scanout backend.
extern "C" bool t733_dpu_surface_enabled(void);

// The caller serializes all operations on the display work loop. Producer
// completion precedes begin. Success transfers the active IOSurfaceFence
// reference to this module and arms VIG0's address/CTL0 flush; the caller clears
// its request's fence slot before leaving the gate. A null fence uses a separate
// IOSurface use count. The prepared descriptor and surface remain retained.
extern "C" IOReturn t733_dpu_surface_begin(OSObject *surface,
    IOMemoryDescriptor *memory, uint64_t stride, uint32_t width, uint32_t height,
    OSObject *fence);

// Poll performs one hardware observation. NotReady retains both buffers while
// the flush or next frame is pending. Success releases the replaced buffer and
// permits the display family to complete its request. Errors retain resources
// until a firmware restore confirms the hardware no longer scans them.
extern "C" IOReturn t733_dpu_surface_poll(void);

// The display gate consumes each successful surface presentation once. A
// restore, failed operation or subsequent begin clears an unconsumed result.
extern "C" bool t733_dpu_surface_take_presentation(NativeDpuPresentation *result);

// Begin restore replaces any pending surface switch with the retained firmware
// framebuffer. Poll confirms that switch before releasing surfaces or fences.
// disableContext also restores CB4's bypass state and releases its tables.
extern "C" IOReturn t733_dpu_surface_begin_restore(bool disableContext);

extern "C" bool t733_dpu_surface_active(void);
extern "C" bool t733_dpu_surface_pending(void);
extern "C" bool t733_dpu_surface_timing_enabled(void);
