#include "NativeLegacyRequest.h"
// SPDX-License-Identifier: BSD-2-Clause-Patent
// Reuse the unchanged original generic framebuffer implementation. Unknown
// backend leaves stop; this class is not yet a published system display.
#include "OriginalLegacyFramebufferABI.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <kern/clock.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMpowerState.h>
#include <libkern/c++/OSData.h>
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
extern "C" bool t733_display_bind_begin(void **previous, int *cpu);
extern "C" void t733_display_bind_end(void *previous);
extern "C" void t733_display_submission_observed(uint64_t start, uint64_t mapTicks,
    uint64_t copyTicks, uint64_t totalTicks, uint64_t bytes,
    bool reused, uint64_t hits, uint64_t mappings, bool paired,
    uint64_t cpuStart, uint64_t cpuEnd, int boundCpu, uint64_t bindTicks);
extern "C" void t733_display_mapping_observed(uint64_t scanout, uint64_t surface);
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
// 3 late (deadline passed, cadence restarted), 4 started, 5 refresh properties reported at that rate.
extern "C" void native_legacy_vsync_notify(void *display,uint64_t timestamp,uint64_t continuousTime,uint64_t interval);
// The unchanged original setDisplayRefreshProperties: publishes IOMFBDisplayRefresh from slot 301 (getRefreshAttributes below).
extern "C" bool native_legacy_set_refresh_properties(void *display);
extern "C" void native_legacy_vsync_observed(unsigned event,unsigned hz);
extern "C" uint64_t mach_continuous_time(void);
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
    bool isValidated(const OriginalLegacySwapRequest *request) const {
        if(!request)return false;
        for(auto entry: validated)if(entry==request)return true;
        return false;
    }
    void removeValidated(const OriginalLegacySwapRequest *request) {for(auto &entry: validated)if(entry==request)entry=nullptr;}
    void addValidated(const OriginalLegacySwapRequest *request) {
        if(isValidated(request))return;
        for(auto &entry: validated)if(!entry){entry=request;return;}
        validated[0]=request;
    }
    const OriginalLegacySwapRequest *gpuRequest=nullptr;
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
        if(vsyncNext<=now){vsyncNext=now+vsyncInterval;native_legacy_vsync_observed(3,vsyncHz);}
        vsyncArmed=true;vsyncTimer->wakeAtTime(vsyncNext);
    }
    void startVsync() {
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if(!hz || hz>240 || !primary || !bootMapping || vsyncTimer)return;
        IOWorkLoop *loop=getWorkLoop();
        if(!loop)return;
        IOTimerEventSource *timer=IOTimerEventSource::timerEventSource(this,&NativeLegacyDisplay::vsyncFired);
        if(!timer)return;
        if(loop->addEventSource(timer)!=kIOReturnSuccess){timer->release();return;}
        nanoseconds_to_absolutetime(1000000000ull/hz,&vsyncInterval);
        vsyncHz=hz;vsyncTimer=timer;native_legacy_vsync_observed(4,hz);
        // A client may already have registered: arm under the gate, like the registration does.
        loop->runAction([](OSObject *owner,void *,void *,void *,void *)->IOReturn {
            static_cast<NativeLegacyDisplay *>(owner)->armVsync();return kIOReturnSuccess;
        },this);
    }
    void stopVsync() {
        if(!vsyncTimer)return;
        vsyncTimer->cancelTimeout();
        if(IOWorkLoop *loop=getWorkLoop())loop->removeEventSource(vsyncTimer);
        OSSafeReleaseNULL(vsyncTimer);vsyncArmed=false;
    }
public:
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
        stopVsync();
        if (logicalPM) {
            PMstop();
            logicalPM = false;
        }
        primary = false;
#if defined(NATIVE_T733_FIRMWARE_DISPLAY) && NATIVE_T733_FIRMWARE_DISPLAY
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
        stopVsync();
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
        if(!startDisplay(provider))return false;
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
        // Actual IOKit PM lifecycle for logical scanout scheduling only. GOP
        // provides no panel power switch: neither state advertises physical
        // power output, power control or a hardware power transition.
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
        native_legacy_pm_observed(3,kIOReturnSuccess,ordinal);
        IOReturn status=IOMobileFramebufferLegacy::setPowerState(ordinal,device);
        native_legacy_pm_observed(4,status,ordinal);return status;
    }
    // The explicit offline system-client experiment publishes only the real
    // GOP mode. Unknown backend calls retain their stop-with-caller guards.
    void registerService(IOOptionBits options=0) override {
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-register",&enabled,sizeof(enabled));
        if(enabled!=1)native_legacy_backend_guard(84,this,__builtin_return_address(0));
        // Nothing in the family publishes IOMFBDisplayRefresh (its platform subclass does), and without it the render server keeps its own
        // 1/60 s heartbeat (int-210: refresh=0). Publish it before any client can find the display, only when sheng-vsync asks for more.
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if(primary && hz>60 && hz<=240)native_legacy_set_refresh_properties(this);
        IOService::registerService(options);native_legacy_registration_observed(this);
    }
    bool isMainDisplay() override {return primary;}
    // The render server paces its display timer by the minimum refresh interval in IOMFBDisplayRefresh (int-209: no vsync client ever
    // registered, frames came every 16.7 ms), and the generic slot 301 fixes it at 1/60 s. With sheng-vsync=<Hz> it reports the panel's
    // own rate instead; the maximum interval and the step keep the original values. Words in 32.32 seconds, then their mach times.
    bool getRefreshAttributes(uint64_t *words,bool fixed) override {
        if(!IOMobileFramebufferLegacy::getRefreshAttributes(words,fixed) || !words)return false;
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if(hz<=60 || hz>240)return true;
        unsigned locked = 0;
        PE_parse_boot_argn("sheng-vsync-fixed", &locked, sizeof(locked));
        const uint64_t interval=(1ull<<32)/hz;
        uint64_t machTime=0;nanoseconds_to_absolutetime(interval*1000000000ull>>32,&machTime);
        words[0]=interval;words[1]=machTime;
        // A fixed-rate panel reports the same minimum and maximum interval.
        if (fixed || locked == 1) {
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
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-transfer",&enabled,sizeof(enabled));
        if(enabled!=1 || transferred || !logicalPM || !bootMapping || !primary)return;
        const OriginalLegacySwapRequest *request=pendingSwapRequest();
        if(!request || !isValidated(request))return;
        // This bounded first-frame transfer preserves the original queue's
        // resource ownership. Separately gated retirement follows the copy;
        // neither path signals a hardware fence, announces a vblank or
        // claims general producer synchronization.
        // The owned GPU instrument completes its work before submission.
        // The separate original software-renderer experiment uses completed
        // synchronous CPU surfaces, with no GPU or display fence fabricated.
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
        // The diagnostic renderer repaints the page over the scanout until it is told the system owns the display; the GPU path needs that as much
        // as the CPU copy (int-093: accepted swaps were GPU-copied under a page that kept drawing).
        if(!gpuBypass) {
            if(!native_legacy_prepare_scanout()) {native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;}
        }
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
    IOReturn collectCompletionReports(uint32_t,void *,uint32_t *count,uint32_t) override {
        // The original completion helper passes null optional outputs and
        // ignores this IOReturn. GOP exposes no hardware completion telemetry.
        if(count)*count=0;
        return kIOReturnUnsupported;
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
    // Selector 74 enumerates optional frame-info trace keys, not display
    // modes. This backend implements no trace-key subscription contract.
    IOReturn supportedFrameInfo(uint32_t,uint32_t *count,char *key,uint32_t *mask) override {
        if(!count || !key || !mask)return kIOReturnBadArgument;
        *count=0;*mask=0;bzero(key,64);return kIOReturnUnsupported;
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
    LEAF(211)
    LEAF(245)
    LEAF(246)
    LEAF(306)
    LEAF(307)
    LEAF(338)
    LEAF(345)
#undef LEAF
};
OSDefineMetaClassAndStructors(NativeLegacyDisplay,IOMobileFramebufferLegacy)
IOService *native_legacy_display_allocate(void) {return new NativeLegacyDisplay;}
extern "C" void native_legacy_gpu_complete(void *display,int status) {if(display)static_cast<NativeLegacyDisplay *>(display)->gpuComplete(status);}
