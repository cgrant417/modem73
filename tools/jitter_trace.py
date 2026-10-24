#!/usr/bin/env python3
"""Extract a timing-jitter trace from a recording of a steady tone (see audio_glitch.py).

The tone's phase against an ideal tone at the nominal sample rate gives the path delay
tau(t). The trace is written at 1 kHz as little-endian float32 microseconds, with the mean
removed but the linear drift (clock offset) kept, for tests/phy_jitter --trace.
A 1 ms packet slip is a whole cycle at 1 kHz and nearly invisible; record a low tone
(e.g. 300 Hz) and pass tone_hz to see slips (tools/slip_correlate.py finds them).
Needs numpy. Usage: jitter_trace.py REC.wav OUT.f32 [tone_hz]
"""
import sys, wave
import numpy as np

src, out = sys.argv[1], sys.argv[2]
F = float(sys.argv[3]) if len(sys.argv) > 3 else 1000.0
w = wave.open(src); r = w.getframerate()
x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(float) / 32768
n = np.arange(len(x))
z = x * np.exp(-2j * np.pi * F * n / r)
L = int(round(r / F)) * 10               # 10 tone cycles: removes the 2F image
z = np.convolve(z, np.ones(L) / L, mode="valid")
hop = r // 1000
z = z[::hop]
amp = np.abs(z)
on = np.flatnonzero(amp > 0.25 * amp.max())
z = z[on[0] + 50:on[-1] - 50]            # drop the tone's edges
tau_us = -np.unwrap(np.angle(z)) / (2 * np.pi * F) * 1e6
t = np.arange(len(tau_us)) / 1000.0
slope, icpt = np.polyfit(t, tau_us, 1)
res = tau_us - (slope * t + icpt)
spec = np.abs(np.fft.rfft(res * np.hanning(len(res))))
fr = np.fft.rfftfreq(len(res), 1e-3)
k = 1 + np.argmax(spec[1:])
d1 = np.diff(res)
print(f"{src}: {t[-1]:.1f} s, clock offset {slope:+.1f} ppm, wobble rms {res.std():.1f} us, "
      f"p-p {res.max() - res.min():.1f} us, strongest at {fr[k]:.2f} Hz, "
      f"max step {np.abs(d1).max():.1f} us/ms")
(tau_us - tau_us.mean()).astype("<f4").tofile(out)
