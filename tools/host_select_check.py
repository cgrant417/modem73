#!/usr/bin/env python3
"""Check --host kiss|vara startup selection.

Runs modem73 headless (PTT none, audio on M73_CARD via a throwaway HOME) and checks:
  - kiss (default): KISS port open, VARA ports closed
  - vara: VARA command + data ports open, KISS port closed; one host per port;
    commands and data are logged; get_status reports host_interface and client_count
  - host_interface=1 in the settings file selects VARA; --host kiss overrides it
  - invalid --host value and a busy VARA data port are startup errors
Usage: tools/host_select_check.py <modem73 binary>
"""
import json, os, shutil, socket, struct, subprocess, sys, time

BIN = sys.argv[1]
CARD = os.environ.get("M73_CARD", "plughw:0,0")
WORK = os.environ.get("M73_WORK", "/tmp/m73host")
KISS, VARA, CTRL = 18001, 18300, 18073
ok = True

def check(cond, what):
    global ok
    ok &= bool(cond)
    print(("PASS " if cond else "FAIL ") + what, flush=True)

def home(name, settings=None):
    h = os.path.join(WORK, name)
    os.makedirs(os.path.join(h, ".config", "modem73fm"))
    with open(os.path.join(h, ".asoundrc"), "w") as f:
        f.write(f'pcm.!default {{ type asym playback.pcm "{CARD}" capture.pcm "{CARD}" }}\n')
    if settings:
        with open(os.path.join(h, ".config", "modem73fm", "settings"), "w") as f:
            f.write(settings)
    return h

def start(h, *args):
    env = dict(os.environ, HOME=h); env.pop("PULSE_SERVER", None)
    log = open(os.path.join(h, "modem73.log"), "w")
    p = subprocess.Popen([BIN, "--headless", "--ptt", "none", "--port", str(KISS),
                          "--vara-port", str(VARA), "--control-port", str(CTRL), *args],
                         stdout=log, stderr=subprocess.STDOUT, env=env, cwd=h)
    return p, os.path.join(h, "modem73.log")

def wait_listening(port, secs=10):
    end = time.time() + secs
    while time.time() < end:
        if is_listening(port): return True
        time.sleep(0.2)
    return False

def is_listening(port):
    try:
        socket.create_connection(("127.0.0.1", port), timeout=0.5).close()
        return True
    except OSError:
        return False

def stop(p):
    p.send_signal(2)
    try: return p.wait(timeout=15)
    except subprocess.TimeoutExpired:
        p.kill(); return p.wait()

def status():
    s = socket.create_connection(("127.0.0.1", CTRL), timeout=5)
    b = json.dumps({"cmd": "get_status"}).encode()
    s.sendall(struct.pack(">I", len(b)) + b)
    buf = b""
    while True:
        buf += s.recv(65536)
        while len(buf) >= 4:
            n = struct.unpack(">I", buf[:4])[0]
            if len(buf) < 4 + n: break
            m = json.loads(buf[4:4 + n]); buf = buf[4 + n:]
            if "client_count" in m:
                s.close(); return m

def read(path):
    with open(path, errors="replace") as f: return f.read()

shutil.rmtree(WORK, ignore_errors=True)

print("== default (kiss)")
p, log = start(home("kiss"))
check(wait_listening(KISS), "KISS port listening")
check(not is_listening(VARA) and not is_listening(VARA + 1), "VARA ports not listening")
check(status()["host_interface"] == "kiss", "get_status host_interface = kiss")
stop(p)

print("== --host vara")
p, log = start(home("vara"), "--host", "vara")
check(wait_listening(VARA) and is_listening(VARA + 1), "VARA command and data ports listening")
check(not is_listening(KISS), "KISS port not listening")
time.sleep(0.5)
cmd = socket.create_connection(("127.0.0.1", VARA), timeout=5)
data = socket.create_connection(("127.0.0.1", VARA + 1), timeout=5)
cmd.sendall(b"MYCALL N0CALL\rLISTEN ON\r")
data.sendall(b"hello vara data")
time.sleep(0.5)
st = status()
check(st["host_interface"] == "vara" and st["client_count"] == 1, "get_status: vara, client_count 1")
extra = socket.create_connection(("127.0.0.1", VARA), timeout=5)
extra.settimeout(3)
try:
    closed = extra.recv(1) == b""
except OSError:
    closed = True
check(closed, "second host on the command port is rejected")
cmd.close(); data.close()
time.sleep(0.5)
check(status()["client_count"] == 0, "client_count back to 0 after host disconnects")
stop(p)
text = read(log)
for needle in ["VARA host interface listening on 127.0.0.1:%d (command) and 127.0.0.1:%d (data)" % (VARA, VARA + 1),
               "host command 'MYCALL N0CALL'", "host command 'LISTEN ON'",
               "15 data bytes from host", "command port already has a host",
               "host disconnected from command port", "host disconnected from data port"]:
    check(needle in text, f"log has: {needle}")
check("KISS TNC listening" not in text, "log has no KISS listener line")

print("== settings file host_interface=1")
p, log = start(home("settings_vara", "host_interface=1\n"))
check(wait_listening(VARA) and not is_listening(KISS), "settings file selects VARA")
stop(p)
p, log = start(home("settings_override", "host_interface=1\n"), "--host", "kiss")
check(wait_listening(KISS) and not is_listening(VARA), "--host kiss overrides settings file")
stop(p)

print("== startup errors")
p, log = start(home("bad"), "--host", "ax25")
check(p.wait(timeout=10) == 1 and "Invalid --host 'ax25'" in read(log), "invalid --host rejected")
blocker = socket.socket(); blocker.bind(("127.0.0.1", VARA + 1)); blocker.listen(1)
p, log = start(home("busy"), "--host", "vara")
rc = p.wait(timeout=10)
check(rc == 1 and f"VARA port {VARA + 1} is already in use" in read(log), "busy VARA data port is a startup error")
blocker.close()

print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
