#include "installer.hpp"
#include <switch.h>
#include <curl/curl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <sstream>

#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"
#include "json.hpp"

using json = nlohmann::json;

namespace inst {

static const char* HOME_DIR = "sdmc:/switch/14CFW";
static const char* CACHE_DIR = "sdmc:/switch/14CFW/cache";
static const char* BACKUP_DIR = "sdmc:/switch/14CFW/backup";
static const char* CA_SD = "sdmc:/switch/14CFW/cacert.pem";
static const char* MANIFEST_SD = "sdmc:/switch/14CFW/manifest.json";

static bool g_psm = false, g_romfs = false;

// ------------------------------------------------------------------ small fs helpers
static std::string P(const std::string& rel) { return "sdmc:/" + rel; }
static bool exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}
static void mkdirs(const std::string& full) {  // creates every directory above the last '/'
    size_t pos = 6;                             // skip "sdmc:/"
    while ((pos = full.find('/', pos)) != std::string::npos) {
        mkdir(full.substr(0, pos).c_str(), 0777);
        pos++;
    }
}
static bool copyFile(const std::string& src, const std::string& dst) {
    FILE* in = fopen(src.c_str(), "rb");
    if (!in) return false;
    mkdirs(dst);
    FILE* out = fopen(dst.c_str(), "wb");
    if (!out) { fclose(in); return false; }
    std::vector<char> buf(64 * 1024);
    size_t n;
    bool ok = true;
    while ((n = fread(buf.data(), 1, buf.size(), in)) > 0)
        if (fwrite(buf.data(), 1, n, out) != n) { ok = false; break; }
    fclose(in);
    fclose(out);
    return ok;
}
static bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
static bool endsWith(const std::string& s, const char* e) {
    size_t n = strlen(e);
    return s.size() >= n && s.compare(s.size() - n, n, e) == 0;
}
static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}
static bool wild(const char* pat, const char* s) {  // '*' wildcard, case-insensitive
    while (*pat) {
        if (*pat == '*') {
            while (*pat == '*') pat++;
            if (!*pat) return true;
            for (; *s; s++)
                if (wild(pat, s)) return true;
            return false;
        }
        if (tolower((unsigned char)*pat) != tolower((unsigned char)*s)) return false;
        pat++;
        s++;
    }
    return !*s;
}

std::string humanSize(unsigned long long b) {
    char t[32];
    if (b >= (1ull << 30)) snprintf(t, sizeof t, "%.1f GB", b / 1073741824.0);
    else if (b >= (1ull << 20)) snprintf(t, sizeof t, "%.1f MB", b / 1048576.0);
    else snprintf(t, sizeof t, "%.0f KB", b / 1024.0);
    return t;
}

// ------------------------------------------------------------------ safety rules
// Paths we never write to. This is what keeps games, saves, emuMMC and keys safe.
static bool neverTouch(const std::string& rel) {
    static const char* pre[] = {"Nintendo/", "emuMMC/", "emummc/", "backup/", "switch/14CFW/", "switch/prod.keys",
                                "switch/title.keys", "switch/dev.keys", "Backup/"};
    for (auto p : pre)
        if (startsWith(rel, p)) return true;
    return false;
}
static bool unsafeRel(const std::string& rel) {
    if (rel.empty() || rel[0] == '/' || rel.find("..") != std::string::npos || rel.find(':') != std::string::npos ||
        rel.find('\\') != std::string::npos)
        return true;
    return false;
}
// Config-like files that already exist are kept when "keep my settings" is on.
static bool isConfig(const std::string& rel) {
    static const char* pre[] = {"atmosphere/config/", "atmosphere/hosts/", "bootloader/ini/", "config/"};
    for (auto p : pre)
        if (startsWith(rel, p)) return true;
    if (rel == "bootloader/hekate_ipl.ini" || rel == "bootloader/patches.ini") return true;
    std::string l = lower(rel);
    return endsWith(l, ".ini") || endsWith(l, ".cfg") || endsWith(l, ".conf") || endsWith(l, ".config") ||
           endsWith(l, ".json");
}

// ------------------------------------------------------------------ install context
struct Ctx {
    Options o;
    Progress* p;
    std::string bdir;
    std::vector<std::string> extracted;
};

static bool backupFile(Ctx& c, const std::string& rel) {
    if (!c.o.backup) return true;
    std::string dst = c.bdir + "/" + rel;
    if (!copyFile(P(rel), dst)) {
        c.p->say("  ! could not back up " + rel);
        return false;
    }
    c.p->backedUp++;
    return true;
}

// Decide whether rel may be written. Returns false (and logs) when skipped.
static bool allowWrite(Ctx& c, const std::string& rel) {
    if (unsafeRel(rel)) { c.p->say("  ! unsafe path ignored: " + rel); c.p->skipped++; return false; }
    if (neverTouch(rel)) { c.p->say("  = protected, left alone: " + rel); c.p->skipped++; return false; }
    bool ex = exists(P(rel));
    if (ex && c.o.keepConfigs && isConfig(rel)) {
        c.p->say("  = kept your existing " + rel);
        c.p->skipped++;
        return false;
    }
    if (ex && !backupFile(c, rel)) {
        c.p->say("  ! backup failed, not overwriting " + rel);
        c.p->skipped++;
        return false;
    }
    return true;
}

static bool commitTmp(const std::string& tmp, const std::string& dst) {
    remove(dst.c_str());
    return rename(tmp.c_str(), dst.c_str()) == 0;
}

struct WriteCtx { FILE* f; bool ok; };
static size_t zipWrite(void* op, mz_uint64, const void* buf, size_t n) {
    WriteCtx* w = (WriteCtx*)op;
    if (!w->ok) return 0;
    if (fwrite(buf, 1, n, w->f) != n) { w->ok = false; return 0; }
    return n;
}

static bool extractZip(Ctx& c, const std::string& zipPath, std::string& err) {
    mz_zip_archive za;
    memset(&za, 0, sizeof za);
    if (!mz_zip_reader_init_file(&za, zipPath.c_str(), 0)) { err = "bad zip file"; return false; }
    mz_uint n = mz_zip_reader_get_num_files(&za);
    bool ok = true;
    for (mz_uint i = 0; i < n && ok; i++) {
        if (c.p->cancel) { err = "cancelled"; ok = false; break; }
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&za, i, &st)) continue;
        std::string rel = st.m_filename;
        c.p->sub = (float)i / (float)(n ? n : 1);
        if (st.m_is_directory) {
            if (!unsafeRel(rel) && !neverTouch(rel)) {
                std::string d = P(rel);
                if (!d.empty() && d.back() != '/') d += "/";
                mkdirs(d + "x");
            }
            continue;
        }
        if (!allowWrite(c, rel)) continue;
        std::string dst = P(rel), tmp = dst + ".14tmp";
        mkdirs(dst);
        FILE* f = fopen(tmp.c_str(), "wb");
        if (!f) { c.p->say("  ! cannot write " + rel); c.p->failed++; continue; }
        WriteCtx w{f, true};
        mz_bool r = mz_zip_reader_extract_to_callback(&za, i, zipWrite, &w, 0);
        fclose(f);
        if (!r || !w.ok || !commitTmp(tmp, dst)) {
            remove(tmp.c_str());
            c.p->say("  ! failed: " + rel);
            c.p->failed++;
            ok = false;
            err = "write failed (SD full?)";
            break;
        }
        c.extracted.push_back(rel);
        c.p->ok++;
    }
    mz_zip_reader_end(&za);
    return ok;
}

static bool placeFile(Ctx& c, const std::string& srcPath, const std::string& rel, std::string& err) {
    if (!allowWrite(c, rel)) return true;  // skipped by rule = not an error
    std::string dst = P(rel), tmp = dst + ".14tmp";
    if (!copyFile(srcPath, tmp) || !commitTmp(tmp, dst)) {
        remove(tmp.c_str());
        err = "could not write " + rel;
        c.p->failed++;
        return false;
    }
    c.p->ok++;
    return true;
}

// ------------------------------------------------------------------ network
struct DL {
    FILE* f = nullptr;
    std::string* mem = nullptr;
    Progress* p = nullptr;
};
static size_t dlWrite(char* d, size_t s, size_t n, void* u) {
    DL* x = (DL*)u;
    if (x->f) return fwrite(d, 1, s * n, x->f);
    if (x->mem) x->mem->append(d, s * n);
    return s * n;
}
static int dlProgress(void* u, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
    DL* x = (DL*)u;
    if (x->p) {
        if (x->p->cancel) return 1;
        if (total > 0) x->p->sub = (float)now / (float)total;
    }
    return 0;
}

static bool http(const std::string& url, std::string* mem, FILE* f, Progress* p, std::string& err, bool api) {
    CURL* c = curl_easy_init();
    if (!c) { err = "curl init failed"; return false; }
    DL dl;
    dl.f = f;
    dl.mem = mem;
    dl.p = p;
    curl_slist* h = nullptr;
    if (api) h = curl_slist_append(h, "Accept: application/vnd.github+json");
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, "14CFW/0.1");
    if (h) curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 40L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, dlWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &dl);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, dlProgress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &dl);
    if (exists(CA_SD)) curl_easy_setopt(c, CURLOPT_CAINFO, CA_SD);
    else {
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    if (h) curl_slist_free_all(h);
    curl_easy_cleanup(c);
    if (rc == CURLE_ABORTED_BY_CALLBACK) { err = "cancelled"; return false; }
    if (rc != CURLE_OK) { err = std::string("network: ") + curl_easy_strerror(rc); return false; }
    if (code != 200) { err = "HTTP " + std::to_string(code); return false; }
    return true;
}

struct Asset { std::string name, url; unsigned long long size = 0; };
struct Release { std::string tag; std::vector<Asset> assets; };

static bool fetchRelease(const std::string& repo, Release& out, std::string& err) {
    std::string body;
    if (!http("https://api.github.com/repos/" + repo + "/releases/latest", &body, nullptr, nullptr, err, true)) return false;
    json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("assets") || !j["assets"].is_array()) { err = "bad GitHub reply"; return false; }
    out.tag = j.value("tag_name", "");
    for (auto& a : j["assets"]) {
        Asset x;
        x.name = a.value("name", "");
        x.url = a.value("browser_download_url", "");
        x.size = a.value("size", 0ull);
        if (!x.name.empty() && !x.url.empty()) out.assets.push_back(x);
    }
    return true;
}

static const Asset* pickAsset(const Release& r, const std::string& pat, const std::string& skip) {
    for (auto& a : r.assets) {
        if (!wild(pat.c_str(), a.name.c_str())) continue;
        if (!skip.empty() && wild(skip.c_str(), a.name.c_str())) continue;
        return &a;
    }
    return nullptr;
}

static bool download(Progress& p, const std::string& url, const std::string& path, std::string& err) {
    mkdirs(path);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { err = "cannot create cache file"; return false; }
    p.sub = 0;
    bool ok = http(url, nullptr, f, &p, err, false);
    fclose(f);
    if (!ok) remove(path.c_str());
    return ok;
}

// ------------------------------------------------------------------ manifest
static const char* DEFAULT_MANIFEST = R"JSON({"components":[
{"id":"ams","name":"Atmosphère","desc":"The custom firmware itself (with hbmenu)","repo":"Atmosphere-NX/Atmosphere","asset":"atmosphere-*.zip","skip":"*WITHOUT_MESOSPHERE*","type":"zip","detect":["atmosphere/package3"],"core":true,"default":true,
 "extras":[{"asset":"fusee.bin","to":"bootloader/payloads/fusee.bin"}]},
{"id":"hekate","name":"Hekate + Nyx","desc":"Bootloader and boot menu (also set as reboot payload)","repo":"CTCaer/hekate","asset":"hekate_ctcaer_*.zip","type":"zip","detect":["bootloader/update.bin"],"core":true,"default":true,
 "post":{"pattern":"hekate_ctcaer_*.bin","to":"atmosphere/reboot_payload.bin"}},
{"id":"ovl","name":"Tesla overlay loader","desc":"nx-ovlloader: overlays on top of games","repo":"WerWolv/nx-ovlloader","asset":"nx-ovlloader*.zip","type":"zip","detect":["atmosphere/contents/420000000007E51A/exefs.nsp"],"default":false},
{"id":"tesla","name":"Tesla menu","desc":"Overlay menu (needs the loader above)","repo":"WerWolv/Tesla-Menu","asset":"ovlmenu.zip","type":"zip","detect":["switch/.overlays/ovlmenu.ovl"],"default":false},
{"id":"checkpoint","name":"Checkpoint","desc":"Save backup and restore","repo":"FlagBrew/Checkpoint","asset":"Checkpoint*.nro","type":"file","to":"switch/Checkpoint/Checkpoint.nro","detect":["switch/Checkpoint/Checkpoint.nro"],"default":true},
{"id":"jksv","name":"JKSV","desc":"Save manager","repo":"J-D-K/JKSV","asset":"JKSV*.nro","type":"file","to":"switch/JKSV/JKSV.nro","detect":["switch/JKSV/JKSV.nro"],"default":false},
{"id":"nxshell","name":"NX-Shell","desc":"File manager","repo":"joel16/NX-Shell","asset":"NX-Shell*.nro","type":"file","to":"switch/NX-Shell/NX-Shell.nro","detect":["switch/NX-Shell/NX-Shell.nro"],"default":true},
{"id":"ftpd","name":"ftpd","desc":"FTP server for your SD card","repo":"mtheall/ftpd","asset":"ftpd*.nro","type":"file","to":"switch/ftpd/ftpd.nro","detect":["switch/ftpd/ftpd.nro"],"default":false},
{"id":"appstore","name":"Homebrew App Store","desc":"Browse and install homebrew","repo":"fortheusers/hb-appstore","asset":"appstore.nro","type":"file","to":"switch/appstore/appstore.nro","detect":["switch/appstore/appstore.nro"],"default":true}
]})JSON";

static void parseManifest(const std::string& text, std::vector<Component>& out) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("components") || !j["components"].is_array()) return;
    for (auto& e : j["components"]) {
        if (!e.is_object()) continue;
        Component c;
        c.id = e.value("id", "");
        c.name = e.value("name", c.id);
        c.desc = e.value("desc", "");
        c.repo = e.value("repo", "");
        c.url = e.value("url", "");
        c.asset = e.value("asset", "");
        c.skip = e.value("skip", "");
        c.type = e.value("type", "zip");
        c.to = e.value("to", "");
        c.core = e.value("core", false);
        c.defSel = e.value("default", false);
        if (e.contains("detect") && e["detect"].is_array())
            for (auto& d : e["detect"])
                if (d.is_string()) c.detect.push_back(d.get<std::string>());
        if (e.contains("extras") && e["extras"].is_array())
            for (auto& x : e["extras"])
                if (x.is_object()) c.extras.push_back({x.value("asset", ""), x.value("to", "")});
        if (e.contains("post") && e["post"].is_object()) {
            c.postPattern = e["post"].value("pattern", "");
            c.postTo = e["post"].value("to", "");
        }
        if (c.id.empty() || (c.repo.empty() && c.url.empty())) continue;
        out.push_back(c);
    }
}

void loadManifest(std::vector<Component>& out) {
    out.clear();
    std::ifstream in(MANIFEST_SD);
    if (in) {
        std::stringstream ss;
        ss << in.rdbuf();
        parseManifest(ss.str(), out);
    }
    if (out.empty()) parseManifest(DEFAULT_MANIFEST, out);
    for (auto& c : out) c.selected = c.defSel;
}

// ------------------------------------------------------------------ system info
void init() {
    mkdir("sdmc:/switch", 0777);
    mkdir(HOME_DIR, 0777);
    mkdir(CACHE_DIR, 0777);
    mkdir(BACKUP_DIR, 0777);
    if (R_SUCCEEDED(psmInitialize())) g_psm = true;
    if (R_SUCCEEDED(romfsInit())) {
        g_romfs = true;
        if (!exists(CA_SD)) copyFile("romfs:/cacert.pem", CA_SD);
    }
}
void shutdown() {
    if (g_psm) psmExit();
    if (g_romfs) romfsExit();
}

static std::string findHekateVersion() {
    DIR* d = opendir("sdmc:/");
    if (!d) return "";
    std::string best;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (startsWith(n, "hekate_ctcaer_") && endsWith(n, ".bin")) {
            std::string v = n.substr(14, n.size() - 14 - 4);
            if (v > best) best = v;
        }
    }
    closedir(d);
    return best;
}

void scan(std::vector<Component>& comps, Env& env) {
    for (auto& c : comps) {
        c.installed = !c.detect.empty();
        for (auto& d : c.detect)
            if (!exists(P(d))) c.installed = false;
    }
    env.ams = exists(P("atmosphere/package3")) || exists(P("atmosphere/contents"));
    env.hekate = exists(P("bootloader/update.bin"));
    env.hekateVer = findHekateVersion();
    env.nintendo = exists(P("Nintendo"));
    env.emummc = exists(P("emuMMC")) || exists(P("emummc"));
    env.tlsOk = exists(CA_SD);
    struct statvfs sv;
    env.sdOk = statvfs("sdmc:/", &sv) == 0;
    if (env.sdOk) {
        env.freeBytes = (unsigned long long)sv.f_bavail * sv.f_frsize;
        env.totalBytes = (unsigned long long)sv.f_blocks * sv.f_frsize;
    }
    env.amsRunning = false;
    env.amsVer.clear();
    if (R_SUCCEEDED(splInitialize())) {
        u64 cfg = 0;
        if (R_SUCCEEDED(splGetConfig((SplConfigItem)65000, &cfg)) && cfg) {
            env.amsRunning = true;
            char v[32];
            snprintf(v, sizeof v, "%u.%u.%u", (unsigned)((cfg >> 56) & 0xFF), (unsigned)((cfg >> 48) & 0xFF), (unsigned)((cfg >> 40) & 0xFF));
            env.amsVer = v;
        }
        splExit();
    }
    if (g_psm) {
        u32 pct = 0;
        PsmChargerType ct = PsmChargerType_Unconnected;
        if (R_SUCCEEDED(psmGetBatteryChargePercentage(&pct))) env.battery = (int)pct;
        if (R_SUCCEEDED(psmGetChargerType(&ct))) env.charging = ct != PsmChargerType_Unconnected;
    }
}

// ------------------------------------------------------------------ update check
void checkUpdates(std::vector<Component> comps, Progress& p) {
    int n = (int)comps.size(), i = 0;
    for (auto& c : comps) {
        p.overall = (float)i++ / (float)(n ? n : 1);
        p.setStep("Checking " + c.name);
        if (c.repo.empty()) continue;
        Release r;
        std::string err;
        if (fetchRelease(c.repo, r, err)) {
            std::lock_guard<std::mutex> g(p.mu);
            p.latest.push_back({c.id, r.tag});
        } else {
            p.say(c.name + ": " + err);
        }
        if (p.cancel) break;
    }
    p.overall = 1;
    p.done = true;
}

// ------------------------------------------------------------------ BMP drawing (boot logo and icon)
static float sdBox(float px, float py, float cx, float cy, float hw, float hh, float r) {
    float qx = std::fabs(px - cx) - (hw - r), qy = std::fabs(py - cy) - (hh - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float inside = std::min(std::max(qx, qy), 0.f);
    return std::sqrt(ox * ox + oy * oy) + inside - r;
}
static float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

static void logoPixel(int x, int y, int w, int h, float k, float cx, float cy, unsigned char* bgra) {
    float v = (float)y / (float)h;
    float r = 11 + 12 * v, g = 14 + 14 * v, b = 23 + 30 * v;
    float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
    // ring
    float dx = fx - cx, dy = fy - cy;
    float dist = std::sqrt(dx * dx + dy * dy);
    float rr = 190.f * k, th = 14.f * k;
    float a = clamp01(0.5f - (std::fabs(dist - rr) - th * 0.5f));
    if (a > 0) {
        float t = 0.5f + 0.5f * std::sin(std::atan2(dy, dx) * 1.0f + 0.6f);
        float cr = 25 + (255 - 25) * t, cg = 230 + (122 - 230) * t, cb = 255 + (69 - 255) * t;
        r += (cr - r) * a; g += (cg - g) * a; b += (cb - b) * a;
    }
    float a2 = clamp01(0.5f - (std::fabs(dist - 150.f * k) - 2.f * k)) * 0.45f;
    if (a2 > 0) { r += (255 - r) * a2; g += (255 - g) * a2; b += (255 - b) * a2; }
    // "14" in seven-segment style
    float bw = 100.f * k, bh = 170.f * k, t = 26.f * k;
    struct D { float x0; int segs; };
    D ds[2] = {{cx - 128.f * k, 0b0000110}, {cx + 28.f * k, 0b1100110}};  // 1: b,c   4: b,c,f,g
    for (auto& d : ds) {
        float y0 = cy - bh / 2;
        float mid = y0 + bh / 2;
        float segd = 1e9f;
        auto seg = [&](float sx, float sy, float sw, float sh) {
            float s = sdBox(fx, fy, sx + sw / 2, sy + sh / 2, sw / 2, sh / 2, t * 0.35f);
            if (s < segd) segd = s;
        };
        if (d.segs & 0b0000010) seg(d.x0 + bw - t, y0, t, bh / 2 + t / 2);        // b
        if (d.segs & 0b0000100) seg(d.x0 + bw - t, mid - t / 2, t, bh / 2 + t / 2);  // c
        if (d.segs & 0b0100000) seg(d.x0, y0, t, bh / 2 + t / 2);                  // f
        if (d.segs & 0b1000000) seg(d.x0, mid - t / 2, bw, t);                     // g
        float sa = clamp01(0.5f - segd);
        if (sa > 0) { r += (240 - r) * sa; g += (244 - g) * sa; b += (255 - b) * sa; }
    }
    // underline bar
    float bd = sdBox(fx, fy, cx, cy + 270.f * k, 150.f * k, 3.f * k, 3.f * k);
    float ba = clamp01(0.5f - bd);
    if (ba > 0) {
        float t2 = clamp01((fx - (cx - 150.f * k)) / (300.f * k));
        float cr = 25 + (255 - 25) * t2, cg = 230 + (122 - 230) * t2, cb = 255 + (69 - 255) * t2;
        r += (cr - r) * ba; g += (cg - g) * ba; b += (cb - b) * ba;
    }
    bgra[0] = (unsigned char)std::min(255.f, b);
    bgra[1] = (unsigned char)std::min(255.f, g);
    bgra[2] = (unsigned char)std::min(255.f, r);
    bgra[3] = 255;
}

static bool writeLogoBmp(const std::string& path, int w, int h, float k, float cx, float cy) {
    mkdirs(path);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    unsigned int imgSize = (unsigned)(w * h * 4), fileSize = 54 + imgSize;
    unsigned char hd[54] = {0};
    hd[0] = 'B'; hd[1] = 'M';
    memcpy(hd + 2, &fileSize, 4);
    unsigned int off = 54, dib = 40;
    memcpy(hd + 10, &off, 4);
    memcpy(hd + 14, &dib, 4);
    int iw = w, ih = h;
    memcpy(hd + 18, &iw, 4);
    memcpy(hd + 22, &ih, 4);
    unsigned short planes = 1, bpp = 32;
    memcpy(hd + 26, &planes, 2);
    memcpy(hd + 28, &bpp, 2);
    memcpy(hd + 34, &imgSize, 4);
    bool ok = fwrite(hd, 1, 54, f) == 54;
    std::vector<unsigned char> row((size_t)w * 4);
    for (int y = h - 1; y >= 0 && ok; y--) {  // bottom-up
        for (int x = 0; x < w; x++) logoPixel(x, y, w, h, k, cx, cy, &row[(size_t)x * 4]);
        ok = fwrite(row.data(), 1, row.size(), f) == row.size();
    }
    fclose(f);
    return ok;
}

static std::string bootIni(const Env& env) {
    std::string common = "fss0=atmosphere/package3\nkip1=atmosphere/kips/*\natmosphere=1\n"
                         "logopath=bootloader/bootlogo_14cfw.bmp\nicon=bootloader/res/icon_14cfw.bmp\n";
    std::string s =
        "[config]\nautoboot=0\nautoboot_list=0\nbootwait=3\nbacklight=100\nnoticker=0\nautohosoff=1\nautonogc=1\n"
        "updater2p=1\nbootprotect=0\n\n{14CFW boot menu}\n{}\n";
    s += "[14CFW | Atmosphere]\n" + common + "\n";
    if (env.emummc) s += "[14CFW | Atmosphere emuMMC]\nemummcforce=1\n" + common + "\n";
    s += "[14CFW | Atmosphere sysMMC]\nemummc_force_disable=1\n" + common + "\n";
    s += "{}\n[Stock | Original firmware]\nfss0=atmosphere/package3\nstock=1\nemummc_force_disable=1\n"
         "logopath=bootloader/bootlogo_14cfw.bmp\nicon=bootloader/res/icon_14cfw.bmp\n";
    return s;
}

static bool installBootMenu(Ctx& c, const Env& env, std::string& err) {
    c.p->setStep("Boot menu");
    c.p->say("Boot menu: drawing logo and icon...");
    if (!writeLogoBmp(P("bootloader/bootlogo_14cfw.bmp"), 720, 1280, 1.0f, 360.f, 520.f)) { err = "cannot write boot logo"; return false; }
    if (!writeLogoBmp(P("bootloader/res/icon_14cfw.bmp"), 192, 192, 0.46f, 96.f, 96.f)) { err = "cannot write icon"; return false; }
    std::string rel = c.o.replaceMenu ? "bootloader/hekate_ipl.ini" : "bootloader/ini/14CFW.ini";
    bool ex = exists(P(rel));
    if (ex && !backupFile(c, rel)) { err = "could not back up " + rel; return false; }
    mkdirs(P(rel));
    std::string tmp = P(rel) + ".14tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << bootIni(env);
        if (!out.good()) { err = "cannot write boot menu"; return false; }
    }
    if (!commitTmp(tmp, P(rel))) { err = "cannot save boot menu"; return false; }
    c.p->say(std::string("Boot menu written to ") + rel + (ex ? " (old one backed up)" : ""));
    c.p->ok++;
    return true;
}

// ------------------------------------------------------------------ install
static bool preflight(const Env& env, Progress& p, unsigned long long need) {
    if (env.battery >= 0 && env.battery < 30 && !env.charging) {
        p.say("Battery is below 30%. Plug in the charger first.");
        return false;
    }
    if (!env.sdOk) { p.say("Cannot read the SD card."); return false; }
    if (env.freeBytes < need) {
        p.say("Not enough free space: need about " + humanSize(need) + ", have " + humanSize(env.freeBytes));
        return false;
    }
    return true;
}

static bool installOne(Ctx& c, const Component& comp, float base, float span, std::string& err) {
    Progress& p = *c.p;
    c.extracted.clear();
    Release rel;
    if (!comp.repo.empty()) {
        p.setStep("Looking up " + comp.name);
        if (!fetchRelease(comp.repo, rel, err)) return false;
    }
    struct Job { std::string url, name, to; bool zip; };
    std::vector<Job> jobs;
    auto baseName = [](const std::string& u) {
        size_t s = u.find_last_of('/');
        std::string n = s == std::string::npos ? u : u.substr(s + 1);
        size_t q = n.find('?');
        return q == std::string::npos ? n : n.substr(0, q);
    };
    if (!comp.url.empty()) {
        jobs.push_back({comp.url, baseName(comp.url), comp.to, comp.type == "zip"});
    } else {
        const Asset* a = pickAsset(rel, comp.asset, comp.skip);
        if (!a) { err = "no matching file in latest release (" + rel.tag + ")"; return false; }
        jobs.push_back({a->url, a->name, comp.to, comp.type == "zip"});
    }
    for (auto& ex : comp.extras) {
        const Asset* a = pickAsset(rel, ex.asset, "");
        if (!a) { p.say("  ! extra file " + ex.asset + " not found, skipping"); continue; }
        jobs.push_back({a->url, a->name, ex.to, false});
    }
    p.say("Installing " + comp.name + (rel.tag.empty() ? "" : " " + rel.tag));
    for (size_t j = 0; j < jobs.size(); j++) {
        if (p.cancel) { err = "cancelled"; return false; }
        float jb = base + span * (float)j / (float)jobs.size(), js = span / (float)jobs.size();
        std::string cache = std::string(CACHE_DIR) + "/" + comp.id + "-" + jobs[j].name;
        p.setStep("Downloading " + jobs[j].name);
        p.overall = jb;
        std::string e2;
        if (!download(p, jobs[j].url, cache, e2)) { err = e2; return false; }
        p.overall = jb + js * 0.6f;
        p.setStep("Installing " + jobs[j].name);
        bool ok;
        if (jobs[j].zip) ok = extractZip(c, cache, e2);
        else ok = placeFile(c, cache, jobs[j].to, e2);
        remove(cache.c_str());
        if (!ok) { err = e2; return false; }
        p.overall = jb + js;
    }
    if (!comp.postPattern.empty() && !comp.postTo.empty()) {
        for (auto& rel2 : c.extracted) {
            if (rel2.find('/') == std::string::npos && wild(comp.postPattern.c_str(), rel2.c_str())) {
                std::string e3;
                placeFile(c, P(rel2), comp.postTo, e3);
                p.say("  " + rel2 + " -> " + comp.postTo);
                break;
            }
        }
    }
    return true;
}

void install(std::vector<Component> comps, Options o, Env env, Progress& p) {
    std::vector<Component> todo;
    for (auto& c : comps)
        if (c.selected) todo.push_back(c);
    if (todo.empty() && !o.withBootMenu) { p.say("Nothing selected."); p.done = true; return; }
    if (!preflight(env, p, 300ull << 20)) { p.failedHard = true; p.done = true; return; }

    Ctx c;
    c.o = o;
    c.p = &p;
    char stamp[32];
    snprintf(stamp, sizeof stamp, "%llu", (unsigned long long)time(NULL));
    c.bdir = std::string(BACKUP_DIR) + "/" + stamp;
    p.backupDir = c.bdir;
    p.say("Your games, saves and emuMMC are never touched.");
    if (o.backup) p.say("Backups go to switch/14CFW/backup/" + std::string(stamp));
    if (!env.tlsOk) p.say("Note: no cacert.pem found, downloads are not certificate-checked.");

    int n = (int)todo.size() + (o.withBootMenu ? 1 : 0);
    int idx = 0;
    std::vector<std::string> failedNames;
    for (auto& comp : todo) {
        if (p.cancel) break;
        float base = (float)idx / (float)n, span = 1.f / (float)n;
        std::string err;
        if (!installOne(c, comp, base, span, err)) {
            p.say("  FAILED: " + comp.name + " - " + err);
            failedNames.push_back(comp.name);
            p.failed++;
            if (err == "cancelled") break;
        } else {
            p.say("  done: " + comp.name);
        }
        idx++;
        p.overall = (float)idx / (float)n;
    }
    if (o.withBootMenu && !p.cancel) {
        std::string err;
        if (!installBootMenu(c, env, err)) { p.say("  FAILED: boot menu - " + err); p.failed++; }
        p.overall = 1;
    }
    {
        std::lock_guard<std::mutex> g(p.mu);
        p.summary = std::to_string(p.ok) + " files written, " + std::to_string(p.skipped) + " kept/skipped, " +
                    std::to_string(p.backedUp) + " backed up, " + std::to_string(p.failed) + " problems";
    }
    p.done = true;
}

void installBootMenuOnly(Options o, Env env, Progress& p) {
    Ctx c;
    c.o = o;
    c.p = &p;
    char stamp[32];
    snprintf(stamp, sizeof stamp, "%llu", (unsigned long long)time(NULL));
    c.bdir = std::string(BACKUP_DIR) + "/" + stamp;
    p.backupDir = c.bdir;
    std::string err;
    if (!installBootMenu(c, env, err)) { p.say("FAILED: " + err); p.failed++; }
    p.overall = 1;
    {
        std::lock_guard<std::mutex> g(p.mu);
        p.summary = std::to_string(p.ok) + " files written, " + std::to_string(p.backedUp) + " backed up, " + std::to_string(p.failed) + " problems";
    }
    p.done = true;
}

}  // namespace inst
