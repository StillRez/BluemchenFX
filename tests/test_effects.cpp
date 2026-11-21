// DSP stability tests: every effect, at parameter extremes and random
// settings, fed with nasty signals, must stay finite and bounded, and
// Reset() must leave it silent. This is the test that would have caught
// the Svf-in-the-feedback-loop blow-up in the delay.
//
// To cover a new effect, add it to AllEffects() below.
#include "check.h"
#include "fx/reverb.h"
#include "fx/delay.h"
#include "fx/crush.h"
#include "fx/chorus.h"
#include "fx/reson.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <chrono>

namespace
{
constexpr float  kSr        = 48000.f;
constexpr size_t kBlock     = 32;     // same as the firmware
// Runaway detection. A long reverb fed seconds of full-scale input can
// legitimately get loud (energy piles up in the tank), so loudness alone
// isn't a failure; what marks instability is NaN/Inf, absurd levels, or
// output that keeps growing after the input has stopped.
constexpr float  kMaxOutput = 32.f;

fx::Reverb g_reverb;
fx::Delay  g_delay;
fx::Crush  g_crush;
fx::Chorus g_chorus;
fx::Reson  g_reson;

struct Entry
{
    fx::Effect *fx;
    bool        inited;
};
Entry g_effects[] = {{&g_reverb, false},
                     {&g_delay, false},
                     {&g_crush, false},
                     {&g_chorus, false},
                     {&g_reson, false}};

// Each effect gets its own "SDRAM" arena, as in a chain slot
constexpr size_t kArenaBytes = 8u << 20;

fx::Effect &Get(size_t i)
{
    if(!g_effects[i].inited)
    {
        static fx::Arena arenas[sizeof(g_effects) / sizeof(g_effects[0])];
        arenas[i].Init(new uint8_t[kArenaBytes], kArenaBytes);
        bool ok = g_effects[i].fx->Init(kSr, arenas[i]);
        CHECKF(ok, "%s: Init failed (arena %zu bytes)", g_effects[i].fx->Name(), kArenaBytes);
        g_effects[i].inited = true;
    }
    return *g_effects[i].fx;
}
constexpr size_t kNumEffects = sizeof(g_effects) / sizeof(g_effects[0]);

// Deterministic PRNG so failures reproduce
struct Rng
{
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1) {}
    uint32_t Next()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float Uni() { return (Next() >> 8) * (1.f / 16777216.f); } // 0..1
    float Bi() { return Uni() * 2.f - 1.f; }                   // -1..1
};

enum Signal
{
    SIG_IMPULSE, // single full-scale click, then silence
    SIG_NOISE,   // full-scale white noise, then silence
    SIG_SINE,    // full-scale 1 kHz sine, then silence
    SIG_SQUARE,  // full-scale 50 Hz square (DC-ish edges), then silence
    SIG_LAST
};
const char *kSignalName[SIG_LAST] = {"impulse", "noise", "sine", "square"};

/** Fills one block of the test signal. Sound for the first `on` samples. */
void MakeSignal(Signal sig, size_t pos, size_t on, Rng &rng, float *l, float *r)
{
    for(size_t i = 0; i < kBlock; i++)
    {
        const size_t n = pos + i;
        float        x = 0.f;
        if(n < on)
        {
            switch(sig)
            {
                case SIG_IMPULSE: x = n == 0 ? 1.f : 0.f; break;
                case SIG_NOISE: x = rng.Bi(); break;
                case SIG_SINE: x = sinf(6.2831853f * 1000.f * n / kSr); break;
                case SIG_SQUARE: x = (n / 480) % 2 ? 1.f : -1.f; break;
                default: break;
            }
        }
        l[i] = x;
        r[i] = sig == SIG_NOISE ? rng.Bi() * (n < on) : x; // decorrelated L/R
    }
}

struct Stats
{
    bool   finite = true;
    float  peak   = 0.f;
    double early  = 0.0; // energy in the 0.25 s right after the input stops
    double late   = 0.0; // energy in the last 0.25 s
    void   Add(const float *x, size_t n)
    {
        for(size_t i = 0; i < n; i++)
        {
            if(!std::isfinite(x[i]))
                finite = false;
            else if(fabsf(x[i]) > peak)
                peak = fabsf(x[i]);
        }
    }
    static double Energy(const float *x, size_t n)
    {
        double e = 0.0;
        for(size_t i = 0; i < n; i++)
            e += (double)x[i] * x[i];
        return e;
    }
    /** The tail must not grow (5 % slack for beating between echoes). */
    bool Decays() const { return late <= early * 1.05 + 1e-9; }
};

/** Runs `seconds` of the signal (sound for the first `on_seconds`). */
Stats Run(fx::Effect &e, Signal sig, float seconds, float on_seconds, uint32_t seed)
{
    Rng          rng(seed);
    float        il[kBlock], ir[kBlock], ol[kBlock], orr[kBlock];
    Stats        st;
    const size_t total = (size_t)(seconds * kSr);
    const size_t on    = (size_t)(on_seconds * kSr);
    for(size_t pos = 0; pos < total; pos += kBlock)
    {
        MakeSignal(sig, pos, on, rng, il, ir);
        e.Process(il, ir, ol, orr, kBlock);
        st.Add(ol, kBlock);
        st.Add(orr, kBlock);
        const double en = Stats::Energy(ol, kBlock) + Stats::Energy(orr, kBlock);
        const size_t win = (size_t)(0.25f * kSr);
        if(pos >= on && pos < on + win)
            st.early += en;
        if(pos + win >= total)
            st.late += en;
    }
    return st;
}

void SetAll(fx::Effect &e, const float *norm)
{
    for(size_t i = 0; i < e.NumParams(); i++)
        e.SetParam(i, norm[i]);
}

/** Lets the effect's parameter smoothing reach its targets. */
void Settle(fx::Effect &e, const float *norm)
{
    float z[kBlock] = {}, ol[kBlock], orr[kBlock];
    for(int b = 0; b < 1500; b++) // 1 s
    {
        SetAll(e, norm);
        e.Process(z, z, ol, orr, kBlock);
    }
    e.Reset();
}

} // namespace

// ------------------------------------------------------------------ tests

TEST(effects_have_sane_param_tables)
{
    for(size_t k = 0; k < kNumEffects; k++)
    {
        fx::Effect &e = Get(k);
        CHECKF(e.NumParams() >= 1 && e.NumParams() <= 8, "%s", e.Name());
        CHECKF(std::strlen(e.Name()) <= 11, "%s name too long", e.Name());
        for(size_t i = 0; i < e.NumParams(); i++)
        {
            const fx::ParamSpec &s = e.Spec(i);
            CHECKF(std::strlen(s.name) <= 7, "%s.%s", e.Name(), s.name);
            CHECKF(s.max > s.min, "%s.%s", e.Name(), s.name);
            CHECKF(s.def >= 0.f && s.def <= 1.f, "%s.%s", e.Name(), s.name);
            if(s.curve == fx::Curve::Exponential)
                CHECKF(s.min > 0.f, "%s.%s exp curve needs min > 0", e.Name(), s.name);
            // ToNorm(ToReal(x)) == x
            for(float n = 0.f; n <= 1.f; n += 0.125f)
                CHECKF(fabsf(s.ToNorm(s.ToReal(n)) - n) < 1e-4f,
                       "%s.%s n=%.3f",
                       e.Name(),
                       s.name,
                       n);
        }
    }
}

// Defaults, all-min, all-max, then random settings, each with every signal.
TEST(effects_stable_at_extremes_and_random_settings)
{
    float peak_seen[kNumEffects] = {};
    for(size_t k = 0; k < kNumEffects; k++)
    {
        fx::Effect &e = Get(k);
        const size_t np = e.NumParams();
        Rng          rng(1234 + (uint32_t)k);

        constexpr int kConfigs = 3 + 12;
        for(int c = 0; c < kConfigs; c++)
        {
            float norm[8];
            for(size_t i = 0; i < np; i++)
                norm[i] = c == 0   ? e.Spec(i).def
                          : c == 1 ? 0.f
                          : c == 2 ? 1.f
                          // random, biased towards the edges (where bugs live)
                          : (rng.Uni() < 0.3f ? (rng.Uni() < 0.5f ? 0.f : 1.f)
                                              : rng.Uni());
            Settle(e, norm);
            for(int s = 0; s < SIG_LAST; s++)
            {
                e.Reset();
                SetAll(e, norm);
                Stats st = Run(e, (Signal)s, 4.f, 2.f, 99 + c);
                CHECKF(st.finite && st.peak < kMaxOutput && st.Decays(),
                       "%s config %d (%s) signal %s: finite=%d peak=%.2f "
                       "tail energy %.3g -> %.3g",
                       e.Name(),
                       c,
                       c == 0 ? "defaults" : c == 1 ? "all min" : c == 2 ? "all max" : "random",
                       kSignalName[s],
                       st.finite,
                       st.peak,
                       st.early,
                       st.late);
                if(st.peak > peak_seen[k])
                    peak_seen[k] = st.peak;
            }
        }
    }
    for(size_t k = 0; k < kNumEffects; k++)
        std::printf("      %-8s loudest output %.2f (full-scale input)\n",
                    Get(k).Name(),
                    peak_seen[k]);
}

// Every parameter changes every block (like a fast LFO on CV) while noise
// plays: smoothing and coefficient updates must stay stable.
TEST(effects_stable_under_param_modulation)
{
    for(size_t k = 0; k < kNumEffects; k++)
    {
        fx::Effect &e = Get(k);
        Rng         rng(777 + (uint32_t)k);
        e.Reset();
        float il[kBlock], ir[kBlock], ol[kBlock], orr[kBlock];
        Stats st;
        for(int b = 0; b < (int)(20.f * kSr / kBlock); b++) // 20 s
        {
            // first half: random jumps; second half: slam between 0 and 1
            for(size_t i = 0; i < e.NumParams(); i++)
                e.SetParam(i, b < 15000 ? rng.Uni() : ((b / 8) & 1 ? 1.f : 0.f));
            for(size_t i = 0; i < kBlock; i++)
            {
                il[i] = rng.Bi();
                ir[i] = rng.Bi();
            }
            e.Process(il, ir, ol, orr, kBlock);
            st.Add(ol, kBlock);
            st.Add(orr, kBlock);
        }
        CHECKF(st.finite && st.peak < kMaxOutput,
               "%s: finite=%d peak=%.2f",
               e.Name(),
               st.finite,
               st.peak);
    }
}

// After Reset(), silence in must give silence out: no stale tails/echoes.
TEST(effects_reset_clears_tails)
{
    for(size_t k = 0; k < kNumEffects; k++)
    {
        fx::Effect &e = Get(k);
        float       norm[8];
        for(size_t i = 0; i < e.NumParams(); i++)
            norm[i] = 1.f; // longest decay / most feedback / fully wet
        Settle(e, norm);
        SetAll(e, norm);
        Run(e, SIG_NOISE, 1.f, 1.f, 5); // fill buffers
        e.Reset();
        SetAll(e, norm);
        Stats st = Run(e, SIG_IMPULSE, 0.5f, 0.f, 5); // silence
        CHECKF(st.finite && st.peak < 1e-4f,
               "%s: output after Reset = %.6f",
               e.Name(),
               st.peak);
    }
}

// Not a pass/fail test: host CPU time per block for each effect, as a
// relative cost indicator. The real numbers come from the CPU page on
// the module (the STM32 is far slower than a PC).
TEST(effects_host_cost_report)
{
    for(size_t k = 0; k < kNumEffects; k++)
    {
        fx::Effect &e = Get(k);
        e.Reset();
        float norm[8];
        for(size_t i = 0; i < e.NumParams(); i++)
            norm[i] = e.Spec(i).def;
        SetAll(e, norm);
        Rng   rng(3);
        float il[kBlock], ir[kBlock], ol[kBlock], orr[kBlock];
        for(size_t i = 0; i < kBlock; i++)
        {
            il[i] = rng.Bi() * 0.5f;
            ir[i] = rng.Bi() * 0.5f;
        }
        const int blocks = 20000;
        auto      t0     = std::chrono::steady_clock::now();
        for(int b = 0; b < blocks; b++)
        {
            SetAll(e, norm);
            e.Process(il, ir, ol, orr, kBlock);
        }
        double us = std::chrono::duration<double, std::micro>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
        std::printf("      %-8s %.2f us/block on host\n", e.Name(), us / blocks);
    }
}
