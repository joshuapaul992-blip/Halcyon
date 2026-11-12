#!/usr/bin/env python3
"""ntfs_check.py: an independent consistency checker for NTFS images (for
the host tests; written separately from kernel/fs/ntfs*.c from the NTFS
documentation, so the two implementations check each other).

usage: ntfs_check.py [-v] [--offset BYTES] IMAGE

Checks:
  - boot sector geometry; every MFT record in the initialized part of $MFT
    (update sequence fixups, header, attribute headers, run lists);
  - $MFT:$BITMAP against the records' in-use flags; $MFTMirr against $MFT;
  - extension records and attribute lists (every entry resolves, every
    attribute of an extension record is listed);
  - cluster accounting: run lists do not overlap, stay in the volume, and
    $Bitmap marks exactly the clusters in use;
  - every $I30 index: B+ tree shape (all leaves at one depth, no empty
    non-root nodes, sub-node VCNs inside the allocation, each block used
    once and marked in $BITMAP, no unused block referenced), entries in
    collation order ($UpCase, then case-sensitive), keys that match a
    $FILE_NAME of the target record (parent, name, namespace), the
    duplicated information (sizes, attributes) equal to the file's;
  - every $FILE_NAME of an in-use file has its index entry; link counts;
  - $Volume flags (dirty), $LogFile (empty or a clean restart area).
Exit status 0 if no errors (warnings are printed with -v), 1 otherwise.
"""
import struct
import sys

errors = []
warnings = []
VERBOSE = False


def err(msg):
    errors.append(msg)


def warn(msg):
    warnings.append(msg)


def u16(b, o): return struct.unpack_from("<H", b, o)[0]
def u32(b, o): return struct.unpack_from("<I", b, o)[0]
def u64(b, o): return struct.unpack_from("<Q", b, o)[0]
def i64(b, o): return struct.unpack_from("<q", b, o)[0]


class Image:
    def __init__(self, path, offset=0):
        self.f = open(path, "rb")
        self.off = offset

    def read(self, pos, n):
        self.f.seek(self.off + pos)
        d = self.f.read(n)
        if len(d) < n:
            d += bytes(n - len(d))
        return d


def fixup(buf, what):
    b = bytearray(buf)
    usa_off, usa_cnt = u16(b, 4), u16(b, 6)
    if usa_cnt != len(b) // 512 + 1 or usa_off + 2 * usa_cnt > 510:
        err("%s: bad update sequence array" % what)
        return None
    usn = u16(b, usa_off)
    for i in range(1, usa_cnt):
        e = i * 512 - 2
        if u16(b, e) != usn:
            err("%s: torn (fixup %d mismatch)" % (what, i))
            return None
        b[e:e + 2] = b[usa_off + 2 * i:usa_off + 2 * i + 2]
    return bytes(b)


def decode_runs(mp, lowest, highest, nclus, what):
    runs = []
    p, vcn, lcn = 0, lowest, 0
    while p < len(mp) and mp[p]:
        lb, ob = mp[p] & 15, mp[p] >> 4
        p += 1
        if lb == 0 or lb > 8 or ob > 8 or p + lb + ob > len(mp):
            err("%s: bad mapping pair" % what)
            return runs
        ln = int.from_bytes(mp[p:p + lb], "little", signed=True)
        p += lb
        if ln <= 0:
            err("%s: run length %d" % (what, ln))
            return runs
        if ob:
            lcn += int.from_bytes(mp[p:p + ob], "little", signed=True)
            p += ob
            if lcn < 0 or lcn + ln > nclus:
                err("%s: run %d+%d outside the volume" % (what, lcn, ln))
            runs.append((vcn, lcn, ln))
        else:
            runs.append((vcn, None, ln))
        vcn += ln
    if vcn != highest + 1:
        err("%s: runs end at VCN %d, highest_vcn %d" % (what, vcn - 1, highest))
    return runs


class Attr:
    pass


def parse_attrs(rec, recno, nclus):
    attrs = []
    first = u16(rec, 0x14)
    used = u32(rec, 0x18)
    p = first
    instances = set()
    while True:
        if p + 4 > used:
            err("record %d: no end marker" % recno)
            break
        t = u32(rec, p)
        if t == 0xFFFFFFFF:
            break
        ln = u32(rec, p + 4)
        if ln < 0x18 or ln % 8 or p + ln > used:
            err("record %d: bad attribute length %d at %d" % (recno, ln, p))
            break
        a = Attr()
        a.type, a.nonres, a.flags = t, rec[p + 8], u16(rec, p + 12)
        nl, no = rec[p + 9], u16(rec, p + 10)
        a.name = rec[p + no:p + no + 2 * nl].decode("utf-16-le", "replace") if nl else ""
        a.instance = u16(rec, p + 14)
        a.rec = recno
        if a.instance in instances:
            err("record %d: duplicate attribute instance %d" % (recno, a.instance))
        instances.add(a.instance)
        if not a.nonres:
            vl, vo = u32(rec, p + 0x10), u16(rec, p + 0x14)
            if vo + vl > ln:
                err("record %d: resident value outside the attribute" % recno)
            a.value = rec[p + vo:p + vo + vl]
            a.lowest = 0
        else:
            a.lowest, a.highest = i64(rec, p + 0x10), i64(rec, p + 0x18)
            mpo = u16(rec, p + 0x20)
            a.cu = rec[p + 0x22]
            a.alloc, a.size, a.init = i64(rec, p + 0x28), i64(rec, p + 0x30), i64(rec, p + 0x38)
            a.comp = i64(rec, p + 0x40) if mpo >= 0x48 else None
            a.runs = decode_runs(rec[p + mpo:p + ln], a.lowest, a.highest, nclus,
                                 "record %d attr 0x%x" % (recno, t))
        attrs.append(a)
        p += ln
    if used != p + 8 and used != (p + 4 + 7) // 8 * 8:
        warn("record %d: bytes_in_use %d, end marker at %d" % (recno, used, p))
    return attrs


def attr_data(img, a, csize, limit=None):
    if not a.nonres:
        return a.value
    size = a.size if limit is None else min(a.size, limit)
    out = bytearray()
    for vcn, lcn, ln in a.runs:
        if len(out) >= size:
            break
        if lcn is None:
            out += bytes(ln * csize)
        else:
            out += img.read(lcn * csize, ln * csize)
    out = out[:size]
    if a.init < len(out):
        out[a.init:] = bytes(len(out) - a.init)
    return bytes(out)


def runs_of(extents):
    runs = []
    for a in sorted(extents, key=lambda x: x.lowest):
        runs += a.runs
    return runs


def runs_read(img, runs, pos, n, csize):
    out = bytearray()
    while n > 0:
        vcn, inn = divmod(pos, csize)
        for v, l, ln in runs:
            if v <= vcn < v + ln:
                break
        else:
            raise ValueError("VCN %d not mapped" % vcn)
        if l is None:
            raise ValueError("VCN %d sparse" % vcn)
        k = min(n, (v + ln - vcn) * csize - inn)
        out += img.read((l + vcn - v) * csize + inn, k)
        pos += k
        n -= k
    return bytes(out)


def main():
    global VERBOSE
    args = sys.argv[1:]
    offset = 0
    if args and args[0] == "-v":
        VERBOSE = True
        args = args[1:]
    if len(args) >= 2 and args[0] == "--offset":
        offset = int(args[1])
        args = args[2:]
    img = Image(args[0], offset)
    bs = img.read(0, 512)
    if bs[3:11] != b"NTFS    ":
        print("not NTFS")
        sys.exit(1)
    ss = u16(bs, 11)
    spc = bs[13]
    csize = ss * (spc if spc <= 0x80 else 1 << (256 - spc))
    total = u64(bs, 0x28)
    nclus = total * ss // csize
    mft_lcn, mirr_lcn = u64(bs, 0x30), u64(bs, 0x38)

    def recbytes(v):
        v = v if v < 128 else v - 256
        return v * csize if v > 0 else 1 << (-v)
    rsize, isize = recbytes(bs[0x40]), recbytes(bs[0x44])

    # $MFT's run list from record 0
    r0 = fixup(img.read(mft_lcn * csize, rsize), "record 0")
    if r0 is None:
        sys.exit(1)
    a0 = parse_attrs(r0, 0, nclus)
    mft_data = [a for a in a0 if a.type == 0x80 and a.name == ""]
    if not mft_data:
        print("no $MFT data")
        sys.exit(1)
    mft_runs = mft_data[0].runs
    mft_init = mft_data[0].init
    nrec = mft_init // rsize

    raw = {}

    def rec_raw(n):
        return runs_read(img, mft_runs, n * rsize, rsize, csize)

    records = {}
    for n in range(nrec):
        b = rec_raw(n)
        raw[n] = b
        if b[:4] != b"FILE":
            continue
        fb = fixup(b, "record %d" % n)
        if fb is None:
            continue
        flags = u16(fb, 0x16)
        if not flags & 1:
            continue
        if u16(fb, 4) >= 0x30 and u32(fb, 0x2C) != n:
            err("record %d: record number field %d" % (n, u32(fb, 0x2C)))
        if u32(fb, 0x1C) != rsize:
            err("record %d: bytes_allocated %d" % (n, u32(fb, 0x1C)))
        r = Attr()
        r.no, r.seq, r.links, r.flags = n, u16(fb, 0x10), u16(fb, 0x12), flags
        r.base = u64(fb, 0x20) & 0xFFFFFFFFFFFF
        r.attrs = parse_attrs(fb, n, nclus)
        records[n] = r

    # $MFT's full run list (attribute list of record 0)
    def gather(base):
        """all attributes of a file: base record plus listed extensions"""
        r = records.get(base)
        if not r:
            return []
        al = [a for a in r.attrs if a.type == 0x20]
        if not al:
            return list(r.attrs)
        data = attr_data(img, al[0], csize)
        out = []
        p = 0
        seen_ext = set()
        while p + 26 <= len(data):
            t, ln = u32(data, p), u16(data, p + 4)
            if ln < 26:
                err("record %d: bad attribute list entry" % base)
                break
            nl, no = data[p + 6], data[p + 7]
            name = data[p + no:p + no + 2 * nl].decode("utf-16-le", "replace") if nl else ""
            low, ref, inst = i64(data, p + 8), u64(data, p + 16), u16(data, p + 24)
            rn, sq = ref & 0xFFFFFFFFFFFF, ref >> 48
            rr = records.get(rn)
            if not rr:
                err("record %d: attribute list points to record %d (not in use)" % (base, rn))
            else:
                if rn != base:
                    seen_ext.add(rn)
                    if rr.base != base:
                        err("record %d: extension %d has base %d" % (base, rn, rr.base))
                if sq and sq != rr.seq:
                    err("record %d: attribute list sequence %d, record %d has %d" % (base, sq, rn, rr.seq))
                m = [a for a in rr.attrs if a.type == t and a.instance == inst]
                if len(m) != 1:
                    err("record %d: list entry 0x%x/%d not found in record %d" % (base, t, inst, rn))
                else:
                    if m[0].name != name or m[0].lowest != low:
                        err("record %d: list entry does not match its attribute" % base)
                    out.append(m[0])
            p += ln
        for rn, rr in records.items():
            if rr.base == base and rn != base:
                for a in rr.attrs:
                    if a not in out:
                        err("record %d: extension %d attribute 0x%x not in the list" % (base, rn, a.type))
        return out

    allattrs = {}
    for n, r in records.items():
        if r.base == 0:
            allattrs[n] = gather(n)
        elif r.base not in records:
            err("record %d: extension of record %d, which is not in use" % (n, r.base))
    mft_ext = [a for a in allattrs.get(0, []) if a.type == 0x80 and a.name == ""]
    if mft_ext:
        mft_runs = runs_of(mft_ext)

    # MFT bitmap
    bm_attr = [a for a in allattrs.get(0, []) if a.type == 0xB0]
    mftbmp = attr_data(img, bm_attr[0], csize) if bm_attr else b""
    for n in range(nrec):
        bit = (mftbmp[n // 8] >> (n % 8)) & 1 if n // 8 < len(mftbmp) else 0
        inuse = n in records
        if bit != inuse and n >= 16:
            err("record %d: in use %d, MFT bitmap %d" % (n, inuse, bit))
        elif bit != inuse and n < 16 and inuse:
            err("system record %d: bitmap clear" % n)

    # $MFTMirr
    mirr = [a for a in allattrs.get(1, []) if a.type == 0x80]
    if mirr:
        mn = min(mirr[0].size // rsize, 4 if csize <= 4 * rsize else csize // rsize)
        for n in range(mn):
            m = runs_read(img, runs_of(mirr), n * rsize, rsize, csize)
            if m != raw.get(n):
                err("$MFTMirr record %d differs from $MFT" % n)

    # $UpCase
    up = [a for a in allattrs.get(10, []) if a.type == 0x80]
    upd = attr_data(img, up[0], csize) if up else b""
    upcase = list(struct.unpack("<%dH" % (len(upd) // 2), upd))

    def upc(c):
        return upcase[c] if c < len(upcase) else c

    def name_cmp(a, b):
        ua, ub = [upc(c) for c in a], [upc(c) for c in b]
        if ua != ub:
            return -1 if ua < ub else 1
        if a != b:
            return -1 if a < b else 1
        return 0

    # cluster accounting
    owner = {}
    used = bytearray((nclus + 7) // 8)

    def mark(lcn, ln, who):
        for c in range(lcn, lcn + ln):
            if c >= nclus:
                return
            if used[c // 8] & (1 << (c % 8)):
                err("cluster %d used twice (%s and %s)" % (c, owner.get(c, "?"), who))
                return
            used[c // 8] |= 1 << (c % 8)
            if len(owner) < 2000000:
                owner[c] = who

    for n, attrs in allattrs.items():
        for a in attrs:
            if a.nonres:
                for vcn, lcn, ln in a.runs:
                    if lcn is not None:
                        mark(lcn, ln, "record %d attr 0x%x" % (n, a.type))
        for a in records[n].attrs:
            if a.type == 0x20 and a.nonres:
                for vcn, lcn, ln in a.runs:
                    if lcn is not None:
                        mark(lcn, ln, "record %d attribute list" % n)
    bmp = [a for a in allattrs.get(6, []) if a.type == 0x80]
    bitmap = attr_data(img, bmp[0], csize) if bmp else b""
    nbad = 0
    for c in range(nclus):
        b1 = (bitmap[c // 8] >> (c % 8)) & 1 if c // 8 < len(bitmap) else 0
        b2 = (used[c // 8] >> (c % 8)) & 1
        if b1 != b2:
            nbad += 1
            if nbad <= 10:
                err("cluster %d: $Bitmap %d, in use %d%s" % (c, b1, b2, " (leaked)" if b1 else ""))
    if nbad > 10:
        err("... %d clusters mismatch in total" % nbad)

    # file names, sizes
    def si_of(n):
        s = [a for a in allattrs.get(n, []) if a.type == 0x10]
        return s[0].value if s else None

    def dup_of(n):
        attrs = allattrs.get(n, [])
        si = si_of(n)
        fattr = u32(si, 32) if si and len(si) >= 48 else 0
        isdir = bool(records[n].flags & 2)
        if isdir:
            return 0, 0, (fattr & ~0x10) | 0x10000000
        d = [a for a in attrs if a.type == 0x80 and a.name == "" and a.lowest == 0]
        if not d:
            return None
        d = d[0]
        if not d.nonres:
            return (len(d.value) + 7) // 8 * 8, len(d.value), fattr & ~0x10
        alloc = d.comp if (d.flags & 0x8001) and d.comp is not None else d.alloc
        return alloc, d.size, fattr & ~0x10

    names = {}       # (rec) -> list of (parent, name tuple, ns)
    for n, attrs in allattrs.items():
        fns = []
        for a in attrs:
            if a.type == 0x30 and not a.nonres:
                v = a.value
                nl, ns = v[64], v[65]
                fns.append((u64(v, 0) & 0xFFFFFFFFFFFF, tuple(struct.unpack_from("<%dH" % nl, v, 66)), ns, v))
        names[n] = fns
        if fns and n >= 16 and records[n].links != len(fns):
            err("record %d: link count %d, %d names" % (n, records[n].links, len(fns)))

    # indexes
    found_entries = set()
    for n, attrs in allattrs.items():
        roots = [a for a in attrs if a.type == 0x90 and a.name == "$I30"]
        if not roots:
            if records[n].flags & 2:
                err("record %d: directory without $I30" % n)
            continue
        if not records[n].flags & 2:
            err("record %d: $I30 in a non-directory record" % n)
        root = roots[0].value
        ibs = u32(root, 8)
        cpb = root[12]
        vunit = csize if ibs >= csize else ibs // cpb
        alloc = [a for a in attrs if a.type == 0xA0 and a.name == "$I30"]
        ibmp = [a for a in attrs if a.type == 0xB0 and a.name == "$I30"]
        aruns = runs_of(alloc) if alloc else []
        asize = min(a.size for a in alloc if a.lowest == 0) if alloc else 0
        ibmpd = attr_data(img, ibmp[0], csize) if ibmp else b""
        if alloc and not ibmp:
            err("record %d: $INDEX_ALLOCATION without $BITMAP" % n)
        visited = set()
        leaf_depths = set()
        prev = [None]
        what = "dir %d" % n

        def entries(buf, off, length):
            out = []
            p = off
            while p + 16 <= off + length:
                ln, kl, fl = u16(buf, p + 8), u16(buf, p + 10), u16(buf, p + 12)
                if ln < 16 or ln % 8:
                    err("%s: bad entry length %d" % (what, ln))
                    break
                e = (u64(buf, p), buf[p + 16:p + 16 + kl], fl,
                     i64(buf, p + ln - 8) if fl & 1 else None)
                out.append(e)
                if fl & 2:
                    break
                p += ln
            else:
                err("%s: node without an END entry" % what)
            return out

        def node(ents, large, depth, is_root):
            if depth > 30:
                err("%s: index too deep" % what)
                return
            real = [e for e in ents if not e[2] & 2]
            if not is_root and not real:
                err("%s: empty non-root index node" % what)
            for e in ents:
                if bool(e[2] & 1) != large:
                    err("%s: NODE flag does not match the node" % what)
            if not large:
                leaf_depths.add(depth)
            for e in ents:
                if e[3] is not None:
                    child(e[3], depth + 1)
                if e[2] & 2:
                    break
                visit_entry(e)

        def child(vcn, depth):
            pos = vcn * vunit
            if pos % ibs or pos + ibs > asize:
                err("%s: sub-node VCN %d outside the allocation" % (what, vcn))
                return
            blk = pos // ibs
            if blk in visited:
                err("%s: index block %d used twice" % (what, blk))
                return
            visited.add(blk)
            if not (blk // 8 < len(ibmpd) and ibmpd[blk // 8] >> (blk % 8) & 1):
                err("%s: index block %d in use, $BITMAP clear" % (what, blk))
            try:
                b = runs_read(img, aruns, pos, ibs, csize)
            except ValueError as ex:
                err("%s: %s" % (what, ex))
                return
            if b[:4] != b"INDX":
                err("%s: block %d is not INDX" % (what, blk))
                return
            b = fixup(b, "%s block %d" % (what, blk))
            if b is None:
                return
            if i64(b, 16) != vcn:
                err("%s: block %d has VCN %d" % (what, blk, i64(b, 16)))
            eo, il, al_, fl = u32(b, 0x18), u32(b, 0x1C), u32(b, 0x20), b[0x24]
            if il > al_ or 0x18 + al_ > ibs:
                err("%s: block %d header" % (what, blk))
                return
            node(entries(b, 0x18 + eo, il - eo), bool(fl & 1), depth, False)

        def visit_entry(e):
            ref, key = e[0], e[1]
            rn, sq = ref & 0xFFFFFFFFFFFF, ref >> 48
            nl, ns = key[64], key[65]
            nm = tuple(struct.unpack_from("<%dH" % nl, key, 66))
            if prev[0] is not None and name_cmp(prev[0], nm) >= 0:
                err("%s: entries out of order: %r >= %r" % (what, bytes(struct.pack("<%dH" % len(prev[0]), *prev[0])).decode("utf-16-le", "replace"),
                                                             bytes(key[66:66 + 2 * nl]).decode("utf-16-le", "replace")))
            prev[0] = nm
            r = records.get(rn)
            label = bytes(key[66:66 + 2 * nl]).decode("utf-16-le", "replace")
            if not r:
                err("%s: entry '%s' points to record %d, not in use" % (what, label, rn))
                return
            if sq and sq != r.seq:
                err("%s: entry '%s' sequence %d, record has %d" % (what, label, sq, r.seq))
            match = [f for f in names.get(rn, []) if f[0] == n and f[1] == nm and f[2] == ns]
            if not match:
                err("%s: entry '%s' has no matching $FILE_NAME in record %d" % (what, label, rn))
                return
            found_entries.add((rn, n, nm, ns))
            if (u64(key, 0) & 0xFFFFFFFFFFFF) != n:
                err("%s: entry '%s' parent field %d" % (what, label, u64(key, 0) & 0xFFFFFFFFFFFF))
            d = dup_of(rn)
            if d is not None and rn >= 16:
                ka, ks, kf = i64(key, 40), i64(key, 48), u32(key, 56)
                if (ka, ks) != d[:2]:
                    err("%s: '%s' duplicated sizes %d/%d, file %d/%d" % (what, label, ka, ks, d[0], d[1]))
                if kf != d[2]:
                    err("%s: '%s' duplicated attributes %x, file %x" % (what, label, kf, d[2]))
                rv = match[0][3]
                if rv[8:56] != key[8:56]:
                    warn("%s: '%s' index entry and $FILE_NAME differ in times/sizes" % (what, label))

        eo, il, fl = u32(root, 16), u32(root, 20), root[28]
        node(entries(root, 16 + eo, il - eo), bool(fl & 1), 0, True)
        if len(leaf_depths) > 1:
            err("%s: leaves at depths %s" % (what, sorted(leaf_depths)))
        nblk = asize // ibs if alloc else 0
        for blk in range(nblk):
            if blk // 8 < len(ibmpd) and ibmpd[blk // 8] >> (blk % 8) & 1 and blk not in visited:
                err("%s: index block %d marked in use, not in the tree" % (what, blk))

    for rn, fns in names.items():
        if rn < 16 and rn != 5:
            continue
        for parent, nm, ns, v in fns:
            if (rn, parent, nm, ns) not in found_entries and not (rn == 5 and parent == 5):
                err("record %d: name '%s' not in directory %d" % (rn, bytes(v[66:66 + 2 * len(nm)]).decode("utf-16-le", "replace"), parent))

    # $Volume and $LogFile
    vi = [a for a in allattrs.get(3, []) if a.type == 0x70]
    vflags = u16(vi[0].value, 10) if vi else 0
    lf = [a for a in allattrs.get(2, []) if a.type == 0x80]
    if lf:
        head = attr_data(img, lf[0], csize, 8192)
        if head != b"\xff" * len(head):
            p = head[:4096]
            if p[:4] != b"RSTR":
                err("$LogFile: neither empty nor a restart page")
    state = "dirty" if vflags & 1 else "clean"
    if VERBOSE:
        for w in warnings[:50]:
            print("warning:", w)
    for e in errors[:100]:
        print("error:", e)
    print("ntfs_check: %d records in use, %d errors, %d warnings, volume %s" % (len(records), len(errors), len(warnings), state))
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
