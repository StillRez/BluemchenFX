#pragma once
/**
 * OLED + encoder user interface for a 64x32 display.
 *
 * Effect chain, phase 1: the pages edit chain slot 1 only (slots 2-4 stay
 * empty until the chain UI in phase 2).
 *
 * Pages (turn the encoder to move between them):
 *   HOME          overview: bar for every parameter + I/O meters
 *   EFFECT        choose the slot's effect (REVERB, DELAY, ...)
 *   BYPASS        OFF / ON (crossfaded, no clicks)
 *   BYP MODE      TRAILS (tails ring out) / HARD (cut, saves CPU)
 *   <param> x N   one page per parameter of the slot's effect
 *   K1 / K2       route knob 1 / knob 2 to a parameter (absolute control)
 *   CV1 / CV2     route CV 1 / CV 2 to a parameter (bipolar offset)
 *   INPUT         STEREO, or MONO (left input copied to right)
 *   CPU           audio callback load: average and peak (click resets peak)
 *   LOAD SD       pick a preset slot (1..99) on the SD card and load it
 *   SAVE SD       pick a slot and write the current setup to it
 *   SAVE          store settings to internal flash (power-up state)
 *
 * Click: enter / leave edit mode (header inverts while editing).
 * Long press (anywhere): save settings to flash.
 */
#include <cstddef>
#include <cstdint>
#include "hw/bluemchen.h"
#include "fx/effect.h"
#include "app/settings.h"
#include "fx/registry.h"

namespace ui
{
enum class SdAction
{
    NONE,
    PEEK, // read slot name / status for display
    LOAD,
    SAVE,
};

class Ui
{
  public:
    /** `effects` is the firmware's effect list; the active one and its
     *  parameters/routing are always read via settings->effect. */
    void Init(bluemchen::Hardware::Display *display,
              const fx::Effect *const      *effects,
              size_t                        num_effects,
              app::Settings                *settings);

    // --- input events (call from main loop) ---------------------------
    void OnTurn(int32_t inc, uint32_t now_ms);
    void OnClick(uint32_t now_ms);
    void OnLongPress(uint32_t now_ms);

    /** Briefly show a parameter page (e.g. when a knob or MIDI CC moves it). */
    void Peek(size_t param, uint32_t now_ms);
    /** Show a short full-screen message. */
    void Flash(const char *msg, uint32_t now_ms);

    // --- outputs -------------------------------------------------------
    /** True once after the user asked to save. */
    bool TakeSaveRequest();
    /** Param index whose value the encoder just edited, or -1. */
    int TakeEditedParam();
    /** True once after a routing change (so knob takeover can reset). */
    bool TakeRouteChanged();
    /** Effect index the user confirmed on the EFFECT page, or -1. The
     *  caller performs the switch on EditSlot() (recall, routes, audio). */
    int TakeEffectRequest();
    /** The chain slot the pages edit. */
    int EditSlot() const { return slot_; }
    /** Pending SD card request, if any (latest request wins). */
    bool TakeSdRequest(SdAction &action, int &slot);
    /** Result of a PEEK for the slot display. `used` = file exists.
     *  Used slot: `text` = preset name, `detail` = its effect name.
     *  Otherwise: `text` = status, `detail` = error code (bottom line). */
    void SetSdSlotInfo(int slot, bool used, const char *text, const char *detail);
    /** CPU load of the audio callback, 0..1 (average, peak since reset). */
    void SetCpuLoad(float avg, float peak)
    {
        cpu_avg_  = avg;
        cpu_peak_ = peak;
    }
    /** True once after the user asked to reset the CPU peak. */
    bool TakeCpuReset();
    /** Immediately draw a full-screen message (for blocking operations). */
    void ShowBusy(const char *msg);

    void Draw(const float *effective_norm,
              float        meter_in,
              float        meter_out,
              uint32_t     now_ms);

  private:
    enum PageKind
    {
        PAGE_HOME,
        PAGE_EFFECT,
        PAGE_BYPASS,
        PAGE_BYPMODE,
        PAGE_PARAM,
        PAGE_ROUTE,
        PAGE_INPUT,
        PAGE_CPU,
        PAGE_SD_LOAD,
        PAGE_SD_SAVE,
        PAGE_SAVE,
    };
    struct Page
    {
        PageKind kind;
        int      index; // param index or route source
    };

    Page   PageAt(int p) const;
    int    NumPages() const;
    void   DrawHome(const float *eff);
    void   DrawParam(size_t idx, const float *eff, bool editing);
    void   DrawRoute(size_t src, bool editing);
    void   DrawInput(bool editing);
    void   DrawCpu();
    void   DrawEffect(bool editing);
    void   DrawBypass(bool editing);
    void   DrawBypMode(bool editing);
    /** The edited slot's effect. Slot 1 always holds one in phase 1; an
     *  empty slot falls back to the first effect's metadata. */
    const fx::Effect &Fx() const
    {
        const fx::Effect *p = fx::Prototype(Sl().type);
        return p ? *p : *effects_[0];
    }
    app::SlotState       &Sl() { return settings_->slot[slot_]; }
    const app::SlotState &Sl() const { return settings_->slot[slot_]; }
    void   DrawSd(bool load, bool editing);
    bool   IsSdPage(int p) const;
    void   RequestSd(SdAction a);
    void   DrawSave(bool editing);
    void   DrawHeader(const char *title, bool inverted, const char *tag);
    void   DrawMeters(float in, float out);
    void   FormatValue(size_t idx, float norm, char *buf, size_t len) const;
    const char *SourceTag(size_t param) const;

    bluemchen::Hardware::Display *disp_     = nullptr;
    const fx::Effect *const      *effects_     = nullptr;
    size_t                        num_effects_ = 0;
    app::Settings                *settings_    = nullptr;

    int slot_           = 0;  // chain slot being edited
    int pending_effect_ = 0;  // choice while editing the EFFECT page
    int effect_request_ = -1; // confirmed choice, for the main loop

    int      page_      = 0;
    bool     editing_   = false;
    uint32_t last_turn_ = 0;

    int      peek_param_ = -1;
    uint32_t peek_until_ = 0;

    const char *flash_msg_   = nullptr;
    uint32_t    flash_until_ = 0;

    bool save_request_  = false;
    bool cpu_reset_     = false;
    float cpu_avg_      = 0.f;
    float cpu_peak_     = 0.f;
    int  edited_param_  = -1;
    bool route_changed_ = false;

    int      sd_slot_      = 1;
    SdAction sd_request_   = SdAction::NONE;
    int      sd_info_slot_ = -1; // slot the info below belongs to
    bool     sd_used_      = false;
    char     sd_text_[17]  = {0};
    char     sd_detail_[17] = {0};
};

} // namespace ui
