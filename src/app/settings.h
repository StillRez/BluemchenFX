#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace app
{
static constexpr size_t kMaxParams = 8; // per effect
static constexpr size_t kNumSlots  = 4; // effect chain length

/** Modulation sources that can be routed to a parameter. */
enum Source : uint8_t
{
    SRC_KNOB_1 = 0,
    SRC_KNOB_2,
    SRC_CV_1,
    SRC_CV_2,
    SRC_LAST
};

static constexpr int8_t kRouteNone = -1;

/** Number of preset slots on the SD card (PRESET01..PRESET99.TXT). */
static constexpr int kPresetSlotCount = 99;

/** Input channel handling, applied before the chain. */
enum InputMode : uint8_t
{
    INPUT_STEREO = 0, // L -> L, R -> R
    INPUT_MONO,       // L -> L and R (right input ignored)
    INPUT_MODE_LAST
};

/** One effect's parameters + routing (param indices). Used by the
 *  single-effect preset text format and by the v2 layout below. */
struct EffectState
{
    float   norm[kMaxParams]; // parameter base values, 0..1
    int8_t  route[SRC_LAST];  // destination param index or kRouteNone
    uint8_t pad[4];
};

// ===================================================================== v4
/** v4: effect chain + output limiter. v3 (chain without limiter) is the
 *  same layout without the last two fields. */
static constexpr uint32_t kSettingsVersion = 0xB10E0004; // bump on layout change

/** Output limiter ranges (SETUP: LIMIT, LIM REL). */
static constexpr float kLimitMinDb      = -12.f;
static constexpr float kLimitMaxDb      = 0.f;
static constexpr float kLimitDefaultDb  = -1.f;
static constexpr float kReleaseMinSec   = 0.02f;
static constexpr float kReleaseMaxSec   = 1.f;
static constexpr float kReleaseDefault  = 0.15f;

/** Slot flag bits. */
enum SlotFlags : uint8_t
{
    SLOT_BYPASS = 1 << 0, // bypassed
    SLOT_HARD   = 1 << 1, // bypass cuts tails (else: trails ring out)
};

struct SlotState
{
    uint8_t type;  // fx::EffectId, or fx::FX_NONE (0xFF) = empty
    uint8_t flags; // SlotFlags
    uint8_t pad[2];
    float   norm[kMaxParams]; // parameter base values, 0..1

    bool Bypassed() const { return flags & SLOT_BYPASS; }
    bool Hard() const { return flags & SLOT_HARD; }
};

/** Where a modulation source goes: a parameter of one slot. */
struct Route
{
    int8_t slot;  // 0..kNumSlots-1, or kRouteNone
    int8_t param; // parameter index, or kRouteNone

    bool None() const { return slot == kRouteNone; }
    bool Is(int s, int p) const { return slot == s && param == p; }
};

/** Everything that is persisted to QSPI flash. Plain-old-data only. */
struct Settings
{
    uint32_t  version;
    uint8_t   input_mode; // InputMode
    uint8_t   topology;   // 0 = serial (reserved for parallel routing)
    uint8_t   ui_slot;    // slot last edited (UI, phase 2)
    uint8_t   pad;
    float     out_gain;   // linear output gain, 1 = unity
    SlotState slot[kNumSlots];
    Route     route[SRC_LAST]; // K1, K2, CV1, CV2
    // --- v4
    float     limit_db;    // output limiter ceiling, dBFS
    float     limit_rel;   // output limiter release, seconds

    bool operator!=(const Settings &o) const
    {
        return std::memcmp(this, &o, sizeof(Settings)) != 0;
    }
};

// ============================================== legacy, for migration
/** v3 layout (phase 1-2 builds, after v0.5.0): Settings without the
 *  limiter fields. */
static constexpr uint32_t kSettingsVersionV3 = 0xB10E0003;
struct SettingsV3
{
    uint32_t  version;
    uint8_t   input_mode;
    uint8_t   topology;
    uint8_t   ui_slot;
    uint8_t   pad;
    float     out_gain;
    SlotState slot[kNumSlots];
    Route     route[SRC_LAST];
};
static_assert(sizeof(SettingsV3) == 164, "v3 layout must not change");

/** v2 layout (firmware 0.4.x-0.5.x): one active effect out of up to 4,
 *  each with its own params and routing. */
static constexpr uint32_t kSettingsVersionV2 = 0xB10E0002;
static constexpr size_t   kMaxEffectsV2      = 4;
struct SettingsV2
{
    uint32_t    version;
    uint8_t     effect;     // index of the active effect
    uint8_t     input_mode; // InputMode
    uint8_t     pad[2];
    EffectState fx[kMaxEffectsV2];
};
static_assert(sizeof(SettingsV2) == 168, "v2 layout must not change");

/** v1 layout (firmware up to v0.3.0): a single effect (the reverb). */
static constexpr uint32_t kSettingsVersionV1 = 0xB10E0001;
struct SettingsV1
{
    uint32_t version;
    float    norm[kMaxParams];
    int8_t   route[SRC_LAST];
    uint8_t  input_mode;
    uint8_t  pad[3];
};

} // namespace app
