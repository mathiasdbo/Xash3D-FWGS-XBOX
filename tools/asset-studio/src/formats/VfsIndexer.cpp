#include "VfsIndexer.h"
#include <iostream>
#include <algorithm>
#include <cctype>

namespace formats {

    void VfsIndexer::scanDirectory(const std::string& rootDir) {
        mEntries.clear();
        mFileNameToPathMap.clear();
        mRelPathToPathMap.clear();
        mWadCount = mBspCount = mMdlCount = mWavCount = mSprCount = 0;

        std::filesystem::path rootPath(rootDir);
        if (!std::filesystem::exists(rootPath)) {
            std::cerr << "VFS Indexer: Root path does not exist: " << rootDir << "\n";
            return;
        }

        std::cout << "VFS Indexer: Scanning game root directory recursively: " << rootDir << "\n";

        for (const auto& entry : std::filesystem::recursive_directory_iterator(rootPath, std::filesystem::directory_options::skip_permission_denied)) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return std::tolower(c); });

                std::string fileName = entry.path().filename().string();
                std::string fileNameLower = fileName;
                std::transform(fileNameLower.begin(), fileNameLower.end(), fileNameLower.begin(), [](unsigned char c){ return std::tolower(c); });

                std::string relPath = std::filesystem::relative(entry.path(), rootPath).string();
                std::replace(relPath.begin(), relPath.end(), '\\', '/');

                VfsFileEntry fEntry;
                fEntry.relativePath = relPath;
                fEntry.fullPath = entry.path().string();
                fEntry.fileName = fileName;
                fEntry.sizeBytes = entry.file_size();

                mEntries.push_back(fEntry);
                mFileNameToPathMap[fileNameLower] = fEntry.fullPath;

                std::string relPathLower = relPath;
                std::transform(relPathLower.begin(), relPathLower.end(), relPathLower.begin(), [](unsigned char c){ return std::tolower(c); });
                mRelPathToPathMap[relPathLower] = fEntry.fullPath;

                if (ext == ".wad") mWadCount++;
                else if (ext == ".bsp") mBspCount++;
                else if (ext == ".mdl") mMdlCount++;
                else if (ext == ".wav") mWavCount++;
                else if (ext == ".spr") mSprCount++;
            }
        }

        std::cout << "VFS Indexing Complete: Registered " << mEntries.size() << " files ("
                  << mWadCount << " WADs, " << mBspCount << " BSP maps, "
                  << mMdlCount << " StudioModels, " << mWavCount << " WAV audio, "
                  << mSprCount << " Sprites)\n";
    }

    std::vector<std::string> VfsIndexer::findFilesByExtension(const std::string& ext) const {
        std::string targetExt = ext;
        if (targetExt.front() != '.') targetExt = "." + targetExt;
        std::transform(targetExt.begin(), targetExt.end(), targetExt.begin(), [](unsigned char c){ return std::tolower(c); });

        std::vector<std::string> results;
        for (const auto& entry : mEntries) {
            std::filesystem::path p(entry.fileName);
            std::string fExt = p.extension().string();
            std::transform(fExt.begin(), fExt.end(), fExt.begin(), [](unsigned char c){ return std::tolower(c); });
            if (fExt == targetExt) {
                results.push_back(entry.fullPath);
            }
        }
        return results;
    }

    std::string VfsIndexer::findFileByName(const std::string& fileName) const {
        // Normalize: lowercase, backslashes -> forward slashes (entity-lump asset
        // paths like "models/player.mdl" are relative paths, not bare filenames -
        // check the relative-path map first, falling back to basename-only lookup
        // for callers that only ever had a bare filename (WAD names, the map path).
        std::string normalized = fileName;
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c){ return std::tolower(c); });

        auto relIt = mRelPathToPathMap.find(normalized);
        if (relIt != mRelPathToPathMap.end()) {
            return relIt->second;
        }

        std::filesystem::path p(normalized);
        std::string nameLower = p.filename().string();

        auto it = mFileNameToPathMap.find(nameLower);
        if (it != mFileNameToPathMap.end()) {
            return it->second;
        }
        return "";
    }

} // namespace formats
