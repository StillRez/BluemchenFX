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
#include "app/chain_settings.h"

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

// ------------------------------------------------------ chain presets
/*
 * Chain presets (phase 3) add [slot1]..[slot4] sections. Files without any
 * section are the single-effect format above and keep loading into slot 1.
 *
 *   name   = Dub Space          global keys before the first section
 *   input  = stereo
 *   out    = -3.0               output level, dB (-24 .. 6, 0.5 dB steps)
 *
 *   [slot1]
 *   effect      = DELAY         none = empty; missing = keep the slot's type
 *   bypass      = off           on | off (default off)
 *   bypass_mode = trails        trails | hard (default trails)
 *   time        = 0.37          parameters of that slot's effect
 *
 *   knob1  = 1.time             routing, anywhere: none | <slot>.<param>,
 *   cv2    = decay              or a bare <param> of the current section
 *                               (slot 1 before the first section)
 *
 * A slot without a section is emptied: a chain preset is the whole chain.
 * A slot that changes effect starts from that effect's defaults; missing
 * keys keep the current value. The limiter (SETUP) is not part of presets.
 */

enum class PresetLoad
{
    OK,
    BAD_FILE,   // nothing usable in it
    BAD_EFFECT, // names an effect this firmware doesn't have
};

/** Writes the whole chain as a chain preset. Returns bytes written (0 if it
 *  didn't fit). */
size_t SerializeChainPreset(const Settings &s,
                            const char     *name,
                            char           *out,
                            size_t          capacity);

/** Loads either format onto `s` (in: current, out: loaded; `s` and
 *  `recall` only change on OK). Old single-effect files go to slot 1 and
 *  empty slots 2-4, as before. */
PresetLoad LoadPresetText(const char *text, size_t len, Settings &s, Recall &recall);

/** What a file holds, without applying it (for the SD slot pages). */
struct PresetInfo
{
    char    name[kPresetNameLen];
    bool    chain;            // has [slotN] sections
    uint8_t type[kNumSlots];  // effect per slot (FX_NONE = empty)
    bool    known;            // types are known (false: old file without
                              // an effect line, or an unknown effect)
};
PresetInfo PeekPresetText(const char *text, size_t len);

/** Short text for the SD slot page: one effect -> "REVERB", two -> "DEL>REV",
 *  three -> "DL>RV>CH", four -> "DLRVCHRS" (2-letter codes), none -> "EMPTY".
 *  "" when not `known`. */
void PresetSummary(const PresetInfo &info, char *buf, size_t cap);

} // namespace app
