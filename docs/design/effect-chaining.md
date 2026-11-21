# Design: effect chaining

Status: **approved design**, phases 0–1 done and verified on hardware, phase 2 done and verified on hardware. Date: 2026-11-18. All open questions answered.

## Decisions already made (StillRez, 2026-11-18)

| Question | Answer |
|---|---|
| How many slots? | Up to **4**. The same effect may appear more than once (e.g. delay → delay). |
| Topology | **Serial now**, with **parallel later**. The engine must not paint us into a corner. |
| Long press | Means **back** (up one level). Saving moves to an explicit SAVE item. |
| Effects planned | Modulation (chorus/flanger/phaser/tremolo), filters/drive, pitch/granular, looper/sampler, wavefolders, dynamics (compressor, limiter), sample-rate reduction and bit reduction. |
| SDRAM split (Q1) | **8 MB per slot + 24 MB shared large pool.** |
| Type change and back (Q2) | **Recall previous settings** per slot and type. |
| Preset with fewer slots (Q3) | **Empty the slots it doesn't mention.** A preset is the whole chain. |
| CPU overload (Q4) | **Warn only.** |
| Bypass (Q5) | **Per-slot choice, default trails.** |

## Goals and non-goals

Goals:
- A chain of 1–4 stereo effect slots, each with its own type, parameters and bypass.
- No clicks when bypassing a slot or changing its type, and no glitches in the other slots
  while that happens.
- Settings, presets, knob/CV routing and MIDI all become chain-aware. Old single-effect
  presets and saved settings still load.
- Adding an effect type stays a matter of implementing `fx::Effect` plus one registry line.
- The CPU cost is visible, so heavy chains are a known quantity instead of a mystery dropout.

Non-goals for this round: parallel routing (designed for, not built), tap tempo / clock sync
(#6, separate), 4-bit SD (#2).

## 1. Engine

### 1.1 Slots

```cpp
struct Slot {
    EffectId    type;      // FX_NONE (0xFF) = empty
    bool        bypass;
    fx::Effect* fx;        // instance owned by this slot, nullptr when empty
    float       norm[kMaxParams];
    Fade        fade;      // bypass / swap crossfade state
};
Slot chain[kNumSlots];     // kNumSlots = 4
```

The audio callback runs the slots in order. Each slot reads the previous slot's output and
writes its own; an empty slot is a wire.

### 1.2 Per-slot effect instances

Today each effect is a single global object with file-static SDRAM buffers
(`g_reverbsc`, `g_line_l/r`). Repeats (two delays) need **one instance per slot**, so:

- **Effect objects** (the small control state: smoothers, filters, coefficients) live in a
  per-slot storage block in fast internal RAM, sized to the largest effect class
  (`static_assert` per type). The slot placement-news the chosen type into it.
- **Big buffers** (delay lines, reverb tanks, loop memory) come from a **per-slot SDRAM
  arena**: a bump allocator handed to the effect in `Init()`. Changing a slot's type resets
  its arena. There is no heap and no `free`.
- Each effect declares its SDRAM need as a constant (`kSdramBytes`), checked at compile time
  against the arena size.

Interface change:

```cpp
virtual void Init(float sample_rate, Arena& sdram) = 0;  // was Init(float)
```

Proposed SDRAM layout (64 MB total):

| Region | Size | Used for |
|---|---|---|
| Slot arenas 1–4 | 4 × 8 MB | normal effects (reverb ≈ 0.4 MB, delay ≈ 0.8 MB); up to ≈ 21 s of stereo float audio per slot |
| Large pool | 24 MB | **one** slot at a time may claim it: looper/sampler (≈ 65 s stereo at 48 kHz), long granular buffers |
| Free | 8 MB | SD buffers, future use |

An effect whose `kSdramBytes` exceeds a slot arena is marked "large". Picking it in a second
slot while the pool is taken shows "POOL BUSY" and is refused.

### 1.3 Bypass and type changes

Both run per slot. The other slots keep playing.

- **Bypass** is a per-slot setting, **trails by default**:
  - *Trails:* the effect's input fades to silence over ~10 ms while the dry signal passes; the
    effect keeps running so tails ring out and its wet output fades away naturally. Effects
    without tails (filters, drive, dynamics) behave like a hard bypass in this mode.
  - *Hard:* an equal-gain ~10 ms crossfade from the slot's output to its input; the effect
    stops running (saves CPU).
- **Type change** (from the main loop, never the ISR):
  1. Fade the slot to its input (it becomes a wire) and set its `swapping` flag.
  2. Wait one audio block. The ISR now skips the slot's `fx`.
  3. Destroy the old object, reset the arena, placement-new and `Init()` the new type, load
     its recalled params (see below), then `Reset()`.
  4. Clear `swapping` and fade back in.

  *As built (phase 1):* parameters reach an effect only while the settings' type for that slot
  matches the loaded effect. Otherwise the old effect, during its fade-out, would get the new
  effect's parameter values (the host tests caught this as a click: a delay's TIME jumping).

  This is the existing global `switching` mechanism scoped to one slot. Init of a large
  buffer happens in step 3 with audio still running through the other slots, so no dropouts.

- **Param recall:** each slot keeps the last `norm[8]` for every effect type it has hosted
  (`recall[slot][type]`, 32 bytes each, ≈ 2 KB for 16 types). Switching a slot back to a type
  restores its settings; a type the slot never had gets defaults. The recall table is RAM
  only (it doesn't survive a power cycle); the active settings of each slot are saved as
  usual. Loading a preset updates recall for the types it sets.

### 1.4 Signal flow and parallel later

Every slot gets its own 2 × 32-sample output buffer (1 KB total). Serial processing is "slot
*n* reads slot *n−1*'s buffer". Parallel then only means a different **topology table**:
which buffer(s) each slot reads, plus a mixer at the merge point. Examples later:
`1 → (2 ∥ 3) → 4`, or two parallel pairs. Settings v3 reserves a `topology` byte (0 =
serial) so adding it doesn't need a v4.

Per-effect MIX stays inside each effect, as today. A global OUT level is added on the setup
page because stacked effects can add up.

### 1.5 CPU budget

At 48 kHz with 32-sample blocks, the callback has **667 µs** per block.

- Add libDaisy's `CpuLoadMeter` around the callback: average and peak load.
- Show it as a thin vertical bar on the HOME screen and as numbers on the setup page
  (`CPU 43% PK 61%`).
- Measure each effect on hardware and record its cost in a table in this doc. This shows which
  chains fit.
- **Overload policy:** warn only. If peak load passes ~95%, the CPU bar inverts and the setup
  page shows `CPU!`. Nothing is refused or auto-bypassed; the user stays in control.
- **Cost table** (fill in from the CPU page as effects are measured on the module; the host
  column is µs per 32-sample block on a PC, from `make -C tests run`, only a relative hint):

  | Effect | Module avg / peak (single effect, defaults) | Host µs/block |
  |---|---|---|
  | (none: dry passthrough, controls, meters) | *pending* | – |
  | REVERB | **14 % / 17 %** (2026-11-18) | 2.3 |
  | DELAY | **5 % / 5 %** (2026-11-18) | 5.9 |
  | DELAY in slot 1 of the phase-1 chain engine | **7 % / 8 %** (2026-11-18) | – |
  | DELAY → REVERB → REVERB → REVERB (phase 2) | **≈ 55 % / 66 %** (2026-11-18) | – |

  The host column has them in the *opposite* order: on the STM32 the reverb's eight
  interpolated delay lines in SDRAM (cache misses) dominate, while on a PC everything sits in
  cache. **Host µs are not a guide to module cost**; always measure on the module. Each
  number includes the fixed overhead (controls, encoder, meters), so a dry baseline will make
  them slightly smaller. The chain engine itself adds about 2 %: per-slot buffer copies and the
  per-sample bypass/swap ramps, which run even when a slot is steady. The fast path (phase 3:
  skip the ramps once g = 1 and b = 0 and process straight into the next buffer; empty slots
  copy nothing) should win most of that back. *Module numbers after it: pending.* Headroom check: four reverbs in a chain ≈ 4 × 14 % ≈ 56 %, so the
  current effects fit comfortably in any 4-slot combination. Measured: D → R → R → R reads
  ≈ 55 % / 66 %, above the 5 + 3 × 14 ≈ 47 % sum; the difference is the chain engine overhead
  (≈ 2 %) plus SDRAM cache contention between three reverb tanks. Still a third of the budget
  free at peak.

- Cheap effects (filters, drive, wavefolder, bit/SR reduction, dynamics, tremolo/chorus)
  should cost only a few percent each. The ones to measure before promising anything are
  reverb (ReverbSc), pitch shifting and granular. The block size stays at 32 unless
  measurements show the per-block overhead matters.

## 2. Settings v3 (QSPI)

```cpp
struct SlotState {           // 36 bytes
    uint8_t type;            // EffectId or FX_NONE
    uint8_t flags;           // bit0 bypass, bit1 hard bypass (0 = trails, the default)
    uint8_t pad[2];
    float   norm[8];
};
struct Route {               // 2 bytes
    int8_t slot;             // -1 = none
    int8_t param;
};
struct SettingsV3 {
    uint32_t  version;       // 0xB10E0003
    uint8_t   input_mode;
    uint8_t   topology;      // 0 = serial (reserved for parallel)
    uint8_t   ui_slot;       // last selected slot
    uint8_t   pad;
    float     out_level;
    SlotState slot[4];
    Route     route[SRC_LAST];   // K1, K2, CV1, CV2 → (slot, param)
};
```

That is about 170 bytes, well inside the settings sector.

Migration from v2: slot 1 becomes the v2 active effect with its params. Its routes become
`(slot 0, param)`. Slots 2–4 are empty. v1 goes v1 → v2 → v3 through the existing path.

## 3. Presets (SD text)

The format gets per-slot sections. Old files remain valid:

```
# BluemchenFX preset
name   = Dub Space
input  = stereo
out    = 0 dB

[slot1]
effect = DELAY
time   = 0.37
feedbk = 55
mix    = 40
bypass = off

[slot2]
effect = REVERB
decay  = 4.0
mix    = 30

# routing: none | <slot>.<param>   e.g. 1.time, 2.decay
knob1  = 1.time
knob2  = 2.mix
cv1    = 1.feedbk
cv2    = none
```

- A file **without** `[slotN]` sections is an old single-effect preset. It loads into slot 1,
  clears slots 2–4, and its routes like `knob1 = decay` mean slot 1.
- A section without `effect =` keeps that slot's current type. A missing section empties the
  slot, so a preset always fully defines the chain and recalls exactly.
- Per slot: `bypass = on|off`, `bypass_mode = trails|hard` (default off / trails).
- *(Phase 3 implementation)* A slot that changes effect starts from the new effect's defaults
  before the file's values apply (as old single-effect files already did). `out` is in dB.
  The limiter is a global setting and stays out of presets. Routing may also name a bare
  parameter of the current section. SAVE SD always writes this format, even for a single
  effect, so a saved preset recalls bypass and the other slots exactly.
- Unknown keys are ignored, as today.

## 4. UI

The display is 64 × 32: four 8-pixel text rows of about 10 characters.

**HOME (chain overview)**
```
1 DELAY   ▌
2 REVERB  ▌   ← selected slot inverted
3 --      ▌   ← CPU bar on the right edge
4 --      ▌
```
Bypassed slots show their name in lowercase (or with a `/` marker). Empty slots show `--`.

Controls:
- **Turn** selects a slot, then **SETUP** below slot 4 (the list scrolls).
- **Click** enters the selected slot or the setup menu.
- **Long press on HOME** saves to flash. HOME has no "back", so the existing habit survives.

**Slot pages** (one page per item, as today):
`TYPE` → `BYPASS` (OFF / ON) → `BYP MODE` (TRAILS / HARD) → the effect's params. Turn moves between pages, click edits, long press
goes back to HOME. Changing `TYPE` on a populated slot asks for a confirmation click
(`DELAY→REV?`), since it replaces the params.

**SETUP pages:**
`K1 / K2 / CV1 / CV2` routes (the value list reads `1:TIME`, `2:DECAY`, …, `NONE`), `INPUT`,
`OUT`, `CPU`, `LOAD SD`, `SAVE SD`, `SAVE`. Long press goes back to HOME.

This replaces the crowded HOME screen from #8.

*As built (phase 2):*
- Long press on a slot or SETUP page goes straight to HOME, also from edit mode (one press,
  always the same result) instead of first leaving edit mode.
- Opening a slot shows its **first parameter** the first time (TYPE for an empty slot), then
  whatever page was open last in that slot. BYPASS / BYP MODE / TYPE are a few detents left.
- TYPE lists `EMPTY` first, so a slot can be cleared from the panel. The confirmation screen is
  `REPLACE?` with the new name, `old: <name>` and `push:ok turn:no`.
- The slot number is a small inverted box at the left of each slot page's header (and on
  peeks), so a knob peek shows which slot it touched.
- OUT: −24…+6 dB in 0.5 dB steps (fast turning: 1 dB), stored as linear gain; 0 dB is exactly
  1.0 so the audio callback can skip the multiply.
- The CPU bar sits at x = 59 on HOME, left of the I/O meters. `ui_slot` (in settings v3)
  remembers the HOME selection across power cycles.

## 5. Knobs, CV and MIDI

- Routes target `(slot, param)`. Knob takeover (0.03 threshold) works per target, unchanged.
- If a slot's type changes, routes pointing at it follow the **new effect's default routing**
  for that source (`Effect::DefaultRoute()`), or go to NONE if it has none. *(Changed during
  phase 1: plain NONE would have left the knobs dead after every effect switch, a regression
  from v2 where each effect had its own routing.)*
- MIDI CC: slot *n* (0–3), param *p* (0–7) → CC `20 + 8n + p`. Slot 1 keeps CC 20–27, so
  existing setups still work. That gives 20–51. CC 52–55 toggle bypass for slots 1–4
  (value ≥ 64 = bypassed). Program Change still loads `PRESETnn.TXT`.

## 6. Testing

A **host test harness** (`tests/`, built with the normal `g++`, run in CI next to the
firmware build):

- **DSP stability:** every effect at its parameter extremes (and random parameter sweeps), fed
  with impulses, noise and full-scale sines for ~30 s of audio. It asserts no NaN/Inf and that
  the output stays bounded. This would have caught the Svf feedback blow-up.
- **Presets:** parse → serialize → parse round-trips; old single-effect files load into
  slot 1; malformed lines are ignored.
- **Settings:** v1 → v3 and v2 → v3 migration produce the expected chain.
- **Chain render:** a 4-slot chain renders, and bypass/swap ramps have no discontinuity above
  a threshold.

It stubs `DSY_SDRAM_BSS` and the arena with plain memory, so the effect sources compile
unmodified.

## 7. Phased plan

Each phase is one or more commits and ends with a hardware check by StillRez.

| Phase | Work | Hardware check |
|---|---|---|
| 0 | CPU meter + host test harness in CI. No behaviour change. **Done, verified on hardware (2026-11-18).** | CPU % shown, reverb/delay costs recorded |
| 1 | Arena + per-slot instances, chain engine, settings v3 + migration. The UI temporarily edits slot 1 only (plus BYPASS / BYP MODE pages). **Done, verified on hardware (2026-11-18)** except MIDI CC (not tested yet). | Old settings come back in slot 1; no clicks on bypass/swap |
| 2 | Hierarchical UI: HOME overview, slot pages, setup menu, long press = back. **Done, verified on hardware (2026-11-18).** | Build a 2–4 slot chain from the panel; closes #8 |
| 3 | Chain presets + CC map, plus the chain engine's fast path for steady slots. **Done, compiled and host-tested (2026-11-20); hardware check pending.** | Old and new presets load; MIDI CCs reach slots 2–4 |
| 4 | New effects, cheapest first: filter/drive, wavefolder, bit/SR reduction, tremolo/chorus, compressor/limiter; then pitch/granular; then the looper (large pool) | Per-effect checks + CPU table |

Related open issues: #7 (feedback saturation, can be done inside phase 4), #6 (tap tempo,
independent), #2 (4-bit SD, independent).

## 8. Open questions

None. Q1–Q5 were answered on 2026-11-18 (see the table at the top).
