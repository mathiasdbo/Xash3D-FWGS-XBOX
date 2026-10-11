cmake_minimum_required(VERSION 3.16)
project(AssetStudio LANGUAGES C CXX)

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
    src/formats/Nv2aWater.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ref/nv2a/nv2a_warp_util.c
    src/formats/MusicBake.cpp
    src/formats/music_adpcm_encode.c
    src/formats/music_mp3_decode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../engine/platform/xbox/xbox_music_adpcm.c
)
# R5 (docs/r3d/divergences.md #147): the baker counts a map's water vertices
# with the renderer's own subdivision and water-texture rule
# (src/formats/Nv2aWater.h), so the C file is compiled here as it is in
# ref/nv2a. Its include paths are scoped to the two files that need them:
# ref/nv2a for nv2a_warp_util.h, engine/ for the sine table it includes
# (warpsin.h).
set_source_files_properties(src/formats/Nv2aWater.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../../ref/nv2a/nv2a_warp_util.c
    PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/../../ref/nv2a;${CMAKE_CURRENT_SOURCE_DIR}/../../engine")
# ../../filesystem: shares filesystem/mpak_texture_dir.h's v10 texture-
# directory rule (chain length, 128-byte padding) with the C reader
# (ref/nv2a) and the PC tests, rather than PakManager.cpp keeping its own
# copy of it - see that header's own file comment. Header-only, so this
# adds nothing to link; checked for name collisions against everything
# already under src/, found none.
target_include_directories(AssetStudioFormats PUBLIC src ${CMAKE_CURRENT_SOURCE_DIR}/../../filesystem)

# divergences.md #171 (x12 music bake): the music tracks are decoded with
# the engine's own libmpg, compiled here from the engine's tree with no
# defines of its own - none of its files test any (an XASH_* or platform
# macro reaches it only through xash3d_types.h's types; ACCURATE_ROUNDING
# is set inside mpg123.h for both) - so the decode is the engine's code.
# What differs from the console is the compiler and the C library: MSVC
# x64's SSE2 against clang's i386 code (its float math in SSE, its double
# expressions - the sin/cos/pow table setup in tabinit.c and layer3.c - on
# the x87), and the libm under those tables. A last-bit difference in a
# table entry can move sample LSBs; r3d_music_crc measures it on the
# console against the CRC baked here. The engine's
# baked-music decoder (engine/platform/xbox/xbox_music_adpcm.c) is
# compiled in too: the encoder scores every code through it and the bake
# reads its own output back through it. Include paths are scoped to the
# files that need them, as for nv2a_warp_util.c above.
set(R3D_LIBMPG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../engine/client/soundlib/libmpg)
file(GLOB R3D_LIBMPG_SOURCES CONFIGURE_DEPENDS ${R3D_LIBMPG_DIR}/*.c)
add_library(AssetStudioLibmpg STATIC ${R3D_LIBMPG_SOURCES})
set(R3D_LIBMPG_INCLUDES ${R3D_LIBMPG_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../../common ${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty/library_suffix/include)
target_include_directories(AssetStudioLibmpg PRIVATE ${R3D_LIBMPG_INCLUDES})
set_source_files_properties(src/formats/music_mp3_decode.c
    PROPERTIES INCLUDE_DIRECTORIES "${R3D_LIBMPG_INCLUDES}")
set_source_files_properties(src/formats/MusicBake.cpp src/formats/music_adpcm_encode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../engine/platform/xbox/xbox_music_adpcm.c
    PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/../../engine/platform/xbox;${CMAKE_CURRENT_SOURCE_DIR}/src/formats")
target_link_libraries(AssetStudioFormats PUBLIC AssetStudioLibmpg)

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

add_executable(worldspawn_wad_test tests/worldspawn_wad_test.cpp)
target_link_libraries(worldspawn_wad_test PRIVATE AssetStudioFormats)
add_test(NAME worldspawn_wad_test COMMAND worldspawn_wad_test)

add_executable(texture_opt_test tests/texture_opt_test.cpp)
target_link_libraries(texture_opt_test PRIVATE AssetStudioFormats)
add_test(NAME texture_opt_test COMMAND texture_opt_test)

add_executable(dxt1_mip_chain_test tests/dxt1_mip_chain_test.cpp)
target_link_libraries(dxt1_mip_chain_test PRIVATE AssetStudioFormats)
add_test(NAME dxt1_mip_chain_test COMMAND dxt1_mip_chain_test)
add_executable(nv2a_map_budget_test tests/nv2a_map_budget_test.cpp)
target_link_libraries(nv2a_map_budget_test PRIVATE AssetStudioFormats)
add_test(NAME nv2a_map_budget_test COMMAND nv2a_map_budget_test)

add_executable(nv2a_map_fit_test tests/nv2a_map_fit_test.cpp)
target_link_libraries(nv2a_map_fit_test PRIVATE AssetStudioFormats)
add_test(NAME nv2a_map_fit_test COMMAND nv2a_map_fit_test)

add_executable(lightmap_atlas_packer_test tests/lightmap_atlas_packer_test.cpp)
target_link_libraries(lightmap_atlas_packer_test PRIVATE AssetStudioFormats)
add_test(NAME lightmap_atlas_packer_test COMMAND lightmap_atlas_packer_test)

add_executable(phs_builder_test tests/phs_builder_test.cpp)
target_link_libraries(phs_builder_test PRIVATE AssetStudioFormats)
add_test(NAME phs_builder_test COMMAND phs_builder_test)

add_executable(menu_art_bake_test tests/menu_art_bake_test.cpp)
target_link_libraries(menu_art_bake_test PRIVATE AssetStudioFormats)
add_test(NAME menu_art_bake_test COMMAND menu_art_bake_test)

add_executable(sky_art_bake_test tests/sky_art_bake_test.cpp)
target_link_libraries(sky_art_bake_test PRIVATE AssetStudioFormats)
add_test(NAME sky_art_bake_test COMMAND sky_art_bake_test)

add_executable(anim_texture_frames_test tests/anim_texture_frames_test.cpp)
target_link_libraries(anim_texture_frames_test PRIVATE AssetStudioFormats)
add_test(NAME anim_texture_frames_test COMMAND anim_texture_frames_test)


# divergences.md #171: the music bake on a synthetic track (never retail
# content): libmpg's decode against the stream model, the encoder and the
# engine's reader on the baker's own output.
add_executable(music_bake_test tests/music_bake_test.cpp)
target_link_libraries(music_bake_test PRIVATE AssetStudioFormats)
set_source_files_properties(tests/music_bake_test.cpp
    PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/../../engine/platform/xbox;${CMAKE_CURRENT_SOURCE_DIR}/src/formats")
add_test(NAME music_bake_test COMMAND music_bake_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/music_synthetic.mp3)
