#include "fx/reverb.h"

using namespace daisysp;

namespace fx
{
// Mean loop delay of the ReverbSc tank (8 lines, 1933..4127 samples @ 48k).
static constexpr float kMeanLoopDelaySec = 3015.5f / 48000.f;

// name      min     max       default  curve               unit
const ParamSpec Reverb::kSpecs[NUM_PARAMS] = {
    {"DECAY", 0.2f, 30.f, 0.45f, Curve::Exponential, Unit::Seconds},
    {"MIX", 0.f, 100.f, 0.35f, Curve::Linear, Unit::Percent},
    {"LO CUT", 20.f, 1000.f, 0.15f, Curve::Exponential, Unit::Hertz},
    {"HI CUT", 500.f, 18000.f, 0.70f, Curve::Exponential, Unit::Hertz},
};

static inline float DecayToFeedback(float rt60)
{
    // Each pass through a loop must attenuate by 60 dB / (rt60 / delay)
    float fb = expf(-6.9077553f * kMeanLoopDelaySec / rt60);
    return fb > 0.999f ? 0.999f : fb;
}

bool Reverb::Init(float sample_rate, Arena &mem)
{
    sr_   = sample_rate;
    tank_ = mem.New<ReverbSc>(); // ~400 KB of delay memory
    if(!tank_)
        return false;
    for(size_t i = 0; i < NUM_PARAMS; i++)
        target_[i] = kSpecs[i].ToReal(kSpecs[i].def);
    Reset();
    return true;
}

void Reverb::Reset()
{
    tank_->Init(sr_); // clears the tank (~400 KB)
    hp_l_.Init(sr_);
    hp_r_.Init(sr_);
    hp_l_.SetRes(0.f);
    hp_r_.SetRes(0.f);

    fb_  = DecayToFeedback(target_[DECAY]);
    mix_ = target_[MIX] * 0.01f;
    lo_  = target_[LO_CUT];
    hi_  = target_[HI_CUT];
}

void Reverb::SetParam(size_t idx, float norm)
{
    if(idx < NUM_PARAMS)
        target_[idx] = kSpecs[idx].ToReal(norm);
}

void Reverb::Process(const float *in_l,
                     const float *in_r,
                     float       *out_l,
                     float       *out_r,
                     size_t       size)
{
    // Block-rate parameter smoothing (~10 ms at 32-sample blocks)
    const float k = 0.08f;
    fonepole(fb_, DecayToFeedback(target_[DECAY]), k);
    fonepole(mix_, target_[MIX] * 0.01f, k);
    fonepole(lo_, target_[LO_CUT], k);
    fonepole(hi_, target_[HI_CUT], k);

    tank_->SetFeedback(fb_);
    tank_->SetLpFreq(hi_);
    hp_l_.SetFreq(lo_);
    hp_r_.SetFreq(lo_);

    // Equal-power crossfade
    const float dry_gain = cosf(mix_ * HALFPI_F);
    const float wet_gain = sinf(mix_ * HALFPI_F);

    for(size_t i = 0; i < size; i++)
    {
        hp_l_.Process(in_l[i]);
        hp_r_.Process(in_r[i]);
        float send_l = hp_l_.High();
        float send_r = hp_r_.High();

        float wet_l, wet_r;
        tank_->Process(send_l, send_r, &wet_l, &wet_r);

        out_l[i] = in_l[i] * dry_gain + wet_l * wet_gain;
        out_r[i] = in_r[i] * dry_gain + wet_r * wet_gain;
    }
}

} // namespace fx
