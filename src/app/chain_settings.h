#pragma once
/**
 * Settings logic for the effect chain, independent of the hardware so it
 * can be tested on the PC: defaults, validation, migration of older
 * layouts, changing a slot's effect (with per-slot recall) and the bridge
 * to single-effect presets.
 */
#include "app/settings.h"
#include "fx/registry.h"

namespace app
{
/** Per slot and effect type: the parameters that slot last had for that
 *  type, so DELAY -> REVERB -> DELAY gets the delay's settings back.
 *  Lives in RAM only (not saved to flash). */
struct Recall
{
    float norm[kNumSlots][fx::FX_COUNT][kMaxParams];
    bool  valid[kNumSlots][fx::FX_COUNT];

    void Clear() { std::memset(this, 0, sizeof(*this)); }
};

/** The effect's default parameter values (zeros past its parameter count;
 *  all zeros for FX_NONE). */
void DefaultNorms(uint8_t type, float *norm);

/** Factory settings: REVERB in slot 1 with its default routing, slots 2-4
 *  empty, stereo input, unity output. */
Settings MakeDefaults();

/** Whether `s` is a plausible v3 settings block (version, types, ranges,
 *  routes pointing at existing parameters). */
bool Valid(const Settings &s);

/** v2 (one active effect) -> v3: the active effect goes to slot 1 with its
 *  parameters and routing; the other effects' stored parameters seed
 *  `recall` for slot 1 (if given). False if `v2` isn't valid. */
bool MigrateV2(const SettingsV2 &v2, Settings &out, Recall *recall);

/** v1 (reverb only) -> v3. False if `v1` isn't valid. */
bool MigrateV1(const SettingsV1 &v1, Settings &out);

/** Puts `type` (or FX_NONE) into `slot`. The slot's current parameters are
 *  kept in `recall`; the new type gets its recalled parameters, else its
 *  defaults. Sources routed to this slot follow the new effect's default
 *  routing (or go to none). Bypass flags are kept. Returns false if the
 *  slot already holds `type`. */
bool SetSlotType(Settings &s, Recall &recall, int slot, uint8_t type);

/** Single-effect view of `slot`: its parameters, and the sources routed to
 *  it as parameter indices (others: none). For the preset text format. */
EffectState SlotAsEffectState(const Settings &s, int slot);

/** Loads a single-effect preset: `type` with `st` into slot 1, slots 2-4
 *  emptied (a preset is the whole chain), bypass off, routes from `st`.
 *  Updates recall for the new contents. */
void ApplySingleEffectPreset(Settings          &s,
                             Recall            &recall,
                             uint8_t            type,
                             const EffectState &st,
                             uint8_t            input_mode);

} // namespace app
