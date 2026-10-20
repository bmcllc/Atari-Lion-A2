#!/usr/bin/env python3
"""Packs a big-endian PowerPC ELF32 into PSX.IMG for stage 0: mkimg.py in.elf out.img (segments aligned to 2048 bytes)."""
import struct, sys
d = open(sys.argv[1], "rb").read()
assert d[:4] == b"\x7fELF" and d[4] == 1 and d[5] == 2, "expected ELF32 big-endian"
entry, phoff = struct.unpack(">II", d[24:28] + d[28:32])
phentsize, phnum = struct.unpack(">HH", d[42:46])
segs = []
for i in range(phnum):
    p_type, off, vaddr, paddr, filesz, memsz, flags, align = struct.unpack(">IIIIIIII", d[phoff + i * phentsize: phoff + i * phentsize + 32])
    if p_type == 1 and filesz:
        segs.append((vaddr, d[off:off + filesz]))
hdr = b"PSXI" + struct.pack(">II", entry, len(segs))
body = b""
off = 2048
for vaddr, data in segs:
    hdr += struct.pack(">III", vaddr, len(data), off)
    pad = (-len(data)) % 2048
    body += data + bytes(pad)
    off += len(data) + pad
assert len(hdr) <= 2048
open(sys.argv[2], "wb").write(hdr + bytes(2048 - len(hdr)) + body)
print(f"{sys.argv[2]}: entry {entry:#x}, {len(segs)} segments, {len(body)} bytes")
