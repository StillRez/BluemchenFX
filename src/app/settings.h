#pragma once
#include <cstdint>
#include <cstring>

namespace app
{
static constexpr size_t   kMaxParams       = 8;
static constexpr uint32_t kSettingsVersion = 0xB10E0001; // bump on layout change

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

/** Everything that is persisted to QSPI flash. Plain-old-data only. */
struct Settings
{
    uint32_t version;
    float    norm[kMaxParams];  // parameter base values, 0..1
    int8_t   route[SRC_LAST];   // destination param index or kRouteNone
    uint8_t  input_mode;        // InputMode (0 = stereo, so older saves load as stereo)
    uint8_t  pad[3];

    bool operator!=(const Settings &o) const
    {
        return std::memcmp(this, &o, sizeof(Settings)) != 0;
    }
};

} // namespace app
