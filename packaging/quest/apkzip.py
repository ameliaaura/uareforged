#!/usr/bin/env python3
"""Repack an APK with fixed entry dates and a stable order (reproducible APK contents).

    apkzip.py <aapt2 output .apk> <out .apk> --assets DIR --lib DIR [--date 2026-11-01]

Keeps what aapt2 linked (AndroidManifest.xml, resources.arsc stored uncompressed, res/...), adds assets/ (deflated)
and lib/ (stored, so the .so files can be mapped straight from the APK: extractNativeLibs=false). Every entry gets
the same date, mode 0644 and no extra fields. zipalign and apksigner run afterwards (build-quest.sh).
"""
import argparse
import os
import zipfile


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("base")
    ap.add_argument("out")
    ap.add_argument("--assets", required=True)
    ap.add_argument("--lib", required=True)
    ap.add_argument("--date", default="2026-11-01")
    a = ap.parse_args()
    y, m, d = (int(x) for x in a.date.split("-"))
    when = (y, m, d, 0, 0, 0)

    def add(z, name, data, stored):
        zi = zipfile.ZipInfo(name, when)
        zi.compress_type = zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED
        zi.external_attr = 0o644 << 16
        zi.create_system = 3
        z.writestr(zi, data, compresslevel=9 if not stored else None)

    with zipfile.ZipFile(a.base) as src, zipfile.ZipFile(a.out, "w") as z:
        names = src.namelist()
        order = ["AndroidManifest.xml", "resources.arsc"] + sorted(n for n in names if n not in ("AndroidManifest.xml", "resources.arsc"))
        for n in order:
            if n not in names:
                continue
            info = src.getinfo(n)
            add(z, n, src.read(n), n == "resources.arsc" or info.compress_type == zipfile.ZIP_STORED)
        for root_dir, prefix, stored in ((a.assets, "assets/", False), (a.lib, "lib/", True)):
            files = []
            for root, dirs, fs in os.walk(root_dir):
                dirs.sort()
                for f in sorted(fs):
                    p = os.path.join(root, f)
                    files.append((prefix + os.path.relpath(p, root_dir).replace(os.sep, "/"), p))
            for n, p in sorted(files):
                with open(p, "rb") as fh:
                    add(z, n, fh.read(), stored)


if __name__ == "__main__":
    main()
