#pragma once
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>

namespace formats {

    // X9-U: does a map fit the Original Xbox's memory and entity limits?
    //
    // Nv2aMapBudget.h's ledger only counts ref/nv2a's own buffers, so a map
    // can pass it and still not load (rocket_frenzy passed at 51%). This
    // header models the whole load as the console runs it, from numbers
    // the baker already has, and is checked twice: before the bake from
    // the source .bsp (main.cpp, cheap, nothing written), and at save time
    // from the finished archive (PakManager::save, exact).
    //
    // The load, in order, and what each check compares:
    //  1. The engine builds its world model while holding the map's
    //     classic prefix (the .bsp view filesystem/mpak.c serves): C1 is
    //     engine structures + prefix against the memory free when the
    //     world starts loading.
    //  2. The prefix is freed, the map's precaches and entities arrive,
    //     then R_NewMap: ref/nv2a builds its tables, then its GPU buffers
    //     (the load temporaries are freed in between). C3 is tables +
    //     max(temporaries, GPU buffers) against what is left.
    //  3. Every entity that survives spawning takes an edict slot: the
    //     peak is checked against the game's max_edicts.
    //
    // Calibrated in xemu (X9-U, 2026-09-25) with a temporary
    // MmQueryStatistics probe on ten maps - crossfire, boot_camp, bounce,
    // disposal, pool_party, contamination, c1a0, c2a5, c4a1, rocket_frenzy
    // - on this build: free memory when the world starts loading was
    // 36.26-36.59 MiB on all ten (engine structures + prefix + what was
    // measured free in Mod_ProcessRenderData, with the structures from
    // nv2aEngineWorldBytes below), and the precaches and entity spawns
    // between that and R_NewMap took 6.2-8.1 MiB (10.1 on rocket_frenzy).
    // Re-measure when the engine's memory changes; the constants are only
    // as good as the build they came from. Known limits (nv2a-xbox-
    // specialist, X9-U review): all ten were cold loads, not a changelevel
    // with the previous map's models still resident; the precache term is
    // flat though it varies by map; and C3 counts bytes, while the lightmap,
    // texture data and vertex buffer each need one physically contiguous
    // run. Roadmap X9 item 12 measures transitions and the largest
    // contiguous run.
    constexpr uint64_t kMiB = 1024ull * 1024ull;
    constexpr uint64_t kXboxFreeAtWorldLoadBytes = 36ull * kMiB + kMiB / 4; // 36.25 MiB, the lowest measured
    constexpr uint64_t kXboxPrecacheBytes = 9ull * kMiB;                    // above the 8.1 MiB highest of the nine maps that load
    constexpr uint64_t kXboxLoadMarginBytes = 1ull * kMiB;                  // C1 headroom
    constexpr uint64_t kXboxSteadyMarginBytes = 3ull * kMiB / 2;            // C3 headroom: decals, sounds played later

    // Byte lengths of a map's classic BSP lumps (the same bytes in the
    // source .bsp and the archive's prefix, except miptex) and its miptex
    // count.
    struct Nv2aWorldLumps {
        uint64_t visibility = 0, nodes = 0, leaves = 0, marksurfaces = 0, planes = 0, clipnodes = 0, models = 0;
        uint64_t entities = 0, texinfo = 0, vertexes = 0, edges = 0, surfedges = 0, faces = 0, lighting = 0, miptex = 0;
        uint32_t textures = 0;
    };

    // What one Mem_Malloc/Mem_Calloc of `size` takes: zone.c's 24-byte
    // memheader_t and 1-byte sentinel, in a dlmalloc chunk (4-byte header,
    // 8-byte granularity, 16 minimum).
    inline uint64_t nv2aZoneChunk(uint64_t size) {
        const uint64_t c = (size + 25 + 4 + 7) & ~uint64_t(7);
        return c < 16 ? 16 : c;
    }

    // The engine's world model once Mod_LoadBrushModel has built it, with
    // the Xbox's 32-bit struct sizes: msurface_t 92, mextrasurf_t 112
    // (divergences #110, #116), one 28-byte mfacebevel_t per face in one block
    // (#113), mnode_t 52, mleaf_t 60, mtexinfo_t 44, texture_t 68, medge16
    // and mclipnode16 8, mplane_t 20, dmodel_t 64; plus the engine's own
    // copies of visibility, lighting and entities, hull 1's clipnodes and
    // the hull 0 built from the nodes.
    inline uint64_t nv2aEngineWorldBytes(const Nv2aWorldLumps& l) {
        const uint64_t faces = l.faces / 20, nodes = l.nodes / 24;
        uint64_t b = nv2aZoneChunk(4ull * l.textures) + l.textures * nv2aZoneChunk(68);
        b += nv2aZoneChunk(20 * (l.planes / 20)) + nv2aZoneChunk(12 * (l.vertexes / 12)) + nv2aZoneChunk(8 * (l.edges / 4));
        b += nv2aZoneChunk(4 * (l.surfedges / 4)) + nv2aZoneChunk(4 * (l.marksurfaces / 2)) + nv2aZoneChunk(44 * (l.texinfo / 40));
        b += nv2aZoneChunk(92 * faces) + nv2aZoneChunk(112 * faces) + nv2aZoneChunk(28 * faces);
        b += nv2aZoneChunk(52 * nodes) + nv2aZoneChunk(60 * (l.leaves / 28));
        b += nv2aZoneChunk(8 * (l.clipnodes / 8)) + nv2aZoneChunk(8 * nodes);
        b += nv2aZoneChunk(l.visibility) + nv2aZoneChunk(l.lighting) + nv2aZoneChunk(l.entities + 1) + nv2aZoneChunk(64 * (l.models / 64));
        return b;
    }

    // The archive's classic prefix: the first sector plus every classic
    // lump padded to a sector (Nv2aMapBudget.h's mpakClassicPrefixBytes,
    // from lengths rather than a header).
    inline uint64_t nv2aClassicPrefixBytes(const Nv2aWorldLumps& l) {
        const uint64_t lens[] = { l.visibility, l.nodes, l.leaves, l.marksurfaces, l.planes, l.clipnodes, l.models,
                                  l.entities, l.texinfo, l.vertexes, l.edges, l.surfedges, l.faces, l.lighting, l.miptex };
        uint64_t b = 2048;
        for (uint64_t len : lens)
            b += ((len + 2047) / 2048) * 2048;
        return b;
    }

    // The Xbox nv2a bake's miptex lump: headers only (divergence #112),
    // a count, a directory and one 40-byte miptex_t per texture.
    inline uint64_t nv2aHeaderOnlyMiptexBytes(uint32_t textures) {
        return 4ull + 44ull * textures;
    }

    // Edict slots at the load's peak: the world, one client, the edicts the
    // SDK's world creates for itself, and every map entity that survives
    // its own Spawn. The HL SDK removes these in Spawn, and during the load
    // the engine reuses a freed slot at once (SV_AllocEdict, sv.time < 2):
    // light, light_spot and light_environment without a targetname
    // (CLight::Spawn), info_null (CNullEntity::Spawn) and info_node /
    // info_node_air (CNodeEnt::Spawn). Everything else holds its slot,
    // including infodecal, which removes itself only on its first think.
    // CWorld::Precache adds a soundent and four bodyque edicts, and an
    // env_message when worldspawn has a netname (deps/hlsdk/dlls/world.cpp).
    // The engine reserves maxclients + 1 slots, so hosting N players takes
    // N - 1 more than this count (roadmap X9 item 13). Not counted: child
    // edicts some entities create in Spawn (an env_laser's sprite). Measured:
    // disposal (887) loads and rocket_frenzy (993) does not, at 900.
    constexpr uint32_t kSdkWorldEdicts = 5;      // soundent + 4 bodyque
    constexpr uint32_t kXboxEdictHeadroom = 8;   // slots left for what spawns at run time (projectiles, gibs, corpses)

    inline uint32_t nv2aPeakEdicts(const char* entities, size_t length) {
        uint32_t count = 2 + kSdkWorldEdicts; // the world, one client, the world's own edicts
        size_t i = 0;
        // One entity block: {"key" "value" ...}. Braces inside a quoted
        // value belong to the value, not the block.
        while (i < length) {
            if (entities[i] != '{') { ++i; continue; }
            std::string cls, targetname, netname, key;
            bool haveKey = false;
            size_t j = i + 1;
            while (j < length && entities[j] != '}') {
                if (entities[j] != '"') { ++j; continue; }
                const size_t start = ++j;
                while (j < length && entities[j] != '"') ++j;
                const std::string token(entities + start, j - start);
                ++j; // past the closing quote
                if (!haveKey) { key = token; haveKey = true; continue; }
                haveKey = false;
                if (key == "classname") cls = token;
                else if (key == "targetname") targetname = token;
                else if (key == "netname") netname = token;
            }
            if (cls == "worldspawn") {
                if (!netname.empty())
                    ++count; // CWorld::Precache's env_message for the chapter title
            } else if (!cls.empty() && cls != "info_null" && cls != "info_node" && cls != "info_node_air"
                       && !(targetname.empty() && (cls == "light" || cls == "light_spot" || cls == "light_environment"))) {
                ++count;
            }
            i = j + 1;
        }
        return count;
    }

    struct Nv2aFit {
        bool ok = true;
        std::string reason; // every failed condition, one per line
    };

    inline std::string nv2aMiB(uint64_t b) {
        std::ostringstream s;
        s.setf(std::ios::fixed);
        s.precision(1);
        s << static_cast<double>(b) / static_cast<double>(kMiB) << " MiB";
        return s.str();
    }

    // Before the bake: entity slots and C1, from the source map.
    inline Nv2aFit nv2aCheckBeforeBake(const Nv2aWorldLumps& l, uint32_t peakEdicts, uint32_t maxEdicts) {
        Nv2aFit f;
        if (peakEdicts + kXboxEdictHeadroom > maxEdicts) {
            f.ok = false;
            f.reason += "entities: " + std::to_string(peakEdicts) + " edict slots at the load's peak + "
                + std::to_string(kXboxEdictHeadroom) + " for run time, the game allows " + std::to_string(maxEdicts)
                + " (max_edicts)\n";
        }
        const uint64_t engine = nv2aEngineWorldBytes(l) + nv2aClassicPrefixBytes(l);
        if (engine + kXboxLoadMarginBytes > kXboxFreeAtWorldLoadBytes) {
            f.ok = false;
            f.reason += "C1 engine load: world structures + prefix " + nv2aMiB(engine) + " + " + nv2aMiB(kXboxLoadMarginBytes)
                + " margin exceed the " + nv2aMiB(kXboxFreeAtWorldLoadBytes) + " free when the world starts loading\n";
        }
        return f;
    }

    // At save time: C1 again with the real prefix, and C3.
    inline Nv2aFit nv2aCheckArchive(const Nv2aWorldLumps& l, uint64_t prefixBytes, uint64_t tableBytes,
                                    uint64_t loadTemporaryBytes, uint64_t gpuBytes) {
        Nv2aFit f;
        const uint64_t world = nv2aEngineWorldBytes(l);
        if (world + prefixBytes + kXboxLoadMarginBytes > kXboxFreeAtWorldLoadBytes) {
            f.ok = false;
            f.reason += "C1 engine load: world structures " + nv2aMiB(world) + " + prefix " + nv2aMiB(prefixBytes)
                + " exceed the " + nv2aMiB(kXboxFreeAtWorldLoadBytes) + " free when the world starts loading\n";
        }
        const uint64_t spent = world + kXboxPrecacheBytes;
        const uint64_t left = spent < kXboxFreeAtWorldLoadBytes ? kXboxFreeAtWorldLoadBytes - spent : 0;
        const uint64_t need = tableBytes + (gpuBytes > loadTemporaryBytes ? gpuBytes : loadTemporaryBytes) + kXboxSteadyMarginBytes;
        if (need > left) {
            f.ok = false;
            f.reason += "C3 renderer: tables + GPU buffers + margin need " + nv2aMiB(need) + ", ~" + nv2aMiB(left)
                + " is left at R_NewMap after the world structures (" + nv2aMiB(world) + ") and precaches (~"
                + nv2aMiB(kXboxPrecacheBytes) + "); short by " + nv2aMiB(need - left) + "\n";
        }
        return f;
    }

} // namespace formats
