#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

extern std::string g_root;  // "sdmc:/" on the Switch

struct HbApp {
    const char* id;    // lowercase key used to detect it
    const char* name;  // folder + display name
    const char* repo;  // GitHub owner/repo
    const char* hint;  // preferred asset name fragment
    const char* desc;
};
const std::vector<HbApp>& hbApps();

struct Env {
    bool sdOk = false;
    unsigned long long sdFree = 0, sdTotal = 0;
    bool online = false;
    bool amsRunning = false;
    std::string amsVer, fw;
    bool amsFiles = false, hekate = false, emummc = false, nintendo = false, bootMenu = false;
    std::vector<std::string> nros;  // lowercase names of .nro files under switch/
};
Env detectEnv();
bool appPresent(const Env& e, const HbApp& a);
std::string fmtBytes(unsigned long long b);

struct Plan {
    bool ams = true, hekate = true, bootMenu = true;
    bool replaceIni = true;  // false: add as bootloader/ini/14CFW.ini
    bool backup = true;
    std::vector<std::string> apps;  // HbApp::id
};

struct Progress {
    std::atomic<float> frac{0};
    std::atomic<bool> done{false}, failed{false}, cancel{false};
    std::mutex m;
    std::string step, detail, error;
    std::vector<std::string> log;
    void set(const std::string& s, const std::string& d = "") {
        std::lock_guard<std::mutex> l(m);
        step = s;
        detail = d;
    }
    void addLog(const std::string& s) {
        std::lock_guard<std::mutex> l(m);
        log.push_back(s);
    }
};

void runInstall(const Plan& p, const Env& e, Progress& pr);
