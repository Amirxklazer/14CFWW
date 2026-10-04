#include "logo.hpp"
#include "logo_data.h"
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
