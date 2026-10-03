#include <switch.h>
#include <curl/curl.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <sys/stat.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include "installer.hpp"

static const int W = 1280, H = 720;
// Windows 10 OOBE Orange/Black theme
static const SDL_Color C_BG{20, 20, 20, 255},          // Dark background
    C_PANEL{30, 30, 30, 255},                          // Panel background
    C_BORDER{50, 50, 50, 255},                         // Border color
    C_TEXT{240, 240, 240, 255},                        // Main text (light gray)
    C_DIM{140, 140, 140, 255},                         // Dimmed text
    C_ORG{255, 140, 0, 255},                           // Orange accent
    C_ORG_DARK{200, 100, 0, 255},                      // Darker orange
    C_OK{76, 175, 80, 255},                            // Green for success
    C_ERR{244, 67, 54, 255},                           // Red for error
    C_WHITE{255, 255, 255, 255},                       // White
    C_WARN{255, 152, 0, 255};                          // Amber for warning

static const char* SETTINGS_PATH = "sdmc:/switch/14CFW/settings.txt";

static SDL_Renderer* R;
static TTF_Font *fBody, *fSmall, *fTitle, *fBig;

// ------------------------------------------------------------------ drawing helpers
static void fillRect(int x, int y, int w, int h, SDL_Color c) {
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, c.a);
    SDL_Rect r{x, y, w, h};
    SDL_RenderFillRect(R, &r);
}

static void rrect(int x, int y, int w, int h, int r, SDL_Color c) {
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, c.a);
    for (int i = 0; i < r; i++) {
        double dy = r - i - 0.5;
        int inset = r - (int)std::lround(std::sqrt((double)r * r - dy * dy));
        SDL_Rect a{x + inset, y + i, w - 2 * inset, 1};
        SDL_Rect b{x + inset, y + h - 1 - i, w - 2 * inset, 1};
        SDL_RenderFillRect(R, &a);
        SDL_RenderFillRect(R, &b);
    }
    SDL_Rect m{x, y + r, w, h - 2 * r};
    SDL_RenderFillRect(R, &m);
}

static void circle(int cx, int cy, int r, SDL_Color c) { 
    rrect(cx - r, cy - r, 2 * r, 2 * r, r, c); 
}

static void panel(int x, int y, int w, int h, int r, SDL_Color fill, SDL_Color border) {
    rrect(x, y, w, h, r, border);
    rrect(x + 1, y + 1, w - 2, h - 2, r - 1 > 0 ? r - 1 : 1, fill);
}

static void line(int x1, int y1, int x2, int y2, SDL_Color c, int t) {
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, c.a);
    for (int o = -(t / 2); o <= t / 2; o++) {
        SDL_RenderDrawLine(R, x1 + o, y1, x2 + o, y2);
        SDL_RenderDrawLine(R, x1, y1 + o, x2, y2 + o);
    }
}

// Draw outlined "14" logo with checkmark style
static void drawLogo14(int cx, int cy, int size) {
    int thick = size / 8;
    SDL_Color col = C_ORG;
    
    // "1" - vertical line
    fillRect(cx - size/4, cy - size/3, thick, 2*size/3, col);
    
    // "4" - outlined style
    fillRect(cx + size/8, cy - size/3, thick, size/3, col);  // vertical top
    fillRect(cx + size/8, cy, thick * 2, thick, col);         // horizontal middle
    fillRect(cx + size/8 + thick, cy, thick, size/3, col);    // vertical bottom
    
    // Checkmark accent on side (small)
    int ck_x = cx + size/2 + size/8;
    int ck_y = cy + size/6;
    line(ck_x - thick, ck_y, ck_x + thick/2, ck_y + thick*2, C_ORG, thick);
    line(ck_x + thick/2, ck_y + thick*2, ck_x + thick*3, ck_y - thick, C_ORG, thick);
}

struct CT { SDL_Texture* t; int w, h; };
static std::map<std::string, CT> tcache;
static CT getText(TTF_Font* f, const std::string& s, SDL_Color c) {
    char kb[48];
    snprintf(kb, sizeof kb, "%p|%02x%02x%02x|", (void*)f, c.r, c.g, c.b);
    std::string key = std::string(kb) + s;
    auto it = tcache.find(key);
    if (it != tcache.end()) return it->second;
    if (tcache.size() > 500) {
        for (auto& kv : tcache)
            if (kv.second.t) SDL_DestroyTexture(kv.second.t);
        tcache.clear();
    }
    CT ct{nullptr, 0, 0};
    SDL_Surface* sf = TTF_RenderUTF8_Blended(f, s.c_str(), c);
    if (sf) {
        ct.t = SDL_CreateTextureFromSurface(R, sf);
        ct.w = sf->w;
        ct.h = sf->h;
        SDL_FreeSurface(sf);
    }
    tcache[key] = ct;
    return ct;
}

static int text(TTF_Font* f, const std::string& s, int x, int y, SDL_Color c, int align = 0) {
    if (s.empty() || !f) return 0;
    CT ct = getText(f, s, c);
    if (!ct.t) return 0;
    int dx = align == 1 ? x - ct.w / 2 : (align == 2 ? x - ct.w : x);
    SDL_Rect d{dx, y, ct.w, ct.h};
    SDL_RenderCopy(R, ct.t, nullptr, &d);
    return ct.w;
}

static int textW(TTF_Font* f, const std::string& s) {
    int w = 0, h = 0;
    TTF_SizeUTF8(f, s.c_str(), &w, &h);
    return w;
}

static std::string fit(TTF_Font* f, std::string s, int maxW) {
    if (textW(f, s) <= maxW) return s;
    while (!s.empty()) {
        size_t i = s.size() - 1;
        while (i > 0 && (s[i] & 0xC0) == 0x80) i--;
        s.erase(i);
        if (textW(f, s + "...") <= maxW) return s + "...";
    }
    return "...";
}

static SDL_Color mix(SDL_Color a, SDL_Color b, float t) {
    return SDL_Color{(Uint8)(a.r + (b.r - a.r) * t), (Uint8)(a.g + (b.g - a.g) * t), (Uint8)(a.b + (b.b - a.b) * t), 255};
}

static void progressBar(int x, int y, int w, int h, float v) {
    fillRect(x, y, w, h, C_BORDER);
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    int fw = (int)(w * v);
    if (fw > 0) fillRect(x, y, fw, h, C_ORG);
}

static void checkbox(int x, int y, int s, bool on) {
    fillRect(x, y, s, s, on ? C_BORDER : C_BORDER);
    if (on) {
        line(x + 4, y + s/2, x + s/2 - 2, y + s - 6, C_ORG, 3);
        line(x + s/2 - 2, y + s - 6, x + s - 4, y + 4, C_ORG, 3);
    }
}

static void toggle(int x, int y, bool on) {
    fillRect(x, y, 56, 30, C_BORDER);
    fillRect(on ? x + 28 : x + 2, y + 2, 26, 26, on ? C_ORG : C_DIM);
}

struct Rc { int x = 0, y = 0, w = 0, h = 0; };
static bool hit(const Rc& r, int px, int py) { return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h; }

static void button(const Rc& r, const std::string& label, bool primary) {
    if (primary) {
        fillRect(r.x, r.y, r.w, r.h, C_ORG);
        text(fSmall, label, r.x + r.w / 2, r.y + (r.h - 20) / 2, C_BG, 1);
    } else {
        fillRect(r.x, r.y, r.w, r.h, C_BORDER);
        text(fSmall, label, r.x + r.w / 2, r.y + (r.h - 20) / 2, C_TEXT, 1);
    }
}

// ------------------------------------------------------------------ state
enum Screen { HOME, COMPS, SETTINGS, CONFIRM, RUN, DONE };
enum JobKind { J_NONE, J_INSTALL, J_MENU, J_CHECK };

static Screen screen = HOME;
static bool menuOnly = false;
static std::vector<Component> comps;
static Env env;
static Options opts;
static int homeSel = 0, compSel = 0, compTop = 0, setSel = 0;
static const int ROWS_VISIBLE = 8;

static Progress* prog = nullptr;
static JobKind jobKind = J_NONE;
static Thread worker;
static bool workerValid = false;
struct JobArg {
    JobKind kind;
    std::vector<Component> comps;
    Options o;
    Env env;
    Progress* p;
};
static JobArg* curArg = nullptr;

static std::string toastMsg;
static Uint32 toastUntil = 0;
static void toast(const std::string& s) {
    toastMsg = s;
    toastUntil = SDL_GetTicks() + 2600;
}

static Rc rcHome[5], rcRows[ROWS_VISIBLE], rcBack, rcCheck, rcAll, rcInstall, rcGo, rcCancel, rcSet[5];

static void saveSettings() {
    FILE* f = fopen(SETTINGS_PATH, "w");
    if (!f) return;
    fprintf(f, "backup=%d\nkeep=%d\nbootmenu=%d\nreplace=%d\n", opts.backup, opts.keepConfigs, opts.withBootMenu, opts.replaceMenu);
    fclose(f);
}

static void loadSettings() {
    std::ifstream in(SETTINGS_PATH);
    std::string l;
    while (std::getline(in, l)) {
        size_t e = l.find('=');
        if (e == std::string::npos) continue;
        std::string k = l.substr(0, e);
        bool v = l.substr(e + 1).find('1') != std::string::npos;
        if (k == "backup") opts.backup = v;
        else if (k == "keep") opts.keepConfigs = v;
        else if (k == "bootmenu") opts.withBootMenu = v;
        else if (k == "replace") opts.replaceMenu = v;
    }
}

static void rescan() { inst::scan(comps, env); }

static void jobThread(void* a) {
    JobArg* j = (JobArg*)a;
    if (j->kind == J_INSTALL) inst::install(j->comps, j->o, j->env, *j->p);
    else if (j->kind == J_MENU) inst::installBootMenuOnly(j->o, j->env, *j->p);
    else if (j->kind == J_CHECK) inst::checkUpdates(j->comps, *j->p);
}

static void joinWorker() {
    if (workerValid) {
        threadWaitForExit(&worker);
        threadClose(&worker);
        workerValid = false;
    }
    delete curArg;
    curArg = nullptr;
}

static void startJob(JobKind k) {
    if (jobKind != J_NONE) return;
    joinWorker();
    delete prog;
    prog = new Progress();
    curArg = new JobArg{k, comps, opts, env, prog};
    jobKind = k;
    screen = RUN;
    if (R_FAILED(threadCreate(&worker, jobThread, curArg, nullptr, 0x100000, 0x2B, -2)) || R_FAILED(threadStart(&worker))) {
        prog->say("Could not start worker thread");
        prog->done = true;
        return;
    }
    workerValid = true;
}

static void finishJob() {
    joinWorker();
    JobKind k = jobKind;
    jobKind = J_NONE;
    if (k == J_CHECK) {
        {
            std::lock_guard<std::mutex> g(prog->mu);
            for (auto& kv : prog->latest)
                for (auto& c : comps)
                    if (c.id == kv.first) c.latest = kv.second;
        }
        screen = COMPS;
        toast("Update check finished");
    } else {
        rescan();
        screen = DONE;
    }
}

static int selectedCount() {
    int n = 0;
    for (auto& c : comps)
        if (c.selected) n++;
    return n;
}

// ------------------------------------------------------------------ screens
static void drawHome() {
    fillRect(0, 0, W, H, C_BG);
    
    // Top panel with logo and title
    fillRect(0, 0, W, 120, C_PANEL);
    fillRect(0, 115, W, 2, C_ORG);
    
    drawLogo14(60, 60, 60);
    text(fBig, "14 Custom Firmware", 130, 30, C_TEXT);
    text(fSmall, "Nintendo Switch Installer", 130, 70, C_DIM);
    
    // Status cards in grid
    int cx = 40, cy = 140, cw = 280, ch = 140, gap = 20;
    
    auto drawCard = [&](int x, int y, const std::string& title, const std::string& value, SDL_Color vc) {
        fillRect(x, y, cw, ch, C_PANEL);
        fillRect(x, y, cw, 2, C_ORG);
        text(fSmall, title, x + 16, y + 12, C_DIM);
        text(fBody, fit(fBody, value, cw - 32), x + 16, y + 50, vc);
    };
    
    std::string amsV = env.amsRunning ? "v" + env.amsVer : (env.ams ? "Installed" : "Not found");
    drawCard(cx, cy, "ATMOSPHERE", amsV, env.ams || env.amsRunning ? C_OK : C_TEXT);
    
    std::string hekV = env.hekate ? (env.hekateVer.empty() ? "Installed" : "v" + env.hekateVer) : "Not found";
    drawCard(cx + cw + gap, cy, "HEKATE", hekV, env.hekate ? C_OK : C_TEXT);
    
    std::string sd = env.sdOk ? inst::humanSize(env.freeBytes) + " free" : "Unreadable";
    drawCard(cx, cy + ch + gap, "SD CARD", sd, env.sdOk ? C_OK : C_ERR);
    
    std::string bat = env.battery >= 0 ? std::to_string(env.battery) + "%" : "Unknown";
    bool lowBat = env.battery >= 0 && env.battery < 30 && !env.charging;
    drawCard(cx + cw + gap, cy + ch + gap, "BATTERY", bat, lowBat ? C_ERR : C_OK);
    
    // Right side menu
    int mx = 800, mh = 500;
    text(fBody, "Setup Options", mx, 140, C_ORG);
    
    static const char* items[5] = {"Install Components", "Boot Menu Only", "Settings", "Refresh Status", "Exit"};
    for (int i = 0; i < 5; i++) {
        rcHome[i] = {mx, 180 + i * 90, 420, 75};
        bool sel = i == homeSel;
        fillRect(rcHome[i].x, rcHome[i].y, rcHome[i].w, rcHome[i].h, sel ? C_BORDER : C_PANEL);
        if (sel) fillRect(rcHome[i].x, rcHome[i].y, 4, rcHome[i].h, C_ORG);
        text(fBody, items[i], mx + 16, rcHome[i].y + 20, C_TEXT);
    }
    
    text(fSmall, "Up/Down navigate • A select • + exit", W/2, 690, C_DIM, 1);
}

static void drawComps() {
    fillRect(0, 0, W, H, C_BG);
    
    // Header
    fillRect(0, 0, W, 100, C_PANEL);
    fillRect(0, 95, W, 2, C_ORG);
    drawLogo14(50, 50, 40);
    text(fTitle, "Choose Components to Install", 110, 30, C_TEXT);
    text(fSmall, "Select components, check for updates", 110, 70, C_DIM);
    
    // Component list
    int n = (int)comps.size();
    if (compSel < compTop) compTop = compSel;
    if (compSel >= compTop + ROWS_VISIBLE) compTop = compSel - ROWS_VISIBLE + 1;
    
    for (int r = 0; r < ROWS_VISIBLE; r++) {
        int i = compTop + r;
        rcRows[r] = {0, 0, 0, 0};
        if (i >= n) continue;
        Component& c = comps[i];
        int y = 120 + r * 60;
        rcRows[r] = {40, y, W - 80, 55};
        bool sel = i == compSel;
        
        fillRect(40, y, W - 80, 55, sel ? C_BORDER : C_PANEL);
        if (sel) fillRect(40, y, 3, 55, C_ORG);
        
        checkbox(60, y + 14, 24, c.selected);
        text(fBody, c.name, 100, y + 8, C_TEXT);
        text(fSmall, fit(fSmall, c.desc, 600), 100, y + 32, C_DIM);
        
        if (c.installed) {
            text(fSmall, "✓ Installed", W - 200, y + 16, C_OK);
        }
    }
    
    // Buttons
    rcBack = {40, 640, 140, 44};
    rcCheck = {196, 640, 200, 44};
    rcAll = {412, 640, 160, 44};
    rcInstall = {W - 240, 640, 240, 44};
    
    button(rcBack, "B Back", false);
    button(rcCheck, "X Check Updates", false);
    button(rcAll, "Y Select All", false);
    button(rcInstall, "+ Install (" + std::to_string(selectedCount()) + ")", true);
}

static void drawSettings() {
    fillRect(0, 0, W, H, C_BG);
    
    // Header
    fillRect(0, 0, W, 100, C_PANEL);
    fillRect(0, 95, W, 2, C_ORG);
    drawLogo14(50, 50, 40);
    text(fTitle, "Installation Settings", 110, 30, C_TEXT);
    
    struct Row { const char* t; const char* s; bool on; };
    Row rows[4] = {
        {"Create backups", "Save overwritten files to switch/14CFW/backup", opts.backup},
        {"Keep existing configs", "Never overwrite .ini, .json, .cfg files", opts.keepConfigs},
        {"Install boot menu", "Add 14CFW Hekate menu with custom logo", opts.withBootMenu},
        {"Replace main menu", "Use as primary menu (old menu backed up)", opts.replaceMenu},
    };
    
    for (int i = 0; i < 4; i++) {
        int y = 130 + i * 110;
        rcSet[i] = {40, y, W - 80, 100};
        bool sel = i == setSel;
        
        fillRect(40, y, W - 80, 100, sel ? C_BORDER : C_PANEL);
        if (sel) fillRect(40, y, 3, 100, C_ORG);
        
        text(fBody, rows[i].t, 70, y + 14, C_TEXT);
        text(fSmall, rows[i].s, 70, y + 48, C_DIM);
        toggle(W - 100, y + 30, rows[i].on);
    }
    
    rcBack = {40, 640, 140, 44};
    button(rcBack, "B Back", false);
    text(fSmall, "A to toggle • B to go back", W - 40, 654, C_DIM, 2);
}

static void drawConfirm() {
    fillRect(0, 0, W, H, C_BG);
    
    // Header
    fillRect(0, 0, W, 100, C_PANEL);
    fillRect(0, 95, W, 2, C_ORG);
    text(fTitle, "Ready to Install?", 40, 35, C_TEXT);
    
    // Summary panel
    fillRect(60, 120, W - 120, 500, C_PANEL);
    fillRect(60, 120, W - 120, 2, C_ORG);
    
    int y = 140;
    text(fBody, "The following will be installed:", 80, y, C_ORG);
    y += 40;
    
    int shown = 0;
    for (auto& c : comps)
        if (c.selected && !menuOnly) {
            if (shown++ < 8) {
                text(fSmall, "• " + c.name + (c.installed ? " (update)" : ""), 100, y, C_TEXT);
                y += 28;
            }
        }
    
    if (opts.withBootMenu || menuOnly) {
        text(fSmall, "• 14CFW Boot Menu", 100, y, C_ORG);
        y += 28;
    }
    
    y += 20;
    text(fSmall, "✓ Your games, saves and emuMMC are protected", 80, y, C_OK);
    y += 28;
    text(fSmall, opts.backup ? "✓ Backups enabled" : "⚠ Backups disabled", 80, y, opts.backup ? C_OK : C_WARN);
    y += 28;
    text(fSmall, opts.keepConfigs ? "✓ Existing configs will be kept" : "⚠ Configs may be overwritten", 80, y, opts.keepConfigs ? C_OK : C_WARN);
    
    rcCancel = {W / 2 - 180, 640, 160, 44};
    rcGo = {W / 2 + 20, 640, 160, 44};
    button(rcCancel, "B Cancel", false);
    button(rcGo, "A Install", true);
}

static void drawRun(Uint32 tick) {
    fillRect(0, 0, W, H, C_BG);
    
    // Header
    fillRect(0, 0, W, 100, C_PANEL);
    fillRect(0, 95, W, 2, C_ORG);
    text(fTitle, jobKind == J_CHECK ? "Checking Updates" : "Installing", 40, 35, C_TEXT);
    
    std::string step;
    std::vector<std::string> lg;
    float ov = 0, sub = 0;
    if (prog) {
        std::lock_guard<std::mutex> g(prog->mu);
        step = prog->step;
        lg = prog->log;
        ov = prog->overall;
        sub = prog->sub;
    }
    
    // Progress section
    fillRect(60, 120, W - 120, 100, C_PANEL);
    fillRect(60, 120, W - 120, 2, C_ORG);
    
    std::string dots(1 + (tick / 400) % 3, '.');
    text(fBody, (step.empty() ? std::string("Starting") : step) + dots, 80, 140, C_TEXT);
    progressBar(80, 180, W - 160, 12, ov);
    progressBar(80, 200, W - 160, 6, sub);
    
    // Log section
    fillRect(60, 240, W - 120, 350, C_PANEL);
    fillRect(60, 240, W - 120, 2, C_ORG);
    
    int maxl = 11;
    int start = lg.size() > (size_t)maxl ? (int)lg.size() - maxl : 0;
    for (int i = start; i < (int)lg.size(); i++) {
        const std::string& s = lg[i];
        SDL_Color col = s.find("FAILED") != std::string::npos ? C_ERR : C_TEXT;
        text(fSmall, fit(fSmall, s, W - 160), 80, 260 + (i - start) * 28, col);
    }
    
    rcCancel = {W / 2 - 160, 640, 320, 44};
    button(rcCancel, prog && prog->cancel ? "Stopping..." : "B Cancel", false);
    text(fSmall, "Do not power off your Switch", W / 2, 690, C_WARN, 1);
}

static void drawDone() {
    fillRect(0, 0, W, H, C_BG);
    
    // Header
    fillRect(0, 0, W, 100, C_PANEL);
    fillRect(0, 95, W, 2, C_ORG);
    text(fTitle, "Installation Complete", 40, 35, C_TEXT);
    
    bool bad = prog && (prog->failedHard || prog->failed > 0);
    
    // Result panel
    fillRect(120, 130, W - 240, 470, C_PANEL);
    fillRect(120, 130, W - 240, 2, bad ? C_ERR : C_ORG);
    
    // Status icon
    int icon_y = 180;
    if (bad) {
        circle(W / 2, icon_y, 35, C_ERR);
        line(W / 2 - 12, icon_y - 8, W / 2 + 12, icon_y + 8, C_BG, 4);
    } else {
        circle(W / 2, icon_y, 35, C_OK);
        line(W / 2 - 15, icon_y, W / 2 - 5, icon_y + 12, C_BG, 4);
        line(W / 2 - 5, icon_y + 12, W / 2 + 15, icon_y - 12, C_BG, 4);
    }
    
    text(fTitle, bad ? "Completed with errors" : "All done!", W / 2, 260, C_TEXT, 1);
    
    std::vector<std::string> lg;
    if (prog) {
        std::lock_guard<std::mutex> g(prog->mu);
        lg = prog->log;
    }
    
    int y = 320;
    int shown = 0;
    for (int i = (int)lg.size() - 1; i >= 0 && shown < 4; i--) {
        if (lg[i].find("FAILED") != std::string::npos) {
            text(fSmall, "✗ " + fit(fSmall, lg[i], W - 280), 150, y, C_ERR);
            y += 28;
            shown++;
        }
    }
    
    text(fSmall, "Restart your Switch to apply changes", W / 2, 480, C_DIM, 1);
    
    rcGo = {W / 2 - 130, 640, 260, 44};
    button(rcGo, "A Back to Menu", true);
}

// ------------------------------------------------------------------ actions
static void homeAction(int i) {
    if (i == 0) { menuOnly = false; screen = COMPS; }
    else if (i == 1) { menuOnly = true; screen = CONFIRM; }
    else if (i == 2) { screen = SETTINGS; }
    else if (i == 3) { rescan(); toast("Status refreshed"); }
}

static bool wantExit = false;

static void startInstallFlow() {
    if (selectedCount() == 0 && !opts.withBootMenu) { toast("Nothing selected"); return; }
    menuOnly = false;
    screen = CONFIRM;
}

static void confirmGo() {
    if (menuOnly) startJob(J_MENU);
    else startJob(J_INSTALL);
}

static void toggleSetting(int i) {
    if (i == 0) opts.backup = !opts.backup;
    else if (i == 1) opts.keepConfigs = !opts.keepConfigs;
    else if (i == 2) opts.withBootMenu = !opts.withBootMenu;
    else if (i == 3) opts.replaceMenu = !opts.replaceMenu;
    else return;
    saveSettings();
}

static void selectAllToggle() {
    bool any = false;
    for (auto& c : comps)
        if (!c.selected) any = true;
    for (auto& c : comps) c.selected = any;
}

static void onTap(int x, int y) {
    if (screen == HOME) {
        for (int i = 0; i < 5; i++)
            if (hit(rcHome[i], x, y)) { homeSel = i; if (i == 4) wantExit = true; else homeAction(i); }
    } else if (screen == COMPS) {
        if (hit(rcBack, x, y)) { screen = HOME; return; }
        if (hit(rcCheck, x, y)) { startJob(J_CHECK); return; }
        if (hit(rcAll, x, y)) { selectAllToggle(); return; }
        if (hit(rcInstall, x, y)) { startInstallFlow(); return; }
        for (int r = 0; r < ROWS_VISIBLE; r++)
            if (hit(rcRows[r], x, y)) {
                int i = compTop + r;
                if (i < (int)comps.size()) { compSel = i; comps[i].selected = !comps[i].selected; }
            }
    } else if (screen == SETTINGS) {
        if (hit(rcBack, x, y)) { screen = HOME; return; }
        for (int i = 0; i < 4; i++)
            if (hit(rcSet[i], x, y)) { setSel = i; toggleSetting(i); }
    } else if (screen == CONFIRM) {
        if (hit(rcCancel, x, y)) screen = HOME;
        else if (hit(rcGo, x, y)) confirmGo();
    } else if (screen == RUN) {
        if (hit(rcCancel, x, y) && prog) prog->cancel = true;
    } else if (screen == DONE) {
        if (hit(rcGo, x, y)) screen = HOME;
    }
}

// ------------------------------------------------------------------ main
int main(int, char**) {
    plInitialize(PlServiceType_User);
    socketInitializeDefault();
    curl_global_init(CURL_GLOBAL_DEFAULT);
    inst::init();
    SDL_Init(SDL_INIT_VIDEO);
    TTF_Init();
    SDL_Window* win = SDL_CreateWindow("14CFW", 0, 0, W, H, SDL_WINDOW_SHOWN);
    R = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);

    PlFontData fd;
    if (R_SUCCEEDED(plGetSharedFontByType(&fd, PlSharedFontType_Standard))) {
        auto open = [&](int sz) { return TTF_OpenFontRW(SDL_RWFromMem(fd.address, fd.size), 1, sz); };
        fSmall = open(17);
        fBody = open(22);
        fTitle = open(30);
        fBig = open(44);
    }

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();

    loadSettings();
    inst::loadManifest(comps);
    rescan();

    if (fBody && fSmall && fTitle && fBig) {
        bool wasDown = false, dragged = false;
        int startY = 0, lastX = 0, lastY = 0;
        while (appletMainLoop() && !wantExit) {
            padUpdate(&pad);
            u64 down = padGetButtonsDown(&pad);
            SDL_Event e;
            while (SDL_PollEvent(&e)) {}

            HidTouchScreenState ts = {0};
            hidGetTouchScreenStates(&ts, 1);
            bool tapped = false;
            if (ts.count > 0) {
                int tx = (int)ts.touches[0].x, ty = (int)ts.touches[0].y;
                if (!wasDown) { startY = ty; dragged = false; }
                else if (std::abs(ty - startY) > 14) dragged = true;
                lastX = tx;
                lastY = ty;
                wasDown = true;
            } else if (wasDown) {
                if (!dragged) tapped = true;
                wasDown = false;
            }
            if (tapped) onTap(lastX, lastY);

            if (jobKind != J_NONE && prog && prog->done) finishJob();

            switch (screen) {
            case HOME:
                if (down & HidNpadButton_Up) homeSel = (homeSel + 4) % 5;
                if (down & HidNpadButton_Down) homeSel = (homeSel + 1) % 5;
                if (down & HidNpadButton_A) { if (homeSel == 4) wantExit = true; else homeAction(homeSel); }
                if (down & HidNpadButton_Plus) wantExit = true;
                break;
            case COMPS: {
                int n = (int)comps.size();
                if (down & HidNpadButton_Up) compSel = (compSel + n - 1) % n;
                if (down & HidNpadButton_Down) compSel = (compSel + 1) % n;
                if ((down & HidNpadButton_A) && n) comps[compSel].selected = !comps[compSel].selected;
                if (down & HidNpadButton_Y) selectAllToggle();
                if (down & HidNpadButton_X) startJob(J_CHECK);
                if (down & HidNpadButton_Plus) startInstallFlow();
                if (down & HidNpadButton_B) screen = HOME;
                break;
            }
            case SETTINGS:
                if (down & HidNpadButton_Up) setSel = (setSel + 3) % 4;
                if (down & HidNpadButton_Down) setSel = (setSel + 1) % 4;
                if (down & HidNpadButton_A) toggleSetting(setSel);
                if (down & HidNpadButton_B) screen = HOME;
                break;
            case CONFIRM:
                if (down & HidNpadButton_A) confirmGo();
                if (down & HidNpadButton_B) screen = HOME;
                break;
            case RUN:
                if ((down & HidNpadButton_B) && prog) prog->cancel = true;
                break;
            case DONE:
                if (down & (HidNpadButton_A | HidNpadButton_B)) screen = HOME;
                if (down & HidNpadButton_Plus) wantExit = true;
                break;
            }

            Uint32 tick = SDL_GetTicks();
            switch (screen) {
            case HOME: drawHome(); break;
            case COMPS: drawComps(); break;
            case SETTINGS: drawSettings(); break;
            case CONFIRM: drawConfirm(); break;
            case RUN: drawRun(tick); break;
            case DONE: drawDone(); break;
            }
            if (SDL_GetTicks() < toastUntil) {
                int w = textW(fSmall, toastMsg) + 44;
                fillRect(W / 2 - w / 2, 590, w, 40, C_PANEL);
                fillRect(W / 2 - w / 2, 590, w, 2, C_ORG);
                text(fSmall, toastMsg, W / 2, 600, C_TEXT, 1);
            }
            SDL_RenderPresent(R);
        }
    }

    if (prog && jobKind != J_NONE) prog->cancel = true;
    joinWorker();
    delete prog;
    for (auto& kv : tcache)
        if (kv.second.t) SDL_DestroyTexture(kv.second.t);
    inst::shutdown();
    curl_global_cleanup();
    socketExit();
    if (R) SDL_DestroyRenderer(R);
    if (win) SDL_DestroyWindow(win);
    TTF_Quit();
    SDL_Quit();
    plExit();
    return 0;
}
