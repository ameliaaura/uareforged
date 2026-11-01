#!/usr/bin/env python3
"""Build the Quest texture part of the Reforge pack (ASTC in KTX 1.1) from an unpacked Reforge pack.

    make_quest_pack.py <reforge-pack dir> <out dir> --version 0.9.0 [--astcenc PATH] [--jobs N] [--zip]

Why a separate part: the Quest GPU cannot sample the BC1/BC5/BC7 (DDS) maps of the desktop pack, and encoding
ASTC on the headset would keep its CPU at full load for many minutes after the download. So the textures are
re-encoded here, once, into a texture part; tools/vr/make_quest_full_pack.py then joins it with the pack files the
headset keeps into the Quest pack (the one download the VR app asks for). It holds ONLY re-encoded pack content
(nothing from the original game).

Output: <out>/reforge-pack-quest/
  hd-models/**/<stem>.ktx     every map named in an .mtl (top level and lego/<set>/): ASTC 6x6 (albedo, sRGB;
                              ORM linear) or 5x5 (normal maps, linear), level 0 capped at 2048 px, full mips.
                              The engine uses <stem>.ktx when it sits beside the map (obj3d.cpp LoadMap).
  hd-models/sky/<sky>/sky_<w>.ktx   every sky panorama, ASTC 6x6 linear, full mips (reforge_sky.cpp).
  QUEST-PACK.txt              version + the counts.
With --zip: <out>/UA-Reforged-pack-<version>-quest.zip (fixed timestamps, sorted entries) + its SHA-256.
Source maps: the PNG when the pack has it, else its DDS (decoded with Pillow).
"""
import argparse
import hashlib
import io
import os
import struct
import sys
import tempfile
import zipfile
from concurrent.futures import ThreadPoolExecutor

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import quest_texconv  # noqa: E402

ZIP_TIME = (2026, 11, 1, 0, 0, 0)


LINKS = {}  # pack path -> the stored file it shares bytes with (LINKS.tsv; install-links.sh makes these)


def load_links(pack):
    p = os.path.join(pack, "LINKS.tsv")
    if os.path.exists(p):
        for line in open(p, encoding="utf-8"):
            t = line.rstrip("\n").split("\t")
            if len(t) >= 2 and not t[0].startswith("#"):
                LINKS[os.path.join(pack, t[0])] = os.path.join(pack, t[1])


def source_of(path):
    """The pack file to read for a map name: the PNG itself (or the stored file it is linked to), else its DDS."""
    for p in (path, os.path.splitext(path)[0] + ".dds"):
        p = os.path.normpath(p)
        if os.path.exists(p):
            return p
        if p in LINKS and os.path.exists(LINKS[p]):
            return LINKS[p]
    return None


# DX10 sRGB formats Pillow does not decode -> the UNORM twin with the same block bytes (the encoder then applies
# the sRGB error metric itself for albedo maps, as for a PNG source).
SRGB_TO_UNORM = {72: 71, 75: 74, 78: 77, 91: 87, 93: 88, 99: 98}


def open_map(src):
    if not src.lower().endswith(".dds"):
        return Image.open(src)
    data = bytearray(open(src, "rb").read())
    if data[:4] == b"DDS " and data[84:88] == b"DX10":
        fmt = struct.unpack_from("<I", data, 128)[0]
        if fmt in SRGB_TO_UNORM:
            struct.pack_into("<I", data, 128, SRGB_TO_UNORM[fmt])
    return Image.open(io.BytesIO(bytes(data)))


def convert_one(job, astcenc):
    src, dst, kind, max_size = job
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if src.lower().endswith(".dds"):
        with tempfile.TemporaryDirectory(dir=os.path.dirname(dst)) as td:
            png = os.path.join(td, "src.png")
            open_map(src).convert("RGBA").save(png)
            return quest_texconv.convert(astcenc, png, dst, kind, max_size)
    return quest_texconv.convert(astcenc, src, dst, kind, max_size)


def collect(pack, out):
    hdm = os.path.join(pack, "hd-models")
    load_links(pack)
    jobs, missing, seen = [], [], set()
    for root, dirs, files in os.walk(hdm):
        dirs.sort()
        rel = os.path.relpath(root, hdm)
        if rel.split(os.sep)[0] == "sky":
            continue
        for f in sorted(files):
            if not f.lower().endswith(".mtl"):
                continue
            for key, name in quest_texconv.maps_of(os.path.join(root, f)):
                if name.startswith("/") or ".." in name:
                    continue
                src = source_of(os.path.join(root, name))
                if not src:
                    missing.append(os.path.join(rel, name))
                    continue
                dst = os.path.join(out, "hd-models", rel, os.path.splitext(name)[0] + ".ktx")
                dst = os.path.normpath(dst)
                if dst in seen:
                    continue
                seen.add(dst)
                jobs.append((src, dst, quest_texconv.kind_of(key, name), 2048))
    sky = os.path.join(hdm, "sky")
    n_sky = 0
    if os.path.isdir(sky):
        for s in sorted(os.listdir(sky)):
            d = os.path.join(sky, s)
            if not os.path.isdir(d):
                continue
            for f in sorted(os.listdir(d)):
                stem, ext = os.path.splitext(f)
                if not f.startswith("sky_") or ext.lower() not in (".dds", ".png"):
                    continue
                if ext.lower() == ".png" and os.path.exists(os.path.join(d, stem + ".dds")):
                    continue
                dst = os.path.join(out, "hd-models", "sky", s, stem + ".ktx")
                if dst in seen:
                    continue
                seen.add(dst)
                jobs.append((os.path.join(d, f), dst, "orm", 0))  # "orm" = ASTC 6x6, linear (skies load UNORM)
                n_sky += 1
    return jobs, missing, n_sky


def write_zip(out_root, zpath):
    names = []
    for root, dirs, files in os.walk(out_root):
        dirs.sort()
        for f in sorted(files):
            names.append(os.path.join(root, f))
    base = os.path.dirname(out_root)
    with zipfile.ZipFile(zpath + ".part", "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for p in names:
            zi = zipfile.ZipInfo(os.path.relpath(p, base), ZIP_TIME)
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = 0o644 << 16
            zi.create_system = 3
            with open(p, "rb") as fh:
                z.writestr(zi, fh.read())
    os.replace(zpath + ".part", zpath)
    h = hashlib.sha256()
    with open(zpath, "rb") as fh:
        for b in iter(lambda: fh.read(1 << 20), b""):
            h.update(b)
    return os.path.getsize(zpath), h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pack")
    ap.add_argument("out")
    ap.add_argument("--version", required=True)
    ap.add_argument("--astcenc", default=os.environ.get("ASTCENC", "astcenc-neon"))
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--zip", action="store_true")
    ap.add_argument("--limit", type=int, default=0, help="convert only the first N maps (a quick test)")
    a = ap.parse_args()
    root = os.path.join(a.out, "reforge-pack-quest")
    os.makedirs(root, exist_ok=True)
    jobs, missing, n_sky = collect(a.pack, root)
    for m in missing:
        print("missing map (no PNG or DDS in the pack):", m, file=sys.stderr)
    todo = [j for j in jobs if not os.path.exists(j[1])]
    if a.limit:
        todo = todo[: a.limit]
    print(f"{len(jobs)} textures ({n_sky} sky panoramas), {len(todo)} to encode, {len(missing)} missing")
    done = 0
    with ThreadPoolExecutor(a.jobs) as ex:
        for dst, w, h, n, size in ex.map(lambda j: convert_one(j, a.astcenc), todo):
            done += 1
            if done % 25 == 0 or done == len(todo):
                print(f"  {done}/{len(todo)} {os.path.relpath(dst, root)} {w}x{h} {size / 1e6:.2f} MB", flush=True)
    ktx = sum(1 for _, _, fs in os.walk(root) for f in fs if f.endswith(".ktx"))
    with open(os.path.join(root, "QUEST-PACK.txt"), "w") as f:
        f.write("UA Reforged pack, Quest textures %s\n" % a.version)
        f.write("ASTC (KTX 1.1) re-encodes of the Reforge pack's own maps and skies, for the Quest GPU.\n")
        f.write("Same licence as the Reforge pack (LICENSE.txt in the pack). No original game files.\n")
        f.write("textures\t%d\n" % ktx)
    if a.zip:
        if missing or len(todo) and a.limit:
            sys.exit("not zipping: missing maps or a --limit run")
        name = "UA-Reforged-pack-%s-quest.zip" % a.version
        size, sha = write_zip(root, os.path.join(a.out, name))
        print(f"part\t{name}\t{size}\t{sha}")
        with open(os.path.join(a.out, name + ".sha256"), "w") as f:
            f.write(f"{sha}  {name}\n")


if __name__ == "__main__":
    main()
