// MusicBake - see MusicBake.h (docs/r3d/divergences.md #171).
#include "MusicBake.h"
#include "music_resample.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

extern "C" {
#include "xbox_music_adpcm.h"
#include "music_adpcm_encode.h"
#include "music_mp3_decode.h"
}

namespace fs = std::filesystem;

namespace formats {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

void put16(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}
void put32(std::vector<uint8_t>& v, uint32_t x) {
    put16(v, x & 0xFFFFu);
    put16(v, x >> 16);
}
void putTag(std::vector<uint8_t>& v, const char* t) { v.insert(v.end(), t, t + 4); }

int threadCount(int requested) {
    if (requested > 0) return requested;
    unsigned hw = std::thread::hardware_concurrency();
    return hw ? static_cast<int>(std::min(hw, 16u)) : 4;
}

// The engine's stream over a stdio file, for the read-back check.
struct StdioCtx { FILE* f; };
int stdioRead(void* ctx, void* dst, int bytes) {
    return static_cast<int>(std::fread(dst, 1, static_cast<size_t>(bytes), static_cast<StdioCtx*>(ctx)->f));
}
int stdioSeek(void* ctx, uint32_t offset) {
    return std::fseek(static_cast<StdioCtx*>(ctx)->f, static_cast<long>(offset), SEEK_SET) == 0 ? 0 : -1;
}

// Plays the track from a fresh open to its end through upstream's
// Stream_ReadMPG over the real libmpg and through the baked stream, with
// requests shaped like S_StreamBackgroundTrack's (`s_stream.c:220-240` in
// the reference: 16 KiB fills, then about a frame's worth), and compares
// every return value and every position; the bytes too when `sameBytes`
// (a PCM bake). Returns an empty string, or where they parted.
std::string replayAgainstLibmpg(const std::string& mp3, xbox_music_stream_t* baked, bool sameBytes) {
    static const int sizes[] = {16384, 16384, 16384, 16384, 2940, 2944, 2936, 2940, 16384, 4, 2940, 8192, 8196, 12, 2940, 3000};
    int rate = 0, channels = 0;
    music_mp3_stream_t* up = Music_Mp3StreamOpen(mp3.c_str(), &rate, &channels);
    if (!up) return "libmpg would not reopen the track";
    std::vector<uint8_t> a(16384), b(16384);
    std::string where;
    int zeros = 0;
    for (uint64_t k = 0; zeros < 2; ++k) {
        const int need = sizes[k % (sizeof(sizes) / sizeof(sizes[0]))];
        const int ra = Music_Mp3StreamRead(up, need, a.data());
        const int rb = Xbox_MusicStream_Read(baked, need, b.data());
        const int pa = Music_Mp3StreamGetPos(up), pb = Xbox_MusicStream_GetPos(baked);
        if (ra != rb || pa != pb || (sameBytes && ra > 0 && std::memcmp(a.data(), b.data(), static_cast<size_t>(ra)))) {
            std::ostringstream s;
            s << "request " << k << " (" << need << " bytes): libmpg returned " << ra << " at " << pa << ", the bake " << rb
              << " at " << pb;
            where = s.str();
            break;
        }
        if (ra == 0) ++zeros;
    }
    Music_Mp3StreamClose(up);
    return where;
}

// A bake resampled to the mix rate (#173) has no libmpg to compare with:
// libmpg's chunks are at the source's rate. Its reference is upstream's
// Stream_ReadMPG loop (`snd_mp3.c:361-398` in the reference), written out
// again here, over a decoder that hands out `pcm` - the bake passes the
// file's own decode - in chunk_bytes chunks and reports failure on the
// short last one, as read_mpeg_stream does; its position is the frames
// handed out, converted to the source's frames here, not by the engine's
// function (positions in a save are the mp3's). Same requests as
// replayAgainstLibmpg; the bytes too when `sameBytes`.
struct ModelStream {
    const int16_t* pcm = nullptr;
    uint32_t frames = 0, frameBytes = 0, chunkBytes = 0;
    uint32_t next = 0;           // frames handed out of the decoder
    std::vector<uint8_t> temp;   // stream->temp
    int pos = 0, buffsize = 0;   // stream->pos, stream->buffsize
    const xbox_xwav_info_t* info = nullptr;
};

bool modelPull(ModelStream& m, int* outsize) {
    const uint32_t chunkFrames = m.chunkBytes / m.frameBytes;
    const uint32_t got = std::min(chunkFrames, m.frames - std::min(m.next, m.frames));
    std::memcpy(m.temp.data(), reinterpret_cast<const uint8_t*>(m.pcm) + static_cast<size_t>(m.next) * m.frameBytes,
                static_cast<size_t>(got) * m.frameBytes);
    m.next += got;
    *outsize = static_cast<int>(got * m.frameBytes);
    return got == chunkFrames;
}

int modelRead(ModelStream& m, int needBytes, uint8_t* buffer) {
    int bytesWritten = 0;
    while (true) {
        if (!m.buffsize) {
            if (!modelPull(m, &m.pos)) break;
        }
        const int outsize = bytesWritten + m.pos > needBytes ? needBytes - bytesWritten : m.pos;
        std::memcpy(buffer + bytesWritten, m.temp.data() + m.buffsize, static_cast<size_t>(outsize));
        bytesWritten += outsize;
        m.pos -= outsize;
        m.buffsize += outsize;
        if (bytesWritten >= needBytes) return bytesWritten;
        m.buffsize = 0;
    }
    return 0;
}

std::string replayAgainstModel(const int16_t* pcm, uint32_t frames, uint32_t channels, const xbox_xwav_info_t& info,
                               xbox_music_stream_t* baked, bool sameBytes) {
    static const int sizes[] = {16384, 16384, 16384, 16384, 2940, 2944, 2936, 2940, 16384, 4, 2940, 8192, 8196, 12, 2940, 3000};
    ModelStream m;
    m.pcm = pcm;
    m.frames = frames;
    m.frameBytes = 2u * channels;
    m.chunkBytes = info.chunk_bytes;
    m.temp.resize(info.chunk_bytes);
    m.info = &info;
    std::vector<uint8_t> a(16384), b(16384);
    int zeros = 0;
    for (uint64_t k = 0; zeros < 2; ++k) {
        const int need = sizes[k % (sizeof(sizes) / sizeof(sizes[0]))];
        const int ra = modelRead(m, need, a.data());
        const int rb = Xbox_MusicStream_Read(baked, need, b.data());
        // the position in the source's frames, computed here rather than
        // through the engine's Xbox_MusicXwav_ToSourceFrames: the frames
        // whose time is at or before the decoder's, floor(next * src / rate)
        const int pa = static_cast<int>((static_cast<uint64_t>(m.next) * info.source_rate) / info.rate);
        const int pb = Xbox_MusicStream_GetPos(baked);
        if (ra != rb || pa != pb || (sameBytes && ra > 0 && std::memcmp(a.data(), b.data(), static_cast<size_t>(ra)))) {
            std::ostringstream s;
            s << "request " << k << " (" << need << " bytes): the model returned " << ra << " at " << pa << ", the bake " << rb
              << " at " << pb;
            return s.str();
        }
        if (ra == 0) ++zeros;
    }
    return std::string();
}

// The positions of a resampled bake (#173): a seek to any source position
// p reports p back (Xbox_MusicXwav_ToFileFrames rounds up, ToSourceFrames
// down) and the next read starts at the file frame p maps to, the first
// whose time is not before p's. `decoded` is the file's decode.
std::string checkResampledSeeks(xbox_music_stream_t* baked, const int16_t* decoded, uint32_t sourceFrames, uint32_t channels) {
    const xbox_xwav_info_t& info = baked->info;
    const uint32_t probes[] = {0u, 1u, 146u, 147u, 148u, 1151u, 1152u, 44099u, sourceFrames / 3, sourceFrames / 2 + 7,
                               sourceFrames > 2048 ? sourceFrames - 2048 : 0u, sourceFrames - 1, sourceFrames, sourceFrames + 5};
    std::vector<uint8_t> b(64);
    for (uint32_t p : probes) {
        if (Xbox_MusicStream_SetPos(baked, static_cast<int>(p)) != 1 || Xbox_MusicStream_GetPos(baked) != static_cast<int>(p)) {
            return "a seek to source frame " + std::to_string(p) + " reports " + std::to_string(Xbox_MusicStream_GetPos(baked));
        }
        const uint32_t f = Xbox_MusicXwav_ToFileFrames(&info, p);
        const uint64_t t0 = static_cast<uint64_t>(f) * info.source_rate, t1 = static_cast<uint64_t>(p) * info.rate;
        if (t0 < t1 || (f > 0 && static_cast<uint64_t>(f - 1) * info.source_rate >= t1)) {
            return "source frame " + std::to_string(p) + " maps to file frame " + std::to_string(f) + ", not the first at or after it";
        }
        const int need = static_cast<int>(2u * channels);
        const int got = Xbox_MusicStream_Read(baked, need, b.data());
        // the decoder hands out a whole chunk from file frame f, or reports
        // the end on a short one, which the read then drops (upstream's tail)
        const uint32_t chunkFrames = info.chunk_bytes / (2u * channels);
        if (static_cast<uint64_t>(f) + chunkFrames <= info.frames) {
            if (got != need || std::memcmp(b.data(), decoded + static_cast<size_t>(f) * channels, static_cast<size_t>(need))) {
                return "after a seek to source frame " + std::to_string(p) + " the first frame is not file frame " + std::to_string(f);
            }
            // one chunk pulled from f: the position is its end, in source
            // frames, computed here (floor(frames * src / rate))
            const int expect = static_cast<int>((static_cast<uint64_t>(f + chunkFrames) * info.source_rate) / info.rate);
            if (Xbox_MusicStream_GetPos(baked) != expect) {
                return "after a seek to source frame " + std::to_string(p) + " and a read, the position is " +
                       std::to_string(Xbox_MusicStream_GetPos(baked)) + ", not " + std::to_string(expect);
            }
        } else if (got != 0) {
            return "a seek to source frame " + std::to_string(p) + " inside the short last chunk read " + std::to_string(got) + " bytes";
        }
    }
    return std::string();
}

} // namespace

std::string musicXwavName(const std::string& name, uint32_t rate) {
    return rate ? name + "." + std::to_string(rate / 1000) + "k.xwav" : name + ".xwav";
}

void measureMusicQuality(const int16_t* source, const int16_t* decoded, uint32_t frames, uint32_t channels,
                         uint32_t rate, double gateDbfs, double& snrDb, double& worstSegmentDb, double& worstAtSec) {
    const uint32_t window = std::max<uint32_t>(1, rate / 20); // 50 ms
    const double fullScale = 32768.0 * 32768.0;
    double sigAll = 0.0, noiseAll = 0.0;
    worstSegmentDb = 200.0;
    worstAtSec = 0.0;

    for (uint32_t start = 0; start < frames; start += window) {
        const uint32_t n = std::min(window, frames - start);
        double sig = 0.0, noise = 0.0;
        for (uint32_t i = start * channels; i < (start + n) * channels; ++i) {
            const double s = source[i], d = static_cast<double>(source[i]) - decoded[i];
            sig += s * s;
            noise += d * d;
        }
        sigAll += sig;
        noiseAll += noise;
        const double rmsDbfs = sig > 0.0 ? 10.0 * std::log10(sig / (static_cast<double>(n) * channels) / fullScale) : -200.0;
        if (n < window / 2 || rmsDbfs < gateDbfs) continue; // a short tail or a quiet window
        const double seg = noise > 0.0 ? 10.0 * std::log10(sig / noise) : 200.0;
        if (seg < worstSegmentDb) {
            worstSegmentDb = seg;
            worstAtSec = static_cast<double>(start) / rate;
        }
    }
    snrDb = noiseAll > 0.0 ? 10.0 * std::log10(sigAll / noiseAll) : 200.0;
}

std::vector<uint8_t> encodeMusicAdpcm(const int16_t* pcm, uint32_t frames, uint32_t channels, int threads) {
    if (channels < 1 || channels > 2) {
        throw std::invalid_argument("encodeMusicAdpcm: 1 or 2 channels");
    }
    const uint32_t blockBytes = XBOX_ADPCM_CHANNEL_BYTES * channels;
    const uint32_t blocks = (frames + XBOX_ADPCM_BLOCK_FRAMES - 1) / XBOX_ADPCM_BLOCK_FRAMES;
    // the block's 64 frames and the next block's first; silence past the end
    std::vector<int16_t> padded(static_cast<size_t>(blocks * XBOX_ADPCM_BLOCK_FRAMES + 1) * channels, 0);
    std::memcpy(padded.data(), pcm, static_cast<size_t>(frames) * channels * sizeof(int16_t));
    std::vector<uint8_t> out(static_cast<size_t>(blocks) * blockBytes);

    Xbox_MusicAdpcm_InitTables(); // before any thread: the tables are built lazily
    const int n = std::max(1, std::min<int>(threadCount(threads), static_cast<int>(blocks)));
    std::vector<std::thread> pool;
    for (int t = 0; t < n; ++t) {
        pool.emplace_back([&, t]() {
            const uint32_t first = static_cast<uint32_t>((static_cast<uint64_t>(blocks) * t) / n);
            const uint32_t last = static_cast<uint32_t>((static_cast<uint64_t>(blocks) * (t + 1)) / n);
            int hint[2] = {0, 0};
            for (uint32_t b = first; b < last; ++b) {
                Music_AdpcmEncodeBlock(padded.data() + static_cast<size_t>(b) * XBOX_ADPCM_BLOCK_FRAMES * channels,
                                       static_cast<int>(channels), hint, out.data() + static_cast<size_t>(b) * blockBytes);
            }
        });
    }
    for (auto& th : pool) th.join();
    return out;
}

std::vector<uint8_t> buildXwav(const XwavFields& f, const std::vector<uint8_t>& data) {
    const bool adpcm = f.tag == XBOX_XWAV_TAG_ADPCM;
    const uint32_t fmtSize = adpcm ? 20 : 16;
    std::vector<uint8_t> v;
    v.reserve(data.size() + 128);

    putTag(v, "RIFF");
    put32(v, 0); // patched below
    putTag(v, "WAVE");

    putTag(v, "fmt ");
    put32(v, fmtSize);
    put16(v, f.tag);
    put16(v, f.channels);
    put32(v, f.rate);
    if (adpcm) {
        put32(v, static_cast<uint32_t>((static_cast<uint64_t>(f.rate) * XBOX_ADPCM_CHANNEL_BYTES * f.channels) / XBOX_ADPCM_BLOCK_FRAMES));
        put16(v, XBOX_ADPCM_CHANNEL_BYTES * f.channels); // nBlockAlign
        put16(v, 4);                                      // wBitsPerSample
        put16(v, 2);                                      // cbSize
        put16(v, XBOX_ADPCM_BLOCK_FRAMES);                // wSamplesPerBlock
    } else {
        put32(v, f.rate * 2u * f.channels);
        put16(v, 2u * f.channels);
        put16(v, 16);
    }

    putTag(v, "r3dm");
    put32(v, XBOX_XWAV_R3DM_BYTES);
    const size_t r3dm = v.size();
    put32(v, XBOX_XWAV_R3DM_VERSION);
    put32(v, f.frames);
    put32(v, f.sourceRate);
    put32(v, f.sourceFrames);
    put32(v, f.crcSource);
    put32(v, f.crcDecoded);
    put32(v, f.chunkBytes);
    put32(v, f.sourceBytes);
    char ext[8] = {};
    std::strncpy(ext, f.sourceExt.c_str(), sizeof(ext) - 1);
    v.insert(v.end(), ext, ext + 8);
    put16(v, f.encoderId);
    put16(v, f.resamplerId);
    put32(v, f.tellLastChunk);
    if (v.size() - r3dm != XBOX_XWAV_R3DM_BYTES) {
        throw std::logic_error("r3dm layout");
    }

    // A JUNK chunk (RIFF's own filler, which every reader skips) puts the
    // samples at offset 512, so the data chunk's header ends exactly where
    // the engine's 512-byte header read does. (The engine's read-ahead
    // reads whole 16 KiB windows of the file from offset 0 since #172, so
    // the offset no longer decides its alignment.)
    const size_t junkTotal = XBOX_XWAV_HEADER_READ - v.size() - 8;
    if (v.size() + 8 + 8 > XBOX_XWAV_HEADER_READ) {
        throw std::logic_error("xwav header longer than the engine's header read");
    }
    putTag(v, "JUNK");
    put32(v, static_cast<uint32_t>(junkTotal - 8));
    v.insert(v.end(), junkTotal - 8, 0);

    putTag(v, "data");
    put32(v, static_cast<uint32_t>(data.size()));
    v.insert(v.end(), data.begin(), data.end());
    if (data.size() & 1) v.push_back(0); // RIFF pad (never for these layouts)

    const uint32_t riff = static_cast<uint32_t>(v.size() - 8);
    std::memcpy(v.data() + 4, &riff, 4); // little-endian host
    return v;
}

MusicTrackResult bakeMusicTrack(const std::string& mp3Path, const std::string& name, const std::string& outPath,
                                bool pcm, const MusicQualityFloor& floor, int threads, uint32_t rate) {
    MusicTrackResult r;
    r.name = name;
    r.codec = pcm ? "pcm" : "adpcm";
    std::error_code ec;
    fs::remove(outPath, ec); // a track that fails below must not leave an older bake of itself behind

    char err[512] = {};
    music_mp3_t m;
    if (!Music_Mp3Decode(mp3Path.c_str(), &m, err, sizeof(err))) {
        r.error = err;
        return r;
    }
    struct Guard { music_mp3_t& m; ~Guard() { Music_Mp3Free(&m); } } guard{m};

    r.rate = r.sourceRate = static_cast<uint32_t>(m.rate);
    r.channels = static_cast<uint32_t>(m.channels);
    r.frames = r.sourceFrames = m.frames;
    r.seekProbes = m.seek_probes;
    r.seekExact = m.seek_exact;
    r.seekWorstOffset = m.seek_worst_offset;
    r.sourceBytes = fs::file_size(mp3Path, ec);
    if (ec || r.sourceBytes > 0x7FFFFFFFu) {
        r.error = "cannot read the source's size";
        return r;
    }

    const uint32_t fb = 2u * r.channels;
    // The stream the engine replays (xbox_music_adpcm.h, THE STREAM) is
    // libmpg handing out whole chunks until a short last one; anything
    // else and the replay would not be the mp3 path.
    if (m.short_ok_reads || m.tell_mismatches || m.chunk_bytes % fb || m.frames == 0 || m.open_tell != 0 ||
        static_cast<uint32_t>(m.done_tell) != m.frames || m.last_ok_tell < 0 || static_cast<uint32_t>(m.last_ok_tell) > m.frames) {
        std::ostringstream s;
        s << "libmpg's output does not fit the replayed stream: " << m.short_ok_reads << " short MP3_OK reads, "
          << m.tell_mismatches << " position mismatches (first after read " << m.tell_first_chunk << " of "
          << m.ok_chunks << ": " << m.tell_first_value << " for " << static_cast<uint64_t>(m.tell_first_chunk) * m.chunk_bytes / fb
          << "), " << m.frames << " frames, tail " << m.tail_bytes << " bytes, position " << m.open_tell << " at open, "
          << m.last_ok_tell << " after the last chunk, " << m.done_tell << " at the end";
        r.error = s.str();
        return r;
    }
    if (m.rate < 8000 || m.rate > 48000) {
        r.error = "unsupported rate " + std::to_string(m.rate);
        return r;
    }

    r.crcSource = Xbox_MusicCrc32(0, m.pcm, static_cast<size_t>(m.frames) * fb);

    // The reference the file is encoded from, measured and replayed
    // against: libmpg's decode, resampled offline to the mix rate when the
    // bake asks for one (#173, music_resample.h).
    const bool resample = rate != 0 && rate != r.sourceRate;
    std::vector<int16_t> resampled;
    const int16_t* ref = m.pcm;
    if (resample) {
        if (!musicResampleSupported(r.sourceRate, rate)) {
            r.error = "cannot resample " + std::to_string(r.sourceRate) + " Hz to " + std::to_string(rate) + " Hz";
            return r;
        }
        MusicResampleStats st;
        resampled = resampleMusic(m.pcm, m.frames, r.channels, r.sourceRate, rate, threads, &st);
        ref = resampled.data();
        r.rate = rate;
        r.frames = static_cast<uint32_t>(resampled.size() / r.channels);
        r.clipped = st.clipped;
    }
    const size_t pcmBytes = static_cast<size_t>(r.frames) * fb;

    std::vector<uint8_t> data;
    std::vector<int16_t> decoded(static_cast<size_t>(r.frames + XBOX_ADPCM_BLOCK_FRAMES) * r.channels);
    if (pcm) {
        data.assign(reinterpret_cast<const uint8_t*>(ref), reinterpret_cast<const uint8_t*>(ref) + pcmBytes);
        std::memcpy(decoded.data(), ref, pcmBytes);
    } else {
        data = encodeMusicAdpcm(ref, r.frames, r.channels, threads);
        const uint32_t blockBytes = XBOX_ADPCM_CHANNEL_BYTES * r.channels;
        for (size_t b = 0; b * blockBytes < data.size(); ++b) {
            if (!Xbox_MusicAdpcm_DecodeBlock(data.data() + b * blockBytes, static_cast<int>(r.channels),
                                             decoded.data() + b * XBOX_ADPCM_BLOCK_FRAMES * r.channels)) {
                r.error = "the encoder wrote a block the decoder refuses";
                return r;
            }
        }
    }
    r.crcDecoded = Xbox_MusicCrc32(0, decoded.data(), pcmBytes);
    measureMusicQuality(ref, decoded.data(), r.frames, r.channels, r.rate, floor.gateDbfs, r.snrDb, r.worstSegmentDb,
                        r.worstSegmentAtSec);

    XwavFields f;
    f.tag = pcm ? XBOX_XWAV_TAG_PCM : XBOX_XWAV_TAG_ADPCM;
    f.channels = r.channels;
    f.rate = r.rate;
    f.frames = r.frames;
    f.sourceRate = r.sourceRate;
    f.sourceFrames = m.frames;
    f.crcSource = r.crcSource;
    f.crcDecoded = r.crcDecoded;
    f.chunkBytes = m.chunk_bytes;
    f.sourceBytes = static_cast<uint32_t>(r.sourceBytes);
    f.sourceExt = lower(fs::path(mp3Path).extension().string().substr(1));
    f.encoderId = pcm ? 0 : MUSIC_ADPCM_ENCODER_ID;
    f.resamplerId = resample ? kMusicResamplerId : 0; // 0: none, the source rate
    // libmpg's position quirk after its last whole chunk belongs to the
    // source's chunk grid; a resampled file has no such chunk (#173)
    f.tellLastChunk = !resample && m.last_ok_tell_differs ? static_cast<uint32_t>(m.last_ok_tell) : 0;
    const std::vector<uint8_t> file = buildXwav(f, data);

    if (!pcm && (r.snrDb < floor.overallDb || r.worstSegmentDb < floor.segmentDb)) {
        std::ostringstream s;
        s << std::fixed << std::setprecision(1) << "under the quality floor: SNR " << r.snrDb << " dB (floor "
          << floor.overallDb << "), worst 50 ms segment " << r.worstSegmentDb << " dB at " << r.worstSegmentAtSec
          << " s (floor " << floor.segmentDb << ") - listen to it, and ship it as PCM with --music-pcm " << name
          << " if it is audible";
        r.error = s.str();
        fs::remove(outPath, ec);
        return r;
    }

    fs::create_directories(fs::path(outPath).parent_path(), ec);
    {
        std::ofstream o(outPath, std::ios::binary);
        o.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
        o.close();
        if (!o) {
            r.error = "cannot write " + outPath;
            return r;
        }
    }

    // Read it back the way the console will: the engine's parser on the
    // first XBOX_XWAV_HEADER_READ bytes, then the engine's stream.
    FILE* in = std::fopen(outPath.c_str(), "rb");
    if (!in) {
        r.error = "cannot reopen " + outPath;
        return r;
    }
    std::vector<uint8_t> head(XBOX_XWAV_HEADER_READ);
    const size_t headLen = std::fread(head.data(), 1, head.size(), in);
    xbox_xwav_info_t info;
    const xbox_xwav_err_t perr = Xbox_MusicXwav_ParseHeader(head.data(), headLen, static_cast<uint32_t>(file.size()), &info);
    bool good = perr == XBOX_XWAV_OK && info.frames == r.frames && info.crc_decoded == r.crcDecoded &&
                info.chunk_bytes == m.chunk_bytes && info.rate == r.rate && info.source_frames == m.frames;
    if (good) {
        StdioCtx ctx{in};
        xbox_music_io_t io{stdioRead, stdioSeek, &ctx};
        std::vector<uint8_t> chunk(info.chunk_bytes), ra(XBOX_MUSIC_READAHEAD_BYTES);
        xbox_music_stream_t* s = new xbox_music_stream_t;
        std::vector<int16_t> back(static_cast<size_t>(r.frames) * r.channels);
        good = Xbox_MusicStream_Init(s, &info, &io, chunk.data(), static_cast<uint32_t>(chunk.size()), ra.data()) &&
               Xbox_MusicStream_DecodeFrames(s, 0, r.frames, back.data()) == r.frames && s->bad_blocks == 0 &&
               Xbox_MusicCrc32(0, back.data(), pcmBytes) == r.crcDecoded;
        // a resampled file's positions: every seek reports its target back
        if (good && resample) {
            const std::string bad = checkResampledSeeks(s, decoded.data(), m.frames, r.channels);
            if (!bad.empty()) {
                delete s;
                std::fclose(in);
                fs::remove(outPath, ec);
                r.error = "the resampled stream's positions: " + bad;
                return r;
            }
        }
        // and the replay of upstream's reader, from a fresh stream
        good = good && Xbox_MusicStream_Init(s, &info, &io, chunk.data(), static_cast<uint32_t>(chunk.size()), ra.data());
        if (good) {
            // the model serves the file's own decode, so every byte of an
            // ADPCM bake is compared too, not only the returns and positions
            const std::string parted = resample ? replayAgainstModel(decoded.data(), r.frames, r.channels, info, s, true)
                                                : replayAgainstLibmpg(mp3Path, s, pcm);
            if (!parted.empty()) {
                delete s;
                std::fclose(in);
                fs::remove(outPath, ec);
                r.error = "the baked stream does not replay upstream's Stream_ReadMPG: " + parted;
                return r;
            }
        }
        delete s;
    }
    std::fclose(in);
    r.fileBytes = file.size();
    if (!good) {
        r.error = perr != XBOX_XWAV_OK
                      ? std::string("the written file does not parse with the engine's parser: ") + Xbox_MusicXwav_ErrorString(perr)
                      : std::string("the written file does not decode back through the engine's stream to the frames and CRC written");
        fs::remove(outPath, ec);
        return r;
    }

    r.ok = true;
    return r;
}

bool bakeMusic(const MusicBakeOptions& opt, std::vector<MusicTrackResult>& results) {
    const fs::path media = fs::path(opt.gameDir) / "media";
    std::error_code ec;
    if (!fs::is_directory(media, ec)) {
        std::cerr << "--music: no media folder under " << opt.gameDir << "\n";
        return false;
    }

    // Nothing from an earlier run may survive into this one's output: the
    // packer trusts the manifest, and the manifest is rewritten below even
    // when the run fails.
    const fs::path manifest = fs::path(opt.outDir) / "music_manifest.txt";
    fs::remove(manifest, ec);
    if (fs::is_directory(fs::path(opt.outDir) / "media", ec)) {
        for (const auto& e : fs::directory_iterator(fs::path(opt.outDir) / "media", ec)) {
            if (lower(e.path().extension().string()) == ".xwav") fs::remove(e.path(), ec);
        }
    }

    std::vector<fs::path> tracks;
    std::set<std::string> seen;
    bool ok = true;
    ec.clear();
    for (const auto& e : fs::directory_iterator(media, ec)) {
        if (!e.is_regular_file()) continue;
        const std::string ext = lower(e.path().extension().string());
        const std::string base = e.path().stem().string();
        if (ext == ".mp3") {
            if (!seen.insert(lower(base)).second) {
                std::cerr << "--music: two tracks named " << base << " (case-insensitive)\n";
                ok = false;
            }
            tracks.push_back(e.path());
        } else if (ext == ".wav" || ext == ".ogg" || ext == ".opus") {
            // The engine tries mp3 first for an extensionless name; a
            // .wav/.ogg/.opus track keeps playing through upstream's
            // decoder, which no bake here replaces.
            std::cerr << "--music: warning: " << e.path().filename().string() << " is not baked (mp3 only)\n";
        }
    }
    if (ec) {
        std::cerr << "--music: listing " << media.string() << " failed: " << ec.message() << "\n";
        return false;
    }
    std::sort(tracks.begin(), tracks.end());
    if (tracks.empty()) {
        std::cerr << "--music: no .mp3 under " << media.string() << "\n";
        return false;
    }
    for (const auto& name : opt.pcmTracks) {
        if (!seen.count(lower(name))) {
            std::cerr << "--music-pcm: no track named " << name << "\n";
            ok = false;
        }
    }

    std::set<std::string> pcmLower;
    for (const auto& n : opt.pcmTracks) pcmLower.insert(lower(n));

    for (const auto& t : tracks) {
        const std::string name = t.stem().string();
        const bool pcm = opt.allPcm || pcmLower.count(lower(name)) != 0;
        const std::string out = (fs::path(opt.outDir) / "media" / musicXwavName(name, opt.rate)).string();
        MusicTrackResult r = bakeMusicTrack(t.string(), name, out, pcm, opt.floor, opt.threads, opt.rate);
        std::cout << std::fixed << std::setprecision(1) << "  " << std::left << std::setw(16) << name << " " << r.codec
                  << "  " << r.frames << " frames  SNR " << r.snrDb << " dB  worst 50 ms " << r.worstSegmentDb
                  << " dB @ " << r.worstSegmentAtSec << " s  " << r.fileBytes << " bytes  libmpg seek " << r.seekExact
                  << "/" << r.seekProbes << (opt.rate ? "  clipped " + std::to_string(r.clipped) : std::string())
                  << (r.ok ? "" : "  FAILED: " + r.error) << "\n";
        ok = ok && r.ok;
        results.push_back(r);
    }

    std::ofstream mf(manifest, std::ios::binary); // LF lines: tools/xbox-xiso.sh reads it in bash
    mf << "# Resonance3D baked music (docs/r3d/divergences.md #171), tools/asset-studio --music\n";
    mf << "# floor: overall >= " << opt.floor.overallDb << " dB, worst 50 ms >= " << opt.floor.segmentDb
       << " dB over windows louder than " << opt.floor.gateDbfs << " dBFS; encoder " << MUSIC_ADPCM_ENCODER_ID << "\n";
    if (opt.rate) {
        // #173: resampled to the mix rate; tools/xbox-xiso.sh reads the
        // file names from the `# file` line (<name><suffix>)
        mf << "# resampled to " << opt.rate << " Hz, resampler " << kMusicResamplerId
           << " (Kaiser-windowed sinc, 192 taps, beta 12, cutoff 21025/44100 of the input rate);"
              " snr and worst 50 ms against the resampled decode; crc_source is libmpg's own output\n";
        mf << "# file " << musicXwavName("", opt.rate) << "\n";
    }
    mf << "# name codec rate channels frames source_frames crc_source crc_decoded snr_db worst_50ms_db worst_at_s"
          " xwav_bytes source_bytes libmpg_seek_exact clipped status\n";
    for (const auto& r : results) {
        mf << r.name << " " << r.codec << " " << r.rate << " " << r.channels << " " << r.frames << " " << r.sourceFrames
           << " " << std::hex << std::setw(8) << std::setfill('0') << r.crcSource << " " << std::setw(8) << r.crcDecoded
           << std::dec << std::setfill(' ') << " " << std::fixed << std::setprecision(2) << r.snrDb << " "
           << r.worstSegmentDb << " " << r.worstSegmentAtSec << " " << r.fileBytes << " " << r.sourceBytes << " "
           << r.seekExact << "/" << r.seekProbes << " " << r.clipped << " " << (r.ok ? "ok" : "FAILED") << "\n";
    }
    // tools/xbox-xiso.sh --music-bake packs a bake only with this line, and
    // only if its count is the number of .xwav files: a run that failed
    // anywhere - a track, or the run itself - never writes it.
    if (ok) {
        mf << "# result ok " << results.size() << "\n";
    }
    mf.close();
    if (!mf) {
        std::cerr << "--music: cannot write " << manifest.string() << "\n";
        return false;
    }
    return ok;
}

} // namespace formats
