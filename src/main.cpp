/**
 * BluemchenFX — effect firmware for the kxmx Bluemchen (Daisy Seed).
 *
 * Effects: REVERB, DELAY (selected on the EFFECT page). Each effect keeps
 * its own parameter values and knob/CV routing.
 *
 * Controls
 *   Encoder turn      browse pages / change value while editing
 *   Encoder click     enter/leave edit mode (SAVE page: store settings)
 *   Encoder hold      store settings to flash
 *   Knob 1 / 2        absolute control of the routed parameter; takes over
 *                     once the knob is moved
 *   CV 1 / 2          bipolar offset (+-5 V = +-full range) on the routed
 *                     parameter
 *   MIDI CC 20..27    parameter 1..8 of the active effect (any channel)
 *   INPUT page        STEREO, or MONO (left input feeds both channels)
 *   LOAD SD / SAVE SD presets as text files on the microSD card
 *                     (0:/BLUEMCHN/PRESET01.TXT .. PRESET99.TXT)
 *   MIDI Program Chg  0..98 loads SD preset 1..99 (switches effect if needed)
 */
#include "daisy_seed.h"
#include "per/qspi.h"
#include "util/PersistentStorage.h"

#include "hw/bluemchen.h"
#include "fx/reverb.h"
#include "fx/delay.h"
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

// ------------------------------------------------------------ effects
// The index is persisted (Settings::effect) and written to nothing else:
// presets use the effect *name*. Only ever append to this list.
enum EffectId : uint8_t
{
    FX_REVERB = 0,
    FX_DELAY  = 1,
    FX_COUNT
};

static fx::Reverb               reverb;
static fx::Delay                delay_fx;
static fx::Effect *const        effects[FX_COUNT] = {&reverb, &delay_fx};
static const fx::Effect *const *effects_const     = effects;
static_assert(FX_COUNT <= app::kMaxEffects, "raise kMaxEffects (layout change)");

// -------------------------------------------------------------- state
static Hardware hw;
static ui::Ui   gui;

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

// Set by the main loop while it swaps effects; the audio callback then
// passes dry audio and touches no effect.
static volatile bool switching = false;

// `switching` is volatile but the settings copy isn't: stop the compiler
// moving plain memory accesses across the flag (single core, so this is
// all the ordering the audio interrupt needs).
static inline void CompilerBarrier()
{
    __asm volatile("" ::: "memory");
}

// Knob takeover state (main loop only)
struct KnobState
{
    float anchor  = 0.f;
    bool  engaged = false;
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

    // Mono: duplicate the left input to the right before any processing
    const float *in_l = in[0];
    const float *in_r = settings->input_mode == app::INPUT_MONO ? in[0] : in[1];

    if(switching)
    {
        for(size_t i = 0; i < size; i++)
        {
            out[0][i] = in_l[i];
            out[1][i] = in_r[i];
        }
    }
    else
    {
        fx::Effect             &effect = *effects[settings->effect];
        const app::EffectState &st     = settings->Active();

        // CV modulation on top of stored base values
        float mod[app::kMaxParams] = {};
        for(int s = app::SRC_CV_1; s <= app::SRC_CV_2; s++)
        {
            int8_t r = st.route[s];
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
            float v      = st.norm[i] + mod[i];
            v            = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
            effective[i] = v;
            effect.SetParam(i, v);
        }

        effect.Process(in_l, in_r, out[0], out[1], size);
    }

    meter_in  = PeakFollow(meter_in, in[0], size);
    meter_out = PeakFollow(meter_out, out[0], size);
}

// ----------------------------------------------------------- settings
static void SetDefaultRouting(app::EffectState &st, int k1, int k2, int cv1, int cv2)
{
    st.route[app::SRC_KNOB_1] = (int8_t)k1;
    st.route[app::SRC_KNOB_2] = (int8_t)k2;
    st.route[app::SRC_CV_1]   = (int8_t)cv1;
    st.route[app::SRC_CV_2]   = (int8_t)cv2;
}

static app::Settings MakeDefaults()
{
    app::Settings s;
    std::memset(&s, 0, sizeof(s));
    s.version    = app::kSettingsVersion;
    s.effect     = FX_REVERB;
    s.input_mode = app::INPUT_STEREO;
    for(size_t e = 0; e < FX_COUNT; e++)
    {
        app::EffectState &st = s.fx[e];
        for(size_t i = 0; i < app::kMaxParams; i++)
            st.norm[i] = i < effects[e]->NumParams() ? effects[e]->Spec(i).def : 0.f;
        for(int r = 0; r < app::SRC_LAST; r++)
            st.route[r] = app::kRouteNone;
    }
    SetDefaultRouting(s.fx[FX_REVERB],
                      fx::Reverb::DECAY,
                      fx::Reverb::MIX,
                      fx::Reverb::DECAY,
                      fx::Reverb::HI_CUT);
    SetDefaultRouting(s.fx[FX_DELAY],
                      fx::Delay::TIME,
                      fx::Delay::MIX,
                      fx::Delay::TIME,
                      fx::Delay::FEEDBACK);
    return s;
}

static bool NormsValid(const float *norm)
{
    for(size_t i = 0; i < app::kMaxParams; i++)
        if(!(norm[i] >= 0.f && norm[i] <= 1.f)) // also catches NaN
            return false;
    return true;
}

static bool RoutesValid(const int8_t *route, size_t num_params)
{
    for(int i = 0; i < app::SRC_LAST; i++)
        if(route[i] < app::kRouteNone || route[i] >= (int)num_params)
            return false;
    return true;
}

static bool SettingsValid(const app::Settings &s)
{
    if(s.version != app::kSettingsVersion || s.effect >= FX_COUNT
       || s.input_mode >= app::INPUT_MODE_LAST)
        return false;
    for(size_t e = 0; e < FX_COUNT; e++)
        if(!NormsValid(s.fx[e].norm)
           || !RoutesValid(s.fx[e].route, effects[e]->NumParams()))
            return false;
    return true;
}

/** Firmware up to v0.3.0 stored a single-effect (reverb) SettingsV1 in the
 *  same flash sector. If that's what's there, convert it so the user keeps
 *  their saved reverb settings. Must run before storage.Init(). */
static bool ReadLegacySettings(app::Settings &out)
{
    using State = PersistentStorage<app::Settings>::State;
    static_assert(sizeof(State) == 4, "PersistentStorage state word changed");
    struct LegacySave // == PersistentStorage<SettingsV1>::SaveStruct
    {
        State           state;
        app::SettingsV1 s;
    };
    const auto *old
        = reinterpret_cast<const LegacySave *>(hw.seed.qspi.GetData(kFlashOffset));
    if(old->state != State::FACTORY && old->state != State::USER)
        return false;
    const app::SettingsV1 &v1 = old->s;
    if(v1.version != app::kSettingsVersionV1 || !NormsValid(v1.norm)
       || !RoutesValid(v1.route, reverb.NumParams())
       || v1.input_mode >= app::INPUT_MODE_LAST)
        return false;

    out            = MakeDefaults();
    out.effect     = FX_REVERB;
    out.input_mode = v1.input_mode;
    std::memcpy(out.fx[FX_REVERB].norm, v1.norm, sizeof(v1.norm));
    std::memcpy(out.fx[FX_REVERB].route, v1.route, sizeof(v1.route));
    return true;
}

// ---------------------------------------------------- boot diagnostics
#ifdef BOOT_DIAG
/** Show a startup step on the OLED (and blink once) so a hang can be
 *  located on hardware without a debugger. Build with `make DIAG=1`. */
static void Diag(const char *step)
{
    auto &d = hw.display;
    d.Fill(false);
    d.SetCursor(0, 0);
    d.WriteString("BOOT DIAG", Font_6x8, true);
    d.SetCursor(0, 14);
    d.WriteString(step, Font_7x10, true);
    d.Update();
    hw.seed.SetLed(true);
    System::Delay(250);
    hw.seed.SetLed(false);
    System::Delay(350);
}
#else
static inline void Diag(const char *) {}
#endif

// ---------------------------------------------------------- main loop
static void ResetKnobTakeover(int k)
{
    knob_state[k].engaged = false;
    knob_state[k].anchor
        = hw.Knob(k == 0 ? Hardware::KNOB_1 : Hardware::KNOB_2);
}

static void UpdateKnobs(uint32_t now)
{
    app::EffectState &st = settings->Active();
    for(int k = 0; k < 2; k++)
    {
        int8_t     r  = st.route[k];
        float      v  = hw.Knob(k == 0 ? Hardware::KNOB_1 : Hardware::KNOB_2);
        KnobState &ks = knob_state[k];
        if(r == app::kRouteNone)
        {
            ks.anchor = v;
            continue;
        }
        // Only take over once the knob is actually moved, so stored /
        // encoder-set values are not overwritten at power-up.
        if(!ks.engaged && fabsf(v - ks.anchor) > kKnobEngage)
            ks.engaged = true;
        if(ks.engaged)
        {
            float &dst = st.norm[r];
            if(fabsf(v - dst) > 0.004f)
            {
                if(fabsf(v - dst) > 0.01f)
                    gui.Peek(r, now);
                dst = v;
            }
        }
    }
}

/** Replace the live settings, switching effect if `next` uses another one.
 *  The new effect is reset (no stale tails/echoes) while audio runs dry. */
static void ApplySettings(const app::Settings &next)
{
    if(next.effect != settings->effect && next.effect < FX_COUNT)
    {
        fx::Effect &incoming = *effects[next.effect];
        switching            = true;
        CompilerBarrier();
        System::Delay(2); // > 1 audio block: the callback now passes dry audio
        for(size_t i = 0; i < incoming.NumParams(); i++)
            incoming.SetParam(i, next.fx[next.effect].norm[i]);
        incoming.Reset(); // clears buffers, snaps smoothing to the new values
        *settings = next;
        for(size_t i = 0; i < app::kMaxParams; i++)
            effective[i] = settings->Active().norm[i];
        CompilerBarrier(); // all of the above must land before audio resumes
        switching = false;
    }
    else
    {
        *settings = next;
    }
    // knobs must be moved again before they override the new values
    ResetKnobTakeover(0);
    ResetKnobTakeover(1);
}

static void SwitchEffect(int idx, uint32_t now)
{
    if(idx < 0 || idx >= FX_COUNT || idx == settings->effect)
        return;
    app::Settings next = *settings;
    next.effect        = (uint8_t)idx;
    ApplySettings(next);
    gui.Flash(effects[idx]->Name(), now);
}

// ------------------------------------------------------- SD presets
using SdStatus = app::PresetStore::Status;

static void RefreshSlotInfo(int slot)
{
    char     name[app::kPresetNameLen];
    char     fx_name[app::kEffectNameLen];
    SdStatus st = presets.Peek(slot, name, sizeof(name), fx_name, sizeof(fx_name));
    if(st == SdStatus::OK)
        gui.SetSdSlotInfo(slot, true, name[0] ? name : "(no name)", fx_name);
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
    app::Settings next = *settings;
    SdStatus      st   = presets.Load(slot, next);
    if(st == SdStatus::OK)
    {
        ApplySettings(next);
        static char msg[10];
        snprintf(msg, sizeof(msg), "LOAD %02d", slot);
        gui.Flash(msg, now);
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
            SdStatus st = presets.Save(slot, *settings);
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
        ControlChangeEvent cc  = ev.AsControlChange();
        int                idx = (int)cc.control_number - kMidiCcBase;
        if(idx < 0 || idx >= (int)effects[settings->effect]->NumParams())
            continue;
        app::EffectState &st = settings->Active();
        st.norm[idx]         = cc.value / 127.f;
        gui.Peek(idx, now);
        // a knob routed here must be moved again before it takes over
        for(int k = 0; k < 2; k++)
            if(st.route[k] == idx)
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
    d.WriteString(effects[settings->effect]->Name(), Font_7x10, true);
    d.Update();
    System::Delay(800);
}

int main(void)
{
    hw.Init(kBlockSize);
    Diag("1 HW OK");
    for(auto *e : effects)
        e->Init(hw.SampleRate()); // also clears its SDRAM buffers
    Diag("2 FX OK");

    // Load settings from QSPI: current layout, else migrate the v1
    // (single-effect) layout, else defaults.
    app::Settings migrated;
    const bool    have_v1 = ReadLegacySettings(migrated);
    Diag(have_v1 ? "3a V1 FOUND" : "3a NO V1");
    storage.Init(MakeDefaults(), kFlashOffset);
    Diag(SettingsValid(storage.GetSettings()) ? "3b SET OK" : "3b SET NEW");
    if(!SettingsValid(storage.GetSettings()))
    {
        if(have_v1)
        {
            storage.GetSettings() = migrated;
            storage.Save();
        }
        else
        {
            storage.RestoreDefaults();
        }
    }
    settings = &storage.GetSettings();
    Diag("3c SETTINGS");

    // Start the active effect from its stored values (no glide at boot)
    {
        fx::Effect &e = *effects[settings->effect];
        for(size_t i = 0; i < e.NumParams(); i++)
            e.SetParam(i, settings->Active().norm[i]);
        e.Reset();
    }
    for(size_t i = 0; i < app::kMaxParams; i++)
        effective[i] = settings->Active().norm[i];

    Diag("4 FX RESET");
    gui.Init(&hw.display, effects_const, FX_COUNT, settings);
    Splash();
    presets.Init(effects_const, FX_COUNT); // card is mounted on first use
    Diag("5 SD INIT");

    hw.StartAdc();
    hw.StartAudio(AudioCallback);
    Diag("6 AUDIO ON");
    System::Delay(20); // let the ADC filters settle
    ResetKnobTakeover(0);
    ResetKnobTakeover(1);

    uint32_t last_draw   = 0;
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
                if(settings->Active().route[k] == edited)
                    ResetKnobTakeover(k);
        if(gui.TakeRouteChanged())
        {
            ResetKnobTakeover(0);
            ResetKnobTakeover(1);
        }
        SwitchEffect(gui.TakeEffectRequest(), now);

        // ---- knobs, MIDI, SD
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
