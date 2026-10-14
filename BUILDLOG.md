# FM-1 → SP-12 Turbo build log

The goal: a complete custom OS that turns the M-VAVE (Cuvave) FM-1 into an SP-12 Turbo-style 12-bit sampling drum
machine, including **sampling over USB on the device itself**.

This repo is a fork of [zp12](https://github.com/zvenson/zp12) (GPL-3.0; built on SLOOP and Felucca). zp12's own
history is kept, so its credits and licence carry over. The upstream README is unchanged in `README.md`.

## Decisions

| Date | Decision | Why |
|---|---|---|
| 2026-10-14 | Fork zp12 instead of writing from scratch | It already runs on the hardware: a 12-bit 26.04 / 27.5 kHz engine, 8 channels, segments, songs, swing, a safe update path and USB rescue. |
| 2026-10-14 | Live USB sampling is a core requirement | The FM-1 has no audio input. The stock firmware already has an isochronous USB audio OUT endpoint, so the hardware can receive audio. |
| 2026-10-14 | Takes are first recorded to RAM, then written to flash in a separate step | A flash write turns off XIP and interrupts for milliseconds, which would drop USB packets mid-take. |
| 2026-10-14 | Package identity `FM-1_96N` (zp12 upstream is `97N`) | The test builds never pass for a zp12 release. |

## Hardware facts this build relies on

- JieLi AC791N, 1 MiB SPI NOR (Puya, JEDEC 0x856014), code runs XIP. App image limit 581,564 B; RAM `.data+.bss` 96 KiB.
- Updates go over USB-MIDI SysEx (device-pull). The Felucca update loader is kept, so the web installer and M-VAVE's
  updater always reach the device. **USB rescue:** hold OCT− while switching on.
- The USB controller is MUSB-style and polled at 2 kHz from TIMER5. EP1 carries MIDI. EP2 OUT isochronous carries
  the sampler (as on stock firmware).

## Phase 1: USB sampling, RAM only (build `0.1-usb`, identity FM-1_961)

What changed (`firmware/src/`):
- `usb.c`, `ZP12_UAI=1`: a class-compliant USB audio output (UAC1, 44.1 kHz 16-bit stereo, EP2 OUT isochronous
  adaptive, max 184 B). Packets are read in `uai_service()` from `usb_poll`. No driver is needed on macOS, Windows,
  Linux, iOS or Android.
- `sp_smp.c` (new): sums the input to mono, applies LEVEL (−12…+24 dB) and meters it. MON plays it on the output.
  The recording path is a 12 kHz 2-pole input filter, then resampling to **27.5 kHz** (the SP-12's rate), then
  12-bit packing into a 72 KiB RAM buffer (1.79 s). THRESH arms the take and MAX limits its length.
- `sp_ui.c`: the **SAMPLE** page. Tap **LFO** to open it (LFO held still means ERASE).

How to use it:
1. Connect the FM-1 by USB. On the computer or phone, pick **"Felucca"** as the audio output and play something.
2. Tap **LFO**. The LCD shows `SAMPLE A1` with a level meter. `NO USB AUDIO` means no packets are arriving.
3. Press a pad key to choose the target pad.
4. KNOB 1 LEVEL, KNOB 2 THRESH (OFF = start at once), KNOB 3 MON (hear the input), KNOB 4 MAX (seconds).
5. **REC** arms the take (REC blinks); the threshold starts it (REC lit); REC again stops it. MAX also stops it.
6. The take lands on the pad as `TAKE` and plays once. Tune, truncate, decay and filter it like any sound.

Limits of this build:
- The take is only in RAM. It is lost at power-off, and a new take replaces it. Saving to flash is Phase 2.
- One take at a time, 1.79 s at most.

Host checks (all pass):
- `tests/sp_smp_test.c`: 24 checks on pitch, level, threshold, MAX, LEVEL, clipping, the filter, the monitor and
  page closing. With an argument it writes a WAV of a sweep.
- `tests/usb_desc_check.py`: walks the configuration descriptor in the built image, as a host would.
- `tests/zp12_ui_test.c`: adds SAMPLE page screens. The upstream suites (core, seq, punch, link) still pass.

## Test on the device (what I need back)

Install `release/0.1-usb/`. Either way needs the FM-1 on a USB data cable.
- **Web:** run `python3 -m http.server 8000` inside `release/0.1-usb/`, open `http://localhost:8000` in Chrome or
  Edge, then press Install.
- **CLI:** `pip install mido python-rtmidi`, then
  `python3 tools/fm1_install.py release/0.1-usb/firmware/zp12-0.1-usb.fwsc`.

To go back, use the zp12 installer (zp12.designburgapps.com/install) or M-VAVE's updater. Your zp12 loops and
sounds stay, because this build reads the same store.

Report back:
1. Does the FM-1 boot, play and save as before?
2. Does the computer or phone list a "Felucca" audio output? Which OS?
3. With audio playing into it, does the SAMPLE page show the meter? Does MON play clean or crackle?
4. Does a take sound right (pitch, no clicks or dropouts)? Try a 1.5 s loop.
5. Anything odd: a crash screen (photo please), USB-MIDI or the installer no longer finding it.

## Roadmap

- **Phase 2:** save takes to flash in the own-sample slots (same directory format as the web editor). Write after
  the take, with audio paused for about a second, as on the real machines. Extend take length by borrowing the FX
  pool while sampling (about 8 s).
- **Phase 3, SP-12 Turbo fidelity:** a 27.5 kHz-centred kit, the SP-12's per-channel output layout, its tuning and
  decay ranges and the segment/song workflow, compared against the original's manual.
- **Phase 4:** its own name, screens and installer page, then a release.

## Building here

```
tools/get_toolchain.sh                         # JieLi clang for pi32v2 -> ~/.jieli/toolchain
# AC79 SDK: only cpu/wl82/tools/{uboot.boot,cfg_tool.bin,cfg/eq_cfg_hw.bin} are needed (sparse clone of
# gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK at AC79NN_SDK_V1.2.1_2023-12-13 into ~/fw-AC79_AIoT_SDK)
sh build.sh --release 0.1-usb                  # -> build/zp12-0.1-usb.fwsc
cc -O2 -Wall -Wno-unused-function -o build/host/sp_smp_test tests/sp_smp_test.c -lm && build/host/sp_smp_test
python3 tests/usb_desc_check.py build/zp12.bin
```
