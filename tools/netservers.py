#!/usr/bin/env python3
"""Host-side servers for the QEMU network test (boottest.py's `net` variant).

QEMU's user network reaches the host's 127.0.0.1 as 10.0.2.2. Every server
binds 127.0.0.1 on a free port; the guest learns the ports from NETTEST.EXE's
arguments and the DNS server's from net.dns in SETTINGS.INI.

  DNS    UDP; a small zone under halcyon.test (A, CNAME in the answer, a CNAME
         chain that needs a second query, three addresses, NXDOMAIN with an
         SOA, a name that is never answered, SERVFAIL, and a forged answer
         with a wrong id sent before the real one)
  HTTP   GET /big/N (N MiB of random bytes), /small/I (20000 + 100 I bytes);
         Content-Length and X-CRC32 (hex) headers
  sink   TCP: reads until end of stream, answers "LENGTH CRC32HEX\\n"
  echo   UDP: sends every datagram back

usage: tools/netservers.py   (starts them and prints the ports, for manual tests)
"""
import http.server
import os
import random
import socket
import socketserver
import struct
import sys
import threading
import time
import zlib

ZONE_A = {
    "files.halcyon.test": ["10.0.2.2"],
    "far.halcyon.test": ["192.0.2.7"],
    "multi.halcyon.test": ["192.0.2.1", "192.0.2.2", "192.0.2.3"],
    "spoof.halcyon.test": ["10.0.2.2"],
}
ZONE_CNAME = {
    "alias.halcyon.test": "files.halcyon.test",
    "chain.halcyon.test": "far.halcyon.test",
}


def _qname(data, off):
    labels = []
    while True:
        n = data[off]
        off += 1
        if n == 0:
            return ".".join(labels), off
        labels.append(data[off:off + n].decode("ascii", "replace"))
        off += n


def _name(n):
    out = b""
    for part in n.split("."):
        out += bytes([len(part)]) + part.encode()
    return out + b"\0"


def _rr(name, rtype, ttl, rdata):
    return _name(name) + struct.pack(">HHIH", rtype, 1, ttl, len(rdata)) + rdata


class DNSServer(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.port = self.sock.getsockname()[1]
        self.queries = {}

    def answer(self, data):
        qid, flags, qd = struct.unpack(">HHH", data[:6])
        name, off = _qname(data, 12)
        qtype, qclass = struct.unpack(">HH", data[off:off + 4])
        question = data[12:off + 4]
        key = name.lower()
        self.queries[key] = self.queries.get(key, 0) + 1
        if key == "slow.halcyon.test":
            return []
        rcode, answers, auth = 0, [], []
        if key == "fail.halcyon.test":
            rcode = 2
        elif key in ZONE_CNAME:
            target = ZONE_CNAME[key]
            answers.append(_rr(key, 5, 60, _name(target)))
            if key == "alias.halcyon.test":
                answers += [_rr(target, 1, 120, socket.inet_aton(a)) for a in ZONE_A[target]]
        elif key in ZONE_A:
            answers += [_rr(key, 1, 300, socket.inet_aton(a)) for a in ZONE_A[key]]
        else:
            rcode = 3
            soa = _name("ns.halcyon.test") + _name("admin.halcyon.test") + struct.pack(">IIIII", 1, 3600, 600, 86400, 60)
            auth.append(_rr("halcyon.test", 6, 120, soa))
        hdr = struct.pack(">HHHHHH", qid, 0x8180 | rcode, 1, len(answers), len(auth), 0)
        reply = hdr + question + b"".join(answers) + b"".join(auth)
        out = []
        if key == "spoof.halcyon.test":     # a forged answer first: wrong id, another address
            forged = struct.pack(">HHHHHH", qid ^ 0x5555, 0x8180, 1, 1, 0, 0) + question + \
                _rr(key, 1, 300, socket.inet_aton("6.6.6.6"))
            out.append(forged)
        out.append(reply)
        return out

    def run(self):
        while True:
            data, addr = self.sock.recvfrom(4096)
            try:
                for r in self.answer(data):
                    self.sock.sendto(r, addr)
            except (IndexError, struct.error):
                pass


_blobs = {}
_blob_lock = threading.Lock()


def blob(kind, n):
    with _blob_lock:
        if (kind, n) not in _blobs:
            if kind == "big":
                data = os.urandom(n << 20)
            else:
                data = random.Random(n).randbytes(20000 + 100 * n)
            _blobs[(kind, n)] = (data, zlib.crc32(data))
        return _blobs[(kind, n)]


class HTTPHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, *a):
        pass

    def do_GET(self):
        parts = self.path.strip("/").split("/")
        try:
            kind, n = parts[0], int(parts[1])
            if kind not in ("big", "small") or n < 0 or n > 512:
                raise ValueError
        except (IndexError, ValueError):
            self.send_error(404)
            return
        data, crc = blob(kind, n)
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("X-CRC32", "%08x" % crc)
        self.end_headers()
        self.wfile.write(data)


class ThreadingHTTP(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    request_queue_size = 128


class Sink(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(16)
        self.port = self.sock.getsockname()[1]

    def serve(self, c):
        total, crc = 0, 0
        with c:
            while True:
                d = c.recv(1 << 16)
                if not d:
                    break
                total += len(d)
                crc = zlib.crc32(d, crc)
            c.sendall(b"%d %08x\n" % (total, crc))

    def run(self):
        while True:
            c, _ = self.sock.accept()
            threading.Thread(target=self.serve, args=(c,), daemon=True).start()


class Echo(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.port = self.sock.getsockname()[1]

    def run(self):
        while True:
            d, a = self.sock.recvfrom(65535)
            self.sock.sendto(d, a)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def start():
    """start every server; returns a dict of ports"""
    dns, sink, echo = DNSServer(), Sink(), Echo()
    for t in (dns, sink, echo):
        t.start()
    httpd = ThreadingHTTP(("127.0.0.1", 0), HTTPHandler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    blob("big", 32)                 # (made before the guest asks)
    return {"dns": dns.port, "http": httpd.server_address[1], "sink": sink.port, "echo": echo.port,
            "dns_server": dns}


def client_test(port, log, start, timeout=120):
    """wait for NETTEST's server line in the serial log, then connect through
    QEMU's host forwarding, send 1 MiB and check the answer; returns an error
    string or None"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            text = open(log, errors="replace").read()
        except OSError:
            text = ""
        if "nettest: server ready" in text[start:]:
            break
        if "nettest: all" in text or "checks FAILED" in text:
            return "nettest ended before its server was ready"
        time.sleep(0.25)
    else:
        return "nettest's server did not start"
    data = os.urandom(1 << 20)
    try:
        c = socket.create_connection(("127.0.0.1", port), timeout=20)
        c.sendall(data)
        c.shutdown(socket.SHUT_WR)
        reply = b""
        while True:
            d = c.recv(256)
            if not d:
                break
            reply += d
        c.close()
    except OSError as e:
        return "host client: %s" % e
    want = b"OK %d %08x\n" % (len(data), zlib.crc32(data))
    if reply != want:
        return "host client: answer %r, want %r" % (reply, want)
    return None


if __name__ == "__main__":
    p = start()
    del p["dns_server"]
    print(p, flush=True)
    while True:
        time.sleep(3600)
