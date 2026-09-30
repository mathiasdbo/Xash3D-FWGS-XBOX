// Unit test for AnimTextureFrames.h: which unreferenced miptex entries the
// nv2a bake keeps because they are frames of an animation group a face
// draws (X9-X item 14 step 9). The same cases are in
// tools/host-tests/nv2a_texanim_util_host_test.c for ref/nv2a's half
// (nv2a_texanim_extra_frames), which must select the same entries.
//
// The last case drives PakManager::pruneUnusedXboxTextures with an append
// list: the appended entries must land after the compacted drawn set, in
// the order given. The BspParser wiring between the two is covered by a
// real bake (the "kept N animation frame(s)" line and the archive's
// textureCount), not here.
#include "formats/AnimTextureFrames.h"
#include "formats/PakManager.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace formats;

namespace {

int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); ++failures; } } while (0)

bool same(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) { return a == b; }

} // namespace

int main() {
    // A used +0 frame pulls in its +1..+n frames and its +a alternates,
    // in miptex order; an unrelated group with no used member stays out.
    {
        std::vector<std::string> names = { "wall", "+0screen", "+1screen", "+2screen", "+ascreen", "+0other", "+1other" };
        std::vector<bool> used      = { true,   true,       false,      false,      false,      false,     false };
        CHECK(same(unreferencedAnimationFrames(names, used), { 2, 3, 4 }), "+0 used: +1, +2 and +a kept; the unused group is not");
    }
    // A used alternate alone (a light switched off in the map) pulls in the +0 set.
    {
        std::vector<std::string> names = { "+0~light", "+a~light" };
        std::vector<bool> used      = { false,      true };
        CHECK(same(unreferencedAnimationFrames(names, used), { 0 }), "+a used: +0 kept");
    }
    // '-' random tiles group like '+' frames.
    {
        std::vector<std::string> names = { "-0out_w3", "-1out_w3", "-2out_w3" };
        std::vector<bool> used      = { true,      false,      true };
        CHECK(same(unreferencedAnimationFrames(names, used), { 1 }), "-0 and -2 used: -1 kept, used ones not repeated");
    }
    // The engine groups by name + 2 only, so '+' and '-' of the same tail are one group.
    {
        std::vector<std::string> names = { "+0foo", "-1foo" };
        std::vector<bool> used      = { true,   false };
        CHECK(same(unreferencedAnimationFrames(names, used), { 1 }), "prefix character is not part of the group key");
    }
    // Names that fail Mod_NameImpliesTextureIsAnimated are never kept, nor do they pull others in.
    {
        std::vector<std::string> names = { "+kbad", "+0bad", "+", "", "-abad" };
        std::vector<bool> used      = { true,    false,   false, false, false };
        CHECK(unreferencedAnimationFrames(names, used).empty(), "+k is not animated, so it groups nothing");
        std::vector<bool> used2     = { false,   false,   false, false, true };
        CHECK(same(unreferencedAnimationFrames(names, used2), { 1 }), "-a (alternate) used: +0bad kept, +kbad not");
    }
    // A 16-character name is 15 characters to the engine (texture_t::name[16]).
    {
        std::vector<std::string> names = { "+0abcdefghijklmn", "+1abcdefghijklmX" }; // same first 15 characters
        std::vector<bool> used      = { true,               false };
        CHECK(same(unreferencedAnimationFrames(names, used), { 1 }), "names compared as the engine truncates them");
    }
    // Nothing used, nothing kept.
    {
        std::vector<std::string> names = { "+0a", "+1a" };
        std::vector<bool> used      = { false, false };
        CHECK(unreferencedAnimationFrames(names, used).empty(), "no used member, no frames kept");
    }

    // PakManager: the appended textures follow the compacted drawn set, in the given order.
    {
        PakManager pak;
        pak.setTargetXbox(true);
        const uint8_t red[4 * 4 * 4] = {};
        uint8_t blue[4 * 4 * 4];
        for (size_t i = 0; i < sizeof(blue); ++i) blue[i] = static_cast<uint8_t>(i);
        TextureLocation a = pak.addTexture(4, 4, red);   // drawn
        TextureLocation b = pak.addTexture(8, 4, blue);  // not drawn, appended
        TextureLocation c = pak.addTexture(4, 8, red);   // not drawn, dropped
        TextureLocation d = pak.addTexture(4, 4, blue);  // drawn
        (void)c;
        std::vector<MpakTextureBatch> batches(2);
        batches[0].textureIndex = d.textureIndex;
        batches[1].textureIndex = a.textureIndex;
        pak.setTextureBatches(batches);
        const size_t dropped = pak.pruneUnusedXboxTextures({ b.textureIndex });
        CHECK(dropped == 2, "two textures no batch draws were dropped from the drawn set");
        CHECK(pak.xboxTextureCountForTest() == 3, "two drawn + one appended");
        CHECK(pak.xboxTextureWidthForTest(2) == 8 && pak.xboxTextureHeightForTest(2) == 4, "the appended entry is b's image, last");
        CHECK(pak.textureBatchIndexForTest(0) == 1 && pak.textureBatchIndexForTest(1) == 0, "batches remapped to the compacted order");
    }

    if (failures == 0) std::printf("anim_texture_frames_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
