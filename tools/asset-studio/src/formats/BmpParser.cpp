#include "BmpParser.h"
#include <fstream>
#include <iostream>

namespace formats {

namespace {

// Little-endian field readers - avoids casting a packed struct over the
// buffer (portability across compilers' struct-packing defaults), matching
// the byte-by-byte style UiPacker.cpp's TGA reader already uses.
uint16_t readU16LE(std::ifstream& f) {
    uint8_t b[2];
    f.read(reinterpret_cast<char*>(b), 2);
    return static_cast<uint16_t>(b[0]) | (static_cast<uint16_t>(b[1]) << 8);
}

uint32_t readU32LE(std::ifstream& f) {
    uint8_t b[4];
    f.read(reinterpret_cast<char*>(b), 4);
    return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
           (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
}

int32_t readI32LE(std::ifstream& f) {
    return static_cast<int32_t>(readU32LE(f));
}

} // namespace

bool BmpParser::read(const std::string& path, std::vector<uint8_t>& outRgba, int& width, int& height) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "BmpParser: could not open " << path << "\n";
        return false;
    }

    // BITMAPFILEHEADER (14 bytes).
    uint16_t bfType = readU16LE(file);
    readU32LE(file); // bfSize - unused, we trust bfOffBits + the info header instead
    readU16LE(file); // bfReserved1
    readU16LE(file); // bfReserved2
    uint32_t bfOffBits = readU32LE(file);

    if (bfType != 0x4D42 /* 'BM' */) {
        std::cerr << "BmpParser: not a BMP file (bad magic): " << path << "\n";
        return false;
    }

    // BITMAPINFOHEADER (or a newer, larger variant - biSize tells us which).
    // We only read the classic 40-byte fields; any extra bytes of a newer
    // header are skipped via bfOffBits below rather than parsed.
    uint32_t biSize = readU32LE(file);
    if (biSize < 40) {
        std::cerr << "BmpParser: unsupported/truncated BITMAPINFOHEADER in " << path << "\n";
        return false;
    }
    int32_t biWidth = readI32LE(file);
    int32_t biHeight = readI32LE(file);
    readU16LE(file); // biPlanes
    uint16_t biBitCount = readU16LE(file);
    uint32_t biCompression = readU32LE(file);
    readU32LE(file); // biSizeImage
    readU32LE(file); // biXPelsPerMeter
    readU32LE(file); // biYPelsPerMeter
    uint32_t biClrUsed = readU32LE(file);
    readU32LE(file); // biClrImportant

    if (!file) {
        std::cerr << "BmpParser: header read failed (truncated file?): " << path << "\n";
        return false;
    }

    // Only the encoding gfx/shell/btns_main.bmp actually uses - see the
    // class comment for why this isn't a general-purpose BMP decoder.
    if (biBitCount != 8 || biCompression != 0 /* BI_RGB */) {
        std::cerr << "BmpParser: unsupported encoding (bitCount=" << biBitCount
                   << ", compression=" << biCompression << ") in " << path << "\n";
        return false;
    }
    // Sanity-bound dimensions before any arithmetic uses them. This guards
    // three things at once: INT32_MIN has no positive int32 representation
    // (negating it below would be signed-overflow UB), width*height*4 could
    // overflow size_t on pathological input, and an unbounded allocation
    // would throw bad_alloc/length_error instead of the documented graceful
    // `return false`. No real menu asset is anywhere close to this large.
    constexpr int32_t kMaxDimension = 16384;
    if (biWidth <= 0 || biWidth > kMaxDimension || biHeight == INT32_MIN ||
        (biHeight >= 0 ? biHeight : -biHeight) > kMaxDimension) {
        std::cerr << "BmpParser: invalid or out-of-range dimensions in " << path << "\n";
        return false;
    }

    width = biWidth;
    const bool bottomUp = biHeight >= 0; // negative biHeight = top-down storage (rare, spec-legal)
    height = biHeight >= 0 ? biHeight : -biHeight;
    if (height <= 0) {
        std::cerr << "BmpParser: invalid height in " << path << "\n";
        return false;
    }

    // Palette: 4 bytes/entry (B, G, R, reserved), immediately after the info
    // header. biClrUsed==0 means "the full 2^bitCount table is present".
    const uint32_t paletteCount = biClrUsed != 0 ? biClrUsed : (1u << biBitCount);
    std::vector<uint8_t> palette(paletteCount * 4);
    file.read(reinterpret_cast<char*>(palette.data()), palette.size());
    if (!file) {
        std::cerr << "BmpParser: palette read failed in " << path << "\n";
        return false;
    }

    // Pixel data starts at bfOffBits, not necessarily immediately after the
    // palette (a writer could pad or use a larger info header) - trust the
    // file header's own offset rather than our running read position.
    file.seekg(bfOffBits, std::ios::beg);
    if (!file) {
        std::cerr << "BmpParser: bfOffBits points outside the file: " << path << "\n";
        return false;
    }

    // BMP rows are padded to a 4-byte boundary.
    const size_t stride = ((static_cast<size_t>(width) * biBitCount + 31) / 32) * 4;
    std::vector<uint8_t> rowBuf(stride);

    outRgba.assign(static_cast<size_t>(width) * height * 4, 0);

    for (int row = 0; row < height; ++row) {
        file.read(reinterpret_cast<char*>(rowBuf.data()), static_cast<std::streamsize>(stride));
        if (!file) {
            // Genuinely truncated pixel data (distinct from harmless trailing
            // padding bytes AFTER all rows, which we simply never read).
            std::cerr << "BmpParser: pixel data truncated at row " << row << " in " << path << "\n";
            return false;
        }

        // Bottom-up BMP storage: file row 0 is the visual bottom, so it maps
        // to output row (height-1). Mirrors mainui/Btns.cpp's own
        // `src_row = src_height - 1 - src_visual_y` flip for this exact file.
        const int destRow = bottomUp ? (height - 1 - row) : row;
        uint8_t* dst = &outRgba[static_cast<size_t>(destRow) * width * 4];

        for (int x = 0; x < width; ++x) {
            uint8_t index = rowBuf[x];
            // biClrUsed can legally be less than 256 (a common BMP writer
            // optimization: "only N of the 256 possible indices are used"),
            // so an index in [0,255] is not guaranteed to be in [0,
            // paletteCount) - bounds-check before dereferencing, same
            // fallback-to-opaque-black convention as readTga's color-map path.
            if (static_cast<uint32_t>(index) < paletteCount) {
                const uint8_t* p = &palette[static_cast<size_t>(index) * 4];
                dst[x * 4 + 0] = p[2]; // R
                dst[x * 4 + 1] = p[1]; // G
                dst[x * 4 + 2] = p[0]; // B
            } else {
                dst[x * 4 + 0] = 0;
                dst[x * 4 + 1] = 0;
                dst[x * 4 + 2] = 0;
            }
            dst[x * 4 + 3] = 255; // no alpha channel in an 8bpp indexed BMP palette
        }
    }

    return true;
}

} // namespace formats
