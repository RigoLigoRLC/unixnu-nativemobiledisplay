#include "NativeLegacyRequest.h"
// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeLegacyDisplay supplies presentation for the original framebuffer
// family's user clients and request queues. Unimplemented backend leaves stop.
#include "OriginalLegacyFramebufferABI.h"
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
#include "OriginalAppleARMBacklightABI.h"
#include "NativeDpuSurface.h"
#include "NativeDpuInterrupt.h"
#include "../Adreno6xx/NativeAdrenoDisplay.h"
#endif
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IONotifier.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <kern/clock.h>
#include <mach/message.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMpowerState.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <pexpert/pexpert.h>
extern "C" __attribute__((noreturn)) void native_legacy_backend_guard(unsigned,void *,void *);
extern "C" IOService *native_legacy_display_allocate(void);
extern "C" bool native_legacy_gop_mapping_observed(IOService *,IOMemoryMap *,uint32_t,uint32_t);
extern "C" void native_legacy_registration_observed(IOService *);
extern "C" void native_legacy_swap_stage(unsigned);
extern "C" void native_legacy_client_enter(task_t,uint32_t);
extern "C" void native_legacy_client_observed(IOReturn,IOUserClient *);
extern "C" void native_legacy_pm_observed(unsigned,IOReturn,unsigned long);
extern "C" IOMemoryDescriptor *native_legacy_surface_descriptor(OSObject *);
extern "C" uint64_t native_legacy_surface_width(OSObject *),native_legacy_surface_height(OSObject *);
extern "C" uint64_t native_legacy_surface_stride(OSObject *),native_legacy_surface_allocation(OSObject *);
extern "C" uint32_t native_legacy_surface_format(OSObject *),native_legacy_surface_planes(OSObject *);
extern "C" void native_legacy_frame_observed(unsigned kind,uint64_t count,uint64_t minx,uint64_t miny,uint64_t maxx,uint64_t maxy);
extern "C" void native_legacy_frame_colors_observed(const uint32_t *colors,const uint64_t *counts,uint64_t other);
extern "C" void native_legacy_swap_validation_observed(bool,unsigned,const void *,const void *);
// The surface of a rejected swap, as the validator read it: width, height, format, planes, stride, allocation, descriptor length.
extern "C" void native_legacy_surface_rejected(uint64_t,uint64_t,uint32_t,uint32_t,uint64_t,uint64_t,uint64_t);
extern "C" bool native_legacy_prepare_scanout(void);
extern "C" void native_legacy_transfer_observed(IOReturn,const void *,uint64_t);
extern "C" void native_legacy_copy_timing_observed(uint64_t ticks,uint64_t bytes);
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
extern "C" bool t733_display_power_start(void);
extern "C" IOReturn t733_display_set_power(bool);
extern "C" IOReturn t733_display_set_brightness(uint32_t, uint32_t);
extern "C" IOReturn t733_display_set_millinits(uint32_t, uint32_t *);
extern "C" bool t733_display_bind_begin(void **previous, int *cpu);
extern "C" void t733_display_bind_end(void *previous);
extern "C" void t733_display_submission_observed(uint64_t start, uint64_t mapTicks,
    uint64_t copyTicks, uint64_t totalTicks, uint64_t bytes,
    bool reused, uint64_t hits, uint64_t mappings, bool paired,
    uint64_t cpuStart, uint64_t cpuEnd, int boundCpu, uint64_t bindTicks);
extern "C" void t733_display_mapping_observed(uint64_t scanout, uint64_t surface);
extern "C" IOReturn native_adreno_surface_acquire_read(IOMemoryDescriptor *,
    void (*)(void *, IOReturn), void *, NativeAdrenoSurfaceRead **)
    __attribute__((weak_import));
extern "C" IOReturn native_adreno_surface_read_status(NativeAdrenoSurfaceRead *, uint32_t *)
    __attribute__((weak_import));
extern "C" void native_adreno_surface_release_read(NativeAdrenoSurfaceRead *)
    __attribute__((weak_import));
extern "C" IOReturn native_adreno_surface_profile_capture(IOMemoryDescriptor *memory,
    uint32_t capturedFence, uint64_t compositionStartContinuousUs,
    NativeAdrenoSurfaceProfile *result) __attribute__((weak_import));
#endif
extern "C" void native_legacy_retirement_observed(IOService *);
// The privileged GPU client accepts frames with sheng-fb-gpu=1 and owns their
// memory during copying. sheng-early-retire controls whether acceptance retires
// the swap immediately or native_legacy_gpu_complete retires it after copying.
extern "C" bool native_legacy_gpu_offer(void *display,IOMemoryDescriptor *,uint64_t stride,uint32_t width,uint32_t height);
extern "C" void native_legacy_gpu_fallback_observed(int status);
extern "C" {
    volatile uint64_t native_legacy_early_retires, native_legacy_early_failures;
}
// Display refresh tick (boot-arg sheng-vsync=<Hz>, 0 or absent: off). The original family sends the compositor's vsync message (type 5)
// through native_legacy_vsync_notify, the unchanged original sender, but only a platform subclass calls it from the panel's vblank and
// that subclass is not part of this port: without it the render server never saw a vsync. The GOP scanout has no vblank interrupt, so a
// work-loop timer stands in for it while at least one client has vsync notifications enabled. Events: 0 armed, 1 sent, 2 idle (no client),
// 3 late (missed intervals skipped), 4 started, 5 refresh properties reported at that rate.
extern "C" void native_legacy_vsync_notify(void *display,uint64_t timestamp,uint64_t continuousTime,uint64_t interval);
extern "C" void native_legacy_frame_info_notify(void *display,
    const OriginalLegacyFrameReport *report);
// The unchanged original setDisplayRefreshProperties: publishes IOMFBDisplayRefresh from slot 301 (getRefreshAttributes below).
extern "C" bool native_legacy_set_refresh_properties(void *display);
extern "C" void native_legacy_vsync_observed(unsigned event,unsigned hz);
extern "C" uint64_t mach_continuous_time(void);

#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
enum : uint32_t {
    NativeFrameCompositionStartValid = 1U,
    NativeFrameIssuedValid = 2U,
    NativeFrameRequestedValid = 4U,
    NativeFrameAcceptedValid = 8U,
    NativeFrameReadFenceActiveValid = 16U,
    NativeFrameProducerReadyValid = 32U,
    NativeFrameGpuMetadataValid = 64U,
    NativeFramePresentationValid = 128U,
    NativeFrameProfileCapacity = 512U
};

// NativeLegacyFrameObservation joins the native swap's composition timestamp
// and ID to source-writing GPU submissions and the DPU switch interval. The
// display gate fills one record while retaining the swap request. The completed
// record owns values only, so the diagnostic history retains no surface or fence.
struct NativeLegacyFrameObservation {
    uint64_t sequence = 0;
    uint64_t compositionStart = 0;
    uint64_t issuedTime = 0;
    uint64_t requestedTime = 0;
    uint64_t acceptedTime = 0;
    uint64_t readFenceActiveTime = 0;
    uint64_t producerWaitTime = 0;
    uint64_t producerReadyTime = 0;
    uint32_t frameID = 0;
    uint32_t completion = 0;
    uint32_t validity = 0;
    uint32_t capturedFence = 0; // The latest hardware fence among this surface's writers.
    bool queued = false;
    IOReturn status = kIOReturnSuccess;
    IOReturn gpuStatus = kIOReturnUnsupported;
    NativeDpuPresentation presentation = {};
    NativeAdrenoSurfaceProfile gpu = {}; // Its timestamps already use continuous microseconds.
};

static NativeLegacyFrameObservation nativeFrameHistory[NativeFrameProfileCapacity];
static uint32_t nativeFrameHistoryLock;
static uint32_t nativeFrameHistoryNext;
static uint32_t nativeFrameHistoryCount;
static uint64_t nativeFrameHistorySequence;
static uint64_t nativeFrameHistoryOverwritten;

static void nativeFrameHistoryEnter() {
    while (__atomic_test_and_set(&nativeFrameHistoryLock, __ATOMIC_ACQUIRE)) {
    }
}

static void nativeFrameHistoryLeave() {
    __atomic_clear(&nativeFrameHistoryLock, __ATOMIC_RELEASE);
}

static uint64_t nativeFrameMicroseconds(uint64_t ticks) {
    uint64_t nanoseconds = 0;
    absolutetime_to_nanoseconds(ticks, &nanoseconds);

    return nanoseconds / 1000;
}

static bool nativeFrameRemember(NativeLegacyFrameObservation &observation) {
    nativeFrameHistoryEnter();
    observation.sequence = ++nativeFrameHistorySequence;
    nativeFrameHistory[nativeFrameHistoryNext] = observation;
    nativeFrameHistoryNext = (nativeFrameHistoryNext + 1) % NativeFrameProfileCapacity;
    const bool overwritten = nativeFrameHistoryCount == NativeFrameProfileCapacity;
    if (overwritten) {
        ++nativeFrameHistoryOverwritten;
    } else {
        ++nativeFrameHistoryCount;
    }
    nativeFrameHistoryLeave();

    return overwritten;
}

extern "C" void native_legacy_frame_profile_dump(void) {
    const size_t snapshotBytes = sizeof(nativeFrameHistory);
    auto snapshot = static_cast<NativeLegacyFrameObservation *>(IOMalloc(snapshotBytes));
    if (!snapshot) {
        IOLog("T733 frame-profile-summary: allocation_failed=1\n");
        return;
    }

    nativeFrameHistoryEnter();
    const uint32_t count = nativeFrameHistoryCount;
    const uint32_t first = (nativeFrameHistoryNext + NativeFrameProfileCapacity - count) %
        NativeFrameProfileCapacity;
    const uint64_t overwritten = nativeFrameHistoryOverwritten;
    const uint64_t sequence = nativeFrameHistorySequence;
    for (uint32_t index = 0; index < count; ++index) {
        snapshot[index] = nativeFrameHistory[(first + index) % NativeFrameProfileCapacity];
    }
    nativeFrameHistoryLeave();

    IOLog("T733 frame-profile-summary: clock=continuous units=us capacity=%u count=%u overwritten=%llu latest_sequence=%llu\n",
        NativeFrameProfileCapacity, count, overwritten, sequence);
    for (uint32_t index = 0; index < count; ++index) {
        const NativeLegacyFrameObservation &frame = snapshot[index];
        const NativeAdrenoSurfaceProfile &gpu = frame.gpu;
        IOLog("T733 frame-profile: sequence=%llu part=0 parts=9 frame_id=%u completion=%u status=%x queued=%u valid=%u gpu_status=%x gpu_flags=%u\n",
            frame.sequence, frame.frameID, frame.completion, frame.status,
            frame.queued, frame.validity, frame.gpuStatus, gpu.flags);
        IOLog("T733 frame-profile: sequence=%llu part=1 composition_start_us=%llu swap_issued_us=%llu\n",
            frame.sequence,
            nativeFrameMicroseconds(frame.compositionStart),
            nativeFrameMicroseconds(frame.issuedTime));
        IOLog("T733 frame-profile: sequence=%llu part=2 requested_presentation_us=%llu accepted_us=%llu\n",
            frame.sequence,
            nativeFrameMicroseconds(frame.requestedTime),
            nativeFrameMicroseconds(frame.acceptedTime));
        IOLog("T733 frame-profile: sequence=%llu part=3 read_fence_active_us=%llu producer_wait_us=%llu producer_ready_us=%llu\n",
            frame.sequence,
            nativeFrameMicroseconds(frame.readFenceActiveTime),
            nativeFrameMicroseconds(frame.producerWaitTime),
            nativeFrameMicroseconds(frame.producerReadyTime));
        IOLog("T733 frame-profile: sequence=%llu part=4 dpu_submit_us=%llu display_lower_us=%llu display_upper_us=%llu\n",
            frame.sequence,
            nativeFrameMicroseconds(frame.presentation.submittedTime),
            nativeFrameMicroseconds(frame.presentation.lowerBound),
            nativeFrameMicroseconds(frame.presentation.upperBound));
        IOLog("T733 frame-profile: sequence=%llu part=5 dpu_frame=%u dpu_source=%u captured_fence=%u first_fence=%u last_fence=%u\n",
            frame.sequence,
            frame.presentation.frameCount, frame.presentation.source,
            frame.capturedFence, gpu.firstFence, gpu.lastFence);
        IOLog("T733 frame-profile: sequence=%llu part=6 submissions=%u exact_submissions=%u fallback_submissions=%u stored_submissions=%u\n",
            frame.sequence, gpu.submissions, gpu.exactSubmissions,
            gpu.fallbackSubmissions, gpu.storedSubmissions);
        IOLog("T733 frame-profile: sequence=%llu part=7 first_submit_us=%llu last_submit_us=%llu gpu_complete_observed_us=%llu\n",
            frame.sequence, gpu.firstSubmitUs, gpu.lastSubmitUs, gpu.completeObservedUs);
        IOLog("T733 frame-profile: sequence=%llu part=8 match_cache_hit=%u match_segments=%u match_us=%llu\n",
            frame.sequence,
            gpu.matchCacheHit, gpu.matchSegments, gpu.matchUs);
        for (uint32_t writer = 0; writer < gpu.storedSubmissions; ++writer) {
            IOLog("T733 frame-write: sequence=%llu index=%u fence=%u submit_sequence=%u\n",
                frame.sequence, writer, gpu.fences[writer], gpu.sequences[writer]);
        }
    }

    IOFree(snapshot, snapshotBytes);
}
#endif

// NativeLegacyFrameInfoSender sends a subscription's async reference and frame
// arguments through IOKit's finite queue. The display records the send result.
class NativeLegacyFrameInfoSender final : private IOUserClient {
    NativeLegacyFrameInfoSender() = delete;

public:
    static IOReturn send(OSAsyncReference64 reference, io_user_reference_t arguments[],
        UInt32 count) {
        return sendAsyncResult64WithOptions(reference, kIOReturnSuccess, arguments,
            count, kIOUserNotifyOptionCanDrop);
    }
};

// 32-bit BGRA, plain ('BGRA') or in Apple's lossless-compressed spelling ('&BGA', one plane): the GPU that renders the surface here writes plain rows
// either way (no compression is implemented), so the bytes are the same.
static inline bool nativeBgra(uint32_t format,uint32_t planes) {
    return (format==0x42475241 && !planes) || (format==0x26424741 && planes<=1);
}

static uint64_t nativeLegacyCounter() {
    uint64_t counter;
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(counter) :: "memory");
    return counter;
}

#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
    static bool nativePublishBrightness(IOService *display, uint32_t level, uint32_t milliNits) {
        OSDictionary *parameters = OSDictionary::withCapacity(2);
        OSDictionary *brightness = OSDictionary::withCapacity(3);
        OSDictionary *nits = OSDictionary::withCapacity(3);
        auto number = [](OSDictionary *dictionary, const char *name, uint32_t value) -> bool {
            OSNumber *object = OSNumber::withNumber(value, 32);
            bool result = dictionary && object && dictionary->setObject(name, object);
            OSSafeReleaseNULL(object);
            return result;
        };
        bool result = parameters && brightness && nits &&
            number(brightness, "min", 0) && number(brightness, "max", 255) &&
            number(brightness, "value", level) &&
            number(nits, "min", 4000) && number(nits, "max", 500000) &&
            number(nits, "value", milliNits) &&
            parameters->setObject("brightness", brightness) &&
            parameters->setObject("BrightnessMilliNits", nits) &&
            display->setProperty("IODisplayParameters", parameters);
        OSSafeReleaseNULL(nits);
        OSSafeReleaseNULL(brightness);
        OSSafeReleaseNULL(parameters);
        return result;
    }

static bool nativePublishBacklightCalibration(IOService *display) {
    OSDictionary *calibration = OSDictionary::withCapacity(9);
    if (!calibration) {
        return false;
    }

    // CoreBrightness dereferences every calibration number. Current fields
    // retain zero defaults; this service controls the panel through nits.
    const struct {
        const char *name;
        uint32_t fixed;
    } entries[] = {
        {"current-for-max-backlight", 0},
        {"current-for-mid-backlight", 0},
        {"hardware-max-current-limit", 0},
        {"lMinProduct", 4U << 16},
        {"lMidProduct", 190U << 16},
        {"lMaxProduct", 500U << 16},
        {"lMinPanel", 4U << 16},
        {"lMidPanel", 190U << 16},
        {"lMaxPanel", 500U << 16}
    };
    bool result = true;
    for (const auto &entry : entries) {
        OSNumber *number = OSNumber::withNumber(entry.fixed, 32);
        if (!number || !calibration->setObject(entry.name, number)) {
            result = false;
        }
        OSSafeReleaseNULL(number);
        if (!result) {
            break;
        }
    }

    if (result) {
        result = display->setProperty("backlight-calibration-parameters", calibration);
    }
    calibration->release();

    return result;
}

// NativeT733Backlight is the T733 panel's brightness service and inherits the
// existing AppleARMPlatform AppleARMBacklight metaclass. CoreBrightness matches
// backlight-control and requires that base class before creating its display
// module, even when QuartzCore provides brightness transactions.
// The primary NativeLegacyDisplay owns the service. Its parameter dictionary
// supplies the panel range, and brightness-nits requests reach DisplayPower's
// serialized DSI, boost and touch operations.
// BrightnessControl delivers those requests while preparing each frame. The
// inherited framebuffer swap method retains the compositor's frame metadata.
class NativeT733Backlight : public AppleARMBacklight {
    OSDeclareDefaultStructors(NativeT733Backlight)
    IOLock *brightnessLock = nullptr;

public:
    bool init(OSDictionary *properties = nullptr) override {
        return IOService::init(properties);
    }

    bool start(IOService *provider) override {
        if (!IOService::start(provider)) {
            return false;
        }

        brightnessLock = IOLockAlloc();
        // CoreBrightness reads equally spaced slider samples as signed 16.16
        // nits and requires this table for new-backlight-architecture displays.
        const int32_t marketingNits[] = {4 << 16, 190 << 16, 500 << 16};
        OSData *marketingTable = OSData::withBytes(marketingNits, sizeof(marketingNits));
        const bool ready = brightnessLock && marketingTable &&
            nativePublishBrightness(this, 125, 190000) &&
            nativePublishBacklightCalibration(this) &&
            setProperty("backlight-control", true) &&
            setProperty("backlight-marketing-table", marketingTable) &&
            setProperty("new-backlight-architecture", true);
        OSSafeReleaseNULL(marketingTable);

        if (!ready) {
            IOService::stop(provider);
            return false;
        }

        setName("T733PanelBacklight");
        return true;
    }

    IOReturn setProperties(OSObject *properties) override {
        OSDictionary *dictionary = OSDynamicCast(OSDictionary, properties);
        if (!dictionary) {
            return kIOReturnBadArgument;
        }

        OSObject *object = dictionary->getObject("brightness-nits");
        const bool usesNits = object != nullptr;
        if (!object) {
            object = dictionary->getObject("brightness");
        }
        if (!object) {
            return IOService::setProperties(properties);
        }
        OSNumber *number = OSDynamicCast(OSNumber, object);
        if (!number) {
            return kIOReturnBadArgument;
        }

        uint32_t level = 0;
        uint32_t milliNits = 0;
        if (usesNits) {
            const int32_t fixed = static_cast<int32_t>(number->unsigned32BitValue());
            milliNits = fixed > 0 ?
                static_cast<uint32_t>((static_cast<uint64_t>(fixed) * 1000 + 32768) >> 16) : 0;
            if (milliNits > 500000) {
                milliNits = 500000;
            }
        } else {
            level = number->unsigned32BitValue();
            if (level > 255) {
                return kIOReturnBadArgument;
            }

            if (level > 125) {
                milliNits = 190000 + (level - 125) * 310000 / 130;
            } else if (level > 2) {
                milliNits = 4000 + (level - 2) * 186000 / 123;
            } else if (level) {
                milliNits = 4000;
            }
        }

        IOLockLock(brightnessLock);
        IOReturn result = usesNits ? t733_display_set_millinits(milliNits, &level) :
            t733_display_set_brightness(level, 255);
        if (result == kIOReturnSuccess && !nativePublishBrightness(this, level, milliNits)) {
            result = kIOReturnNoMemory;
        }
        IOLockUnlock(brightnessLock);

        return result;
    }

    void free() override {
        if (brightnessLock) {
            IOLockFree(brightnessLock);
            brightnessLock = nullptr;
        }
        IOService::free();
    }

    IOWorkLoop *getWorkLoop() const override {
        return IOService::getWorkLoop();
    }

    IOReturn configureReport(IOReportChannelList *channels, IOReportConfigureAction action,
        void *result, void *destination) override {
        return IOService::configureReport(channels, action, result, destination);
    }

    IOReturn updateReport(IOReportChannelList *channels, IOReportUpdateAction action,
        void *result, void *destination) override {
        return IOService::updateReport(channels, action, result, destination);
    }

    IOReturn callPlatformFunction(const OSSymbol *name, bool wait,
        void *param1, void *param2, void *param3, void *param4) override {
        return IOService::callPlatformFunction(name, wait, param1, param2, param3, param4);
    }

    IOReturn message(UInt32 type, IOService *provider, void *argument) override {
        return IOService::message(type, provider, argument);
    }

    IOReturn setPowerState(unsigned long ordinal, IOService *device) override {
        return IOService::setPowerState(ordinal, device);
    }
};
OSDefineMetaClassAndStructors(NativeT733Backlight, AppleARMBacklight)

// NativeLegacyCopyBinding keeps the current non-realtime thread on an online
// T733 performance CPU while a frame is copied. The work-loop gate remains
// held across the scheduling point, so another submission cannot enter.
// Restoration occurs before completion and also on every early return.
class NativeLegacyCopyBinding {
    void *previous = nullptr;
    bool active = false;

public:
    int cpu = -1;
    uint64_t ticks = 0;

    NativeLegacyCopyBinding() {
        static int enabled = -1;
        if (enabled < 0) {
            unsigned argument = 0;
            PE_parse_boot_argn("t733-display-bind-big", &argument, sizeof(argument));
            enabled = argument == 1;
        }
        if (!enabled) {
            return;
        }

        const uint64_t start = nativeLegacyCounter();
        active = t733_display_bind_begin(&previous, &cpu);
        ticks = nativeLegacyCounter() - start;
    }

    NativeLegacyCopyBinding(const NativeLegacyCopyBinding &) = delete;
    NativeLegacyCopyBinding &operator=(const NativeLegacyCopyBinding &) = delete;

    void restore() {
        if (!active) {
            return;
        }

        t733_display_bind_end(previous);
        active = false;
    }

    ~NativeLegacyCopyBinding() {
        restore();
    }
};

// The caller validates both row ranges before copying. Each iteration transfers
// 64 bytes with general registers; the caller drains stores before completion.
static bool nativeLegacyCopyPairs(volatile void *destination, uint64_t destinationStride,
    const void *source, uint64_t sourceStride, uint32_t width, uint32_t height) {
    const uintptr_t destinationBase = reinterpret_cast<uintptr_t>(destination);
    const uintptr_t sourceBase = reinterpret_cast<uintptr_t>(source);
    if ((destinationBase | sourceBase | destinationStride | sourceStride) & 7) {
        return false;
    }

    const uint64_t rowBytes = static_cast<uint64_t>(width) * 4;
    const uint64_t pairedBytes = rowBytes & ~63ull;
    for (uint32_t row = 0; row < height; ++row) {
        uintptr_t output = destinationBase + row * destinationStride;
        uintptr_t input = sourceBase + row * sourceStride;
        uint64_t blocks = pairedBytes / 64;
        if (blocks) {
            __asm__ volatile(
                "1:\n\t"
                "ldp x8, x9, [%[input]]\n\t"
                "ldp x10, x11, [%[input], #16]\n\t"
                "ldp x12, x13, [%[input], #32]\n\t"
                "ldp x14, x15, [%[input], #48]\n\t"
                "stp x8, x9, [%[output]]\n\t"
                "stp x10, x11, [%[output], #16]\n\t"
                "stp x12, x13, [%[output], #32]\n\t"
                "stp x14, x15, [%[output], #48]\n\t"
                "add %[input], %[input], #64\n\t"
                "add %[output], %[output], #64\n\t"
                "subs %[blocks], %[blocks], #1\n\t"
                "b.ne 1b\n\t"
                : [input] "+&r"(input), [output] "+&r"(output), [blocks] "+&r"(blocks)
                :
                : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "cc", "memory");
        }

        for (uint64_t offset = pairedBytes; offset < rowBytes; offset += 4) {
            *reinterpret_cast<volatile uint32_t *>(output) =
                *reinterpret_cast<const uint32_t *>(input);
            output += 4;
            input += 4;
        }
    }
    return true;
}
#endif

class NativeLegacyDisplay : public IOMobileFramebufferLegacy {
    OSDeclareDefaultStructors(NativeLegacyDisplay)
    IOMemoryMap *bootMapping=nullptr;
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
    IONotifier *shutdownNotifier = nullptr;

    static IOReturn shutdownNotification(void *target, void *, UInt32 message,
        IOService *, void *, vm_size_t) {
        if (message != kIOMessageSystemPagingOff) {
            return kIOReturnSuccess;
        }

        auto display = static_cast<NativeLegacyDisplay *>(target);
        // PagingOff precedes vnode destruction. The display gate stops new
        // presents before the firmware flip retires the final surface read.
        display->stopFrameInfo();
        display->stopVsync();
        display->stopDpuSurface();
        display->clearSourceMappings();
        IOLog("T733 display shutdown: paging_off=1 presents_stopped=%u scanout_retained=%u\n",
            unsigned(display->displayStopping), unsigned(display->dpuBlocked));
        return kIOReturnSuccess;
    }

    void removeShutdownNotifier() {
        if (shutdownNotifier) {
            shutdownNotifier->remove();
            shutdownNotifier = nullptr;
        }
    }
#endif

    static constexpr uint32_t FrameInfoPresentationMask = 1U;
    static constexpr uint32_t FrameInfoRequestedMask = 2U;
    static constexpr uint32_t FrameInfoSupportedMask =
        FrameInfoPresentationMask | FrameInfoRequestedMask;

    // FrameInfoStatistics accounts for presentation, report ownership and
    // delivery separately. The display work-loop gate serializes every update.
    struct FrameInfoStatistics {
        uint64_t completed = 0;
        uint64_t completionFailed = 0;
        uint64_t missingPresentation = 0;
        uint64_t unmatchedRequest = 0;
        uint64_t generated = 0;
        uint64_t consumed = 0;
        uint64_t pendingDiscarded = 0;
        uint64_t unmatchedQuery = 0;
        uint64_t sent = 0;
        uint64_t queueFull = 0;
        uint64_t sendErrors = 0;
        uint64_t subscriptions = 0;
        uint64_t unsubscriptions = 0;
        uint64_t maskChanges = 0;
        uint64_t historyOverwritten = 0;
        uint64_t uncertaintyTotal = 0;
        uint64_t uncertaintyMaximum = 0;
        IOReturn lastSendResult = kIOReturnSuccess;
    };

    FrameInfoStatistics frameInfoStatistics;
    uint64_t frameInfoSummaryDeadline = 0;
    OriginalLegacyFrameReport pendingFrameInfo = {};
    bool pendingFrameInfoValid = false;
    bool frameInfoStopped = false;

    static uint32_t frameInfoMask(const OriginalLegacyFrameInfoSubscription *subscription) {
        uint32_t mask = 0;
        const auto bytes = reinterpret_cast<const unsigned char *>(subscription);
        memcpy(&mask, bytes + 0x78, sizeof(mask));
        return mask;
    }

    static void storeFrameInfoMask(OriginalLegacyFrameInfoSubscription *subscription,
        uint32_t mask) {
        auto bytes = reinterpret_cast<unsigned char *>(subscription);
        memcpy(bytes + 0x78, &mask, sizeof(mask));
    }

    void discardFrameInfoReport() {
        if (pendingFrameInfoValid) {
            ++frameInfoStatistics.pendingDiscarded;
        }
        pendingFrameInfoValid = false;
        pendingFrameInfo = {};
    }

    void stopFrameInfo() {
        auto action = [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            NativeLegacyDisplay *display = static_cast<NativeLegacyDisplay *>(owner);
            display->frameInfoStopped = true;
            display->discardFrameInfoReport();
            return kIOReturnSuccess;
        };
        if (IOWorkLoop *loop = getWorkLoop()) {
            loop->runAction(action, this);
        } else {
            action(this, nullptr, nullptr, nullptr, nullptr);
        }
    }

    void summarizeFrameInfo() {
        if (mach_absolute_time() < frameInfoSummaryDeadline) {
            return;
        }

        clock_interval_to_deadline(5000, kMillisecondScale, &frameInfoSummaryDeadline);
        uint64_t maximumNanoseconds = 0;
        absolutetime_to_nanoseconds(frameInfoStatistics.uncertaintyMaximum, &maximumNanoseconds);
        IOLog("T733 FrameInfo: complete=%llu failed=%llu missing=%llu unmatched=%llu reports=%llu consumed=%llu discarded=%llu query-miss=%llu sent=%llu full=%llu errors=%llu status=%x subscriptions=%llu/%llu masks=%llu history-overwritten=%llu uncertainty-max-us=%llu\n",
            frameInfoStatistics.completed, frameInfoStatistics.completionFailed,
            frameInfoStatistics.missingPresentation, frameInfoStatistics.unmatchedRequest,
            frameInfoStatistics.generated, frameInfoStatistics.consumed,
            frameInfoStatistics.pendingDiscarded, frameInfoStatistics.unmatchedQuery,
            frameInfoStatistics.sent, frameInfoStatistics.queueFull,
            frameInfoStatistics.sendErrors, frameInfoStatistics.lastSendResult,
            frameInfoStatistics.subscriptions, frameInfoStatistics.unsubscriptions,
            frameInfoStatistics.maskChanges, frameInfoStatistics.historyOverwritten,
            maximumNanoseconds / 1000);
    }

#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
    void stageFrameInfoReport(const OriginalLegacySwapRequest *request,
        const NativeDpuPresentation &presentation) {
        discardFrameInfoReport();
        pendingFrameInfo.setPresentation(request, presentation.upperBound);
        pendingFrameInfoValid = true;
        ++frameInfoStatistics.generated;
        const uint64_t uncertainty = presentation.upperBound - presentation.lowerBound;
        frameInfoStatistics.uncertaintyTotal += uncertainty;
        if (uncertainty > frameInfoStatistics.uncertaintyMaximum) {
            frameInfoStatistics.uncertaintyMaximum = uncertainty;
        }

    }

    NativeT733Backlight *backlight = nullptr;

    bool startBacklight() {
        if (backlight || !primary) {
            return true;
        }

        NativeT733Backlight *service = new NativeT733Backlight;
        if (!service || !service->init()) {
            OSSafeReleaseNULL(service);
            return false;
        }

        if (!service->attach(this)) {
            service->release();
            return false;
        }
        if (!service->start(this)) {
            service->detach(this);
            service->release();
            return false;
        }

        backlight = service;
        backlight->registerService();
        IOLog("T733 display: NativeT733Backlight service ready base=AppleARMBacklight parameters=1 backlight=1 calibration-nits=6 marketing-table=3 new-architecture=1\n");
        return true;
    }

    void stopBacklight() {
        if (!backlight) {
            return;
        }

        backlight->terminate(kIOServiceRequired | kIOServiceSynchronous);
        OSSafeReleaseNULL(backlight);
    }

    // T733 records copy timings without two extra full-screen pixel scans.
    uint32_t scanBudget=0;
    bool pairedScanout = false;
    // Each source mapping keeps its IOSurface owner and descriptor alive while
    // subsequent swaps reuse the kernel page mappings. The family owns swaps.
    struct SourceMapping {
        OSObject *surface = nullptr;
        IOMemoryDescriptor *memory = nullptr;
        IOMemoryMap *mapping = nullptr;
        uint64_t memoryLength = 0;
    };
    SourceMapping sourceMappings[4];
    IOLock *sourceMappingLock = nullptr;
    bool sourceMappingsEnabled = false;
    unsigned nextSourceMapping = 0;
    uint64_t sourceMappingHits = 0;
    uint64_t sourceMappingCreates = 0;

    static void releaseSourceMapping(SourceMapping &entry) {
        OSSafeReleaseNULL(entry.mapping);
        OSSafeReleaseNULL(entry.memory);
        OSSafeReleaseNULL(entry.surface);
        entry.memoryLength = 0;
    }

    void clearSourceMappings() {
        if (!sourceMappingLock) {
            return;
        }

        SourceMapping removed[4];
        IOLockLock(sourceMappingLock);
        sourceMappingsEnabled = false;
        for (unsigned index = 0; index < 4; ++index) {
            removed[index] = sourceMappings[index];
            sourceMappings[index] = {};
        }
        nextSourceMapping = 0;
        IOLockUnlock(sourceMappingLock);

        for (auto &entry : removed) {
            releaseSourceMapping(entry);
        }
    }

    IOMemoryMap *mapSource(OSObject *surface, IOMemoryDescriptor *memory,
        uint64_t allocation, bool &reused, uint64_t &hits, uint64_t &mappings) {
        reused = false;
        if (!sourceMappingLock) {
            return nullptr;
        }

        IOLockLock(sourceMappingLock);
        if (!sourceMappingsEnabled) {
            IOLockUnlock(sourceMappingLock);
            return nullptr;
        }

        const uint64_t memoryLength = memory->getLength();
        for (auto &entry : sourceMappings) {
            if (entry.memory == memory && entry.surface == surface &&
                entry.memoryLength == memoryLength && entry.mapping &&
                entry.mapping->getVirtualAddress() && entry.mapping->getLength() >= allocation) {
                IOMemoryMap *mapping = entry.mapping;
                mapping->retain();
                reused = true;
                hits = ++sourceMappingHits;
                mappings = sourceMappingCreates;
                IOLockUnlock(sourceMappingLock);
                return mapping;
            }
        }

        IOMemoryMap *mapping = memory->map(kIOMapAnywhere | kIOMapReadOnly);
        SourceMapping removed;
        if (mapping && mapping->getVirtualAddress() && mapping->getLength() >= allocation) {
            ++sourceMappingCreates;
            if (mapping->getLength() <= (64ull << 20)) {
                unsigned slot = nextSourceMapping;
                for (unsigned index = 0; index < 4; ++index) {
                    if (!sourceMappings[index].mapping || sourceMappings[index].memory == memory) {
                        slot = index;
                        break;
                    }
                }
                removed = sourceMappings[slot];
                surface->retain();
                memory->retain();
                mapping->retain();
                sourceMappings[slot] = {surface, memory, mapping, memoryLength};
                nextSourceMapping = (slot + 1) % 4;
            }
        }
        hits = sourceMappingHits;
        mappings = sourceMappingCreates;
        IOLockUnlock(sourceMappingLock);

        releaseSourceMapping(removed);
        return mapping;
    }
#else
    uint32_t scanBudget=32;
#endif
    void *shadow=nullptr;uint64_t shadowBytes=0;bool shadowValid=false;
    bool primary=false;
    bool logicalPM=false;
    bool transferred=false;
    // Every request the validator accepted and the family has not retired yet. A client may submit swap N+1 while swap N is still on the GPU
    // (int-131: swap 3 was validated during swap 2's GPU copy and one remembered pointer lost swap 2, which then never retired and left
    // every wait on it busy), so more than one can be pending.
    const OriginalLegacySwapRequest *validated[8]={};
    uint64_t validatedTime[8] = {};
    bool isValidated(const OriginalLegacySwapRequest *request) const {
        if(!request)return false;
        for(auto entry: validated)if(entry==request)return true;
        return false;
    }
    void removeValidated(const OriginalLegacySwapRequest *request) {
        for (unsigned index = 0; index < 8; ++index) {
            if (validated[index] == request) {
                validated[index] = nullptr;
                validatedTime[index] = 0;
            }
        }
    }

    void addValidated(const OriginalLegacySwapRequest *request) {
        if (isValidated(request)) {
            return;
        }

        unsigned slot = 0;
        for (unsigned index = 0; index < 8; ++index) {
            if (!validated[index]) {
                slot = index;
                break;
            }
        }
        validated[slot] = request;
        validatedTime[slot] = mach_continuous_time();
    }

    uint64_t validationTime(const OriginalLegacySwapRequest *request) const {
        for (unsigned index = 0; index < 8; ++index) {
            if (validated[index] == request) {
                return validatedTime[index];
            }
        }

        return 0;
    }
    const OriginalLegacySwapRequest *gpuRequest=nullptr;
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
    IOReturn surfaceProducerError = kIOReturnSuccess;
    bool dpuFallback = false;
    bool dpuBlocked = false;
    bool displayStopping = false;
    IOTimerEventSource *surfaceTimer = nullptr;
    IOInterruptEventSource *completionEvent = nullptr;
    IOInterruptEventSource *vblankEvent = nullptr;
    IOLock *producerLock = nullptr;
    bool surfaceTimerAdded = false;
    bool completionEventAdded = false;
    bool vblankEventAdded = false;
    bool vblankInterrupt = false;

    // The GPU completion callback only records the result and wakes the display
    // work loop. The display gate owns the request and transfers its read
    // reservation to the DPU buffer when scanout begins.
    NativeAdrenoSurfaceRead *producerRead = nullptr;
    uint32_t producerFence = 0;
    uint64_t producerReadFenceActiveTime = 0;
    uint64_t producerWaitTime = 0;
    uint64_t producerReadyTime = 0;
    uint64_t producerDeadline = 0;
    IOReturn producerResult = kIOReturnNotReady;
    bool producerDone = false;
    const OriginalLegacySwapRequest *producerRequest = nullptr;

    enum DpuCompletion {
        NoDpuCompletion,
        SurfaceCompletion,
        BlankCompletion,
        FallbackCompletion,
        DroppedCompletion
    };
    DpuCompletion dpuCompletion = NoDpuCompletion;
    const OriginalLegacySwapRequest *dpuRequest = nullptr;
    bool dpuRestoring = false;
    uint64_t dpuLength = 0;
    IOReturn dpuResult = kIOReturnSuccess;
    NativeLegacyFrameObservation dpuFrameObservation;

    static OSObject *requestObject(const OriginalLegacySwapRequest *request) {
        return reinterpret_cast<OSObject *>(const_cast<OriginalLegacySwapRequest *>(request));
    }

    void armSurfaceTimer(unsigned milliseconds = 1) {
        if (surfaceTimer && !displayStopping) {
            if (vblankInterrupt && milliseconds < 100) {
                milliseconds = 100;
            }
            surfaceTimer->setTimeoutMS(milliseconds);
        }
    }

    void clearProducerRequest() {
        NativeAdrenoSurfaceRead *read = producerRead;
        producerRead = nullptr;
        if (read) {
            native_adreno_surface_release_read(read);
        }
        if (producerLock) {
            IOLockLock(producerLock);
            producerDone = false;
            producerReadyTime = 0;
            IOLockUnlock(producerLock);
        }

        const OriginalLegacySwapRequest *request = producerRequest;
        producerRequest = nullptr;
        if (request) {
            requestObject(request)->release();
        }
    }

    static void completionFired(OSObject *owner, IOInterruptEventSource *, int) {
        static_cast<NativeLegacyDisplay *>(owner)->surfaceTick();
    }

    static void vblankWake(void *context) {
        static_cast<IOInterruptEventSource *>(context)->interruptOccurred(nullptr, nullptr, 0);
    }

    static void vblankFired(OSObject *owner, IOInterruptEventSource *, int) {
        NativeLegacyDisplay *display = static_cast<NativeLegacyDisplay *>(owner);
        NativeDpuVblankSample sample = {};
        if (!display->displayStopping && native_dpu_vblank_take(&sample)) {
            display->surfaceTick(&sample);
            if (display->vsyncInterval && display->vsyncWanted()) {
                native_legacy_vsync_notify(display, sample.absoluteTime,
                    sample.continuousTime, display->vsyncInterval);
                native_legacy_vsync_observed(1, display->vsyncHz);
            }
        }
    }

    static void producerReady(void *context, IOReturn result) {
        NativeLegacyDisplay *display = static_cast<NativeLegacyDisplay *>(context);
        IOInterruptEventSource *event = display->completionEvent;
        event->retain();
        IOLockLock(display->producerLock);
        display->producerResult = result;
        display->producerReadyTime = result == kIOReturnSuccess ? mach_continuous_time() : 0;
        display->producerDone = true;
        IOLockUnlock(display->producerLock);

        event->interruptOccurred(nullptr, nullptr, 0);
        event->release();
    }

    bool waitSurfaceProducer(const OriginalLegacySwapRequest *request,
        IOMemoryDescriptor *memory, uint32_t *fence) {
        if (!producerLock || !completionEvent || displayStopping) {
            return false;
        }
        if (producerRequest != request) {
            clearProducerRequest();
            requestObject(request)->retain();
            producerRequest = request;
            producerReadFenceActiveTime = layerZeroFence(request) ? mach_continuous_time() : 0;

            IOLockLock(producerLock);
            producerWaitTime = mach_continuous_time();
            clock_interval_to_deadline(5000, kMillisecondScale, &producerDeadline);
            producerFence = 0;
            producerResult = kIOReturnNotReady;
            IOLockUnlock(producerLock);

            IOReturn result = kIOReturnSuccess;
            const bool hasAcquire = !!native_adreno_surface_acquire_read;
            if (hasAcquire != !!native_adreno_surface_read_status ||
                hasAcquire != !!native_adreno_surface_release_read) {
                result = kIOReturnUnsupported;
            } else if (hasAcquire) {
                result = native_adreno_surface_acquire_read(memory,
                    &NativeLegacyDisplay::producerReady, this, &producerRead);
            }
            IOLockLock(producerLock);
            if (!producerDone && result != kIOReturnNotReady) {
                producerResult = result;
                producerReadyTime = result == kIOReturnSuccess ? mach_continuous_time() : 0;
                producerDone = true;
            }
            IOLockUnlock(producerLock);
            if (result == kIOReturnNotReady) {
                armSurfaceTimer(5000);
            }
        }

        IOLockLock(producerLock);
        if (!producerDone && mach_absolute_time() >= producerDeadline) {
            producerResult = kIOReturnTimeout;
            producerDone = true;
        }
        const bool done = producerDone;
        const IOReturn result = producerResult;
        IOLockUnlock(producerLock);
        if (!done) {
            return false;
        }
        if (result != kIOReturnSuccess) {
            if (result != surfaceProducerError) {
                IOLog("T733 display: producer status=%x fence=%u request=%p\n",
                    result, producerFence, request);
                surfaceProducerError = result;
            }
            holdDpuRequest(request, DroppedCompletion, 0);
            dpuResult = result;
            clearProducerRequest();
            completeDpuRequest();
            return false;
        }

        if (producerRead) {
            const IOReturn status = native_adreno_surface_read_status(producerRead, &producerFence);
            if (status != kIOReturnSuccess) {
                holdDpuRequest(request, DroppedCompletion, 0);
                dpuResult = status;
                clearProducerRequest();
                completeDpuRequest();
                return false;
            }
        }
        *fence = producerFence;
        surfaceProducerError = kIOReturnSuccess;
        return true;
    }

    void holdDpuRequest(const OriginalLegacySwapRequest *request, DpuCompletion completion,
        uint64_t length) {
        requestObject(request)->retain();
        dpuRequest = request;
        dpuCompletion = completion;
        dpuLength = length;
        dpuResult = kIOReturnSuccess;
        transferred = true;

        OriginalLegacyFrameReport report = {};
        report.setPresentation(request, 0);
        dpuFrameObservation = {};
        dpuFrameObservation.frameID = report.frameID();
        dpuFrameObservation.completion = completion;
        dpuFrameObservation.compositionStart =
            OriginalLegacyFrameReport::compositionStartTime(request);
        dpuFrameObservation.issuedTime = report.issuedTime();
        dpuFrameObservation.requestedTime = report.requestedPresentation();
        dpuFrameObservation.acceptedTime = validationTime(request);
        const uint64_t now = mach_continuous_time();
        if (dpuFrameObservation.issuedTime && dpuFrameObservation.issuedTime <= now) {
            dpuFrameObservation.validity |= NativeFrameIssuedValid;
        }
        if (dpuFrameObservation.compositionStart &&
            dpuFrameObservation.compositionStart <= dpuFrameObservation.issuedTime &&
            (dpuFrameObservation.validity & NativeFrameIssuedValid)) {
            dpuFrameObservation.validity |= NativeFrameCompositionStartValid;
        }
        if (dpuFrameObservation.requestedTime) {
            dpuFrameObservation.validity |= NativeFrameRequestedValid;
        }
        if (dpuFrameObservation.acceptedTime && dpuFrameObservation.acceptedTime <= now) {
            dpuFrameObservation.validity |= NativeFrameAcceptedValid;
        }
        if (producerRequest == request) {
            IOLockLock(producerLock);
            dpuFrameObservation.readFenceActiveTime = producerReadFenceActiveTime;
            dpuFrameObservation.producerWaitTime = producerWaitTime;
            dpuFrameObservation.producerReadyTime = producerReadyTime;
            dpuFrameObservation.capturedFence = producerFence;
            IOLockUnlock(producerLock);
            if (dpuFrameObservation.readFenceActiveTime) {
                dpuFrameObservation.validity |= NativeFrameReadFenceActiveValid;
            }
            if (dpuFrameObservation.producerReadyTime) {
                dpuFrameObservation.validity |= NativeFrameProducerReadyValid;
            }
        }
    }

    void completeDpuRequest() {
        const OriginalLegacySwapRequest *request = dpuRequest;
        const DpuCompletion completion = dpuCompletion;
        const uint64_t length = dpuLength;
        const IOReturn result = dpuResult;
        dpuRequest = nullptr;
        dpuCompletion = NoDpuCompletion;
        dpuRestoring = false;
        dpuBlocked = false;

        NativeDpuPresentation presentation = {};
        const bool hasPresentation = t733_dpu_surface_take_presentation(&presentation);
        const bool queued = request && pendingSwapRequest() == request && isValidated(request);
        if (request) {
            dpuFrameObservation.completion = completion;
            dpuFrameObservation.status = result;
            dpuFrameObservation.queued = queued;
            if (hasPresentation) {
                dpuFrameObservation.presentation = presentation;
                dpuFrameObservation.validity |= NativeFramePresentationValid;
            }
            if (nativeFrameRemember(dpuFrameObservation)) {
                ++frameInfoStatistics.historyOverwritten;
            }
            dpuFrameObservation = {};
        }
        if (completion == SurfaceCompletion && result == kIOReturnSuccess) {
            ++frameInfoStatistics.completed;
            if (!hasPresentation) {
                ++frameInfoStatistics.missingPresentation;
            } else if (!queued) {
                ++frameInfoStatistics.unmatchedRequest;
            } else if (!frameInfoStopped) {
                stageFrameInfoReport(request, presentation);
            }
        }

        if (queued && completion != FallbackCompletion) {
            native_legacy_transfer_observed(result, request,
                completion == DroppedCompletion ? 0 : length);
            retireSwap(request, true);
        } else if (request && !queued) {
            removeValidated(request);
        }
        discardFrameInfoReport();
        transferred = false;
        if (request) {
            requestObject(request)->release();
        }
        if (completion == FallbackCompletion || completion == DroppedCompletion) {
            gpuBypass = true;
        }
        summarizeFrameInfo();
        kickSwapQueueGated();
    }

    bool beginDpuRestore(bool disableContext) {
        const IOReturn result = t733_dpu_surface_begin_restore(disableContext);
        if (result != kIOReturnSuccess) {
            IOLog("T733 DPU surface: restore submit failed status=%x\n", result);
            dpuBlocked = true;
            return false;
        }

        dpuRestoring = true;
        armSurfaceTimer();
        return true;
    }

    static void surfaceFired(OSObject *owner, IOTimerEventSource *) {
        static_cast<NativeLegacyDisplay *>(owner)->surfaceTick();
    }

    void surfaceTick(const NativeDpuVblankSample *sample = nullptr) {
        if (displayStopping || dpuBlocked) {
            return;
        }
        if (dpuCompletion != NoDpuCompletion) {
            const IOReturn result = sample ? t733_dpu_surface_vblank(sample) : t733_dpu_surface_poll();
            if (result == kIOReturnNotReady) {
                armSurfaceTimer();
                return;
            }
            if (result != kIOReturnSuccess) {
                IOLog("T733 DPU surface: asynchronous completion status=%x restore=%u\n",
                    result, dpuRestoring);
                if (dpuRestoring) {
                    dpuBlocked = true;
                    return;
                }

                ++frameInfoStatistics.completionFailed;
                summarizeFrameInfo();
                dpuFallback = true;
                dpuCompletion = DroppedCompletion;
                dpuResult = result;
                beginDpuRestore(true);
                return;
            }

            completeDpuRequest();
            return;
        }

        if (producerRequest) {
            if (pendingSwapRequest() != producerRequest || !isValidated(producerRequest)) {
                removeValidated(producerRequest);
                clearProducerRequest();
            }
            kickSwapQueueGated();
        }
    }

    bool startSurfaceEvents() {
        IOWorkLoop *loop = getWorkLoop();
        if (!loop) {
            return false;
        }
        producerLock = IOLockAlloc();
        surfaceTimer = IOTimerEventSource::timerEventSource(this, &NativeLegacyDisplay::surfaceFired);
        completionEvent = IOInterruptEventSource::interruptEventSource(this,
            &NativeLegacyDisplay::completionFired);
        vblankEvent = IOInterruptEventSource::interruptEventSource(this,
            &NativeLegacyDisplay::vblankFired);
        if (!producerLock || !surfaceTimer || !completionEvent || !vblankEvent) {
            return false;
        }
        if (loop->addEventSource(surfaceTimer) != kIOReturnSuccess) {
            return false;
        }
        surfaceTimerAdded = true;
        if (loop->addEventSource(completionEvent) != kIOReturnSuccess) {
            return false;
        }
        completionEventAdded = true;
        completionEvent->enable();
        if (loop->addEventSource(vblankEvent) != kIOReturnSuccess) {
            return false;
        }
        vblankEventAdded = true;
        vblankEvent->enable();
        const IOReturn irqResult = native_dpu_vblank_start(&NativeLegacyDisplay::vblankWake,
            vblankEvent);
        vblankInterrupt = irqResult == kIOReturnSuccess;
        IOLog("T733 display: DPU vblank IRQ status=%x active=%u\n", irqResult, vblankInterrupt);

        displayStopping = false;
        frameInfoStopped = false;
        return true;
    }

    void stopDpuSurface() {
        IOWorkLoop *loop = getWorkLoop();
        const bool alreadyGated = loop && loop->inGate();
        if (!producerLock && !surfaceTimer && !completionEvent && !vblankEvent) {
            return;
        }

        auto begin = [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            NativeLegacyDisplay *display = static_cast<NativeLegacyDisplay *>(owner);
            display->displayStopping = true;
            display->frameInfoStopped = true;
            display->discardFrameInfoReport();
            display->transferred = true;
            if (display->vblankInterrupt) {
                native_dpu_vblank_stop();
                display->vblankInterrupt = false;
            }
            if (display->surfaceTimer) {
                display->surfaceTimer->disable();
                display->surfaceTimer->cancelTimeout();
            }
            display->clearProducerRequest();
            return t733_dpu_surface_begin_restore(true);
        };
        IOReturn result = loop ? loop->runAction(begin, this) : begin(this, nullptr, nullptr, nullptr, nullptr);

        if (completionEvent) {
            completionEvent->disable();
        }
        if (vblankEvent) {
            vblankEvent->disable();
        }
        if (vblankEventAdded && loop) {
            loop->removeEventSource(vblankEvent);
            vblankEventAdded = false;
        }
        if (completionEventAdded && loop) {
            loop->removeEventSource(completionEvent);
            completionEventAdded = false;
        }
        if (surfaceTimerAdded && loop) {
            loop->removeEventSource(surfaceTimer);
            surfaceTimerAdded = false;
        }
        OSSafeReleaseNULL(completionEvent);
        OSSafeReleaseNULL(vblankEvent);
        OSSafeReleaseNULL(surfaceTimer);

        auto poll = [](OSObject *, void *, void *, void *, void *) -> IOReturn {
            return t733_dpu_surface_poll();
        };
        while (result == kIOReturnSuccess || result == kIOReturnNotReady) {
            result = loop ? loop->runAction(poll, this) : poll(this, nullptr, nullptr, nullptr, nullptr);
            if (result != kIOReturnNotReady || alreadyGated) {
                break;
            }
            IOSleep(1);
        }
        if (result != kIOReturnSuccess) {
            IOLog("T733 DPU surface: stop restore failed status=%x; scanout resources retained\n", result);
        }
        if (dpuRequest) {
            requestObject(dpuRequest)->release();
            dpuRequest = nullptr;
        }
        dpuCompletion = NoDpuCompletion;
        dpuRestoring = false;
        dpuBlocked = result != kIOReturnSuccess;
        transferred = false;
        if (producerLock) {
            IOLockFree(producerLock);
            producerLock = nullptr;
        }
    }

    bool presentDpuSurface(const OriginalLegacySwapRequest *request, OSObject *surface,
        IOMemoryDescriptor *memory, uint64_t stride, uint32_t width, uint32_t height,
        uint32_t producerFence) {
        if (dpuBlocked) {
            return true;
        }
        if (dpuFallback || !t733_dpu_surface_enabled()) {
            return false;
        }

        holdDpuRequest(request, SurfaceCompletion, stride * height);
        IOReturn result = t733_dpu_surface_begin(surface, memory, stride, width, height,
            layerZeroFence(request), producerRead);
        if (result == kIOReturnSuccess) {
            producerRead = nullptr;
            transferLayerZeroFence(request);
            if (native_adreno_surface_profile_capture) {
                const uint64_t compositionStartUs =
                    dpuFrameObservation.validity & NativeFrameCompositionStartValid ?
                    nativeFrameMicroseconds(dpuFrameObservation.compositionStart) : 0;
                dpuFrameObservation.gpuStatus = native_adreno_surface_profile_capture(memory,
                    producerFence, compositionStartUs, &dpuFrameObservation.gpu);
                if (dpuFrameObservation.gpuStatus == kIOReturnSuccess) {
                    dpuFrameObservation.validity |= NativeFrameGpuMetadataValid;
                }
            }
            clearProducerRequest();
            armSurfaceTimer();
            return true;
        }

        IOLog("T733 DPU surface: present failed status=%x fence=%u request=%p\n",
            result, producerFence, request);
        ++frameInfoStatistics.completionFailed;
        summarizeFrameInfo();
        dpuFallback = true;
        dpuCompletion = FallbackCompletion;
        dpuResult = result;
        beginDpuRestore(true);
        return true;
    }
#endif
    uint64_t gpuLength=0;
    bool gpuBypass=false;
    // The accepted GPU frame has already completed the family's swap request.
    bool gpuRetired = false;
    // The refresh tick runs on the family's work loop, the loop of the command gate that registers and removes the vsync clients, so the
    // client queue at +5592 never changes while the sender walks it.
    IOTimerEventSource *vsyncTimer=nullptr;
    uint64_t vsyncInterval=0,vsyncNext=0;
    unsigned vsyncHz=0;
    bool vsyncArmed=false;
    bool vsyncWanted() const {
        const void *head=nullptr;memcpy(&head,reinterpret_cast<const unsigned char *>(this)+5592,sizeof(head));return head;
    }
    // Selector 72's registration of a vsync client ends in kickSwapQueue (slot 290), which arms the tick; the tick stops by itself once
    // the last client is removed.
    void armVsync() {
        if(!vsyncTimer || vsyncArmed || !vsyncWanted())return;
        vsyncArmed=true;vsyncNext=mach_absolute_time()+vsyncInterval;
        vsyncTimer->wakeAtTime(vsyncNext);native_legacy_vsync_observed(0,vsyncHz);
    }
    static void vsyncFired(OSObject *owner,IOTimerEventSource *) {static_cast<NativeLegacyDisplay *>(owner)->vsyncTick();}
    void vsyncTick() {
        vsyncArmed=false;
        if(!vsyncTimer)return;
        if(!vsyncWanted()){native_legacy_vsync_observed(2,vsyncHz);return;}
        const uint64_t now=mach_absolute_time();
        native_legacy_vsync_notify(this,now,mach_continuous_time(),vsyncInterval);
        native_legacy_vsync_observed(1,vsyncHz);
        vsyncNext+=vsyncInterval;
        if (vsyncNext <= now) {
            vsyncNext += ((now - vsyncNext) / vsyncInterval + 1) * vsyncInterval;
            native_legacy_vsync_observed(3, vsyncHz);
        }
        vsyncArmed=true;vsyncTimer->wakeAtTime(vsyncNext);
    }
    void startVsync() {
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if(!hz || hz>240 || !primary || !bootMapping || vsyncTimer)return;
        nanoseconds_to_absolutetime(1000000000ull/hz,&vsyncInterval);
        vsyncHz=hz;
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
        if (vblankInterrupt) {
            native_legacy_vsync_observed(4, hz);
            return;
        }
#endif
        IOWorkLoop *loop=getWorkLoop();
        if(!loop)return;
        IOTimerEventSource *timer=IOTimerEventSource::timerEventSource(this,&NativeLegacyDisplay::vsyncFired);
        if(!timer)return;
        if(loop->addEventSource(timer)!=kIOReturnSuccess){timer->release();return;}
        vsyncHz=hz;vsyncTimer=timer;native_legacy_vsync_observed(4,hz);
        // A client may already have registered: arm under the gate, like the registration does.
        loop->runAction([](OSObject *owner,void *,void *,void *,void *)->IOReturn {
            static_cast<NativeLegacyDisplay *>(owner)->armVsync();return kIOReturnSuccess;
        },this);
    }
    void stopVsync() {
        if(!vsyncTimer)return;
        auto disable = [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            NativeLegacyDisplay *display = static_cast<NativeLegacyDisplay *>(owner);
            display->vsyncTimer->disable();
            display->vsyncTimer->cancelTimeout();
            return kIOReturnSuccess;
        };
        if (IOWorkLoop *loop = getWorkLoop()) {
            loop->runAction(disable, this);
            loop->removeEventSource(vsyncTimer);
        } else {
            disable(this, nullptr, nullptr, nullptr, nullptr);
        }
        OSSafeReleaseNULL(vsyncTimer);vsyncArmed=false;
    }
public:
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
    IOReturn applyDisplayState() override {
        IOReturn result = t733_display_set_power(requestedAndPowered());
        return result == kIOReturnNotReady ? kIOReturnSuccess : result;
    }
#endif
    // Called by the GPU client's completion (any thread): the swap is finished on the framebuffer's work loop.
    void gpuComplete(int status) {
        struct Args {NativeLegacyDisplay *display;int status;} args={this,status};
        IOWorkLoop *loop=getWorkLoop();
        auto action=[](OSObject *,void *a0,void *,void *,void *)->IOReturn {
            Args *a=static_cast<Args *>(a0);a->display->finishGpuSwap(a->status);return kIOReturnSuccess;
        };
        if(loop)loop->runAction(action,this,&args);else finishGpuSwap(status);
    }
    void free() override {
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
        removeShutdownNotifier();
#endif
        stopFrameInfo();
        stopVsync();
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
        stopDpuSurface();
#endif
        if (logicalPM) {
            PMstop();
            logicalPM = false;
        }
        primary = false;
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
        stopBacklight();
        clearSourceMappings();
        if (sourceMappingLock) {
            IOLockFree(sourceMappingLock);
            sourceMappingLock = nullptr;
        }
#endif
        OSSafeReleaseNULL(bootMapping);
        IOMobileFramebufferLegacy::free();
    }
    void stop(IOService *provider) override {
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
        removeShutdownNotifier();
#endif
        stopFrameInfo();
        stopVsync();
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
        stopDpuSurface();
        stopBacklight();
#endif
        transferred = false;
        for (auto &entry : validated) {
            entry = nullptr;
        }
        gpuRequest = nullptr;
        gpuRetired = false;
        if(logicalPM){PMstop();logicalPM=false;}
        primary=false;IOMobileFramebufferLegacy::stop(provider);OSSafeReleaseNULL(bootMapping);
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
        clearSourceMappings();
#endif
    }
    // The panel is already lit by the firmware's GOP framebuffer, which is the one thing the original code's power-state word (display+5840,
    // the "powered" half of the state pair next to +5836) is meant to say; the platform subclass that normally writes it is not part of this
    // port, so without it the user client's wait (selector 6) answers kIOReturnBusy forever and the compositor never presents. This is the
    // original state setter (virtual 354, the one the original code calls with this same request shape) with only the powered word named.
    void announcePoweredByFirmware() {
        struct StateRequest {uint32_t skipRequested,requested,skipPowered,powered;uint8_t changed;uint8_t pad[11];} request={1,0,0,1,1,{}};
        typedef int (*Setter)(void *,StateRequest *,bool);
        Setter setter=reinterpret_cast<Setter *>(*reinterpret_cast<void **>(this))[354];
        setter(this,&request,false);
    }
    bool start(IOService *provider) override {
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
        t733_display_power_start();
#endif
        if(!startDisplay(provider))return false;
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
        if (primary && bootMapping && !startSurfaceEvents()) {
            stop(provider);
            return false;
        }
        if (primary && bootMapping) {
            shutdownNotifier = registerPrioritySleepWakeInterest(
                shutdownNotification, this, nullptr);
            if (!shutdownNotifier) {
                stop(provider);
                return false;
            }
        }
#endif
        startVsync();return true;
    }
    bool startDisplay(IOService *provider) {
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
        if (!sourceMappingLock) {
            sourceMappingLock = IOLockAlloc();
        }
        if (!sourceMappingLock) {
            return false;
        }
        IOLockLock(sourceMappingLock);
        sourceMappingsEnabled = true;
        IOLockUnlock(sourceMappingLock);
#endif
        if(!IOMobileFramebufferLegacy::start(provider))return false;
        if(primary && bootMapping)announcePoweredByFirmware();
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-pm",&enabled,sizeof(enabled));
        if(!enabled)return true;
        unsigned phase=0;PE_parse_boot_argn("offline-legacy-phase",&phase,sizeof(phase));
        if(enabled!=1 || phase!=4 || !provider || !bootMapping || !primary) {
            native_legacy_pm_observed(1,kIOReturnNotReady,0);IOMobileFramebufferLegacy::stop(provider);return false;
        }
        // The framebuffer PM state controls the panel and touch coordinator.
        // The firmware video engine continues reading the retained framebuffer.
        static IOPMPowerState states[2]={};
        states[0].version=states[1].version=1;
        states[1].capabilityFlags=kIOPMDeviceUsable;
        PMinit();logicalPM=initialized;
        if(!logicalPM){native_legacy_pm_observed(1,kIOReturnNotReady,0);IOMobileFramebufferLegacy::stop(provider);return false;}
        provider->joinPMtree(this);
        IOReturn status=registerPowerDriver(this,states,2);native_legacy_pm_observed(1,status,0);
        if(status==kIOReturnSuccess){status=changePowerStateToPriv(1);native_legacy_pm_observed(2,status,1);}
        if(status!=kIOReturnSuccess){PMstop();logicalPM=false;IOMobileFramebufferLegacy::stop(provider);return false;}
        return true;
    }
    IOReturn setPowerState(unsigned long ordinal,IOService *device) override {
        if(!logicalPM)return IOMobileFramebufferLegacy::setPowerState(ordinal,device);
        if(ordinal>1 || !bootMapping){native_legacy_pm_observed(3,kIOReturnUnsupported,ordinal);return kIOReturnUnsupported;}
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
        IOReturn power = t733_display_set_power(ordinal != 0 && requestedAndPowered());
        if (power != kIOReturnSuccess && power != kIOReturnNotReady) {
            native_legacy_pm_observed(3, power, ordinal);
            return power;
        }
#endif
        native_legacy_pm_observed(3,kIOReturnSuccess,ordinal);
        IOReturn status=IOMobileFramebufferLegacy::setPowerState(ordinal,device);
        native_legacy_pm_observed(4,status,ordinal);return status;
    }
    // The explicit offline system-client experiment publishes only the real
    // GOP mode. Unknown backend calls retain their stop-with-caller guards.
    void registerService(IOOptionBits options=0) override {
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
        if (!startBacklight()) {
            IOLog("T733 display: NativeT733Backlight service could not start\n");
        }
#endif
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-register",&enabled,sizeof(enabled));
        if(enabled!=1)native_legacy_backend_guard(84,this,__builtin_return_address(0));
        // The platform publishes the selected panel interval before a client
        // can inspect IOMFBDisplayRefresh.
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if (primary && hz && hz <= 240) {
            native_legacy_set_refresh_properties(this);
        }
        IOService::registerService(options);native_legacy_registration_observed(this);
    }
    bool isMainDisplay() override {return primary;}
    bool isTimingEnabled() override {
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
        return t733_dpu_surface_timing_enabled();
#else
        return primary && bootMapping;
#endif
    }
    // IOMFBDisplayRefresh contains the selected interval in 32.32 seconds and
    // mach ticks. T733's 60 Hz video panel reports equal minimum and maximum
    // intervals; other modes use the original range unless fixed is requested.
    bool getRefreshAttributes(uint64_t *words,bool fixed) override {
        if(!IOMobileFramebufferLegacy::getRefreshAttributes(words,fixed) || !words)return false;
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if (!hz || hz > 240) {
            return true;
        }
        unsigned locked = 0;
        PE_parse_boot_argn("sheng-vsync-fixed", &locked, sizeof(locked));
        const uint64_t interval=(1ull<<32)/hz;
        uint64_t machTime=0;nanoseconds_to_absolutetime(interval*1000000000ull>>32,&machTime);
        words[0]=interval;words[1]=machTime;
        // A fixed-rate panel reports the same minimum and maximum interval.
        bool fixedPanel = fixed || locked == 1;
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
        fixedPanel = fixedPanel || hz == 60;
#endif
        if (fixedPanel) {
            words[2] = interval;
            words[3] = machTime;
        }
        native_legacy_vsync_observed(5,hz);
        return true;
    }
    // A swap the client submitted while the previous one was still on its way (transferred) is not kicked by the family again: without the
    // re-kick after each retirement it waited for the next submission (int-143: every offer came one frame late, a still screen showed the
    // previous frame until the user touched it again). A retirement inside a kick only asks the running kick to loop.
    bool kicking=false,rekick=false;
    void kickSwapQueue() override {
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
        IOWorkLoop *loop = getWorkLoop();
        if (!loop) {
            return;
        }
        loop->runAction([](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            static_cast<NativeLegacyDisplay *>(owner)->kickSwapQueueGated();
            return kIOReturnSuccess;
        }, this);
#else
        kickSwapQueueGated();
#endif
    }
    void kickSwapQueueGated() {
        armVsync();
        if(kicking){rekick=true;return;}
        kicking=true;
        do {
            rekick=false;
            kickOnce();
            gpuBypass=false;
        } while(rekick && !transferred);
        kicking=false;
    }
    void kickOnce() {
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
        if (displayStopping || dpuBlocked) {
            return;
        }
#endif
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-transfer",&enabled,sizeof(enabled));
        if(enabled!=1 || transferred || !logicalPM || !bootMapping || !primary)return;
        const OriginalLegacySwapRequest *request=pendingSwapRequest();
        if(!request || !isValidated(request))return;
        // The family owns this pending request until presentation completes.
        // DPU submission transfers its active surface read fence before the
        // display gate opens; producer acquisition finishes on another loop.
        OSObject *surface=nullptr;
        memcpy(&surface,reinterpret_cast<const unsigned char *>(request)+2624,sizeof(surface));
        if(!surface) {
            // Validated blank swap (all layers disabled over black): solid-fill the actual GOP.
            PE_Video blank={};
            uint32_t bw=0,bh=0;getDisplayDimensions(&bw,&bh);
            uint64_t blankLength=(uint64_t)bw*bh*4;
            if(PE_current_console(&blank) || blank.v_baseAddr!=bootMapping->getPhysicalAddress() ||
               blank.v_width!=bw || blank.v_height!=bh || blank.v_depth!=32 || blank.v_rotate ||
               !native_legacy_surface_bounds(bw,bh,blank.v_rowBytes,bootMapping->getLength(),bootMapping->getLength())) {
                native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
            }
            if(!native_legacy_prepare_scanout()) {native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;}
            if(!native_legacy_fill_pixels(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                bootMapping->getLength(),blank.v_rowBytes,bw,bh,0xff000000U)) {
                native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
            }
            __asm__ volatile("dsb sy" ::: "memory");
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
            if (t733_dpu_surface_active()) {
                shadowValid = false;
                holdDpuRequest(request, BlankCompletion, blankLength);
                beginDpuRestore(false);
                return;
            }
#endif
            shadowValid=false;
            transferred=true;
            native_legacy_frame_observed(1,0,0,0,0,0);
            {const uint32_t colors[4]={0xff000000U,0,0,0};const uint64_t counts[4]={(uint64_t)bw*bh,0,0,0};native_legacy_frame_colors_observed(colors,counts,0);}
            native_legacy_transfer_observed(kIOReturnSuccess,request,blankLength);
            finishTransfer(request);
            return;
        }
        if(!surface->metaCast("IOSurface"))return;
        IOMemoryDescriptor *memory=native_legacy_surface_descriptor(surface);
        PE_Video video={};
        uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
        uint64_t length=(uint64_t)width*height*4;
        uint64_t sourceStride=native_legacy_surface_stride(surface);
        uint64_t allocation=native_legacy_surface_allocation(surface);
        if(!memory || !memory->metaCast("IOMemoryDescriptor") || !native_legacy_surface_bounds(width,height,sourceStride,allocation,memory->getLength()) ||
           native_legacy_surface_width(surface)<width || native_legacy_surface_height(surface)<height ||
           !nativeBgra(native_legacy_surface_format(surface),native_legacy_surface_planes(surface)) ||
           PE_current_console(&video) || video.v_baseAddr!=bootMapping->getPhysicalAddress() ||
           video.v_width!=width || video.v_height!=height || video.v_depth!=32 || video.v_rotate ||
           !native_legacy_surface_bounds(width,height,video.v_rowBytes,bootMapping->getLength(),bootMapping->getLength())) {
            native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
        }
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
        if (!layerZeroFenceActive(request)) {
            return;
        }
        uint32_t producerFence = 0;
        if (!waitSurfaceProducer(request, memory, &producerFence)) {
            return;
        }
#endif
        // The diagnostic renderer repaints the page over the scanout until it is told the system owns the display; the GPU path needs that as much
        // as the CPU copy (int-093: accepted swaps were GPU-copied under a page that kept drawing).
        if(!gpuBypass) {
            if(!native_legacy_prepare_scanout()) {native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;}
        }
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
        if (presentDpuSurface(request, surface, memory, sourceStride, width, height, producerFence)) {
            return;
        }
#endif
        if(!gpuBypass && native_legacy_gpu_offer(this,memory,sourceStride,width,height)) {
            // The GPU writes the scanout memory behind the CPU's back: the differential copy's picture of it is stale from now on.
            native_legacy_swap_stage(1);
            shadowValid = false;
            gpuRequest = request;
            gpuLength = length;
            transferred = true;

            // The GPU presenter retains the source memory while the compositor
            // renders its next frame. transferred blocks its next submission
            // until finishGpuSwap reports completion of this copy.
            unsigned early = 1;
            PE_parse_boot_argn("sheng-early-retire", &early, sizeof(early));
            if (early == 1 && retireSwap(request, true)) {
                gpuRetired = true;
                native_legacy_early_retires = native_legacy_early_retires + 1;
            }
            return;
        }
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
        const uint64_t submissionStart = nativeLegacyCounter();
        bool sourceReused = false;
        uint64_t sourceHits = 0, sourceCreates = 0;
        IOMemoryMap *source = mapSource(surface, memory, allocation,
            sourceReused, sourceHits, sourceCreates);
        const uint64_t mappingEnd = nativeLegacyCounter();
#else
        IOMemoryMap *source=memory->map(kIOMapAnywhere|kIOMapReadOnly);
#endif
        if(!source || !source->getVirtualAddress() || source->getLength()<allocation) {
            OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnNoMemory,request,0);return;
        }
        if(!native_legacy_copy_layout(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
            bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
            source->getLength(),sourceStride,width,height)) {
            OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
        }
        // The two content scans read every pixel again; they only describe the first frames (the probe keeps 32).
        if(scanBudget) {
            --scanBudget;
            uint64_t scan[5];
            if(native_legacy_frame_scan(reinterpret_cast<const void *>(source->getVirtualAddress()),source->getLength(),sourceStride,width,height,scan))
                native_legacy_frame_observed(0,scan[0],scan[1],scan[2],scan[3],scan[4]);
            uint32_t colors[4];uint64_t counts[4],other=0;
            if(native_legacy_frame_colors(reinterpret_cast<const void *>(source->getVirtualAddress()),source->getLength(),sourceStride,width,height,colors,counts,&other))
                native_legacy_frame_colors_observed(colors,counts,other);
        }
        if(!native_legacy_prepare_scanout()) {
            OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
        }
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
        NativeLegacyCopyBinding copyBinding;
#endif
        const uint64_t copyStart = nativeLegacyCounter();
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
        uint64_t copyCpuStart;
        __asm__ volatile("mrs %0, mpidr_el1" : "=r"(copyCpuStart));
        bool pairedCopy = false;
#endif
        uint64_t stored=UINT64_MAX;
        {
            // Store only what changed since the previous frame (the scanout memory is slow device memory).
            uint64_t need=(uint64_t)width*height*4;
            // Differential copy is opt-in (sheng-fb-diff=1): on hardware it reads the whole source every frame and was not faster
            // than the plain copy (143-285 ms per swap either way), and the first boots with it were followed by an unresponsive
            // lock screen that disappeared with the plain copy (int-005).
            static int diffOff=-1;
            if(diffOff<0){unsigned v=0;PE_parse_boot_argn("sheng-fb-diff",&v,sizeof v);diffOff=(v!=1);}
            if(!diffOff && !shadow && need<=(64ull<<20)){shadow=IOMallocAligned((vm_size_t)need,16384);if(shadow){shadowBytes=need;shadowValid=false;}}
            if(!diffOff && shadow && shadowBytes>=need)
                stored=native_legacy_copy_diff(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                    bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
                    source->getLength(),sourceStride,width,height,shadow,shadowValid);
            if(stored!=UINT64_MAX)shadowValid=true;
        }
        if(stored==UINT64_MAX) {
            shadowValid=false;
            bool copied = false;
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
            if (pairedScanout) {
                copied = nativeLegacyCopyPairs(
                    reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                    video.v_rowBytes, reinterpret_cast<const void *>(source->getVirtualAddress()),
                    sourceStride, width, height);
                pairedCopy = copied;
            }
#endif
            if(!copied && !native_legacy_copy_pixels(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
                source->getLength(),sourceStride,width,height)) {
                OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
            }
            stored=length;
        }
        __asm__ volatile("dsb sy" ::: "memory");
        const uint64_t copyEnd = nativeLegacyCounter();
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
        uint64_t copyCpuEnd;
        __asm__ volatile("mrs %0, mpidr_el1" : "=r"(copyCpuEnd));
        copyBinding.restore();
        t733_display_mapping_observed(bootMapping->getVirtualAddress(),
            source->getVirtualAddress());
#endif
        OSSafeReleaseNULL(source);
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
        // The total includes source acquisition, layout handling, copying and
        // releasing the call's mapping reference; cached mappings remain live.
        // The original display family retires the request below this report.
        t733_display_submission_observed(submissionStart, mappingEnd-submissionStart,
            copyEnd-copyStart, nativeLegacyCounter()-submissionStart, stored,
            sourceReused, sourceHits, sourceCreates, pairedCopy, copyCpuStart, copyCpuEnd,
            copyBinding.cpu, copyBinding.ticks);
#else
        native_legacy_copy_timing_observed(copyEnd-copyStart,stored);
#endif
        transferred=true;
        native_legacy_transfer_observed(kIOReturnSuccess,request,length);
        finishTransfer(request);
    }
    // The GPU client reported its copy (status 0) or lost it. Success retires the swap like a finished copy; anything else uses the CPU copy.
    void finishGpuSwap(int status) {
        const OriginalLegacySwapRequest *request=gpuRequest;
        if(!request)return;
        gpuRequest=nullptr;
        if (gpuRetired) {
            // Retirement released the family's request. A failed GPU copy
            // drops this frame and sends the next queued frame through CPU copy.
            gpuRetired = false;
            if (status == 0) {
                native_legacy_transfer_observed(kIOReturnSuccess, request, gpuLength);
            } else {
                native_legacy_early_failures = native_legacy_early_failures + 1;
                native_legacy_gpu_fallback_observed(status);
                gpuBypass = true;
            }
            transferred = false;
            kickSwapQueue();
            return;
        }
        if(status==0) {
            native_legacy_transfer_observed(kIOReturnSuccess,request,gpuLength);
            finishTransfer(request,true);return;
        }
        native_legacy_gpu_fallback_observed(status);
        transferred=false;gpuBypass=true;kickSwapQueue();
    }
    // The family completes the pending request and releases its references.
    // Callers retain transferred until their CPU or GPU copy has finished.
    bool retireSwap(const OriginalLegacySwapRequest *request, bool force) {
        unsigned retire=0,cpuPresent=0;
        PE_parse_boot_argn("offline-legacy-retire",&retire,sizeof(retire));
        PE_parse_boot_argn("offline-legacy-cpu-present",&cpuPresent,sizeof(cpuPresent));
        if (!(retire == 1 || cpuPresent == 1 || force)) {
            return false;
        }
        if (!activateOnlyPendingSwap(request)) {
            return false;
        }

        removeValidated(request);
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
        if (producerRequest == request) {
            clearProducerRequest();
        }
#endif
        IOMobileFramebufferLegacy::completeCurrentSwap();
        native_legacy_retirement_observed(this);
        return true;
    }
    // CPU and blank-frame callers complete their stores before retiring the
    // request. transferred remains set while family completion can reenter.
    void finishTransfer(const OriginalLegacySwapRequest *request, bool force = false) {
        if (!retireSwap(request, force)) {
            return;
        }

        transferred = false;
        kickSwapQueue();
    }
    IOReturn collectCompletionReports(uint32_t frameID, OriginalLegacyFrameReport *reports,
        uint32_t *count, uint32_t capacity) override {
        if (count) {
            *count = 0;
        }
        if (reports && (!count || !capacity)) {
            return kIOReturnBadArgument;
        }

        IOWorkLoop *loop = getWorkLoop();
        if (!loop) {
            return kIOReturnNotReady;
        }
        if (!loop->inGate()) {
            struct Arguments {
                uint32_t frameID;
                OriginalLegacyFrameReport *reports;
                uint32_t *count;
                uint32_t capacity;
            } arguments = {frameID, reports, count, capacity};
            return loop->runAction([](OSObject *owner, void *argument, void *, void *, void *) -> IOReturn {
                auto args = static_cast<Arguments *>(argument);
                return static_cast<NativeLegacyDisplay *>(owner)->collectCompletionReports(
                    args->frameID, args->reports, args->count, args->capacity);
            }, this, &arguments);
        }

        if (!pendingFrameInfoValid) {
            return kIOReturnSuccess;
        }
        if (pendingFrameInfo.frameID() != frameID) {
            ++frameInfoStatistics.unmatchedQuery;
            return kIOReturnSuccess;
        }

        const OriginalLegacyFrameReport report = pendingFrameInfo;
        pendingFrameInfoValid = false;
        pendingFrameInfo = {};
        ++frameInfoStatistics.consumed;

        if (reports) {
            reports[0] = report;
            *count = 1;
        } else {
            native_legacy_frame_info_notify(this, &report);
        }
        return kIOReturnSuccess;
    }
    bool validateSwap(const OriginalLegacySwapRequest *request) override {
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
        IOWorkLoop *loop = getWorkLoop();
        if (!loop) {
            return false;
        }
        struct Validation {
            const OriginalLegacySwapRequest *request;
            bool accepted;
        } validation = {request, false};
        loop->runAction([](OSObject *owner, void *argument, void *, void *, void *) -> IOReturn {
            Validation *validation = static_cast<Validation *>(argument);
            validation->accepted = static_cast<NativeLegacyDisplay *>(owner)->validateSwapGated(validation->request);
            return kIOReturnSuccess;
        }, this, &validation);
        return validation.accepted;
#else
        return validateSwapGated(request);
#endif
    }
    bool validateSwapGated(const OriginalLegacySwapRequest *request) {
        // Qualified original request fields are packed; use memcpy for each
        // access. Accept only one full-frame, untransformed, nonplanar BGRA
        // surface with real original backing memory. No generic capability
        // or success is supplied for other layer/format/geometry contracts.
        auto bytes=reinterpret_cast<const unsigned char *>(request);
        auto word=[&](unsigned offset){uint32_t value=0;memcpy(&value,bytes+offset,4);return value;};
        OSObject *surface=nullptr;unsigned reason=1;bool accepted=false;
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-validate",&enabled,sizeof(enabled));
        if(enabled==1 && logicalPM && primary && bootMapping && request) {
            reason=2;OSObject *surfaces[4]={};memcpy(surfaces,bytes+2624,sizeof(surfaces));surface=surfaces[0];
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            if(surface && surface->metaCast("IOSurface") && !surfaces[1] && !surfaces[2] && !surfaces[3] &&
               native_legacy_single_layer_state(word(792),word(796),word(800))) {
                reason=3;bool rectangles=true;
                const unsigned rectOffsets[]={17,81};
                for(unsigned offset: rectOffsets) {
                    uint32_t rect[4]={};memcpy(rect,bytes+offset,sizeof(rect));
                    rectangles=rectangles && rect[0]==0 && rect[1]==0 && rect[2]==width && rect[3]==height;
                }
                bool options=true;for(unsigned offset=804;offset<=812;offset+=4)options=options && word(offset)==0;
                if(width && height && rectangles && options && word(816)==width && word(820)==height) {
                    reason=4;
                    IOMemoryDescriptor *memory=native_legacy_surface_descriptor(surface);
                    // A surface larger than the display (rows or lines padded to the GPU's alignment) is accepted: only the visible rectangle,
                    // which the request's rectangles already name as the whole display, is copied.
                    if(native_legacy_surface_width(surface)>=width && native_legacy_surface_height(surface)>=height &&
                       nativeBgra(native_legacy_surface_format(surface),native_legacy_surface_planes(surface)) &&
                       memory && memory->metaCast("IOMemoryDescriptor") &&
                       native_legacy_surface_bounds(width,height,native_legacy_surface_stride(surface),
                           native_legacy_surface_allocation(surface),memory->getLength())) {
                        reason=0;accepted=true;
                    } else {
                        native_legacy_surface_rejected(native_legacy_surface_width(surface),native_legacy_surface_height(surface),
                            native_legacy_surface_format(surface),native_legacy_surface_planes(surface),native_legacy_surface_stride(surface),
                            native_legacy_surface_allocation(surface),memory?memory->getLength():0);
                    }
                }
            }
        }
        if(!accepted && reason==2 && !surface && enabled==1) {
            // No surface in any slot and every updated layer disabled over black: present a blank screen.
            OSObject *none[4]={};memcpy(none,bytes+2624,sizeof(none));
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            bool options=true;for(unsigned offset=804;offset<=812;offset+=4)options=options && word(offset)==0;
            if(!none[0] && !none[1] && !none[2] && !none[3] && width && height && options &&
               word(816)==width && word(820)==height && native_legacy_blank_state(word(792),word(796),word(800))) {
                reason=5;accepted=true;
            }
        }
        if(accepted)addValidated(request);else removeValidated(request);
        native_legacy_swap_validation_observed(accepted,reason,request,surface);return accepted;
    }
    // The original slot-188 caller passes a uint32_t output and consumes an
    // IOReturn. Its implementation assumes an AppleARMIODevice provider.
    // Query our actual generic provider instead; GOP supplies no dot pitch.
    IOReturn getDotPitch(uint32_t *pitch) override {
        if(!pitch)return kIOReturnBadArgument;
        *pitch=0;IOService *provider=getProvider();if(!provider)return kIOReturnNotReady;
        OSObject *property=provider->getProperty("dot-pitch");
        if(!property)return kIOReturnNotFound;
        OSData *data=OSDynamicCast(OSData,property);
        if(!data || data->getLength()!=sizeof(*pitch))return kIOReturnBadArgument;
        memcpy(pitch,data->getBytesNoCopy(),sizeof(*pitch));return kIOReturnSuccess;
    }
    // Selector 74 returns key names and the client transport selection. Zero
    // selects the Mach notification path whose async reference slot 336 enables.
    IOReturn supportedFrameInfo(uint32_t index, uint32_t *count, char *key,
        uint32_t *transport) override {
        if (!count || !key || !transport) {
            return kIOReturnBadArgument;
        }

        *count = 0;
        *transport = 0;
        bzero(key, 64);
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
        if (primary && t733_dpu_surface_enabled()) {
            static const char *const keys[] = {
                "Presentation_time", "Requested_presentation"
            };
            *count = 2;
            if (index >= *count) {
                return kIOReturnBadArgument;
            }

            strlcpy(key, keys[index], 64);
            return kIOReturnSuccess;
        }
#endif
        return kIOReturnUnsupported;
    }

    bool usesSharedFrameInfoQueue() override {
        return false;
    }

    IOReturn setFrameInfoNotificationMask(uint32_t type,
        OriginalLegacyFrameInfoSubscription *subscription, uint32_t mask) override {
        if (type != 3) {
            return kIOReturnUnsupported;
        }
        if (!subscription || (mask & ~FrameInfoSupportedMask)) {
            return kIOReturnBadArgument;
        }

        storeFrameInfoMask(subscription, mask);
        ++frameInfoStatistics.maskChanges;
        return kIOReturnSuccess;
    }

    void frameInfoSubscriptionChanged(OriginalLegacyFrameInfoSubscription *subscription,
        bool enabled) override {
        if (!subscription) {
            return;
        }

        storeFrameInfoMask(subscription, 0);
        if (enabled) {
            ++frameInfoStatistics.subscriptions;
        } else {
            ++frameInfoStatistics.unsubscriptions;
        }
    }

    // The original sender holds the display gate and owns this subscription.
    // A full notification port drops only telemetry; request retirement proceeds.
    IOReturn sendFrameInfo(OriginalLegacyFrameInfoSubscription *subscription,
        const OriginalLegacyFrameReport *report) override {
        if (!subscription || !report) {
            return kIOReturnBadArgument;
        }

        const uint32_t mask = frameInfoMask(subscription) & FrameInfoSupportedMask;
        if (!mask || frameInfoStopped) {
            return kIOReturnSuccess;
        }

        io_user_reference_t arguments[4] = {mask, report->frameID(), 0, 0};
        UInt32 count = 2;
        if (mask & FrameInfoPresentationMask) {
            arguments[count++] = report->presentationTime();
        }
        if (mask & FrameInfoRequestedMask) {
            arguments[count++] = report->requestedPresentation();
        }

        auto bytes = reinterpret_cast<unsigned char *>(subscription);
        auto reference = reinterpret_cast<io_user_reference_t *>(bytes + 0x18);
        const IOReturn result = NativeLegacyFrameInfoSender::send(reference, arguments,
            count);
        frameInfoStatistics.lastSendResult = result;
        if (result == kIOReturnSuccess) {
            ++frameInfoStatistics.sent;
        } else if (result == MACH_SEND_TIMED_OUT) {
            ++frameInfoStatistics.queueFull;
        } else {
            ++frameInfoStatistics.sendErrors;
        }

        return kIOReturnSuccess;
    }
    IOReturn newUserClient(task_t task,void *security,UInt32 type,IOUserClient **client) override {
        native_legacy_client_enter(task,type);
        IOReturn status=IOMobileFramebufferLegacy::newUserClient(task,security,type,client);
        native_legacy_client_observed(status,status==kIOReturnSuccess && client?*client:nullptr);return status;
    }
    bool startHardware() override {
        unsigned enabled=0;
        PE_parse_boot_argn("offline-legacy-gop",&enabled,sizeof(enabled));
        if(enabled!=1)return false;
        PE_Video video={};IOService *provider=getProvider();
        if(!provider || PE_current_console(&video) || !video.v_baseAddr ||
           !video.v_width || !video.v_height || video.v_depth!=32 || video.v_rotate ||
           video.v_width>16384 || video.v_height>16384 || video.v_rowBytes<video.v_width*4 ||
           video.v_rowBytes>UINT64_MAX/video.v_height)return false;
        IOOptionBits cacheMode = kIOMapInhibitCache;
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
        unsigned writeCombine = 0;
        PE_parse_boot_argn("t733-fb-writecombine", &writeCombine, sizeof(writeCombine));
        if (writeCombine == 1) {
            // The console maps this reserved DRAM with the same Normal
            // Non-Cacheable attribute before either display mapping is used.
            cacheMode = kIOMapWriteCombineCache;
        }
#endif
        IOMemoryMap *mapping=provider->mapDeviceMemoryWithIndex(0,cacheMode);
        if(!mapping)return false;
        bool valid=mapping->getVirtualAddress() && mapping->getPhysicalAddress()==video.v_baseAddr &&
                   mapping->getLength()==video.v_rowBytes*video.v_height;
        if(valid) {
            installBootDimensions((uint32_t)video.v_width,(uint32_t)video.v_height);
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            valid=width==video.v_width && height==video.v_height;
            if(valid) {
                OSSafeReleaseNULL(bootMapping);bootMapping=mapping;mapping=nullptr;
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
                // T733 has one firmware scanout. v_display selects graphics or
                // verbose text at boot; both modes use this same primary panel.
                primary = true;
                pairedScanout = (bootMapping->getMapOptions() & kIOMapCacheMask) == kIOMapWriteCombineCache;
#else
                primary = video.v_display == 1;
#endif
                valid=native_legacy_gop_mapping_observed(this,bootMapping,width,height);
            }
        }
        OSSafeReleaseNULL(mapping);return valid;
    }
#define LEAF(n) __attribute__((noreturn,noinline)) void opaque##n() override { native_legacy_backend_guard(n,this,__builtin_return_address(0)); }
    LEAF(245)
    LEAF(246)
    LEAF(338)
#undef LEAF
};
OSDefineMetaClassAndStructors(NativeLegacyDisplay,IOMobileFramebufferLegacy)
IOService *native_legacy_display_allocate(void) {return new NativeLegacyDisplay;}
extern "C" void native_legacy_gpu_complete(void *display,int status) {if(display)static_cast<NativeLegacyDisplay *>(display)->gpuComplete(status);}
