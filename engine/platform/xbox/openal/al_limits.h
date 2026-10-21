/* The OpenAL sources and buffer names the engine can hold at once: the
   library's AL_MAX_SOURCES and AL_MAX_BUFFERS as al_config.h builds it,
   here alone so the engine's own files can check against them without
   al_config.h's OPENAL_APU_REAL_MMIO (which AL/alext.h reads). Both were 8
   and 64 while the mix was the only source; on the console stage A's 7th
   voice failed alGenSources, then every sound and the music failed
   alGenBuffers, with AL_OUT_OF_MEMORY (2026-10-19). */
#ifndef R3D_AL_LIMITS_H
#define R3D_AL_LIMITS_H

/* The mix and the room stream (#178, #180), the 64 sound voices (#182,
   xbox_snd_voices.c checks it at build time), the music (#183), and the 8
   speech feeds (#184, xbox_snd_feed.c checks it). 96, not the library's
   256: about 260 bytes of bss each, and the voice tick walks only up to
   the highest in use. */
#define R3D_AL_SOURCES 96

/* The mix's and the room stream's queues (XBOX_SND_AL_MAX_BUFFERS, 32
   each), the music's (64, xbox_snd_music_voice.c checks it), 512 for the
   sounds on voices, one per resident sound played (a map holds 200-300;
   past it a sound stays in the CPU mix), and 64 for the speech feeds'
   pieces (8 a feed, #184, xbox_snd_feed.c checks it). 704, not the
   library's 2048: about 88 bytes of bss each. */
#define R3D_AL_BUFFERS 704

#endif
