# Third-party components

Halcyon's own code is under the MIT License (see `LICENSE`). These parts come
from elsewhere and keep their own licenses.

| Component | Where | License | Notes |
|---|---|---|---|
| BearSSL 0.6 (Thomas Pornin) | `userland/third_party/bearssl/` | MIT (`LICENSE.txt` there) | Unmodified; version and SHA-256 in `VENDOR.txt`. Linked into the web browser, WGET and WEBTEST. |
| Mozilla CA certificate list, as published by curl (`cacert.pem`) | `userland/third_party/cacert/` | MPL-2.0 (`MPL-2.0.txt` there) | Unmodified; date and SHA-256 in `VENDOR.txt`. Converted to trust anchors at build time by `tools/mktrust.py`. The source form is the file in this repository. |
| DejaVu fonts | not in this repository: read from `/usr/share/fonts/truetype/dejavu` at build time | Bitstream Vera Fonts license, with DejaVu changes in the public domain | `tools/mkfont.py` and `tools/mkicons.py` render them into bitmaps that are built into the kernel. See https://dejavu-fonts.github.io/License.html. |
| nouveau (Linux `drivers/gpu/drm/nouveau`, Red Hat Inc. and contributors) | `kernel/drivers/nv/` (`i2c.c`, `nv.c`, `chip.c`, `vbios.c`) | MIT (below) | No files copied. The NVIDIA identification code (M10a) is Halcyon's own, written from NVIDIA's open-gpu-doc (the BIT and DCB 4.x specifications) and the Linux kernel's nova-core VBIOS notes (`Documentation/gpu/nova/core/vbios.rst`, GPL-2.0+ OR MIT: the IFR header); nouveau served as the reference for register addresses and sequences that are not documented there, and some are ported closely: the DisplayPort AUX transaction of `nvkm/subdev/i2c/auxg94.c` and `auxgm200.c` (in `i2c.c`), the I2C port, hybrid pad and GPIO registers (`busnv04.c`, `busnv4e.c`, `busnv50.c`, `busgf119.c`, `padg94.c`, `padgm200.c`, `nvkm/subdev/gpio/*`; in `nv.c`), the PRAMIN / PROM shadowing rules (`nvkm/subdev/bios/shadow*.c`; in `nv.c`), the chipset decoding and per-generation VRAM registers (`nvkm/engine/device/base.c`, `nvkm/subdev/fb/ram*.c`; in `chip.c`) and the DCB 1.5 / 2.x layouts (`nouveau_bios.c`, `nvkm/subdev/bios/dcb.c`, `i2c.c`, `gpio.c`, `conn.c`; in `vbios.c`). |

Files under `userland/third_party/bearssl-port/` are Halcyon's own small
C-library shims for building BearSSL and are under Halcyon's MIT License.

## nouveau

The files of nouveau used as a reference carry this notice (for example
`nvkm/subdev/bios/base.c`, `nvkm/subdev/i2c/auxg94.c`):

```
Copyright 2012 Red Hat Inc.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.
```

Authors named in those files: Ben Skeggs. The other files used (2014-2016)
carry the same notice with "Copyright 2014 Red Hat Inc." or later years.

NVIDIA's open-gpu-doc (BIOS Information Table and DCB 4.x specifications)
is MIT-licensed documentation, the nova-core VBIOS notes are dual-licensed
GPL-2.0+ OR MIT; nothing of them is copied beyond the names and values of
fields.
