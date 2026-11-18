#include "fx/delay.h"
#include "dev/sdram.h"

using namespace daisysp;

namespace fx
{
// 2 s at 48 kHz plus headroom for the interpolating read
static constexpr size_t kMaxDelaySamples = 96000 + 256;

// ~386 KB per channel: SDRAM
static DelayLine<float, kMaxDelaySamples> DSY_SDRAM_BSS g_line_l;
static DelayLine<float, kMaxDelaySamples> DSY_SDRAM_BSS g_line_r;

// name       min      max      default  curve               unit
const ParamSpec Delay::kSpecs[NUM_PARAMS] = {
    {"TIME", 0.01f, 2.f, 0.68f, Curve::Exponential, Unit::Seconds},  // ~0.37 s
    {"FEEDBK", 0.f, 95.f, 0.42f, Curve::Linear, Unit::Percent},      // 40 %
    {"MIX", 0.f, 100.f, 0.35f, Curve::Linear, Unit::Percent},
    {"PING", 0.f, 100.f, 0.f, Curve::Linear, Unit::Percent},
    {"LO CUT", 20.f, 1000.f, 0.28f, Curve::Exponential, Unit::Hertz}, // ~60 Hz
    {"HI CUT", 500.f, 18000.f, 0.70f, Curve::Exponential, Unit::Hertz},
};

// Gentle saturation with headroom: transparent below ~0.3, limits at +-2.
static inline float Saturate(float x)
{
    return 2.f * SoftClip(0.5f * x);
}

void Delay::Init(float sample_rate)
{
    sr_ = sample_rate;
    for(size_t i = 0; i < NUM_PARAMS; i++)
        target_[i] = kSpecs[i].ToReal(kSpecs[i].def);
    Reset();
}

void Delay::InitFilters()
{
    hp_l_.Init();
    hp_r_.Init();
    lp_l_.Init();
    lp_r_.Init();
    hp_l_.SetFilterMode(OnePole::FILTER_MODE_HIGH_PASS);
    hp_r_.SetFilterMode(OnePole::FILTER_MODE_HIGH_PASS);
    lp_l_.SetFilterMode(OnePole::FILTER_MODE_LOW_PASS);
    lp_r_.SetFilterMode(OnePole::FILTER_MODE_LOW_PASS);
}

void Delay::SetFilterFreqs()
{
    const float lo = lo_ / sr_, hi = hi_ / sr_; // normalised frequency
    hp_l_.SetFrequency(lo);
    hp_r_.SetFrequency(lo);
    lp_l_.SetFrequency(hi);
    lp_r_.SetFrequency(hi);
}

void Delay::SnapSmoothing()
{
    delay_samps_ = target_[TIME] * sr_;
    fb_          = target_[FEEDBACK] * 0.01f;
    mix_         = target_[MIX] * 0.01f;
    ping_        = target_[PING] * 0.01f;
    lo_          = target_[LO_CUT];
    hi_          = target_[HI_CUT];
}

void Delay::Reset()
{
    g_line_l.Init(); // zeroes the buffer
    g_line_r.Init();
    InitFilters();
    SnapSmoothing();
    SetFilterFreqs();
}

void Delay::SetParam(size_t idx, float norm)
{
    if(idx < NUM_PARAMS)
        target_[idx] = kSpecs[idx].ToReal(norm);
}

void Delay::Process(const float *in_l,
                    const float *in_r,
                    float       *out_l,
                    float       *out_r,
                    size_t       size)
{
    // Block-rate smoothing for everything except time
    const float k = 0.08f;
    fonepole(fb_, target_[FEEDBACK] * 0.01f, k);
    fonepole(mix_, target_[MIX] * 0.01f, k);
    fonepole(ping_, target_[PING] * 0.01f, k);
    fonepole(lo_, target_[LO_CUT], k);
    fonepole(hi_, target_[HI_CUT], k);
    SetFilterFreqs();

    const float dry_gain = cosf(mix_ * HALFPI_F);
    const float wet_gain = sinf(mix_ * HALFPI_F);
    const float straight = 1.f - ping_;

    // Per-sample time glide: ~50 ms time constant
    const float target_samps = target_[TIME] * sr_;
    const float glide        = 1.f / (0.05f * sr_);

    for(size_t i = 0; i < size; i++)
    {
        delay_samps_ += (target_samps - delay_samps_) * glide;
        g_line_l.SetDelay(delay_samps_);
        g_line_r.SetDelay(delay_samps_);

        // Read and filter the delayed signal (feeds both wet out and feedback)
        const float wet_l = lp_l_.Process(hp_l_.Process(g_line_l.Read()));
        const float wet_r = lp_r_.Process(hp_r_.Process(g_line_r.Read()));

        // Ping-pong: feedback crosses over, input moves into the left line
        const float fb_l = straight * wet_l + ping_ * wet_r;
        const float fb_r = straight * wet_r + ping_ * wet_l;
        const float mono = 0.5f * (in_l[i] + in_r[i]);
        const float send_l = straight * in_l[i] + ping_ * mono;
        const float send_r = straight * in_r[i];

        g_line_l.Write(Saturate(send_l + fb_ * fb_l));
        g_line_r.Write(Saturate(send_r + fb_ * fb_r));

        out_l[i] = in_l[i] * dry_gain + wet_l * wet_gain;
        out_r[i] = in_r[i] * dry_gain + wet_r * wet_gain;
    }
}

} // namespace fx
