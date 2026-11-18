/**
 * BluemchenFX — effect firmware for the kxmx Bluemchen (Daisy Seed).
 *
 * Current effect: stereo reverb (decay, mix, lo cut, hi cut).
 *
 * Controls
 *   Encoder turn      browse pages / change value while editing
 *   Encoder click     enter/leave edit mode (SAVE page: store settings)
 *   Encoder hold      store settings to flash
 *   Knob 1 / 2        absolute control of the routed parameter (default
 *                     DECAY / MIX); takes over once the knob is moved
 *   CV 1 / 2          bipolar offset (+-5 V = +-full range) on the routed
 *                     parameter (default DECAY / HI CUT)
 *   MIDI CC 20..27    parameter 1..8 (any channel)
 *   INPUT page        STEREO, or MONO (left input feeds both channels)
 *   LOAD SD / SAVE SD presets as text files on the microSD card
 *                     (0:/BLUEMCHN/PRESET01.TXT .. PRESET99.TXT)
 *   MIDI Program Chg  0..98 loads SD preset 1..99
 */
#include "daisy_seed.h"
#include "per/qspi.h"
#include "util/PersistentStorage.h"

#include "hw/bluemchen.h"
#include "fx/reverb.h"
#include "ui/ui.h"
#include "app/settings.h"
#include "app/preset_store.h"

#include <cmath>
#include <cstdio>

using namespace daisy;
using bluemchen::Hardware;

// ------------------------------------------------------------- config
static constexpr size_t   kBlockSize     = 32;
static constexpr uint32_t kFlashOffset   = 0x7FF000; // last 4 KB sector of QSPI
static constexpr float    kLongPressMs   = 700.f;
static constexpr float    kCvDeadband    = 0.02f;
static constexpr float    kKnobEngage    = 0.03f;
static constexpr uint8_t  kMidiCcBase    = 20;
static constexpr uint32_t kDisplayPeriod = 33; // ms (~30 fps)

// -------------------------------------------------------------- state
static Hardware   hw;
static fx::Reverb reverb;
static fx::Effect &effect = reverb;
static ui::Ui      gui;

static PersistentStorage<app::Settings> storage(hw.seed.qspi);
static app::Settings                   *settings = nullptr;
static app::PresetStore                 presets;

// Written in audio ISR, read in main loop
static volatile int32_t  enc_total   = 0; // running sum, ISR-only writer
static volatile uint32_t click_count = 0;
static volatile uint32_t long_count  = 0;
static bool              long_fired  = false;
static float             effective[app::kMaxParams];
static float             meter_in = 0.f, meter_out = 0.f;

// Knob takeover state (main loop only)
struct KnobState
{
    float anchor   = 0.f;
    bool  engaged  = false;
};
static KnobState knob_state[2];

// --------------------------------------------------------------- audio
static inline float PeakFollow(float env, const float *buf, size_t n)
{
    float pk = 0.f;
    for(size_t i = 0; i < n; i++)
    {
        float a = fabsf(buf[i]);
        pk      = a > pk ? a : pk;
    }
    return pk > env ? pk : env * 0.97f;
}

static void AudioCallback(AudioHandle::InputBuffer  in,
                          AudioHandle::OutputBuffer out,
                          size_t                    size)
{
    hw.ProcessAllControls();

    // Encoder events must be sampled at the debounce rate
    enc_total = enc_total + hw.encoder.Increment();
    if(hw.encoder.RisingEdge())
        long_fired = false;
    if(!long_fired && hw.encoder.Pressed()
       && hw.encoder.TimeHeldMs() >= kLongPressMs)
    {
        long_fired = true;
        long_count = long_count + 1;
    }
    if(hw.encoder.FallingEdge() && !long_fired)
        click_count = click_count + 1;

    // CV modulation on top of stored base values
    float mod[app::kMaxParams] = {};
    for(int s = app::SRC_CV_1; s <= app::SRC_CV_2; s++)
    {
        int8_t r = settings->route[s];
        if(r == app::kRouteNone)
            continue;
        float cv = hw.CvBipolar(s == app::SRC_CV_1 ? Hardware::CV_1
                                                   : Hardware::CV_2);
        if(fabsf(cv) < kCvDeadband)
            cv = 0.f;
        mod[r] += cv;
    }

    const size_t n = effect.NumParams();
    for(size_t i = 0; i < n; i++)
    {
        float v      = settings->norm[i] + mod[i];
        v            = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
        effective[i] = v;
        effect.SetParam(i, v);
    }

    // Mono: duplicate the left input to the right before any processing
    const float *in_l = in[0];
    const float *in_r = settings->input_mode == app::INPUT_MONO ? in[0] : in[1];
    effect.Process(in_l, in_r, out[0], out[1], size);

    meter_in  = PeakFollow(meter_in, in[0], size);
    meter_out = PeakFollow(meter_out, out[0], size);
}

// ----------------------------------------------------------- settings
static app::Settings MakeDefaults()
{
    app::Settings s;
    std::memset(&s, 0, sizeof(s));
    s.version = app::kSettingsVersion;
    for(size_t i = 0; i < app::kMaxParams; i++)
        s.norm[i] = i < effect.NumParams() ? effect.Spec(i).def : 0.f;
    s.route[app::SRC_KNOB_1] = fx::Reverb::DECAY;
    s.route[app::SRC_KNOB_2] = fx::Reverb::MIX;
    s.route[app::SRC_CV_1]   = fx::Reverb::DECAY;
    s.route[app::SRC_CV_2]   = fx::Reverb::HI_CUT;
    s.input_mode             = app::INPUT_STEREO;
    return s;
}

static bool SettingsValid(const app::Settings &s)
{
    if(s.version != app::kSettingsVersion)
        return false;
    for(size_t i = 0; i < app::kMaxParams; i++)
        if(!(s.norm[i] >= 0.f && s.norm[i] <= 1.f)) // also catches NaN
            return false;
    if(s.input_mode >= app::INPUT_MODE_LAST)
        return false;
    for(int i = 0; i < app::SRC_LAST; i++)
        if(s.route[i] < app::kRouteNone || s.route[i] >= (int)effect.NumParams())
            return false;
    return true;
}

// ---------------------------------------------------------- main loop
static void ResetKnobTakeover(int k)
{
    knob_state[k].engaged = false;
    knob_state[k].anchor
        = hw.Knob(k == 0 ? Hardware::KNOB_1 : Hardware::KNOB_2);
}

static void UpdateKnobs(uint32_t now)
{
    for(int k = 0; k < 2; k++)
    {
        int8_t r = settings->route[k];
        float  v = hw.Knob(k == 0 ? Hardware::KNOB_1 : Hardware::KNOB_2);
        KnobState &st = knob_state[k];
        if(r == app::kRouteNone)
        {
            st.anchor = v;
            continue;
        }
        // Only take over once the knob is actually moved, so stored /
        // encoder-set values are not overwritten at power-up.
        if(!st.engaged && fabsf(v - st.anchor) > kKnobEngage)
            st.engaged = true;
        if(st.engaged)
        {
            float &dst = settings->norm[r];
            if(fabsf(v - dst) > 0.004f)
            {
                if(fabsf(v - dst) > 0.01f)
                    gui.Peek(r, now);
                dst = v;
            }
        }
    }
}

static void ResetKnobTakeover(int k);

// ------------------------------------------------------- SD presets
using SdStatus = app::PresetStore::Status;

static void RefreshSlotInfo(int slot)
{
    char     name[app::kPresetNameLen];
    SdStatus st = presets.Peek(slot, effect, name, sizeof(name));
    if(st == SdStatus::OK)
        gui.SetSdSlotInfo(slot, true, name[0] ? name : "(no name)", nullptr);
    else
        gui.SetSdSlotInfo(
            slot, false, app::PresetStore::StatusText(st), presets.LastError());
}

/** Mounting a card can take a moment; say so instead of looking frozen. */
static void SdBusyHint()
{
    if(!presets.Mounted())
        gui.ShowBusy("SD...");
}

static void LoadPreset(int slot, uint32_t now)
{
    SdBusyHint();
    SdStatus st = presets.Load(slot, effect, *settings);
    if(st == SdStatus::OK)
    {
        static char msg[10];
        snprintf(msg, sizeof(msg), "LOAD %02d", slot);
        gui.Flash(msg, now);
        // knobs must be moved again before they override loaded values
        ResetKnobTakeover(0);
        ResetKnobTakeover(1);
    }
    else
    {
        gui.Flash(app::PresetStore::StatusText(st), now);
    }
}

static void HandleSd(uint32_t now)
{
    ui::SdAction action;
    int          slot;
    if(!gui.TakeSdRequest(action, slot))
        return;
    SdBusyHint();
    switch(action)
    {
        case ui::SdAction::PEEK: break;
        case ui::SdAction::LOAD: LoadPreset(slot, now); break;
        case ui::SdAction::SAVE:
        {
            SdStatus st = presets.Save(slot, effect, *settings);
            if(st == SdStatus::OK)
            {
                static char msg[10];
                snprintf(msg, sizeof(msg), "SAVE %02d", slot);
                gui.Flash(msg, now);
            }
            else
            {
                gui.Flash(app::PresetStore::StatusText(st), now);
            }
            break;
        }
        default: break;
    }
    RefreshSlotInfo(slot);
}

static void HandleMidi(uint32_t now)
{
    hw.midi.Listen();
    while(hw.midi.HasEvents())
    {
        MidiEvent ev = hw.midi.PopEvent();
        if(ev.type == ProgramChange)
        {
            int slot = ev.AsProgramChange().program + 1;
            if(slot <= app::kPresetSlotCount)
                LoadPreset(slot, now);
            continue;
        }
        if(ev.type != ControlChange)
            continue;
        ControlChangeEvent cc = ev.AsControlChange();
        int idx = (int)cc.control_number - kMidiCcBase;
        if(idx < 0 || idx >= (int)effect.NumParams())
            continue;
        settings->norm[idx] = cc.value / 127.f;
        gui.Peek(idx, now);
        // a knob routed here must be moved again before it takes over
        for(int k = 0; k < 2; k++)
            if(settings->route[k] == idx)
                ResetKnobTakeover(k);
    }
}

static void Splash()
{
    auto &d = hw.display;
    d.Fill(false);
    d.SetCursor(2, 4);
    d.WriteString("BLUEMCHEN", Font_6x8, true);
    d.SetCursor(2, 18);
    d.WriteString(effect.Name(), Font_7x10, true);
    d.Update();
    System::Delay(800);
}

int main(void)
{
    hw.Init(kBlockSize);
    effect.Init(hw.SampleRate());

    // Load settings from QSPI (falls back to defaults if blank / old layout)
    storage.Init(MakeDefaults(), kFlashOffset);
    if(!SettingsValid(storage.GetSettings()))
        storage.RestoreDefaults();
    settings = &storage.GetSettings();

    for(size_t i = 0; i < app::kMaxParams; i++)
        effective[i] = settings->norm[i];

    gui.Init(&hw.display, &effect, settings);
    Splash();
    presets.Init(); // driver setup only; the card is mounted on first use

    hw.StartAdc();
    hw.StartAudio(AudioCallback);
    System::Delay(20); // let the ADC filters settle
    ResetKnobTakeover(0);
    ResetKnobTakeover(1);

    uint32_t last_draw = 0;
    uint32_t clicks_seen = 0, longs_seen = 0;
    int32_t  enc_seen    = 0;

    for(;;)
    {
        const uint32_t now = System::GetNow();

        // ---- encoder
        int32_t total = enc_total;
        gui.OnTurn(total - enc_seen, now);
        enc_seen = total;

        uint32_t c = click_count;
        for(; clicks_seen != c; clicks_seen++)
            gui.OnClick(now);
        uint32_t l = long_count;
        for(; longs_seen != l; longs_seen++)
            gui.OnLongPress(now);

        // encoder edits take priority over a knob routed to the same param
        int edited = gui.TakeEditedParam();
        if(edited >= 0)
            for(int k = 0; k < 2; k++)
                if(settings->route[k] == edited)
                    ResetKnobTakeover(k);
        if(gui.TakeRouteChanged())
        {
            ResetKnobTakeover(0);
            ResetKnobTakeover(1);
        }

        // ---- knobs and MIDI
        UpdateKnobs(now);
        HandleMidi(now);
        HandleSd(now);

        // ---- persistence (QSPI erase/write blocks for a few ms; audio
        //      keeps running from internal flash)
        if(gui.TakeSaveRequest())
            storage.Save();

        // ---- display
        if(now - last_draw >= kDisplayPeriod)
        {
            last_draw = now;
            gui.Draw(effective, sqrtf(meter_in), sqrtf(meter_out), now);
        }
    }
}
