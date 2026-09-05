#include <iostream>
#include <fstream>
#include <string>
#include <regex>
#include <set>
#include "formats/BspParser.h"
#include "formats/PakManager.h"
#include "formats/VfsIndexer.h"
#include "formats/UiPacker.h"
#include "formats/MdlParser.h"
#include "formats/SprParser.h"

namespace {

// Reads a whole file into a byte buffer, or returns an empty vector on
// failure (callers already warn on an empty/unresolved path via VfsIndexer,
// so a second warning here would be redundant - only genuine I/O errors
// after a successful VFS resolution land here).
std::vector<uint8_t> readFileBytes(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        return {};
    }
    return buffer;
}

// Packs sound/<name> (raw bytes, no transcoding - see WavParser's removal:
// most of the retail sound tree is already 8-bit PCM, where ADPCM would
// cost quality for little size win) plus sound/sentences.txt and
// sound/materials.txt if present, both of which load through the SDK's
// own pfnLoadFileForMe("sound/...") calls verbatim.
void packSounds(const std::string& manifestPath, formats::VfsIndexer& vfs, formats::PakManager& pak) {
    std::ifstream manifest(manifestPath);
    if (!manifest) {
        std::cerr << "Warning: could not open sound manifest: " << manifestPath << "\n";
        return;
    }

    size_t packedCount = 0;
    std::string line;
    while (std::getline(manifest, line)) {
        // Trim trailing CR (Windows-authored manifests) and surrounding whitespace.
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
            line.pop_back();
        }
        size_t start = line.find_first_not_of(' ');
        if (start == std::string::npos || line.empty() || line[0] == '#') {
            continue;
        }
        line = line.substr(start);

        // Manifest entries are raw precache names, e.g. "player/pl_step1.wav" -
        // the exact strings the SDK passes to PRECACHE_SOUND/EMIT_SOUND, always
        // implicitly relative to sound/. Packed asset names carry the "sound/"
        // prefix so they match what pfnLoadFileForMe/find_asset resolve against.
        std::string assetName = "sound/" + line;
        std::string resolvedPath = vfs.findFileByName(assetName);
        if (resolvedPath.empty()) {
            std::cerr << "Warning: sound not found in VFS: " << assetName << "\n";
            continue;
        }
        auto bytes = readFileBytes(resolvedPath);
        if (bytes.empty()) {
            std::cerr << "Warning: failed to read sound file: " << resolvedPath << "\n";
            continue;
        }
        pak.addAsset(assetName, bytes);
        ++packedCount;
    }
    std::cout << "Packed " << packedCount << " sounds from manifest: " << manifestPath << "\n";

    for (const char* support : {"sound/sentences.txt", "sound/materials.txt"}) {
        std::string resolvedPath = vfs.findFileByName(support);
        if (resolvedPath.empty()) {
            std::cout << "Note: " << support << " not found - skipping (SENTENCEG_Init/TEXTURETYPE_Init "
                      << "will degrade gracefully, matching real GoldSrc's own missing-file behavior)\n";
            continue;
        }
        auto bytes = readFileBytes(resolvedPath);
        if (!bytes.empty()) {
            pak.addAsset(support, bytes);
            std::cout << "Packed " << support << "\n";
        }
    }
}

// Adds every name in a manifest to the asset set, for models the BSP entity
// lump never mentions.
//
// The entity-lump scrape further down only finds models a MAPPER placed by
// name. Monsters and weapons choose their own model in CODE -
// CScientist::Spawn calls SET_MODEL(ENT(pev), "models/scientist.mdl"), and a
// weapon deploys "models/v_9mmhandgun.mdl" - so none of those are
// discoverable from the map at all, and GET_MODEL_PTR returned null for
// every one of them. There is no way to derive that set from the BSP; it
// has to be supplied.
//
// Names are inserted into the same uniqueAssets set the scrape feeds, so
// they go through the identical MdlParser + addTexture packing path rather
// than a second, divergent one.
void packModelManifest(const std::string& manifestPath, std::set<std::string>& uniqueAssets) {
    std::ifstream manifest(manifestPath);
    if (!manifest) {
        std::cerr << "Warning: could not open models manifest: " << manifestPath << "\n";
        return;
    }

    size_t listed = 0;
    std::string line;
    while (std::getline(manifest, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
            line.pop_back();
        }
        size_t start = line.find_first_not_of(' ');
        if (start == std::string::npos || line.empty() || line[0] == '#') {
            continue;
        }
        // Entries are VFS-relative names exactly as the SDK precaches them,
        // e.g. "models/scientist.mdl" - the same spelling the asset dict is
        // keyed by, so find_asset resolves them at runtime unchanged.
        uniqueAssets.insert(line.substr(start));
        ++listed;
    }
    std::cout << "Model manifest listed " << listed << " models: " << manifestPath << "\n";
}

// Packs hud/<name> (raw bytes through addAsset, NOT addTexture) - real
// sprites/hud.txt + the 15 sprites/weapon_<classname>.txt manifests +
// their sprite sheets, for the P16+ Sprite HUD & Weapon Selection phase.
//
// Two deliberate departures from how every other .mdl/.spr in this build
// gets packed (see the uniqueAssets loop below):
// - addAsset, not addTexture: addTexture rescales into the 64/256/512
//   runtime texture-array size bins (see PakManager::addTexture), which
//   would distort e.g. 640hud7.spr (256x128) into a square bin and
//   destroy every pixel-exact rect hud.txt's x/y/w/h columns describe.
//   These sheets are decoded and uploaded as their own standalone
//   textures at load time instead - addAsset's raw-bytes-through path is
//   the right fit. (Consumer TBD: the engine-side reader has not been
//   written yet, see docs/r3d/fork-plan.md F1.)
// - the "hud/" prefix, not "sprites/": this same build already packs a
//   PATCHED "sprites/crosshairs.spr" for 3D world-space rendering (see
//   uniqueAssets' hardcoded global-asset list below), and the engine feeds
//   every ".spr"-suffixed asset-dict name to that patched-format parser. A raw retail sheet packed under the same name would
//   collide and be silently misinterpreted as patched data. "hud/" keeps
//   the two asset families disjoint by construction.
void packSprites(const std::string& manifestPath, formats::VfsIndexer& vfs, formats::PakManager& pak) {
    std::ifstream manifest(manifestPath);
    if (!manifest) {
        std::cerr << "Warning: could not open sprites manifest: " << manifestPath << "\n";
        return;
    }

    size_t packedCount = 0;
    std::string line;
    while (std::getline(manifest, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
            line.pop_back();
        }
        size_t start = line.find_first_not_of(' ');
        if (start == std::string::npos || line.empty() || line[0] == '#') {
            continue;
        }
        line = line.substr(start);

        // Manifest entries are paths relative to sprites/, e.g. "hud.txt",
        // "640hud1.spr", "weapon_crowbar.txt".
        std::string vfsName = "sprites/" + line;
        std::string resolvedPath = vfs.findFileByName(vfsName);
        if (resolvedPath.empty()) {
            std::cerr << "Warning: sprite asset not found in VFS: " << vfsName << "\n";
            continue;
        }
        auto bytes = readFileBytes(resolvedPath);
        if (bytes.empty()) {
            std::cerr << "Warning: failed to read sprite asset: " << resolvedPath << "\n";
            continue;
        }
        pak.addAsset("hud/" + line, bytes);
        ++packedCount;
    }
    std::cout << "Packed " << packedCount << " HUD/weapon sprite assets from manifest: " << manifestPath << "\n";
}

} // namespace

int main(int argc, char *argv[]) {
    std::cout << "===============================================\n";
    std::cout << "  Resonance3D Asset Studio - Retail VFS Compiler\n";
    std::cout << "===============================================\n";

    std::string gameDir = ".";
    std::string mapPath = "test_map.bsp";
    std::string outputPath = "../test_map.mpak";
    std::string soundsManifest;
    std::string spritesManifest;
    std::string modelsManifest;
    bool packUi = false;
    bool buildGlobal = false;
    formats::PakManager pak;

    // --pack-menu specific options. Separate from --game-dir/--sprites since
    // menu art lives in a different source tree (xash3d-fwgs's vendored
    // xash-extras, not a retail gamedir) with a different manifest shape
    // (banner names, not sprites/-relative paths).
    bool packMenu = false;
    std::string xashExtrasDir;
    std::string menuManifest = "menu_assets_manifest.txt";
    std::string debugDumpDir;
    // Optional: a retail gamedir's valve/resource/ folder, for the
    // animated Main-menu logo (resource/logo*.tga) - see UiPacker.h's
    // packMenuAssets doc comment for why this is separate from
    // xashExtrasDir and allowed to be empty.
    std::string resourceDir;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--game-dir" && i + 1 < argc) {
            gameDir = argv[++i];
        } else if (arg == "--map" && i + 1 < argc) {
            mapPath = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            outputPath = argv[++i];
        } else if (arg == "--sounds" && i + 1 < argc) {
            soundsManifest = argv[++i];
        } else if (arg == "--sprites" && i + 1 < argc) {
            spritesManifest = argv[++i];
        } else if (arg == "--models" && i + 1 < argc) {
            modelsManifest = argv[++i];
        } else if (arg == "--target" && i + 1 < argc) {
            std::string target = argv[++i];
            if (target != "pc" && target != "xbox") {
                std::cerr << "Invalid target: " << target << ". Must be 'pc' or 'xbox'.\n";
                return 1;
            }
            if (target == "xbox") {
                pak.setTargetXbox(true);
            }
        } else if (arg == "--pack-ui") {
            packUi = true;
        } else if (arg == "--global") {
            buildGlobal = true;
        } else if (arg == "--pack-menu") {
            packMenu = true;
        } else if (arg == "--xash-extras-dir" && i + 1 < argc) {
            xashExtrasDir = argv[++i];
        } else if (arg == "--menu-manifest" && i + 1 < argc) {
            menuManifest = argv[++i];
        } else if (arg == "--debug-dump-dir" && i + 1 < argc) {
            debugDumpDir = argv[++i];
        } else if (arg == "--resource-dir" && i + 1 < argc) {
            resourceDir = argv[++i];
        }
    }

    if (packMenu) {
        if (xashExtrasDir.empty()) {
            std::cerr << "--pack-menu requires --xash-extras-dir <path to xash3d-fwgs/3rdparty/extras/xash-extras>\n";
            return 1;
        }
        // packMenuAssets bakes raw RGBA8 unconditionally - fine for PC, but
        // Xbox needs a compact paletted/16bpp encode plus NV2A swizzling
        // (P8 for the button atlas and banners, since both are genuinely
        // 8bpp-indexed with no color key in their source art) before this is
        // viable in the ~10-12MB GPU heap. That work is explicitly deferred
        // to P18/P19 - see docs/plans/won_menu_ui_plan.md's M7 entry. Reject
        // the combination loudly rather than silently emitting a PC-format
        // archive under --target xbox, which would look like it worked and
        // then OOM (or worse, silently mislabel itself) at Xbox bring-up.
        if (pak.isXboxTarget()) {
            std::cerr << "--pack-menu --target xbox is not yet supported: menu art still bakes as "
                          "raw RGBA8, which is too large for the Xbox GPU heap unswizzled/uncompressed. "
                          "See docs/plans/won_menu_ui_plan.md's M7 entry.\n";
            return 1;
        }
        bool ok = formats::UiPacker::packMenuAssets(xashExtrasDir, menuManifest, outputPath, debugDumpDir, resourceDir);
        return ok ? 0 : 1;
    }

    std::cout << "CLI Parameters:\n";
    std::cout << " - Game Root Dir: " << gameDir << "\n";
    std::cout << " - Target Map:    " << mapPath << "\n";
    std::cout << " - MPAK Output:   " << outputPath << "\n\n";

    if (packUi) {
        if (!formats::UiPacker::packMenuBackground(gameDir, outputPath, pak.isXboxTarget())) {
            return 1;
        }
        return 0;
    }

    // 1. Universal VFS Indexing across retail game directory tree
    formats::VfsIndexer vfs;
    vfs.scanDirectory(gameDir);

    std::set<std::string> uniqueAssets;

    if (buildGlobal) {
        std::cout << "\nBuilding global.mpak with shared engine assets...\n";
        uniqueAssets.insert("models/player.mdl");
        uniqueAssets.insert("models/v_crowbar.mdl");
        uniqueAssets.insert("models/w_crowbar.mdl");
        uniqueAssets.insert("sprites/crosshairs.spr");
        // Track C item 10: combat-feedback temp-entity sprites. Precached
        // by fixed C++ strings in the weapon DLL
        // (deps/halflife-updated/dlls/weapons.cpp:332-337), never as an
        // entity keyvalue, so the per-map entity-lump scrape below can
        // never discover them - same reason crosshairs.spr needs an
        // explicit listing here instead of relying on that scrape.
        uniqueAssets.insert("sprites/zerogxplode.spr"); // g_sModelIndexFireball (TE_EXPLOSION)
        uniqueAssets.insert("sprites/WXplo1.spr");       // g_sModelIndexWExplosion (underwater variant)
        uniqueAssets.insert("sprites/steam1.spr");       // g_sModelIndexSmoke (TE_SMOKE)
        uniqueAssets.insert("sprites/bloodspray.spr");   // g_sModelIndexBloodSpray (TE_BLOODSPRITE)
        uniqueAssets.insert("sprites/blood.spr");        // g_sModelIndexBloodDrop (TE_BLOODSPRITE)
    } else {
        // 2. Load BSP Map
        formats::BspParser bsp;
        std::string resolvedMapPath = vfs.findFileByName(mapPath);
        if (resolvedMapPath.empty()) {
            resolvedMapPath = mapPath;
        }

        if (!bsp.load(resolvedMapPath)) {
            std::cerr << "Could not load BSP map file: " << resolvedMapPath << "\n";
            return 1;
        }

        // 3. Export to MPAK with VFS WAD Resolution
        bsp.exportToPak(pak, vfs);
        
        // 4. Scrape entities for MDL/SPR assets and pack them
        const auto& entities = bsp.getLumpData(formats::LUMP_ENTITIES);
        std::string entStr(reinterpret_cast<const char*>(entities.data()), entities.size());
        std::regex assetRegex("\"([^\"]+\\.(mdl|spr))\"");
        std::smatch match;
        std::string::const_iterator searchStart(entStr.cbegin());
        while (std::regex_search(searchStart, entStr.cend(), match, assetRegex)) {
            std::string asset = match[1];
            if (asset == "models/player.mdl" || asset == "models/v_crowbar.mdl" || 
                asset == "models/w_crowbar.mdl" || asset == "sprites/crosshairs.spr") {
                searchStart = match.suffix().first;
                continue;
            }
            uniqueAssets.insert(asset);
            searchStart = match.suffix().first;
        }
    }

    // Merged in before the packing loop so manifest-listed models take the
    // same MdlParser/addTexture path as entity-scraped ones, and duplicates
    // between the two collapse (uniqueAssets is a set).
    if (!modelsManifest.empty()) {
        packModelManifest(modelsManifest, uniqueAssets);
    }

    for (const auto& asset : uniqueAssets) {
        std::string resolvedPath = vfs.findFileByName(asset);
        if (resolvedPath.empty()) {
            std::cerr << "Warning: Could not find asset in VFS: " << asset << "\n";
            continue;
        }

        if (asset.find(".mdl") != std::string::npos) {
            formats::MdlParser mdl;
            if (mdl.load(resolvedPath)) {
                std::vector<formats::TextureLocation> locs;
                for (const auto& tex : mdl.getTextures()) {
                    bool masked = (tex.flags & formats::STUDIO_NF_MASKED) != 0;
                    locs.push_back(pak.addTexture(tex.width, tex.height, tex.rgba.data(), masked));
                }
                auto patched = mdl.getPatchedData(locs);
                pak.addAsset(asset, patched);
                std::cout << "Packed MDL: " << asset << "\n";
            }
        } else if (asset.find(".spr") != std::string::npos) {
            formats::SprParser spr;
            if (spr.load(resolvedPath)) {
                bool masked = (spr.getHeader().texFormat == formats::SPR_ALPHTEST);
                std::vector<formats::TextureLocation> locs;
                for (const auto& frame : spr.getFrames()) {
                    locs.push_back(pak.addTexture(frame.width, frame.height, frame.rgba.data(), masked));
                }
                auto patched = spr.getPatchedData(locs);
                pak.addAsset(asset, patched);
                std::cout << "Packed SPR: " << asset << "\n";
            }
        }
    }

    if (!soundsManifest.empty()) {
        packSounds(soundsManifest, vfs, pak);
    }

    if (!spritesManifest.empty()) {
        packSprites(spritesManifest, vfs, pak);
    }

    if (!pak.save(outputPath)) {
        std::cerr << "Failed to save MPAK archive: " << outputPath << "\n";
        return 1;
    }

    if (buildGlobal) {
        std::cout << "\nSuccessfully compiled global.mpak into " << outputPath << "!\n";
    } else {
        std::cout << "\nSuccessfully compiled map into " << outputPath << "!\n";
    }
    return 0;
}
