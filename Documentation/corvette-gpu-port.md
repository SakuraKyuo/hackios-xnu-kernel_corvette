# Corvette Adreno 750 port status

Checked: 2026-11-26. This records the verified gap between the Corvette implementation
and the current common kernel/Metal tree. It is not a claim that the GPU is enabled.

## Device evidence

`D:/hackios-xnu-kernel/.work/corvette-port-m4/device-springboard-reboot.txt`:

- Line 16: `gpu=0 cpus=1` and `loader_return_after=180`.
- Line 474: SpringBoard reports `no GPU draws`.
- Line 4881 records a newly made IOSurface port immediately before the assertion on line 4882: `found != slist->end ()` in `push_surface` (`ogl-context.cpp:1309`). backboardd exits by signal 6 at line 4911.
- Lines 4976/5016 show SpringBoard being SIGKILLed by runningboardd and ending by signal 9 in the same broad process-termination burst; this is not a SpringBoard SIGABRT stack.
- Line 5193 records `loader_return_after expired`. The assertion wall-clock time is about 22:03:02, roughly 175 seconds after the boot-args timestamp at 22:00:07, so the 180-second loader return follows about five seconds later and confounds the black-screen observation.
- Line 6564 later records another backboardd `userabort` at uptime 407.224874; its cause is also not identified here.

Therefore this run provides no A7xx submit, fence, DeviceLost, or GPU-OOM evidence. The
assertion is in the userspace surface path; its cause is not identified by this log. The
180-second return occurs about 5.4 seconds after the backboardd assertion and confounds
the persistent-black-screen observation. Do not label this event a GPU reset.

## Current common implementation

- The `common` branch tracks no Corvette `board.dts` or SM8650 `soc.dtsi`; `git ls-files | grep -i corvette` lists only this document and `arch/arm64/configs/kconfig/corvette.config`. The config does not supply board GPU data. The earlier claim that a Corvette DTS listed `touch-synaptics` was incorrect and is withdrawn.
- `drivers/board/board.h` exposes `HAKIOS_HAS_GPU_A660`. `drivers/board/adreno_profile.h`
  rejects boards without that feature, and `drivers/board/adreno.c` expects the A660
  firmware names and A660 initialization path. There is no A750/Gen7 driver path here.
- `components/metal/plugin/include/hakios-metal/a6xx.hpp` defines chip ID `0x06030500`;
  `src/compiler.cpp` hard-codes it for ir3. `include/hakios-metal/gpu_identity.hpp` now
  classifies the legacy Corvette MSM GPU_ID `0x43051401` as A7xx while keeping it
  unavailable to the A6xx backend; GMU chip ID `0x07090000` is explicitly not treated as
  the MSM GPU_ID. This prevents mistaken A6xx dispatch; it does not enable rendering.
- Current `drivers/iokit/BoardGPU.cpp` is the MSM DRM transport used by this source plugin.
  The old `NativeAdreno` tunnel is a separate ABI; its client-private VA sizes and ioctl
  behavior must not be copied as if they were current MSM behavior.

## Legacy implementation to adapt

The requested reference range is `6f90e14f..ce8ad510`:

- `5ef4f3c3` adds the Corvette Adreno 750 / Gen7 kernel path, including its board data,
  GMU/HFI/CP setup and NativeAdreno tunnel integration.
- `f9b5c2aa` integrates the then-current Native Metal desktop and widgets.
- `ce8ad510` records the tested scope and limitations.

The legacy driver distinguishes the GMU chip value `0x07090000` from the GPU ID exposed
through MSM (`0x43051401`). Keep those identifiers separate. Port the Gen7 register,
firmware and submission behavior into the common driver's profile/API; do not label the
A750 as A660 or reuse the A6xx packet/register tables. The new source plugin is the current
MSM client, so the NativeAdreno tunnel is not itself a drop-in fix for it.

The legacy `6f90e14f..5ef4f3c3` Adreno patch changes the old driver by 2,652 insertions and
349 deletions. A zero-fuzz dry run against `drivers/board/adreno.c` applies only 50 of 90
hunks; adapt the Gen7 changes by subsystem rather than applying that file diff wholesale.

## Required work sequence

1. Add a distinct Gen7 board feature/profile and map only values supported by the SM8650
   board data. Gate GPU register access until the profile and its firmware are complete.
2. Adapt the legacy A7xx startup, GMU/HFI, ring submission and retirement logic to the
   current `drivers/board/adreno.c` and `drivers/iokit/BoardGPU.cpp` contracts. Keep the
   MSM ABI coherent with the plugin; do not mix NativeAdreno-only methods into MSM calls.
3. Add an A7xx backend in `components/metal/plugin`: chip selection, PM4/register tables,
   descriptor formats and Mesa/ir3 compiler selection. Keep A6xx tests unchanged; add fake
   GPU tests for the A7xx-specific words and refusal paths before enabling the feature.
4. Run the host Metal suite and board/profile tests. Then make a device image with one GPU
   variable changed, no QEMU, no fastboot/adb operation by the agent, and no UFS writes.
   The user performs hardware testing. Capture chip/GMU IDs, initialization stage, command
   submits, fence times, frame rate, memory refusals and process exits.
5. Investigate `push_surface` separately with a symbolized userspace stack and matching
   MiniIOSurface/framebuffer lifecycle evidence. Do not combine it with GPU bring-up unless
   a GPU-enabled run reproduces it.

## Firmware already present locally

The six files below exist in `/home/sakurakyuo/hackios-xnu-kernel_corvette/fw-gpu/` and
must remain out of source commits. Their SHA-256 values are:

| File | SHA-256 |
| --- | --- |
| `gmu_gen70900.bin` | `9685f62c42543befcaee68d25355e16bf55cdb0d573bddcc9f6633f59eec3f72` |
| `gen70900_sqe.fw` | `f756bafe5f69a87161c7c53385386374a791861a574c244e845baba677993842` |
| `gen70900_aqe.fw` | `25237c161d727b8ea92b4beb837a31651b44ea78bfc34d95672af8f346a45825` |
| `gen70900_zap.mdt` | `440b907703560015b551d5ba70d002dd9160471d5ee456b38bc8a6df4c98610d` |
| `gen70900_zap.mbn` | `0fe84cfae8bb92335dd37d31729278a39872b82a193ff90432f7ffc691bb95bf` |
| `gen70900_zap.b02` | `bb8e56248f7ae112e8df6a1a365afe616ab24f9faf330b4b94115b3d7efa998f` |

No additional firmware upload is needed at this stage. The image builder still needs an
explicit Gen7 firmware mapping before `gpu=1` can be enabled.

## Host test result

On 2026-11-26, after the GPU-ID routing change below, `make test` in
`components/metal/plugin` completed with 303 tests and 0 failed checks. This was a host/fake-
GPU run without Mesa enabled; it is not A7xx or device validation. The post-test worktree
contained no generated fixture changes.

## 2026-11-26 identity-routing checkpoint

`components/metal/plugin/include/hakios-metal/gpu_identity.hpp` distinguishes the Corvette
MSM GPU_ID from the Gen7 GMU ID and labels `0x43051401` as A7xx for future routing. The
A7xx backend remains unavailable: `hakios_metal_chip_known()` still rejects it, so no A6xx
packet, descriptor, or compiler path can accidentally claim the device. `git diff --check`
passed. No kernel image was produced and no device or emulator was run. The next code step
is the Gen7 packet/descriptor/compiler backend plus the matching kernel profile, not enabling
`gpu=1` on the A660 path.
