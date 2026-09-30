#include "UiPacker.h"
#include "PakManager.h"
#include "BmpParser.h"
#include "../utils/TextureOpt.h"
#include "../utils/rgbcx.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <utility>
#include <tuple>
#include <cmath>
#include <cstddef>
#include <cctype>
#include <filesystem>
#include <system_error>

namespace formats {

struct TgaHeader {
    uint8_t idLength;
    uint8_t colorMapType;
    uint8_t imageType;
    uint16_t colorMapFirstIndex;
    uint16_t colorMapLength;
    uint8_t colorMapEntrySize; // bits/entry
    uint16_t xOrigin;
    uint16_t yOrigin;
    uint16_t width;
    uint16_t height;
    uint8_t pixelDepth;
    uint8_t imageDescriptor;
};

// TGA image types this reader supports. background_*.tga (the menu backdrop
// tiles) are Uncompressed_Truecolor; every gfx/shell/ WON menu asset from
// xash-extras (head_*.tga, cb_*.tga, slider.tga, etc.) turned out to be
// RLE-compressed and 8bpp color-mapped instead - verified by reading their
// headers directly, not assumed. A reader that only handled type 2 (the
// original state of this function) could bake the background but not one
// single piece of shell art, and would fail *silently* (return false, no
// exception), so this needs to be verified by hashing decoded output, not by
// "it didn't crash".
enum class TgaImageType : uint8_t {
    NoImageData = 0,
    UncompressedColorMapped = 1,
    UncompressedTruecolor = 2,
    UncompressedGrayscale = 3,
    RleColorMapped = 9,
    RleTruecolor = 10,
    RleGrayscale = 11,
};

namespace {

// Unpacks a TGA RLE (types 9-11) pixel stream into `bytesPerPixel`-sized
// pixels, `pixelCount` of them, written flat into `out`. TGA RLE packets can
// span row boundaries - the stream is one contiguous run over the whole
// image, not row-by-row - so this decodes into one flat buffer first; the
// caller re-slices it into rows and applies the vertical flip afterward,
// exactly like the uncompressed path already did.
bool unpackTgaRle(std::ifstream& file, int pixelCount, int bytesPerPixel, std::vector<uint8_t>& out) {
    out.resize(static_cast<size_t>(pixelCount) * bytesPerPixel);
    int written = 0;
    uint8_t pixel[4];

    while (written < pixelCount) {
        uint8_t packetHeader = 0;
        file.read(reinterpret_cast<char*>(&packetHeader), 1);
        if (!file) return false;

        int count = (packetHeader & 0x7F) + 1;
        bool isRun = (packetHeader & 0x80) != 0;

        if (isRun) {
            file.read(reinterpret_cast<char*>(pixel), bytesPerPixel);
            if (!file) return false;
            for (int i = 0; i < count && written < pixelCount; ++i, ++written) {
                std::memcpy(&out[static_cast<size_t>(written) * bytesPerPixel], pixel, bytesPerPixel);
            }
        } else {
            for (int i = 0; i < count && written < pixelCount; ++i, ++written) {
                file.read(reinterpret_cast<char*>(&out[static_cast<size_t>(written) * bytesPerPixel]), bytesPerPixel);
                if (!file) return false;
            }
        }
    }
    return true;
}

} // namespace

static bool readTga(const std::string& path, std::vector<uint8_t>& outRgba, int& width, int& height) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    TgaHeader header;
    file.read(reinterpret_cast<char*>(&header.idLength), 1);
    file.read(reinterpret_cast<char*>(&header.colorMapType), 1);
    file.read(reinterpret_cast<char*>(&header.imageType), 1);
    file.read(reinterpret_cast<char*>(&header.colorMapFirstIndex), 2);
    file.read(reinterpret_cast<char*>(&header.colorMapLength), 2);
    file.read(reinterpret_cast<char*>(&header.colorMapEntrySize), 1);
    file.read(reinterpret_cast<char*>(&header.xOrigin), 2);
    file.read(reinterpret_cast<char*>(&header.yOrigin), 2);
    file.read(reinterpret_cast<char*>(&header.width), 2);
    file.read(reinterpret_cast<char*>(&header.height), 2);
    file.read(reinterpret_cast<char*>(&header.pixelDepth), 1);
    file.read(reinterpret_cast<char*>(&header.imageDescriptor), 1);
    if (!file) return false;

    const auto imageType = static_cast<TgaImageType>(header.imageType);
    const bool isColorMapped = imageType == TgaImageType::UncompressedColorMapped || imageType == TgaImageType::RleColorMapped;
    const bool isTruecolor = imageType == TgaImageType::UncompressedTruecolor || imageType == TgaImageType::RleTruecolor;
    const bool isGrayscale = imageType == TgaImageType::UncompressedGrayscale || imageType == TgaImageType::RleGrayscale;
    const bool isRle = imageType == TgaImageType::RleColorMapped || imageType == TgaImageType::RleTruecolor || imageType == TgaImageType::RleGrayscale;

    if (!isColorMapped && !isTruecolor && !isGrayscale) {
        std::cerr << "readTga: unsupported image type " << static_cast<int>(header.imageType) << " in " << path << "\n";
        return false;
    }
    if (isColorMapped && header.pixelDepth != 8) {
        std::cerr << "readTga: color-mapped TGA with non-8bpp indices unsupported in " << path << "\n";
        return false;
    }
    if (isTruecolor && header.pixelDepth != 24 && header.pixelDepth != 32) {
        std::cerr << "readTga: unsupported truecolor depth " << static_cast<int>(header.pixelDepth) << " in " << path << "\n";
        return false;
    }
    if (isGrayscale && header.pixelDepth != 8) {
        std::cerr << "readTga: unsupported grayscale depth " << static_cast<int>(header.pixelDepth) << " in " << path << "\n";
        return false;
    }

    file.seekg(header.idLength, std::ios::cur);

    // Color map, if present. All shell assets that carry one use 24bpp (BGR)
    // entries, matching a standard TGA palette; 32bpp (BGRA) is supported too
    // since it's legal and cheap to handle, but nothing on disk needs it.
    std::vector<uint8_t> palette;
    int paletteEntryBytes = 0;
    if (header.colorMapType == 1) {
        if (header.colorMapEntrySize != 24 && header.colorMapEntrySize != 32) {
            std::cerr << "readTga: unsupported color map entry size " << static_cast<int>(header.colorMapEntrySize) << " in " << path << "\n";
            return false;
        }
        paletteEntryBytes = header.colorMapEntrySize / 8;
        palette.resize(static_cast<size_t>(header.colorMapLength) * paletteEntryBytes);
        file.read(reinterpret_cast<char*>(palette.data()), palette.size());
        if (!file) return false;
    } else if (isColorMapped) {
        std::cerr << "readTga: color-mapped image type without a color map in " << path << "\n";
        return false;
    }

    // Sanity-bound dimensions before they drive any arithmetic. header.width/
    // height are uint16_t (max 65535 each); width*height as `int` can
    // overflow well before that (~46341^2). No real menu asset is anywhere
    // close to this large - this exists purely to keep pixelCount, and every
    // size derived from it, inside int32 range.
    constexpr int kMaxDimension = 8192;
    width = header.width;
    height = header.height;
    if (width <= 0 || height <= 0 || width > kMaxDimension || height > kMaxDimension) return false;

    const int srcBpp = header.pixelDepth / 8; // per-pixel bytes in the (possibly indexed) pixel stream
    const int pixelCount = width * height;

    std::vector<uint8_t> srcPixels;
    if (isRle) {
        if (!unpackTgaRle(file, pixelCount, srcBpp, srcPixels)) {
            std::cerr << "readTga: RLE stream truncated/corrupt in " << path << "\n";
            return false;
        }
    } else {
        srcPixels.resize(static_cast<size_t>(pixelCount) * srcBpp);
        file.read(reinterpret_cast<char*>(srcPixels.data()), srcPixels.size());
        if (!file) return false;
    }

    outRgba.resize(static_cast<size_t>(pixelCount) * 4);
    for (int i = 0; i < pixelCount; ++i) {
        const uint8_t* s = &srcPixels[static_cast<size_t>(i) * srcBpp];
        if (isColorMapped) {
            // On-disk pixel values are indices into the CONCEPTUAL full color
            // table; only colorMapLength entries starting at colorMapFirstIndex
            // are actually stored in `palette`, so the effective lookup offset
            // is (index - colorMapFirstIndex), not the raw index. Every asset
            // this project ships has colorMapFirstIndex == 0 (verified against
            // the real files), so this was previously a silent no-op, but the
            // field is part of the format and must be honored for correctness.
            int effectiveIndex = static_cast<int>(s[0]) - static_cast<int>(header.colorMapFirstIndex);
            // Bounds-check: a corrupt/truncated palette, or an index below
            // colorMapFirstIndex, must not read out of bounds. Out-of-range
            // indices decode to opaque black rather than aborting the whole
            // bake over one asset.
            if (effectiveIndex >= 0 &&
                static_cast<size_t>(effectiveIndex) * paletteEntryBytes + paletteEntryBytes <= palette.size()) {
                const uint8_t* p = &palette[static_cast<size_t>(effectiveIndex) * paletteEntryBytes];
                outRgba[i * 4 + 0] = p[2]; // R (TGA palette is BGR/BGRA)
                outRgba[i * 4 + 1] = p[1]; // G
                outRgba[i * 4 + 2] = p[0]; // B
                outRgba[i * 4 + 3] = (paletteEntryBytes == 4) ? p[3] : 255;
            } else {
                outRgba[i * 4 + 0] = 0;
                outRgba[i * 4 + 1] = 0;
                outRgba[i * 4 + 2] = 0;
                outRgba[i * 4 + 3] = 255;
            }
        } else if (isGrayscale) {
            outRgba[i * 4 + 0] = s[0];
            outRgba[i * 4 + 1] = s[0];
            outRgba[i * 4 + 2] = s[0];
            outRgba[i * 4 + 3] = 255;
        } else { // truecolor
            outRgba[i * 4 + 0] = s[2]; // R (TGA is BGR)
            outRgba[i * 4 + 1] = s[1]; // G
            outRgba[i * 4 + 2] = s[0]; // B
            outRgba[i * 4 + 3] = (srcBpp == 4) ? s[3] : 255;
        }
    }

    // TGA might be flipped vertically depending on imageDescriptor bit 5
    if (!(header.imageDescriptor & 0x20)) {
        // Flip vertically
        for (int y = 0; y < height / 2; ++y) {
            for (int x = 0; x < width * 4; ++x) {
                std::swap(outRgba[y * width * 4 + x], outRgba[(height - 1 - y) * width * 4 + x]);
            }
        }
    }

    return true;
}

namespace {

// One tile placement parsed from a real BackgroundLayout.txt-format file -
// `<relative path> fit <x> <y>` per line, path relative to `valve/` (the
// files themselves say "resource/background/...", matching where they
// really live under a Half-Life install's game directory).
struct BackgroundTile {
    std::string relPath;
    int x = 0;
    int y = 0;
};

// Parses the real BackgroundLayout.txt/HD_BackgroundLayout.txt format
// directly - a first `resolution <w> <h>` line, then any number of
// `<path> fit <x> <y>` tile placement lines (blank lines and anything
// before "resolution" ignored). Tile SIZE isn't declared by this format at
// all (real mainui's own LoadSteamBackground reads it from each loaded
// image's own dimensions, not the layout file) - packMenuBackground reads
// it the same way, from each tile's actual TGA header.
//
// Reading this generically (any resolution, any tile count/paths) instead
// of a hardcoded grid means this also transparently supports a
// differently-shaped background for a mod/game conversion with its own
// resource/*BackgroundLayout.txt - not just retail Half-Life's.
bool parseBackgroundLayout(const std::string& path, int& outResW, int& outResH, std::vector<BackgroundTile>& outTiles) {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    bool haveResolution = false;
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream iss(line);
        std::vector<std::string> tokens;
        std::string tok;
        while (iss >> tok) tokens.push_back(tok);
        if (tokens.empty()) continue;

        if (!haveResolution) {
            if (tokens.size() >= 3 && tokens[0] == "resolution") {
                outResW = std::atoi(tokens[1].c_str());
                outResH = std::atoi(tokens[2].c_str());
                haveResolution = outResW > 0 && outResH > 0;
            }
            continue;
        }

        // "<path> fit <x> <y>" - the "fit" keyword itself is real mainui's
        // own scaling-mode hint (LoadSteamBackground's comment: "ignore
        // 'scaled' attribute. What does it mean?" - it never actually uses
        // it either), so it's parsed past, not interpreted. A non-empty
        // line with fewer than 4 tokens is malformed, not a blank line to
        // silently skip - fail loudly instead, matching packMenuAssets's
        // own "a silent decode failure must never reach the mpak" design:
        // silently accepting a near-empty parse here (a corrupt or
        // truncated layout file with, say, 1 of 105 real tile lines
        // intact) would otherwise bake a mostly-black background with no
        // error at all.
        if (tokens.size() < 4) {
            std::cerr << "parseBackgroundLayout: malformed tile line in " << path << ": \"" << line << "\"\n";
            return false;
        }

        BackgroundTile t;
        t.relPath = tokens[0];
        t.x = std::atoi(tokens[2].c_str());
        t.y = std::atoi(tokens[3].c_str());
        outTiles.push_back(t);
    }

    return haveResolution && !outTiles.empty();
}

// XM item 0 stage B (fork-plan.md): writes an uncompressed 32-bit truecolor
// TGA (image type 2) that the real engine's Image_LoadTGA reads correctly
// via the background-tile load path (PIC_Load(..., PIC_NOFLIP_TGA) ->
// IL_DONTFLIP_TGA, img_tga.c) - that path takes the file's on-disk row
// order literally and ignores the descriptor byte's own flip bit entirely
// (`if (!Image_CheckFlag(IL_DONTFLIP_TGA) && attributes & 0x20)` - false
// whenever IL_DONTFLIP_TGA is set, so the attribute is read but never
// acted on). Every real retail background tile has that bit UNSET (0x08,
// checked directly against 800_1_a_loading.tga's own header) yet displays
// correctly through this exact load path - meaning retail's own on-disk
// row order, taken literally with no flip applied, is already what the
// engine expects: row 0 of the file is the top of the image. This writer
// matches that: `rgba`'s row 0 (as this tool's own readTga already
// normalizes it - top-down, canonical) is written first, verbatim, and
// the descriptor byte is set to the same 0x08 retail's own files use, so
// a baked tile is byte-for-byte interchangeable with a retail one from
// this loader's point of view. NOT verified against a generic (flip-
// respecting) TGA viewer - the only consumer that matters for this bake
// is the Xbox engine's own NOFLIP path, confirmed by booting a real disc
// with baked tiles and reading the result off the screen, not assumed
// from the format spec.
bool writeTga(const std::string& path, const std::vector<uint8_t>& rgba, int w, int h) {
    if (w <= 0 || h <= 0 || rgba.size() != static_cast<size_t>(w) * h * 4) return false;

    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;

    uint8_t header[18] = {0};
    header[2] = 2; // uncompressed truecolor
    header[12] = static_cast<uint8_t>(w & 0xFF);
    header[13] = static_cast<uint8_t>((w >> 8) & 0xFF);
    header[14] = static_cast<uint8_t>(h & 0xFF);
    header[15] = static_cast<uint8_t>((h >> 8) & 0xFF);
    header[16] = 32; // bits per pixel
    header[17] = 0x08; // 8 alpha bits, origin/interleave bits 0 - matches retail's own tiles
    f.write(reinterpret_cast<const char*>(header), sizeof(header));

    std::vector<uint8_t> row(static_cast<size_t>(w) * 4);
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = &rgba[static_cast<size_t>(y) * w * 4];
        for (int x = 0; x < w; ++x) {
            // TGA truecolor is stored BGRA, mirroring readTga's own R/B swap.
            row[x * 4 + 0] = src[x * 4 + 2];
            row[x * 4 + 1] = src[x * 4 + 1];
            row[x * 4 + 2] = src[x * 4 + 0];
            row[x * 4 + 3] = src[x * 4 + 3];
        }
        f.write(reinterpret_cast<const char*>(row.data()), row.size());
        if (!f) return false;
    }
    return true;
}

// Bilinear resize, RGBA, straightforward and not performance-critical (this
// runs once per bake on a desktop host, not per frame). Alpha is resampled
// the same as color - correct for premultiplied-adjacent sampling only if
// the source is already premultiplied, which retail's opaque background
// tiles are moot on (alpha is uniformly 255, checked directly) and the
// animated title's own layers are not (real, non-uniform alpha) - a
// straight unpremultiplied bilinear resize of RGBA can bleed color from
// fully-transparent neighboring texels into a partially-transparent edge.
// Not a concern for the background bake (opaque source); flagged here for
// whoever extends this to the title bake next, not silently inherited.
std::vector<uint8_t> resizeBilinear(const std::vector<uint8_t>& src, int srcW, int srcH, int dstW, int dstH) {
    std::vector<uint8_t> dst(static_cast<size_t>(dstW) * dstH * 4);
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return dst;

    const double xRatio = static_cast<double>(srcW) / dstW;
    const double yRatio = static_cast<double>(srcH) / dstH;

    for (int dy = 0; dy < dstH; ++dy) {
        double sy = (dy + 0.5) * yRatio - 0.5;
        int y0 = static_cast<int>(std::floor(sy));
        double fy = sy - y0;
        int y0c = std::min(std::max(y0, 0), srcH - 1);
        int y1c = std::min(std::max(y0 + 1, 0), srcH - 1);

        for (int dx = 0; dx < dstW; ++dx) {
            double sx = (dx + 0.5) * xRatio - 0.5;
            int x0 = static_cast<int>(std::floor(sx));
            double fx = sx - x0;
            int x0c = std::min(std::max(x0, 0), srcW - 1);
            int x1c = std::min(std::max(x0 + 1, 0), srcW - 1);

            for (int c = 0; c < 4; ++c) {
                double top = src[(static_cast<size_t>(y0c) * srcW + x0c) * 4 + c] * (1.0 - fx)
                           + src[(static_cast<size_t>(y0c) * srcW + x1c) * 4 + c] * fx;
                double bot = src[(static_cast<size_t>(y1c) * srcW + x0c) * 4 + c] * (1.0 - fx)
                           + src[(static_cast<size_t>(y1c) * srcW + x1c) * 4 + c] * fx;
                double v = top * (1.0 - fy) + bot * fy;
                dst[(static_cast<size_t>(dy) * dstW + dx) * 4 + c] = static_cast<uint8_t>(std::min(255.0, std::max(0.0, v + 0.5)));
            }
        }
    }
    return dst;
}

} // namespace

bool UiPacker::packMenuBackground(const std::string& inputDir, const std::string& outputPath, bool isXbox) {
    // Real mainui prefers HD_BackgroundLayout.txt over BackgroundLayout.txt
    // when the active mod's gameinfo sets GFL_HD_BACKGROUND
    // (BackgroundBitmap.cpp's LoadSteamBackground) - this project has no
    // per-mod gameinfo flag system, so it always prefers the HD layout when
    // present (real retail Half-Life ships both; the HD one - a much
    // higher-resolution, 7x15-tile background - is what the modern Steam
    // client actually shows) and falls back to the plain layout otherwise.
    int resW = 0, resH = 0;
    std::vector<BackgroundTile> tiles;
    std::string layoutPath = inputDir + "/valve/resource/HD_BackgroundLayout.txt";
    if (!parseBackgroundLayout(layoutPath, resW, resH, tiles)) {
        layoutPath = inputDir + "/valve/resource/BackgroundLayout.txt";
        tiles.clear();
        if (!parseBackgroundLayout(layoutPath, resW, resH, tiles)) {
            std::cerr << "packMenuBackground: no usable BackgroundLayout.txt found under " << inputDir << "/valve/resource/\n";
            return false;
        }
    }
    std::cout << "packMenuBackground: using " << layoutPath << " (" << resW << "x" << resH << ", " << tiles.size() << " tiles)\n";

    const int contentW = resW;
    const int contentH = resH;

    int totalW = contentW;
    int totalH = contentH;

    // Original Xbox (NV2A) requires power-of-two dimensions for swizzled textures
    if (isXbox) {
        auto nextPot = [](int v) {
            int p = 1;
            while (p < v) p <<= 1;
            return p;
        };
        totalW = nextPot(contentW);
        totalH = nextPot(contentH);
    }

    std::vector<uint8_t> finalImg(static_cast<size_t>(totalW) * totalH * 4, 0);

    for (const auto& tile : tiles) {
        std::string fullPath = inputDir + "/valve/" + tile.relPath;

        std::vector<uint8_t> tileRgba;
        int tw, th;
        if (!readTga(fullPath, tileRgba, tw, th)) {
            std::cerr << "Missing or invalid background tile: " << fullPath << "\n";
            continue;
        }

        for (int y = 0; y < th; ++y) {
            for (int x = 0; x < tw; ++x) {
                int destX = tile.x + x;
                int destY = tile.y + y;
                if (destX < 0 || destY < 0 || destX >= contentW || destY >= contentH) continue;

                int destIdx = (destY * totalW + destX) * 4;
                int srcIdx = (y * tw + x) * 4;

                finalImg[destIdx + 0] = tileRgba[srcIdx + 0];
                finalImg[destIdx + 1] = tileRgba[srcIdx + 1];
                finalImg[destIdx + 2] = tileRgba[srcIdx + 2];
                finalImg[destIdx + 3] = 255;
            }
        }
    }

    std::cout << "Compressing background to DXT1...\n";
    std::vector<uint8_t> dxt = TextureOpt::compressToDXT(finalImg.data(), totalW, totalH, false);

    if (isXbox) {
        dxt = TextureOpt::swizzleNV2A(dxt, totalW / 4, totalH / 4, 8);
    }

    // Pinned offsets: this archive has no dheader_t and its descriptor sits
    // at offset 0, sections shaped as a 24-byte meta block then one DXT blob
    // (NOT the map format's dheader_t + sector-padded section walk) - so a
    // map-format field-list change can silently move a field this function
    // writes without this function's own layout changing at all. If one of
    // these fires: the number below is wrong for what MpakHeader6 actually
    // looks like now - fix the OFFSET, and bump MPAK_UI_BACKGROUND_VERSION
    // (MpakHeader.h) since this archive's on-disk layout just changed.
    static_assert(offsetof(MpakHeader6, version) == 4, "MpakHeader6::version moved - bump MPAK_UI_BACKGROUND_VERSION and update this offset");
    static_assert(offsetof(MpakHeader6, headerSize) == 8, "MpakHeader6::headerSize moved - bump MPAK_UI_BACKGROUND_VERSION and update this offset");
    static_assert(offsetof(MpakHeader6, flags) == 16, "MpakHeader6::flags moved - bump MPAK_UI_BACKGROUND_VERSION and update this offset");
    static_assert(offsetof(MpakHeader6, lenTextureData) == 68, "MpakHeader6::lenTextureData moved - bump MPAK_UI_BACKGROUND_VERSION and update this offset");
    static_assert(offsetof(MpakHeader6, count64) == 84, "MpakHeader6::count64 moved - bump MPAK_UI_BACKGROUND_VERSION and update this offset");
    static_assert(offsetof(MpakHeader6, lenArray64) == 88, "MpakHeader6::lenArray64 moved - bump MPAK_UI_BACKGROUND_VERSION and update this offset");
    static_assert(offsetof(MpakHeader6, count256) == 92, "MpakHeader6::count256 moved - bump MPAK_UI_BACKGROUND_VERSION and update this offset");
    static_assert(offsetof(MpakHeader6, lenArray256) == 96, "MpakHeader6::lenArray256 moved - bump MPAK_UI_BACKGROUND_VERSION and update this offset");
    // Not sizeof == 176: appended fields only grow headerSize and move no
    // section, since every archive's sections start at sector 2048 regardless
    // of headerSize. Only the pad below actually depends on the struct
    // fitting inside one sector.
    static_assert(sizeof(MpakHeader6) <= 2048, "MpakHeader6 no longer fits in the first sector - the pad below assumes it does");

    MpakHeader6 header{};
    header.version = MPAK_UI_BACKGROUND_VERSION;
    header.headerSize = static_cast<uint32_t>(sizeof(MpakHeader6));
    header.flags = isXbox ? MPAK_FLAG_XBOX : 0;

    std::vector<uint8_t> meta(24, 0);
    *reinterpret_cast<uint32_t*>(&meta[0]) = totalW;
    *reinterpret_cast<uint32_t*>(&meta[4]) = totalH;
    *reinterpret_cast<uint32_t*>(&meta[8]) = contentW;
    *reinterpret_cast<uint32_t*>(&meta[12]) = contentH;
    *reinterpret_cast<uint32_t*>(&meta[16]) = isXbox ? 1 : 0; // swizzled flag

    if (isXbox) {
        header.count64 = 1;
        header.lenArray64 = static_cast<uint32_t>(meta.size());
        // MpakHeader.h's v8 -> v9 bump removed atlasWidth/atlasHeight/lenAtlas
        // (the map-baking atlas this archive type never used anyway - it
        // always carried its own totalW/totalH inside `meta` above, written
        // a few lines up). `lenTextureData` is repurposed here for this
        // archive's own single DXT blob byte length, a reasonable fit for a
        // v9 field defined as "total bytes of all per-texture DXT1 blobs" -
        // this background has exactly one. Nothing in tools/asset-studio
        // reads it back out; if an engine-side menu-background loader reads
        // this field directly, it needs a matching update outside this
        // tool's scope.
        header.lenTextureData = static_cast<uint32_t>(dxt.size());
    } else {
        header.count64 = 1;
        header.lenArray64 = static_cast<uint32_t>(meta.size());
        header.count256 = 1;
        header.lenArray256 = static_cast<uint32_t>(dxt.size());
    }

    std::ofstream out(outputPath, std::ios::binary);
    if (!out.is_open()) return false;
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    std::vector<char> pad(2048 - sizeof(header), 0);
    out.write(pad.data(), pad.size());

    auto writePadded = [&out](const void* data, size_t size) {
        out.write(reinterpret_cast<const char*>(data), size);
        size_t remainder = size % 2048;
        if (remainder != 0) {
            std::vector<char> p(2048 - remainder, 0);
            out.write(p.data(), p.size());
        }
    };

    writePadded(meta.data(), meta.size());
    writePadded(dxt.data(), dxt.size());

    // header.version, not a literal "v6" - same fix PakManager::save() already
    // made to its own success message (found stale there for the same reason:
    // a hardcoded string can't track a struct field it doesn't read back).
    std::cout << "Successfully serialized UI background archive (format v" << header.version
              << ") to " << outputPath << "\n";
    return true;
}

namespace {

// pixelFormat values for the withDimHeader prefix below. Mirrors
// MpakHeader6::lightmapFormat's own convention ("0 = RGBA8; reserved for
// RGB565/ARGB1555 (Xbox)") rather than inventing a new one - same project,
// same problem (Xbox needs a compact texture format; RGBA8 is 4x the size
// P8/16bpp would be for menu art that has no real use for 32bpp fidelity on
// a 480i CRT). Only RGBA8 is produced today; the field exists so that
// switching a bake to P8 or RGB565 later is a bake-flag change, not a wire
// format break that has to be coordinated across every existing consumer.
enum class MenuPixelFormat : uint32_t {
    Rgba8 = 0,
};

// Every "menu/*.rgba" asset dict entry is a raw pixel blob prefixed with a
// 12-byte little-endian (width, height, format) header. Unlike .spr/.txt
// assets, which carry their own format-native header, a bare pixel buffer
// has no self-describing dimensions or encoding - one shared prefix
// convention here is simpler than inventing a different sidecar per asset.
// (Decoding to raw pixels at bake time - rather than shipping BMP/TGA bytes
// for the engine to decode - mirrors packMenuBackground/
// PakManager::addTexture's existing philosophy: the Rust side should need
// zero image decoders.)
std::vector<uint8_t> withDimHeader(uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba,
                                    MenuPixelFormat format = MenuPixelFormat::Rgba8) {
    uint32_t formatValue = static_cast<uint32_t>(format);
    std::vector<uint8_t> out(12 + rgba.size());
    std::memcpy(&out[0], &w, 4);
    std::memcpy(&out[4], &h, 4);
    std::memcpy(&out[8], &formatValue, 4);
    std::memcpy(&out[12], rgba.data(), rgba.size());
    return out;
}

// Writes an uncompressed 32bpp top-down BMP for visual QA. Only used when
// packMenuAssets is given a debugDumpDir - no Rust loader for menu.mpak
// exists yet (M0 makes no engine changes), so this is how baked art gets
// verified by eye until a later milestone adds one. Never fails loudly: a
// debug aid that can't write its output should warn, not abort the bake.
void writeDebugBmp(const std::string& path, const std::vector<uint8_t>& rgba, int w, int h) {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << "debug dump: could not open " << path << "\n";
        return;
    }

    uint32_t pixelBytes = static_cast<uint32_t>(w) * 4 * static_cast<uint32_t>(h);
    uint32_t fileSize = 14 + 40 + pixelBytes;

    auto u16 = [&f](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    auto u32 = [&f](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto i32 = [&f](int32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };

    f.write("BM", 2);
    u32(fileSize);
    u16(0); u16(0);
    u32(14 + 40); // bfOffBits: no color table on a 32bpp image

    u32(40);      // biSize
    i32(w);
    i32(-h);      // negative = top-down storage, matching our RGBA's row-0-is-top convention
    u16(1);       // biPlanes
    u16(32);      // biBitCount
    u32(0);       // BI_RGB
    u32(pixelBytes);
    i32(0); i32(0); // pels/meter, unused
    u32(0); u32(0);  // biClrUsed, biClrImportant

    for (int y = 0; y < h; ++y) {
        const uint8_t* row = &rgba[static_cast<size_t>(y) * w * 4];
        for (int x = 0; x < w; ++x) {
            uint8_t bgra[4] = { row[x * 4 + 2], row[x * 4 + 1], row[x * 4 + 0], row[x * 4 + 3] };
            f.write(reinterpret_cast<const char*>(bgra), 4);
        }
    }
}

// Blits a decoded RGBA image into a larger RGBA canvas at (dstX, dstY). No
// bounds growth - callers are responsible for sizing the canvas and
// rejecting placements that don't fit (see the "atlas overflow" checks
// below), matching PakManager::addTexture's existing shelf-packer, which
// warns and drops rather than growing or corrupting memory.
void blit(std::vector<uint8_t>& dst, int dstW, int dstX, int dstY,
          const std::vector<uint8_t>& src, int srcW, int srcH) {
    for (int y = 0; y < srcH; ++y) {
        const uint8_t* srow = &src[static_cast<size_t>(y) * srcW * 4];
        uint8_t* drow = &dst[(static_cast<size_t>(dstY + y) * dstW + dstX) * 4];
        std::memcpy(drow, srow, static_cast<size_t>(srcW) * 4);
    }
}

std::string trimManifestLine(std::string line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
        line.pop_back();
    }
    size_t start = line.find_first_not_of(' ');
    if (start == std::string::npos) return "";
    return line.substr(start);
}

} // namespace

// XM item 0 stage B (fork-plan.md): the ref_soft-profile background bake.
// Retail art is authored for 800x600 (or a 3840x1600 HD set neither this
// port's renderer nor its 640x480 screen has any use for); this composites
// whichever layout the caller's own gameDir carries into one canvas exactly
// the way real mainui's LoadSteamBackground would (same tile-parsing rule:
// size comes from each tile's own header, never the layout file), resizes
// the RESULT to 640x480 - not a per-tile resize, which would distort the
// composite - and re-tiles it into ordinary files under the names and
// layout `CMenuBackgroundBitmap::LoadSteamBackground` already reads: a
// rewritten `resource/BackgroundLayout.txt` plus `resource/background/
// bake_<row>_<col>.tga` tiles. No archive, no new loader - this is
// deliberately not UiPacker's own MPAKv6 format (see the plan's own "three
// facts" for why: this engine's mainui loads through PIC_Load/imagelib,
// not through any consumer this tool's archives have).
//
// Writes plain TGA by default (the ref_soft profile); with dxt1, opaque
// DXT1 .dds tiles for ref/nv2a (docs/r3d/divergences.md #131), a quarter
// of the GPU memory of the RGBA32 texture a TGA tile becomes.
std::vector<int> UiPacker::potTileSpans(int total, int maxTile) {
    std::vector<int> spans;
    if (total <= 0 || maxTile <= 0 || (maxTile & (maxTile - 1)) != 0) {
        return spans;
    }
    int remaining = total;
    while (remaining > 0) {
        int span = maxTile;
        while (span > remaining) {
            span >>= 1;
        }
        spans.push_back(span);
        remaining -= span;
    }
    return spans;
}

std::vector<uint8_t> UiPacker::makeDxt1Dds(int width, int height, const std::vector<uint8_t>& blocks) {
    // Field order is img_dds.c's dds_t (engine/common/imagelib/imagelib.h):
    // ident, size, flags, height, width, linearSize, depth, mipMapCount,
    // alphaBitDepth, reserved1[10], pixelformat{size, flags, fourCC,
    // bitcount, 4 masks}, caps{caps1..caps4}, textureStage.
    const uint32_t kDdsdCaps = 0x1, kDdsdHeight = 0x2, kDdsdWidth = 0x4, kDdsdPixelFormat = 0x1000,
                   kDdsdLinearSize = 0x80000, kDdpfFourCC = 0x4, kDdsCapsTexture = 0x1000;
    std::vector<uint8_t> out(128 + blocks.size(), 0);
    auto put = [&out](size_t off, uint32_t v) { std::memcpy(&out[off], &v, 4); };
    put(0, 0x20534444u);               // "DDS "
    put(4, 124);                       // dwSize: the header minus the ident
    put(8, kDdsdCaps | kDdsdHeight | kDdsdWidth | kDdsdPixelFormat | kDdsdLinearSize);
    put(12, static_cast<uint32_t>(height));
    put(16, static_cast<uint32_t>(width));
    put(20, static_cast<uint32_t>(blocks.size()));
    put(76, 32);                       // pixel format dwSize
    put(80, kDdpfFourCC);
    put(84, 0x31545844u);              // "DXT1"
    put(108, kDdsCapsTexture);
    std::memcpy(&out[128], blocks.data(), blocks.size());
    return out;
}

bool UiPacker::bakeSkyArt(const std::string& gameDir, const std::string& outDir, int& setsWritten) {
    // r_skyBoxSuffix, engine/client/dll_int/ref_common.c:33.
    static const char* const kSides[6] = {"rt", "bk", "lf", "ft", "up", "dn"};
    setsWritten = 0;

    const std::filesystem::path envDir = std::filesystem::path(gameDir) / "gfx" / "env";
    std::error_code ec;
    if (!std::filesystem::is_directory(envDir, ec)) {
        std::cerr << "bakeSkyArt: no " << envDir.string() << " directory\n";
        return false;
    }

    // Base names with a .tga for some side, as spelled on disk (lower-cased
    // for matching; the disc is case-insensitive, the spelling is kept).
    std::vector<std::pair<std::string, std::string>> bases; // {lower, as on disk}
    for (const auto& entry : std::filesystem::directory_iterator(envDir, ec)) {
        if (!entry.is_regular_file()) continue;
        std::string name = entry.path().filename().string();
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower.size() <= 6 || lower.compare(lower.size() - 4, 4, ".tga") != 0) continue;
        std::string stemLower = lower.substr(0, lower.size() - 4);
        for (const char* side : kSides) {
            if (stemLower.size() > 2 && stemLower.compare(stemLower.size() - 2, 2, side) == 0) {
                std::string baseLower = stemLower.substr(0, stemLower.size() - 2);
                std::string base = name.substr(0, baseLower.size());
                bool seen = false;
                for (const auto& b : bases) seen = seen || b.first == baseLower;
                if (!seen) bases.emplace_back(baseLower, base);
                break;
            }
        }
    }
    std::sort(bases.begin(), bases.end());
    if (ec) {
        std::cerr << "bakeSkyArt: listing " << envDir.string() << " failed: " << ec.message() << "\n";
        return false;
    }

    const std::filesystem::path outEnv = std::filesystem::path(outDir) / "gfx" / "env";
    std::filesystem::create_directories(outEnv, ec);
    if (ec) {
        std::cerr << "bakeSkyArt: could not create " << outEnv.string() << ": " << ec.message() << "\n";
        return false;
    }

    rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal); // the decoder model the world and menu DXT1 bake with

    for (const auto& base : bases) {
        // Find each side's file case-insensitively (same base spelling
        // normally, but a retail set is not guaranteed consistent).
        std::string sidePaths[6];
        bool complete = true;
        for (int s = 0; s < 6 && complete; ++s) {
            std::string want = base.first + kSides[s] + ".tga";
            for (const auto& entry : std::filesystem::directory_iterator(envDir, ec)) {
                std::string name = entry.path().filename().string();
                std::string lower = name;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lower == want) {
                    sidePaths[s] = entry.path().string();
                    break;
                }
            }
            complete = !sidePaths[s].empty();
        }
        if (!complete) {
            std::cout << "bakeSkyArt: " << base.second << ": incomplete .tga set, left as is\n";
            continue;
        }

        std::vector<std::vector<uint8_t>> dds(6);
        bool bakeable = true;
        for (int s = 0; s < 6; ++s) {
            std::vector<uint8_t> rgba;
            int w = 0, h = 0;
            if (!readTga(sidePaths[s], rgba, w, h)) {
                std::cerr << "bakeSkyArt: could not decode " << sidePaths[s] << "\n";
                return false;
            }
            const bool pot = (w & (w - 1)) == 0 && (h & (h - 1)) == 0;
            if (!pot || w < 4 || h < 4) {
                std::cout << "bakeSkyArt: " << base.second << ": side " << kSides[s] << " is " << w << "x" << h
                          << ", not power-of-two whole DXT blocks - set left as TGA\n";
                bakeable = false;
                break;
            }
            // Opaque: the engine draws the box with blending and alpha
            // test off (gl_warp.c:340-341) - no block may take BC1's
            // transparent selector.
            for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
            std::vector<uint8_t> blocks = TextureOpt::compressToDXT(rgba.data(), w, h, false);
            dds[s] = makeDxt1Dds(w, h, blocks);
        }
        if (!bakeable) continue;

        for (int s = 0; s < 6; ++s) {
            std::filesystem::path out = outEnv / (base.second + kSides[s] + ".dds");
            std::ofstream f(out, std::ios::binary);
            f.write(reinterpret_cast<const char*>(dds[s].data()), static_cast<std::streamsize>(dds[s].size()));
            if (!f.good()) {
                std::cerr << "bakeSkyArt: could not write " << out.string() << "\n";
                return false;
            }
        }
        std::cout << "bakeSkyArt: " << base.second << ": 6 sides -> DXT1 .dds\n";
        ++setsWritten;
    }

    std::cout << "bakeSkyArt: " << setsWritten << " sky set(s) baked into " << outEnv.string() << "\n";
    return true;
}

bool UiPacker::bakeBackgroundArt(const std::string& gameDir, const std::string& outDir,
                                  int canvasW, int canvasH, int tileSize, bool dxt1) {
    if (dxt1 && (canvasW % 4 != 0 || canvasH % 4 != 0 || potTileSpans(canvasW, tileSize).empty())) {
        std::cerr << "bakeBackgroundArt: DXT1 needs a canvas in whole 4x4 blocks and a power-of-two tile size ("
                   << canvasW << "x" << canvasH << ", tile " << tileSize << ")\n";
        return false;
    }
    int resW = 0, resH = 0;
    std::vector<BackgroundTile> tiles;
    std::string layoutPath = gameDir + "/resource/BackgroundLayout.txt";
    if (!parseBackgroundLayout(layoutPath, resW, resH, tiles)) {
        std::cerr << "bakeBackgroundArt: no usable " << layoutPath << "\n";
        return false;
    }
    std::cout << "bakeBackgroundArt: compositing " << layoutPath << " (" << resW << "x" << resH
               << ", " << tiles.size() << " tiles)\n";

    std::vector<uint8_t> canvas(static_cast<size_t>(resW) * resH * 4, 0);
    for (const auto& tile : tiles) {
        std::string fullPath = gameDir + "/" + tile.relPath;
        std::vector<uint8_t> tileRgba;
        int tw = 0, th = 0;
        if (!readTga(fullPath, tileRgba, tw, th)) {
            std::cerr << "bakeBackgroundArt: missing or unreadable tile " << fullPath << "\n";
            return false;
        }
        if (tile.x < 0 || tile.y < 0 || tile.x + tw > resW || tile.y + th > resH) {
            std::cerr << "bakeBackgroundArt: tile " << fullPath << " does not fit the declared "
                       << resW << "x" << resH << " canvas at (" << tile.x << "," << tile.y << ")\n";
            return false;
        }
        blit(canvas, resW, tile.x, tile.y, tileRgba, tw, th);
    }

    std::cout << "bakeBackgroundArt: resizing " << resW << "x" << resH << " -> "
               << canvasW << "x" << canvasH << "\n";
    std::vector<uint8_t> resized = resizeBilinear(canvas, resW, resH, canvasW, canvasH);

    std::string outResourceDir = outDir + "/resource";
    std::string outBgDir = outResourceDir + "/background";
#if defined(_WIN32)
    system(("mkdir \"" + outBgDir + "\" 2>nul").c_str());
#else
    system(("mkdir -p \"" + outBgDir + "\"").c_str());
#endif

    std::ofstream layoutOut(outResourceDir + "/BackgroundLayout.txt");
    if (!layoutOut.is_open()) {
        std::cerr << "bakeBackgroundArt: could not create " << outResourceDir << "/BackgroundLayout.txt\n";
        return false;
    }
    layoutOut << "resolution\t" << canvasW << "\t" << canvasH << "\n\n";

    // TGA keeps the fixed tileSize grid (edge tiles take the remainder);
    // DXT1 needs power-of-two tiles (see the header), so its grid is the
    // greedy power-of-two spans of each axis.
    std::vector<int> colSpans, rowSpans;
    if (dxt1) {
        colSpans = potTileSpans(canvasW, tileSize);
        rowSpans = potTileSpans(canvasH, tileSize);
        rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal); // the same decoder model PakManager bakes world DXT1 with
    } else {
        for (int x = 0; x < canvasW; x += tileSize) colSpans.push_back(std::min(tileSize, canvasW - x));
        for (int y = 0; y < canvasH; y += tileSize) rowSpans.push_back(std::min(tileSize, canvasH - y));
    }

    int tileCount = 0;
    int ty = 0;
    for (size_t row = 0; row < rowSpans.size(); ty += rowSpans[row], ++row) {
        int th = rowSpans[row];
        int tx = 0;
        for (size_t col = 0; col < colSpans.size(); tx += colSpans[col], ++col) {
            int tw = colSpans[col];

            std::vector<uint8_t> tileBuf(static_cast<size_t>(tw) * th * 4);
            for (int y = 0; y < th; ++y) {
                std::memcpy(&tileBuf[static_cast<size_t>(y) * tw * 4],
                            &resized[(static_cast<size_t>(ty + y) * canvasW + tx) * 4],
                            static_cast<size_t>(tw) * 4);
            }

            char name[64];
            std::snprintf(name, sizeof(name), "bake_%d_%d.%s", static_cast<int>(row), static_cast<int>(col),
                           dxt1 ? "dds" : "tga");
            std::string tilePath = outBgDir + "/" + name;
            bool written = false;
            if (dxt1) {
                // The background is opaque (mainui draws it with PIC_Draw,
                // no blending): force alpha to 255 so no block is offered
                // BC1's transparent selector.
                for (size_t i = 3; i < tileBuf.size(); i += 4) tileBuf[i] = 255;
                std::vector<uint8_t> blocks = TextureOpt::compressToDXT(tileBuf.data(), tw, th, false);
                std::vector<uint8_t> dds = makeDxt1Dds(tw, th, blocks);
                std::ofstream f(tilePath, std::ios::binary);
                f.write(reinterpret_cast<const char*>(dds.data()), static_cast<std::streamsize>(dds.size()));
                written = f.good();
            } else {
                written = writeTga(tilePath, tileBuf, tw, th);
            }
            if (!written) {
                std::cerr << "bakeBackgroundArt: could not write " << tilePath << "\n";
                return false;
            }
            layoutOut << "resource/background/" << name << "\tfit\t" << tx << "\t" << ty << "\n";
            ++tileCount;
        }
    }
    layoutOut.flush();
    if (!layoutOut.good()) {
        std::cerr << "bakeBackgroundArt: write failed to " << outResourceDir << "/BackgroundLayout.txt\n";
        return false;
    }

    std::cout << "bakeBackgroundArt: wrote " << tileCount << " tile(s) to " << outBgDir
               << " and " << outResourceDir << "/BackgroundLayout.txt\n";
    return true;
}

bool UiPacker::packMenuAssets(const std::string& xashExtrasDir, const std::string& bannerManifestPath,
                               const std::string& outputPath, const std::string& debugDumpDir,
                               const std::string& resourceDir) {
    const std::string shellDir = xashExtrasDir + "/gfx/shell/";
    const std::string fontsDir = xashExtrasDir + "/gfx/fonts/";

    PakManager pak;
    size_t packedCount = 0;
    size_t failedCount = 0;

    // --- Button atlas: btns_main.bmp -> one POT RGBA8 atlas ---
    // Cell/state layout matches mainui/Btns.cpp exactly: 26px tall per state,
    // 3 states (normal/focus/pressed) stacked per button, read from BmpParser's
    // already-top-down output. 13x6 = 78 cells >= the 71 buttons the retail
    // atlas actually carries, and 2048x512 is POT for NV2A (Xbox swizzling).
    {
        std::vector<uint8_t> srcRgba;
        int srcW, srcH;
        if (BmpParser::read(shellDir + "btns_main.bmp", srcRgba, srcW, srcH)) {
            const int stateH = 26;
            const int cellW = srcW;
            const int cellH = stateH * 3;
            const int buttonCount = srcH / cellH;
            const int cols = 13;
            const int atlasW = 2048, atlasH = 512;

            // Must check the FULL row width (cols*cellW), not just a single
            // cell - a single-cell check alone passes for a button.bmp wider
            // than expected and then overruns the atlas row for high-column
            // buttons, corrupting memory past the end of the atlas buffer for
            // buttons placed in its last row.
            if (buttonCount <= 0 || cellW > atlasW || cols * cellW > atlasW ||
                (((buttonCount + cols - 1) / cols) * cellH) > atlasH) {
                std::cerr << "packMenuAssets: btns_main.bmp geometry (" << srcW << "x" << srcH
                          << ", " << buttonCount << " buttons) doesn't fit the " << atlasW << "x" << atlasH << " atlas\n";
                ++failedCount;
            } else {
                std::vector<uint8_t> atlas(static_cast<size_t>(atlasW) * atlasH * 4, 0);
                std::string sidecar;

                for (int id = 0; id < buttonCount; ++id) {
                    int col = id % cols;
                    int row = id / cols;
                    int destX = col * cellW;
                    int destY = row * cellH;

                    for (int state = 0; state < 3; ++state) {
                        int srcY = id * cellH + state * stateH;
                        for (int line = 0; line < stateH; ++line) {
                            const uint8_t* srow = &srcRgba[static_cast<size_t>(srcY + line) * cellW * 4];
                            uint8_t* drow = &atlas[(static_cast<size_t>(destY + state * stateH + line) * atlasW + destX) * 4];
                            std::memcpy(drow, srow, static_cast<size_t>(cellW) * 4);
                        }
                        // Zero the bottom visual row of each state - some
                        // fork atlases (gearbox) ship a 1px-misaligned last
                        // row here; mainui/Btns.cpp:166-171 clears it
                        // unconditionally rather than trusting alignment,
                        // and we do the same for the same reason.
                        uint8_t* lastRow = &atlas[(static_cast<size_t>(destY + state * stateH + stateH - 1) * atlasW + destX) * 4];
                        std::memset(lastRow, 0, static_cast<size_t>(cellW) * 4);
                    }

                    char line[64];
                    snprintf(line, sizeof(line), "%d %d %d %d %d\n", id, destX, destY, cellW, cellH);
                    sidecar += line;
                }

                pak.addAsset("menu/btns_main.rgba", withDimHeader(atlasW, atlasH, atlas));
                pak.addAsset("menu/btns_main.txt", std::vector<uint8_t>(sidecar.begin(), sidecar.end()));
                if (!debugDumpDir.empty()) writeDebugBmp(debugDumpDir + "/btns_main_atlas.bmp", atlas, atlasW, atlasH);
                packedCount += 2;
                std::cout << "packMenuAssets: baked " << buttonCount << " WON buttons (3 states each) into " << atlasW << "x" << atlasH << " atlas\n";
            }
        } else {
            std::cerr << "packMenuAssets: failed to decode btns_main.bmp\n";
            ++failedCount;
        }
    }

    // --- Dialog banners: head_*.tga, listed in the manifest, baked 1:1 ---
    // Not atlased - only one banner is ever visible at a time, so there's no
    // batching benefit, and keeping them standalone means a screen only pays
    // for the one texture upload it actually needs.
    {
        std::ifstream manifest(bannerManifestPath);
        if (!manifest) {
            std::cerr << "packMenuAssets: could not open banner manifest: " << bannerManifestPath << "\n";
            ++failedCount;
        } else {
            std::string rawLine;
            while (std::getline(manifest, rawLine)) {
                std::string name = trimManifestLine(rawLine);
                if (name.empty() || name[0] == '#') continue;

                std::vector<uint8_t> rgba;
                int w, h;
                if (readTga(shellDir + "head_" + name + ".tga", rgba, w, h)) {
                    pak.addAsset("menu/head_" + name + ".rgba", withDimHeader(w, h, rgba));
                    if (!debugDumpDir.empty()) writeDebugBmp(debugDumpDir + "/head_" + name + ".bmp", rgba, w, h);
                    ++packedCount;
                } else {
                    std::cerr << "packMenuAssets: failed to decode banner head_" << name << ".tga\n";
                    ++failedCount;
                }
            }
        }
    }

    // --- Small widget icons: shelf-packed into one RGBA8 sheet ---
    // gamma.tga (288x280, the video gamma-calibration image) is baked
    // standalone alongside the banners above instead of joining this atlas -
    // geometrically it's a one-off full image, not a small reusable icon,
    // and forcing it into a shared sheet would either force the sheet huge
    // or waste most of a shelf row around it.
    {
        static const char* kWidgetNames[] = {
            "btn_favorite", "btn_unfavorite", // 156x78 each, largest - placed first for shelf packing
            "cb_checked", "cb_empty", "cb_over", "cb_down", "cb_disabled",
            "cls_n", "cls_f", "cls_d", "min_n", "min_f", "min_d",
            "larrowdefault", "larrowflyover", "larrowpressed",
            "rarrowdefault", "rarrowflyover", "rarrowpressed",
            "uparrowd", "uparrowf", "uparrowp",
            "dnarrowd", "dnarrowf", "dnarrowp",
            "slider", "lock", "favorite",
        };

        const int atlasW = 256, atlasH = 256;
        std::vector<uint8_t> atlas(static_cast<size_t>(atlasW) * atlasH * 4, 0);
        std::string sidecar;
        int curX = 0, curY = 0, rowH = 0;

        for (const char* name : kWidgetNames) {
            std::vector<uint8_t> rgba;
            int w, h;
            if (!readTga(shellDir + std::string(name) + ".tga", rgba, w, h)) {
                std::cerr << "packMenuAssets: failed to decode widget " << name << ".tga\n";
                ++failedCount;
                continue;
            }

            // A single widget wider than the whole atlas can never fit no
            // matter how many times we wrap to a new row - check that FIRST,
            // separately from the "does it fit on the current row" wrap
            // check below, or an unfittable item falls through into blit()
            // (which trusts its caller and does no bounds checking of its
            // own) and overruns the atlas buffer.
            if (w > atlasW || h > atlasH) {
                std::cerr << "packMenuAssets: widget " << name << " (" << w << "x" << h
                          << ") is larger than the " << atlasW << "x" << atlasH << " atlas, dropped\n";
                ++failedCount;
                continue;
            }

            if (curX + w > atlasW) {
                curX = 0;
                curY += rowH + 1;
                rowH = 0;
            }
            if (curY + h > atlasH) {
                std::cerr << "packMenuAssets: widget atlas overflow, dropped " << name << " (" << atlasW << "x" << atlasH << " too small)\n";
                ++failedCount;
                continue;
            }

            blit(atlas, atlasW, curX, curY, rgba, w, h);
            char line[80];
            snprintf(line, sizeof(line), "%s %d %d %d %d\n", name, curX, curY, w, h);
            sidecar += line;

            curX += w + 1;
            rowH = std::max(rowH, h);
            ++packedCount;
        }

        pak.addAsset("menu/widgets.rgba", withDimHeader(atlasW, atlasH, atlas));
        pak.addAsset("menu/widgets.txt", std::vector<uint8_t>(sidecar.begin(), sidecar.end()));
        if (!debugDumpDir.empty()) writeDebugBmp(debugDumpDir + "/widgets_atlas.bmp", atlas, atlasW, atlasH);
    }

    // --- Gamma calibration image, standalone (see note above) ---
    {
        std::vector<uint8_t> rgba;
        int w, h;
        if (readTga(shellDir + "gamma.tga", rgba, w, h)) {
            pak.addAsset("menu/gamma.rgba", withDimHeader(w, h, rgba));
            if (!debugDumpDir.empty()) writeDebugBmp(debugDumpDir + "/gamma.bmp", rgba, w, h);
            ++packedCount;
        } else {
            std::cerr << "packMenuAssets: failed to decode gamma.tga\n";
            ++failedCount;
        }
    }

    // --- Animated Main-menu logo: resource/logo*.tga, all-or-nothing ---
    // Unlike every asset above, these don't live in xash-extras - they're
    // read from resourceDir, a retail gamedir's valve/resource/ folder (the
    // same directory packMenuBackground reads BackgroundLayout.txt from).
    // Real CMenuAnimatedBanner::TryLoad (controls/AnimatedBanner.cpp) only
    // adds the animated logo to the Main menu if EVERY one of these 11
    // files loads - a mod/conversion that doesn't ship one simply doesn't
    // get an animated logo, which is a per-mod-optional gap, not a build
    // failure (unlike every asset above, which xash-extras always ships).
    // So an empty resourceDir, or any single missing/undecodable logo file,
    // logs a warning and skips this block entirely rather than failing the
    // whole bake - packedCount/failedCount are deliberately left untouched
    // either way.
    if (!resourceDir.empty()) {
        struct LogoAsset {
            const char* file;
            const char* key;
        };
        static const LogoAsset kLogoAssets[] = {
            {"logo.tga", "menu/logo.rgba"},
            {"logo_blip.tga", "menu/logo_blip_0.rgba"},
            {"logo_blip2.tga", "menu/logo_blip_1.rgba"},
            {"logo_big_blurred_0.tga", "menu/logo_blur_0.rgba"},
            {"logo_big_blurred_1.tga", "menu/logo_blur_1.rgba"},
            {"logo_big_blurred_2.tga", "menu/logo_blur_2.rgba"},
            {"logo_big_blurred_3.tga", "menu/logo_blur_3.rgba"},
            {"logo_big_blurred_blip_0.tga", "menu/logo_blur_blip_0.rgba"},
            {"logo_big_blurred_blip_1.tga", "menu/logo_blur_blip_1.rgba"},
            {"logo_big_blurred_blip_2.tga", "menu/logo_blur_blip_2.rgba"},
            {"logo_big_blurred_blip_3.tga", "menu/logo_blur_blip_3.rgba"},
        };

        std::vector<std::pair<std::string, std::vector<uint8_t>>> decoded;
        std::vector<std::tuple<std::string, std::vector<uint8_t>, int, int>> dumps;
        bool allOk = true;
        for (const auto& asset : kLogoAssets) {
            std::vector<uint8_t> rgba;
            int w, h;
            if (!readTga(resourceDir + "/" + asset.file, rgba, w, h)) {
                std::cout << "packMenuAssets: animated logo asset " << asset.file
                          << " unavailable in " << resourceDir << " - Main menu will render without the animated logo\n";
                allOk = false;
                break;
            }
            if (!debugDumpDir.empty()) dumps.emplace_back(asset.file, rgba, w, h);
            decoded.emplace_back(asset.key, withDimHeader(w, h, rgba));
        }

        if (allOk) {
            for (auto& [key, data] : decoded) {
                pak.addAsset(key, data);
            }
            for (auto& [file, rgba, w, h] : dumps) {
                writeDebugBmp(debugDumpDir + "/" + file + ".bmp", rgba, w, h);
            }
            packedCount += decoded.size();
            std::cout << "packMenuAssets: baked " << decoded.size() << " animated-logo assets\n";
        }
    } else {
        std::cout << "packMenuAssets: no --resource-dir given - Main menu will render without the animated logo\n";
    }

    // --- Menu font: tahoma.ttf, passed through byte-for-byte ---
    // Bundled here (not decoded - egui consumes TTF bytes directly via
    // FontData::from_owned) rather than FiraSans (440KB, no fidelity gain
    // for a period-correct GoldSrc UI) or the .fnt bitmap fonts (codepage-
    // locked to cp1251/cp1252).
    {
        std::ifstream font(fontsDir + "tahoma.ttf", std::ios::binary | std::ios::ate);
        if (font) {
            std::streamsize size = font.tellg();
            font.seekg(0, std::ios::beg);
            std::vector<uint8_t> bytes(static_cast<size_t>(size));
            if (font.read(reinterpret_cast<char*>(bytes.data()), size)) {
                pak.addAsset("menu/tahoma.ttf", bytes);
                ++packedCount;
            } else {
                std::cerr << "packMenuAssets: failed to read tahoma.ttf\n";
                ++failedCount;
            }
        } else {
            std::cerr << "packMenuAssets: could not open tahoma.ttf\n";
            ++failedCount;
        }
    }

    std::cout << "packMenuAssets: " << packedCount << " assets packed, " << failedCount << " failed\n";

    // A menu.mpak that's missing pieces is worse than no menu.mpak - it
    // would bake "successfully" and only fail later, at runtime, in a much
    // harder place to diagnose. Skip the save entirely on any failure rather
    // than writing a partial file to outputPath and merely returning false -
    // a caller that only checks stdout instead of the exit code (or a stale
    // file left over from a previous successful bake at the same path)
    // should never be able to mistake a failed bake for a complete one.
    if (failedCount > 0) {
        std::cerr << "packMenuAssets: " << failedCount << " asset(s) failed - not writing " << outputPath << "\n";
        return false;
    }

    return pak.save(outputPath);
}

}
