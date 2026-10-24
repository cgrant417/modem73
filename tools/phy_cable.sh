#!/bin/bash
# Real-hardware PHY comparison: play tests/phy_jitter's transmit audio out of one sound
# card, record it on another joined by an audio cable, and decode the recording with each
# tone layout. Pause PipeWire/PulseAudio first so they don't hold the cards.
#
# Usage: tools/phy_cable.sh SRC_CARD DST_CARD [frames]
# Env:   PROFILES (default "legacy n30 n60"), MODES (default below), SNR is not used
set -e
SRC=$1; DST=$2; N=${3:-6}
[ -n "$SRC" ] && [ -n "$DST" ] || { echo "usage: $0 SRC_CARD DST_CARD [frames]"; exit 2; }
PJ=${PJ:-tests/phy_jitter}
PROFILES=${PROFILES:-"legacy n30 n60"}
MODES=${MODES:-"BPSK 1/2 normal|QPSK 1/2 normal|QAM16 3/4 normal|QAM64 5/6 normal"}
W=$(mktemp -d /tmp/phycable.XXXX)
IFS='|' read -ra MODE_LIST <<< "$MODES"
for m in "${MODE_LIST[@]}"; do
  for p in $PROFILES; do
    $PJ --profile $p --mode "$m" --frames $N --tx-wav $W/tx.wav 2>/dev/null >/dev/null
    secs=$(python3 -c "import wave; w=wave.open('$W/tx.wav'); print(int(w.getnframes()/w.getframerate())+3)")
    arecord -q -D plughw:$DST,0 -f S16_LE -r 48000 -c 1 -d $secs $W/rx.wav &
    sleep 0.5
    aplay -q -D plughw:$SRC,0 $W/tx.wav
    wait
    $PJ --profile $p --mode "$m" --frames $N --rx-wav $W/rx.wav 2>/dev/null | sed -E "s/^/card $SRC->$DST /; s/snr +[0-9]+ dB  //" || true
  done
done
rm -rf $W
