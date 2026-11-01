#!/usr/bin/env python3
"""Reforge pack hd-models -> Quest textures (ASTC in KTX 1.1, full mip chain).

    quest_texconv.py <hd-models src> <dst dir> [--astcenc PATH] [--max-size N] [--jobs N]

For every *.mtl: copies the .mtl and its .obj, and converts each referenced map (map_Kd, norm,
map_ORM; the PNG named in the .mtl) to <stem>.ktx beside it. The engine (obj3d.cpp LoadMap)
uses <stem>.ktx when the GL has ASTC (Quest: GL_KHR_texture_compression_astc_ldr). BC/DDS and
PNG maps are not shipped.
  albedo / fx maps: ASTC 6x6, encoded with sRGB error metric (-cs)
  ORM:              ASTC 6x6, linear (-cl)
  normal:           ASTC 5x5, linear (-cl), full RGB (the engine rebuilds nothing: NormalRG=0)
Mip levels: the authored <stem>_mipN.png far-read levels when present at the right size, else a
Lanczos downscale of the previous level, down to 1x1. --max-size caps level 0 (default: none).
Encoder: ARM astcenc 5.x (built from source; --astcenc or $ASTCENC), -medium quality.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

from PIL import Image

GL_ASTC = {(4, 4): 0x93B0, (5, 5): 0x93B2, (6, 6): 0x93B4, (8, 8): 0x93B7}
KTX_ID = bytes([0xAB, 0x4B, 0x54, 0x58, 0x20, 0x31, 0x31, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A])


def maps_of(mtl_path):
    out = []
    for line in open(mtl_path, encoding="utf-8", errors="replace"):
        t = line.split()
        if not t:
            continue
        if t[0] in ("map_Kd", "map_ORM", "norm", "map_Bump", "bump"):
            out.append((t[0], t[-1]))
    return out


def kind_of(key, name):
    n = name.lower()
    if key in ("norm", "map_Bump", "bump") or "_normal" in n:
        return "normal"
    if key == "map_ORM" or "_orm" in n:
        return "orm"
    return "albedo"


def mip_chain(src_png, max_size):
    img = Image.open(src_png)
    img = img.convert("RGBA")
    if max_size and max(img.size) > max_size:
        s = max_size / max(img.size)
        img = img.resize((max(1, int(img.width * s)), max(1, int(img.height * s))), Image.LANCZOS)
    levels = [img]
    stem, ext = os.path.splitext(src_png)
    lv = 1
    while levels[-1].width > 1 or levels[-1].height > 1:
        w, h = max(1, levels[-1].width // 2), max(1, levels[-1].height // 2)
        authored = f"{stem}_mip{lv}{ext}"
        nxt = None
        if not max_size and os.path.exists(authored):
            a = Image.open(authored).convert("RGBA")
            if a.size == (w, h):
                nxt = a
        if nxt is None:
            nxt = levels[-1].resize((w, h), Image.LANCZOS)
        levels.append(nxt)
        lv += 1
    return levels


def encode(astcenc, png, out_astc, block, srgb):
    mode = "-cs" if srgb else "-cl"
    cmd = [astcenc, mode, png, out_astc, f"{block[0]}x{block[1]}", "-medium", "-silent", "-j", "2"]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    data = open(out_astc, "rb").read()
    if struct.unpack("<I", data[:4])[0] != 0x5CA1AB13:
        raise RuntimeError(f"bad astc output {out_astc}")
    return data[16:]


def convert(astcenc, src_png, dst_ktx, kind, max_size):
    block = (5, 5) if kind == "normal" else (6, 6)
    srgb = kind == "albedo"
    levels = mip_chain(src_png, max_size)
    blobs = []
    with tempfile.TemporaryDirectory(dir=os.path.dirname(dst_ktx)) as td:
        for i, im in enumerate(levels):
            p = os.path.join(td, f"l{i}.png")
            im.save(p)
            blobs.append(encode(astcenc, p, os.path.join(td, f"l{i}.astc"), block, srgb))
    w, h = levels[0].size
    fmt = GL_ASTC[block] + (0x20 if srgb else 0)
    hdr = KTX_ID + struct.pack("<13I", 0x04030201, 0, 1, 0, fmt, 0x1908, w, h, 0, 0, 1, len(blobs), 0)
    with open(dst_ktx + ".part", "wb") as f:
        f.write(hdr)
        for b in blobs:
            f.write(struct.pack("<I", len(b)))
            f.write(b)
            f.write(b"\0" * ((4 - len(b) % 4) % 4))
    os.replace(dst_ktx + ".part", dst_ktx)
    return dst_ktx, w, h, len(blobs), os.path.getsize(dst_ktx)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--astcenc", default=os.environ.get("ASTCENC", "astcenc-neon"))
    ap.add_argument("--max-size", type=int, default=0)
    ap.add_argument("--jobs", type=int, default=4)
    a = ap.parse_args()
    os.makedirs(a.dst, exist_ok=True)
    jobs = []
    for f in sorted(os.listdir(a.src)):
        if not f.lower().endswith(".mtl"):
            continue
        stem = f[:-4]
        shutil.copy2(os.path.join(a.src, f), os.path.join(a.dst, f))
        obj = os.path.join(a.src, stem + ".obj")
        if os.path.exists(obj):
            shutil.copy2(obj, os.path.join(a.dst, stem + ".obj"))
        for key, name in maps_of(os.path.join(a.src, f)):
            png = os.path.join(a.src, name)
            if not os.path.exists(png):
                print(f"missing map {png}", file=sys.stderr)
                continue
            dst = os.path.join(a.dst, os.path.splitext(name)[0] + ".ktx")
            if os.path.exists(dst) and os.path.getmtime(dst) >= os.path.getmtime(png):
                continue
            jobs.append((png, dst, kind_of(key, name)))
    # de-duplicate (maps shared between materials)
    seen, uniq = set(), []
    for j in jobs:
        if j[1] not in seen:
            seen.add(j[1])
            uniq.append(j)
    print(f"{len(uniq)} maps to convert")
    with ThreadPoolExecutor(a.jobs) as ex:
        for dst, w, h, n, size in ex.map(lambda j: convert(a.astcenc, j[0], j[1], j[2], a.max_size), uniq):
            print(f"  {os.path.basename(dst)} {w}x{h} {n} levels {size / 1e6:.2f} MB")


if __name__ == "__main__":
    main()
