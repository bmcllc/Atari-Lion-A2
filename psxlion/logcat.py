#!/usr/bin/env python3
"""Prints the psxlion log ring from a --dump-ram file: logcat.py dump.bin [tail lines].

Layout (rt/hw.c): u32 big-endian count of every byte ever written, then the text, wrapping at the end of the dump."""
import struct, sys
d = open(sys.argv[1], 'rb').read()
n = struct.unpack('>I', d[:4])[0]
buf, cap = d[4:], len(d) - 4
if n <= cap:
    text = buf[:n]
else:                                   # wrapped: oldest byte is at n % cap; drop the partial first line
    at = n % cap
    text = buf[at:] + buf[:at]
    text = text[text.find(b'\n') + 1:]
lines = text.decode('latin1').split('\n')
k = int(sys.argv[2]) if len(sys.argv) > 2 else 0
print('\n'.join(lines[-k:] if k else lines))
