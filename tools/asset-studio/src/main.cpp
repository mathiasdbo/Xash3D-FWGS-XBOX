#include <iostream>
#include <fstream>
#include <string>
#include <regex>
#include <set>
#include "formats/BspParser.h"
#include "formats/PakManager.h"
#include "formats/VfsIndexer.h"
#include "formats/UiPacker.h"
#include "formats/Nv2aMapFit.h"
#include "formats/Nv2aWater.h" // R5: the water pieces in the engine's world bytes
#include "formats/MusicBake.h" // #171: media/*.mp3 baked to .xwav
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstring>

namespace {

// X9-U: the game's edict limit, as the engine resolves it
// (filesystem/gameinfo.c FS_ParseGameInfo): gameinfo.txt's max_edicts, or
// liblist.gam's edicts when there is no gameinfo.txt, clamped to
// MIN_EDICTS..MAX_EDICTS (64..8192, engine/common/protocol.h), else
// DEFAULT_MAX_EDICTS (common/defaults.h, 1200). The retail
// valve/gameinfo.txt sets 900. This reads the game folder the baker is
// given - the staged Xbox disc copies the same gameinfo.txt.
uint32_t readGameMaxEdicts(const std::string& gameDir) {
    auto readKey = [](const std::string& path, const char* key, uint32_t& out) -> bool {
        std::ifstream f(path);
        if (!f) return false;
        std::string line;
        const size_t keyLen = std::strlen(key);
        while (std::getline(f, line)) {
            size_t p = line.find_first_not_of(" \t");
            if (p == std::string::npos || line.size() < p + keyLen) continue;
            std::string word = line.substr(p, keyLen);
            std::transform(word.begin(), word.end(), word.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (word != key) continue;
            const size_t after = p + keyLen;
            if (after < line.size() && !std::isspace(static_cast<unsigned char>(line[after])) && line[after] != '"') continue;
            const size_t digit = line.find_first_of("0123456789", after);
            if (digit == std::string::npos) continue;
            out = static_cast<uint32_t>(std::strtoul(line.c_str() + digit, nullptr, 10));
            return true;
        }
        return false;
    };
    uint32_t value = 1200; // DEFAULT_MAX_EDICTS
    const std::string gameinfo = gameDir + "/gameinfo.txt";
    if (std::ifstream(gameinfo)) {
        readKey(gameinfo, "max_edicts", value);
    } else {
        readKey(gameDir + "/liblist.gam", "edicts", value);
    }
    return std::min<uint32_t>(std::max<uint32_t>(value, 64), 8192);
}

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
    std::string verifyPath;
    bool packUi = false;
    bool buildGlobal = false;
    bool bakeMenuArt = false;
    bool menuArtDxt1 = false;
    bool bakeSkyDxt1 = false;
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

    // divergences.md #171: --music <out_dir> bakes <game-dir>/media/*.mp3
    // to <out_dir>/media/<Track>.xwav plus <out_dir>/music_manifest.txt,
    // an overlay tree for tools/xbox-xiso.sh --music-bake. With
    // --music-rate 48000 (#173) each track is resampled to the console's
    // mix rate and written as <out_dir>/media/<Track>.48k.xwav.
    std::string musicOut;
    formats::MusicBakeOptions music;

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
        } else if (arg == "--profile" && i + 1 < argc) {
            // core (default): the classic lumps the engine's unmodified
            // loaders consume - everything X2..X5 needs. nv2a: additionally
            // the baked Vertex/index/DXT/lightmap-atlas block for the X9
            // hardware renderer (docs/r3d/mpak-loading-design.md, §5-§6).
            std::string profile = argv[++i];
            if (profile != "core" && profile != "nv2a") {
                std::cerr << "Invalid profile: " << profile << ". Must be 'core' or 'nv2a'.\n";
                return 1;
            }
            pak.setProfileNv2a(profile == "nv2a");
        } else if (arg == "--verify" && i + 1 < argc) {
            // Bake the map in memory exactly as --output would, then instead
            // of writing, byte-compare the given archive against those
            // sources (PakManager::verifyFile). Exit 1 on any difference -
            // this is how a corrupted copy is proven to be caught.
            verifyPath = argv[++i];
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
        } else if (arg == "--bake-menu-art") {
            bakeMenuArt = true;
        } else if (arg == "--bake-sky-dxt1") {
            // divergences.md #137: gfx/env skyboxes as DXT1 .dds for ref/nv2a
            bakeSkyDxt1 = true;
        } else if (arg == "--menu-art-dxt1") {
            // divergences.md #131: DXT1 .dds tiles for ref/nv2a instead of TGA
            menuArtDxt1 = true;
        } else if (arg == "--music") {
            if (i + 1 >= argc) {
                std::cerr << "--music requires <out_dir>\n";
                return 1;
            }
            musicOut = argv[++i];
        } else if (arg == "--music-codec" && i + 1 < argc) {
            const std::string codec = argv[++i];
            if (codec != "adpcm" && codec != "pcm") {
                std::cerr << "Invalid --music-codec: " << codec << ". Must be 'adpcm' or 'pcm'.\n";
                return 1;
            }
            music.allPcm = codec == "pcm";
        } else if (arg == "--music-pcm" && i + 1 < argc) {
            // a comma-separated list of tracks to ship as 16-bit PCM
            std::stringstream list(argv[++i]);
            std::string name;
            while (std::getline(list, name, ',')) {
                if (!name.empty()) music.pcmTracks.insert(name);
            }
        } else if ((arg == "--music-floor-db" || arg == "--music-floor-seg-db") && i + 1 < argc) {
            // a typo or "nan" would otherwise switch the quality gate off
            char* end = nullptr;
            const double v = std::strtod(argv[++i], &end);
            if (!end || *end != '\0' || end == argv[i] || !(v >= 0.0 && v <= 100.0)) {
                std::cerr << arg << ": not a number of dB in 0..100: " << argv[i] << "\n";
                return 1;
            }
            (arg == "--music-floor-db" ? music.floor.overallDb : music.floor.segmentDb) = v;
        } else if (arg == "--music-threads" && i + 1 < argc) {
            char* end = nullptr;
            const long v = std::strtol(argv[++i], &end, 10);
            if (!end || *end != '\0' || v < 0 || v > 64) {
                std::cerr << "--music-threads: not a count in 0..64: " << argv[i] << "\n";
                return 1;
            }
            music.threads = static_cast<int>(v);
        } else if (arg == "--music-rate" && i + 1 < argc) {
            // divergences.md #173: the console mixes at 48 kHz; a track
            // resampled to it offline is written as <Track>.48k.xwav
            const std::string rate = argv[++i];
            if (rate != "48000") {
                std::cerr << "--music-rate: only 48000 (the console's mix rate) is supported, not " << rate << "\n";
                return 1;
            }
            music.rate = 48000;
        }
    }

    if (!musicOut.empty()) {
        if (gameDir == ".") {
            std::cerr << "--music requires --game-dir <path to a gamedir, e.g. .../valve>\n";
            return 1;
        }
        music.gameDir = gameDir;
        music.outDir = musicOut;
        std::vector<formats::MusicTrackResult> results;
        const bool ok = formats::bakeMusic(music, results);
        if (!ok) {
            std::cerr << "--music: the bake FAILED - see the tracks marked FAILED above\n";
        }
        return ok ? 0 : 1;
    }

    if (bakeSkyDxt1) {
        // X9-X item 14 step 10: --game-dir is a gamedir root (e.g. a Steam
        // install's "valve", the directory gfx/env/ sits under); --output
        // is a directory, an overlay tree for tools/xbox-xiso.sh
        // --sky-bake, as with --bake-menu-art.
        if (gameDir == ".") {
            std::cerr << "--bake-sky-dxt1 requires --game-dir <path to a gamedir, e.g. .../valve>\n";
            return 1;
        }
        if (outputPath == "../test_map.mpak") {
            std::cerr << "--bake-sky-dxt1 requires --output <directory> (an overlay tree, not a file)\n";
            return 1;
        }
        int sets = 0;
        bool ok = formats::UiPacker::bakeSkyArt(gameDir, outputPath, sets);
        return ok ? 0 : 1;
    }

    if (bakeMenuArt) {
        // XM item 0 stage B (fork-plan.md): --game-dir is a gamedir root
        // (e.g. a Steam install's "valve") - not the whole Steam install,
        // matching what resource/BackgroundLayout.txt is already relative
        // to. --output is a directory (an overlay tree for
        // tools/xbox-xiso.sh --menu-bake), not a file, unlike every other
        // mode this tool has - checked here rather than left to
        // bakeBackgroundArt to discover as a confusing mkdir failure.
        if (gameDir == ".") {
            std::cerr << "--bake-menu-art requires --game-dir <path to a gamedir, e.g. .../valve>\n";
            return 1;
        }
        if (outputPath == "../test_map.mpak") {
            std::cerr << "--bake-menu-art requires --output <directory> (an overlay tree, not a file)\n";
            return 1;
        }
        bool ok = formats::UiPacker::bakeBackgroundArt(gameDir, outputPath, 640, 480, 256, menuArtDxt1);
        return ok ? 0 : 1;
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
        // codex review, real finding: packMenuBackground's Xbox branch
        // repurposes MpakHeader6::lenTextureData for its own single DXT
        // blob (UiPacker.cpp's own comment there), written AFTER its meta
        // block (lenArray64) - but the real MPAK v9 per-texture-map
        // contract (tools/inspect_mpak.cpp's cursor walk, matching
        // PakManager.cpp save()'s own write order) places lenTextureData
        // BEFORE any array64 content and expects it to mean "the per-
        // texture-directory map's own DXT blob", not an unrelated single-
        // blob archive's. The two writers share one struct's field names
        // but not one section-order contract, so a --pack-ui --target
        // xbox archive's declared layout and its real byte layout
        // disagree - inspect_mpak.exe (correctly, per the real map
        // contract) refuses to parse it. Reject loudly here rather than
        // emit an archive whose own format is internally inconsistent -
        // matching --pack-menu --target xbox's own established rejection
        // pattern immediately above. Real fix (a distinct, self-consistent
        // menu-background archive shape, or aligning this writer with the
        // real v9 contract) is tracked as an open follow-up,
        // docs/r3d/divergences.md #101.
        if (pak.isXboxTarget()) {
            std::cerr << "--pack-ui --target xbox is not yet supported: its Xbox archive layout "
                          "conflicts with MPAK v9's own per-texture-map field contract (lenTextureData "
                          "means different things to the two writers, in a different section order) - "
                          "tools/inspect_mpak.exe correctly refuses to parse the result. "
                          "See docs/r3d/divergences.md #101.\n";
            return 1;
        }
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

        // X9-U: before the bake, does this map fit the Original Xbox at all
        // (Nv2aMapFit.h)? Its entity slots and the engine's own load come
        // straight from the source map, so a map that cannot load fails
        // here, before anything is written; PakManager::save repeats the
        // check on the finished archive and adds the renderer's side.
        if (pak.isXboxTarget() && pak.isProfileNv2a()) {
            formats::Nv2aWorldLumps lumps;
            lumps.visibility = bsp.getLumpData(formats::LUMP_VISIBILITY).size();
            lumps.nodes = bsp.getLumpData(formats::LUMP_NODES).size();
            lumps.leaves = bsp.getLumpData(formats::LUMP_LEAVES).size();
            lumps.marksurfaces = bsp.getLumpData(formats::LUMP_MARKSURFACES).size();
            lumps.planes = bsp.getLumpData(formats::LUMP_PLANES).size();
            lumps.clipnodes = bsp.getLumpData(formats::LUMP_CLIPNODES).size();
            lumps.models = bsp.getLumpData(formats::LUMP_MODELS).size();
            lumps.entities = bsp.getLumpData(formats::LUMP_ENTITIES).size();
            lumps.texinfo = bsp.getLumpData(formats::LUMP_TEXINFO).size();
            lumps.vertexes = bsp.getLumpData(formats::LUMP_VERTICES).size();
            lumps.edges = bsp.getLumpData(formats::LUMP_EDGES).size();
            lumps.surfedges = bsp.getLumpData(formats::LUMP_SURFEDGES).size();
            lumps.faces = bsp.getLumpData(formats::LUMP_FACES).size();
            lumps.lighting = bsp.getLumpData(formats::LUMP_LIGHTING).size();
            const auto& miptex = bsp.getLumpData(formats::LUMP_TEXTURES);
            int32_t textures = 0;
            if (miptex.size() >= sizeof(textures))
                std::memcpy(&textures, miptex.data(), sizeof(textures));
            lumps.textures = textures > 0 ? static_cast<uint32_t>(textures) : 0u;
            lumps.miptex = formats::nv2aHeaderOnlyMiptexBytes(lumps.textures);
            {
                // R5: the water's CPU-side pieces, counted from the source
                // lumps the engine will load (Nv2aWater.h).
                formats::Nv2aWaterLumps w;
                const auto& faces = bsp.getLumpData(formats::LUMP_FACES);
                const auto& surfedges = bsp.getLumpData(formats::LUMP_SURFEDGES);
                const auto& edges = bsp.getLumpData(formats::LUMP_EDGES);
                const auto& vertexes = bsp.getLumpData(formats::LUMP_VERTICES);
                const auto& texinfo = bsp.getLumpData(formats::LUMP_TEXINFO);
                w.faces = faces.data(); w.facesLen = faces.size();
                w.surfedges = surfedges.data(); w.surfedgesLen = surfedges.size();
                w.edges = edges.data(); w.edgesLen = edges.size();
                w.vertexes = vertexes.data(); w.vertexesLen = vertexes.size();
                w.texinfo = texinfo.data(); w.texinfoLen = texinfo.size();
                w.miptex = miptex.data(); w.miptexLen = miptex.size();
                lumps.waterPolyBytes = formats::nv2aWaterCount(w).polyBytes;
            }
            const auto& ents = bsp.getLumpData(formats::LUMP_ENTITIES);
            const uint32_t peak = formats::nv2aPeakEdicts(reinterpret_cast<const char*>(ents.data()), ents.size());
            const uint32_t maxEdicts = readGameMaxEdicts(gameDir);
            const formats::Nv2aFit fit = formats::nv2aCheckBeforeBake(lumps, peak, maxEdicts);
            pak.setEdictBudget(peak, maxEdicts);
            if (!fit.ok) {
                std::cerr << "ERROR: " << mapPath << " does not fit the Original Xbox (tools/asset-studio/src/formats/Nv2aMapFit.h). Not baking it.\n"
                          << fit.reason;
                return 1;
            }
            std::cout << "Xbox fit (before the bake): " << peak << " of " << maxEdicts << " edict slots, engine load "
                      << formats::nv2aMiB(formats::nv2aEngineWorldBytes(lumps) + formats::nv2aClassicPrefixBytes(lumps)) << "\n";
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
    if (!buildGlobal && !verifyPath.empty()) {
        if (!pak.verifyFile(verifyPath)) {
            std::cerr << "Verification FAILED: " << verifyPath << " does not match a fresh bake of " << mapPath << "\n";
            return 1;
        }
        std::cout << "Verified: " << verifyPath << " matches a fresh bake of " << mapPath << " lump for lump\n";
        return 0;
    }

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
