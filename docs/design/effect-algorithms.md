# Effect algorithms: REVERB and DELAY

How the two effects in the current build work, down to the library code they run on, and where
each piece comes from. Read against the pinned submodules: DaisySP `2c72eaf`, DaisySP-LGPL
`c89d380`. Numbers assume the module's 48 kHz sample rate and 32-sample blocks.

Both effects are verified on hardware (see DEVLOG, 2026-11-18). This document describes the code
and changes no behaviour.

**Shared plumbing (both effects)**

- **Parameter mapping.** Each parameter is stored normalised (0–1) and converted by
  `ParamSpec::ToReal()` (`src/fx/effect.h`). Linear curves map `min + (max − min)·n`.
  Exponential curves map `min · (max/min)^n`, so equal knob travel gives equal *ratios*
  (octaves of frequency, doublings of time).
- **Smoothing.** Every block, each parameter's working value moves toward its target with
  DaisySP's `fonepole(out, in, 0.08)`, i.e. `out += 0.08 · (in − out)`. At 32-sample blocks that
  is a one-pole lag with a time constant of about **8 ms**, enough to stop zipper noise when a
  knob or CV moves. The delay's TIME is the exception (per-sample glide, below).
- **Dry/wet mix (equal power).** `dry = cos(mix · π/2)`, `wet = sin(mix · π/2)`. Since
  `dry² + wet² = 1`, uncorrelated dry and wet signals keep a constant loudness across the sweep.
  At 50 % both gains are 0.707 (−3 dB). The dry path is never filtered or delayed.

---

## REVERB (`src/fx/reverb.*`)

### Signal path

```
in L ─► Lo Cut (12 dB/oct HP) ─┐                         ┌─► wet L ─┐
                               ├─► ReverbSc 8-line FDN ──┤          ├─► equal-power mix ─► out
in R ─► Lo Cut (12 dB/oct HP) ─┘   (Hi Cut = in-loop LP) └─► wet R ─┘
                                                          dry L/R ──┘
```

| Param | Range | Curve | Default | What it drives |
|---|---|---|---|---|
| DECAY | 0.2–30 s | exp | ≈ 1.9 s | ReverbSc feedback, via an RT60 conversion |
| MIX | 0–100 % | lin | 35 % | equal-power dry/wet |
| LO CUT | 20 Hz–1 kHz | exp | ≈ 36 Hz | SVF high-pass on the send |
| HI CUT | 500 Hz–18 kHz | exp | ≈ 6.1 kHz | ReverbSc's in-loop damping low-pass |

Default knob/CV routing: K1 = DECAY, K2 = MIX, CV1 = DECAY, CV2 = HI CUT.

### The tank: ReverbSc, Costello's 8-line FDN

ReverbSc is a **feedback delay network** (FDN) of eight delay lines whose outputs are mixed back
into each other's inputs. Its feedback matrix comes from a physical model: a **scattering
junction of eight lossless waveguides of equal impedance** (J. O. Smith's waveguide-network
approach to reverb). Per sample (`reverbsc.cpp`, `ReverbSc::Process`):

1. **Junction pressure.** Sum the eight lines' (already damped) outputs and scale by 2/N = 0.25
   (`kJpScale`). That's the pressure at a junction of N = 8 equal waveguides.
2. **Inject the input.** Add the left input to that pressure for the even lines (0, 2, 4, 6), the
   right input for the odd lines (1, 3, 5, 7).
3. **Scatter.** Each line is written with `junction − its own returning wave`. As a matrix, the
   feedback is `A = (2/N)·11ᵀ − I`, a Householder reflection. It's orthogonal (lossless), so it
   only spreads energy between lines and never adds or removes it. It's also cheap: one sum
   instead of an 8×8 multiply. Every line feeds every other line, so left and right cross-couple
   and the echo density builds up quickly.
4. **Read with modulation.** Each line is read through a 4-point cubic (Lagrange) interpolator
   at a read position that drifts slowly (next section).
5. **Loss and damping.** The read value is multiplied by the feedback gain `fb`, then passed
   through a one-pole low-pass: `y = (1 − d)·x + d·y₋₁`. The coefficient is
   `b = 2 − cos(2π·fc/fs)`, `d = b − √(b² − 1)`, which puts the −3 dB point at `fc`. This filter
   sits *inside* every loop, so high frequencies lose a little more energy on every pass and
   bright content dies away faster than dark: the "air absorption" of a real room.
6. **Output.** Even lines are summed to the left output, odd lines to the right, times 0.35
   (`kOutputGain`). There's no output tap matrix and no pre-delay.

**Delay lengths and modulation.** The eight base lengths are all prime sample counts at
48 kHz: 2473, 2767, 3217, 3557, 3907, 4127, 2143 and 1933 samples (40–86 ms, mean 3015.5).
Prime, unequal lengths share no common factors, so the lines' resonant modes don't line up. On top of that, each line's
length wanders randomly by ±0.6 to ±1.7 ms at a 0.9–4 Hz rate. A 16-bit linear-congruential
generator (`seed · 15625 + 1`, with a per-line seed) picks a new target length every
`fs / rate` samples. The read pointer then moves toward it in a straight line, by adjusting a
fixed-point (28-bit fraction) read increment. That's a small, uncorrelated pitch wobble on every
line. It smears the fixed modal resonances that make plain FDNs and comb reverbs sound metallic,
and makes the tail sound denser than eight lines should. The modulation depth (`i_pitch_mod_`)
is fixed at 1 ("normal") and isn't exposed.

These per-line lengths, deviations and rates are the numbers from Sean Costello's original
1999 Csound instrument (his `randi` lines `.001, 3.1`, `.0011, 3.5`, `.0017, 1.11`, …).

### What BluemchenFX adds around it

**DECAY as a real RT60.** ReverbSc's own control is a raw loop gain from 0 to 1 (Costello's
rule of thumb: 0.6 small room, 0.8 small hall, 0.9 large hall). BluemchenFX converts a decay
time in seconds instead. A signal circulating with loop delay τ is multiplied by `fb` every τ
seconds, so to fall 60 dB (a factor of 1000) in T60 seconds:

```
fb^(T60/τ) = 10^-3   ⇒   fb = exp(−ln(1000) · τ / T60) = exp(−6.9078 · τ / T60)
```

with τ = the tank's mean loop delay, 3015.5 samples = 62.8 ms (`DecayToFeedback()` in
`reverb.cpp`). For example, 0.2 s → fb 0.114, 1.9 s → 0.797, 8 s → 0.947, 30 s → 0.986. `fb` is
capped at 0.999, which would be about 7 minutes, well past the 30 s range top.

It's an approximation. All eight lines share one `fb`, so the short lines (40 ms) lose their
60 dB a bit faster than the long ones (86 ms). The in-loop Hi Cut also takes extra energy out of
the upper spectrum. Host measurements (DEVLOG 2026-11-17): 0.5 / 2 / 8 s targets measured
0.53 / 1.82 / 6.71 s, so long settings run short, mainly because of the damping. Exact per-line
RT60 (Jot's method: `g_i = exp(−6.9078·L_i / T60)` per line) would need a change inside ReverbSc,
which only has one feedback value.

**LO CUT on the send.** A DaisySP `Svf` high-pass on each channel, before the tank, so the
reverb never builds up low-frequency mud while the dry signal keeps its bass. With resonance 0,
the SVF's damping is 2 (Q = 0.5): a gentle 12 dB/oct slope that's about −6 dB at the set
frequency, no peak. Its cubic drive term is multiplied by resonance, so it's 0 here (and the SVF
sits outside any feedback loop anyway; see the CLAUDE.md note on Svf). The SVF is Andrew
Simper's double-sampled, stable state-variable filter (from musicdsp.org, per the DaisySP
header): it runs the Chamberlin SVF twice per sample and averages, which keeps it stable up to
high cutoffs.

**HI CUT** is passed straight through as ReverbSc's damping frequency (`SetLpFreq`). Because
the filter is in the loop, it shapes the decay (how fast highs die) rather than acting as a
plain EQ on the output.

### Memory and cost

The `ReverbSc` object is ~396 KB (`aux_[98936]` floats) and comes from the slot's SDRAM arena.
At 48 kHz the eight lines need only ~24.7 k samples (~99 KB). The rest is because the DaisySP
port advances each line's start offset by its size in *bytes* while indexing a *float* array.
The lines end up spread 4× further apart than needed, but still within the buffer at 48 kHz.
It's harmless, but it's why the reverb reserves ~4× the memory it uses, and partly why it causes
more SDRAM cache misses than the delay.

CPU on the module: **14 % average / 17 % peak** per instance (DEVLOG, 2026-11-18). Eight
interpolated reads from SDRAM per sample dominate.

---

## DELAY (`src/fx/delay.*`)

### Signal path (per channel)

```
in ─► [ping-pong input routing] ─► (+) ─► soft clip ─► delay line ─┬─► Lo Cut HP ─► Hi Cut LP ─┬─► wet
                                    ▲                              │      (6 dB/oct each)      │
                                    └── × FEEDBK ◄─ [ping-pong cross-feed] ◄───────────────────┘
```

| Param | Range | Curve | Default | What it drives |
|---|---|---|---|---|
| TIME | 10 ms–2 s | exp | ≈ 0.37 s | delay length, with per-sample glide |
| FEEDBK | 0–95 % | lin | ≈ 40 % | loop gain |
| MIX | 0–100 % | lin | 35 % | equal-power dry/wet |
| PING | 0–100 % | lin | 0 % | blend from independent L/R to full ping-pong |
| LO CUT | 20 Hz–1 kHz | exp | ≈ 60 Hz | one-pole HP, in the loop and on the wet out |
| HI CUT | 500 Hz–18 kHz | exp | ≈ 6.1 kHz | one-pole LP, in the loop and on the wet out |

Default knob/CV routing: K1 = TIME, K2 = MIX, CV1 = TIME, CV2 = FEEDBK.

This is BluemchenFX's own design (MIT, this repo): a classic tape-style feedback delay built from
DaisySP primitives. The per-sample loop in `Delay::Process`:

1. **Glide the time.** `delay += (target − delay) / (0.05 · fs)`, so the length follows TIME
   with a ~50 ms exponential lag.
2. **Read** each line at that (fractional) length, with DaisySP `DelayLine`'s **linear
   interpolation** between the two neighbouring samples.
3. **Filter** the read: one-pole high-pass (LO CUT), then one-pole low-pass (HI CUT). This
   filtered signal is both the **wet output** and the **feedback** source.
4. **Ping-pong mix** (with `p` = PING, `s = 1 − p`):
   - feedback: `fb_L = s·wet_L + p·wet_R`, `fb_R = s·wet_R + p·wet_L`;
   - input: `send_L = s·in_L + p·(in_L + in_R)/2`, `send_R = s·in_R`.
5. **Write** `Saturate(send + FEEDBK · fb)` into each line.
6. **Output** `dry·in + wet·wet_out` per channel (equal-power gains).

### How each part behaves

**Filters in the loop = darker, thinner repeats.** Each pass goes through both filters again, so
the *n*-th repeat has been filtered *n* times. The rolloff steepens by 6 dB/oct per repeat on each
side, which is the way tape and bucket-brigade echoes lose top and bottom with each repeat. The
first echo is filtered once too, because the wet output is taken after the filters.

The filters are DaisySP `OnePole`, a port of Émilie Gillet's stmlib one-pole (Mutable
Instruments). It's a **zero-delay-feedback / topology-preserving-transform** one-pole (the form
described in Vadim Zavalishin's *The Art of VA Filter Design*): `g = tan(π·fc/fs)`,
`lp = (g·x + s)/(1 + g)`, `s = g·(x − lp) + lp`, with `hp = x − lp`. The cutoff is exact (no
warping), the filter is unconditionally stable for any cutoff, and it has no drive term. That's
why it replaced `Svf` in this loop: Svf's cubic `band³` term blew up at 95 % feedback in the host
tests.

**Saturation keeps runaway feedback musical.** `Saturate(x) = 2 · SoftClip(x / 2)`. `SoftClip`
(from stmlib, via DaisySP) is the rational tanh-like curve `x·(27 + x²) / (27 + 9x²)` for
|x| ≤ 3, hard ±1 beyond. Scaled ×2 on both axes, the write path is effectively linear for normal
levels (gain 1 at 0, still about 0.99 at ±0.3), bends progressively above that, and can never
exceed ±2. So at high FEEDBK, or with hot input stacking on top of the repeats, the loop
compresses and warms up instead of clipping or growing without bound. Issue #7 tracks tuning
how this saturation sounds.

**Stability.** FEEDBK tops out at 95 %, the filters' passband gain is ≤ 1, and the ping-pong
matrix `[[s, p], [p, s]]` has eigenvalues 1 and `s − p` (both |·| ≤ 1). So the linear loop gain
never exceeds FEEDBK, and the saturator bounds whatever is left.

**PING.** At 0 % the two channels are independent stereo delays. At 100 %, the input is summed
to mono and fed only into the left line. The feedback crosses over completely, so a single hit
gives L at T, R at 2T, L at 3T, and so on: alternating echoes even from a mono source. Values in
between blend the two: some of each repeat stays on its side and some crosses.

**TIME glide = tape-style pitch bend.** Changing the length while reading moves the read head
relative to the audio at a rate other than 1 sample per sample, which shifts pitch. That's
exactly a tape echo's varispeed behaviour (pitch drops while lengthening, rises while
shortening), instead of a click. Slow knob or CV moves give gentle bends. A *large instant* jump
(for example a preset load or a fast encoder spin) sweeps the 50 ms glide hard: a 0.37 → 0.74 s
jump initially moves the read head ~7× faster than real time, which is the classic "zip". Linear
interpolation is cheap but slightly dulls the highs while the length is fractional and moving.
Neither matters at a steady TIME setting.

### Memory and cost

Two `DelayLine<float, 96256>` (2 s at 48 kHz + 256 samples headroom), ~385 KB each (~770 KB),
from the slot's SDRAM arena. CPU on the module: **5 % / 5 %** standalone, **7 % / 8 %** in
slot 1 of the chain engine (DEVLOG, 2026-11-18).

---

## Sources and licences

| Piece | Origin | Licence in this build |
|---|---|---|
| `ReverbSc` (tank) | Sean Costello's 8-line FDN/waveguide reverb, a Csound orchestra instrument from October 1999. C opcode `reverbsc` by Istvan Varga for Csound (2005). Ported to Paul Batchelor's Soundpipe (`sp_revsc`), then to C++ in Electrosmith's **DaisySP-LGPL** (`Source/Effects/reverbsc.*`, © 2023 Electrosmith, Costello, Varga, Batchelor). Theory: J. O. Smith, "A New Approach to Digital Reverberation Using Closed Waveguide Networks", ICMC 1985 (cited in the Csound source). | **LGPL-2.1** (see README → License) |
| `Svf` (reverb LO CUT) | Andrew Simper's double-sampled stable SVF (musicdsp.org), in DaisySP `Filters/svf.*` | MIT |
| `OnePole` (delay filters) | Émilie Gillet, `pichenettes/stmlib/dsp/filter.h`, ported to DaisySP `Filters/onepole.h` | MIT |
| `SoftClip` / `SoftLimit` | stmlib, via DaisySP `Utility/dsp.h` | MIT |
| `DelayLine` | DaisySP `Utility/delayline.h` (Electrosmith) | MIT |
| `fonepole` smoothing | DaisySP `Utility/dsp.h` | MIT |
| Effect wrappers: RT60 mapping, send filtering, equal-power mix, delay loop, ping-pong, saturation staging, time glide | BluemchenFX (`src/fx/reverb.*`, `src/fx/delay.*`) | MIT |

Further reading:
- Csound manual, `reverbsc`: https://csound.com/manual/opcodes/reverbsc/
- Costello's original 1999 post of the instrument to the Csound list:
  https://ml.csound.1bpm.net/mailinglist.csound/thread/78692
- Paul Batchelor, *Bigverb* (a literate walkthrough of the same topology):
  https://paulbatchelor.github.io/sndkit/bigverb
- DaisySP-LGPL: https://github.com/electro-smith/DaisySP-LGPL ·
  DaisySP: https://github.com/electro-smith/DaisySP
