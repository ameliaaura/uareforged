#ifndef TEX_CLEANUP_H_INCLUDED
#define TEX_CLEANUP_H_INCLUDED

#include <stdint.h>
#include <string>

// The 1998 artists left joke graffiti and a signature in three of the city
// texture sheets: "GURU MEDITATION ERROR 8100000C", "INSANE IN THE MEMBRANE",
// initials and more in the empty corners of city1, and "(c) 95 bernd
// beyreuther" along the bottom edge of shrttwnd and fhrttwnd. Normally they
// sit in parts of the sheet no wall uses, but some walls graze those parts
// and the lettering shows on buildings.
//
// The sharp (HD) texture pack was retouched for this in August 2026
// (docs/logs/hd-tag-removal-2026-08-17.md). This file applies the very same
// rules to the original 256 x 256 sheets when the game loads them, so the
// Quest app (which has no sharp textures) and desktop players with sharp
// textures off see the same clean walls.
//
// Pure pixel code: no SDL, no game headers. It works on a plain RGBA buffer
// (four bytes per pixel in the order R, G, B, A, rows top to bottom), so
// tests/tex_cleanup_test.cpp can check it with made-up images.
namespace TexCleanup
{
// What a sheet is, judged by its resource name only.
enum Sheet
{
    SHEET_OTHER = 0,
    SHEET_CITY1,      // city1: graffiti in the transparent gaps
    SHEET_SIGNATURE,  // shrttwnd / fhrttwnd: the signature on the bottom strip
};

// `name` is the texture's resource name as the engine knows it (any case,
// e.g. "city1.ilbm"). fcity1 and every other sheet are SHEET_OTHER.
Sheet Classify(const std::string &name);

// 64-bit FNV-1a over the pixels in R, G, B, A order, row by row, without any
// row padding. The same number whatever the engine's own pixel layout is, and
// the same as hashing the RGBA bytes of a PNG written with UA_TEX_DUMP=1.
uint64_t HashRGBA(const uint8_t *rgba, int w, int h, int stride);

// The two city1 bitmaps that carry the graffiti, by HashRGBA of the original
// 256 x 256 sheet (Berlin buildings in the warm daylight palette and in the
// night palette). The other city1 variants have no graffiti and, unlike these
// two, real artwork in the columns the rules clear, so they are never touched.
extern const uint64_t kCity1WarmHash;
extern const uint64_t kCity1NightHash;

enum Result
{
    UNCHANGED = 0,    // nothing to do (wrong size, unknown variant, already clean)
    CITY1_WARM,       // city1 daylight graffiti removed
    CITY1_NIGHT,      // city1 night graffiti removed
    SIGNATURE_COVERED,
    SIGNATURE_KEPT,   // the player turned Wall Signatures on: left as painted
    SIGNATURE_SKIPPED // the strip under the signature is not the plain dark
                      // pattern this was made for (a modded sheet): left alone
};

// city1: only the two known bitmaps (by hash). Two rules, both of which only
// ever turn opaque pixels fully transparent (0,0,0,0), so the graffiti
// becomes the same empty padding that already surrounds it:
//   1. columns x 250-255, full height, hold nothing but lettering;
//   2. small opaque islands (at most 700 pixels, pixels joined by an edge or
//      a corner) found separately inside three zones:
//      A x 0-39,    y 116-255  initials, GURU MEDITATION box, INSANE...
//      B x 240-249, y 0-83     the arrow glyphs and "akku"
//      C x 0-71,    y 248-255  the night set's extra line along the bottom
// The zone edges are measured, not rounded: x 40 and x 239 are the first
// columns of real buildings, and a zone that reaches into a building cuts a
// small slice off it that would look like an island. Don't widen a zone
// without re-measuring the columns first (see the August log, section 4).
//
// shrttwnd / fhrttwnd: the signature box x 0-92, y 252-255 is painted over
// with the plain dark pattern right next to it on the same strip, from
// x = 94 + (x % 50) on the same row. Even offsets keep the 2-pixel checker
// pattern in step, and copying from the same sheet keeps each level's own
// colours. `keepSignature` (the Wall Signatures setting) leaves it as is.
//
// Returns the number of pixels changed through `changed` (may be NULL).
// Running it twice changes nothing the second time.
Result Apply(Sheet sheet, uint8_t *rgba, int w, int h, int stride,
             bool keepSignature, int *changed);

// The same with the two city1 hashes passed in, so the tests can use made-up
// sheets. The game always calls Apply above.
Result ApplyWithHashes(uint64_t warmHash, uint64_t nightHash, Sheet sheet,
                       uint8_t *rgba, int w, int h, int stride,
                       bool keepSignature, int *changed);

// One line for the game log, e.g. "city1 graffiti removed (daylight set)".
const char *Describe(Result r);
}

#endif
