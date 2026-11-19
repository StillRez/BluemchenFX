# Host tests

Tests that run on the PC, no module needed:

```
make -C tests run            # build and run everything (~15 s)
make -C tests run ARGS=chain # only tests whose name contains "chain"
make -C tests run ARGS=delay # only tests whose name contains "delay"
```

They compile the firmware's hardware-independent code (`src/fx/*`, `src/app/*` except the SD
store, `src/ui/ui.cpp`) and DaisySP / DaisySP-LGPL with the native `g++`, plus libDaisy's display
headers and fonts for the UI tests. `stubs/` stands in for the libDaisy headers
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
| `chain_registry_matches_test_list` | every registered effect is also in these tests' lists |
| `settings_layout_is_stable` | the persisted v3 layout (size, offsets) hasn't changed by accident |
| `settings_defaults`, `settings_validation_rejects_bad_data` | factory settings are valid; corrupt flash data is refused |
| `settings_migrate_v2`, `settings_migrate_v1` | old saved settings land in slot 1 (v2's other effect seeds recall) |
| `settings_slot_type_recall_and_routes` | switching a slot's effect recalls its last settings per type; routes follow the new effect's defaults |
| `settings_single_effect_preset` | old-format presets load into slot 1 and empty slots 2–4 |
| `chain_empty_is_a_wire` | an empty chain passes audio unchanged, bit for bit |
| `chain_single_slot_equals_effect` | after the fade-in, a slot outputs exactly what the effect does |
| `chain_serial_and_independent_instances` | delay → reverb → delay in the chain = the same three effects run by hand (two delays with different settings don't share buffers) |
| `chain_bypass_and_swap_are_click_free` | hard/trails bypass and effect swaps never step faster than the crossfade allows (a 100 Hz sine vs its delayed copy) |
| `chain_hard_bypass_is_exact_and_clears_tails` | hard bypass = exact wire; un-bypassing brings no stale echoes |
| `chain_trails_bypass_lets_tails_ring` | trails bypass keeps the tail ringing over the dry signal, and it dies away |
| `chain_swap_keeps_other_slots_playing` | a swap completes in ~20 ms while the other slots keep running |
| `chain_effect_too_big_for_arena` | an effect that doesn't fit its arena leaves the slot empty (a wire) and is reported |
| `chain_stress_random_changes` | 30 s of random swaps, bypass toggles and parameter jumps stay finite and bounded |
| `ui_home_lists_the_chain` | HOME shows one row per slot (lowercase when bypassed), SETUP scrolls into view |
| `ui_build_a_chain_from_the_panel` | fill all 4 slots (incl. the same effect twice) and empty one again, only with turns/clicks/holds |
| `ui_replacing_an_effect_needs_confirmation` | `REPLACE?` step; turning or holding backs out |
| `ui_long_press_saves_on_home_and_goes_back_elsewhere` | hold = save on HOME, back everywhere else |
| `ui_edits_reach_the_open_slot` | parameter / bypass edits land in the slot that's open, nowhere else |
| `ui_routes_reach_every_slot` | KNOB/CV pages list every parameter of every filled slot, plus NONE |
| `ui_out_level_in_half_db_steps` | OUT page: 0.5 dB steps, exact unity back at 0 dB, −24…+6 dB |
| `ui_peek_shows_any_slot`, `ui_survives_type_changes_underneath`, `ui_remembers_selection_and_pages` | peeks, effect changed under an open page, remembered pages and HOME selection |
| `ui_every_screen_fits_the_display` | every page of every level, editing or not: no text past x = 59 (the meters) |

**Adding an effect:** add it to `g_effects[]` at the top of `test_effects.cpp` and to the
`tested` list in `test_chain.cpp`.
