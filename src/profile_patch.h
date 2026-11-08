#ifndef PROFILE_PATCH_H_INCLUDED
#define PROFILE_PATCH_H_INCLUDED

#include <string>
#include <vector>

// Small, self-contained helpers for changing a few profile (user.txt) lines without a
// full profile save. No game headers, so tests/controller_settings_test.cpp can build
// them on their own.
namespace ProfilePatch
{
// Patch `fresh` lines ("key = value") into the "new_input" section of profile text.
// Only lines of that section whose first word is one of the keys are touched: a
// changed value is rewritten in place (indent, inline comment dropped, line end kept),
// an unchanged line stays byte-identical, a missing key is added before the section's
// "end". Every other byte is kept. Returns false (text untouched) when the profile has
// no complete input section.
bool PatchInputSection(std::string &text, const std::vector<std::string> &fresh);

// Replace the file at `path` with `data` through a uniquely named sibling temp file
// (write, flush, sync, close, rename over). On any failure the old file stays as it
// was and the temp file is removed.
bool WriteFileAtomic(const std::string &path, const std::string &data);
}

#endif
