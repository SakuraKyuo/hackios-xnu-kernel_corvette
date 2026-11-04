#!/usr/bin/env python3
"""Build only boot-v4.img for corvette (OnePlus Ace 3 Pro, SM8650).

The image contains the UEFI FD and USB XNU loader, with Android boot header v4
and no boot ramdisk. The device's existing stock init_boot supplies its generic
ramdisk. No init_boot image or alternate boot layout is generated.

usage: devices/corvette/mkflashable.py [-o OUT]
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
LOADER = os.path.join(ROOT, 'xnuloader', 'loader.efi')
# From the phone's xbl MemoryMapLib; see docs/PORTING.md.
LAYOUT = {'FD_BASE': 0xA7100000, 'FD_SIZE': 0x300000, 'PAYLOAD_BASE': 0xA8000000}
STOCK_OS = {'BOOT_OS_VERSION': '14.0.0', 'BOOT_OS_PATCH': '2026-04'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('-o', '--out', default=os.path.join(ROOT, 'out-corvette'))
    # Accept earlier automation's argument without reading or changing init_boot.
    parser.add_argument('-f', '--firmware', help=argparse.SUPPRESS)
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    env = dict(os.environ, UEFI_TARGET='corvette', BOOT_HEADER_VERSION='4',
               DTB=os.path.join(HERE, 'prebuilt', 'device-tree.dtb'), **STOCK_OS)
    env.pop('RAMDISK', None)
    for key, value in LAYOUT.items():
        env[key] = hex(value)
    image = os.path.abspath(os.path.join(args.out, 'boot-v4.img'))
    with tempfile.TemporaryDirectory(prefix='corvette-boot-') as temporary:
        payload = os.path.join(temporary, 'usb-payload.bin')
        subprocess.check_call([sys.executable, os.path.join(ROOT, 'xnuloader', 'mkpayload.py'),
                               '-o', payload, f'loader.efi={LOADER}', '--usb'])
        subprocess.check_call([sys.executable, os.path.join(ROOT, 'mkxnuimg.py'),
                               '-o', image, '-p', payload], env=env, cwd=ROOT)
    with open(image, 'rb') as file:
        header = file.read(4096)
    assert header[:8] == b'ANDROID!', 'unexpected boot magic'
    kernel, ramdisk, os_version = struct.unpack_from('<III', header, 8)
    version = struct.unpack_from('<I', header, 40)[0]
    assert version == 4 and kernel != 0 and ramdisk == 0, 'expected v4 boot with no ramdisk'
    assert os_version == 0x1c0001a4, 'unexpected stock OS metadata'
    print(f'boot-v4.img: header={version} kernel={kernel} ramdisk={ramdisk} os_version={os_version:#x}')


if __name__ == '__main__':
    main()
