#!/usr/bin/env python3
"""Drive a running QEMU through its HMP monitor socket.

usage: qemu_ctl.py SOCK cmd [cmd ...]
  cmd = any HMP command, e.g. "screendump /tmp/x.ppm", "sendkey ret",
        "mouse_move 10 5", "mouse_button 1", or "sleep:0.5"
  "png:PATH" takes a screendump and converts it to PNG.
  "move:X,Y" / "click:X,Y" / "rclick:X,Y" / "dclick:X,Y" / "drag:X1,Y1,X2,Y2"
  send absolute pointer events through QMP (needs a usb-tablet and the
  QMP socket next to the monitor socket, screen assumed 1024x768, or
  SCREEN=WxH in the environment).
  "press:X,Y" / "release:X,Y" (",right" for the right button) hold and let go
  of a button, e.g. to take a screenshot in the middle of a drag;
  "keydown:ctrl" / "keyup:ctrl" hold a key (QEMU qcode) for Ctrl+click.
  "type:TEXT" types text with sendkey.
"""
import json
import os
import socket
import sys
import time


class QMP:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(50):
            try:
                self.s.connect(path)
                break
            except OSError:
                time.sleep(0.1)
        self.f = self.s.makefile("rw")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        self.f.write(json.dumps({"execute": name, "arguments": args}) + "\n")
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r:
                return r

    def abs(self, x, y, btn=None, down=None):
        sw, sh = (int(v) for v in os.environ.get("SCREEN", "1024x768").split("x"))
        ev = [{"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / (sw - 1))}},
              {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / (sh - 1))}}]
        if btn:
            ev.append({"type": "btn", "data": {"down": down, "button": btn}})
        self.cmd("input-send-event", events=ev)


KEYMAP = {" ": "spc", "\n": "ret", ".": "dot", ",": "comma", "/": "slash", "-": "minus", "=": "equal",
          ":": "shift-semicolon", ";": "semicolon", "_": "shift-minus", "\\": "backslash", "*": "shift-8",
          ">": "shift-dot", "<": "shift-comma", "!": "shift-1", "?": "shift-slash", "'": "apostrophe",
          '"': "shift-apostrophe", "(": "shift-9", ")": "shift-0"}


def main():
    sock_path = sys.argv[1]
    qmp = None
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
        if cmd.split(":")[0] in ("keydown", "keyup"):
            if qmp is None:
                qmp = QMP(sock_path.replace("mon.sock", "qmp.sock"))
            kind, key = cmd.split(":", 1)
            qmp.cmd("input-send-event", events=[{"type": "key", "data": {"down": kind == "keydown",
                                                                          "key": {"type": "qcode", "data": key}}}])
            time.sleep(0.1)
            continue
        if cmd.split(":")[0] in ("move", "click", "rclick", "dclick", "drag", "press", "release"):
            if qmp is None:
                qmp = QMP(sock_path.replace("mon.sock", "qmp.sock"))
            kind, arg = cmd.split(":", 1)
            right = arg.endswith(",right")
            v = [int(a) for a in arg.replace(",right", "").split(",")]
            if kind in ("press", "release"):
                qmp.abs(v[0], v[1]); time.sleep(0.1)
                qmp.abs(v[0], v[1], "right" if right else "left", kind == "press")
            elif kind == "move":
                qmp.abs(v[0], v[1])
            elif kind == "drag":
                qmp.abs(v[0], v[1]); time.sleep(0.15)
                qmp.abs(v[0], v[1], "left", True); time.sleep(0.15)
                for i in range(1, 6):
                    qmp.abs(v[0] + (v[2] - v[0]) * i // 5, v[1] + (v[3] - v[1]) * i // 5); time.sleep(0.08)
                qmp.abs(v[2], v[3], "left", False)
            else:
                b = "right" if kind == "rclick" else "left"
                qmp.abs(v[0], v[1]); time.sleep(0.15)
                for _ in range(2 if kind == "dclick" else 1):
                    qmp.abs(v[0], v[1], b, True); time.sleep(0.06)
                    qmp.abs(v[0], v[1], b, False); time.sleep(0.06)
            time.sleep(0.3)
            continue
        if cmd.startswith("type:"):
            for ch in cmd[5:].replace("\\n", "\n"):
                if ch.isalpha() and ch.isupper():
                    k = "shift-" + ch.lower()
                elif ch.isalnum():
                    k = ch
                else:
                    k = KEYMAP.get(ch, "spc")
                s.sendall(("sendkey %s\n" % k).encode())
                time.sleep(0.04)
            drain()
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
