#pragma once
/**
 * The list of effects a chain slot can hold.
 *
 * Two kinds of objects per effect type:
 * - a *prototype*: one static, never-initialised instance used only for
 *   metadata (name, parameter table, default routing). UI pages, presets
 *   and settings validation read these.
 * - *instances*: one per chain slot that holds this type, constructed in
 *   that slot's storage block with Construct() and initialised with the
 *   slot's SDRAM arena.
 *
 * Adding an effect: append its id to EffectId (never reorder: ids are
 * persisted in flash), then add it to the tables in registry.cpp and to
 * tests/test_effects.cpp.
 */
#include <cstddef>
#include <cstdint>
#include "fx/effect.h"

namespace fx
{
enum EffectId : uint8_t
{
    FX_REVERB = 0,
    FX_DELAY  = 1,
    FX_COUNT,
    FX_NONE = 0xFF, // empty slot
};

/** Bytes (and alignment) of the per-slot storage that holds one effect
 *  object. Big buffers are NOT in here: they come from the arena.
 *  Checked against every effect class in registry.cpp. */
static constexpr size_t kEffectStorageBytes = 512;
static constexpr size_t kEffectStorageAlign = 16;

/** Metadata instance for `id` (nullptr for FX_NONE / unknown ids). */
const Effect *Prototype(uint8_t id);

/** All prototypes, indexed by EffectId (for UI lists and preset lookup). */
const Effect *const *Prototypes();

/** Constructs an *uninitialised* instance of `id` in `storage`
 *  (kEffectStorageBytes, kEffectStorageAlign). Call Init() next; destroy
 *  with Destroy(). Returns nullptr for FX_NONE / unknown ids. */
Effect *Construct(uint8_t id, void *storage);

/** Runs the destructor (the storage itself is the caller's). */
inline void Destroy(Effect *e)
{
    if(e)
        e->~Effect();
}

} // namespace fx
