#include "logo.hpp"
#ifdef DEV_EDITION
#include "logo_data_dev.h"
#else
#include "logo_data.h"
#endif
#include <cstdio>
#include <cstring>

bool logoLoad(LogoSize s, Rgba& out) {
    const EmbImg* e = s == LOGO_S_BIG ? &LOGO_BIG : &LOGO_ICON;
    out.w = e->w;
    out.h = e->h;
    out.px.assign((size_t)e->rawlen, 0);
    if (!zinflate(e->z, e->zlen, out.px.data(), e->rawlen)) {
        out.px.clear();
        return false;
    }
    return true;
}

static void le32(unsigned char* p, unsigned v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

// 32-bit uncompressed BMP header (rows are written bottom-up after it).
static bool bmpHeader(FILE* f, int w, int h) {
    unsigned char hd[54];
    memset(hd, 0, sizeof hd);
    unsigned img = (unsigned)w * (unsigned)h * 4u;
    hd[0] = 'B';
    hd[1] = 'M';
    le32(hd + 2, 54u + img);
    le32(hd + 10, 54u);
    le32(hd + 14, 40u);
    le32(hd + 18, (unsigned)w);
    le32(hd + 22, (unsigned)h);
    hd[26] = 1;
    hd[28] = 32;
    le32(hd + 34, img);
    return fwrite(hd, 1, sizeof hd, f) == sizeof hd;
}

// logo pixel blended over white; returns white outside the logo
static void pixelOverWhite(const Rgba& lg, int ix, int iy, unsigned char& r, unsigned char& g, unsigned char& b) {
    r = g = b = 255;
    if (ix < 0 || iy < 0 || ix >= lg.w || iy >= lg.h) return;
    const unsigned char* s = &lg.px[((size_t)iy * lg.w + ix) * 4];
    int a = s[3];
    r = (unsigned char)((s[0] * a + 255 * (255 - a)) / 255);
    g = (unsigned char)((s[1] * a + 255 * (255 - a)) / 255);
    b = (unsigned char)((s[2] * a + 255 * (255 - a)) / 255);
}

bool writeBootLogoBmp(const std::string& path) {
    Rgba lg;
    if (!logoLoad(LOGO_S_BIG, lg)) return false;
    const int PW = 720, PH = 1280;  // the file, in the panel's own (portrait) orientation
    const int LW = 1280, LH = 720;  // the picture as you see it on the console
    int ox = (LW - lg.w) / 2, oy = (LH - lg.h) / 2;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = bmpHeader(f, PW, PH);
    std::vector<unsigned char> row((size_t)PW * 4);
    for (int py = PH - 1; py >= 0 && ok; py--) {
        for (int px = 0; px < PW; px++) {
            // The console shows this file turned 90 degrees clockwise, so file pixel (px, py)
            // lands on picture pixel (LW-1-py, px).
            unsigned char r, g, b;
            pixelOverWhite(lg, (LW - 1 - py) - ox, px - oy, r, g, b);
            unsigned char* d = &row[(size_t)px * 4];
            d[0] = b;
            d[1] = g;
            d[2] = r;
            d[3] = 255;
        }
        ok = fwrite(row.data(), 1, row.size(), f) == row.size();
    }
    fclose(f);
    return ok;
}

bool writeIconBmp(const std::string& path) {
    Rgba lg;
    if (!logoLoad(LOGO_S_ICON, lg)) return false;
    const int S = 192;
    int ox = (S - lg.w) / 2, oy = (S - lg.h) / 2;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = bmpHeader(f, S, S);
    std::vector<unsigned char> row((size_t)S * 4);
    for (int y = S - 1; y >= 0 && ok; y--) {
        for (int x = 0; x < S; x++) {
            unsigned char r, g, b;
            pixelOverWhite(lg, x - ox, y - oy, r, g, b);
            unsigned char* d = &row[(size_t)x * 4];
            d[0] = b;
            d[1] = g;
            d[2] = r;
            d[3] = 255;
        }
        ok = fwrite(row.data(), 1, row.size(), f) == row.size();
    }
    fclose(f);
    return ok;
}

// Atmosphere's splash lives at 0x400000 in package3: 1280 rows of 768 BGRA pixels (720 used), i.e. the
// 720x1280 portrait panel image, same orientation as Hekate's logopath file.
bool buildPatchedPackage3(const std::string& path, const std::string& outPath, std::string& err) {
    const size_t P3 = 0x800000, OFF = 0x400000, STRIDE = 768;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { err = "package3 not found"; return false; }
    std::vector<unsigned char> d(P3);
    size_t n = fread(d.data(), 1, P3, f);
    unsigned char extra;
    bool bigger = fread(&extra, 1, 1, f) == 1;
    fclose(f);
    if (n != P3 || bigger || memcmp(d.data(), "PK31", 4) != 0) { err = "this Atmosphere version has a different package3 layout"; return false; }
    Rgba lg;
    if (!logoLoad(LOGO_S_BIG, lg)) { err = "logo data failed to load"; return false; }
    const int LW = 1280, LH = 720;
    int ox = (LW - lg.w) / 2, oy = (LH - lg.h) / 2;  // just the 14, centered
    for (int py = 0; py < 1280; py++) {
        unsigned char* row = &d[OFF + (size_t)py * STRIDE * 4];
        for (int px = 0; px < 720; px++) {
            int lx = LW - 1 - py, ly = px;  // landscape position of this panel pixel
            unsigned char r, g, b;
            pixelOverWhite(lg, lx - ox, ly - oy, r, g, b);
            unsigned char* o = row + px * 4;
            o[0] = b; o[1] = g; o[2] = r; o[3] = 255;
        }
        memset(row + 720 * 4, 0, (STRIDE - 720) * 4);
    }
    (void)LH;
    FILE* o = fopen(outPath.c_str(), "wb");
    if (!o) { err = "couldn't write to the SD card"; return false; }
    bool ok = fwrite(d.data(), 1, P3, o) == P3;
    fclose(o);
    if (!ok) { err = "SD card write failed"; return false; }
    return true;
}
