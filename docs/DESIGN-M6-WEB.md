# Milestone 6 design: the web (HTTP, TLS and a minimal browser)

This part of milestone 6 is all user mode: a library for HTTP and HTTPS
that programs link (libweb), a TLS implementation (BearSSL, vendored), a
command-line fetcher (`WGET.EXE`) and a small web browser (`BROWSER.EXE`,
"Web Browser"). The kernel side of the milestone (network cards, IPv4, TCP,
UDP, DHCP, DNS behind the BSD socket API) is separate work; libweb only
uses the socket calls of `hal.h` and `resolve()` (docs/DESIGN-M6.md).

Without a network (no adapter, cable unplugged, no DNS server) a request
ends with `WEB_E_NONET` and the browser shows its "not connected to a
network" page.

## Structure

```
userland/third_party/bearssl/   BearSSL 0.6, unmodified (src/, inc/, LICENSE.txt, VENDOR.txt)
userland/third_party/bearssl-port/  string.h, stdlib.h, stdio.h, limits.h for its freestanding build
userland/third_party/cacert/    Mozilla's CA list as curl publishes it (cacert.pem, MPL-2.0.txt, VENDOR.txt)
tools/mktrust.py                cacert.pem -> BearSSL trust anchors (C), at build time
userland/lib/web/               libweb
    web.h        the API (below)
    url.c        URLs: parse, format, resolve (RFC 3986), address bar input, form encoding
    http.c       HTTP/1.1: requests, responses, chunked, redirects, cookies, keep-alive pool
    tls.c        HTTPS: BearSSL setup, certificate checks and their reasons, session cache
    inflate.c    deflate / zlib / gzip decoder (our own)
    job.c        a request in a worker thread, with cancellation
    plat.c       Halcyon or Linux: sockets, name lookup, clock, random bytes, threads
userland/programs/wget.c        WGET.EXE
userland/programs/browser.c     BROWSER.EXE: window, navigation, history, drawing, forms, downloads
userland/programs/browser/      html.c (parser), layout.c (layout), image.c (PNG, GIF, BMP), browser.h
userland/programs/webtest.c     WEBTEST.EXE: checks of BearSSL and libweb as built for Halcyon
tests/web/                      host tests (Linux, ASan + UBSan), see "Tests"
```

The Makefile builds BearSSL's `src/*.c` with its own rules into
`build/user/libbearssl.a` (freestanding: `-nostdinc`, the compiler's
`stddef.h`/`stdint.h`, the headers in `bearssl-port/`; no OS random source,
no OS clock) and libweb into `build/user/libweb.a`. Only `BROWSER.EXE`,
`WGET.EXE` and `WEBTEST.EXE` link them (explicit rules); every other program
is built exactly as before. With the CA list a program grows by about
250 KB.

## libweb

```c
web_session_t *s = web_session_new();            /* cookies, kept-alive connections, TLS sessions */
web_request_t rq = { .url = "https://example.com/", .max_body = 1 << 20 };
web_response_t rs;
int err = web_fetch(s, &rq, &rs);                 /* 0, or WEB_E_* / E_* (web_strerror) */
/* rs.status, rs.content_type, rs.charset, rs.body / rs.body_len, rs.url (after redirects),
 * rs.error_text ("The certificate of example.com expired on 2025-01-01."), rs.tls_info */
web_response_free(&rs);
```

- **Requests:** `GET` (default), `HEAD` and `POST` (body and content type);
  `Host`, `User-Agent: Halcyon/1.0`, `Accept`, `Accept-Language`,
  `Accept-Encoding: gzip, deflate` (unless `WEB_REQ_NO_GZIP`), `Referer`,
  `Cookie`. Keep-alive by default: up to four idle connections per session
  are kept for 30 s and reused for the same host, port and scheme (a reused
  connection that turns out closed is retried once on a new one; POST never
  reuses one).
- **Responses:** status line (HTTP/1.0 and 1.1; 1xx answers skipped),
  headers (at most 64 KB and 256 lines, lines up to 8 KB, obsolete folding
  joined), `Content-Length` (conflicting values are an error),
  `Transfer-Encoding: chunked` (strict: hex size, CR LF after each chunk,
  trailers skipped), read-until-close, `Content-Encoding: gzip` / `deflate`
  (zlib or raw), `Content-Type` and charset, `Content-Disposition` file
  name. The body is kept in memory (limit `max_body`, 16 MiB by default, which
  also bounds the decompressed size) or streamed to `on_data` (decoded at
  the end when it is compressed). `rs.status` and the headers are set before
  the first `on_data` call.
- **Redirects:** 301, 302, 303, 307, 308, at most `max_redirects` (10); 303
  (and 301/302 after a POST) turn into GET, 307/308 keep method and body.
  Relative `Location`s are resolved against the current URL.
- **Timeouts and cancellation:** every wait (connect, send, receive) is a
  `poll` in slices of 100 ms that also checks `rq.cancel`; `connect_timeout_ms`
  (15 s per address) and `idle_timeout_ms` (30 s without data) bound it.
- **Worker thread:** `web_job_start(s, &rq)` copies the request and runs
  `web_fetch` in a thread; `web_job_done`, `web_job_wait`, `web_job_cancel`,
  `web_job_response`, `web_job_free` (a running job is cancelled and freed by
  its thread when it ends; its callbacks are not called any more).
- **Cookies:** a simple jar per session (up to 300): `Domain` (must contain
  the host, at least two labels, not for IP addresses), `Path`, `Secure`,
  `Max-Age`, `Expires` (all HTTP date formats), host-only cookies. There is
  no public-suffix list, so a site could set a cookie for `co.uk`; cookies
  last for the session only (nothing is written to disk).
- **Name lookup:** IPv4 literals directly; names through system call 116,
  `long resolve(const char *host, unsigned *ipv4_out, int max, int
  timeout_ms)` of the internet stack (`hal.h`). No IPv6.
- **URLs:** `url_parse`, `url_format`, `url_resolve` (RFC 3986 section 5,
  with browsers' leniencies: backslashes, spaces and non-ASCII bytes are
  encoded, white space trimmed), `url_from_user` (address bar input:
  `example.com`, `C:\dir\page.htm`), `url_decode`, `url_form_encode`.
  Non-ASCII host names (IDN) are not supported.
- **inflate:** `inflate_raw`, `inflate_zlib`, `inflate_gzip` (several
  members, header fields, CRC-32 and size checked), `inflate_zlib_into` (an
  exact output size, for PNG). Written for Halcyon after the structure of
  the format's description (canonical Huffman codes from code lengths, a
  9-bit lookup table, bit-by-bit decoding for longer codes). It never reads
  past its input, checks every code for over-subscription and (where the
  format requires) completeness, rejects distances before the start of the
  output and never grows the output past the caller's limit.

The library compiles for Linux as well (`-DWEB_HOST`, `plat.c` maps the
calls to POSIX and errno to `E_*`), which is how most of it is tested.

## TLS and certificates

- **BearSSL 0.6**, client side only. libweb allows **TLS 1.2 only** (BearSSL
  has no TLS 1.3; TLS 1.0 and 1.1 are deprecated): servers that accept only
  TLS 1.3 fail with "The server does not speak TLS 1.2 (it probably requires
  TLS 1.3...)". Cipher suites: ECDHE with ECDSA or RSA first (ChaCha20-Poly1305,
  AES-GCM, AES-CBC), then RSA key exchange with AES; no 3DES, no static ECDH.
  AES-NI is used when the CPU has it.
- **Entropy:** 32 bytes from `getrandom()` (the kernel's ChaCha20 generator)
  for every connection, injected before the handshake. BearSSL has no other
  source in this build.
- **Server name:** sent as SNI and checked against the certificate's
  subjectAltName DNS names (or the common name), a leading wildcard allowed.
  An IP address is checked the same way (BearSSL compares DNS names only,
  so certificates for IP addresses do not match: the request fails, it is
  never accepted unchecked).
- **Chain:** to one of Mozilla's 118 root certificates (curl's
  `cacert.pem`, converted at build time by `tools/mktrust.py`), with
  signatures (RSA and ECDSA, SHA-1 to SHA-512; MD5 refused), validity
  dates, basic constraints, key usage and critical extensions checked by
  BearSSL's X.509 "minimal" engine. Not checked: revocation (no OCSP, no
  CRLs), name constraints, Certificate Transparency.
- **No way around it:** a rejected certificate ends the request with
  `WEB_E_CERT` and a reason taken from BearSSL's error and the server's
  certificate (decoded again for the message only): expired on DATE / not
  valid before DATE, not for this host (it is for NAMES), self-signed,
  issued by an untrusted authority (named), not a CA, bad signature, weak
  key, incomplete chain. The browser shows it on an error page; neither it
  nor wget offers to continue.
- **The clock:** certificate dates need the time in UTC. Halcyon's
  real-time clock keeps local time (as Windows does). `wp_utc_now()` reads
  `datetime()`; once a time zone is chosen in Control Panel (Time Zone,
  `clock.utc_offset` = minutes east of UTC in `C:\HALCYON\SETTINGS.INI`)
  the time is exact. Without one the true time lies within 14 hours of
  the clock, and the chain is validated twice side by side (two BearSSL
  X.509 contexts fed the same certificates) at both ends of that window: it
  is accepted if it is valid at either end. A certificate issued an hour
  ago is therefore not refused because of the time zone, and one that
  expired more than 14 hours ago is still refused. A clock that is wrong by
  more than that gives date errors ("check the computer's clock").
- **Session resumption:** the parameters of the last session with each of
  eight host:port pairs are kept in the session and offered again.
- **Test anchors:** `web_session_add_ca_pem()` adds trust anchors from PEM
  certificates (BearSSL's PEM and X.509 decoders). Only the host tests use
  it (their test CA); the programs on Halcyon trust Mozilla's list only.

## The browser

`BROWSER.EXE` is an ordinary program: menus (File, Edit, View, Go, Help),
tool buttons (Back, Forward, Stop, Reload, Home), the address bar (a text
box with Go; Enter goes, Ctrl+L / F6 focus it), the page with a vertical
scroll bar, and a status bar (the link under the pointer, the loading phase
and bytes received, errors; the zone: My Computer, Internet, Secure). The
page title goes into the window title.

- **Loading:** `http:` and `https:` in a worker thread (a `web_job`, polled
  by a 100 ms window timer; Stop or Esc cancels it); `file:` URLs and plain
  paths (`C:\dir\page.htm`) are read directly, a folder gives a listing;
  `data:` URLs; `about:home` (the start page) and `about:blank`. HTML is
  shown, `text/*` as plain text, PNG / GIF / BMP as an image page, anything
  else (or `Content-Disposition: attachment`) is saved through the file
  dialog. `<meta http-equiv=refresh>` with a URL is followed after 1.5 s (at
  most 5 times in a row). `javascript:` and `mailto:` links only show a
  message.
- **History:** up to 50 entries for the session; the pages' bytes are cached
  (24 MiB in all) so Back and Forward show them again without the network,
  at the old scroll position. A page that answered a POST is not sent again
  without asking (Reload, or Back to an entry whose bytes were dropped).
- **Parsing (`html.c`):** the bytes are converted to UTF-8 first: a BOM
  wins, then the HTTP charset, then `<meta charset>` / `http-equiv`; UTF-8,
  ISO-8859-1 / Windows-1252 / ASCII and UTF-16 are decoded, an unknown
  charset is taken as UTF-8 if the bytes are valid UTF-8 and as
  Windows-1252 otherwise. The tokenizer handles tags, attributes (quoted or
  not), comments, doctypes, character references (named, decimal, hex, the
  legacy ones without `;`), raw text elements (`script`, `style`, `svg`,
  `math`, `template`, `iframe` are dropped; `title`, `textarea`, `xmp`,
  `plaintext` keep their text). The tree builder applies the common implied
  end tags (`p`, `li`, `dt`/`dd`, `option`, table rows and cells, headings,
  nested `a`, `form`), void elements and `</p>` / `</br>` quirks.
- **Layout (`layout.c`):** blocks with collapsed margins, inline text broken
  into lines at spaces (long words split), aligned on a common baseline and
  left / centre / right; headings in the larger fonts (`h1` huge, `h2` large,
  `h3` title, `h4`-`h6` bold), bold, monospace for `pre`, `code`, `tt`...,
  underline and strike-through, `font color` / `size`, `center` and `align`,
  lists (bullets by level, numbers, letters, Roman numerals, `start`,
  `value`), `blockquote`, `dl`, `hr`, `br`, `nobr`. Tables: automatic column
  widths from each cell's minimum and maximum content width (measured by the
  same code without output, cached per element), `width` on tables and cells
  (pixels or %), `colspan` (not `rowspan`), `cellspacing`, `cellpadding`,
  `border`, `bgcolor`, `align`, `valign` (middle by default). From CSS only
  the `style` attribute's `display` (none / block / inline / list-item),
  `visibility: hidden`, `color`, `background(-color)`, `font-weight`,
  `text-align`, `text-decoration`, `white-space` and `font-family:
  monospace`; style sheets are ignored. There is no italic font, so italic
  text is upright. Links get the hand cursor and the status bar shows their
  target; `<a name>` and `id` are anchors for `#fragment` links.
- **Forms:** text and password fields, check boxes, radio buttons (grouped
  by name), `select` (a popup menu), `textarea`, hidden fields, submit /
  reset / image / `button` buttons; the browser draws and edits them itself
  (so they scroll with the page): click or Tab to focus, caret keys,
  Backspace / Delete, Ctrl+V, Enter submits. Submission is
  `application/x-www-form-urlencoded` (UTF-8) with GET (the data replace the
  action's query) or POST.
- **Images (`image.c`):** PNG (all colour types and bit depths, `tRNS`,
  Adam7 interlacing; every chunk's CRC checked; the image data must inflate
  to exactly the size the header implies), GIF (first frame, transparency,
  interlacing, LZW checked code by code) and BMP (1, 4, 8, 16, 24, 32 bits,
  bit fields, top-down). Images are fetched one after the other after the
  page (8 MiB each), decoded, scaled to `width` / `height` and blended into
  the page; the page is laid out again as they arrive. JPEG, WebP and SVG are
  not decoded: their `alt` text shows (JPEG URLs are not even fetched).
- **Drawing:** text with the system fonts through `gfx_text_n`, everything
  else (backgrounds, rules, images, controls) straight into the window's
  mapped pixels (`win_canvas`), clipped to the page view; only the display
  list entries in view are drawn.

### Security model

The browser parses untrusted input (HTML, images, compressed data,
certificates) in its own address space: a bug can at worst end the browser
(Halcyon's crash dialog), not touch other programs or the kernel. On top of
that the code is written to fail cleanly:

- **Bounded structures:** a document holds at most 200,000 nodes (deeper
  than 200 levels are flattened), 32 attributes per element, 400,000 display
  list entries, 200 images, 5000 form controls, 64 MiB of arena memory;
  layout recursion stops at 60 nested boxes; tables at 4000 rows and 64
  columns; page height at 4,000,000 pixels.
- **Bounded input:** pages up to 32 MiB, images up to 8 MiB and 8192 x 8192
  (16 megapixels), headers 64 KB, chunk sizes checked for overflow, the
  decompressed size limited like the body.
- **No scripts:** there is no JavaScript (or any other script); `<script>`
  content is dropped. Nothing on a page can make the browser send data
  except following a link or submitting a form the user clicks.
- **Fuzzed:** the host tests run the parser and layout on random tag soup
  and mutated pages, the image decoders on 40,000 mutated files and inflate
  on 30,000 mutated streams, all under AddressSanitizer and UBSan.

What it does not protect against: a hostile site can show anything it
likes (there is no phishing or malware list), cookies have no public-suffix
check, and certificate revocation is not checked.

## Limits (summary)

| What | Limit |
|------|-------|
| TLS versions | 1.2 only (no TLS 1.3 in BearSSL 0.6) |
| redirects | 10 |
| page / download body | 32 MiB (browser), 1 GiB (wget; compressed bodies are decoded in memory) |
| response headers | 64 KB, 256 lines, 8 KB per line |
| image | 8 MiB, 8192 x 8192, 16 megapixels |
| connect / idle timeout | 15 s per address / 30 s |
| kept-alive connections | 4 per session, 30 s |
| cookies | 300 per session, 4 KB each, not saved |
| history | 50 pages, 24 MiB of cached pages |

## WGET.EXE

`wget [-O file|-] [-q] [-S] [--head] [--post-data data] [--max-redirect n]
[--timeout s] [--no-compression] URL` fetches a URL with libweb and saves it
under the name from `Content-Disposition` or the URL (in the current
folder), or to the console with `-O -`. It prints the phases, redirects, the
TLS version and cipher, the status and the type, and progress. Exit code 0,
1 for an error (no answer, certificate...), 8 if the server answered with an
error status.

## The shell

The desktop has a Web Browser icon and the Start menu a "Web Browser"
entry (the menu is one row taller). `.htm` and `.html` files open in the
browser (`shell_open` in `kernel/gui/desktop.c`; if `BROWSER.EXE` cannot
start they open in Notepad as before), and Explorer shows them as "HTML
Document" with the web icon. New icons: web, back, forward, reload, stop,
home (`tools/mkicons.py`, `HICON_*` in `sysabi.h`).

## Vendored components

| Component | Version | License | Source | SHA-256 |
|-----------|---------|---------|--------|---------|
| BearSSL | 0.6 (2018-08-14) | MIT | https://bearssl.org/bearssl-0.6.tar.gz | `6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14` |
| CA certificates | Mozilla data of 2026-11-05 | MPL 2.0 | https://curl.se/ca/cacert.pem | `427c064216761a1e72a8b30f0d954d08cb6e4435a82091fb865afd20c8497285` |

BearSSL's publisher gives no checksum; the archive was compared file by
file with the upstream Git tag `v0.6` (commit
`8ef7680081c61b486622f2d983c0d3d21e83caad`) and is identical. The CA
bundle's hash matches curl's published `cacert.pem.sha256`. Both were
downloaded into an empty directory; nothing from them was run. Only the
sources, headers and license texts are in the tree (`VENDOR.txt` in each
directory lists what was left out and how to update). Everything else
(libweb, inflate, the HTML parser, layout, the PNG / GIF / BMP decoders,
the trust anchor converter) is written for Halcyon.

## Tests

- `tests/web/run.sh` (host, also run by `tools/ci.sh`): libweb and the
  browser's parser, layout and decoders built for Linux with ASan and UBSan.
  URL resolution (RFC 3986's examples and more); the trust anchors made by
  `mktrust.py` compared one by one with BearSSL's own decoding of the same
  PEM file; inflate against data compressed by Python's zlib and gzip in
  every mode, corrupt streams (bad headers, checksums, block types, codes,
  distances, truncation), output limits and a 200 MB "bomb", plus 30,000
  mutations; the image decoders against Pillow's pixels for every PNG colour
  type and depth (and Adam7, written by the test), GIF and BMP variants,
  plus 40,000 mutations; the parser and layout on fixed cases (implied end
  tags, entities, charsets, line breaking, tables, anchors, forms) and
  thousands of mutated pages. Then 75 HTTP and HTTPS checks against
  `tests/web/server.py` with certificates from a test CA made by openssl:
  good RSA and EC certificates, an intermediate CA, expired, not yet valid,
  wrong host, self-signed, untrusted CA, a non-CA intermediate, IP address,
  a TLS-1.3-only server, plain HTTP to a TLS port; gzip, deflate (zlib and
  raw), corrupt gzip, a gzip bomb, chunked (good, bad sizes, missing CR LF,
  huge sizes, truncated), truncated bodies, read-until-close, HTTP/1.0,
  1xx answers, huge and too many headers, folded headers, conflicting and
  invalid Content-Length, garbage, no answer, redirects (relative, absolute,
  loops, limits, 303 after POST), POST bodies, User-Agent and Host,
  timeouts, size limits (in memory and streamed), cancellation of a worker
  job, keep-alive, cookies, and the time-zone window for certificate dates.
- `WEBTEST.EXE` (QEMU boot test variant `xhci:bios:web`, in the full
  matrix): BearSSL as compiled for Halcyon. TLS 1.2 handshakes and
  application data between a BearSSL client and server inside the program
  (over memory) with seven cipher suites and RSA and EC certificates of a
  test CA, a wrong name, an expired and a not-yet-valid certificate; gzip;
  URLs; the clock; a fetch over the system's network (in the boot test,
  which has no adapter: "no network") and a cancelled job. Under QEMU (TCG) a handshake takes 15-30 ms.
- In QEMU by hand: the sample pages in `C:\Documents\Web` (text, lists,
  tables, images, forms, Latin-1), folder listings, saved real-world pages
  (Wikipedia, Hacker News), the no-network error page, form submission,
  Save Page As, resizing, the Start menu and desktop icon, wget in the
  Command Prompt.
- From the host, libweb fetched real sites over HTTPS (Wikipedia, Google,
  bearssl.org, curl.se, example.com) and plain HTTP. The development
  machine's outbound HTTPS goes through an intercepting proxy: without its
  CA libweb correctly refuses every site ("issued by ..., which is not a
  trusted certificate authority"); with the proxy's CA added as a test
  anchor the pages load (so those runs tested the TLS code against the
  proxy, not against the sites' own certificates).

- On Halcyon over the stack (QEMU user networking, virtio-net and e1000e):
  the browser loads http://example.com/; https://example.com/ ends on the
  certificate page, because the development machine's intercepting proxy
  answers for every site (see above) and its CA is not one of Mozilla's.
  On a normal network the same request completes the handshake against
  the site's own certificate; that has not been tried yet.

## Still to do

- Real sites over HTTPS from Halcyon on a network without an intercepting
  proxy (real hardware, or QEMU on another machine), and the timings of
  connect and DNS failures and of Stop in each phase.
- Possible next steps for the browser: a CSS subset (selectors for
  `display: none`, colours, widths), JPEG decoding, rowspan, text selection
  and copy, find in page, bookmarks, persistent cookies, IPv6, TLS 1.3 when
  BearSSL (or a replacement) has it. M15 (Firefox on the Linux layer) is
  the real browser; this one stays a small, safe viewer.
