#ifndef MDLPARSER_H
#define MDLPARSER_H

#include <string>
#include <vector>
#include <cstdint>
#include "PakManager.h"

namespace formats {

#pragma pack(push, 1)
    struct studiohdr_t {
        int32_t id;         // "IDST"
        int32_t version;    // typically 10 for GoldSrc
        char name[64];
        int32_t length;
        float eyeposition[3];
        float min[3];
        float max[3];
        float bbmin[3];
        float bbmax[3];
        int32_t flags;
        
        int32_t numbones;
        int32_t boneindex;
        
        int32_t numbonecontrollers;
        int32_t bonecontrollerindex;
        
        int32_t numhitboxes;
        int32_t hitboxindex;
        
        int32_t numseq;
        int32_t seqindex;
        
        int32_t numseqgroups;
        int32_t seqgroupindex;
        
        int32_t numtextures;
        int32_t textureindex;
        int32_t texturedataindex;
        
        int32_t numskinref;
        int32_t numskinfamilies;
        int32_t skinindex;
        
        int32_t numbodyparts;
        int32_t bodypartindex;
        
        int32_t numattachments;
        int32_t attachmentindex;
        
        int32_t soundtable;
        int32_t soundindex;
        int32_t soundgroups;
        int32_t soundgroupindex;
        
        int32_t numtransitions;
        int32_t transitionindex;
    };

    struct mstudiotexture_t {
        char name[64];
        int32_t flags;
        int32_t width;
        int32_t height;
        int32_t index;
    };
#pragma pack(pop)

    // GoldSrc BSP30/studio.h texinfo flag: masked (keyed) transparency, index 255 is
    // the transparent key. Named STUDIO_NF_MASKED in xash3d-fwgs; hlsdk's own
    // studio.h calls the same bit "TRANSPARENT" - Xash's naming is followed here
    // since it's what actually renders retail content correctly.
    constexpr int32_t STUDIO_NF_MASKED = 0x0040;

    struct MdlTexture {
        std::string name;
        uint32_t width;
        uint32_t height;
        int32_t flags = 0; // STUDIO_NF_* - preserved into the patched mstudiotexture_t
        std::vector<uint8_t> rgba; // 32-bit decoded texture
    };

    class MdlParser {
    public:
        MdlParser() = default;
        bool load(const std::string& filePath);

        const studiohdr_t& getHeader() const { return mHeader; }
        const std::vector<MdlTexture>& getTextures() const { return mTextures; }

        std::vector<uint8_t> getPatchedData(const std::vector<TextureLocation>& locs);

    private:
        // Parses a texture header table + pixel/palette data from an arbitrary raw
        // MDL buffer (the main file, or a spliced T.mdl) into outTextures. Every
        // offset is bounds-checked against rawData's size; a texture whose pixel
        // data doesn't fit gets a solid-magenta placeholder (sized correctly) rather
        // than being dropped, so texture indices stay parallel with the source
        // header's numtextures/skinref table.
        static void parseTextures(const std::vector<uint8_t>& rawData, const studiohdr_t& hdr, std::vector<MdlTexture>& outTextures);
        // Parses the numskinfamilies x numskinref int16 skin-remap table. Falls back
        // to an identity table (one family, skinref[i] = i) if the declared table is
        // out of bounds or the model has no skin data at all.
        static void parseSkinTable(const std::vector<uint8_t>& rawData, const studiohdr_t& hdr, uint32_t numTextures, std::vector<int16_t>& outSkinRefs, int32_t& outNumSkinFamilies, int32_t& outNumSkinRef);

        studiohdr_t mHeader;
        std::vector<MdlTexture> mTextures;
        std::vector<int16_t> mSkinRefs;
        int32_t mNumSkinFamilies = 0;
        int32_t mNumSkinRef = 0;
        // True when mHeader.numtextures was 0 and texture data was spliced in from a
        // <stem>T.mdl sibling - the main file never had a texture header table, so
        // getPatchedData must not try to truncate at its (meaningless) textureindex.
        bool mIsSpliced = false;
        // The ORIGINAL (pre-patch) textureindex, valid only when !mIsSpliced. Texture
        // headers always precede their pixel payloads in a real GoldSrc MDL, so this
        // is also the correct truncation point for discarding old headers/skin table/
        // pixel data before appending freshly rebuilt versions of all three.
        int32_t mOriginalTextureIndex = 0;
        std::vector<uint8_t> mRawData;
    };

} // namespace formats

#endif // MDLPARSER_H
