#!/usr/bin/env python3
"""Test vectors for libweb's inflate (tests/web/inflatetest.c): data
compressed by Python's zlib and gzip modules in every mode, plus corrupt and
truncated streams that must fail cleanly.

    gen_inflate.py OUTDIR

Writes NNN.in (the compressed bytes), NNN.out (the expected output, absent
for streams that must fail) and NNN.txt ("raw|zlib|gzip ok|fail name")."""
import gzip
import io
import os
import random
import sys
import zlib

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
rnd = random.Random(1234)
n = 0


def case(kind, data_in, expect, name):
    global n
    with open(os.path.join(out, "%03d.in" % n), "wb") as f:
        f.write(data_in)
    if expect is not None:
        with open(os.path.join(out, "%03d.out" % n), "wb") as f:
            f.write(expect)
    with open(os.path.join(out, "%03d.txt" % n), "w") as f:
        f.write("%s %s %s\n" % (kind, "ok" if expect is not None else "fail", name))
    n += 1


def raw(data, level=6, strategy=zlib.Z_DEFAULT_STRATEGY, wbits=-15, memlevel=8):
    c = zlib.compressobj(level, zlib.DEFLATED, wbits, memlevel, strategy)
    return c.compress(data) + c.flush()


samples = {
    "empty": b"",
    "one": b"a",
    "text": (b"The quick brown fox jumps over the lazy dog. " * 200),
    "html": open(__file__, "rb").read() * 3,
    "random": bytes(rnd.getrandbits(8) for _ in range(70000)),
    "zeros": bytes(300000),
    "runs": b"".join(bytes([rnd.randrange(4)]) * rnd.randrange(1, 300) for _ in range(2000)),
    "binary": bytes(range(256)) * 300,
}

for name, data in samples.items():
    for level in (0, 1, 6, 9):
        case("raw", raw(data, level), data, "%s-raw-l%d" % (name, level))
        case("zlib", zlib.compress(data, level), data, "%s-zlib-l%d" % (name, level))
    case("raw", raw(data, 6, zlib.Z_FIXED), data, name + "-fixed")
    case("raw", raw(data, 6, zlib.Z_HUFFMAN_ONLY), data, name + "-huffman")
    case("raw", raw(data, 6, zlib.Z_RLE), data, name + "-rle")
    case("raw", raw(data, 9, wbits=-9, memlevel=1), data, name + "-smallwindow")
    case("gzip", gzip.compress(data), data, name + "-gzip")

# gzip with header fields (name, comment, extra, header crc) via a hand-built header
body = samples["text"]
comp = raw(body, 9)
trailer = zlib.crc32(body).to_bytes(4, "little") + len(body).to_bytes(4, "little")
hdr = bytes([0x1F, 0x8B, 8, 4 | 8 | 16 | 2, 0, 0, 0, 0, 0, 255])
extra = b"AB\x02\x00xy"
fields = len(extra).to_bytes(2, "little") + extra + b"name.txt\0" + b"a comment\0"
hcrc = (zlib.crc32(hdr + fields) & 0xFFFF).to_bytes(2, "little")
case("gzip", hdr + fields + hcrc + comp + trailer, body, "gzip-allfields")
# two members (concatenated, as gzip -c a b)
two = gzip.compress(b"first part\n") + gzip.compress(b"second part\n")
case("gzip", two, b"first part\nsecond part\n", "gzip-two-members")
# trailing garbage after a member is ignored
case("gzip", gzip.compress(b"hello") + b"\0\0\0", b"hello", "gzip-trailing-zeros")
# a stream made with flushes (several blocks, stored/sync points)
c = zlib.compressobj(6, zlib.DEFLATED, -15)
fl = b""
parts = []
for i in range(20):
    p = (b"block %d " % i) * (i * 50 + 1)
    parts.append(p)
    fl += c.compress(p) + c.flush(zlib.Z_SYNC_FLUSH if i % 2 else zlib.Z_FULL_FLUSH)
fl += c.flush()
case("raw", fl, b"".join(parts), "flushes")

# ---- streams that must fail ----
good = zlib.compress(samples["text"], 9)
case("zlib", good[:-1], None, "zlib-truncated-adler")
case("zlib", good[:len(good) // 2], None, "zlib-truncated-middle")
case("zlib", good[:2], None, "zlib-header-only")
case("zlib", good[:-4] + bytes(4), None, "zlib-bad-adler")
case("zlib", bytes([0x78, 0x9D]) + good[2:], None, "zlib-bad-header-check")
case("zlib", bytes([0x79, 0x9C]) + good[2:], None, "zlib-bad-method")
case("zlib", bytes([0x78, 0xBB]) + good[2:], None, "zlib-preset-dictionary")
g = gzip.compress(samples["html"])
case("gzip", g[:-3], None, "gzip-truncated-trailer")
case("gzip", g[:-8] + bytes(8), None, "gzip-bad-crc")
case("gzip", g[:-4] + (12345).to_bytes(4, "little"), None, "gzip-bad-size")
case("gzip", b"\x1f\x8b\x09" + g[3:], None, "gzip-bad-method")
case("gzip", b"\x1f\x8b\x08\xe0" + g[4:], None, "gzip-reserved-flags")
case("gzip", b"\x1f\x8b\x08\x08" + g[4:10] + b"unterminated", None, "gzip-unterminated-name")
case("gzip", b"plain text", None, "gzip-not-gzip")
case("raw", bytes([0x07]), None, "raw-block-type-3")
case("raw", bytes([0x01, 0x05, 0x00, 0xFA, 0xFF]) + b"abc", None, "raw-stored-truncated")
case("raw", bytes([0x01, 0x05, 0x00, 0x00, 0x00]) + b"abcde", None, "raw-stored-bad-nlen")
case("raw", b"", None, "raw-empty-input")
# a fixed block with a distance before the start: literal 'a' then a copy of distance 2
# fixed codes: 'a' = 0x61 -> 8-bit code 0x30+0x61; length 3 = symbol 257 (7 bits 0000001), distance code 1 (5 bits)
def bits_writer():
    acc = [0, 0, bytearray()]

    def put(v, nbits, msb_first=False):
        if msb_first:
            v = int("{:0{}b}".format(v, nbits)[::-1], 2)
        for i in range(nbits):
            acc[0] |= ((v >> i) & 1) << acc[1]
            acc[1] += 1
            if acc[1] == 8:
                acc[2].append(acc[0])
                acc[0] = 0
                acc[1] = 0

    def done():
        if acc[1]:
            acc[2].append(acc[0])
        return bytes(acc[2])
    return put, done


put, done = bits_writer()
put(1, 1); put(1, 2)                    # last block, fixed
put(0x30 + 0x61, 8, True)               # 'a'
put(1, 7, True)                         # length 3
put(1, 5, True)                         # distance 2 (code 1): only 1 byte of output so far
put(0, 7, True)                         # end of block
case("raw", done(), None, "raw-distance-too-far")
put, done = bits_writer()
put(1, 1); put(1, 2)
put(0x30 + 0x61, 8, True)
put(0b11000110, 8, True)                # symbol 286 (invalid length code) = 8-bit 0xC0 + 6
case("raw", done(), None, "raw-length-symbol-286")
put, done = bits_writer()
put(1, 1); put(1, 2)
put(0x30 + 0x61, 8, True)
put(1, 7, True)
put(30, 5, True)                        # distance symbol 30 (invalid)
case("raw", done(), None, "raw-distance-symbol-30")
put, done = bits_writer()
put(1, 1); put(1, 2)
put(0x30 + 0x61, 8, True)               # no end of block: input ends
case("raw", done(), None, "raw-no-end-of-block")
# dynamic block with an over-subscribed code-length code: HCLEN=19, all lengths 1
put, done = bits_writer()
put(1, 1); put(2, 2); put(0, 5); put(0, 5); put(15, 4)
for i in range(19):
    put(1, 3)
case("raw", done() + bytes(10), None, "raw-oversubscribed")
# dynamic block whose first code-length symbol is 16 (repeat with nothing before)
put, done = bits_writer()
put(1, 1); put(2, 2); put(0, 5); put(0, 5); put(0, 4)    # 4 code length codes: 16, 17, 18, 0
put(1, 3); put(1, 3); put(0, 3); put(0, 3)              # 16 and 17 get 1-bit codes (complete)
put(0, 1); put(0, 2)                                    # symbol 16 (code 0) + 2 bits
case("raw", done() + bytes(4), None, "raw-repeat-first")
# a "bomb": 200 MB of zeros in about 200 KB; must stop at the caller's limit
case("zlib", zlib.compress(bytes(200 << 20), 9), None, "zeros-bomb")
case("gzip", gzip.compress(bytes(200 << 20), 9), None, "gzip-bomb")
# random corruptions of a good stream (single bit flips): only checked for
# not crashing and staying in the limits (inflatetest decides), so "any"
for i in range(40):
    b = bytearray(raw(samples["html"], 6))
    k = rnd.randrange(len(b))
    b[k] ^= 1 << rnd.randrange(8)
    case("raw", bytes(b), None, "flip-%d-any" % i)
print("%d inflate test vectors in %s" % (n, out))
