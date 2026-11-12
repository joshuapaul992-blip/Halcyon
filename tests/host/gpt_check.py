#!/usr/bin/env python3
"""Independent checker for the partition tables the installer writes
(kernel/fs/parttab.c), written from the UEFI specification (5.2, 5.3) and
the classic MBR layout, not from the C code. CRC32s with Python's zlib.

usage: gpt_check.py IMAGE [SECTOR_SIZE]      prints one line per partition:
         gpt N START END TYPEGUID UNIQUEGUID NAME     or
         mbr N START SIZE TYPE ACTIVE
       and exits 1 with the problems found (on stderr) if any.
"""
import struct
import sys
import uuid
import zlib


def guid(b):
    return str(uuid.UUID(bytes_le=bytes(b))).upper()


def main():
    path = sys.argv[1]
    ss = int(sys.argv[2]) if len(sys.argv) > 2 else 512
    data = open(path, "rb")
    data.seek(0, 2)
    size = data.tell()
    nsec = size // ss
    problems = []

    def sector(lba, n=1):
        data.seek(lba * ss)
        return data.read(n * ss)

    mbr = sector(0)
    if mbr[510:512] != b"\x55\xaa":
        print("no boot signature", file=sys.stderr)
        sys.exit(1)
    ents = [mbr[446 + 16 * i: 462 + 16 * i] for i in range(4)]
    if any(e[4] == 0xEE for e in ents):
        pm = [e for e in ents if e[4] == 0xEE]
        if len(pm) != 1 or ents[0][4] != 0xEE:
            problems.append("protective MBR: the 0xEE entry must be the first and only one")
        e = pm[0]
        start, cnt = struct.unpack_from("<II", e, 8)
        if start != 1 or cnt != min(nsec - 1, 0xFFFFFFFF):
            problems.append("protective MBR: entry covers %d+%d, not the disk" % (start, cnt))
        if e[0] != 0:
            problems.append("protective MBR: boot indicator set")
        for o in ents[1:]:
            if any(o):
                problems.append("protective MBR: other entries not empty")
        arrays = []
        hdrs = []
        for which, lba in (("primary", 1), ("backup", nsec - 1)):
            h = sector(lba)
            if h[:8] != b"EFI PART":
                problems.append("%s header: no signature at LBA %d" % (which, lba))
                continue
            rev, hsize, crc = struct.unpack_from("<III", h, 8)
            if rev != 0x00010000 or hsize != 92:
                problems.append("%s header: revision %#x size %d" % (which, rev, hsize))
            calc = zlib.crc32(h[:16] + b"\0\0\0\0" + h[20:hsize]) & 0xFFFFFFFF
            if calc != crc:
                problems.append("%s header CRC %08x, computed %08x" % (which, crc, calc))
            if any(h[hsize:ss]):
                problems.append("%s header: bytes after the header not zero" % which)
            my, alt, first, last = struct.unpack_from("<QQQQ", h, 24)
            dguid = h[56:72]
            elba, num, esz, ecrc = struct.unpack_from("<QIII", h, 72)
            if my != lba:
                problems.append("%s header: MyLBA %d" % (which, my))
            if alt != (nsec - 1 if lba == 1 else 1):
                problems.append("%s header: AlternateLBA %d" % (which, alt))
            esec = (num * esz + ss - 1) // ss
            if lba == 1 and not (elba >= 2 and elba + esec <= first):
                problems.append("primary entry array at %d overlaps the usable area (%d)" % (elba, first))
            if lba != 1 and not (elba > last and elba + esec <= nsec - 1):
                problems.append("backup entry array at %d not between the usable area and the header" % elba)
            arr = sector(elba, esec)[:num * esz]
            if zlib.crc32(arr) & 0xFFFFFFFF != ecrc:
                problems.append("%s entry array CRC mismatch" % which)
            arrays.append(arr)
            hdrs.append((first, last, dguid, num, esz))
        if len(arrays) == 2:
            if arrays[0] != arrays[1]:
                problems.append("primary and backup entry arrays differ")
            if hdrs[0] != hdrs[1]:
                problems.append("primary and backup headers differ in usable range, disk GUID or array size")
        if arrays:
            first, last, dguid, num, esz = hdrs[0]
            used = []
            for i in range(num):
                e = arrays[0][i * esz:(i + 1) * esz]
                if not any(e[:16]):
                    if any(e):
                        problems.append("entry %d: unused but not zero" % (i + 1))
                    continue
                s, l, attrs = struct.unpack_from("<QQQ", e, 32)
                name = e[56:128].decode("utf-16-le").split("\0")[0]
                if not (first <= s <= l <= last):
                    problems.append("entry %d: %d-%d outside the usable LBAs %d-%d" % (i + 1, s, l, first, last))
                for (n2, s2, l2) in used:
                    if s <= l2 and s2 <= l:
                        problems.append("entries %d and %d overlap" % (n2, i + 1))
                used.append((i + 1, s, l))
                print("gpt %d %d %d %s %s %s" % (i + 1, s, l, guid(e[:16]), guid(e[16:32]), name or "-"))
    else:
        used = []
        for i, e in enumerate(ents):
            if not e[4]:
                if any(e):
                    problems.append("MBR entry %d: type 0 but not empty" % (i + 1))
                continue
            if e[0] not in (0, 0x80):
                problems.append("MBR entry %d: boot indicator %#x" % (i + 1, e[0]))
            s, n = struct.unpack_from("<II", e, 8)
            if not s or not n or s + n > nsec:
                problems.append("MBR entry %d: %d+%d outside the disk" % (i + 1, s, n))
            for (n2, s2, c2) in used:
                if s < s2 + c2 and s2 < s + n:
                    problems.append("MBR entries %d and %d overlap" % (n2, i + 1))
            used.append((i + 1, s, n))
            print("mbr %d %d %d %02x %d" % (i + 1, s, n, e[4], 1 if e[0] == 0x80 else 0))
        if sum(1 for e in ents if e[0] == 0x80) > 1:
            problems.append("more than one active MBR partition")
    for p in problems:
        print(p, file=sys.stderr)
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
