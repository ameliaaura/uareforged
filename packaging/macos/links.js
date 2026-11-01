// UA Reforged - strict check of the site's links.json (JavaScript for Automation, macOS built-in).
//   osascript -l JavaScript links.js <file> "<allowed host> ..." <allow local http 0|1>
// Only reads and checks data: JSON.parse, types, lengths, https URLs on an allowlist of hosts. Nothing in the file
// is ever run. Prints "key<TAB>value" lines (parts as "part<TAB>name<TAB>bytes<TAB>sha256"); any problem rejects
// the WHOLE file (exit 1, the reason on stderr) and the app uses its built-in links.
ObjC.import('Foundation');
ObjC.import('stdlib');

function fail(why) {
    var err = $.NSFileHandle.fileHandleWithStandardError;
    err.writeData($('links.json: ' + why + '\n').dataUsingEncoding($.NSUTF8StringEncoding));
    $.exit(1);
}

function run(argv) {
    var path = argv[0], hosts = (argv[1] || '').split(/\s+/).filter(function (h) { return h; }), local = argv[2] === '1';
    var data = $.NSData.dataWithContentsOfFile(path);
    if (data.isNil() || data.length > 16384) fail('missing or too big');
    var text = ObjC.unwrap($.NSString.alloc.initWithDataEncoding(data, $.NSUTF8StringEncoding));
    if (typeof text !== 'string') fail('not UTF-8 text');
    var j;
    try { j = JSON.parse(text); } catch (e) { fail('not valid JSON'); }
    if (j === null || typeof j !== 'object' || Array.isArray(j)) fail('not a JSON object');

    function url(k) {
        var u = j[k];
        if (typeof u !== 'string' || u.length > 512 || /[\s\u0000-\u001f\u007f"'`\\]/.test(u)) fail(k + ': not a plain URL');
        var m = /^(https|http):\/\/([A-Za-z0-9.-]+)(:[0-9]{1,5})?(\/[^#?]*)?(\?[^#]*)?(#[A-Za-z0-9_-]*)?$/.exec(u);
        if (!m) fail(k + ': not an http(s) URL');
        var host = m[2].toLowerCase();
        if (m[1] === 'http') {
            if (!(local && (host === '127.0.0.1' || host === 'localhost'))) fail(k + ': https only');
            return u;
        }
        if (m[3]) fail(k + ': no port numbers');
        var ok = hosts.some(function (h) { h = h.toLowerCase(); return host === h || host.slice(-h.length - 1) === '.' + h; });
        if (!ok) fail(k + ': host ' + host + ' is not on the allowlist');
        return u;
    }

    var out = [];
    ['game_url', 'pack_url', 'help_url', 'pack_help_url', 'quest_apk_url', 'quest_help_url'].forEach(function (k) {
        if (k in j) out.push(k + '\t' + url(k));
    });
    if ('game_min_bytes' in j) {
        var n = j.game_min_bytes;
        if (typeof n !== 'number' || n % 1 !== 0 || n < 1e6 || n > 1e10) fail('game_min_bytes: not a whole number of bytes (1 MB..10 GB)');
        out.push('game_min_bytes\t' + n);
    }
    if ('message' in j) {
        var msg = j.message;
        if (typeof msg !== 'string' || msg.length > 300 || /[\u0000-\u001f\u007f]/.test(msg)) fail('message: plain text of at most 300 characters');
        if (msg) out.push('message\t' + msg);
    }
    if ('pack_parts' in j) {
        var p = j.pack_parts;
        if (!Array.isArray(p) || p.length < 1 || p.length > 8) fail('pack_parts: a list of 1 to 8 parts');
        p.forEach(function (x, i) {
            if (x === null || typeof x !== 'object' || Array.isArray(x)) fail('pack_parts[' + i + ']: not an object');
            if (typeof x.name !== 'string' || !/^[A-Za-z0-9._-]{1,80}\.zip$/.test(x.name) || x.name.indexOf('..') >= 0) fail('pack_parts[' + i + '].name');
            if (typeof x.bytes !== 'number' || x.bytes % 1 !== 0 || x.bytes < 1 || x.bytes > 1e10) fail('pack_parts[' + i + '].bytes');
            if (typeof x.sha256 !== 'string' || !/^[0-9a-f]{64}$/.test(x.sha256)) fail('pack_parts[' + i + '].sha256');
            out.push('part\t' + x.name + '\t' + x.bytes + '\t' + x.sha256);
        });
    }
    function sha(v) { return typeof v === 'string' && /^[0-9a-f]{64}$/.test(v); }
    function bytes(v) { return typeof v === 'number' && v % 1 === 0 && v >= 1 && v <= 1e10; }
    if ('quest_apk' in j) {
        var a = j.quest_apk;
        if (a === null || typeof a !== 'object' || Array.isArray(a)) fail('quest_apk: not an object');
        if (typeof a.name !== 'string' || !/^[A-Za-z0-9._-]{1,80}\.apk$/.test(a.name) || a.name.indexOf('..') >= 0) fail('quest_apk.name');
        if (!bytes(a.bytes)) fail('quest_apk.bytes');
        if (!sha(a.sha256)) fail('quest_apk.sha256');
        if (typeof a.version_code !== 'number' || a.version_code % 1 !== 0 || a.version_code < 1 || a.version_code > 2100000000) fail('quest_apk.version_code');
        if (typeof a.version !== 'string' || !/^[0-9A-Za-z.+-]{1,32}$/.test(a.version)) fail('quest_apk.version');
        out.push('quest_apk\t' + a.name + '\t' + a.bytes + '\t' + a.sha256 + '\t' + a.version_code + '\t' + a.version);
    }
    ['adb_mac', 'adb_win'].forEach(function (k) {
        if (!(k in j)) return;
        var a = j[k];
        if (a === null || typeof a !== 'object' || Array.isArray(a)) fail(k + ': not an object');
        j[k + '.url'] = a.url;
        var u = url(k + '.url');
        if (!/\.zip$/.test(u)) fail(k + '.url: not a zip');
        if (!bytes(a.bytes)) fail(k + '.bytes');
        if (!sha(a.sha256)) fail(k + '.sha256');
        out.push(k + '\t' + u + '\t' + a.bytes + '\t' + a.sha256);
    });
    return out.join('\n');
}
