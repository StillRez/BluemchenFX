#pragma once
/**
 * Generic effect interface. Each effect describes its own parameters
 * (name, range, curve, display format); the UI, knob/CV routing, MIDI CC
 * mapping and flash storage are all driven from these descriptions, so a
 * new effect only has to implement this class.
 */
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace fx
{
enum class Curve : uint8_t
{
    Linear,
    Exponential, // min * (max/min)^n   (min must be > 0)
};

enum class Unit : uint8_t
{
    Seconds,
    Percent,
    Hertz,
};

struct ParamSpec
{
    const char *name;    // <= 7 chars to fit the 64 px header
    float       min;     // real-world minimum
    float       max;     // real-world maximum
    float       def;     // default, normalised 0..1
    Curve       curve;
    Unit        unit;

    float ToReal(float n) const
    {
        n = n < 0.f ? 0.f : (n > 1.f ? 1.f : n);
        if(curve == Curve::Exponential)
            return min * powf(max / min, n);
        return min + (max - min) * n;
    }

    /** Inverse of ToReal; out-of-range values are clamped. */
    float ToNorm(float v) const
    {
        float n;
        if(curve == Curve::Exponential)
            n = v <= min ? 0.f : logf(v / min) / logf(max / min);
        else
            n = (v - min) / (max - min);
        return n < 0.f ? 0.f : (n > 1.f ? 1.f : n);
    }
};

class Effect
{
  public:
    virtual ~Effect() {}
    virtual const char *Name() const = 0;
    virtual void        Init(float sample_rate) = 0;

    virtual size_t           NumParams() const       = 0;
    virtual const ParamSpec &Spec(size_t idx) const = 0;

    /** Set parameter in normalised units (0..1). Called once per block
     *  from the audio callback; the effect handles its own smoothing. */
    virtual void SetParam(size_t idx, float norm) = 0;

    /** Non-interleaved stereo block processing. */
    virtual void Process(const float *in_l,
                         const float *in_r,
                         float       *out_l,
                         float       *out_r,
                         size_t       size)
        = 0;
};

} // namespace fx
