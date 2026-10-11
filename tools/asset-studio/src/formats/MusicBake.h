#pragma once
// MusicBake - retail music tracks baked to .xwav for the console
// (docs/r3d/divergences.md #171).
//
// Every <game-dir>/media/*.mp3 is decoded with the engine's own libmpg
// (music_mp3_decode.c) and written as media/<Track>.xwav: Xbox ADPCM by
// default, 16-bit PCM for a track named in pcmTracks. The engine opens
// the .xwav in place of the mp3 (engine/platform/xbox/xbox_music_stream.c)
// and falls back to the mp3 when it is missing or refused.
//
// The bake fails (returns false) for any track that does not hold:
// - libmpg's own output must fit the stream model the engine replays
//   (every MP3_OK read a whole OUTBUF_SIZE chunk, get_stream_pos equal to
//   the frames handed out);
// - the written file must parse with the engine's parser, decode through
//   the engine's stream to the frame count libmpg produced, and match the
//   CRC the encoder recorded;
// - an ADPCM track must clear the quality floor (overall SNR and worst
//   50 ms segment against libmpg's decode). A track under it fails the
//   bake rather than shipping hiss: name it in pcmTracks to ship it as PCM.

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace formats {

// The default floor is an outlier detector, not a perceptual proof (the
// console listening test is that). 4-bit IMA on the 27 retail tracks
// measures 29.3-50.9 dB overall and 19.2-41.4 dB in its worst 50 ms
// window (encoder 1, docs/r3d/divergences.md #171), so a track under
// 25 / 15 dB is noisier than anything Valve shipped by 4 dB or more:
// something about it - level, content, a broken source - is not what
// the retail set is, and it fails the bake until someone has listened to
// it and either ships it as PCM (--music-pcm) or lowers the floor for it.
struct MusicQualityFloor {
    // Signal-to-noise over the whole track, dB.
    double overallDb = 25.0;
    // The worst 50 ms window whose source is louder than gateDbfs, dB.
    double segmentDb = 15.0;
    // Windows quieter than this (RMS, dBFS) are not scored on their own:
    // there the noise sits under the console's 16-bit output floor
    // whatever its ratio to the signal. They still count in overallDb.
    double gateDbfs = -60.0;
};

struct MusicBakeOptions {
    std::string gameDir;       // e.g. .../Half-Life/valve
    std::string outDir;        // an overlay tree: <outDir>/media/<Track>.xwav
    bool allPcm = false;       // --music-codec pcm
    std::set<std::string> pcmTracks; // --music-pcm, track names without extension, case-insensitive
    MusicQualityFloor floor;
    int threads = 0;           // 0: the machine's
};

struct MusicTrackResult {
    std::string name;          // e.g. Half-Life01
    std::string codec;         // adpcm or pcm
    uint32_t rate = 0, channels = 0, frames = 0, sourceFrames = 0;
    uint32_t crcSource = 0, crcDecoded = 0;
    uint64_t fileBytes = 0, sourceBytes = 0;
    double snrDb = 0.0, worstSegmentDb = 0.0;
    double worstSegmentAtSec = 0.0;
    uint32_t seekProbes = 0, seekExact = 0;
    int32_t seekWorstOffset = 0;
    bool ok = false;
    std::string error;
};

// Quality of `decoded` against `source`, both `frames` interleaved frames.
void measureMusicQuality(const int16_t* source, const int16_t* decoded, uint32_t frames, uint32_t channels,
                         uint32_t rate, double gateDbfs, double& snrDb, double& worstSegmentDb, double& worstAtSec);

// Encodes interleaved PCM to Xbox ADPCM blocks (36 * channels bytes per 64
// frames, the last block padded with silence), on `threads` threads; the
// result does not depend on the thread count.
std::vector<uint8_t> encodeMusicAdpcm(const int16_t* pcm, uint32_t frames, uint32_t channels, int threads);

// The container (xbox_music_adpcm.h): RIFF/WAVE, fmt, r3dm, data.
struct XwavFields {
    uint32_t tag = 0, channels = 0, rate = 0, frames = 0;
    uint32_t sourceRate = 0, sourceFrames = 0, crcSource = 0, crcDecoded = 0;
    uint32_t chunkBytes = 0, sourceBytes = 0;
    std::string sourceExt;
    uint32_t encoderId = 0, resamplerId = 0;
    uint32_t tellLastChunk = 0; // libmpg's position after the last whole chunk, when not the count (0: the count)
};
std::vector<uint8_t> buildXwav(const XwavFields& f, const std::vector<uint8_t>& data);

// Bakes one track. `name` is the file's base name. Writes the .xwav only
// when every check above passes.
MusicTrackResult bakeMusicTrack(const std::string& mp3Path, const std::string& name, const std::string& outPath,
                                bool pcm, const MusicQualityFloor& floor, int threads);

// The whole media/ folder; writes <outDir>/music_manifest.txt. Returns
// false when any track failed.
bool bakeMusic(const MusicBakeOptions& opt, std::vector<MusicTrackResult>& results);

} // namespace formats
