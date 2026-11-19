#include "app/chain_settings.h"
#include <cmath>
#include <cstddef>

namespace app
{
static bool NormsValid(const float *norm)
{
    for(size_t i = 0; i < kMaxParams; i++)
        if(!(norm[i] >= 0.f && norm[i] <= 1.f)) // also catches NaN
            return false;
    return true;
}

static bool ParamRoutesValid(const int8_t *route, size_t num_params)
{
    for(int i = 0; i < SRC_LAST; i++)
        if(route[i] < kRouteNone || route[i] >= (int)num_params)
            return false;
    return true;
}

static Route NoRoute()
{
    return {kRouteNone, kRouteNone};
}

void DefaultNorms(uint8_t type, float *norm)
{
    const fx::Effect *p = fx::Prototype(type);
    for(size_t i = 0; i < kMaxParams; i++)
        norm[i] = p && i < p->NumParams() ? p->Spec(i).def : 0.f;
}

Settings MakeDefaults()
{
    Settings s;
    std::memset(&s, 0, sizeof(s));
    s.version    = kSettingsVersion;
    s.input_mode = INPUT_STEREO;
    s.topology   = 0;
    s.out_gain   = 1.f;
    s.limit_db   = kLimitDefaultDb;
    s.limit_rel  = kReleaseDefault;
    for(size_t k = 0; k < kNumSlots; k++)
    {
        s.slot[k].type  = fx::FX_NONE;
        s.slot[k].flags = 0;
        DefaultNorms(fx::FX_NONE, s.slot[k].norm);
    }
    for(int r = 0; r < SRC_LAST; r++)
        s.route[r] = NoRoute();

    // Slot 1: reverb, with the knobs and CVs on its default parameters
    const fx::Effect *rev = fx::Prototype(fx::FX_REVERB);
    s.slot[0].type        = fx::FX_REVERB;
    DefaultNorms(fx::FX_REVERB, s.slot[0].norm);
    for(int r = 0; r < SRC_LAST; r++)
    {
        int p = rev->DefaultRoute(r);
        if(p >= 0)
            s.route[r] = {0, (int8_t)p};
    }
    return s;
}

bool Valid(const Settings &s)
{
    if(s.version != kSettingsVersion || s.input_mode >= INPUT_MODE_LAST
       || s.topology != 0 || s.ui_slot >= kNumSlots
       || !(s.out_gain >= 0.f && s.out_gain <= 4.f)
       || !(s.limit_db >= kLimitMinDb && s.limit_db <= kLimitMaxDb)
       || !(s.limit_rel >= kReleaseMinSec * 0.99f && s.limit_rel <= kReleaseMaxSec * 1.01f))
        return false;
    for(size_t k = 0; k < kNumSlots; k++)
    {
        const SlotState &sl = s.slot[k];
        if(sl.type != fx::FX_NONE && sl.type >= fx::FX_COUNT)
            return false;
        if(sl.flags & ~(SLOT_BYPASS | SLOT_HARD))
            return false;
        if(!NormsValid(sl.norm))
            return false;
    }
    for(int r = 0; r < SRC_LAST; r++)
    {
        const Route &rt = s.route[r];
        if(rt.None())
        {
            if(rt.param != kRouteNone)
                return false;
            continue;
        }
        if(rt.slot < 0 || rt.slot >= (int)kNumSlots)
            return false;
        const fx::Effect *p = fx::Prototype(s.slot[rt.slot].type);
        if(!p || rt.param < 0 || rt.param >= (int)p->NumParams())
            return false;
    }
    return true;
}

bool MigrateV2(const SettingsV2 &v2, Settings &out, Recall *recall)
{
    if(v2.version != kSettingsVersionV2 || v2.effect >= fx::FX_COUNT
       || v2.input_mode >= INPUT_MODE_LAST)
        return false;
    // v2 stored a state for every registered effect; all must be sane
    for(uint8_t e = 0; e < fx::FX_COUNT && e < kMaxEffectsV2; e++)
        if(!NormsValid(v2.fx[e].norm)
           || !ParamRoutesValid(v2.fx[e].route, fx::Prototype(e)->NumParams()))
            return false;

    out            = MakeDefaults();
    out.input_mode = v2.input_mode;
    const EffectState &act = v2.fx[v2.effect];
    out.slot[0].type       = v2.effect;
    std::memcpy(out.slot[0].norm, act.norm, sizeof(act.norm));
    for(int r = 0; r < SRC_LAST; r++)
        out.route[r] = act.route[r] == kRouteNone ? NoRoute()
                                                  : Route{0, act.route[r]};
    if(recall)
    {
        recall->Clear();
        for(uint8_t e = 0; e < fx::FX_COUNT && e < kMaxEffectsV2; e++)
        {
            std::memcpy(recall->norm[0][e], v2.fx[e].norm, sizeof(v2.fx[e].norm));
            recall->valid[0][e] = true;
        }
    }
    return true;
}

// v4 is v3 with the limiter fields appended (MigrateV3 copies the prefix)
static_assert(offsetof(Settings, route) == offsetof(SettingsV3, route), "v3 prefix");
static_assert(offsetof(Settings, limit_db) == sizeof(SettingsV3), "v4 appends");

bool MigrateV3(const SettingsV3 &v3, Settings &out)
{
    if(v3.version != kSettingsVersionV3)
        return false;
    Settings s = MakeDefaults(); // limiter defaults
    std::memcpy(&s, &v3, sizeof(v3)); // v4 = v3 + limiter fields at the end
    s.version = kSettingsVersion;
    if(!Valid(s))
        return false;
    out = s;
    return true;
}

bool MigrateV1(const SettingsV1 &v1, Settings &out)
{
    const fx::Effect *rev = fx::Prototype(fx::FX_REVERB);
    if(v1.version != kSettingsVersionV1 || !NormsValid(v1.norm)
       || !ParamRoutesValid(v1.route, rev->NumParams())
       || v1.input_mode >= INPUT_MODE_LAST)
        return false;
    out            = MakeDefaults(); // reverb in slot 1
    out.input_mode = v1.input_mode;
    std::memcpy(out.slot[0].norm, v1.norm, sizeof(v1.norm));
    for(int r = 0; r < SRC_LAST; r++)
        out.route[r]
            = v1.route[r] == kRouteNone ? NoRoute() : Route{0, v1.route[r]};
    return true;
}

bool SetSlotType(Settings &s, Recall &recall, int slot, uint8_t type)
{
    if(slot < 0 || slot >= (int)kNumSlots)
        return false;
    if(type != fx::FX_NONE && type >= fx::FX_COUNT)
        return false;
    SlotState &sl = s.slot[slot];
    if(sl.type == type)
        return false;

    // Remember what this slot had for the outgoing type
    if(sl.type < fx::FX_COUNT)
    {
        std::memcpy(recall.norm[slot][sl.type], sl.norm, sizeof(sl.norm));
        recall.valid[slot][sl.type] = true;
    }

    sl.type = type;
    if(type < fx::FX_COUNT && recall.valid[slot][type])
        std::memcpy(sl.norm, recall.norm[slot][type], sizeof(sl.norm));
    else
        DefaultNorms(type, sl.norm);

    // Sources that pointed at this slot follow the new effect's defaults
    const fx::Effect *p = fx::Prototype(type);
    for(int r = 0; r < SRC_LAST; r++)
    {
        if(s.route[r].slot != slot)
            continue;
        int param  = p ? p->DefaultRoute(r) : -1;
        s.route[r] = param >= 0 ? Route{(int8_t)slot, (int8_t)param} : NoRoute();
    }
    return true;
}

EffectState SlotAsEffectState(const Settings &s, int slot)
{
    EffectState st;
    std::memset(&st, 0, sizeof(st));
    std::memcpy(st.norm, s.slot[slot].norm, sizeof(st.norm));
    for(int r = 0; r < SRC_LAST; r++)
        st.route[r] = s.route[r].slot == slot ? s.route[r].param : kRouteNone;
    return st;
}

void ApplySingleEffectPreset(Settings          &s,
                             Recall            &recall,
                             uint8_t            type,
                             const EffectState &st,
                             uint8_t            input_mode)
{
    for(int k = 1; k < (int)kNumSlots; k++)
        SetSlotType(s, recall, k, fx::FX_NONE); // keeps their params in recall
    SetSlotType(s, recall, 0, type);
    std::memcpy(s.slot[0].norm, st.norm, sizeof(st.norm));
    s.slot[0].flags = 0; // old presets know nothing about bypass
    if(type < fx::FX_COUNT)
    {
        std::memcpy(recall.norm[0][type], st.norm, sizeof(st.norm));
        recall.valid[0][type] = true;
    }
    for(int r = 0; r < SRC_LAST; r++)
        s.route[r] = st.route[r] == kRouteNone ? NoRoute() : Route{0, st.route[r]};
    s.input_mode = input_mode;
}

} // namespace app
