#pragma once
/**
 * Human-readable preset text format (no hardware dependencies, so it can
 * be unit-tested on a PC).
 *
 *   # comment                  ('#' or ';' start a comment, also inline)
 *   name   = Big Hall          (shown on the OLED, up to 16 chars)
 *   effect = REVERB            (which effect the rest applies to; if
 *                               missing, the current effect is used)
 *   input  = stereo            (stereo | mono)
 *   decay  = 2.35              (real units: s, %, Hz; "8.5k" = 8500)
 *   mix    = 35
 *   lo_cut = 120
 *   hi_cut = 8000
 *   knob1  = decay             (none | any parameter key)
 *   knob2  = mix
 *   cv1    = decay
 *   cv2    = hi_cut
 *
 * Keys are case-insensitive; parameter keys are the parameter name in
 * lower case with spaces as underscores (they depend on the effect: the
 * delay has time / feedbk / mix / ping / lo_cut / hi_cut). Missing keys
 * leave the current value unchanged; unknown keys are ignored.
 */
#include <cstddef>
#include "fx/effect.h"
#include "app/settings.h"

namespace app
{
static constexpr size_t kPresetNameLen   = 17; // 16 chars + NUL
static constexpr size_t kEffectNameLen   = 12;
static constexpr int    kPresetNoEffect  = -1; // file has no "effect" line
static constexpr int    kPresetBadEffect = -2; // "effect" names an unknown effect

/** Writes the preset as text. Returns bytes written (0 if it didn't fit). */
size_t SerializePreset(const fx::Effect  &effect,
                       const EffectState &state,
                       uint8_t            input_mode,
                       const char        *name,
                       char              *out,
                       size_t             capacity);

struct ParseResult
{
    int  applied;                // number of recognised key/value pairs
    int  errors;                 // lines that could not be understood
    char name[kPresetNameLen];   // "name" value, or empty
    char effect[kEffectNameLen]; // "effect" value as written, or empty
};

/** Applies every recognised key in `text` to `state` / `input_mode`,
 *  interpreting parameter keys for `effect`. */
ParseResult ParsePreset(const fx::Effect &effect,
                        const char       *text,
                        size_t            len,
                        EffectState      &state,
                        uint8_t          &input_mode);

/** Which of `effects` the preset is for: its index, kPresetNoEffect or
 *  kPresetBadEffect. Matching is case-insensitive on Effect::Name(). */
int FindPresetEffect(const fx::Effect *const *effects,
                     size_t                   num_effects,
                     const char              *text,
                     size_t                   len);

} // namespace app
