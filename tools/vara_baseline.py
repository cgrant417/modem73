#!/usr/bin/env python3
"""Drive two VARA-protocol modems over TCP and measure a one-way transfer.

Station A connects to station B and sends random bytes on its data port; B checks them.
Every command-port message from both sides is logged with a timestamp, so the speed
level changes and retries can be read afterwards. Works against VARA FM/HF, or any
modem speaking the same host protocol (our --host vara mode later).

Usage: tools/vara_baseline.py [--a HOST:PORT] [--b HOST:PORT] [--bytes N] [--reverse]
  --a, --b    command ports; the data port is command port + 1
  --reverse   B sends to A instead
  --log FILE  also write the message log to FILE
"""
import argparse
import random
import select
import socket
import sys
import time


# VARA FM 4.4 never sent a 3-byte write; 12 bytes and up went through
GREETING = b'GREETING FROM THE CALLED STATION'


class Station:
    def __init__(self, name, addr, call):
        host, port = addr.rsplit(':', 1)
        self.name, self.call = name, call
        self.cmd = socket.create_connection((host, int(port)), timeout=10)
        self.data = socket.create_connection((host, int(port) + 1), timeout=10)
        self.cmd.setblocking(False)
        self.data.setblocking(False)
        self.buf = b''
        self.rx = bytearray()
        self.msgs = []

    def send(self, line):
        self.cmd.sendall(line.encode() + b'\r')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--a', default='127.0.0.1:8300')
    ap.add_argument('--b', default='127.0.0.1:8310')
    ap.add_argument('--call-a', default='N0AAA')
    ap.add_argument('--call-b', default='N0BBB')
    ap.add_argument('--bytes', type=int, default=4096)
    ap.add_argument('--reverse', action='store_true')
    ap.add_argument('--timeout', type=float, default=600)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--compression', help='OFF, TEXT or FILES (default: leave VARA\'s setting; '
                    'the payload is random, so compression cannot inflate the rate)')
    ap.add_argument('--settle', type=float, default=6, help='seconds to wait after CONNECTED')
    ap.add_argument('--chunk', type=int, default=256, help='bytes handed over every 0.5 s')
    ap.add_argument('--log')
    args = ap.parse_args()

    a = Station('A', args.a, args.call_a)
    b = Station('B', args.b, args.call_b)
    tx, rx = (b, a) if args.reverse else (a, b)
    logf = open(args.log, 'w') if args.log else None
    t0 = time.time()

    def log(s):
        line = '%7.2f %s' % (time.time() - t0, s)
        print(line, flush=True)
        if logf:
            logf.write(line + '\n')

    def pump(wait):
        """Read both stations for up to `wait` seconds; returns new (station, message) pairs."""
        new = []
        socks = [a.cmd, a.data, b.cmd, b.data]
        r, _, _ = select.select(socks, [], [], wait)
        for s in r:
            st = a if s in (a.cmd, a.data) else b
            chunk = s.recv(65536)
            if not chunk:
                raise SystemExit('%s closed its %s port' % (st.name, 'command' if s is st.cmd else 'data'))
            if s is st.data:
                st.rx += chunk
                continue
            st.buf += chunk
            while b'\r' in st.buf:
                m, st.buf = st.buf.split(b'\r', 1)
                m = m.decode(errors='replace').strip()
                if not m:
                    continue
                st.msgs.append((time.time() - t0, m))
                if m not in ('IAMALIVE', 'PTT ON', 'PTT OFF'):
                    log('%s< %s' % (st.name, m))
                new.append((st, m))
        return new

    def cmd(st, line):
        log('%s> %s' % (st.name, line))
        st.send(line)

    def wait_for(st, prefixes, limit):
        end = time.time() + limit
        while time.time() < end:
            for s, m in pump(0.2):
                if s is st and m.split(' ')[0] in prefixes:
                    return m
        return None

    for st in (a, b):
        cmd(st, 'MYCALL ' + st.call)
        if args.compression:
            cmd(st, 'COMPRESSION ' + args.compression)
        cmd(st, 'LISTEN ON')
    pump(1.0)

    payload = random.Random(args.seed).randbytes(args.bytes)  # incompressible
    cmd(tx, 'CONNECT %s %s' % (tx.call, rx.call))
    m = wait_for(tx, ('CONNECTED', 'DISCONNECTED'), 120)
    if not m or not m.startswith('CONNECTED'):
        log('RESULT connect failed (%s)' % m)
        return 1
    t_conn = time.time()
    # let the session finish setting up before handing over data
    end = t_conn + args.settle
    while time.time() < end:
        pump(0.2)
    # VARA FM ignores data from the calling side until the called side has sent something
    # (Winlink's gateway always speaks first), so the called side sends a greeting first.
    called, peer = rx, tx
    called.data.sendall(GREETING)
    log('%s sent %d-byte greeting' % (called.name, len(GREETING)))
    end = time.time() + args.timeout
    while len(peer.rx) < len(GREETING) and time.time() < end:
        pump(0.5)
    if len(peer.rx) < len(GREETING):
        log('RESULT greeting not received')
        return 1
    log('%s got greeting after %.1f s' % (peer.name, time.time() - t_conn))
    del peer.rx[:len(GREETING)]
    t_conn = time.time()
    log('sending %d bytes %s -> %s in %d-byte chunks' % (len(payload), tx.name, rx.name, args.chunk))

    t_first = None
    last_report = 0
    end = t_conn + args.timeout
    disconnected = False
    sent = 0
    while time.time() < end and len(rx.rx) < len(payload):
        if sent < len(payload):
            n = min(args.chunk, len(payload) - sent)
            tx.data.setblocking(True)
            tx.data.sendall(payload[sent:sent + n])
            tx.data.setblocking(False)
            sent += n
        for s, m in pump(0.5):
            if m.startswith('DISCONNECTED'):
                disconnected = True
        if rx.rx and t_first is None:
            t_first = time.time()
        if len(rx.rx) - last_report >= 1024 or (disconnected and len(rx.rx) != last_report):
            last_report = len(rx.rx)
            log('%s has %d/%d bytes' % (rx.name, len(rx.rx), len(payload)))
        if disconnected:
            break
    t_done = time.time()

    ok = bytes(rx.rx[:len(payload)]) == payload[:len(rx.rx)]
    secs = t_done - t_conn
    log('RESULT %s->%s %d/%d bytes in %.1f s after connect = %.0f bps net, data %s%s' % (
        tx.name, rx.name, len(rx.rx), len(payload), secs, len(rx.rx) * 8 / max(secs, 1e-3),
        'intact' if ok else 'CORRUPT', ', link dropped' if disconnected else ''))

    if not disconnected:
        cmd(tx, 'DISCONNECT')
        wait_for(tx, ('DISCONNECTED',), 60)
    pump(1.0)
    return 0 if ok and len(rx.rx) == len(payload) else 1


if __name__ == '__main__':
    sys.exit(main())
