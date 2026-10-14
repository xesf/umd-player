#!/usr/bin/env python3
"""Compare two raw interleaved float32 PCM files. Exit 1 if 16-bit output differs by more than 1 LSB."""
import array
import math
import sys

a = array.array('f', open(sys.argv[1], 'rb').read())
b = array.array('f', open(sys.argv[2], 'rb').read())
n = min(len(a), len(b))
maxd = err = sig = 0.0
s16 = 0
for i in range(n):
    d = abs(a[i] - b[i])
    maxd = max(maxd, d)
    err += d * d
    sig += b[i] * b[i]
    x = max(-32768, min(32767, round(a[i] * 32768)))
    y = max(-32768, min(32767, round(b[i] * 32768)))
    s16 = max(s16, abs(x - y))
snr = 10 * math.log10(sig / err) if err else float('inf')
print('samples %d/%d, max diff %.2g, SNR %.1f dB, max s16 diff %d LSB' % (len(a), len(b), maxd, snr, s16))
sys.exit(0 if len(a) == len(b) and s16 <= 1 else 1)
