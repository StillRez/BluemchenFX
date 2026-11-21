#include "fx/registry.h"
#include "fx/reverb.h"
#include "fx/delay.h"
#include "fx/crush.h"
#include "fx/chorus.h"
#include "fx/reson.h"
#include <new>

namespace fx
{
#define CHECK_STORAGE(T)                                                     \
    static_assert(sizeof(T) <= kEffectStorageBytes,                          \
                  #T " doesn't fit kEffectStorageBytes: raise it, or move "  \
                     "big members into the arena");                          \
    static_assert(alignof(T) <= kEffectStorageAlign, #T " alignment")

CHECK_STORAGE(Reverb);
CHECK_STORAGE(Delay);
CHECK_STORAGE(Crush);
CHECK_STORAGE(Chorus);
CHECK_STORAGE(Reson);

static Reverb              proto_reverb;
static Delay               proto_delay;
static Crush               proto_crush;
static Chorus              proto_chorus;
static Reson               proto_reson;
// Index = EffectId (persisted): append only
static const Effect *const protos[FX_COUNT]
    = {&proto_reverb, &proto_delay, &proto_crush, &proto_chorus, &proto_reson};

const Effect *Prototype(uint8_t id)
{
    return id < FX_COUNT ? protos[id] : nullptr;
}

const Effect *const *Prototypes()
{
    return protos;
}

Effect *Construct(uint8_t id, void *storage)
{
    switch(id)
    {
        case FX_REVERB: return new(storage) Reverb();
        case FX_DELAY: return new(storage) Delay();
        case FX_CRUSH: return new(storage) Crush();
        case FX_CHORUS: return new(storage) Chorus();
        case FX_RESON: return new(storage) Reson();
        default: return nullptr;
    }
}

} // namespace fx
