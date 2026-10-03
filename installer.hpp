#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct Extra { std::string asset, to; };

struct Component {
    std::string id, name, desc;
    std::string repo, url, asset, skip;   // GitHub repo + asset pattern ("*" wildcard), or a direct url
    std::string type = "zip";             // "zip" (extract to SD root) or "file" (copy to 'to')
    std::string to;                       // destination for type "file"
    std::vector<std::string> detect;      // paths (relative to SD root) that prove it is installed
    std::vector<Extra> extras;            // extra release assets placed at fixed paths
    std::string postPattern, postTo;      // after extracting: copy first entry matching pattern to postTo
    bool defSel = false, core = false;
    bool installed = false, selected = false;
    std::string latest;
};

struct Options {
    bool backup = true;        // copy files to /switch/14CFW/backup before overwriting
    bool keepConfigs = true;   // never overwrite existing config files
    bool withBootMenu = true;  // also install the 14CFW boot menu
    bool replaceMenu = true;   // true: replace bootloader/hekate_ipl.ini (backed up); false: add bootloader/ini/14CFW.ini
};

struct Env {
    bool amsRunning = false, ams = false, hekate = false, nintendo = false, emummc = false;
    bool tlsOk = false, charging = false, sdOk = false;
    std::string amsVer, hekateVer;
    unsigned long long freeBytes = 0, totalBytes = 0;
    int battery = -1;
};

struct Progress {
    std::mutex mu;
    std::vector<std::string> log;
    std::string step, backupDir, summary;
    std::vector<std::pair<std::string, std::string>> latest;  // id -> tag (from update check)
    float overall = 0, sub = 0;
    std::atomic<bool> cancel{false}, done{false}, failedHard{false};
    int ok = 0, skipped = 0, failed = 0, backedUp = 0;
    void say(const std::string& s) {
        std::lock_guard<std::mutex> g(mu);
        log.push_back(s);
        if (log.size() > 200) log.erase(log.begin());
    }
    void setStep(const std::string& s) {
        std::lock_guard<std::mutex> g(mu);
        step = s;
    }
};

namespace inst {
void init();
void shutdown();
void loadManifest(std::vector<Component>& out);
void scan(std::vector<Component>& comps, Env& env);
void checkUpdates(std::vector<Component> comps, Progress& p);
void install(std::vector<Component> comps, Options o, Env env, Progress& p);
void installBootMenuOnly(Options o, Env env, Progress& p);
std::string humanSize(unsigned long long b);
}
