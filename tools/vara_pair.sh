#!/bin/bash
# Start two modem73 instances in VARA host mode, joined by audio:
#   station A: VARA ports 8400/8401, station B: 8410/8411.
# Default audio is the ALSA loopback card (sudo modprobe snd-aloop): A plays into B's
# capture and B into A's. Set CARD_A / CARD_B (e.g. plughw:2,0) to use real interfaces,
# and PTT_A / PTT_B to key radios (e.g. "--ptt com --com-port /dev/ttyUSB0"; default none).
# CALL_A / CALL_B set each modem's PHY callsign (default N0AAA / N0BBB).
# Then drive them with tools/vara_baseline.py --a 127.0.0.1:8400 --b 127.0.0.1:8410.
#
# Usage: tools/vara_pair.sh start|stop [extra modem73 options]
BIN=${BIN:-$(dirname "$0")/../modem73fm}
WORK=${WORK:-/tmp/m73vara}
cmd=$1; shift

asoundrc() {  # playback capture
  printf 'pcm.!default { type asym playback.pcm "%s" capture.pcm "%s" }\n' "$1" "$2"
}

case $cmd in
start)
  mkdir -p $WORK/A $WORK/B
  if [ -n "$CARD_A" ]; then
    asoundrc "$CARD_A" "$CARD_A" > $WORK/A/.asoundrc
    asoundrc "$CARD_B" "$CARD_B" > $WORK/B/.asoundrc
  else
    asoundrc plughw:Loopback,0,0 plughw:Loopback,0,1 > $WORK/A/.asoundrc
    asoundrc plughw:Loopback,1,1 plughw:Loopback,1,0 > $WORK/B/.asoundrc
  fi
  for s in A:8400:${CALL_A:-N0AAA} B:8410:${CALL_B:-N0BBB}; do
    IFS=: read name port call <<< "$s"
    ptt=PTT_$name; ptt=${!ptt:---ptt none}
    HOME=$WORK/$name setsid "$BIN" --headless --host vara --vara-port $port --control-port 0 \
      $ptt -c $call "$@" > $WORK/$name.log 2>&1 < /dev/null &
    echo $! > $WORK/$name.pid
  done
  sleep 3
  for name in A B; do
    if kill -0 $(cat $WORK/$name.pid) 2>/dev/null; then echo "$name running (log $WORK/$name.log)"
    else echo "$name FAILED:"; tail -5 $WORK/$name.log; fi
  done
  ;;
stop)
  for name in A B; do
    [ -f $WORK/$name.pid ] || continue
    pid=$(cat $WORK/$name.pid)
    if kill $pid 2>/dev/null; then
      # wait for it to release its ports and sound card
      for i in $(seq 50); do kill -0 $pid 2>/dev/null || break; sleep 0.1; done
      echo "$name stopped"
    fi
    rm -f $WORK/$name.pid
  done
  ;;
*) echo "usage: $0 start|stop [modem73 options]"; exit 2 ;;
esac
