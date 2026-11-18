#pragma once
#include <cstdint>
#include <cstring>

namespace app
{
static constexpr size_t kMaxParams  = 8; // per effect
static constexpr size_t kMaxEffects = 4; // room to grow without a layout change

/** v2: per-effect parameters + routing, active effect index. */
static constexpr uint32_t kSettingsVersion = 0xB10E0002; // bump on layout change

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

/** Input channel handling, applied before the effect. */
enum InputMode : uint8_t
{
    INPUT_STEREO = 0, // L -> L, R -> R
    INPUT_MONO,       // L -> L and R (right input ignored)
    INPUT_MODE_LAST
};

/** Everything one effect remembers: parameter values and routing. */
struct EffectState
{
    float   norm[kMaxParams];  // parameter base values, 0..1
    int8_t  route[SRC_LAST];   // destination param index or kRouteNone
    uint8_t pad[4];
};

/** Everything that is persisted to QSPI flash. Plain-old-data only. */
struct Settings
{
    uint32_t    version;
    uint8_t     effect;     // index of the active effect
    uint8_t     input_mode; // InputMode
    uint8_t     pad[2];
    EffectState fx[kMaxEffects];

    EffectState       &Active() { return fx[effect]; }
    const EffectState &Active() const { return fx[effect]; }

    bool operator!=(const Settings &o) const
    {
        return std::memcmp(this, &o, sizeof(Settings)) != 0;
    }
};

/** v1 layout (firmware up to v0.3.0), kept only to migrate saved settings.
 *  v1 had a single effect (the reverb). */
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
