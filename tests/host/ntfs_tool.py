#!/usr/bin/env python3
"""ntfs_tool.py - a small, independent NTFS reader used to check volumes made
by Halcyon's NTFS formatter (kernel/fs/ntfs_mkfs.c) and, for comparison, by
mkntfs. Written from the Linux-NTFS project's NTFS documentation; it shares
no code with the formatter or with ntfs-3g.

usage: ntfs_tool.py IMAGE[@@OFFSET] COMMAND

  check     verify the volume's metadata; exit status 1 on errors:
            boot sector and its backup, $MFTMirr == the first $MFT records,
            update sequence arrays, attribute order / instances / lengths,
            run lists, $Bitmap == the clusters the run lists use (no cross
            links, nothing leaked), $MFT:$BITMAP == the records in use,
            every directory index (sorted with the volume's $UpCase, entries
            match the files' $FILE_NAME attributes, link counts), $Secure
            ($SDS hashes and mirror copies, $SDH / $SII match $SDS, every
            security id in use exists), $LogFile restart pages, $AttrDef,
            $Volume, $UpCase
  dump      the system files (records 0-26) in a normalised form for diffing
            against another formatter: no times, serial numbers or LCNs
  info      a summary: geometry, MFT / mirror / log positions, free space
  upcase    write the volume's $UpCase table (128 KiB) to stdout
"""
import struct
import sys

AT_NAMES = {
    0x10: "$STANDARD_INFORMATION", 0x20: "$ATTRIBUTE_LIST", 0x30: "$FILE_NAME",
    0x40: "$OBJECT_ID", 0x50: "$SECURITY_DESCRIPTOR", 0x60: "$VOLUME_NAME",
    0x70: "$VOLUME_INFORMATION", 0x80: "$DATA", 0x90: "$INDEX_ROOT",
    0xA0: "$INDEX_ALLOCATION", 0xB0: "$BITMAP", 0xC0: "$REPARSE_POINT",
    0xD0: "$EA_INFORMATION", 0xE0: "$EA", 0x100: "$LOGGED_UTILITY_STREAM",
}


class NtfsError(Exception):
    pass


def u16(b, o): return struct.unpack_from("<H", b, o)[0]
def u32(b, o): return struct.unpack_from("<I", b, o)[0]
def u64(b, o): return struct.unpack_from("<Q", b, o)[0]
def i64(b, o): return struct.unpack_from("<q", b, o)[0]


def fixup(rec, what):
    """apply the update sequence array (512-byte strides); returns bytes"""
    rec = bytearray(rec)
    uo, uc = u16(rec, 4), u16(rec, 6)
    if uc != len(rec) // 512 + 1:
        raise NtfsError("%s: usa_count %d for %d bytes" % (what, uc, len(rec)))
    if uo & 1 or uo + 2 * uc > 510:
        raise NtfsError("%s: bad usa_offset %d" % (what, uo))
    usn = rec[uo:uo + 2]
    for i in range(1, uc):
        e = i * 512 - 2
        if rec[e:e + 2] != usn:
            raise NtfsError("%s: update sequence mismatch in stride %d" % (what, i))
        rec[e:e + 2] = rec[uo + 2 * i:uo + 2 * i + 2]
    return bytes(rec)


def decode_runs(b, o, end):
    """mapping pairs -> list of (vcn, lcn or None for sparse, length)"""
    runs, vcn, lcn = [], 0, 0
    while o < end and b[o]:
        h = b[o]
        ln, lo = h & 15, h >> 4
        o += 1
        if ln == 0 or ln > 8 or lo > 8:
            raise NtfsError("bad run header 0x%02x" % h)
        length = int.from_bytes(b[o:o + ln], "little", signed=True)
        o += ln
        if length <= 0:
            raise NtfsError("run length %d" % length)
        if lo:
            lcn += int.from_bytes(b[o:o + lo], "little", signed=True)
            o += lo
            runs.append((vcn, lcn, length))
        else:
            runs.append((vcn, None, length))
        vcn += length
    if o >= end:
        raise NtfsError("run list not terminated")
    return runs


class Attr:
    def __init__(self, rec, o, recno):
        self.type = u32(rec, o)
        self.length = u32(rec, o + 4)
        self.nonres = rec[o + 8]
        nl, no = rec[o + 9], u16(rec, o + 10)
        self.flags = u16(rec, o + 12)
        self.instance = u16(rec, o + 14)
        self.name = rec[o + no:o + no + 2 * nl].decode("utf-16-le") if nl else ""
        self.name_offset = no
        self.offset = o
        raw = rec[o:o + self.length]
        self.raw = raw
        if not self.nonres:
            vl, vo = u32(raw, 16), u16(raw, 20)
            self.resident_flags = raw[22]
            if vo + vl > self.length:
                raise NtfsError("record %d: resident value beyond the attribute" % recno)
            self.value = raw[vo:vo + vl]
            self.value_offset = vo
            self.data_size = vl
        else:
            self.lowest_vcn = i64(raw, 16)
            self.highest_vcn = i64(raw, 24)
            mpo = u16(raw, 32)
            self.compression_unit = raw[34]
            self.alloc_size = i64(raw, 40)
            self.data_size = i64(raw, 48)
            self.init_size = i64(raw, 56)
            self.mp_offset = mpo
            self.runs = decode_runs(raw, mpo, self.length)

    def key(self):
        return (self.type, self.name)


class Volume:
    def __init__(self, path, offset=0):
        self.f = open(path, "rb")
        self.base = offset
        b = self.read(0, 512)
        if b[3:11] != b"NTFS    ":
            raise NtfsError("no NTFS boot sector")
        self.boot = b
        self.ss = u16(b, 11)
        spc = b[13]
        self.spc = spc if spc <= 0x80 else 1 << (256 - spc)
        self.cs = self.ss * self.spc
        self.total_sectors = u64(b, 0x28)
        self.clusters = self.total_sectors // self.spc
        self.mft_lcn = u64(b, 0x30)
        self.mirr_lcn = u64(b, 0x38)
        self.rec_size = self.enc_size(struct.unpack_from("<b", b, 0x40)[0])
        self.idx_size = self.enc_size(struct.unpack_from("<b", b, 0x44)[0])
        self.serial = u64(b, 0x48)
        # $MFT's own record, then its run list
        r0 = fixup(self.read(self.mft_lcn * self.cs, self.rec_size), "MFT record 0")
        self.mft_runs = None
        attrs = self.parse_attrs(r0, 0)
        data = [a for a in attrs if a.type == 0x80 and a.name == ""]
        if not data or not data[0].nonres:
            raise NtfsError("$MFT has no non-resident $DATA")
        self.mft_runs = data[0].runs
        self.mft_size = data[0].data_size
        self.nrec = self.mft_size // self.rec_size
        self.upcase = None

    def enc_size(self, v):
        return v * self.cs if v > 0 else 1 << -v

    def read(self, off, n):
        self.f.seek(self.base + off)
        b = self.f.read(n)
        if len(b) != n:
            raise NtfsError("short read at %d" % off)
        return b

    def read_runs(self, runs, size):
        out = bytearray()
        for vcn, lcn, ln in runs:
            if lcn is None:
                out += bytes(ln * self.cs)
            else:
                if lcn + ln > self.clusters + 1:
                    raise NtfsError("run %d+%d beyond the volume" % (lcn, ln))
                out += self.read(lcn * self.cs, ln * self.cs)
        if len(out) < size:
            raise NtfsError("runs shorter than the data size")
        return bytes(out[:size])

    def record_raw(self, n):
        off = n * self.rec_size
        for vcn, lcn, ln in self.mft_runs:
            if vcn * self.cs <= off < (vcn + ln) * self.cs:
                return self.read(lcn * self.cs + off - vcn * self.cs, self.rec_size)
        raise NtfsError("record %d not mapped" % n)

    def record(self, n):
        raw = self.record_raw(n)
        if u32(raw, 0) != 0x454C4946:
            return None
        return fixup(raw, "MFT record %d" % n)

    def parse_attrs(self, rec, n):
        o = u16(rec, 20)
        attrs = []
        used = u32(rec, 24)
        while True:
            if o + 4 > used:
                raise NtfsError("record %d: attributes run past bytes_in_use" % n)
            t = u32(rec, o)
            if t == 0xFFFFFFFF:
                break
            ln = u32(rec, o + 4)
            if ln < 0x18 or ln & 7 or o + ln > used:
                raise NtfsError("record %d: bad attribute length %d at 0x%x" % (n, ln, o))
            attrs.append(Attr(rec, o, n))
            o += ln
        if (o + 8 + 7) & ~7 != used and o + 4 != used and o + 8 != used:
            raise NtfsError("record %d: bytes_in_use %d, end marker at %d" % (n, used, o))
        return attrs

    def attr_data(self, a):
        return a.value if not a.nonres else self.read_runs(a.runs, a.data_size)

    def file_attrs(self, n):
        rec = self.record(n)
        if rec is None:
            return None, None
        return rec, self.parse_attrs(rec, n)

    def stream(self, n, type_, name=""):
        rec, attrs = self.file_attrs(n)
        for a in attrs or []:
            if a.type == type_ and a.name == name:
                return self.attr_data(a)
        return None

    def get_upcase(self):
        if self.upcase is None:
            d = self.stream(10, 0x80)
            self.upcase = struct.unpack("<%dH" % (len(d) // 2), d)
        return self.upcase


# ---------------------------------------------------------------- indexes

def index_entries(buf, ho, what):
    """entries of an index node whose ntfs_index_header is at buf[ho]"""
    eo, il, al, fl = u32(buf, ho), u32(buf, ho + 4), u32(buf, ho + 8), buf[ho + 12]
    if il > al or ho + al > len(buf):
        raise NtfsError("%s: index_length %d allocated %d" % (what, il, al))
    o, end = ho + eo, ho + il
    out = []
    while True:
        if o + 16 > end:
            raise NtfsError("%s: index entries run past index_length" % what)
        ln, kl, efl = u16(buf, o + 8), u16(buf, o + 10), u16(buf, o + 12)
        if ln < 16 or ln & 7 or o + ln > end:
            raise NtfsError("%s: bad index entry length %d" % (what, ln))
        e = {"raw": buf[o:o + ln], "flags": efl, "key": buf[o + 16:o + 16 + kl],
             "file": u64(buf, o), "doff": u16(buf, o), "dlen": u16(buf, o + 2)}
        if efl & 1:
            e["vcn"] = i64(buf, o + ln - 8)
        out.append(e)
        o += ln
        if efl & 2:
            break
    if o != end:
        raise NtfsError("%s: %d bytes after the end entry" % (what, end - o))
    return out, fl


def walk_index(vol, n, name, attrs, errors):
    """all keys of index `name` in record n in tree order (list of entries)"""
    root = [a for a in attrs if a.type == 0x90 and a.name == name]
    if not root:
        return None, None
    r = root[0].value
    itype, coll, bsize = u32(r, 0), u32(r, 4), u32(r, 8)
    ents, fl = index_entries(r, 16, "record %d %s root" % (n, name))
    alloc = [a for a in attrs if a.type == 0xA0 and a.name == name]
    bmp = [a for a in attrs if a.type == 0xB0 and a.name == name]
    blocks = {}
    if alloc:
        data = vol.attr_data(alloc[0])
        bm = vol.attr_data(bmp[0]) if bmp else b""
        for i in range(len(data) // bsize):
            if i // 8 < len(bm) and bm[i // 8] >> (i % 8) & 1:
                blocks[i] = fixup(data[i * bsize:(i + 1) * bsize], "record %d %s block %d" % (n, name, i))
    elif fl & 1:
        errors.append("record %d %s: large index without $INDEX_ALLOCATION" % (n, name))
    out = []
    seen = set()
    vcn_unit = vol.cs if vol.cs <= bsize else 512    # (blocks smaller than a cluster: 512-byte VCNs)

    def walk(ents, depth):
        for e in ents:
            if "vcn" in e:
                bi = e["vcn"] * vcn_unit // bsize
                if bi not in blocks or bi in seen or depth > 32:
                    errors.append("record %d %s: bad sub-node vcn %d" % (n, name, e["vcn"]))
                else:
                    seen.add(bi)
                    blk = blocks[bi]
                    if u32(blk, 0) != 0x58444E49 or i64(blk, 16) != e["vcn"]:
                        errors.append("record %d %s: block %d header" % (n, name, bi))
                    walk(index_entries(blk, 24, "record %d %s block %d" % (n, name, bi))[0], depth + 1)
            if not e["flags"] & 2:
                out.append(e)
    walk(ents, 0)
    if set(blocks) != seen:
        errors.append("record %d %s: blocks in use but not in the tree: %s" % (n, name, sorted(set(blocks) - seen)))
    return out, (itype, coll, bsize)


def upcase_key(up, s):
    return [up[ord(c)] if ord(c) < 0x10000 else ord(c) for c in s]


def fn_parse(v):
    nl = v[64]
    return {"parent": u64(v, 0), "ctime": i64(v, 8), "mtime": i64(v, 16), "rtime": i64(v, 24),
            "atime": i64(v, 32), "alloc": i64(v, 40), "size": i64(v, 48), "fattr": u32(v, 56),
            "reparse": u32(v, 60), "ns": v[65], "name": v[66:66 + 2 * nl].decode("utf-16-le", "replace"),
            "units": struct.unpack_from("<%dH" % nl, v, 66)}


def sd_hash(sd):
    h = 0
    for i in range(len(sd) // 4):
        h = (((h >> 29) | (h << 3)) + u32(sd, 4 * i)) & 0xFFFFFFFF
    return h


def sd_valid(sd):
    """minimal self-relative security descriptor validation"""
    if len(sd) < 20 or sd[0] != 1:
        return "revision"
    ctl = u16(sd, 2)
    if not ctl & 0x8000:
        return "not self-relative"
    for fo in (4, 8):
        o = u32(sd, fo)
        if o:
            if o + 8 > len(sd) or sd[o] != 1 or o + 8 + 4 * sd[o + 1] > len(sd):
                return "bad SID at %d" % o
    for fo, bit in ((12, 0x10), (16, 0x4)):
        o = u32(sd, fo)
        if o and ctl & bit:
            if o + 8 > len(sd):
                return "bad ACL"
            sz, cnt = u16(sd, o + 2), u16(sd, o + 4)
            if o + sz > len(sd):
                return "ACL beyond the descriptor"
            p = o + 8
            for _ in range(cnt):
                if p + 8 > o + sz:
                    return "ACE beyond the ACL"
                asz = u16(sd, p + 2)
                if asz < 16 or p + asz > o + sz:
                    return "bad ACE size"
                p += asz
    return None


# ---------------------------------------------------------------- check

def check(vol):
    errs = []
    E = errs.append
    cs, rs = vol.cs, vol.rec_size
    b = vol.boot
    # boot sector
    if u16(b, 0x1FE) != 0xAA55:
        E("boot sector signature")
    if vol.ss not in (256, 512, 1024, 2048, 4096):
        E("sector size %d" % vol.ss)
    if b[14] or b[15] or any(b[16:21]) or b[0x15] != 0xF8:
        E("boot sector reserved fields")
    if vol.mft_lcn >= vol.clusters or vol.mirr_lcn >= vol.clusters:
        E("MFT / mirror LCN beyond the volume")
    try:
        bak = vol.read(vol.total_sectors * vol.ss, vol.ss)
        if bak[:512] != b[:512]:
            E("backup boot sector differs from the boot sector")
    except NtfsError:
        E("no backup boot sector after the volume")
    # records
    extents = []        # (lcn, clusters, who) of every run

    def claim(runs, who):
        for vcn, lcn, ln in runs:
            if lcn is not None:
                extents.append((lcn, ln, who))

    mftbmp = vol.stream(0, 0xB0)
    files = {}
    for n in range(vol.nrec):
        raw = vol.record_raw(n)
        magic = u32(raw, 0)
        inbm = n // 8 < len(mftbmp) and (mftbmp[n // 8] >> (n % 8)) & 1
        if magic != 0x454C4946:
            if inbm:
                E("record %d in use in $MFT:$BITMAP but has no FILE header" % n)
            continue
        try:
            rec = fixup(raw, "record %d" % n)
        except NtfsError as e:
            E(str(e))
            continue
        flags = u16(rec, 22)
        if bool(flags & 1) != bool(inbm):
            if not (n < 16 and inbm):   # reserved records: bitmap set, record unused
                E("record %d: in-use flag %d, $MFT:$BITMAP bit %d" % (n, flags & 1, inbm))
        if u32(rec, 28) != rs:
            E("record %d: bytes_allocated %d" % (n, u32(rec, 28)))
        if not flags & 1:
            continue
        if u16(rec, 4) >= 0x30 and u32(rec, 44) != n:
            E("record %d: record_number field %d" % (n, u32(rec, 44)))
        if u32(rec, 24) > rs or u32(rec, 24) & 7:
            E("record %d: bytes_in_use %d" % (n, u32(rec, 24)))
        try:
            attrs = vol.parse_attrs(rec, n)
        except NtfsError as e:
            E(str(e))
            continue
        nxt = u16(rec, 40)
        insts = set()
        prev = None
        for a in attrs:
            if a.instance >= nxt:
                E("record %d: attribute instance %d >= next_attr_instance %d" % (n, a.instance, nxt))
            if a.instance in insts:
                E("record %d: duplicate attribute instance %d" % (n, a.instance))
            insts.add(a.instance)
            k = (a.type, upcase_key(vol.get_upcase(), a.name) if vol.upcase or n > 10 else a.name)
            if prev is not None and (a.type < prev[0] or (a.type == prev[0] and a.name and prev[1] and k[1] < prev[1])):
                E("record %d: attributes out of order (0x%x '%s')" % (n, a.type, a.name))
            prev = k
            if a.name and a.name_offset + 2 * len(a.name) > (a.mp_offset if a.nonres else a.value_offset):
                E("record %d: attribute name overlaps the value" % n)
            if a.nonres:
                vcns = sum(r[2] for r in a.runs)
                if a.lowest_vcn != 0:
                    E("record %d: lowest_vcn %d (attribute lists are not expected here)" % (n, a.lowest_vcn))
                if a.highest_vcn != vcns - 1:
                    E("record %d 0x%x '%s': highest_vcn %d, runs cover %d clusters" % (n, a.type, a.name, a.highest_vcn, vcns))
                if a.alloc_size != vcns * cs:
                    E("record %d 0x%x '%s': allocated_size %d != %d clusters" % (n, a.type, a.name, a.alloc_size, vcns))
                if not (a.init_size <= a.data_size <= a.alloc_size):
                    E("record %d 0x%x: sizes init %d data %d alloc %d" % (n, a.type, a.init_size, a.data_size, a.alloc_size))
                claim(a.runs, "record %d 0x%x '%s'" % (n, a.type, a.name))
            if a.type in (0x10, 0x30, 0x60, 0x70, 0x90) and a.nonres:
                E("record %d: attribute 0x%x must be resident" % (n, a.type))
        base = u64(rec, 32) & 0xFFFFFFFFFFFF
        if not base and (not attrs or attrs[0].type != 0x10):
            E("record %d: no $STANDARD_INFORMATION first" % n)
        fns = [fn_parse(a.value) for a in attrs if a.type == 0x30]
        si = attrs[0].value if attrs and not base else b""
        files[n] = {"rec": rec, "attrs": attrs, "fns": fns, "flags": flags, "si": si,
                    "links": u16(rec, 18), "seq": u16(rec, 16), "base": base}
    # extension records (files with an attribute list): their names count for the base record
    for n, f in files.items():
        if f["base"] and f["base"] in files:
            files[f["base"]]["fns"] += f["fns"]
        elif f["base"]:
            E("extension record %d: base record %d not in use" % (n, f["base"]))
    # $Bitmap
    bm = vol.stream(6, 0x80)
    if bm is None:
        E("no $Bitmap")
    else:
        if len(bm) * 8 < vol.clusters:
            E("$Bitmap is %d bytes for %d clusters" % (len(bm), vol.clusters))
        if len(bm) % 8:
            E("$Bitmap size %d is not a multiple of 8" % len(bm))
        # cross links: overlapping extents
        extents.sort()
        for (a, an, aw), (b, bn, bw) in zip(extents, extents[1:]):
            if b < a + an:
                E("clusters %d.. used by %s and %s" % (b, aw, bw))
        # the bitmap the extents call for (as one big integer: bit c = cluster c)
        want = 0
        for lcn, ln, who in extents:
            if lcn + ln > vol.clusters:
                E("%s: clusters %d..%d beyond the volume" % (who, lcn, lcn + ln - 1))
            want |= ((1 << ln) - 1) << lcn
        nbits = len(bm) * 8
        have = int.from_bytes(bm, "little")
        vol_mask = (1 << vol.clusters) - 1
        tail = ((1 << nbits) - 1) & ~vol_mask
        if have & tail != tail:
            E("$Bitmap bits past the volume end are not set")
        have &= vol_mask
        leaked, missing = have & ~want, want & ~have & vol_mask
        if leaked:
            E("%d clusters allocated in $Bitmap but not used by any file (first %d)" % (
                bin(leaked).count("1"), (leaked & -leaked).bit_length() - 1))
        if missing:
            c = (missing & -missing).bit_length() - 1
            who = [w for l, n, w in extents if l <= c < l + n]
            E("%d clusters used but free in $Bitmap (first %d, %s)" % (bin(missing).count("1"), c, who[0] if who else "?"))
        vol.free_clusters = vol.clusters - bin(have).count("1")
    # $MFTMirr
    mirr = vol.stream(1, 0x80)
    if mirr is None or len(mirr) < 4 * rs:
        E("$MFTMirr smaller than 4 records")
    else:
        for i in range(len(mirr) // rs):
            if mirr[i * rs:(i + 1) * rs] != vol.record_raw(i):
                E("$MFTMirr record %d differs from $MFT" % i)
        mattrs = files.get(1, {}).get("attrs", [])
        md = [a for a in mattrs if a.type == 0x80 and not a.name]
        if md and md[0].nonres and md[0].runs[0][1] != vol.mirr_lcn:
            E("$MFTMirr's data is not at the boot sector's mirror LCN")
    if vol.mft_runs[0][1] != vol.mft_lcn:
        E("$MFT's data is not at the boot sector's MFT LCN")
    if 7 in files:
        bd = [a for a in files[7]["attrs"] if a.type == 0x80 and not a.name]
        if not bd or not bd[0].nonres or bd[0].runs[0][1] != 0:
            E("$Boot does not start at cluster 0")
    # system file names and directories
    sysnames = ["$MFT", "$MFTMirr", "$LogFile", "$Volume", "$AttrDef", ".", "$Bitmap", "$Boot",
                "$BadClus", "$Secure", "$UpCase", "$Extend"]
    for i, nm in enumerate(sysnames):
        if i not in files:
            E("system file %d (%s) missing" % (i, nm))
        elif [f["name"] for f in files[i]["fns"]] != [nm]:
            E("record %d: name %s, expected %s" % (i, [f["name"] for f in files[i]["fns"]], nm))
        elif files[i]["seq"] != (i if i else 1):
            E("record %d: sequence number %d" % (i, files[i]["seq"]))
    # directory indexes
    up = vol.get_upcase()
    linked = {}
    for n, f in files.items():
        if f["flags"] & 2:
            try:
                ents, info = walk_index(vol, n, "$I30", f["attrs"], errs)
            except NtfsError as e:
                E(str(e))
                continue
            if ents is None:
                E("directory %d has no $I30 index" % n)
                continue
            if info[0] != 0x30 or info[1] != 1 or info[2] != vol.idx_size:
                E("directory %d: $I30 type 0x%x collation %d block size %d" % (n, *info))
            prevk = None
            for e in ents:
                fn = fn_parse(e["key"])
                k = (upcase_key(up, fn["name"]), list(fn["units"]))
                if prevk is not None and not prevk < k:
                    E("directory %d: index out of order at '%s'" % (n, fn["name"]))
                prevk = k
                if fn["parent"] & 0xFFFFFFFFFFFF != n:
                    E("directory %d: entry '%s' has parent %d" % (n, fn["name"], fn["parent"] & 0xFFFFFFFFFFFF))
                t = e["file"] & 0xFFFFFFFFFFFF
                tf = files.get(t)
                if tf is None:
                    E("directory %d: entry '%s' -> record %d not in use" % (n, fn["name"], t))
                    continue
                if e["file"] >> 48 != tf["seq"]:
                    E("directory %d: entry '%s' sequence %d, record has %d" % (n, fn["name"], e["file"] >> 48, tf["seq"]))
                match = [x for x in tf["fns"] if x["name"] == fn["name"] and x["parent"] == fn["parent"]]
                if not match:
                    E("directory %d: entry '%s' has no matching $FILE_NAME in record %d" % (n, fn["name"], t))
                else:
                    m = match[0]
                    # (the duplicated sizes and attributes of user files are
                    # kept current lazily by Windows and ntfs-3g: only the
                    # formatter's own files must match exactly)
                    for fld in ("ns", "fattr", "size", "alloc") if t < 27 and t != 5 else ("ns",):
                        if m[fld] != fn[fld]:
                            E("directory %d: entry '%s' %s %r, the file's $FILE_NAME has %r" % (n, fn["name"], fld, fn[fld], m[fld]))
                if t != n:
                    linked[t] = linked.get(t, 0) + 1
    for n, f in files.items():
        if u64(f["rec"], 32):
            continue
        want = linked.get(n, 0) + (1 if n == 5 else 0)
        if f["links"] != want and not (n in (5,) and f["links"] == 1):
            E("record %d: link count %d, %d index entries refer to it" % (n, f["links"], want))
        # $FILE_NAME sizes vs. $DATA (the duplicated information)
        dat = [a for a in f["attrs"] if a.type == 0x80 and not a.name]
        for fn in f["fns"]:
            if f["flags"] & 2:
                if not fn["fattr"] & 0x10000000:
                    E("record %d: directory $FILE_NAME without the index flag" % n)
            elif dat and 0 < n < 27:
                a = dat[0]
                alloc = a.alloc_size if a.nonres else (a.data_size + 7) & ~7
                if fn["size"] != a.data_size or (fn["alloc"] != alloc and n > 26):
                    E("record %d: $FILE_NAME sizes %d/%d, $DATA %d/%d" % (n, fn["size"], fn["alloc"], a.data_size, alloc))
    # $Volume
    if 3 in files:
        vi = [a for a in files[3]["attrs"] if a.type == 0x70]
        if not vi or len(vi[0].value) < 12:
            E("$Volume has no $VOLUME_INFORMATION")
        else:
            v = vi[0].value
            if (v[8], v[9]) not in ((3, 0), (3, 1)):
                E("NTFS version %d.%d" % (v[8], v[9]))
            if u16(v, 10) & 1:
                E("volume is marked dirty")
            vol.version = (v[8], v[9])
            vol.vflags = u16(v, 10)
        vn = [a for a in files[3]["attrs"] if a.type == 0x60]
        vol.label = vn[0].value.decode("utf-16-le") if vn else ""
    # $UpCase
    if len(up) != 65536:
        E("$UpCase has %d entries" % len(up))
    else:
        for c in range(ord("a"), ord("z") + 1):
            if up[c] != c - 32:
                E("$UpCase does not map ASCII")
                break
    # $AttrDef
    ad = vol.stream(4, 0x80)
    if ad is None or len(ad) % 160:
        E("$AttrDef size")
    else:
        types = [u32(ad, i * 160 + 128) for i in range(len(ad) // 160)]
        types = [t for t in types if t]
        if types != sorted(types) or 0x10 not in types or 0x80 not in types:
            E("$AttrDef types %s" % types)
    # $Secure
    check_secure(vol, files, E)
    # $LogFile
    check_logfile(vol, E)
    # $BadClus
    if 8 in files:
        bad = [a for a in files[8]["attrs"] if a.type == 0x80 and a.name == "$Bad"]
        if not bad or not bad[0].nonres:
            E("$BadClus has no non-resident $Bad stream")
        elif bad[0].data_size != vol.clusters * cs:
            E("$BadClus:$Bad size %d, volume %d" % (bad[0].data_size, vol.clusters * cs))
    # $Extend
    if 11 in files:
        try:
            ents, _ = walk_index(vol, 11, "$I30", files[11]["attrs"], errs)
            names = sorted(fn_parse(e["key"])["name"] for e in ents or [])
            for need in ("$ObjId", "$Quota", "$Reparse"):
                if need not in names:
                    E("$Extend has no %s" % need)
        except NtfsError as e:
            E(str(e))
    vol.files = files
    return errs


def check_secure(vol, files, E):
    if 9 not in files:
        return
    attrs = files[9]["attrs"]
    sds = [a for a in attrs if a.type == 0x80 and a.name == "$SDS"]
    if not sds:
        E("$Secure has no $SDS")
        return
    d = vol.attr_data(sds[0])
    ids = {}
    o = 0
    while o + 20 <= len(d):
        if (o & 0x3FFFF) + 20 > 0x40000 or (o // 0x40000) % 2:
            o = (o // 0x40000 + 1) * 0x40000
            if (o // 0x40000) % 2:
                o += 0x40000
            continue
        h, sid, off, ln = struct.unpack_from("<IIQI", d, o)
        if ln == 0 and sid == 0:
            o = (o // 0x40000 + 2) * 0x40000
            continue
        if off != o or ln < 20 or o + ln > len(d):
            E("$SDS entry at %d: offset %d length %d" % (o, off, ln))
            break
        sd = d[o + 20:o + ln]
        if sd_hash(sd) != h:
            E("$SDS entry %d: hash 0x%08x, computed 0x%08x" % (sid, h, sd_hash(sd)))
        why = sd_valid(sd)
        if why:
            E("$SDS entry %d: %s" % (sid, why))
        if d[o + 0x40000:o + 0x40000 + ln] != d[o:o + ln]:
            E("$SDS entry %d: mirror copy differs" % sid)
        ids[sid] = (h, off, ln)
        o = (o + ln + 15) & ~15
    vol.sec_ids = ids
    try:
        errs = []
        sii, info = walk_index(vol, 9, "$SII", attrs, errs)
        sdh, info2 = walk_index(vol, 9, "$SDH", attrs, errs)
        for e in errs:
            E(e)
    except NtfsError as e:
        E(str(e))
        return
    if sii is None or sdh is None:
        E("$Secure lacks $SII or $SDH")
        return
    if info[1] != 0x10 or info2[1] != 0x12 or info[0] or info2[0]:
        E("$SII / $SDH collation or type")
    got = {}
    prev = None
    for e in sii:
        k = u32(e["key"], 0)
        if prev is not None and k <= prev:
            E("$SII out of order")
        prev = k
        h, sid, off, ln = struct.unpack_from("<IIQI", e["raw"], e["doff"])
        got[k] = (h, off, ln)
        if sid != k:
            E("$SII key %d data id %d" % (k, sid))
    if got != ids:
        E("$SII %s != $SDS %s" % (sorted(got), sorted(ids)))
    got = {}
    prev = None
    for e in sdh:
        k = struct.unpack_from("<II", e["key"], 0)
        if prev is not None and k <= prev:
            E("$SDH out of order")
        prev = k
        h, sid, off, ln = struct.unpack_from("<IIQI", e["raw"], e["doff"])
        got[sid] = (h, off, ln)
        if (h, sid) != k:
            E("$SDH key/data mismatch")
    if got != ids:
        E("$SDH %s != $SDS %s" % (sorted(got), sorted(ids)))
    for n, f in files.items():
        si = f["si"]
        if f["base"]:
            continue
        if len(si) >= 72:
            sid = u32(si, 52)
            if sid and sid not in ids:
                E("record %d uses security id %d, not in $Secure" % (n, sid))
        elif not [a for a in f["attrs"] if a.type == 0x50]:
            E("record %d: neither a security id nor a security descriptor" % n)


def check_logfile(vol, E):
    d = vol.stream(2, 0x80)
    if d is None:
        E("no $LogFile")
        return
    if all(x == 0xFF for x in d[:8192]):
        vol.log_state = "empty (all 0xFF)"
        return
    states = []
    for po in (0, 4096):
        p = d[po:po + 4096]
        if p[:4] != b"RSTR":
            E("$LogFile restart page at %d: magic %r" % (po, p[:4]))
            continue
        try:
            sps = u32(p, 16)
            p = fixup(d[po:po + sps], "$LogFile restart page %d" % po)
        except NtfsError as e:
            E(str(e))
            continue
        lps, rao = u32(p, 20), u16(p, 24)
        minor, major = struct.unpack_from("<hh", p, 26)
        if sps not in (4096, 8192, 16384, 65536) or lps != 4096 or (major, minor) not in ((1, 1), (2, 0)):
            E("$LogFile page sizes %d/%d version %d.%d" % (sps, lps, major, minor))
        ra = p[rao:]
        cur, nclients, free, inuse, flags = struct.unpack_from("<QHHHH", ra, 0)
        seqbits = u32(ra, 16)
        ralen, cao = u16(ra, 20), u16(ra, 22)
        fsize = i64(ra, 24)
        if fsize != len(d) and fsize > len(d):
            E("$LogFile restart area file size %d, file is %d" % (fsize, len(d)))
        if cao & 7 or cao + 0xA0 * nclients > ralen or rao + ralen > sps:
            E("$LogFile restart area lengths")
        if free != 0xFFFF and free >= nclients or inuse != 0xFFFF and inuse >= nclients:
            E("$LogFile client lists")
        clean = inuse == 0xFFFF or flags & 2
        if not clean:
            E("$LogFile is not clean")
        if (fsize >> 3) >= 1 << (64 - seqbits):
            E("$LogFile seq_number_bits %d too large for %d bytes" % (seqbits, fsize))
        states.append("clean" if clean else "dirty")
    for pg in range(2, len(d) // 4096):
        m = d[pg * 4096:pg * 4096 + 4]
        if m not in (b"\xff\xff\xff\xff", b"RCRD"):
            E("$LogFile page %d: magic %r" % (pg, m))
            break
    vol.log_state = "restart pages " + "/".join(states)


# ---------------------------------------------------------------- dump

def describe_attr(vol, n, a):
    s = "  0x%02x %-22s" % (a.type, AT_NAMES.get(a.type, "?"))
    if a.name:
        s += " '%s'" % a.name
    s += " %s flags=0x%x" % ("nonres" if a.nonres else "res", a.flags)
    if a.nonres:
        s += " size=%d alloc=%d init=%d runs=%d" % (a.data_size, a.alloc_size, a.init_size, len(a.runs))
        if any(r[1] is None for r in a.runs):
            s += " sparse"
    else:
        s += " len=%d rflags=%d" % (a.data_size, a.resident_flags)
    v = a.value if not a.nonres else None
    if a.type == 0x10 and v:
        s += " fattr=0x%x" % u32(v, 32)
        if len(v) >= 72:
            s += " secid=0x%x" % u32(v, 52)
    elif a.type == 0x30 and v:
        f = fn_parse(v)
        s += " parent=%d name=%s ns=%d fattr=0x%x size=%d alloc=%d" % (
            f["parent"] & 0xFFFFFFFFFFFF, f["name"], f["ns"], f["fattr"], f["size"], f["alloc"])
    elif a.type == 0x70 and v:
        s += " version=%d.%d vflags=0x%x" % (v[8], v[9], u16(v, 10))
    elif a.type == 0x90 and v:
        s += " type=0x%x coll=%d bsize=%d cpb=%d ihflags=%d" % (u32(v, 0), u32(v, 4), u32(v, 8), v[12], v[28])
    return s


def dump(vol):
    out = []
    for n in range(min(vol.nrec, 27)):
        rec = vol.record(n)
        mb = vol.stream(0, 0xB0)
        inbm = (mb[n // 8] >> (n % 8)) & 1
        if rec is None:
            out.append("record %d: no FILE header, bitmap=%d" % (n, inbm))
            continue
        flags = u16(rec, 22)
        line = "record %d: flags=0x%x bitmap=%d seq=%d links=%d base=%d" % (
            n, flags, inbm, u16(rec, 16), u16(rec, 18), u64(rec, 32))
        out.append(line)
        if not flags & 1:
            continue
        for a in vol.parse_attrs(rec, n):
            out.append(describe_attr(vol, n, a))
    return out


def info(vol, errs):
    print("sector %d, cluster %d, MFT record %d, index block %d" % (vol.ss, vol.cs, vol.rec_size, vol.idx_size))
    print("clusters %d (%d bytes), free %d" % (vol.clusters, vol.clusters * vol.cs, getattr(vol, "free_clusters", -1)))
    print("MFT at LCN %d (%d records), mirror at LCN %d" % (vol.mft_lcn, vol.nrec, vol.mirr_lcn))
    lg = [a for a in vol.files.get(2, {}).get("attrs", []) if a.type == 0x80]
    if lg:
        print("$LogFile %d bytes at LCN %d, %s" % (lg[0].data_size, lg[0].runs[0][1], getattr(vol, "log_state", "?")))
    print("label '%s', version %s, flags 0x%x, serial %016X" % (getattr(vol, "label", ""), getattr(vol, "version", "?"),
                                                              getattr(vol, "vflags", 0), vol.serial))
    print("security ids %s" % sorted(getattr(vol, "sec_ids", {})))


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    img, cmd = sys.argv[1], sys.argv[2]
    off = 0
    if "@@" in img:
        img, o = img.split("@@")
        off = int(o, 0)
    try:
        vol = Volume(img, off)
        if cmd == "check":
            errs = check(vol)
            for e in errs[:50]:
                print("ERROR: " + e)
            if len(errs) > 50:
                print("... %d more" % (len(errs) - 50))
            sys.exit(1 if errs else 0)
        elif cmd == "dump":
            print("\n".join(dump(vol)))
        elif cmd == "info":
            errs = check(vol)
            info(vol, errs)
            for e in errs:
                print("ERROR: " + e)
        elif cmd == "upcase":
            sys.stdout.buffer.write(vol.stream(10, 0x80))
        else:
            print(__doc__)
            sys.exit(2)
    except NtfsError as e:
        print("ERROR: " + str(e))
        sys.exit(1)


if __name__ == "__main__":
    main()
