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
    ['game_url', 'pack_url', 'help_url', 'pack_help_url'].forEach(function (k) {
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
    return out.join('\n');
}
