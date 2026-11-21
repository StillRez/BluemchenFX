#pragma once
/**
 * OLED + encoder user interface for a 64x32 display: a three-level menu
 * for the effect chain (phase 2 of docs/design/effect-chaining.md).
 *
 * HOME          the chain: one row per slot ("1 DELAY"; bypassed slots in
 *               lowercase, empty slots "--"), then SETUP. Turn selects,
 *               click enters, long press saves to flash. CPU bar on the
 *               right, next to the I/O meters.
 * Slot pages    TYPE -> BYPASS -> BYP MODE -> the effect's parameters. An
 *               empty slot only has TYPE. Entering a slot opens its first
 *               parameter (TYPE if empty), afterwards the page last used.
 *               Replacing an effect asks for a confirmation click.
 * SETUP pages   K1 / K2 / CV1 / CV2 routing (to any slot's parameter),
 *               INPUT, OUT level, LIMIT (output limiter ceiling, shows the
 *               gain reduction), LIM REL (its release), CPU, LOAD SD,
 *               SAVE SD, SAVE.
 *
 * On slot and setup pages: turn = next/previous page, click = edit
 * (header inverts; turn changes the value, click again leaves edit mode),
 * long press = back to HOME (also cancels an edit).
 *
 * The UI draws through libDaisy's OneBitGraphicsDisplay interface, so the
 * host tests can run it against an in-memory framebuffer.
 */
#include <cstddef>
#include <cstdint>
#include "hid/disp/display.h"
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
    /** Output level range of the OUT page (dB, 0.5 dB steps). */
    static constexpr float kOutMinDb = -24.f;
    static constexpr float kOutMaxDb = 6.f;

    void Init(daisy::OneBitGraphicsDisplay *display, app::Settings *settings);

    // --- input events (call from main loop) ---------------------------
    void OnTurn(int32_t inc, uint32_t now_ms);
    void OnClick(uint32_t now_ms);
    void OnLongPress(uint32_t now_ms);

    /** Briefly show a parameter page (e.g. when a knob or MIDI CC moves it). */
    void Peek(int slot, size_t param, uint32_t now_ms);
    /** Show a short full-screen message. */
    void Flash(const char *msg, uint32_t now_ms);

    // --- outputs -------------------------------------------------------
    /** True once after the user asked to save. */
    bool TakeSaveRequest();
    /** The parameter the encoder just edited, if any. */
    bool TakeEditedParam(int &slot, int &param);
    /** True once after a routing change (so knob takeover can reset). */
    bool TakeRouteChanged();
    /** A type change the user confirmed on a slot's TYPE page: `type` is an
     *  fx::EffectId or fx::FX_NONE (empty the slot). The caller performs it
     *  (recall, routes, audio swap). */
    bool TakeTypeRequest(int &slot, uint8_t &type);
    /** Pending SD card request, if any (latest request wins). */
    bool TakeSdRequest(SdAction &action, int &slot);
    /** Result of a PEEK for the slot display. `used` = file exists.
     *  Used slot: `text` = preset name, `detail` = what it holds
     *  ("REVERB", "DEL>REV", "D>R>R>R"; empty if not known).
     *  Otherwise: `text` = status, `detail` = error code (bottom line). */
    void SetSdSlotInfo(int slot, bool used, const char *text, const char *detail);
    /** CPU load of the audio callback, 0..1 (average, peak since reset). */
    void SetCpuLoad(float avg, float peak)
    {
        cpu_avg_  = avg;
        cpu_peak_ = peak;
    }
    /** Output limiter gain reduction in dB (<= 0), for the LIMIT page. */
    void SetLimiterGr(float db) { limiter_gr_ = db; }
    /** True once after the user asked to reset the CPU peak. */
    bool TakeCpuReset();
    /** Immediately draw a full-screen message (for blocking operations). */
    void ShowBusy(const char *msg);

    /** `effective` = every slot's live parameter values (base + CV). */
    void Draw(const float (*effective)[app::kMaxParams],
              float    meter_in,
              float    meter_out,
              uint32_t now_ms);

    // --- state, for tests and the main loop -----------------------------
    enum Level
    {
        LVL_HOME,
        LVL_SLOT,
        LVL_SETUP,
    };
    enum PageKind
    {
        // slot pages
        PAGE_TYPE,
        PAGE_BYPASS,
        PAGE_BYPMODE,
        PAGE_PARAM,
        // setup pages
        PAGE_ROUTE,
        PAGE_INPUT,
        PAGE_OUT,
        PAGE_LIMIT,
        PAGE_RELEASE,
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

    Level Lvl() const { return level_; }
    /** HOME: selected row (0-3 = slots, 4 = SETUP). */
    int  HomeSel() const { return sel_; }
    /** The slot whose pages are open (LVL_SLOT). */
    int  Slot() const { return slot_; }
    Page CurrentPage() const { return PageAt(page_); }
    bool Editing() const { return editing_; }
    /** TYPE page: waiting for the click that confirms replacing an effect. */
    bool Confirming() const { return confirm_; }

    static constexpr int kSetupRow = (int)app::kNumSlots; // HOME row index

  private:
    Page PageAt(int p) const;
    int  NumPages() const;
    void ClampPage();
    void EnterHome();
    void TurnHome(int dir);
    void TurnValue(int32_t inc, uint32_t now_ms);
    void ClickPage(uint32_t now_ms);

    void DrawHome();
    void DrawParam(int slot, size_t idx, const float *eff, bool editing);
    void DrawType(bool editing);
    void DrawBypass(bool editing);
    void DrawBypMode(bool editing);
    void DrawRoute(size_t src, bool editing);
    void DrawInput(bool editing);
    void DrawOut(bool editing);
    void DrawLimit(bool editing);
    void DrawRelease(bool editing);
    /** Value line in the big font if it fits, else the medium one. */
    void DrawValue(const char *text);
    void DrawCpu();
    void DrawSd(bool load, bool editing);
    void DrawSave();
    void DrawHeader(const char *title, bool inverted, const char *tag, int slot = -1);
    void DrawHint(const char *hint);
    /** 4x6 text at x, y, always in capitals. */
    void DrawSmall(int x, int y, const char *text);
    void DrawMeters(float in, float out);
    void DrawMessage(const char *msg);

    /** Effect in `slot`, or nullptr if empty. */
    const fx::Effect     *FxIn(int slot) const { return fx::Prototype(settings_->slot[slot].type); }
    app::SlotState       &Sl() { return settings_->slot[slot_]; }
    const app::SlotState &Sl() const { return settings_->slot[slot_]; }
    bool                  IsSdPage(int p) const;
    void                  FormatValue(const fx::Effect &e, size_t idx, float norm, char *buf, size_t len) const;
    const char           *SourceTag(int slot, size_t param) const;
    /** Route destinations in list order: none, then every parameter of
     *  every filled slot. Returns the position of `rt` (0 = none). */
    int  RoutePos(const app::Route &rt) const;
    int  NumRouteDests() const;
    app::Route RouteAt(int pos) const;

    daisy::OneBitGraphicsDisplay *disp_     = nullptr;
    app::Settings                *settings_ = nullptr;

    Level level_   = LVL_HOME;
    int   sel_     = 0; // HOME selection
    int   top_     = 0; // HOME first visible row
    int   slot_    = 0; // open slot (LVL_SLOT)
    int   page_    = 0; // page within the level
    bool  editing_ = false;
    int   slot_page_[app::kNumSlots] = {-1, -1, -1, -1}; // last page per slot
    int   setup_page_ = 0;

    uint8_t pending_type_ = 0;     // choice while editing TYPE
    bool    confirm_      = false; // TYPE: waiting for the confirming click
    int     type_req_slot_ = -1;
    uint8_t type_req_      = 0;

    uint32_t last_turn_ = 0;

    int      peek_slot_  = -1;
    int      peek_param_ = -1;
    uint32_t peek_until_ = 0;

    const char *flash_msg_   = nullptr;
    uint32_t    flash_until_ = 0;

    bool  save_request_  = false;
    bool  cpu_reset_     = false;
    float cpu_avg_       = 0.f;
    float cpu_peak_      = 0.f;
    float limiter_gr_    = 0.f;
    int   edited_slot_   = -1;
    int   edited_param_  = -1;
    bool  route_changed_ = false;

    int      sd_slot_       = 1;
    SdAction sd_request_    = SdAction::NONE;
    int      sd_info_slot_  = -1; // slot the info below belongs to
    bool     sd_used_       = false;
    char     sd_text_[17]   = {0};
    char     sd_detail_[17] = {0};
};

} // namespace ui
