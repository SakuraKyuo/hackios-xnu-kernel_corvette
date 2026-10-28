#!/usr/bin/env python3
"""
Pack the redwood UEFI (with the hook that starts the XNU loader) and an optional payload
into an Android boot image for `fastboot boot`.

  kernel = gzip(BootShim + UEFI FD + ["XNUP" container]) + DTB

The boot shim copies the FD to its base and the payload to PAYLOAD_BASE.

The UEFI pieces are taken from a Mu-Silicium tree built here (Mu-Silicium/, see devices/poco-x5-pro/uefi/README.md)
or, when there is none, from devices/poco-x5-pro/prebuilt/.

usage: mkxnuimg.py [-p payload.bin] [-o out.img]
"""
import argparse, gzip, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
MU = os.path.join(HERE, 'Mu-Silicium')
# The device whose prebuilt/ the pieces come from: devices/<name>/prebuilt, chosen with
# UEFI_TARGET=<device>. A piece the device's own directory does not carry (mkbootimg.py, the 5-byte
# placeholder ramdisk) falls back to the poco-x5-pro port, which does.
DEVICE = os.environ.get('UEFI_TARGET') or 'poco-x5-pro'
PREBUILT = os.path.join(HERE, 'devices', DEVICE, 'prebuilt')
FALLBACK = os.path.join(HERE, 'devices', 'poco-x5-pro', 'prebuilt')


def part(name):
    p = os.path.join(PREBUILT, name)
    return p if os.path.isfile(p) else os.path.join(FALLBACK, name)

# Header version of the image written: 0/1 keep the ramdisk inside boot.img (the firmware then takes
# it from there), 4 is the Android 13+ layout with the ramdisk in init_boot instead.
HEADER_VERSION = os.environ.get('BOOT_HEADER_VERSION') or '1'
# The stock images of the phone carry "Android 14.0.0, 2026-04"; the firmware may refuse an image
# that claims an older one, so these are passed through when set.
OS_VERSION = os.environ.get('BOOT_OS_VERSION', '')
OS_PATCH = os.environ.get('BOOT_OS_PATCH', '')
# Where the boot shim copies the UEFI FD and the payload: a property of the device's memory map
# (xbl's MemoryMapLib), so a port passes its own with these variables.
FD_BASE = int(os.environ.get('FD_BASE', '0x9FD00000'), 16)
FD_SIZE = int(os.environ.get('FD_SIZE', '0x200000'), 16)
PAYLOAD_BASE = int(os.environ.get('PAYLOAD_BASE', '0xF0000000'), 16)
PAYLOAD_MAX = 0x04000000

ap = argparse.ArgumentParser()
ap.add_argument('-p', '--payload')
ap.add_argument('-o', '--output', default=os.path.join(HERE, 'xnu-redwood.img'))
ap.add_argument('--mode', default='RELEASE')
args = ap.parse_args()

fd_path = os.path.join(MU, f'Build/redwoodPkg/{args.mode}_CLANGPDB/FV/SILICIUM_UEFI.fd')
if os.path.isfile(fd_path):
    shim_dir = os.path.join(MU, 'BootShim')
    for f in ('BootShim.elf', 'BootShim.bin'):
        try: os.unlink(os.path.join(shim_dir, f))
        except FileNotFoundError: pass
    subprocess.check_call(['make', 'REQUIRES_KERNEL_HEADER=1', f'FD_BASE={FD_BASE:#x}', f'FD_SIZE={FD_SIZE:#x}',
                           f'CFLAGS=--target=aarch64-linux-gnu -DPAYLOAD_BASE={PAYLOAD_BASE:#x}'],
                          cwd=shim_dir, stdout=subprocess.DEVNULL)
    parts = {'shim': os.path.join(shim_dir, 'BootShim.bin'), 'fd': fd_path,
             'dtb': os.path.join(MU, 'Resources/DTBs/redwood.dtb'), 'ramdisk': os.path.join(MU, 'Resources/ramdisk'),
             'mkbootimg': os.path.join(MU, 'Resources/Scripts/mkbootimg.py')}
else:
    parts = {'shim': part('BootShim.bin'), 'fd': part('SILICIUM_UEFI.fd'),
             'dtb': part('device-tree.dtb'), 'ramdisk': part('ramdisk'),
             'mkbootimg': part('mkbootimg.py')}
shim = open(parts['shim'], 'rb').read()
fd = open(parts['fd'], 'rb').read()
assert len(fd) == FD_SIZE, hex(len(fd))

blob = shim + fd
if args.payload:
    data = open(args.payload, 'rb').read()
    # the payload file is a complete "XNUP" container (see xnuloader/mkpayload.py)
    assert data[:4] == b'XNUP' and len(data) <= PAYLOAD_MAX - 0x1000, 'bad or oversized payload'
    blob += data
    blob += b'\0' * (-len(blob) % 16)
    print(f'payload: {len(data)} bytes, bytesum={sum(data) & 0xffffffff:08x}')

if os.environ.get('DTB'): parts['dtb'] = os.environ['DTB']
if os.environ.get('RAMDISK'): parts['ramdisk'] = os.environ['RAMDISK']
dtb = open(parts['dtb'], 'rb').read()
kernel = args.output + '.kernel~'
open(kernel, 'wb').write(gzip.compress(blob, 6) + dtb)
try:
    cmd = [sys.executable, parts['mkbootimg'], '--kernel', kernel, '--header_version', HEADER_VERSION,
           '-o', args.output]
    if int(HEADER_VERSION) < 3:          # v3+ images carry no ramdisk at all
        cmd += ['--ramdisk', parts['ramdisk']]
    if OS_VERSION:
        cmd += ['--os_version', OS_VERSION]
    if OS_PATCH:
        cmd += ['--os_patch_level', OS_PATCH]
    subprocess.check_call(cmd)
finally:
    os.unlink(kernel)
print(f'{args.output}: {os.path.getsize(args.output)} bytes (shim {len(shim)}, uncompressed {len(blob)})')
