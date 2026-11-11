#!/usr/bin/env python3
"""exfat_tool.py - a small, independent exFAT implementation used to cross-check
Halcyon's exFAT driver (kernel/fs/exfat.c). It is written from the Microsoft
exFAT specification and shares no code with the driver.

usage: exfat_tool.py IMAGE[@@OFFSET] COMMAND [ARGS]

  info                      label, geometry, flags (VolumeDirty, ActiveFat), free clusters
  ls [-l] [PATH]            list a tree recursively: "D path" / "F size path"
                            (-l adds attributes, ValidDataLength, NoFatChain, first cluster)
  get PATH OUTFILE          extract a file ("-" = stdout)
  mkdir [-p] PATH           create a directory (contiguous, NoFatChain)
  put HOSTFILE PATH [--vdl N] [--chain] [--frag] [--attr A] [--benign]
                            add a file. Default: contiguous NoFatChain clusters.
                            --vdl N   ValidDataLength N < size (bytes past N are
                                      filled with 0xA5 on disk: undefined data)
                            --chain   FAT chain instead of NoFatChain
                            --frag    FAT chain over every other free cluster
                            --benign  add a Vendor Extension entry (benign secondary)
  rm PATH                   delete a file or an empty directory
  check                     verify the whole volume (boot checksums, up-case table,
                            entry sets, name hashes, bitmap vs. allocations,
                            cross-links, directory sizes); exit status 1 on errors

Paths use '/' and are matched case-insensitively with the volume's up-case table.
"""
import os
import struct
import sys
import time

EOC = 0xFFFFFFFF
BAD = 0xFFFFFFF7


class ExfatError(Exception):
    pass


def boot_checksum(data, ss):
    """checksum of the first 11 sectors of a boot region (VolumeFlags and
    PercentInUse excluded)"""
    c = 0
    for i, b in enumerate(data[:11 * ss]):
        if i in (106, 107, 112):
            continue
        c = (((c & 1) << 31) | (c >> 1)) + b
        c &= 0xFFFFFFFF
    return c


def table_checksum(data):
    c = 0
    for b in data:
        c = ((((c & 1) << 31) | (c >> 1)) + b) & 0xFFFFFFFF
    return c


def set_checksum(entries):
    c = 0
    for i, b in enumerate(entries):
        if i in (2, 3):
            continue
        c = ((((c & 1) << 15) | (c >> 1)) + b) & 0xFFFF
    return c


def timestamp_now():
    t = time.localtime()
    ts = ((t.tm_year - 1980) << 25) | (t.tm_mon << 21) | (t.tm_mday << 16) | \
         (t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2)
    return ts, (t.tm_sec % 2) * 100


class Entry:
    """a parsed File directory entry set"""

    def __init__(self):
        self.name = ""
        self.units = []
        self.attr = 0
        self.first = 0
        self.size = 0
        self.vdl = 0
        self.nofat = False
        self.offsets = []       # byte offsets (in the volume) of every entry of the set
        self.raw = b""
        self.secondary_flags = 0
        self.name_hash = 0
        self.benign = []        # raw benign secondary entries

    @property
    def is_dir(self):
        return bool(self.attr & 0x10)


class Volume:
    def __init__(self, spec, write=False):
        path, off = spec, 0
        if "@@" in spec:
            path, o = spec.rsplit("@@", 1)
            off = int(o, 0)
        self.f = open(path, "r+b" if write else "rb")
        self.base = off
        self.errors = []
        b = self.rd(0, 512)
        if b[3:11] != b"EXFAT   ":
            raise ExfatError("not an exFAT volume")
        if any(b[11:64]):
            raise ExfatError("MustBeZero field not zero")
        (self.part_off, self.vol_len, self.fat_off, self.fat_len, self.heap_off, self.nclus,
         self.root_clus, self.serial, self.rev, self.flags, self.ss_shift, self.spc_shift,
         self.nfats, _drive, self.pct) = struct.unpack_from("<QQIIIIIIHHBBBBB", b, 64)
        if not 9 <= self.ss_shift <= 12 or self.spc_shift > 25 - self.ss_shift:
            raise ExfatError("bad sector / cluster shift")
        if self.nfats not in (1, 2):
            raise ExfatError("bad NumberOfFats")
        if b[510:512] != b"\x55\xaa":
            raise ExfatError("no boot signature")
        self.ss = 1 << self.ss_shift
        self.csize = self.ss << self.spc_shift
        region = self.rd(0, 12 * self.ss)
        want = boot_checksum(region, self.ss)
        for i in range(0, self.ss, 4):
            if struct.unpack_from("<I", region, 11 * self.ss + i)[0] != want:
                self.errors.append("main boot region checksum mismatch")
                break
        for s in range(1, 9):
            if region[(s + 1) * self.ss - 4:(s + 1) * self.ss] != b"\x00\x00\x55\xaa":
                self.errors.append("extended boot sector %d: bad signature" % s)
        if self.fat_off < 24 or self.heap_off < self.fat_off + self.fat_len * self.nfats:
            raise ExfatError("bad FAT / cluster heap layout")
        if self.fat_len * self.ss < (self.nclus + 2) * 4:
            raise ExfatError("FAT too small")
        if self.heap_off + (self.nclus << self.spc_shift) > self.vol_len:
            raise ExfatError("cluster heap exceeds the volume")
        if not 2 <= self.root_clus < self.nclus + 2:
            raise ExfatError("bad root cluster")
        self.active = (self.flags & 1) if self.nfats == 2 else 0
        self.fat_base = (self.fat_off + self.active * self.fat_len) * self.ss
        self.fat = bytearray(self.rd(self.fat_base, (self.nclus + 2) * 4))
        self.fat_dirty = False
        # root directory: allocation bitmap, up-case table, label
        self.label = ""
        self.bitmap_ent = None
        self.upcase_ent = None
        self.upcase = None
        root = self.chain(self.root_clus, False, None)
        self.root_clusters = root
        for off in self.dir_offsets(root):
            e = self.rd(off, 32)
            t = e[0]
            if t == 0:
                break
            if t == 0x81 and (e[1] & 1) == self.active and self.bitmap_ent is None:
                self.bitmap_ent = (off, struct.unpack_from("<IQ", e, 20))
            elif t == 0x82 and self.upcase_ent is None:
                self.upcase_ent = (off, struct.unpack_from("<I", e, 4)[0], struct.unpack_from("<IQ", e, 20))
            elif t == 0x83:
                n = e[1]
                if n > 11:
                    self.errors.append("volume label too long")
                    n = 11
                self.label = e[2:2 + 2 * n].decode("utf-16-le", "replace")
        if self.bitmap_ent is None:
            raise ExfatError("no allocation bitmap entry")
        bfirst, blen = self.bitmap_ent[1]
        if blen < (self.nclus + 7) // 8:
            raise ExfatError("allocation bitmap too small")
        self.bitmap_clusters = self.chain(bfirst, False, blen)
        self.bitmap = bytearray(self.read_clusters(self.bitmap_clusters, blen))
        self.bitmap_dirty = False
        if self.upcase_ent is None:
            raise ExfatError("no up-case table entry")
        _uoff, ucsum, (ufirst, ulen) = self.upcase_ent
        self.upcase_clusters = self.chain(ufirst, False, ulen)
        raw = self.read_clusters(self.upcase_clusters, ulen)
        if table_checksum(raw) != ucsum:
            self.errors.append("up-case table checksum mismatch")
        self.upcase = self.decompress_upcase(raw)

    # ---- I/O ----
    def rd(self, off, n):
        self.f.seek(self.base + off)
        d = self.f.read(n)
        if len(d) != n:
            raise ExfatError("read past the end of the image at %#x" % off)
        return d

    def wr(self, off, data):
        self.f.seek(self.base + off)
        self.f.write(data)

    def clus_off(self, c):
        return self.heap_off * self.ss + (c - 2) * self.csize

    def read_clusters(self, clusters, length):
        out = bytearray()
        for c in clusters:
            out += self.rd(self.clus_off(c), self.csize)
        return bytes(out[:length])

    # ---- FAT / bitmap ----
    def fat_get(self, c):
        return struct.unpack_from("<I", self.fat, 4 * c)[0]

    def fat_set(self, c, v):
        struct.pack_into("<I", self.fat, 4 * c, v)
        self.fat_dirty = True

    def chain(self, first, nofat, size):
        """cluster list of an allocation; size None = follow the FAT to its end"""
        if first == 0:
            if size:
                raise ExfatError("allocation of %d bytes without clusters" % size)
            return []
        if not 2 <= first < self.nclus + 2:
            raise ExfatError("cluster %d out of range" % first)
        if nofat:
            n = (size + self.csize - 1) // self.csize
            if first + n > self.nclus + 2:
                raise ExfatError("contiguous allocation past the cluster heap")
            return list(range(first, first + n))
        out = [first]
        c = first
        limit = None if size is None else (size + self.csize - 1) // self.csize
        while limit is None or len(out) < limit:
            n = self.fat_get(c)
            if n == EOC:
                break
            if not 2 <= n < self.nclus + 2:
                raise ExfatError("bad FAT entry %#x at cluster %d" % (n, c))
            out.append(n)
            if len(out) > self.nclus:
                raise ExfatError("FAT chain loop at cluster %d" % first)
            c = n
        if limit is not None and len(out) < limit:
            raise ExfatError("FAT chain shorter than the allocation (first %d)" % first)
        return out

    def is_free(self, c):
        i = c - 2
        return not (self.bitmap[i >> 3] >> (i & 7)) & 1

    def set_used(self, c, used):
        i = c - 2
        if used:
            self.bitmap[i >> 3] |= 1 << (i & 7)
        else:
            self.bitmap[i >> 3] &= ~(1 << (i & 7)) & 0xFF
        self.bitmap_dirty = True

    def free_count(self):
        return sum(1 for c in range(2, self.nclus + 2) if self.is_free(c))

    def find_run(self, n):
        run, start = 0, 2
        for c in range(2, self.nclus + 2):
            if self.is_free(c):
                if run == 0:
                    start = c
                run += 1
                if run == n:
                    return start
            else:
                run = 0
        return None

    def flush(self):
        if self.fat_dirty:
            self.wr(self.fat_base, bytes(self.fat))
            self.fat_dirty = False
        if self.bitmap_dirty:
            data = bytes(self.bitmap)
            for i, c in enumerate(self.bitmap_clusters):
                chunk = data[i * self.csize:(i + 1) * self.csize]
                self.wr(self.clus_off(c), chunk)
            self.bitmap_dirty = False
        self.f.flush()

    # ---- names ----
    @staticmethod
    def decompress_upcase(raw):
        words = struct.unpack("<%dH" % (len(raw) // 2), raw[:len(raw) // 2 * 2])
        table = []
        i = 0
        while i < len(words) and len(table) < 0x10000:
            w = words[i]
            if w == 0xFFFF and i + 1 < len(words):
                table.extend(range(len(table), len(table) + words[i + 1]))
                i += 2
            else:
                table.append(w)
                i += 1
        return table

    def up(self, u):
        return self.upcase[u] if u < len(self.upcase) else u

    def name_hash(self, units):
        h = 0
        for u in units:
            u = self.up(u)
            for b in (u & 0xFF, u >> 8):
                h = ((((h & 1) << 15) | (h >> 1)) + b) & 0xFFFF
        return h

    def key(self, units):
        return tuple(self.up(u) for u in units)

    @staticmethod
    def units_of(name):
        d = name.encode("utf-16-le")
        return list(struct.unpack("<%dH" % (len(d) // 2), d))

    # ---- directories ----
    def dir_offsets(self, clusters):
        per = self.csize // 32
        for c in clusters:
            base = self.clus_off(c)
            for i in range(per):
                yield base + 32 * i

    def dir_clusters(self, ent):
        if ent is None:
            return self.chain(self.root_clus, False, None)
        return self.chain(ent.first, ent.nofat, ent.size)

    def read_dir(self, ent, strict=False):
        offs = list(self.dir_offsets(self.dir_clusters(ent)))
        raw = [self.rd(o, 32) for o in offs]
        out = []
        i = 0
        while i < len(raw):
            e = raw[i]
            t = e[0]
            if t == 0:
                for j in range(i + 1, len(raw)):
                    if raw[j][0] != 0 and strict:
                        self.errors.append("entry in use after the end-of-directory marker")
                        break
                break
            if t != 0x85:
                if strict and t & 0x80 and not t & 0x40 and t not in (0x81, 0x82, 0x83, 0xA0, 0xA1, 0xA2):
                    self.errors.append("unknown primary entry type %#x" % t)
                i += 1
                continue
            sc = e[1]
            if sc < 2 or sc > 18 or i + sc >= len(raw):
                self.errors.append("file entry with SecondaryCount %d" % sc)
                i += 1
                continue
            ents = raw[i:i + sc + 1]
            data = b"".join(ents)
            if set_checksum(data) != struct.unpack_from("<H", data, 2)[0]:
                self.errors.append("entry set checksum mismatch at %#x" % offs[i])
                i += 1
                continue
            st = ents[1]
            if st[0] != 0xC0:
                self.errors.append("first secondary is not a stream extension")
                i += 1
                continue
            x = Entry()
            x.attr = struct.unpack_from("<H", e, 4)[0]
            x.secondary_flags = st[1]
            x.nofat = bool(st[1] & 2)
            nlen = st[3]
            x.name_hash = struct.unpack_from("<H", st, 4)[0]
            x.vdl, x.first, x.size = struct.unpack_from("<Q4xIQ", st, 8)
            nent = (nlen + 14) // 15
            if nlen == 0 or 1 + nent > sc:
                self.errors.append("bad NameLength %d" % nlen)
                i += 1
                continue
            units = []
            for k in range(nent):
                ne = ents[2 + k]
                if ne[0] != 0xC1:
                    self.errors.append("missing file name entry")
                units += struct.unpack_from("<15H", ne, 2)
            x.units = units[:nlen]
            x.name = struct.pack("<%dH" % nlen, *x.units).decode("utf-16-le", "replace")
            for k in range(2 + nent, sc + 1):
                if not ents[k][0] & 0x20:
                    self.errors.append("unknown critical secondary %#x" % ents[k][0])
                x.benign.append(ents[k])
            x.offsets = offs[i:i + sc + 1]
            x.raw = data
            if strict:
                if self.name_hash(x.units) != x.name_hash:
                    self.errors.append("name hash mismatch for '%s'" % x.name)
                if any(u < 0x20 or chr(u) in '"*/:<>?\\|' for u in x.units):
                    self.errors.append("illegal character in '%s'" % x.name)
                if x.vdl > x.size:
                    self.errors.append("'%s': ValidDataLength > DataLength" % x.name)
                if x.is_dir and (x.size == 0 or x.size % self.csize or x.vdl != x.size):
                    self.errors.append("directory '%s': bad size %d / %d" % (x.name, x.size, x.vdl))
            out.append(x)
            i += sc + 1
        return out

    def lookup(self, path):
        """(parent entry or None for root, entry or None for root); raises if missing"""
        parent, cur = None, None
        for comp in [p for p in path.split("/") if p]:
            if cur is not None and not cur.is_dir:
                raise ExfatError("not a directory: %s" % path)
            k = self.key(self.units_of(comp))
            found = None
            for x in self.read_dir(cur):
                if self.key(x.units) == k:
                    found = x
                    break
            if found is None:
                raise ExfatError("not found: %s" % path)
            parent, cur = cur, found
        return parent, cur

    def find_dir(self, path):
        _p, d = self.lookup(path)
        if d is not None and not d.is_dir:
            raise ExfatError("not a directory: %s" % path)
        return d

    def read_file(self, ent):
        cl = self.chain(ent.first, ent.nofat, ent.size)
        data = bytearray(self.read_clusters(cl, ent.size))
        data[ent.vdl:] = bytes(len(data) - ent.vdl)
        return bytes(data)

    # ---- modification ----
    def alloc(self, n, contiguous=True, frag=False):
        """allocate n clusters; returns (first, nofat, list)"""
        if n == 0:
            return 0, False, []
        if contiguous:
            s = self.find_run(n)
            if s is None:
                raise ExfatError("no contiguous run of %d clusters" % n)
            cl = list(range(s, s + n))
            for c in cl:
                self.set_used(c, True)
            return s, True, cl
        free = [c for c in range(2, self.nclus + 2) if self.is_free(c)]
        if frag:
            free = free[::2] + free[1::2]
        if len(free) < n:
            raise ExfatError("disk full")
        cl = free[:n]
        for i, c in enumerate(cl):
            self.set_used(c, True)
            self.fat_set(c, cl[i + 1] if i + 1 < n else EOC)
        return cl[0], False, cl

    def write_set(self, ent):
        data = bytearray(ent.raw)
        struct.pack_into("<H", data, 2, set_checksum(data))
        ent.raw = bytes(data)
        for k, off in enumerate(ent.offsets):
            self.wr(off, ent.raw[32 * k:32 * k + 32])

    def grow_dir(self, d):
        """add one zeroed cluster to directory d (None = root)"""
        if d is None:
            cl = self.chain(self.root_clus, False, None)
            new = next((c for c in range(2, self.nclus + 2) if self.is_free(c)), None)
            if new is None:
                raise ExfatError("disk full")
            self.set_used(new, True)
            self.fat_set(cl[-1], new)
            self.fat_set(new, EOC)
        else:
            cl = self.chain(d.first, d.nofat, d.size)
            nxt = cl[-1] + 1
            if d.nofat and nxt < self.nclus + 2 and self.is_free(nxt):
                new = nxt
                self.set_used(new, True)
            else:
                if d.nofat:
                    for i, c in enumerate(cl):
                        self.fat_set(c, cl[i + 1] if i + 1 < len(cl) else EOC)
                    d.nofat = False
                new = next((c for c in range(2, self.nclus + 2) if self.is_free(c)), None)
                if new is None:
                    raise ExfatError("disk full")
                self.set_used(new, True)
                self.fat_set(cl[-1], new)
                self.fat_set(new, EOC)
            d.size += self.csize
            d.vdl = d.size
            raw = bytearray(d.raw)
            raw[32 + 1] = (raw[32 + 1] & ~2 & 0xFF) | (2 if d.nofat else 0)
            struct.pack_into("<Q", raw, 32 + 8, d.vdl)
            struct.pack_into("<Q", raw, 32 + 24, d.size)
            d.raw = bytes(raw)
            self.write_set(d)
        self.wr(self.clus_off(new), bytes(self.csize))

    def add_entry(self, parent, name, attr, first, nofat, size, vdl, benign=False):
        units = self.units_of(name)
        if not 1 <= len(units) <= 255 or any(u < 0x20 or chr(u) in '"*/:<>?\\|' for u in units) \
                or name in (".", ".."):
            raise ExfatError("invalid name: %r" % name)
        k = self.key(units)
        if any(self.key(x.units) == k for x in self.read_dir(parent)):
            raise ExfatError("exists: %s" % name)
        nent = (len(units) + 14) // 15
        count = 2 + nent + (1 if benign else 0)
        ts, ms = timestamp_now()
        fe = bytearray(32)
        fe[0] = 0x85
        fe[1] = count - 1
        struct.pack_into("<HHIII", fe, 4, attr, 0, ts, ts, ts)
        fe[20] = ms
        fe[21] = ms
        st = bytearray(32)
        st[0] = 0xC0
        st[1] = 1 | (2 if nofat else 0)
        st[3] = len(units)
        struct.pack_into("<H", st, 4, self.name_hash(units))
        struct.pack_into("<Q4xIQ", st, 8, vdl, first, size)
        raw = bytes(fe) + bytes(st)
        padded = units + [0] * (nent * 15 - len(units))
        for i in range(nent):
            ne = bytearray(32)
            ne[0] = 0xC1
            struct.pack_into("<15H", ne, 2, *padded[15 * i:15 * i + 15])
            raw += bytes(ne)
        if benign:
            ve = bytearray(32)
            ve[0] = 0xE0
            ve[2:18] = b"HalcyonTestGUID!"
            ve[18:32] = b"vendor payload"
            raw += bytes(ve)
        while True:
            offs = list(self.dir_offsets(self.dir_clusters(parent)))
            run = []
            for o in offs:
                if self.rd(o, 1)[0] < 0x80:
                    run.append(o)
                    if len(run) == count:
                        break
                else:
                    run = []
            if len(run) == count:
                break
            self.grow_dir(parent)
        x = Entry()
        x.raw = raw
        x.offsets = run
        self.write_set(x)

    def put(self, host, path, vdl=None, chain=False, frag=False, attr=0x20, benign=False):
        data = open(host, "rb").read()
        parent_path, _, name = path.rstrip("/").rpartition("/")
        parent = self.find_dir(parent_path)
        n = (len(data) + self.csize - 1) // self.csize
        first, nofat, cl = self.alloc(n, contiguous=not (chain or frag), frag=frag)
        if vdl is None:
            vdl = len(data)
        disk = bytearray(data) + bytes(n * self.csize - len(data))
        for i in range(vdl, len(disk)):
            disk[i] = 0xA5      # undefined data past ValidDataLength
        for i, c in enumerate(cl):
            self.wr(self.clus_off(c), bytes(disk[i * self.csize:(i + 1) * self.csize]))
        self.add_entry(parent, name, attr, first, nofat, len(data), vdl, benign)
        self.flush()

    def mkdir(self, path, parents=False):
        comps = [p for p in path.split("/") if p]
        cur = ""
        for i, comp in enumerate(comps):
            sub = (cur + "/" + comp) if cur else comp
            try:
                d = self.find_dir(sub)
                if i == len(comps) - 1 and not parents:
                    raise ExfatError("exists: %s" % path)
            except ExfatError as e:
                if "not found" not in str(e):
                    raise
                parent = self.find_dir(cur)
                first, _nofat, cl = self.alloc(1)
                self.wr(self.clus_off(cl[0]), bytes(self.csize))
                self.add_entry(parent, comp, 0x10, first, True, self.csize, self.csize)
                self.flush()
            cur = sub

    def rm(self, path):
        _parent, ent = self.lookup(path)
        if ent is None:
            raise ExfatError("cannot remove the root")
        if ent.is_dir and self.read_dir(ent):
            raise ExfatError("directory not empty")
        for off in ent.offsets:
            t = self.rd(off, 1)[0]
            self.wr(off, bytes([t & 0x7F]))
        for c in self.chain(ent.first, ent.nofat, ent.size):
            self.set_used(c, False)
        self.flush()

    # ---- whole-volume check ----
    def check(self):
        owner = {}

        def claim(clusters, what):
            for c in clusters:
                if c in owner:
                    self.errors.append("cluster %d cross-linked: %s and %s" % (c, owner[c], what))
                owner[c] = what

        claim(self.root_clusters, "root directory")
        claim(self.bitmap_clusters, "allocation bitmap")
        claim(self.upcase_clusters, "up-case table")
        if len(self.upcase) < 128:
            self.errors.append("up-case table too short")
        # the mandatory part of the up-case table maps a-z to A-Z
        for u in range(ord("a"), ord("z") + 1):
            if self.up(u) != u - 32:
                self.errors.append("up-case table does not map %s" % chr(u))
                break
        todo = [("", None)]
        ndirs = nfiles = 0
        while todo:
            path, d = todo.pop()
            ndirs += 1
            names = set()
            for x in self.read_dir(d, strict=True):
                p = path + "/" + x.name
                k = self.key(x.units)
                if k in names:
                    self.errors.append("duplicate name %s" % p)
                names.add(k)
                try:
                    cl = self.chain(x.first, x.nofat, x.size)
                except ExfatError as e:
                    self.errors.append("%s: %s" % (p, e))
                    continue
                claim(cl, p)
                if x.is_dir:
                    todo.append((p, x))
                else:
                    nfiles += 1
        for c in range(2, self.nclus + 2):
            used = not self.is_free(c)
            if used and c not in owner:
                self.errors.append("cluster %d marked used but not allocated" % c)
            elif not used and c in owner:
                self.errors.append("cluster %d allocated to %s but marked free" % (c, owner[c]))
        return ndirs, nfiles


def main(argv):
    if len(argv) < 3:
        sys.stderr.write(__doc__)
        return 2
    img, cmd, args = argv[1], argv[2], argv[3:]
    write = cmd in ("mkdir", "put", "rm")
    try:
        v = Volume(img, write)
        if cmd == "info":
            print("label: %s" % v.label)
            print("sector size: %d" % v.ss)
            print("cluster size: %d" % v.csize)
            print("clusters: %d" % v.nclus)
            print("free clusters: %d" % v.free_count())
            print("fats: %d active: %d" % (v.nfats, v.active))
            print("flags: %#x" % v.flags)
            print("dirty: %d" % ((v.flags >> 1) & 1))
            print("percent in use: %d" % v.pct)
        elif cmd == "ls":
            long = "-l" in args
            args = [a for a in args if a != "-l"]
            start = args[0] if args else ""
            d = v.find_dir(start)
            todo = [(start.strip("/"), d)]
            while todo:
                path, d = todo.pop(0)
                for x in v.read_dir(d):
                    p = (path + "/" + x.name) if path else x.name
                    extra = ""
                    if long:
                        extra = " attr=%#x vdl=%d nofat=%d first=%d benign=%s" % (
                            x.attr, x.vdl, x.nofat, x.first,
                            ",".join(b[2:32].decode("latin-1") for b in x.benign) or "-")
                    if x.is_dir:
                        print("D %s%s" % (p, extra))
                        todo.append((p, x))
                    else:
                        print("F %d %s%s" % (x.size, p, extra))
        elif cmd == "get":
            _p, e = v.lookup(args[0])
            if e is None or e.is_dir:
                raise ExfatError("not a file: %s" % args[0])
            data = v.read_file(e)
            if args[1] == "-":
                sys.stdout.buffer.write(data)
            else:
                open(args[1], "wb").write(data)
        elif cmd == "mkdir":
            parents = "-p" in args
            args = [a for a in args if a != "-p"]
            v.mkdir(args[0], parents)
        elif cmd == "put":
            host, path = args[0], args[1]
            opts = args[2:]
            vdl = None
            attr = 0x20
            if "--vdl" in opts:
                vdl = int(opts[opts.index("--vdl") + 1], 0)
            if "--attr" in opts:
                attr = int(opts[opts.index("--attr") + 1], 0)
            v.put(host, path, vdl, "--chain" in opts, "--frag" in opts, attr, "--benign" in opts)
        elif cmd == "rm":
            v.rm(args[0])
        elif cmd == "check":
            nd, nf = v.check()
            if v.errors:
                for e in v.errors:
                    print("error: %s" % e)
                return 1
            print("ok: %d directories, %d files, %d free clusters" % (nd, nf, v.free_count()))
        else:
            sys.stderr.write(__doc__)
            return 2
    except (ExfatError, OSError) as e:
        print("exfat_tool: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
