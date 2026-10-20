#!/usr/bin/env python3
"""Rewrites BOOT.LION and PSX.IMG inside a disc made by mkiso.sh, in place.

The game image (MK4.BIN etc., hundreds of MB) never changes between runtime builds, so instead of packing a new ISO
this overwrites the two small files: BOOT.LION keeps its extent (it must fit the sectors it has), PSX.IMG is the last
file on the disc and may grow. Directory record sizes and the volume size are updated.
    patchiso.py <disc.iso> <build out dir with BOOT.LION and PSX.IMG>
"""
import os, struct, sys

iso, out = sys.argv[1], sys.argv[2]
with open(iso, 'r+b') as f:
    f.seek(16 * 2048); pvd = bytearray(f.read(2048))
    assert pvd[0] == 1 and pvd[1:6] == b'CD001', 'not an ISO9660 volume'
    root_lba, root_len = struct.unpack_from('<I', pvd, 156 + 2)[0], struct.unpack_from('<I', pvd, 156 + 10)[0]
    f.seek(root_lba * 2048); d = bytearray(f.read(root_len))
    last_end = 0
    for name in ('BOOT.LION', 'PSX.IMG'):
        data = open(os.path.join(out, name), 'rb').read()
        i, rec = 0, None
        while i < len(d) and d[i]:
            n = d[i + 32]
            if d[i + 33:i + 33 + n].split(b';')[0] == name.encode(): rec = i
            i += d[i]
        assert rec is not None, name + ' not on the disc'
        lba, size = struct.unpack_from('<I', d, rec + 2)[0], struct.unpack_from('<I', d, rec + 10)[0]
        others = []
        i = 0
        while i < len(d) and d[i]:
            if i != rec and d[i + 25] & 2 == 0: others.append(struct.unpack_from('<I', d, i + 2)[0])
            i += d[i]
        if any(o > lba for o in others):                    # not the last file: must fit its current extent
            assert len(data) <= (size + 2047) // 2048 * 2048, f'{name} grew past its extent ({len(data)} > {size})'
        f.seek(lba * 2048); f.write(data + b'\0' * (-len(data) % 2048))
        struct.pack_into('<I', d, rec + 10, len(data)); struct.pack_into('>I', d, rec + 14, len(data))
        last_end = max(last_end, lba + (len(data) + 2047) // 2048)
    f.seek(root_lba * 2048); f.write(d)
    vol = max(struct.unpack_from('<I', pvd, 80)[0], last_end)
    struct.pack_into('<I', pvd, 80, vol); struct.pack_into('>I', pvd, 84, vol)
    f.seek(16 * 2048); f.write(pvd)
    f.truncate(vol * 2048)
print(f'{iso}: patched BOOT.LION and PSX.IMG ({vol} sectors)')
