#!/usr/bin/env python3
"""
Build the images to flash on corvette (OnePlus Ace 3 Pro, SM8650) from the stock firmware and the
UEFI FD + XNU loader built in this tree.

The phone came back to fastboot with the v0 image whose ramdisk was only the upstream 5-byte dummy:
with no ramdisk anywhere the firmware may refuse to hand the kernel over. So there are two images,
one for each way the firmware can find a real ramdisk:

  boot-v4.img          header v4: no ramdisk in boot.img at all, the firmware takes the generic
                       ramdisk from the stock init_boot (flash the stock init_boot back). This is
                       the layout the phone ships with, so the least is changed.
  boot-v0-ramdisk.img  header v0 with the stock init_boot ramdisk carried inside boot.img, for the
                       setup tested with the earlier port (init_boot emptied).
  boot-uefi-only.img   boot-v4.img without the payload, to watch the UEFI alone come up.
  init_boot-empty.img  the stock init_boot with its ramdisk emptied, only for boot-v0-ramdisk.img.
  stock-ramdisk.lz4    the generic ramdisk taken out of the stock init_boot.

Both boot images claim the stock os version (Android 14.0.0, 2026-04) so that a firmware that
enforces a rollback index has nothing to complain about.

usage: devices/corvette/mkflashable.py [-f FIRMWARE] [-o OUT]
"""
import argparse, os, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
STOCK_FW = ('/mnt/d/OPAce3Pro_ColorOS_17.0.0.100_Testing_Nolog_Donated_By_Hawthorne.zip/'
            'OPAce3Pro_ColorOS_17.0.0.100_Testing_Nolog_Donated_By_Hawthorne/firmware')
LOADER = os.path.join(ROOT, 'xnuloader', 'loader.efi')
# from xbl's MemoryMapLib; see docs/PORTING.md
LAYOUT = {'FD_BASE': 0xA7100000, 'FD_SIZE': 0x300000, 'PAYLOAD_BASE': 0xA8000000}
# decoded from the stock images (os_version = 0x1c0001a4)
STOCK_OS = {'BOOT_OS_VERSION': '14.0.0', 'BOOT_OS_PATCH': '2026-04'}

ap = argparse.ArgumentParser()
ap.add_argument('-f', '--firmware', default=STOCK_FW)
ap.add_argument('-o', '--out', default=os.path.join(ROOT, 'out-corvette'))
args = ap.parse_args()

os.makedirs(args.out, exist_ok=True)
base_env = dict(os.environ, UEFI_TARGET='corvette',
                 DTB=os.path.join(HERE, 'prebuilt', 'device-tree.dtb'), **STOCK_OS)
for k, v in LAYOUT.items():
    base_env[k] = hex(v)

payload = os.path.join(args.out, 'usb-payload.bin')
subprocess.check_call([sys.executable, os.path.join(ROOT, 'xnuloader', 'mkpayload.py'),
                       '-o', payload, f'loader.efi={LOADER}', '--usb'])


def build_boot(out, header, ramdisk=None, with_payload=True):
    env = dict(base_env, BOOT_HEADER_VERSION=str(header))
    if ramdisk:
        env['RAMDISK'] = ramdisk
    cmd = [sys.executable, os.path.join(ROOT, 'mkxnuimg.py'), '-o', out]
    if with_payload:
        cmd += ['-p', payload]
    subprocess.check_call(cmd, env=env, cwd=ROOT)
    d = open(out, 'rb').read()
    if header >= 3:
        print(f'  {os.path.basename(out)}: header={struct.unpack_from("<I", d, 40)[0]} '
              f'kernel={struct.unpack_from("<I", d, 8)[0]} ramdisk={struct.unpack_from("<I", d, 12)[0]} '
              f'os_version={struct.unpack_from("<I", d, 16)[0]:#x}')
    else:
        print(f'  {os.path.basename(out)}: header={struct.unpack_from("<I", d, 40)[0]} '
              f'page={struct.unpack_from("<I", d, 36)[0]} kernel={struct.unpack_from("<I", d, 8)[0]} '
              f'ramdisk={struct.unpack_from("<I", d, 16)[0]} os_version={struct.unpack_from("<I", d, 44)[0]:#x}')


stock = open(os.path.join(args.firmware, 'init_boot.img'), 'rb').read()
assert stock[:8] == b'ANDROID!' and struct.unpack_from('<I', stock, 40)[0] == 4, 'unexpected init_boot'
rsz = struct.unpack_from('<I', stock, 12)[0]
ramdisk = os.path.join(args.out, 'stock-ramdisk.lz4')
open(ramdisk, 'wb').write(stock[4096:4096 + rsz])
print(f'stock-ramdisk.lz4: {rsz} bytes (generic ramdisk of the stock init_boot)')

build_boot(os.path.join(args.out, 'boot-v4.img'), 4)
build_boot(os.path.join(args.out, 'boot-uefi-only.img'), 4, with_payload=False)
build_boot(os.path.join(args.out, 'boot-v0-ramdisk.img'), 0, ramdisk=ramdisk)

img = bytearray(stock)
struct.pack_into('<I', img, 12, 0)
img[4096:] = bytes(len(img) - 4096)
dst = os.path.join(args.out, 'init_boot-empty.img')
open(dst, 'wb').write(img)
print(f'init_boot-empty.img: ramdisk {rsz} -> 0 bytes, image still {len(img)} bytes')
