# CLAUDE.md — BluemchenFX

Effect firmware for the **kxmx Bluemchen** (4HP Eurorack, Daisy Seed). Owner: StillRez
(GitHub `StillRez`). **MIT licensed, meant to be fully open source.** It links
DaisySP-LGPL (`ReverbSc`, LGPL-2.1), so the README's License section explains the LGPL terms. Keep
new third-party code MIT-compatible and credit it in that table. Read `DEVLOG.md` for history and the reasoning behind decisions;
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
| Boot | **Daisy bootloader v6.4 (`intdfu-2000ms`) + `APP_TYPE = BOOT_SRAM`** since v0.5.0: app stored in QSPI at `0x90040000`, copied to AXI SRAM at boot. Verified on hardware: boot, flashing, settings, QSPI saves, SD presets (DMA buffers in SDRAM), boot with SD card inserted. |

Pin map originally from https://github.com/recursinging/kxmx_bluemchen (old libDaisy),
rewritten for current libDaisy in `src/hw/bluemchen.*`.

## Build

```
git clone --recursive https://github.com/StillRez/BluemchenFX.git
./setup.sh      # builds lib/libDaisy with OPT=-Os, then lib/DaisySP (+ DaisySP-LGPL)
make            # -> build/BluemchenFX.bin
```

- Toolchain: arm-none-eabi-gcc. CI and `prebuilt/` use xPack **13.3.1-1.1** (112,244-byte bin
  at v0.3.0). StillRez's Daisy Toolchain on Windows is **GCC 10.2.1 (10-2020-q4)** and produces a
  slightly larger, not byte-identical bin (113,908 bytes, 86.9 %). Check flash headroom against
  the larger of the two. Release binaries come from CI.
- Line endings: `.gitattributes` keeps `*.sh` and `Makefile` LF everywhere. StillRez's clone uses
  Git for Windows' CRLF conversion for other text files.
- **Memory budget (BOOT_SRAM, the default):** code + rodata in AXI SRAM, **480 KB** (131 KB used,
  27 %, at `-O2`). `.data`/`.bss` + stack in **DTCM, 128 KB** (57 KB used); the stack grows down
  from `0x20020000` into whatever's left. Big buffers go in SDRAM (`DSY_SDRAM_BSS`, 64 MB). The
  project builds at `OPT = -O2`; libDaisy stays `-Os` (setup.sh), which is fine for HAL code.
  Legacy internal-flash build: `make APP_TYPE=BOOT_NONE OPT=-Os` (91 % of 128 KB, will stop fitting).
- Submodules are pinned (libDaisy `1289a00`, DaisySP `2c72eaf`, DaisySP-LGPL `c89d380`).
  DaisySP's `.gitmodules` uses `https://www.github.com/...` for DaisySP-LGPL. Some sandboxed
  networks block `www.github.com`; clone `https://github.com/electro-smith/DaisySP-LGPL` at
  `c89d380` into `lib/DaisySP/DaisySP-LGPL` by hand if that happens.
- After a change that should ship, copy `build/BluemchenFX.bin` to `prebuilt/`. CI warns if
  they differ.

## Flashing

Two levels:
1. **Bootloader, once per module:** system DFU (hold BOOT, tap RESET), then `make program-boot`
   (installs `lib/libDaisy/core/dsy_bootloader_v6_4-intdfu-2000ms.bin` to internal flash), or the
   web programmer's "DFU in FS Mode" with that file. Settings in QSPI survive this.
2. **App, every update:** tap and release RESET, wait for the LED to pulse, then tap BOOT
   *while it pulses* (rapid blinks, then it keeps pulsing). BOOT held during RESET = STM system DFU
   instead, which can't take a BOOT_SRAM app: the web programmer then says "incompatible binary",
   and a mis-flash leaves the bootloader with no program (LED pulses then goes solid, OLED blank).
   That's exactly what happened on the first v0.5.0 attempt. Then flash with `make program-dfu` (the project Makefile makes it rebuild first; libDaisy's own
   target doesn't, which once flashed stale firmware), or the web programmer's "Daisy Bootloader"
   with the `.bin`.
On Windows, Zadig/WinUSB may be needed once. dfu-util's trailing `Error during download get_status` /
make `Error 74` after "File downloaded successfully" is normal: the Daisy resets because of `:leave`.

## Code map

```
src/main.cpp            audio callback, encoder events, knob takeover, MIDI, QSPI save, SD actions
src/hw/bluemchen.*      hardware layer
src/fx/effect.h         Effect interface + ParamSpec (name, range, curve, unit, ToReal/ToNorm)
src/fx/reverb.*         ReverbSc (SDRAM) + SVF lo cut + equal-power mix; decay = RT60 seconds
src/fx/delay.*          stereo delay <= 2 s (SDRAM DelayLines), OnePole loop filters, ping-pong
src/ui/ui.*             page/menu system, 64x32 drawing
src/app/settings.h      persisted POD struct v2: active effect + per-effect EffectState (QSPI)
src/app/preset_text.*   preset text format (hardware-independent, host-testable)
src/app/preset_store.*  SD mount + slot files (FatFS)
sd-card/BLUEMCHN/       example presets
```

## Conventions and gotchas

- **New effects** implement `fx::Effect` (incl. `Reset()`) and publish a `ParamSpec` table. UI
  pages, knob/CV routing, MIDI CC (20+n) and preset keys are generated from it. Max 8 params.
  Header names can be up to 7 chars. Register by **appending** to `EffectId` / `effects[]` in
  `main.cpp`: the index is persisted, so never reorder. Presets refer to effects by `Name()`.
  Up to `kMaxEffects` = 4 without a layout change.
- **Effect switching** goes through `ApplySettings()` in `main.cpp`: it sets `switching` (audio
  callback passes dry), waits 2 ms, pushes params, calls `Reset()` on the incoming effect,
  swaps settings, then clears `switching`. Never change `settings->effect` any other way.
- **`Settings` layout is persisted** (v2 = `0xB10E0002`, 168 bytes). Adding fields: use `pad`
  bytes where zero means "old behaviour", or bump the version **and** add a migration like
  `ReadLegacySettings()` (v1 -> v2), so users keep their settings.
- **Don't use `daisysp::Svf` inside feedback loops.** Its built-in cubic drive term
  (`band -= 0.5*band^3`) goes unstable at the levels a saturating loop reaches (host test blew
  up at 95 % feedback). Use `daisysp::OnePole` there. Svf is fine on bounded inputs (reverb send).
- **No float `printf`/`snprintf("%f")`** (newlib-nano). Format with integers, as `FormatValue`
  and `FormatReal` do.
- **Audio ISR vs main loop:** the audio callback is the only writer of encoder counters and
  `effective[]`. The main loop writes `settings` (benign races on float/byte stores). Never do
  SD, QSPI, or display I/O in the audio callback.
- **DMA-reachable memory:** in BOOT_SRAM builds ordinary globals land in **DTCM**, which most
  DMA engines (incl. SDMMC1) can't reach. Anything a peripheral DMAs into or out of goes in
  SDRAM (`DSY_SDRAM_BSS`, cacheable, so 32-byte align it and make it whole cache lines) or in
  libDaisy's `DMA_BUFFER_MEM_SECTION` (D2 SRAM, uncached). Check with `arm-none-eabi-nm`:
  `0x2000xxxx` = DTCM = wrong for DMA.
- **SD:** mount lazily, never at boot. Every mount attempt must `f_mount(NULL)`,
  `SdmmcHandler::Reset()`, and clear `disk.is_initialized[pdrv]` (libDaisy's FatFS glue latches
  that flag even when init fails). FatFS objects must keep their sector buffers 32-byte aligned
  (`DmaSafe` wrapper + `static_assert`s), because libDaisy's DMA cache maintenance works on whole lines.
  They live in SDRAM (see DMA-reachable memory). Don't put `.bin` files in the SD root: the
  bootloader flashes the first one it finds there at power-up.
- **Preset text format** is documented in `src/app/preset_text.h` and the README. Keep it
  forward-compatible: unknown keys ignored, missing keys keep current values.
- **Knob takeover:** knobs only take control after moving > 0.03; encoder edits, MIDI CC and
  preset loads disengage them.
- **Hardware debugging without a debugger:** `make DIAG=1 BUILD_DIR=build-diag` (LED blinks after
  `seed.Init`, OLED shows each startup step via `Diag()` in `main.cpp`); `tools/bootcheck` is a
  minimal BOOT_SRAM program to test the bootloader path on its own.
- StillRez's working copy is a clone at `C:\Coding Projects\BluemchenFX` (Windows, Git Bash +
  Daisy Toolchain). He gets changes with `git pull` (and `git submodule update --init --recursive`
  if submodule pins change). The old non-git folder `C:\Coding Projects\Daisy effects module`
  is retired. Don't write to it.

## Workflow

1. Make the change. Build locally if a toolchain is available, and check flash usage.
2. Test hardware-independent code on the host where possible (e.g. `preset_text.cpp` builds
   with plain g++).
3. Update `prebuilt/BluemchenFX.bin` if the firmware changed.
4. Add a dated entry to `DEVLOG.md`: what changed, why, and what's verified on hardware vs only
   compiled. Mark hardware confirmation as pending until StillRez reports back.
5. Commit, push, and reference or close the relevant issue.
6. Release builds StillRez flashes as `vX.Y.Z`: either push a tag, or create the release in GitHub (UI or `POST /repos/.../releases`). CI builds and attaches `BluemchenFX.bin` either way. Mark it pre-release until it's confirmed on hardware. Claude's cloud sessions can't push tags or create releases, so StillRez publishes releases in the GitHub UI (Releases → Draft a new release → new tag). Claude can draft the release notes.

Things are only "verified" once StillRez has run them on the module. Say so explicitly otherwise.
