# Third-party components

Halcyon's own code is under the MIT License (see `LICENSE`). These parts come
from elsewhere and keep their own licenses.

| Component | Where | License | Notes |
|---|---|---|---|
| BearSSL 0.6 (Thomas Pornin) | `userland/third_party/bearssl/` | MIT (`LICENSE.txt` there) | Unmodified; version and SHA-256 in `VENDOR.txt`. Linked into the web browser, WGET and WEBTEST. |
| Mozilla CA certificate list, as published by curl (`cacert.pem`) | `userland/third_party/cacert/` | MPL-2.0 (`MPL-2.0.txt` there) | Unmodified; date and SHA-256 in `VENDOR.txt`. Converted to trust anchors at build time by `tools/mktrust.py`. The source form is the file in this repository. |
| DejaVu fonts | not in this repository: read from `/usr/share/fonts/truetype/dejavu` at build time | Bitstream Vera Fonts license, with DejaVu changes in the public domain | `tools/mkfont.py` and `tools/mkicons.py` render them into bitmaps that are built into the kernel. See https://dejavu-fonts.github.io/License.html. |

Files under `userland/third_party/bearssl-port/` are Halcyon's own small
C-library shims for building BearSSL and are under Halcyon's MIT License.
