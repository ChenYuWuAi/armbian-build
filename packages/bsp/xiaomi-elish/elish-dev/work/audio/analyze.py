#!/usr/bin/env python3
"""Measure loudness (RMS dBFS) and THD of a 1 kHz tone captured from a microphone.
Usage: analyze.py <wav> [expected_f0]
"""
import sys, wave
import numpy as np

path = sys.argv[1]
f0_exp = float(sys.argv[2]) if len(sys.argv) > 2 else 1000.0

with wave.open(path, 'rb') as w:
    n_ch, sw, sr, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    raw = w.readframes(n)
dtype = {1: np.int8, 2: np.int16, 4: np.int32}[sw]
x = np.frombuffer(raw, dtype=dtype).astype(np.float64)
if n_ch > 1:
    x = x.reshape(-1, n_ch).mean(axis=1)
if sw == 2:
    x /= 32768.0
elif sw == 4:
    x /= 2147483648.0
elif sw == 1:
    x /= 128.0

if len(x) == 0:
    print("EMPTY capture"); sys.exit(1)

rms = float(np.sqrt(np.mean(x ** 2)))
peak = float(np.max(np.abs(x)))
dbfs = 20 * np.log10(rms) if rms > 0 else -999.0
peak_dbfs = 20 * np.log10(peak) if peak > 0 else -999.0

# spectrum
N = 1 << int(np.floor(np.log2(min(len(x), 1 << 18))))
seg = x[:N] * np.hanning(N)
sp = np.abs(np.fft.rfft(seg)) / (N / 4)
freqs = np.fft.rfftfreq(N, 1.0 / sr)

# find strongest peak in 200..5000 Hz
band = (freqs >= 200) & (freqs <= 5000)
pk = np.argmax(sp[band])
f_peak = freqs[band][pk]
a_peak = sp[band][pk]

def bin_at(f):
    i = int(round(f * N / sr))
    lo, hi = max(0, i - 2), min(len(sp), i + 3)
    return float(np.max(sp[lo:hi]))

# THD: harmonics 2..6 relative to fundamental (use the measured peak freq)
h = [bin_at(f_peak * k) for k in range(2, 7)]
thd = float(np.sqrt(sum(v * v for v in h)) / a_peak * 100) if a_peak > 0 else -1.0
# noise floor: median of spectrum above 5 kHz
nf = float(np.median(sp[freqs > 5000])) if np.any(freqs > 5000) else 0.0
snr = 20 * np.log10(a_peak / nf) if nf > 0 else -1.0

print(f"file       : {path}")
print(f"rate/ch    : {sr} Hz / {n_ch}ch   samples={len(x)} ({len(x)/sr:.2f}s)")
print(f"RMS        : {rms:.6f}  = {dbfs:8.2f} dBFS")
print(f"PEAK       : {peak:.6f}  = {peak_dbfs:8.2f} dBFS")
print(f"peak freq  : {f_peak:8.1f} Hz (expected {f0_exp:.0f}, amp {a_peak:.6f})")
print(f"THD(2-6)   : {thd:8.2f} %")
print(f"SNR(5k+)   : {snr:8.2f} dB")
print("top bins   :", ", ".join(f"{freqs[i]:.0f}Hz:{sp[i]:.5f}"
      for i in np.argsort(sp)[::-1][:5]))
