#!/usr/bin/env python3
"""Turn an anonymous crash report back into function names, offline (docs/usage-stats.md, event "crash").

    python3 tools/symbolicate-crash.py <crash> <symbols folder>

<crash>: a crash body as the server stored it (JSON), or a usage-stats-crash.txt / queue line
(key=value pairs separated by tabs). <symbols folder>: the UNSTRIPPED binaries of exactly that build
(the "build" field), kept with the release builds and never shipped:
    macOS    UA_source               (engine-build/UA_source, before the app's strip -S)
    Windows  UA_source.exe           (engine-build/UA_source.exe; the app installs it stripped as UA-Reforged-engine.exe)
    Quest    libuavr.so              (build-quest/cmake/libuavr.so, before llvm-strip)
Frames in other modules (system libraries, drivers) are printed as they are.

Uses llvm-symbolizer when it can find one (PATH, Xcode, the Android NDK, or SYMBOLIZER=<path>), else atos
(macOS) or x86_64-w64-mingw32-addr2line (Windows builds). Python standard library only; reads the crash and the
binaries, writes nothing.
"""
import glob
import json
import os
import shutil
import struct
import subprocess
import sys

ALIASES = {"UA-Reforged-engine.exe": "UA_source.exe"}


def read_crash(path):
    text = open(path, encoding="utf-8", errors="replace").read().strip()
    if text.startswith("{"):
        d = json.loads(text)
        frames = d.get("frames", [])
    else:
        d = dict(kv.split("=", 1) for kv in text.splitlines()[0].split("\t") if "=" in kv)
        frames = [f for f in d.get("frames", "").split(",") if f]
    if d.get("event") != "crash":
        sys.exit("not a crash report")
    return d, frames


def image_base(path):
    """The address the offsets are relative to: Mach-O __TEXT vmaddr, PE ImageBase, ELF lowest PT_LOAD vaddr."""
    with open(path, "rb") as f:
        head = f.read(4096)
        magic = head[:4]
        if magic == b"\xcf\xfa\xed\xfe":  # Mach-O 64, little endian
            ncmds = struct.unpack_from("<I", head, 16)[0]
            f.seek(32)
            for _ in range(ncmds):
                cmd, size = struct.unpack("<II", f.read(8))
                body = f.read(size - 8)
                if cmd == 0x19 and body[:16].rstrip(b"\0") == b"__TEXT":  # LC_SEGMENT_64
                    return "macho", struct.unpack_from("<Q", body, 16)[0]
            return "macho", 0x100000000
        if magic == b"\xca\xfe\xba\xbe":
            sys.exit("universal binary: give the arm64 slice (lipo -thin arm64)")
        if head[:2] == b"MZ":
            pe = struct.unpack_from("<I", head, 0x3C)[0]
            opt = pe + 24
            kind = struct.unpack_from("<H", head, opt)[0]
            base = struct.unpack_from("<Q", head, opt + 24)[0] if kind == 0x20B else struct.unpack_from("<I", head, opt + 28)[0]
            return "pe", base
        if magic == b"\x7fELF":
            phoff = struct.unpack_from("<Q", head, 0x20)[0]
            phentsize, phnum = struct.unpack_from("<HH", head, 0x36)
            f.seek(phoff)
            lows = []
            for _ in range(phnum):
                ph = f.read(phentsize)
                if struct.unpack_from("<I", ph, 0)[0] == 1:  # PT_LOAD
                    lows.append(struct.unpack_from("<Q", ph, 16)[0])
            return "elf", min(lows) if lows else 0
    sys.exit("unknown binary format: " + path)


def find_symbolizer():
    cand = [os.environ.get("SYMBOLIZER"), shutil.which("llvm-symbolizer")]
    try:
        cand.append(subprocess.run(["xcrun", "--find", "llvm-symbolizer"], capture_output=True, text=True).stdout.strip())
    except OSError:
        pass
    for root in (os.environ.get("ANDROID_NDK_HOME"), os.environ.get("ANDROID_NDK_ROOT")):
        if root:
            cand += glob.glob(os.path.join(root, "toolchains/llvm/prebuilt/*/bin/llvm-symbolizer"))
    for sdk in (os.environ.get("ANDROID_SDK_ROOT"), os.path.expanduser("~/Library/Android/sdk")):
        if sdk:
            cand += sorted(glob.glob(os.path.join(sdk, "ndk/*/toolchains/llvm/prebuilt/*/bin/llvm-symbolizer")))[-1:]
    for c in cand:
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return None


def symbolize(binary, kind, addrs, tool):
    hexes = ["0x%x" % a for a in addrs]
    if tool:
        out = subprocess.run([tool, "--obj=" + binary, "--functions=linkage", "--demangle", "--inlining=false"] + hexes,
                             capture_output=True, text=True).stdout
        blocks = [b.strip().splitlines() for b in out.strip().split("\n\n")]
        return [" ".join(x.strip() for x in b) for b in blocks]
    if kind == "macho" and shutil.which("atos"):
        out = subprocess.run(["atos", "-o", binary, "-l", "0x%x" % image_base(binary)[1]] + hexes, capture_output=True, text=True).stdout
        return out.strip().splitlines()
    if kind == "pe" and shutil.which("x86_64-w64-mingw32-addr2line"):
        out = subprocess.run(["x86_64-w64-mingw32-addr2line", "-f", "-C", "-e", binary] + hexes, capture_output=True, text=True).stdout
        lines = out.strip().splitlines()
        return [lines[i] + " " + lines[i + 1] for i in range(0, len(lines) - 1, 2)]
    return ["(no symbolizer found)"] * len(addrs)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    crash, frames = read_crash(sys.argv[1])
    symdir = sys.argv[2]
    print("crash: %s  version %s  build %s  preset %s  level %s  after %s min" % (
        crash.get("signal"), crash.get("version", "?"), crash.get("build"), crash.get("preset"),
        crash.get("level", "-"), crash.get("minutes")))
    tool = find_symbolizer()
    parsed = []
    for i, fr in enumerate(frames):
        mod, _, off = fr.rpartition("+")
        # frames after the first are return addresses: look up the call instruction just before
        parsed.append((mod, int(off, 16) - (1 if i else 0)))
    names = {}
    for mod in sorted(set(m for m, _ in parsed)):
        path = os.path.join(symdir, ALIASES.get(mod, mod))
        if not os.path.isfile(path):
            continue
        kind, base = image_base(path)
        offs = [o for m, o in parsed if m == mod]
        res = symbolize(path, kind, [base + o for o in offs], tool)
        for o, r in zip(offs, res):
            names[(mod, o)] = r
    for i, (mod, off) in enumerate(parsed):
        print("#%-2d %s  %s" % (i, frames[i], names.get((mod, off), "")))


if __name__ == "__main__":
    main()
