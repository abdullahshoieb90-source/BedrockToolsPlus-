#!/usr/bin/env python3
"""Check Spatial GUI candidate signatures against a user-supplied AArch64 ELF.

Usage: python3 scripts/check_spatial_signatures.py /path/to/libminecraftpe.so
The proprietary library is read locally and must not be committed. A unique
byte match does NOT prove function identity, calling convention or safety.
"""
import argparse
import mmap
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SIGNATURES = ROOT / 'src/core/memory/Signatures.cpp'
NAMES = (
    'ScreenViewRender', 'ContainerScreenControllerOpen', 'ContainerScreenControllerDtor',
    'ContainerScreenControllerOnContainerSlotSelected', 'ContainerScreenControllerGetItemStack',
    'RenderLevel', 'TessellatorBegin', 'TessellatorVertex', 'MeshHelpersRenderMeshImmediately2',
)


def executable_sections(data):
    # ELF64, little endian, EM_AARCH64. Scan only SHF_EXECINSTR sections.
    if len(data) < 64 or data[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', data, 18)[0] != 183:
        raise ValueError('expected a little-endian AArch64 ELF64 file')
    shoff = struct.unpack_from('<Q', data, 40)[0]
    entsize, count = struct.unpack_from('<HH', data, 58)
    if entsize < 64 or count == 0 or shoff + entsize * count > len(data):
        raise ValueError('missing or invalid ELF section table')
    sections = []
    for i in range(count):
        off = shoff + i * entsize
        flags, addr, fileoff, size = struct.unpack_from('<QQQQ', data, off + 8)
        if flags & 4 and size:
            if fileoff + size > len(data):
                raise ValueError('invalid executable section bounds')
            sections.append((fileoff, size, addr))
    if not sections:
        raise ValueError('no executable sections')
    return sections


def find_matches(data, sections, pattern):
    tokens = pattern.split()
    if not tokens or any(t != '?' and not re.fullmatch(r'[0-9a-fA-F]{2}', t) for t in tokens):
        raise ValueError('invalid signature')
    fixed = [None if x == '?' else int(x, 16) for x in tokens]
    mask = ''.join('?' if x is None else 'x' for x in fixed)
    runs = list(re.finditer('x+', mask))
    if not runs:
        raise ValueError('signature has no fixed bytes')
    run = max(runs, key=lambda x: x.end() - x.start())
    needle = bytes(fixed[run.start():run.end()])
    matches = []
    for off, size, addr in sections:
        end = off + size
        cursor = off
        while True:
            found = data.find(needle, cursor, end)
            if found < 0:
                break
            cursor = found + 1
            start = found - run.start()
            if start < off or start + len(fixed) > end or (addr + start - off) % 4:
                continue
            if all(byte is None or data[start + i] == byte for i, byte in enumerate(fixed)):
                matches.append(addr + start - off)
    return matches


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('library', type=Path)
    args = parser.parse_args()
    try:
        source = SIGNATURES.read_text()
        patterns = dict(re.findall(r'SignatureDefinition\{SignatureId::(\w+),\s*"([^"]+)"', source))
        with args.library.open('rb') as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
            sections = executable_sections(data)
            ok = True
            for name in NAMES:
                matches = find_matches(data, sections, patterns[name])
                print(f'{name}: {len(matches)} match(es)' + (f' at {matches[0]:#x}' if len(matches) == 1 else ''))
                ok &= len(matches) == 1
        print('Matches do not verify function identity, ABI, or runtime behavior.')
        return 0 if ok else 1
    except (OSError, ValueError, KeyError, struct.error) as error:
        print(f'error: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
