#!/usr/bin/env python3
"""Make NTFS test images for the host tests (ntfstest.c) and the boot test:
mkntfs formats the image, the ntfs-3g FUSE driver populates it.

usage: ntfs_mkimg.py IMAGE SIZE_MB PROFILE [mkntfs options...]

Profiles:
  basic   files of many sizes (resident, non-resident, empty), a deep tree,
          Unicode names (names differing only in case, letters whose case
          pairs come from $UpCase), hard links, a file with 160 hard links
          (an attribute list), sparse files, LZNT1-compressed files (a
          directory with the compressed attribute, mounted with
          -o compression), symbolic links, alternate data streams, DOS
          names, fragmented files, a directory of 3000 files (a B+ tree
          with index blocks), hidden / read-only / system attributes
  big N   one directory with N files (several index levels)
  tiny    a few files on a small volume (fuzzing seeds)
  empty   nothing (write tests start from it)
  boot    the boot test's volume: HELLO.TXT and a folder

Content is deterministic: data(name, size) below; the C test compares
everything with the image mounted read-only by ntfs-3g, so no manifest is
needed. Needs root (FUSE), mkntfs and ntfs-3g.
"""
import os
import struct
import subprocess
import sys
import tempfile


def data(name, size, text=False):
    """deterministic bytes for a file"""
    seed = 2166136261
    for ch in name.encode():
        seed = ((seed ^ ch) * 16777619) & 0xFFFFFFFF
    if text:
        words = [b"halcyon ", b"ntfs ", b"cluster ", b"record ", b"index ", b"lznt1 ", b"\n", b"compressed "]
        out = bytearray()
        x = seed or 1
        while len(out) < size:
            x ^= (x << 13) & 0xFFFFFFFF
            x ^= x >> 17
            x ^= (x << 5) & 0xFFFFFFFF
            out += words[x & 7]
        return bytes(out[:size])
    out = bytearray(size)
    x = seed or 1
    for i in range(0, size, 4):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        out[i:i + 4] = struct.pack("<I", x)[:min(4, size - i)]
    return bytes(out)


def put(path, size, text=False):
    with open(path, "wb") as f:
        f.write(data(os.path.basename(path), size, text))


def symlink_buf(target, rel):
    t = target.encode("utf-16-le")
    d = struct.pack("<HHHHI", 0, len(t), len(t), len(t), 1 if rel else 0) + t + t
    return struct.pack("<IHH", 0xA000000C, len(d), 0) + d


def junction_buf(target):
    sub, pr = ("\\??\\" + target).encode("utf-16-le"), target.encode("utf-16-le")
    d = struct.pack("<HHHH", 0, len(sub), len(sub) + 2, len(pr)) + sub + b"\0\0" + pr + b"\0\0"
    return struct.pack("<IHH", 0xA0000003, len(d), 0) + d


def reparse_links(m):
    """Windows symbolic links and a junction (reparse points, set through
    ntfs-3g's system.ntfs_reparse_data)"""
    open(os.path.join(m, "win_symlink"), "wb").close()
    os.setxattr(os.path.join(m, "win_symlink"), "system.ntfs_reparse_data", symlink_buf("small.txt", True))
    os.mkdir(os.path.join(m, "win_junction"))
    os.setxattr(os.path.join(m, "win_junction"), "system.ntfs_reparse_data", junction_buf("C:\\Unicode"))
    os.mkdir(os.path.join(m, "win_dirlink"))
    os.setxattr(os.path.join(m, "win_dirlink"), "system.ntfs_reparse_data", symlink_buf("Unicode", True))


def set_attrib(path, bits):
    v = os.getxattr(path, "system.ntfs_attrib_be")
    a = struct.unpack(">I", v)[0] | bits
    os.setxattr(path, "system.ntfs_attrib_be", struct.pack(">I", a))


def fragment(m, dirname, names, holes):
    """files made of many fragments: one-cluster files, every other one
    deleted, the rest of the volume filled, then the files written into
    the holes (the filler is deleted afterwards)"""
    fr = os.path.join(m, dirname)
    os.mkdir(fr)
    st = os.statvfs(m)
    cl = st.f_bsize
    holes = min(holes, st.f_bavail // 6)
    tmp = os.path.join(fr, "tmp")
    os.mkdir(tmp)
    for i in range(2 * holes):
        with open(os.path.join(tmp, "%05d" % i), "wb") as f:
            f.write(b"x" * cl)
    st = os.statvfs(m)
    free = st.f_bavail * st.f_frsize
    filler = os.path.join(fr, "filler")
    with open(filler, "wb") as f:
        left = max(0, free - max(64 * cl, 256 * 1024))
        chunk = b"\1" * (1 << 20)
        while left > 0:
            n = min(left, len(chunk))
            f.write(chunk[:n])
            left -= n
    for i in range(0, 2 * holes, 2):
        os.unlink(os.path.join(tmp, "%05d" % i))
    per = holes * cl * 4 // 5 // len(names)
    for n in names:
        try:
            put(os.path.join(fr, n), per - 100)
        except OSError:         # small clusters: ntfs-3g may need more room for metadata
            pass
    os.unlink(filler)
    for i in range(1, 2 * holes, 2):
        os.unlink(os.path.join(tmp, "%05d" % i))
    os.rmdir(tmp)


def profile_basic(m):
    for n, sz in [("empty.txt", 0), ("one.bin", 1), ("small.txt", 600), ("resident_max.bin", 700),
                  ("page.bin", 4096), ("medium.bin", 50000), ("large.bin", 3 * 1024 * 1024 + 123)]:
        put(os.path.join(m, n), sz)
    # deep tree
    p = m
    for i in range(14):
        p = os.path.join(p, "level%02d" % i)
        os.mkdir(p)
        put(os.path.join(p, "file%02d.dat" % i), 100 * i + 7)
    # Unicode names
    u = os.path.join(m, "Unicode")
    os.mkdir(u)
    for n in ["Grüße.txt", "日本語のファイル.txt", "Ελληνικά.txt", "Кириллица.txt", "emoji_\U0001F600.txt",
              "a.txt", "A.txt", "Straße", "ÄÖÜ.txt", "ωμέγα.txt", "Ǆ-digraph.txt", "space name .txt.x"]:
        put(os.path.join(u, n), len(n) * 10)
    # hard links
    h = os.path.join(m, "Links")
    os.mkdir(h)
    os.mkdir(os.path.join(h, "other"))
    put(os.path.join(h, "original.txt"), 5000)
    os.link(os.path.join(h, "original.txt"), os.path.join(h, "second.txt"))
    os.link(os.path.join(h, "original.txt"), os.path.join(h, "other", "third.txt"))
    many = os.path.join(m, "ManyLinks")
    os.mkdir(many)
    put(os.path.join(many, "target.bin"), 20000)
    for i in range(160):
        os.link(os.path.join(many, "target.bin"), os.path.join(many, "link_with_a_long_name_%03d.bin" % i))
    # sparse files
    s = os.path.join(m, "Sparse")
    os.mkdir(s)
    with open(os.path.join(s, "hole_end.bin"), "wb") as f:
        f.seek(10 * 1024 * 1024)
        f.write(b"end")
    with open(os.path.join(s, "holes.bin"), "wb") as f:
        d = data("holes.bin", 65536)
        f.write(d)
        f.seek(2 * 1024 * 1024)
        f.write(d)
        f.seek(5 * 1024 * 1024 + 1234)
        f.write(d[:999])
    # compressed files
    c = os.path.join(m, "Compressed")
    os.mkdir(c)
    set_attrib(c, 0x800)
    put(os.path.join(c, "text.txt"), 300000, text=True)
    put(os.path.join(c, "random.bin"), 100000)
    put(os.path.join(c, "small.txt"), 2000, text=True)
    with open(os.path.join(c, "mixed.bin"), "wb") as f:
        f.write(data("mixed.bin", 70000, text=True))
        f.write(bytes(200000))
        f.write(data("mixed2", 50000))
        f.write(data("mixed3", 33333, text=True))
    # symbolic links: Interix style (ntfs-3g's default: system files) and reparse points
    reparse_links(m)
    os.symlink("small.txt", os.path.join(m, "link_to_small"))
    os.symlink("Unicode", os.path.join(m, "link_to_dir"))
    os.symlink("/nowhere/at/all", os.path.join(m, "link_absolute"))
    # alternate data streams (mounted with streams_interface=windows)
    put(os.path.join(m, "with_streams.txt"), 1234)
    with open(os.path.join(m, "with_streams.txt:secret"), "wb") as f:
        f.write(b"hidden stream data" * 100)
    with open(os.path.join(m, "with_streams.txt:big"), "wb") as f:
        f.write(data("bigstream", 100000))
    # DOS names
    d = os.path.join(m, "DosNames")
    os.mkdir(d)
    put(os.path.join(d, "A long file name.txt"), 333)
    os.setxattr(os.path.join(d, "A long file name.txt"), "system.ntfs_dos_name", b"ALONGF~1.TXT")
    os.mkdir(os.path.join(d, "Program Files"))
    os.setxattr(os.path.join(d, "Program Files"), "system.ntfs_dos_name", b"PROGRA~1")
    # a large directory
    big = os.path.join(m, "BigDir")
    os.mkdir(big)
    for i in range(3000):
        put(os.path.join(big, "entry number %05d with padding.txt" % i), i % 50)
    # attributes
    put(os.path.join(m, "hidden.txt"), 10)
    set_attrib(os.path.join(m, "hidden.txt"), 0x2)
    put(os.path.join(m, "readonly.txt"), 10)
    set_attrib(os.path.join(m, "readonly.txt"), 0x1)
    os.mkdir(os.path.join(m, "System Volume Information"))
    set_attrib(os.path.join(m, "System Volume Information"), 0x6)
    os.mkdir(os.path.join(m, "EmptyDir"))
    fragment(m, "Fragmented", ["frag_a.bin", "frag_b.bin"], 2000)


def profile_big(m, n):
    big = os.path.join(m, "Huge")
    os.mkdir(big)
    for i in range(n):
        fd = os.open(os.path.join(big, "f%06d_%s.txt" % (i, "x" * (i % 23))), os.O_CREAT | os.O_WRONLY, 0o644)
        os.close(fd)


def profile_tiny(m):
    put(os.path.join(m, "a.txt"), 100)
    put(os.path.join(m, "b.bin"), 9000)
    os.mkdir(os.path.join(m, "dir"))
    put(os.path.join(m, "dir", "c.txt"), 50)
    os.symlink("a.txt", os.path.join(m, "lnk"))
    reparse_links(m)
    for i in range(40):
        put(os.path.join(m, "dir", "n%02d.txt" % i), 3)


def profile_boot(m):
    with open(os.path.join(m, "HELLO.TXT"), "wb") as f:
        f.write(b"Hello from NTFS!\r\n")
    os.mkdir(os.path.join(m, "Folder"))
    put(os.path.join(m, "Folder", "data.bin"), 100000)
    os.makedirs(os.path.join(m, "Windows", "System32"))


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(2)
    img, size_mb, prof = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    rest = sys.argv[4:]
    count = 0
    if prof == "big":
        count = int(rest[0])
        rest = rest[1:]
    with open(img, "wb") as f:
        f.truncate(size_mb * 1024 * 1024)
    subprocess.run(["mkntfs", "-F", "-q", "-Q", "-L", "Halcyon Test"] + rest + [img], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if prof == "empty":
        return
    mnt = tempfile.mkdtemp(prefix="ntfsimg")
    opts = "compression,streams_interface=windows"
    subprocess.run(["ntfs-3g", "-o", opts, img, mnt], check=True)
    try:
        if prof == "basic":
            profile_basic(mnt)
        elif prof == "big":
            profile_big(mnt, count)
        elif prof == "tiny":
            profile_tiny(mnt)
        elif prof == "boot":
            profile_boot(mnt)
        else:
            raise SystemExit("unknown profile " + prof)
    finally:
        subprocess.run(["fusermount", "-u", mnt], check=True)
        os.rmdir(mnt)


if __name__ == "__main__":
    main()
