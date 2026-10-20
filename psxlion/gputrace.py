#!/usr/bin/env python3
"""Decodes a GPU command trace (rt/gpu.c, PSX_GPU_TRACE_FRAME) from a --dump-ram of 0x1400000: gputrace.py dump.bin [max]."""
import struct, sys
d = open(sys.argv[1], 'rb').read()
lim = int(sys.argv[2]) if len(sys.argv) > 2 else 1 << 30
w = struct.unpack('>%dI' % (len(d) // 4), d)
n, i, k = w[0], 1, 0
def sx(v): v &= 0x7FF; return v - 0x800 if v & 0x400 else v
def xy(v): return '(%d,%d)' % (sx(v), sx(v >> 16))
while i <= n and k < lim:
    cnt = w[i]; c = w[i + 1:i + 1 + cnt]; i += 1 + cnt; k += 1
    op = c[0] >> 24
    if c[0] == 0xF1000000: print('GP1 %08X' % c[1]); continue
    if 0x20 <= op <= 0x3F:
        tex, quad, gour = op & 4, op & 8, op & 0x10
        nv = 4 if quad else 3; per = 1 + (1 if tex else 0) + (1 if gour else 0); j = 1
        vs = []
        for v in range(nv):
            col = c[0] if v == 0 or not gour else c[j]; j += 1 if (gour and v > 0) else 0
            p = c[j]; j += 1
            uv = ''
            if tex: uv = ' uv=%d,%d' % (c[j] & 0xFF, (c[j] >> 8) & 0xFF) + (' clut=%04X' % (c[j] >> 16) if v == 0 else ' tp=%04X' % (c[j] >> 16) if v == 1 else ''); j += 1
            vs.append(xy(p) + uv + (' c=%06X' % (col & 0xFFFFFF) if gour or v == 0 else ''))
        print('POLY %02X %s%s%s%s %s' % (op, 'tex ' if tex else '', 'quad ' if quad else '', 'gour ' if gour else '', 'semi ' if op & 2 else '', ' | '.join(vs)))
    elif 0x60 <= op <= 0x7F: print('RECT %02X %s' % (op, ' '.join('%08X' % x for x in c)))
    elif 0x40 <= op <= 0x5F: print('LINE %02X %s' % (op, ' '.join('%08X' % x for x in c)))
    else: print('CMD  %02X %s' % (op, ' '.join('%08X' % x for x in c)))
