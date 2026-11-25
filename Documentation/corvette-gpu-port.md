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

## 2026-11-26 vendor-DT and Metal divergence audit

- The clean vendor tree `/home/sakurakyuo/opdt` is at `a7ab73a78d71452a1f3bd4c04e2f8aa9ea4ea014` (`oneplus/sm8650_b_16.0.0_ace_3_pro`). Its `kernel_platform/qcom/proprietary/devicetree/qcom/pineapple.dtsi:1264-1278` defines `gpucc` at `0x3d90000` with size `0x9800`; line 2721 is only an empty `msm_gpu: qcom,kgsl-3d0@3d00000 { };` placeholder. The two Corvette overlay files searched contain no GPU/Adreno properties. These fragments do not provide the full A750 board/profile data.
- The current Metal source itself marks generation boundaries: `plugin/src/render.cpp:1206` says the A7xx `SP_PS_2D_WINDOW_OFFSET` is not represented by the A6xx path; `plugin/src/compute.cpp:232` and `plugin/src/texture.cpp:456` explicitly leave UBWC-through-IBO/storage-image paths unavailable before A7xx; `plugin/include/hakios-metal/a6xx.hpp:3578` records different A7xx texture/sampler slot handling. These are source comments, not proof that A7xx code is already implemented.
- Porting consequence: keep the current A6xx table immutable. Add a distinct A7xx register/descriptor backend and compiler target, then add fake-GPU fixtures for window offsets, texture/sampler slots and UBWC IBO semantics. The known A750 ID remains rejected until those paths and the Gen7 kernel profile are complete.

`components/metal/plugin/include/hakios-metal/gpu_identity.hpp` distinguishes the Corvette
MSM GPU_ID from the Gen7 GMU ID and labels `0x43051401` as A7xx for future routing. The
A7xx backend remains unavailable: `hakios_metal_chip_known()` still rejects it, so no A6xx
packet, descriptor, or compiler path can accidentally claim the device. `git diff --check`
passed. No kernel image was produced and no device or emulator was run. The next code step
is the Gen7 packet/descriptor/compiler backend plus the matching kernel profile, not enabling
`gpu=1` on the A660 path.

## 2026-11-26 full-device-tree correction and compiler-target change

### Device tree and incident evidence

The previous audit of `/home/sakurakyuo/opdt` did not include the separate local full-tree dump
`/home/sakurakyuo/corvette-full.dts` (SHA-256
`52bf18b84e20860d3316ee281ba9d0bcf7c4278f38a1b1e9641d55a755cb6c04`). The dump provides
additional A750 facts that should be used for the port, while remaining a local, untracked
artifact:

- `qcom,kgsl-3d0@3d00000`, lines 19296-19316, names `Adreno750v2`, has chip ID `0x43051401`
  and register windows `0x3d00000/0x40000`, CX debug `0x3d61000/0x3000`, RSCC
  `0x3d50000/0x10000`, and CX misc `0x3d9e000/0x2000`. It points its zap shader at reserved
  memory phandle `0x2fe`; that phandle still needs resolving before any secure-load attempt.
- `clock-controller@3d90000`, lines 13096-13107, is GPUCC at `0x3d90000/0x9800`.
- `qcom,gmu@3d69000`, lines 34927-34945, describes Gen7 GMU register window
  `0x3d68000/0x37000`, PDC `0x0b280000/0x10000`, and AO block `0x3d40000/0x10000`.
- `qcom,kgsl-iommu@3da0000`, lines 34899-34923, provides the GPU SMMU window
  `0x3da0000/0x40000` and distinct user/LPAC/secure contexts. The GPU speed-bin cell is at
  QFPROM offset `0x9b`, bits 4-6 (lines 20218-20221); the QFPROM base is `0x221c2000`.

This corrects only the claim that no full Corvette DTS evidence was available. The common
source branch still contains no Corvette board/SM8650 DTS package, and the dump alone does not
supply a verified end-to-end kernel profile, RPMh/GPUCC sequence, or safe zap reserved-memory
mapping.

For the SpringBoard incident, `.work/corvette-port-m4/device-springboard-reboot.txt` line 16
reports `gpu=0 cpus=1`, and line 474 says `SpringBoard: no GPU draws`. Lines 4881-4911 show a
backboardd `push_surface` assertion followed by backboardd signal 6; lines 4976/5016 show
runningboardd killing SpringBoard (signal 9), not a SpringBoard SIGABRT stack. Line 5193 is the
180-second `loader_return_after` expiry, about five seconds after the assertion. A later,
separate backboardd `userabort` at line 6566/uptime 407.224874 remains unsymbolized. This run
cannot diagnose A7xx submissions, fences, DeviceLost, or GPU OOM; do not attribute either
backboardd event to an active GPU driver from this log.

### Old-to-new implementation delta and applied first step

The four legacy commits are `6f90e14f`, `5ef4f3c3`, `f9b5c2aa`, and `ce8ad510`. The central
A750 bring-up is `5ef4f3c3`: it added Gen7 GMU/HFI/context setup, CP submission and retirement,
plus a separate NativeAdreno tunnel. The current common kernel remains an A660 implementation;
its `BoardGPU.cpp` contract is MSM DRM, so the legacy Native tunnel is not a drop-in ABI. The
new Metal plugin's `compiler.cpp` previously hard-coded Mesa `fd_dev_id.chip_id` to A635
`0x06030500`, even though Mesa 25.0.7 lists FD750 at `0x43051401` in
`src/freedreno/common/freedreno_devices.py:1289-1290`.

The plugin now maps the **MSM GPU_ID** `0x43051401` to Mesa's A750 `chip_id` for ir3 compiler
target selection, rebuilds each thread-local compiler if the selected target changes, and
rejects the Gen7 GMU firmware ID `0x07090000` as a compiler GPU ID. The hardware backend gate
is intentionally unchanged: `hakios_metal_chip_known(0x43051401)` remains false until kernel
submission and A7xx PM4/register/descriptor backends are implemented. This compiler-target
change alone does not enable GPU access or fix device frame rate.

### Verification and next change

- `make test` in `components/metal/plugin`: **303 tests, 0 failed checks**, exit 0. This is the
  host/FakeGPU build without Mesa; it tests target-ID mapping and refusal of the GMU ID, not
  Mesa's A750 compiler execution.
- The existing `build-softpipe-freedreno` was not compatible with the current Mesa source headers
  for `compiler-smoke` (generated format names differ). A clean `tools/mk-mesa-host.sh` attempt
  stopped in Meson setup because network requests for libxml2/WrapDB were reset/timed out. No
  A750 Mesa smoke result is claimed.
- `git diff --check` passes. No kernel/image build, QEMU, fastboot, adb, or UFS operation was run.
- All six Gen7 firmware files remain present under the original Corvette repository's `fw-gpu/`
  with the SHA-256 values recorded above. **No additional firmware is requested.**

Next: add a Corvette/SM8650 board data package from the verified full dump, resolve the zap memory
phandle and exact RPMh/GPUCC inputs, then port the old Gen7 startup/HFI/ring-retirement behavior
into the common kernel's MSM contract. Separately implement A7xx PM4, descriptor, and Mesa-target
paths in `components/metal/plugin`. Keep the A7xx hardware gate closed until both sides have
matching FakeGPU/host tests. Investigate `push_surface` and the later `userabort` separately from
GPU bring-up.
