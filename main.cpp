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
static const SDL_Color C_BG{11, 14, 23, 255}, C_CARD{18, 22, 36, 255}, C_CARD2{26, 32, 52, 255}, C_BORDER{42, 48, 80, 255},
    C_TEXT{232, 234, 240, 255}, C_DIM{140, 148, 170, 255}, C_CYAN{25, 230, 255, 255}, C_ORG{255, 122, 69, 255},
    C_OK{47, 191, 143, 255}, C_ERR{232, 110, 110, 255}, C_WHITE{255, 255, 255, 255}, C_WARN{255, 196, 70, 255};

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
static void circle(int cx, int cy, int r, SDL_Color c) { rrect(cx - r, cy - r, 2 * r, 2 * r, r, c); }
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
static void gradBar(int x, int y, int w, int h) {
    int seg = 48;
    for (int i = 0; i < seg; i++) fillRect(x + i * w / seg, y, w / seg + 1, h, mix(C_CYAN, C_ORG, (float)i / (seg - 1)));
}
static void progressBar(int x, int y, int w, int h, float v) {
    rrect(x, y, w, h, h / 2, C_CARD2);
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    int fw = (int)(w * v);
    if (fw >= h) {
        rrect(x, y, fw, h, h / 2, C_CYAN);
    } else if (fw > 0) {
        rrect(x, y, h, h, h / 2, C_CYAN);
    }
}
static void checkbox(int x, int y, int s, bool on) {
    panel(x, y, s, s, 7, on ? C_CYAN : C_CARD, on ? C_CYAN : C_BORDER);
    if (on) {
        line(x + 6, y + s / 2, x + s / 2 - 1, y + s - 8, C_BG, 3);
        line(x + s / 2 - 1, y + s - 8, x + s - 6, y + 7, C_BG, 3);
    }
}
static void toggle(int x, int y, bool on) {
    rrect(x, y, 56, 30, 15, on ? C_CYAN : C_BORDER);
    circle(on ? x + 41 : x + 15, y + 15, 11, C_WHITE);
}

struct Rc { int x = 0, y = 0, w = 0, h = 0; };
static bool hit(const Rc& r, int px, int py) { return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h; }
static void button(const Rc& r, const std::string& label, bool primary) {
    panel(r.x, r.y, r.w, r.h, r.h / 2, primary ? C_CYAN : C_CARD2, primary ? C_CYAN : C_BORDER);
    text(fSmall, label, r.x + r.w / 2, r.y + (r.h - 20) / 2, primary ? C_BG : C_TEXT, 1);
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
static void header(const std::string& title) {
    int w1 = text(fBig, "14", 40, 14, C_ORG);
    text(fBig, "CFW", 40 + w1 + 4, 14, C_CYAN);
    text(fTitle, title, W - 40, 22, C_TEXT, 2);
    gradBar(40, 70, W - 80, 3);
}

static void card(int x, int y, int w, int h, const std::string& title, const std::string& value, const std::string& sub, SDL_Color vc) {
    panel(x, y, w, h, 16, C_CARD, C_BORDER);
    text(fSmall, title, x + 20, y + 14, C_DIM);
    text(fTitle, fit(fTitle, value, w - 40), x + 20, y + 40, vc);
    text(fSmall, fit(fSmall, sub, w - 40), x + 20, y + h - 30, C_DIM);
}

static void drawHome() {
    fillRect(0, 0, W, H, C_BG);
    header("Custom firmware installer");
    int cx = 40, cy = 96, cw = 366, ch = 118, gap = 16;
    std::string amsV = env.amsRunning ? "v" + env.amsVer : (env.ams ? "Installed" : "Not found");
    card(cx, cy, cw, ch, "ATMOSPHERE", amsV, env.amsRunning ? "You are running it right now" : (env.ams ? "Files found on the SD card" : "Not installed yet"), env.ams || env.amsRunning ? C_OK : C_WARN);
    card(cx + cw + gap, cy, cw, ch, "HEKATE", env.hekate ? (env.hekateVer.empty() ? "Installed" : "v" + env.hekateVer) : "Not found", env.hekate ? "Bootloader detected" : "Not installed yet", env.hekate ? C_OK : C_WARN);
    std::string sd = env.sdOk ? inst::humanSize(env.freeBytes) + " free" : "Unreadable";
    card(cx, cy + ch + gap, cw, ch, "SD CARD", sd, env.sdOk ? "of " + inst::humanSize(env.totalBytes) : "", env.sdOk ? C_TEXT : C_ERR);
    std::string bat = env.battery >= 0 ? std::to_string(env.battery) + "%" + (env.charging ? "  charging" : "") : "Unknown";
    bool lowBat = env.battery >= 0 && env.battery < 30 && !env.charging;
    card(cx + cw + gap, cy + ch + gap, cw, ch, "BATTERY", bat, lowBat ? "Plug in the charger before installing" : "OK for installing", lowBat ? C_ERR : C_TEXT);
    card(cx, cy + 2 * (ch + gap), cw, ch, "GAMES AND SAVES", env.nintendo ? "Protected" : "No Nintendo folder", "The Nintendo folder is never modified", C_OK);
    card(cx + cw + gap, cy + 2 * (ch + gap), cw, ch, "EMUMMC", env.emummc ? "Found, protected" : "Not found", "The emuMMC folder is never modified", env.emummc ? C_OK : C_DIM);

    // detected apps
    std::string det;
    int n = 0;
    for (auto& c : comps)
        if (c.installed) { det += (det.empty() ? "" : ", ") + c.name; n++; }
    text(fSmall, "Detected: " + (det.empty() ? std::string("none of the known apps yet") : fit(fSmall, det, 640)), 40, 540 + 20, C_DIM);
    text(fSmall, std::to_string(n) + " of " + std::to_string(comps.size()) + " known components are already installed", 40, 568 + 20, C_DIM);

    // menu
    int mx = 800, mw = 440;
    panel(mx - 20, 96, mw + 40, 560, 20, C_CARD, C_BORDER);
    static const char* items[5] = {"Install / Update", "Install 14CFW boot menu", "Settings", "Rescan SD card", "Exit"};
    static const char* subs[5] = {"Pick components, customize the install", "Custom Hekate menu, logo and icon", "Backups, configs, boot menu mode", "Detect what is already installed", "Return to hbmenu"};
    for (int i = 0; i < 5; i++) {
        rcHome[i] = {mx, 120 + i * 100, mw, 84};
        bool sel = i == homeSel;
        panel(rcHome[i].x, rcHome[i].y, rcHome[i].w, rcHome[i].h, 16, sel ? C_CARD2 : C_CARD, sel ? C_CYAN : C_BORDER);
        circle(mx + 36, rcHome[i].y + 42, 7, sel ? C_ORG : C_BORDER);
        text(fBody, items[i], mx + 64, rcHome[i].y + 14, C_TEXT);
        text(fSmall, subs[i], mx + 64, rcHome[i].y + 46, C_DIM);
    }
    text(fSmall, "Up/Down choose   A open   + exit", 640, 690, C_DIM, 1);
}

static void drawComps() {
    fillRect(0, 0, W, H, C_BG);
    header("Choose what to install");
    text(fSmall, "Existing files are backed up. Your games, saves and emuMMC are never touched.", 40, 84, C_DIM);
    int n = (int)comps.size();
    if (compSel < compTop) compTop = compSel;
    if (compSel >= compTop + ROWS_VISIBLE) compTop = compSel - ROWS_VISIBLE + 1;
    for (int r = 0; r < ROWS_VISIBLE; r++) {
        int i = compTop + r;
        rcRows[r] = {0, 0, 0, 0};
        if (i >= n) continue;
        Component& c = comps[i];
        int y = 112 + r * 64;
        rcRows[r] = {40, y, W - 80, 58};
        bool sel = i == compSel;
        panel(40, y, W - 80, 58, 14, sel ? C_CARD2 : C_CARD, sel ? C_CYAN : C_BORDER);
        checkbox(58, y + 15, 28, c.selected);
        text(fBody, c.name, 104, y + 6, C_TEXT);
        text(fSmall, fit(fSmall, c.desc, 700), 104, y + 33, C_DIM);
        int rx = W - 60;
        if (c.installed) {
            int bw = textW(fSmall, "Installed") + 28;
            panel(rx - bw, y + 15, bw, 28, 14, C_CARD, C_OK);
            text(fSmall, "Installed", rx - bw / 2, y + 19, C_OK, 1);
            rx -= bw + 12;
        } else {
            int bw = textW(fSmall, "Not installed") + 28;
            panel(rx - bw, y + 15, bw, 28, 14, C_CARD, C_BORDER);
            text(fSmall, "Not installed", rx - bw / 2, y + 19, C_DIM, 1);
            rx -= bw + 12;
        }
        if (!c.latest.empty()) text(fSmall, "latest " + c.latest, rx, y + 19, C_CYAN, 2);
    }
    if (n > ROWS_VISIBLE) text(fSmall, std::to_string(compSel + 1) + " / " + std::to_string(n), W - 40, 84, C_DIM, 2);
    rcBack = {40, 640, 150, 44};
    rcCheck = {206, 640, 230, 44};
    rcAll = {452, 640, 200, 44};
    rcInstall = {W - 40 - 260, 640, 260, 44};
    button(rcBack, "B  Back", false);
    button(rcCheck, "X  Check updates", false);
    button(rcAll, "Y  Select all", false);
    button(rcInstall, "+  Install (" + std::to_string(selectedCount()) + ")", true);
}

static void drawSettings() {
    fillRect(0, 0, W, H, C_BG);
    header("Settings");
    struct Row { const char* t; const char* s; bool on; bool isToggle; std::string v; };
    Row rows[5] = {
        {"Back up before overwriting", "Copies every file it would replace to switch/14CFW/backup", opts.backup, true, ""},
        {"Keep my existing settings", "Never overwrites config files you already have (ini, json, cfg)", opts.keepConfigs, true, ""},
        {"Install the 14CFW boot menu", "Also writes the custom Hekate menu, logo and icon", opts.withBootMenu, true, ""},
        {"Boot menu mode", "", false, false, opts.replaceMenu ? "Replace main menu (old one backed up)" : "Add as an extra config"},
        {"Certificate check", "", false, false, env.tlsOk ? "ON" : "OFF  (put cacert.pem in switch/14CFW)"},
    };
    for (int i = 0; i < 5; i++) {
        int y = 100 + i * 96;
        rcSet[i] = {40, y, W - 80, 84};
        bool sel = i == setSel;
        panel(40, y, W - 80, 84, 16, sel ? C_CARD2 : C_CARD, sel ? C_CYAN : C_BORDER);
        text(fBody, rows[i].t, 70, y + 14, C_TEXT);
        if (rows[i].isToggle) {
            text(fSmall, rows[i].s, 70, y + 48, C_DIM);
            toggle(W - 40 - 30 - 56, y + 27, rows[i].on);
        } else {
            text(fSmall, rows[i].v, 70, y + 48, i == 4 && !env.tlsOk ? C_WARN : C_CYAN);
        }
    }
    rcBack = {40, 640, 150, 44};
    button(rcBack, "B  Back", false);
    text(fSmall, "A change", W - 40, 654, C_DIM, 2);
}

static void drawConfirm() {
    fillRect(0, 0, W, H, C_BG);
    header("Ready to install");
    panel(120, 100, W - 240, 520, 20, C_CARD, C_BORDER);
    int y = 122;
    text(fBody, "This will install:", 160, y, C_TEXT);
    y += 40;
    int shown = 0;
    for (auto& c : comps)
        if (c.selected && !menuOnly) {
            if (shown++ < 7) text(fBody, "+  " + c.name + (c.installed ? "   (update)" : ""), 180, y, C_CYAN);
            else if (shown == 8) text(fSmall, "and more...", 180, y, C_DIM);
            if (shown <= 7) y += 32;
        }
    if (opts.withBootMenu || menuOnly) { text(fBody, "+  14CFW boot menu", 180, y, C_ORG); y += 32; }
    y = std::max(y + 10, 420);
    text(fSmall, "Games, saves and emuMMC are never touched.", 160, y, C_OK); y += 26;
    text(fSmall, opts.backup ? "Replaced files are backed up first (switch/14CFW/backup)." : "Backups are OFF. Replaced files cannot be restored.", 160, y, opts.backup ? C_OK : C_WARN); y += 26;
    text(fSmall, opts.keepConfigs ? "Your existing config files are kept." : "Existing config files may be overwritten.", 160, y, opts.keepConfigs ? C_OK : C_WARN); y += 26;
    if (env.battery >= 0 && env.battery < 30 && !env.charging) text(fSmall, "Battery is low. Plug in the charger.", 160, y, C_ERR);
    rcGo = {W / 2 + 20, 640, 240, 44};
    rcCancel = {W / 2 - 180, 640, 180, 44};
    button(rcCancel, "B  Cancel", false);
    button(rcGo, "A  Start", true);
}

static void drawRun(Uint32 tick) {
    fillRect(0, 0, W, H, C_BG);
    const char* title = jobKind == J_CHECK ? "Checking for updates" : (jobKind == J_MENU ? "Installing boot menu" : "Installing");
    header(title);
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
    panel(60, 100, W - 120, 120, 20, C_CARD, C_BORDER);
    std::string dots(1 + (tick / 400) % 3, '.');
    text(fBody, (step.empty() ? std::string("Starting") : step) + dots, 90, 118, C_TEXT);
    progressBar(90, 160, W - 180, 14, ov);
    progressBar(90, 186, W - 180, 8, sub);
    panel(60, 236, W - 120, 380, 20, C_CARD, C_BORDER);
    int maxl = 11;
    int start = lg.size() > (size_t)maxl ? (int)lg.size() - maxl : 0;
    for (int i = start; i < (int)lg.size(); i++) {
        const std::string& s = lg[i];
        SDL_Color col = s.find("FAILED") != std::string::npos || s.find("  !") == 0 ? C_ERR : (s.find("  =") == 0 ? C_DIM : C_TEXT);
        text(fSmall, fit(fSmall, s, W - 180), 90, 254 + (i - start) * 31, col);
    }
    rcCancel = {W / 2 - 160, 640, 320, 44};
    button(rcCancel, prog && prog->cancel ? "Stopping after this file..." : "B  Cancel", false);
    text(fSmall, "Do not turn off the console while this is running.", W / 2, 690, C_WARN, 1);
}

static void drawDone() {
    fillRect(0, 0, W, H, C_BG);
    header("Finished");
    bool bad = prog && (prog->failedHard || prog->failed > 0);
    panel(120, 100, W - 240, 520, 20, C_CARD, C_BORDER);
    circle(W / 2, 170, 42, bad ? C_WARN : C_OK);
    if (bad) { line(W / 2, 150, W / 2, 175, C_BG, 5); fillRect(W / 2 - 3, 186, 7, 7, C_BG); }
    else { line(W / 2 - 18, 172, W / 2 - 4, 188, C_BG, 6); line(W / 2 - 4, 188, W / 2 + 22, 154, C_BG, 6); }
    std::string sum, bdir;
    std::vector<std::string> lg;
    if (prog) {
        std::lock_guard<std::mutex> g(prog->mu);
        sum = prog->summary;
        bdir = prog->backupDir;
        lg = prog->log;
    }
    text(fTitle, bad ? "Done, with some problems" : "All done", W / 2, 232, C_TEXT, 1);
    if (!sum.empty()) text(fSmall, sum, W / 2, 278, C_DIM, 1);
    int y = 316;
    int shown = 0;
    for (int i = (int)lg.size() - 1; i >= 0 && shown < 6; i--)
        if (lg[i].find("FAILED") != std::string::npos || lg[i].find("Not enough") != std::string::npos || lg[i].find("Battery") != std::string::npos) {
            text(fSmall, fit(fSmall, lg[i], W - 320), 160, y, C_ERR);
            y += 28;
            shown++;
        }
    y = std::max(y + 10, 440);
    if (!bdir.empty() && opts.backup) text(fSmall, "Backups: " + bdir.substr(6), 160, y, C_DIM), y += 28;
    text(fBody, "Restart your Switch to apply the changes.", W / 2, 520, C_CYAN, 1);
    text(fSmall, "Hold Power, then choose Power Options, then Restart.", W / 2, 556, C_DIM, 1);
    rcGo = {W / 2 - 130, 640, 260, 44};
    button(rcGo, "A  Back to menu", true);
}

// ------------------------------------------------------------------ actions
static void homeAction(int i) {
    if (i == 0) { menuOnly = false; screen = COMPS; }
    else if (i == 1) { menuOnly = true; screen = CONFIRM; }
    else if (i == 2) { screen = SETTINGS; }
    else if (i == 3) { rescan(); toast("SD card scanned"); }
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
        for (int i = 0; i < 5; i++)
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
                if (down & HidNpadButton_Up) setSel = (setSel + 4) % 5;
                if (down & HidNpadButton_Down) setSel = (setSel + 1) % 5;
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
                panel(W / 2 - w / 2, 590, w, 40, 20, C_CARD2, C_CYAN);
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
