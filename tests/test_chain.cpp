// Effect chain: settings logic (defaults, validation, migration, recall,
// routing) and the engine (serial processing, independent instances,
// click-free bypass and effect swaps).
#include "check.h"
#include "app/chain.h"
#include "app/chain_settings.h"
#include "fx/registry.h"
#include "fx/reverb.h"
#include "fx/delay.h"
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>

using namespace app;

namespace
{
constexpr float  kSr    = 48000.f;
constexpr size_t kBlock = 32;
constexpr size_t kArena = 2u << 20; // plenty for reverb (~0.4 MB) / delay (~0.8 MB)

// ------------------------------------------------------------- helpers
struct Rng
{
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed) {}
    float Bi()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return (s >> 8) * (2.f / 16777216.f) - 1.f;
    }
};

/** A chain with heap "SDRAM" arenas, plus params for every slot. */
struct Rig
{
    std::unique_ptr<uint8_t[]> mem[kNumSlots];
    fx::Arena                  arenas[kNumSlots];
    Chain                      chain;
    float                      params[kNumSlots][kMaxParams] = {};
    uint8_t types[kNumSlots] = {fx::FX_NONE, fx::FX_NONE, fx::FX_NONE, fx::FX_NONE};

    explicit Rig(size_t arena_bytes = kArena)
    {
        for(size_t k = 0; k < kNumSlots; k++)
        {
            mem[k].reset(new uint8_t[arena_bytes]);
            arenas[k].Init(mem[k].get(), arena_bytes);
        }
        chain.Init(kSr, arenas);
    }

    void Load(size_t slot, uint8_t type, const float *norm = nullptr)
    {
        float def[kMaxParams];
        DefaultNorms(type, def);
        const float *n = norm ? norm : def;
        std::memcpy(params[slot], n, sizeof(def));
        types[slot] = type;
        chain.SetType(slot, type, n);
    }

    /** One block through the chain, then a main-loop Poll(). */
    void Block(const float *il, const float *ir, float *ol, float *orr)
    {
        chain.Process(il, ir, ol, orr, kBlock, params, types);
        chain.Poll();
    }
};

/** A standalone effect instance (its own storage + arena). */
struct Solo
{
    alignas(fx::kEffectStorageAlign) uint8_t storage[fx::kEffectStorageBytes];
    std::unique_ptr<uint8_t[]> mem{new uint8_t[kArena]};
    fx::Arena                  arena{mem.get(), kArena};
    fx::Effect                *e = nullptr;

    Solo(uint8_t type, const float *norm)
    {
        e = fx::Construct(type, storage);
        e->Init(kSr, arena);
        for(size_t i = 0; i < e->NumParams(); i++)
            e->SetParam(i, norm[i]);
        e->Reset();
    }
    ~Solo() { fx::Destroy(e); }
    void Block(const float *il, const float *ir, const float *norm, float *ol, float *orr)
    {
        for(size_t i = 0; i < e->NumParams(); i++)
            e->SetParam(i, norm[i]);
        e->Process(il, ir, ol, orr, kBlock);
    }
};

void Noise(Rng &rng, float *l, float *r, float amp = 0.5f)
{
    for(size_t i = 0; i < kBlock; i++)
    {
        l[i] = rng.Bi() * amp;
        r[i] = rng.Bi() * amp;
    }
}

float MaxAbsDiff(const float *a, const float *b, size_t n)
{
    float m = 0.f;
    for(size_t i = 0; i < n; i++)
        m = fmaxf(m, fabsf(a[i] - b[i]));
    return m;
}

constexpr size_t kFadeBlocks = (size_t)(Chain::kFadeSec * kSr / kBlock) + 2;

} // namespace

// ================================================================ settings

TEST(chain_registry_matches_test_list)
{
    // Every registered effect must also be in test_effects.cpp's stability
    // list; this catches "added to the registry, forgot the tests".
    const char *tested[] = {"REVERB", "DELAY"};
    for(uint8_t id = 0; id < fx::FX_COUNT; id++)
    {
        bool found = false;
        for(const char *t : tested)
            found |= std::strcmp(fx::Prototype(id)->Name(), t) == 0;
        CHECKF(found, "%s is not in the tests' effect lists", fx::Prototype(id)->Name());
    }
    CHECK(fx::Prototype(fx::FX_NONE) == nullptr);
    CHECK(fx::Prototype(fx::FX_COUNT) == nullptr);
}

TEST(settings_layout_is_stable)
{
    // Persisted in flash: changing it needs a version bump + migration
    CHECK(sizeof(Settings) == 172);
    CHECK(offsetof(Settings, out_gain) == 8);
    CHECK(offsetof(Settings, slot) == 12);
    CHECK(sizeof(SlotState) == 36);
    CHECK(offsetof(Settings, route) == 156);
    CHECK(offsetof(Settings, limit_db) == 164);
    CHECK(offsetof(Settings, limit_rel) == 168);
    CHECK(kSettingsVersion == 0xB10E0004);
}

TEST(settings_defaults)
{
    Settings s = MakeDefaults();
    CHECK(Valid(s));
    CHECK(s.slot[0].type == fx::FX_REVERB);
    for(size_t k = 1; k < kNumSlots; k++)
        CHECK(s.slot[k].type == fx::FX_NONE);
    CHECK(s.route[SRC_KNOB_1].Is(0, fx::Reverb::DECAY));
    CHECK(s.route[SRC_KNOB_2].Is(0, fx::Reverb::MIX));
    CHECK(s.route[SRC_CV_1].Is(0, fx::Reverb::DECAY));
    CHECK(s.route[SRC_CV_2].Is(0, fx::Reverb::HI_CUT));
    CHECK(s.out_gain == 1.f);
    CHECK(s.limit_db == -1.f && s.limit_rel == 0.15f);
}

TEST(settings_validation_rejects_bad_data)
{
    Settings s = MakeDefaults();
    Settings b;

    b         = s;
    b.version = kSettingsVersionV2;
    CHECK(!Valid(b));
    b              = s;
    b.slot[1].type = 7; // unknown effect
    CHECK(!Valid(b));
    b                 = s;
    b.slot[0].norm[2] = NAN;
    CHECK(!Valid(b));
    b               = s;
    b.slot[2].flags = 0x80;
    CHECK(!Valid(b));
    b          = s;
    b.route[0] = {2, 0}; // slot 3 is empty
    CHECK(!Valid(b));
    b          = s;
    b.route[0] = {0, 4}; // reverb has 4 params (0..3)
    CHECK(!Valid(b));
    b          = s;
    b.route[0] = {kRouteNone, 2}; // half-set route
    CHECK(!Valid(b));
    b          = s;
    b.out_gain = NAN;
    CHECK(!Valid(b));
    b            = s;
    b.input_mode = 9;
    CHECK(!Valid(b));    b          = s;
    b.limit_db = 3.f; // above 0 dBFS
    CHECK(!Valid(b));
    b           = s;
    b.limit_rel = NAN;
    CHECK(!Valid(b));
    b           = s;
    b.limit_rel = 0.f;
    CHECK(!Valid(b));
}

TEST(settings_migrate_v2)
{
    SettingsV2 v2;
    std::memset(&v2, 0, sizeof(v2));
    v2.version    = kSettingsVersionV2;
    v2.effect     = fx::FX_DELAY; // active
    v2.input_mode = INPUT_MONO;
    for(size_t i = 0; i < kMaxParams; i++)
    {
        v2.fx[fx::FX_REVERB].norm[i] = 0.1f * i;
        v2.fx[fx::FX_DELAY].norm[i]  = 0.9f - 0.1f * i;
    }
    int8_t rev_routes[4] = {0, 1, 2, kRouteNone};
    int8_t dly_routes[4] = {5, kRouteNone, 3, 1};
    std::memcpy(v2.fx[fx::FX_REVERB].route, rev_routes, 4);
    std::memcpy(v2.fx[fx::FX_DELAY].route, dly_routes, 4);
    for(size_t e = 2; e < kMaxEffectsV2; e++)
        for(int r = 0; r < SRC_LAST; r++)
            v2.fx[e].route[r] = kRouteNone;

    Settings out;
    Recall   rc;
    rc.Clear();
    CHECK(MigrateV2(v2, out, &rc));
    CHECK(Valid(out));
    CHECK(out.slot[0].type == fx::FX_DELAY);
    CHECK(out.input_mode == INPUT_MONO);
    CHECK(std::memcmp(out.slot[0].norm, v2.fx[fx::FX_DELAY].norm, sizeof(out.slot[0].norm)) == 0);
    CHECK(out.route[0].Is(0, 5));
    CHECK(out.route[1].None());
    CHECK(out.route[2].Is(0, 3));
    CHECK(out.route[3].Is(0, 1));
    for(size_t k = 1; k < kNumSlots; k++)
        CHECK(out.slot[k].type == fx::FX_NONE);

    // The inactive effect's v2 values come back when slot 1 switches to it
    CHECK(rc.valid[0][fx::FX_REVERB]);
    CHECK(SetSlotType(out, rc, 0, fx::FX_REVERB));
    CHECK(std::memcmp(out.slot[0].norm, v2.fx[fx::FX_REVERB].norm, sizeof(out.slot[0].norm)) == 0);

    // Garbage v2 is refused
    SettingsV2 bad = v2;
    bad.effect     = 9;
    CHECK(!MigrateV2(bad, out, nullptr));
    bad                     = v2;
    bad.fx[0].norm[0]       = -1.f;
    CHECK(!MigrateV2(bad, out, nullptr));
    bad                     = v2;
    bad.fx[fx::FX_DELAY].route[0] = 6; // delay has 6 params (0..5)
    CHECK(!MigrateV2(bad, out, nullptr));
}

TEST(settings_migrate_v3)
{
    // A phase 1/2 chain setup keeps everything; the limiter gets defaults
    Recall rc;
    rc.Clear();
    Settings cur = MakeDefaults();
    SetSlotType(cur, rc, 1, fx::FX_DELAY);
    SetSlotType(cur, rc, 2, fx::FX_REVERB);
    cur.slot[1].flags = SLOT_BYPASS;
    cur.slot[2].norm[0] = 0.77f;
    cur.route[SRC_KNOB_2] = {1, 2};
    cur.input_mode = INPUT_MONO;
    cur.out_gain   = 0.5f;
    cur.ui_slot    = 2;

    SettingsV3 v3;
    std::memcpy(&v3, &cur, sizeof(v3)); // the v3 bytes are the v4 prefix
    v3.version = kSettingsVersionV3;
    Settings out;
    CHECK(MigrateV3(v3, out));
    CHECK(Valid(out));
    CHECK(out.version == kSettingsVersion);
    CHECK(std::memcmp(out.slot, cur.slot, sizeof(cur.slot)) == 0);
    CHECK(std::memcmp(out.route, cur.route, sizeof(cur.route)) == 0);
    CHECK(out.input_mode == INPUT_MONO && out.out_gain == 0.5f && out.ui_slot == 2);
    CHECK(out.limit_db == kLimitDefaultDb && out.limit_rel == kReleaseDefault);

    v3.version = kSettingsVersionV2;
    CHECK(!MigrateV3(v3, out));
    v3.version = kSettingsVersionV3;
    v3.slot[0].norm[0] = 2.f; // corrupt
    CHECK(!MigrateV3(v3, out));
}

TEST(settings_migrate_v1)
{
    SettingsV1 v1;
    std::memset(&v1, 0, sizeof(v1));
    v1.version    = kSettingsVersionV1;
    v1.input_mode = INPUT_STEREO;
    for(size_t i = 0; i < kMaxParams; i++)
        v1.norm[i] = 0.05f * i;
    int8_t routes[4] = {3, 2, kRouteNone, 0};
    std::memcpy(v1.route, routes, 4);
    Settings out;
    CHECK(MigrateV1(v1, out));
    CHECK(Valid(out));
    CHECK(out.slot[0].type == fx::FX_REVERB);
    CHECK(out.slot[0].norm[3] == v1.norm[3]);
    CHECK(out.route[0].Is(0, 3));
    CHECK(out.route[2].None());
    v1.version = 0;
    CHECK(!MigrateV1(v1, out));
}

TEST(settings_slot_type_recall_and_routes)
{
    Settings s = MakeDefaults();
    Recall   rc;
    rc.Clear();
    s.slot[0].norm[fx::Reverb::MIX] = 0.77f; // user's reverb tweak
    s.route[SRC_KNOB_2]             = {kRouteNone, kRouteNone}; // user unrouted K2

    // reverb -> delay: delay defaults, routed sources follow delay defaults
    CHECK(SetSlotType(s, rc, 0, fx::FX_DELAY));
    CHECK(Valid(s));
    float def[kMaxParams];
    DefaultNorms(fx::FX_DELAY, def);
    CHECK(std::memcmp(s.slot[0].norm, def, sizeof(def)) == 0);
    CHECK(s.route[SRC_KNOB_1].Is(0, fx::Delay::TIME));
    CHECK(s.route[SRC_KNOB_2].None()); // wasn't on this slot: untouched
    CHECK(s.route[SRC_CV_2].Is(0, fx::Delay::FEEDBACK));

    s.slot[0].norm[fx::Delay::FEEDBACK] = 0.66f; // user's delay tweak

    // delay -> reverb: the reverb tweak is back
    CHECK(SetSlotType(s, rc, 0, fx::FX_REVERB));
    CHECK(s.slot[0].norm[fx::Reverb::MIX] == 0.77f);
    // reverb -> delay: and the delay tweak
    CHECK(SetSlotType(s, rc, 0, fx::FX_DELAY));
    CHECK(s.slot[0].norm[fx::Delay::FEEDBACK] == 0.66f);
    // same type again: no change
    CHECK(!SetSlotType(s, rc, 0, fx::FX_DELAY));

    // Recall is per slot: slot 2 starts from defaults
    CHECK(SetSlotType(s, rc, 1, fx::FX_DELAY));
    CHECK(std::memcmp(s.slot[1].norm, def, sizeof(def)) == 0);
    s.route[SRC_KNOB_2] = {1, fx::Delay::MIX};
    CHECK(Valid(s));

    // Emptying a slot drops the routes that pointed at it, keeps others
    CHECK(SetSlotType(s, rc, 1, fx::FX_NONE));
    CHECK(s.route[SRC_KNOB_2].None());
    CHECK(s.route[SRC_KNOB_1].Is(0, fx::Delay::TIME));
    CHECK(Valid(s));

    // Bypass flags survive a type change
    s.slot[0].flags = SLOT_BYPASS | SLOT_HARD;
    CHECK(SetSlotType(s, rc, 0, fx::FX_REVERB));
    CHECK(s.slot[0].flags == (SLOT_BYPASS | SLOT_HARD));

    // Bad input
    CHECK(!SetSlotType(s, rc, 4, fx::FX_DELAY));
    CHECK(!SetSlotType(s, rc, 0, 42));
}

TEST(settings_single_effect_preset)
{
    Settings s = MakeDefaults();
    Recall   rc;
    rc.Clear();
    SetSlotType(s, rc, 1, fx::FX_DELAY);
    SetSlotType(s, rc, 2, fx::FX_REVERB);
    s.slot[0].flags = SLOT_BYPASS;
    s.route[SRC_CV_1] = {2, fx::Reverb::MIX};

    // Round trip of slot 1 as a single-effect state
    EffectState st = SlotAsEffectState(s, 0);
    CHECK(st.route[SRC_KNOB_1] == fx::Reverb::DECAY);
    CHECK(st.route[SRC_CV_1] == kRouteNone); // points at slot 3, not slot 1

    // Loading a delay preset: slot 1 = delay, the rest emptied
    EffectState p;
    std::memset(&p, 0, sizeof(p));
    for(size_t i = 0; i < kMaxParams; i++)
        p.norm[i] = 0.25f;
    int8_t routes[4] = {fx::Delay::PING, kRouteNone, fx::Delay::TIME, kRouteNone};
    std::memcpy(p.route, routes, 4);
    ApplySingleEffectPreset(s, rc, fx::FX_DELAY, p, INPUT_MONO);
    CHECK(Valid(s));
    CHECK(s.slot[0].type == fx::FX_DELAY);
    CHECK(s.slot[0].flags == 0);
    CHECK(s.slot[0].norm[3] == 0.25f);
    for(size_t k = 1; k < kNumSlots; k++)
        CHECK(s.slot[k].type == fx::FX_NONE);
    CHECK(s.route[SRC_KNOB_1].Is(0, fx::Delay::PING));
    CHECK(s.route[SRC_KNOB_2].None());
    CHECK(s.route[SRC_CV_1].Is(0, fx::Delay::TIME));
    CHECK(s.input_mode == INPUT_MONO);
    // and recall knows the preset's delay values for slot 1
    CHECK(rc.valid[0][fx::FX_DELAY] && rc.norm[0][fx::FX_DELAY][0] == 0.25f);
}

// ================================================================== engine

TEST(chain_empty_is_a_wire)
{
    Rig   rig;
    Rng   rng(1);
    float il[kBlock], ir[kBlock], ol[kBlock], orr[kBlock];
    for(int b = 0; b < 100; b++)
    {
        Noise(rng, il, ir);
        rig.Block(il, ir, ol, orr);
        CHECK(MaxAbsDiff(il, ol, kBlock) == 0.f && MaxAbsDiff(ir, orr, kBlock) == 0.f);
    }
}

// After the fade-in, a slot's output is exactly the effect's output.
TEST(chain_single_slot_equals_effect)
{
    for(uint8_t type = 0; type < fx::FX_COUNT; type++)
    {
        Rig rig;
        rig.Load(2, type); // any slot; the others are wires
        CHECK(rig.chain.Loaded(2) == type);
        Solo  solo(type, rig.params[2]);
        Rng   rng(7);
        float il[kBlock], ir[kBlock], ol[kBlock], orr[kBlock], sl[kBlock], sr[kBlock];
        float worst = 0.f;
        for(int b = 0; b < 3000; b++)
        {
            Noise(rng, il, ir);
            rig.Block(il, ir, ol, orr);
            solo.Block(il, ir, rig.params[2], sl, sr);
            if(b > (int)kFadeBlocks)
                worst = fmaxf(worst, fmaxf(MaxAbsDiff(ol, sl, kBlock), MaxAbsDiff(orr, sr, kBlock)));
        }
        CHECKF(worst == 0.f, "%s: chain differs from effect by %g", fx::Prototype(type)->Name(), worst);
    }
}

// Two delays with different settings: proves instances don't share buffers
// (the old global SDRAM buffers would have mixed them up), and that slots
// run in series.
TEST(chain_serial_and_independent_instances)
{
    Rig   rig;
    float a[kMaxParams], b[kMaxParams];
    DefaultNorms(fx::FX_DELAY, a);
    DefaultNorms(fx::FX_DELAY, b);
    a[fx::Delay::TIME] = fx::Prototype(fx::FX_DELAY)->Spec(fx::Delay::TIME).ToNorm(0.13f);
    b[fx::Delay::TIME] = fx::Prototype(fx::FX_DELAY)->Spec(fx::Delay::TIME).ToNorm(0.31f);
    b[fx::Delay::PING] = 1.f;
    rig.Load(0, fx::FX_DELAY, a);
    rig.Load(1, fx::FX_REVERB);
    rig.Load(3, fx::FX_DELAY, b);

    Solo  s1(fx::FX_DELAY, a), s2(fx::FX_REVERB, rig.params[1]), s3(fx::FX_DELAY, b);
    Rng   rng(9);
    float il[kBlock], ir[kBlock], ol[kBlock], orr[kBlock];
    float t1l[kBlock], t1r[kBlock], t2l[kBlock], t2r[kBlock], t3l[kBlock], t3r[kBlock];
    float worst = 0.f;
    for(int blk = 0; blk < 6000; blk++) // 4 s
    {
        // Silence until every slot has faded in (during a fade the slot
        // output is a dry/wet blend, which the long reverb tail would
        // remember), then 0.2 s of noise, then the tails.
        if(blk > (int)kFadeBlocks && blk < 300)
            Noise(rng, il, ir);
        else
            for(size_t i = 0; i < kBlock; i++)
                il[i] = ir[i] = 0.f;
        rig.Block(il, ir, ol, orr);
        s1.Block(il, ir, a, t1l, t1r);
        s2.Block(t1l, t1r, rig.params[1], t2l, t2r);
        s3.Block(t2l, t2r, b, t3l, t3r);
        worst = fmaxf(worst, fmaxf(MaxAbsDiff(ol, t3l, kBlock), MaxAbsDiff(orr, t3r, kBlock)));
    }
    CHECKF(worst < 1e-5f, "chain vs delay->reverb->delay differs by %g", worst);
}

// Toggling bypass (both modes) and swapping effects must not click.
// Input: 100 Hz sine; the delay (100 % wet, 0.25 s) is a phase-shifted copy,
// so an instant switch would jump by up to ~2x the amplitude.
TEST(chain_bypass_and_swap_are_click_free)
{
    Rig   rig;
    float n[kMaxParams];
    DefaultNorms(fx::FX_DELAY, n);
    const fx::Effect *d = fx::Prototype(fx::FX_DELAY);
    n[fx::Delay::TIME]     = d->Spec(fx::Delay::TIME).ToNorm(0.2525f);
    n[fx::Delay::FEEDBACK] = 0.f;
    n[fx::Delay::MIX]      = 1.f;
    n[fx::Delay::LO_CUT]   = 0.f;
    n[fx::Delay::HI_CUT]   = 1.f;
    rig.Load(0, fx::FX_DELAY, n);

    const float amp      = 0.5f;
    const float max_step = amp * 2.f * 3.1415927f * 100.f / kSr; // sine's own
    float       il[kBlock], ir[kBlock], ol[kBlock], orr[kBlock];
    float       prev = 0.f, worst = 0.f;
    size_t      t    = 0;
    auto        run  = [&](int blocks) {
        for(int b = 0; b < blocks; b++)
        {
            for(size_t i = 0; i < kBlock; i++, t++)
                il[i] = ir[i] = amp * sinf(2.f * 3.1415927f * 100.f * t / kSr);
            rig.Block(il, ir, ol, orr);
            for(size_t i = 0; i < kBlock; i++)
            {
                worst = fmaxf(worst, fabsf(ol[i] - prev));
                prev  = ol[i];
            }
        }
    };
    run(1000); // fill the delay, settle

    // Each transition: allowed step = the sine's own slope + the ramp
    // moving between two signals that differ by at most 2·amp.
    const float allowed = max_step + 2.f * amp / (Chain::kFadeSec * kSr) * 1.2f;

    worst = 0.f;
    rig.chain.SetBypass(0, true, true); // hard
    run(100);
    rig.chain.SetBypass(0, false, true);
    run(100);
    CHECKF(worst <= allowed, "hard bypass on/off: step %.4f > %.4f", worst, allowed);

    worst = 0.f;
    rig.chain.SetBypass(0, true, false); // trails
    run(100);
    rig.chain.SetBypass(0, false, false);
    run(100);
    CHECKF(worst <= allowed, "trails bypass on/off: step %.4f > %.4f", worst, allowed);

    // Swap to a reverb at MIX 0 (output exactly dry), so only the swap's
    // own crossfade is measured, not the reverb's (jagged) early echoes.
    worst = 0.f;
    float rv[kMaxParams];
    DefaultNorms(fx::FX_REVERB, rv);
    rv[fx::Reverb::MIX] = 0.f;
    rig.Load(0, fx::FX_REVERB, rv);
    run(200);
    CHECK(rig.chain.Loaded(0) == fx::FX_REVERB && !rig.chain.Swapping(0));
    rig.Load(0, fx::FX_NONE);
    run(100);
    CHECK(rig.chain.Loaded(0) == fx::FX_NONE);
    CHECKF(worst <= allowed * 1.5f, "effect swap: step %.4f > %.4f", worst, allowed * 1.5f);

    // Sanity: the two signals really are far apart (an instant switch clicks)
    CHECK(2.f * amp > allowed * 10.f);
}

TEST(chain_hard_bypass_is_exact_and_clears_tails)
{
    Rig   rig;
    float n[kMaxParams];
    DefaultNorms(fx::FX_DELAY, n);
    n[fx::Delay::FEEDBACK] = 0.9f;
    n[fx::Delay::MIX]      = 1.f;
    rig.Load(0, fx::FX_DELAY, n);
    Rng   rng(3);
    float il[kBlock], ir[kBlock], ol[kBlock], orr[kBlock];
    for(int b = 0; b < 1500; b++) // 1 s of noise into a long feedback delay
    {
        Noise(rng, il, ir);
        rig.Block(il, ir, ol, orr);
    }
    rig.chain.SetBypass(0, true, true);
    float worst = 0.f;
    for(int b = 0; b < 300; b++)
    {
        Noise(rng, il, ir);
        rig.Block(il, ir, ol, orr);
        if(b > (int)kFadeBlocks)
            worst = fmaxf(worst, MaxAbsDiff(il, ol, kBlock));
    }
    CHECKF(worst == 0.f, "hard-bypassed slot isn't a wire (%g)", worst);

    // Un-bypass into silence: the old echoes must be gone
    rig.chain.SetBypass(0, false, true);
    float peak = 0.f;
    for(int b = 0; b < 1500; b++)
    {
        for(size_t i = 0; i < kBlock; i++)
            il[i] = ir[i] = 0.f;
        rig.Block(il, ir, ol, orr);
        for(size_t i = 0; i < kBlock; i++)
            peak = fmaxf(peak, fmaxf(fabsf(ol[i]), fabsf(orr[i])));
    }
    CHECKF(peak < 1e-6f, "stale echoes after hard bypass: %g", peak);
}

TEST(chain_trails_bypass_lets_tails_ring)
{
    Rig   rig;
    float n[kMaxParams];
    DefaultNorms(fx::FX_DELAY, n);
    n[fx::Delay::FEEDBACK] = 0.6f;
    n[fx::Delay::MIX]      = 1.f;
    rig.Load(0, fx::FX_DELAY, n);
    Rng   rng(5);
    float il[kBlock], ir[kBlock], ol[kBlock], orr[kBlock];
    for(int b = 0; b < 750; b++)
    {
        Noise(rng, il, ir);
        rig.Block(il, ir, ol, orr);
    }
    rig.chain.SetBypass(0, true, false);
    // Keep playing: output = dry input + the ringing tail
    double tail_early = 0.0, tail_late = 0.0;
    for(int b = 0; b < 6000; b++) // 4 s
    {
        Noise(rng, il, ir);
        rig.Block(il, ir, ol, orr);
        double e = 0.0;
        for(size_t i = 0; i < kBlock; i++)
            e += (double)(ol[i] - il[i]) * (ol[i] - il[i]);
        if(b > (int)kFadeBlocks && b < 1500)
            tail_early += e;
        if(b >= 4500)
            tail_late += e;
    }
    CHECKF(tail_early > 1.0, "no tail after trails bypass (%g)", tail_early);
    CHECKF(tail_late < tail_early * 0.01, "tail doesn't die away (%g -> %g)", tail_early, tail_late);
}

TEST(chain_swap_keeps_other_slots_playing)
{
    Rig rig;
    rig.Load(0, fx::FX_REVERB);
    rig.Load(1, fx::FX_DELAY);
    Rng   rng(11);
    float il[kBlock], ir[kBlock], ol[kBlock], orr[kBlock];
    for(int b = 0; b < 200; b++)
    {
        Noise(rng, il, ir);
        rig.Block(il, ir, ol, orr);
    }
    // Swap slot 2 delay -> reverb; slot 1 keeps going, the swap completes
    rig.Load(1, fx::FX_REVERB);
    CHECK(rig.chain.Swapping(1));
    int done_at = -1;
    for(int b = 0; b < 200; b++)
    {
        Noise(rng, il, ir);
        rig.Block(il, ir, ol, orr);
        CHECK(rig.chain.Loaded(0) == fx::FX_REVERB);
        if(done_at < 0 && !rig.chain.Swapping(1))
            done_at = b;
    }
    CHECKF(done_at >= 0 && done_at < (int)kFadeBlocks * 2,
           "swap took %d blocks",
           done_at);
    CHECK(rig.chain.Loaded(1) == fx::FX_REVERB);
}

TEST(chain_effect_too_big_for_arena)
{
    Rig rig(64 * 1024); // 64 KB arenas: nothing fits
    rig.Load(0, fx::FX_REVERB);
    CHECK(rig.chain.Loaded(0) == fx::FX_NONE);
    CHECK(rig.chain.TakeInitFailure(0));
    CHECK(!rig.chain.TakeInitFailure(0));
    float il[kBlock] = {0.5f}, ir[kBlock] = {}, ol[kBlock], orr[kBlock];
    rig.Block(il, ir, ol, orr);
    CHECK(MaxAbsDiff(il, ol, kBlock) == 0.f); // wire
}

// Random swaps, bypass toggles and parameter jumps for 30 s of noise.
TEST(chain_stress_random_changes)
{
    Rig   rig;
    Rng   rng(2024);
    float il[kBlock], ir[kBlock], ol[kBlock], orr[kBlock];
    bool  finite = true;
    float peak   = 0.f;
    auto  pick   = [&](int n) { return (int)((rng.Bi() * 0.5f + 0.5f) * n) % n; };
    for(int b = 0; b < 45000; b++)
    {
        if(b % 50 == 0)
        {
            size_t  k    = (size_t)pick((int)kNumSlots);
            int     what = pick(4);
            if(what == 0)
            {
                int t = pick(fx::FX_COUNT + 1);
                rig.Load(k, t == fx::FX_COUNT ? fx::FX_NONE : (uint8_t)t);
            }
            else if(what == 1)
                rig.chain.SetBypass(k, pick(2) == 1, pick(2) == 1);
            else
                for(size_t i = 0; i < kMaxParams; i++)
                    rig.params[k][i] = rng.Bi() * 0.5f + 0.5f;
        }
        Noise(rng, il, ir, 0.3f);
        rig.Block(il, ir, ol, orr);
        for(size_t i = 0; i < kBlock; i++)
        {
            finite &= std::isfinite(ol[i]) && std::isfinite(orr[i]);
            peak = fmaxf(peak, fmaxf(fabsf(ol[i]), fabsf(orr[i])));
        }
    }
    CHECKF(finite && peak < 32.f, "finite=%d peak=%.2f", finite, peak);
}
