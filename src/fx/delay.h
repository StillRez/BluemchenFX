#pragma once
#include "fx/effect.h"
#include "daisysp.h"

namespace fx
{
/**
 * Stereo delay, up to 2 s.
 *
 * Signal path (per channel):
 *   in -> [ping-pong input routing] -> + -> soft clip -> delay line --+--> wet
 *                                      ^                              |
 *                                      +-- feedback <- [cross-feed] <-+
 *                                           <- Hi Cut LP <- Lo Cut HP
 *
 * - The filters sit in the feedback loop *and* on the wet output, so every
 *   repeat gets a little thinner/darker (tape-style).
 * - PING 0 % = independent L/R echoes; 100 % = full ping-pong. At 100 % the
 *   input is summed into the left line and the feedback crosses over, so
 *   echoes alternate L/R even from a mono source.
 * - Delay time is smoothed per sample, so turning TIME glides (pitch bends
 *   like tape) instead of clicking.
 * - Writes are soft-clipped, so high feedback saturates instead of blowing up.
 * - Loop filters are 6 dB/oct zero-delay one-poles (unconditionally stable).
 *   Not daisysp::Svf: its built-in cubic "drive" term goes unstable at the
 *   signal levels a saturating feedback loop reaches.
 */
class Delay : public Effect
{
  public:
    enum Param
    {
        TIME = 0,
        FEEDBACK,
        MIX,
        PING,
        LO_CUT,
        HI_CUT,
        NUM_PARAMS
    };

    const char *Name() const override { return "DELAY"; }
    bool        Init(float sample_rate, Arena &mem) override;
    void        Reset() override;

    size_t           NumParams() const override { return NUM_PARAMS; }
    const ParamSpec &Spec(size_t idx) const override { return kSpecs[idx]; }
    void             SetParam(size_t idx, float norm) override;
    int              DefaultRoute(int src) const override
    {
        static const int8_t r[4] = {TIME, MIX, TIME, FEEDBACK}; // K1 K2 CV1 CV2
        return src >= 0 && src < 4 ? r[src] : -1;
    }

    // 2 s at 48 kHz plus headroom for the interpolating read
    static constexpr size_t kMaxDelaySamples = 96000 + 256;
    using Line = daisysp::DelayLine<float, kMaxDelaySamples>;

    void Process(const float *in_l,
                 const float *in_r,
                 float       *out_l,
                 float       *out_r,
                 size_t       size) override;

  private:
    static const ParamSpec kSpecs[NUM_PARAMS];

    void SnapSmoothing();
    void InitFilters();
    void SetFilterFreqs();

    float sr_ = 48000.f;

    float target_[NUM_PARAMS] = {};
    float delay_samps_ = 0.f; // smoothed per sample
    float fb_ = 0.f, mix_ = 0.f, ping_ = 0.f, lo_ = 20.f, hi_ = 18000.f;

    daisysp::OnePole hp_l_, hp_r_, lp_l_, lp_r_;
    Line            *line_l_ = nullptr, *line_r_ = nullptr; // ~386 KB each, SDRAM arena
};

} // namespace fx
