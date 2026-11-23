#ifndef UI_FONT_COVER_H
#define UI_FONT_COVER_H

// Does a font draw every character of a language? Used before the modern menu style swaps the
// language's own font for its Latin-only face: a language that face cannot draw (Russian,
// Japanese) keeps its 1998 font instead of showing empty boxes. Header only, no SDL, so the
// unit test (tests/ui_font_cover_test.cpp) runs without the game.

#include <cstdint>
#include <string>
#include <vector>

namespace UiFontCover {

// Next code point of a UTF-8 string, from byte `i` (advanced). Malformed or truncated
// sequences come back as U+FFFD, one byte at a time, like SDL_ttf's own decoder: a font
// without U+FFFD draws a box for them too, so they count as not covered.
inline uint32_t NextCodepoint(const std::string &s, size_t &i)
{
    const unsigned char c = (unsigned char)s[i++];
    if ( c < 0x80 )
        return c;

    int n;
    uint32_t cp;
    if ( (c & 0xE0) == 0xC0 )      { n = 1; cp = c & 0x1F; }
    else if ( (c & 0xF0) == 0xE0 ) { n = 2; cp = c & 0x0F; }
    else if ( (c & 0xF8) == 0xF0 ) { n = 3; cp = c & 0x07; }
    else
        return 0xFFFD;

    if ( i + n > s.size() )
        return 0xFFFD;
    for ( int k = 0; k < n; k++ )
        if ( ((unsigned char)s[i + k] & 0xC0) != 0x80 )
            return 0xFFFD;

    for ( int k = 0; k < n; k++ )
        cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
    i += n;

    static const uint32_t kMin[4] = {0, 0x80, 0x800, 0x10000};
    if ( cp < kMin[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) )
        return 0xFFFD;   // overlong, out of range or a surrogate
    return cp;
}

// True when `has(cp)` holds for every drawable character of every string (control
// characters, the line breaks of multi-line texts included, are never drawn as glyphs).
// On false, *missing (if given) = the first character the font lacks.
template <class Has>
bool Covers(const std::vector<std::string> &strings, Has has, uint32_t *missing = nullptr)
{
    for ( const std::string &s : strings )
    {
        size_t i = 0;
        while ( i < s.size() )
        {
            const uint32_t cp = NextCodepoint(s, i);
            if ( cp < 0x20 || cp == 0x7F )
                continue;
            if ( !has(cp) )
            {
                if ( missing )
                    *missing = cp;
                return false;
            }
        }
    }
    return true;
}

}

#endif
