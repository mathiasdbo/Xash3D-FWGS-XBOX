#include <iostream>
#include <fstream>
#include <string>
#include <regex>
#include <set>
#include "formats/BspParser.h"
#include "formats/PakManager.h"
#include "formats/VfsIndexer.h"
#include "formats/UiPacker.h"

namespace {

// AssetManifest: the replacement for embedding models/sprites/sounds into
// the .mpak archive at all. Scope decision, found while starting X2's real
// engine-side reader (docs/r3d/fork-plan.md): .mpak stays limited to what
// filesystem/mpak.c actually needs a monolithic, offset-addressed archive
// for - BSP geometry, level (WAD-resolved) textures, and the lightmap
// atlas, all genuinely sequential-I/O-shaped data a per-map bundle helps
// with on the Xbox's slow IDE bus. Models, sprites and sounds are ordinary,
// already-self-contained GoldSrc files (a real .mdl has its own embedded
// skins, a real .spr its own frames) upstream Xash3D-FWGS's own loaders
// already know how to read - baking them into .mpak meant re-parsing and
// re-encoding formats this project does not need to touch at all, just to
// put them somewhere they can be found by name.
//
// Every function below that used to call pak.addAsset/addTexture now
// writes to this manifest instead - the same asset names, in the same
// discovery order, just recorded rather than embedded. A later step (not
// this tool's job) copies the named loose files onto whatever the engine's
// real search path ends up being; filesystem/mpak.c never needs to know
// these names at all.
// Found in review: the first version of this class only ever warned on a
// failed open and silently dropped every add() from then on - main()
// still printed "Successfully compiled..." and returned 0 regardless, so
// a bad output directory or a permissions error produced a believable
// success with a missing or truncated manifest. ok() now reflects real
// stream state (checked after every write, not just at construction) and
// main() checks it before ever reporting success.
class AssetManifest {
public:
    explicit AssetManifest(const std::string& path) : mPath(path), mFile(path) {
        if (!mFile) {
            std::cerr << "Error: could not open asset manifest for writing: " << path << "\n";
        }
    }

    void add(const std::string& assetName) {
        if (!mFile.good()) {
            return; // already failed; ok() will report it
        }
        if (mSeen.insert(assetName).second) {
            mFile << assetName << "\n";
            if (!mFile.good()) {
                std::cerr << "Error: write failed to asset manifest: " << mPath << "\n";
            }
        }
    }

    // Must be called after every add() this run will do, before trusting
    // the manifest exists and is complete - flushes and checks the
    // stream's own error state, which a mid-write disk-full or permission
    // failure would otherwise leave undetected until the process exits.
    bool ok() {
        mFile.flush();
        return mFile.good();
    }

    size_t count() const { return mSeen.size(); }

private:
    std::string mPath;
    std::ofstream mFile;
    std::set<std::string> mSeen;
};

// Records sound/<name> - manifest entries mirror precache names exactly,
// e.g. "player/pl_step1.wav", the strings the SDK passes to
// PRECACHE_SOUND/EMIT_SOUND, always implicitly relative to sound/ - plus
// sound/sentences.txt and sound/materials.txt if present, both of which
// load through the SDK's own pfnLoadFileForMe("sound/...") calls verbatim.
//
// Found in review (round 2): a caller explicitly passing --sounds <path>
// that fails to open only ever printed a warning and returned - main()
// never checked that, so a typo'd or moved manifest path produced a
// believable "Successfully compiled..." with the sounds simply missing,
// exactly the false-clean result this branch's ok()-checked AssetManifest
// was already trying to rule out for write failures. Returns false now so
// main() can treat an explicitly-requested manifest that can't be opened
// as fatal, same as a write failure.
bool packSounds(const std::string& manifestPath, formats::VfsIndexer& vfs, AssetManifest& out) {
    std::ifstream manifest(manifestPath);
    if (!manifest) {
        std::cerr << "Error: could not open sound manifest: " << manifestPath << "\n";
        return false;
    }

    size_t listedCount = 0;
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

        std::string assetName = "sound/" + line;
        std::string resolvedPath = vfs.findFileByName(assetName);
        if (resolvedPath.empty()) {
            std::cerr << "Warning: sound not found in VFS: " << assetName << "\n";
            continue;
        }
        out.add(assetName);
        ++listedCount;
    }
    std::cout << "Listed " << listedCount << " sounds from manifest: " << manifestPath << "\n";

    for (const char* support : {"sound/sentences.txt", "sound/materials.txt"}) {
        std::string resolvedPath = vfs.findFileByName(support);
        if (resolvedPath.empty()) {
            std::cout << "Note: " << support << " not found - skipping (SENTENCEG_Init/TEXTURETYPE_Init "
                      << "will degrade gracefully, matching real GoldSrc's own missing-file behavior)\n";
            continue;
        }
        out.add(support);
        std::cout << "Listed " << support << "\n";
    }
    return true;
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
// they go through the identical VFS-resolution-then-manifest path rather
// than a second, divergent one.
//
// Found in review (round 2): same false-clean-success shape as
// packSounds above - an explicitly-requested manifest that fails to open
// only warned, so main() reported success while silently missing every
// code-only-referenced model this manifest exists to cover.
bool packModelManifest(const std::string& manifestPath, std::set<std::string>& uniqueAssets) {
    std::ifstream manifest(manifestPath);
    if (!manifest) {
        std::cerr << "Error: could not open models manifest: " << manifestPath << "\n";
        return false;
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
    return true;
}

// Records sprites/<name> - real sprites/hud.txt + the 15
// sprites/weapon_<classname>.txt manifests + their sprite sheets, for the
// P16+ Sprite HUD & Weapon Selection phase.
//
// Corrected, found in review: an earlier version of this wrote "hud/" +
// line to the manifest instead of the real sprites/ path - a leftover
// from when this build embedded a raw sheet under "hud/" specifically to
// disambiguate it from a PATCHED "sprites/crosshairs.spr" sharing the
// asset dict's flat namespace (real MSVC/lld semantics do not apply here,
// this was this tool's own internal dict). That disambiguation reason is
// gone - nothing is patched or embedded any more - but the write was
// never updated to match, so the manifest named a path
// ("hud/hud.txt") the engine never actually asks for.
// SPR_GetList("sprites/hud.txt", ...) (deps/hlsdk/cl_dll/hud.cpp:447) is
// what the real client loads, via FS_LoadFile on that exact string
// (engine/client/dll_int/cl_game.c:1565) - the manifest now records
// vfsName (already computed below as "sprites/" + line) verbatim, so a
// deployment step staging these names finds files the engine will
// genuinely request.
//
// Found in review (round 2): same false-clean-success shape as
// packSounds/packModelManifest above - fixed the same way.
bool packSprites(const std::string& manifestPath, formats::VfsIndexer& vfs, AssetManifest& out) {
    std::ifstream manifest(manifestPath);
    if (!manifest) {
        std::cerr << "Error: could not open sprites manifest: " << manifestPath << "\n";
        return false;
    }

    size_t listedCount = 0;
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
        out.add(vfsName);
        ++listedCount;
    }
    std::cout << "Listed " << listedCount << " HUD/weapon sprite assets from manifest: " << manifestPath << "\n";
    return true;
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

    // Manifest path: outputPath with its extension replaced by
    // ".assets.txt" - e.g. "c1a3.mpak" -> "c1a3.assets.txt",
    // "global.mpak" -> "global.assets.txt". Same directory, same base
    // name, so the two files this tool now produces per invocation stay
    // obviously paired without a separate CLI flag.
    std::string manifestPath = outputPath;
    {
        size_t dot = manifestPath.find_last_of('.');
        size_t slash = manifestPath.find_last_of("/\\");
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
            manifestPath = manifestPath.substr(0, dot);
        }
        manifestPath += ".assets.txt";
    }
    AssetManifest assetManifest(manifestPath);

    if (buildGlobal) {
        // Scope decision, X2 (docs/r3d/fork-plan.md): .mpak is now limited
        // to BSP geometry/level textures/lightmaps - none of which a
        // "shared engine assets" bundle has any of. global.mpak as an
        // archive is retired; this mode now only ever produces the
        // manifest below, listing the same always-needed names it used to
        // embed, for whatever deployment step stages loose files.
        std::cout << "\nListing shared engine assets (global.assets.txt, no global.mpak)...\n";
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

    // Merged in before the listing loop so manifest-listed models go
    // through the same VFS-resolution check as entity-scraped ones, and
    // duplicates between the two collapse (uniqueAssets is a set).
    if (!modelsManifest.empty() && !packModelManifest(modelsManifest, uniqueAssets)) {
        return 1;
    }

    // Scope decision, X2 (docs/r3d/fork-plan.md): models and sprites are
    // no longer parsed, re-encoded, or embedded here at all - a real .mdl
    // already has its own skins, a real .spr its own frames, and upstream
    // Xash3D-FWGS's own loaders already read both formats unmodified. This
    // loop's only remaining job is to confirm each referenced asset
    // genuinely exists in the retail tree (still real, still useful - it
    // is exactly the check that used to precede parsing) and record its
    // name in the manifest instead of loading/rewriting its bytes.
    for (const auto& asset : uniqueAssets) {
        std::string resolvedPath = vfs.findFileByName(asset);
        if (resolvedPath.empty()) {
            std::cerr << "Warning: Could not find asset in VFS: " << asset << "\n";
            continue;
        }
        assetManifest.add(asset);
    }

    if (!soundsManifest.empty() && !packSounds(soundsManifest, vfs, assetManifest)) {
        return 1;
    }

    if (!spritesManifest.empty() && !packSprites(spritesManifest, vfs, assetManifest)) {
        return 1;
    }

    if (!assetManifest.ok()) {
        std::cerr << "Failed to write asset manifest: " << manifestPath << "\n";
        return 1;
    }
    std::cout << "Wrote " << assetManifest.count() << " asset name(s) to " << manifestPath << "\n";

    // buildGlobal never had BSP geometry to begin with (see the comment
    // above) - nothing left to save once the manifest above is written.
    if (!buildGlobal && !pak.save(outputPath)) {
        std::cerr << "Failed to save MPAK archive: " << outputPath << "\n";
        return 1;
    }

    if (buildGlobal) {
        std::cout << "\nSuccessfully listed shared engine assets into " << manifestPath << "!\n";
    } else {
        std::cout << "\nSuccessfully compiled map into " << outputPath << " and " << manifestPath << "!\n";
    }
    return 0;
}
