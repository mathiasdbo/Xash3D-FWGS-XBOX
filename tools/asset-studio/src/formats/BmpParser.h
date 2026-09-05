#ifndef BMPPARSER_H
#define BMPPARSER_H

#include <cstdint>
#include <string>
#include <vector>

namespace formats {

// Decodes Windows BMP files into flat RGBA8 buffers for the asset baker.
//
// Only 8bpp indexed, uncompressed (BI_RGB) BMPs are supported today - that is
// exactly what Xash3D FWGS's WON menu button atlas (gfx/shell/btns_main.bmp)
// is, and it is the only BMP asset this project bakes. Extending to 24/32bpp
// truecolor or RLE-compressed BMPs is straightforward (same palette-vs-direct
// split TgaParser uses) but deliberately deferred until something real needs
// it - see UiPacker.cpp's readTga() rewrite for the analogous TGA case, which
// DOES need multiple encodings because multiple real shipped assets use them.
class BmpParser {
public:
    // Reads `path` and fills `outRgba` (width*height*4 bytes, row-major,
    // top-down - row 0 is the visual top of the image, matching UiPacker's
    // TGA output convention regardless of BMP's on-disk row order). Returns
    // false (with a diagnostic on stderr) on any I/O error or unsupported
    // encoding; never throws.
    //
    // Alpha is always forced to 255: 8bpp indexed BMP palettes carry no
    // alpha channel, and Xash3D FWGS's own atlas loader
    // (mainui/Btns.cpp:158-162) does the same "fixup alpha" force for
    // exactly this reason.
    static bool read(const std::string& path, std::vector<uint8_t>& outRgba, int& width, int& height);
};

} // namespace formats

#endif // BMPPARSER_H
