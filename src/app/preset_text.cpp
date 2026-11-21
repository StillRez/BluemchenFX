#include "app/preset_text.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include "fx/registry.h"

namespace app
{
static const char *const kSourceKey[SRC_LAST] = {"knob1", "knob2", "cv1", "cv2"};

// ------------------------------------------------------------- helpers
static char Lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/** "LO CUT" -> "lo_cut" (also used to normalise keys read from file). */
static void MakeKey(const char *src, char *dst, size_t cap)
{
    size_t i = 0;
    for(; src[i] && i + 1 < cap; i++)
    {
        char c = src[i];
        dst[i] = (c == ' ' || c == '-') ? '_' : Lower(c);
    }
    dst[i] = 0;
}

static const char *UnitSuffix(fx::Unit u)
{
    switch(u)
    {
        case fx::Unit::Seconds: return "s";
        case fx::Unit::Percent: return "%";
        case fx::Unit::Hertz: return "Hz";
        case fx::Unit::Millis: return "ms";
        case fx::Unit::Bits: return "bits";
    }
    return "";
}

/** Number to text without %f (newlib-nano). */
static void FormatReal(fx::Unit u, float v, char *buf, size_t cap)
{
    if(u == fx::Unit::Seconds || (u == fx::Unit::Hertz && v < 9.995f))
    {
        int c = (int)(v * 100.f + 0.5f);
        snprintf(buf, cap, "%d.%02d", c / 100, c % 100);
    }
    else if(u == fx::Unit::Millis && v < 9.95f)
    {
        int d = (int)(v * 10.f + 0.5f);
        snprintf(buf, cap, "%d.%d", d / 10, d % 10);
    }
    else
    {
        snprintf(buf, cap, "%d", (int)(v + 0.5f));
    }
}

/** Parses "12", "-3.5", "8.5k" (k = x1000). Trailing units are ignored. */
static bool ParseNumber(const char *s, float &out)
{
    while(*s == ' ' || *s == '\t')
        s++;
    float sign = 1.f;
    if(*s == '-' || *s == '+')
    {
        sign = *s == '-' ? -1.f : 1.f;
        s++;
    }
    float v = 0.f;
    bool  any = false;
    while(*s >= '0' && *s <= '9')
    {
        v   = v * 10.f + (float)(*s++ - '0');
        any = true;
    }
    if(*s == '.')
    {
        s++;
        float scale = 0.1f;
        while(*s >= '0' && *s <= '9')
        {
            v += scale * (float)(*s++ - '0');
            scale *= 0.1f;
            any = true;
        }
    }
    if(!any)
        return false;
    while(*s == ' ' || *s == '\t')
        s++;
    if(*s == 'k' || *s == 'K')
        v *= 1000.f;
    out = v * sign;
    return true;
}

static void Trim(char *s)
{
    char *start = s;
    while(*start == ' ' || *start == '\t')
        start++;
    if(start != s)
        memmove(s, start, strlen(start) + 1);
    size_t n = strlen(s);
    while(n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = 0;
}

struct Writer
{
    char  *buf;
    size_t cap, len;
    bool   overflow;
    int    keyw = 7; // key column width

    void Add(const char *s)
    {
        size_t n = strlen(s);
        if(len + n >= cap)
        {
            overflow = true;
            return;
        }
        memcpy(buf + len, s, n);
        len += n;
        buf[len] = 0;
    }
    void Line(const char *s = "")
    {
        Add(s);
        Add("\r\n"); // CRLF so Windows Notepad is happy
    }
    void KeyValue(const char *key, const char *value, const char *comment = nullptr)
    {
        char line[128];
        if(comment)
            snprintf(line, sizeof(line), "%-*s= %-10s # %s", keyw, key, value, comment);
        else
            snprintf(line, sizeof(line), "%-*s= %s", keyw, key, value);
        Line(line);
    }
};

// ----------------------------------------------------------- serialize
size_t SerializePreset(const fx::Effect  &effect,
                       const EffectState &state,
                       uint8_t            input_mode,
                       const char        *name,
                       char              *out,
                       size_t             capacity)
{
    Writer w{out, capacity, 0, false};
    char   key[24], val[16], cmt[48], lo[16], hi[16];

    w.Line("# BluemchenFX preset");
    w.Line("# Edit with any text editor. '#' starts a comment.");
    w.Line("# Lines you delete keep the module's current value.");
    w.Line();
    w.KeyValue("name", name);
    w.KeyValue("effect", effect.Name());
    w.Line();
    w.Line("# input: stereo | mono (mono = left input feeds both channels)");
    w.KeyValue("input", input_mode == INPUT_MONO ? "mono" : "stereo");
    w.Line();
    w.Line("# parameters, in real units (k = x1000, e.g. 8.5k)");
    for(size_t i = 0; i < effect.NumParams(); i++)
    {
        const fx::ParamSpec &s = effect.Spec(i);
        MakeKey(s.name, key, sizeof(key));
        FormatReal(s.unit, s.ToReal(state.norm[i]), val, sizeof(val));
        FormatReal(s.unit, s.min, lo, sizeof(lo));
        FormatReal(s.unit, s.max, hi, sizeof(hi));
        snprintf(cmt, sizeof(cmt), "%s .. %s %s", lo, hi, UnitSuffix(s.unit));
        w.KeyValue(key, val, cmt);
    }
    w.Line();

    // routing help line: "none | decay | mix | ..."
    char help[96] = "# routing: none";
    for(size_t i = 0; i < effect.NumParams(); i++)
    {
        MakeKey(effect.Spec(i).name, key, sizeof(key));
        if(strlen(help) + strlen(key) + 4 < sizeof(help))
        {
            strcat(help, " | ");
            strcat(help, key);
        }
    }
    w.Line(help);
    w.Line("# knobs set the value directly, CV adds +-5V = +-full range");
    for(int s = 0; s < SRC_LAST; s++)
    {
        int8_t r = state.route[s];
        if(r == kRouteNone)
            strcpy(key, "none");
        else
            MakeKey(effect.Spec(r).name, key, sizeof(key));
        w.KeyValue(kSourceKey[s], key);
    }

    return w.overflow ? 0 : w.len;
}

// --------------------------------------------------------------- parse
static int FindParam(const fx::Effect &effect, const char *key)
{
    char k[24];
    for(size_t i = 0; i < effect.NumParams(); i++)
    {
        MakeKey(effect.Spec(i).name, k, sizeof(k));
        if(strcmp(k, key) == 0)
            return (int)i;
    }
    return -1;
}

static bool ApplyLine(const fx::Effect &effect,
                      char             *line,
                      EffectState      &s,
                      uint8_t          &input_mode,
                      ParseResult      &res)
{
    // strip comment
    for(char *p = line; *p; p++)
        if(*p == '#' || *p == ';')
        {
            *p = 0;
            break;
        }
    Trim(line);
    if(!line[0])
        return true; // blank / comment only

    char *eq = strchr(line, '=');
    if(!eq)
        return false;
    *eq       = 0;
    char *raw = line;
    char *val = eq + 1;
    Trim(raw);
    Trim(val);

    char key[24], lval[24];
    MakeKey(raw, key, sizeof(key));
    MakeKey(val, lval, sizeof(lval));

    if(strcmp(key, "name") == 0)
    {
        strncpy(res.name, val, kPresetNameLen - 1);
        res.name[kPresetNameLen - 1] = 0;
        return true;
    }
    if(strcmp(key, "effect") == 0)
    {
        strncpy(res.effect, val, kEffectNameLen - 1);
        res.effect[kEffectNameLen - 1] = 0;
        return true;
    }
    if(strcmp(key, "input") == 0)
    {
        if(strcmp(lval, "mono") == 0)
            input_mode = INPUT_MONO;
        else if(strcmp(lval, "stereo") == 0)
            input_mode = INPUT_STEREO;
        else
            return false;
        res.applied++;
        return true;
    }
    for(int src = 0; src < SRC_LAST; src++)
    {
        if(strcmp(key, kSourceKey[src]) != 0)
            continue;
        if(strcmp(lval, "none") == 0 || strcmp(lval, "off") == 0
           || strcmp(lval, "__") == 0)
        {
            s.route[src] = kRouteNone;
            res.applied++;
            return true;
        }
        int p = FindParam(effect, lval);
        if(p < 0)
            return false;
        s.route[src] = (int8_t)p;
        res.applied++;
        return true;
    }
    int p = FindParam(effect, key);
    if(p >= 0)
    {
        float v;
        if(!ParseNumber(val, v))
            return false;
        s.norm[p] = effect.Spec(p).ToNorm(v);
        res.applied++;
        return true;
    }
    return true; // unknown key: ignore (forward compatible)
}

ParseResult ParsePreset(const fx::Effect &effect,
                        const char       *text,
                        size_t            len,
                        EffectState      &state,
                        uint8_t          &input_mode)
{
    ParseResult res;
    res.applied   = 0;
    res.errors    = 0;
    res.name[0]   = 0;
    res.effect[0] = 0;

    char   line[96];
    size_t i = 0;
    while(i < len)
    {
        size_t n = 0;
        while(i < len && text[i] != '\n' && text[i] != '\r')
        {
            if(n + 1 < sizeof(line))
                line[n++] = text[i];
            i++;
        }
        while(i < len && (text[i] == '\n' || text[i] == '\r'))
            i++;
        line[n] = 0;
        if(!ApplyLine(effect, line, state, input_mode, res))
            res.errors++;
    }
    return res;
}

int FindPresetEffect(const fx::Effect *const *effects,
                     size_t                   num_effects,
                     const char              *text,
                     size_t                   len)
{
    if(num_effects == 0)
        return kPresetBadEffect;
    // Parse into scratch state just to pick up the "effect" value
    EffectState scratch;
    uint8_t     scratch_input = 0;
    std::memset(&scratch, 0, sizeof(scratch));
    ParseResult res = ParsePreset(*effects[0], text, len, scratch, scratch_input);
    if(!res.effect[0])
        return kPresetNoEffect;

    char want[kEffectNameLen], have[kEffectNameLen];
    MakeKey(res.effect, want, sizeof(want));
    for(size_t i = 0; i < num_effects; i++)
    {
        MakeKey(effects[i]->Name(), have, sizeof(have));
        if(strcmp(want, have) == 0)
            return (int)i;
    }
    return kPresetBadEffect;
}

// ======================================================= chain presets
static constexpr float kOutMinDb = -24.f;
static constexpr float kOutMaxDb = 6.f;

/** One line of a preset file, split up. */
struct Line
{
    enum Kind
    {
        BLANK,
        SECTION, // [slotN]: section = 0..3, or -1 for an unknown section
        KEY_VALUE,
        JUNK,
    } kind;
    int  section;
    char key[24];  // normalised key
    char lval[24]; // normalised value (lower case, '_' for spaces)
    char val[48];  // value as written (trimmed)
};

/** Copies the next line of `text` (from `i`) into `buf`. False at the end. */
static bool NextLine(const char *text, size_t len, size_t &i, char *buf, size_t cap)
{
    if(i >= len)
        return false;
    size_t n = 0;
    while(i < len && text[i] != '\n' && text[i] != '\r')
    {
        if(n + 1 < cap)
            buf[n++] = text[i];
        i++;
    }
    while(i < len && (text[i] == '\n' || text[i] == '\r'))
        i++;
    buf[n] = 0;
    return true;
}

static Line SplitLine(char *raw)
{
    Line l;
    l.kind    = Line::BLANK;
    l.section = -1;
    l.key[0] = l.lval[0] = l.val[0] = 0;
    for(char *p = raw; *p; p++)
        if(*p == '#' || *p == ';')
        {
            *p = 0;
            break;
        }
    Trim(raw);
    if(!raw[0])
        return l;
    if(raw[0] == '[')
    {
        char *end = strchr(raw, ']');
        l.kind    = Line::SECTION;
        if(!end)
        {
            l.kind = Line::JUNK;
            return l;
        }
        *end = 0;
        char name[24];
        MakeKey(raw + 1, name, sizeof(name));
        // accept "slot1", "slot 1", "slot_1"
        char *n = name;
        while(*n == '_')
            n++;
        if(strncmp(n, "slot", 4) == 0)
        {
            n += 4;
            while(*n == '_')
                n++;
            if(n[0] >= '1' && n[0] < '1' + (int)kNumSlots && (!n[1] || n[1] == '_'))
                l.section = n[0] - '1';
        }
        return l;
    }
    char *eq = strchr(raw, '=');
    if(!eq)
    {
        l.kind = Line::JUNK;
        return l;
    }
    *eq = 0;
    char *k = raw, *v = eq + 1;
    Trim(k);
    Trim(v);
    l.kind = Line::KEY_VALUE;
    MakeKey(k, l.key, sizeof(l.key));
    MakeKey(v, l.lval, sizeof(l.lval));
    strncpy(l.val, v, sizeof(l.val) - 1);
    l.val[sizeof(l.val) - 1] = 0;
    return l;
}

/** Effect id for a name ("none"/empty -> FX_NONE); false if unknown. */
static bool LookupEffect(const char *lval, uint8_t &type)
{
    if(!lval[0] || strcmp(lval, "none") == 0 || strcmp(lval, "empty") == 0
       || strcmp(lval, "__") == 0)
    {
        type = fx::FX_NONE;
        return true;
    }
    char have[kEffectNameLen];
    for(uint8_t t = 0; t < fx::FX_COUNT; t++)
    {
        MakeKey(fx::Prototype(t)->Name(), have, sizeof(have));
        if(strcmp(lval, have) == 0)
        {
            type = t;
            return true;
        }
    }
    return false;
}

static bool OnOff(const char *lval, bool &on)
{
    if(!strcmp(lval, "on") || !strcmp(lval, "yes") || !strcmp(lval, "1"))
        on = true;
    else if(!strcmp(lval, "off") || !strcmp(lval, "no") || !strcmp(lval, "0"))
        on = false;
    else
        return false;
    return true;
}

static int SourceIndex(const char *key)
{
    for(int src = 0; src < SRC_LAST; src++)
        if(strcmp(key, kSourceKey[src]) == 0)
            return src;
    return -1;
}

/** Whether the text has at least one [section] line. */
static bool HasSections(const char *text, size_t len)
{
    char   buf[96];
    size_t i = 0;
    while(NextLine(text, len, i, buf, sizeof(buf)))
        if(SplitLine(buf).kind == Line::SECTION)
            return true;
    return false;
}

static void FormatDb(float db, char *buf, size_t cap)
{
    int t = (int)roundf(db * 10.f); // tenths
    snprintf(buf, cap, "%s%d.%d", t < 0 ? "-" : "", (t < 0 ? -t : t) / 10, (t < 0 ? -t : t) % 10);
}

static void RouteValue(const Settings &s, int src, char *buf, size_t cap)
{
    const Route       &r = s.route[src];
    const fx::Effect *e = r.None() ? nullptr : fx::Prototype(s.slot[r.slot].type);
    if(!e || r.param < 0 || r.param >= (int)e->NumParams())
    {
        snprintf(buf, cap, "none");
        return;
    }
    char key[16];
    MakeKey(e->Spec(r.param).name, key, sizeof(key)); // names are <= 7 chars
    snprintf(buf, cap, "%d.%s", r.slot + 1, key);
}

size_t SerializeChainPreset(const Settings &s, const char *name, char *out, size_t capacity)
{
    Writer w{out, capacity, 0, false, 12};
    char   key[24], val[32], cmt[48], lo[16], hi[16], line[48];

    w.Line("# BluemchenFX chain preset");
    w.Line("# Edit with any text editor. '#' starts a comment.");
    w.Line("# Lines you delete keep the module's current value;");
    w.Line("# a [slotN] section you delete empties that slot.");
    w.Line();
    w.KeyValue("name", name);
    w.Line("# input: stereo | mono (mono = left input feeds both channels)");
    w.KeyValue("input", s.input_mode == INPUT_MONO ? "mono" : "stereo");
    const float db = s.out_gain > 0.f ? 20.f * log10f(s.out_gain) : kOutMinDb;
    FormatDb(roundf(db * 2.f) * 0.5f, val, sizeof(val));
    w.KeyValue("out", val, "output level, -24.0 .. 6.0 dB");

    for(size_t k = 0; k < kNumSlots; k++)
    {
        const SlotState  &sl = s.slot[k];
        const fx::Effect *e  = fx::Prototype(sl.type);
        w.Line();
        snprintf(line, sizeof(line), "[slot%d]", (int)k + 1);
        w.Line(line);
        if(!e)
        {
            w.KeyValue("effect", "none");
            continue;
        }
        w.KeyValue("effect", e->Name());
        w.KeyValue("bypass", sl.Bypassed() ? "on" : "off");
        w.KeyValue("bypass_mode", sl.Hard() ? "hard" : "trails", "trails | hard");
        for(size_t i = 0; i < e->NumParams(); i++)
        {
            const fx::ParamSpec &sp = e->Spec(i);
            MakeKey(sp.name, key, sizeof(key));
            FormatReal(sp.unit, sp.ToReal(sl.norm[i]), val, sizeof(val));
            FormatReal(sp.unit, sp.min, lo, sizeof(lo));
            FormatReal(sp.unit, sp.max, hi, sizeof(hi));
            snprintf(cmt, sizeof(cmt), "%s .. %s %s", lo, hi, UnitSuffix(sp.unit));
            w.KeyValue(key, val, cmt);
        }
    }

    w.Line();
    w.Line("# routing: none | <slot>.<param>, e.g. 1.time or 2.decay");
    w.Line("# knobs set the value directly, CV adds +-5V = +-full range");
    for(int src = 0; src < SRC_LAST; src++)
    {
        RouteValue(s, src, val, sizeof(val));
        w.KeyValue(kSourceKey[src], val);
    }
    return w.overflow ? 0 : w.len;
}

/** Old single-effect file: same behaviour as before phase 3. */
static PresetLoad LoadSingle(const char *text, size_t len, Settings &s, Recall &recall)
{
    int idx = FindPresetEffect(fx::Prototypes(), fx::FX_COUNT, text, len);
    if(idx == kPresetBadEffect)
        return PresetLoad::BAD_EFFECT;
    uint8_t cur  = s.slot[0].type;
    uint8_t type = idx >= 0 ? (uint8_t)idx : (cur < fx::FX_COUNT ? cur : (uint8_t)fx::FX_REVERB);
    const fx::Effect *e = fx::Prototype(type);

    EffectState st = SlotAsEffectState(s, 0);
    if(type != cur)
    {
        // A file for another effect starts from that effect's defaults
        DefaultNorms(type, st.norm);
        for(int r = 0; r < SRC_LAST; r++)
        {
            int p       = e->DefaultRoute(r);
            st.route[r] = (int8_t)(p >= 0 ? p : kRouteNone);
        }
    }
    uint8_t     mode = s.input_mode;
    ParseResult res  = ParsePreset(*e, text, len, st, mode);
    if(res.applied == 0)
        return PresetLoad::BAD_FILE;
    ApplySingleEffectPreset(s, recall, type, st, mode);
    return PresetLoad::OK;
}

PresetLoad LoadPresetText(const char *text, size_t len, Settings &s, Recall &recall)
{
    if(!HasSections(text, len))
        return LoadSingle(text, len, s, recall);

    Settings next = s;
    Recall   rec  = recall;
    char     buf[96];
    size_t   i;

    // Pass 1: which slots are mentioned, and with which effect
    bool    present[kNumSlots] = {};
    bool    has_fx[kNumSlots]  = {};
    uint8_t want[kNumSlots];
    int     section = -2; // -2 = global, -1 = unknown section
    i               = 0;
    while(NextLine(text, len, i, buf, sizeof(buf)))
    {
        Line l = SplitLine(buf);
        if(l.kind == Line::SECTION)
        {
            section = l.section;
            if(section >= 0)
                present[section] = true;
        }
        else if(l.kind == Line::KEY_VALUE && section >= 0 && !strcmp(l.key, "effect"))
        {
            if(!LookupEffect(l.lval, want[section]))
                return PresetLoad::BAD_EFFECT;
            has_fx[section] = true;
        }
    }

    // Set the chain's shape. Changed slots start from the new defaults.
    for(size_t k = 0; k < kNumSlots; k++)
    {
        uint8_t t = !present[k] ? (uint8_t)fx::FX_NONE
                    : has_fx[k] ? want[k]
                                : next.slot[k].type;
        if(t != next.slot[k].type)
        {
            SetSlotType(next, rec, (int)k, t);
            DefaultNorms(t, next.slot[k].norm);
        }
        next.slot[k].flags = 0; // bypass off, trails unless the file says so
    }

    // Pass 2: values
    section = -2;
    i       = 0;
    while(NextLine(text, len, i, buf, sizeof(buf)))
    {
        Line l = SplitLine(buf);
        if(l.kind == Line::SECTION)
        {
            section = l.section;
            continue;
        }
        if(l.kind != Line::KEY_VALUE || section == -1)
            continue; // blank, junk, or inside an unknown section
        const int         slot = section >= 0 ? section : 0;
        SlotState        &sl   = next.slot[slot];
        const fx::Effect *e    = fx::Prototype(sl.type);

        const int src = SourceIndex(l.key);
        if(src >= 0)
        {
            if(!strcmp(l.lval, "none") || !strcmp(l.lval, "off"))
            {
                next.route[src] = Route{kRouteNone, kRouteNone};
                continue;
            }
            int         rs = slot;
            const char *pk = l.lval;
            if(pk[0] >= '1' && pk[0] < '1' + (int)kNumSlots && pk[1] == '.')
            {
                rs = pk[0] - '1';
                pk += 2;
            }
            const fx::Effect *re = fx::Prototype(next.slot[rs].type);
            int               p  = re ? FindParam(*re, pk) : -1;
            if(p >= 0)
                next.route[src] = Route{(int8_t)rs, (int8_t)p};
            continue;
        }
        if(section < 0)
        {
            if(!strcmp(l.key, "input"))
            {
                if(!strcmp(l.lval, "mono"))
                    next.input_mode = INPUT_MONO;
                else if(!strcmp(l.lval, "stereo"))
                    next.input_mode = INPUT_STEREO;
            }
            else if(!strcmp(l.key, "out"))
            {
                float db;
                if(ParseNumber(l.val, db))
                {
                    db = roundf(db * 2.f) * 0.5f;
                    db = db < kOutMinDb ? kOutMinDb : (db > kOutMaxDb ? kOutMaxDb : db);
                    next.out_gain = db == 0.f ? 1.f : powf(10.f, db / 20.f);
                }
            }
            continue; // name, unknown global keys
        }
        bool on;
        if(!strcmp(l.key, "bypass"))
        {
            if(OnOff(l.lval, on))
                sl.flags = on ? (sl.flags | SLOT_BYPASS) : (sl.flags & ~SLOT_BYPASS);
            continue;
        }
        if(!strcmp(l.key, "bypass_mode"))
        {
            if(!strcmp(l.lval, "hard"))
                sl.flags |= SLOT_HARD;
            else if(!strcmp(l.lval, "trails"))
                sl.flags &= ~SLOT_HARD;
            continue;
        }
        if(!e)
            continue;
        int p = FindParam(*e, l.key);
        float v;
        if(p >= 0 && ParseNumber(l.val, v))
        {
            float n   = e->Spec(p).ToNorm(v);
            sl.norm[p] = n < 0.f ? 0.f : (n > 1.f ? 1.f : n);
        }
    }

    // Routes must point at a parameter that exists
    for(int src = 0; src < SRC_LAST; src++)
    {
        Route            &r = next.route[src];
        const fx::Effect *e = r.None() ? nullptr : fx::Prototype(next.slot[r.slot].type);
        if(!e || r.param < 0 || r.param >= (int)e->NumParams())
            r = Route{kRouteNone, kRouteNone};
    }
    // Recall follows what the preset loaded
    for(size_t k = 0; k < kNumSlots; k++)
        if(next.slot[k].type < fx::FX_COUNT)
        {
            std::memcpy(rec.norm[k][next.slot[k].type], next.slot[k].norm, sizeof(next.slot[k].norm));
            rec.valid[k][next.slot[k].type] = true;
        }

    s      = next;
    recall = rec;
    return PresetLoad::OK;
}

PresetInfo PeekPresetText(const char *text, size_t len)
{
    PresetInfo info;
    std::memset(&info, 0, sizeof(info));
    for(size_t k = 0; k < kNumSlots; k++)
        info.type[k] = fx::FX_NONE;
    info.chain = HasSections(text, len);
    info.known = true;

    char   buf[96];
    size_t i       = 0;
    int    section = -2;
    bool   present[kNumSlots] = {};
    bool   has_fx[kNumSlots]  = {};
    bool   any_fx              = false;
    while(NextLine(text, len, i, buf, sizeof(buf)))
    {
        Line l = SplitLine(buf);
        if(l.kind == Line::SECTION)
        {
            section = l.section;
            if(section >= 0)
                present[section] = true;
            continue;
        }
        if(l.kind != Line::KEY_VALUE)
            continue;
        if(section == -2 && !strcmp(l.key, "name"))
        {
            snprintf(info.name, sizeof(info.name), "%.*s", (int)kPresetNameLen - 1, l.val);
        }
        else if(!strcmp(l.key, "effect") && (info.chain ? section >= 0 : true))
        {
            int     slot = info.chain ? section : 0;
            uint8_t t;
            if(!LookupEffect(l.lval, t))
                info.known = false;
            else
                info.type[slot] = t;
            has_fx[slot] = true;
            any_fx       = true;
        }
    }
    if(info.chain)
    {
        // a section without an effect line keeps the module's current type
        for(size_t k = 0; k < kNumSlots; k++)
            if(present[k] && !has_fx[k])
                info.known = false;
    }
    else if(!any_fx)
    {
        info.known = false; // applies to whatever is in slot 1
    }
    return info;
}

void PresetSummary(const PresetInfo &info, char *buf, size_t cap)
{
    buf[0] = 0;
    if(!info.known || cap == 0)
        return;
    const fx::Effect *e[kNumSlots];
    size_t            n = 0;
    for(size_t k = 0; k < kNumSlots; k++)
        if(const fx::Effect *p = fx::Prototype(info.type[k]))
            e[n++] = p;
    if(n == 0)
    {
        snprintf(buf, cap, "EMPTY");
        return;
    }
    if(n == 1)
    {
        snprintf(buf, cap, "%s", e[0]->Name());
        return;
    }
    // two: first 3 letters of each name; three: 2-letter codes with '>';
    // four: codes back to back (9 characters fit the SD page)
    size_t len = 0;
    for(size_t j = 0; j < n; j++)
    {
        if(j && n < 4 && len + 1 < cap)
            buf[len++] = '>';
        const char  *src   = n == 2 ? e[j]->Name() : e[j]->Code();
        const size_t chars = n == 2 ? 3 : 2;
        for(size_t c = 0; c < chars && src[c] && len + 1 < cap; c++)
            buf[len++] = src[c];
    }
    buf[len] = 0;
}

} // namespace app
