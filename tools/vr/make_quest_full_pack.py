#!/usr/bin/env python3
"""Build the Quest pack: the one download UA Reforged VR asks for (instead of the desktop pack parts + a texture part).

    make_quest_full_pack.py --version 0.9.0 --date 2026-11-02 <out.zip> <desktop part 1.zip> [<part 2.zip> ...] <quest part.zip>

It holds exactly what the headset keeps (platform/quest/src/setup/setup_flow.cpp, WantPackEntry):
  from the desktop pack parts   hd-models/ without the .dds maps, ui/, ground/, LICENSE.txt, README.md, PROVENANCE.md
  from the Quest texture part   hd-models/**.ktx (ASTC re-encodes, tools/vr/make_quest_pack.py)
  LINKS.tsv                     the pack's shared files, only the lines whose link the headset keeps (the app makes
                                these as hard links after the download and checks each one's SHA-256)
  QUEST-PACK.txt                version, sources and counts
Every entry goes under reforge-pack/. Nothing from the original game.

Reproducible: entries sorted by path, one timestamp (--date at 00:00), Unix modes 644/755, no extra fields or
comments; PNG/JPG are stored, everything else deflated (level 6). The bytes come straight from the input zips, so
the same inputs give the same zip. Prints the size and SHA-256.
"""
import argparse
import datetime
import hashlib
import os
import sys
import zipfile
import zlib

STORE = ('.png', '.jpg', '.jpeg', '.webp', '.avif', '.zip', '.gz')
DOCS = ('LICENSE.txt', 'README.md', 'PROVENANCE.md')
PREFIXES = ('reforge-pack/', 'reforge-pack-quest/')


def want(rel):
    """The headset's keep rule (setup_flow.cpp WantPackEntry), without the two files written here."""
    if rel.startswith('hd-models/'):
        return not rel.lower().endswith('.dds')
    return rel.startswith('ui/') or rel.startswith('ground/') or rel in DOCS


def split(name):
    for p in PREFIXES:
        if name.startswith(p):
            return p, name[len(p):]
    return None, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--version', required=True)
    ap.add_argument('--date', required=True)
    ap.add_argument('out')
    ap.add_argument('parts', nargs='+')
    a = ap.parse_args()
    d = datetime.date.fromisoformat(a.date)
    when = (d.year, d.month, d.day, 0, 0, 0)

    files = {}   # rel -> (zip path, entry name, crc, size)
    links = []   # (link, target, sha256)
    for zp in a.parts:
        with zipfile.ZipFile(zp) as z:
            for i in z.infolist():
                pre, rel = split(i.filename)
                if pre is None or i.is_dir() or not rel:
                    continue
                if rel == 'LINKS.tsv' and pre == 'reforge-pack/':
                    for line in z.read(i).decode('utf-8').splitlines():
                        t = line.split('\t')
                        if line and not line.startswith('#') and len(t) == 3 and (t[0], t[1], t[2]) not in links:
                            links.append((t[0], t[1], t[2]))
                    continue
                if not want(rel):
                    continue
                if rel in files:
                    if files[rel][2] != i.CRC or files[rel][3] != i.file_size:
                        sys.exit('two different files for ' + rel)
                    continue
                files[rel] = (zp, i.filename, i.CRC, i.file_size)

    kept = [l for l in links if want(l[0])]
    for link, target, _ in kept:
        if target not in files:
            sys.exit('LINKS.tsv: target not in the Quest pack: %s -> %s' % (link, target))
        if link in files:
            sys.exit('LINKS.tsv: link is also a stored file: ' + link)
    ktx = sum(1 for r in files if r.endswith('.ktx'))
    if not ktx:
        sys.exit('no Quest textures (.ktx) in the inputs')

    links_tsv = ('# link\ttarget\tsha256  (the app makes <link> a hard link of <target>; paths relative to the pack)\n' +
                 ''.join('%s\t%s\t%s\n' % l for l in sorted(kept))).encode('utf-8')
    count = lambda top: sum(1 for r in files if r.startswith(top))
    info = ('UA Reforged pack for Quest %s\n'
            'What UA Reforged VR installs on the headset: the Reforge pack\'s models, menus and ground (hd-models without\n'
            'the desktop DDS maps, ui, ground) plus ASTC (KTX 1.1) re-encodes of the pack\'s own maps and skies for the\n'
            'Quest GPU. Same licence as the Reforge pack (LICENSE.txt). No original game files.\n'
            'files\t%d\nhd-models\t%d\ntextures\t%d\nui\t%d\nground\t%d\nshared\t%d\n'
            % (a.version, len(files) + 2, count('hd-models/'), ktx, count('ui/'), count('ground/'), len(kept))).encode('utf-8')

    entries = {'reforge-pack/' + r: v for r, v in files.items()}
    entries['reforge-pack/LINKS.tsv'] = links_tsv
    entries['reforge-pack/QUEST-PACK.txt'] = info
    dirs = set()
    for n in entries:
        parts = n.split('/')[:-1]
        for k in range(1, len(parts) + 1):
            dirs.add('/'.join(parts[:k]) + '/')

    tmp = a.out + '.part'
    if os.path.exists(tmp):
        os.remove(tmp)
    opened = {}
    try:
        with zipfile.ZipFile(tmp, 'w', allowZip64=True) as out:
            for name in sorted(set(entries) | dirs):
                zi = zipfile.ZipInfo(name, date_time=when)
                zi.create_system = 3
                if name.endswith('/'):
                    zi.external_attr = (0o40755 << 16) | 0x10
                    out.writestr(zi, b'')
                    continue
                zi.external_attr = 0o100644 << 16
                stored = name.lower().endswith(STORE)
                zi.compress_type = zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED
                v = entries[name]
                if isinstance(v, bytes):
                    data = v
                else:
                    zp, en, crc, size = v
                    if zp not in opened:
                        opened[zp] = zipfile.ZipFile(zp)
                    data = opened[zp].read(en)
                    if len(data) != size or (zlib.crc32(data) & 0xffffffff) != crc:
                        sys.exit('read error: ' + en)
                out.writestr(zi, data, compress_type=zi.compress_type, compresslevel=None if stored else 6)
    finally:
        for z in opened.values():
            z.close()
    os.replace(tmp, a.out)
    h = hashlib.sha256()
    with open(a.out, 'rb') as fh:
        for b in iter(lambda: fh.read(1 << 20), b''):
            h.update(b)
    print('%s\t%d\t%s' % (os.path.basename(a.out), os.path.getsize(a.out), h.hexdigest()))
    print('files %d (hd-models %d, of which .ktx %d; ui %d; ground %d), shared %d' %
          (len(files) + 2, count('hd-models/'), ktx, count('ui/'), count('ground/'), len(kept)))


if __name__ == '__main__':
    main()
