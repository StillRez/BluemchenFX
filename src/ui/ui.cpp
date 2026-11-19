#include "ui/ui.h"
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace daisy;

namespace ui
{
static constexpr int      kContentW   = 60; // x 0..59; meters at 61 & 63
static constexpr int      kCpuBarX    = 59; // HOME only
static constexpr int      kHomeRows   = 4;  // 8 px rows on a 32 px display
static constexpr int      kHomeItems  = (int)app::kNumSlots + 1; // slots + SETUP
static constexpr uint32_t kPeekMs     = 1200;
static constexpr float    kStepFine   = 0.005f;
static constexpr float    kStepNormal = 0.01f;
static constexpr float    kStepFast   = 0.04f;

// Slot pages: TYPE, BYPASS, BYP MODE, then the parameters
static constexpr int kFirstParamPage = 3;
// Setup pages: the 4 routes, then these
static constexpr int kSetupExtra = 6; // INPUT, OUT, CPU, LOAD SD, SAVE SD, SAVE

static const char *const kSourceTitle[app::SRC_LAST]
    = {"KNOB 1", "KNOB 2", "CV 1", "CV 2"};
static const char *const kSourceTag[app::SRC_LAST] = {"K1", "K2", "C1", "C2"};

static inline float Clamp01(float x)
{
    return x < 0.f ? 0.f : (x > 1.f ? 1.f : x);
}

static inline int Wrap(int v, int n)
{
    return ((v % n) + n) % n;
}

void Ui::Init(OneBitGraphicsDisplay *display, app::Settings *settings)
{
    disp_     = display;
    settings_ = settings;
    level_    = LVL_HOME;
    sel_      = settings_->ui_slot < app::kNumSlots ? settings_->ui_slot : 0;
    top_      = 0;
    page_     = 0;
    editing_  = false;
    confirm_  = false;
    for(int &p : slot_page_)
        p = -1;
    setup_page_ = 0;
}

// ---------------------------------------------------------------- pages
int Ui::NumPages() const
{
    switch(level_)
    {
        case LVL_SLOT:
        {
            const fx::Effect *e = FxIn(slot_);
            return e ? kFirstParamPage + (int)e->NumParams() : 1; // empty: TYPE
        }
        case LVL_SETUP: return app::SRC_LAST + kSetupExtra;
        default: return 1;
    }
}

Ui::Page Ui::PageAt(int p) const
{
    if(level_ == LVL_SLOT)
    {
        if(p == 0)
            return {PAGE_TYPE, 0};
        if(p == 1)
            return {PAGE_BYPASS, 0};
        if(p == 2)
            return {PAGE_BYPMODE, 0};
        return {PAGE_PARAM, p - kFirstParamPage};
    }
    if(p < app::SRC_LAST)
        return {PAGE_ROUTE, p};
    switch(p - app::SRC_LAST)
    {
        case 0: return {PAGE_INPUT, 0};
        case 1: return {PAGE_OUT, 0};
        case 2: return {PAGE_CPU, 0};
        case 3: return {PAGE_SD_LOAD, 0};
        case 4: return {PAGE_SD_SAVE, 0};
        default: return {PAGE_SAVE, 0};
    }
}

void Ui::ClampPage()
{
    const int n = NumPages();
    if(page_ >= n) // e.g. the slot's effect changed underneath us
    {
        page_    = n - 1;
        editing_ = false;
        confirm_ = false;
    }
    if(page_ < 0)
        page_ = 0;
}

bool Ui::IsSdPage(int p) const
{
    if(level_ != LVL_SETUP)
        return false;
    PageKind k = PageAt(p).kind;
    return k == PAGE_SD_LOAD || k == PAGE_SD_SAVE;
}

void Ui::EnterHome()
{
    if(level_ == LVL_SLOT)
        slot_page_[slot_] = page_;
    else if(level_ == LVL_SETUP)
        setup_page_ = page_;
    level_   = LVL_HOME;
    page_    = 0;
    editing_ = false;
    confirm_ = false;
}

// --------------------------------------------------------------- routes
int Ui::NumRouteDests() const
{
    int n = 1; // none
    for(size_t k = 0; k < app::kNumSlots; k++)
        if(const fx::Effect *e = FxIn((int)k))
            n += (int)e->NumParams();
    return n;
}

app::Route Ui::RouteAt(int pos) const
{
    if(pos > 0)
    {
        int i = pos - 1;
        for(size_t k = 0; k < app::kNumSlots; k++)
        {
            const fx::Effect *e = FxIn((int)k);
            if(!e)
                continue;
            if(i < (int)e->NumParams())
                return {(int8_t)k, (int8_t)i};
            i -= (int)e->NumParams();
        }
    }
    return {app::kRouteNone, app::kRouteNone};
}

int Ui::RoutePos(const app::Route &rt) const
{
    if(rt.None())
        return 0;
    int pos = 1;
    for(int k = 0; k < (int)app::kNumSlots; k++)
    {
        const fx::Effect *e = FxIn(k);
        if(!e)
            continue;
        if(k == rt.slot)
            return rt.param < (int)e->NumParams() ? pos + rt.param : 0;
        pos += (int)e->NumParams();
    }
    return 0;
}

// --------------------------------------------------------------- events
void Ui::OnTurn(int32_t inc, uint32_t now_ms)
{
    if(inc == 0)
        return;
    peek_until_ = 0; // turning cancels a peek and returns to the current page
    const int dir = inc > 0 ? 1 : -1;

    if(level_ == LVL_HOME)
    {
        TurnHome(dir);
        return;
    }

    ClampPage();
    if(!editing_)
    {
        page_ += dir;
        ClampPage();
        if(IsSdPage(page_))
            sd_request_ = SdAction::PEEK; // refresh slot info (card may have changed)
        return;
    }
    TurnValue(inc, now_ms);
}

void Ui::TurnHome(int dir)
{
    sel_ += dir;
    if(sel_ < 0)
        sel_ = 0;
    if(sel_ >= kHomeItems)
        sel_ = kHomeItems - 1;
    if(sel_ < top_)
        top_ = sel_;
    if(sel_ >= top_ + kHomeRows)
        top_ = sel_ - kHomeRows + 1;
    if(sel_ < (int)app::kNumSlots)
        settings_->ui_slot = (uint8_t)sel_;
}

void Ui::TurnValue(int32_t inc, uint32_t now_ms)
{
    const int dir = inc > 0 ? 1 : -1;
    // Acceleration: fast turning = bigger steps
    const uint32_t dt   = now_ms - last_turn_;
    const bool     fast = dt < 25;
    last_turn_          = now_ms;

    Page pg = PageAt(page_);
    switch(pg.kind)
    {
        case PAGE_PARAM:
        {
            float step = fast ? kStepFast : (dt < 80 ? kStepNormal : kStepFine);
            float &v   = Sl().norm[pg.index];
            v          = Clamp01(v + step * (float)inc);
            edited_slot_  = slot_;
            edited_param_ = pg.index;
            break;
        }
        case PAGE_TYPE:
        {
            // choices: empty, then every effect
            confirm_ = false;
            int pos  = pending_type_ == fx::FX_NONE ? 0 : pending_type_ + 1;
            pos      = Wrap(pos + dir, fx::FX_COUNT + 1);
            pending_type_ = pos == 0 ? (uint8_t)fx::FX_NONE : (uint8_t)(pos - 1);
            break;
        }
        case PAGE_BYPASS: Sl().flags ^= app::SLOT_BYPASS; break;
        case PAGE_BYPMODE: Sl().flags ^= app::SLOT_HARD; break;
        case PAGE_ROUTE:
        {
            app::Route &rt = settings_->route[pg.index];
            rt             = RouteAt(Wrap(RoutePos(rt) + dir, NumRouteDests()));
            route_changed_ = true;
            break;
        }
        case PAGE_INPUT:
            settings_->input_mode
                = (uint8_t)Wrap(settings_->input_mode + dir, app::INPUT_MODE_LAST);
            break;
        case PAGE_OUT:
        {
            const float g  = settings_->out_gain;
            float       db = g > 0.f ? roundf(20.f * log10f(g) * 2.f) * 0.5f : kOutMinDb;
            db += (fast ? 1.f : 0.5f) * (float)dir;
            db = db < kOutMinDb ? kOutMinDb : (db > kOutMaxDb ? kOutMaxDb : db);
            settings_->out_gain = db == 0.f ? 1.f : powf(10.f, db / 20.f);
            break;
        }
        case PAGE_SD_LOAD:
        case PAGE_SD_SAVE:
            sd_slot_    = Wrap(sd_slot_ - 1 + dir, app::kPresetSlotCount) + 1;
            sd_request_ = SdAction::PEEK;
            break;
        default: break;
    }
}

void Ui::OnClick(uint32_t now_ms)
{
    peek_until_ = 0;
    if(level_ == LVL_HOME)
    {
        if(sel_ < (int)app::kNumSlots)
        {
            level_ = LVL_SLOT;
            slot_  = sel_;
            // first visit: the first parameter (TYPE if empty), then the
            // page last used
            page_ = slot_page_[slot_] >= 0 ? slot_page_[slot_]
                                           : (FxIn(slot_) ? kFirstParamPage : 0);
        }
        else
        {
            level_ = LVL_SETUP;
            page_  = setup_page_;
        }
        editing_ = false;
        confirm_ = false;
        ClampPage();
        if(IsSdPage(page_))
            sd_request_ = SdAction::PEEK;
        return;
    }
    ClampPage();
    ClickPage(now_ms);
}

void Ui::ClickPage(uint32_t now_ms)
{
    Page pg = PageAt(page_);
    switch(pg.kind)
    {
        case PAGE_TYPE:
            if(!editing_)
            {
                pending_type_ = Sl().type;
                editing_      = true;
            }
            else if(pending_type_ == Sl().type)
            {
                editing_ = false; // unchanged
            }
            else if(Sl().type != fx::FX_NONE && !confirm_)
            {
                confirm_ = true; // replacing an effect: one more click
            }
            else
            {
                type_req_slot_ = slot_;
                type_req_      = pending_type_;
                editing_       = false;
                confirm_       = false;
            }
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
                sd_request_ = pg.kind == PAGE_SD_LOAD ? SdAction::LOAD : SdAction::SAVE;
            else
                sd_request_ = SdAction::PEEK;
            editing_ = !editing_;
            break;
        default: editing_ = !editing_; break;
    }
}

void Ui::OnLongPress(uint32_t now_ms)
{
    peek_until_ = 0;
    if(level_ == LVL_HOME)
    {
        save_request_ = true;
        Flash("SAVED", now_ms);
        return;
    }
    EnterHome(); // back (also cancels an edit or a pending type change)
}

void Ui::Peek(int slot, size_t param, uint32_t now_ms)
{
    if(slot < 0 || slot >= (int)app::kNumSlots)
        return;
    // Don't hijack the screen while the user is on that very page
    if(level_ == LVL_SLOT && slot == slot_ && page_ < NumPages())
    {
        Page pg = PageAt(page_);
        if(pg.kind == PAGE_PARAM && (size_t)pg.index == param)
            return;
    }
    peek_slot_  = slot;
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

bool Ui::TakeEditedParam(int &slot, int &param)
{
    if(edited_param_ < 0)
        return false;
    slot          = edited_slot_;
    param         = edited_param_;
    edited_param_ = -1;
    return true;
}

bool Ui::TakeTypeRequest(int &slot, uint8_t &type)
{
    if(type_req_slot_ < 0)
        return false;
    slot           = type_req_slot_;
    type           = type_req_;
    type_req_slot_ = -1;
    return true;
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
void Ui::DrawMessage(const char *msg)
{
    const FontDef &f = Font_7x10;
    int            w = (int)strlen(msg) * f.FontWidth;
    disp_->DrawRect(0, 0, 63, 31, true, false);
    disp_->SetCursor((64 - w) / 2, (32 - f.FontHeight) / 2);
    disp_->WriteString(msg, f, true);
}

void Ui::ShowBusy(const char *msg)
{
    disp_->Fill(false);
    DrawMessage(msg);
    disp_->Update();
}

void Ui::Draw(const float (*eff)[app::kMaxParams],
              float    meter_in,
              float    meter_out,
              uint32_t now_ms)
{
    disp_->Fill(false);

    if(flash_msg_ && now_ms < flash_until_)
    {
        DrawMessage(flash_msg_);
        disp_->Update();
        return;
    }

    ClampPage();
    const fx::Effect *peek_fx = peek_slot_ >= 0 ? FxIn(peek_slot_) : nullptr;
    if(peek_fx && peek_param_ >= 0 && peek_param_ < (int)peek_fx->NumParams()
       && now_ms < peek_until_)
    {
        DrawParam(peek_slot_, (size_t)peek_param_, eff[peek_slot_], false);
    }
    else if(level_ == LVL_HOME)
    {
        DrawHome();
    }
    else
    {
        Page pg = PageAt(page_);
        switch(pg.kind)
        {
            case PAGE_TYPE: DrawType(editing_); break;
            case PAGE_BYPASS: DrawBypass(editing_); break;
            case PAGE_BYPMODE: DrawBypMode(editing_); break;
            case PAGE_PARAM: DrawParam(slot_, pg.index, eff[slot_], editing_); break;
            case PAGE_ROUTE: DrawRoute(pg.index, editing_); break;
            case PAGE_INPUT: DrawInput(editing_); break;
            case PAGE_OUT: DrawOut(editing_); break;
            case PAGE_CPU: DrawCpu(); break;
            case PAGE_SD_LOAD: DrawSd(true, editing_); break;
            case PAGE_SD_SAVE: DrawSd(false, editing_); break;
            case PAGE_SAVE: DrawSave(); break;
        }
    }

    DrawMeters(meter_in, meter_out);
    disp_->Update();
}

void Ui::DrawHeader(const char *title, bool inverted, const char *tag, int slot)
{
    if(inverted)
        disp_->DrawRect(0, 0, kContentW - 1, 7, true, true);
    int x = 1;
    if(slot >= 0)
    {
        // slot number badge: a box in the opposite colour of the header
        char num[2] = {(char)('1' + slot), 0};
        disp_->DrawRect(0, 0, 5, 7, !inverted, true);
        disp_->SetCursor(1, 1);
        disp_->WriteString(num, Font_4x6, inverted);
        x = 7;
    }
    disp_->SetCursor(x, 0);
    disp_->WriteString(title, Font_6x8, !inverted);
    if(tag && tag[0])
    {
        // right-aligned, cut to the room left after the title
        const int room  = kContentW - (x + (int)strlen(title) * Font_6x8.FontWidth) - 1;
        int       chars = (int)strlen(tag);
        if(chars * Font_4x6.FontWidth > room)
            chars = room / Font_4x6.FontWidth;
        if(chars > 0)
        {
            char buf[12];
            chars = chars < (int)sizeof(buf) - 1 ? chars : (int)sizeof(buf) - 1;
            memcpy(buf, tag, chars);
            buf[chars] = 0;
            disp_->SetCursor(kContentW - chars * Font_4x6.FontWidth, 1);
            disp_->WriteString(buf, Font_4x6, !inverted);
        }
    }
}

void Ui::DrawSmall(int x, int y, const char *text)
{
    // The 4x6 font's lowercase is hard to read on the OLED (hardware
    // feedback, phase 2): small text is always shown in capitals.
    char buf[24];
    size_t i = 0;
    for(; text[i] && i < sizeof(buf) - 1; i++)
        buf[i] = (text[i] >= 'a' && text[i] <= 'z') ? (char)(text[i] - 'a' + 'A') : text[i];
    buf[i] = 0;
    disp_->SetCursor(x, y);
    disp_->WriteString(buf, Font_4x6, true);
}

void Ui::DrawHint(const char *hint)
{
    DrawSmall(0, 26, hint);
}

void Ui::DrawHome()
{
    const int text_right = kCpuBarX - 2; // inverted row ends here
    for(int r = 0; r < kHomeRows; r++)
    {
        const int item = top_ + r;
        if(item >= kHomeItems)
            break;
        const int  y   = r * 8;
        const bool sel = item == sel_;
        char       line[24];
        if(item < (int)app::kNumSlots)
        {
            const app::SlotState &sl = settings_->slot[item];
            const fx::Effect     *e  = FxIn(item);
            snprintf(line, sizeof(line), "%d %s", item + 1, e ? e->Name() : "--");
            if(e && sl.Bypassed()) // bypassed: lowercase name
                for(char *c = line + 2; *c; c++)
                    if(*c >= 'A' && *c <= 'Z')
                        *c = (char)(*c - 'A' + 'a');
        }
        else
        {
            snprintf(line, sizeof(line), "SETUP");
        }
        if(sel)
            disp_->DrawRect(0, y, text_right, y + 7, true, true);
        disp_->SetCursor(1, y);
        disp_->WriteString(line, Font_6x8, !sel);
    }

    // more rows above / below: small arrows at the right of the text area
    const int ax = text_right - 6;
    if(top_ > 0)
        for(int i = 0; i < 3; i++)
            disp_->DrawLine(ax + 2 - i, i, ax + 2 + i, i, top_ != sel_);
    if(top_ + kHomeRows < kHomeItems)
        for(int i = 0; i < 3; i++)
            disp_->DrawLine(ax + 2 - i, 31 - i, ax + 2 + i, 31 - i, top_ + kHomeRows - 1 != sel_);

    // CPU bar (average load); the whole column inverts when the peak gets
    // close to the audio deadline
    const bool over = cpu_peak_ >= 0.95f;
    const int  h    = (int)(Clamp01(cpu_avg_) * 32.f + 0.5f);
    if(over)
        disp_->DrawLine(kCpuBarX, 0, kCpuBarX, 31 - h, true);
    else if(h > 0)
        disp_->DrawLine(kCpuBarX, 32 - h, kCpuBarX, 31, true);
}

void Ui::DrawParam(int slot, size_t idx, const float *eff, bool editing)
{
    const fx::Effect &e = *FxIn(slot);
    DrawHeader(e.Spec(idx).name, editing, SourceTag(slot, idx), slot);

    // Value: big font if it fits, else medium
    char buf[12];
    FormatValue(e, idx, eff[idx], buf, sizeof(buf));
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
    int tick = (int)(settings_->slot[slot].norm[idx] * bx1 + 0.5f);
    disp_->DrawLine(tick, 26, tick, 27, true);
}

void Ui::DrawType(bool editing)
{
    const uint8_t     cur  = Sl().type;
    const uint8_t     show = editing ? pending_type_ : cur;
    const fx::Effect *e    = fx::Prototype(show);
    DrawHeader(confirm_ ? "REPLACE?" : "TYPE", editing, nullptr, slot_);
    disp_->SetCursor(0, 9);
    disp_->WriteString(e ? e->Name() : "EMPTY", Font_7x10, true);
    if(confirm_)
    {
        const fx::Effect *old = fx::Prototype(cur);
        char              buf[16];
        snprintf(buf, sizeof(buf), "old: %s", old ? old->Name() : "--");
        DrawSmall(0, 19, buf);
        DrawHint("push:ok turn:no");
        return;
    }
    DrawHint(!editing        ? "push: change"
             : show == cur   ? "push: keep"
             : cur == fx::FX_NONE ? "push: set"
                                  : "push: replace");
}

void Ui::DrawBypass(bool editing)
{
    DrawHeader("BYPASS", editing, nullptr, slot_);
    disp_->SetCursor(0, 10);
    disp_->WriteString(Sl().Bypassed() ? "ON" : "OFF", Font_7x10, true);
    DrawHint(Sl().Bypassed() ? "effect bypassed" : "effect active");
}

void Ui::DrawBypMode(bool editing)
{
    DrawHeader("BYP MODE", editing, nullptr, slot_);
    const bool hard = Sl().Hard();
    disp_->SetCursor(0, 10);
    disp_->WriteString(hard ? "HARD" : "TRAILS", Font_7x10, true);
    DrawHint(hard ? "cut tails" : "tails ring out");
}

void Ui::DrawRoute(size_t src, bool editing)
{
    DrawHeader(kSourceTitle[src], editing, src < app::SRC_CV_1 ? "SET" : "MOD");
    const app::Route &rt = settings_->route[src];
    const fx::Effect *e  = rt.None() ? nullptr : FxIn(rt.slot);
    char              buf[16];
    if(e && rt.param >= 0 && rt.param < (int)e->NumParams())
        snprintf(buf, sizeof(buf), "%d:%s", rt.slot + 1, e->Spec(rt.param).name);
    else
        snprintf(buf, sizeof(buf), "NONE");
    disp_->SetCursor(0, 10);
    disp_->WriteString(buf, Font_7x10, true);
    DrawHint(editing ? "turn: dest" : "push: edit");
}

void Ui::DrawInput(bool editing)
{
    DrawHeader("INPUT", editing, nullptr);
    const bool mono = settings_->input_mode == app::INPUT_MONO;
    disp_->SetCursor(0, 10);
    disp_->WriteString(mono ? "MONO" : "STEREO", Font_7x10, true);
    DrawHint(mono ? "L in -> L+R" : "L->L  R->R");
}

void Ui::DrawOut(bool editing)
{
    DrawHeader("OUT", editing, nullptr);
    const float g      = settings_->out_gain;
    const int   halves = g > 0.f ? (int)roundf(20.f * log10f(g) * 2.f) : (int)(kOutMinDb * 2.f);
    const int   a      = halves < 0 ? -halves : halves;
    char        buf[24];
    snprintf(buf, sizeof(buf), "%s%d.%ddB", halves < 0 ? "-" : (halves > 0 ? "+" : ""),
             a / 2, (a % 2) * 5);
    const FontDef *f = &Font_11x18;
    if((int)strlen(buf) * f->FontWidth > kContentW)
        f = &Font_7x10;
    disp_->SetCursor(0, f == &Font_11x18 ? 8 : 12);
    disp_->WriteString(buf, *f, true);
    DrawHint(halves == 0 ? "unity" : "output level");
}

void Ui::DrawCpu()
{
    // Average load large, peak (since the last reset) underneath.
    // Peak >= 95 % inverts the header: the audio deadline is close.
    const int avg  = (int)(cpu_avg_ * 100.f + 0.5f);
    const int peak = (int)(cpu_peak_ * 100.f + 0.5f);
    DrawHeader(peak >= 95 ? "CPU!" : "CPU", peak >= 95, nullptr);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", avg);
    disp_->SetCursor(0, 8);
    disp_->WriteString(buf, Font_11x18, true);

    snprintf(buf, sizeof(buf), "pk %d%% push:clr", peak);
    DrawHint(buf);
}

void Ui::DrawSd(bool load, bool editing)
{
    DrawHeader(load ? "LOAD SD" : "SAVE SD", editing, nullptr);

    char num[4];
    snprintf(num, sizeof(num), "%02d", sd_slot_);
    disp_->SetCursor(0, 8);
    disp_->WriteString(num, Font_11x18, true);

    // status next to the slot number, name on the bottom line
    const bool fresh = sd_info_slot_ == sd_slot_;
    // used slot: show which effect it's for (falls back to "used")
    const char *status = !fresh         ? "..."
                         : !sd_used_     ? sd_text_
                         : sd_detail_[0] ? sd_detail_
                                         : "used";
    DrawSmall(24, 10, status);
    DrawSmall(24, 18, editing ? (load ? "push:load" : "push:save") : "push:pick");
    // bottom line: preset name, or an error code if something failed
    const char *bottom = !fresh ? "" : (sd_used_ ? sd_text_ : sd_detail_);
    if(bottom[0])
    {
        char line[16];
        strncpy(line, bottom, 15);
        line[15] = 0;
        DrawHint(line);
    }
}

void Ui::DrawSave()
{
    DrawHeader("SAVE", false, nullptr);
    disp_->SetCursor(0, 8);
    disp_->WriteString("PUSH", Font_11x18, true);
    DrawHint("power-up setup");
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

const char *Ui::SourceTag(int slot, size_t param) const
{
    static char tag[9];
    tag[0] = 0;
    for(int s = 0; s < app::SRC_LAST; s++)
        if(settings_->route[s].Is(slot, (int)param))
            strcat(tag, kSourceTag[s]);
    return tag;
}

void Ui::FormatValue(const fx::Effect &e, size_t idx, float norm, char *buf, size_t len) const
{
    const fx::ParamSpec &s = e.Spec(idx);
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
