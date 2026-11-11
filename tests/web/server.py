#!/usr/bin/env python3
"""Test web server for tests/web/run.sh: plain HTTP and HTTPS (one port per
certificate) with good and deliberately broken answers.

    server.py PORTFILE plain-port [https-port:certfile:keyfile[:tls13]]...

Writes "ready" to PORTFILE when all ports listen."""
import gzip
import http.server
import socketserver
import ssl
import sys
import threading
import time
import zlib

BODY = b"<html><head><title>Hello</title></head><body><p>Hello, Halcyon!</p></body></html>\n"


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def raw(self, data):
        self.wfile.write(data)
        self.wfile.flush()

    def send(self, code, body, ctype="text/html; charset=utf-8", extra=(), length=True):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        for k, v in extra:
            self.send_header(k, v)
        if length:
            self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def do_HEAD(self):
        self.do_GET()

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        data = self.rfile.read(n)
        if self.path.startswith("/redirect303"):
            self.send(303, b"", extra=[("Location", "/echo-method")])
            return
        self.send(200, b"method=POST type=" + self.headers.get("Content-Type", "").encode() + b" body=" + data,
                  "text/plain")

    def do_GET(self):
        p = self.path
        ae = self.headers.get("Accept-Encoding", "")
        if p == "/hello":
            self.send(200, BODY)
        elif p == "/ua":
            self.send(200, ("ua=%s host=%s" % (self.headers.get("User-Agent"), self.headers.get("Host"))).encode(), "text/plain")
        elif p == "/echo-method":
            self.send(200, ("method=%s" % self.command).encode(), "text/plain")
        elif p == "/gzip":
            if "gzip" not in ae:
                self.send(500, b"no gzip accepted")
                return
            self.send(200, gzip.compress(BODY * 100), extra=[("Content-Encoding", "gzip")])
        elif p == "/deflate":
            self.send(200, zlib.compress(BODY * 50), extra=[("Content-Encoding", "deflate")])
        elif p == "/rawdeflate":
            c = zlib.compressobj(6, zlib.DEFLATED, -15)
            self.send(200, c.compress(BODY * 50) + c.flush(), extra=[("Content-Encoding", "deflate")])
        elif p == "/badgzip":
            g = bytearray(gzip.compress(BODY * 100))
            g[len(g) // 2] ^= 0x55
            self.send(200, bytes(g), extra=[("Content-Encoding", "gzip")])
        elif p == "/gzipbomb":
            self.send(200, gzip.compress(bytes(64 << 20), 9), "application/octet-stream", extra=[("Content-Encoding", "gzip")])
        elif p == "/chunked":
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for part in (b"Hello, ", b"chunked ", b"world!" * 1000):
                self.raw(b"%x;ext=1\r\n%s\r\n" % (len(part), part))
            self.raw(b"0\r\nX-Trailer: yes\r\n\r\n")
        elif p == "/chunkedgzip":
            data = gzip.compress(BODY * 30)
            self.send_response(200)
            self.send_header("Content-Encoding", "gzip")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for i in range(0, len(data), 100):
                self.raw(b"%x\r\n%s\r\n" % (len(data[i:i + 100]), data[i:i + 100]))
            self.raw(b"0\r\n\r\n")
        elif p.startswith("/badchunk"):
            self.raw(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n")
            kind = p[len("/badchunk"):]
            if kind == "1":
                self.raw(b"zz\r\nhello\r\n0\r\n\r\n")
            elif kind == "2":
                self.raw(b"5\r\nhelloXX0\r\n\r\n")
            elif kind == "3":
                self.raw(b"ffffffffffffffffff\r\n")
            else:
                self.raw(b"5\r\nhel")
            self.close_connection = True
        elif p == "/truncated":
            self.raw(b"HTTP/1.1 200 OK\r\nContent-Length: 1000\r\nContent-Type: text/plain\r\n\r\nonly a few bytes")
            self.close_connection = True
        elif p == "/close":
            self.raw(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n" + b"until the end. " * 500)
            self.close_connection = True
        elif p == "/http10":
            self.raw(b"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nold server")
            self.close_connection = True
        elif p == "/hugeheader":
            self.raw(b"HTTP/1.1 200 OK\r\nX-Big: " + b"a" * 20000 + b"\r\nContent-Length: 2\r\n\r\nok")
            self.close_connection = True
        elif p == "/manyheaders":
            self.raw(b"HTTP/1.1 200 OK\r\n" + b"".join(b"X-H%d: %d\r\n" % (i, i) for i in range(1000)) + b"Content-Length: 2\r\n\r\nok")
            self.close_connection = True
        elif p == "/folded":
            self.raw(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain;\r\n charset=latin-1\r\nContent-Length: 2\r\n\r\nok")
        elif p == "/badlength":
            self.raw(b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 7\r\n\r\nhello")
            self.close_connection = True
        elif p == "/notalength":
            self.raw(b"HTTP/1.1 200 OK\r\nContent-Length: -5\r\n\r\nhello")
            self.close_connection = True
        elif p == "/garbage":
            self.raw(b"SSH-2.0-OpenSSH\r\n\r\n")
            self.close_connection = True
        elif p == "/nothing":
            self.close_connection = True
        elif p == "/continue":
            self.raw(b"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: </x>\r\n\r\n"
                     b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nDONE")
        elif p.startswith("/redirect/"):
            n = int(p.split("/")[2])
            if n <= 0:
                self.send(200, b"arrived", "text/plain")
            else:
                self.send(302, b"moved", extra=[("Location", "/redirect/%d" % (n - 1))])
        elif p == "/loop":
            self.send(301, b"", extra=[("Location", "/loop")])
        elif p == "/abs-redirect":
            self.send(307, b"", extra=[("Location", "http://127.0.0.1:%d/hello" % PLAIN)])
        elif p == "/setcookie":
            self.send(200, b"set", "text/plain", extra=[("Set-Cookie", "sid=abc123; Path=/"),
                                                         ("Set-Cookie", "theme=blue; Max-Age=3600"),
                                                         ("Set-Cookie", "gone=1; Max-Age=0"),
                                                         ("Set-Cookie", "evil=1; Domain=example.com")])
        elif p == "/getcookie":
            self.send(200, ("cookie=%s" % self.headers.get("Cookie")).encode(), "text/plain")
        elif p == "/slow":
            time.sleep(3)
            self.send(200, b"late")
        elif p == "/slowbody":
            self.raw(b"HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nabc")
            time.sleep(4)
            self.close_connection = True
        elif p == "/big":
            self.send(200, b"x" * (3 << 20), "application/octet-stream")
        elif p == "/latin1":
            self.send(200, "<p>caf\xe9</p>".encode("latin-1"), "text/html; charset=ISO-8859-1")
        elif p == "/download":
            self.send(200, bytes(range(256)) * 40, "application/octet-stream",
                      extra=[("Content-Disposition", 'attachment; filename="data/../file.bin"')])
        else:
            self.send(404, b"<h1>Not found</h1>")


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def handle_error(self, request, client_address):
        pass                        # clients that give up early (timeouts, cancels) are expected


def serve(port, ctx=None):
    s = Server(("127.0.0.1", port), H)
    if ctx:
        s.socket = ctx.wrap_socket(s.socket, server_side=True)
    t = threading.Thread(target=s.serve_forever, daemon=True)
    t.start()
    return s


PLAIN = int(sys.argv[2])
servers = [serve(PLAIN)]
for spec in sys.argv[3:]:
    parts = spec.split(":")
    port, cert, key = int(parts[0]), parts[1], parts[2]
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    if len(parts) > 3 and parts[3] == "tls13":
        ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    servers.append(serve(port, ctx))
with open(sys.argv[1], "w") as f:
    f.write("ready\n")
while True:
    time.sleep(3600)
