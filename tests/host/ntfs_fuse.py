#!/usr/bin/env python3
"""ntfs_fuse.py - exercise an NTFS image through the ntfs-3g FUSE driver
(used by ntfsfmt.c on volumes made by Halcyon's formatter).

usage: ntfs_fuse.py IMAGE MOUNTPOINT [FILES] [SEED]

Mounts the image, creates a tree of directories with FILES files of
pseudo-random sizes (0 bytes to a few hundred KiB, a few of several MiB,
names with mixed case and non-ASCII characters, one directory large enough
to need a multi-level index), deletes and renames a share of them, writes
some files again, unmounts, checks the image with ntfsfix -n, mounts it a
second time and verifies every file and directory against the expected
state (contents by SHA-256), and deletes most of the tree again, unmounting
at the end. Exit status 0 on success; failures are printed.
"""
import hashlib
import os
import random
import subprocess
import sys


def run(cmd):
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return p.returncode, p.stdout


def data_for(seed, size):
    return random.Random(seed).randbytes(size)


def mount(img, mnt):
    rc, out = run(["ntfs-3g", img, mnt])
    if rc:
        raise SystemExit("ntfs-3g mount failed (%d): %s" % (rc, out.strip()))


def umount(mnt):
    rc, out = run(["fusermount", "-u", mnt])
    if rc:
        raise SystemExit("fusermount -u failed: %s" % out.strip())


def ntfsfix(img, what):
    rc, out = run(["ntfsfix", "-n", img])
    if rc or "processed successfully" not in out:
        print("FAIL ntfsfix -n %s:\n%s" % (what, out))
        return 1
    return 0


def main():
    img, mnt = sys.argv[1], sys.argv[2]
    nfiles = int(sys.argv[3]) if len(sys.argv) > 3 else 2000
    seed = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    rnd = random.Random(seed)
    fails = 0
    os.makedirs(mnt, exist_ok=True)
    mount(img, mnt)
    expect = {}          # relative path -> sha256 hex, or None for a directory
    try:
        st = os.statvfs(mnt)
        free = st.f_bavail * st.f_frsize
        budget = free * 6 // 10
        dirs = [""]
        names = ["Dokument", "résumé", "ÄÖÜ", "日本語", "Ελληνικά", "file", "Data", "x"]
        ndirs = max(4, nfiles // 40)
        for i in range(ndirs):
            parent = rnd.choice(dirs)
            d = os.path.join(parent, "dir%03d_%s" % (i, rnd.choice(names)))
            os.mkdir(os.path.join(mnt, d))
            dirs.append(d)
            expect[d] = None
        big = os.path.join("big")             # many entries: a multi-level index
        os.mkdir(os.path.join(mnt, big))
        dirs.append(big)
        expect[big] = None
        used = 0
        files = []
        for i in range(nfiles):
            d = big if i % 3 == 0 else rnd.choice(dirs)
            r = rnd.random()
            size = 0 if r < 0.05 else rnd.randint(1, 700) if r < 0.5 else rnd.randint(700, 64 * 1024) \
                if r < 0.95 else rnd.randint(64 * 1024, 4 << 20)
            if used + size > budget:
                size = rnd.randint(0, 700)
            used += size
            name = "%s_%05d.%s" % (rnd.choice(names), i, rnd.choice(["txt", "BIN", "dat", ""]))
            p = os.path.join(d, name.rstrip("."))
            dat = data_for(seed * 100000 + i, size)
            with open(os.path.join(mnt, p), "wb") as f:
                f.write(dat)
            expect[p] = hashlib.sha256(dat).hexdigest()
            files.append(p)
        # delete a third, rename some, rewrite some
        rnd.shuffle(files)
        for p in files[:len(files) // 3]:
            os.unlink(os.path.join(mnt, p))
            del expect[p]
        for j, p in enumerate(files[len(files) // 3:len(files) // 3 + len(files) // 10]):
            q = p + ".renamed%d" % j
            os.rename(os.path.join(mnt, p), os.path.join(mnt, q))
            expect[q] = expect.pop(p)
        for j, p in enumerate(files[-len(files) // 10:]):
            dat = data_for(seed * 7777 + j, rnd.randint(0, 20000))
            with open(os.path.join(mnt, p), "wb") as f:
                f.write(dat)
            expect[p] = hashlib.sha256(dat).hexdigest()
    finally:
        umount(mnt)
    fails += ntfsfix(img, "after writing")
    # second mount: verify
    mount(img, mnt)
    try:
        seen = {}
        for root, ds, fs in os.walk(mnt):
            rel = os.path.relpath(root, mnt)
            for d in ds:
                seen[os.path.normpath(os.path.join(rel, d))] = None
            for f in fs:
                p = os.path.normpath(os.path.join(rel, f))
                with open(os.path.join(root, f), "rb") as fh:
                    seen[p] = hashlib.sha256(fh.read()).hexdigest()
        want = {os.path.normpath(k): v for k, v in expect.items()}
        if seen != want:
            missing = sorted(set(want) - set(seen))
            extra = sorted(set(seen) - set(want))
            bad = sorted(k for k in set(want) & set(seen) if want[k] != seen[k])
            print("FAIL contents after remount: %d missing %s, %d unexpected %s, %d differ %s" % (
                len(missing), missing[:3], len(extra), extra[:3], len(bad), bad[:3]))
            fails += 1
        nf = sum(1 for v in want.values() if v is not None)
        nd = len(want) - nf
        print("ntfs-3g: %d files and %d directories written, %d deleted, verified after remount" % (
            nf, nd, nfiles - nf))
        # remove most of it again
        for p in sorted((k for k, v in want.items() if v is not None), reverse=True)[: nf * 3 // 4]:
            os.unlink(os.path.join(mnt, p))
    finally:
        umount(mnt)
    fails += ntfsfix(img, "after deleting")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
