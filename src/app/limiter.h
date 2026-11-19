#pragma once
/**
 * Always-on output limiter (after OUT, at the end of the chain).
 *
 * Stacked effects can push sustained, tonal material well past full scale
 * (three reverbs in series: about +15 dB on a held note, DEVLOG
 * 2026-11-18), which clipped harshly at the codec. This keeps the output
 * under a ceiling instead:
 *
 * 1. Gain computer: a stereo-linked peak envelope (instant rise,
 *    exponential fall over the RELEASE time) sets a gain that brings the
 *    envelope down to the threshold, 1 dB below the ceiling. The gain moves
 *    towards a lower value over ~0.5 ms (attack), so it doesn't jump within
 *    a waveform cycle.
 * 2. Soft clip: whatever the gain stage lets through above the threshold
 *    (transient onsets during the attack) is bent smoothly towards the
 *    ceiling, so the output never goes past it.
 *
 * Below the threshold the signal passes unchanged (gain exactly 1, clip
 * stage the identity). No look-ahead, so no latency. Hardware-independent:
 * host-tested in tests/test_limiter.cpp.
 */
#include <cstddef>

namespace app
{
class Limiter
{
  public:
    static constexpr float kKneeDb = 1.f; // threshold below the ceiling

    void Init(float sample_rate);

    /** Output ceiling in dBFS (the UI allows -12..0). */
    void SetCeilingDb(float db);
    /** Release time in seconds (time constant of the envelope's fall). */
    void SetRelease(float seconds);

    /** In place, both channels with the same gain (keeps the stereo image). */
    void Process(float *l, float *r, size_t n);

    /** Current gain reduction in dB (0 = not limiting, negative while
     *  limiting). Readable from the main loop. */
    float GainReductionDb() const;

    void Reset();

  private:
    float sr_      = 48000.f;
    float ceil_    = 1.f;  // linear ceiling
    float thr_     = 1.f;  // linear threshold (ceiling - knee)
    float rel_     = 1.f;  // per-sample envelope fall factor
    float att_     = 0.f;  // per-sample gain attack coefficient
    float env_     = 0.f;  // peak envelope
    float gain_    = 1.f;  // applied gain
    volatile float gr_gain_ = 1.f; // lowest gain in the last block (for display)
};

} // namespace app
