// Chain presets (phase 3) and the MIDI CC map: the whole chain round-trips
// through text, old single-effect files still load into slot 1, and CCs
// reach every slot.
#include "check.h"
#include "app/preset_text.h"
#include "app/chain_settings.h"
#include "fx/registry.h"
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
struct Rng
{
    uint32_t s = 7;
    float    Uni()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return (s >> 8) * (1.f / 16777216.f);
    }
};

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

float Resolution(const fx::ParamSpec &s)
{
    // what the text format keeps: 2 decimals for seconds and for Hz below 10,
    // 1 decimal for ms below 10, whole numbers otherwise
    if(s.unit == fx::Unit::Seconds || (s.unit == fx::Unit::Hertz && s.max < 10.f))
        return 0.005f;
    return 0.5f;
}

app::Settings Load(const char *text, app::PresetLoad *res = nullptr)
{
    app::Settings s = app::MakeDefaults();
    app::Recall   r;
    r.Clear();
    app::PresetLoad st = app::LoadPresetText(text, std::strlen(text), s, r);
    if(res)
        *res = st;
    return s;
}

/** A random but valid chain. */
app::Settings RandomChain(Rng &rng, int trial)
{
    app::Settings s = app::MakeDefaults();
    app::Recall   r;
    r.Clear();
    for(int k = 0; k < (int)app::kNumSlots; k++)
    {
        int     pick = (int)(rng.Uni() * (fx::FX_COUNT + 1));
        uint8_t t    = pick >= fx::FX_COUNT ? (uint8_t)fx::FX_NONE : (uint8_t)pick;
        app::SetSlotType(s, r, k, t);
        if(const fx::Effect *e = fx::Prototype(t))
            for(size_t i = 0; i < e->NumParams(); i++)
                s.slot[k].norm[i] = trial == 0 ? 0.f : trial == 1 ? 1.f : rng.Uni();
        s.slot[k].flags = (uint8_t)(rng.Uni() * 4);
    }
    for(int src = 0; src < app::SRC_LAST; src++)
    {
        int               k = (int)(rng.Uni() * app::kNumSlots);
        const fx::Effect *e = fx::Prototype(s.slot[k].type);
        if(e && rng.Uni() < 0.8f)
            s.route[src] = app::Route{(int8_t)k, (int8_t)(rng.Uni() * e->NumParams())};
        else
            s.route[src] = app::Route{app::kRouteNone, app::kRouteNone};
    }
    s.input_mode = trial & 1;
    float db     = -24.f + 0.5f * (int)(rng.Uni() * 61);
    s.out_gain   = db == 0.f ? 1.f : powf(10.f, db / 20.f);
    return s;
}
} // namespace

TEST(chain_presets_round_trip)
{
    Rng rng;
    for(int trial = 0; trial < 300; trial++)
    {
        app::Settings in = RandomChain(rng, trial);
        char          text[4096];
        size_t        len = app::SerializeChainPreset(in, "Trial", text, sizeof(text));
        CHECKF(len > 0 && len < sizeof(text), "trial %d: serialize len %zu", trial, len);

        // Load onto a different starting chain: the file defines it all
        app::Settings out = RandomChain(rng, 2);
        app::Recall   rec;
        rec.Clear();
        CHECK(app::LoadPresetText(text, len, out, rec) == app::PresetLoad::OK);
        CHECK(out.input_mode == in.input_mode);
        CHECKF(fabsf(20.f * log10f(out.out_gain) - 20.f * log10f(in.out_gain)) < 0.01f,
               "trial %d: out gain %.4f -> %.4f",
               trial,
               in.out_gain,
               out.out_gain);
        for(int k = 0; k < (int)app::kNumSlots; k++)
        {
            CHECKF(out.slot[k].type == in.slot[k].type, "trial %d slot %d type", trial, k);
            const fx::Effect *e = fx::Prototype(in.slot[k].type);
            if(!e)
                continue;
            CHECKF(out.slot[k].flags == in.slot[k].flags, "trial %d slot %d flags", trial, k);
            for(size_t i = 0; i < e->NumParams(); i++)
            {
                const fx::ParamSpec &sp = e->Spec(i);
                float a = sp.ToReal(in.slot[k].norm[i]), b = sp.ToReal(out.slot[k].norm[i]);
                CHECKF(fabsf(a - b) <= Resolution(sp) * 1.001f + 1e-4f * fabsf(a),
                       "trial %d %d.%s: %.4f -> %.4f",
                       trial,
                       k + 1,
                       sp.name,
                       a,
                       b);
            }
            // recall now holds what was loaded
            CHECK(rec.valid[k][in.slot[k].type]);
        }
        for(int src = 0; src < app::SRC_LAST; src++)
            CHECKF(out.route[src].slot == in.route[src].slot
                       && out.route[src].param == in.route[src].param,
                   "trial %d route %d",
                   trial,
                   src);
        CHECK(app::Valid(out));

        // Writing it again gives the same text
        char   text2[4096];
        size_t len2 = app::SerializeChainPreset(out, "Trial", text2, sizeof(text2));
        CHECKF(len2 == len && std::memcmp(text, text2, len) == 0,
               "trial %d: second serialization differs",
               trial);

        app::PresetInfo info = app::PeekPresetText(text, len);
        CHECK(info.chain && info.known && std::strcmp(info.name, "Trial") == 0);
        for(int k = 0; k < (int)app::kNumSlots; k++)
            CHECK(info.type[k] == in.slot[k].type);
    }
}

TEST(chain_presets_full_chain_fits_the_sd_buffer)
{
    // Biggest case: four delays (most parameters), every route set
    app::Settings s = app::MakeDefaults();
    app::Recall   r;
    r.Clear();
    for(int k = 0; k < (int)app::kNumSlots; k++)
        app::SetSlotType(s, r, k, fx::FX_DELAY);
    char   text[4096];
    size_t len = app::SerializeChainPreset(s, "0123456789ABCDEF", text, sizeof(text));
    CHECKF(len > 0 && len < 3000, "4 x DELAY preset is %zu bytes", len);
}

TEST(chain_presets_old_single_effect_files_load_into_slot_1)
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

        // Start from a full chain: an old preset still empties slots 2-4
        app::Settings s = app::MakeDefaults();
        app::Recall   r;
        r.Clear();
        for(int k = 0; k < (int)app::kNumSlots; k++)
            app::SetSlotType(s, r, k, fx::FX_DELAY);
        s.slot[0].flags     = app::SLOT_BYPASS;
        app::PresetLoad res = app::LoadPresetText(text.data(), text.size(), s, r);
        CHECKF(res == app::PresetLoad::OK, "%s: %d", path, (int)res);
        app::PresetInfo info = app::PeekPresetText(text.data(), text.size());
        CHECKF(info.name[0] != 0, "%s: no name", path);
        CHECKF(info.known, "%s: effect not known", path);
        if(!info.chain)
        {
            CHECKF(s.slot[0].type == info.type[0], "%s: slot 1 type", path);
            CHECK(s.slot[0].flags == 0);
            for(int k = 1; k < (int)app::kNumSlots; k++)
                CHECKF(s.slot[k].type == fx::FX_NONE, "%s: slot %d not emptied", path, k + 1);
        }
        else
        {
            for(int k = 0; k < (int)app::kNumSlots; k++)
                CHECKF(s.slot[k].type == info.type[k], "%s: slot %d type", path, k + 1);
            // re-serializing the loaded chain reproduces the shipped file
            char   out[4096];
            size_t n = app::SerializeChainPreset(s, info.name, out, sizeof(out));
            CHECKF(n == text.size() && std::memcmp(out, text.data(), n) == 0,
                   "%s: not what the serializer writes",
                   path);
        }
        CHECKF(app::Valid(s), "%s: invalid settings", path);
    }
    CHECKF(found >= 12, "only %d example presets found", found);
}

TEST(chain_presets_sections_rules)
{
    // A slot without a section is emptied; a section without an effect
    // line keeps the type; bare routing names mean the current section
    const char text[]
        = "name = Test\n"
          "out = -3 dB\n"
          "input = mono\n"
          "[slot1]\n"
          "time = 0.5\n" // slot 1 is REVERB in the defaults: no TIME -> ignored
          "decay = 4.0\n"
          "bypass = on\n"
          "bypass_mode = hard\n"
          "[ Slot 3 ]\n"
          "effect = delay\n"
          "feedbk = 60\n"
          "knob2 = time\n"
          "[slot9]\n"
          "decay = 1\n" // unknown section: ignored
          "[slot4]\n"
          "effect = none\n"
          "knob1 = 1.decay\n"
          "cv1 = 2.decay\n" // slot 2 is empty: ignored, stays
          "cv2 = none\n";
    app::PresetLoad res;
    app::Settings   s = Load(text, &res);
    CHECK(res == app::PresetLoad::OK);
    CHECK(s.input_mode == app::INPUT_MONO);
    CHECK(fabsf(20.f * log10f(s.out_gain) + 3.f) < 0.01f);

    const fx::Effect *rev = fx::Prototype(fx::FX_REVERB);
    const fx::Effect *del = fx::Prototype(fx::FX_DELAY);
    CHECK(s.slot[0].type == fx::FX_REVERB);
    CHECK(fabsf(rev->Spec(fx::Reverb::DECAY).ToReal(s.slot[0].norm[fx::Reverb::DECAY]) - 4.f)
          < 0.01f);
    CHECK(s.slot[0].Bypassed() && s.slot[0].Hard());
    CHECK(s.slot[1].type == fx::FX_NONE);
    CHECK(s.slot[2].type == fx::FX_DELAY && s.slot[2].flags == 0);
    CHECK(fabsf(del->Spec(fx::Delay::FEEDBACK).ToReal(s.slot[2].norm[fx::Delay::FEEDBACK]) - 60.f)
          < 0.5f);
    // a slot that changes effect starts from that effect's defaults
    CHECK(s.slot[2].norm[fx::Delay::MIX] == del->Spec(fx::Delay::MIX).def);
    CHECK(s.slot[3].type == fx::FX_NONE);
    CHECK(s.route[app::SRC_KNOB_1].Is(0, fx::Reverb::DECAY));
    CHECK(s.route[app::SRC_KNOB_2].Is(2, fx::Delay::TIME));
    CHECK(s.route[app::SRC_CV_2].None());
    CHECK(app::Valid(s));

    app::PresetInfo info = app::PeekPresetText(text, sizeof(text) - 1);
    CHECK(info.chain && !info.known); // slot 1 keeps "whatever is there"
    CHECK(std::strcmp(info.name, "Test") == 0);
}

TEST(chain_presets_unknown_effect_changes_nothing)
{
    const char text[] = "[slot1]\neffect = DELAY\n[slot2]\neffect = FLANGERATOR\n";
    app::Settings   s   = app::MakeDefaults();
    app::Settings   ref = s;
    app::Recall     r;
    r.Clear();
    CHECK(app::LoadPresetText(text, sizeof(text) - 1, s, r) == app::PresetLoad::BAD_EFFECT);
    CHECK(!(s != ref));

    const char old[] = "effect = FLANGERATOR\nmix = 3\n";
    CHECK(app::LoadPresetText(old, sizeof(old) - 1, s, r) == app::PresetLoad::BAD_EFFECT);
    const char junk[] = "nothing to see\n";
    CHECK(app::LoadPresetText(junk, sizeof(junk) - 1, s, r) == app::PresetLoad::BAD_FILE);
    CHECK(!(s != ref));
}

TEST(chain_presets_garbage_is_safe)
{
    Rng rng;
    for(int trial = 0; trial < 200; trial++)
    {
        // A valid file with random bytes flipped
        app::Settings in = RandomChain(rng, trial);
        char          text[4096];
        size_t        len = app::SerializeChainPreset(in, "X", text, sizeof(text));
        for(int j = 0; j < 20; j++)
            text[(size_t)(rng.Uni() * len)] = (char)(rng.Uni() * 256);
        app::Settings s = app::MakeDefaults();
        app::Recall   r;
        r.Clear();
        app::LoadPresetText(text, len, s, r);
        CHECKF(app::Valid(s), "trial %d: invalid settings after garbage", trial);
        app::PresetInfo info = app::PeekPresetText(text, len);
        char            sum[16];
        app::PresetSummary(info, sum, sizeof(sum));
        CHECK(std::strlen(sum) < sizeof(sum));
    }
    app::Settings s = app::MakeDefaults();
    app::Recall   r;
    r.Clear();
    app::LoadPresetText("", 0, s, r);
    app::LoadPresetText("[slot1", 6, s, r);
    app::LoadPresetText("[slot1]", 7, s, r); // no newline
    CHECK(app::Valid(s));
}

TEST(chain_presets_summary_text)
{
    app::PresetInfo info;
    std::memset(&info, 0, sizeof(info));
    info.known = true;
    char buf[16];
    auto set = [&](uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
        info.type[0] = a;
        info.type[1] = b;
        info.type[2] = c;
        info.type[3] = d;
        app::PresetSummary(info, buf, sizeof(buf));
    };
    const uint8_t N = fx::FX_NONE, R = fx::FX_REVERB, D = fx::FX_DELAY;
    set(N, N, N, N);
    CHECK(std::strcmp(buf, "EMPTY") == 0);
    set(R, N, N, N);
    CHECK(std::strcmp(buf, "REVERB") == 0);
    set(N, N, D, N);
    CHECK(std::strcmp(buf, "DELAY") == 0);
    set(D, N, R, N);
    CHECK(std::strcmp(buf, "DEL>REV") == 0);
    set(D, R, R, R);
    CHECK(std::strcmp(buf, "DLRVRVRV") == 0);
    set(D, N, R, R);
    CHECK(std::strcmp(buf, "DL>RV>RV") == 0);
    const uint8_t C = fx::FX_CRUSH, H = fx::FX_CHORUS, S = fx::FX_RESON;
    set(C, H, N, N);
    CHECK(std::strcmp(buf, "CRU>CHO") == 0);
    set(R, S, N, N);
    CHECK(std::strcmp(buf, "REV>RES") == 0);
    set(C, H, S, R);
    CHECK(std::strcmp(buf, "CRCHRSRV") == 0);
    info.known = false;
    app::PresetSummary(info, buf, sizeof(buf));
    CHECK(buf[0] == 0);
    // must fit the SD page's status area (x = 24..59, 4 px per char)
    set(D, R, D, R);
    CHECK(std::strlen(buf) <= 9);
}

TEST(effect_codes_and_names_are_unique)
{
    for(uint8_t a = 0; a < fx::FX_COUNT; a++)
    {
        const fx::Effect *ea = fx::Prototype(a);
        CHECKF(std::strlen(ea->Code()) == 2, "%s: code must be 2 letters", ea->Name());
        CHECKF(std::strlen(ea->Name()) <= 7, "%s: name longer than 7", ea->Name());
        for(uint8_t b = a + 1; b < fx::FX_COUNT; b++)
        {
            const fx::Effect *eb = fx::Prototype(b);
            CHECKF(std::strcmp(ea->Code(), eb->Code()) != 0, "%s / %s share a code", ea->Name(), eb->Name());
            CHECKF(std::strncmp(ea->Name(), eb->Name(), 3) != 0,
                   "%s / %s share their first 3 letters (2-effect summary)",
                   ea->Name(),
                   eb->Name());
        }
    }
}

TEST(midi_cc_map_reaches_every_slot)
{
    app::Settings s = app::MakeDefaults();
    app::Recall   r;
    r.Clear();
    app::SetSlotType(s, r, 1, fx::FX_DELAY);
    app::SetSlotType(s, r, 3, fx::FX_REVERB);
    // slot 1 = REVERB (4 params), 2 = DELAY (6), 3 = empty, 4 = REVERB

    for(int cc = 0; cc < 128; cc++)
    {
        app::CcTarget t = app::MapMidiCc(s, (uint8_t)cc);
        if(cc >= 20 && cc < 52)
        {
            int               k = (cc - 20) / 8, p = (cc - 20) % 8;
            const fx::Effect *e = fx::Prototype(s.slot[k].type);
            bool              want = e && p < (int)e->NumParams();
            CHECKF((t.kind == app::CcTarget::PARAM) == want, "CC %d", cc);
            if(want)
                CHECK(t.slot == k && t.param == p);
        }
        else if(cc >= 52 && cc < 56)
        {
            bool want = s.slot[cc - 52].type != fx::FX_NONE;
            CHECKF((t.kind == app::CcTarget::BYPASS) == want, "CC %d", cc);
        }
        else
        {
            CHECKF(t.kind == app::CcTarget::NONE, "CC %d", cc);
        }
    }

    // Slot 1 keeps CC 20-27 as before phase 3
    CHECK(app::ApplyMidiCc(s, 21, 127).kind == app::CcTarget::PARAM);
    CHECK(s.slot[0].norm[fx::Reverb::MIX] == 1.f);
    // CC 28 + 2 = slot 2, param 2 (MIX of the delay)
    app::ApplyMidiCc(s, 30, 0);
    CHECK(s.slot[1].norm[fx::Delay::MIX] == 0.f);
    // slot 4, param 0
    app::ApplyMidiCc(s, 44, 64);
    CHECK(fabsf(s.slot[3].norm[0] - 64.f / 127.f) < 1e-6f);
    // empty slot 3: nothing changes
    app::Settings before = s;
    CHECK(app::ApplyMidiCc(s, 36, 100).kind == app::CcTarget::NONE);
    CHECK(app::ApplyMidiCc(s, 54, 127).kind == app::CcTarget::NONE);
    CHECK(!(s != before));

    // Bypass CCs: >= 64 on, < 64 off; the bypass mode is kept
    s.slot[1].flags = app::SLOT_HARD;
    app::ApplyMidiCc(s, 53, 64);
    CHECK(s.slot[1].Bypassed() && s.slot[1].Hard());
    app::ApplyMidiCc(s, 53, 63);
    CHECK(!s.slot[1].Bypassed() && s.slot[1].Hard());
    app::ApplyMidiCc(s, 52, 127);
    CHECK(s.slot[0].Bypassed());
    CHECK(app::Valid(s));
}
