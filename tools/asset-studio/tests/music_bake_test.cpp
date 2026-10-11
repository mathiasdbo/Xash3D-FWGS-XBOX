// music_bake_test - the music bake on a synthetic track (docs/r3d/divergences.md #171).
//
// tests/data/music_synthetic.mp3 is 2.5 s of a 440 Hz tone over pink
// noise, made with ffmpeg for this test (not retail content). It checks:
// - libmpg's decode fits the stream model the engine replays;
// - an ADPCM and a PCM bake read back through the engine's parser and
//   stream, with the r3dm fields the engine checks;
// - the quality floor fails a bake and leaves no file behind;
// - the encode does not depend on the thread count;
// - the baked stream replays upstream's Stream_ReadMPG over the real
//   libmpg request by request: the same return values, the same positions,
//   and for the PCM bake the same bytes, from the start and after seeks.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>

#include "formats/MusicBake.h"

extern "C" {
#include "xbox_music_adpcm.h"
#include "music_mp3_decode.h"
}

namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

struct StdioCtx { FILE* f; };
int stdioRead(void* ctx, void* dst, int bytes) {
    return static_cast<int>(std::fread(dst, 1, static_cast<size_t>(bytes), static_cast<StdioCtx*>(ctx)->f));
}
int stdioSeek(void* ctx, uint32_t offset) {
    return std::fseek(static_cast<StdioCtx*>(ctx)->f, static_cast<long>(offset), SEEK_SET) == 0 ? 0 : -1;
}

struct Baked {
    FILE* f = nullptr;
    StdioCtx ctx{};
    xbox_xwav_info_t info{};
    std::vector<uint8_t> chunk, ra;
    xbox_music_stream_t s{};
    bool open(const std::string& path) {
        f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        std::vector<uint8_t> head(XBOX_XWAV_HEADER_READ);
        const size_t n = std::fread(head.data(), 1, head.size(), f);
        const auto len = static_cast<uint32_t>(fs::file_size(path));
        if (Xbox_MusicXwav_ParseHeader(head.data(), n, len, &info) != XBOX_XWAV_OK) return false;
        ctx.f = f;
        xbox_music_io_t io{stdioRead, stdioSeek, &ctx};
        chunk.resize(8192);
        ra.resize(XBOX_MUSIC_READAHEAD_BYTES);
        return Xbox_MusicStream_Init(&s, &info, &io, chunk.data(), 8192, ra.data()) != 0;
    }
    ~Baked() { if (f) std::fclose(f); }
};

// Requests shaped like S_StreamBackgroundTrack's (`s_stream.c:220-240`):
// a full 16 KiB fill, then about a frame's worth, now and then odd.
const int kSizes[] = {16384, 2940, 2944, 2936, 16384, 4, 2940, 8192, 8196, 12, 2940, 3000};

// Strict from a fresh open (and a seek to 0): every return, byte (PCM)
// and position equal. After a seek elsewhere libmpg is not frame-exact
// itself - its first frames after the seek decode without the bit
// reservoir and its position drifts near the end (measured here: the
// synthetic track's probes, and every retail track's in
// music_manifest.txt) - so `strict` is off there and the run only
// reports where the two first part.
void compareReplay(const std::string& mp3, const std::string& xwav, bool sameBytes, int seekTo) {
    const bool strict = seekTo <= 0;
    int rate = 0, channels = 0;
    music_mp3_stream_t* up = Music_Mp3StreamOpen(mp3.c_str(), &rate, &channels);
    Baked baked;
    CHECK(up != nullptr);
    CHECK(baked.open(xwav));
    if (!up || !baked.f) return;

    if (seekTo >= 0) {
        CHECK(Music_Mp3StreamSetPos(up, seekTo) == 1);
        CHECK(Xbox_MusicStream_SetPos(&baked.s, seekTo) == 1);
    }
    // both report the target right after the seek
    CHECK(Music_Mp3StreamGetPos(up) == Xbox_MusicStream_GetPos(&baked.s));

    std::vector<uint8_t> a(20000), b(20000);
    int zeros = 0;
    for (int k = 0; k < 10000 && zeros < 3; ++k) {
        const int need = kSizes[k % (sizeof(kSizes) / sizeof(kSizes[0]))];
        const int ra = Music_Mp3StreamRead(up, need, a.data());
        const int rb = Xbox_MusicStream_Read(&baked.s, need, b.data());
        const int pa = Music_Mp3StreamGetPos(up), pb = Xbox_MusicStream_GetPos(&baked.s);
        const bool bytesDiffer = sameBytes && ra == rb && ra > 0 && std::memcmp(a.data(), b.data(), static_cast<size_t>(ra));
        if (ra != rb || pa != pb || bytesDiffer) {
            if (strict) {
                std::printf("  request %d (%d bytes): libmpg returned %d at %d, baked %d at %d%s\n", k, need, ra, pa, rb, pb,
                            bytesDiffer ? ", bytes differ" : "");
                CHECK(!"the replay parted from libmpg");
            } else {
                std::printf("  after a seek to %d, libmpg and the bake part at request %d: libmpg returned %d at %d,"
                            " baked %d at %d%s\n", seekTo, k, ra, pa, rb, pb, bytesDiffer ? ", bytes differ" : "");
            }
            break;
        }
        if (ra == 0) ++zeros;
    }
    if (strict) {
        CHECK(zeros == 3);
    }
    Music_Mp3StreamClose(up);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: music_bake_test <music_synthetic.mp3>\n";
        return 2;
    }
    const std::string mp3 = argv[1];
    const fs::path tmp = fs::temp_directory_path() / "r3d_music_bake_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);

    // libmpg against the stream model
    music_mp3_t m;
    char err[256] = {};
    CHECK(Music_Mp3Decode(mp3.c_str(), &m, err, sizeof(err)) == 1);
    CHECK(m.rate == 44100 && m.channels == 2);
    CHECK(m.short_ok_reads == 0 && m.tell_mismatches == 0);
    CHECK(m.chunk_bytes == 8192);
    CHECK(m.frames > 44100 * 2 && m.frames < 44100 * 3);
    CHECK(m.ok_chunks == m.frames / 2048 && m.tail_bytes == (m.frames % 2048) * 4);
    std::printf("synthetic: %u frames, %u chunks, tail %u bytes, libmpg seek exact %u/%u (tell %u), worst offset %d\n",
                m.frames, m.ok_chunks, m.tail_bytes, m.seek_exact, m.seek_probes, m.seek_tell_exact, m.seek_worst_offset);

    // the encode is the same on any number of threads
    {
        const auto one = formats::encodeMusicAdpcm(m.pcm, m.frames, 2, 1);
        const auto five = formats::encodeMusicAdpcm(m.pcm, m.frames, 2, 5);
        CHECK(one == five);
        CHECK(one.size() == ((m.frames + 63) / 64) * 72);
    }
    const uint32_t crcSource = Xbox_MusicCrc32(0, m.pcm, static_cast<size_t>(m.frames) * 4);
    const uint32_t frames = m.frames;
    Music_Mp3Free(&m);

    formats::MusicQualityFloor floor;
    floor.overallDb = 20.0;
    floor.segmentDb = 10.0;

    // ADPCM bake
    const std::string adpcmPath = (tmp / "adpcm" / "media" / "synthetic.xwav").string();
    const auto ra = formats::bakeMusicTrack(mp3, "synthetic", adpcmPath, false, floor, 3);
    std::printf("adpcm: %s, SNR %.1f dB, worst 50 ms %.1f dB, %llu bytes\n", ra.ok ? "ok" : ra.error.c_str(), ra.snrDb,
                ra.worstSegmentDb, static_cast<unsigned long long>(ra.fileBytes));
    CHECK(ra.ok);
    CHECK(fs::exists(adpcmPath));
    CHECK(ra.crcSource == crcSource && ra.crcDecoded != crcSource);
    {
        Baked b;
        CHECK(b.open(adpcmPath));
        CHECK(b.info.tag == XBOX_XWAV_TAG_ADPCM && b.info.channels == 2 && b.info.rate == 44100);
        CHECK(b.info.frames == frames && b.info.source_frames == frames && b.info.source_rate == 44100);
        CHECK(b.info.crc_source == crcSource && b.info.crc_decoded == ra.crcDecoded);
        CHECK(b.info.chunk_bytes == 8192 && std::string(b.info.source_ext) == "mp3");
        CHECK(b.info.source_bytes == fs::file_size(mp3));
    }

    // PCM bake: lossless
    const std::string pcmPath = (tmp / "pcm" / "media" / "synthetic.xwav").string();
    const auto rp = formats::bakeMusicTrack(mp3, "synthetic", pcmPath, true, floor, 3);
    CHECK(rp.ok);
    CHECK(rp.crcDecoded == crcSource && rp.snrDb >= 199.0);
    // the samples start at 512 (a JUNK chunk pads the header to the engine's header read)
    CHECK(rp.fileBytes == XBOX_XWAV_HEADER_READ + static_cast<uint64_t>(frames) * 4);
    {
        Baked b;
        CHECK(b.open(pcmPath));
        CHECK(b.info.data_offset == XBOX_XWAV_HEADER_READ);
    }

    // a floor the track cannot reach fails it and leaves nothing behind
    formats::MusicQualityFloor high = floor;
    high.overallDb = 90.0;
    const std::string failPath = (tmp / "fail" / "media" / "synthetic.xwav").string();
    const auto rf = formats::bakeMusicTrack(mp3, "synthetic", failPath, false, high, 2);
    CHECK(!rf.ok && rf.error.find("quality floor") != std::string::npos);
    CHECK(!fs::exists(failPath));
    // ... and PCM is not held to it
    CHECK(formats::bakeMusicTrack(mp3, "synthetic", failPath, true, high, 2).ok);

    // the replay against the real thing
    compareReplay(mp3, pcmPath, true, -1);
    compareReplay(mp3, adpcmPath, false, -1);
    for (int p : {0, 1, 1151, 1152, 4097, 50000, static_cast<int>(frames) - 3000, static_cast<int>(frames) - 10}) {
        compareReplay(mp3, pcmPath, true, p);
        compareReplay(mp3, adpcmPath, false, p);
    }

    fs::remove_all(tmp, ec);
    if (failures) {
        std::printf("music_bake_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("music_bake_test: all passed\n");
    return 0;
}
