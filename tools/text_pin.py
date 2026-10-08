#!/usr/bin/env python3
"""Print the (byte length, FNV-1a 32) pin of a decoded ROM string, for the host tests.

The tests never carry game text verbatim; they pin an expected decode by length + hash.
Usage: tools/text_pin.py "<decoded string>"   (UTF-8; é is the two-byte sequence)
"""
import sys


def fnv1a32(data: bytes) -> int:
    h = 0x811C9DC5
    for c in data:
        h = ((h ^ c) * 0x01000193) & 0xFFFFFFFF
    return h


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    raw = argv[1].encode("utf-8")
    print(f"{{ {len(raw)}, 0x{fnv1a32(raw):08X}u }}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
