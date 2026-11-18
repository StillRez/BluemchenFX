#pragma once
/**
 * Bluemchen hardware abstraction (clean rewrite for current libDaisy).
 *
 * Pin map and init sequence based on recursinging/kxmx_bluemchen
 * (src/kxmx_bluemchen.cpp, MIT License, Copyright (c) 2020 recursinging):
 *   Encoder A / B / Click : D27 / D26 / D28
 *   Pot 1 / Pot 2         : D16 (A1) / D15 (A0)
 *   CV 1  / CV 2          : D21 (A6) / D18 (A3)
 *   OLED                  : SSD1306 64x32 on I2C1 (libDaisy default transport)
 *   MIDI in               : UART (libDaisy MidiUartHandler defaults)
 *   Audio                 : Daisy Seed codec, 2 in / 2 out
 */
#include "daisy_seed.h"
#include "dev/oled_ssd130x.h"

namespace bluemchen
{
class Hardware
{
  public:
    enum Ctrl
    {
        KNOB_1 = 0,
        KNOB_2,
        CV_1,
        CV_2,
        CTRL_LAST
    };

    using Display = daisy::OledDisplay<daisy::SSD130xI2c64x32Driver>;

    static constexpr int kDisplayWidth  = 64;
    static constexpr int kDisplayHeight = 32;

    /** Initialise seed, controls, display, MIDI and DAC.
     *  Call SetAudioBlockSize / SetSampleRate *after* Init but before
     *  StartAudio; controls are re-initialised for the new callback rate. */
    void Init(size_t block_size = 32,
              daisy::SaiHandle::Config::SampleRate sr
              = daisy::SaiHandle::Config::SampleRate::SAI_48KHZ);

    void StartAudio(daisy::AudioHandle::AudioCallback cb);
    void StartAdc() { seed.adc.Start(); }

    /** Call once per audio callback. Debounces encoder, filters ADCs. */
    void ProcessAllControls();

    /** Knobs: 0..1. CV: -1..+1 (bipolar, 0 = 0 V). */
    float Knob(Ctrl c) const { return controls_[c].Value(); }
    float CvBipolar(Ctrl c) const;

    float SampleRate() { return seed.AudioSampleRate(); }
    float CallbackRate() { return seed.AudioCallbackRate(); }

    daisy::DaisySeed       seed;
    daisy::Encoder         encoder;
    daisy::MidiUartHandler midi;
    Display                display;

  private:
    void InitControls();
    void InitDisplay();
    void InitMidi();
    void InitDac();

    daisy::AnalogControl controls_[CTRL_LAST];
};

} // namespace bluemchen
