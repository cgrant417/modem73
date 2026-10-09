#!/usr/bin/env python3
"""Check modem73 tx_event / tx_pending_bytes / tx_frame_count over the control port.

Sends 3 KISS frames at once (default CSMA burst 2 -> bursts of 2 + 1 frames), records
control-port events and get_status, then checks event order and byte accounting.
PTT none; audio on the default card via a throwaway HOME. Usage: tx_event_check.py <modem73>
"""
import json, os, shutil, socket, struct, subprocess, sys, threading, time

BIN = sys.argv[1]
KPORT, CPORT, CARD = 18001, 18073, "plughw:0,0"
HOME = "/tmp/m73evt"
FEND, FESC, TFEND, TFESC = 0xC0, 0xDB, 0xDC, 0xDD
SIZES = [100, 200, 300]

def wrap(d):
    o = bytearray([FEND, 0])
    for b in d:
        o += bytes([FESC, TFEND]) if b == FEND else bytes([FESC, TFESC]) if b == FESC else bytes([b])
    return bytes(o + bytes([FEND]))

def connect(port):
    for _ in range(50):
        try: return socket.create_connection(("127.0.0.1", port), timeout=5)
        except OSError: time.sleep(0.2)
    raise RuntimeError(f"no listener on {port}")

def ctrl_send(s, obj):
    b = json.dumps(obj).encode()
    s.sendall(struct.pack(">I", len(b)) + b)

shutil.rmtree(HOME, ignore_errors=True); os.makedirs(HOME)
with open(os.path.join(HOME, ".asoundrc"), "w") as f:
    f.write(f'pcm.!default {{ type asym playback.pcm "{CARD}" capture.pcm "{CARD}" }}\n')
env = dict(os.environ, HOME=HOME); env.pop("PULSE_SERVER", None)
log = open(os.path.join(HOME, "modem73.log"), "w")
p = subprocess.Popen([BIN, "--headless", "--port", str(KPORT), "--control-port", str(CPORT),
                      "--ptt", "none"], stdout=log, stderr=subprocess.STDOUT, env=env, cwd=HOME)

msgs, statuses = [], []
ctrl = connect(CPORT)
def reader():
    buf = b""
    while True:
        try: d = ctrl.recv(65536)
        except OSError: return
        if not d: return
        buf += d
        while len(buf) >= 4:
            n = struct.unpack(">I", buf[:4])[0]
            if len(buf) < 4 + n: break
            m = json.loads(buf[4:4 + n]); buf = buf[4 + n:]
            m["_t"] = time.time()
            (statuses if "tx_frame_count" in m else msgs).append(m)
threading.Thread(target=reader, daemon=True).start()

kiss = connect(KPORT)
time.sleep(1)
ctrl_send(ctrl, {"cmd": "get_status"})
time.sleep(0.5)
kiss.sendall(b"".join(wrap(bytes([i & 0xFF]) * n) for i, n in enumerate(SIZES)))
time.sleep(0.3)
ctrl_send(ctrl, {"cmd": "get_status"})          # mid-flight snapshot
deadline = time.time() + 90
while time.time() < deadline and sum(m.get("type") == "frame_done" for m in msgs) < len(SIZES):
    time.sleep(0.5)
time.sleep(2)
ctrl_send(ctrl, {"cmd": "get_status"})
time.sleep(1)
p.send_signal(2); p.wait(timeout=15)

ev = [m for m in msgs if m.get("event") == "tx_event"]
print("events:")
t0 = ev[0]["_t"] if ev else 0
for m in ev:
    extra = f" bytes={m['bytes']} ok={m['ok']}" if m["type"] == "frame_done" else ""
    print(f"  +{m['_t'] - t0:6.2f}s {m['type']:10s}{extra} beacon={m['beacon']} pending={m['tx_pending_bytes']}")
for i, s in enumerate(statuses):
    print(f"status {i}: tx_frame_count={s['tx_frame_count']} tx_queue={s['tx_queue']} "
          f"tx_pending_bytes={s['tx_pending_bytes']} ptt_on={s['ptt_on']}")

ok = True
def check(cond, what):
    global ok
    ok &= bool(cond)
    print(("PASS " if cond else "FAIL ") + what)

data = [m for m in ev if not m["beacon"]]
types = [m["type"] for m in data]
check(types and types[0] == "key_up" and types[-1] == "key_down", "starts with key_up, ends with key_down")
depth, nested_ok = 0, True
for t in types:
    if t == "key_up": nested_ok &= depth == 0; depth = 1
    elif t == "key_down": nested_ok &= depth == 1; depth = 0
    else: nested_ok &= depth == 1
check(nested_ok, "every frame_done is inside a key_up/key_down pair, no nesting")
fd = [m for m in data if m["type"] == "frame_done"]
check([m["bytes"] for m in fd] == SIZES and all(m["ok"] for m in fd), f"frame_done bytes {SIZES}, all ok")
check([m["tx_pending_bytes"] for m in fd] == [sum(SIZES[i + 1:]) for i in range(len(SIZES))],
      "tx_pending_bytes after each frame_done = bytes still unsent")
check(types.count("key_up") == 2, "CSMA burst 2: two key-ups for three frames")
check(len(statuses) == 3 and statuses[0]["tx_frame_count"] == 0 and statuses[0]["tx_pending_bytes"] == 0,
      "status before: 0 frames, 0 pending")
check(len(statuses) == 3 and statuses[1]["tx_pending_bytes"] == sum(SIZES), "status mid-flight: all bytes pending")
check(len(statuses) == 3 and statuses[2]["tx_frame_count"] == 3 and statuses[2]["tx_pending_bytes"] == 0,
      "status after: 3 frames, 0 pending")
print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
