#pragma once
/**
 * OLED + encoder user interface for a 64x32 display.
 *
 * Pages (turn the encoder to move between them):
 *   HOME          overview: bar for every parameter + I/O meters
 *   <param> x N   one page per effect parameter
 *   K1 / K2       route knob 1 / knob 2 to a parameter (absolute control)
 *   CV1 / CV2     route CV 1 / CV 2 to a parameter (bipolar offset)
 *   INPUT         STEREO, or MONO (left input copied to right)
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
    void Init(bluemchen::Hardware::Display *display,
              fx::Effect                   *effect,
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
    /** Pending SD card request, if any (latest request wins). */
    bool TakeSdRequest(SdAction &action, int &slot);
    /** Result of a PEEK for the slot display. `used` = file exists.
     *  `detail` is shown on the bottom line when the slot isn't used
     *  (e.g. an SD error code). */
    void SetSdSlotInfo(int slot, bool used, const char *text, const char *detail);
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
        PAGE_PARAM,
        PAGE_ROUTE,
        PAGE_INPUT,
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
    void   DrawSd(bool load, bool editing);
    bool   IsSdPage(int p) const;
    void   RequestSd(SdAction a);
    void   DrawSave(bool editing);
    void   DrawHeader(const char *title, bool inverted, const char *tag);
    void   DrawMeters(float in, float out);
    void   FormatValue(size_t idx, float norm, char *buf, size_t len) const;
    const char *SourceTag(size_t param) const;

    bluemchen::Hardware::Display *disp_     = nullptr;
    fx::Effect                   *fx_       = nullptr;
    app::Settings                *settings_ = nullptr;

    int      page_      = 0;
    bool     editing_   = false;
    uint32_t last_turn_ = 0;

    int      peek_param_ = -1;
    uint32_t peek_until_ = 0;

    const char *flash_msg_   = nullptr;
    uint32_t    flash_until_ = 0;

    bool save_request_  = false;
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
