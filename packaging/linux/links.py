#!/usr/bin/env python3
"""UA Reforged - strict check of the site's links.json (the Linux twin of packaging/macos/links.js).
    python3 -I links.py <file> "<allowed host> ..." <allow local http 0|1> [platform for "latest", linux]
Only reads and checks data: json.loads, types, lengths, https URLs on an allowlist of hosts. Nothing in the file is
ever run. Prints "key<TAB>value" lines (parts as "part<TAB>name<TAB>bytes<TAB>sha256"); any problem rejects the
WHOLE file (exit 1, the reason on stderr) and the launcher uses its built-in links."""
import json
import re
import sys


def fail(why):
    sys.stderr.write('links.json: ' + why + '\n')
    sys.exit(1)


def is_int(v, lo, hi):
    return type(v) in (int, float) and not isinstance(v, bool) and v == int(v) and lo <= v <= hi


def main():
    path = sys.argv[1]
    hosts = [h.lower() for h in (sys.argv[2] if len(sys.argv) > 2 else '').split() if h]
    local = len(sys.argv) > 3 and sys.argv[3] == '1'
    plat = sys.argv[4] if len(sys.argv) > 4 and sys.argv[4] else 'linux'
    try:
        with open(path, 'rb') as f:
            data = f.read(16385)
    except OSError:
        fail('missing or too big')
    if len(data) > 16384:
        fail('missing or too big')
    try:
        text = data.decode('utf-8')
    except UnicodeDecodeError:
        fail('not UTF-8 text')
    try:
        j = json.loads(text)
    except ValueError:
        fail('not valid JSON')
    if not isinstance(j, dict):
        fail('not a JSON object')

    def url(k, u):
        if not isinstance(u, str) or len(u) > 512 or re.search(r'[\s\x00-\x1f\x7f"\'`\\]', u):
            fail(k + ': not a plain URL')
        m = re.match(r'^(https|http)://([A-Za-z0-9.-]+)(:[0-9]{1,5})?(/[^#?]*)?(\?[^#]*)?(#[A-Za-z0-9_-]*)?$', u)
        if not m:
            fail(k + ': not an http(s) URL')
        host = m.group(2).lower()
        if m.group(1) == 'http':
            if not (local and host in ('127.0.0.1', 'localhost')):
                fail(k + ': https only')
            return u
        if m.group(3):
            fail(k + ': no port numbers')
        if not any(host == h or host.endswith('.' + h) for h in hosts):
            fail(k + ': host ' + host + ' is not on the allowlist')
        return u

    out = []
    for k in ('game_url', 'pack_url', 'help_url', 'pack_help_url', 'quest_apk_url', 'quest_help_url'):
        if k in j:
            out.append(k + '\t' + url(k, j[k]))
    if 'game_min_bytes' in j:
        n = j['game_min_bytes']
        if not is_int(n, 1e6, 1e10):
            fail('game_min_bytes: not a whole number of bytes (1 MB..10 GB)')
        out.append('game_min_bytes\t%d' % n)
    if 'message' in j:
        msg = j['message']
        if not isinstance(msg, str) or len(msg) > 300 or re.search(r'[\x00-\x1f\x7f]', msg):
            fail('message: plain text of at most 300 characters')
        if msg:
            out.append('message\t' + msg)
    sha = re.compile(r'^[0-9a-f]{64}$')
    if 'pack_parts' in j:
        p = j['pack_parts']
        if not isinstance(p, list) or not 1 <= len(p) <= 8:
            fail('pack_parts: a list of 1 to 8 parts')
        for i, x in enumerate(p):
            if not isinstance(x, dict):
                fail('pack_parts[%d]: not an object' % i)
            name = x.get('name')
            if not isinstance(name, str) or not re.match(r'^[A-Za-z0-9._-]{1,80}\.zip$', name) or '..' in name:
                fail('pack_parts[%d].name' % i)
            if not is_int(x.get('bytes'), 1, 1e10):
                fail('pack_parts[%d].bytes' % i)
            if not isinstance(x.get('sha256'), str) or not sha.match(x['sha256']):
                fail('pack_parts[%d].sha256' % i)
            out.append('part\t%s\t%d\t%s' % (name, x['bytes'], x['sha256']))
    if 'quest_apk' in j:
        a = j['quest_apk']
        if not isinstance(a, dict):
            fail('quest_apk: not an object')
        if not isinstance(a.get('name'), str) or not re.match(r'^[A-Za-z0-9._-]{1,80}\.apk$', a['name']) or '..' in a['name']:
            fail('quest_apk.name')
        if not is_int(a.get('bytes'), 1, 1e10):
            fail('quest_apk.bytes')
        if not isinstance(a.get('sha256'), str) or not sha.match(a['sha256']):
            fail('quest_apk.sha256')
        if not is_int(a.get('version_code'), 1, 2100000000):
            fail('quest_apk.version_code')
        if not isinstance(a.get('version'), str) or not re.match(r'^[0-9A-Za-z.+-]{1,32}$', a['version']):
            fail('quest_apk.version')
    for k in ('adb_mac', 'adb_win'):
        if k not in j:
            continue
        a = j[k]
        if not isinstance(a, dict):
            fail(k + ': not an object')
        u = url(k + '.url', a.get('url'))
        if not u.endswith('.zip'):
            fail(k + '.url: not a zip')
        if not is_int(a.get('bytes'), 1, 1e10):
            fail(k + '.bytes')
        if not isinstance(a.get('sha256'), str) or not sha.match(a['sha256']):
            fail(k + '.sha256')
    # latest: the newest version per platform, for the "new version" notice at start-up:
    #   "latest": {"linux": {"version": "0.8.5", "url": "https://uareforged.pages.dev/downloads/"}, ...}
    # Checked as strictly as the rest, but on its own: a problem here drops only this field (one "latest_ignored" line
    # with the reason), the links above still count. The same rules as the Mac and Windows apps.
    if 'latest' in j:
        why, lv = '', j['latest']
        if not isinstance(lv, dict):
            why = 'latest: not an object'
        elif plat in lv:
            e = lv[plat]
            if not isinstance(e, dict):
                why = 'latest.' + plat + ': not an object'
            elif not isinstance(e.get('version'), str) or not VERSION.fullmatch(e['version']):
                why = 'latest.' + plat + '.version'
            elif not release_url(e.get('url')):
                why = 'latest.' + plat + '.url'
            else:
                out.append('latest\t' + e['version'] + '\t' + e['url'])
                # update (0.8.5 and later): the signed one-click update for this platform. Only its shape is checked
                # here; the launcher checks the signature, the manifest and the file. A bad one drops only the update.
                if 'update' in e:
                    uw = update_why(e['update'], local)
                    if uw:
                        out.append('latest_update_ignored\tlatest.' + plat + '.update' + uw)
                    else:
                        u = e['update']
                        out.append('latest_update\t' + u['url'] + '\t' + u['manifest'] + '\t' + u['sig'])
        if why:
            out.append('latest_ignored\t' + why)
    print('\n'.join(out))


VERSION = re.compile(r'[0-9]{1,6}(\.[0-9]{1,6}){1,3}')
RELEASE = re.compile(r'https://(github\.com/ameliaaura/uareforged|uareforged\.pages\.dev)((/[A-Za-z0-9._~-]*)*)(#[A-Za-z0-9_-]*)?')


def release_url(u):
    """The release page the notice opens: https on github.com/ameliaaura/uareforged or uareforged.pages.dev only,
    plain path characters, no "." or ".." segments, no query, no port."""
    if not isinstance(u, str) or len(u) > 300:
        return False
    m = RELEASE.fullmatch(u)
    return bool(m) and not re.search(r'/\.{1,2}(/|$)', m.group(2))


def update_why(u, local):
    """"update": {"url": "<file>", "manifest": "<base64>", "sig": "<base64>"}: the file on the release page (or on
    http://127.0.0.1 in test runs), the signed manifest and its signature. Returns '' when the shape is fine."""
    if not isinstance(u, dict):
        return ': not an object'
    url = u.get('url')
    ok = release_url(url) and re.search(r'/[A-Za-z0-9._-]+\.(zip|exe|AppImage)\Z', url) is not None
    if not ok and local and isinstance(url, str):
        ok = re.fullmatch(r'http://127\.0\.0\.1:[0-9]{1,5}(/[A-Za-z0-9._~-]+)+\.(zip|exe|AppImage)', url) is not None
    if not ok:
        return '.url'
    if not isinstance(u.get('manifest'), str) or not re.fullmatch(r'[A-Za-z0-9+/]{20,1024}={0,2}', u['manifest']):
        return '.manifest'
    if not isinstance(u.get('sig'), str) or not re.fullmatch(r'[A-Za-z0-9+/]{20,200}={0,2}', u['sig']):
        return '.sig'
    return ''


main()
