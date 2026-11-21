#pragma once
#include "fx/effect.h"
#include "daisysp.h"

namespace fx
{
/**
 * Stereo chorus / flanger.
 *
 * Signal path (per channel):
 *   in -> + -> delay line --(modulated tap)--+--> wet
 *         ^                                  |
 *         +---- soft clip <- FEEDBK <--------+
 *
 * - One LFO per channel, the right one SPREAD x 180 degrees behind the left,
 *   so a mono source comes out wide. The LFO is a triangle shaped into a
 *   near-sine (cheaper than sinf per sample, no harmonics worth hearing on
 *   a delay-time modulator).
 * - The tap moves around DELAY by up to +-95 % of DELAY (DEPTH 100 %), so it
 *   never reaches zero delay. Short DELAY + FEEDBK = flanger; longer DELAY,
 *   no feedback = chorus/vibrato (MIX 100 % = pure vibrato).
 * - Reads are 4-point Hermite interpolated: a moving tap with linear
 *   interpolation dulls the highs.
 * - The feedback is soft-clipped, so high FEEDBK saturates instead of
 *   blowing up; with FEEDBK at 0 the wet signal is a clean copy.
 * - Buffers: two 4096-sample lines (85 ms) from the slot's SDRAM arena.
 */
class Chorus : public Effect
{
  public:
    enum Param
    {
        RATE = 0,
        DEPTH,
        DELAY,
        FEEDBACK,
        SPREAD,
        MIX,
        NUM_PARAMS
    };

    const char *Name() const override { return "CHORUS"; }
    const char *Code() const override { return "CH"; }
    bool        Init(float sample_rate, Arena &mem) override;
    void        Reset() override;

    size_t           NumParams() const override { return NUM_PARAMS; }
    const ParamSpec &Spec(size_t idx) const override { return kSpecs[idx]; }
    void             SetParam(size_t idx, float norm) override;
    int              DefaultRoute(int src) const override
    {
        static const int8_t r[4] = {RATE, DEPTH, DEPTH, RATE}; // K1 K2 CV1 CV2
        return src >= 0 && src < 4 ? r[src] : -1;
    }

    // Longest read: DELAY 30 ms + 95 % of it, plus interpolation headroom
    static constexpr size_t kMaxDelaySamples = 4096;
    using Line = daisysp::DelayLine<float, kMaxDelaySamples>;

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
    float depth_ = 0.f, delay_ = 0.f, fb_ = 0.f, spread_ = 0.f, mix_ = 0.f;
    float phase_ = 0.f; // left LFO phase, 0..1
    Line *line_l_ = nullptr, *line_r_ = nullptr;
};

} // namespace fx
