# NativeMobileDisplay

Mobile display driver and GPU scanout hand-off. Validates swaps, retires frames and drives the firmware framebuffer.

Current state: the CPU and GPU scanout paths work. The GOP scanout has no vblank interrupt; with `sheng-vsync=<Hz>` a work-loop timer sends the original family's vsync message to the compositor.

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
when the previous surface can be released.

Linked into XNU by `unixnu build kernel`.
