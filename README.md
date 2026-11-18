# BluemchenFX

[![Build firmware](https://github.com/jeffreykrueger/BluemchenFX/actions/workflows/build.yml/badge.svg)](https://github.com/jeffreykrueger/BluemchenFX/actions/workflows/build.yml)

Clean-slate effect firmware for the **kxmx Bluemchen** (Daisy Seed, 4HP), built on current
[libDaisy](https://github.com/electro-smith/libDaisy) and [DaisySP](https://github.com/electro-smith/DaisySP).
Two effects, **reverb** and **delay**, switchable from the OLED + encoder UI, with knob/CV
assignment, MIDI control and text-file presets on the SD card.

Hardware pin map comes from [recursinging/kxmx_bluemchen](https://github.com/recursinging/kxmx_bluemchen),
rewritten against today's libDaisy API (named `seed::Dxx` pins, current `Encoder`/`OledDisplay` APIs).

## Quick start

**Flash the prebuilt binary** (`prebuilt/BluemchenFX.bin`, or the `.bin` attached to each [release](https://github.com/jeffreykrueger/BluemchenFX/releases)): put the Daisy in DFU mode (hold BOOT, tap RESET), then either
drag `prebuilt/BluemchenFX.bin` into the [Daisy Web Programmer](https://electro-smith.github.io/Programmer/),
or run:

```
dfu-util -a 0 -s 0x08000000:leave -D prebuilt/BluemchenFX.bin -d ,0483:df11
```

**Build from source** (needs the [Daisy Toolchain](https://github.com/electro-smith/DaisyWiki/wiki/1.-Setting-Up-Your-Development-Environment); on Windows use Git Bash):

```
git clone --recursive https://github.com/jeffreykrueger/BluemchenFX.git
cd BluemchenFX
./setup.sh          # once: fetches libDaisy + DaisySP (pinned submodules) and builds them
make                # builds build/BluemchenFX.bin
make program-dfu    # flashes over USB (DFU mode)
```

## Controls

| Control | Function |
|---|---|
| Encoder turn | Move between pages; change the value while editing |
| Encoder click | Enter/leave edit mode (header inverts while editing). On HOME it jumps to the first parameter; on SAVE it stores settings |
| EFFECT page | Click, turn to choose **REVERB** or **DELAY**, click to switch. Audio passes dry for a moment while the new effect's buffers are cleared, so there's no burst of old echoes |
| Encoder hold (0.7 s) | Store all settings to flash ("SAVED") |
| Knob 1 / Knob 2 | Absolute control of the routed parameter (defaults: reverb **Decay** / **Mix**, delay **Time** / **Mix**). A knob only takes over once you move it, so stored values aren't overwritten at power-up |
| CV 1 / CV 2 | Bipolar offset on the routed parameter, ±5 V = ±full range (defaults: reverb **Decay** / **Hi Cut**, delay **Time** / **Feedback**) |
| MIDI CC 20+ | Parameters of the active effect in page order, CC 20 = first (reverb: Decay, Mix, Lo Cut, Hi Cut; delay: Time, Feedback, Mix, Ping, Lo Cut, Hi Cut). Any channel |
| MIDI Program Change 0–98 | Load SD preset 1–99 (switches effect if the preset is for the other one) |
| INPUT page | **STEREO** (default) or **MONO**. In mono, the left input is copied to the right channel before any effect processing, so a single cable into the left jack gives full stereo output. "MONO" also shows in the HOME header |

Each effect remembers its own parameter values **and** its own knob/CV assignments, so switching
back and forth doesn't lose anything.

Moving a knob or sending a CC briefly pops up that parameter's page.

## Display pages (64×32 OLED)

1. **HOME** – active effect's name and a bar per parameter (live values including CV)
2. **EFFECT** – REVERB / DELAY
3. **Parameter pages** for the active effect (see below)
4. **KNOB 1 / KNOB 2 / CV 1 / CV 2** – choose which parameter each source controls (or `--`)
5. **INPUT** – STEREO / MONO (click, turn to change, click to confirm; saved with the other settings)
6. **LOAD SD** – click, turn to pick a slot (01–99), click again to load it
7. **SAVE SD** – click, turn to pick a slot, click again to write the current setup to it
8. **SAVE** – store the current setup to internal flash as the power-up state

### Reverb parameters

| Page | Range | What it does |
|---|---|---|
| DECAY | 0.20 s … 30 s | Reverb time (RT60), exponential |
| MIX | 0 … 100 % | Equal-power dry/wet crossfade |
| LO CUT | 20 Hz … 1 kHz | 12 dB/oct high-pass on the reverb send |
| HI CUT | 500 Hz … 18 kHz | Damping inside the tank: higher frequencies decay faster |

### Delay parameters

| Page | Range | What it does |
|---|---|---|
| TIME | 10 ms … 2 s | Delay time, exponential. Changes glide smoothly (tape-style pitch bend) instead of clicking |
| FEEDBK | 0 … 95 % | How much of each echo is fed back. Soft-clipped, so high settings saturate rather than run away |
| MIX | 0 … 100 % | Equal-power dry/wet crossfade |
| PING | 0 … 100 % | 0 = separate L/R echoes, 100 = full ping-pong (echoes alternate sides, even from a mono input) |
| LO CUT | 20 Hz … 1 kHz | 6 dB/oct high-pass in the feedback loop: each repeat gets thinner |
| HI CUT | 500 Hz … 18 kHz | 6 dB/oct low-pass in the feedback loop: each repeat gets darker |

On parameter pages the big number is the live value, the bar below shows it graphically, and a
small tick above the bar marks the stored value (they differ while CV is modulating). Tags in the
header (`K1`, `C2`…) show which sources are routed to that parameter. The two 1‑pixel columns at
the right edge are input (left) and output (right) level meters.

## SD card presets

Presets are plain text files on the microSD card (FAT32), one per slot:

```
BLUEMCHN/PRESET01.TXT … PRESET99.TXT
```

The LOAD SD / SAVE SD pages show the slot number, which effect a used slot is for (`REVERB` /
`DELAY`) or `EMPTY` (or `NO CARD`), and the preset's name. Loading a preset for the other effect
switches to it. Saving writes the active effect. The card is never touched at boot; it's mounted the first time you open
an SD page (the screen shows `SD...` while that happens, and audio keeps running). It can be
inserted or swapped at any time. Saving to a slot that already has a name keeps that name; new
slots are named `PRESET nn`.

If something goes wrong, the bottom line of the SD page shows a short code such as `F3 E4 4b`:
the FatFS result (`F3` = not ready, `F1` = disk error, `F13` = no FAT filesystem), the STM32 SD
error bits in hex (`E4` = no reply to commands, i.e. no card; `E8` = data timeout; `E2` = data
CRC error), and the bus mode in use (`1b`).

The card runs in 1-bit mode at 12.5 MHz. 4-bit mode doesn't get data back on the Bluemchen,
and each failed attempt waits out the STM32 HAL's fixed hardware timeout (2^32 SD clocks,
about 3 minutes at 25 MHz), so it's not used. Preset files are tiny, so the speed difference
doesn't matter.

Loading a preset changes the live sound but not the power-up state. Hold the encoder (or use
the SAVE page) if you want the loaded preset to come back at power-up. Knobs routed to a
parameter only take over again once you move them, so a loaded preset isn't overwritten by
the knob positions.

The `sd-card` folder in this project has example presets. Copy its `BLUEMCHN` folder to the root
of the card:

| Slot | Effect | Name |
|---|---|---|
| 01–05 | Reverb | Small Room, Plate, Big Hall, Dark Cave, Mono Wash |
| 06–09 | Delay | Slapback, Ping Pong, Dub Echo, Tape Wash |

### File format

```
# '#' or ';' starts a comment (also at the end of a line)
name   = Big Hall      # up to 16 characters, shown on the OLED
effect = REVERB        # REVERB | DELAY (which effect the lines below are for)
input  = stereo        # stereo | mono
decay  = 4.50          # seconds, 0.20 .. 30.00
mix    = 40            # percent, 0 .. 100
lo_cut = 120           # Hz, 20 .. 1000
hi_cut = 7000          # Hz, 500 .. 18000  ("7k" works too)
knob1  = decay         # none | decay | mix | lo_cut | hi_cut
knob2  = mix
cv1    = decay
cv2    = hi_cut
```

- Keys are case-insensitive, and spaces or dashes in keys work too (`Hi Cut = 7k` is fine).
- Values are in real units; unit text after the number is ignored (`4.5 s`, `40%`, `120Hz`).
- Out-of-range values are clamped to the range.
- Delete a line and loading keeps the module's current value for that setting.
- Unknown keys are ignored, so files stay loadable as the firmware grows.
- If a file has nothing usable in it, the module shows `BAD FILE` and changes nothing; an
  unknown `effect` name shows `BAD FX`. A file without an `effect` line applies to the active effect.
- Delay keys: `time` (seconds), `feedbk` (%), `mix` (%), `ping` (%), `lo_cut`, `hi_cut` (Hz).

## Code layout

```
src/
  main.cpp            audio callback, encoder events, knob takeover, MIDI, flash storage
  hw/bluemchen.*      hardware layer: encoder, 2 pots, 2 CV, OLED, MIDI, DAC, audio
  fx/effect.h         Effect interface + ParamSpec (name, range, curve, unit)
  fx/reverb.*         ReverbSc + SVF lo-cut + equal-power mix
  fx/delay.*          stereo delay: SDRAM delay lines, one-pole loop filters, ping-pong
  ui/ui.*             page/menu system and OLED drawing
  app/settings.h      persisted settings: per-effect params/routing (QSPI, last 4 KB sector)
  app/preset_text.*   preset text format: write + parse (no hardware deps)
  app/preset_store.*  SD card mount + slot files (FatFS)
sd-card/BLUEMCHN/     example presets to copy to the card
```

### Adding another effect

Implement `fx::Effect`: list your parameters in a `ParamSpec` table, handle them in `SetParam`,
process audio in `Process`, and clear buffers in `Reset`. Then **append** it to `EffectId` /
`effects[]` in `main.cpp` (never reorder: the index is saved in flash) and give it default
routing in `MakeDefaults()`. The EFFECT page, parameter pages, knob/CV routing, MIDI CCs, presets
and flash storage all follow from the parameter table (up to 8 parameters, up to 4 effects
before `kMaxEffects` must grow, which is a settings layout change).

## Notes

- **Flash usage is ~92 % of the 128 KB internal flash** (two effects).
  A third effect will likely need the bootloader (issue #5). To make room for the SD card code, the
  project and libDaisy are built with `-Os` (size); the DSP files opt back in to `-O2`
  with a pragma, and DaisySP stays at its normal optimisation. If you set the project up
  before this change, re-run `./setup.sh` to rebuild libDaisy with `OPT=-Os`, otherwise the
  link fails with "FLASH overflowed". For much bigger effects, switch to the Daisy bootloader
  (`APP_TYPE = BOOT_SRAM` in the Makefile, `make program-boot`).
  If you use the bootloader, settings are stored at the very end of QSPI (`kFlashOffset` in
  `main.cpp`) which stays clear of the bootloader's program area.
- ReverbSc is part of **DaisySP-LGPL** (enabled with `USE_DAISYSP_LGPL = 1`), so the firmware
  binary falls under the LGPL's terms if you distribute it. The rest of this code is yours to license.
- The decay display is an RT60 estimate from ReverbSc's mean loop delay; a host-side check measured
  roughly 0.53 s / 1.8 s / 6.7 s for settings of 0.5 s / 2 s / 8 s (Hi Cut damping shortens long tails a bit).
- Settings saved by v0.3.0 (single-effect layout) are converted automatically on first boot
  of this firmware: your reverb settings carry over and the delay starts from its defaults.
- CV polarity follows the kxmx reference code. If a CV input moves its parameter the wrong
  way, change `flip` for that input in `Hardware::InitControls()`.
