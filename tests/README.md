# Host tests

Tests that run on the PC, no module needed:

```
make -C tests run            # build and run everything (~15 s)
make -C tests run ARGS=delay # only tests whose name contains "delay"
```

They compile the firmware's hardware-independent code (`src/fx/*`, `src/app/preset_text.cpp`)
and DaisySP / DaisySP-LGPL with the native `g++`. `stubs/` stands in for the libDaisy headers
the effects include. CI runs them on every push, and the firmware build waits for them.

| Test | What it checks |
|---|---|
| `effects_have_sane_param_tables` | names fit the display, ranges valid, `ToNorm(ToReal(x)) == x` |
| `effects_stable_at_extremes_and_random_settings` | defaults, all-min, all-max and 12 random settings per effect, each fed an impulse, full-scale noise, sine and square for 2 s then silence. Output must stay finite, below 32, and **decay after the input stops** (a self-oscillating loop fails even if a clipper keeps it bounded) |
| `effects_stable_under_param_modulation` | every parameter jumps each block for 20 s under noise |
| `effects_reset_clears_tails` | after `Reset()`, silence in gives silence out |
| `effects_host_cost_report` | not pass/fail: µs per 32-sample block on the PC, as a relative cost hint. Real numbers come from the CPU page on the module |
| `presets_example_files_parse_cleanly` | every `sd-card/BLUEMCHN/PRESETnn.TXT` parses with no errors |
| `presets_round_trip` | random states → text → state match within the text's resolution, and re-serializing gives identical text |
| `presets_without_effect_line_and_unknown_effect` | `effect =` detection |
| `presets_garbage_is_ignored` | junk lines are counted as errors and change nothing; empty/truncated input is safe |

**Adding an effect:** add it to `g_effects[]` at the top of `test_effects.cpp`.
