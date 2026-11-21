#pragma once
#include "fx/effect.h"
#include "daisysp.h"

namespace fx
{
/**
 * Modal resonator: the input excites a bank of tuned band-pass "modes", like
 * striking a string, bar or bell. Modelled on Mutable Instruments Rings'
 * modal resonator (as ported to DaisySP's Resonator), rewritten for an effect:
 *
 * - kModes band-pass modes per channel (zero-delay-feedback SVFs). Their
 *   coefficients are computed once per 32-sample block, not per sample as in
 *   DaisySP's Resonator, which keeps a stereo bank cheap.
 * - PITCH sets the fundamental. STRUCT spreads the partials: 25-30 % is
 *   harmonic (string), below that they're squeezed together, above it they
 *   stretch out into bells and bars (Rings' stiffness curve; the 3rd partial
 *   stays in tune).
 * - DECAY is the ring time (RT60) of the fundamental in seconds. BRIGHT sets
 *   how much faster the upper partials die away (Rings' q_loss), so low
 *   BRIGHT gives a dark, soft ring.
 * - Left and right banks share all frequencies but weight the partials
 *   differently (the right one flips every other partial), so even a mono
 *   input comes out wide.
 * - Each mode is scaled so that broadband input comes out at a similar
 *   level whatever the DECAY; a sustained note right on a mode's frequency
 *   still rings up (that's resonance), and the wet signal is soft-clipped
 *   at +-2. The output limiter catches the rest.
 * - No buffers: nothing is taken from the slot's SDRAM arena.
 */
class Reson : public Effect
{
  public:
    enum Param
    {
        PITCH = 0,
        STRUCT,
        BRIGHT,
        DECAY,
        MIX,
        NUM_PARAMS
    };

    static constexpr int kModes = 16;

    const char *Name() const override { return "RESON"; }
    const char *Code() const override { return "RS"; }
    bool        Init(float sample_rate, Arena &mem) override;
    void        Reset() override;

    size_t           NumParams() const override { return NUM_PARAMS; }
    const ParamSpec &Spec(size_t idx) const override { return kSpecs[idx]; }
    void             SetParam(size_t idx, float norm) override;
    int              DefaultRoute(int src) const override
    {
        static const int8_t r[4] = {PITCH, DECAY, PITCH, STRUCT}; // K1 K2 CV1 CV2
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
    void UpdateModes();

    float sr_ = 48000.f;
    float target_[NUM_PARAMS] = {};
    // smoothed (block rate); pitch is smoothed in octaves
    float oct_ = 0.f, struct_ = 0.f, bright_ = 0.f, decay_ = 1.f, mix_ = 0.f;

    // Per-mode coefficients (block rate) and state (per channel)
    int   active_ = 0;            // modes below Nyquist
    float g_[kModes];             // tan(pi f)
    float a1_[kModes];            // 1 / (1 + g (g + k))
    float gain_[kModes];          // output weight (left; right flips odd)
    float s1_[2][kModes], s2_[2][kModes];
};

} // namespace fx
