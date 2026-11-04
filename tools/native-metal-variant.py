#!/usr/bin/env python3
"""Reproduce the tested corvette variant of a builder-supplied Native plugin.

The input binary is not distributed here. Refuse unknown versions and CMS
signatures; update only the two reviewed instructions and ad-hoc page hashes.
"""
import argparse
import hashlib
from pathlib import Path
import struct

ORIGINAL = '47eb3a6bb536ca8ec59073c441215ca3ee3dfe7442d4b5951fa7eabed32522ce'
VARIANT = '2a01d8997eddbc59294326dddb636dcc5d295a53c3a385970ac678eceb312fe5'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def generate(original):
    require(hashlib.sha256(original).hexdigest() == ORIGINAL, 'unreviewed Native plugin input')
    data = bytearray(original)
    # Let backboardd reach the existing publish/clients policy and real factory.
    require(struct.unpack_from('<I', data, 0x5b30)[0] == 0x34000f00, 'factory instruction changed')
    struct.pack_into('<I', data, 0x5b30, 0xd503201f)
    # UChar3 remains type 51. The source adapter supplies its padded fourth byte.
    entry = 0x1074c68 + 2 * (51 - 3)
    require(0x1c57c + 4 * struct.unpack_from('<H', data, entry)[0] == 0x1ca28,
            'constant table changed')
    require(struct.unpack_from('<I', data, 0x1c574)[0] == 0x52800093, 'padded byte-count path changed')
    struct.pack_into('<H', data, entry, (0x1c5a0 - 0x1c57c) // 4)
    at = 32
    signature = None
    for _ in range(struct.unpack_from('<I', original, 16)[0]):
        command, size = struct.unpack_from('<II', original, at)
        require(size >= 8 and at + size <= len(original), 'invalid Mach-O load command')
        if command == 0x1d:
            signature = struct.unpack_from('<II', original, at + 8)
        at += size
    require(signature is not None, 'missing ad-hoc signature')
    sigoff, sigsize = signature
    magic, length, count = struct.unpack_from('>III', original, sigoff)
    require(magic == 0xfade0cc0 and length <= sigsize, 'invalid signature superblob')
    directories = 0
    for index in range(count):
        _, relative = struct.unpack_from('>II', original, sigoff + 12 + 8 * index)
        blob = sigoff + relative
        magic = struct.unpack_from('>I', original, blob)[0]
        require(magic != 0xfade0b01, 'CMS signature requires its signing identity')
        if magic != 0xfade0c02:
            continue
        _, _, hashoff, _, _, codes, limit = struct.unpack_from('>7I', original, blob + 8)
        hs, ht, _, shift = struct.unpack_from('4B', original, blob + 36)
        require(ht == 2 and hs == 32 and limit == sigoff, 'unsupported CodeDirectory')
        page_size = 1 << shift
        require(codes == (limit + page_size - 1) // page_size, 'invalid code-page count')
        for page in range(codes):
            lo, hi = page * page_size, min((page + 1) * page_size, limit)
            where = blob + hashoff + page * hs
            require(hashlib.sha256(original[lo:hi]).digest() == original[where:where + hs],
                    'input code-page hash mismatch')
            data[where:where + hs] = hashlib.sha256(data[lo:hi]).digest()
        directories += 1
    require(directories > 0, 'no CodeDirectory')
    require(hashlib.sha256(data).hexdigest() == VARIANT, 'variant differs from tested binary')
    return data


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    require(args.input.resolve() != args.output.resolve(), 'preserve the original plugin')
    result = generate(args.input.read_bytes())
    args.output.write_bytes(result)
    args.output.chmod(0o755)
    print(f'{VARIANT}  {args.output}')
