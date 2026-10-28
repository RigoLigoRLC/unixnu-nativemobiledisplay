# NativeMobileDisplay

Mobile display driver and GPU scanout hand-off. Validates swaps, retires frames and drives the firmware framebuffer.

The CPU and GPU scanout paths share the display work loop. On SM7325, the MDSS
interrupt records INTF1 vblank, frame count and CTL0 flush state, then wakes that
work loop. With `sheng-vsync=<Hz>`, the work loop sends the original family's
vsync message from those observations. The firmware framebuffer path uses a
timer when the platform supplies no display interrupt.

On SM7325, `t733-dpu-scanout=1` maps each prepared IOSurface into display SMMU
CB4 and switches VIG0 through CTL0. CPU write-back and write-through pages use
the coherent WB MAIR entry; Normal NC pages use the NC entry. Managed RT pages
use NC when the selected `t733-managed-rt-normal=1` XNU policy applies. These
attributes come from the physical pages supplied by the IOSurface descriptor.

CB4 reads its CPU write-back page tables with outer-shareable WBWA walks,
matching the original SM7325 apps SMMU configuration. Store barriers publish
table updates before TLB synchronization and display submission. The driver
performs no CPU cache clean or invalidate for surfaces or tables. The retained
IOSurface read fence, cleared CTL flush word and advancing frame counter govern
when the previous surface can be released. A GPU scheduler read reservation
waits for writes to the surface's physical backing and prevents later GPU writes
while DPU scans it. A successful switch releases the previous surface's
reservation. CPU-produced surfaces are ready without a GPU writer dependency.

Linked into XNU by `unixnu build kernel`.

`native_legacy_frame_profile_dump` copies the latest 512 completed DPU requests
from a fixed history and prints them after releasing the history lock. The
render and presentation paths only save values. The requested GPU diagnostic
invokes this dump; collecting frame records adds no per-frame console output.

Each `T733 frame-profile:` row carries its history sequence, the native frame
ID, completion type, result, validity bits and continuous-clock microseconds.
`composition_start_us` comes from the request's type-2 timestamp at `+0x2e0`:
QuartzCore records it at the end of `begin_update`, before rendering, and its
IOMobileFramebuffer input path converts it to the continuous clock. The swap's
issued and requested presentation times retain their separate meanings.
`accepted_us` records validation, `read_fence_active_us` records the first
positive observation of the request's active IOSurface read fence, and
`producer_ready_us` records completion of the GPU writers captured for the
surface's backing. Their callback wakes the display work loop directly.

The Adreno profile matches the scanout descriptor to imported buffer backing
and selects direct writes submitted since the valid native composition start.
Each `T733 frame-write:` row names an exact submission sequence and fence for
that frame record. Offscreen work remains in the separate GPU submission
history. `gpu_complete_observed_us` is the CPU's first recorded observation that
the last matching write completed. `dpu_submit_us` is sampled immediately before
the CTL0 flush write. The actual DPU switch lies between `display_lower_us` and
`display_upper_us`, ending at the first clear-flush and advanced-frame observation.
These observation bounds remain visible when calculating generation and display
latency. An interrupt observation includes interrupt delivery latency; the
fallback hardware poll includes its scheduling delay.

The `valid` mask uses bits 0 through 7 for composition start, issued time,
requested time, acceptance, active read fence, completed surface producer wait,
successful GPU metadata query, and DPU bounds, respectively. A zero or future
composition start is preserved in the row without its validity bit. GPU query
status and `gpu_flags` describe absent, expired, incomplete or truncated source
write metadata. The completion values are 1 for a surface, 2 for blanking, 3 for
firmware fallback, and 4 for a dropped surface. The summary states the history
capacity and overwrite count, so a capture can account for gaps.
