#pragma once
/**
 * Bump allocator over a fixed memory region (an SDRAM slot arena).
 *
 * Effects take their big buffers (delay lines, reverb tanks) from the
 * arena they're given in Init(). There is no free(): the owner calls
 * Reset() after destroying the effect, which releases everything at once.
 * Memory is NOT zeroed (SDRAM .bss isn't cleared at boot); effects clear
 * their own buffers in Init()/Reset().
 */
#include <cstddef>
#include <cstdint>
#include <new>

namespace fx
{
class Arena
{
  public:
    Arena() = default;
    Arena(void *base, size_t size) { Init(base, size); }

    void Init(void *base, size_t size)
    {
        base_ = static_cast<uint8_t *>(base);
        size_ = size;
        used_ = 0;
    }

    /** `bytes` aligned to `align` (power of two; default = cache line, so
     *  a buffer never shares a line with another), or nullptr if full. */
    void *Alloc(size_t bytes, size_t align = 32)
    {
        uintptr_t start = reinterpret_cast<uintptr_t>(base_) + used_;
        uintptr_t a     = (start + (align - 1)) & ~(uintptr_t)(align - 1);
        size_t    off   = (size_t)(a - reinterpret_cast<uintptr_t>(base_));
        if(base_ == nullptr || off > size_ || bytes > size_ - off)
            return nullptr;
        used_ = off + bytes;
        return reinterpret_cast<void *>(a);
    }

    /** Placement-constructs a T in the arena (nullptr if it doesn't fit). */
    template <typename T, typename... Args>
    T *New(Args &&...args)
    {
        void *p = Alloc(sizeof(T), alignof(T) > 32 ? alignof(T) : 32);
        return p ? new(p) T(static_cast<Args &&>(args)...) : nullptr;
    }

    void   Reset() { used_ = 0; }
    size_t Used() const { return used_; }
    size_t Size() const { return size_; }

  private:
    uint8_t *base_ = nullptr;
    size_t   size_ = 0;
    size_t   used_ = 0;
};

} // namespace fx
