#ifndef UA_WINPORT_H_INCLUDED
#define UA_WINPORT_H_INCLUDED

// Windows (MinGW-w64) stand-ins for the few POSIX calls the reforge code uses.
// Only included under #ifdef _WIN32; other platforms never see this file.
//
// Paths: the reforge loaders treat a path as "on disk" when it is absolute.
// On Windows an absolute path starts with a drive letter ("C:/..."), so the
// realpath() stand-in below returns one with forward slashes, and
// uaIsAbsPath() accepts both forms.

#ifdef _WIN32

#include <direct.h>
#include <io.h>
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <string>

// mkdir(path, mode): Windows has no mode argument.
static inline int mkdir(const char *path, int /*mode*/)
{
    return _mkdir(path);
}

// realpath(path, NULL): absolute path with '/' separators, NULL when the path
// does not exist (as on POSIX). The caller frees the result.
static inline char *realpath(const char *path, char *resolved)
{
    if (!path || !path[0])
        return NULL;
    struct _stat64 st;
    if (_stat64(path, &st) != 0)
        return NULL;
    char *abs = _fullpath(resolved, path, resolved ? _MAX_PATH : 0);
    if (!abs)
        return NULL;
    for (char *p = abs; *p; ++p)
        if (*p == '\\')
            *p = '/';
    // no trailing separator (except the drive root "C:/")
    size_t n = strlen(abs);
    while (n > 3 && abs[n - 1] == '/')
        abs[--n] = 0;
    return abs;
}

// strcasestr: case-insensitive strstr (glibc / BSD extension).
static inline char *strcasestr(const char *hay, const char *needle)
{
    if (!*needle)
        return (char *)hay;
    for (; *hay; ++hay)
    {
        const char *h = hay, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n))
        {
            ++h;
            ++n;
        }
        if (!*n)
            return (char *)hay;
    }
    return NULL;
}

// windows.h (pulled in by the GL headers) defines near/far as empty macros;
// the reforge code uses them as variable names.
#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif

// An absolute disk path: "/..." or "C:/..." / "C:\\...".
static inline bool uaIsAbsPath(const std::string &p)
{
    if (p.size() > 2 && isalpha((unsigned char)p[0]) && p[1] == ':' && (p[2] == '/' || p[2] == '\\'))
        return true;
    return p[0] == '/';
}

#endif // _WIN32

#endif // UA_WINPORT_H_INCLUDED
