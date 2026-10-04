// 14CFW - Windows 10 OOBE style installer for Atmosphere + Hekate + homebrew
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include "installer.hpp"
#include "logo.hpp"
#ifdef __SWITCH__
#include <switch.h>
#endif

static const int W = 1280, H = 720, MX = 96;

// ------------------------------------------------------------------ input
enum { K_A = 1, K_B = 2, K_X = 4, K_Y = 8, K_UP = 16, K_DOWN = 32, K_LEFT = 64, K_RIGHT = 128, K_PLUS = 256 };
struct In {
    unsigned down = 0;
    bool tap = false;
    int tx = 0, ty = 0;
};

// ------------------------------------------------------------------ globals
static SDL_Window* g_win = nullptr;
static SDL_Renderer* g_r = nullptr;
static TTF_Font *fS, *fB, *fM, *fL, *fXL;
static SDL_Texture *texBG, *texBig, *texIcon, *texCircle;
static float g_alpha = 1.f;  // page fade
static int g_dx = 0;         // page slide
static bool g_quit = false, g_reboot = false;

static const SDL_Color WHITE{255, 255, 255, 255}, ACCENT{0, 120, 215, 255}, DIM{198, 214, 236, 255}, GOOD{150, 232, 170, 255},
    WARN{255, 208, 110, 255}, BAD{255, 150, 140, 255};

// ------------------------------------------------------------------ drawing helpers
struct TT {
    SDL_Texture* t;
    int w, h;
};
static std::map<std::string, TT> g_tc;
static TT getText(TTF_Font* f, const std::string& s) {
    char key[32];
    snprintf(key, sizeof key, "%p|", (void*)f);
    std::string k = key + s;
    auto it = g_tc.find(k);
    if (it != g_tc.end()) return it->second;
    if (g_tc.size() > 300) {
        for (auto& p : g_tc) SDL_DestroyTexture(p.second.t);
        g_tc.clear();
    }
    TT t{nullptr, 0, 0};
    SDL_Surface* sf = TTF_RenderUTF8_Blended(f, s.c_str(), WHITE);
    if (sf) {
        t.t = SDL_CreateTextureFromSurface(g_r, sf);
        t.w = sf->w;
        t.h = sf->h;
        SDL_SetTextureBlendMode(t.t, SDL_BLENDMODE_BLEND);
        SDL_FreeSurface(sf);
    }
    g_tc[k] = t;
    return t;
}
static int textW(TTF_Font* f, const std::string& s) {
    int w = 0, h = 0;
    TTF_SizeUTF8(f, s.c_str(), &w, &h);
    return w;
}
// align: 0 left, 1 center, 2 right. returns width
static int text(TTF_Font* f, const std::string& s, int x, int y, SDL_Color c, int a = 255, int align = 0, bool slide = true) {
    if (s.empty()) return 0;
    TT t = getText(f, s);
    if (!t.t) return 0;
    int px = align == 0 ? x : align == 1 ? x - t.w / 2 : x - t.w;
    if (slide) px += g_dx;
    SDL_SetTextureColorMod(t.t, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(t.t, (Uint8)(a * g_alpha));
    SDL_Rect d{px, y, t.w, t.h};
    SDL_RenderCopy(g_r, t.t, nullptr, &d);
    return t.w;
}
static int wrap(TTF_Font* f, const std::string& s, int x, int y, int maxw, SDL_Color c, int lh, int a = 255) {
    std::string line, word;
    auto flush = [&]() {
        text(f, line, x, y, c, a);
        y += lh;
        line.clear();
    };
    for (size_t i = 0; i <= s.size(); i++) {
        if (i == s.size() || s[i] == ' ') {
            std::string t = line.empty() ? word : line + " " + word;
            if (!line.empty() && textW(f, t) > maxw) {
                flush();
                line = word;
            } else
                line = t;
            word.clear();
        } else
            word += s[i];
    }
    if (!line.empty()) flush();
    return y;
}
static void fillRect(int x, int y, int w, int h, SDL_Color c, int a = 255, bool slide = true) {
    SDL_SetRenderDrawBlendMode(g_r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g_r, c.r, c.g, c.b, (Uint8)(a * g_alpha));
    SDL_Rect r{x + (slide ? g_dx : 0), y, w, h};
    SDL_RenderFillRect(g_r, &r);
}
static void strokeRect(int x, int y, int w, int h, int t, SDL_Color c, int a = 255) {
    fillRect(x, y, w, t, c, a);
    fillRect(x, y + h - t, w, t, c, a);
    fillRect(x, y, t, h, c, a);
    fillRect(x + w - t, y, t, h, c, a);
}
static void circle(float cx, float cy, float r, SDL_Color c, int a = 255, bool slide = true) {
    SDL_SetTextureColorMod(texCircle, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(texCircle, (Uint8)(a * g_alpha));
    SDL_Rect d{(int)(cx - r + (slide ? g_dx : 0)), (int)(cy - r), (int)(2 * r), (int)(2 * r)};
    SDL_RenderCopy(g_r, texCircle, nullptr, &d);
}
static void chip(const std::string& l, int x, int y) {  // button glyph: ring + letter
    circle(x + 11, y + 11, 11, WHITE, 255, false);
    circle(x + 11, y + 11, 9, SDL_Color{12, 52, 104, 255}, 255, false);
    text(fS, l, x + 11, y + 1, WHITE, 255, 1, false);
}
static void toggle(int x, int y, bool on, bool dis) {  // pill 52x26
    int a = dis ? 120 : 255;
    if (on) {
        circle(x + 13, y + 13, 13, ACCENT, a);
        circle(x + 39, y + 13, 13, ACCENT, a);
        fillRect(x + 13, y, 26, 26, ACCENT, a);
        circle(x + 39, y + 13, 7, WHITE, a);
    } else {
        circle(x + 13, y + 13, 13, WHITE, a);
        circle(x + 39, y + 13, 13, WHITE, a);
        fillRect(x + 13, y, 26, 26, WHITE, a);
        circle(x + 13, y + 13, 11, SDL_Color{10, 48, 98, 255}, a);
        circle(x + 39, y + 13, 11, SDL_Color{10, 48, 98, 255}, a);
        fillRect(x + 13, y + 2, 26, 22, SDL_Color{10, 48, 98, 255}, a);
        circle(x + 13, y + 13, 6, WHITE, a);
    }
}
static void spinner(float cx, float cy, float r, double t) {  // Windows-style five racing dots
    for (int i = 0; i < 5; i++) {
        double u = fmod(t / 2.4 - i * 0.075, 1.0);
        if (u < 0) u += 1;
        double e = u * u * (3 - 2 * u);
        double ang = -M_PI / 2 + e * 2 * M_PI * 1.0;
        double al = u < 0.04 ? u / 0.04 : (u > 0.96 ? (1 - u) / 0.04 : 1);
        circle((float)(cx + cos(ang) * r), (float)(cy + sin(ang) * r), r * 0.115f, WHITE, (int)(255 * al));
    }
}

// ------------------------------------------------------------------ state
enum Page { P_HELLO, P_SCAN, P_FOUND, P_MODE, P_CORE, P_APPS, P_OPTS, P_INSTALL, P_DONE, P_ERROR };
enum Act { A_NONE, A_START, A_NEXT, A_BACK, A_EXPRESS, A_CUSTOM, A_INSTALL, A_EXIT, A_REBOOT, A_RETRY };

struct Row {
    std::string title, sub, tag;
    SDL_Color tagCol = WHITE;
    bool* val = nullptr;  // toggle
    bool dis = false;
};
struct Btn {
    std::string label;
    Act act;
    bool primary;
    SDL_Rect r;
};

static Page g_page = P_HELLO;
static std::vector<Page> g_stack;
static double g_pageT = 0, g_now = 0;
static int g_focus = 0, g_scroll = 0;  // focus: row index, or rows.size()+btn index
static Env g_env;
static Plan g_plan;
static bool g_ams = true, g_hek = true, g_menu = true, g_replace = true, g_backup = true;
static std::vector<char> g_appOn;
static double g_scanStart = 0;
static Progress* g_pr = nullptr;
static std::thread g_th;
static bool g_installing = false;
static std::vector<Row> g_rows;
static std::vector<Btn> g_btns;
static std::vector<SDL_Rect> g_rowRects;
static bool g_confirmCancel = false;

static void goPage(Page p, bool push = true) {
    if (push) g_stack.push_back(g_page);
    g_page = p;
    g_pageT = g_now;
    g_focus = 0;
    g_scroll = 0;
    if (p == P_SCAN) {
        g_scanStart = g_now;
        g_env = detectEnv();
        if (g_appOn.size() != hbApps().size()) {
            g_appOn.assign(hbApps().size(), 1);
        }
    }
}
static void goBack() {
    if (g_stack.empty()) return;
    Page p = g_stack.back();
    g_stack.pop_back();
    if (p == P_SCAN) {  // never land on the spinner
        if (g_stack.empty()) return;
        p = g_stack.back();
        g_stack.pop_back();
    }
    goPage(p, false);
}

static void startInstall(bool express) {
    g_plan = Plan();
    if (express) {
        g_plan.ams = g_plan.hekate = g_plan.bootMenu = true;
        g_plan.replaceIni = g_plan.backup = true;
        for (auto& a : hbApps()) g_plan.apps.push_back(a.id);
    } else {
        g_plan.ams = g_ams;
        g_plan.hekate = g_hek;
        g_plan.bootMenu = g_menu;
        g_plan.replaceIni = g_replace;
        g_plan.backup = g_backup;
        for (size_t i = 0; i < hbApps().size(); i++)
            if (g_appOn[i]) g_plan.apps.push_back(hbApps()[i].id);
    }
    if (g_th.joinable()) g_th.join();
    delete g_pr;
    g_pr = new Progress();
    g_installing = true;
    Plan p = g_plan;
    Env e = g_env;
    g_th = std::thread([p, e]() { runInstall(p, e, *g_pr); });
    g_stack.clear();
    goPage(P_INSTALL, false);
}

// ------------------------------------------------------------------ page content
static std::string title, sub;
static void buildPage() {
    g_rows.clear();
    g_btns.clear();
    title = sub = "";
    int by = 600, bh = 48;
    auto addBtn = [&](const std::string& l, Act a, bool primary) { g_btns.push_back({l, a, primary, {0, by, std::max(150, textW(fB, l) + 56), bh}}); };
    switch (g_page) {
    case P_HELLO:
        title = "Hi there";
        sub = "Let's get your Switch set up with Atmosphère, Hekate and your favorite homebrew.";
        addBtn("Let's go", A_START, true);
        break;
    case P_SCAN:
        title = "Just a moment";
        sub = "We're taking a look at your Switch. Nothing is changed during this step.";
        break;
    case P_FOUND: {
        title = "Here's what we found";
        sub = "Your games and saves are never touched by the installer.";
        char t[96];
        Row r;
        r.title = "SD card";
        snprintf(t, sizeof t, "%s free of %s", fmtBytes(g_env.sdFree).c_str(), fmtBytes(g_env.sdTotal).c_str());
        r.tag = g_env.sdOk ? t : "Can't be read";
        r.tagCol = g_env.sdOk && g_env.sdFree > 300ull * 1048576 ? GOOD : BAD;
        g_rows.push_back(r);
        r = Row();
        r.title = "Wi-Fi";
        r.tag = g_env.online ? "Connected" : "Not connected";
        r.tagCol = g_env.online ? GOOD : WARN;
        g_rows.push_back(r);
        r = Row();
        r.title = "Atmosphère";
        r.tag = g_env.amsRunning ? "Running " + g_env.amsVer : g_env.amsFiles ? "Files found" : "Not installed";
        r.tagCol = g_env.amsRunning || g_env.amsFiles ? GOOD : DIM;
        g_rows.push_back(r);
        r = Row();
        r.title = "Hekate";
        r.tag = g_env.hekate ? "Found" : "Not installed";
        r.tagCol = g_env.hekate ? GOOD : DIM;
        g_rows.push_back(r);
        r = Row();
        r.title = "emuMMC";
        r.tag = g_env.emummc ? "Found - will be left alone" : "Not found";
        r.tagCol = g_env.emummc ? GOOD : DIM;
        g_rows.push_back(r);
        r = Row();
        r.title = "Your games";
        r.tag = g_env.nintendo ? "Safe - never touched" : "Safe";
        r.tagCol = GOOD;
        g_rows.push_back(r);
        int n = 0;
        for (auto& a : hbApps()) n += appPresent(g_env, a);
        r = Row();
        r.title = "Pack apps already here";
        snprintf(t, sizeof t, "%d of %d", n, (int)hbApps().size());
        r.tag = t;
        g_rows.push_back(r);
        addBtn("Next", A_NEXT, true);
        break;
    }
    case P_MODE:
        title = "Get going fast";
        sub = "Express installs Atmosphère, Hekate, the 14CFW boot menu and the homebrew pack. Your games, saves and settings stay exactly as they are.";
        if (!g_env.online) {
            Row r;
            r.title = "Your Switch isn't online";
            r.sub = "Connect to Wi-Fi in System Settings. The installer downloads everything fresh.";
            r.dis = true;
            r.tag = "!";
            r.tagCol = WARN;
            g_rows.push_back(r);
        }
        addBtn("Use Express settings", A_EXPRESS, true);
        addBtn("Customize", A_CUSTOM, false);
        break;
    case P_CORE: {
        title = "Customize your install";
        sub = "Choose what goes on your SD card.";
        Row r;
        r.title = "Atmosphère";
        r.sub = g_env.amsFiles ? "Custom firmware. Already here: will update, settings kept." : "Custom firmware for your Switch.";
        r.val = &g_ams;
        g_rows.push_back(r);
        r = Row();
        r.title = "Hekate";
        r.sub = g_env.hekate ? "Bootloader. Already here: will update." : "The bootloader that shows your boot menu.";
        r.val = &g_hek;
        g_rows.push_back(r);
        r = Row();
        r.title = "14CFW boot menu";
        r.sub = (!g_hek && !g_env.hekate) ? "Needs Hekate. Turn Hekate on first." : "Your logo, icons and Atmosphère / emuMMC / sysMMC / Stock entries.";
        r.val = &g_menu;
        r.dis = !g_hek && !g_env.hekate;
        g_rows.push_back(r);
        addBtn("Back", A_BACK, false);
        addBtn("Next", A_NEXT, true);
        break;
    }
    case P_APPS:
        title = "Homebrew pack";
        sub = "Pick the apps you want. Ones you already have get updated.";
        for (size_t i = 0; i < hbApps().size(); i++) {
            Row r;
            r.title = hbApps()[i].name;
            r.sub = std::string(hbApps()[i].desc) + (appPresent(g_env, hbApps()[i]) ? "  -  already installed" : "");
            r.val = (bool*)nullptr;
            g_rows.push_back(r);
        }
        addBtn("Back", A_BACK, false);
        addBtn("Next", A_NEXT, true);
        break;
    case P_OPTS: {
        title = "A few more choices";
        sub = "Almost there.";
        Row r;
        r.title = "Replace Hekate's boot menu";
        r.sub = g_replace ? "14CFW becomes your boot menu. Your old one is backed up." : "Off: 14CFW is added under Hekate's More configs instead.";
        r.val = &g_replace;
        g_rows.push_back(r);
        r = Row();
        r.title = "Back up files I replace";
        r.sub = "Copies go to switch/14CFW/backup on your SD card.";
        r.val = &g_backup;
        g_rows.push_back(r);
        r = Row();
        r.title = "Keep my games, saves and settings";
        r.sub = "Always on. Nintendo, emuMMC and your keys are never written to.";
        static bool always = true;
        r.val = &always;
        r.dis = true;
        g_rows.push_back(r);
        addBtn("Back", A_BACK, false);
        addBtn("Install", A_INSTALL, true);
        break;
    }
    case P_INSTALL:
        title = "Setting things up";
        sub = "This might take a few minutes. Don't turn off your Switch.";
        break;
    case P_DONE:
        title = "All done!";
        sub = "Restart, then pick Atmosphère from the 14CFW boot menu.";
        addBtn("Back to menu", A_EXIT, false);
        addBtn("Restart", A_REBOOT, true);
        break;
    case P_ERROR:
        title = "Something went wrong";
        addBtn("Exit", A_EXIT, false);
        addBtn("Try again", A_RETRY, true);
        break;
    }
    // right-align buttons
    int x = W - MX;
    for (int i = (int)g_btns.size() - 1; i >= 0; i--) {
        x -= g_btns[i].r.w;
        g_btns[i].r.x = x;
        x -= 12;
    }
}

static bool rowOn(int i) {
    if (g_page == P_APPS) return g_appOn[i];
    return g_rows[i].val && *g_rows[i].val;
}
static bool rowIsToggle(int i) { return g_page == P_APPS || g_rows[i].val; }
static void rowFlip(int i) {
    if (g_rows[i].dis) return;
    if (g_page == P_APPS) g_appOn[i] = !g_appOn[i];
    else if (g_rows[i].val) *g_rows[i].val = !*g_rows[i].val;
}

static void doAct(Act a) {
    switch (a) {
    case A_START: goPage(P_SCAN); break;
    case A_NEXT:
        if (g_page == P_FOUND) goPage(P_MODE);
        else if (g_page == P_CORE) goPage(P_APPS);
        else if (g_page == P_APPS) goPage(P_OPTS);
        break;
    case A_BACK: goBack(); break;
    case A_EXPRESS: startInstall(true); break;
    case A_CUSTOM: goPage(P_CORE); break;
    case A_INSTALL: startInstall(false); break;
    case A_EXIT: g_quit = true; break;
    case A_REBOOT: g_reboot = true; g_quit = true; break;
    case A_RETRY: g_stack.clear(); goPage(P_FOUND, false); break;
    default: break;
    }
}

static void handleInput(const In& in) {
    if (g_page == P_SCAN) return;
    if (g_page == P_INSTALL) {
        if (in.down & K_B) g_confirmCancel = !g_confirmCancel;
        if (g_confirmCancel && (in.down & K_A) && g_pr) {
            g_pr->cancel = true;
            g_confirmCancel = false;
        }
        return;
    }
    buildPage();
    int nr = (int)g_rows.size(), nb = (int)g_btns.size();
    bool listPage = (g_page == P_CORE || g_page == P_APPS || g_page == P_OPTS);
    if (in.tap) {
        for (int i = 0; i < nb; i++) {
            SDL_Rect r = g_btns[i].r;
            if (in.tx >= r.x && in.tx < r.x + r.w && in.ty >= r.y && in.ty < r.y + r.h) return doAct(g_btns[i].act);
        }
        if (listPage)
            for (int i = 0; i < (int)g_rowRects.size() && i < nr; i++) {
                SDL_Rect r = g_rowRects[i];
                if (in.tx >= r.x && in.tx < r.x + r.w && in.ty >= r.y && in.ty < r.y + r.h && r.h > 0) {
                    g_focus = i;
                    rowFlip(i);
                    return;
                }
            }
    }
    unsigned d = in.down;
    if (d & K_B) {
        if (g_page == P_HELLO) g_quit = true;
        else if (g_page == P_DONE || g_page == P_ERROR) g_quit = true;
        else if (g_page == P_FOUND) goBack();
        else goBack();
        return;
    }
    if (listPage) {
        int total = nr + nb;
        if (d & K_DOWN) g_focus = std::min(total - 1, g_focus + 1);
        if (d & K_UP) g_focus = std::max(0, g_focus - 1);
        if (g_focus >= nr) {
            if (d & K_LEFT) g_focus = std::max(nr, g_focus - 1);
            if (d & K_RIGHT) g_focus = std::min(total - 1, g_focus + 1);
        }
        if (d & K_PLUS) { for (auto& b : g_btns) if (b.primary) return doAct(b.act); }
        if (d & K_A) {
            if (g_focus < nr) rowFlip(g_focus);
            else return doAct(g_btns[g_focus - nr].act);
        }
        return;
    }
    if (d & K_X) { for (auto& b : g_btns) if (!b.primary && b.act != A_BACK) return doAct(b.act); }
    if (d & (K_A | K_PLUS)) { for (auto& b : g_btns) if (b.primary) return doAct(b.act); }
}

// ------------------------------------------------------------------ frame
static void drawRow(int i, int x, int y, int w, int h, bool focus) {
    const Row& r = g_rows[i];
    fillRect(x, y, w, h, WHITE, focus ? 48 : 26);
    if (focus) strokeRect(x, y, w, h, 2, WHITE, 230);
    bool tg = rowIsToggle(i);
    int tx = x + 20;
    if (r.sub.empty()) text(fB, r.title, tx, y + (h - 30) / 2, WHITE, r.dis ? 160 : 255);
    else {
        text(fB, r.title, tx, y + 8, WHITE, r.dis ? 170 : 255);
        text(fS, r.sub, tx, y + 8 + 31, DIM, r.dis ? 150 : 255);
    }
    if (tg) toggle(x + w - 20 - 52, y + (h - 26) / 2, rowOn(i), r.dis);
    else if (!r.tag.empty()) {
        int tw = textW(fS, r.tag);
        text(fS, r.tag, x + w - 20, y + (h - 24) / 2, r.tagCol, 255, 2);
        circle(x + w - 20 - tw - 16, y + h / 2, 5, r.tagCol);
    }
}

static void drawStatus() {
    int y = 676;
    int x = MX;
    struct H2 { const char *k, *l; };
    std::vector<H2> hs;
    if (g_page == P_INSTALL) hs = {{"B", "Cancel"}};
    else if (g_page == P_HELLO) hs = {{"A", "Start"}, {"B", "Exit"}};
    else if (g_page == P_SCAN) hs = {};
    else if (g_page == P_CORE || g_page == P_APPS || g_page == P_OPTS) hs = {{"A", "Select"}, {"B", "Back"}, {"+", "Next"}};
    else if (g_page == P_MODE) hs = {{"A", "Express"}, {"X", "Customize"}, {"B", "Back"}};
    else if (g_page == P_DONE || g_page == P_ERROR) hs = {{"A", "Continue"}, {"B", "Exit"}};
    else hs = {{"A", "Next"}, {"B", "Back"}};
    for (auto& h : hs) {
        chip(h.k, x, y);
        x += 30;
        x += text(fS, h.l, x, y - 1, DIM, 255, 0, false) + 28;
    }
    int bat = 87;
#ifdef __SWITCH__
    u32 b = 0;
    if (R_SUCCEEDED(psmGetBatteryChargePercentage(&b))) bat = (int)b;
#endif
    char t[64];
    snprintf(t, sizeof t, "%d%%", bat);
    int rx = W - MX;
    int tw = text(fS, t, rx, y - 1, DIM, 255, 2, false);
    int bx = rx - tw - 34;
    strokeRect(bx, y + 4, 24, 12, 2, DIM, 255);
    fillRect(bx + 24, y + 7, 3, 6, DIM, 255, false);
    fillRect(bx + 3, y + 7, std::max(1, (int)(18 * bat / 100.0)), 6, DIM, 255, false);
    if (g_env.sdOk && g_page != P_HELLO) {
        std::string s = fmtBytes(g_env.sdFree) + " free";
        text(fS, s, bx - 26, y - 1, DIM, 255, 2, false);
    }
}

static void drawFrame(double now) {
    g_now = now;
    // poll install
    if (g_installing && g_pr && g_pr->done) {
        if (g_th.joinable()) g_th.join();
        g_installing = false;
        goPage(g_pr->failed ? P_ERROR : P_DONE, false);
        if (g_pr->failed) g_env = detectEnv();
    }
    if (g_page == P_SCAN && now - g_scanStart > 2.0) goPage(P_FOUND, false);

    double ft = std::min(1.0, (now - g_pageT) / 0.28);
    double e = 1 - pow(1 - ft, 3);
    g_alpha = (float)e;
    g_dx = (int)((1 - e) * 28);

    SDL_SetRenderDrawColor(g_r, 0, 0, 0, 255);
    SDL_RenderClear(g_r);
    SDL_RenderCopy(g_r, texBG, nullptr, nullptr);
    buildPage();

    // hello: big emblem
    if (g_page == P_HELLO) {
        SDL_SetTextureAlphaMod(texBig, (Uint8)(255 * g_alpha));
        SDL_Rect d{W - MX - 278 - 60 + g_dx, 130, 278, 416};
        SDL_RenderCopy(g_r, texBig, nullptr, &d);
    }
    int titleF = (g_page == P_HELLO) ? 1 : 0;
    if (titleF) text(fXL, title, MX, 150, WHITE);
    else text(fL, title, MX, 82, WHITE);
    int subY = titleF ? 280 : 150;
    if (!sub.empty()) wrap(fM, sub, MX, subY, g_page == P_HELLO ? 560 : 900, DIM, 36);

    // content
    g_rowRects.assign(g_rows.size(), SDL_Rect{0, 0, 0, 0});
    int cx = MX, cw = W - 2 * MX;
    if (g_page == P_FOUND || g_page == P_MODE) {
        int rh = g_page == P_FOUND ? 44 : 64, gap = 6, y = g_page == P_FOUND ? 218 : 280;
        if (g_page == P_MODE) y = 300;
        for (size_t i = 0; i < g_rows.size(); i++) {
            drawRow((int)i, cx, y, cw, rh, false);
            y += rh + gap;
        }
        if (g_page == P_MODE && g_env.online) {
            const char* l[] = {"Atmosphère and Hekate (latest)", "14CFW boot menu with your logo", "Homebrew pack: save manager, file manager, FTP and more", "Backups of anything it replaces"};
            int yy = 270;
            (void)l;
            (void)yy;
            int by = 290;
            for (int i = 0; i < 4; i++) {
                circle(cx + 6, by + 14 + i * 38, 5, ACCENT);
                text(fM, l[i], cx + 26, by + i * 38, WHITE);
            }
        }
    } else if (g_page == P_CORE || g_page == P_APPS || g_page == P_OPTS) {
        int rh = 64, pitch = 72, top = 212, vis = 5;
        int nr = (int)g_rows.size();
        if (g_focus < nr) {
            if (g_focus < g_scroll) g_scroll = g_focus;
            if (g_focus >= g_scroll + vis) g_scroll = g_focus - vis + 1;
        }
        SDL_Rect clip{0, top - 4, W, vis * pitch};
        SDL_RenderSetClipRect(g_r, &clip);
        for (int i = g_scroll; i < nr && i < g_scroll + vis; i++) {
            int y = top + (i - g_scroll) * pitch;
            drawRow(i, cx, y, cw, rh, g_focus == i);
            g_rowRects[i] = SDL_Rect{cx, y, cw, rh};
        }
        SDL_RenderSetClipRect(g_r, nullptr);
        if (nr > vis) {
            int th = vis * pitch - 8, bh = th * vis / nr, by = top + (th - bh) * g_scroll / std::max(1, nr - vis);
            fillRect(W - MX + 24, top, 4, th, WHITE, 40, false);
            fillRect(W - MX + 24, by, 4, bh, WHITE, 200, false);
        }
    } else if (g_page == P_SCAN) {
        spinner(MX + 60, 340, 44, now);
        text(fM, "Checking your SD card and apps...", MX + 140, 322, WHITE);
    } else if (g_page == P_INSTALL && g_pr) {
        std::string st, dt;
        std::vector<std::string> lg;
        {
            std::lock_guard<std::mutex> l(g_pr->m);
            st = g_pr->step;
            dt = g_pr->detail;
            lg = g_pr->log;
        }
        spinner(MX + 60, 300, 44, now);
        text(fB, st.empty() ? "Getting started" : st, MX + 140, 268, WHITE);
        text(fS, dt, MX + 140, 306, DIM);
        int bx = MX, by = 420, bw = cw;
        fillRect(bx, by, bw, 6, WHITE, 40);
        fillRect(bx, by, (int)(bw * g_pr->frac.load()), 6, WHITE, 255);
        char t[16];
        snprintf(t, sizeof t, "%d%%", (int)(g_pr->frac.load() * 100));
        text(fS, t, bx + bw, by + 16, DIM, 255, 2);
        int ly = 470;
        for (int i = std::max(0, (int)lg.size() - 4); i < (int)lg.size(); i++) text(fS, lg[i], bx, ly += 26, DIM);
        if (g_confirmCancel) {
            fillRect(MX, 540, cw, 56, SDL_Color{0, 0, 0, 255}, 110);
            text(fB, "Stop the install?  A = Yes, stop     B = Keep going", MX + 20, 552, WHITE);
        }
    } else if (g_page == P_DONE && g_pr) {
        std::vector<std::string> lg;
        {
            std::lock_guard<std::mutex> l(g_pr->m);
            lg = g_pr->log;
        }
        int y = 230;
        for (auto& s : lg) {
            bool warn = s.rfind("Warning", 0) == 0 || s.find("skipped") != std::string::npos;
            circle(cx + 6, y + 14, 5, warn ? WARN : GOOD);
            text(fM, s, cx + 26, y, WHITE);
            y += 36;
            if (y > 570) break;
        }
    } else if (g_page == P_ERROR && g_pr) {
        std::string er;
        std::vector<std::string> lg;
        {
            std::lock_guard<std::mutex> l(g_pr->m);
            er = g_pr->error;
            lg = g_pr->log;
        }
        int y = wrap(fM, er, MX, 170, 900, WHITE, 36) + 20;
        for (int i = std::max(0, (int)lg.size() - 5); i < (int)lg.size(); i++) y += 0 * text(fS, lg[i], MX, y, DIM) + 26;
    }

    // buttons
    int nr = (int)g_rows.size();
    bool listPage = (g_page == P_CORE || g_page == P_APPS || g_page == P_OPTS);
    for (size_t i = 0; i < g_btns.size(); i++) {
        const Btn& b = g_btns[i];
        bool foc = listPage && g_focus == nr + (int)i;
        if (b.primary) fillRect(b.r.x - g_dx, b.r.y, b.r.w, b.r.h, ACCENT, 255);
        else fillRect(b.r.x - g_dx, b.r.y, b.r.w, b.r.h, WHITE, 40);
        if (foc || (!listPage && b.primary)) strokeRect(b.r.x - g_dx, b.r.y, b.r.w, b.r.h, 2, WHITE, foc ? 255 : 150);
        text(fB, b.label, b.r.x + b.r.w / 2 - g_dx, b.r.y + 9, WHITE, 255, 1);
    }
    drawStatus();
    SDL_RenderPresent(g_r);
}

// ------------------------------------------------------------------ init
static SDL_Texture* mkTex(const Rgba& im) {
    SDL_Texture* t = SDL_CreateTexture(g_r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, im.w, im.h);
    SDL_UpdateTexture(t, nullptr, im.px.data(), im.w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}
static TTF_Font* openFont(int sz);

static bool uiInit() {
    g_win = SDL_CreateWindow("14CFW", 0, 0, W, H, SDL_WINDOW_SHOWN);
    if (!g_win) return false;
    g_r = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_r) g_r = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
    if (!g_r) return false;
    SDL_SetRenderDrawBlendMode(g_r, SDL_BLENDMODE_BLEND);
    TTF_Init();
    fS = openFont(17);
    fB = openFont(22);
    fM = openFont(26);
    fL = openFont(48);
    fXL = openFont(92);
    if (!fS || !fB || !fM || !fL || !fXL) return false;
    // background: deep blue gradient with soft light
    SDL_Surface* bg = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_RGBA32);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float u = (x / (float)W + y / (float)H) / 2;
            float r = 8 + (2 - 8) * u, g = 78 + (30 - 78) * u, b = 150 + (78 - 150) * u;
            float dx = (x - W * 0.82f) / 700.f, dy = (y - H * 0.12f) / 520.f;
            float glow = std::max(0.f, 1 - sqrtf(dx * dx + dy * dy));
            glow = glow * glow * 46;
            float dx2 = (x - W * 0.1f) / 500.f, dy2 = (y - H * 1.0f) / 380.f;
            float g2 = std::max(0.f, 1 - sqrtf(dx2 * dx2 + dy2 * dy2));
            g2 = g2 * g2 * 20;
            Uint8* p = (Uint8*)bg->pixels + y * bg->pitch + x * 4;
            p[0] = (Uint8)std::min(255.f, r + glow * 0.5f + g2 * 0.3f);
            p[1] = (Uint8)std::min(255.f, g + glow * 0.9f + g2 * 0.6f);
            p[2] = (Uint8)std::min(255.f, b + glow + g2);
            p[3] = 255;
        }
    texBG = SDL_CreateTextureFromSurface(g_r, bg);
    SDL_FreeSurface(bg);
    // circle
    SDL_Surface* cs = SDL_CreateRGBSurfaceWithFormat(0, 128, 128, 32, SDL_PIXELFORMAT_RGBA32);
    for (int y = 0; y < 128; y++)
        for (int x = 0; x < 128; x++) {
            float d = sqrtf((x - 63.5f) * (x - 63.5f) + (y - 63.5f) * (y - 63.5f));
            float a = std::max(0.f, std::min(1.f, 64.f - d));
            Uint8* p = (Uint8*)cs->pixels + y * cs->pitch + x * 4;
            p[0] = p[1] = p[2] = 255;
            p[3] = (Uint8)(a * 255);
        }
    texCircle = SDL_CreateTextureFromSurface(g_r, cs);
    SDL_SetTextureBlendMode(texCircle, SDL_BLENDMODE_BLEND);
    SDL_FreeSurface(cs);
    Rgba big, ic;
    if (logoLoad(LOGO_S_BIG, big)) texBig = mkTex(big);
    if (logoLoad(LOGO_S_ICON, ic)) texIcon = mkTex(ic);
    if (!texBig) { Rgba e; e.w = e.h = 1; e.px = {0, 0, 0, 0}; texBig = mkTex(e); }
    g_appOn.assign(hbApps().size(), 1);
    return true;
}

static void uiShutdown() {
    if (g_pr) g_pr->cancel = true;
    if (g_th.joinable()) g_th.join();
    for (auto& p : g_tc) SDL_DestroyTexture(p.second.t);
    g_tc.clear();
    if (texBG) SDL_DestroyTexture(texBG);
    if (texBig) SDL_DestroyTexture(texBig);
    if (texIcon) SDL_DestroyTexture(texIcon);
    if (texCircle) SDL_DestroyTexture(texCircle);
    if (g_r) SDL_DestroyRenderer(g_r);
    if (g_win) SDL_DestroyWindow(g_win);
    TTF_Quit();
}

#ifdef __SWITCH__
static PlFontData g_fd;
static TTF_Font* openFont(int sz) { return TTF_OpenFontRW(SDL_RWFromMem(g_fd.address, g_fd.size), 0, sz); }
#else
static TTF_Font* openFont(int sz) { return TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", sz); }
#endif

#ifndef CFW_NO_MAIN
int main(int, char**) {
#ifdef __SWITCH__
    socketInitializeDefault();
    plInitialize(PlServiceType_User);
    psmInitialize();
    plGetSharedFontByType(&g_fd, PlSharedFontType_Standard);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();
    SDL_Init(SDL_INIT_VIDEO);
#else
    SDL_Init(SDL_INIT_VIDEO);
#endif
    if (!uiInit()) return 1;
    goPage(P_HELLO, false);
    bool wasTouch = false;
#ifdef __SWITCH__
    while (appletMainLoop() && !g_quit) {
        padUpdate(&pad);
        u64 d = padGetButtonsDown(&pad);
        In in;
        if (d & HidNpadButton_A) in.down |= K_A;
        if (d & HidNpadButton_B) in.down |= K_B;
        if (d & HidNpadButton_X) in.down |= K_X;
        if (d & HidNpadButton_Y) in.down |= K_Y;
        if (d & (HidNpadButton_Up | HidNpadButton_StickLUp)) in.down |= K_UP;
        if (d & (HidNpadButton_Down | HidNpadButton_StickLDown)) in.down |= K_DOWN;
        if (d & (HidNpadButton_Left | HidNpadButton_StickLLeft)) in.down |= K_LEFT;
        if (d & (HidNpadButton_Right | HidNpadButton_StickLRight)) in.down |= K_RIGHT;
        if (d & HidNpadButton_Plus) in.down |= K_PLUS;
        HidTouchScreenState ts = {0};
        hidGetTouchScreenStates(&ts, 1);
        if (ts.count > 0 && !wasTouch) {
            in.tap = true;
            in.tx = (int)ts.touches[0].x;
            in.ty = (int)ts.touches[0].y;
        }
        wasTouch = ts.count > 0;
        handleInput(in);
        drawFrame(SDL_GetTicks() / 1000.0);
    }
    uiShutdown();
    if (g_reboot) {
        if (R_SUCCEEDED(spsmInitialize())) {
            spsmShutdown(true);
            spsmExit();
        }
    }
    psmExit();
    plExit();
    socketExit();
#else
    (void)wasTouch;
    uiShutdown();
#endif
    return 0;
}
#endif
