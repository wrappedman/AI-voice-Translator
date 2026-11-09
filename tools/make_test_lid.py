#!/usr/bin/env python3
"""Write a tiny *synthetic* fastText supervised file for the worker tests.

This is test data, not a language model: two labels, four words, two dimensions.
It exists so `make tests` can exercise the real worker handshake and a real
`detect` round trip without downloading anything. The format is fastText's
documented binary layout (magic, version, Args, Dictionary, dense matrices), so
the vendored fastText loads it exactly like an official lid.176 file.

Nothing here is shipped: the file is written into build/ and never packaged.

Usage: make_test_lid.py <output.bin>
"""
import struct
import sys

MAGIC = 793712314
VERSION = 12

WORDS = ["hello", "world", "سلام", "خداحافظ"]
LABELS = ["__label__en", "__label__fa"]
DIM = 2


def args_block() -> bytes:
    # dim, ws, epoch, minCount, neg, wordNgrams, loss(softmax=3), model(sup=3),
    # bucket, minn, maxn, lrUpdateRate, t(double)
    return struct.pack("<12id", DIM, 5, 1, 1, 5, 1, 3, 3, 0, 0, 0, 100, 0.0001)


def dict_block() -> bytes:
    entries = [(w, 10, 0) for w in WORDS] + [(l, 10, 1) for l in LABELS]
    out = struct.pack("<iii qq", len(entries), len(WORDS), len(LABELS), 40, -1)
    for word, count, kind in entries:
        out += word.encode("utf-8") + b"\0" + struct.pack("<qb", count, kind)
    return out


def matrix_block(rows: list) -> bytes:
    out = struct.pack("<qq", len(rows), DIM)
    for row in rows:
        out += struct.pack("<%df" % DIM, *row)
    return out


def build() -> bytes:
    # English words point along x, Persian words along y; the output rows pick
    # the matching label, so predictions are deterministic.
    inputs = [[1.0, 0.0], [1.0, 0.0], [0.0, 1.0], [0.0, 1.0]]
    outputs = [[8.0, 0.0], [0.0, 8.0]]
    return (
        struct.pack("<ii", MAGIC, VERSION)
        + args_block()
        + dict_block()
        + b"\0"                      # quant_input = false
        + matrix_block(inputs)
        + b"\0"                      # qout = false
        + matrix_block(outputs)
    )


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    with open(sys.argv[1], "wb") as f:
        f.write(build())
    print("test fixture: %s (%d bytes, 2 labels)" % (sys.argv[1], len(build())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
