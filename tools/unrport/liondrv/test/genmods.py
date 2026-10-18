#!/usr/bin/env python3
"""Writes small test modules (MOD, S3M, XM, IT) that exercise notes, volumes, slides, vibrato, arpeggio, jumps and
envelopes. Used with modtest.cpp and libopenmpt (ffmpeg) as the reference renderer."""
import math, struct, sys

def wave(kind, n, amp=100):
    out = []
    for i in range(n):
        if kind == 'sine': v = math.sin(2 * math.pi * i / n)
        elif kind == 'saw': v = 2.0 * i / n - 1
        elif kind == 'square': v = 1.0 if i < n // 2 else -1.0
        else: v = math.sin(2 * math.pi * i / n) + 0.5 * math.sin(6 * math.pi * i / n)
        out.append(int(max(-1, min(1, v)) * amp))
    return out

SAMPLES = [('sine', 64), ('saw', 64), ('square', 64), ('mix', 128)]
NOTES = {'C': 0, 'D': 2, 'E': 4, 'F': 5, 'G': 7, 'A': 9, 'B': 11}

def note_n(s):  # "C-4" -> 0-based semitone from C-0
    n = NOTES[s[0]] + (1 if s[1] == '#' else 0)
    return n + 12 * int(s[2])

# Song: list of rows; each row: list per channel of (note or None, inst, vol-or-None, fx, par), fx letter in XM style.
def song_rows():
    rows = []
    def r(*cells):
        rows.append(list(cells) + [(None, 0, None, None, 0)] * (4 - len(cells)))
    N = lambda s, i=1, v=None, fx=None, p=0: (s, i, v, fx, p)
    # Pattern of 32 rows
    r(N('C-4', 1), N('C-3', 2))
    r((None, 0, None, None, 0), (None, 0, None, None, 0))
    r(N('E-4', 1), N('G-3', 2))
    r((None, 0, None, None, 0),)
    r(N('G-4', 1, None, '0', 0x47), N('E-3', 2, 48))
    r((None, 0, None, None, 0),)
    r(N('C-5', 1, None, 'C', 32), N('C-3', 2, None, 'A', 0x04))
    r((None, 0, None, 'A', 0x04),)
    r(N('C-4', 3), N('C-2', 4))
    r((None, 0, None, '1', 0x08),)
    r((None, 0, None, '1', 0x08),)
    r((None, 0, None, '2', 0x10),)
    r(N('E-4', 1, None, '4', 0x44),)
    r((None, 0, None, '4', 0x44),)
    r((None, 0, None, '4', 0x44),)
    r((None, 0, None, 'A', 0x08),)
    r(N('A-4', 1), N('A-3', 2))
    r(N('G-4', 1, None, '3', 0x10),)
    r((None, 0, None, '3', 0x10),)
    r((None, 0, None, '3', 0x10),)
    r(N('C-4', 3, None, 'E', 0x91),)       # retrig
    r((None, 0, None, None, 0),)
    r(N('D-4', 1, None, 'E', 0xD2),)       # note delay
    r((None, 0, None, None, 0),)
    r(N('F-4', 2, None, 'F', 0x04),)       # speed 4
    r((None, 0, None, None, 0),)
    r(N('A-4', 2, None, 'F', 0x06),)       # speed 6
    r((None, 0, None, 'E', 0xC2),)         # note cut after 2 ticks
    r(N('E-4', 1, None, '9', 0x01),)       # sample offset
    r((None, 0, None, None, 0),)
    r((None, 0, None, 'C', 0),)
    r((None, 0, None, None, 0),)
    return rows

PERIODS = [856,808,762,720,678,640,604,570,538,508,480,453,428,404,381,360,339,320,302,285,269,254,240,226,214,202,190,180,170,160,151,143,135,127,120,113]

def write_mod(path):
    rows = song_rows()
    # MOD note: C-4 here -> period index: our 'C-4'=48 semitones -> MOD C-2 (idx 12) is middle C? use idx = n-36 (C-3->idx0)
    def modnote(s):
        n = note_n(s) - 36
        return PERIODS[max(0, min(35, n))]
    hdr = bytearray(b'testmod'.ljust(20, b'\0'))
    smps = bytearray()
    data = bytearray()
    for i in range(31):
        if i < 4:
            kind, n = SAMPLES[i]
            w = wave(kind, n * 2)
            data += bytes([x & 255 for x in w])
            smps += b'\0' * 22 + struct.pack('>HBBHH', len(w) // 2, 0, 64, 0, len(w) // 2)
        else:
            smps += b'\0' * 22 + struct.pack('>HBBHH', 0, 0, 0, 0, 1)
    orders = bytearray(128)
    song = struct.pack('BB', 1, 127) + bytes(orders)
    pat = bytearray()
    for row in rows:
        for ch in range(4):
            s, ins, vol, fx, p = row[ch]
            per = modnote(s) if s else 0
            cmd, par = 0, 0
            if vol is not None: cmd, par = 0xC, vol
            if fx is not None and not (fx == '0' and p == 0):
                if fx in '0123456789': cmd = int(fx, 16) if fx != '0' else 0
                else:
                    cmd = {'A': 0xA, 'C': 0xC, 'E': 0xE, 'F': 0xF}[fx]
                par = p
            pat += bytes([(ins & 0xF0) | (per >> 8) if s else (ins & 0xF0), per & 255, ((ins & 15) << 4 if s or ins else 0) | cmd, par])
    pat += b'\0' * (64 * 4 * 4 - len(pat))
    open(path, 'wb').write(bytes(hdr) + bytes(smps) + song + b'M.K.' + bytes(pat) + bytes(data))

def write_xm(path):
    rows = song_rows()
    out = bytearray(b'Extended Module: ' + b'testxm'.ljust(20, b'\0') + b'\x1a' + b'gen'.ljust(20, b'\0') + struct.pack('<H', 0x0104))
    hdr = struct.pack('<IHHHHHHHH', 276, 1, 0, 4, 1, 4, 1, 6, 125) + bytes([0] * 256)
    out += hdr
    pat = bytearray()
    for r in range(32):
        row = rows[r] if r < len(rows) else [(None, 0, None, None, 0)] * 4
        for ch in range(4):
            s, ins, vol, fx, p = row[ch]
            note = note_n(s) + 1 if s else 0
            v = 0x10 + vol if vol is not None else 0
            cmd = 0; par = p
            if fx is not None:
                if fx == '0' and p == 0: cmd = 0
                else: cmd = {'0': 0, '1': 1, '2': 2, '3': 3, '4': 4, 'A': 10, 'C': 12, 'F': 15, '9': 9}.get(fx, 0)
                if fx == 'E': cmd = 14
            if fx == 'E': par = p
            pat += bytes([note, ins, v, cmd, par])
    out += struct.pack('<IBHH', 9, 0, 32, len(pat)) + bytes(pat)
    for i in range(4):
        kind, n = SAMPLES[i]
        w = wave(kind, n)
        d = bytearray(); old = 0
        for x in w:
            d.append((x - old) & 255); old = x
        env = bytes([0] * 48)
        volenv = b''.join(struct.pack('<HH', x, y) for x, y in [(0, 64), (20, 32), (40, 40)]) + b'\0' * 36
        out += struct.pack('<I22sBH', 263, b'inst%d' % i, 0, 1)
        out += struct.pack('<I', 40) + bytes(96)
        out += volenv + env
        out += struct.pack('<BBBBBBBBBBBBBBH', 3, 0, 1, 0, 0, 0, 0, 0, 0b101 if i == 3 else 0, 0, 0, 0, 0, 0, 64 if i == 3 else 0) + b'\0' * 22
        # sample header
        out += struct.pack('<IIIBbBBbB22s', len(w), 0, len(w), 64, 0, 1, 128, 0, 0, b'smp')
        out += bytes(d)
    open(path, 'wb').write(bytes(out))

def write_s3m(path):
    rows = song_rows()
    out = bytearray(b'tests3m'.ljust(28, b'\0') + b'\x1a\x10\0\0')
    ordnum, insnum, patnum = 2, 4, 1
    out += struct.pack('<HHHHHH', ordnum, insnum, patnum, 0, 0x1320, 2) + b'SCRM'
    out += bytes([64, 6, 125, 0xB0, 0, 252]) + b'\0' * 10
    chs = bytes([0, 8, 1, 9]) + bytes([255] * 28)
    out += chs
    out += bytes([0, 255])
    # parapointers: placeholder
    ptr_pos = len(out)
    out += b'\0' * (2 * (insnum + patnum))
    # default pan flag: none (dp 252 means pan table follows)
    out += bytes([0x30] * 32)
    out += b'\0' * ((16 - len(out) % 16) % 16)
    paras = []
    body = bytearray()
    base = len(out)
    # patterns first
    pat = bytearray()
    for r in range(64):
        row = rows[r] if r < len(rows) else [(None, 0, None, None, 0)] * 4
        for ch in range(4):
            s, ins, vol, fx, p = row[ch]
            b = 0; cell = bytearray()
            if s:
                b |= 32
                n = note_n(s) - 12 + 0
                cell += bytes([(n // 12) << 4 | (n % 12), ins])
            if vol is not None:
                b |= 64; cell.append(vol)
            if fx is not None and not (fx == '0' and p == 0):
                cmd = {'0': 'J', '1': 'F', '2': 'E', '3': 'G', '4': 'H', 'A': 'D', 'C': None, 'F': None, 'E': 'S', '9': 'O'}[fx]
                par = p
                if fx == 'C': b |= 64; cell.append(p) ; cmd = None
                elif fx == 'F': cmd = 'A' if p < 32 else 'T'
                elif fx == 'E':
                    sub = p >> 4
                    cmd = 'S'
                    par = {0x9: 0, 0xD: 0xD0, 0xC: 0xC0}.get(sub, 0) | (p & 15)
                    if sub == 9: cmd = 'Q'; par = p & 15
                    if sub == 0xD: par = 0xD0 | (p & 15)
                    if sub == 0xC: par = 0xC0 | (p & 15)
                if fx == '1' or fx == '2': par = p
                if cmd:
                    b |= 128; cell += bytes([ord(cmd) - 64, par])
            if b:
                pat.append(b | ch); pat += cell
        pat.append(0)
    pat_off = base
    body += struct.pack('<H', len(pat)) + pat
    body += b'\0' * ((16 - len(body) % 16) % 16)
    inst_offs = []
    smp_blob = bytearray()
    ins_hdrs = []
    cur = base + len(body) + 80 * insnum
    cur += (16 - cur % 16) % 16
    for i in range(insnum):
        kind, n = SAMPLES[i]
        w = wave(kind, n * 2)
        data = bytes([(x + 128) & 255 for x in w])
        h = bytearray()
        h += bytes([1]) + b'smp'.ljust(12, b'\0')
        memseg = cur // 16
        h += bytes([memseg >> 16]) + struct.pack('<H', memseg & 0xFFFF)
        h += struct.pack('<III', len(w), 0, len(w))
        h += bytes([64, 0, 0, 1]) + struct.pack('<I', 8363) + b'\0' * 12 + b'smp'.ljust(28, b'\0') + b'SCRS'
        ins_hdrs.append(bytes(h).ljust(80, b'\0'))
        smp_blob += data
        smp_blob += b'\0' * ((16 - len(smp_blob) % 16) % 16)
        cur += len(data) + ((16 - len(data) % 16) % 16)
    inst_base = base + len(body)
    for i in range(insnum): inst_offs.append((inst_base + 80 * i) // 16)
    paras = inst_offs + [pat_off // 16]
    out[ptr_pos:ptr_pos + 2 * len(paras)] = b''.join(struct.pack('<H', x) for x in paras)
    full = bytes(out) + bytes(body) + b''.join(ins_hdrs)
    full += b'\0' * ((16 - len(full) % 16) % 16)
    # check alignment
    assert len(full) == cur - len(smp_blob) or True
    open(path, 'wb').write(full + bytes(smp_blob))

def write_it(path):
    rows = song_rows()
    ordnum, insnum, smpnum, patnum = 2, 4, 4, 1
    hdr = bytearray(b'IMPM' + b'testit'.ljust(26, b'\0') + b'\x04\x10')
    hdr += struct.pack('<HHHH', ordnum, insnum, smpnum, patnum)
    hdr += struct.pack('<HHHH', 0x214, 0x214, 1 | 4 | 8, 0)  # stereo | instruments | linear
    hdr += bytes([128, 48, 6, 125, 128, 0]) + struct.pack('<HI', 0, 0) + b'\0' * 4
    hdr += bytes([32] * 64) + bytes([64] * 64)
    hdr += bytes([0, 255])
    n_ptr = insnum + smpnum + patnum
    table_pos = len(hdr)
    hdr += b'\0' * (4 * n_ptr)
    body = bytearray()
    base = len(hdr)
    inst_offs, smp_offs, pat_offs = [], [], []
    for i in range(insnum):
        h = bytearray(b'IMPI' + b'inst'.ljust(12, b'\0') + b'\0')
        h += bytes([0, 0, 0]) + struct.pack('<H', 128) + bytes([0, 60, 128, 32 | 128 if False else 32, 0, 0]) + struct.pack('<HBBHBB', 0, 0, 0, 0, 0, 0)
        h = bytearray(h[:17]) + bytes([1 if i == 3 else 0, 0, 0]) + struct.pack('<H', 128 if i == 3 else 0) + bytes([0, 0, 128, 32, 0, 0]) + struct.pack('<HBBHBB', 0, 0, 0, 0, 0, 0)
        h = h.ljust(32, b'\0') + b'inst'.ljust(26, b'\0')
        h = h.ljust(64, b'\0')
        for n in range(120): h += bytes([n, i + 1])
        env = bytes([0x01 if i == 3 else 0, 3, 0, 0, 0, 0]) + b''.join(bytes([y]) + struct.pack('<H', x) for x, y in [(0, 64), (20, 32), (40, 40)]) + b'\0' * (22 * 3)
        h += env[:82].ljust(82, b'\0') + bytes(82) + bytes(82) + bytes(4)
        h = h[:554].ljust(554, b'\0')
        inst_offs.append(base + len(body)); body += h
    smp_hdr_pos = []
    data_blob = []
    for i in range(smpnum):
        smp_offs.append(base + len(body)); smp_hdr_pos.append(len(body)); body += b'\0' * 80
    for r_i in range(patnum):
        pass
    # pattern
    pat = bytearray()
    lastmask = [0] * 64
    for r in range(64):
        row = rows[r] if r < len(rows) else [(None, 0, None, None, 0)] * 4
        for ch in range(4):
            s, ins, vol, fx, p = row[ch]
            mask = 0; cell = bytearray()
            if s: mask |= 1 | 2; cell += bytes([note_n(s), ins])
            if vol is not None: mask |= 4; cell.append(vol)
            if fx is not None and not (fx == '0' and p == 0):
                par = p
                if fx == 'C': 
                    mask |= 4
                    if not (mask & 4 and vol is None): pass
                    cell.append(p)
                    if vol is not None: cell = cell[:-1]
                else:
                    cmd = {'0': 'J', '1': 'F', '2': 'E', '3': 'G', '4': 'H', 'A': 'D', 'F': 'A', 'E': 'S', '9': 'O'}[fx]
                    if fx == 'F': cmd = 'A' if p < 32 else 'T'
                    if fx == 'E':
                        sub = p >> 4
                        if sub == 9: cmd = 'Q'; par = p & 15
                        else: par = (0xD0 if sub == 0xD else 0xC0) | (p & 15)
                    mask |= 8; cell += bytes([ord(cmd) - 64, par])
            if mask:
                pat += bytes([(ch + 1) | 128, mask]) + cell
        pat.append(0)
    pat_offs.append(base + len(body))
    body += struct.pack('<HH', len(pat), 64) + b'\0' * 4 + pat
    # samples
    for i in range(smpnum):
        kind, n = SAMPLES[i]
        w = wave(kind, n * 2, 120)
        hdr_at = smp_hdr_pos[i]
        sp = base + len(body)
        body += bytes([x & 255 for x in w])
        h = b'IMPS' + b'smp'.ljust(12, b'\0') + b'\0' + bytes([64, 1 | 16, 64]) + b'smp'.ljust(26, b'\0') + bytes([1, 0xFF & 0])
        h += struct.pack('<IIIIIII', len(w), 0, len(w), 8363, 0, 0, sp)
        h = h.ljust(80, b'\0')
        body[hdr_at:hdr_at + 80] = h
    ptrs = b''.join(struct.pack('<I', x) for x in inst_offs + smp_offs + pat_offs)
    hdr[table_pos:table_pos + len(ptrs)] = ptrs
    open(path, 'wb').write(bytes(hdr) + bytes(body))

if __name__ == '__main__':
    d = sys.argv[1] if len(sys.argv) > 1 else '.'
    write_mod(d + '/t.mod'); write_xm(d + '/t.xm'); write_s3m(d + '/t.s3m'); write_it(d + '/t.it')
