#!/usr/bin/env python3
"""Match timing jumps in a tone recording against USB capture errors from the kernel.

Records made with a CM108-type USB sound card can lose a 1 ms isochronous packet
("frame 0 active: -71" with the snd_usb_audio retire_capture_urb debug message on).
A lost packet shifts everything after it by 1 ms. This finds such steps in the
recording's phase and lines them up with the dmesg error times.

Use a low tone (default 300 Hz): with 1 kHz a 1 ms step is exactly one cycle and
almost invisible in the phase.

Usage: slip_correlate.py REC.wav START_UPTIME DMESG.txt USBDEV [tone_hz]
  START_UPTIME  /proc/uptime (seconds) when the recording started
  DMESG.txt     output of `dmesg` (raw [seconds] timestamps)
  USBDEV        kernel name of the capture device, e.g. 1-7.1
Needs numpy.
"""
import re
import sys
import wave

import numpy as np

src, t0, dm, dev = sys.argv[1], float(sys.argv[2]), sys.argv[3], sys.argv[4]
F = float(sys.argv[5]) if len(sys.argv) > 5 else 300.0

w = wave.open(src)
r = w.getframerate()
x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(float) / 32768
n = np.arange(len(x))
z = x * np.exp(-2j * np.pi * F * n / r)
L = int(round(r / F * 4))                 # 4 tone cycles: removes the 2F image
z = np.convolve(z, np.ones(L) / L, mode="valid")
hop = r // 1000
z = z[::hop]
amp = np.abs(z)
on = np.flatnonzero(amp > 0.25 * amp.max())
first, last = on[0] + 50, on[-1] - 50
z = z[first:last]
tau = -np.unwrap(np.angle(z)) / (2 * np.pi * F) * 1e6      # microseconds
t = (np.arange(len(tau)) + first) / 1000.0                  # seconds from record start

# step = change of tau across a 30 ms gap, compared over a short window so slow
# wobble (tens of us over ~100 ms) stays below the threshold
G = 30
d = tau[G:] - tau[:-G]
thr = 400.0
steps = []
i = 0
while i < len(d):
    if abs(d[i]) > thr:
        j = i + np.argmax(np.abs(d[i:i + G]))
        steps.append((t[j + G // 2], d[j]))
        i = j + 2 * G
    else:
        i += 1

errs = []
for line in open(dm, errors="replace"):
    m = re.match(r"\[\s*([0-9.]+)\]\s+usb (\S+): frame \d+ active: (-?\d+)", line)
    if m and m.group(2) == dev:
        te = float(m.group(1)) - t0
        if t[0] - 1 <= te <= t[-1] + 1:
            errs.append((te, int(m.group(3))))

print(f"{src}: {t[-1] - t[0]:.1f} s of tone at {F:.0f} Hz, capture device {dev}")
print(f"  timing steps > {thr:.0f} us: {len(steps)}   kernel capture errors: {len(errs)}")
TOL = 0.25
matched = 0
used = set()
for ts, size in steps:
    best = min(range(len(errs)), key=lambda k: abs(errs[k][0] - ts), default=None)
    ok = best is not None and abs(errs[best][0] - ts) <= TOL and best not in used
    if ok:
        used.add(best)
        matched += 1
    print(f"  step at {ts:7.3f} s  {size:+7.0f} us   " +
          (f"error {errs[best][1]} at {errs[best][0]:7.3f} s (dt {1000 * (errs[best][0] - ts):+.0f} ms)"
           if ok else "no kernel error nearby"))
for k, (te, code) in enumerate(errs):
    if k not in used:
        print(f"  kernel error {code} at {te:7.3f} s with no timing step")
print(f"  matched {matched} of {len(steps)} steps; {len(errs) - len(used)} errors unmatched")
