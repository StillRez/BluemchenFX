// UI tests: the real ui::Ui drawing into a 64x32 framebuffer on the PC,
// driven by simulated encoder turns, clicks and long presses.
//
// Set UI_SHOTS=<dir> to also write every distinct screen as a PBM image
// (tools/ui_shots.py turns them into one PNG contact sheet).
#include "check.h"
#include "ui/ui.h"
#include "app/chain_settings.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
class FrameBuffer : public daisy::OneBitGraphicsDisplayImpl<FrameBuffer>
{
  public:
    struct Text
    {
        std::string str;
        int         x, y, w, h;
    };

    uint16_t Width() const override { return 64; }
    uint16_t Height() const override { return 32; }
    void     Fill(bool on) override
    {
        for(auto &row : px)
            for(auto &p : row)
                p = on;
        texts.clear();
    }
    void DrawPixel(uint_fast8_t x, uint_fast8_t y, bool on) override
    {
        if(x < 64 && y < 32)
            px[y][x] = on;
    }
    char WriteString(const char *str, FontDef font, bool on) override
    {
        texts.push_back({str,
                         currentX_,
                         currentY_,
                         (int)std::strlen(str) * font.FontWidth,
                         font.FontHeight});
        return OneBitGraphicsDisplayImpl<FrameBuffer>::WriteString(str, font, on);
    }
    void Update() override {}
    bool UpdateFinished() override { return true; }

    bool Has(const char *s) const
    {
        for(const Text &t : texts)
            if(t.str == s)
                return true;
        return false;
    }
    std::string All() const
    {
        std::string r;
        for(const Text &t : texts)
            r += "[" + t.str + "]";
        return r;
    }

    bool px[32][64];
    std::vector<Text> texts;
};

// The display and the UI under test, with settings and the effective
// parameter values the main loop would pass in.
struct Rig
{
    FrameBuffer   fb;
    app::Settings s;
    app::Recall   recall;
    ui::Ui        gui;
    float         eff[app::kNumSlots][app::kMaxParams];
    uint32_t      now = 1000;

    Rig()
    {
        s = app::MakeDefaults(); // REVERB in slot 1
        recall.Clear();
        gui.Init(&fb, &s);
        Draw();
    }
    void Draw()
    {
        for(size_t k = 0; k < app::kNumSlots; k++)
            for(size_t i = 0; i < app::kMaxParams; i++)
                eff[k][i] = s.slot[k].norm[i];
        gui.Draw(eff, 0.f, 0.f, now);
        Shot();
    }
    // Slow detents (no acceleration), one at a time like the encoder
    void Turn(int n)
    {
        for(int i = 0; i < (n < 0 ? -n : n); i++)
        {
            now += 200;
            gui.OnTurn(n < 0 ? -1 : 1, now);
        }
        Pump();
    }
    void Click()
    {
        now += 200;
        gui.OnClick(now);
        Pump();
    }
    void Hold()
    {
        now += 200;
        gui.OnLongPress(now);
        now += 1000; // let a "SAVED" flash expire
        Pump();
    }
    // What the main loop does with the UI's requests
    void Pump()
    {
        int     slot;
        uint8_t type;
        while(gui.TakeTypeRequest(slot, type))
        {
            app::SetSlotType(s, recall, slot, type);
            now += 1000; // the type name flash expires
        }
        Draw();
    }
    // HOME -> slot k's TYPE page -> pick `type` (and confirm if needed)
    void SetType(int k, uint8_t type)
    {
        GoHome();
        Turn(-10);
        Turn(k);
        Click(); // enter slot
        Turn(-20); // TYPE is the first page
        Click(); // edit
        const int target = type == fx::FX_NONE ? 0 : type + 1;
        const int cur    = s.slot[k].type == fx::FX_NONE ? 0 : s.slot[k].type + 1;
        Turn(target - cur);
        Click();
        if(gui.Confirming())
            Click();
        GoHome();
    }
    void GoHome()
    {
        if(gui.Lvl() != ui::Ui::LVL_HOME)
            Hold();
    }

    // Writes each new screen as PBM if UI_SHOTS is set
    void Shot()
    {
        static const char *dir = getenv("UI_SHOTS");
        static std::vector<std::string> seen;
        if(!dir)
            return;
        std::string img;
        for(int y = 0; y < 32; y++)
            for(int x = 0; x < 64; x++)
                img += fb.px[y][x] ? '1' : '0';
        for(const std::string &o : seen)
            if(o == img)
                return;
        seen.push_back(img);
        char path[512];
        snprintf(path, sizeof(path), "%s/screen_%03zu.pbm", dir, seen.size());
        if(FILE *f = fopen(path, "w"))
        {
            fprintf(f, "P1\n64 32\n");
            for(int y = 0; y < 32; y++)
            {
                for(int x = 0; x < 64; x++)
                    fputc(fb.px[y][x] ? '1' : '0', f);
                fputc('\n', f);
            }
            fclose(f);
        }
    }

    // Nothing but the meters/CPU bar may draw right of the content area,
    // and every text must fit inside it.
    void CheckLayout(const char *where)
    {
        const bool home = gui.Lvl() == ui::Ui::LVL_HOME;
        const int  edge = home ? 58 : 60; // HOME: CPU bar at x 59
        for(const FrameBuffer::Text &t : fb.texts)
            CHECKF(t.x + t.w <= edge && t.y + t.h <= 32,
                   "%s: '%s' at x=%d w=%d", where, t.str.c_str(), t.x, t.w);
        // small (4x6) text: capitals only, its lowercase is hard to read
        for(const FrameBuffer::Text &t : fb.texts)
            if(t.h == 6)
                for(char c : t.str)
                    CHECKF(!(c >= 'a' && c <= 'z'), "%s: lowercase in '%s'", where, t.str.c_str());
        for(int y = 0; y < 32; y++)
            for(int x = edge; x < 64; x++)
                if(fb.px[y][x] && !(home && x == 59)) // CPU bar
                {
                    CHECKF(false, "%s: pixel at %d,%d (meters are 0) %s",
                           where, x, y, fb.All().c_str());
                    return;
                }
    }
};
} // namespace

TEST(ui_home_lists_the_chain)
{
    Rig r;
    CHECK(r.gui.Lvl() == ui::Ui::LVL_HOME);
    CHECK(r.fb.Has("1 REVERB"));
    CHECK(r.fb.Has("2 --"));
    CHECK(r.fb.Has("4 --"));
    CHECK(!r.fb.Has("SETUP")); // 5th row: scrolled into view below

    r.Turn(10);
    CHECK(r.gui.HomeSel() == ui::Ui::kSetupRow);
    CHECK(r.fb.Has("SETUP"));
    CHECK(!r.fb.Has("1 REVERB"));
    r.Turn(-10);
    CHECK(r.gui.HomeSel() == 0);
    CHECK(r.fb.Has("1 REVERB"));

    // bypassed slots are shown in lowercase
    r.s.slot[0].flags |= app::SLOT_BYPASS;
    r.Draw();
    CHECKF(r.fb.Has("1 reverb"), "%s", r.fb.All().c_str());
}

TEST(ui_build_a_chain_from_the_panel)
{
    Rig r;
    // slot 2: HOME, turn, click opens TYPE (the slot is empty)
    r.Turn(1);
    r.Click();
    CHECK(r.gui.Lvl() == ui::Ui::LVL_SLOT && r.gui.Slot() == 1);
    CHECK(r.gui.CurrentPage().kind == ui::Ui::PAGE_TYPE);
    CHECK(r.fb.Has("EMPTY"));
    r.Turn(3); // an empty slot has no other pages
    CHECK(r.gui.CurrentPage().kind == ui::Ui::PAGE_TYPE);

    r.Click();   // edit
    r.Turn(2);   // EMPTY -> REVERB -> DELAY
    CHECK(r.fb.Has("DELAY"));
    CHECK(r.s.slot[1].type == fx::FX_NONE); // nothing happens before the click
    r.Click();   // empty slot: no confirmation needed
    CHECK(r.s.slot[1].type == fx::FX_DELAY);
    CHECK(!r.gui.Editing());
    r.Turn(3); // now: BYPASS, BYP MODE, first parameter
    CHECK(r.gui.CurrentPage().kind == ui::Ui::PAGE_PARAM);
    CHECK(r.fb.Has("TIME"));

    r.Hold(); // back
    CHECK(r.gui.Lvl() == ui::Ui::LVL_HOME);
    CHECK(r.fb.Has("2 DELAY"));

    // the same effect twice, and all four slots
    r.SetType(2, fx::FX_DELAY);
    r.SetType(3, fx::FX_REVERB);
    CHECK(r.s.slot[2].type == fx::FX_DELAY && r.s.slot[3].type == fx::FX_REVERB);
    r.Turn(-10);
    CHECK(r.fb.Has("1 REVERB") && r.fb.Has("2 DELAY") && r.fb.Has("3 DELAY")
          && r.fb.Has("4 REVERB"));
    CHECK(app::Valid(r.s));

    // emptying a slot again
    r.SetType(1, fx::FX_NONE);
    CHECK(r.s.slot[1].type == fx::FX_NONE);
    CHECK(r.fb.Has("2 --"));
}

TEST(ui_replacing_an_effect_needs_confirmation)
{
    Rig r;
    r.Click(); // slot 1 (REVERB): opens on its first parameter
    CHECK(r.gui.CurrentPage().kind == ui::Ui::PAGE_PARAM
          && r.gui.CurrentPage().index == 0);
    r.Turn(-3);
    CHECK(r.gui.CurrentPage().kind == ui::Ui::PAGE_TYPE);
    r.Click();
    r.Turn(1); // DELAY
    r.Click();
    CHECK(r.gui.Confirming());
    CHECK(r.fb.Has("REPLACE?") && r.fb.Has("OLD: REVERB"));
    CHECK(r.s.slot[0].type == fx::FX_REVERB);

    r.Turn(1); // turning backs out of the confirmation (now: EMPTY)
    CHECK(!r.gui.Confirming() && r.gui.Editing());
    r.Turn(-1); // DELAY again
    r.Click();
    CHECK(r.gui.Confirming());
    r.Hold(); // long press cancels and goes home
    CHECK(r.gui.Lvl() == ui::Ui::LVL_HOME);
    CHECK(r.s.slot[0].type == fx::FX_REVERB);

    r.Click();
    CHECK(r.gui.CurrentPage().kind == ui::Ui::PAGE_TYPE); // remembered page
    r.Click();
    r.Turn(1);
    r.Click();
    r.Click(); // confirm
    CHECK(r.s.slot[0].type == fx::FX_DELAY);

    // choosing the current type again changes nothing
    r.Click();
    r.Click();
    CHECK(!r.gui.Editing() && !r.gui.Confirming());
    CHECK(r.s.slot[0].type == fx::FX_DELAY);
}

TEST(ui_long_press_saves_on_home_and_goes_back_elsewhere)
{
    Rig r;
    r.Hold();
    CHECK(r.gui.TakeSaveRequest());
    CHECK(r.gui.Lvl() == ui::Ui::LVL_HOME);

    r.Turn(1);
    r.Click();
    r.Click(); // editing TYPE
    CHECK(r.gui.Editing());
    r.Hold();
    CHECK(r.gui.Lvl() == ui::Ui::LVL_HOME && !r.gui.Editing());
    CHECK(!r.gui.TakeSaveRequest());
    CHECK(r.gui.HomeSel() == 1); // selection kept

    r.Turn(10);
    r.Click(); // SETUP
    CHECK(r.gui.Lvl() == ui::Ui::LVL_SETUP);
    r.Hold();
    CHECK(r.gui.Lvl() == ui::Ui::LVL_HOME && r.gui.HomeSel() == ui::Ui::kSetupRow);
    CHECK(!r.gui.TakeSaveRequest());

    // SETUP's SAVE page saves too
    r.Click();
    r.Turn(20);
    CHECK(r.gui.CurrentPage().kind == ui::Ui::PAGE_SAVE);
    r.Click();
    CHECK(r.gui.TakeSaveRequest());
}

TEST(ui_edits_reach_the_open_slot)
{
    Rig r;
    r.SetType(1, fx::FX_DELAY);
    const app::SlotState before0 = r.s.slot[0];
    const float          time0   = r.s.slot[1].norm[0];

    r.Turn(-10);
    r.Turn(1);
    r.Click(); // slot 2: reopens where SetType left it (TYPE)
    r.Turn(3); // first parameter, TIME
    CHECK(r.fb.Has("TIME"));
    CHECK(r.fb.Has("2")); // slot badge
    r.Click();
    r.Turn(5);
    CHECK(r.s.slot[1].norm[0] > time0);
    CHECK(std::memcmp(&before0, &r.s.slot[0], sizeof(before0)) == 0);
    int slot, param;
    CHECK(r.gui.TakeEditedParam(slot, param) && slot == 1 && param == 0);
    CHECK(!r.gui.TakeEditedParam(slot, param));

    // bypass + mode of that slot
    r.Click();
    r.Turn(-2); // BYPASS
    r.Click();
    r.Turn(1);
    r.Click();
    CHECK(r.s.slot[1].Bypassed() && !r.s.slot[0].Bypassed());
    r.Turn(1); // BYP MODE
    r.Click();
    r.Turn(1);
    CHECK(r.s.slot[1].Hard());
    r.Hold();
    CHECK(r.fb.Has("2 delay"));
}

TEST(ui_routes_reach_every_slot)
{
    Rig r;
    r.SetType(1, fx::FX_DELAY); // REVERB (4 params) + DELAY (6 params)
    r.Turn(10);
    r.Click(); // SETUP, first page KNOB 1
    CHECK(r.gui.CurrentPage().kind == ui::Ui::PAGE_ROUTE
          && r.gui.CurrentPage().index == app::SRC_KNOB_1);
    CHECK(r.fb.Has("1:DECAY"));
    r.Click();
    // none + 4 + 6 destinations: a full turn comes back to the start
    bool saw_other_slot = false, saw_none = false;
    for(int i = 0; i < 11; i++)
    {
        r.Turn(1);
        CHECK(app::Valid(r.s));
        saw_other_slot |= r.fb.Has("2:FEEDBK");
        saw_none |= r.fb.Has("NONE");
    }
    CHECK(saw_other_slot && saw_none);
    CHECK(r.s.route[app::SRC_KNOB_1].Is(0, 0));
    CHECK(r.gui.TakeRouteChanged());

    // route knob 1 to slot 2 TIME: 1:DECAY -> 4 reverb params -> 2:TIME
    r.Turn(4);
    CHECK(r.s.route[app::SRC_KNOB_1].Is(1, 0));
    CHECK(r.fb.Has("2:TIME"));
    r.Turn(-5);
    CHECK(r.s.route[app::SRC_KNOB_1].None());

    // a routed parameter shows its source tag
    r.Turn(5);
    r.Hold();
    r.Turn(-10);
    r.Turn(1);
    r.Click();
    r.Turn(3); // TIME
    CHECKF(r.fb.Has("K1"), "%s", r.fb.All().c_str());
}

TEST(ui_out_level_in_half_db_steps)
{
    Rig r;
    r.Turn(10);
    r.Click();
    r.Turn(5); // KNOB 1 .. CV 2, INPUT, OUT
    CHECK(r.gui.CurrentPage().kind == ui::Ui::PAGE_OUT);
    CHECK(r.fb.Has("0.0dB"));
    r.Click();
    r.Turn(-6);
    CHECKF(std::fabs(r.s.out_gain - 0.7079f) < 0.001f, "gain %f", r.s.out_gain);
    CHECKF(r.fb.Has("-3.0dB"), "%s", r.fb.All().c_str());
    r.Turn(6);
    CHECK(r.s.out_gain == 1.f); // exactly unity again (the audio skips the multiply)
    r.Turn(100);
    CHECK(r.fb.Has("+6.0dB"));
    r.Turn(-200);
    CHECK(r.fb.Has("-24.0dB"));
    CHECK(app::Valid(r.s));
}

TEST(ui_limiter_pages)
{
    Rig r;
    r.Turn(10);
    r.Click();
    r.Turn(6); // KNOB 1 .. CV 2, INPUT, OUT, LIMIT
    CHECK(r.gui.CurrentPage().kind == ui::Ui::PAGE_LIMIT);
    CHECKF(r.fb.Has("-1.0dB") && r.fb.Has("GR 0.0 DB"), "%s", r.fb.All().c_str());
    r.Click();
    r.Turn(-4);
    CHECK(r.s.limit_db == -3.f);
    r.Turn(-100);
    CHECK(r.s.limit_db == app::kLimitMinDb);
    r.Turn(100);
    CHECK(r.s.limit_db == app::kLimitMaxDb);
    CHECK(r.fb.Has("0.0dB"));
    r.Click();

    // live gain reduction, with an ON tag while limiting
    r.gui.SetLimiterGr(-3.44f);
    r.Draw();
    CHECKF(r.fb.Has("GR -3.4 DB") && r.fb.Has("ON"), "%s", r.fb.All().c_str());
    r.gui.SetLimiterGr(0.f);
    r.Draw();
    CHECK(!r.fb.Has("ON"));

    r.Turn(1); // LIM REL
    CHECK(r.gui.CurrentPage().kind == ui::Ui::PAGE_RELEASE);
    CHECK(r.fb.Has("150ms"));
    r.Click();
    r.Turn(-100);
    CHECK(std::fabs(r.s.limit_rel - app::kReleaseMinSec) < 1e-6f && r.fb.Has("20ms"));
    r.Turn(100);
    CHECK(std::fabs(r.s.limit_rel - app::kReleaseMaxSec) < 1e-6f && r.fb.Has("1.00s"));
    CHECK(app::Valid(r.s));
}

TEST(ui_peek_shows_any_slot)
{
    Rig r;
    r.SetType(1, fx::FX_DELAY);
    r.gui.Peek(1, 1, r.now); // knob moved slot 2 FEEDBK
    r.Draw();
    CHECK(r.fb.Has("FEEDBK") && r.fb.Has("2"));
    r.Turn(1); // turning cancels it
    CHECK(!r.fb.Has("FEEDBK"));
    r.gui.Peek(1, 1, r.now);
    r.now += 2000; // and it times out
    r.Draw();
    CHECK(!r.fb.Has("FEEDBK"));
}

TEST(ui_survives_type_changes_underneath)
{
    Rig r;
    r.SetType(1, fx::FX_DELAY);
    r.Turn(-10);
    r.Turn(1);
    r.Click();
    r.Turn(20); // last DELAY page: HI CUT
    r.Click();  // editing
    // something else (a preset load) puts a 4-param REVERB there, then
    // empties the slot
    app::SetSlotType(r.s, r.recall, 1, fx::FX_REVERB);
    r.Draw();
    CHECK(r.gui.CurrentPage().kind == ui::Ui::PAGE_PARAM
          && r.gui.CurrentPage().index == 3);
    CHECK(!r.gui.Editing());
    app::SetSlotType(r.s, r.recall, 1, fx::FX_NONE);
    r.Turn(1);
    CHECK(r.gui.CurrentPage().kind == ui::Ui::PAGE_TYPE);
    // a peek at a slot that has since been emptied is ignored
    r.gui.Peek(1, 3, r.now);
    r.Draw();
    CHECK(r.fb.Has("TYPE"));
}

TEST(ui_remembers_selection_and_pages)
{
    Rig r;
    r.SetType(1, fx::FX_DELAY);
    r.Turn(-10);
    r.Turn(1);
    CHECK(r.s.ui_slot == 1); // saved with the settings
    r.Click();
    r.Turn(5); // TYPE -> 3rd DELAY param
    r.Hold();
    r.Click();
    CHECK(r.gui.CurrentPage().kind == ui::Ui::PAGE_PARAM
          && r.gui.CurrentPage().index == 2);

    ui::Ui fresh; // power-up: HOME selection comes back from the settings
    fresh.Init(&r.fb, &r.s);
    CHECK(fresh.HomeSel() == 1);
}

TEST(ui_every_screen_fits_the_display)
{
    // Long names and many routes on one parameter, every page of every
    // level, editing and not
    Rig r;
    r.SetType(1, fx::FX_DELAY);
    r.SetType(2, fx::FX_DELAY);
    r.SetType(3, fx::FX_REVERB);
    for(int s = 0; s < app::SRC_LAST; s++)
        r.s.route[s] = {1, 1}; // all four on 2:FEEDBK
    r.s.slot[2].flags = app::SLOT_BYPASS | app::SLOT_HARD;

    for(int item = 0; item <= ui::Ui::kSetupRow; item++)
    {
        r.GoHome();
        r.Turn(-10);
        r.Turn(item);
        r.CheckLayout("home");
        r.Click();
        r.Turn(-30);
        for(int p = 0; p < 20; p++)
        {
            char where[48];
            snprintf(where, sizeof(where), "row %d page %d", item, p);
            r.CheckLayout(where);
            const ui::Ui::PageKind k = r.gui.CurrentPage().kind;
            // edit mode too (not where a click acts: SAVE, CPU, SD)
            if(k != ui::Ui::PAGE_SAVE && k != ui::Ui::PAGE_CPU
               && k != ui::Ui::PAGE_SD_LOAD && k != ui::Ui::PAGE_SD_SAVE)
            {
                r.Click();
                r.CheckLayout(where);
                if(k == ui::Ui::PAGE_TYPE)
                {
                    r.Turn(1);
                    r.Click(); // REPLACE? screen
                    r.CheckLayout(where);
                    r.Turn(-1);
                }
                r.Click();
                if(r.gui.Editing())
                    r.Click();
            }
            r.gui.Peek(1, 1, r.now); // a peek with four source tags
            r.Draw();
            r.CheckLayout("peek");
            r.Turn(1);
        }
    }
    // the CPU page's overload state and an SD error line
    r.gui.SetCpuLoad(0.9f, 0.99f);
    r.gui.SetSdSlotInfo(1, false, "SD ERROR", "F3 E8 1bit");
    r.GoHome();
    r.CheckLayout("home cpu");
    r.Turn(10);
    r.Click();
    for(int p = 0; p < 12; p++)
    {
        r.Turn(1);
        r.CheckLayout("setup cpu/sd");
    }
}
