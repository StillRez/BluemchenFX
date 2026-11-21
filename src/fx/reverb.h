#pragma once
#include "fx/effect.h"
#include "daisysp.h"

namespace fx
{
/**
 * Stereo reverb built on DaisySP ReverbSc (Sean Costello's FDN, LGPL).
 *
 * Signal path:
 *   in -> Lo Cut (12 dB/oct SVF high-pass) -> ReverbSc (Hi Cut = tank damping)
 *      -> equal-power dry/wet mix -> out
 *
 * Decay is specified as an RT60 time in seconds and converted to the
 * ReverbSc feedback coefficient using the tank's mean loop delay.
 */
class Reverb : public Effect
{
  public:
    enum Param
    {
        DECAY = 0,
        MIX,
        LO_CUT,
        HI_CUT,
        NUM_PARAMS
    };

    const char *Name() const override { return "REVERB"; }
    const char *Code() const override { return "RV"; }
    bool        Init(float sample_rate, Arena &mem) override;
    void        Reset() override;

    size_t           NumParams() const override { return NUM_PARAMS; }
    const ParamSpec &Spec(size_t idx) const override { return kSpecs[idx]; }
    void             SetParam(size_t idx, float norm) override;
    int              DefaultRoute(int src) const override
    {
        static const int8_t r[4] = {DECAY, MIX, DECAY, HI_CUT}; // K1 K2 CV1 CV2
        return src >= 0 && src < 4 ? r[src] : -1;
    }

    void Process(const float *in_l,
                 const float *in_r,
                 float       *out_l,
                 float       *out_r,
                 size_t       size) override;

  private:
    static const ParamSpec kSpecs[NUM_PARAMS];

    float sr_ = 48000.f;

    // Targets (set from control side) and smoothed values (audio side)
    float target_[NUM_PARAMS] = {};
    float fb_ = 0.f, mix_ = 0.f, lo_ = 20.f, hi_ = 18000.f;

    daisysp::Svf       hp_l_, hp_r_;
    daisysp::ReverbSc *tank_ = nullptr; // ~400 KB, in the slot's SDRAM arena
};

} // namespace fx
