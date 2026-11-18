#include "hw/bluemchen.h"

using namespace daisy;
using namespace daisy::seed;

namespace bluemchen
{
// Pin assignments (see header for source)
static constexpr Pin kPinEncA     = D27;
static constexpr Pin kPinEncB     = D26;
static constexpr Pin kPinEncClick = D28;
static constexpr Pin kPinKnob1    = D16;
static constexpr Pin kPinKnob2    = D15;
static constexpr Pin kPinCv1      = D21;
static constexpr Pin kPinCv2      = D18;

void Hardware::Init(size_t block_size, SaiHandle::Config::SampleRate sr)
{
    seed.Configure();
    seed.Init(true); // boost to 480 MHz
    seed.SetAudioBlockSize(block_size);
    seed.SetAudioSampleRate(sr);

    encoder.Init(kPinEncA, kPinEncB, kPinEncClick);
    InitControls();
    InitDisplay();
    InitMidi();
    InitDac();
}

void Hardware::StartAudio(AudioHandle::AudioCallback cb)
{
    seed.StartAudio(cb);
}

void Hardware::ProcessAllControls()
{
    for(auto &c : controls_)
        c.Process();
    encoder.Debounce();
}

float Hardware::CvBipolar(Ctrl c) const
{
    // CV inputs are initialised with flip=true (inverting input stage),
    // giving 0..1 with ~0.5 at 0 V.
    return (controls_[c].Value() - 0.5f) * 2.f;
}

void Hardware::InitControls()
{
    AdcChannelConfig cfg[CTRL_LAST];
    cfg[KNOB_1].InitSingle(kPinKnob1);
    cfg[KNOB_2].InitSingle(kPinKnob2);
    cfg[CV_1].InitSingle(kPinCv1);
    cfg[CV_2].InitSingle(kPinCv2);
    seed.adc.Init(cfg, CTRL_LAST);

    const float rate = seed.AudioCallbackRate();
    controls_[KNOB_1].Init(seed.adc.GetPtr(KNOB_1), rate, false);
    controls_[KNOB_2].Init(seed.adc.GetPtr(KNOB_2), rate, false);
    controls_[CV_1].Init(seed.adc.GetPtr(CV_1), rate, true);
    controls_[CV_2].Init(seed.adc.GetPtr(CV_2), rate, true);
}

void Hardware::InitDisplay()
{
    Display::Config cfg;
    cfg.driver_config.transport_config.Defaults();
    display.Init(cfg);
    display.Fill(false);
    display.Update();
}

void Hardware::InitMidi()
{
    MidiUartHandler::Config cfg;
    midi.Init(cfg);
    midi.StartReceive();
}

void Hardware::InitDac()
{
    DacHandle::Config cfg;
    cfg.bitdepth   = DacHandle::BitDepth::BITS_12;
    cfg.buff_state = DacHandle::BufferState::ENABLED;
    cfg.mode       = DacHandle::Mode::POLLING;
    cfg.chn        = DacHandle::Channel::BOTH;
    seed.dac.Init(cfg);
    seed.dac.WriteValue(DacHandle::Channel::BOTH, 0);
}

} // namespace bluemchen
