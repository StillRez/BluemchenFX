#include "fx/chorus.h"

using namespace daisysp;

namespace fx
{
// name       min      max      default  curve               unit
const ParamSpec Chorus::kSpecs[NUM_PARAMS] = {
    {"RATE", 0.05f, 8.f, 0.4537f, Curve::Exponential, Unit::Hertz},  // 0.5 Hz
    {"DEPTH", 0.f, 100.f, 0.3f, Curve::Linear, Unit::Percent},
    {"DELAY", 1.f, 30.f, 0.7306f, Curve::Exponential, Unit::Millis}, // 12 ms
    {"FEEDBK", 0.f, 90.f, 0.f, Curve::Linear, Unit::Percent},
    {"SPREAD", 0.f, 100.f, 0.5f, Curve::Linear, Unit::Percent},
    {"MIX", 0.f, 100.f, 0.5f, Curve::Linear, Unit::Percent},
};

// Gentle saturation with headroom: transparent below ~0.3, limits at +-2.
static inline float Saturate(float x)
{
    return 2.f * SoftClip(0.5f * x);
}

/** Triangle shaped into a near-sine, phase 0..1 -> -1..1 (sin(2*pi*p)). */
static inline float Lfo(float p)
{
    // triangle with the same phase as sin: 0 at p = 0, +1 at 0.25
    float t = p < 0.25f ? 4.f * p : (p < 0.75f ? 2.f - 4.f * p : 4.f * p - 4.f);
    return t * (1.5f - 0.5f * t * t); // cubic soft shape, <1 % THD-ish
}

bool Chorus::Init(float sample_rate, Arena &mem)
{
    sr_     = sample_rate;
    line_l_ = mem.New<Line>();
    line_r_ = mem.New<Line>();
    if(!line_l_ || !line_r_)
        return false;
    for(size_t i = 0; i < NUM_PARAMS; i++)
        target_[i] = kSpecs[i].ToReal(kSpecs[i].def);
    Reset();
    return true;
}

void Chorus::SnapSmoothing()
{
    depth_  = target_[DEPTH] * 0.01f;
    delay_  = target_[DELAY] * 0.001f * sr_;
    fb_     = target_[FEEDBACK] * 0.01f;
    spread_ = target_[SPREAD] * 0.01f;
    mix_    = target_[MIX] * 0.01f;
}

void Chorus::Reset()
{
    line_l_->Init(); // zeroes the buffer
    line_r_->Init();
    phase_ = 0.f;
    SnapSmoothing();
}

void Chorus::SetParam(size_t idx, float norm)
{
    if(idx < NUM_PARAMS)
        target_[idx] = kSpecs[idx].ToReal(norm);
}

void Chorus::Process(const float *in_l,
                     const float *in_r,
                     float       *out_l,
                     float       *out_r,
                     size_t       size)
{
    // Block-rate smoothing. DELAY and DEPTH move the tap, so they get a
    // slower one (~30 ms) to glide rather than step.
    fonepole(depth_, target_[DEPTH] * 0.01f, 0.03f);
    fonepole(delay_, target_[DELAY] * 0.001f * sr_, 0.03f);
    fonepole(fb_, target_[FEEDBACK] * 0.01f, 0.08f);
    fonepole(spread_, target_[SPREAD] * 0.01f, 0.08f);
    fonepole(mix_, target_[MIX] * 0.01f, 0.08f);

    const float inc      = target_[RATE] / sr_; // LFO rate needs no smoothing
    const float swing    = depth_ * 0.95f * delay_;
    const float offset   = 0.5f * spread_; // right LFO lag, in cycles
    const float dry_gain = cosf(mix_ * HALFPI_F);
    const float wet_gain = sinf(mix_ * HALFPI_F);

    for(size_t i = 0; i < size; i++)
    {
        phase_ += inc;
        if(phase_ >= 1.f)
            phase_ -= 1.f;
        float pr = phase_ - offset;
        if(pr < 0.f)
            pr += 1.f;

        const float d_l   = delay_ + swing * Lfo(phase_);
        const float d_r   = delay_ + swing * Lfo(pr);
        const float wet_l = line_l_->ReadHermite(d_l);
        const float wet_r = line_r_->ReadHermite(d_r);

        // Only the feedback is saturated: the input goes in clean, and the
        // loop stays bounded (each pass adds at most +-2)
        line_l_->Write(in_l[i] + Saturate(fb_ * wet_l));
        line_r_->Write(in_r[i] + Saturate(fb_ * wet_r));

        out_l[i] = in_l[i] * dry_gain + wet_l * wet_gain;
        out_r[i] = in_r[i] * dry_gain + wet_r * wet_gain;
    }
}

} // namespace fx
