#!/usr/bin/env python3
"""compare.py mine.wav ref.wav: per 0.25 s window, RMS and dominant frequency of both renders."""
import sys, wave
import numpy as np
def load(p):
    w = wave.open(p); n = w.getnframes(); a = np.frombuffer(w.readframes(n), dtype='<i2').reshape(-1, w.getnchannels()).astype(float)
    return a.mean(axis=1), w.getframerate()
a, ra = load(sys.argv[1]); b, rb = load(sys.argv[2])
win = int(0.25 * ra)
n = min(len(a), len(b)) // win
print(" t    rmsA   rmsB   fA     fB")
for i in range(n):
    x = a[i*win:(i+1)*win]; y = b[i*win:(i+1)*win]
    def pk(s):
        if np.sqrt((s**2).mean()) < 50: return 0
        sp = np.abs(np.fft.rfft(s * np.hanning(len(s)))); return np.argmax(sp[1:]) * ra / len(s) + ra / len(s)
    print("%4.2f %6.0f %6.0f %6.0f %6.0f" % (i*0.25, np.sqrt((x**2).mean()), np.sqrt((y**2).mean()), pk(x), pk(y)))
