#!/usr/bin/env python3
"""Fetch the Windows (x86_64, UCRT) dependency sysroot for the UA Reforged
cross-build (packaging/windows/build-windows.sh): MSYS2 ucrt64 binary packages (dependency-resolved from the repo DB) plus
BtbN's LGPL shared FFmpeg 8.1 build. Output: <out>/sysroot (headers, import libs,
pkg-config files rewritten to the sysroot path, runtime DLLs in sysroot/bin).

usage: fetch-deps.py <out-dir>
"""
import os, sys, tarfile, urllib.request, subprocess, re, shutil, zipfile

MIRROR = "https://mirror.msys2.org/mingw/ucrt64"
PFX = "mingw-w64-ucrt-x86_64-"
ROOTS = ["SDL2", "SDL2_image", "SDL2_ttf", "SDL2_net", "libvorbis", "openal", "lua",
         "gcc-libs", "libwinpthread"]
# Build/tooling-only dependencies that MSYS2 declares but a game runtime never loads
# (the post-build DLL walk in build-windows.sh proves nothing needed is missing).
SKIP = {PFX + x for x in ["python", "python-packaging", "tcl", "tk", "sqlite3", "ncurses",
        "tzdata", "mpdecimal", "wineditline", "libb2"]}
FFMPEG_URL = ("https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/"
              "ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip")

out = os.path.abspath(sys.argv[1])
cache = os.path.join(out, "cache"); os.makedirs(cache, exist_ok=True)
sysroot = os.path.join(out, "sysroot")
shutil.rmtree(sysroot, ignore_errors=True); os.makedirs(sysroot)

def fetch(url, dest):
    if not os.path.exists(dest):
        print("GET", url); tmp = dest + ".part"
        subprocess.check_call(["curl", "-sfL", "-o", tmp, url]); os.rename(tmp, dest)
    return dest

db = fetch(f"{MIRROR}/ucrt64.db", os.path.join(cache, "ucrt64.db"))
pkgs, provides = {}, {}
with tarfile.open(db) as t:
    for m in t.getmembers():
        if not m.name.endswith("/desc"): continue
        txt = t.extractfile(m).read().decode()
        f = {}
        for block in txt.strip().split("\n\n"):
            lines = block.strip().split("\n"); f[lines[0].strip("%")] = lines[1:]
        name = f["NAME"][0]; pkgs[name] = f
        for p in f.get("PROVIDES", []): provides[re.split("[<>=]", p)[0]] = name

def resolve(dep):
    n = re.split("[<>=]", dep)[0]
    return n if n in pkgs else provides.get(n)

want, stack = set(), [PFX + r for r in ROOTS]
while stack:
    n = resolve(stack.pop())
    if not n or n in want or n in SKIP: continue
    want.add(n); stack.extend(pkgs[n].get("DEPENDS", []))
print(len(want), "packages:", " ".join(sorted(x[len(PFX):] for x in want)))

for n in sorted(want):
    fn = pkgs[n]["FILENAME"][0]
    p = fetch(f"{MIRROR}/{fn}", os.path.join(cache, fn))
    subprocess.call(["tar", "-xf", p, "-C", sysroot, "--strip-components=1", "ucrt64"],
                    stderr=subprocess.DEVNULL)  # meta packages carry no ucrt64/ tree

z = fetch(FFMPEG_URL, os.path.join(cache, os.path.basename(FFMPEG_URL)))
with zipfile.ZipFile(z) as zf:
    top = zf.namelist()[0].split("/")[0]
    for m in zf.namelist():
        rel = m[len(top) + 1:]
        if not rel or m.endswith("/") or rel.startswith("doc/"): continue
        if rel.startswith("bin/") and rel.endswith(".exe"): continue
        dest = os.path.join(sysroot, rel); os.makedirs(os.path.dirname(dest), exist_ok=True)
        with zf.open(m) as src, open(dest, "wb") as d: shutil.copyfileobj(src, d)

# pkg-config files carry prefix=/ucrt64 (MSYS2) or a CI path (BtbN) -> point at the sysroot
pcdir = os.path.join(sysroot, "lib", "pkgconfig")
for pc in os.listdir(pcdir):
    path = os.path.join(pcdir, pc); s = open(path).read()
    s = re.sub(r"^prefix=.*$", "prefix=${pcfiledir}/../..", s, flags=re.M)  # relocatable
    open(path, "w").write(s)
print("sysroot ready:", sysroot)
