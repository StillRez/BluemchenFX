#include "app/chain.h"
#include <cstring>

namespace app
{
// Single core: the audio interrupt can't run *during* a main-loop
// statement, but the compiler must not move memory accesses across the
// hand-over flags.
static inline void CompilerBarrier()
{
    __asm volatile("" ::: "memory");
}

void Chain::Init(float sample_rate, fx::Arena *arenas)
{
    sr_   = sample_rate;
    step_ = 1.f / (kFadeSec * sample_rate);
    for(size_t k = 0; k < kNumSlots; k++)
    {
        Slot &s   = slots_[k];
        s.arena   = &arenas[k];
        s.fx      = nullptr;
        s.type    = fx::FX_NONE;
        s.target  = fx::FX_NONE;
        s.pending = false;
        s.want_on = false;
        s.bypass  = false;
        s.hard    = false;
        s.idle    = true;
        s.g = s.b = 0.f;
        s.cleared      = true;
        s.init_failure = false;
    }
}

void Chain::SetType(size_t slot, uint8_t type, const float *norm)
{
    if(slot >= kNumSlots)
        return;
    Slot &s = slots_[slot];
    if(type != fx::FX_NONE && !fx::Prototype(type))
        type = fx::FX_NONE;
    std::memcpy(s.pending_norm, norm, sizeof(s.pending_norm));
    s.target  = type;
    s.pending = true;
    Poll(); // starts the fade-out now (or swaps at once if already idle)
}

void Chain::SetBypass(size_t slot, bool bypass, bool hard)
{
    if(slot >= kNumSlots)
        return;
    slots_[slot].hard   = hard;
    slots_[slot].bypass = bypass;
}

bool Chain::TakeInitFailure(size_t slot)
{
    bool r                    = slots_[slot].init_failure;
    slots_[slot].init_failure = false;
    return r;
}

void Chain::Poll()
{
    for(size_t k = 0; k < kNumSlots; k++)
    {
        Slot &s = slots_[k];

        if(s.pending)
        {
            s.want_on = false; // audio fades the slot to a wire
            CompilerBarrier();
            if(!s.idle)
                continue; // not faded out yet: try again next Poll()

            // Audio no longer touches s.fx: swap it.
            fx::Destroy(s.fx);
            s.fx   = nullptr;
            s.type = fx::FX_NONE;
            s.arena->Reset();
            if(s.target != fx::FX_NONE)
            {
                fx::Effect *e = fx::Construct(s.target, s.storage);
                if(e && e->Init(sr_, *s.arena))
                {
                    for(size_t i = 0; i < e->NumParams(); i++)
                        e->SetParam(i, s.pending_norm[i]);
                    e->Reset(); // snaps smoothing to the new values
                    s.fx   = e;
                    s.type = s.target;
                }
                else
                {
                    fx::Destroy(e);
                    s.arena->Reset();
                    s.target       = fx::FX_NONE;
                    s.init_failure = true;
                }
            }
            s.cleared = true;
            s.pending = false;
            CompilerBarrier(); // the new effect is complete before audio sees it
            s.want_on = s.fx != nullptr;
            continue;
        }

        // A hard-bypassed effect that has faded out is cleared once, so it
        // comes back without stale tails/echoes when un-bypassed.
        if(s.idle)
        {
            if(s.fx && !s.cleared && s.bypass && s.hard)
            {
                s.fx->Reset();
                s.cleared = true;
            }
        }
        else
        {
            s.cleared = false;
        }
    }
}

bool Chain::ProcessSlot(Slot        &s,
                        const float *in_l,
                        const float *in_r,
                        float       *out_l,
                        float       *out_r,
                        size_t       n,
                        const float *params,
                        uint8_t      params_type)
{
    const bool  hard_off = s.bypass && s.hard;
    const float g_target = s.want_on && !hard_off ? 1.f : 0.f;
    const float b_target = s.bypass && !s.hard ? 1.f : 0.f;

    if(s.g <= 0.f && g_target <= 0.f)
    {
        // Wire: the effect is not processed (and may be swapped/reset).
        // Nothing to copy: the next slot reads this slot's input.
        s.g    = 0.f;
        s.idle = true;
        return false;
    }
    s.idle = false;

    fx::Effect &e = *s.fx; // non-null: g > 0 only ever with an effect
    if(params_type == s.type)
        for(size_t i = 0; i < e.NumParams(); i++)
            e.SetParam(i, params[i]);

    // Steady and fully on (no ramp, not bypassed): the mix below would give
    // exactly the effect's output, so skip the ramps and copies.
    if(s.g >= 1.f && g_target >= 1.f && s.b <= 0.f && b_target <= 0.f)
    {
        e.Process(in_l, in_r, out_l, out_r, n);
        return true;
    }

    // Per-sample ramps, recorded so the mix below uses the same values
    float g[kMaxBlock], b[kMaxBlock];
    float gv = s.g, bv = s.b;
    for(size_t i = 0; i < n; i++)
    {
        gv += gv < g_target ? step_ : (gv > g_target ? -step_ : 0.f);
        gv = gv < 0.f ? 0.f : (gv > 1.f ? 1.f : gv);
        bv += bv < b_target ? step_ : (bv > b_target ? -step_ : 0.f);
        bv   = bv < 0.f ? 0.f : (bv > 1.f ? 1.f : bv);
        g[i] = gv;
        b[i] = bv;
        x_[0][i] = in_l[i] * (1.f - bv);
        x_[1][i] = in_r[i] * (1.f - bv);
    }
    s.g = gv;
    s.b = bv;

    e.Process(x_[0], x_[1], y_[0], y_[1], n);

    for(size_t i = 0; i < n; i++)
    {
        out_l[i] = in_l[i] * (1.f - g[i]) + g[i] * (y_[0][i] + in_l[i] * b[i]);
        out_r[i] = in_r[i] * (1.f - g[i]) + g[i] * (y_[1][i] + in_r[i] * b[i]);
    }
    return true;
}

void Chain::Process(const float   *in_l,
                    const float   *in_r,
                    float         *out_l,
                    float         *out_r,
                    size_t         size,
                    const float (*params)[kMaxParams],
                    const uint8_t *types)
{
    for(size_t pos = 0; pos < size; pos += kMaxBlock)
    {
        const size_t n = size - pos < kMaxBlock ? size - pos : kMaxBlock;

        // Copy in (input may alias the output, and L may alias R in mono)
        int cur = 0;
        std::memcpy(buf_[cur][0], in_l + pos, n * sizeof(float));
        std::memcpy(buf_[cur][1], in_r + pos, n * sizeof(float));

        for(size_t k = 0; k < kNumSlots; k++)
        {
            const int nxt = cur ^ 1;
            if(ProcessSlot(slots_[k],
                           buf_[cur][0],
                           buf_[cur][1],
                           buf_[nxt][0],
                           buf_[nxt][1],
                           n,
                           params[k],
                           types[k]))
                cur = nxt;
        }

        std::memcpy(out_l + pos, buf_[cur][0], n * sizeof(float));
        std::memcpy(out_r + pos, buf_[cur][1], n * sizeof(float));
    }
}

} // namespace app
