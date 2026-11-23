// Modern menu font: a language whose characters the Latin-only UI face cannot draw keeps its own
// font (src/ui_font_cover.h). No SDL, no game data; with the repo's locale/ folder as the argument
// it also checks the shipped language files.
//   c++ -std=c++14 -Wall -Isrc tests/ui_font_cover_test.cpp -o ui_font_cover_test && ./ui_font_cover_test locale
#include "ui_font_cover.h"

#include <cstdio>
#include <fstream>
#include <sstream>

using namespace UiFontCover;

static int g_checks = 0, g_failed = 0;

static void Check(bool ok, const char *what, unsigned a = 0, unsigned b = 0)
{
    g_checks++;
    if ( !ok )
    {
        g_failed++;
        printf("FAIL: %s (%X vs %X)\n", what, a, b);
    }
}

static std::vector<uint32_t> Decode(const std::string &s)
{
    std::vector<uint32_t> out;
    size_t i = 0;
    while ( i < s.size() )
        out.push_back(NextCodepoint(s, i));
    return out;
}

// What a Latin UI face such as Rajdhani carries: Basic Latin, Latin-1, Latin Extended-A and the
// general punctuation block (dashes, quotes, ellipsis).
static bool LatinFace(uint32_t cp)
{
    return (cp >= 0x20 && cp <= 0x17F) || (cp >= 0x2000 && cp <= 0x206F);
}

static std::vector<std::string> ReadLines(const std::string &path, bool *ok)
{
    std::ifstream f(path, std::ios::binary);
    *ok = f.good();
    std::vector<std::string> out;
    std::string line;
    while ( std::getline(f, line) )
        out.push_back(line);
    return out;
}

int main(int argc, char **argv)
{
    // Decoder
    {
        std::vector<uint32_t> d = Decode("A\xC3\xA4\xD0\x94\xE3\x81\xAF\xF0\x9F\x98\x80");   // A ä Д は 😀
        Check(d.size() == 5, "five code points", (unsigned)d.size(), 5);
        if ( d.size() == 5 )
        {
            Check(d[0] == 0x41, "ASCII", d[0], 0x41);
            Check(d[1] == 0xE4, "2-byte", d[1], 0xE4);
            Check(d[2] == 0x414, "Cyrillic", d[2], 0x414);
            Check(d[3] == 0x306F, "3-byte", d[3], 0x306F);
            Check(d[4] == 0x1F600, "4-byte", d[4], 0x1F600);
        }
        std::vector<uint32_t> bad = Decode("\xE4x");          // a cp1252 'ä' (not UTF-8) then 'x'
        Check(bad.size() == 2 && bad[0] == 0xFFFD && bad[1] == 'x', "stray byte -> U+FFFD, next char kept");
        Check(Decode("\xC3")[0] == 0xFFFD, "truncated sequence");
        Check(Decode("\xC0\xAF")[0] == 0xFFFD, "overlong");
        Check(Decode("\xED\xA0\x80")[0] == 0xFFFD, "surrogate");
    }

    // Covers
    {
        uint32_t miss = 0;
        Check(Covers({"Mission laden", "Dr\xC3\xBC" "cken\nSie", ""}, LatinFace, &miss), "German is Latin");
        Check(!Covers({"OK", "\xD0\x9E\xD0\xBF\xD1\x86\xD0\xB8\xD0\xB8"}, LatinFace, &miss) && miss == 0x41E,
              "Russian: first missing = U+041E", miss, 0x41E);
        Check(Covers({"a\tb\r\n\x7F"}, [](uint32_t cp) { return cp == 'a' || cp == 'b'; }), "control chars are not glyphs");
        Check(!Covers({"\xE4"}, LatinFace, &miss) && miss == 0xFFFD, "broken UTF-8 is not covered", miss, 0xFFFD);
        Check(Covers({}, [](uint32_t) { return false; }), "no strings: covered");
    }

    // The shipped language files (optional: repo locale/ folder)
    if ( argc > 1 )
    {
        struct { const char *file; bool latin; } langs[] = {
            {"language.lng", true}, {"german.lng", true}, {"french.lng", true},
            {"russian.lng", false}, {"japanese.lng", false}};
        for ( auto &l : langs )
        {
            bool ok = false;
            std::vector<std::string> lines = ReadLines(std::string(argv[1]) + "/" + l.file, &ok);
            Check(ok && !lines.empty(), l.file);
            uint32_t miss = 0;
            const bool cov = Covers(lines, LatinFace, &miss);
            Check(cov == l.latin, l.file, miss, l.latin);
            Check(Covers(lines, [](uint32_t cp) { return cp != 0xFFFD; }), "valid UTF-8", miss, 0);
        }
    }

    printf("ui_font_cover: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
