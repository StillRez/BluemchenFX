# BluemchenFX devlog

Running notes on what was built, why, and what broke along the way. Newest entries at the
bottom of each day. Hardware: kxmx Bluemchen (4HP, Daisy Seed, 64×32 OLED, encoder,
2 pots, 2 CV, MIDI TRS in, microSD, stereo audio).

---

## 2026-11-17 — Project start: clean firmware + reverb

**Goal:** a clean-slate effects firmware for the Bluemchen. It has proper hardware support, a
UI for setting parameters on the OLED with the encoder, and a first effect: a reverb with decay
time, dry/wet mix and filtering.

**Hardware layer (`src/hw/bluemchen.*`)**
- Pin map taken from [recursinging/kxmx_bluemchen](https://github.com/recursinging/kxmx_bluemchen)
  (`src/kxmx_bluemchen.cpp`): encoder D27/D26/D28, pots D16/D15, CV D21/D18, SSD1306 64×32 on
  I2C, MIDI on UART, SD on SDMMC1.
- The kxmx code targets a 2021-era libDaisy (`3e200cb`). Rewritten against current libDaisy
  (`1289a00`): named `seed::Dxx` pins, the current `Encoder` / `OledDisplay` / `AnalogControl`
  APIs.
- CV inputs initialised with `flip = true` like the original (inverting input stage) and
  mapped to −1…+1.

**Architecture**
- `fx::Effect` interface: each effect publishes a `ParamSpec` table (name, range,
  linear/exponential curve, unit). The UI pages, knob/CV routing, MIDI CC map and flash
  storage are all generated from that table. A new effect only has to implement the interface.
- Audio callback (32-sample blocks, 48 kHz): debounces the encoder, reads the ADCs, applies
  CV modulation and runs the effect. Encoder events are passed to the main loop as
  monotonically increasing counters, so the ISR is the only writer and there are no
  read-modify-write races.
- Main loop: UI events, knob takeover, MIDI, flash saves, display at ~30 fps.

**Reverb (`src/fx/reverb.*`)**
- DaisySP `ReverbSc` (Costello FDN, from DaisySP-LGPL). Its ~400 KB of delay memory is
  placed in SDRAM with `DSY_SDRAM_BSS`.
- **Decay is a real RT60 in seconds** (0.2–30 s). It's converted to the tank feedback via the
  mean loop delay of ReverbSc's 8 lines (3015.5 samples @ 48 kHz):
  `fb = exp(-6.908 · d / T60)`. Host-side measurement: 0.5 / 2 / 8 s targets measured
  0.53 / 1.82 / 6.71 s. Close enough; the Hi Cut damping shortens long tails a bit.
- Lo Cut: 12 dB/oct SVF high-pass on the reverb send (20 Hz–1 kHz).
- Hi Cut: ReverbSc's in-tank damping LPF (500 Hz–18 kHz).
- Mix: equal-power crossfade.
- Block-rate one-pole smoothing on all parameters.

**UI (`src/ui/ui.*`)**
- Pages: HOME (bar per parameter), one page per parameter, KNOB 1/2 and CV 1/2 routing,
  SAVE. Click toggles edit mode (header inverts). Encoder acceleration. Long press saves.
- Parameter pages show the live value large (Font_11x18, falls back to 7x10 if too wide),
  a bar for the effective value and a tick for the stored base value. Tags show which
  sources are routed to the parameter. Two 1-px I/O meters run up the right edge.
- Knobs use **move-to-take-over**, so power-up knob positions don't overwrite stored values.
  An encoder edit or MIDI CC on a knob's parameter disengages that knob until it's moved again.
- Moving a knob or receiving a CC briefly "peeks" that parameter's page.
- No float `printf` (newlib-nano): all value formatting is integer-based.

**Persistence:** `PersistentStorage<Settings>` in the last 4 KB sector of QSPI, with a
version word and range validation on load.

**Build:** compile-verified with arm-none-eabi-gcc 13.3.1 against current libDaisy/DaisySP.
116 KB flash (88 % of 128 KB). The remote file bridge refused to write a file named
`Makefile` into the project folder, so it shipped as `Makefile.in` and `setup.sh` copies it.

## 2026-11-18 — First flash on hardware ✅

Flashed `prebuilt/BluemchenFX.bin` with the Daisy Web Programmer (DFU in FS mode).
Reverb, UI, knobs and display all working on the module.

## 2026-11-18 — Mono / stereo input ✅

- New INPUT page: STEREO or MONO. In mono, the left input feeds both channels before any
  processing. Implemented by passing `in[0]` as the right-channel input pointer, so there's
  no copy.
- Stored in a previously-zero padding byte of `Settings` (0 = stereo), so existing saved
  settings stay valid with no version bump.
- "MONO" tag on the HOME header.
- Verified working on hardware.

## 2026-11-18 — SD card presets

- Presets are human-readable text files: `BLUEMCHN/PRESET01.TXT … PRESET99.TXT`.
  Each line is `key = value` in real units (`decay = 4.50`, `hi_cut = 7k`), with comments, CRLF
  line endings, case-insensitive keys and units after numbers ignored. Missing keys keep current
  values and unknown keys are ignored, so the format stays forward-compatible.
- `app/preset_text.*` is hardware-independent and was round-trip and hand-edit tested on the PC.
  `app/preset_store.*` does the FatFS I/O.
- UI: LOAD SD / SAVE SD pages (click → pick slot → click). Saving keeps an existing
  file's `name`. MIDI Program Change 0–98 loads slots 1–99.
- Five example presets in `sd-card/` (Small Room, Plate, Big Hall, Dark Cave, Mono Wash),
  generated by the real serializer.
- **Flash overflow:** FatFS pushed the image to 143 KB (109 %). The fix was building the
  project and libDaisy with `-Os` (libDaisy defaults to `-O3`), with `reverb.cpp` opting back
  into `-O2` via a pragma. Result: 111 KB (85 %). Project-only `-Os` still overflowed by 2 KB,
  which is why `setup.sh` now builds libDaisy with `OPT=-Os`.

## 2026-11-18 — SD bugs: card not detected, boot freeze ✅

**Symptoms**
1. No card at boot, card inserted later: SD pages always show NO CARD.
2. Card inserted at boot: stuck on the splash screen for minutes with no UI and no audio.
   It then continued with NO CARD.

**Causes**
- **libDaisy's FatFS glue latches the "initialised" flag before trying.**
  `disk_initialize()` sets `disk.is_initialized[pdrv] = 1` *before* calling the driver, and
  never clears it on failure. After one failed mount, FatFS never touches the card again until
  a reboot. That explains symptom 1, and the NO CARD after symptom 2.
- The card was mounted during boot, before audio and the UI started, so any slow SD failure
  froze the whole module.
- Found during review: libDaisy's SD driver DMA-reads sectors into FatFS's buffers and then
  invalidates the D-cache over that range rounded to 32-byte lines. `FATFS::win` (offset 56)
  and `FIL::buf` (offset 44) aren't line-aligned, so the edge cache lines could discard pending
  writes to neighbouring variables.

**Fixes**
- No card access at boot. Mounting happens on first use of an SD page (an `SD...` screen shows,
  and audio keeps running).
- Before every mount attempt: `f_mount(NULL)`, `SdmmcHandler::Reset()`, and clearing
  `disk.is_initialized[pdrv]`.
- Own `FATFS` / `FIL` objects wrapped in an `alignas(32)` struct whose padding puts the sector
  buffer exactly on a cache line, with `static_assert`s to keep it that way.
- On-screen diagnostics on SD failure: `F<fresult> E<HAL SD error bits> <bus mode>`.

Verified on hardware: hot insertion works, and loading/saving/overwriting presets works.

## 2026-11-18 — SD mount took ~3 minutes → 1-bit bus only

With the fixes above it worked, but the first SD access took about 3 minutes (16 GB card).
Card size wasn't the cause; a FAT32 mount reads only a few sectors.

The firmware tried **4-bit @ 25 MHz** first, then fell back to **1-bit @ 12.5 MHz**. On the
Bluemchen, 4-bit data transfers never complete. The STM32 HAL sets the hardware data timeout to
`SDMMC_DATATIMEOUT = 0xFFFFFFFF` SD clocks: 2³² / 25 MHz ≈ **172 s**, which matches the
~3 minutes. The 1-bit fallback then succeeded, and later accesses were fast because the working
mode was remembered.

The fix is to use only 1-bit @ 12.5 MHz. Presets are ~600 bytes, so speed is irrelevant.
The 4-bit entry is left documented in `preset_store.cpp` for anyone who wants to investigate
(it must go *after* the 1-bit entry). The cause of the 4-bit failure isn't known.
DAT1–DAT3 routing or pull-ups on the board are the first suspect.

*Pending: confirmation on hardware that the first mount is now fast.*

---

## Open items / ideas

- Confirm fast SD mount with the 1-bit-only build.
- Find out why 4-bit SD fails (scope DAT1–3, check pull-ups against the kxmx schematic).
- Flash is at ~86 %. For more effects, move to the Daisy bootloader (`APP_TYPE = BOOT_SRAM`).
  Settings live at the end of QSPI, clear of the bootloader's program area.
- More effects through the `fx::Effect` interface (delay, chorus, …), with an effect selector page.
- License: ReverbSc is LGPL (DaisySP-LGPL), which matters if binaries are distributed.
  Pick a license for this repo's own code.
