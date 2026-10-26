# NativeMobileDisplay

Mobile display driver and GPU scanout hand-off. Validates swaps, retires frames and drives the firmware framebuffer.

On SM-T733, `t733-dpu-scanout=1` lets DPU scan the composited IOSurface after its
GPU producer completes. Four cached 32 MiB IOVA ranges retain prepared surface
descriptors and page tables. Presenting a cached surface still cleans its pages;
the old surface's active read fence completes only after CTL flush clears and
the display frame counter advances. Eviction and firmware restoration remove
the mappings and release the descriptors. The mapping reuse follows
[GopFramebuffer 33e941a](https://github.com/UniXNU/GopFramebuffer/blob/33e941a878dcf01b98dbd9160d798bd56e7551ea/NativeLegacyDisplay.cpp#L158).

`native_display_profile_dump` prints one summary and eight cumulative stage rows
with a common `snapshot` number. They report cache hits, misses and eviction
attempts, plus counts, total elapsed time and maximum elapsed time for mapping,
unmapping, surface cache cleaning, producer wait, submission, scanout wait,
read-fence release and complete presentation. Timing uses `mach_absolute_time`,
so scheduling delays remain included. Normal presentation emits no per-frame
timing rows. CPU copying remains available through the firmware framebuffer.

With `sheng-vsync=<Hz>`, the display work-loop timer observes the hardware frame
counter and sends the original family's vsync message to the compositor.

Linked into XNU by `unixnu build kernel`.
