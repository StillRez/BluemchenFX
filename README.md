# BluemchenFX

Clean-slate effect firmware for the **kxmx Bluemchen** (Daisy Seed, 4HP), built on current
[libDaisy](https://github.com/electro-smith/libDaisy) and [DaisySP](https://github.com/electro-smith/DaisySP).
The first effect is a stereo reverb with decay time, dry/wet mix and filtering, all set from the
OLED + encoder UI.

Hardware pin map comes from [recursinging/kxmx_bluemchen](https://github.com/recursinging/kxmx_bluemchen),
rewritten against today's libDaisy API (named `seed::Dxx` pins, current `Encoder`/`OledDisplay` APIs).

## Quick start

**Flash the prebuilt binary:** put the Daisy in DFU mode (hold BOOT, tap RESET), then either
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
| Encoder hold (0.7 s) | Store all settings to flash ("SAVED") |
| Knob 1 / Knob 2 | Absolute control of the routed parameter (default **Decay** / **Mix**). A knob only takes over once you move it, so stored values aren't overwritten at power-up |
| CV 1 / CV 2 | Bipolar offset on the routed parameter, ±5 V = ±full range (default **Decay** / **Hi Cut**) |
| MIDI CC 20–23 | Decay, Mix, Lo Cut, Hi Cut (any channel) |
| MIDI Program Change 0–98 | Load SD preset 1–99 |
| INPUT page | **STEREO** (default) or **MONO**. In mono, the left input is copied to the right channel before any effect processing, so a single cable into the left jack gives a full stereo reverb. "MONO" also shows in the HOME header |

Moving a knob or sending a CC briefly pops up that parameter's page.

## Display pages (64×32 OLED)

1. **HOME** – a bar per parameter (shows the live value including CV)
2. **DECAY** – 0.20 s … 30 s (RT60, exponential)
3. **MIX** – 0 … 100 % (equal-power crossfade)
4. **LO CUT** – 20 Hz … 1 kHz, 12 dB/oct high-pass on the reverb send
5. **HI CUT** – 500 Hz … 18 kHz, damping inside the reverb tank (higher frequencies decay faster)
6. **KNOB 1 / KNOB 2 / CV 1 / CV 2** – choose which parameter each source controls (or `--`)
7. **INPUT** – STEREO / MONO (click, turn to change, click to confirm; saved with the other settings)
8. **LOAD SD** – click, turn to pick a slot (01–99), click again to load it
9. **SAVE SD** – click, turn to pick a slot, click again to write the current setup to it
10. **SAVE** – store the current setup to internal flash as the power-up state

On parameter pages the big number is the live value, the bar below shows it graphically, and a
small tick above the bar marks the stored value (they differ while CV is modulating). Tags in the
header (`K1`, `C2`…) show which sources are routed to that parameter. The two 1‑pixel columns at
the right edge are input (left) and output (right) level meters.

## SD card presets

Presets are plain text files on the microSD card (FAT32), one per slot:

```
BLUEMCHN/PRESET01.TXT … PRESET99.TXT
```

The LOAD SD / SAVE SD pages show the slot number, whether it's `used` or `EMPTY` (or `NO CARD`),
and the preset's name. The card is never touched at boot; it's mounted the first time you open
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

The `sd-card` folder in this project has five example presets (Small Room, Plate, Big Hall,
Dark Cave, Mono Wash). Copy its `BLUEMCHN` folder to the root of the card.

### File format

```
# '#' or ';' starts a comment (also at the end of a line)
name   = Big Hall      # up to 16 characters, shown on the OLED
effect = REVERB        # informational
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
- If a file has nothing usable in it, the module shows `BAD FILE` and changes nothing.

## Code layout

```
src/
  main.cpp            audio callback, encoder events, knob takeover, MIDI, flash storage
  hw/bluemchen.*      hardware layer: encoder, 2 pots, 2 CV, OLED, MIDI, DAC, audio
  fx/effect.h         Effect interface + ParamSpec (name, range, curve, unit)
  fx/reverb.*         ReverbSc + SVF lo-cut + equal-power mix
  ui/ui.*             page/menu system and OLED drawing
  app/settings.h      persisted settings struct (QSPI, last 4 KB sector)
  app/preset_text.*   preset text format: write + parse (no hardware deps)
  app/preset_store.*  SD card mount + slot files (FatFS)
sd-card/BLUEMCHN/     example presets to copy to the card
```

### Adding another effect

Implement `fx::Effect` (list your parameters in a `ParamSpec` table, handle them in `SetParam`,
process audio in `Process`), then point `effect` in `main.cpp` at it and update `MakeDefaults()`.
The UI pages, knob/CV routing, MIDI CCs and flash storage are generated from the parameter table
(up to 8 parameters). Bump `kSettingsVersion` in `app/settings.h` if you change the parameter set.

## Notes

- **Flash usage is ~85 % of the 128 KB internal flash.** To make room for the SD card code, the
  project and libDaisy are built with `-Os` (size); `fx/reverb.cpp` opts back in to `-O2`
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
- The reverb, UI and mono/stereo input have been run on the hardware; the SD preset code was
  compiled and its text format tested on a PC, but not yet run on the module. The SD
  interface uses the same 4-bit / 50 MHz settings as the kxmx reference code.
- CV polarity follows the kxmx reference code. If a CV input moves its parameter the wrong
  way, change `flip` for that input in `Hardware::InitControls()`.
