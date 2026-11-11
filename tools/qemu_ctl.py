#!/usr/bin/env python3
"""Drive a running QEMU through its HMP monitor socket.

usage: qemu_ctl.py SOCK cmd [cmd ...]
  cmd = any HMP command, e.g. "screendump /tmp/x.ppm", "sendkey ret",
        "mouse_move 10 5", "mouse_button 1", or "sleep:0.5"
  "png:PATH" takes a screendump and converts it to PNG.
"""
import os
import socket
import sys
import time


def main():
    sock_path = sys.argv[1]
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    for _ in range(50):
        try:
            s.connect(sock_path)
            break
        except OSError:
            time.sleep(0.1)
    s.settimeout(0.3)

    def drain():
        data = b""
        try:
            while True:
                d = s.recv(65536)
                if not d:
                    break
                data += d
        except socket.timeout:
            pass
        return data.decode(errors="replace")

    drain()
    for cmd in sys.argv[2:]:
        if cmd.startswith("sleep:"):
            time.sleep(float(cmd[6:]))
            continue
        if cmd.startswith("png:"):
            path = cmd[4:]
            ppm = path + ".ppm"
            s.sendall(("screendump %s\n" % ppm).encode())
            time.sleep(0.5)
            drain()
            from PIL import Image
            for _ in range(20):
                if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
                    break
                time.sleep(0.1)
            time.sleep(0.2)
            Image.open(ppm).save(path)
            os.remove(ppm)
            continue
        s.sendall((cmd + "\n").encode())
        out = drain()
        lines = [l for l in out.splitlines() if l.strip() and not l.startswith("(qemu)") and cmd not in l]
        if lines:
            print("\n".join(lines))


if __name__ == "__main__":
    main()
