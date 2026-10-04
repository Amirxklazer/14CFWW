#pragma once
#include <cstddef>
#include <string>
#include <vector>

// Straight-alpha RGBA picture (bytes are R, G, B, A).
struct Rgba {
    int w = 0, h = 0;
    std::vector<unsigned char> px;
};

enum LogoSize { LOGO_S_BIG, LOGO_S_ICON };

// zlib inflate; implemented in installer.cpp (miniz) so the header-only miniz is only compiled once.
bool zinflate(const unsigned char* z, size_t zlen, unsigned char* out, size_t rawLen);

bool logoLoad(LogoSize s, Rgba& out);

// 720x1280 BMP for Hekate's "logopath". The console panel is portrait, so the file is pre-rotated:
// it shows up upright when you hold the Switch normally.
bool writeBootLogoBmp(const std::string& path);

// 192x192 BMP for Hekate's "icon".
bool writeIconBmp(const std::string& path);
