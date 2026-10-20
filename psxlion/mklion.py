#!/usr/bin/env python3
"""Packs a big-endian PowerPC ELF32 into the Lion's .lion image: mklion.py in.elf out.lion"""
import struct, sys
d = open(sys.argv[1], "rb").read()
assert d[:4] == b"\x7fELF" and d[4] == 1 and d[5] == 2, "expected ELF32 big-endian"
entry, phoff = struct.unpack(">II", d[24:32][:4] + d[28:32])
phentsize, phnum = struct.unpack(">HH", d[42:46])
segs = []
for i in range(phnum):
    p_type, off, vaddr, paddr, filesz, memsz, flags, align = struct.unpack(">IIIIIIII", d[phoff + i * phentsize: phoff + i * phentsize + 32])
    if p_type == 1 and filesz:
        segs.append((vaddr, d[off:off + filesz]))
out = bytearray(b"LION" + struct.pack(">III", 1, entry, len(segs)))
base = 16 + 12 * len(segs)
body = b""
for vaddr, data in segs:
    out += struct.pack(">III", vaddr, len(data), base + len(body))
    body += data
open(sys.argv[2], "wb").write(bytes(out) + body)
print(f"{sys.argv[2]}: entry {entry:#x}, {len(segs)} segments, {len(body)} bytes")
