# BluemchenFX

[![Build firmware](https://github.com/StillRez/BluemchenFX/actions/workflows/build.yml/badge.svg)](https://github.com/StillRez/BluemchenFX/actions/workflows/build.yml)

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

Put the bootloader in **flashing mode**: tap and **release** RESET, wait for the Seed's LED to start
its slow pulse, and **while it's pulsing** tap BOOT once. You'll see a few rapid blinks, then the slow
pulse continues indefinitely, waiting for you. (Pressing BOOT *during* RESET instead enters the
STM system DFU mode, which can't load this firmware.) Then either:
- `make program-dfu` (builds first if needed), or
- in the [Daisy Web Programmer](https://flash.daisy.audio/), connect to **Daisy Bootloader** and
  flash `prebuilt/BluemchenFX.bin` or the `.bin` from a [release](https://github.com/StillRez/BluemchenFX/releases).

From then on, every power-up spends ~2 seconds in the bootloader (LED pulsing) before
BluemchenFX starts.

**Troubleshooting**
- Web programmer says *"Attempting to flash an incompatible binary. Try entering the Daisy
  bootloader"*: the module is in system DFU mode, not the Daisy bootloader. Redo the
  RESET → wait for pulse → BOOT sequence and connect to **Daisy Bootloader**.
- `dfu-util -l` tells you which mode you're in: the Daisy bootloader lists a QSPI region (`0x90…`),
  while system DFU lists only `@Internal Flash /0x08000000/…`.
- LED pulses briefly at power-up and then stays solid, with a blank OLED: the bootloader has no
  valid program to start, usually because the last flash went to the wrong mode. Flash the
  firmware again in the Daisy bootloader.
- For deeper problems, `make DIAG=1 BUILD_DIR=build-diag` builds a version that blinks the LED and
  shows each startup step on the OLED, and `tools/bootcheck` is a minimal test program for the
  bootloader itself.

**Build from source** (needs the [Daisy Toolchain](https://github.com/electro-smith/DaisyWiki/wiki/1.-Setting-Up-Your-Development-Environment); on Windows use Git Bash):

```
git clone --recursive https://github.com/StillRez/BluemchenFX.git
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

Audio runs through a **chain of up to 4 effect slots** (slot 1 → 2 → 3 → 4). Each slot holds
an effect (REVERB, DELAY; the same one twice is fine) or is empty. Changing an effect,
bypassing and loading presets crossfade over ~10 ms, so none of them click, and the other
slots keep playing. Saved settings from earlier versions move into slot 1 automatically.

| Control | Function |
|---|---|
| Encoder turn | HOME: select a slot (or SETUP). On other pages: next/previous page; while editing: change the value |
| Encoder click | HOME: open the selected slot or SETUP. Elsewhere: enter/leave edit mode (header inverts while editing); on SAVE: store settings |
| Encoder hold (0.7 s) | HOME: store all settings to flash ("SAVED"). Elsewhere: **back to HOME** (also cancels an edit) |
| Knob 1 / Knob 2 | Absolute control of the routed parameter, in any slot (defaults: reverb **Decay** / **Mix**, delay **Time** / **Mix**). A knob only takes over once you move it, so stored values aren't overwritten at power-up |
| CV 1 / CV 2 | Bipolar offset on the routed parameter, ±5 V = ±full range (defaults: reverb **Decay** / **Hi Cut**, delay **Time** / **Feedback**) |
| MIDI CC 20–51 | Parameters of every slot, in page order: CC = 20 + 8 × (slot − 1) + parameter number (from 0). Slot 1 = CC 20–27, slot 2 = 28–35, slot 3 = 36–43, slot 4 = 44–51 (the order of the tables below, e.g. reverb: Decay, Mix, Lo Cut, Hi Cut). Any channel |
| MIDI CC 52–55 | Bypass slots 1–4: value 64–127 = bypassed, 0–63 = on (in the slot's bypass mode) |
| MIDI Program Change 0–98 | Load SD preset 1–99 |

MIDI comes in on the TRS jack. Set the module's MIDI jumpers to **TRS Type A**, or nothing will arrive.

Moving a knob or sending a CC briefly pops up that parameter's page (with its slot number).

## Display pages (64×32 OLED)

**HOME** shows the chain, one row per slot: `1 DELAY`, `2 reverb` (lowercase = bypassed),
`3 --` (empty), then **SETUP** below slot 4 (the list scrolls; small arrows show there's more).
The thin bar right of the list is the CPU load; it inverts when the peak gets close to dropouts.
The two bars on the far right are the input and output meters.

**Slot pages** (click a slot on HOME). The slot number is in the box at the top left.
1. **TYPE** – EMPTY / REVERB / DELAY. Click, turn, click. Replacing an effect asks once more
   (`REPLACE?`): click to confirm, turn or hold to back out. Switching a slot back to an
   effect it had brings back that effect's settings (until power-off). Knobs/CVs that
   controlled the slot move to the new effect's usual parameters.
2. **BYPASS** – OFF / ON
3. **BYP MODE** – TRAILS (bypass mutes the effect's input, so reverb/echo tails ring out) or
   HARD (cuts the effect immediately and stops processing it, which saves CPU)
4. **Parameter pages** for the effect (see below). Tags in the header (`K1`, `C2`…) show which
   knobs/CVs control that parameter.

An empty slot only has TYPE. Opening a slot shows its first parameter the first time, then
the page you were on last.

**SETUP pages** (click SETUP on HOME)
1. **KNOB 1 / KNOB 2 / CV 1 / CV 2** – which parameter each source controls: `1:DECAY`,
   `2:TIME`, … across all slots, or `NONE`
2. **INPUT** – STEREO / MONO. In mono, the left input is copied to the right channel before any
   effect processing, so a single cable into the left jack gives full stereo output
3. **OUT** – output level, −24 to +6 dB in 0.5 dB steps (0 dB = unity). Useful when stacked
   effects get loud
4. **LIMIT** – ceiling of the always-on output limiter, −12 to 0 dB (default −1 dB). The
   bottom line shows the live gain reduction (`GR -3.4 DB`), and an `ON` tag appears in the
   header while it's limiting. Stacked effects can get much louder than their input (three
   reverbs push a held note about +15 dB). The limiter holds the output just under the ceiling
   instead of letting it clip at the codec. Below the ceiling minus 1 dB it doesn't touch the
   sound
5. **LIM REL** – how fast the limiter lets go, 20 ms to 1 s (default 150 ms). Short is
   punchier but can distort low notes; long is smoother
6. **CPU** – audio processing load: average (large) and peak since the last reset (`pk`). Click
   resets the peak. The header shows `CPU!` inverted when the peak reaches 95 %
7. **LOAD SD** – click, turn to pick a slot (01–99), click again to load it
8. **SAVE SD** – click, turn to pick a slot, click again to write the whole chain to it
9. **SAVE** – store the current setup to internal flash as the power-up state

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

### Crush parameters (bit crusher)

| Page | Range | What it does |
|---|---|---|
| BITS | 1 … 16 bit | Bit depth. Continuous, so turning it sweeps smoothly; at low depths quiet sounds drop out, as on real low-bit converters |
| RATE | 100 Hz … 48 kHz | Sample-and-hold rate, exponential. Any rate, not just divisions of 48 kHz; the aliasing is the point. 48 kHz = off |
| DRIVE | 0 … 100 % | Gain into the crusher, up to +24 dB, clipped hard at full scale |
| HI CUT | 500 Hz … 18 kHz | 6 dB/oct low-pass after the crusher, to tame the aliasing. All the way up = off |
| MIX | 0 … 100 % | Equal-power dry/wet crossfade (default 100 %) |

### Chorus parameters

| Page | Range | What it does |
|---|---|---|
| RATE | 0.05 … 8 Hz | LFO speed, exponential |
| DEPTH | 0 … 100 % | How far the delay swings: up to ±95 % of DELAY |
| DELAY | 1 … 30 ms | Centre delay. Short + FEEDBK = flanger; 10–25 ms = chorus |
| FEEDBK | 0 … 90 % | Feedback for flanging; soft-clipped |
| SPREAD | 0 … 100 % | Phase offset between the left and right LFO, 0 … 180°: wide stereo even from a mono input |
| MIX | 0 … 100 % | Equal-power dry/wet crossfade. 100 % = pure vibrato |

### Reson parameters (modal resonator)

The input excites a bank of 16 tuned resonant modes per channel, like striking a string, bar or
bell (modelled on Mutable Instruments Rings' resonator). Drums, plucks and noise bring it to life;
a held note on the pitch rings up strongly (the output limiter catches it).

| Page | Range | What it does |
|---|---|---|
| PITCH | 30 Hz … 2 kHz | Fundamental, exponential. CV covers ~6 octaves over ±5 V (not 1 V/oct) |
| STRUCT | 0 … 100 % | Partial spacing: 25–30 % harmonic (string); lower squeezes them, higher stretches them into bars and bells |
| BRIGHT | 0 … 100 % | How long the upper partials ring compared with the fundamental: low = dark and soft |
| DECAY | 0.05 … 10 s | Ring time (RT60) of the fundamental |
| MIX | 0 … 100 % | Equal-power dry/wet crossfade |

Knob and CV routing when an effect is loaded into a slot they point at:

| Effect | Knob 1 | Knob 2 | CV 1 | CV 2 |
|---|---|---|---|---|
| REVERB | DECAY | MIX | DECAY | HI CUT |
| DELAY | TIME | MIX | TIME | FEEDBK |
| CRUSH | BITS | RATE | RATE | BITS |
| CHORUS | RATE | DEPTH | DEPTH | RATE |
| RESON | PITCH | DECAY | PITCH | STRUCT |

On parameter pages the big number is the live value, the bar below shows it graphically, and a
small tick above the bar marks the stored value (they differ while CV is modulating). Tags in the
header (`K1`, `C2`…) show which sources are routed to that parameter. The two 1‑pixel columns at
the right edge are input (left) and output (right) level meters.

## SD card presets

Presets are plain text files on the microSD card (FAT32), one per slot:

```
BLUEMCHN/PRESET01.TXT … PRESET99.TXT
```

A preset is the **whole chain**: all four slots (effect, bypass, parameters), the knob/CV
routing, the input mode and the OUT level. The limiter settings are not part of presets.
The LOAD SD / SAVE SD pages show the slot number, what a used slot holds (`REVERB`, `DEL>REV`
for two effects, `DL>RV>CH` for three, `DLRVCHRS` for four, using the codes RV, DL, CR, CH, RS), `EMPTY` (or `NO CARD`), and the preset's name.
The card is never touched at boot; it's mounted the first time you open
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
| 10 | Delay → Reverb | Dub Space |
| 11 | Reverb → Delay | Room Echo |
| 12 | Delay → Reverb → Reverb (bypassed) | Infinite Wash |
| 13 | Crush → Chorus | Lo-Fi Chorus |
| 14 | Reson → Reverb | Struck Bells (feed it drums or plucks) |
| 15 | Chorus (as a flanger) | Jet Flanger |

### Chain file format

What SAVE SD writes (shortened):

```
name        = Dub Space
input       = stereo
out         = 0.0      # output level in dB, -24.0 .. 6.0

[slot1]
effect      = DELAY    # REVERB | DELAY | CRUSH | CHORUS | RESON | none
bypass      = off      # on | off
bypass_mode = trails   # trails | hard
time        = 0.37
feedbk      = 55
mix         = 40

[slot2]
effect      = REVERB
decay       = 4.00
mix         = 30

[slot3]
effect      = none

[slot4]
effect      = none

knob1       = 1.time   # none | <slot>.<param>
knob2       = 2.mix
cv1         = 1.feedbk
cv2         = 2.decay
```

- A slot without a `[slotN]` section is emptied; a section without an `effect` line keeps the
  slot's current effect.
- A slot that changes effect starts from that effect's defaults, then takes the file's values.
  `bypass` / `bypass_mode` default to `off` / `trails` when missing.
- Routing can also name a parameter of the current section without the slot number
  (`knob2 = mix` inside `[slot2]` means `2.mix`).
- An unknown `effect` name anywhere shows `BAD FX` and changes nothing.
- All the rules below (comments, units, clamping, missing and unknown keys) apply too.

### Single-effect file format (before v0.7)

Files without `[slotN]` sections still load: the effect goes into slot 1 and slots 2–4 are
emptied.

```
# '#' or ';' starts a comment (also at the end of a line)
name   = Big Hall      # up to 16 characters, shown on the OLED
effect = REVERB        # any effect name (which effect the lines below are for)
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
  unknown `effect` name shows `BAD FX`. A file without an `effect` line applies to slot 1's effect.
- Delay keys: `time` (seconds), `feedbk` (%), `mix` (%), `ping` (%), `lo_cut`, `hi_cut` (Hz).

## Code layout

```
src/
  main.cpp            audio callback, encoder events, knob takeover, MIDI, flash storage
  hw/bluemchen.*      hardware layer: encoder, 2 pots, 2 CV, OLED, MIDI, DAC, audio
  fx/effect.h         Effect interface + ParamSpec (name, range, curve, unit)
  fx/arena.h          per-slot SDRAM bump allocator for effect buffers
  fx/registry.*       the effect list: ids (persisted), metadata prototypes, construction
  fx/reverb.*         ReverbSc + SVF lo-cut + equal-power mix
  fx/delay.*          stereo delay: SDRAM delay lines, one-pole loop filters, ping-pong
  fx/crush.*          bit crusher / sample-rate reducer
  fx/chorus.*         stereo chorus / flanger (SDRAM delay lines, Hermite reads)
  fx/reson.*          modal resonator (16 ZDF band-pass modes per channel, after Rings)
  ui/ui.*             menus (HOME chain overview, slot pages, SETUP) and OLED drawing
  app/settings.h      persisted settings v3: chain slots, routing (QSPI, last 4 KB sector)
  app/chain.*         chain engine: 4 serial slots, click-free bypass / effect swaps
  app/chain_settings.* defaults, validation, v1/v2 migration, slot recall (no hardware deps)
  app/preset_text.*   preset text format: write + parse (no hardware deps)
  app/preset_store.*  SD card mount + slot files (FatFS)
sd-card/BLUEMCHN/     example presets to copy to the card
tests/                host-side tests (DSP stability, preset format); see tests/README.md
docs/design/          design documents (effect chaining)
```

**Tests:** `make -C tests run` builds the effects and the preset code with your normal PC
compiler and checks them (no hardware needed; on Windows this needs a native `g++`, e.g. from
MSYS2). CI runs them on every push and won't build firmware if they fail.

### Adding another effect

Implement `fx::Effect`: give it a `Name()` (up to 7 characters, first 3 letters unique) and a
unique 2-letter `Code()` for chain summaries, list your parameters in a `ParamSpec` table, handle them in `SetParam`,
process audio in `Process`, clear buffers in `Reset`, and say which parameters the knobs/CVs
should control by default in `DefaultRoute`. Take big buffers from the arena passed to
`Init(sample_rate, arena)` and keep no global state: a chain can hold several copies of the
same effect. Then **append** it to `EffectId` and the tables in `src/fx/registry.cpp` (never
reorder: the id is saved in flash). The TYPE page, parameter pages, knob/CV routing, MIDI CCs,
presets and flash storage all follow from the parameter table (up to 8 parameters). Also add it
to the lists in `tests/test_effects.cpp` and `tests/test_chain.cpp`, so CI checks it for
stability (a test fails if you forget), and add tests for what it should do (see
`tests/test_effect_behaviour.cpp`).

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
