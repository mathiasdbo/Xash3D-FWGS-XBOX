#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>

namespace formats {

    struct VfsFileEntry {
        std::string relativePath;
        std::string fullPath;
        std::string fileName;
        uintmax_t sizeBytes = 0;
    };

    class VfsIndexer {
    public:
        VfsIndexer() = default;
        void scanDirectory(const std::string& rootDir);

        const std::vector<VfsFileEntry>& getEntries() const { return mEntries; }
        std::vector<std::string> findFilesByExtension(const std::string& ext) const;
        std::string findFileByName(const std::string& fileName) const;

        size_t countWads() const { return mWadCount; }
        size_t countBsps() const { return mBspCount; }
        size_t countMdls() const { return mMdlCount; }
        size_t countWavs() const { return mWavCount; }
        size_t countSprs() const { return mSprCount; }

    private:
        std::vector<VfsFileEntry> mEntries;
        std::unordered_map<std::string, std::string> mFileNameToPathMap;
        std::unordered_map<std::string, std::string> mRelPathToPathMap;
        size_t mWadCount = 0;
        size_t mBspCount = 0;
        size_t mMdlCount = 0;
        size_t mWavCount = 0;
        size_t mSprCount = 0;
    };

} // namespace formats
