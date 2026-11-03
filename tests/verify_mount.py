#!/usr/bin/env python3
"""Compares files on a mounted ext4 drive with the md5 list written by
make_test_images.sh.

    python tests/verify_mount.py Q: build/testdata/expected.txt
"""
import hashlib
import os
import sys

FORBIDDEN = set(b'"*:<>?\\|') | set(range(0x20))


def map_name(raw: bytes) -> str:
    """Same name mapping as src/core/names.cpp (Linux bytes -> Windows)."""
    out = []
    i = 0
    while i < len(raw):
        # try to decode one UTF-8 character
        for n in (1, 2, 3, 4):
            try:
                ch = raw[i:i + n].decode('utf-8')
                break
            except UnicodeDecodeError:
                ch = None
        if ch is None:
            out.append(chr(0xF100 + raw[i]))
            i += 1
            continue
        last = i + n == len(raw)
        c = ord(ch)
        if c < 0x80 and (c in FORBIDDEN or (last and ch in '. ' and raw not in (b'.', b'..'))):
            out.append(chr(0xF000 + c))
        else:
            out.append(ch)
        i += n
    return ''.join(out)


def main() -> int:
    drive, expected = sys.argv[1], sys.argv[2]
    ok = bad = 0
    with open(expected, 'rb') as f:
        records = [r for r in f.read().split(b'\0') if r]  # md5sum -z output
        for line in records:
            digest, _, path = line.partition(b'  ')
            parts = [p for p in path.split(b'/') if p not in (b'', b'.')]
            win = os.path.join(drive + '\\', *[map_name(p) for p in parts])
            try:
                h = hashlib.md5()
                with open(win, 'rb') as g:
                    while chunk := g.read(1 << 20):
                        h.update(chunk)
                if h.hexdigest().encode() == digest:
                    ok += 1
                    continue
                reason = 'checksum differs'
            except OSError as e:
                reason = str(e)
            bad += 1
            print(f'FAIL {path!r}: {reason}')
    print(f'{ok} ok, {bad} failed')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
