cmake_minimum_required(VERSION 3.16)
project(AssetStudio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Everything but main.cpp, so the unit tests link the real code rather
# than a copy of it.
add_library(AssetStudioFormats STATIC
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
target_include_directories(AssetStudioFormats PUBLIC src)

add_executable(AssetStudio src/main.cpp)
target_link_libraries(AssetStudio PRIVATE AssetStudioFormats)

# XB's own "two censuses first" (docs/r3d/fork-plan.md): one-shot
# measurement tools over a retail install, not part of the baking
# pipeline - their own answers are meant to be read once and written
# into the fork plan, not run on every build.
add_executable(census_lightstyles tools/census_lightstyles.cpp)
target_link_libraries(census_lightstyles PRIVATE AssetStudioFormats)
add_executable(census_bones tools/census_bones.cpp)
target_link_libraries(census_bones PRIVATE AssetStudioFormats)

# `ctest -C Release` from the build directory runs them; build-test-package.ps1
# does not, on purpose - a bake is the integration test, these are the unit
# tests for the pieces that have no retail data to bake.
enable_testing()
add_executable(miptex_lump_test tests/miptex_lump_test.cpp)
target_link_libraries(miptex_lump_test PRIVATE AssetStudioFormats)
add_test(NAME miptex_lump_test COMMAND miptex_lump_test)
