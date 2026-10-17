/* nxdk-ex's OpenAL, built into the engine for the real APU
   (docs/r3d/divergences.md #178): the pattern of its samples' mmio/
   wrappers, lib/openal/README.md "Building for the real APU" */
#define OPENAL_APU_REAL_MMIO 1
/* One source (the mix), so 8 slots, not 256: ~40 KiB of bss. Every library
   file must agree on it (apu_voice_mgr.c sizes a table by it). */
#define AL_MAX_SOURCES 8
#include "../../../../3rdparty/nxdk-ex/lib/openal/src/apu_voice.c"
