// What CRUSH, CHORUS and RESON actually do (test_effects.cpp only checks
// that every effect stays stable): quantiser levels, sample-and-hold rate,
// chorus tap position and stereo spread, resonator pitch, decay and level.
#include "check.h"
#include "fx/crush.h"
#include "fx/chorus.h"
#include "fx/reson.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

namespace
{
constexpr float  kSr    = 48000.f;
constexpr size_t kBlock = 32;

/** An effect with its own arena, run in firmware-sized blocks. */
template <typename T>
struct Rig
{
    T                    fx;
    std::vector<uint8_t> mem;
    fx::Arena            arena;
    explicit Rig(std::initializer_list<std::pair<int, float>> real = {})
    : mem(1u << 20)
    {
        arena.Init(mem.data(), mem.size());
        CHECK(fx.Init(kSr, arena));
        for(size_t i = 0; i < fx.NumParams(); i++)
            fx.SetParam(i, fx.Spec(i).def);
        for(auto &p : real)
            fx.SetParam(p.first, fx.Spec(p.first).ToNorm(p.second));
        // let the block-rate smoothing settle, then start from silence
        std::vector<float> z(kSr, 0.f), l(kSr), r(kSr);
        Run(z, z, l, r);
        fx.Reset();
    }
    void Run(const std::vector<float> &il,
             const std::vector<float> &ir,
             std::vector<float>       &ol,
             std::vector<float>       &orr)
    {
        ol.resize(il.size());
        orr.resize(il.size());
        for(size_t i = 0; i + kBlock <= il.size(); i += kBlock)
            fx.Process(&il[i], &ir[i], &ol[i], &orr[i], kBlock);
    }
};

std::vector<float> Sine(float hz, float amp, size_t n)
{
    std::vector<float> v(n);
    for(size_t i = 0; i < n; i++)
        v[i] = amp * sinf(2.f * (float)M_PI * hz * i / kSr);
    return v;
}

std::vector<float> Noise(float amp, size_t n, uint32_t seed = 1)
{
    std::vector<float> v(n);
    for(size_t i = 0; i < n; i++)
    {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        v[i] = amp * ((seed >> 8) * (2.f / 16777216.f) - 1.f);
    }
    return v;
}

float Rms(const std::vector<float> &v, size_t from = 0, size_t to = 0)
{
    if(!to)
        to = v.size();
    double s = 0;
    for(size_t i = from; i < to; i++)
        s += (double)v[i] * v[i];
    return (float)sqrt(s / (to - from));
}

/** Power of v at frequency hz (Goertzel), normalised by length. */
float Tone(const std::vector<float> &v, float hz, size_t from, size_t to)
{
    const double w = 2 * M_PI * hz / kSr, c = 2 * cos(w);
    double       s1 = 0, s2 = 0;
    for(size_t i = from; i < to; i++)
    {
        double s0 = v[i] + c * s1 - s2;
        s2        = s1;
        s1        = s0;
    }
    return (float)((s1 * s1 + s2 * s2 - c * s1 * s2) / ((to - from) * (to - from)));
}
} // namespace

// ================================================================== CRUSH

TEST(crush_quantises_to_the_bit_depth)
{
    using C = fx::Crush;
    for(float bits : {1.f, 2.f, 3.f, 4.f, 8.f})
    {
        Rig<C> rig({{C::BITS, bits}, {C::RATE, 48000.f}, {C::MIX, 100.f}});
        auto   in = Sine(110.f, 0.9f, 9600);
        std::vector<float> l, r;
        rig.Run(in, in, l, r);
        const float   levels = powf(2.f, bits - 1.f);
        std::set<int> seen;
        float         worst = 0.f;
        for(size_t i = 0; i < l.size(); i++)
        {
            float steps = l[i] * levels;
            worst       = fmaxf(worst, fabsf(steps - roundf(steps)));
            seen.insert((int)roundf(steps));
            // and it's the nearest level to the input
            worst = fmaxf(worst, fabsf(l[i] - roundf(in[i] * levels) / levels) * levels);
        }
        CHECKF(worst < 1e-4f, "%g bits: output off the grid by %g steps", bits, worst);
        // a 0.9 sine crosses every level from -0.9 to +0.9
        int want = 2 * (int)roundf(0.9f * levels) + 1;
        CHECKF((int)seen.size() == want, "%g bits: %zu levels used, want %d", bits, seen.size(), want);
    }
}

TEST(crush_holds_samples_at_the_rate)
{
    using C = fx::Crush;
    for(float rate : {48000.f, 12000.f, 1000.f, 300.f})
    {
        Rig<C> rig({{C::BITS, 16.f}, {C::RATE, rate}, {C::MIX, 100.f}});
        auto   in = Noise(0.5f, 48000);
        std::vector<float> l, r;
        rig.Run(in, in, l, r);
        size_t changes = 0;
        for(size_t i = 1; i < l.size(); i++)
            changes += fabsf(l[i] - l[i - 1]) > 1e-5f; // MIX 100 % leaks ~4e-8 dry
        // one new value per tick of the S&H clock (white noise: always new)
        CHECKF(fabsf((float)changes - rate) < rate * 0.01f + 2.f,
               "rate %g: %zu new samples in 1 s",
               rate,
               changes);
    }
}

TEST(crush_16_bit_full_rate_is_nearly_transparent)
{
    using C = fx::Crush;
    Rig<C> rig({{C::BITS, 16.f}, {C::RATE, 48000.f}, {C::MIX, 100.f}});
    auto   in = Sine(1000.f, 0.5f, 4800);
    std::vector<float> l, r;
    rig.Run(in, in, l, r);
    float worst = 0.f;
    for(size_t i = 0; i < l.size(); i++)
        worst = fmaxf(worst, fabsf(l[i] - in[i]));
    CHECKF(worst <= 0.5f / 32768.f + 1e-7f, "differs from the input by %g", worst);
}

TEST(crush_drive_and_hi_cut)
{
    using C = fx::Crush;
    // DRIVE 100 % = +24 dB into a clamp at +-1
    Rig<C> hot({{C::BITS, 16.f}, {C::RATE, 48000.f}, {C::DRIVE, 100.f}, {C::MIX, 100.f}});
    auto   in = Sine(200.f, 0.1f, 9600);
    std::vector<float> l, r;
    hot.Run(in, in, l, r);
    float peak = 0.f;
    for(float v : l)
        peak = fmaxf(peak, fabsf(v));
    CHECKF(peak > 0.99f && peak <= 1.f, "0.1 sine +24 dB peaks at %g", peak);

    // Hi Cut 1 kHz takes most of an 8 kHz tone away
    Rig<C> dark({{C::BITS, 16.f}, {C::RATE, 48000.f}, {C::HI_CUT, 1000.f}, {C::MIX, 100.f}});
    auto   hf = Sine(8000.f, 0.5f, 9600);
    dark.Run(hf, hf, l, r);
    float ratio = Rms(l, 4800) / Rms(hf, 4800);
    CHECKF(ratio < 0.2f, "8 kHz through a 1 kHz Hi Cut keeps %g", ratio);
}

// ================================================================= CHORUS

TEST(chorus_tap_sits_at_delay)
{
    using H = fx::Chorus;
    for(float ms : {1.f, 5.f, 12.f, 30.f})
    {
        Rig<H> rig({{H::DELAY, ms}, {H::DEPTH, 0.f}, {H::FEEDBACK, 0.f}, {H::MIX, 100.f}});
        std::vector<float> in(9600, 0.f), l, r;
        in[64] = 1.f;
        rig.Run(in, in, l, r);
        size_t at = 0;
        for(size_t i = 1; i < l.size(); i++)
            if(fabsf(l[i]) > fabsf(l[at]))
                at = i;
        const float got = (at - 64) * 1000.f / kSr;
        CHECKF(fabsf(got - ms) <= 1000.f / kSr + 1e-3f, "DELAY %g ms: impulse at %g ms", ms, got);
        CHECKF(fabsf(l[at] - 1.f) < 0.02f, "DELAY %g: peak %g, neighbours %g %g", ms, l[at], l[at - 1], l[at + 1]);
    }
}

TEST(chorus_depth_moves_the_tap_and_spread_widens)
{
    using H = fx::Chorus;
    // Impulse train every 50 ms; with DEPTH, arrival times swing by up to
    // +-95 % x DEPTH x DELAY around DELAY
    auto arrivals = [](Rig<H> &rig, std::vector<float> &al, std::vector<float> &ar) {
        std::vector<float> in(48000 * 2, 0.f), l, r;
        for(size_t i = 0; i < in.size(); i += 2400)
            in[i] = 1.f;
        rig.Run(in, in, l, r);
        for(size_t i = 0; i + 2400 <= in.size(); i += 2400)
        {
            size_t bl = i, br = i;
            for(size_t j = i + 1; j < i + 2400; j++)
            {
                if(fabsf(l[j]) > fabsf(l[bl]))
                    bl = j;
                if(fabsf(r[j]) > fabsf(r[br]))
                    br = j;
            }
            al.push_back((bl - i) * 1000.f / kSr);
            ar.push_back((br - i) * 1000.f / kSr);
        }
    };
    Rig<H> mono({{H::DELAY, 10.f}, {H::DEPTH, 50.f}, {H::RATE, 1.f}, {H::SPREAD, 0.f}, {H::MIX, 100.f}});
    std::vector<float> al, ar;
    arrivals(mono, al, ar);
    float lo = 1e9f, hi = 0.f, lr = 0.f;
    for(size_t i = 0; i < al.size(); i++)
    {
        lo = fminf(lo, al[i]);
        hi = fmaxf(hi, al[i]);
        lr = fmaxf(lr, fabsf(al[i] - ar[i]));
    }
    // 10 ms +- 4.75 ms
    CHECKF(lo > 4.5f && lo < 6.f && hi > 14.f && hi < 15.5f, "swing %g..%g ms", lo, hi);
    CHECKF(lr < 0.05f, "SPREAD 0: L and R differ by %g ms", lr);

    Rig<H> wide({{H::DELAY, 10.f}, {H::DEPTH, 50.f}, {H::RATE, 1.f}, {H::SPREAD, 100.f}, {H::MIX, 100.f}});
    al.clear();
    ar.clear();
    arrivals(wide, al, ar);
    lr = 0.f;
    for(size_t i = 0; i < al.size(); i++)
        lr = fmaxf(lr, fabsf(al[i] - ar[i]));
    // 180 degrees apart: L and R up to 2 x 4.75 ms apart
    CHECKF(lr > 8.f, "SPREAD 100: L and R at most %g ms apart", lr);
}

TEST(chorus_feedback_makes_repeats)
{
    using H = fx::Chorus;
    Rig<H> rig({{H::DELAY, 5.f}, {H::DEPTH, 0.f}, {H::FEEDBACK, 80.f}, {H::MIX, 100.f}});
    std::vector<float> in(4800, 0.f), l, r;
    in[0] = 0.25f;
    rig.Run(in, in, l, r);
    // repeats every 5 ms (240 samples), each ~0.8 x the one before
    const float a1 = fabsf(l[240]), a2 = fabsf(l[480]), a3 = fabsf(l[720]);
    CHECKF(fabsf(a2 / a1 - 0.8f) < 0.05f && fabsf(a3 / a2 - 0.8f) < 0.05f,
           "repeats %g %g %g",
           a1,
           a2,
           a3);
}

// ================================================================== RESON

TEST(reson_rings_at_pitch)
{
    using S = fx::Reson;
    for(float hz : {55.f, 110.f, 220.f, 440.f, 1000.f})
    {
        // harmonic STRUCT; dark, so the fundamental dominates
        Rig<S> rig({{S::PITCH, hz}, {S::STRUCT, 27.f}, {S::BRIGHT, 0.f}, {S::DECAY, 2.f}, {S::MIX, 100.f}});
        std::vector<float> in(48000, 0.f), l, r;
        in[0] = 1.f;
        rig.Run(in, in, l, r);
        const float at  = Tone(l, hz, 4800, 48000);
        const float off = fmaxf(Tone(l, hz * 0.94f, 4800, 48000), Tone(l, hz * 1.06f, 4800, 48000));
        CHECKF(at > 30.f * off, "%g Hz: on-pitch %g vs 6 %% off %g", hz, at, off);
        // with BRIGHT up, the harmonics ring too
        Rig<S> bright({{S::PITCH, hz}, {S::STRUCT, 27.f}, {S::BRIGHT, 100.f}, {S::DECAY, 2.f}, {S::MIX, 100.f}});
        bright.Run(in, in, l, r);
        if(hz * 3 < 20000.f)
        {
            const float h3 = Tone(l, hz * 3.f, 4800, 48000);
            const float n3 = Tone(l, hz * 3.2f, 4800, 48000);
            CHECKF(h3 > 30.f * n3, "%g Hz: 3rd harmonic %g vs off %g", hz, h3, n3);
        }
    }
}

TEST(reson_decay_is_rt60)
{
    using S = fx::Reson;
    for(float t60 : {0.3f, 1.f, 3.f})
    {
        Rig<S> rig({{S::PITCH, 220.f}, {S::BRIGHT, 0.f}, {S::DECAY, t60}, {S::MIX, 100.f}});
        std::vector<float> in((size_t)(kSr * (t60 * 1.2f + 0.3f)), 0.f), l, r;
        in[0] = 1.f;
        rig.Run(in, in, l, r);
        // The fundamental's level in 50 ms windows (the upper partials die
        // sooner at BRIGHT 0, so the total output would mislead): fit the
        // slope between -5 and -45 dB of its first window
        const size_t w   = 2400;
        const float  ref = Tone(l, 220.f, 0, w);
        float        t5 = -1, t45 = -1;
        for(size_t i = 0; i + w <= l.size(); i += w)
        {
            float db = 10.f * log10f(Tone(l, 220.f, i, i + w) / ref + 1e-20f);
            if(t5 < 0 && db < -5.f)
                t5 = i / kSr;
            if(t45 < 0 && db < -45.f)
                t45 = i / kSr;
        }
        CHECKF(t5 > 0 && t45 > 0, "T60 %g: fundamental never fell 45 dB", t60);
        const float got = (t45 - t5) * 1.5f; // 40 dB -> 60 dB
        CHECKF(fabsf(got / t60 - 1.f) < 0.15f, "DECAY %g s: fundamental rings for %g s", t60, got);
    }
}

TEST(reson_levels)
{
    using S = fx::Reson;
    // Broadband input: wet level about the same as the input, for short
    // and long DECAY alike
    for(float t60 : {0.2f, 1.2f, 8.f})
    {
        Rig<S> rig({{S::DECAY, t60}, {S::MIX, 100.f}});
        auto   in = Noise(0.25f, 96000);
        std::vector<float> l, r;
        rig.Run(in, in, l, r);
        const float g = Rms(l, 48000) / Rms(in, 48000);
        std::printf("      RESON noise gain at DECAY %.1f s: %.2f (%.1f dB)\n", t60, g, 20 * log10f(g));
        CHECKF(g > 0.5f && g < 2.f, "DECAY %g: noise comes out at x%g", t60, g);
    }
    // A sustained note right on the pitch rings up, but the soft clip keeps
    // the wet signal within +-2
    Rig<S> rig({{S::PITCH, 220.f}, {S::DECAY, 10.f}, {S::BRIGHT, 100.f}, {S::MIX, 100.f}});
    auto   in = Sine(220.f, 0.5f, 96000);
    std::vector<float> l, r;
    rig.Run(in, in, l, r);
    float peak = 0.f;
    for(size_t i = 0; i < l.size(); i++)
        peak = fmaxf(peak, fmaxf(fabsf(l[i]), fabsf(r[i])));
    std::printf("      RESON on-pitch sine 0.5 -> peak %.2f\n", peak);
    CHECKF(peak <= 2.001f, "on-pitch sine peaks at %g", peak);
}

TEST(reson_mono_input_comes_out_wide)
{
    using S = fx::Reson;
    Rig<S> rig({{S::BRIGHT, 80.f}, {S::MIX, 100.f}});
    auto   in = Noise(0.25f, 48000);
    std::vector<float> l, r;
    rig.Run(in, in, l, r);
    double lr = 0, ll = 0, rr = 0;
    for(size_t i = 4800; i < l.size(); i++)
    {
        lr += (double)l[i] * r[i];
        ll += (double)l[i] * l[i];
        rr += (double)r[i] * r[i];
    }
    const float corr = (float)(lr / sqrt(ll * rr));
    std::printf("      RESON L/R correlation from mono noise: %.2f\n", corr);
    CHECKF(fabsf(corr) < 0.8f, "L/R correlation %g: not much width", corr);
}
