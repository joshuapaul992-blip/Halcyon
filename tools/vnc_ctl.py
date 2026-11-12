#!/usr/bin/env python3
"""Ask QEMU to resize, connect or disconnect one monitor of a virtio GPU,
through a VNC server attached to that head (RFB SetDesktopSize).

usage: vnc_ctl.py SOCK WIDTHxHEIGHT      e.g. build/vnc1.sock 1024x768 (0x0: disconnect)

QEMU (8.2) has no monitor command that changes a virtio GPU's outputs; its
display back ends do it when their window is resized. A VNC server bound to
one head (-vnc unix:SOCK,id=v1,display=gpu0,head=1) forwards a client's
SetDesktopSize to the device as that output's new size (a size of 0 turns
the output off), which raises the device's configuration-change interrupt
(VIRTIO_GPU_EVENT_DISPLAY): hotplug as the guest sees it. This is a minimal
RFB 3.8 client: version, security "None", ClientInit, SetEncodings with
the ExtendedDesktopSize pseudo-encoding, SetDesktopSize.
"""
import socket
import struct
import sys
import time


def recv_exact(s, n):
    b = b""
    while len(b) < n:
        d = s.recv(n - len(b))
        if not d:
            raise EOFError("connection closed")
        b += d
    return b


def resize(path, w, h, timeout=5.0):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    end = time.time() + timeout
    while True:
        try:
            s.connect(path)
            break
        except OSError:
            if time.time() > end:
                raise
            time.sleep(0.1)
    version = recv_exact(s, 12)
    if not version.startswith(b"RFB "):
        raise RuntimeError("not an RFB server: %r" % version)
    s.sendall(b"RFB 003.008\n")
    n = recv_exact(s, 1)[0]
    if n == 0:
        reason = recv_exact(s, struct.unpack(">I", recv_exact(s, 4))[0])
        raise RuntimeError("refused: %r" % reason)
    types = recv_exact(s, n)
    if 1 not in types:
        raise RuntimeError("no 'None' security type (%r)" % list(types))
    s.sendall(bytes([1]))
    if struct.unpack(">I", recv_exact(s, 4))[0] != 0:
        raise RuntimeError("security handshake failed")
    s.sendall(bytes([1]))                                   # ClientInit: shared
    fw, fh = struct.unpack(">HH", recv_exact(s, 4))
    recv_exact(s, 16)                                       # pixel format
    recv_exact(s, struct.unpack(">I", recv_exact(s, 4))[0])  # name
    # SetEncodings: raw, DesktopSize (-223), ExtendedDesktopSize (-308)
    encs = [0, -223, -308]
    s.sendall(struct.pack(">BBH", 2, 0, len(encs)) + b"".join(struct.pack(">i", e) for e in encs))
    # SetDesktopSize with one screen covering it
    s.sendall(struct.pack(">BBHHBB", 251, 0, w, h, 1, 0) + struct.pack(">IHHHHI", 0, 0, 0, w, h, 0))
    time.sleep(0.3)
    s.close()
    return fw, fh


def main():
    path, size = sys.argv[1], sys.argv[2]
    w, h = (int(v) for v in size.lower().split("x"))
    fw, fh = resize(path, w, h)
    print("vnc: %s was %dx%d, asked for %dx%d" % (path, fw, fh, w, h))


if __name__ == "__main__":
    main()
