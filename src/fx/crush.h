#pragma once
#include "fx/effect.h"
#include "daisysp.h"

namespace fx
{
/**
 * Bit crusher / sample-rate reducer.
 *
 * Signal path (per channel, both channels share one sample-and-hold clock):
 *   in -> DRIVE gain -> clamp +-1 -> sample & hold at RATE -> quantise to
 *   BITS -> Hi Cut LP -> equal-power mix with the dry signal
 *
 * - RATE is a free-running phase accumulator, so any rate between 100 Hz
 *   and 48 kHz works (not just integer divisions); the aliasing is the
 *   point. At 48 kHz every sample is taken.
 * - BITS is continuous (the step size is 2^-(bits-1)), so turning it sweeps
 *   smoothly instead of jumping. The quantiser is mid-tread: at low bit
 *   depths quiet signals drop to silence, as on real low-bit converters.
 * - DRIVE pushes the signal into the clamp and the quantiser (up to +24 dB).
 * - Hi Cut (6 dB/oct one-pole) tames the aliasing when wanted. All the way
 *   up (the default) it's off.
 * - No buffers: nothing is taken from the slot's SDRAM arena.
 */
class Crush : public Effect
{
  public:
    enum Param
    {
        BITS = 0,
        RATE,
        DRIVE,
        HI_CUT,
        MIX,
        NUM_PARAMS
    };

    const char *Name() const override { return "CRUSH"; }
    const char *Code() const override { return "CR"; }
    bool        Init(float sample_rate, Arena &mem) override;
    void        Reset() override;

    size_t           NumParams() const override { return NUM_PARAMS; }
    const ParamSpec &Spec(size_t idx) const override { return kSpecs[idx]; }
    void             SetParam(size_t idx, float norm) override;
    int              DefaultRoute(int src) const override
    {
        static const int8_t r[4] = {BITS, RATE, RATE, BITS}; // K1 K2 CV1 CV2
        return src >= 0 && src < 4 ? r[src] : -1;
    }

    void Process(const float *in_l,
                 const float *in_r,
                 float       *out_l,
                 float       *out_r,
                 size_t       size) override;

  private:
    static const ParamSpec kSpecs[NUM_PARAMS];

    void SnapSmoothing();

    float sr_ = 48000.f;
    float target_[NUM_PARAMS] = {};
    // smoothed (block rate)
    float bits_ = 8.f, rate_ = 12000.f, drive_ = 0.f, hi_ = 18000.f, mix_ = 1.f;
    float phase_  = 0.f; // sample & hold clock, 0..1
    float hold_l_ = 0.f, hold_r_ = 0.f;
    daisysp::OnePole lp_l_, lp_r_;
};

} // namespace fx
