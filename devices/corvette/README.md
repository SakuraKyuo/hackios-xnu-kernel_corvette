# Device: OnePlus Ace 3 Pro (corvette)

Everything in HakiOS that belongs to one phone is here. The kernel, loader and user space above
know nothing about the phone: addresses, interrupts, power rails and firmware come from its device
tree and its Android.

| What | Where |
|---|---|
| SoC | Qualcomm Snapdragon 8 Gen 3 (SM8650, "pineapple" in the device tree) |
| UEFI for the phone | `uefi/`: a port of Mu-Silicium (a patch and files; `apply.sh` clones and applies, `export.sh` saves) |
| Prebuilt UEFI images | `prebuilt/`: `SILICIUM_UEFI.fd` (3 MB), `BootShim.bin`, the device tree `device-tree.dtb` (the phone's Android kernel's) |
| Images to flash | `mkflashable.py`: `boot-v4.img` only |
| The phone's firmware | **not in the repository**: `pull-firmware.sh` pulls it off your phone over `adb` |

The touchscreen is a **Synaptics S3910** (TCM over SPI), not the Goodix GT9916S of the POCO: the
driver is in `xnu/files/bsd/dev/hakios.goodix.c` and picks this phone's numbers in `board_corvette`
(raw full scale 12640 x 27800: the chip counts tenths of a pixel; MOVE reports are capped at 125 Hz
because the compositor cannot take more).

## GPU: basic accelerated desktop milestone

Adreno 750 / gen70900 can run the Native Metal backend, composite the desktop,
and draw widgets. The kernel draw/readback check stays enabled; its orange/white/blue
panel pattern is now opt-in (`gpu_test_pattern=1`). Normal GPU traces and touch
position/periodic logs are quiet by default; errors remain visible.

This is a bring-up milestone, not complete iOS Metal/media compatibility or a
long-term stability certification. See [GPU build, evidence and remaining work](../../docs/corvette-gpu.md)
for the exact external binary inputs, reproducible deployment and known limits.

## The memory map

Taken from the phone's own `xbl.img` (`MemoryMapLib`), not from the carveout list of the device tree:

| | Address |
|---|---|
| UEFI FD | base `0xA7100000`, size `0x300000` |
| Loader / kernel payload | `0xA8000000` |

`BootShim.bin` and `xnuloader/loader.c` (`PAYLOAD_BASE`) are built for those, and `mkxnuimg.py`
takes them from the environment: `FD_BASE`, `FD_SIZE`, `PAYLOAD_BASE`, `UEFI_TARGET=corvette`.
The FD the shim copies is checked to be exactly `FD_SIZE` bytes, so a mismatched UEFI build stops
the packing instead of producing an image that will not boot.

## Building

From the top of the repository:

```sh
make kernel JOBS=8                        # xnu-src/, DEVICE-independent
make loader                               # needs xnuloader/identity.h (from your own IPSW)
devices/corvette/mkflashable.py           # boot-v4.img, from the UEFI FD + loader.efi
```

`mkflashable.py` writes only `boot-v4.img` into `out-corvette/` (or `-o`).
It packages the UEFI FD and USB loader with Android boot header v4 and the stock
Android 14.0.0 / 2026-04 OS metadata. Its temporary USB payload is removed after
packing; no stock firmware extraction is needed.

The boot image has no ramdisk. The device uses the generic ramdisk already present
in its stock `init_boot`; this script generates no `init_boot` image and requires
no changes to that partition.

## What the loader knows about this phone

`xnuloader/loader.c`, `platform_corvette`, told apart from redwood and QEMU by the framebuffer base
its UEFI reports (`0xD5100000`):

| | |
|---|---|
| GIC distributor / redistributor | `0x17100000` / `0x17180000`, 8 frames of `0x40000` |
| redistributor stride | `0x40000`, passed to the kernel as `redistributor-stride` in the device tree it hands over |
| TLMM (pinctrl) | `0x0F100000` |
| cores | 8 (2 x A520, 5 x A720, 1 x X4); `small-cores` = `0x3` |

A kernel started on CPU7 (`bootcpu=7`) leaves CPU0 to the loader, whose USB polling keeps the
`oem run` / `stage` link alive; without it the link is gone the moment the kernel takes CPU0.

## Running in RAM

Nothing of the phone's user data is written. With the loader running (UEFI from `boot-v4.img`):

```sh
fastboot reboot bootloader               # reset SoC state before a fresh GPU run
fastboot oem ramdisk                      # the next download becomes the root image in RAM
fastboot stage <system.apfs>              # the iOS root volume, ~2.4 GB (md0)
fastboot stage run-kernel.bin             # the XNUP payload: the kernel, ~15 MB
fastboot oem run                          # start it on CPU7
```

The log is pulled with `fastboot get_staged`:

- `result: running` — `fastboot oem log` first (asks the loader to dump its buffer), then `get_staged`;
- `result: returned` — straight to `get_staged`; an `oem log` in between overwrites what the
  kernel left there.

The kernel keeps running until an explicit halt or return request; the host log wait does not stop it.
Warm reruns are currently suspended after reported 900E failures. `oem stop` does
not reset SoC registers, and uploading a full image does not reset them either.
Reboot into the bootloader before a fresh full stage. Do not repeat a failing
address-access experiment to collect evidence after a 900E.


## Wi-Fi: WCN7850 "Kiwi"

The device tree has `qcom,cnss-kiwi@b0000000` (`supported-ids = <0x1107>`, `wlan-en-gpio` = TLMM
16) and one `qcom,pcie@1c00000`, and **no `wpss@` node at all**: this phone's Wi-Fi is a PCIe + MHI
device with its own firmware, not the WCN6750 the POCO runs. The WCN6750 path in
`xnu/files/bsd/dev/hakios.wifi.c` (WPSS / SMEM / SMP2P / IPCC / GLINK / QRTR) therefore has nothing
to talk to here, which is why the setting can be seen but not switched on. Bringing it up needs a
new path for this phone: PCIe enumeration, MHI, `amss.bin` + `bdwlan.*` + `regdb.bin`, then QMI/WMI.

Its firmware, off the phone (see `pull-firmware.sh`):

```
/vendor/firmware_mnt/image/kiwi/amss.bin      (7.4 MB)
/vendor/firmware_mnt/image/kiwi/amss20.bin    (6.4 MB)
/vendor/firmware_mnt/image/kiwi/bdwlan.elf, bdwlan.e01 ... e3d
/vendor/firmware_mnt/image/kiwi/regdb.bin
/vendor/etc/wifi/WCNSS_qcom_cfg.ini
```

## Touch

A five-finger press held for 1.5 s is the exit gesture (back to the loader): while testing
multitouch, do not hold five fingers down at once.
