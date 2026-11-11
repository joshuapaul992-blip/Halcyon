#!/usr/bin/env python3
"""Convert a PEM bundle of CA certificates (Mozilla's, as published by curl:
userland/third_party/cacert/cacert.pem) into BearSSL trust anchors, as C.

    python3 tools/mktrust.py cacert.pem out.c [symbol]

For each certificate the output has the subject's distinguished name (the raw
DER of the Name, which BearSSL compares byte for byte with an issuer name),
the public key (RSA modulus and exponent, or the curve and point of an EC key)
and BR_X509_TA_CA. The bundle's "##" header (where it comes from, its date)
is kept as a comment. Certificates with other key types are skipped with a
note. The DER parser below is our own and checks every length."""
import base64
import re
import sys

OID_RSA = "1.2.840.113549.1.1.1"
OID_EC = "1.2.840.10045.2.1"
CURVES = {"1.2.840.10045.3.1.7": ("BR_EC_secp256r1", 65),
          "1.3.132.0.34": ("BR_EC_secp384r1", 97),
          "1.3.132.0.35": ("BR_EC_secp521r1", 133)}
OID_BASIC_CONSTRAINTS = "2.5.29.19"


class DerError(Exception):
    pass


def tlv(buf, pos, end):
    """one DER element at pos: (tag, start of contents, end of contents)"""
    if pos + 2 > end:
        raise DerError("truncated element")
    tag = buf[pos]
    if tag & 0x1F == 0x1F:
        raise DerError("multi-byte tag")
    n = buf[pos + 1]
    p = pos + 2
    if n & 0x80:
        k = n & 0x7F
        if k == 0 or k > 4 or p + k > end:
            raise DerError("bad length")
        n = int.from_bytes(buf[p:p + k], "big")
        if n < 0x80 or (k > 1 and buf[p] == 0):
            raise DerError("length not minimal")
        p += k
    if p + n > end:
        raise DerError("element longer than its container")
    return tag, p, p + n


def children(buf, start, end):
    out = []
    pos = start
    while pos < end:
        t, s, e = tlv(buf, pos, end)
        out.append((t, s, e, pos))
        pos = e
    return out


def oid_str(b):
    if not b:
        raise DerError("empty OID")
    first = b[0]
    parts = [min(first // 40, 2), first - 40 * min(first // 40, 2)]
    v = 0
    for c in b[1:]:
        v = (v << 7) | (c & 0x7F)
        if not c & 0x80:
            parts.append(v)
            v = 0
    return ".".join(map(str, parts))


def parse_cert(der):
    t, s, e = tlv(der, 0, len(der))
    if t != 0x30 or e != len(der):
        raise DerError("not a certificate")
    cert = children(der, s, e)
    if len(cert) != 3 or cert[0][0] != 0x30:
        raise DerError("bad certificate structure")
    tbs = children(der, cert[0][1], cert[0][2])
    i = 0
    if tbs[0][0] == 0xA0:          # [0] version
        i = 1
    # serial, signature algorithm, issuer, validity, subject, subjectPublicKeyInfo
    if len(tbs) < i + 6:
        raise DerError("short TBSCertificate")
    subj = tbs[i + 4]
    if subj[0] != 0x30:
        raise DerError("bad subject")
    dn = bytes(der[subj[3]:subj[2]])
    spki = tbs[i + 5]
    alg, bits = children(der, spki[1], spki[2])
    algp = children(der, alg[1], alg[2])
    oid = oid_str(der[algp[0][1]:algp[0][2]])
    if bits[0] != 0x03 or der[bits[1]] != 0:
        raise DerError("bad public key bit string")
    key = bytes(der[bits[1] + 1:bits[2]])
    is_ca = None
    for ext_t, ext_s, ext_e, _ in tbs[i + 6:]:
        if ext_t != 0xA3:
            continue
        seq = children(der, ext_s, ext_e)
        for _, xs, xe, _ in children(der, seq[0][1], seq[0][2]):
            parts = children(der, xs, xe)
            if oid_str(der[parts[0][1]:parts[0][2]]) != OID_BASIC_CONSTRAINTS:
                continue
            val = parts[-1]
            bc = children(der, val[1], val[2])
            inner = children(der, bc[0][1], bc[0][2])
            is_ca = bool(inner) and inner[0][0] == 0x01 and der[inner[0][1]] != 0
    if oid == OID_RSA:
        t, s, e = tlv(key, 0, len(key))
        nn, ee = children(key, s, e)
        if nn[0] != 0x02 or ee[0] != 0x02:
            raise DerError("bad RSA key")
        n = key[nn[1]:nn[2]].lstrip(b"\0")
        x = key[ee[1]:ee[2]].lstrip(b"\0")
        return dn, ("RSA", n, x), is_ca
    if oid == OID_EC:
        curve = oid_str(der[algp[1][1]:algp[1][2]]) if len(algp) > 1 and algp[1][0] == 0x06 else None
        if curve not in CURVES:
            return dn, None, is_ca
        name, qlen = CURVES[curve]
        if len(key) != qlen or key[0] != 4:
            raise DerError("bad EC point")
        return dn, ("EC", name, key), is_ca
    return dn, None, is_ca


def c_bytes(b):
    lines = []
    for i in range(0, len(b), 16):
        lines.append("\t" + " ".join("0x%02X," % c for c in b[i:i + 16]))
    return "\n".join(lines)


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: mktrust.py bundle.pem out.c [symbol]")
    src, out = sys.argv[1], sys.argv[2]
    sym = sys.argv[3] if len(sys.argv) > 3 else "web_trust_anchors"
    text = open(src, encoding="utf-8").read()
    header = [ln for ln in text.splitlines() if ln.startswith("##")]
    blocks = re.findall(r"(?:^|\n)([^\n]*)\n=+\n(?:#[^\n]*\n)*-----BEGIN CERTIFICATE-----\n(.*?)-----END CERTIFICATE-----",
                        text, re.S)
    if len(blocks) != text.count("-----BEGIN CERTIFICATE-----"):
        sys.exit("mktrust: could not match every certificate with its title")
    o = ["/* generated by tools/mktrust.py from %s: do not edit */" % src.replace("\\", "/"),
         "/* The bundle's header:"]
    o += [" * " + h.replace("*/", "* /") for h in header]
    o += [" */", '#include "bearssl.h"', ""]
    names = []
    skipped = 0
    for idx, (title, b64) in enumerate(blocks):
        der = base64.b64decode("".join(b64.split()), validate=True)
        try:
            dn, key, is_ca = parse_cert(der)
        except (DerError, IndexError) as ex:
            sys.exit("mktrust: %s: %s" % (title, ex))
        safe = title.replace("*/", "* /")
        if key is None:
            o.append("/* skipped (key type BearSSL cannot use): %s */" % safe)
            skipped += 1
            continue
        if is_ca is False:
            print("mktrust: note: %s is not marked as a CA; trusted as one anyway" % title, file=sys.stderr)
        k = len(names)
        o.append("/* %s */" % safe)
        o.append("static const unsigned char TA%d_DN[] = {\n%s\n};" % (k, c_bytes(dn)))
        if key[0] == "RSA":
            o.append("static const unsigned char TA%d_N[] = {\n%s\n};" % (k, c_bytes(key[1])))
            o.append("static const unsigned char TA%d_E[] = {\n%s\n};" % (k, c_bytes(key[2])))
        else:
            o.append("static const unsigned char TA%d_Q[] = {\n%s\n};" % (k, c_bytes(key[2])))
        names.append((k, key))
    o.append("")
    o.append("const br_x509_trust_anchor %s[%d] = {" % (sym, len(names)))
    for k, key in names:
        if key[0] == "RSA":
            o.append("\t{ { (unsigned char *)TA%d_DN, sizeof TA%d_DN }, BR_X509_TA_CA,"
                     " { BR_KEYTYPE_RSA, { .rsa = { (unsigned char *)TA%d_N, sizeof TA%d_N,"
                     " (unsigned char *)TA%d_E, sizeof TA%d_E } } } }," % (k, k, k, k, k, k))
        else:
            o.append("\t{ { (unsigned char *)TA%d_DN, sizeof TA%d_DN }, BR_X509_TA_CA,"
                     " { BR_KEYTYPE_EC, { .ec = { %s, (unsigned char *)TA%d_Q, sizeof TA%d_Q } } } },"
                     % (k, k, key[1], k, k))
    o.append("};")
    o.append("const unsigned %s_count = %d;" % (sym, len(names)))
    with open(out, "w") as f:
        f.write("\n".join(o) + "\n")
    print("mktrust: %d trust anchors (%d skipped)" % (len(names), skipped), file=sys.stderr)


if __name__ == "__main__":
    main()
