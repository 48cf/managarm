#!/usr/bin/env python3

import sys
import struct


def patch_eir(input: str, output: str) -> None:
    with open(input, "rb") as f:
        data = bytearray(f.read())

    # patch the kernel image size
    struct.pack_into("<Q", data, 16, len(data))

    with open(output, "wb") as f:
        f.write(data)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input> <output>")
        sys.exit(1)

    patch_eir(sys.argv[1], sys.argv[2])
