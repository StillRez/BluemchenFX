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
- #8 UI tweaks: HOME page crowding and other improvements
