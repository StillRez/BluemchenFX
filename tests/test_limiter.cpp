// Output limiter (src/app/limiter.*): transparent below the threshold,
// never above the ceiling, recovers with the release time; plus the
// stacked-reverb case that motivated it (DEVLOG 2026-11-18).
#include "check.h"
#include "app/limiter.h"
#include "app/chain.h"
#include "app/chain_settings.h"
#include <cmath>
#include <memory>
#include <random>

namespace
{
constexpr float  kSr    = 48000.f;
constexpr size_t kBlock = 32;

float Db(float lin)
{
    return 20.f * log10f(lin);
}
} // namespace

TEST(limiter_is_transparent_below_threshold)
{
    app::Limiter lim;
    lim.Init(kSr); // -1 dB ceiling, threshold -2 dB
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> u(-0.75f, 0.75f); // < -2.5 dBFS
    for(int b = 0; b < 3000; b++)
    {
        float l[kBlock], r[kBlock], l0[kBlock], r0[kBlock];
        for(size_t i = 0; i < kBlock; i++)
            l[i] = l0[i] = u(rng), r[i] = r0[i] = u(rng);
        lim.Process(l, r, kBlock);
        for(size_t i = 0; i < kBlock; i++)
            if(!CHECK(l[i] == l0[i] && r[i] == r0[i]))
                return;
    }
    CHECK(lim.GainReductionDb() == 0.f);
}

TEST(limiter_never_exceeds_the_ceiling)
{
    // Random loud material incl. sudden jumps, at several ceilings/releases
    const float ceilings[] = {0.f, -1.f, -6.f, -12.f};
    const float releases[] = {0.02f, 0.15f, 1.f};
    std::mt19937 rng(7);
    std::normal_distribution<float> n(0.f, 1.f);
    for(float c : ceilings)
        for(float rel : releases)
        {
            app::Limiter lim;
            lim.Init(kSr);
            lim.SetCeilingDb(c);
            lim.SetRelease(rel);
            const float ceil = powf(10.f, c / 20.f);
            float       worst = 0.f;
            for(int b = 0; b < 6000; b++)
            {
                // bursts up to +24 dB, silences, steps
                const float lvl = (b / 300) % 3 == 0 ? 16.f : ((b / 300) % 3 == 1 ? 0.f : 2.f);
                float l[kBlock], r[kBlock];
                for(size_t i = 0; i < kBlock; i++)
                {
                    l[i] = lvl * n(rng);
                    r[i] = (b & 1) ? -l[i] : lvl * n(rng);
                }
                lim.Process(l, r, kBlock);
                for(size_t i = 0; i < kBlock; i++)
                {
                    worst = fabsf(l[i]) > worst ? fabsf(l[i]) : worst;
                    worst = fabsf(r[i]) > worst ? fabsf(r[i]) : worst;
                    CHECK(std::isfinite(l[i]) && std::isfinite(r[i]));
                }
            }
            CHECKF(worst <= ceil, "ceiling %.1f dB rel %.2f s: peak %.2f dB", c, rel, Db(worst));
        }
}

TEST(limiter_holds_a_loud_tone_at_threshold_and_recovers)
{
    app::Limiter lim;
    lim.Init(kSr);
    lim.SetCeilingDb(-1.f);
    lim.SetRelease(0.1f);
    // +12 dB sine for 1 s: settled peak sits near the threshold (-2 dB),
    // so the soft clip barely acts (low distortion)
    float  peak = 0.f;
    size_t t    = 0;
    for(int b = 0; b < (int)(kSr / kBlock); b++)
    {
        float l[kBlock], r[kBlock];
        for(size_t i = 0; i < kBlock; i++, t++)
            l[i] = r[i] = 4.f * sinf(2.f * (float)M_PI * 220.f * t / kSr);
        lim.Process(l, r, kBlock);
        if(b > (int)(0.5f * kSr / kBlock))
            for(size_t i = 0; i < kBlock; i++)
                peak = fabsf(l[i]) > peak ? fabsf(l[i]) : peak;
    }
    CHECKF(Db(peak) > -2.3f && Db(peak) < -1.f, "settled peak %.2f dB", Db(peak));
    CHECKF(lim.GainReductionDb() < -13.f && lim.GainReductionDb() > -15.f, "GR %.2f dB",
           lim.GainReductionDb());

    // then quiet: gain comes back within ~5 release times
    float last_gr = 0.f;
    for(int b = 0; b < (int)(0.5f * kSr / kBlock); b++)
    {
        float l[kBlock], r[kBlock];
        for(size_t i = 0; i < kBlock; i++, t++)
            l[i] = r[i] = 0.1f * sinf(2.f * (float)M_PI * 220.f * t / kSr);
        lim.Process(l, r, kBlock);
        last_gr = lim.GainReductionDb();
    }
    CHECKF(last_gr == 0.f, "GR after 0.5 s quiet: %.2f dB", last_gr);
}

TEST(limiter_tames_stacked_reverbs)
{
    // DELAY > REVERB > REVERB > REVERB at defaults with a held 220 Hz note
    // peaked at ~2.7 (+8.6 dBFS) on the module's chain; limited, it stays
    // under the -1 dB ceiling.
    constexpr size_t kArena = 2u << 20;
    std::unique_ptr<uint8_t[]> mem[app::kNumSlots];
    fx::Arena                  arenas[app::kNumSlots];
    for(size_t k = 0; k < app::kNumSlots; k++)
    {
        mem[k].reset(new uint8_t[kArena]);
        arenas[k].Init(mem[k].get(), kArena);
    }
    app::Chain chain;
    chain.Init(kSr, arenas);
    uint8_t types[app::kNumSlots] = {fx::FX_DELAY, fx::FX_REVERB, fx::FX_REVERB, fx::FX_REVERB};
    float   p[app::kNumSlots][app::kMaxParams];
    for(size_t k = 0; k < app::kNumSlots; k++)
    {
        app::DefaultNorms(types[k], p[k]);
        chain.SetType(k, types[k], p[k]);
    }
    chain.Poll();
    app::Limiter lim;
    lim.Init(kSr);

    float  raw = 0.f, limited = 0.f;
    size_t t   = 0;
    for(int b = 0; b < (int)(2.f * kSr / kBlock); b++)
    {
        float il[kBlock], ir[kBlock], ol[kBlock], orr[kBlock];
        for(size_t i = 0; i < kBlock; i++, t++)
            il[i] = ir[i] = 0.5f * sinf(2.f * (float)M_PI * 220.f * t / kSr);
        chain.Process(il, ir, ol, orr, kBlock, p, types);
        chain.Poll();
        for(size_t i = 0; i < kBlock; i++)
            raw = fabsf(ol[i]) > raw ? fabsf(ol[i]) : raw;
        lim.Process(ol, orr, kBlock);
        for(size_t i = 0; i < kBlock; i++)
            limited = fabsf(ol[i]) > limited ? fabsf(ol[i]) : limited;
    }
    std::printf("      held note through D>R>R>R: %.1f dBFS raw, %.1f dBFS limited\n",
                Db(raw), Db(limited));
    CHECK(raw > 1.5f); // the problem is still there without the limiter
    CHECK(limited <= powf(10.f, -1.f / 20.f));
}
