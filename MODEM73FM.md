# modem73fm

modem73fm is a fork of [modem73](https://github.com/RFnexus/modem73) that adds a **VARA-compatible host interface** with its own ARQ link layer on top of modem73's OFDM modem. Winlink software that already drives VARA, such as Pat or LinBPQ, can use it unchanged. It is aimed at FM radios through their 9600 data jacks.

Everything else in modem73 is still here. The KISS interface is still available with `--host kiss`, and the upstream [README](README.md) still describes the modem, its modes, CSMA and PTT options.

> **Not on-air compatible with VARA.** modem73fm speaks VARA's *host* protocol, the TCP command and data ports, not VARA's radio signal. Both ends of a link must run modem73fm. A station running real VARA FM cannot connect to it.

## What is different from modem73

| | modem73 | modem73fm |
|---|---|---|
| Binary | `modem73` | `modem73fm` |
| Default host interface | KISS (TCP 8001) | VARA (TCP 8300 command, 8301 data) |
| Link layer | none: KISS frames | ARQ: connect, selective repeat, adaptive speed ladder |
| Settings | `~/.config/modem73/` | `~/.config/modem73fm/` |
| `make install` | `/usr/local/bin/modem73` | `/usr/local/bin/modem73fm` |

Both can be installed side by side.

## Install

On a Debian, Ubuntu or Arch system, the installer fetches the build dependencies, builds, and offers to install:

```
sudo apt install git          # or: sudo pacman -S git
git clone https://github.com/cgrant417/modem73fm modem73fm
cd modem73fm
./install.sh
```

It asks whether to add Hamlib (rig-control PTT) and hidapi (CM108 USB PTT), builds `modem73fm`, and offers to install it to `/usr/local/bin`. `modem73fm` is the repository's default branch, so a plain clone gets it.

Or build by hand:

```
sudo apt install git build-essential libncurses-dev pkg-config   # plus libhamlib-dev, libhidapi-dev if wanted
git clone https://github.com/cgrant417/modem73fm modem73fm
cd modem73fm
make
sudo make install        # optional; sudo make uninstall removes it again
```

`make install` copies `modem73fm` to `/usr/local/bin`, and the CM108 and GPIO PTT udev rules to `/etc/udev/rules.d`, as upstream does. `PREFIX=` changes the binary's location (default `/usr/local`). `make uninstall` removes the binary but leaves the udev rules, which upstream modem73 installs too.

To update later, run `./update.sh`. It updates `~/modem73fm`, or the checkout named by `MODEM73FM_DIR`, and rebuilds. It refuses to touch a checkout whose `origin` is not this fork.

`modem73fm --version` prints the fork's version and the modem73 version it is based on, such as `modem73fm 0.1 (modem73 2.4.2)`.

## Setting up a station

Once per machine:

1. **Serial PTT access.** For COM PTT, such as a Digirig, add yourself to the `dialout` group, then log out and back in:
   ```
   sudo usermod -aG dialout $USER
   ```
   CM108 and GPIO PTT are covered by the udev rules that `make install` adds.
2. **Stop things that grab serial ports.** On a machine with no cellular modem, run `sudo systemctl disable --now ModemManager`, and remove brltty if it is installed (`sudo apt purge brltty`). See [Serial PTT safety](#serial-ptt-safety).
3. **Optionally, a fixed name for the PTT port.** A udev rule matching the interface's serial number, such as `SYMLINK+="digirig"`, keeps the name the same however the ports are numbered.

Then set modem73fm up in its screen UI:

1. Run `modem73fm`, without `--headless`.
2. On the CONFIG tab, set the callsign, audio input and output (picked from a list of what is present), and the PTT type and port. Settings are saved to `~/.config/modem73fm/settings` as they change.
3. Make a test connection and check the S/N that each side reports (see [Levels](#levels)).
4. From then on, `modem73fm --headless` starts it with the saved settings. Options given on the command line override the file for that run. If the screen UI saves while they are in effect, they are stored too.

The settings file is plain `key=value` lines and can be edited by hand. Several values are list positions rather than names, such as `modem_type=0` for OFDM and `host_interface=1` for VARA, so letting the screen UI write it first is easiest. `--config FILE` uses a different file, for example one per radio.

## Running in VARA mode

VARA mode is the default, and it needs the OFDM modem, which is also the default:

```
modem73fm --headless -c YOURCALL \
    --ptt com --com-port /dev/ttyUSB0 --com-line rts \
    --input-device "USB Audio Device Mono" --output-device "USB Audio Device Analog Stereo"
```

- **Ports:** the VARA command port is 8300 and the data port is 8301, on 127.0.0.1. Use `--vara-port` and `--vara-bind` to change them. 8300 is also real VARA's port, so on a machine that runs VARA as well, pick another, for example `--vara-port 8400`.
- **PTT:** modem73fm keys the radio itself, with any of modem73's PTT types (`com`, `cm108`, `rigctl`, `vox`, and others). Turn PTT control **off** in the host program (Pat, LinBPQ).
- **Callsign:** `-c` sets the callsign sent in every OFDM frame. The host's `MYCALL` sets the ARQ session's callsign.
- **Bandwidth:** narrow (2.4 kHz) by default. With `--vara-bw wide`, a station offers the wide waveform (300–6300 Hz) when it calls and accepts it when called. A session is wide only if both stations offer it; otherwise it falls back to narrow. Use wide only when both radios are on their 9600 data ports, since mic and speaker audio cuts off above about 3 kHz. A host's `BW WIDE` or `BW NARROW` changes the setting for the next connection. `--no-wide` leaves the wide decoder out, to save CPU.
- **One host at a time** per port, as with VARA.

### Audio devices

`modem73fm --list-audio` lists the device names it can use. If PipeWire or PulseAudio is running, as on most desktop installs, modem73fm goes through it and **ignores `~/.asoundrc`**. Choose the interface by name with `--input-device` and `--output-device`, or `-d` for both. Otherwise it plays to and records from the system default, which may be the wrong card.

### Levels

Set the levels with the radio's data jack at 9600:

- Keep the receive peak well below full scale: about −5 dBFS on the loudest signal.
- A clipped input reads as poor S/N, and the link stays at slow speeds.
- The S/N each side measures is reported to the host (`SN`), so it is a quick check after a test connection.

## Pat

Pat's `varafm` transport points at modem73fm's command port. The data port is the next one up.

```json
"varafm": { "addr": "localhost:8300", "rig": "", "ptt_ctrl": false }
```

- Listen for peer-to-peer connections: `pat --listen varafm http`, or the same with `interactive`.
- Connect to another station: `pat connect varafm:///THEIRCALL`.

Pat checks over the internet that the *calling* station's Winlink account is active. If it is not, Pat stops at an interactive over-the-air activation prompt before connecting. Call with a callsign that has an account. The listening side is not checked.

## LinBPQ

Add a VARA port pointing at modem73fm, with no PTT settings, since modem73fm keys the radio:

```
PORT
   PORTNUM=4
   ID=modem73fm
   DRIVER=VARA
   CONFIG
   ADDR 127.0.0.1 8300
ENDPORT
```

- To call out from the node: `ATTACH 4`, then `C THEIRCALL`. `C 4 THEIRCALL` is refused, because a VARA port is not an AX.25 port.
- A Winlink client (Pat, Winlink Express) connecting to the BBS needs its user record flagged **RMS Express User**. Otherwise the BBS treats it as a terminal user and does not offer B2 forwarding.

A gateway connected to Winlink's servers (an RMS gateway) works the same way, but only stations running modem73fm can reach it. Winlink's gateway listings know VARA FM, not modem73fm, so do not list it as a VARA gateway.

## Serial PTT safety

modem73fm opens the PTT port at startup and clears RTS and DTR at once. Other things on Linux can key a radio by accident:

- **Any `open()` of a serial port raises RTS and DTR**, even a read-only check, and even while modem73fm holds the port. On a Digirig, RTS is PTT. Don't probe a PTT port to "see if it is keyed".
- **HUPCL:** with `hupcl` set, the kernel drops RTS when the last program closes the port, so a crashed modem unkeys the radio. Check with `stty -F /dev/ttyUSB0 -a`; set it with `stty -F /dev/ttyUSB0 hupcl`. VARA under Wine clears it, and it resets when the interface is replugged.
- **ModemManager** probes new serial ports looking for cellular modems, and **brltty** can claim USB serial adapters. On a station with no cellular modem, run `sudo systemctl disable --now ModemManager`. Remove brltty if it is installed.

## Speeds measured

All runs carried 64 KB of random data with the VARA host protocol. Each row compares modem73fm and VARA FM at the same bandwidth.

**Narrow:**

| Path | modem73fm narrow | VARA FM narrow |
|---|---|---|
| Digirig-to-Digirig audio cable | 7.0 to 9.5 kbps net | 9.5 kbps |
| IC-7000 to FT-857D over the air (S/N about 27 dB) | 6.0 kbps | 4.8 kbps |
| FT-857D to IC-7000 over the air (S/N about 21 dB) | 5.5 kbps | 4.8 kbps |

**Wide** (both stations `--vara-bw wide`):

| Path | modem73fm wide | VARA FM wide |
|---|---|---|
| Digirig-to-Digirig audio cable | 14.2 to 16.2 kbps net | 16.0 kbps |
| IC-7000 and FT-857D over the air | TBD | 4.2 to 5.1 kbps (32–128 KB) |

The narrow speed ladder runs from BPSK 1/2 to QAM1024 5/6 (about 0.7 to 13 kbps per frame), and the wide one from BPSK 1/2 to QAM1024 5/6 (about 1.8 to 32 kbps per frame). Their switch points were measured on a cable with added noise. They have not yet been checked over a wider range of radio paths.

## VARA commands supported

- **Used:** `MYCALL`, `LISTEN ON|OFF`, `CONNECT <mycall> <target>`, `DISCONNECT`, `ABORT`, `DIRTYDISCONNECT`, `VERSION`.
- **Bandwidth:** `BW WIDE` or `BWWIDE` offers wide, `BW NARROW` or `BWNARROW` narrow only. VARA HF's widths (`BW500`, `BW2300`, `BW2750`) leave the setting as it is.
- **Accepted and ignored:** `LISTEN CQ`, `COMPRESSION`, `CWID`, `CHAT`, `PUBLIC`, `ENCRYPTION`, `WINLINK`, `P2P`, `CLEANTXBUFFER`. A digipeater path after `CONNECT` is ignored.
- **Anything else:** gets `WRONG`, and the command is logged.
- **Replies:** `OK`, `WRONG`, `PENDING`, `CANCELPENDING`, `CONNECTED <caller> <called> NARROW`, `DISCONNECTED`, `BUFFER n`, `PTT ON|OFF`, `SN x`, `BITRATE (level) n bps`, `BUSY ON|OFF`, and `IAMALIVE` every 60 s.

Run with `-v` to log every command and reply.

## Not done yet

- **Wide mode over the air:** wide is calibrated and tested on a cable, but not yet over the radios.
- **Timing:** each turnaround costs about 5 s. That dominates short Winlink exchanges, where the message protocol (B2F) makes many small turns.
- **More radios:** the speed ladder's switch points have only been checked on the bench radios.

## Development tools

- `make arq_test`: ARQ engine simulator, 281 checks, including stress runs over 1000 narrow and 500 wide random seeds.
- `make vara_test`: two VARA hosts over a simulated channel, 113 checks.
- `tools/vara_pair.sh start|stop`: two VARA-mode instances, on the ALSA loopback card or on real interfaces (`CARD_A`/`CARD_B`), optionally keying radios (`PTT_A`/`PTT_B`).
- `tools/vara_baseline.py`: drives two VARA (or modem73fm) instances the way Pat does, and reports throughput.
- `tools/host_select_check.py`: checks the `--host` startup choice.
