#!/usr/bin/env python3
"""Build Halcyon's help pages (C:\\HALCYON\\HELP) from help/.

usage: tools/mkhelp.py SRCDIR OUTDIR

SRCDIR holds the pages (*.htm, a small subset of HTML that HELP.EXE draws
with the Web Browser's engine) and contents.txt, the Contents tree:
"Title|page.htm" per line, indented by two spaces per level; an entry
followed by deeper ones is a book. Each page names its index keywords in
    <meta name="keywords" content="keyword; another keyword#anchor; ...">
(a keyword may point to an anchor in the page).

OUTDIR gets the pages, CONTENTS.TXT and INDEX.TXT ("keyword<TAB>page#anchor
<TAB>page title" per line, sorted by keyword without regard to case).
The build fails if a page lacks a title or keywords, a link or a keyword
points to a page or anchor that does not exist, a page is missing from the
contents, or a page uses markup HELP.EXE does not draw. Comments
<!-- CHECK: ... --> mark text written before the feature it describes was
merged; MKHELP_CHECKS=1 lists them.
"""
import html
import os
import re
import shutil
import sys

ALLOWED = {"html", "head", "title", "meta", "body", "h2", "h3", "h4", "p", "br", "b", "strong", "i", "em", "u",
           "code", "kbd", "tt", "pre", "ul", "ol", "li", "dl", "dt", "dd", "a", "table", "tr", "td", "th", "div",
           "span", "hr", "small", "big", "font"}


def fail(msg):
    print("mkhelp: " + msg, file=sys.stderr)
    sys.exit(1)


def main():
    src, out = sys.argv[1], sys.argv[2]
    pages = sorted(f for f in os.listdir(src) if f.endswith(".htm"))
    info = {}
    checks = []
    for p in pages:
        if p != p.lower():
            fail("%s: page names are lower case" % p)
        text = open(os.path.join(src, p), encoding="utf-8").read()
        m = re.search(r"<title>(.*?)</title>", text, re.S)
        if not m:
            fail("%s: no <title>" % p)
        title = html.unescape(m.group(1).strip())
        m = re.search(r'<meta name="keywords" content="([^"]*)">', text)
        if not m:
            fail('%s: no <meta name="keywords" content="...">' % p)
        kws = [k.strip() for k in html.unescape(m.group(1)).split(";") if k.strip()]
        ids = set(re.findall(r'\b(?:id|name)="([^"]+)"', text))
        links = re.findall(r'<a\s+href="([^"]*)"', text)
        tags = set(t.lower() for t in re.findall(r"</?([a-zA-Z0-9]+)", text))
        bad = tags - ALLOWED
        if bad:
            fail("%s: markup HELP.EXE does not draw: %s" % (p, ", ".join(sorted(bad))))
        if "<h1" in text:
            fail("%s: use <h2> for the page title (h1 is too large)" % p)
        for c in re.findall(r"<!--\s*CHECK:(.*?)-->", text, re.S):
            checks.append("%s: %s" % (p, " ".join(c.split())))
        info[p] = (title, kws, ids, links)
    # links and keywords
    for p, (title, kws, ids, links) in info.items():
        for h in links:
            if h.startswith(("run:", "http:", "https:")):
                continue
            page, _, anchor = h.partition("#")
            page = page or p
            if page not in info:
                fail("%s: link to a missing page: %s" % (p, h))
            if anchor and anchor not in info[page][2]:
                fail("%s: link to a missing anchor: %s" % (p, h))
        for k in kws:
            _, _, anchor = k.partition("#")
            if anchor and anchor not in ids:
                fail("%s: keyword %r: no anchor %s" % (p, k, anchor))
    # the contents
    toc = []
    for line in open(os.path.join(src, "contents.txt"), encoding="utf-8"):
        line = line.rstrip("\r\n")
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        ind = len(line) - len(line.lstrip(" "))
        if ind % 2:
            fail("contents.txt: odd indentation: %r" % line)
        title, _, page = line.strip().partition("|")
        if page and page not in info:
            fail("contents.txt: no page %s" % page)
        toc.append(page)
    missing = set(info) - set(toc)
    if missing:
        fail("pages not in contents.txt: %s" % ", ".join(sorted(missing)))
    # output
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        os.remove(os.path.join(out, f))
    for p in pages:
        shutil.copy(os.path.join(src, p), os.path.join(out, p))
    shutil.copy(os.path.join(src, "contents.txt"), os.path.join(out, "CONTENTS.TXT"))
    entries = []
    for p, (title, kws, ids, links) in info.items():
        for k in kws:
            kw, _, anchor = k.partition("#")
            entries.append((kw.strip(), p + ("#" + anchor if anchor else ""), title))
    entries.sort(key=lambda e: (e[0].lower(), e[2].lower()))
    with open(os.path.join(out, "INDEX.TXT"), "w", encoding="utf-8", newline="\r\n") as f:
        for e in entries:
            f.write("%s\t%s\t%s\n" % e)
    print("mkhelp: %d pages, %d contents entries, %d index entries, %d notes to check (MKHELP_CHECKS=1 lists them)"
          % (len(pages), len(toc), len(entries), len(checks)))
    if os.environ.get("MKHELP_CHECKS"):
        for c in checks:
            print("mkhelp: to check: " + c)


if __name__ == "__main__":
    main()
