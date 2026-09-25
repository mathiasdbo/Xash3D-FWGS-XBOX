#pragma once
// X9-R: the Xbox lightmap atlas packer, moved out of BspParser.cpp's own
// anonymous namespace unchanged so tests/lightmap_atlas_packer_test.cpp can
// exercise it (AGENTS.md: unit tests for every core utility). BspParser.cpp
// is its only production user and pulls it in with a using-directive.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace formats {
namespace lightmap_atlas {

    constexpr uint32_t LM_PAGE_SIZE = 1024;   // luxels per atlas page ROW (the width; matches BLOCK_SIZE_MAX)
    // X9-T: a page is LM_PAGE_SIZE wide but may grow to this many rows.
    // ref/nv2a draws one lightmap page only (it rejects a vertex on any
    // other layer), so a map whose blocks overflowed a square 1024x1024
    // page (disposal, rocket_frenzy) could not draw at all; one taller
    // page keeps every block on layer 0, and atlasHeight() still trims
    // it to the rows used. 4096 is the NV2A's own texture size limit.
    constexpr uint32_t LM_PAGE_MAX_HEIGHT = 4096;
    constexpr uint32_t LM_SAMPLE_SIZE = 16;   // GoldSrc's fixed lightmap luxel granularity
    constexpr uint32_t LM_MAX_LUXELS = 64;    // reject pathological face extents
    // Faces with no real lightmap (sky/liquid/fullbright/anomalous) sample the
    // white end of the ramp strip below instead of getting their own allocation.
    // Width covers the 256-value gradient plus a small white guard band; height
    // is a few rows so a sampled point can be centered away from row edges.
    constexpr uint32_t LM_RAMP_GRADIENT_WIDTH = 256; // one luxel per 0..255 grayscale value
    constexpr uint32_t LM_RAMP_WHITE_WIDTH = 4;       // pure-white guard band after the gradient
    constexpr uint32_t LM_RESERVED_MARGIN = LM_RAMP_GRADIENT_WIDTH + LM_RAMP_WHITE_WIDTH; // 260
    constexpr uint32_t LM_RAMP_HEIGHT = 4;

    struct LightmapPage {
        std::vector<uint8_t> rgba;
        uint32_t cursorX = 0;
        uint32_t cursorY = 0;
        uint32_t rowHeight = 0;
        uint32_t usedHeight = 0; // highest Y written to, for atlas height trimming

        LightmapPage() : rgba(static_cast<size_t>(LM_PAGE_SIZE) * LM_PAGE_MAX_HEIGHT * 4, 255) {}
    };

    struct LightmapAllocation {
        uint32_t page = 0;
        uint32_t blockX = 0; // origin of the (smax+2) x (tmax+2) BORDERED block
        uint32_t blockY = 0;
    };

    // Shelf-packs bordered lightmap blocks into 1024-luxel-wide RGBA8 pages
    // up to LM_PAGE_MAX_HEIGHT rows tall,
    // mirroring the row-cursor packing already used for Xbox texture atlases in
    // PakManager::addTexture. Each block gets a 1-luxel replicated border so
    // bilinear sampling never bleeds across faces (see B4's sampler notes).
    class LightmapAtlasPacker {
    public:
        LightmapAtlasPacker() {
            mPages.emplace_back();

            // Bake a luminance ramp strip into page 0's top-left corner: a
            // 0..255 grayscale gradient (one texel per value) followed by a
            // white guard band. This does double duty:
            //  - It replaces the old flat white reserved corner as the
            //    no-lightmap-data fallback sample point (in the white band).
            //  - Per-vertex studio-model lighting (later phases) can encode an
            //    illumination value by sampling into the gradient instead of
            //    needing a shader change - the existing x2 overbright multiply
            //    stays correct because the gradient's own values already carry
            //    the intended brightness (e.g. a flat-shaded model samples
            //    x=96 for its ~192/510 GoldSrc ambient-only illumination).
            // The page starts fully white (LightmapPage's ctor), so only the
            // gradient portion (not the white band after it) needs writing.
            for (uint32_t y = 0; y < LM_RAMP_HEIGHT; ++y) {
                for (uint32_t x = 0; x < LM_RAMP_GRADIENT_WIDTH; ++x) {
                    uint8_t value = static_cast<uint8_t>(x);
                    size_t off = (static_cast<size_t>(y) * LM_PAGE_SIZE + x) * 4;
                    mPages[0].rgba[off + 0] = value;
                    mPages[0].rgba[off + 1] = value;
                    mPages[0].rgba[off + 2] = value;
                    mPages[0].rgba[off + 3] = 255;
                }
            }

            // Reserve the ramp's full footprint (gradient + white band) so the
            // shelf packer never allocates a real lightmap block over it.
            mPages[0].cursorX = LM_RESERVED_MARGIN;
            mPages[0].rowHeight = LM_RAMP_HEIGHT;
            mPages[0].usedHeight = LM_RAMP_HEIGHT;
        }

        LightmapAllocation allocate(uint32_t smax, uint32_t tmax) {
            uint32_t paddedW = smax + 2;
            uint32_t paddedH = tmax + 2;

            if (mPages.back().cursorX + paddedW > LM_PAGE_SIZE) {
                mPages.back().cursorY += mPages.back().rowHeight;
                mPages.back().cursorX = 0;
                mPages.back().rowHeight = 0;
            }
            if (mPages.back().cursorY + paddedH > LM_PAGE_MAX_HEIGHT) {
                mPages.emplace_back();
            }

            LightmapPage& page = mPages.back();
            LightmapAllocation alloc{static_cast<uint32_t>(mPages.size() - 1), page.cursorX, page.cursorY};

            page.cursorX += paddedW;
            if (paddedH > page.rowHeight) page.rowHeight = paddedH;
            uint32_t bottom = page.cursorY + page.rowHeight;
            if (bottom > page.usedHeight) page.usedHeight = bottom;

            return alloc;
        }

        // Writes an smax x tmax RGB24 block into the page at alloc's bordered
        // origin, then replicates a 1-luxel border around it.
        void writeBlock(const LightmapAllocation& alloc, uint32_t smax, uint32_t tmax, const uint8_t* rgb24) {
            LightmapPage& page = mPages[alloc.page];
            uint32_t destX = alloc.blockX + 1;
            uint32_t destY = alloc.blockY + 1;

            for (uint32_t y = 0; y < tmax; ++y) {
                for (uint32_t x = 0; x < smax; ++x) {
                    size_t destOffset = (static_cast<size_t>(destY + y) * LM_PAGE_SIZE + (destX + x)) * 4;
                    const uint8_t* src = &rgb24[(y * smax + x) * 3];
                    page.rgba[destOffset + 0] = src[0];
                    page.rgba[destOffset + 1] = src[1];
                    page.rgba[destOffset + 2] = src[2];
                    page.rgba[destOffset + 3] = 255;
                }
            }

            // Replicate left/right edges, then top/bottom (incl. corners).
            for (uint32_t y = 0; y < tmax; ++y) {
                size_t rowLeft = (static_cast<size_t>(destY + y) * LM_PAGE_SIZE + (destX - 1)) * 4;
                size_t rowLeftSrc = (static_cast<size_t>(destY + y) * LM_PAGE_SIZE + destX) * 4;
                std::memcpy(&page.rgba[rowLeft], &page.rgba[rowLeftSrc], 4);

                size_t rowRight = (static_cast<size_t>(destY + y) * LM_PAGE_SIZE + (destX + smax)) * 4;
                size_t rowRightSrc = (static_cast<size_t>(destY + y) * LM_PAGE_SIZE + (destX + smax - 1)) * 4;
                std::memcpy(&page.rgba[rowRight], &page.rgba[rowRightSrc], 4);
            }
            for (uint32_t x = 0; x < smax + 2; ++x) {
                size_t top = (static_cast<size_t>(alloc.blockY) * LM_PAGE_SIZE + alloc.blockX + x) * 4;
                size_t topSrc = (static_cast<size_t>(destY) * LM_PAGE_SIZE + alloc.blockX + x) * 4;
                std::memcpy(&page.rgba[top], &page.rgba[topSrc], 4);

                size_t bottom = (static_cast<size_t>(destY + tmax) * LM_PAGE_SIZE + alloc.blockX + x) * 4;
                size_t bottomSrc = (static_cast<size_t>(destY + tmax - 1) * LM_PAGE_SIZE + alloc.blockX + x) * 4;
                std::memcpy(&page.rgba[bottom], &page.rgba[bottomSrc], 4);
            }
        }

        uint32_t pageCount() const { return static_cast<uint32_t>(mPages.size()); }

        // X9-R: allocate every block up front, tallest first, instead of
        // in face order. A shelf packer's waste is the gap between each
        // row's tallest block and the shorter blocks beside it; feeding
        // it blocks sorted by height (then width) makes every row nearly
        // uniform. Measured on the retail corpus: every map that used to
        // spill onto a second 1024x1024 page (11 of them, e.g. boot_camp)
        // now fits on one, using 448-711 of its 1024 rows. Stable order
        // (ties keep request order), so the bake stays deterministic.
        // `sizes` are the UNbordered smax/tmax pairs, exactly what
        // allocate() takes; the result is indexed like `sizes`.
        std::vector<LightmapAllocation> allocateSorted(const std::vector<std::pair<uint32_t, uint32_t>>& sizes) {
            std::vector<size_t> order(sizes.size());
            for (size_t k = 0; k < order.size(); ++k) order[k] = k;
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                if (sizes[a].second != sizes[b].second) return sizes[a].second > sizes[b].second;
                return sizes[a].first > sizes[b].first;
            });
            std::vector<LightmapAllocation> out(sizes.size());
            for (size_t k : order)
                out[k] = allocate(sizes[k].first, sizes[k].second);
            return out;
        }

        // X9-R: the atlas height the baker ships. One page is trimmed to
        // the rows it actually uses (rounded up to a multiple of 4);
        // several pages keep the full LM_PAGE_MAX_HEIGHT, since every
        // layer of one atlas must share a height. (X9-T: the page used to
        // stop at 1024 rows; a single page now runs to 4096, so a second
        // page only opens past 4096 rows, which no retail map reaches.) Only valid once every block has
        // been allocated - the caller normalizes lv by this value, which
        // is why allocation now happens in a pre-pass before any vertex
        // is built.
        uint32_t atlasHeight() const {
            if (mPages.size() != 1)
                return LM_PAGE_MAX_HEIGHT;
            const uint32_t h = (mPages[0].usedHeight + 3u) & ~3u;
            return h < LM_RAMP_HEIGHT ? LM_RAMP_HEIGHT : (h > LM_PAGE_MAX_HEIGHT ? LM_PAGE_MAX_HEIGHT : h);
        }

        // Flattens all pages into one layer-major RGBA8 buffer, each page
        // cut to atlasHeight() rows.
        //
        // X9-R: this used to always return the full LM_PAGE_SIZE rows,
        // because every vertex's lv was divided by LM_PAGE_SIZE before
        // the final height was known - an earlier attempt to trim here
        // scrambled every map's lightmaps for exactly that reason. The
        // caller now allocates every block before building any vertex
        // and divides lv by atlasHeight(), so trimming is safe: the
        // renderer multiplies lv back by the header's lightmapHeight
        // (ref/nv2a nv2a_load_map_geometry), which is this same value.
        std::vector<uint8_t> flatten(uint32_t& outHeight) const {
            outHeight = atlasHeight();

            std::vector<uint8_t> out;
            const size_t pageBytes = static_cast<size_t>(outHeight) * LM_PAGE_SIZE * 4;
            out.reserve(static_cast<size_t>(mPages.size()) * pageBytes);
            for (const auto& p : mPages) {
                out.insert(out.end(), p.rgba.begin(), p.rgba.begin() + pageBytes);
            }
            return out;
        }

    private:
        std::vector<LightmapPage> mPages;
    };

} // namespace lightmap_atlas
} // namespace formats
