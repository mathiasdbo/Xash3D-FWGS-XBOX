// MusicBake - see MusicBake.h (docs/r3d/divergences.md #171).
#include "MusicBake.h"

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

} // namespace

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
    // samples at offset 512: the engine's read-ahead then refills on
    // sector boundaries (XBOX_MUSIC_READAHEAD_BYTES is 18 sectors), and the
    // data chunk's header ends exactly where the engine's 512-byte header
    // read does.
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
                                bool pcm, const MusicQualityFloor& floor, int threads) {
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

    r.rate = static_cast<uint32_t>(m.rate);
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

    const size_t pcmBytes = static_cast<size_t>(m.frames) * fb;
    r.crcSource = Xbox_MusicCrc32(0, m.pcm, pcmBytes);

    std::vector<uint8_t> data;
    std::vector<int16_t> decoded(static_cast<size_t>(m.frames + XBOX_ADPCM_BLOCK_FRAMES) * r.channels);
    if (pcm) {
        data.assign(reinterpret_cast<const uint8_t*>(m.pcm), reinterpret_cast<const uint8_t*>(m.pcm) + pcmBytes);
        std::memcpy(decoded.data(), m.pcm, pcmBytes);
    } else {
        data = encodeMusicAdpcm(m.pcm, m.frames, r.channels, threads);
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
    measureMusicQuality(m.pcm, decoded.data(), m.frames, r.channels, r.rate, floor.gateDbfs, r.snrDb, r.worstSegmentDb,
                        r.worstSegmentAtSec);

    XwavFields f;
    f.tag = pcm ? XBOX_XWAV_TAG_PCM : XBOX_XWAV_TAG_ADPCM;
    f.channels = r.channels;
    f.rate = r.rate;
    f.frames = m.frames;
    f.sourceRate = r.rate;
    f.sourceFrames = m.frames;
    f.crcSource = r.crcSource;
    f.crcDecoded = r.crcDecoded;
    f.chunkBytes = m.chunk_bytes;
    f.sourceBytes = static_cast<uint32_t>(r.sourceBytes);
    f.sourceExt = lower(fs::path(mp3Path).extension().string().substr(1));
    f.encoderId = pcm ? 0 : MUSIC_ADPCM_ENCODER_ID;
    f.resamplerId = 0; // none: the source rate
    f.tellLastChunk = m.last_ok_tell_differs ? static_cast<uint32_t>(m.last_ok_tell) : 0;
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
    bool good = perr == XBOX_XWAV_OK && info.frames == m.frames && info.crc_decoded == r.crcDecoded &&
                info.chunk_bytes == m.chunk_bytes;
    if (good) {
        StdioCtx ctx{in};
        xbox_music_io_t io{stdioRead, stdioSeek, &ctx};
        std::vector<uint8_t> chunk(info.chunk_bytes), ra(XBOX_MUSIC_READAHEAD_BYTES);
        xbox_music_stream_t* s = new xbox_music_stream_t;
        std::vector<int16_t> back(static_cast<size_t>(m.frames) * r.channels);
        good = Xbox_MusicStream_Init(s, &info, &io, chunk.data(), static_cast<uint32_t>(chunk.size()), ra.data()) &&
               Xbox_MusicStream_DecodeFrames(s, 0, m.frames, back.data()) == m.frames && s->bad_blocks == 0 &&
               Xbox_MusicCrc32(0, back.data(), pcmBytes) == r.crcDecoded;
        // and the replay of upstream's reader, from a fresh stream
        good = good && Xbox_MusicStream_Init(s, &info, &io, chunk.data(), static_cast<uint32_t>(chunk.size()), ra.data());
        if (good) {
            const std::string parted = replayAgainstLibmpg(mp3Path, s, pcm);
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
        const std::string out = (fs::path(opt.outDir) / "media" / (name + ".xwav")).string();
        MusicTrackResult r = bakeMusicTrack(t.string(), name, out, pcm, opt.floor, opt.threads);
        std::cout << std::fixed << std::setprecision(1) << "  " << std::left << std::setw(16) << name << " " << r.codec
                  << "  " << r.frames << " frames  SNR " << r.snrDb << " dB  worst 50 ms " << r.worstSegmentDb
                  << " dB @ " << r.worstSegmentAtSec << " s  " << r.fileBytes << " bytes  libmpg seek " << r.seekExact
                  << "/" << r.seekProbes << (r.ok ? "" : "  FAILED: " + r.error) << "\n";
        ok = ok && r.ok;
        results.push_back(r);
    }

    std::ofstream mf(manifest, std::ios::binary); // LF lines: tools/xbox-xiso.sh reads it in bash
    mf << "# Resonance3D baked music (docs/r3d/divergences.md #171), tools/asset-studio --music\n";
    mf << "# floor: overall >= " << opt.floor.overallDb << " dB, worst 50 ms >= " << opt.floor.segmentDb
       << " dB over windows louder than " << opt.floor.gateDbfs << " dBFS; encoder " << MUSIC_ADPCM_ENCODER_ID << "\n";
    mf << "# name codec rate channels frames source_frames crc_source crc_decoded snr_db worst_50ms_db worst_at_s"
          " xwav_bytes source_bytes libmpg_seek_exact status\n";
    for (const auto& r : results) {
        mf << r.name << " " << r.codec << " " << r.rate << " " << r.channels << " " << r.frames << " " << r.sourceFrames
           << " " << std::hex << std::setw(8) << std::setfill('0') << r.crcSource << " " << std::setw(8) << r.crcDecoded
           << std::dec << std::setfill(' ') << " " << std::fixed << std::setprecision(2) << r.snrDb << " "
           << r.worstSegmentDb << " " << r.worstSegmentAtSec << " " << r.fileBytes << " " << r.sourceBytes << " "
           << r.seekExact << "/" << r.seekProbes << " " << (r.ok ? "ok" : "FAILED") << "\n";
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
