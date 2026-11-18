#pragma once
/**
 * Preset slots on the microSD card: 0:/BLUEMCHN/PRESET01.TXT .. PRESET99.TXT
 *
 * The card is mounted on demand (never at boot), so it can be inserted or
 * swapped at any time. Every mount attempt fully resets the SDMMC
 * peripheral and the FatFS driver state first. The bus runs in 1-bit mode
 * at 12.5 MHz (4-bit doesn't work on the Bluemchen; see preset_store.cpp).
 *
 * All calls block (normally a few ms, longer while a card is being
 * initialised) and must be made from the main loop, never from the audio
 * callback. Audio keeps running meanwhile.
 */
#include <cstddef>
#include "daisy_seed.h"
#include "fx/effect.h"
#include "app/settings.h"
#include "app/preset_text.h"

namespace app
{
class PresetStore
{
  public:
    enum class Status
    {
        OK,
        NO_CARD,
        NO_FILE,
        IO_ERROR,
        PARSE_ERROR, // file read but contained no usable settings
        BAD_EFFECT,  // "effect = ..." names an effect this firmware lacks
    };

    /** No card access here; only sets up the driver. `effects` is the
     *  firmware's effect list (index = Settings::effect). */
    void Init(const fx::Effect *const *effects, size_t num_effects);

    bool Mounted() const { return mounted_; }

    /** Writes the active effect's settings to slot (1..99). Keeps the
     *  slot's existing name if it has one, otherwise "PRESET nn". */
    Status Save(int slot, const Settings &settings);

    /** Applies the slot's file on top of `settings` (only on success).
     *  May change settings.effect if the preset is for another effect. */
    Status Load(int slot, Settings &settings);

    /** Reads only the slot's name and effect name (empty if missing). */
    Status Peek(int slot, char *name, size_t len, char *effect, size_t effect_len);

    static const char *StatusText(Status s);

    /** Short diagnostic for the last failure: FatFS result, HAL SD error
     *  bits (hex) and bus mode, e.g. "F3 E4 4b". "" after success. */
    const char *LastError() const { return last_error_; }

  private:
    bool   EnsureMounted();
    bool   TryMount(int bus_cfg);
    void   Unmount();
    void   SlotPath(int slot, char *buf, size_t len) const;
    Status ReadSlot(int slot, size_t &len);
    void   RecordError(int fresult, int bus_cfg);
    Status MountFailureStatus() const;

    const fx::Effect *const *effects_     = nullptr;
    size_t                   num_effects_ = 0;

    daisy::SdmmcHandler   sd_;
    daisy::FatFSInterface fsi_;
    bool                  mounted_       = false;
    int                   bus_cfg_       = 0; // index of last working bus config
    uint32_t              last_sd_error_ = 0;
    char                  last_error_[20] = {0};
};

} // namespace app
