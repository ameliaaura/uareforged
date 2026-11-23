"""Local HTTP server for the launcher download tests (download_resume_test.sh): serves the files in <dir> with Range
support and the failure modes a real connection or host can have. No network beyond 127.0.0.1.
    python3 -I dl_test_server.py <dir> <port> <log file>
Path = /<mode>/<file name>. Every request is logged as one line: <mode> <name> range=<start or -> -> <status> sent=<bytes>.
  ok        normal: 200, or 206 for a Range request; ETag = the file's size + modification time (a new version = a new tag)
  drop      every response sends at most 30 % of the file, then closes the connection (Content-Length says more)
  close     like drop, but HTTP/1.0 without Content-Length: the transfer just ends (curl sees no error, the file is short)
  stall     the first download response sends 30 %, then sends nothing more (the client must give up and carry on)
  norange   the first download drops at 30 %; then a Range request is answered with 200 and the whole file
  r416      the first download drops at 30 %; then a Range request is answered with 416
  badonce   the first download sends the file with one byte changed (right size, wrong SHA-256), then the right file
  bad       always the file with one byte changed
  dead      download requests get the headers and no data, then the connection closes (the precheck still works)
  flaky<N>  the first N download requests are like dead, then like drop (so a later try carries on and finishes)
  slow      200 KB/s (for stopping a download half way)
A download request is one that is not the launcher's 1 KB link check (Range: bytes=0-1023)."""
import http.server, os, sys, threading, time

ROOT, PORT, LOG = os.path.abspath(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
COUNT = {}
LOCK = threading.Lock()


def log(line):
    with LOCK, open(LOG, 'a') as f:
        f.write(line + '\n')


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, fmt, *a):
        pass

    def do_GET(self):
        parts = self.path.split('?')[0].strip('/').split('/')
        if len(parts) != 2:
            self.send_error(404); return
        mode, name = parts
        p = os.path.join(ROOT, name)
        if not os.path.isfile(p):
            log('%s %s range=- -> 404 sent=0' % (mode, name)); self.send_error(404); return
        with open(p, 'rb') as f:
            data = f.read()
        size = len(data); st = os.stat(p)
        tag = '"%x-%x"' % (size, int(st.st_mtime_ns))
        rng = self.headers.get('Range') or ''
        check = rng == 'bytes=0-1023'
        n = 0
        if not check:
            with LOCK:
                n = COUNT.get((mode, name), 0) + 1; COUNT[(mode, name)] = n
        start, end = 0, size - 1
        if rng.startswith('bytes='):
            a, _, b = rng[6:].partition('-')
            start = int(a) if a else 0
            end = min(int(b), size - 1) if b else size - 1
        if mode in ('bad', 'badonce') and not check and (mode == 'bad' or n == 1):
            data = bytearray(data); data[size // 2] ^= 0xFF; data = bytes(data)
        cut = None
        if not check:
            if mode in ('drop', 'close'):
                cut = int(size * 0.3)
            elif mode in ('stall', 'norange', 'r416') and n == 1:
                cut = int(size * 0.3)
            elif mode == 'dead':
                cut = 0
            elif mode.startswith('flaky'):
                cut = 0 if n <= int(mode[5:] or 0) else int(size * 0.3)
        if rng and not check and start > 0 and mode == 'r416':
            self.send_response(416); self.send_header('Content-Range', 'bytes */%d' % size)
            self.send_header('Content-Length', '0'); self.end_headers()
            log('%s %s range=%d -> 416 sent=0' % (mode, name, start)); return
        if rng and start >= size:
            self.send_response(416); self.send_header('Content-Range', 'bytes */%d' % size)
            self.send_header('Content-Length', '0'); self.end_headers()
            log('%s %s range=%d -> 416 sent=0' % (mode, name, start)); return
        partial = bool(rng) and not (mode == 'norange' and not check and start > 0)
        if mode == 'close' and not check:
            self.protocol_version = 'HTTP/1.0'
        if partial:
            self.send_response(206); self.send_header('Content-Range', 'bytes %d-%d/%d' % (start, end, size))
        else:
            start, end = 0, size - 1
            self.send_response(200)
        self.send_header('Accept-Ranges', 'bytes'); self.send_header('ETag', tag)
        self.send_header('Content-Type', 'application/zip')
        if not (mode == 'close' and not check):
            self.send_header('Content-Length', str(end - start + 1))
        self.send_header('Connection', 'close')
        self.end_headers()
        body = data[start:end + 1]
        if cut is not None:
            body = body[:cut]
        sent = 0
        try:
            if mode == 'slow' and not check:
                for i in range(0, len(body), 20000):
                    self.wfile.write(body[i:i + 20000]); self.wfile.flush(); sent += len(body[i:i + 20000]); time.sleep(0.1)
            else:
                self.wfile.write(body); self.wfile.flush(); sent = len(body)
            if mode == 'stall' and n == 1 and not check:
                log('%s %s range=%s -> %d sent=%d (stalling)' % (mode, name, start if rng else '-', 206 if partial else 200, sent))
                time.sleep(3600)
        except (BrokenPipeError, ConnectionResetError):
            pass
        log('%s %s range=%s -> %d sent=%d' % (mode, name, start if rng else '-', 206 if partial else 200, sent))
        self.close_connection = True


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def server_bind(self):   # no reverse-DNS lookup
        import socketserver
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = 'localhost', self.server_address[1]


Server(('127.0.0.1', PORT), H).serve_forever()
