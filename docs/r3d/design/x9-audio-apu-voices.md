# X9 design: X12 step 5b, sounds on APU voices

Input for X12 item 5 step 5b (`docs/r3d/fork-plan.md`, "Step 5b, sounds on
APU voices"), written 2026-10-19 on branch `x9-audio-gp-room` after the
room effect moved onto the GP (#180). Read against `engine/client/sound/s_mix.c`,
`s_main.c`, `common/sound_api.h` and nxdk-ex 0c77b1d (`lib/openal`). Nothing
here has run on a console yet. Line numbers drift: search for the named
function.

## 0. What this tree already gives 5b

- **Every normal channel goes through the room.** `S_MixNormalChannelsToRoombuffer`
  mixes all of `snd.channels` into `roombuffer`; `SX_RoomFX` processes it in
  place and `S_MixBufferWithGain` adds it, at the master volume, to the
  paint buffer. With #180 that whole step is the GP kernel `gp_hldsp`: its
  input is the slot's send (the room stream at 1.0), its output replaces the
  room buffer. So **a channel on an APU voice is a source whose direct path
  is muted and whose send goes into that same slot**, as the room stream's
  does. The kernel then sees the sum of the room stream and every voice,
  which is what `roombuffer` held.
- **The library** (`lib/openal/docs/ENGINE_INTEGRATION.md`): 120 voices (64
  on xemu); `alBufferDataStatic` plays the engine's memory in place, any
  start and end byte (3.10); `AL_LOOP_POINTS_SOFT` (3.3); direct mode,
  `AL_XBOX_DIRECT_GAINS` (3.2); `AL_XBOX_PRIORITY`, `AL_XBOX_VIRTUALIZE`
  `AL_FALSE`, `AL_XBOX_HAS_VOICE`; gapless queued buffers (3.10, sentences);
  Xbox ADPCM decoded by the APU (3.11, the baked music).
- **`channel_t`** (`common/sound_api.h`) has `engine_reserved[8]` for the
  voice's state, so no upstream struct changes. #177 holds `[0..1]` (its
  slot tokens; `run.sh`'s `vox_engine_reserved` keeps other code out of
  them); the voice takes `[2..]`, and that check learns about it.

## 1. The rule that keeps the game's view unchanged: the CPU still walks the channel

Upstream's channel state is read outside the mixer: `ch->sample` by save and
restore (`S_GetCurrentStaticSounds`/`S_GetCurrentDynamicSounds`, `samplePos`),
by `SND_GetChannelTimeLeft` (which channel a new sound steals), by
`S_AdjustLoopedSamplePosition`; `FL_CHAN_FINISHED` frees the channel;
`forced_end` cuts sentence words. If the voice's own position replaced
them, every one of those would read a different clock.

So a channel on a voice **still goes through `S_MixChannelToBuffer`**, with
the mix itself skipped: the same `S_AdjustNumSamples`, the same
`S_RetrieveAudioSamples` walk (a pointer, no copy), the same
`chan->sample += out_count * rate * timecompress_rate`, the same
`FL_CHAN_FINISHED`. Only `S_MixAudio` is not called. The cost is the loop's
arithmetic, a few dozen operations per channel per paint chunk, against a
multiply-add per sample. Everything that reads the channel reads exactly
upstream's values; the voice only makes the sound.

The same holds for the skips: a channel the loop `continue`s past (paused by
the menu in single player, inaudible under 8, the console with a background
map) has its voice paused, and it resumes where the walk resumes.

## 2. Timing: voices must start and stop on the stream's clock

The engine mixes `_snd_mixahead` (0.12 s) ahead of what is heard
(`xbox_snd_al.c`). A sound the CPU mixes from the paint chunk at
`snd.paintedtime` is heard about 0.12 s after it was started; a voice
started at that call is heard at once. Mixed together, voices would run
0.12 s ahead of every sound still on the CPU (the channels over the voice
budget, sentences until stage B, raw channels: cinematics, voice chat) and of
the room's parameter changes, which #180 times to the stream. And a voice
stopped at once (a sound replaced on its entity channel, `S_StopSound`) would
lose its last 0.12 s, which the CPU mix had already painted.

So the voice starts and stops **at the stream frame of the paint chunk** that
starts or ends it, the way `alXboxEffectSlotAtSample` times the kernel's
parameters. This is a library mechanism (generic: "start/stop a source at a
streaming source's frame"), requested in section 4. Volume and pitch changes
are not timed: they reach the voice at once, 0.12 s before the CPU mix
would have applied them, and the APU ramps them. That is a divergence only
in the direction of less latency (the listener's position and the entity's
are this frame's).

A sound that ends by itself is not stopped: the walk marks it finished
0.12 s before the voice reaches its end, and `S_FreeChannel` lets the voice
play out. Its source is reaped when the library reports it stopped.

## 3. 8-bit sounds must stay unsigned on the Xbox

Half-Life's effects are 8-bit. `Sound_LoadWAV` turns them into signed bytes
(`snd_wav.c`, "now convert 8-bit sounds to signed"), and the mixer reads
`int8_t`. The APU plays 8-bit only as unsigned (`apu_vp.c` `apu_vp_format`:
U8 in a 1-byte container); there is no signed 8-bit voice format. Playing the
cache in place (the RAM case of the owner question, 2026-10-13) therefore
means **the cache keeps the file's unsigned bytes on the Xbox**, and every
reader of 8-bit cache data reads `(int8_t)( b ^ 0x80 )`:

- the 8-bit mixers (`S_MixMono8`/`S_MixStereo8`, and the Pitch/Lerp ones of
  `xbox_s_mix_pitch.h`);
- `SND_MoveMouth8` (`s_mouth.c`);
- the sentence trim (`S_ShouldTrimSample8` and its two callers in `s_vox.c`);
- the dialogue windows (`xbox_vox_stream.c` XORs to signed on read: it stops
  doing so) and `S_R3D_VoxTrimData`;
- the resampler, when a sound is resampled at load (`Sound_Process`,
  `snd_utils.c`, width 1).

Each is one read changed under `#if XASH_XBOX`. The host tests
(`s_mix_host_test.c`, the vox stream tests) run the same sounds both ways and
must come out identical. The alternative, `alBufferData` copying every sound
into the library, holds every resident effect twice (several MiB per map).

## 4. What nxdk-ex needs (generic, nothing Half-Life-specific)

1. **Sends panned by the direct gains.** Today a mono source sends half its
   gain to each of the kernel's two input bins (`apu_voice.c`
   `apu_voice_send_atten`), and a stereo source its whole gain per channel
   (#47): neither follows `AL_XBOX_DIRECT_GAINS`. Half-Life's room input is
   panned (`leftvol`/`rightvol` per ear). Request: a source property
   (`AL_XBOX_SEND_PANNED`, default off) under which, in direct mode, send n's
   two outputs carry `send * left` and `send * right` (a mono voice's sample
   on both, a stereo voice's left and right on their own), whatever the
   direct path's own filter does. The engine mutes the direct path with a
   lowpass at gain 0, as the room stream does.
2. **Starts and stops at a stream frame.** `alXboxSourcePlayAtSample(source,
   stream, sample)` and `alXboxSourceStopAtSample(source, stream, sample)`:
   the source starts (stops) at the frame that mixes frame `sample` of the
   streaming source `stream`, its position as `alXboxEffectSlotAtSample`
   counts it. A start is exact if the library starts the voice at once at a
   negative offset into silence (the 0x00 or 0x80 pages the APU already reads
   after a one-shot); a stop sets the voice's end to the position it will
   have reached. While the stream is stopped or paused, both act at once.
3. **Already there, used as documented:** `alBufferDataStatic` (8-bit
   unsigned in place), `AL_LOOP_POINTS_SOFT` (a cue's loop start to the end),
   `AL_PITCH`, `AL_XBOX_PRIORITY`, `AL_XBOX_VIRTUALIZE` `AL_FALSE`.

## 5. Stages

- **A: effects.** Channels without `words`, whose sound is resident (not a
  #177 window, `S_R3D_ChannelSource` is the cache), 8- or 16-bit, mono or
  stereo, looping or not, go onto voices while `r3d_snd_voices 1` is in a
  `userconfig.d` cfg and the room is on the GP (#180; without it there is no
  kernel to send into, and they stay on the CPU). One source per channel,
  made when the channel first mixes and kept while its sound plays; gains
  `leftvol/256`, `rightvol/256` (the CPU's `(data * vol) >> 8` on 16-bit
  samples, the APU's 8-bit samples being the same scale), `AL_PITCH` the
  walk's `pitch` (upstream's `rate` is `pitch * sound_rate / out_rate`; the
  APU plays the buffer at `pitch * sound_rate`). A channel the library gives
  no voice (`AL_XBOX_HAS_VOICE` false after the start) mixes on the CPU from
  the walk's position, as today. The priority is the channel's loudness.
  - Divergences by design: the APU's interpolation, not `s_lerping`'s; volumes
    and pitch changed at once (section 2); the APU's 12-bit attenuation for the
    8-bit volumes.
- **A0, before A:** the 8-bit cache unsigned (section 3), on its own, with
  the host tests showing the CPU mix unchanged sample for sample.
- **B: sentences and streamed speech.** Designed in section 7: the walk
  feeds what it would have mixed onto a streaming source, so `timecompress`,
  the trims and #177's windows need nothing new; per-word pitch and volume
  go to the voice at the stream frame of their chunk. Gated on stage A's
  console numbers (7.6).
- **C: music.** The baked `.xwav` tracks (#171) as an ADPCM stream on one
  source, direct (not through the room, as upstream's background track goes
  to the paint buffer); `k_snd_music` goes. Built (#183) as:
  - *The stream decodes nothing.* A track opened while the sounds are on
    voices gets a sink: each block a pull of libmpg's chunk reaches goes to
    the module once, in file order, and `Read` copies nothing out. Upstream's
    loop in `S_StreamBackgroundTrack` reads as before (the chunks, the
    dropped tail, the positions a save writes), and its read goes to the
    module in place of `S_RawEntSamples`: the blocks covering the frames it
    handed out (`Xbox_MusicStream_HandedFrame`) are queued as `alBufferData`
    copies of a ring of the copied blocks (not static buffers: 7.5). nxdk-ex streams ADPCM in whole blocks,
    so a track's last frames upstream drops (under 64) play, and a restore
    starts at its block.
  - *The clock.* The first buffer starts at the room stream's frame for the
    raw channel's end, or the paint time if later. After that the raw
    channel's end is `snd.soundtime` plus what is queued and not yet played
    (the source's offset), at the mix rate: upstream's loop then keeps
    `MAX_RAW_SAMPLES` ahead of where the APU really is. A count of what was
    read would drift by the APU's rate error (its pitch register's
    resolution, tens of ms over a long track) until the voice ran dry.
  - *Gaps.* Upstream stops mixing the raw channel when the game pauses, and
    something can empty or take the channel; each paint chunk checks for
    those and stops the voice at the painted frame. A read after any stop,
    or after the source played everything (the game stopped reading), starts
    it again from what that read hands out. The raw channel is kept silent
    (`master_vol` 0: `S_SpatializeRawChannels` gives an unattenuated raw
    channel its master volume every frame).
  - *What stays upstream's.* PCM bakes; mp3s; a track opened while the
    sounds are on the CPU; and, if the voices go while a track plays, the
    rest of it (its sink is taken off, the chunk in hand decoded, and that
    one read dropped).
- **D: lip sync.** A voice's samples never reach the CPU's mix, but
  `SND_MoveMouth8` reads the cache or the window, not the mix, so it keeps
  working as it does today (section 1). Stage B keeps the CPU walking the
  speech too, so no prebaked envelope is needed (7.4); D is closed unless a
  later design stops the CPU reading speech samples.
- **E: RAM.** The caller-provided tail (the plan's request for nxdk-ex): the
  sound pool allocates each resident sound with its padding and tail, and the
  library copies nothing. Read since (2026-10-19): `alBufferDataStatic`
  (`al_buffer.c`) allocates the copied last page and the silent tail with
  `apu_mem_alloc_phys` - from the APU pool, which `xbox_snd_al.c` sizes at
  64 KiB, and past it from `MmAllocateContiguousMemoryEx`, a contiguous
  kernel allocation per sound (4-8 KiB, page granular) - and runs `wbinvd`,
  writing back and invalidating the whole CPU cache, on every call. Stage A
  makes its buffer the first time a sound plays, so each new sound costs a
  kernel allocation and a cache flush in the frame it starts. E is therefore
  also about time: make the buffers at precache (or in one batch with one
  flush), and give the library the tail.

## 6. What measures it

`k_snd_mix` and `k_snd_extra`/`sndx_mix` on `tools/xemu-route-bmi.sh` and
the console route (`tools/xbox-ftp-route-bmi.py`), against `bmi-gproom-a`
(the room on the GP, every channel on the CPU); `sndalprof.txt` for the
voices in use, the misses and the library's update time; the listening test
for timing (a door and its sentence, a looped ambient across a save and a
load).

## 7. Stage B: sentences and streamed speech

Written 2026-10-19 after stages A (#182) and C (#183) were built, before any
of them ran on a console. It covers every channel stage A leaves on the CPU
for speech: sentences (`ch->words`, resident or streamed) and the
non-sentence speech #177 streams from a window (precached speech in
`r3d_vox_dirs`).

### 7.1 What the retail data asks for

`valve/sound/sentences.txt` (the staged disc tree), parsed as `VOX_ParseString`
and `VOX_ParseWordParams` do, with each word's WAV header read:

| | sentences (of 1065) |
|---|---|
| one word | 593 |
| a word's pitch differs from another's | 181 (almost all `hgrunt/`: the radio clicks at p110-p120) |
| some pitch other than 100 | 201 |
| timecompress on some word | 79 |
| start or end trim on some word | 140 |
| a word's volume differs from another's | 33 |
| words of two formats (rate) | 1 (`FAR_WAR16`: 11025 and 22050 Hz) |

Of 3859 word uses with a WAV, 3804 are 8-bit mono 11025 Hz and 55 8-bit mono
22050 Hz; none is 16-bit or stereo. A word's data is 7.5 KB at the median,
42 KB at the 95th percentile, 409 KB at most. So a design that handles
timecompress, trims and pitch per word covers the whole set, and one format
per source loses one sentence.

### 7.2 Why not play the words in place

Stage A plays a resident sound where it lies. Speech cannot, for four
reasons:

- **The windows move.** #177 keeps three units of a streamed word in a
  window that slides with a `memmove` as the walk enters its second unit; a
  static buffer over it would play bytes that are no longer there.
- **timecompress skips data per paint chunk.** `S_MixChannelToBuffer` mixes
  `out_count` samples, then advances `chan->sample` by
  `out_count * rate * timecompress_rate`: what plays depends on where the
  chunks fall, which no queue of whole word ranges reproduces.
- **Pitch is per word, and upstream changes it per paint call.**
  `S_MixNormalChannelsToRoombuffer` takes `VOX_ModifyPitch` once per channel
  per paint call, so a word that starts inside a call plays at the previous
  word's pitch until the next one.
- **Volume is per word** (`VOX_SetChanVol`, in the spatialization, once a
  frame).

### 7.3 The design: the walk feeds the voice

The CPU walks a speech channel exactly as today (section 1): the same
`S_MixChannelToBuffer` loop, `S_RetrieveAudioSamples` (the window, or the
cache), `S_AdjustNumSamples`, the trims, `VOX_LoadWord`, the mouth. Where it
would call `S_MixAudio`, it hands the voice the source samples that chunk
covers instead: `audio[0 .. floor(sample_frac + out_count * rate))`, the span
from the chunk's first sample to where an untimecompressed walk would be. The
next chunk starts at the walk's new position, so timecompress's skip, the
trims, a word's end and the next word's start are in the data exactly as
upstream mixes them, in order. The APU resamples it at the word's rate times
the pitch, which is what upstream's `rate` is.

- **One streaming source per channel**, set up as stage A's (direct path
  muted, the send into the room kernel's slot, panned by the channel's
  volumes), taken from the same 64 sources. The fed samples are copied into
  a ring of the module's own, and from there onto the source (7.5).
- **Timing.** The first chunk's data starts at the room stream's frame for
  its paint time (`alXboxSourcePlayAtSample`, as stage A). After that the
  data is gapless: each chunk is fed when it is painted, `_snd_mixahead`
  (0.12 s) before it is heard, so the source is never short while the
  channel is walked. Over a word the fed amount matches the output time to
  a sample per chunk, without drift: the walk's positions are absolute.
- **Pitch and volume per word.** A chunk's pitch or volumes differ from the
  last sent: they go to the voice at the stream frame of that chunk's paint
  time, through a new timed parameter call (7.5). That is upstream's own
  granularity - pitch per paint call, volume per frame - placed on the same
  clock as the data, within half a voice tick (6 ms).
- **The end.** A sentence that finishes, or a word that ends without a next,
  feeds nothing more: the source plays what it has and stops by itself, at
  the end of the data, which is where upstream's mix ends. A channel freed
  early stops at the frame being painted (stage A's timed stop).
- **Skips.** A channel the mixer skips (the menu, inaudible) is not walked
  and feeds nothing. The source plays the 0.12 s it has - as the dry stream
  plays the 0.12 s already mixed - and runs dry. When the channel is walked
  again, the source starts again at the frame of that chunk: upstream
  resumes at that paint time too.
- **What stays on the CPU.** A sentence whose next word has another rate or
  width (a source holds one format: `FAR_WAR16`), from that word on; a
  channel that gets no voice, or loses it (its queued 0.12 s is lost, the
  CPU mixes from the walk's position); more speech channels than
  `FEED_VOICES` (8) at once.

### 7.4 Lip sync: no prebake

`SND_MoveMouth8` runs before the walk, on the window or the cache, and reads
one sample in 80-111 (`i += 80 + (data & 0x1F)`). Stage B keeps the CPU
walking every speech sample it feeds, so the mouth reads the same bytes at
the same positions as today, upstream's values bit for bit, at a few dozen
reads a frame.

A prebaked envelope would be needed only by a design in which the CPU no
longer reads speech samples at all (the voice playing a word straight from a
buffer the CPU never walks), and 7.2 rules that out. It would not be exact
either: the mouth's stride depends on the sample values it lands on and on
where each paint chunk starts, so an offline table at a fixed stride gives
other mouth values. Revisit only if 7.6 shows the walk itself is the cost.

### 7.5 What nxdk-ex needs (generic)

- **Timed parameters.** `alXboxSourcefvAtSample(source, param, values,
  stream, sample)` for `AL_PITCH` and `AL_XBOX_DIRECT_GAINS`: a short queue
  per source (8), applied by the voice tick at the first update within half
  an update of the frame, as `alXboxSourcePlayAtSample`. Stage A can use it
  too: its volume and pitch changes are sent at once today, 0.12 s before
  the chunk they belong to is heard.
- **A ring sized by the format.** A streaming source allocates
  `AL_STREAM_RING_FRAMES * 4` bytes (64 KiB, contiguous, kept after use) for
  any format (`al_stream.c`, `RING_BYTES_MAX`). 8-bit mono needs 16 KiB of
  it; eight speech sources would hold 512 KiB where they need 128 KiB.
- **Measured before it is asked for: an append without buffer objects.**
  Every fed piece is today `alBufferData` (a copy into CPU memory) plus
  `alSourceQueueBuffers` and later an unqueue. The dry stream's upload
  measured about 2.7 us an update on the console (`bmi-al-g`), which is of
  the order of mixing one 8-bit mono channel for a chunk. So the module
  queues a piece only every `FEED_BATCH_MS` (48 ms) per source, from its own
  ring, not every chunk: the lead stays above 70 ms. If 7.6 shows the queue
  calls still cost what the mix did, `alXboxSourceAppend(source, data,
  bytes)` - into the stream's ring directly, no buffer name - is the request.
- **Not static buffers.** `alBufferDataStatic` allocates a contiguous tail
  page and runs `wbinvd` on every call (stage E), for data a stream only
  copies on the CPU. #183 was changed from them for that reason (fc02302c).

### 7.6 The gate, and what measures it

Stage B saves what one speech channel's mix costs minus what feeding it
costs. Both are small, and only the console knows them:

1. **Stage A's run** (`r3d_snd_voices 1` against `0`) gives `k_snd_mix` per
   channel moved to a voice (the voices line's starts and peak against the
   difference in `k_snd_mix`). That is roughly what a speech channel's mix
   costs, since its loop is the same 8-bit mono one.
2. **B1 behind its own cfg line** (`r3d_snd_voices_speech 1`) on c1a0c,
   where scientists talk over the ambience, against B off: `k_snd_mix`, and a
   new `k_snd_feed` span over the copies and the queue calls.
3. **Go only if** the feed costs under half the mix it replaces; otherwise
   stop at B1 and ask nxdk-ex for the append (7.5) first.

Listening: a scientist's line through a door, a grunt squad's radio (pitch
and timecompress), a line across the pause menu, a save restored mid-line.

### 7.7 Steps

- **B0 (nxdk-ex):** the timed parameters and the ring sized by format, with
  library tests; a 5 min host test of a fed stream at 11025 Hz.
- **B1:** the feed for one-format channels with a constant pitch and volume:
  the 593 one-word sentences and the streamed non-sentence speech, the bulk
  of what is heard. Host test: the engine's real `S_MixChannelToBuffer`
  walk (as `xbox_vox_stream_host_test.c` builds it) over every retail
  sentence, the fed bytes compared with the samples the CPU mix reads,
  chunk by chunk, timecompress and trims included.
- **B2:** the timed pitch and volume; the multi-word sentences.
- **B3, if 7.6 asks:** the append.

Divergences by design, as stage A's plus: the APU resamples each chunk's
span instead of upstream's per-output-sample read (the same samples, other
interpolation; at a timecompress skip the APU interpolates across the jump
where upstream does not); a pitch or volume change lands within 6 ms of its
chunk's frame.
