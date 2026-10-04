#include "installer.hpp"
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <curl/curl.h>
#include "json.hpp"
#include "logo.hpp"
#include "miniz.h"
#ifdef __SWITCH__
#include <switch.h>
#endif

using json = nlohmann::json;
std::string g_root = "sdmc:/";

bool zinflate(const unsigned char* z, size_t zlen, unsigned char* out, size_t rawLen) {
    mz_ulong n = (mz_ulong)rawLen;
    return mz_uncompress(out, &n, z, (mz_ulong)zlen) == MZ_OK && n == rawLen;
}

const std::vector<HbApp>& hbApps() {
    static const std::vector<HbApp> v = {
        {"checkpoint", "Checkpoint", "FlagBrew/Checkpoint", "checkpoint", "Back up and restore your game saves"},
        {"jksv", "JKSV", "J-D-K/JKSV", "jksv", "Another save manager"},
        {"nx-shell", "NX-Shell", "joel16/NX-Shell", "nx-shell", "File manager for your SD card"},
        {"ftpd", "ftpd", "mtheall/ftpd", "ftpd", "FTP server to move files from your PC or phone"},
        {"goldleaf", "Goldleaf", "XorTroll/Goldleaf", "goldleaf", "File browser and content manager"},
        {"appstore", "Homebrew App Store", "fortheusers/hb-appstore", "appstore", "Get more homebrew with one tap"},
        {"nxdumptool", "NXDumpTool", "DarkMatterCore/nxdumptool", "nxdumptool", "Dump your own cartridges and keys"},
    };
    return v;
}

// ---------------------------------------------------------------- helpers
static std::string sd(const std::string& rel) { return g_root + rel; }
static bool exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}
static bool isDir(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
static std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
static bool endsWith(const std::string& s, const std::string& e) {
    return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0;
}
static bool startsWith(const std::string& s, const std::string& b) { return s.compare(0, b.size(), b) == 0; }

static void mkdirs(const std::string& full) {  // full path, creates every missing level
    size_t start = full.find(":/");
    start = start == std::string::npos ? 0 : start + 2;
    if (full.size() && full[0] == '/') start = 1;
    for (size_t i = start; i <= full.size(); i++) {
        if (i == full.size() || full[i] == '/') {
            std::string d = full.substr(0, i);
            if (!d.empty() && !isDir(d)) mkdir(d.c_str(), 0777);
        }
    }
}
static void mkdirsFor(const std::string& file) {
    size_t p = file.find_last_of('/');
    if (p != std::string::npos) mkdirs(file.substr(0, p));
}
static bool copyFile(const std::string& a, const std::string& b) {
    FILE* in = fopen(a.c_str(), "rb");
    if (!in) return false;
    mkdirsFor(b);
    FILE* out = fopen(b.c_str(), "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    std::vector<char> buf(64 * 1024);
    size_t n;
    bool ok = true;
    while ((n = fread(buf.data(), 1, buf.size(), in)) > 0)
        if (fwrite(buf.data(), 1, n, out) != n) {
            ok = false;
            break;
        }
    fclose(in);
    fclose(out);
    return ok;
}
static bool writeText(const std::string& path, const std::string& txt) {
    mkdirsFor(path);
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(txt.data(), 1, txt.size(), f) == txt.size();
    fclose(f);
    if (!ok) return false;
    remove(path.c_str());
    return rename(tmp.c_str(), path.c_str()) == 0;
}
static bool replaceWith(const std::string& tmp, const std::string& path) {
    remove(path.c_str());
    return rename(tmp.c_str(), path.c_str()) == 0;
}

std::string fmtBytes(unsigned long long b) {
    char t[32];
    if (b >= (1ull << 30)) snprintf(t, sizeof t, "%.1f GB", b / 1073741824.0);
    else if (b >= (1ull << 20)) snprintf(t, sizeof t, "%.0f MB", b / 1048576.0);
    else snprintf(t, sizeof t, "%llu KB", b / 1024);
    return t;
}

// ---------------------------------------------------------------- detection
Env detectEnv() {
    Env e;
    struct statvfs sv;
    if (statvfs(g_root.c_str(), &sv) == 0) {
        e.sdOk = true;
        e.sdFree = (unsigned long long)sv.f_bavail * sv.f_frsize;
        e.sdTotal = (unsigned long long)sv.f_blocks * sv.f_frsize;
    }
    e.online = true;
    e.fw = "";
#ifdef __SWITCH__
    {
        if (R_SUCCEEDED(nifmInitialize(NifmServiceType_User))) {
            NifmInternetConnectionType t;
            u32 s;
            NifmInternetConnectionStatus st;
            e.online = R_SUCCEEDED(nifmGetInternetConnectionStatus(&t, &s, &st)) && st == NifmInternetConnectionStatus_Connected;
            nifmExit();
        } else
            e.online = false;
        if (R_SUCCEEDED(setsysInitialize())) {
            SetSysFirmwareVersion fv;
            if (R_SUCCEEDED(setsysGetFirmwareVersion(&fv))) e.fw = fv.display_version;
            setsysExit();
        }
        if (R_SUCCEEDED(splInitialize())) {
            u64 v = 0;
            if (R_SUCCEEDED(splGetConfig((SplConfigItem)65000, &v))) {
                e.amsRunning = true;
                char t[32];
                snprintf(t, sizeof t, "%d.%d.%d", (int)((v >> 56) & 0xFF), (int)((v >> 48) & 0xFF), (int)((v >> 40) & 0xFF));
                e.amsVer = t;
            }
            splExit();
        }
    }
#endif
    e.amsFiles = exists(sd("atmosphere/package3")) || isDir(sd("atmosphere/contents"));
    e.hekate = exists(sd("bootloader/update.bin")) || exists(sd("bootloader/hekate_ipl.ini")) || isDir(sd("bootloader/sys"));
    e.emummc = isDir(sd("emuMMC"));
    e.nintendo = isDir(sd("Nintendo"));
    e.bootMenu = exists(sd("bootloader/res/14cfw_logo.bmp"));
    auto scan = [&](const std::string& dirRel, int depth, auto&& self) -> void {
        DIR* d = opendir(sd(dirRel).c_str());
        if (!d) return;
        while (dirent* en = readdir(d)) {
            std::string n = en->d_name;
            if (n == "." || n == "..") continue;
            std::string rel = dirRel + "/" + n;
            if (endsWith(lower(n), ".nro")) e.nros.push_back(lower(n.substr(0, n.size() - 4)));
            else if (depth < 1 && isDir(sd(rel))) self(rel, depth + 1, self);
        }
        closedir(d);
    };
    scan("switch", 0, scan);
    return e;
}

bool appPresent(const Env& e, const HbApp& a) {
    for (auto& n : e.nros)
        if (n.find(a.id) != std::string::npos || n.find(lower(a.hint)) != std::string::npos) return true;
    return false;
}

// ---------------------------------------------------------------- network
static size_t wrStr(void* p, size_t s, size_t n, void* u) {
    ((std::string*)u)->append((char*)p, s * n);
    return s * n;
}
static size_t wrFile(void* p, size_t s, size_t n, void* u) { return fwrite(p, s, n, (FILE*)u); }

struct DlCtx {
    Progress* pr;
    float base, span;
};
static int xfer(void* u, curl_off_t tot, curl_off_t now, curl_off_t, curl_off_t) {
    DlCtx* c = (DlCtx*)u;
    if (c->pr->cancel) return 1;
    if (tot > 0) c->pr->frac = c->base + c->span * (float)((double)now / (double)tot);
    return 0;
}
static void curlCommon(CURL* c) {
    curl_easy_setopt(c, CURLOPT_USERAGENT, "14CFW/1.1");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);  // the Switch has no CA bundle for libcurl
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
}
static bool httpGet(const std::string& url, std::string& out) {
    CURL* c = curl_easy_init();
    if (!c) return false;
    curlCommon(c);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wrStr);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    return rc == CURLE_OK && code == 200;
}
static bool httpFile(const std::string& url, const std::string& path, Progress& pr, float base, float span) {
    mkdirsFor(path);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    CURL* c = curl_easy_init();
    if (!c) {
        fclose(f);
        return false;
    }
    DlCtx ctx{&pr, base, span};
    curlCommon(c);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wrFile);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xfer);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &ctx);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    fclose(f);
    if (rc != CURLE_OK || code != 200) {
        remove(path.c_str());
        return false;
    }
    return true;
}

struct Asset {
    std::string name, url, tag;
};
// all assets of the latest release
static bool latestAssets(const std::string& repo, std::vector<Asset>& out, std::string& tag) {
    std::string body;
    if (!httpGet("https://api.github.com/repos/" + repo + "/releases/latest", body)) return false;
    try {
        json j = json::parse(body);
        tag = j.value("tag_name", "");
        for (auto& a : j["assets"]) out.push_back({a.value("name", ""), a.value("browser_download_url", ""), tag});
    } catch (...) {
        return false;
    }
    return !out.empty();
}

// ---------------------------------------------------------------- safety rules
// Never written, whatever a zip contains: games, saves, emuMMC and keys.
static bool isProtected(const std::string& relRaw) {
    std::string r = lower(relRaw);
    if (startsWith(r, "nintendo/") || startsWith(r, "emummc/") || startsWith(r, "switch/14cfw/")) return true;
    if (endsWith(r, ".keys")) return true;
    return false;
}
// Existing user settings are kept: only written when the file isn't there yet.
static bool keepExisting(const std::string& relRaw) {
    std::string r = lower(relRaw);
    return startsWith(r, "atmosphere/config/") || startsWith(r, "atmosphere/hosts/") || r == "bootloader/hekate_ipl.ini" ||
           startsWith(r, "bootloader/ini/") || r == "atmosphere/system_settings.ini";
}
static bool needsBackup(const std::string& relRaw) {
    std::string r = lower(relRaw);
    return r == "bootloader/hekate_ipl.ini" || r == "bootloader/bootlogo.bmp" || r == "atmosphere/reboot_payload.bin" ||
           r == "bootloader/update.bin" || r == "atmosphere/package3";
}

struct Ctx {
    const Plan* plan;
    Progress* pr;
    std::string stamp;
    int backups = 0;
};
static void backupFile(Ctx& c, const std::string& rel) {
    if (!c.plan->backup || !exists(sd(rel))) return;
    if (copyFile(sd(rel), sd("switch/14CFW/backup/" + c.stamp + "/" + rel))) c.backups++;
}

// Extract a zip to the SD root following the safety rules. skipRoot: ignore root-level files matching.
static int extractZip(Ctx& c, const std::string& zipPath, bool skipRootBins) {
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    if (!mz_zip_reader_init_file(&z, zipPath.c_str(), 0)) return -1;
    int n = (int)mz_zip_reader_get_num_files(&z), done = 0;
    for (int i = 0; i < n; i++) {
        if (c.pr->cancel) break;
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, i, &st)) continue;
        std::string rel = st.m_filename;
        if (rel.empty() || rel.find("..") != std::string::npos) continue;
        if (mz_zip_reader_is_file_a_directory(&z, i)) {
            if (!isProtected(rel)) mkdirs(sd(rel));
            continue;
        }
        if (isProtected(rel)) continue;
        if (skipRootBins && rel.find('/') == std::string::npos && endsWith(lower(rel), ".bin")) continue;
        if (keepExisting(rel) && exists(sd(rel))) continue;
        if (needsBackup(rel)) backupFile(c, rel);
        std::string out = sd(rel);
        mkdirsFor(out);
        std::string tmp = out + ".14tmp";
        if (mz_zip_reader_extract_to_file(&z, i, tmp.c_str(), 0)) {
            replaceWith(tmp, out);
            done++;
        } else
            remove(tmp.c_str());
    }
    mz_zip_reader_end(&z);
    return done;
}

// first .bin at the root of a zip -> given destinations
static bool zipRootBin(const std::string& zipPath, const std::vector<std::string>& dests, Ctx& c) {
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    if (!mz_zip_reader_init_file(&z, zipPath.c_str(), 0)) return false;
    bool ok = false;
    for (int i = 0; i < (int)mz_zip_reader_get_num_files(&z) && !ok; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, i, &st)) continue;
        std::string rel = st.m_filename;
        if (rel.find('/') != std::string::npos || !endsWith(lower(rel), ".bin")) continue;
        std::string first = sd("switch/14CFW/cache/payload.bin");
        mkdirsFor(first);
        if (!mz_zip_reader_extract_to_file(&z, i, first.c_str(), 0)) continue;
        ok = true;
        for (auto& d : dests) {
            if (needsBackup(d)) backupFile(c, d);
            std::string tmp = sd(d) + ".14tmp";
            mkdirsFor(tmp);
            if (copyFile(first, tmp)) replaceWith(tmp, sd(d));
        }
        remove(first.c_str());
    }
    mz_zip_reader_end(&z);
    return ok;
}

// ---------------------------------------------------------------- boot menu
static std::string makeIni(const Plan& p, const Env& e, bool full) {
    std::string logo = "bootloader/res/14cfw_logo.bmp", icon = "bootloader/res/14cfw_icon.bmp";
    std::string common = "fss0=atmosphere/package3\nkip1=atmosphere/kips/*\natmosphere=1\nlogopath=" + logo + "\nicon=" + icon + "\n";
    std::string s;
    if (full) {
        s += "[config]\nautoboot=0\nautoboot_list=0\nbootwait=3\nbacklight=100\nnoticker=0\nautohosoff=1\nautonogc=1\nupdater2p=1\nbootprotect=0\n\n";
        s += "{14CFW boot menu}\n{}\n\n";
    }
    s += "[Atmosphere]\n" + common + "\n";
    if (e.emummc) s += "[emuMMC]\nemummcforce=1\n" + common + "\n";
    s += "[sysMMC]\nemummc_force_disable=1\n" + common + "\n";
    s += "[Stock]\nfss0=atmosphere/package3\nemummc_force_disable=1\nstock=1\nlogopath=" + logo + "\nicon=" + icon + "\n";
    (void)p;
    return s;
}

static bool installBootMenu(Ctx& c, const Env& e) {
    const Plan& p = *c.plan;
    std::string tmp = sd("switch/14CFW/cache/logo.tmp");
    mkdirsFor(tmp);
    if (!writeBootLogoBmp(tmp)) return false;
    mkdirs(sd("bootloader/res"));
    if (!replaceWith(tmp, sd("bootloader/res/14cfw_logo.bmp"))) return false;
    if (!writeIconBmp(tmp)) return false;
    if (!replaceWith(tmp, sd("bootloader/res/14cfw_icon.bmp"))) return false;
    if (p.replaceIni) {
        // Hekate's own startup logo too
        if (!exists(sd("bootloader/bootlogo.bmp")) || p.backup) {
            backupFile(c, "bootloader/bootlogo.bmp");
            copyFile(sd("bootloader/res/14cfw_logo.bmp"), sd("bootloader/bootlogo.bmp"));
        }
        backupFile(c, "bootloader/hekate_ipl.ini");
        return writeText(sd("bootloader/hekate_ipl.ini"), makeIni(p, e, true));
    }
    mkdirs(sd("bootloader/ini"));
    return writeText(sd("bootloader/ini/14CFW.ini"), makeIni(p, e, false));
}

// ---------------------------------------------------------------- homebrew
static bool installApp(Ctx& c, const HbApp& a) {
    std::vector<Asset> as;
    std::string tag;
    if (!latestAssets(a.repo, as, tag)) return false;
    const Asset* pick = nullptr;
    for (int pass = 0; pass < 4 && !pick; pass++)
        for (auto& x : as) {
            std::string n = lower(x.name);
            bool nro = endsWith(n, ".nro"), zip = endsWith(n, ".zip"), hint = n.find(lower(a.hint)) != std::string::npos;
            if ((pass == 0 && nro && hint) || (pass == 1 && nro) || (pass == 2 && zip && hint) || (pass == 3 && zip)) {
                pick = &x;
                break;
            }
        }
    if (!pick) return false;
    bool isZip = endsWith(lower(pick->name), ".zip");
    std::string dl = sd("switch/14CFW/cache/" + std::string(a.id) + (isZip ? ".zip" : ".nro"));
    if (!httpFile(pick->url, dl, *c.pr, c.pr->frac, 0.0f)) return false;
    bool ok = false;
    std::string folder = std::string("switch/") + a.name;
    if (!isZip) {
        mkdirs(sd(folder));
        std::string out = sd(folder + "/" + a.name + ".nro");
        std::string tmp = out + ".14tmp";
        ok = copyFile(dl, tmp) && replaceWith(tmp, out);
    } else {
        mz_zip_archive z;
        memset(&z, 0, sizeof z);
        if (mz_zip_reader_init_file(&z, dl.c_str(), 0)) {
            bool hasSwitch = false;
            int n = (int)mz_zip_reader_get_num_files(&z);
            for (int i = 0; i < n; i++) {
                mz_zip_archive_file_stat st;
                if (mz_zip_reader_file_stat(&z, i, &st) && startsWith(lower(st.m_filename), "switch/")) hasSwitch = true;
            }
            if (hasSwitch) {
                mz_zip_reader_end(&z);
                ok = extractZip(c, dl, false) > 0;
            } else {
                for (int i = 0; i < n; i++) {
                    mz_zip_archive_file_stat st;
                    if (!mz_zip_reader_file_stat(&z, i, &st) || mz_zip_reader_is_file_a_directory(&z, i)) continue;
                    std::string rel = st.m_filename;
                    if (!endsWith(lower(rel), ".nro") || rel.find("..") != std::string::npos) continue;
                    std::string base = rel.substr(rel.find_last_of('/') == std::string::npos ? 0 : rel.find_last_of('/') + 1);
                    mkdirs(sd(folder));
                    std::string out = sd(folder + "/" + base), tmp = out + ".14tmp";
                    if (mz_zip_reader_extract_to_file(&z, i, tmp.c_str(), 0) && replaceWith(tmp, out)) ok = true;
                }
                mz_zip_reader_end(&z);
            }
        }
    }
    remove(dl.c_str());
    return ok;
}

// ---------------------------------------------------------------- main routine
static std::string stampNow() {
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char b[32];
    strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tmv);
    return b;
}

void runInstall(const Plan& plan, const Env& env, Progress& pr) {
    Ctx c{&plan, &pr, stampNow()};
    auto fail = [&](const std::string& m) {
        std::lock_guard<std::mutex> l(pr.m);
        pr.error = m;
        pr.failed = true;
        pr.done = true;
    };
    bool needNet = plan.ams || plan.hekate || !plan.apps.empty();
    int total = (plan.hekate ? 1 : 0) + (plan.ams ? 1 : 0) + (plan.bootMenu ? 1 : 0) + (int)plan.apps.size();
    if (total == 0) {
        pr.frac = 1;
        pr.addLog("Nothing was selected, so nothing was changed.");
        pr.done = true;
        return;
    }
    if (!env.sdOk) return fail("The SD card can't be read.");
    if (needNet && !env.online) return fail("Your Switch isn't connected to the internet. Connect to Wi-Fi and try again.");
    if (env.sdFree < 200ull * 1024 * 1024 && (plan.ams || plan.hekate)) return fail("Not enough free space on the SD card (need about 200 MB).");
    curl_global_init(CURL_GLOBAL_DEFAULT);
    mkdirs(sd("switch/14CFW/cache"));
    int step = 0;
    auto base = [&]() { return (float)step / (float)total; };
    auto span = [&]() { return 1.0f / (float)total; };
    bool hekateAvail = env.hekate;

    if (plan.hekate && !pr.cancel) {
        pr.set("Installing Hekate", "Looking for the latest version");
        std::vector<Asset> as;
        std::string tag;
        const Asset* pick = nullptr;
        if (!latestAssets("CTCaer/hekate", as, tag)) return fail("Couldn't reach GitHub to get Hekate.");
        for (auto& a : as)
            if (startsWith(lower(a.name), "hekate_ctcaer") && endsWith(lower(a.name), ".zip")) pick = &a;
        if (!pick) return fail("Couldn't find the Hekate download.");
        pr.set("Installing Hekate", "Downloading " + tag);
        std::string zp = sd("switch/14CFW/cache/hekate.zip");
        if (!httpFile(pick->url, zp, pr, base(), span() * 0.8f)) return fail(pr.cancel ? "Cancelled." : "Hekate download failed.");
        pr.set("Installing Hekate", "Unpacking");
        if (extractZip(c, zp, true) < 0) return fail("Hekate package couldn't be opened.");
        zipRootBin(zp, {"bootloader/update.bin", "atmosphere/reboot_payload.bin"}, c);
        remove(zp.c_str());
        pr.addLog("Hekate " + tag + " installed");
        hekateAvail = true;
        step++;
        pr.frac = base();
    }
    if (plan.ams && !pr.cancel) {
        pr.set("Installing Atmosphère", "Looking for the latest version");
        std::vector<Asset> as;
        std::string tag;
        const Asset* pick = nullptr;
        if (!latestAssets("Atmosphere-NX/Atmosphere", as, tag)) return fail("Couldn't reach GitHub to get Atmosphère.");
        for (auto& a : as) {
            std::string n = lower(a.name);
            if (startsWith(n, "atmosphere-") && endsWith(n, ".zip") && n.find("without") == std::string::npos) pick = &a;
        }
        if (!pick) return fail("Couldn't find the Atmosphère download.");
        pr.set("Installing Atmosphère", "Downloading " + tag);
        std::string zp = sd("switch/14CFW/cache/atmosphere.zip");
        if (!httpFile(pick->url, zp, pr, base(), span() * 0.8f)) return fail(pr.cancel ? "Cancelled." : "Atmosphère download failed.");
        pr.set("Installing Atmosphère", "Unpacking (your settings are kept)");
        int n = extractZip(c, zp, false);
        remove(zp.c_str());
        if (n < 0) return fail("Atmosphère package couldn't be opened.");
        pr.addLog("Atmosphère " + tag + " installed");
        step++;
        pr.frac = base();
    }
    if (plan.bootMenu && !pr.cancel) {
        pr.set("Setting up the 14CFW boot menu", "Writing logo, icons and entries");
        if (!hekateAvail) {
            pr.addLog("Boot menu skipped: it needs Hekate");
        } else if (!installBootMenu(c, env)) {
            pr.addLog("Warning: the boot menu couldn't be written");
        } else
            pr.addLog(plan.replaceIni ? "14CFW boot menu installed" : "14CFW added to Hekate's More configs");
        step++;
        pr.frac = base();
    }
    for (auto& id : plan.apps) {
        if (pr.cancel) break;
        for (auto& a : hbApps())
            if (id == a.id) {
                pr.set(std::string("Adding ") + a.name, "Downloading");
                if (installApp(c, a)) pr.addLog(std::string(a.name) + " ready");
                else pr.addLog(std::string("Warning: ") + a.name + " couldn't be installed");
            }
        step++;
        pr.frac = base();
    }
    // tidy
    remove(sd("switch/14CFW/cache/logo.tmp").c_str());
    if (pr.cancel) return fail("Cancelled. Nothing you own was touched.");
    if (c.backups) pr.addLog("Backed up " + std::to_string(c.backups) + " file(s) to switch/14CFW/backup/" + c.stamp);
    pr.addLog("Your games, saves and keys were not touched");
    pr.frac = 1;
    pr.done = true;
}
