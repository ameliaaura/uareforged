// FMV stub for builds without FFmpeg (UA_NO_FMV: the Quest build).
// Every movie is skipped exactly as if the player had pressed a key on its
// first frame; the settings plumbing keeps working so profiles round-trip.
// The real player is system/movie.cpp.

#include <stdio.h>
#include "movie.h"
#include "inivals.h"

namespace System
{

struct TMovie::Context {};

TMovie TMovie::Instance;

namespace
{
int _hdEnabledState = -1;
int _fillEnabledState = -1;
}

TMovie::TMovie() : _ctx(NULL) {}
TMovie::~TMovie() {}

void TMovie::Init()
{
    printf("movie: FMV disabled in this build (UA_NO_FMV) - movies are skipped\n");
}

void TMovie::PlayMovie(const std::string &fname, int volume)
{
    (void)volume;
    printf("movie: skip '%s' (UA_NO_FMV)\n", fname.c_str());
}

void TMovie::SetHDEnabled(bool on) { _hdEnabledState = on ? 1 : 0; }
bool TMovie::HDEnabled() { return _hdEnabledState > 0; }

std::string TMovie::ResolveMoviePath(const std::string &fname, bool *isHD, bool *is43)
{
    if (isHD) *isHD = false;
    if (is43) *is43 = false;
    return fname;
}

void TMovie::SetFillEnabled(bool on) { _fillEnabledState = on ? 1 : 0; }
bool TMovie::FillEnabled() { return _fillEnabledState > 0; }
bool TMovie::FillDefaultFromIni() { return false; }
MovieScaleMode TMovie::ScaleMode() { return MOVIE_SCALE_MAX; }

bool TMovie::OpenFile(const std::string &, bool) { return false; }
void TMovie::Close() {}
void TMovie::ReadFrames() {}
void TMovie::ProcessFrame(uint32_t) {}
void TMovie::ProcessAudio() {}
int TMovie::EventsWatcher(void *, SDL_Event *) { return 1; }

}
