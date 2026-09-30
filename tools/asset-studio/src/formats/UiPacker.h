#ifndef UIPACKER_H
#define UIPACKER_H

#include <string>
#include <vector>

namespace formats {

class UiPacker {
public:
    // Reads the real BackgroundLayout.txt/HD_BackgroundLayout.txt format
    // directly (resolution line + `<path> fit <x> <y>` tile placements) -
    // not a hardcoded grid - and composites every tile it lists into one
    // background image, preferring the HD layout when present (see the
    // .cpp for the full precedence story matching real mainui's own
    // GFL_HD_BACKGROUND check). Because the parser is generic rather than
    // baking in retail Half-Life's specific tile count/naming, this also
    // works unmodified for a mod/game conversion shipping its own
    // resource/*BackgroundLayout.txt with a different resolution or tile
    // layout entirely.
    //
    // One shared background, no Main-vs-other-screens split: real mainui's
    // CMenuBackgroundBitmap::Draw (controls/BackgroundBitmap.cpp) draws a
    // single uiStatic.background identically regardless of which screen is
    // active, with no per-screen background selection anywhere in that
    // reimplementation - an earlier version of this function baked a
    // second "logo-free" variant to try to match a real retail screenshot
    // that appeared to show no logo on non-Main screens, but that wasn't a
    // mechanism verified against source, just a guess, and the real HD
    // background's logo watermark turned out to be small and subtle
    // rather than the large wordmark an earlier (wrong) source asset had
    // baked in - not worth the fragility of per-tile masking to hide.
    static bool packMenuBackground(const std::string& inputDir, const std::string& outputPath, bool isXbox);

    // Bakes the WON menu widget art (button atlas, dialog banners, small
    // widget icons, the menu font) into a standalone asset-dict menu.mpak -
    // separate from packMenuBackground's single-blob background, since this
    // needs name-keyed lookup (PakManager::addAsset), not one fixed image.
    //
    // xashExtrasDir must contain gfx/shell/ and gfx/fonts/ (i.e. point at
    // xash3d-fwgs/3rdparty/extras/xash-extras). bannerManifestPath lists
    // head_*.tga banner names (without extension), one per line, matching
    // the #-comment / blank-line-skipping convention main.cpp's
    // packSprites()/packSounds() already use. debugDumpDir, if non-empty,
    // additionally writes every baked image as an uncompressed BMP there for
    // visual QA - no Rust-side menu.mpak loader exists yet (that's a later
    // milestone), so this is the only way to verify decoded output by eye
    // until then. Returns false if ANY asset from xashExtrasDir failed to
    // decode/pack; callers should treat that as build failure, not a
    // partial-success warning - the whole point of shipping a verified
    // pipeline is that a silent decode failure (readTga's old behavior)
    // must never reach the mpak.
    //
    // resourceDir, unlike every other asset above, points at a retail
    // gamedir's valve/resource/ folder (the same directory
    // packMenuBackground reads BackgroundLayout.txt from) - it bakes the
    // Main-menu-only animated logo (resource/logo*.tga), which is not part
    // of xash-extras at all. May be empty, meaning "no animated logo art
    // available" - that's a per-mod-optional asset (real
    // CMenuAnimatedBanner::TryLoad itself only adds the item if every logo
    // asset loads), not a fatal error, so an empty/missing resourceDir logs
    // a warning and simply omits it from the mpak rather than failing the
    // whole bake.
    static bool packMenuAssets(const std::string& xashExtrasDir, const std::string& bannerManifestPath,
                                const std::string& outputPath, const std::string& debugDumpDir,
                                const std::string& resourceDir = "");

    // XM item 0 stage B (fork-plan.md): bakes a retail gamedir's menu
    // background - whichever of BackgroundLayout.txt/HD_BackgroundLayout.txt
    // it carries - to a canvasW x canvasH canvas re-tiled at tileSize x
    // tileSize, writing ordinary TGA files and a rewritten
    // resource/BackgroundLayout.txt under outDir (NOT an archive - this
    // engine's mainui loads through PIC_Load, not any consumer UiPacker's
    // other archives have). gameDir is a gamedir root (e.g. a Steam
    // install's "valve", not its parent) - the same directory
    // resource/BackgroundLayout.txt already lives under. ref_soft (TGA)
    // profile only; the NV2A/DXT profile is X9's to add once it exists to
    // consume it. Returns false on any missing/malformed input or write
    // failure - never a partial bake.
    //
    // dxt1 (docs/r3d/divergences.md #131): instead of TGA, write each tile
    // as an opaque DXT1 .dds (resource/background/bake_<row>_<col>.dds).
    // ref/nv2a uploads a DXT pic as is and sizes it by the FORMAT word's
    // BASE_SIZE_U/V, which hold log2 - so in this mode every tile is a
    // power of two on both axes: each axis is cut greedily into the
    // largest power-of-two spans no wider than tileSize (potTileSpans),
    // 640 -> 256+256+128 and 480 -> 256+128+64+32, which covers the canvas
    // exactly with no padding. Requires canvasW/canvasH to be multiples
    // of 4 (whole DXT blocks).
    static bool bakeBackgroundArt(const std::string& gameDir, const std::string& outDir,
                                   int canvasW = 640, int canvasH = 480, int tileSize = 256,
                                   bool dxt1 = false);

    // Greedy power-of-two spans covering `total` (each <= maxTile, a power
    // of two itself). Empty if total <= 0 or maxTile is not a power of two.
    static std::vector<int> potTileSpans(int total, int maxTile);

    // A complete .dds file (128-byte header + data) around `blocks`, the
    // DXT1 level-0 blocks of a width x height image, in the exact shape
    // engine/common/imagelib/img_dds.c Image_LoadDDS accepts: FourCC DXT1,
    // DDSD_LINEARSIZE with dwLinearSize == blocks.size(), no mip count,
    // every reserved field 0 (dwReserved1[0] is read back as the image's
    // encode method, img_dds.c:374 - it must stay DXT_ENCODE_DEFAULT).
    static std::vector<uint8_t> makeDxt1Dds(int width, int height, const std::vector<uint8_t>& blocks);

    // X9-X item 14 step 10 (docs/r3d/divergences.md #137): bakes every
    // complete skybox under <gameDir>/gfx/env - the six
    // <name>{rt,bk,lf,ft,up,dn}.tga the engine's R_SetupSky looks for
    // (engine/client/dll_int/ref_common.c:33, :71) - to opaque DXT1 .dds
    // files of the same names under <outDir>/gfx/env, an overlay tree for
    // tools/xbox-xiso.sh --sky-bake. The engine's CheckSkybox tries .dds
    // before .tga (ref_common.c:73), so nothing else changes: ref/nv2a
    // uploads the DXT1 as is (r_image.c nv2a_tex_upload_compressed), 32 KiB
    // a side instead of the 256 KiB RGBA32 texture a 256x256 TGA side
    // becomes. Rows are written top-down, as readTga and the engine's own
    // TGA loader both normalise them (img_tga.c:138-146). A set whose
    // sides are not all power-of-two squares' worth of whole 4x4 blocks
    // (ref/nv2a sizes DXT by log2 BASE_SIZE) is left out and reported -
    // the engine then keeps loading its TGA. Only complete sets are
    // written: CheckSkybox picks one extension for all six sides, so a
    // partial .dds set would be ignored. Returns false on an unreadable
    // or undecodable side or a write failure; `setsWritten` counts the
    // baked sets.
    static bool bakeSkyArt(const std::string& gameDir, const std::string& outDir, int& setsWritten);
};

}

#endif // UIPACKER_H
