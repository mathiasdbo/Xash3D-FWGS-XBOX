// Unit test for buildEmbeddedMiptexLump (BspParser.cpp): hand-built
// LUMP_TEXTURES inputs, no retail install needed. Every case here is a
// policy the engine's own texture loader depends on, or a bug review
// found in the first version of the builder:
//  - embedded pixels win over a same-named WAD lump (r_wadtextures
//    defaults to 0, engine/common/model.c; Mod_LoadTextureData only tries
//    WADs when that is set or offsets[0] <= 0, mod_bmodel.c:2834-2835);
//  - a header-only external reference resolves from the WAD;
//  - an unresolved header keeps name and dimensions with zero offsets;
//  - a missing directory entry (dataofs < 0) stays -1;
//  - a later dataofs pointing past the lump must not extend a copy
//    beyond the input (found in review: it used to);
//  - a truncated embedded payload becomes header-only rather than a
//    short copy the engine would over-read.
#include "formats/BspParser.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

using namespace formats;

namespace {

int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); ++failures; } } while (0)

// A complete 8x8 miptex entry: header + 4 mips + count + palette, the
// exact shape a WAD3 lump has. Pixel and palette bytes carry `fill`.
std::vector<uint8_t> makeMiptex(const char* name, uint32_t w, uint32_t h, uint8_t fill, bool withPixels) {
    std::vector<uint8_t> out(sizeof(miptex_t), 0);
    miptex_t* mt = reinterpret_cast<miptex_t*>(out.data());
    std::strncpy(mt->name, name, sizeof(mt->name) - 1);
    mt->width = w;
    mt->height = h;
    if (!withPixels) return out;
    size_t mips = w * h + (w / 2) * (h / 2) + (w / 4) * (h / 4) + (w / 8) * (h / 8);
    mt->offsets[0] = sizeof(miptex_t);
    mt->offsets[1] = mt->offsets[0] + w * h;
    mt->offsets[2] = mt->offsets[1] + (w / 2) * (h / 2);
    mt->offsets[3] = mt->offsets[2] + (w / 4) * (h / 4);
    out.insert(out.end(), mips, fill);
    out.push_back(0x00); out.push_back(0x01); // palette count 256, little-endian
    out.insert(out.end(), 768, fill);
    return out;
}

// LUMP_TEXTURES from a list of entries; a null entry writes dataofs -1.
std::vector<uint8_t> makeLump(const std::vector<const std::vector<uint8_t>*>& entries) {
    std::vector<uint8_t> lump;
    int32_t n = static_cast<int32_t>(entries.size());
    lump.resize(4 + 4 * entries.size(), 0);
    std::memcpy(lump.data(), &n, 4);
    for (size_t i = 0; i < entries.size(); ++i) {
        int32_t ofs = -1;
        if (entries[i]) {
            ofs = static_cast<int32_t>(lump.size());
            lump.insert(lump.end(), entries[i]->begin(), entries[i]->end());
        }
        std::memcpy(lump.data() + 4 + 4 * i, &ofs, 4);
    }
    return lump;
}

const miptex_t* entryAt(const std::vector<uint8_t>& lump, int i, int32_t* ofsOut = nullptr) {
    int32_t ofs;
    std::memcpy(&ofs, lump.data() + 4 + 4 * i, 4);
    if (ofsOut) *ofsOut = ofs;
    if (ofs < 0) return nullptr;
    return reinterpret_cast<const miptex_t*>(lump.data() + ofs);
}

WadLump makeWadLump(const char* name, std::vector<uint8_t> data) {
    WadLump l{};
    std::strncpy(l.name, name, sizeof(l.name) - 1);
    l.data = std::move(data);
    return l;
}

} // namespace

int main() {
    // --- precedence: embedded pixels beat a same-named WAD lump ---
    {
        auto embedded = makeMiptex("wall", 8, 8, 0xAA, true);
        auto lump = makeLump({ &embedded });
        WadLump wad = makeWadLump("wall", makeMiptex("wall", 8, 8, 0xBB, true));
        std::unordered_map<std::string, const WadLump*> wads{ { "wall", &wad } };

        auto out = buildEmbeddedMiptexLump(lump, wads);
        const miptex_t* mt = entryAt(out, 0);
        CHECK(mt != nullptr, "embedded-wins: entry present");
        CHECK(mt && mt->offsets[0] == sizeof(miptex_t), "embedded-wins: pixels kept");
        CHECK(mt && reinterpret_cast<const uint8_t*>(mt)[mt->offsets[0]] == 0xAA, "embedded-wins: embedded bytes, not the WAD's");
    }

    // --- an external reference (header, no pixels) resolves from the WAD ---
    {
        auto external = makeMiptex("floor", 8, 8, 0, false);
        auto lump = makeLump({ &external });
        WadLump wad = makeWadLump("floor", makeMiptex("floor", 8, 8, 0xCC, true));
        std::unordered_map<std::string, const WadLump*> wads{ { "floor", &wad } };

        auto out = buildEmbeddedMiptexLump(lump, wads);
        const miptex_t* mt = entryAt(out, 0);
        CHECK(mt && mt->offsets[0] == sizeof(miptex_t), "wad-resolve: pixels present");
        CHECK(mt && reinterpret_cast<const uint8_t*>(mt)[mt->offsets[0]] == 0xCC, "wad-resolve: WAD bytes copied");
        CHECK(mt && std::strcmp(mt->name, "floor") == 0, "wad-resolve: name kept");
    }

    // --- unresolved header keeps name/dimensions, zero offsets ---
    {
        auto external = makeMiptex("+0missing", 32, 16, 0, false);
        auto lump = makeLump({ &external });
        std::unordered_map<std::string, const WadLump*> wads;

        auto out = buildEmbeddedMiptexLump(lump, wads);
        const miptex_t* mt = entryAt(out, 0);
        CHECK(mt != nullptr, "unresolved: entry present");
        CHECK(mt && std::strcmp(mt->name, "+0missing") == 0, "unresolved: name preserved (animation/sky/water semantics)");
        CHECK(mt && mt->width == 32 && mt->height == 16, "unresolved: dimensions preserved");
        CHECK(mt && mt->offsets[0] == 0 && mt->offsets[3] == 0, "unresolved: offsets zero");
    }

    // --- missing directory entry stays -1 ---
    {
        auto real = makeMiptex("a", 8, 8, 0x11, true);
        auto lump = makeLump({ nullptr, &real });
        std::unordered_map<std::string, const WadLump*> wads;

        auto out = buildEmbeddedMiptexLump(lump, wads);
        int32_t ofs0;
        entryAt(out, 0, &ofs0);
        CHECK(ofs0 == -1, "absent: dataofs -1 preserved");
        const miptex_t* mt = entryAt(out, 1);
        CHECK(mt && mt->offsets[0] == sizeof(miptex_t), "absent: following entry intact");
    }

    // --- a later dataofs past the lump must not extend the copy past the input ---
    {
        // 16x16 embedded texture with NO palette in the source (exactly the
        // paletteless size), followed by a directory entry pointing far past
        // the lump. The old code took that bogus offset as the entry's limit
        // and copied 770 bytes beyond the buffer.
        auto embedded = makeMiptex("tex", 16, 16, 0x5A, true);
        embedded.resize(sizeof(miptex_t) + 16 * 16 + 8 * 8 + 4 * 4 + 2 * 2); // strip count + palette
        auto lump = makeLump({ &embedded, nullptr });
        int32_t bogus = 10000;
        std::memcpy(lump.data() + 4 + 4 * 1, &bogus, 4);
        std::unordered_map<std::string, const WadLump*> wads;

        auto out = buildEmbeddedMiptexLump(lump, wads);
        const miptex_t* mt = entryAt(out, 0);
        int32_t ofs0;
        entryAt(out, 0, &ofs0);
        CHECK(mt != nullptr, "bounds: entry present");
        CHECK(mt && out.size() - static_cast<size_t>(ofs0) == embedded.size(), "bounds: copy limited to the input's bytes");
        int32_t ofs1;
        entryAt(out, 1, &ofs1);
        CHECK(ofs1 == -1, "bounds: out-of-range entry becomes -1");
    }

    // --- a truncated embedded payload becomes header-only, not a short copy ---
    {
        auto embedded = makeMiptex("cut", 16, 16, 0x33, true);
        embedded.resize(sizeof(miptex_t) + 100); // far less than the 16x16 mips need
        auto lump = makeLump({ &embedded });
        std::unordered_map<std::string, const WadLump*> wads;

        auto out = buildEmbeddedMiptexLump(lump, wads);
        const miptex_t* mt = entryAt(out, 0);
        CHECK(mt != nullptr, "truncated: entry present");
        CHECK(mt && mt->offsets[0] == 0, "truncated: demoted to header-only");
        CHECK(mt && std::strcmp(mt->name, "cut") == 0 && mt->width == 16, "truncated: name and dimensions kept");
    }

    // --- WAD precedence for the core index: last listed wins, as the
    //     reference walks world.wadlist in reverse ---
    {
        std::vector<WadLump> first{ makeWadLump("shared", makeMiptex("shared", 8, 8, 0x11, true)),
                                    makeWadLump("only1", makeMiptex("only1", 8, 8, 0x12, true)) };
        std::vector<WadLump> second{ makeWadLump("shared", makeMiptex("shared", 8, 8, 0x22, true)) };
        std::unordered_map<std::string, const WadLump*> index;
        indexWadLumpsForCore(index, first);
        indexWadLumpsForCore(index, second);
        CHECK(index.count("shared") == 1 && index["shared"]->data[sizeof(miptex_t)] == 0x22, "wad-order: later listed WAD overrides earlier");
        CHECK(index.count("only1") == 1 && index["only1"]->data[sizeof(miptex_t)] == 0x12, "wad-order: unique names from earlier WADs survive");

        // And an external reference resolves through that index, not
        // through anything unlisted - the builder only ever sees the index
        // it is handed.
        auto external = makeMiptex("shared", 8, 8, 0, false);
        auto lump = makeLump({ &external });
        auto out = buildEmbeddedMiptexLump(lump, index);
        const miptex_t* mt = entryAt(out, 0);
        CHECK(mt && reinterpret_cast<const uint8_t*>(mt)[mt->offsets[0]] == 0x22, "wad-order: resolved from the last listed WAD");
    }

    // --- headersOnly (the nv2a profile): no pixels, embedded or from a WAD,
    //     only names and dimensions ---
    {
        auto embedded = makeMiptex("own", 8, 8, 0x44, true);
        auto external = makeMiptex("fromwad", 16, 8, 0, false);
        std::vector<WadLump> wadLumps{ makeWadLump("fromwad", makeMiptex("fromwad", 16, 8, 0x55, true)) };
        std::unordered_map<std::string, const WadLump*> wads;
        indexWadLumpsForCore(wads, wadLumps);
        auto lump = makeLump({ &embedded, &external });

        auto out = buildEmbeddedMiptexLump(lump, wads, true);
        const miptex_t* a = entryAt(out, 0);
        const miptex_t* b = entryAt(out, 1);
        int32_t ofs0, ofs1;
        entryAt(out, 0, &ofs0);
        entryAt(out, 1, &ofs1);
        CHECK(a && std::strcmp(a->name, "own") == 0 && a->width == 8 && a->height == 8 && a->offsets[0] == 0,
              "headers-only: embedded texture keeps name and size, loses its pixels");
        CHECK(b && std::strcmp(b->name, "fromwad") == 0 && b->width == 16 && b->offsets[0] == 0,
              "headers-only: a WAD texture is not resolved");
        CHECK(ofs1 - ofs0 == static_cast<int32_t>(sizeof(miptex_t)) && out.size() == static_cast<size_t>(ofs1) + sizeof(miptex_t),
              "headers-only: each entry is exactly one header");
    }

    // --- empty / degenerate lumps ---
    {
        std::unordered_map<std::string, const WadLump*> wads;
        std::vector<uint8_t> empty;
        auto out = buildEmbeddedMiptexLump(empty, wads);
        int32_t n = -1;
        std::memcpy(&n, out.data(), 4);
        CHECK(out.size() == 4 && n == 0, "empty input: nummiptex 0");
    }

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("success\n");
    return 0;
}
