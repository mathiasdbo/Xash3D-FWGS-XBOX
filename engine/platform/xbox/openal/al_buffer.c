/* nxdk-ex's OpenAL, built into the engine for the real APU
   (docs/r3d/divergences.md #178): the pattern of its samples' mmio/
   wrappers, lib/openal/README.md "Building for the real APU" */
#define OPENAL_APU_REAL_MMIO 1
/* One source (the mix), so 8 slots, not 256: ~40 KiB of bss. Every library
   file must agree on it (apu_voice_mgr.c sizes a table by it). */
#define AL_MAX_SOURCES 8
/* The output queues at most XBOX_SND_AL_MAX_BUFFERS (32): 64 names instead of
   the library's 2048 saves ~170 KiB of bss. Only al_buffer.c sizes by it.
   X12 5b (sounds on voices) needs more; raise it there. */
#define AL_MAX_BUFFERS 64
#include "../../../../3rdparty/nxdk-ex/lib/openal/src/al_buffer.c"
