#include "app/preset_text.h"
#include <cstdio>
#include <cstring>

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
    }
    return "";
}

/** Number to text without %f (newlib-nano). */
static void FormatReal(fx::Unit u, float v, char *buf, size_t cap)
{
    if(u == fx::Unit::Seconds)
    {
        int c = (int)(v * 100.f + 0.5f);
        snprintf(buf, cap, "%d.%02d", c / 100, c % 100);
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
        char line[96];
        if(comment)
            snprintf(line, sizeof(line), "%-7s= %-10s # %s", key, value, comment);
        else
            snprintf(line, sizeof(line), "%-7s= %s", key, value);
        Line(line);
    }
};

// ----------------------------------------------------------- serialize
size_t SerializePreset(const fx::Effect &effect,
                       const Settings   &settings,
                       const char       *name,
                       char             *out,
                       size_t            capacity)
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
    w.KeyValue("input", settings.input_mode == INPUT_MONO ? "mono" : "stereo");
    w.Line();
    w.Line("# parameters, in real units (k = x1000, e.g. 8.5k)");
    for(size_t i = 0; i < effect.NumParams(); i++)
    {
        const fx::ParamSpec &s = effect.Spec(i);
        MakeKey(s.name, key, sizeof(key));
        FormatReal(s.unit, s.ToReal(settings.norm[i]), val, sizeof(val));
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
        int8_t r = settings.route[s];
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
                      Settings         &s,
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
        return true; // informational
    if(strcmp(key, "input") == 0)
    {
        if(strcmp(lval, "mono") == 0)
            s.input_mode = INPUT_MONO;
        else if(strcmp(lval, "stereo") == 0)
            s.input_mode = INPUT_STEREO;
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
                        Settings         &settings)
{
    ParseResult res;
    res.applied = 0;
    res.errors  = 0;
    res.name[0] = 0;

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
        if(!ApplyLine(effect, line, settings, res))
            res.errors++;
    }
    return res;
}

} // namespace app
