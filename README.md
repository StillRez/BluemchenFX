# BluemchenFX

[![Build firmware](https://github.com/jeffreykrueger/BluemchenFX/actions/workflows/build.yml/badge.svg)](https://github.com/jeffreykrueger/BluemchenFX/actions/workflows/build.yml)

Clean-slate effect firmware for the **kxmx Bluemchen** (Daisy Seed, 4HP), built on current
[libDaisy](https://github.com/electro-smith/libDaisy) and [DaisySP](https://github.com/electro-smith/DaisySP).
Two effects, **reverb** and **delay**, switchable from the OLED + encoder UI, with knob/CV
assignment, MIDI control and text-file presets on the SD card.

Hardware pin map comes from [recursinging/kxmx_bluemchen](https://github.com/recursinging/kxmx_bluemchen),
rewritten against today's libDaisy API (named `seed::Dxx` pins, current `Encoder`/`OledDisplay` APIs).

## Quick start

BluemchenFX runs through the **Daisy bootloader** (since v0.5.0). The firmware is stored in the
Seed's 8 MB QSPI flash and runs from its 480 KB SRAM, so there's room for bigger effects than the
128 KB internal flash allows. That means two steps: install the bootloader once, then flash
the firmware through it.

### 1. Install the bootloader (once per module)

Put the Daisy in **system DFU mode**: hold BOOT, tap RESET, release BOOT. Then either:
- `make program-boot` (flashes `lib/libDaisy/core/dsy_bootloader_v6_4-intdfu-2000ms.bin`), or
- in the [Daisy Web Programmer](https://flash.daisy.audio/), connect to **DFU in FS Mode** and flash
  that same `.bin` file.

Your saved settings are kept: they live in QSPI, and installing the bootloader only rewrites the
internal flash.

### 2. Flash the firmware

Put the bootloader in **flashing mode**: tap RESET, then tap BOOT within ~2 seconds. The Seed's LED
keeps pulsing slowly, waiting for you. Then either:
- `make program-dfu` (builds first if needed), or
- in the [Daisy Web Programmer](https://flash.daisy.audio/), connect to **Daisy Bootloader** and
  flash `prebuilt/BluemchenFX.bin` or the `.bin` from a [release](https://github.com/jeffreykrueger/BluemchenFX/releases).

From then on, every power-up spends ~2 seconds in the bootloader (LED pulsing) before
BluemchenFX starts.

**Build from source** (needs the [Daisy Toolchain](https://github.com/electro-smith/DaisyWiki/wiki/1.-Setting-Up-Your-Development-Environment); on Windows use Git Bash):

```
git clone --recursive https://github.com/jeffreykrueger/BluemchenFX.git
cd BluemchenFX
./setup.sh          # once: fetches libDaisy + DaisySP (pinned submodules) and builds them
make                # builds build/BluemchenFX.bin
make program-boot   # once per module: installs the Daisy bootloader (system DFU mode)
make program-dfu    # rebuilds if needed, then flashes via the bootloader
```

`make program-dfu` usually ends with `Error during download get_status` / `Error 74` right after
`File downloaded successfully`. That's harmless: the Daisy restarts into the new firmware before
answering dfu-util's last status request. Check that the `size = …` it reports matches your new
`build/BluemchenFX.bin`.

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

## License

BluemchenFX is open source under the **MIT License**. See [LICENSE](LICENSE).

The firmware builds on these libraries, included as git submodules under `lib/`, each under its own license:

| Component | Used for | License |
|---|---|---|
| [libDaisy](https://github.com/electro-smith/libDaisy) | Daisy Seed hardware support (bundles ST's HAL/CMSIS and FatFs under their own permissive licenses) | MIT |
| [DaisySP](https://github.com/electro-smith/DaisySP) | DSP building blocks (delay line, filters, soft clipping) | MIT |
| [DaisySP-LGPL](https://github.com/electro-smith/DaisySP-LGPL) | `ReverbSc`, the reverb engine | **LGPL-2.1** |
| [kxmx_bluemchen](https://github.com/recursinging/kxmx_bluemchen) | Hardware pin map and init sequence that `src/hw/bluemchen.*` is based on | MIT (© 2020 recursinging) |

**About the LGPL part:** the reverb comes from DaisySP-LGPL and is statically linked into the
firmware, so the `.bin` files in `prebuilt/` and on the Releases page contain LGPL-2.1 code. That's
compatible with this project's MIT license. The LGPL's main requirement for distributed binaries
is that people can rebuild them with a modified version of the library. This repository already
provides that: the complete source, the pinned library versions and the build scripts. If you
distribute your own builds, keep doing the same: ship or link the source and the build steps.
If you need a binary with no LGPL code, remove the reverb (`src/fx/reverb.*` and
`USE_DAISYSP_LGPL` in the Makefile).

## Notes

- **Memory:** the bootloader build (`APP_TYPE = BOOT_SRAM`, the default) runs code from the
  480 KB AXI SRAM (~27 % used with two effects, at `-O2`). Variables and the stack live in the
  128 KB DTCM (~45 % used), and effect buffers live in the 64 MB SDRAM. Settings are stored in
  the last 4 KB of QSPI, far from the program area that starts at `0x90040000`. libDaisy is
  built with `-Os` (`setup.sh`), because its hardware code isn't performance-critical.
- **Without the bootloader:** `make APP_TYPE=BOOT_NONE OPT=-Os` still builds the old
  internal-flash version (91 % of 128 KB) and flashes in system DFU mode. It will stop fitting as
  effects grow.
- **Don't put `.bin` files in the SD card's root folder.** During its 2-second startup window the
  bootloader looks for firmware there and would flash it. Presets live in `BLUEMCHN/`, which it
  ignores. Deliberately copying `BluemchenFX.bin` to the root should update the firmware without
  USB, but that's untested on the Bluemchen: the bootloader uses its own SD driver, and this board
  has trouble with 4-bit SD transfers (see the SD notes above).
- The decay display is an RT60 estimate from ReverbSc's mean loop delay; a host-side check measured
  roughly 0.53 s / 1.8 s / 6.7 s for settings of 0.5 s / 2 s / 8 s (Hi Cut damping shortens long tails a bit).
- Settings saved by v0.3.0 (single-effect layout) are converted automatically on first boot
  of this firmware: your reverb settings carry over and the delay starts from its defaults.
- CV polarity follows the kxmx reference code. If a CV input moves its parameter the wrong
  way, change `flip` for that input in `Hardware::InitControls()`.
