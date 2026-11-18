# CLAUDE.md — BluemchenFX

Effect firmware for the **kxmx Bluemchen** (4HP Eurorack, Daisy Seed). Owner: Jeff Krueger
(GitHub `jeffreykrueger`). Read `DEVLOG.md` for history and the reasoning behind decisions;
open work is tracked in GitHub Issues.

## Hardware facts

Verified on the module unless noted.

| Thing | Detail |
|---|---|
| MCU | Daisy Seed (STM32H750, 480 MHz), 64 MB SDRAM, 8 MB QSPI |
| Encoder A / B / click | D27 / D26 / D28 |
| Pots 1 / 2 | D16 / D15 (`flip = false`) |
| CV 1 / 2 | D21 / D18 (`flip = true`, inverting stage, mapped to -1..+1) |
| OLED | SSD1306 64x32, I2C (libDaisy `SSD130xI2c64x32Driver` defaults) |
| MIDI | TRS in, UART (`MidiUartHandler` defaults) |
| microSD | SDMMC1. **1-bit only.** 4-bit transfers never complete on this board, and each failed attempt waits ~172 s (HAL timeout 2^32 clocks @ 25 MHz). Do not re-enable 4-bit ahead of 1-bit. 1-bit at 12.5 MHz: first mount is near-instant (confirmed on hardware, v0.3.0). |
| Audio | 48 kHz, 32-sample blocks, stereo in/out |

Pin map originally from https://github.com/recursinging/kxmx_bluemchen (old libDaisy),
rewritten for current libDaisy in `src/hw/bluemchen.*`.

## Build

```
git clone --recursive https://github.com/jeffreykrueger/BluemchenFX.git
./setup.sh      # builds lib/libDaisy with OPT=-Os, then lib/DaisySP (+ DaisySP-LGPL)
make            # -> build/BluemchenFX.bin
```

- Toolchain: arm-none-eabi-gcc (CI uses xPack 13.3.1-1.1; Jeff uses the Daisy Toolchain on Windows / Git Bash).
- **Flash budget: 128 KB internal, currently ~86 % used.** The project and libDaisy build with
  `-Os` to fit. `src/fx/reverb.cpp` opts back into `-O2` with `#pragma GCC optimize`; do the
  same for new DSP files. If the link fails with "FLASH overflowed", libDaisy was probably built
  without `OPT=-Os`. The next step for more room is the Daisy bootloader (`APP_TYPE = BOOT_SRAM`).
- Submodules are pinned (libDaisy `1289a00`, DaisySP `2c72eaf`, DaisySP-LGPL `c89d380`).
  DaisySP's `.gitmodules` uses `https://www.github.com/...` for DaisySP-LGPL. Some sandboxed
  networks block `www.github.com`; clone `https://github.com/electro-smith/DaisySP-LGPL` at
  `c89d380` into `lib/DaisySP/DaisySP-LGPL` by hand if that happens.
- After a change that should ship, copy `build/BluemchenFX.bin` to `prebuilt/`. CI warns if
  they differ.

## Flashing

DFU: hold BOOT, tap RESET, then either `make program-dfu` or the Daisy Web Programmer
(https://flash.daisy.audio/, choose "DFU in FS Mode", not "Daisy Bootloader"). On Windows,
Zadig/WinUSB may be needed once.

## Code map

```
src/main.cpp            audio callback, encoder events, knob takeover, MIDI, QSPI save, SD actions
src/hw/bluemchen.*      hardware layer
src/fx/effect.h         Effect interface + ParamSpec (name, range, curve, unit, ToReal/ToNorm)
src/fx/reverb.*         ReverbSc (SDRAM) + SVF lo cut + equal-power mix; decay = RT60 seconds
src/ui/ui.*             page/menu system, 64x32 drawing
src/app/settings.h      persisted POD struct (QSPI, last 4 KB sector)
src/app/preset_text.*   preset text format (hardware-independent, host-testable)
src/app/preset_store.*  SD mount + slot files (FatFS)
sd-card/BLUEMCHN/       example presets
```

## Conventions and gotchas

- **New effects** implement `fx::Effect` and publish a `ParamSpec` table. UI pages, knob/CV
  routing, MIDI CC (20+n) and preset keys are generated from it. Max 8 params. Header names
  can be up to 7 chars.
- **`Settings` layout is persisted.** Adding fields: use the `pad` bytes, where zero must mean
  "old behaviour", or bump `kSettingsVersion`. The latter resets users' saved settings.
- **No float `printf`/`snprintf("%f")`** (newlib-nano). Format with integers, as `FormatValue`
  and `FormatReal` do.
- **Audio ISR vs main loop:** the audio callback is the only writer of encoder counters and
  `effective[]`. The main loop writes `settings` (benign races on float/byte stores). Never do
  SD, QSPI, or display I/O in the audio callback.
- **SD:** mount lazily, never at boot. Every mount attempt must `f_mount(NULL)`,
  `SdmmcHandler::Reset()`, and clear `disk.is_initialized[pdrv]` (libDaisy's FatFS glue latches
  that flag even when init fails). FatFS objects must keep their sector buffers 32-byte aligned
  (`DmaSafe` wrapper + `static_assert`s), because libDaisy's DMA cache maintenance works on whole lines.
- **Preset text format** is documented in `src/app/preset_text.h` and the README. Keep it
  forward-compatible: unknown keys ignored, missing keys keep current values.
- **Knob takeover:** knobs only take control after moving > 0.03; encoder edits, MIDI CC and
  preset loads disengage them.
- The remote file bridge used to copy files to Jeff's PC refuses to write a file named
  `Makefile`, so his non-git folder has `Makefile.in` and `setup.sh` copies it.

## Workflow

1. Make the change. Build locally if a toolchain is available, and check flash usage.
2. Test hardware-independent code on the host where possible (e.g. `preset_text.cpp` builds
   with plain g++).
3. Update `prebuilt/BluemchenFX.bin` if the firmware changed.
4. Add a dated entry to `DEVLOG.md`: what changed, why, and what's verified on hardware vs only
   compiled. Mark hardware confirmation as pending until Jeff reports back.
5. Commit, push, and reference or close the relevant issue.
6. Release builds Jeff flashes as `vX.Y.Z`: either push a tag, or create the release in GitHub (UI or `POST /repos/.../releases`). CI builds and attaches `BluemchenFX.bin` either way. Mark it pre-release until it's confirmed on hardware. Claude's cloud sessions can't push tags or create releases, so Jeff publishes releases in the GitHub UI (Releases → Draft a new release → new tag). Claude can draft the release notes.

Things are only "verified" once Jeff has run them on the module. Say so explicitly otherwise.
