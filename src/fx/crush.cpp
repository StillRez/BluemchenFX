#include "fx/crush.h"

using namespace daisysp;

namespace fx
{
// name       min      max       default  curve               unit
const ParamSpec Crush::kSpecs[NUM_PARAMS] = {
    {"BITS", 1.f, 16.f, 0.4667f, Curve::Linear, Unit::Bits},          // 8 bits
    {"RATE", 100.f, 48000.f, 0.775f, Curve::Exponential, Unit::Hertz}, // ~12 kHz
    {"DRIVE", 0.f, 100.f, 0.f, Curve::Linear, Unit::Percent},
    {"HI CUT", 500.f, 18000.f, 1.f, Curve::Exponential, Unit::Hertz},
    {"MIX", 0.f, 100.f, 1.f, Curve::Linear, Unit::Percent},
};

bool Crush::Init(float sample_rate, Arena &mem)
{
    (void)mem; // no buffers
    sr_ = sample_rate;
    for(size_t i = 0; i < NUM_PARAMS; i++)
        target_[i] = kSpecs[i].ToReal(kSpecs[i].def);
    Reset();
    return true;
}

void Crush::SnapSmoothing()
{
    bits_  = target_[BITS];
    rate_  = target_[RATE];
    drive_ = target_[DRIVE];
    hi_    = target_[HI_CUT];
    mix_   = target_[MIX] * 0.01f;
}

void Crush::Reset()
{
    lp_l_.Init();
    lp_r_.Init();
    lp_l_.SetFilterMode(OnePole::FILTER_MODE_LOW_PASS);
    lp_r_.SetFilterMode(OnePole::FILTER_MODE_LOW_PASS);
    phase_  = 1.f; // take a sample on the very first tick
    hold_l_ = hold_r_ = 0.f;
    SnapSmoothing();
}

void Crush::SetParam(size_t idx, float norm)
{
    if(idx < NUM_PARAMS)
        target_[idx] = kSpecs[idx].ToReal(norm);
}

static inline float Clamp1(float x)
{
    return x < -1.f ? -1.f : (x > 1.f ? 1.f : x);
}

void Crush::Process(const float *in_l,
                    const float *in_r,
                    float       *out_l,
                    float       *out_r,
                    size_t       size)
{
    const float k = 0.08f;
    fonepole(bits_, target_[BITS], k);
    fonepole(rate_, target_[RATE], k);
    fonepole(drive_, target_[DRIVE], k);
    fonepole(hi_, target_[HI_CUT], k);
    fonepole(mix_, target_[MIX] * 0.01f, k);

    const float hi = hi_ / sr_;
    lp_l_.SetFrequency(hi);
    lp_r_.SetFrequency(hi);
    // Hi Cut all the way up = filter off, so 16 bits at 48 kHz is plain
    // 16-bit quantisation. It only switches where the filter barely acts.
    const bool filter = hi_ < kSpecs[HI_CUT].max * 0.98f;

    // Quantiser: `levels` steps per unit, i.e. 2^(bits-1)
    const float levels = powf(2.f, bits_ - 1.f);
    const float inv    = 1.f / levels;
    const float gain   = powf(10.f, drive_ * 0.012f); // 0..100 % = 0..+24 dB
    // At 48 kHz (or within a hair of it) every sample is taken
    const float inc = rate_ >= sr_ * 0.999f ? 1.f : rate_ / sr_;

    const float dry_gain = cosf(mix_ * HALFPI_F);
    const float wet_gain = sinf(mix_ * HALFPI_F);

    for(size_t i = 0; i < size; i++)
    {
        phase_ += inc;
        if(phase_ >= 1.f)
        {
            phase_ -= 1.f;
            if(phase_ >= 1.f) // inc == 1 leaves exactly 1 - 1 = 0
                phase_ = 0.f;
            hold_l_ = roundf(Clamp1(in_l[i] * gain) * levels) * inv;
            hold_r_ = roundf(Clamp1(in_r[i] * gain) * levels) * inv;
        }
        const float wet_l = filter ? lp_l_.Process(hold_l_) : hold_l_;
        const float wet_r = filter ? lp_r_.Process(hold_r_) : hold_r_;
        out_l[i]          = in_l[i] * dry_gain + wet_l * wet_gain;
        out_r[i]          = in_r[i] * dry_gain + wet_r * wet_gain;
    }
}

} // namespace fx
