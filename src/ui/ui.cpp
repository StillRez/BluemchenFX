#include "ui/ui.h"
#include <cstdio>
#include <cstring>

using namespace daisy;

namespace ui
{
static constexpr int      kContentW     = 60; // x 0..59, meters at 61 & 63
static constexpr uint32_t kPeekMs       = 1200;
static constexpr float    kStepFine     = 0.005f;
static constexpr float    kStepNormal   = 0.01f;
static constexpr float    kStepFast     = 0.04f;

static constexpr int      kFirstParamPage = 4; // after HOME, EFFECT, BYPASS, BYP MODE

static const char *const kSourceTitle[app::SRC_LAST]
    = {"KNOB 1", "KNOB 2", "CV 1", "CV 2"};
static const char *const kSourceTag[app::SRC_LAST] = {"K1", "K2", "C1", "C2"};

static inline float Clamp01(float x)
{
    return x < 0.f ? 0.f : (x > 1.f ? 1.f : x);
}

void Ui::Init(bluemchen::Hardware::Display *display,
              const fx::Effect *const      *effects,
              size_t                        num_effects,
              app::Settings                *settings)
{
    disp_        = display;
    effects_     = effects;
    num_effects_ = num_effects;
    settings_    = settings;
    page_     = 0;
    editing_  = false;
}

// ---------------------------------------------------------------- pages
int Ui::NumPages() const
{
    // HOME + EFFECT + BYPASS + BYP MODE + params + routes
    //   + INPUT + CPU + LOAD SD + SAVE SD + SAVE
    return kFirstParamPage + (int)Fx().NumParams() + app::SRC_LAST + 5;
}

Ui::Page Ui::PageAt(int p) const
{
    const int n = (int)Fx().NumParams();
    if(p == 0)
        return {PAGE_HOME, 0};
    if(p == 1)
        return {PAGE_EFFECT, 0};
    if(p == 2)
        return {PAGE_BYPASS, 0};
    if(p == 3)
        return {PAGE_BYPMODE, 0};
    p -= kFirstParamPage - 1; // first param page -> 1
    if(p <= n)
        return {PAGE_PARAM, p - 1};
    if(p <= n + app::SRC_LAST)
        return {PAGE_ROUTE, p - 1 - n};
    const int k = p - (n + app::SRC_LAST);
    if(k == 1)
        return {PAGE_INPUT, 0};
    if(k == 2)
        return {PAGE_CPU, 0};
    if(k == 3)
        return {PAGE_SD_LOAD, 0};
    if(k == 4)
        return {PAGE_SD_SAVE, 0};
    return {PAGE_SAVE, 0};
}

bool Ui::IsSdPage(int p) const
{
    PageKind k = PageAt(p).kind;
    return k == PAGE_SD_LOAD || k == PAGE_SD_SAVE;
}

void Ui::RequestSd(SdAction a)
{
    sd_request_ = a;
}

// --------------------------------------------------------------- events
void Ui::OnTurn(int32_t inc, uint32_t now_ms)
{
    if(inc == 0)
        return;

    // Turning the encoder cancels a peek and returns to the current page.
    peek_until_ = 0;
    if(page_ >= NumPages()) // effect changed underneath us
        page_ = NumPages() - 1;

    if(!editing_)
    {
        page_ += inc > 0 ? 1 : -1;
        if(page_ < 0)
            page_ = 0;
        if(page_ >= NumPages())
            page_ = NumPages() - 1;
        if(IsSdPage(page_))
            RequestSd(SdAction::PEEK); // refresh slot info (card may have changed)
        return;
    }

    Page pg = PageAt(page_);
    if(pg.kind == PAGE_PARAM)
    {
        // Acceleration: fast turning = bigger steps
        uint32_t dt   = now_ms - last_turn_;
        float    step = dt < 25 ? kStepFast : (dt < 80 ? kStepNormal : kStepFine);
        last_turn_    = now_ms;

        float &v = Sl().norm[pg.index];
        v        = Clamp01(v + step * (float)inc);
        edited_param_ = pg.index;
    }
    else if(pg.kind == PAGE_ROUTE)
    {
        // cycle: none, param 0 .. param N-1 of this slot
        app::Route &rt = settings_->route[pg.index];
        int r = (rt.slot == slot_ ? rt.param : app::kRouteNone) + (inc > 0 ? 1 : -1);
        int n = (int)Fx().NumParams();
        if(r < app::kRouteNone)
            r = n - 1;
        if(r >= n)
            r = app::kRouteNone;
        rt = r == app::kRouteNone ? app::Route{app::kRouteNone, app::kRouteNone}
                                  : app::Route{(int8_t)slot_, (int8_t)r};
        route_changed_             = true;
    }
    else if(pg.kind == PAGE_EFFECT)
    {
        int e = pending_effect_ + (inc > 0 ? 1 : -1);
        if(e < 0)
            e = (int)num_effects_ - 1;
        if(e >= (int)num_effects_)
            e = 0;
        pending_effect_ = e;
    }
    else if(pg.kind == PAGE_BYPASS)
    {
        Sl().flags ^= app::SLOT_BYPASS;
    }
    else if(pg.kind == PAGE_BYPMODE)
    {
        Sl().flags ^= app::SLOT_HARD;
    }
    else if(pg.kind == PAGE_INPUT)
    {
        int m = settings_->input_mode + (inc > 0 ? 1 : -1);
        if(m < 0)
            m = app::INPUT_MODE_LAST - 1;
        if(m >= app::INPUT_MODE_LAST)
            m = 0;
        settings_->input_mode = (uint8_t)m;
    }
    else if(pg.kind == PAGE_SD_LOAD || pg.kind == PAGE_SD_SAVE)
    {
        sd_slot_ += inc > 0 ? 1 : -1;
        if(sd_slot_ < 1)
            sd_slot_ = app::kPresetSlotCount;
        if(sd_slot_ > app::kPresetSlotCount)
            sd_slot_ = 1;
        RequestSd(SdAction::PEEK);
    }
}

void Ui::OnClick(uint32_t now_ms)
{
    peek_until_ = 0;
    Page pg     = PageAt(page_);
    switch(pg.kind)
    {
        case PAGE_HOME: page_ = kFirstParamPage; break; // jump to first parameter
        case PAGE_EFFECT:
            // 1st click: choose. 2nd click: switch (if changed).
            if(!editing_)
                pending_effect_ = Sl().type < num_effects_ ? Sl().type : 0;
            else if(pending_effect_ != Sl().type)
                effect_request_ = pending_effect_;
            editing_ = !editing_;
            break;
        case PAGE_CPU: cpu_reset_ = true; break; // clear the peak
        case PAGE_SAVE:
            save_request_ = true;
            Flash("SAVED", now_ms);
            break;
        case PAGE_SD_LOAD:
        case PAGE_SD_SAVE:
            // 1st click: choose slot. 2nd click: do it.
            if(editing_)
                RequestSd(pg.kind == PAGE_SD_LOAD ? SdAction::LOAD
                                                  : SdAction::SAVE);
            else
                RequestSd(SdAction::PEEK);
            editing_ = !editing_;
            break;
        default: editing_ = !editing_; break;
    }
}

void Ui::OnLongPress(uint32_t now_ms)
{
    editing_      = false;
    save_request_ = true;
    Flash("SAVED", now_ms);
}

void Ui::Peek(size_t param, uint32_t now_ms)
{
    // Don't hijack the screen while the user is on that very page
    Page pg = PageAt(page_);
    if(pg.kind == PAGE_PARAM && (size_t)pg.index == param)
        return;
    peek_param_ = (int)param;
    peek_until_ = now_ms + kPeekMs;
}

void Ui::Flash(const char *msg, uint32_t now_ms)
{
    flash_msg_   = msg;
    flash_until_ = now_ms + 900;
}

bool Ui::TakeSaveRequest()
{
    bool r        = save_request_;
    save_request_ = false;
    return r;
}

int Ui::TakeEditedParam()
{
    int r         = edited_param_;
    edited_param_ = -1;
    return r;
}

bool Ui::TakeSdRequest(SdAction &action, int &slot)
{
    if(sd_request_ == SdAction::NONE)
        return false;
    action      = sd_request_;
    slot        = sd_slot_;
    sd_request_ = SdAction::NONE;
    return true;
}

void Ui::SetSdSlotInfo(int slot, bool used, const char *text, const char *detail)
{
    sd_info_slot_ = slot;
    sd_used_      = used;
    strncpy(sd_text_, text ? text : "", sizeof(sd_text_) - 1);
    sd_text_[sizeof(sd_text_) - 1] = 0;
    strncpy(sd_detail_, detail ? detail : "", sizeof(sd_detail_) - 1);
    sd_detail_[sizeof(sd_detail_) - 1] = 0;
}

void Ui::ShowBusy(const char *msg)
{
    const FontDef &f = Font_7x10;
    int            w = (int)strlen(msg) * f.FontWidth;
    disp_->Fill(false);
    disp_->DrawRect(0, 0, 63, 31, true, false);
    disp_->SetCursor((64 - w) / 2, (32 - f.FontHeight) / 2);
    disp_->WriteString(msg, f, true);
    disp_->Update();
}

int Ui::TakeEffectRequest()
{
    int r           = effect_request_;
    effect_request_ = -1;
    return r;
}

bool Ui::TakeCpuReset()
{
    bool r     = cpu_reset_;
    cpu_reset_ = false;
    return r;
}

bool Ui::TakeRouteChanged()
{
    bool r         = route_changed_;
    route_changed_ = false;
    return r;
}

// -------------------------------------------------------------- drawing
void Ui::Draw(const float *eff, float meter_in, float meter_out, uint32_t now_ms)
{
    disp_->Fill(false);

    if(flash_msg_ && now_ms < flash_until_)
    {
        const FontDef &f = Font_7x10;
        int w = (int)strlen(flash_msg_) * f.FontWidth;
        disp_->DrawRect(0, 0, 63, 31, true, false);
        disp_->SetCursor((64 - w) / 2, (32 - f.FontHeight) / 2);
        disp_->WriteString(flash_msg_, f, true);
        disp_->Update();
        return;
    }

    if(page_ >= NumPages()) // effect changed underneath us
        page_ = NumPages() - 1;

    if(peek_param_ >= 0 && peek_param_ < (int)Fx().NumParams()
       && now_ms < peek_until_)
    {
        DrawParam((size_t)peek_param_, eff, false);
    }
    else
    {
        Page pg = PageAt(page_);
        switch(pg.kind)
        {
            case PAGE_HOME: DrawHome(eff); break;
            case PAGE_EFFECT: DrawEffect(editing_); break;
            case PAGE_BYPASS: DrawBypass(editing_); break;
            case PAGE_BYPMODE: DrawBypMode(editing_); break;
            case PAGE_PARAM: DrawParam(pg.index, eff, editing_); break;
            case PAGE_ROUTE: DrawRoute(pg.index, editing_); break;
            case PAGE_INPUT: DrawInput(editing_); break;
            case PAGE_CPU: DrawCpu(); break;
            case PAGE_SD_LOAD: DrawSd(true, editing_); break;
            case PAGE_SD_SAVE: DrawSd(false, editing_); break;
            case PAGE_SAVE: DrawSave(editing_); break;
        }
    }

    DrawMeters(meter_in, meter_out);
    disp_->Update();
}

void Ui::DrawHeader(const char *title, bool inverted, const char *tag)
{
    if(inverted)
        disp_->DrawRect(0, 0, kContentW - 1, 7, true, true);
    disp_->SetCursor(1, 0);
    disp_->WriteString(title, Font_6x8, !inverted);
    if(tag && tag[0])
    {
        int w = (int)strlen(tag) * Font_4x6.FontWidth;
        disp_->SetCursor(kContentW - w, 1);
        disp_->WriteString(tag, Font_4x6, !inverted);
    }
}

void Ui::DrawHome(const float *eff)
{
    DrawHeader(Fx().Name(),
               false,
               Sl().Bypassed()                              ? "BYP"
               : settings_->input_mode == app::INPUT_MONO ? "MONO"
                                                            : nullptr);

    const int n     = (int)Fx().NumParams();
    const int col_w = kContentW / n;
    const int top = 10, bottom = 24; // bar area
    for(int i = 0; i < n; i++)
    {
        int x0 = i * col_w + 2;
        int x1 = x0 + col_w - 5;
        disp_->DrawRect(x0, top, x1, bottom, true, false);
        int h = (int)(eff[i] * (bottom - top) + 0.5f);
        if(h > 0)
            disp_->DrawRect(x0, bottom - h, x1, bottom, true, true);

        // Short label that fits the column (4 px per char): 3 chars for up
        // to 4 params ("DECAY" -> "DEC", "LO CUT" -> "LO"), 2 for more.
        const int   max_chars = col_w / 4 < 3 ? col_w / 4 : 3;
        char        label[4]  = {0};
        const char *name      = Fx().Spec(i).name;
        for(int c = 0; c < max_chars && name[c] && name[c] != ' '; c++)
            label[c] = name[c];
        disp_->SetCursor(x0, 26);
        disp_->WriteString(label, Font_4x6, true);
    }
}

void Ui::DrawParam(size_t idx, const float *eff, bool editing)
{
    DrawHeader(Fx().Spec(idx).name, editing, SourceTag(idx));

    // Value: big font if it fits, else medium
    char buf[12];
    FormatValue(idx, eff[idx], buf, sizeof(buf));
    const FontDef *f = &Font_11x18;
    if((int)strlen(buf) * f->FontWidth > kContentW)
        f = &Font_7x10;
    disp_->SetCursor(0, f == &Font_11x18 ? 8 : 12);
    disp_->WriteString(buf, *f, true);

    // Bar: outline + fill = effective value, tick = stored (base) value
    const int bx1 = kContentW - 1;
    disp_->DrawRect(0, 28, bx1, 31, true, false);
    int fill = (int)(eff[idx] * bx1 + 0.5f);
    if(fill > 0)
        disp_->DrawRect(0, 28, fill, 31, true, true);
    int tick = (int)(Sl().norm[idx] * bx1 + 0.5f);
    disp_->DrawLine(tick, 26, tick, 27, true);
}

void Ui::DrawRoute(size_t src, bool editing)
{
    DrawHeader(kSourceTitle[src], editing, src < app::SRC_CV_1 ? "SET" : "MOD");
    const app::Route &rt = settings_->route[src];
    const char       *dest = "--";
    char              buf[12];
    if(rt.slot == slot_)
        dest = Fx().Spec(rt.param).name;
    else if(!rt.None())
    {
        // routed to another slot (set by a chain preset): "3:DECAY"
        const fx::Effect *p = fx::Prototype(settings_->slot[rt.slot].type);
        snprintf(buf, sizeof(buf), "%d:%s", rt.slot + 1, p ? p->Spec(rt.param).name : "?");
        dest = buf;
    }
    disp_->SetCursor(0, 10);
    disp_->WriteString(">", Font_7x10, true);
    disp_->SetCursor(10, 10);
    disp_->WriteString(dest, Font_7x10, true);
    disp_->SetCursor(0, 25);
    disp_->WriteString(editing ? "turn: dest" : "push: edit", Font_4x6, true);
}

void Ui::DrawEffect(bool editing)
{
    DrawHeader("EFFECT", editing, nullptr);
    const int   cur   = Sl().type < num_effects_ ? Sl().type : 0;
    const int   shown = editing ? pending_effect_ : cur;
    const char *name  = effects_[shown]->Name();
    disp_->SetCursor(0, 10);
    disp_->WriteString(name, Font_7x10, true);
    const char *hint = !editing                               ? "push: change"
                       : pending_effect_ != cur ? "push: switch"
                                                              : "push: keep";
    disp_->SetCursor(0, 25);
    disp_->WriteString(hint, Font_4x6, true);
}

void Ui::DrawBypass(bool editing)
{
    DrawHeader("BYPASS", editing, nullptr);
    disp_->SetCursor(0, 10);
    disp_->WriteString(Sl().Bypassed() ? "ON" : "OFF", Font_7x10, true);
    disp_->SetCursor(0, 25);
    disp_->WriteString(Sl().Bypassed() ? "effect bypassed" : "effect active", Font_4x6, true);
}

void Ui::DrawBypMode(bool editing)
{
    DrawHeader("BYP MODE", editing, nullptr);
    const bool hard = Sl().Hard();
    disp_->SetCursor(0, 10);
    disp_->WriteString(hard ? "HARD" : "TRAILS", Font_7x10, true);
    disp_->SetCursor(0, 25);
    disp_->WriteString(hard ? "cut tails" : "tails ring out", Font_4x6, true);
}

void Ui::DrawInput(bool editing)
{
    DrawHeader("INPUT", editing, nullptr);
    const bool mono = settings_->input_mode == app::INPUT_MONO;
    disp_->SetCursor(0, 10);
    disp_->WriteString(mono ? "MONO" : "STEREO", Font_7x10, true);
    disp_->SetCursor(0, 25);
    disp_->WriteString(mono ? "L in -> L+R" : "L->L  R->R", Font_4x6, true);
}

void Ui::DrawCpu()
{
    // Average load large, peak (since the last reset) underneath.
    // Peak >= 95 % inverts the header: the audio deadline is close.
    const int avg  = (int)(cpu_avg_ * 100.f + 0.5f);
    const int peak = (int)(cpu_peak_ * 100.f + 0.5f);
    DrawHeader("CPU", peak >= 95, nullptr);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", avg);
    disp_->SetCursor(0, 8);
    disp_->WriteString(buf, Font_11x18, true);

    snprintf(buf, sizeof(buf), "pk %d%% push:clr", peak);
    disp_->SetCursor(0, 26);
    disp_->WriteString(buf, Font_4x6, true);
}

void Ui::DrawSd(bool load, bool editing)
{
    DrawHeader(load ? "LOAD SD" : "SAVE SD", editing, nullptr);

    char num[4];
    snprintf(num, sizeof(num), "%02d", sd_slot_);
    disp_->SetCursor(0, 8);
    disp_->WriteString(num, Font_11x18, true);

    // status next to the slot number, name on the bottom line
    const bool  fresh  = sd_info_slot_ == sd_slot_;
    // used slot: show which effect it's for (falls back to "used")
    const char *status = !fresh     ? "..."
                         : !sd_used_ ? sd_text_
                         : sd_detail_[0] ? sd_detail_
                                         : "used";
    disp_->SetCursor(26, 10);
    disp_->WriteString(status, Font_4x6, true);
    disp_->SetCursor(26, 18);
    disp_->WriteString(editing ? (load ? "push:load" : "push:save")
                               : "push:pick",
                       Font_4x6,
                       true);
    // bottom line: preset name, or an error code if something failed
    const char *bottom = !fresh ? "" : (sd_used_ ? sd_text_ : sd_detail_);
    if(bottom[0])
    {
        char line[16];
        strncpy(line, bottom, 15);
        line[15] = 0;
        disp_->SetCursor(0, 26);
        disp_->WriteString(line, Font_4x6, true);
    }
}

void Ui::DrawSave(bool)
{
    DrawHeader("SAVE", false, nullptr);
    disp_->SetCursor(0, 8);
    disp_->WriteString("PUSH", Font_11x18, true);
    disp_->SetCursor(0, 26);
    disp_->WriteString("power-up setup", Font_4x6, true);
}

void Ui::DrawMeters(float in, float out)
{
    auto bar = [this](int x, float v) {
        v     = Clamp01(v);
        int h = (int)(v * 32.f + 0.5f);
        if(h > 0)
            disp_->DrawLine(x, 32 - h, x, 31, true);
    };
    bar(61, in);
    bar(63, out);
}

const char *Ui::SourceTag(size_t param) const
{
    static char tag[9];
    tag[0] = 0;
    for(int s = 0; s < app::SRC_LAST; s++)
    {
        if(settings_->route[s].Is(slot_, (int)param) && strlen(tag) < 6)
            strcat(tag, kSourceTag[s]);
    }
    return tag;
}

void Ui::FormatValue(size_t idx, float norm, char *buf, size_t len) const
{
    const fx::ParamSpec &s = Fx().Spec(idx);
    float                v = s.ToReal(norm);
    // integer formatting only (newlib-nano printf has no %f by default)
    switch(s.unit)
    {
        case fx::Unit::Seconds:
            if(v < 10.f)
            {
                int c = (int)(v * 100.f + 0.5f);
                snprintf(buf, len, "%d.%02ds", c / 100, c % 100);
            }
            else
            {
                int d = (int)(v * 10.f + 0.5f);
                snprintf(buf, len, "%d.%ds", d / 10, d % 10);
            }
            break;
        case fx::Unit::Percent: snprintf(buf, len, "%d%%", (int)(v + 0.5f)); break;
        case fx::Unit::Hertz:
            if(v < 1000.f)
                snprintf(buf, len, "%dHz", (int)(v + 0.5f));
            else
            {
                int d = (int)(v / 100.f + 0.5f); // tenths of kHz
                snprintf(buf, len, "%d.%dk", d / 10, d % 10);
            }
            break;
    }
}

} // namespace ui
