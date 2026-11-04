# corvette GPU: basic desktop milestone

As of 2026-11-04, Adreno 750 (SM8650/gen70900) runs real Native Metal work through
the XNU NativeAdreno tunnel. Desktop composition and widgets are usable in the
tested iOS 16.5 environment. This closes the basic bring-up phase; full iOS
Metal/media compatibility and long-term stability remain open.

## Evidence and limits

- Mesa/AIR readback matched its CPU reference; 71 submissions/70 swaps crossed the
  former 64-retirement queue limit. Later Native Metal source shaders and imported
  IOSurface panel tests also completed, including asynchronous same-surface reuse.
- Per-client low-4-GiB VA reservations prevent clients overwriting one another's
  PTEs. Busy BoFree retains its BO/IOVA until real completion; pending releases
  keep a fixed last-use fence. backboardd reserves 512 MiB of VA, other clients
  256 MiB each. These are address reservations, not physical RAM allocations.
- Desktop respawns stopped in the retained short tests after those fixes. The
  actual backboardd mappings crossed the former 256 MiB VA ceiling. This does not
  prove unbounded lifetime, absence of leaks, or all OOM paths fixed.
- Widgets were visually confirmed working after chronod selected RenderBox's own
  extended mode, using real RGBA16Float (115). The adapter sets RB_COLOR_MODE only
  for chronod with the actual Native accelerator; it does not reinterpret the
  private dual-plane format 542 as a standard texture format.
- The last standalone RGBA16 numerical probe failed at device acquisition
  (`rgba16 FAIL no device`), before resource creation or GPU submission. It is
  **not a passing pixel comparison** and does not itself establish a GPU hang.
- Native media reflection (`NBFunction.returnType`), LibraryKind 1, HostMemory,
  layout/capability gaps and some pipeline/function compatibility remain open.
  Foreign-function errors can include a nil vertex probe; absence in one log does
  not prove all pipelines work. No fabricated capabilities/return types are added.
- A previous warm rerun entered 900E after `oem run`; a separate 60–90 second
  failure had no retained log. Cause is unresolved. Warm reruns are suspended;
  this is not a general device-recovery or full-system stability release.

## Kernel integration

The source of truth is `xnu/files/`; synchronize overlays before compiling into
`xnu-src/xnu-8796.121.2`. `xnu/apply.sh` installs overlays when preparing the XNU
tree; an already prepared tree needs updated files copied again. The changes
include the GPU, input, MiniIOSurface/framebuffer overlays, input header and GIC
redistributor stride support. Do not edit only the generated XNU copy.

The context queue must reach memory before the GMU doorbell. Fence polling drains
HFI F2H queues rather than merely clearing an interrupt: queue 1 holds only 64
aligned retire messages. Keep hwsched's IFPC exclusion, BR-thread fence trailer,
and the A7xx boot probe's equal-size NOP replacing its unpaired ZPASS query.

The dispatch word at index 7 is the legitimate `big_ib_gmu_va` field, not an
extraneous zero. Type-7 PM4 opcodes are **7 bits**; packet definitions come from
the locally checked-in A7xx XML and Qualcomm's matching vendor sources.

`gpu_redirect=0` is required for real client rendering. The default redirect is
still a kernel diagnostic. Never write GX GDSCR POWER_OFF from the host; GMU owns
that domain. Do not carry the redwood `wifi=1` path into a corvette boot.

The orange/white/blue GPU self-test continues drawing and checking memory, but no
longer copies its picture to the panel during normal startup. Request
`gpu_test_pattern=1` explicitly to show it. This final display-policy change is
host-built/checked; no new device validation is claimed by this source commit.

## Local inputs and provenance

The external plugin and Vulkan binaries are **not distributed by this change**.
Their source/licensing provenance is not fully established in this repository;
hashes identify the tested input, not a license or a claim that the external
backend is reproducibly built from these sources. Supply your own authorized
copies at `prebuilt/Extensions/NativeAdrenoMetal.bundle/`:

| Input | Bytes | SHA256 |
| --- | ---: | --- |
| NativeAdrenoMetal | 22237984 | `47eb3a6bb536ca8ec59073c441215ca3ee3dfe7442d4b5951fa7eabed32522ce` |
| libvulkan_freedreno.dylib | 17340192 | `8d7b560fa2b2b8114321a5c236565776f51ee96df437220adbb3e8a2163a8725` |

Keep the original bundle Info.plist (principal class NativeAdrenoMetalDevice),
NativeAdrenoMetal.publish (`1\n`) and NativeAdrenoMetal.clients (`*\n`) beside/in
the bundle as supplied by the tested setup. The kernel exec-injection list is a
separate **exact process allowlist**, not a wildcard.

`tools/native-metal-variant.py INPUT OUTPUT` checks the complete input hash and
ad-hoc CodeDirectory hashes, applies the two reviewed factory/UChar3 corrections,
and regenerates affected code-page hashes. It refuses CMS-signed or unknown inputs
and preserves the original. The result must have SHA256
`2a01d8997eddbc59294326dddb636dcc5d295a53c3a385970ac678eceb312fe5`, identical to
the device-tested variant. UChar3 stays Metal type 51; the source adapter handles
its padded constant bytes. This tool is version-specific, not a generic patcher.

Apple framework shaders/resources must come from the builder's matching iOS 16.5
20F66 extraction. They are not committed, replaced with unrelated libraries, or
made by this helper. `METAL_RESOURCES` points to a directory containing:

```text
FrameworkResources/QuartzCore.framework/default.metallib
FrameworkResources/CoreImage.framework/*.metallib
FrameworkResources/MetalPerformanceShaders.framework/Frameworks/MPSCore.framework/default.metallib
FrameworkResources/MetalPerformanceShaders.framework/Frameworks/MPSImage.framework/default.metallib
FrameworkResources/MetalPerformanceShaders.framework/Frameworks/MPSFunctions.framework/default.metallib
FrameworkResources/MetalPerformanceShaders.framework/Frameworks/MPSMatrix.framework/default.metallib
PrivateFrameworkResources/RenderBox.framework/default.metallib
```

Provide the existing iOS System image, shared cache, launchd jobs and firmware via
the project's usual local extraction/build process. In particular cache_probe
and private framework offsets are qualified against the tested 20F66 cache
(UUID `e5420db330d033aa8b7e05b5038bb46d`), not every iOS version. GPU firmware is
the phone's own gen70900 GMU/SQE/ZAP set. No private assets belong in a source commit.

## Build and deploy into RAM root

With the cross toolchain, iPhoneOS16.5 SDK, prepared XNU tree and local inputs:

```sh
make kernel JOBS=8
# Build the usual base RAM root with your selected iOS jobs and original resources:
NATIVE_METAL=1 METAL_RESOURCES=/path/to/local/resources bash userland/build.sh
# Or rebuild and stage just this integration in an existing qualified RAM root:
METAL_RESOURCES=/path/to/local/resources bash userland/build-native-metal.sh root
python3 tools/apfs/mkapfs.py -z --plain fw -o userland/root.apfs redwood=userland/root
python3 xnuloader/mkpayload.py -o run-corvette-gpu.bin \
  kernel=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837 \
  ramdisk=userland/root.apfs \
  --cmdline 'touchscreen=1 touch_board=1 bootcpu=7 gpu=1 gpu_frame=0 gpu_log=0 gpu_hwsched=1 adreno_dms=0 gpu_redirect=0'
devices/corvette/mkflashable.py       # generates only boot-v4.img
```

The staging helper's `root` argument is relative to `userland/`. It verifies the
external binary pair, builds cache_probe, the observation tool, injection libraries
and the Native source/system adapter, and updates the exact process allowlist.
It requires real framework resources and does not regenerate the base System or
job selection. `METAL_TESTS=1` additionally builds manual smoke, panel, vec3 and
RGBA16 programs; it installs no auto-start test job or timed halt. It does not
alter any existing jobs; disable legacy test RunAtLoad entries when reusing a
diagnostic root. The release RAM image has those entries disabled.

The existing post-link symbolset/check_all_exports failure may return make exit 2.
Check the new linked Mach-O, markers and full compiler output; do not treat every
exit 2 as success. A build is not device validation.

All device commands are performed by the owner. Reboot into the bootloader for
clean SoC state, then stage System as md0 before the kernel/RAM-root payload.
`oem stop` and a full download do not reset registers. While running, request
`oem log` before get_staged; after returned, get_staged directly. Never write
userdata, metadata, init_boot or vendor_boot; no erase, `-w` or `ufs_write=1`.

Normal tracing is quiet: kernel `gpu_debug=1`, `gpu_log=1`, `fbtrace=1`,
`touch_debug=1` and user environment `HAKIOS_GPU_DEBUG=1` opt into diagnostics.
Rejects, failures and CPU faults remain visible. The observation tool reports PID
changes and new error bytes; it does not stop or reboot the device.
