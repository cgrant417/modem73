#!/usr/bin/env python3
"""Play a steady 1 kHz tone out of one card, record on the other, and find discontinuities.

A clean path gives a sine whose phase advances smoothly; a dropped/inserted USB packet
shows up as a phase jump. SRC and DST are ALSA card numbers joined by an audio cable.
Needs alsa-utils and sox. Usage: audio_glitch.py SRC DST [seconds]
"""
import math, struct, subprocess, sys, time, wave

src, dst = sys.argv[1], sys.argv[2]
secs = int(sys.argv[3]) if len(sys.argv) > 3 else 20
RATE, F = 48000, 1000.0
tone = "/tmp/glitch_tone.wav"
rec = f"/tmp/glitch_{src}to{dst}.wav"
subprocess.run(["sox", "-n", "-r", str(RATE), "-c", "1", "-b", "16", tone, "synth", str(secs), "sine", str(int(F)), "vol", "0.5"], check=True)
r = subprocess.Popen(["arecord", "-q", "-D", f"plughw:{dst},0", "-f", "S16_LE", "-r", str(RATE), "-c", "1", "-d", str(secs + 2), rec])
time.sleep(0.5)
subprocess.run(["aplay", "-q", "-D", f"plughw:{src},0", tone], check=True)
r.wait()

w = wave.open(rec)
n = w.getnframes()
x = struct.unpack(f"<{n}h", w.readframes(n))
# Measure the tone's phase in 10 ms blocks (480 samples = 10 cycles of 1 kHz)
B = 480
phases, amps = [], []
for i in range(0, n - B, B):
    c = s = 0.0
    for k in range(B):
        a = 2 * math.pi * F * (i + k) / RATE
        c += x[i + k] * math.cos(a); s += x[i + k] * math.sin(a)
    amps.append(math.hypot(c, s) * 2 / B / 32768)
    phases.append(math.atan2(s, c))
on = [i for i, a in enumerate(amps) if a > 0.05]
if not on:
    print(f"{src}->{dst}: no tone received"); sys.exit(1)
a0, a1 = on[0] + 5, on[-1] - 5
jumps = []
drift = []
for i in range(a0 + 1, a1):
    d = (phases[i] - phases[i - 1] + math.pi) % (2 * math.pi) - math.pi
    drift.append(d)
med = sorted(drift)[len(drift) // 2]
for j, d in enumerate(drift):
    e = (d - med + math.pi) % (2 * math.pi) - math.pi
    if abs(e) > 0.2:
        jumps.append((round((a0 + 1 + j) * B / RATE, 2), round(e * RATE / (2 * math.pi * F), 1)))
ppm = med / (2 * math.pi * F * B / RATE) * 1e6
print(f"{src}->{dst}: {(a1 - a0) * B / RATE:.1f}s of tone, clock offset {ppm:+.1f} ppm, "
      f"{len(jumps)} discontinuities")
for t, samp in jumps[:15]:
    print(f"   t={t:6.2f}s  jump of {samp:+.1f} samples")
