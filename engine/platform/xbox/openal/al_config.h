/* nxdk-ex's OpenAL as the engine builds it (docs/r3d/divergences.md #178):
   every wrapper in this folder includes this before its library file, so
   the library files agree on it (apu_voice_mgr.c sizes a table by
   AL_MAX_SOURCES, al_source.c its pool, al_buffer.c its by AL_MAX_BUFFERS). */
#ifndef R3D_AL_CONFIG_H
#define R3D_AL_CONFIG_H

#include "al_limits.h"

#define OPENAL_APU_REAL_MMIO 1
#define AL_MAX_SOURCES R3D_AL_SOURCES
#define AL_MAX_BUFFERS R3D_AL_BUFFERS

#endif
