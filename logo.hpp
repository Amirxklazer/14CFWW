#pragma once
#include <cstddef>
#include <string>
#include <vector>

// Straight-alpha RGBA picture (bytes are R, G, B, A).
struct Rgba {
    int w = 0, h = 0;
    std::vector<unsigned char> px;
};

enum LogoSize { LOGO_S_BIG, LOGO_S_ICON, LOGO_S_TAG };

// zlib inflate; implemented in installer.cpp (miniz) so the header-only miniz is only compiled once.
bool zinflate(const unsigned char* z, size_t zlen, unsigned char* out, size_t rawLen);

bool logoLoad(LogoSize s, Rgba& out);

// 720x1280 BMP for Hekate's "logopath". The console panel is portrait, so the file is pre-rotated:
// it shows up upright when you hold the Switch normally.
bool writeBootLogoBmp(const std::string& path);

// 192x192 BMP for Hekate's "icon".
bool writeIconBmp(const std::string& path);

// Replaces the Atmosphere boot splash inside an atmosphere/package3 file (1280x720, your 14 + version tag).
// Writes the patched copy to outPath; the original is left alone. Returns false (with err) if the file isn't what we expect.
bool buildPatchedPackage3(const std::string& package3, const std::string& outPath, std::string& err);
