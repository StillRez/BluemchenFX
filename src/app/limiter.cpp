#include "app/limiter.h"
#include <cmath>

namespace app
{
static constexpr float kAttackSec = 0.0005f;

static inline float DbToLin(float db)
{
    return powf(10.f, db / 20.f);
}

void Limiter::Init(float sample_rate)
{
    sr_  = sample_rate;
    att_ = 1.f - expf(-1.f / (kAttackSec * sr_));
    SetCeilingDb(-1.f);
    SetRelease(0.15f);
    Reset();
}

void Limiter::Reset()
{
    env_     = 0.f;
    gain_    = 1.f;
    gr_gain_ = 1.f;
}

void Limiter::SetCeilingDb(float db)
{
    ceil_ = DbToLin(db);
    thr_  = DbToLin(db - kKneeDb);
}

void Limiter::SetRelease(float seconds)
{
    if(seconds < 0.001f)
        seconds = 0.001f;
    rel_ = expf(-1.f / (seconds * sr_));
}

float Limiter::GainReductionDb() const
{
    const float g = gr_gain_;
    return g >= 1.f ? 0.f : 20.f * log10f(g);
}

void Limiter::Process(float *l, float *r, size_t n)
{
    const float thr   = thr_;
    const float range = ceil_ - thr_; // soft-clip zone, thr..ceiling
    const float inv   = 1.f / range;
    float       env = env_, gain = gain_, lowest = 1.f;

    for(size_t i = 0; i < n; i++)
    {
        // 1. stereo-linked peak envelope -> target gain
        const float al = fabsf(l[i]), ar = fabsf(r[i]);
        const float pk = al > ar ? al : ar;
        env            = pk > env ? pk : env * rel_;
        const float target = env > thr ? thr / env : 1.f;
        // fall fast (attack), rise as fast as the envelope allows (its
        // release already shapes the recovery)
        gain = target < gain ? gain + att_ * (target - gain) : target;
        lowest = gain < lowest ? gain : lowest;

        // 2. gain, then soft clip anything still above the threshold
        float yl = l[i] * gain, yr = r[i] * gain;
        float a  = fabsf(yl);
        if(a > thr)
            yl = copysignf(thr + range * tanhf((a - thr) * inv), yl);
        a = fabsf(yr);
        if(a > thr)
            yr = copysignf(thr + range * tanhf((a - thr) * inv), yr);
        l[i] = yl;
        r[i] = yr;
    }
    env_     = env;
    gain_    = gain;
    gr_gain_ = lowest;
}

} // namespace app
