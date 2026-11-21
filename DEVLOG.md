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

*Confirmed on hardware later the same day (see below).*

---

## 2026-11-18 — Project infrastructure

- Code is on GitHub (github.com/StillRez/BluemchenFX), with libDaisy / DaisySP / DaisySP-LGPL
  pinned as submodules. A fresh `git clone --recursive` + `setup.sh` + `make` reproduced
  `prebuilt/BluemchenFX.bin` byte for byte.
- `CLAUDE.md` holds the hardware facts, build quirks, conventions and workflow, so a new
  session starts with the context instead of rediscovering it.
- GitHub Actions (`.github/workflows/build.yml`) builds every push with the same xPack GCC
  13.3.1, reports flash usage, warns if `prebuilt/` is stale, uploads the `.bin` as an artifact,
  and publishes a GitHub Release with the `.bin` for `v*` tags.
- Open items moved to GitHub Issues (#1–#5).
- Releases: CI attaches `BluemchenFX.bin` to any release published in the GitHub UI (or to
  pushed `v*` tags). Claude's sessions can't create tags or releases, so StillRez publishes them.
  First one planned: `v0.3.0` (reverb + mono/stereo + SD text presets, 1-bit SD), as a
  pre-release until #1 is confirmed.

## 2026-11-18 — v0.3.0 released; fast SD mount confirmed ✅

- StillRez published `v0.3.0` in the GitHub UI. The release workflow built and attached
  `BluemchenFX.bin` automatically, so the release path works end to end.
- Flashed on the module: with the 1-bit-only SD build, the first SD access is near-instant
  (was ~3 minutes). Closes #1.
- Everything in v0.3.0 is now verified on hardware: reverb, UI, knobs/CV, mono/stereo, SD
  load/save/overwrite, hot insertion, fast mount.

## 2026-11-18 — Local build on StillRez's PC

- StillRez cloned the repo to `C:\Coding Projects\BluemchenFX`, ran `setup.sh` and `make`
  successfully with the Daisy Toolchain.
- His compiler is GCC 10.2.1 (10-2020-q4) vs 13.3.1 in CI, so his bin isn't byte-identical:
  113,908 bytes (86.9 % flash) vs 112,244. Same source, and the memory layout is correct
  (SD sector buffers still cache-line aligned). His local build hasn't been flashed yet.
- Git for Windows had checked text files out as CRLF, which can break `setup.sh`. Added
  `.gitattributes` to keep `*.sh` / `Makefile` / workflows LF, and the example presets byte-exact CRLF.

## 2026-11-18 — Delay effect + effect selector (#4)

**Delay (`src/fx/delay.*`)**
- Stereo, 10 ms–2 s. Two `DelayLine<float, 96256>` in SDRAM (~770 KB).
- Params: TIME (exp), FEEDBK 0–95 %, MIX (equal power), PING 0–100 %, LO CUT 20 Hz–1 kHz,
  HI CUT 500 Hz–18 kHz.
- Filters sit in the feedback loop and on the wet output, so each repeat gets thinner/darker.
- PING crossfeeds the feedback and moves the input into the left line. At 100 %, echoes
  alternate L/R even from a mono source.
- TIME glides per sample (~50 ms one-pole), so it bends pitch like tape instead of clicking.
- Writes are soft-clipped (`2·SoftClip(x/2)`).
- **Bug caught in host testing:** with `daisysp::Svf` as the loop filters, 95 % feedback plus a
  full-scale noise burst diverged to ±1e33 and then NaN. Svf has a built-in cubic "drive" term
  that is unstable at the levels a saturating loop reaches. Switching to `daisysp::OnePole`
  (zero-delay, unconditionally stable) fixed it: max output 1.93 over 20 s, no NaN.
- Host tests: first echo at exactly 250.0 ms for a 250 ms setting, echo decay follows
  feedback, ping-pong alternates L→R→L→R, and a 0.1 s→1.0 s time sweep stays bounded.

**Effect selector**
- EFFECT page after HOME: click, turn, click to switch. Switching sets a `switching` flag (the audio
  callback passes dry), waits 2 ms, resets the incoming effect's buffers and smoothing, swaps
  settings, and clears the flag. That avoids clicks and stale echoes/tails.
- **Settings v2** (`0xB10E0002`, 168 B): active effect index plus an `EffectState` (8 norms +
  4 routes) per effect slot, room for 4 effects. Each effect keeps its own knob/CV routing:
  reverb K1/K2/CV1/CV2 = Decay/Mix/Decay/Hi Cut, delay = Time/Mix/Time/Feedback.
- **Migration:** if the flash sector holds v0.3.0's v1 struct (state word FACTORY/USER +
  version `0xB10E0001`), it's converted on first boot. The reverb keeps its settings and input
  mode, and the delay gets its defaults. The v1 struct was checked against v0.3.0's
  `settings.h` for size and every field offset, on the host and on the ARM ABI with `-fshort-enums`.
- Presets: `effect = …` now selects the effect (case-insensitive on `Name()`). Loading another
  effect's preset switches to it. A file without an effect line applies to the active effect;
  an unknown effect shows `BAD FX`. SD slot pages show which effect a used slot holds.
  The existing reverb presets (01–05) parse unchanged. Added delay examples 06–09
  (Slapback, Ping Pong, Dub Echo, Tape Wash).
- MIDI CC 20+n maps to parameter n of the active effect. Program Change can switch effect via the
  preset.
- HOME bars: labels shrink to 2 characters when there are more than 4 params (delay has 6).

**Flash:** 119,972 B = 91.5 % (was 85.6 %). The delay plus the selector cost ~7.7 KB.

*Status: compiled (no warnings), DSP and presets host-tested.*

**On hardware (later the same day):** flashed and running. **Switching between REVERB and DELAY
works** ✅. Still to confirm: the delay's detailed behaviour (TIME glide, high FEEDBK, PING),
per-effect values/routing surviving switches, migration of the saved v0.3.0 settings, delay
presets 06–09, and the 6-bar HOME page.

The first attempt flashed stale firmware: after `git pull`, `make program-dfu` only flashed the
old `build/` contents (libDaisy's program targets don't rebuild). The 113,908-byte size in
dfu-util's output gave it away. The project Makefile now makes `program-dfu` / `program` /
`program-boot` depend on `all`. dfu-util's trailing `get_status` error / make `Error 74` is
harmless (the device resets because of `:leave`) and is now documented.

## 2026-11-18 — Delay + effect selector verified on hardware ✅ (closes #4)

StillRez's test results:
- v0.3.0 → v2 settings migration: reverb values, routing and input mode carried over ✅
- Switching REVERB ↔ DELAY; each effect keeps its own parameters ✅
- TIME changes don't click; echoes already in the buffer pitch-shift as the time changes, which is
  the expected tape-style behaviour ✅
- High FEEDBK doesn't run away ✅, but it doesn't really saturate either. It stays fairly clean.
  Sound tuning is deferred to #7.
- Delay presets switch effect, and SD slot pages show the effect type ✅
- HOME page is readable but crowded with 6 bars. StillRez has more UI tweaks in mind (#8).
- PING / ping-pong works as expected ✅ (confirmed right after)

Ready to release as **v0.4.0**.

## 2026-11-18 — License: MIT (closes #3)

- StillRez wants the project fully open source and chose **MIT** (over GPL-3.0), matching
  kxmx_bluemchen, libDaisy and DaisySP. `LICENSE` © 2026 StillRez.
- README has a License section crediting libDaisy (MIT), DaisySP (MIT), DaisySP-LGPL (LGPL-2.1,
  `ReverbSc`) and kxmx_bluemchen (MIT, the pin map basis, also credited in `src/hw/bluemchen.h`).
- LGPL note: ReverbSc is statically linked, so the released `.bin` contains LGPL-2.1 code. That's
  compatible with MIT. The relinking requirement is met by the public source, pinned submodules
  and build scripts. Removing the reverb gives an LGPL-free build.

## 2026-11-18 — Daisy bootloader: run from SRAM (#5)

StillRez plans more complex effects and effect chaining, and internal flash was at 92 %.

**Change:** `APP_TYPE = BOOT_SRAM` with the Daisy bootloader v6.4 (`intdfu-2000ms`, the one libDaisy
ships and StillRez already has). The app is stored in QSPI at `0x90040000` and copied to AXI SRAM at boot.
- Code: **131 KB of 480 KB SRAM (27 %)** at `-O2`, so the size tricks went away: project `OPT`
  back to `-O2`, and the `#pragma GCC optimize("O2")` lines removed from the DSP files.
  libDaisy stays `-Os` (HAL code, not performance-critical, no rebuild needed).
- Variables + stack: DTCM, 57 KB of 128 KB. Effect buffers: SDRAM as before.
- Legacy build still possible: `make APP_TYPE=BOOT_NONE OPT=-Os` → 119,308 B (91 %).

**Bug avoided:** in BOOT_SRAM builds `.data`/`.bss` move from AXI SRAM to **DTCM**, which SDMMC1's
DMA can't access. The first BOOT_SRAM link put the SD buffers (`s_fs`, `s_file`, `s_buf`) at
`0x2000xxxx`, which would have broken SD presets on the module. They're now in SDRAM
(`DSY_SDRAM_BSS`, `0xC011xxxx`, still 32-byte aligned). libDaisy's SD driver has no scratch
buffer, so all DMA goes straight into these. libDaisy's own DMA buffers (audio, ADC, UART)
already use `DMA_BUFFER_MEM_SECTION` (D2 SRAM).

**Checked against libDaisy / the bootloader docs:**
- QSPI is still initialised for SRAM apps (only skipped for apps running from QSPI), so saving
  settings to QSPI keeps working. Settings at `0x907FF000` are far from the app area.
- Bootloader limits: ≤ 480 KB for SRAM apps (it reserves the last 32 KB of SRAM).
- During its 2.5 s grace period the bootloader scans the **SD card root** for `.bin` files and
  flashes the first valid one. Presets in `BLUEMCHN/` are ignored.
- `make program-boot` installs the bootloader. The project's "rebuild before flashing" rule now
  applies to `program-dfu` / `program` only.

**Flashing workflow change:** install the bootloader once (system DFU), then flash the app
during the bootloader's grace period (tap RESET, then tap BOOT). Every power-up now waits ~2 s
in the bootloader before BluemchenFX starts.

*Status: compiled; memory layout checked with `nm` / `objdump`. Not yet run on hardware. To
confirm on the module: bootloader install, app flash via the bootloader, settings survive, SD
presets load/save (the DMA-in-SDRAM change), audio/UI normal. Also check **boot time with an SD
card inserted**: the bootloader scans the card with its own driver, and this board's 4-bit SD
trouble could slow that scan.*

## 2026-11-18 — Bootloader build doesn't start on hardware ❌ (investigating)

**Symptom:** after `make program-boot` + `make program-dfu`, the Seed LED pulses briefly and then
stays **solid on**. The OLED stays blank and there's no audio. Same with or without an SD card
(so the bootloader's SD scan isn't the cause). We don't know yet whether it eventually starts.

**Read through the bootloader source (electro-smith/DaisyBootloader) and libDaisy:**
- The bootloader's SD scan uses 1-bit @ 12.5 MHz, the same mode that works on this board.
- Its LED is solid only while a DFU download is in progress, and that flag is set only by
  a QSPI erase from a real download.
- Hand-over: it copies the full 480 KB program area from QSPI to SRAM, writes JUMP + address to
  backup SRAM, resets the chip, then sets VTOR/MSP and jumps. The reset turns the LED off, and our
  firmware never turns it on.
- libDaisy (v6.x bootloader detected) still initialises QSPI, SDRAM and the LED for SRAM apps.
  libDaisy's fault handlers just stop (`bkpt`), so a crash also freezes the LED.
- OLED (blocking I2C), ADC/UART/audio (libDaisy DMA sections): no DTCM-DMA problem found.

**Next step: diagnostics on the module.**
- `tools/bootcheck/`: a minimal BOOT_SRAM program (3 quick LED blinks, then "BOOT OK" + counter
  on the OLED). If it runs, the bootloader and hand-over are fine and the problem is ours.
- `make DIAG=1 BUILD_DIR=build-diag`: the firmware blinks 3× right after `seed.Init`, keeps the
  LED on until the OLED is up, then shows each startup step (`1 HW OK` … `6 AUDIO ON`).
  The last step shown locates the hang.

**Result (same evening):**
- The web programmer refused `bootcheck.bin` with "incompatible binary, try entering the Daisy
  bootloader". The module was in **STM system DFU**, not the Daisy bootloader: BOOT was pressed
  during RESET instead of during the bootloader's pulse.
- With the right sequence (release RESET, wait for the pulse, then tap BOOT), `bootcheck` ran: LED
  blinking, "BOOT OK" + counter ✅. So the bootloader and hand-over work.
- `BluemchenFX-diag.bin` booted through every step to the normal UI ✅. The firmware is fine
  from SRAM.
- **Conclusion:** the first v0.5.0 attempt most likely never got the app into QSPI (flashed in the
  wrong mode), so the bootloader had nothing valid to start. No firmware bug. README/CLAUDE.md now
  spell out the button sequence and the "incompatible binary" / solid-LED symptoms.

## 2026-11-18 — Bootloader build verified on hardware ✅ (closes #5)

Normal build flashed through the Daisy bootloader. StillRez's checklist all passed:
- Saved settings carried over (both effects, routing, input mode) ✅
- Saving (long press) persists across a power cycle ✅
- SD presets load and save, with the FatFS/DMA buffers now in SDRAM ✅
- Power-up with an SD card inserted: no noticeable delay from the bootloader's SD scan ✅
- Audio and UI as before ✅

Room for growth: code at 27 % of 480 KB SRAM, DTCM at 45 %, SDRAM effectively unlimited.
Ready to release as **v0.5.0**: the first release that requires the Daisy bootloader.

---

## 2026-11-18 — Design pass: effect chaining

Docs only. **No code changed, nothing compiled or verified on hardware.**

- StillRez's decisions: up to 4 serial slots, repeats allowed, parallel routing later, long press
  = back, and the planned effect families (modulation, filters/drive, pitch/granular,
  looper/sampler, wavefolders, dynamics, SR/bit reduction).
- Wrote `docs/design/effect-chaining.md`, covering:
  - the engine: per-slot instances, SDRAM arenas plus a shared large pool for the looper, and
    per-slot bypass/swap crossfades;
  - a topology byte reserved for parallel routing;
  - settings v3 with v2 migration;
  - a chain preset format that still reads old files into slot 1;
  - the hierarchical UI (folds in #8), the CC map 20 + 8·slot + param, and a CPU meter;
  - a host test harness and a phased plan.
- Open questions Q1–Q5 (arena sizes, param recall, partial presets, CPU overload policy,
  bypass default) need StillRez's answers before phase 1. Phase 0 (CPU meter + host tests) doesn't
  depend on them.


## 2026-11-18 — Effect chaining: design questions answered

Docs only. **No code changed.**

StillRez's answers, now in `docs/design/effect-chaining.md`:
- **SDRAM:** 8 MB per slot (≈ 21 s stereo) plus a 24 MB large pool (≈ 65 s looper), one slot
  at a time.
- **Recall:** switching a slot back to an effect type restores that slot's previous settings
  for it. RAM only; it doesn't survive a power cycle.
- **Presets:** a preset is the whole chain. Slots it doesn't mention are emptied.
- **CPU overload:** warn only (CPU bar inverts, `CPU!`). Nothing is refused or auto-bypassed.
- **Bypass:** per-slot TRAILS / HARD, default TRAILS.

The design is approved. Next: phase 0 (CPU meter + host test harness).


## 2026-11-18 — Phase 0: CPU meter + host test harness

The first step of the effect-chaining plan (`docs/design/effect-chaining.md`). No change to the
sound.

**CPU meter (firmware)**
- libDaisy `CpuLoadMeter` wraps the whole audio callback (controls, encoder, effect, meters).
- New **CPU** page after INPUT shows:
  - the average load (large);
  - `pk` = peak since the last reset. Click resets it.
  - The header inverts when the peak reaches 95 %.
- The peak resets once 2 s after boot, so start-up blocks don't count.
- The numbers go into a cost table in the design doc. They decide how many heavy effects fit in a
  chain.

**Host tests (`tests/`, `make -C tests run`, ~15 s)**
- Effects and preset code are built with the PC's g++ against DaisySP / DaisySP-LGPL, with stubs
  standing in for libDaisy.
- Every effect is tested at defaults, all-min, all-max and random settings. Each setting gets an
  impulse, full-scale noise, sine and square, then silence. The output must stay finite and
  bounded, and its **tail must decay**.
  - The first version (with a plain "peak < 8" bound) flagged the reverb at a 30 s decay
    reaching 8.5 with 2 s of full-scale square in. That's energy piling up in a long tank, not
    instability. On the module it would just clip at the DAC.
  - The decay rule distinguishes the two. Sanity check: a delay deliberately allowed 130 %
    feedback fails it, even though the soft clipper keeps it bounded.
- Other tests: parameters jumping every block for 20 s; `Reset()` must give silence; parameter
  tables are sane.
- Presets: all 9 example files parse with no errors. Random states round-trip through text to
  the text's resolution, and re-serializing gives identical text. Junk lines are ignored safely.
- CI has a new `host-tests` job. The firmware build (and so any release) now waits for it to
  pass.
- Host cost hint, µs per 32-sample block on a PC: reverb 2.3, delay 5.9. The delay's per-sample
  time glide makes it the more expensive of the two.

*Status: host tests pass locally (9 tests, 6304 checks). Firmware compiles: SRAM 131,916 B
(26.8 %), DTCM 43.6 %. `prebuilt/` updated. **Not yet run on hardware.** To check on the module:*
- *the CPU page appears after INPUT and shows plausible numbers;*
- *click resets the peak;*
- *record the average/peak for REVERB and DELAY at their default settings;*
- *everything else behaves as before.*


## 2026-11-18 — Phase 0 verified on hardware ✅

StillRez flashed the Phase 0 build. The CPU page works, and the CI `host-tests` job is green.
Measured on the module, each effect alone, with audio playing:

| Effect | Average | Peak |
|---|---|---|
| REVERB | 14 % | 17 % |
| DELAY | 5 % | 5 % |

On the PC the order is reversed (delay 5.9 µs/block vs reverb 2.3). On the STM32 the reverb's
eight interpolated SDRAM delay lines cost far more (cache misses), so host timings are no
guide to module cost. The design doc's cost table now says so.

What this means for chaining: even four reverbs (≈ 56 %) fit in a chain, so the current
effects can't overload the module. Next: phase 1 (chain engine, per-slot instances and SDRAM
arenas, settings v3 with migration).


## 2026-11-18 — Phase 1: chain engine + settings v3

Audio now runs through `app::Chain`, an engine with 4 serial slots. To keep this phase testable
on its own, the UI, MIDI and presets still work on **slot 1** only. Slots 2–4 exist in the
engine and the settings but stay empty until the chain UI (phase 2).

**Engine (`src/app/chain.*`)**
- Each slot has its own effect instance: the object sits in a 512-byte storage block in DTCM,
  and the big buffers in that slot's **8 MB SDRAM arena** (`fx::Arena`, a bump allocator).
- The effects lost their file-static SDRAM globals. `Init(sr, Arena&)` now allocates, so two
  delays in one chain no longer share a buffer.
- Each slot has two ramps of about 10 ms: `g` (run gain: dry wire ↔ effect) and `b` (trails: the
  effect's input fades out while the dry signal fades in).
  - **Hard bypass** ramps `g` to 0. The effect then stops running and gets `Reset()` once, so it
    doesn't come back with stale echoes.
  - **Trails bypass** ramps `b` to 1, so tails ring out over the dry signal.
- **Effect swaps** fade the slot to a wire. `Poll()` (main loop, non-blocking) then destroys the
  old effect, builds and initialises the new one in the same storage and arena, and fades it
  in. The other slots keep playing throughout.
- The old global `switching` / dry-switch path is gone.
- **Bug caught by the new tests:** during a swap, the old effect's fade-out was being fed the
  *new* effect's parameter values. A fading delay got the reverb's numbers, its TIME jumped,
  and it clicked. Parameters now only reach an effect while the settings' type matches the
  loaded effect. A mutation check confirms the test fails without the fix.

**Settings v3 (`0xB10E0003`, 164 bytes)**
- Contents: 4 slots (type, bypass/hard flags, 8 params), routes as (slot, param) per source,
  input mode, a topology byte (0 = serial, reserved), and out gain.
- **Migration:** v2 (0.4–0.5) and v1 (≤ 0.3) are read raw from the flash sector before
  `storage.Init()`. The active effect lands in slot 1 with its params and routing. v2's
  *other* effect seeds recall, so switching slot 1 to it brings your old values back.
- **Recall** (RAM only): each slot remembers the last params per effect type.
- **Routes on a type change:** sources that pointed at the slot follow the new effect's
  `DefaultRoute()` (reverb K1/K2/CV1/CV2 = Decay/Mix/Decay/Hi Cut, delay = Time/Mix/Time/
  Feedback). This changes the design doc's "→ NONE": that would have left the knobs dead after
  every effect switch, a regression from v2.
- `fx/registry.*`: `EffectId` (persisted), metadata prototypes, `Construct()` / `Destroy()`.

**UI:** new **BYPASS** (OFF/ON) and **BYP MODE** (TRAILS/HARD) pages after EFFECT. HOME shows
`BYP` when bypassed.

**Presets:** still the single-effect text format. Loading one puts it into slot 1 and empties
slots 2–4 (a preset is the whole chain). A file for another effect now starts from that
effect's defaults rather than its last v2 values.

**Host tests:** 17 new, 26 in total, all pass. They cover settings layout, validation,
migration, recall and routes, plus the engine:
- an empty chain is bit-exact;
- slot output = effect output;
- delay → reverb → delay in the chain = the three effects run by hand;
- bypass and swaps are click-free;
- hard bypass is exact and clears tails; trails keep ringing and then die away;
- swaps don't interrupt the other slots;
- an effect too big for its arena is handled;
- 30 s of random changes stay stable.

**Memory:** SRAM 136.7 KB (27.8 %), DTCM 61.9 KB (47 %), SDRAM 32 MB of arenas at `0xC0000000`.

*Status: compiled (CI toolchain); host tests pass; `prebuilt/` updated. **Not yet run on
hardware.** To check on the module:*
1. *Your saved setup comes back after flashing: same effect, values, routing, input mode.*
2. *Switching REVERB ↔ DELAY on the EFFECT page doesn't click. Switching back restores what
   you'd dialled in.*
3. *BYPASS ON/OFF doesn't click in either mode. TRAILS lets the tail ring out; HARD cuts it,
   and un-bypassing doesn't bring back old echoes.*
4. *Knobs follow the effect after a switch (K1 = Decay on reverb, Time on delay).*
5. *SD presets load/save. MIDI CC still controls the parameters.*
6. *Save (long press) + power cycle keeps everything, including bypass.*
7. *CPU page: about the same as before (reverb ~14 %, delay ~5 %).*


## 2026-11-18 — Phase 1 verified on hardware ✅ (MIDI CC pending)

StillRez flashed the Phase 1 build. Everything on the checklist passed:
- settings migrated into slot 1;
- REVERB ↔ DELAY switches are click-free and recall the previous values;
- bypass in both modes, no clicks, and HARD leaves no stale echoes;
- knobs follow the effect after a switch;
- SD presets load and save;
- save + power cycle.

**Not tested yet: MIDI CC.** StillRez wants to check the module's MIDI input hardware first.
Still to confirm.

**CPU:** DELAY in slot 1 now reads **7 % average / 8 % peak** (Phase 0: 5 % / 5 %). The
roughly 2 % comes from the chain engine itself: per-slot buffer copies for all 4 slots (empty
ones too) and the per-sample bypass/swap ramps, which run even when a slot is steady. A fast
path for steady slots (no ramps, process straight into the next buffer) should win most of
it back. Noted in the design doc; worth doing before heavy effects arrive.


## 2026-11-18 — Phase 2: chain UI (HOME / slot pages / SETUP)

The UI now edits the whole chain. Before this phase only slot 1 was reachable from the panel.
It also replaces the crowded 6-bar HOME page from #8.

**Menus (`src/ui/ui.*`, rewritten)**
- **HOME:** one row per slot (`1 DELAY`; lowercase = bypassed; `--` = empty), then SETUP
  below slot 4. The list scrolls, and small arrows show there's more. Turn selects, click
  opens, hold saves to flash. A thin CPU bar sits left of the I/O meters and inverts at ≥ 95 %
  peak. The selected row is stored in the settings' `ui_slot` byte, so it survives power cycles.
- **Slot pages:** TYPE → BYPASS → BYP MODE → parameters, with the slot number in a small box at
  the top left.
  - An empty slot only has TYPE.
  - Opening a slot shows its first parameter the first time, then the page used last.
  - TYPE offers EMPTY / REVERB / DELAY. Replacing an existing effect needs a second click
    (`REPLACE?`, `old: REVERB`); turning or holding backs out.
- **SETUP:** KNOB 1/2 and CV 1/2 can now target any parameter of any slot (`2:FEEDBK`,
  `NONE`). Then INPUT, a new **OUT** page (−24…+6 dB in 0.5 dB steps, stored as the existing
  `out_gain`, exactly 1.0 at 0 dB), CPU (`CPU!` at ≥ 95 %), LOAD SD, SAVE SD, SAVE.
- **Long press** = back to HOME from anywhere (also cancels an edit); on HOME it saves.
- Knob and MIDI peeks now work for any slot and show the slot number.

**main.cpp:** type changes come from the UI as (slot, type) and go through
`app::SetSlotType()` as before. Slot 1 may now be empty: the boot-time "slot 1 must hold an
effect" fix-up is gone. MIDI CCs and SD presets still cover slot 1 only; they are phase 3.
SAVE SD with an empty slot 1 shows `BAD FX`.

**UI host tests (new).** The UI now draws through libDaisy's `OneBitGraphicsDisplay`
interface instead of the concrete OLED class. That lets `tests/test_ui.cpp` run the real
`ui.cpp` against a 64×32 framebuffer that records every string it draws.
- 11 tests drive it with turns, clicks and holds: building a 4-slot chain (the same effect
  twice, then emptying a slot), the replace confirmation, back/save, edits landing in the open
  slot, routes across slots, OUT steps, peeks, a type changing under an open page, and
  remembered pages.
- `ui_every_screen_fits_the_display` renders every page of every level (editing, confirming,
  peeking with 4 source tags) and fails if any text runs past x = 59. It caught a bug that
  dates back to v0.3: the SD pages' `push:pick` / `push:load` hint ran 2 px into the input
  meter. Fixed by moving that column 2 px left.
- `tools/ui_shots.py` turns the tests' screens into a PNG contact sheet:
  `UI_SHOTS=/tmp/s make -C tests run ARGS=ui_`, then `python3 tools/ui_shots.py /tmp/s sheet.png`.

**Build:** host tests 37/37 pass (7891 checks). Firmware compiles with no warnings (xPack GCC
13.3.1, the CI toolchain): SRAM 136,336 B (27.7 %), DTCM 61,976 B (47.3 %), essentially
unchanged. `prebuilt/` updated.

*Status: compiled and host-tested. **Not yet run on hardware.** To check on the module:*
1. *Power-up: HOME shows `1 REVERB` (or your saved effect), `2 --`, `3 --`, `4 --`. Your
   saved settings and routing are unchanged.*
2. *Build a 2–4 slot chain from the panel (e.g. 1 DELAY → 2 REVERB, or two delays). Audio
   follows each change without clicks, and the other slots keep playing.*
3. *Replace an effect: the `REPLACE?` step appears; turning or holding backs out. Setting a
   slot to EMPTY removes the effect.*
4. *Bypass a slot: its HOME row turns lowercase.*
5. *SETUP → KNOB 1: route it to a parameter in slot 2. Moving the knob peeks the page with a
   `2` badge.*
6. *OUT: −3 dB is audibly quieter, 0 dB back to normal.*
7. *Hold on a slot page goes back to HOME; hold on HOME saves. After a power cycle the chain,
   the routing and the HOME selection come back.*
8. *Check readability: HOME rows, the slot badge, the 4×6 hints.*
9. *CPU page with 2–4 slots filled: please note the average/peak for the cost table.*


## 2026-11-18 — Phase 2 verified on hardware ✅ (closes #8); small text in capitals

StillRez's checklist: all passed.
- The saved setup came back in slot 1.
- He built a 2–4 slot chain from the panel with no clicks.
- `REPLACE?` and EMPTY both work.
- A knob routed to slot 2 peeks with a `2` badge.
- OUT at −3 dB is quieter.
- Hold goes back on slot pages and saves on HOME.
- The chain and routing survive a power cycle.

**CPU:** DELAY → REVERB → REVERB → REVERB reads **≈ 55 % average / 66 % peak**. That's above the
sum of the single-effect numbers (5 + 3 × 14 ≈ 47 %). The difference is the chain engine's
overhead plus three reverb tanks competing for the SDRAM cache. It's still a third of the
budget free at peak. Added to the cost table in the design doc.

**Small text → capitals.** StillRez found the lowercase hint line (4×6 font) hard to read.
All 4×6 text now goes through `DrawSmall()`, which uppercases it (hints, `OLD: DELAY`, SD
status, preset names on the SD pages). The layout test now also fails on lowercase 4×6 text.
Bypassed slots on HOME stay lowercase (6×8 font, readable). *Compiled and host-tested
(37/37); the capitals are not yet seen on the module.*

**Audio peaking about a second after power-up: investigated, not a start-up bug.**
StillRez heard peaking about a second after the effects loaded (D → R → R → R). I reproduced
the chain on the host from a cold start with steady input, and measured output peak and RMS
per 100 ms:

| Chain (defaults) | Sustained 220 Hz sine, peak 0.5 | Noise, RMS 0.25 |
|---|---|---|
| D | peak 0.49 | RMS 0.22 |
| R | peak **1.02** | – |
| R → R | peak **1.82** | – |
| R → R → R | peak **3.14** | RMS 0.21 |
| D → R → R → R | peak **2.72** | RMS 0.19 |

- Each ReverbSc roughly **doubles the peak on tonal, sustained input**: the tank's resonances
  pile up on a steady pitch. Broadband material stays level.
- In series this compounds to about +15 dB. The tanks need a few hundred ms to fill, which
  matches the "about a second".
- The output then goes past full scale and clips at the codec. It happens whenever a held note
  starts into such a chain, not only at power-up.
- Ruled out: uninitialised SDRAM (ReverbSc and DelayLine both zero their buffers on Init), the
  CV/ADC start-up (2 ms slew), and parameter smoothing (~10 ms).

Fix options are a sound-design call for StillRez. Until then, the OUT page (−6 dB or so)
avoids the clipping.


## 2026-11-18 — Always-on output limiter (#10)

StillRez chose option 1 for the stacked-reverb peaking: a soft limiter at the output that is
always on, with its parameters in SETUP.

**DSP (`src/app/limiter.*`, after OUT in the audio callback)**
- A stereo-linked peak envelope rises instantly and falls with the release time. It sets a
  gain that brings the peaks down to a threshold 1 dB under the ceiling. The gain falls over
  ~0.5 ms (attack), so it doesn't jump inside a waveform cycle.
- A soft clip (tanh shape) between the threshold and the ceiling catches what the attack lets
  through. The output never goes past the ceiling: in float, a huge overshoot lands exactly on
  it.
- Below the threshold the signal is bit-exact (gain exactly 1, soft clip off).
- No look-ahead, so no added latency. Cost per sample is two `fabsf`, a compare and a
  multiply, plus `tanhf` only on overs.

**UI:** two new SETUP pages after OUT.
- **LIMIT**: ceiling −12…0 dB in 0.5 dB steps, default −1 dB. The bottom line shows the live
  gain reduction (`GR -3.4 DB`), and an `ON` tag appears while it's limiting. I first had the
  header invert while limiting, but dropped that: inverted means "editing" everywhere else.
- **LIM REL**: release 20 ms…1 s, ~10 % per detent, default 150 ms.

**Settings v4** (`0xB10E0004`, 172 bytes): v3 plus `limit_db` and `limit_rel` appended.
`MigrateV3()` copies the v3 prefix and sets the limiter defaults. `static_assert`s pin the
prefix, and `ReadLegacySettings()` handles v3 as well as v2 and v1. The limiter is a global
setting; chain presets (phase 3) won't change it unless we decide otherwise.

**Host tests:** 6 new, 43 in total, all pass.
- Transparent below the threshold (bit-exact).
- Never past the ceiling: random bursts up to +24 dB, 4 ceilings × 3 releases.
- A +12 dB tone settles at −2.3…−1 dBFS with ~14 dB of gain reduction, and recovers fully
  within 0.5 s of quiet.
- **The DELAY → 3 × REVERB held note: +8.7 dBFS raw → −1.5 dBFS limited.**
- v3 → v4 migration keeps the chain, routes, input, OUT and HOME selection.
- LIMIT / LIM REL pages, including the `ON` tag and their ranges.

**Build:** compiles with no warnings. SRAM 138,612 B (28.2 %), DTCM 47.3 %. `prebuilt/` updated.

*Status: compiled and host-tested. **Not yet run on hardware.** To check on the module:*
1. *After flashing, your chain, routing and OUT come back (v3 → v4 migration).*
2. *The D → R → R → R chain with a held note: no more harsh clipping. SETUP → LIMIT shows GR
   and `ON` while it works.*
3. *Normal-level playing sounds the same as before.*
4. *Try LIMIT at −6 dB and LIM REL at 20 ms vs 1 s to hear the range.*
5. *CPU page: about the same as before (≈ 55 % / 66 % for D → R → R → R).*


## 2026-11-19 — Output limiter verified on hardware ✅ (closes #10)

StillRez flashed the limiter build. All checks passed:
- Saved chain, routing and OUT came back (v3 → v4 settings migration) ✅
- D → R → R → R with a held note: no more harsh clipping; LIMIT shows the gain reduction and
  the `ON` tag ✅
- Normal-level material sounds unchanged ✅
- LIMIT / LIM REL ranges behave as expected ✅
- CPU about the same as before ✅

The capital-letter small text (from the phase 2 follow-up) is in this build too and was in use
throughout the test.

Everything since v0.5.0 is now verified on hardware: CPU meter + host tests (phase 0), chain
engine + settings v3 (phase 1, except MIDI CC, still pending a check of the MIDI input),
chain UI (phase 2), limiter + settings v4. Ready to release as **v0.6.0**.

## 2026-11-19 — Effect algorithm documentation

New `docs/design/effect-algorithms.md`: how REVERB and DELAY work, traced through the pinned
DaisySP / DaisySP-LGPL code, and where each piece comes from (Costello 1999 Csound instrument →
Varga's Csound opcode → Soundpipe → DaisySP-LGPL for ReverbSc; stmlib for OnePole and SoftClip;
Simper's SVF; the rest is BluemchenFX's own code). Covers the waveguide-junction feedback matrix,
the per-line random delay modulation, the RT60 → feedback mapping and why long decays measure
short, the delay's in-loop filtering, saturation, ping-pong matrix and TIME glide. CLAUDE.md
points to it.

Found while reading ReverbSc: the DaisySP port spaces its eight lines by their size in bytes
while indexing a float array, so the tank reserves ~396 KB but uses ~99 KB at 48 kHz. It's
harmless at 48 kHz, so nothing changed.

Docs only: no firmware change, `prebuilt/` unchanged, nothing to verify on hardware.

## 2026-11-20 — MIDI CC verified on hardware ✅

StillRez tested MIDI input on the v0.6.0 build, the last unchecked item from phase 1.
- The module's MIDI input needed its jumpers set to **TRS Type A**. Until then nothing arrived.
- With TRS-A set, MIDI works: CC 20+n reaches the parameters of slot 1's effect, and Program
  Change loads SD presets ✅

No firmware change. Everything through v0.6.0 is now verified on hardware. Next: phase 3
(chain presets + a CC map covering all four slots).

## 2026-11-20 — Phase 3: chain presets, full MIDI CC map, chain fast path

SD presets and MIDI now cover the whole chain. Before this, both only reached slot 1.

**Chain presets (`app/preset_text.*`)**
- SAVE SD writes the whole chain: `[slot1]`..`[slot4]` sections with `effect`, `bypass`,
  `bypass_mode` and the parameters, plus global `name`, `input`, `out` (dB) and routing as
  `<slot>.<param>` (`knob1 = 1.time`). Empty slots are written as `effect = none`.
- Loading follows the design doc: a slot without a section is emptied, a section without an
  `effect` line keeps the slot's type, a slot that changes effect starts from its defaults,
  missing keys keep current values, unknown keys are ignored. An unknown effect name anywhere
  shows `BAD FX` and changes nothing.
- Files without sections are the old single-effect format and load exactly as before (into
  slot 1, slots 2–4 emptied). All nine shipped examples still load.
- The limiter stays out of presets (it's a global SETUP setting).
- SD slot pages show what a preset holds: `REVERB`, `DEL>REV` for two effects, `D>R>R>R` for
  three or four.
- Saving no longer needs an effect in slot 1 (it used to show `BAD FX` when slot 1 was empty).
- The SD read/write buffer grew from 2 KB to 4 KB (SDRAM). A 4 × DELAY chain file is ~1.6 KB.
- Three new example chains, generated by the serializer: 10 Dub Space (DELAY → REVERB),
  11 Room Echo (REVERB → DELAY, ping-pong), 12 Infinite Wash (DELAY → REVERB → REVERB, the
  third bypassed in TRAILS mode, OUT −3 dB).
- `PresetStore` now takes the whole `Settings` (+ recall) instead of one effect's state.

**MIDI CC map (`app::MapMidiCc()` / `ApplyMidiCc()`)**
- CC 20 + 8·(slot−1) + parameter: slot 1 = CC 20–27 (unchanged), slot 2 = 28–35,
  slot 3 = 36–43, slot 4 = 44–51. CCs for empty slots or missing parameters are ignored.
- CC 52–55 bypass slots 1–4: 64–127 = bypassed, 0–63 = on. The slot's bypass mode
  (TRAILS/HARD) is kept, and the usual ramps make it click-free.
- Parameter CCs peek the page with the slot badge, and disengage a knob routed to that
  parameter, as before. Program Change still loads PRESET01–99 (now whole chains).

**Chain fast path (`Chain::ProcessSlot()`)**
- A wire (empty or hard-bypassed and faded-out) slot no longer copies its buffers: the next
  slot reads the same buffer.
- A steady, fully-on, unbypassed slot runs its effect straight from one buffer to the next,
  skipping the per-sample ramp loops and the scratch copies.
- Both give exactly the same output as before: the existing exact-equality chain tests
  (empty chain = wire, slot = effect, delay → reverb → delay = by hand) and the click tests pass
  unchanged. This is aimed at the ~2 % chain overhead measured in phase 1.

**Host tests:** 8 new, 51 in total, all pass.
- Random 4-slot chains round-trip through text (types, bypass flags, parameters to the text's
  resolution, routes, input, OUT), and re-serializing gives identical text. A mutation check
  (bypass always written as `off`) makes the round-trip test fail.
- All shipped presets load; chain examples are byte-identical to what the serializer writes.
- Section rules, unknown effects, corrupted files (random bytes flipped), summaries, and every
  CC 0–127 against a mixed chain.
- The UI layout test now includes an SD page showing a 4-slot chain summary.

*Status: host-tested; the firmware compiles in CI (xPack GCC 13.3.1, no new warnings):
SRAM 142,708 B (29.0 %), DTCM 62,024 B (47.3 %). This session had no ARM toolchain and couldn't
download CI artifacts, so **`prebuilt/BluemchenFX.bin` is still the v0.6.0 build** (CI flags it
as stale). Build locally (`make program-dfu`) or use the CI artifact / a release. **Not yet run
on hardware.** To check on the module:*
1. *Your saved setup comes back unchanged after flashing (no settings change in this phase).*
2. *Old presets 01–09 load as before: into slot 1, slots 2–4 emptied.*
3. *Copy the new `PRESET10`–`12.TXT` to the card. LOAD SD shows `DEL>REV`, `REV>DEL` and
   `D>R>R` for them, and loading builds those chains (12: slot 3 shows lowercase = bypassed).*
4. *Build a 3–4 slot chain with some bypass and routing, SAVE SD it, change everything, LOAD SD
   it back: it all returns. Optional: open the file on a PC and check it reads sensibly.*
5. *MIDI: CC 28 (slot 2 param 1) and CC 44 (slot 4 param 1) move those slots' parameters
   and peek with the right badge. Slot 1 CC 20–27 as before.*
6. *MIDI: CC 53 at 127 bypasses slot 2 (row turns lowercase), at 0 un-bypasses it, no clicks.*
7. *Program Change 9 loads the Dub Space chain.*
8. *CPU page: D → R → R → R (was ≈ 55 % / 66 %) and DELAY alone in slot 1 (was 7 % / 8 %).
   Both should be a little lower now.*

## 2026-11-21 — Phase 3 on hardware: chain presets ✅, CPU lower ✅ (MIDI part pending)

StillRez flashed the phase 3 build:
- Saving and loading chain presets on the SD card works with chained effects ✅
- The CPU page reads lower than before the fast path ✅ (exact numbers not recorded yet)

Still to confirm on the module: MIDI CCs for slots 2–4 (CC 28–51), bypass CCs 52–55, Program
Change loading a chain preset, and old presets 01–09 still loading into slot 1.

## 2026-11-21 — Three new effects: CRUSH, CHORUS, RESON

StillRez wants more effect types before finishing the phase 3 MIDI checks. Added a bit crusher,
a chorus and a resonator. Details and sources: `docs/design/effect-algorithms.md`.

**CRUSH** (`src/fx/crush.*`): BITS 1–16 (continuous), RATE 100 Hz–48 kHz (free-running
sample & hold, any rate), DRIVE 0–+24 dB into a hard clamp, HI CUT (one-pole, off at the top of
its range), MIX (default 100 %). No buffers.

**CHORUS** (`src/fx/chorus.*`): RATE 0.05–8 Hz, DEPTH (swing up to ±95 % of DELAY), DELAY
1–30 ms, FEEDBK 0–90 % (flanger), SPREAD (L/R LFO offset 0–180°, wide from mono), MIX. Hermite
reads, near-sine LFO from a shaped triangle, two 4096-sample lines (32 KB) in the slot arena.
*Caught in testing:* soft-clipping every write (as the delay does) coloured even the clean wet
signal (full scale came out at 0.93). Only the feedback is saturated now.

**RESON** (`src/fx/reson.*`): modal resonator after Mutable Instruments Rings: 16 band-pass modes
per channel. PITCH 30 Hz–2 kHz, STRUCT (Rings' stiffness curve: harmonic at 25–30 %, bells
above), BRIGHT (upper partials' decay), DECAY 0.05–10 s (RT60 of the fundamental), MIX.
- Own code rather than DaisySP's `Resonator`, which recomputes all mode coefficients every
  sample. Here they're computed once per block (ZDF SVFs, stable under modulation).
- Each mode's gain is `1/√Q`, so broadband input comes out at about the same level at any
  DECAY (−0.5 … +0.9 dB from 0.2 to 8 s). A held note right on a partial still rings up, into a
  soft clip at ±2 and then the limiter.
- The right bank flips odd partials: mono input comes out wide (L/R correlation 0.05).
- The object is ~600 bytes, so `kEffectStorageBytes` went from 512 to 1024 (4 KB of DTCM for
  the four slots).

**Supporting changes**
- New parameter units: `Millis`, `Bits`, and Hz below 10 shown with two decimals (`0.50Hz`), in
  the UI and in preset files. Existing presets are written exactly as before.
- Each effect has a 2-letter `Code()` (RV, DL, CR, CH, RS): with five effects, first letters
  collide (CRUSH/CHORUS, REVERB/RESON). SD summaries are now `DEL>REV` (2 effects), `DL>RV>CH`
  (3) and `DLRVCHRS` (4); before this, 3–4 effects were `D>R>R`.
- `EffectId` appended: CRUSH = 2, CHORUS = 3, RESON = 4 (persisted ids unchanged). No settings
  layout change; recall memory grows to 5 types (640 B).
- Example presets 13 Lo-Fi Chorus (CRUSH → CHORUS), 14 Struck Bells (RESON → REVERB),
  15 Jet Flanger (CHORUS as a flanger).

**Host tests:** 64, all pass (13 new).
- The three effects joined the stability suite: extremes, random settings, modulation, Reset.
- New `tests/test_effect_behaviour.cpp` checks what they do:
  - CRUSH: output on the BITS grid using every level; one new sample per RATE tick; 16-bit /
    48 kHz within half a 16-bit step of the input; DRIVE and HI CUT.
  - CHORUS: tap at DELAY to the sample (1–30 ms); DEPTH swing; SPREAD 0 vs 100 %; feedback
    repeat ratio.
  - RESON: rings at PITCH (55 Hz–1 kHz) and on its 3rd harmonic; fundamental RT60 = DECAY within
    15 %; noise level; on-pitch peak; stereo width.
  - Mutation checks: the SPREAD test fails if SPREAD is ignored, and the pitch test fails with
    the resonator 3 % out of tune.
- New UI test renders every page of every effect at min/max/default: all fit, checked on a
  contact sheet too. Effect names/codes are tested unique.

*Status: host-tested. Firmware compiled by CI (this session has no ARM toolchain);
`prebuilt/` not updated. **Not yet run on hardware.** To check on the module:*
1. *TYPE lists CRUSH, CHORUS, RESON; each loads into a slot without clicks and sounds right.*
2. *CRUSH: BITS down to 1–4 gets gritty and quiet parts drop out; RATE down gives aliasing;
   HI CUT softens it.*
3. *CHORUS: default is a gentle stereo chorus; short DELAY + FEEDBK flanges (try preset 15);
   SPREAD 0 = mono, 100 = widest.*
4. *RESON: feed it drums or plucks. PITCH tunes it; STRUCT 27 % is string-like, higher gets
   bell-like; DECAY sets the ring; BRIGHT darkens. Knob 1 / CV 1 on PITCH.*
5. *Presets 13–15 load and show `CRU>CHO`, `RES>REV`, `CHORUS`.*
6. *CPU page, each effect alone in slot 1, and RESON × 4 if you like (fills the cost table).*

---

## Open items

Tracked as GitHub Issues:
- ~~#1 Confirm the SD card mounts quickly with the 1-bit-only build~~ (done)
- #2 Investigate why 4-bit SD transfers fail on the Bluemchen
- ~~#3 Add a LICENSE~~ (done: MIT)
- ~~#4 Effect selector + second effect (delay)~~ (done, v0.4.0)
- ~~#5 Flash headroom: move to the Daisy bootloader~~ (done, v0.5.0)
- #6 Delay: tap tempo and/or MIDI clock sync
- #7 Delay sound tuning: feedback saturation
- ~~#8 UI tweaks: HOME page crowding~~ (done: phase 2 chain HOME, small text in capitals)
- ~~#10 Output limiter for stacked-effect peaking~~ (done)
- ~~MIDI CC re-test since the chain engine~~ (done: works with the input jumpered for TRS-A)
- ~~Chain engine overhead (~2 % CPU): fast path for steady slots~~ (done in phase 3; lower CPU
  confirmed on the module, numbers pending)
- *(no issue yet)* Phase 3 MIDI checks on the module: CCs for slots 2–4 (28–51), bypass CCs
  52–55, Program Change loading a chain. StillRez will come back to these.
- *(no issue yet)* Module CPU numbers for CRUSH, CHORUS, RESON (cost table).
- #9 Tune each effect's default settings (`ParamSpec::def`) by ear, so loading an effect into an
  empty slot sounds good straight away. StillRez will do this on the module.
