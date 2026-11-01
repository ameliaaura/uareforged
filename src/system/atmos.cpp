// Reforge ATMOSPHERE - see atmos.h.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "atmos.h"
#include "../gfxsettings.h"
#include "../yw.h"
#include "inivals.h"

namespace Atmos
{
namespace
{
State gState;

bool ParseFloats(const char *s, float *out, int n)
{
    for (int i = 0; i < n; ++i)
    {
        char *end = NULL;
        out[i] = strtof(s, &end);
        if (end == s)
            return false;
        s = end;
        if (i + 1 < n)
        {
            if (*s != ',')
                return false;
            ++s;
        }
    }
    return true;
}

// One table line -> st. Returns false when the line is not for `sky`.
bool ParseLine(const char *line, const std::string &sky, State *st)
{
    char buf[4096];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    float hazen[24], hazeo[24];
    bool haveN = false, haveO = false;
    bool match = false;
    for (char *tok = strtok(buf, " \t\r\n"); tok; tok = strtok(NULL, " \t\r\n"))
    {
        char *eq = strchr(tok, '=');
        if (!eq)
            continue;
        *eq = 0;
        const char *k = tok, *v = eq + 1;
        if (!strcmp(k, "sky"))
        {
            if (strcasecmp(v, sky.c_str()) != 0)
                return false;
            match = true;
        }
        else if (!match)
            return false;
        else if (!strcmp(k, "fam"))
            st->family = !strcmp(v, "overcast") ? FAM_OVERCAST : !strcmp(v, "storm") ? FAM_STORM
                       : !strcmp(v, "night") ? FAM_NIGHT : FAM_CLEAR;
        else if (!strcmp(k, "night"))
            st->night = atoi(v) != 0;
        else if (!strcmp(k, "sun"))
        {
            float a[2];
            if (ParseFloats(v, a, 2))
            {
                st->sunAz = a[0];
                st->sunEl = a[1];
            }
        }
        else if (!strcmp(k, "suntint"))
            ParseFloats(v, st->sunTint, 3);
        else if (!strcmp(k, "ambtint"))
            ParseFloats(v, st->ambTint, 3);
        else if (!strcmp(k, "grade"))
            ParseFloats(v, st->grade, 3);
        else if (!strcmp(k, "hazen"))
            haveN = ParseFloats(v, hazen, 24);
        else if (!strcmp(k, "hazeo"))
            haveO = ParseFloats(v, hazeo, 24);
        else if (!strcmp(k, "near"))
        {
            float a[3];
            if (ParseFloats(v, a, 3))
            {
                st->nearStart = a[0];
                st->nearRange = a[1] > 1.0f ? a[1] : 1.0f;
                st->nearMax = a[2];
            }
        }
        else if (!strcmp(k, "weather"))
        {
            char w[32] = {0};
            float kk = 0.0f;
            const char *c = strchr(v, ',');
            size_t n = c ? (size_t)(c - v) : strlen(v);
            if (n >= sizeof(w))
                n = sizeof(w) - 1;
            memcpy(w, v, n);
            if (c)
                kk = atof(c + 1);
            st->weather = !strcmp(w, "dust") ? W_DUST : !strcmp(w, "rain") ? W_RAIN : !strcmp(w, "snow") ? W_SNOW
                        : W_NONE;
            st->weatherK = kk;
        }
    }
    if (!match)
        return false;
    const float *h = (st->newSky && haveN) ? hazen : (haveO ? hazeo : (haveN ? hazen : NULL));
    if (h)
        memcpy(st->haze, h, sizeof(st->haze));
    return true;
}

std::string TablePath()
{
    if (const char *v = getenv("UA_ATMOS_FILE"))
        if (v[0])
            return v;
    std::string dir = "hd-sky";
    if (const char *v = getenv("UA_SKY_DIR"))
        if (v[0])
            dir = v;
    return dir + "/atmos.txt";
}

std::string SkyBase(const std::string &skyName)
{
    std::string b = skyName;
    size_t s = b.find_last_of("/\\:");
    if (s != std::string::npos)
        b = b.substr(s + 1);
    size_t d = b.find('.');
    if (d != std::string::npos)
        b = b.substr(0, d);
    for (char &c : b)
        c = tolower(c);
    return b;
}
} // namespace

const State &Get()
{
    return gState;
}

void OnLevelLoad(NC_STACK_ypaworld *yw, const std::string &skyName, bool newSky)
{
    const int level = gfxLevel("ATMOS", 0);
    if (level <= 0)
    {
        gState = State(); // off: nothing below runs, no file is opened
        return;
    }
    State st;
    st.level = level > 3 ? 3 : level;
    st.sky = SkyBase(skyName);
    st.newSky = newSky;
    const std::string path = TablePath();
    FILE *f = fopen(path.c_str(), "r");
    bool found = false;
    if (f)
    {
        char line[4096];
        while (!found && fgets(line, sizeof(line), f))
        {
            if (line[0] == '#')
                continue;
            State t = st;
            if (ParseLine(line, st.sky, &t))
            {
                st = t;
                found = true;
            }
        }
        fclose(f);
    }
    if (!found)
    {
        printf("atmos: level %d but no row for sky '%s' in %s - atmosphere off for this level\n", st.level,
               st.sky.c_str(), path.c_str());
        gState = State();
        return;
    }
    // Look-dev overrides (inert unless set).
    if (const char *v = getenv("UA_ATMOS_SUN")) // az,el
    {
        float a[2];
        if (ParseFloats(v, a, 2))
        {
            st.sunAz = a[0];
            st.sunEl = a[1];
        }
    }
    const double az = st.sunAz * M_PI / 180.0, el = st.sunEl * M_PI / 180.0;
    st.sunDir[0] = (float)(sin(az) * cos(el));
    st.sunDir[1] = (float)(-sin(el)); // y down: up = -y
    st.sunDir[2] = (float)(cos(az) * cos(el));
    if (st.level >= 2 && st.night)
    {
        st.windows = 1.0f;
        st.hostLight = 1.8f;
        if (const char *v = getenv("UA_ATMOS_WINDOWS"))
            st.windows = atof(v);
    }
    if (st.level < 3)
        st.weather = W_NONE;
    if (const char *v = getenv("UA_ATMOS_WEATHER")) // look-dev: none|dust|rain|snow[,k]
    {
        st.weather = !strncmp(v, "dust", 4) ? W_DUST : !strncmp(v, "rain", 4) ? W_RAIN
                   : !strncmp(v, "snow", 4) ? W_SNOW : W_NONE;
        if (const char *c = strchr(v, ','))
            st.weatherK = atof(c + 1);
        else if (st.weatherK <= 0.0f)
            st.weatherK = 0.6f;
    }
    if (st.weather == W_NONE)
        st.weatherK = 0.0f;
    st.active = true;
    gState = st;
    static const char *famName[] = {"clear", "overcast", "storm", "night"};
    static const char *wName[] = {"none", "dust", "rain", "snow"};
    printf("atmos: level %d sky '%s' (%s%s, %s sky) sun az %.0f el %.0f tint sun %.2f,%.2f,%.2f amb %.2f,%.2f,%.2f "
           "grade %.2f,%.2f,%.2f haze0 %.2f,%.2f,%.2f near %.0f+%.0f x%.2f windows %.1f hosts %.1f weather %s %.2f | "
           "cue %d-%d sky fade %d+%d\n",
           st.level, st.sky.c_str(), famName[st.family], st.night ? ", night" : "", st.newSky ? "new" : "1998",
           st.sunAz, st.sunEl, st.sunTint[0], st.sunTint[1], st.sunTint[2], st.ambTint[0], st.ambTint[1],
           st.ambTint[2], st.grade[0], st.grade[1], st.grade[2], st.haze[0][0], st.haze[0][1], st.haze[0][2],
           st.nearStart, st.nearRange, st.nearMax, st.windows, st.hostLight, wName[st.weather], st.weatherK,
           yw ? yw->_normalVizLimit - yw->_normalFadeLength : 0, yw ? yw->_normalVizLimit : 0,
           (int)System::IniConf::GfxSkyDistance.Get<int>(), (int)System::IniConf::GfxSkyLength.Get<int>());
}

void HazeAt(float wx, float wz, float out[3])
{
    const State &s = gState;
    float a = atan2f(wx, wz) * (float)(180.0 / M_PI); // -180..180
    float t = (a + 180.0f) / 45.0f;                    // sector centres at 0..7
    float fl = floorf(t);
    float fr = t - fl;
    int i0 = ((int)fl) & 7, i1 = (i0 + 1) & 7;
    for (int c = 0; c < 3; ++c)
        out[c] = s.haze[i0][c] * (1.0f - fr) + s.haze[i1][c] * fr;
}
}
