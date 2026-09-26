// Unit test for stripWorldspawnWadKey (BspParser.cpp): the Xbox nv2a
// bake drops the worldspawn "wad" key so the engine takes the same
// no-pixels path ref/nv2a's own texture manager already does - see
// divergences.md #112. Cases mirror the spec's own list: key present,
// key absent, mixed case, two keys, a later entity's own "wad" key left
// untouched, other keys byte-identical, and malformed input.
#include "formats/BspParser.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace formats;

namespace {

int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); ++failures; } } while (0)

std::vector<uint8_t> bytes(const char* s) {
    return std::vector<uint8_t>(s, s + std::strlen(s));
}

std::string str(const std::vector<uint8_t>& v) {
    return std::string(reinterpret_cast<const char*>(v.data()), v.size());
}

} // namespace

int main() {
    // --- key present: stripped, along with its trailing newline ---
    {
        auto in = bytes("{\n\"classname\" \"worldspawn\"\n\"wad\" \"c:\\a.wad\"\n\"message\" \"hi\"\n}\n");
        auto out = stripWorldspawnWadKey(in);
        CHECK(str(out).find("\"wad\"") == std::string::npos, "key present: \"wad\" pair removed");
        CHECK(str(out) == "{\n\"classname\" \"worldspawn\"\n\"message\" \"hi\"\n}\n", "key present: surrounding bytes untouched, no blank line left");
    }

    // --- key absent: unchanged ---
    {
        auto in = bytes("{\n\"classname\" \"worldspawn\"\n\"message\" \"hi\"\n}\n");
        auto out = stripWorldspawnWadKey(in);
        CHECK(out == in, "key absent: input returned unchanged");
    }

    // --- mixed-case key: still matched (Q_stricmp semantics, mod_bmodel.c:2462) ---
    {
        auto in = bytes("{\n\"classname\" \"worldspawn\"\n\"WaD\" \"c:\\a.wad\"\n}\n");
        auto out = stripWorldspawnWadKey(in);
        CHECK(str(out) == "{\n\"classname\" \"worldspawn\"\n}\n", "mixed-case key: removed case-insensitively");
    }

    // --- two "wad" keys in the same (first) entity: both removed ---
    {
        auto in = bytes("{\n\"wad\" \"a.wad\"\n\"classname\" \"worldspawn\"\n\"wad\" \"b.wad\"\n}\n");
        auto out = stripWorldspawnWadKey(in);
        CHECK(str(out) == "{\n\"classname\" \"worldspawn\"\n}\n", "two wad keys: both removed");
    }

    // --- a "wad" key in a LATER entity is left untouched ---
    {
        auto in = bytes("{\n\"classname\" \"worldspawn\"\n\"wad\" \"a.wad\"\n}\n{\n\"classname\" \"info_bsp\"\n\"wad\" \"b.wad\"\n}\n");
        auto out = stripWorldspawnWadKey(in);
        CHECK(str(out) == "{\n\"classname\" \"worldspawn\"\n}\n{\n\"classname\" \"info_bsp\"\n\"wad\" \"b.wad\"\n}\n",
              "later entity's own wad key untouched");
    }

    // --- other keys byte-identical ---
    {
        auto in = bytes("{\n\"classname\" \"worldspawn\"\n\"wad\" \"a.wad\"\n\"_compiler\" \"chompers\"\n}\n");
        auto out = stripWorldspawnWadKey(in);
        CHECK(str(out) == "{\n\"classname\" \"worldspawn\"\n\"_compiler\" \"chompers\"\n}\n",
              "other keys kept byte-identical, in order");
    }

    // --- malformed input: returned unchanged rather than guessed at ---
    {
        auto noBrace = bytes("\"classname\" \"worldspawn\"");
        CHECK(stripWorldspawnWadKey(noBrace) == noBrace, "malformed: no leading brace returned unchanged");

        auto unterminated = bytes("{\n\"classname\" \"worldspawn\"\n\"wad\" \"a.wad\"\n");
        CHECK(stripWorldspawnWadKey(unterminated) == unterminated, "malformed: unterminated block returned unchanged");

        auto badKey = bytes("{\nclassname \"worldspawn\"\n}\n");
        CHECK(stripWorldspawnWadKey(badKey) == badKey, "malformed: unquoted key returned unchanged");

        std::vector<uint8_t> empty;
        CHECK(stripWorldspawnWadKey(empty) == empty, "malformed: empty input returned unchanged");
    }

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("success\n");
    return 0;
}
