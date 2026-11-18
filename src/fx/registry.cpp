#include "fx/registry.h"
#include "fx/reverb.h"
#include "fx/delay.h"
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

static Reverb              proto_reverb;
static Delay               proto_delay;
static const Effect *const protos[FX_COUNT] = {&proto_reverb, &proto_delay};

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
        default: return nullptr;
    }
}

} // namespace fx
