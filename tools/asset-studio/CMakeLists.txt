cmake_minimum_required(VERSION 3.16)
project(FerrumAssetStudio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(FerrumAssetStudio
    src/main.cpp
    src/formats/BspParser.cpp
    src/formats/PakManager.cpp
    src/formats/WadParser.cpp
    src/formats/VfsIndexer.cpp
    src/formats/MdlParser.cpp
    src/formats/SprParser.cpp
    src/formats/UiPacker.cpp
    src/formats/BmpParser.cpp
    src/utils/TextureOpt.cpp
    src/utils/rgbcx.cpp
)
