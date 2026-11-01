#!/usr/bin/env python3
"""Reproducible, metadata-free zip of one or more folders.

    python3 mkzip.py [--date YYYY-MM-DD] <out.zip> <folder> [<folder> ...]

Each folder is stored under its own name. What goes in, and what does not:
  - entries sorted by path; folders get their own entries
  - every entry carries the same timestamp: --date at 00:00 (default: $SOURCE_DATE_EPOCH, else 1980-01-01)
  - Unix modes normalised: folders and executables 755, other files 644; symlinks stored as links
  - no extra fields (no owner uid/gid, no extended timestamps), no comments
  - no extended attributes, resource forks or Finder data: no __MACOSX/ or ._* entries, no .DS_Store
  - already-compressed media (png, mkv, webp, avif, jpg, mp4, zip) are stored, everything else deflated
"""
import argparse, datetime, os, stat, sys, zipfile

STORE = ('.png', '.mkv', '.webp', '.avif', '.jpg', '.jpeg', '.mp4', '.zip', '.gz')
SKIP = ('.DS_Store',)


def date_tuple(s):
    if s:
        d = datetime.date.fromisoformat(s)
    elif os.environ.get('SOURCE_DATE_EPOCH'):
        d = datetime.datetime.fromtimestamp(int(os.environ['SOURCE_DATE_EPOCH']), datetime.timezone.utc).date()
    else:
        d = datetime.date(1980, 1, 1)
    return (d.year, d.month, d.day, 0, 0, 0)


def entries(folder):
    base = os.path.basename(os.path.normpath(folder))
    parent = os.path.dirname(os.path.abspath(folder))
    out = [(base + '/', os.path.abspath(folder))]
    for root, dirs, files in os.walk(folder):
        dirs.sort()
        for d in dirs:
            p = os.path.join(root, d)
            out.append((os.path.relpath(p, parent).replace(os.sep, '/') + ('' if os.path.islink(p) else '/'), p))
        for f in sorted(files):
            if f in SKIP or f.startswith('._'):
                continue
            p = os.path.join(root, f)
            out.append((os.path.relpath(p, parent).replace(os.sep, '/'), p))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--date')
    ap.add_argument('out')
    ap.add_argument('folders', nargs='+')
    a = ap.parse_args()
    when = date_tuple(a.date)
    items = sorted(e for f in a.folders for e in entries(f))
    if os.path.exists(a.out):
        os.remove(a.out)
    with zipfile.ZipFile(a.out, 'w', allowZip64=True) as z:
        for name, path in items:
            zi = zipfile.ZipInfo(name, date_time=when)
            zi.create_system = 3
            st = os.lstat(path)
            if stat.S_ISLNK(st.st_mode):
                zi.external_attr = (stat.S_IFLNK | 0o755) << 16
                z.writestr(zi, os.readlink(path), compress_type=zipfile.ZIP_STORED)
            elif stat.S_ISDIR(st.st_mode):
                zi.external_attr = ((stat.S_IFDIR | 0o755) << 16) | 0x10
                z.writestr(zi, b'')
            else:
                mode = 0o755 if st.st_mode & 0o111 else 0o644
                zi.external_attr = (stat.S_IFREG | mode) << 16
                zi.compress_type = zipfile.ZIP_STORED if name.lower().endswith(STORE) else zipfile.ZIP_DEFLATED
                zi.file_size = st.st_size
                with open(path, 'rb') as src, z.open(zi, 'w', force_zip64=st.st_size >= 0x7fffffff) as dst:
                    while True:
                        b = src.read(1 << 20)
                        if not b:
                            break
                        dst.write(b)
    print('%s: %d entries' % (a.out, len(items)))


if __name__ == '__main__':
    main()
