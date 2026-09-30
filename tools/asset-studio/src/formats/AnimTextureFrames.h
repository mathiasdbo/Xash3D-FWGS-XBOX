#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace formats {

    // X9-X item 14 step 9: which textures of a map's miptex lump belong to an
    // animation group that a face draws, although no face draws them itself.
    //
    // The engine links a map's animated textures into chains by name when it
    // loads the world (Mod_SequenceAnimatedTexture, engine/common/
    // mod_bmodel.c:3043; root clone mod_bmodel.c:2967): a texture whose name
    // starts with '+' or '-' and whose second character is '0'..'9' (a
    // frame) or 'a'..'j' (an alternate frame) is grouped with every other
    // such texture whose name from the third character on is the same
    // (Mod_NameImpliesTextureIsAnimated, mod_bmodel.c:703; the comparison is
    // Q_strcmp( altTexture->name + 2, baseTexture->name + 2 ), and it does
    // not look at the first character, so "+0foo" and "-1foo" are one
    // group). The renderer then draws a face's texture by walking that chain
    // (R_TextureAnimation, ref/gl/gl_rsurf.c:518): frames by time, the
    // alternate set when the entity's frame is non-zero, and the '-' random
    // tiles by the face's position. A compiler writes only the texture the
    // mapper picked into a face's texinfo, usually "+0..." or "-0...", so
    // every other frame of the group is referenced by nothing but the chain.
    //
    // The nv2a bake keeps only the textures a face draws
    // (PakManager::pruneUnusedXboxTextures) and so dropped all of those
    // frames. The bake now appends them to the texture directory, after the
    // textures faces draw, in miptex order - and ref/nv2a rebuilds the same
    // list from the engine's own texture_t names to find them
    // (ref/nv2a/nv2a_texanim_util.c, nv2a_texanim_extra_frames). The two
    // implementations must select the same textures in the same order; this
    // function is the baker's half and is unit-tested in
    // tests/anim_texture_frames_test.cpp against the same cases as the host
    // test of the engine's half.
    //
    // names: each miptex's name, lower-cased (the engine lower-cases in
    // Mod_LoadTexture, mod_bmodel.c:3027), empty for an entry with no header.
    // used: whether some face's texinfo names that miptex.
    // Returns the indices of the miptex entries that are unused, animated and
    // in a group with at least one used animated texture, ascending.

    // texture_t::name is char[16] and Mod_LoadTexture copies it with
    // Q_strnlwr( ..., sizeof( texture->name )), so the engine sees at most
    // 15 characters of a 16-character miptex name. Compare what it sees.
    inline std::string engineTextureName(const std::string& lowerName) {
        return lowerName.size() > 15 ? lowerName.substr(0, 15) : lowerName;
    }

    // Mod_NameImpliesTextureIsAnimated (mod_bmodel.c:703).
    inline bool nameImpliesAnimated(const std::string& name) {
        if (name.size() < 2) return false;
        if (name[0] != '-' && name[0] != '+') return false;
        const char c = name[1];
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'j');
    }

    inline std::vector<uint32_t> unreferencedAnimationFrames(const std::vector<std::string>& names,
                                                             const std::vector<bool>& used) {
        std::vector<uint32_t> out;
        const size_t count = names.size();
        std::vector<std::string> engineNames(count);
        for (size_t i = 0; i < count; ++i) engineNames[i] = engineTextureName(names[i]);

        for (size_t j = 0; j < count; ++j) {
            if ((j < used.size() && used[j]) || !nameImpliesAnimated(engineNames[j])) continue;
            const std::string group = engineNames[j].substr(2);
            for (size_t i = 0; i < count; ++i) {
                if (i >= used.size() || !used[i] || !nameImpliesAnimated(engineNames[i])) continue;
                if (engineNames[i].compare(2, std::string::npos, group) == 0) {
                    out.push_back(static_cast<uint32_t>(j));
                    break;
                }
            }
        }
        return out;
    }

} // namespace formats
