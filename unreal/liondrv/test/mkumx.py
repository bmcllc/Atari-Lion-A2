#!/usr/bin/env python3
"""mkumx.py module out.umx ObjectName: wraps a tracker module in an Unreal v61 music package (what a .umx is)."""
import struct, sys

def idx(v):  # Unreal compact index
    neg = v < 0
    a = -v if neg else v
    out = bytearray()
    b = (0x80 if neg else 0) | (a & 0x3F)
    a >>= 6
    if a: b |= 0x40
    out.append(b)
    while a:
        b = a & 0x7F
        a >>= 7
        if a: b |= 0x80
        out.append(b)
    return bytes(out)

mod, out, name = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(mod, 'rb').read()
ftype = mod.rsplit('.', 1)[-1].lower()
names = ['None', 'Core', 'Class', 'Package', 'Engine', 'Music', name, ftype]
ni = {n: i for i, n in enumerate(names)}
nametab = b''.join(n.encode() + b'\0' + struct.pack('<I', 0x00070010) for n in names)
imports = (idx(ni['Core']) + idx(ni['Package']) + struct.pack('<i', 0) + idx(ni['Engine']) +
           idx(ni['Core']) + idx(ni['Class']) + struct.pack('<i', -1) + idx(ni['Music']))
obj = idx(ni['None']) + idx(ni[ftype]) + idx(len(data)) + data
header_size = 4 + 4 + 4 + 8 * 4
name_off = header_size
imp_off = name_off + len(nametab)
exp_off = imp_off + len(imports)
# One export; its data follows the tables.
def export(size, off):
    return idx(-2) + idx(0) + struct.pack('<i', 0) + idx(ni[name]) + struct.pack('<I', 0x000F0004) + idx(size) + idx(off)
exp_len = len(export(len(obj), 0))
data_off = exp_off + exp_len
exp = export(len(obj), data_off)
assert len(exp) == exp_len or True
if len(exp) != exp_len:  # offset width changed
    exp_len = len(exp); data_off = exp_off + exp_len; exp = export(len(obj), data_off)
hdr = struct.pack('<IiIiiiiiiii', 0x9E2A83C1, 61, 0, len(names), name_off, 1, exp_off, 2, imp_off, 0, 0)
open(out, 'wb').write(hdr + nametab + imports + exp + obj)
