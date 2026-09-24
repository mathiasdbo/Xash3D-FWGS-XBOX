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
# ../../filesystem: shares filesystem/mpak_texture_dir.h's v10 texture-
# directory rule (chain length, 128-byte padding) with the C reader
# (ref/nv2a) and the PC tests, rather than PakManager.cpp keeping its own
# copy of it - see that header's own file comment. Header-only, so this
# adds nothing to link; checked for name collisions against everything
# already under src/, found none.
target_include_directories(AssetStudioFormats PUBLIC src ${CMAKE_CURRENT_SOURCE_DIR}/../../filesystem)

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
add_executable(inspect_mpak tools/inspect_mpak.cpp)
target_link_libraries(inspect_mpak PRIVATE AssetStudioFormats)

# `ctest -C Release` from the build directory runs them; build-test-package.ps1
# does not, on purpose - a bake is the integration test, these are the unit
# tests for the pieces that have no retail data to bake.
enable_testing()
add_executable(miptex_lump_test tests/miptex_lump_test.cpp)
target_link_libraries(miptex_lump_test PRIVATE AssetStudioFormats)
add_test(NAME miptex_lump_test COMMAND miptex_lump_test)

add_executable(texture_opt_test tests/texture_opt_test.cpp)
target_link_libraries(texture_opt_test PRIVATE AssetStudioFormats)
add_test(NAME texture_opt_test COMMAND texture_opt_test)

add_executable(dxt1_mip_chain_test tests/dxt1_mip_chain_test.cpp)
target_link_libraries(dxt1_mip_chain_test PRIVATE AssetStudioFormats)
add_test(NAME dxt1_mip_chain_test COMMAND dxt1_mip_chain_test)
