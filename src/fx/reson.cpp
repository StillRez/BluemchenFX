#include "fx/reson.h"

using namespace daisysp;

namespace fx
{
// name       min      max       default  curve               unit
const ParamSpec Reson::kSpecs[NUM_PARAMS] = {
    {"PITCH", 30.f, 2000.f, 0.309f, Curve::Exponential, Unit::Hertz}, // 110 Hz
    {"STRUCT", 0.f, 100.f, 0.27f, Curve::Linear, Unit::Percent},      // harmonic
    {"BRIGHT", 0.f, 100.f, 0.6f, Curve::Linear, Unit::Percent},
    {"DECAY", 0.05f, 10.f, 0.6f, Curve::Exponential, Unit::Seconds},  // ~1.2 s
    {"MIX", 0.f, 100.f, 0.5f, Curve::Linear, Unit::Percent},
};

// Overall wet level: white noise in comes out at roughly the same RMS at the
// default settings (measured on the host, tests/test_effects.cpp prints it).
static constexpr float kWetScale = 1.f;
// Modes at or above this normalised frequency are silent (tan() blows up
// towards 0.5, and they'd only alias).
static constexpr float kMaxModeFreq = 0.45f;

// Gentle saturation with headroom: transparent below ~0.3, limits at +-2.
static inline float Saturate(float x)
{
    return 2.f * SoftClip(0.5f * x);
}

/** STRUCT 0..1 -> partial stiffness (Rings' curve, via DaisySP Resonator):
 *  < 0.25 squeezes the partials, 0.25-0.3 is harmonic, above that they
 *  stretch, steeply in the last 10 %. */
static float Stiffness(float s)
{
    if(s < 0.25f)
        return -(0.25f - s) * 0.25f;
    if(s < 0.3f)
        return 0.f;
    if(s < 0.9f)
        return (s - 0.3f) / 0.6f;
    float t = (s - 0.9f) * 10.f;
    return 1.5f - cosf(t * t * PI_F) * 0.5f;
}

/** Factor that keeps the n-th partial at n x PITCH despite the stretch. */
static float HarmonicCompensation(int n, float stiffness)
{
    float stretch = 1.f;
    for(int i = 0; i < n - 1; i++)
    {
        stretch += stiffness;
        stiffness *= stiffness < 0.f ? 0.93f : 0.98f;
    }
    return 1.f / stretch;
}

bool Reson::Init(float sample_rate, Arena &mem)
{
    (void)mem; // no buffers
    sr_ = sample_rate;
    for(size_t i = 0; i < NUM_PARAMS; i++)
        target_[i] = kSpecs[i].ToReal(kSpecs[i].def);
    Reset();
    return true;
}

void Reson::SnapSmoothing()
{
    oct_    = log2f(target_[PITCH]);
    struct_ = target_[STRUCT] * 0.01f;
    bright_ = target_[BRIGHT] * 0.01f;
    decay_  = target_[DECAY];
    mix_    = target_[MIX] * 0.01f;
}

void Reson::Reset()
{
    for(int c = 0; c < 2; c++)
        for(int i = 0; i < kModes; i++)
            s1_[c][i] = s2_[c][i] = 0.f;
    SnapSmoothing();
    UpdateModes();
}

void Reson::SetParam(size_t idx, float norm)
{
    if(idx < NUM_PARAMS)
        target_[idx] = kSpecs[idx].ToReal(norm);
}

void Reson::UpdateModes()
{
    float       stiffness = Stiffness(struct_);
    const float f0 = exp2f(oct_) / sr_ * HarmonicCompensation(3, stiffness);
    // Q that gives the fundamental an RT60 of DECAY: a band-pass of quality
    // Q at f (cycles/sample) decays with time constant Q / (pi f) samples.
    float       q      = decay_ * sr_ * PI_F / 6.908f;
    const float q_loss = 0.15f + 0.85f * bright_ * (2.f - bright_);

    float harmonic = f0, stretch = 1.f;
    active_ = 0;
    for(int i = 0; i < kModes; i++)
    {
        float f = harmonic * stretch;
        if(f < kMaxModeFreq && f > 0.f)
        {
            const float Q = 1.f + f * q;
            const float g = tanf(PI_F * f);
            const float k = 1.f / Q;
            g_[i]         = g;
            a1_[i]        = 1.f / (1.f + g * (g + k));
            // sqrt(k): equal broadband level for any Q; fade out near Nyquist
            gain_[i] = kWetScale * sqrtf(k) * (1.f - f * 2.f);
            active_  = i + 1;
        }
        else
        {
            // keep the filter well-defined but silent
            g_[i]    = tanf(PI_F * kMaxModeFreq);
            a1_[i]   = 1.f / (1.f + g_[i] * (g_[i] + 1.f));
            gain_[i] = 0.f;
        }
        stretch += stiffness;
        stiffness *= stiffness < 0.f ? 0.93f : 0.98f;
        harmonic += f0;
        q *= q_loss;
    }
    // Modes above the active range aren't processed: clear them, so one that
    // comes back (pitch turned down) doesn't resume an old ring
    for(int i = active_; i < kModes; i++)
        s1_[0][i] = s2_[0][i] = s1_[1][i] = s2_[1][i] = 0.f;
}

void Reson::Process(const float *in_l,
                    const float *in_r,
                    float       *out_l,
                    float       *out_r,
                    size_t       size)
{
    const float k = 0.08f;
    fonepole(oct_, log2f(target_[PITCH]), 0.15f);
    fonepole(struct_, target_[STRUCT] * 0.01f, k);
    fonepole(bright_, target_[BRIGHT] * 0.01f, k);
    fonepole(decay_, target_[DECAY], k);
    fonepole(mix_, target_[MIX] * 0.01f, k);
    UpdateModes();

    const float dry_gain = cosf(mix_ * HALFPI_F);
    const float wet_gain = sinf(mix_ * HALFPI_F);
    const int   n        = active_;

    for(size_t i = 0; i < size; i++)
    {
        const float xl = in_l[i], xr = in_r[i];
        float       yl = 0.f, yr = 0.f;
        for(int m = 0; m < n; m++)
        {
            const float g = g_[m], a1 = a1_[m];
            // zero-delay-feedback SVF (Simper), band-pass output v1
            float v1l  = a1 * (s1_[0][m] + g * (xl - s2_[0][m]));
            float v2l  = s2_[0][m] + g * v1l;
            s1_[0][m]  = 2.f * v1l - s1_[0][m];
            s2_[0][m]  = 2.f * v2l - s2_[0][m];
            float v1r  = a1 * (s1_[1][m] + g * (xr - s2_[1][m]));
            float v2r  = s2_[1][m] + g * v1r;
            s1_[1][m]  = 2.f * v1r - s1_[1][m];
            s2_[1][m]  = 2.f * v2r - s2_[1][m];
            const float w = gain_[m];
            yl += w * v1l;
            yr += (m & 1) ? -w * v1r : w * v1r; // right: odd partials flipped
        }
        out_l[i] = xl * dry_gain + Saturate(yl) * wet_gain;
        out_r[i] = xr * dry_gain + Saturate(yr) * wet_gain;
    }
}

} // namespace fx
