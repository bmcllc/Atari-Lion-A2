#!/usr/bin/env python3
"""Reference software render of a GPU trace (rt/gpu.c, PSX_GPU_TRACE_FRAME): replays the recorded GP0 commands over the VRAM
snapshot taken when recording started, the way a PS1 GPU draws them, to compare with what the Spike path showed.

  gpuref.py gt.bin vram.bin out.png [frames]

Renders until the given number of display flips (GP1 05) has passed (default 2) and writes the drawing area of the last
frame drawn. Not covered: CPU->VRAM uploads (their data words are not in the trace), lines, dithering, mask bits."""
import struct, sys
import numpy as np
from PIL import Image

d = open(sys.argv[1], 'rb').read()
w = struct.unpack('>%dI' % (len(d) // 4), d)
vram = np.frombuffer(open(sys.argv[2], 'rb').read(), dtype='>u2').astype(np.uint32).reshape(512, 1024).copy()
want = int(sys.argv[4]) if len(sys.argv) > 4 else 2
who = [tuple(map(int, a.split(','))) for a in sys.argv[5:]]   # pixels (in the drawing area) to explain

area = [0, 0, 1023, 511]; off = [0, 0]; texpage = 0; twin = 0
ids = np.full((512, 1024), -1, np.int64); rec = [None, -1]     # which record drew each pixel (gpuref.py ... --who X,Y)

def sx(v): v &= 0x7FF; return v - 0x800 if v & 0x400 else v

def texels(tp, clut, us, vs):
    """Fetches 15-bit texels for integer texture coordinates (arrays)."""
    mx, my, ox, oy = (twin & 31) * 8, ((twin >> 5) & 31) * 8, ((twin >> 10) & 31) * 8, ((twin >> 15) & 31) * 8
    us = (us & ~mx) | (ox & mx); vs = (vs & ~my) | (oy & my)
    depth, tx, ty = (tp >> 7) & 3, (tp & 15) * 64, ((tp >> 4) & 1) * 256
    cx, cy = (clut & 0x3F) * 16, (clut >> 6) & 0x1FF
    row = (ty + vs) & 511
    if depth == 0:
        word = vram[row, (tx + (us >> 2)) & 1023]
        return vram[cy, (cx + ((word >> ((us & 3) * 4)) & 15)) & 1023]
    if depth == 1:
        word = vram[row, (tx + (us >> 1)) & 1023]
        return vram[cy, (cx + ((word >> ((us & 1) * 8)) & 255)) & 1023]
    return vram[row, (tx + us) & 1023]

def blend(bg, fg, mode):
    out = []
    for s in (0, 5, 10):
        b = (bg >> s) & 31; f = (fg >> s) & 31
        r = [(b + f) // 2, b + f, b - f, b + f // 4][mode]
        out.append(np.clip(r, 0, 31).astype(np.uint32) << s)
    return out[0] | out[1] | out[2]

def draw(px, py, col, semi, mode):
    """Writes 15-bit colors at pixel arrays (already inside the drawing area); semi: per-pixel bool."""
    cur = vram[py, px]
    out = np.where(semi, blend(cur, col, mode), col)
    vram[py, px] = out
    ids[py, px] = rec[1]

def tri(v, tex, gour, semi, raw, tp, clut, c0):
    xs = [p[0] for p in v]; ys = [p[1] for p in v]
    x0, x1 = max(min(xs), area[0]), min(max(xs), area[2]); y0, y1 = max(min(ys), area[1]), min(max(ys), area[3])
    if x1 < x0 or y1 < y0 or max(xs) - min(xs) > 1023 or max(ys) - min(ys) > 511: return
    gx, gy = np.meshgrid(np.arange(x0, x1 + 1), np.arange(y0, y1 + 1))
    (ax, ay), (bx, by), (cx, cy) = [(p[0], p[1]) for p in v]
    den = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax)
    if den == 0: return
    w0 = ((bx - gx) * (cy - gy) - (by - gy) * (cx - gx)) / den
    w1 = ((cx - gx) * (ay - gy) - (cy - gy) * (ax - gx)) / den
    w2 = 1 - w0 - w1
    m = (w0 >= 0) & (w1 >= 0) & (w2 >= -1e-9)
    if not m.any(): return
    gx, gy, w0, w1, w2 = gx[m], gy[m], w0[m], w1[m], w2[m]
    if gour:
        cc = [np.clip(w0 * ((v[0][2] >> s) & 255) + w1 * ((v[1][2] >> s) & 255) + w2 * ((v[2][2] >> s) & 255), 0, 255) for s in (0, 8, 16)]
    else:
        cc = [np.full(gx.shape, (c0 >> s) & 255, float) for s in (0, 8, 16)]
    mode = ((tp if tex else texpage) >> 5) & 3
    if tex:
        u = np.floor(w0 * v[0][3] + w1 * v[1][3] + w2 * v[2][3] + 1e-6).astype(np.int64) & 255
        t = np.floor(w0 * v[0][4] + w1 * v[1][4] + w2 * v[2][4] + 1e-6).astype(np.int64) & 255
        tx = texels(tp, clut, u, t)
        keep = tx != 0
        gx, gy, tx = gx[keep], gy[keep], tx[keep]; cc = [c[keep] for c in cc]
        if raw: col = tx & 0x7FFF
        else:
            col = 0
            for i, s in enumerate((0, 5, 10)):
                col = col | (np.clip(((tx >> s) & 31) * cc[i] / 128, 0, 31).astype(np.uint32) << s)
        draw(gx, gy, col, (tx & 0x8000) != 0 if semi else np.zeros(gx.shape, bool), mode)
    else:
        col = (cc[0].astype(np.uint32) >> 3) | ((cc[1].astype(np.uint32) >> 3) << 5) | ((cc[2].astype(np.uint32) >> 3) << 10)
        draw(gx, gy, col, np.full(gx.shape, bool(semi)), mode)

def poly(c):
    global texpage
    op = c[0] >> 24
    tex, quad, gour, semi, raw = op & 4, op & 8, op & 0x10, op & 2, op & 1
    n = 4 if quad else 3; j = 1; v = []; clut = tp = 0
    for i in range(n):
        col = c[0] & 0xFFFFFF if i == 0 or not gour else c[j]
        if gour and i > 0: j += 1
        xy = c[j]; j += 1
        u = t = 0
        if tex:
            u, t = c[j] & 255, (c[j] >> 8) & 255
            if i == 0: clut = c[j] >> 16
            if i == 1: tp = c[j] >> 16
            j += 1
        v.append((sx(xy) + off[0], sx(xy >> 16) + off[1], col, u, t))
    if tex: texpage = (texpage & ~0x9FF) | (tp & 0x9FF)
    tri(v[:3], tex, gour, semi, raw, tp, clut, c[0] & 0xFFFFFF)
    if quad: tri([v[1], v[2], v[3]], tex, gour, semi, raw, tp, clut, c[0] & 0xFFFFFF)

def rect(c):
    op = c[0] >> 24
    tex, semi, raw = op & 4, op & 2, op & 1
    x, y = sx(c[1]) + off[0], sx(c[1] >> 16) + off[1]; j = 2; u = t = clut = 0
    if tex: u, t, clut = c[2] & 255, (c[2] >> 8) & 255, c[2] >> 16; j = 3
    size = (op >> 3) & 3
    wd, ht = [(c[j] & 0x3FF, (c[j] >> 16) & 0x1FF) if size == 0 else None, (1, 1), (8, 8), (16, 16)][size]
    x0, x1, y0, y1 = max(x, area[0]), min(x + wd - 1, area[2]), max(y, area[1]), min(y + ht - 1, area[3])
    if x1 < x0 or y1 < y0: return
    gx, gy = np.meshgrid(np.arange(x0, x1 + 1), np.arange(y0, y1 + 1)); gx, gy = gx.ravel(), gy.ravel()
    c0 = c[0] & 0xFFFFFF; mode = (texpage >> 5) & 3
    if tex:
        tx = texels(texpage, clut, (u + gx - x) & 255, (t + gy - y) & 255)
        keep = tx != 0; gx, gy, tx = gx[keep], gy[keep], tx[keep]
        if raw: col = tx & 0x7FFF
        else:
            col = 0
            for i, s in enumerate((0, 5, 10)):
                col = col | (np.clip(((tx >> s) & 31) * ((c0 >> (8 * i)) & 255) / 128, 0, 31).astype(np.uint32) << s)
        draw(gx, gy, col, (tx & 0x8000) != 0 if semi else np.zeros(gx.shape, bool), mode)
    else:
        col = ((c0 & 255) >> 3) | (((c0 >> 8) & 255) >> 3) << 5 | (((c0 >> 16) & 255) >> 3) << 10
        draw(gx, gy, np.full(gx.shape, col, np.uint32), np.full(gx.shape, bool(semi)), mode)

n, i, flips = w[0], 1, 0
while i <= n:
    cnt = w[i]; c = w[i + 1:i + 1 + cnt]; rec[1] = i; i += 1 + cnt
    if c[0] == 0xF1000000:
        if (c[1] >> 24) == 0x05:
            flips += 1
            if flips == want: break
        continue
    op = c[0] >> 24
    if 0x20 <= op <= 0x3F: poly(c)
    elif 0x60 <= op <= 0x7F: rect(c)
    elif op == 0x02:
        x, y, wd, ht = c[1] & 0x3F0, (c[1] >> 16) & 0x1FF, ((c[2] & 0x3FF) + 15) & ~15, (c[2] >> 16) & 0x1FF
        c0 = c[0] & 0xFFFFFF
        vram[y:y + ht, x:x + wd] = ((c0 & 255) >> 3) | (((c0 >> 8) & 255) >> 3) << 5 | (((c0 >> 16) & 255) >> 3) << 10
    elif op == 0x80:
        sxy, dxy, wh = c[1], c[2], c[3]
        sx0, sy0, dx0, dy0 = sxy & 1023, (sxy >> 16) & 511, dxy & 1023, (dxy >> 16) & 511
        wd, ht = wh & 0xFFFF or 1024, (wh >> 16) or 512
        vram[dy0:dy0 + ht, dx0:dx0 + wd] = vram[sy0:sy0 + ht, sx0:sx0 + wd].copy()
    elif op == 0xE1: texpage = c[0] & 0xFFFFFF
    elif op == 0xE2: twin = c[0] & 0xFFFFF
    elif op == 0xE3: area[0], area[1] = c[0] & 1023, (c[0] >> 10) & 511
    elif op == 0xE4: area[2], area[3] = c[0] & 1023, (c[0] >> 10) & 511
    elif op == 0xE5: off[0], off[1] = sx(c[0]), sx(c[0] >> 11)

v = vram[area[1]:area[3] + 1, area[0]:area[2] + 1]
rgb = np.stack([(v & 31) << 3, ((v >> 5) & 31) << 3, ((v >> 10) & 31) << 3], -1).astype(np.uint8)
Image.fromarray(rgb).save(sys.argv[3])
print('flips %d, area %s' % (flips, area))
for x, y in who:
    k = ids[area[1] + y, area[0] + x]
    print('pixel', x, y, '%04X' % vram[area[1] + y, area[0] + x], 'record', k, ' '.join('%08X' % q for q in w[k + 1:k + 1 + w[k]]) if k >= 0 else '')
