// Guards against textures that are drawn before every texel was written.
//
// glTexImage2D(..., NULL) leaves a texture's texels undefined. Apple's GL
// returns zeros, Windows drivers commonly return recycled video memory. The 2D
// screen and text layers are blended over the whole window but then only get
// partial uploads, so allocating them without data garbled the next menu or
// loading screen after a resolution change on Windows (0.8.2).
//
// Scans the engine sources (no game data, no GL):
//   c++ -std=c++17 -Wall tests/gl_texture_init_test.cpp -o /tmp/gtit && /tmp/gtit src
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { checks++; if ( !(cond) ) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static std::string ReadFile(const fs::path &p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// The argument list of the call starting at `open` (index of its '('), without
// the parentheses; empty if unbalanced.
static std::string CallArgs(const std::string &s, size_t open)
{
    int depth = 0;
    for (size_t i = open; i < s.size(); i++)
    {
        if ( s[i] == '(' )
            depth++;
        else if ( s[i] == ')' && --depth == 0 )
            return s.substr(open + 1, i - open - 1);
    }
    return std::string();
}

static std::string Trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

static std::string LastArg(const std::string &args)
{
    size_t comma = args.rfind(',');
    return Trim(comma == std::string::npos ? args : args.substr(comma + 1));
}

// Body of the function definition `sig` (e.g. "void GFXEngine::RecreateTextSurface()").
static std::string FunctionBody(const std::string &s, const std::string &sig)
{
    size_t at = s.find(sig);
    if ( at == std::string::npos )
        return std::string();
    size_t open = s.find('{', at);
    if ( open == std::string::npos )
        return std::string();
    int depth = 0;
    for (size_t i = open; i < s.size(); i++)
    {
        if ( s[i] == '{' )
            depth++;
        else if ( s[i] == '}' && --depth == 0 )
            return s.substr(open, i - open + 1);
    }
    return std::string();
}

static size_t Count(const std::string &s, const std::string &what)
{
    size_t n = 0;
    for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + what.size()))
        n++;
    return n;
}

int main(int argc, char **argv)
{
    const fs::path src = argc > 1 ? argv[1] : "src";
    if ( !fs::is_directory(src / "system") )
    {
        std::printf("usage: %s <engine src dir>\n", argv[0]);
        return 2;
    }

    // Data-less allocations that are fine, and why. A new one must be added
    // here on purpose, after checking it is fully written before it is drawn.
    const std::map<std::string, int> allowed = {
        {"system/gfx.cpp",      1}, // TexImageNoData() itself (FBO colour targets: rendered before sampled)
        {"system/gfx_glow.cpp", 2}, // glow render targets
        {"system/shadows.cpp",  1}, // shadow depth target
        {"system/movie.cpp",    1}, // movie frame: the full frame is uploaded right after
        {"gui/root.cpp",        1}, // StreamTex: the full image is uploaded right after
        {"vr/vr_sim.cpp",       1}, // VR simulator eye targets
    };

    std::map<std::string, int> found;
    for (const fs::directory_entry &e : fs::recursive_directory_iterator(src))
    {
        const std::string ext = e.path().extension().string();
        if ( !e.is_regular_file() || (ext != ".cpp" && ext != ".mm" && ext != ".h") )
            continue;
        const std::string rel = fs::relative(e.path(), src).generic_string();
        const std::string s = ReadFile(e.path());
        const std::string call = "glTexImage2D(";
        for (size_t at = s.find(call); at != std::string::npos; at = s.find(call, at + call.size()))
        {
            if ( at > 0 && (isalnum((unsigned char)s[at - 1]) || s[at - 1] == '_') )
                continue; // some other identifier ending in glTexImage2D
            const std::string last = LastArg(CallArgs(s, at + call.size() - 1));
            if ( last == "NULL" || last == "nullptr" || last == "0" )
                found[rel]++;
        }
    }

    for (const auto &f : found)
    {
        auto it = allowed.find(f.first);
        const int ok = it == allowed.end() ? 0 : it->second;
        if ( f.second > ok )
            std::printf("FAIL: %s allocates %d texture(s) without data (allowed %d)\n", f.first.c_str(), f.second, ok);
        CHECK(f.second <= ok);
    }

    // The 2D layers start as a copy of their cleared CPU surface.
    const std::string gfx = ReadFile(src / "system/gfx.cpp");
    const std::string scr = FunctionBody(gfx, "void GFXEngine::RecreateScreenSurface()");
    const std::string txt = FunctionBody(gfx, "void GFXEngine::RecreateTextSurface()");
    CHECK(!scr.empty());
    CHECK(!txt.empty());
    CHECK(Count(scr, "TexImageFromSurface(ScreenSurface") == 1);
    CHECK(Count(txt, "TexImageFromSurface(TextSurface") == 1);
    CHECK(Count(scr, "TexImageNoData(") == 0);
    CHECK(Count(txt, "TexImageNoData(") == 0);
    CHECK(Count(scr, "glTexImage2D(") == 0);
    CHECK(Count(txt, "glTexImage2D(") == 0);

    // The text layer is cleared before its texture is made from it.
    const size_t clear = txt.find("SDL_FillRect(TextSurface");
    const size_t alloc = txt.find("TexImageFromSurface(TextSurface");
    CHECK(clear != std::string::npos && alloc != std::string::npos && clear < alloc);

    // TexImageFromSurface uploads the surface's pixels with its own row length.
    const std::string helper = FunctionBody(gfx, "static void TexImageFromSurface(");
    CHECK(Count(helper, "s->pixels") == 1);
    CHECK(Count(helper, "GL_UNPACK_ROW_LENGTH, s->pitch") == 1);

    std::printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
