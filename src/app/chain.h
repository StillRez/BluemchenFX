#pragma once
/**
 * The effect chain engine: up to 4 stereo slots processed in series.
 *
 * Threads: Process() runs in the audio interrupt; everything else runs in
 * the main loop. They hand over through a few volatile flags per slot:
 *
 *   main: want_on  (the slot's effect should be heard)
 *         bypass / hard
 *   audio: idle    (gain has reached 0 and the effect is no longer called)
 *
 * Each slot has two ramps (~10 ms, per sample):
 *   g  run gain:   0 = slot is a wire (effect not processed), 1 = effect on
 *   b  trail mix:  fades the effect's *input* out while the dry signal
 *                  fades in around it, so tails ring out (trails bypass)
 *
 *   out = in·(1-g) + g·( fx(in·(1-b)) + in·b )
 *
 * Hard bypass and effect swaps ramp g to 0; trails bypass ramps b to 1.
 * Swapping an effect: main sets want_on = false, waits for idle (the old
 * effect has faded out and is untouched by audio), destroys it, builds and
 * initialises the new one in the same storage + arena, then sets want_on
 * again and audio fades it in. Poll() does this step by step, so the main
 * loop never blocks and the other slots keep playing throughout.
 */
#include <cstddef>
#include <cstdint>
#include "fx/arena.h"
#include "fx/registry.h"
#include "app/settings.h"

namespace app
{
class Chain
{
  public:
    static constexpr size_t kMaxBlock = 64;    // samples per Process() chunk
    static constexpr float  kFadeSec  = 0.01f; // bypass / swap ramp

    /** `arenas`: one per slot (each slot's effect allocates from its own). */
    void Init(float sample_rate, fx::Arena *arenas);

    // ---- main loop -----------------------------------------------------
    /** Make `slot` hold `type` (FX_NONE = empty), starting from `norm`
     *  (kMaxParams values). Takes effect over the next few Poll() calls. */
    void SetType(size_t slot, uint8_t type, const float *norm);
    void SetBypass(size_t slot, bool bypass, bool hard);

    /** Advances pending swaps and clears hard-bypassed effects. Call every
     *  main loop iteration. */
    void Poll();

    /** Type the slot is heading for (what SetType last asked). */
    uint8_t Target(size_t slot) const { return slots_[slot].target; }
    /** True while a swap for this slot is still in progress. */
    bool Swapping(size_t slot) const { return slots_[slot].pending; }
    /** Effect type actually loaded (FX_NONE while empty / mid-swap). */
    uint8_t Loaded(size_t slot) const { return slots_[slot].type; }
    /** SetType failed: the effect didn't fit its arena. Cleared on read. */
    bool TakeInitFailure(size_t slot);

    // ---- audio interrupt -------------------------------------------------
    /** Runs the chain. `params[slot][i]` are the effective (modulated)
     *  normalised parameter values for each slot, meant for effect type
     *  `types[slot]`. They're only applied while that matches the effect
     *  actually loaded, so an effect fading out during a swap never gets
     *  the incoming effect's values. In and out may alias. */
    void Process(const float   *in_l,
                 const float   *in_r,
                 float         *out_l,
                 float         *out_r,
                 size_t         size,
                 const float (*params)[kMaxParams],
                 const uint8_t *types);

  private:
    struct Slot
    {
        alignas(fx::kEffectStorageAlign) uint8_t storage[fx::kEffectStorageBytes];
        fx::Effect *fx    = nullptr; // audio uses it only while !idle
        fx::Arena  *arena = nullptr;
        uint8_t     type  = fx::FX_NONE;

        // main -> audio
        volatile bool want_on = false;
        volatile bool bypass  = false;
        volatile bool hard    = false;
        // audio -> main
        volatile bool idle = true;

        // audio state
        float g = 0.f, b = 0.f;

        // main state
        uint8_t target  = fx::FX_NONE;
        bool    pending = false;
        float   pending_norm[kMaxParams];
        bool    cleared      = true; // effect reset since it last ran
        bool    init_failure = false;
    };

    void ProcessSlot(Slot        &s,
                     const float *in_l,
                     const float *in_r,
                     float       *out_l,
                     float       *out_r,
                     size_t       n,
                     const float *params,
                     uint8_t      params_type);

    Slot  slots_[kNumSlots];
    float sr_   = 48000.f;
    float step_ = 1.f / 480.f; // ramp increment per sample

    // audio scratch: ping-pong buffers between slots, effect in/out
    float buf_[2][2][kMaxBlock];
    float x_[2][kMaxBlock], y_[2][kMaxBlock];
};

} // namespace app
