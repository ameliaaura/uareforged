#include "profile_patch.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#if defined(_WIN32) && !defined(__WINE__)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#endif

namespace ProfilePatch
{

static bool SameWord(const std::string &a, const std::string &b)
{
    if ( a.size() != b.size() )
        return false;
    for (size_t i = 0; i < a.size(); i++)
        if ( tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]) )
            return false;
    return true;
}

// first word as the script parser sees it; comment lines never count
static std::string FirstWord(const std::string &l)
{
    size_t a = l.find_first_not_of(" \t");
    if ( a == std::string::npos || l[a] == ';' )
        return std::string();
    size_t e = l.find_first_of(" \t=;\r\n", a);
    return l.substr(a, e == std::string::npos ? std::string::npos : e - a);
}

// value = text after '=' up to an inline "; comment", without blanks
static std::string ValueOf(const std::string &l)
{
    std::string v;
    size_t eq = l.find('=');
    if ( eq == std::string::npos )
        return v;
    for (size_t i = eq + 1; i < l.size() && l[i] != ';'; i++)
        if ( l[i] != ' ' && l[i] != '\t' && l[i] != '\r' && l[i] != '\n' )
            v += l[i];
    return v;
}

bool PatchInputSection(std::string &text, const std::vector<std::string> &fresh)
{
    std::vector<std::string> lines;
    for (size_t at = 0; at < text.size();)
    {
        size_t e = text.find('\n', at);
        e = (e == std::string::npos) ? text.size() : e + 1;
        lines.push_back(text.substr(at, e - at));
        at = e;
    }

    int begin = -1, end = -1;
    for (size_t i = 0; i < lines.size() && end < 0; i++)
    {
        const std::string w = FirstWord(lines[i]);
        if ( begin < 0 && SameWord(w, "new_input") )
            begin = (int)i;
        else if ( begin >= 0 && SameWord(w, "end") )
            end = (int)i;
    }
    if ( begin < 0 || end < 0 )
        return false;

    std::vector<bool> used(fresh.size(), false);
    for (int i = begin + 1; i < end; i++)
    {
        const std::string k = FirstWord(lines[i]);
        for (size_t f = 0; f < fresh.size(); f++)
        {
            if ( k.empty() || !SameWord(k, FirstWord(fresh[f])) )
                continue;
            used[f] = true;
            if ( ValueOf(lines[i]) == ValueOf(fresh[f]) )
                break;
            std::string &l = lines[i];
            std::string eol;
            while ( !l.empty() && (l.back() == '\n' || l.back() == '\r') )
            {
                eol.insert(eol.begin(), l.back());
                l.pop_back();
            }
            l = l.substr(0, l.find_first_not_of(" \t")) + fresh[f] + eol;
            break;
        }
    }

    std::vector<std::string> add;
    for (size_t f = 0; f < fresh.size(); f++)
        if ( !used[f] )
            add.push_back("    " + fresh[f] + "\n");
    if ( !add.empty() )
    {
        std::string &prev = lines[end - 1];
        if ( !prev.empty() && prev.back() != '\n' && prev.back() != '\r' )
            prev += '\n';
        lines.insert(lines.begin() + end, add.begin(), add.end());
    }

    std::string out;
    out.reserve(text.size() + 64);
    for (const std::string &l : lines)
        out += l;
    text.swap(out);
    return true;
}

bool WriteFileAtomic(const std::string &path, const std::string &data)
{
#if defined(_WIN32) && !defined(__WINE__)
    const std::string tmp = path + ".tmp" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount());
    int fd = _open(tmp.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
    if ( fd < 0 )
        return false;
    FILE *f = _fdopen(fd, "wb");
    if ( !f )
    {
        _close(fd);
        remove(tmp.c_str());
        return false;
    }
#else
    std::string tmp = path + ".tmpXXXXXX";
    int fd = mkstemp(&tmp[0]);
    if ( fd < 0 )
        return false;
    FILE *f = fdopen(fd, "wb");
    if ( !f )
    {
        close(fd);
        remove(tmp.c_str());
        return false;
    }
#endif
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (fflush(f) == 0) && ok;
#if defined(_WIN32) && !defined(__WINE__)
    ok = (_commit(_fileno(f)) == 0) && ok;
#else
    ok = (fsync(fileno(f)) == 0) && ok;
#endif
    ok = (fclose(f) == 0) && ok;
#if defined(_WIN32) && !defined(__WINE__)
    ok = ok && MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    ok = ok && rename(tmp.c_str(), path.c_str()) == 0;
#endif
    if ( !ok )
        remove(tmp.c_str());
    return ok;
}

}
