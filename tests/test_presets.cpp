// Preset text format: the shipped example presets parse cleanly,
// serialize -> parse round-trips, and bad input is ignored safely.
#include "check.h"
#include "app/preset_text.h"
#include "fx/reverb.h"
#include "fx/delay.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#ifndef SD_DIR
#define SD_DIR "../sd-card/BLUEMCHN"
#endif

namespace
{
// Same order as the firmware's EffectId (main.cpp); Name() is what matters.
fx::Reverb              p_reverb;
fx::Delay               p_delay;
const fx::Effect *const kEffects[] = {&p_reverb, &p_delay};
constexpr size_t        kNum       = sizeof(kEffects) / sizeof(kEffects[0]);

app::EffectState Defaults(const fx::Effect &e)
{
    app::EffectState st;
    std::memset(&st, 0, sizeof(st));
    for(size_t i = 0; i < e.NumParams(); i++)
        st.norm[i] = e.Spec(i).def;
    for(int s = 0; s < app::SRC_LAST; s++)
        st.route[s] = app::kRouteNone;
    return st;
}

bool ReadFile(const char *path, std::string &out)
{
    FILE *f = std::fopen(path, "rb");
    if(!f)
        return false;
    char   buf[4096];
    size_t n;
    out.clear();
    while((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    std::fclose(f);
    return true;
}

/** Largest difference a value may have after text formatting: seconds
 *  are written with 2 decimals, everything else as integers. */
float Resolution(const fx::ParamSpec &s)
{
    return s.unit == fx::Unit::Seconds ? 0.005f : 0.5f;
}

struct Rng
{
    uint32_t s = 42;
    float    Uni()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return (s >> 8) * (1.f / 16777216.f);
    }
};
} // namespace

TEST(presets_example_files_parse_cleanly)
{
    int found = 0;
    for(int slot = 1; slot <= app::kPresetSlotCount; slot++)
    {
        char path[256];
        std::snprintf(path, sizeof(path), "%s/PRESET%02d.TXT", SD_DIR, slot);
        std::string text;
        if(!ReadFile(path, text))
            continue;
        found++;
        int idx = app::FindPresetEffect(kEffects, kNum, text.data(), text.size());
        CHECKF(idx >= 0, "%s: effect not recognised (%d)", path, idx);
        if(idx < 0)
            continue;
        app::EffectState st   = Defaults(*kEffects[idx]);
        uint8_t          mode = 0;
        app::ParseResult r
            = app::ParsePreset(*kEffects[idx], text.data(), text.size(), st, mode);
        CHECKF(r.errors == 0, "%s: %d unparsed lines", path, r.errors);
        CHECKF(r.applied > 0, "%s: nothing applied", path);
        CHECKF(r.name[0] != 0, "%s: no name", path);
    }
    CHECKF(found >= 9, "only %d example presets found in %s", found, SD_DIR);
}

TEST(presets_round_trip)
{
    Rng rng;
    for(size_t k = 0; k < kNum; k++)
    {
        const fx::Effect &e = *kEffects[k];
        for(int trial = 0; trial < 200; trial++)
        {
            app::EffectState in = Defaults(e);
            for(size_t i = 0; i < e.NumParams(); i++)
                in.norm[i] = trial == 0 ? 0.f : trial == 1 ? 1.f : rng.Uni();
            for(int s = 0; s < app::SRC_LAST; s++)
                in.route[s] = (int8_t)((int)(rng.Uni() * (e.NumParams() + 1)) - 1);
            const uint8_t mode_in = trial & 1;

            char   text[2048];
            size_t len = app::SerializePreset(e, in, mode_in, "Trial", text, sizeof(text));
            CHECKF(len > 0 && len < sizeof(text), "%s: serialize len %zu", e.Name(), len);

            CHECK(app::FindPresetEffect(kEffects, kNum, text, len) == (int)k);

            app::EffectState out  = Defaults(e);
            uint8_t          mode = 0xFF;
            app::ParseResult r    = app::ParsePreset(e, text, len, out, mode);
            CHECKF(r.errors == 0, "%s trial %d: %d errors", e.Name(), trial, r.errors);
            CHECK(std::strcmp(r.name, "Trial") == 0);
            CHECK(mode == mode_in);
            for(int s = 0; s < app::SRC_LAST; s++)
                CHECKF(out.route[s] == in.route[s], "%s route %d", e.Name(), s);
            for(size_t i = 0; i < e.NumParams(); i++)
            {
                const fx::ParamSpec &sp = e.Spec(i);
                float a = sp.ToReal(in.norm[i]), b = sp.ToReal(out.norm[i]);
                CHECKF(fabsf(a - b) <= Resolution(sp) * 1.001f + 1e-4f * fabsf(a),
                       "%s.%s: %.4f -> %.4f",
                       e.Name(),
                       sp.name,
                       a,
                       b);
            }

            // Writing the parsed state again gives the same text
            char   text2[2048];
            size_t len2 = app::SerializePreset(e, out, mode, "Trial", text2, sizeof(text2));
            CHECKF(len2 == len && std::memcmp(text, text2, len) == 0,
                   "%s trial %d: second serialization differs",
                   e.Name(),
                   trial);
        }
    }
}

TEST(presets_without_effect_line_and_unknown_effect)
{
    const char none[] = "name = x\nmix = 50\n";
    CHECK(app::FindPresetEffect(kEffects, kNum, none, sizeof(none) - 1)
          == app::kPresetNoEffect);
    const char bad[] = "effect = FLANGERATOR\nmix = 50\n";
    CHECK(app::FindPresetEffect(kEffects, kNum, bad, sizeof(bad) - 1)
          == app::kPresetBadEffect);
    const char lower[] = "effect = delay\n";
    CHECK(app::FindPresetEffect(kEffects, kNum, lower, sizeof(lower) - 1) == 1);
}

TEST(presets_garbage_is_ignored)
{
    const fx::Effect &e    = p_reverb;
    app::EffectState  st   = Defaults(e);
    app::EffectState  orig = st;
    uint8_t           mode = 0;
    const char        text[]
        = "this is not a preset\r\n"
          "= = =\n"
          "mix = banana\n"
          "decay =\n"
          "frobnicate = 12\n"
          "knob1 = nosuchparam\n"
          "\x01\x02\xff\n"
          "hi_cut = 999999999999\n"; // out of range: clamped, not an error
    app::ParseResult r = app::ParsePreset(e, text, sizeof(text) - 1, st, mode);
    CHECK(r.errors > 0);
    CHECK(st.norm[fx::Reverb::MIX] == orig.norm[fx::Reverb::MIX]);
    CHECK(st.norm[fx::Reverb::DECAY] == orig.norm[fx::Reverb::DECAY]);
    CHECK(st.route[app::SRC_KNOB_1] == orig.route[app::SRC_KNOB_1]);
    CHECK(st.norm[fx::Reverb::HI_CUT] <= 1.f);
    for(size_t i = 0; i < e.NumParams(); i++)
        CHECK(std::isfinite(st.norm[i]) && st.norm[i] >= 0.f && st.norm[i] <= 1.f);

    // Empty and truncated input must not crash
    app::ParsePreset(e, "", 0, st, mode);
    app::ParsePreset(e, "decay = 1", 9, st, mode); // no trailing newline
    CHECK(fabsf(e.Spec(fx::Reverb::DECAY).ToReal(st.norm[fx::Reverb::DECAY]) - 1.f) < 0.01f);
}
