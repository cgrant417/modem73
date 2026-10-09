#!/usr/bin/env python3
"""KISS regression test: baseline vs refactored modem73 over an ALSA file loopback.

TX run:  KISS frames in -> modem73 -> ALSA 'file' plugin records the audio to a raw file
         (paced by the real sound card, PTT none).
RX run:  ALSA 'file' plugin plays that raw file in as capture -> modem73 decodes ->
         every connected KISS client must receive exactly the expected KISS bytes.

Each modem73 run gets a throwaway HOME, so ~/.asoundrc and modem73 settings are isolated.
Every binary transmits once; every binary then decodes every recording. With two binaries
the TX and RX logs are also compared (decoder SNR lines vary slightly run to run).

Needs: Linux, python3, a real ALSA capture/playback card for timing (no radio, PTT stays
off), and modem73 built with ALSA (do not install libpulse: miniaudio would prefer Pulse).

Usage:
  tools/kiss_regress.py base=/path/to/master/modem73 new=./modem73
  RX_REPEAT=base:3 tools/kiss_regress.py base=... new=...   # re-decode air_base 3x per binary
Env: M73_CARD (default plughw:0,0), M73_PORT (default 18001), M73_WORK (default /tmp/m73regress)

Baseline build, e.g.:
  git archive master | (mkdir -p ~/m73-base && tar -x -C ~/m73-base) && make -C ~/m73-base
"""
import os, re, shutil, socket, subprocess, sys, time

WORK = os.environ.get("M73_WORK", "/tmp/m73regress")
PORT = int(os.environ.get("M73_PORT", "18001"))
CARD = os.environ.get("M73_CARD", "plughw:0,0")
FEND, FESC, TFEND, TFESC = 0xC0, 0xDB, 0xDC, 0xDD

FRAMES = [
    b"M73TEST frame 1 plain ascii",
    bytes([0x01, FEND, 0x02, FESC, 0x03, TFEND, TFESC, FEND, FEND, FESC, FESC]) + b" escapes",
    bytes((i * 37 + 11) & 0xFF for i in range(300)),
]

def wrap(data, port=0):
    out = bytearray([FEND, (port << 4) | 0x00])
    for b in data:
        if b == FEND: out += bytes([FESC, TFEND])
        elif b == FESC: out += bytes([FESC, TFESC])
        else: out.append(b)
    out.append(FEND)
    return bytes(out)

def asoundrc(home, playback_file=None, capture_infile=None):
    def chain(inner):
        return f"""{{ type plug
    slave {{ pcm {{ {inner} }} format S16_LE rate 48000 channels 1 }} }}"""
    play = chain(f'type file slave.pcm "{CARD}" file "{playback_file}" format raw') \
        if playback_file else f'"{CARD}"'
    cap = chain(f'type file slave.pcm "{CARD}" file "/dev/null" infile "{capture_infile}" format raw') \
        if capture_infile else f'"{CARD}"'
    with open(os.path.join(home, ".asoundrc"), "w") as f:
        f.write(f"pcm.!default {{ type asym\n  playback.pcm {play}\n  capture.pcm {cap}\n}}\n")

def start(binary, home, log):
    env = dict(os.environ, HOME=home)
    env.pop("PULSE_SERVER", None)
    return subprocess.Popen([binary, "--headless", "--port", str(PORT), "--control-port", "0",
                             "--ptt", "none"],
                            stdout=log, stderr=subprocess.STDOUT, env=env, cwd=home)

def connect(retries=50):
    for _ in range(retries):
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=2)
            s.settimeout(0.2)
            return s
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("could not connect to KISS port")

def stop(p):
    p.send_signal(2)
    try: p.wait(timeout=10)
    except subprocess.TimeoutExpired:
        p.kill(); p.wait()

def drain(socks, bufs, seconds):
    end = time.time() + seconds
    while time.time() < end:
        for i, s in enumerate(socks):
            try:
                d = s.recv(65536)
                if d: bufs[i] += d
            except socket.timeout:
                pass
            except OSError:
                pass

def read(path):
    with open(path, errors="replace") as f:
        return f.read()

def tx_run(name, binary):
    home = os.path.join(WORK, f"tx_{name}")
    os.makedirs(home)
    air = os.path.join(WORK, f"air_{name}.raw")
    asoundrc(home, playback_file=air)
    logp = os.path.join(WORK, f"tx_{name}.log")
    with open(logp, "w") as log:
        p = start(binary, home, log)
        time.sleep(3)
        c1, c2 = connect(), connect()
        time.sleep(0.5)
        c1.sendall(bytes([FEND, 0x01, 50, FEND]))           # TXDELAY (ignored)
        c1.sendall(bytes([FEND, 0x05, 0x01, FEND]))         # FULLDUPLEX (logged, ignored)
        c1.sendall(bytes([FEND, 0x0E, 0x00, FEND]))         # unknown command
        for fr in FRAMES:
            c1.sendall(wrap(fr))
        deadline = time.time() + 60
        while time.time() < deadline and len(re.findall(r"TX: \d+ samples", read(logp))) < len(FRAMES):
            time.sleep(0.5)
        time.sleep(3)
        c2.close()
        time.sleep(0.5)
        c1.close()
        time.sleep(0.5)
        stop(p)
    return air, logp

def rx_run(name, binary, air):
    home = os.path.join(WORK, f"rx_{name}")
    os.makedirs(home)
    asoundrc(home, capture_infile=air)
    logp = os.path.join(WORK, f"rx_{name}.log")
    seconds = os.path.getsize(air) / (48000 * 2)
    with open(logp, "w") as log:
        p = start(binary, home, log)
        socks = [connect(), connect()]
        bufs = [b"", b""]
        drain(socks, bufs, seconds + 3)
        # client limit: 2 open + 16 more -> the 17th is rejected
        extra = [connect() for _ in range(16)]
        drain(socks, bufs, 1.5)
        for s in extra: s.close()
        for s in socks: s.close()
        time.sleep(1)
        stop(p)
    return bufs, logp

NOISE = re.compile(r"(Client connected: 127\.0\.0\.1:)\d+|MODEM73 build .*|SNR=-?\d+dB|"
                   r"CSMA: .*|MFSK: Sync.*|\d+ samples, [\d.]+ seconds|Waiting for .*")

def summary(logp):
    keep = []
    for line in read(logp).splitlines():
        line = NOISE.sub(lambda m: (m.group(1) + "N") if m.group(1) else "<v>", line)
        if line.strip() and line != "<v>":
            keep.append(line)
    return keep

def rx_repeat(bins, src, n):
    """Decode one recorded air file n times per binary; print per-frame SNR to expose run-to-run jitter."""
    expected = b"".join(wrap(f) for f in FRAMES)
    air = os.path.join(WORK, f"air_{src}.raw")
    for i in range(n):
        for name, binary in bins.items():
            tag = f"rep{i}_{name}"
            shutil.rmtree(os.path.join(WORK, f"rx_{tag}"), ignore_errors=True)
            bufs, log = rx_run(tag, binary, air)
            snrs = re.findall(r"Frame decoded \d+ bytes, SNR=([-\d.]+) dB, BER=([\d.]+)%", read(log))
            match = all(b == expected for b in bufs)
            print(f"run {i} {name:4s}: KISS {'MATCH' if match else 'MISMATCH'}  "
                  + "  ".join(f"SNR {float(a):5.1f} BER {float(b):4.1f}%" for a, b in snrs), flush=True)

def main():
    if os.environ.get("RX_REPEAT"):
        src, n = os.environ["RX_REPEAT"].split(":")
        return rx_repeat(dict(a.split("=", 1) for a in sys.argv[1:]), src, int(n))
    bins = dict(a.split("=", 1) for a in sys.argv[1:])
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK)
    expected = b"".join(wrap(f) for f in FRAMES)
    results, ok = {}, True

    for name, binary in bins.items():
        print(f"== TX run: {name}", flush=True)
        air, log = tx_run(name, binary)
        results[f"tx_{name}"] = summary(log)
        print(f"   air file {os.path.getsize(air)} bytes, "
              f"{len(re.findall(r'TX: \d+ bytes', read(log)))} frames transmitted")

    first_air = os.path.join(WORK, f"air_{next(iter(bins))}.raw")
    for name, binary in bins.items():
        for src in bins:
            air = os.path.join(WORK, f"air_{src}.raw")
            print(f"== RX run: {name} decoding air_{src}", flush=True)
            bufs, log = rx_run(f"{name}_from_{src}", binary, air)
            results[f"rx_{name}_from_{src}"] = summary(log)
            for i, b in enumerate(bufs):
                good = b == expected
                ok &= good
                print(f"   client {i+1}: {len(b)} bytes, {'MATCH' if good else 'MISMATCH'}")
                if not good:
                    print(f"     expected {expected.hex()}\n     got      {b.hex()}")

    names = list(bins)
    if len(names) == 2:
        a, b = names
        for kind in ["tx_{}", "rx_{}_from_" + a]:
            la, lb = results[kind.format(a)], results[kind.format(b)]
            same = la == lb
            ok &= same
            print(f"== log compare {kind.format('*')}: {'IDENTICAL' if same else 'DIFFERENT'}")
            if not same:
                import difflib
                for d in difflib.unified_diff(la, lb, a, b, lineterm="", n=0):
                    print("   " + d)
    print("RESULT:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)

main()
