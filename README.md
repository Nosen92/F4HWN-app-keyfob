# F4HWN-app-keyfob

An overlay app for the Quansheng UV-K1, running [F4HWN v6.0.0](https://github.com/armel/uv-k5-firmware-custom) firmware, that transmits a fixed-frequency FSK keyfob signal (Lock / Unlock / Trunk) using the radio's BK4819 chip.

## How it works

Each command is a 64-bit code followed by its 64-bit bitwise complement, Manchester-encoded (`0`→`01`, `1`→`10`, MSB-first) into 256 bits. Transmission keeps the BK4819's FSK TX FIFO continuously fed for the whole PTT hold, rather than sending a series of separate bursts, so the radio only emits one preamble/sync per hold instead of one per repeat.

## Setup

The codes in `keyfob_app.c` (`LOCK_WORDS`, `UNLOCK_WORDS`, `TRUNK_WORDS`) are placeholders. Replace them with your own keyfob's captured codes, Manchester-encoded the same way. You'll also want to set `TARGET_FREQ_X10HZ`, `DEV_GAIN`, and `PA_BIAS` to match your own keyfob's frequency, deviation, and output level.

## Building

Requires the `arm-none-eabi-gcc` toolchain and Python 3. From this directory:

```
bash build.sh
```

This produces `Keyfob.app`, ready to load onto the radio's overlay app storage.

Building from just this folder also needs `app_api.h` and `pack_app.py`, both one directory up in `App/apps/` of the [uv-k1-k5v3-firmware-custom](https://github.com/armel/uv-k5-firmware-custom) repo this app is built against.

## Usage

- **LEFT / RIGHT** — select Lock, Unlock, or Trunk
- **PTT** (hold) — transmit the selected command
- **EXIT** — quit

## License

Apache 2.0. See the header in `keyfob_app.c`.
