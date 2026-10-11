#!/usr/bin/env python3
"""Fetch your whole Readwise library and build the Inkplate's highlight files.

Reads READWISE_TOKEN from firmware/typewriter/secrets.h, pages through
GET /api/v2/export/, and writes to build/highlights/:

  highlights.jsonl  one highlight per line: {"id","text","title","author","fav"},
                    raw Readwise text; see build() for what's left out
  index.bin         8 bytes per line: byte offset (uint32 LE), favorite (uint8), 3 zero bytes
  state.json        {"version", "syncedAt", "count", "favorites"}; the device asks
                    Readwise for changes after syncedAt
  export.json       the raw export, for looking at the data (stays on the Mac)

Then prints counts, how long highlights are, and characters the device's fonts lack.
With --send it copies the three device files onto the SD card (tools/sd_put.py).

Usage: tools/readwise_seed.py [--check] [--send] [--port PORT]
"""
import argparse
import json
import os
import re
import statistics
import struct
import sys
import time
import urllib.error
import urllib.request
from collections import Counter
from datetime import datetime, timezone

ROOT = os.path.join(os.path.dirname(__file__), "..")
SECRETS = os.path.join(ROOT, "firmware", "typewriter", "secrets.h")
OUT = os.path.join(ROOT, "build", "highlights")
API = "https://readwise.io/api/v2"
CARD_DIR = "/highlights"

# What the device's font table holds (tools/make_font.py): ASCII, Latin-1, and these.
FONT_EXTRA = {0x2018, 0x2019, 0x201A, 0x201C, 0x201D, 0x201E, 0x2013, 0x2014, 0x2026, 0x2022, 0x2032,
              0x2033, 0x2039, 0x203A, 0x20AC, 0x2122, 0x2190, 0x2192, 0x2191, 0x2193, 0x2212, 0x2248,
              0x2260, 0x2264, 0x2265}


def token():
    m = re.search(r'#define\s+READWISE_TOKEN\s+"([^"]*)"', open(SECRETS).read())
    if not m or not m.group(1):
        sys.exit(f"Put your token in {os.path.relpath(SECRETS)}: #define READWISE_TOKEN \"...\"")
    return m.group(1)


def get(path, tok):
    """GET an API path; waits out rate limits. Returns (status, parsed JSON or None)."""
    while True:
        req = urllib.request.Request(API + path, headers={"Authorization": f"Token {tok}"})
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                body = r.read()
                return r.status, json.loads(body) if body else None
        except urllib.error.HTTPError as e:
            if e.code == 429:
                wait = int(e.headers.get("Retry-After", "10"))
                print(f"rate limited, waiting {wait} s")
                time.sleep(wait)
                continue
            raise SystemExit(f"Readwise {path}: HTTP {e.code} {e.read()[:200]!r}")


def fetch_all(tok):
    books, cursor, page = [], None, 0
    while True:
        page += 1
        status, data = get("/export/" + (f"?pageCursor={cursor}" if cursor else ""), tok)
        books += data["results"]
        print(f"page {page}: {len(data['results'])} sources, {len(books)} so far")
        cursor = data.get("nextPageCursor")
        if not cursor:
            return books


def in_font(ch):
    cp = ord(ch)
    return 0x20 <= cp < 0x7F or 0xA0 <= cp <= 0xFF or cp in FONT_EXTRA or ch == "\n"


def approx_clean(text):
    """Roughly what the device's cleaner will show, for the length report only."""
    text = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", text)  # [words](url) -> words
    text = re.sub(r"(\*\*|__|\*|_)(\S.*?\S|\S)\1", r"\2", text)
    return text.strip()


# Left out (the device's monthly sync applies the same rules):
# - Chinese, Japanese, Korean text anywhere in the highlight, title or author: the fonts are Latin only
# - supplementals: highlights Readwise adds from books you picked, not ones you made
# - highlights Reader's Ghostreader made (tagged AI_TAG)
# A highlight counts as a favorite if it's marked one, or its source is tagged FAVORITE_TAG.
CJK = re.compile(r"[\u2E80-\u9FFF\uAC00-\uD7AF\uF900-\uFAFF\uFE30-\uFE4F\uFF00-\uFFEF]")
AI_TAG = "\U0001F47B ai highlighted"
FAVORITE_TAG = "favorite"


def build(books):
    records, skipped = [], Counter()
    for b in books:
        n = len(b.get("highlights", []))
        title = b.get("readable_title") or b.get("title") or ""
        author = b.get("author") or ""
        if b.get("is_deleted"):
            skipped["source deleted"] += n
            continue
        if b.get("category") == "supplementals":
            skipped["supplemental"] += n
            continue
        fav_source = any(t.get("name", "").lower() == FAVORITE_TAG for t in b.get("book_tags", []))
        for h in b.get("highlights", []):
            text = h.get("text") or ""
            if h.get("is_deleted"):
                skipped["deleted"] += 1
            elif h.get("is_discard"):
                skipped["discarded"] += 1
            elif not text.strip():
                skipped["empty"] += 1
            elif any(t.get("name") == AI_TAG for t in h.get("tags", [])):
                skipped["AI highlighted"] += 1
            elif CJK.search(text + title + author):
                skipped["Chinese/Japanese/Korean"] += 1
            else:
                records.append({"id": h["id"], "text": text, "title": title, "author": author,
                                "fav": bool(h.get("is_favorite")) or fav_source})
    return records, skipped


def write(records, synced_at, books):
    os.makedirs(OUT, exist_ok=True)
    index = bytearray()
    with open(os.path.join(OUT, "highlights.jsonl"), "wb") as f:
        for r in records:
            index += struct.pack("<IB3x", f.tell(), 1 if r["fav"] else 0)
            f.write(json.dumps(r, ensure_ascii=False, separators=(",", ":")).encode() + b"\n")
    with open(os.path.join(OUT, "index.bin"), "wb") as f:
        f.write(index)
    state = {"version": 1, "syncedAt": synced_at, "count": len(records),
             "favorites": sum(r["fav"] for r in records)}
    with open(os.path.join(OUT, "state.json"), "w") as f:
        json.dump(state, f)
    with open(os.path.join(OUT, "export.json"), "w") as f:
        json.dump(books, f, ensure_ascii=False)


def report(records, skipped):
    favs = sum(r["fav"] for r in records)
    size = os.path.getsize(os.path.join(OUT, "highlights.jsonl"))
    print(f"\n{len(records)} highlights ({favs} favorites), {size / 1024:.0f} KB on the card")
    print("left out: " + (", ".join(f"{n} {k}" for k, n in skipped.items()) or "none"))
    lengths = sorted(len(approx_clean(r["text"])) for r in records)
    if lengths:
        q = statistics.quantiles(lengths, n=100) if len(lengths) > 1 else [lengths[0]] * 99
        print(f"length in characters (after stripping links/emphasis): median {q[49]:.0f}, "
              f"90th {q[89]:.0f}, 99th {q[98]:.0f}, longest {lengths[-1]}")
        for lo, hi in [(0, 100), (100, 200), (200, 400), (400, 800), (800, 10**9)]:
            n = sum(lo <= x < hi for x in lengths)
            label = f"{lo}-{hi}" if hi < 10**9 else f"{lo}+"
            print(f"  {label:>8}: {n:5} {'#' * round(60 * n / len(lengths))}")
    missing = Counter()
    affected = 0
    for r in records:
        bad = {c for c in approx_clean(r["text"]) + r["title"] + r["author"] if not in_font(c)}
        affected += bool(bad)
        missing.update(bad)
    print(f"{affected} highlights use characters outside the font table")
    for ch, n in missing.most_common(25):
        print(f"  U+{ord(ch):04X} {ch!r}: in {n}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="only check the token")
    ap.add_argument("--send", action="store_true", help="copy the files onto the SD card over USB")
    ap.add_argument("--port", default="/dev/cu.usbserial-110")
    args = ap.parse_args()
    tok = token()
    status, _ = get("/auth/", tok)
    print(f"token: {'ok' if status == 204 else f'unexpected HTTP {status}'}")
    if args.check:
        return
    synced_at = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")  # before fetching: nothing slips past
    books = fetch_all(tok)
    records, skipped = build(books)
    write(records, synced_at, books)
    report(records, skipped)
    if args.send:
        sys.path.insert(0, os.path.dirname(__file__))
        import sd_put
        fd = sd_put.open_port(args.port)
        lines = sd_put.Lines(fd)
        for name in ("highlights.jsonl", "index.bin", "state.json"):  # state last: it marks a complete set
            sd_put.put(fd, lines, os.path.join(OUT, name), f"{CARD_DIR}/{name}")
        os.close(fd)


if __name__ == "__main__":
    main()
