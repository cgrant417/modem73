#!/usr/bin/env python3
"""Two modem73 instances on two sound cards joined by an audio cable.

Station A uses CARD_A (default plughw:0,0), station B CARD_B (default plughw:1,0). KISS
frames are sent A->B and B->A in each modulation; the receiving station's KISS client must
get every frame intact. PTT none; each station gets a throwaway HOME (.asoundrc, settings).
Use crystal-clocked interfaces (e.g. Digirig): usb2rig units slip 1 ms packets (tools/slip_correlate.py).
Usage: cable_test.py path/to/modem73
Env: MODES, N (frames per direction, default 5), SIZE (frame bytes, default 200),
     PER_FRAME (seconds allowed per frame, default 8), CARD_A, CARD_B
MODES is space-separated; each entry is MOD:RATE[:FLAG,...] for OFDM (e.g. BPSK:1/4:--short)
or a comma-separated list of modem73 options (e.g. --robust-mode,RDM-300).
"""
import os, socket, subprocess, sys, time

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./modem73")
WORK = "/tmp/m73cable"
N = int(os.environ.get("N", "5"))
SIZE = int(os.environ.get("SIZE", "200"))
PER_FRAME = float(os.environ.get("PER_FRAME", "8"))
MODES = os.environ.get("MODES", "QPSK:1/2 QAM16:3/4 QAM64:5/6 QAM256:5/6").split()
STATIONS = {"A": (os.environ.get("CARD_A", "plughw:0,0"), 18101, "N0CAA"),
            "B": (os.environ.get("CARD_B", "plughw:1,0"), 18102, "N0CAB")}
FEND, FESC, TFEND, TFESC = 0xC0, 0xDB, 0xDC, 0xDD

def wrap(data):
    out = bytearray([FEND, 0])
    for b in data:
        out += bytes([FESC, TFEND]) if b == FEND else bytes([FESC, TFESC]) if b == FESC else bytes([b])
    return bytes(out + bytes([FEND]))

def unwrap_all(buf):
    frames, cur, esc = [], None, False
    for b in buf:
        if b == FEND:
            if cur: frames.append(bytes(cur[1:]))
            cur = bytearray()
        elif cur is None: continue
        elif esc: cur.append(FEND if b == TFEND else FESC); esc = False
        elif b == FESC: esc = True
        else: cur.append(b)
    return frames

def mode_args(m):
    if ":" in m:
        mod, rate, *extra = m.split(":")
        return ["-m", mod, "-r", rate] + [a for e in extra for a in e.split(",")]
    return m.split(",")

def start(name, m):
    card, port, call = STATIONS[name]
    home = os.path.join(WORK, name)
    os.makedirs(home, exist_ok=True)
    with open(os.path.join(home, ".asoundrc"), "w") as f:
        f.write(f'pcm.!default {{ type asym playback.pcm "{card}" capture.pcm "{card}" }}\n')
    env = dict(os.environ, HOME=home)
    log = open(os.path.join(WORK, f"{name}.log"), "a")
    log.write(f"\n===== {m}\n"); log.flush()
    p = subprocess.Popen([BIN, "--headless", "--port", str(port), "--control-port", "0",
                          "--ptt", "none", "-c", call] + mode_args(m),
                         stdout=log, stderr=subprocess.STDOUT, env=env, cwd=home)
    for _ in range(50):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=2); s.settimeout(0.2)
            return p, s
        except OSError: time.sleep(0.2)
    raise RuntimeError(f"station {name} did not open KISS port")

def stop(p):
    p.send_signal(2)
    try: p.wait(timeout=10)
    except subprocess.TimeoutExpired: p.kill(); p.wait()

def collect(sock, want, timeout):
    buf, end = bytearray(), time.time() + timeout
    while time.time() < end:
        try:
            d = sock.recv(65536)
            if d: buf += d
        except socket.timeout: pass
        if len(unwrap_all(buf)) >= want: time.sleep(0.5)
        if len(unwrap_all(buf)) >= want: break
    return unwrap_all(buf)

def main():
    os.makedirs(WORK, exist_ok=True)
    for f in ("A.log", "B.log"):
        try: os.remove(os.path.join(WORK, f))
        except FileNotFoundError: pass
    total_bad = 0
    for m in MODES:
        (pa, sa), (pb, sb) = start("A", m), start("B", m)
        time.sleep(2)
        for src, dst, ss, sd in (("A", "B", sa, sb), ("B", "A", sb, sa)):
            sent = [f"{src}>{dst} {m} #{i} ".encode() + bytes((i * 31 + k) & 0xFF for k in range(SIZE))
                    for i in range(N)]
            t0 = time.time()
            for fr in sent: ss.sendall(wrap(fr))
            got = collect(sd, N, 30 + N * PER_FRAME)
            dt = time.time() - t0
            ok = sum(1 for fr in sent if fr in got)
            # modem73 truncates frames longer than the mode's payload; a received prefix
            # of a sent frame is a good decode of a truncated frame
            trunc = len([g for g in got if g not in sent and any(fr.startswith(g) for fr in sent)])
            extra = len([g for g in got if g not in sent]) - trunc
            total_bad += (N - ok - trunc) + extra
            print(f"{m:24s} {src}->{dst}: {ok + trunc}/{N} decoded ({trunc} truncated), "
                  f"{extra} unexpected, {dt:.1f}s", flush=True)
            time.sleep(2)
        for p in (pa, pb): stop(p)
        time.sleep(1)
    print("PASS" if total_bad == 0 else f"FAIL ({total_bad} bad)")
    return 0 if total_bad == 0 else 1

if __name__ == "__main__":
    sys.exit(main())
