/**
 * BluemchenFX — effect firmware for the kxmx Bluemchen (Daisy Seed).
 *
 * Audio runs through an effect chain of up to 4 slots (app::Chain), built
 * and edited from the panel (docs/design/effect-chaining.md, phase 2). SD
 * presets and MIDI CCs still cover slot 1 only until phase 3.
 *
 * Controls (menu details in ui/ui.h)
 *   HOME              the chain, one row per slot, then SETUP. Turn selects,
 *                     click enters, hold stores settings to flash
 *   Slot / SETUP      turn = page, click = edit value, hold = back to HOME
 *   Knob 1 / 2        absolute control of the routed parameter (any slot);
 *                     takes over once the knob is moved
 *   CV 1 / 2          bipolar offset (+-5 V = +-full range) on the routed
 *                     parameter
 *   MIDI CC 20..27    parameter 1..8 of slot 1's effect (any channel)
 *   LOAD SD / SAVE SD presets as text files on the microSD card
 *                     (0:/BLUEMCHN/PRESET01.TXT .. PRESET99.TXT), slot 1
 *   MIDI Program Chg  0..98 loads SD preset 1..99
 */
#include "daisy_seed.h"
#include "per/qspi.h"
#include "util/PersistentStorage.h"
#include "util/CpuLoadMeter.h"

#include "hw/bluemchen.h"
#include "fx/registry.h"
#include "ui/ui.h"
#include "app/settings.h"
#include "app/chain.h"
#include "app/chain_settings.h"
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
static constexpr uint32_t kCpuPeakSettle = 2000; // ms: ignore start-up blocks in the peak

// ------------------------------------------------------- SDRAM arenas
// Each chain slot's effect takes its big buffers from its own 8 MB arena
// (≈ 21 s of stereo float audio). SDRAM .bss is NOLOAD: not zeroed at
// boot, effects clear what they allocate. A 24 MB shared pool for one
// looper/long sampler at a time is reserved in the design (phase 4).
static constexpr size_t kSlotArenaBytes = 8u * 1024u * 1024u;
static uint8_t DSY_SDRAM_BSS arena_mem[app::kNumSlots][kSlotArenaBytes]
    __attribute__((aligned(32)));
static fx::Arena arenas[app::kNumSlots];

// -------------------------------------------------------------- state
static Hardware hw;
static ui::Ui   gui;
static app::Chain chain;
static app::Recall recall; // per slot + type: last parameters (RAM only)

static PersistentStorage<app::Settings> storage(hw.seed.qspi);
static app::Settings                   *settings = nullptr;
static app::PresetStore                 presets;

// Written in audio ISR, read in main loop
static volatile int32_t  enc_total   = 0; // running sum, ISR-only writer
static volatile uint32_t click_count = 0;
static volatile uint32_t long_count  = 0;
static bool              long_fired  = false;
static float             effective[app::kNumSlots][app::kMaxParams];
static float             meter_in = 0.f, meter_out = 0.f;

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

// Whole-callback CPU load: average (smoothed) and peak since last reset.
// Written by the audio ISR, read and reset by the main loop.
static CpuLoadMeter cpu_meter;

static void AudioCallback(AudioHandle::InputBuffer  in,
                          AudioHandle::OutputBuffer out,
                          size_t                    size)
{
    cpu_meter.OnBlockStart();
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

    // Effective parameters: stored base values + CV offsets on routed params
    for(size_t k = 0; k < app::kNumSlots; k++)
        for(size_t i = 0; i < app::kMaxParams; i++)
            effective[k][i] = settings->slot[k].norm[i];
    for(int s = app::SRC_CV_1; s <= app::SRC_CV_2; s++)
    {
        const app::Route rt = settings->route[s];
        if(rt.None() || rt.slot >= (int)app::kNumSlots || rt.param < 0
           || rt.param >= (int)app::kMaxParams)
            continue;
        float cv = hw.CvBipolar(s == app::SRC_CV_1 ? Hardware::CV_1 : Hardware::CV_2);
        if(fabsf(cv) < kCvDeadband)
            cv = 0.f;
        float &v = effective[rt.slot][rt.param];
        v        = v + cv;
        v        = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
    }

    uint8_t types[app::kNumSlots];
    for(size_t k = 0; k < app::kNumSlots; k++)
        types[k] = settings->slot[k].type;
    chain.Process(in_l, in_r, out[0], out[1], size, effective, types);

    const float gain = settings->out_gain;
    if(gain != 1.f)
        for(size_t i = 0; i < size; i++)
        {
            out[0][i] *= gain;
            out[1][i] *= gain;
        }

    meter_in  = PeakFollow(meter_in, in[0], size);
    meter_out = PeakFollow(meter_out, out[0], size);
    cpu_meter.OnBlockEnd();
}

// ----------------------------------------------------------- settings
/** Earlier firmware stored v1 (<= 0.3.0) or v2 (0.4-0.5) settings in the
 *  same flash sector. If that's what's there, convert it so the user keeps
 *  their setup (the active effect lands in slot 1). Run before
 *  storage.Init(). */
static bool ReadLegacySettings(app::Settings &out)
{
    using State = PersistentStorage<app::Settings>::State;
    static_assert(sizeof(State) == 4, "PersistentStorage state word changed");
    const uint8_t *raw = static_cast<const uint8_t *>(hw.seed.qspi.GetData(kFlashOffset));
    State          state;
    uint32_t       version;
    std::memcpy(&state, raw, sizeof(state));
    std::memcpy(&version, raw + sizeof(State), sizeof(version));
    if(state != State::FACTORY && state != State::USER)
        return false;
    if(version == app::kSettingsVersionV2)
    {
        app::SettingsV2 v2;
        std::memcpy(&v2, raw + sizeof(State), sizeof(v2));
        return app::MigrateV2(v2, out, &recall);
    }
    if(version == app::kSettingsVersionV1)
    {
        app::SettingsV1 v1;
        std::memcpy(&v1, raw + sizeof(State), sizeof(v1));
        return app::MigrateV1(v1, out);
    }
    return false;
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

static void ResetAllKnobs()
{
    ResetKnobTakeover(0);
    ResetKnobTakeover(1);
}

static void UpdateKnobs(uint32_t now)
{
    for(int k = 0; k < 2; k++)
    {
        const app::Route rt = settings->route[k];
        float      v  = hw.Knob(k == 0 ? Hardware::KNOB_1 : Hardware::KNOB_2);
        KnobState &ks = knob_state[k];
        if(rt.None())
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
            float &dst = settings->slot[rt.slot].norm[rt.param];
            if(fabsf(v - dst) > 0.004f)
            {
                if(fabsf(v - dst) > 0.01f)
                    gui.Peek(rt.slot, rt.param, now);
                dst = v;
            }
        }
    }
}

/** Makes the chain engine follow the settings: effect types (click-free
 *  swaps, done over several calls) and bypass. Called every loop. */
static void SyncChain(uint32_t now)
{
    for(size_t k = 0; k < app::kNumSlots; k++)
    {
        const app::SlotState &sl = settings->slot[k];
        if(sl.type != chain.Target(k))
            chain.SetType(k, sl.type, sl.norm);
        chain.SetBypass(k, sl.Bypassed(), sl.Hard());
    }
    chain.Poll();
    for(size_t k = 0; k < app::kNumSlots; k++)
        if(chain.TakeInitFailure(k))
            gui.Flash("NO MEM", now); // effect didn't fit its arena
}

/** Replace the live settings (e.g. from a preset). The chain follows in
 *  SyncChain(); knobs must be moved again before they override values. */
static void ApplySettings(const app::Settings &next)
{
    *settings = next;
    ResetAllKnobs();
}

/** Puts `type` (an effect or FX_NONE) into `slot`, as asked for on its TYPE
 *  page. The chain swaps click-free in SyncChain(). */
static void SetSlotType(int slot, uint8_t type, uint32_t now)
{
    app::Settings next = *settings;
    if(!app::SetSlotType(next, recall, slot, type))
        return;
    ApplySettings(next);
    const fx::Effect *e = fx::Prototype(type);
    gui.Flash(e ? e->Name() : "EMPTY", now);
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
    // Single-effect presets: loaded into slot 1, slots 2-4 emptied
    uint8_t          type  = settings->slot[0].type < fx::FX_COUNT ? settings->slot[0].type
                                                                   : (uint8_t)fx::FX_REVERB;
    app::EffectState state = app::SlotAsEffectState(*settings, 0);
    uint8_t          mode  = settings->input_mode;
    SdStatus         st    = presets.Load(slot, type, state, mode);
    if(st == SdStatus::OK)
    {
        app::Settings next = *settings;
        app::ApplySingleEffectPreset(next, recall, type, state, mode);
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
            SdStatus st = presets.Save(slot,
                                       settings->slot[0].type,
                                       app::SlotAsEffectState(*settings, 0),
                                       settings->input_mode);
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
        const fx::Effect  *e   = fx::Prototype(settings->slot[0].type);
        if(!e || idx < 0 || idx >= (int)e->NumParams())
            continue;
        settings->slot[0].norm[idx] = cc.value / 127.f;
        gui.Peek(0, idx, now);
        // a knob routed here must be moved again before it takes over
        for(int k = 0; k < 2; k++)
            if(settings->route[k].Is(0, idx))
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
    // the first effect in the chain
    const fx::Effect *e = nullptr;
    for(size_t k = 0; k < app::kNumSlots && !e; k++)
        e = fx::Prototype(settings->slot[k].type);
    d.WriteString(e ? e->Name() : "--", Font_7x10, true);
    d.Update();
    System::Delay(800);
}

int main(void)
{
    hw.Init(kBlockSize);
    Diag("1 HW OK");
    for(size_t k = 0; k < app::kNumSlots; k++)
        arenas[k].Init(arena_mem[k], kSlotArenaBytes);
    chain.Init(hw.SampleRate(), arenas);
    recall.Clear();
    Diag("2 CHAIN OK");

    // Load settings from QSPI: current layout, else migrate v2/v1, else
    // defaults.
    app::Settings migrated;
    const bool    have_old = ReadLegacySettings(migrated);
    Diag(have_old ? "3a OLD FOUND" : "3a NO OLD");
    storage.Init(app::MakeDefaults(), kFlashOffset);
    Diag(app::Valid(storage.GetSettings()) ? "3b SET OK" : "3b SET NEW");
    if(!app::Valid(storage.GetSettings()))
    {
        if(have_old)
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

    // Build the chain's effects from the stored settings (audio isn't
    // running yet, so every slot swaps immediately)
    SyncChain(0);
    for(size_t k = 0; k < app::kNumSlots; k++)
        for(size_t i = 0; i < app::kMaxParams; i++)
            effective[k][i] = settings->slot[k].norm[i];

    Diag("4 FX RESET");
    gui.Init(&hw.display, settings);
    Splash();
    presets.Init(fx::Prototypes(), fx::FX_COUNT); // card is mounted on first use
    Diag("5 SD INIT");

    hw.StartAdc();
    cpu_meter.Init(hw.SampleRate(), (int)kBlockSize);
    hw.StartAudio(AudioCallback);
    Diag("6 AUDIO ON");
    System::Delay(20); // let the ADC filters settle
    ResetAllKnobs();

    uint32_t last_draw   = 0;
    bool     cpu_settled = false;
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
        int edit_slot, edit_param;
        if(gui.TakeEditedParam(edit_slot, edit_param))
            for(int k = 0; k < 2; k++)
                if(settings->route[k].Is(edit_slot, edit_param))
                    ResetKnobTakeover(k);
        if(gui.TakeRouteChanged())
            ResetAllKnobs();
        int     type_slot;
        uint8_t type;
        if(gui.TakeTypeRequest(type_slot, type))
            SetSlotType(type_slot, type, now);

        // ---- knobs, MIDI, SD
        UpdateKnobs(now);
        HandleMidi(now);
        HandleSd(now);

        // ---- chain: effect swaps (click-free), bypass
        SyncChain(now);

        // ---- persistence (QSPI erase/write blocks for a few ms; audio
        //      keeps running from internal flash)
        if(gui.TakeSaveRequest())
            storage.Save();

        // ---- display
        // ---- CPU meter: drop start-up blocks from the peak once, and
        //      reset the peak when asked from the CPU page
        if((!cpu_settled && now >= kCpuPeakSettle) || gui.TakeCpuReset())
        {
            cpu_settled = true;
            cpu_meter.Reset();
        }
        gui.SetCpuLoad(cpu_meter.GetAvgCpuLoad(), cpu_meter.GetMaxCpuLoad());

        if(now - last_draw >= kDisplayPeriod)
        {
            last_draw = now;
            gui.Draw(effective, sqrtf(meter_in), sqrtf(meter_out), now);
        }
    }
}
