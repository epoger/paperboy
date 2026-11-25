#!/usr/bin/env python3
"""
CYD News Bridge
===============
Google News' RSS feed only has headlines - no pictures - and its links are
encrypted redirects that an ESP32 can't follow. This tiny server does the heavy
lifting for the CYD:

  1. downloads the Google News RSS feed,
  2. resolves each article's real URL and finds its lead image (og:image),
  3. crops/shrinks that image to a *baseline* JPEG of the size the device asks for
     (default 240x176; the watch asks for its own via ?w=&h=), and
  4. serves a compact JSON feed + the JPEGs over plain HTTP on your LAN.

Endpoints
  /                    status page with a preview of every slide
  /feed.json?topic=X   headlines (TOP, WORLD, NATION, BUSINESS, TECHNOLOGY,
                       ENTERTAINMENT, SPORTS, SCIENCE, HEALTH)
  /feed.json?q=text    headlines for a Google News search
  /img/<id>.jpg        the slide image for one headline (404 = none found)
"""
import argparse
import hashlib
import html
import io
import json
import re
import threading
import time
import unicodedata
import xml.etree.ElementTree as ET
from concurrent.futures import ThreadPoolExecutor
from email.utils import parsedate_to_datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, quote_plus, urljoin, urlparse

import requests
from PIL import Image, ImageOps

try:  # optional but strongly recommended: turns news.google.com links into real URLs
    from googlenewsdecoder import gnewsdecoder
except ImportError:  # pragma: no cover
    gnewsdecoder = None

DEFAULT_W, DEFAULT_H = 240, 176  # CYD slide; each device may ask for its own size with ?w=&h=
UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/124.0 Safari/537.36")
TOPICS = {"TOP", "WORLD", "NATION", "BUSINESS", "TECHNOLOGY",
          "ENTERTAINMENT", "SPORTS", "SCIENCE", "HEALTH"}

cfg = argparse.Namespace()       # filled in main()
log_lock = threading.Lock()


def log(*a):
    with log_lock:
        print(time.strftime("%H:%M:%S"), *a, flush=True)


# --------------------------------------------------------------------------- text
_REPL = {"‘": "'", "’": "'", "‚": "'", "“": '"', "”": '"',
         "„": '"', "–": "-", "—": "-", "―": "-", "…": "...",
         " ": " ", "•": "-", "·": "-", "−": "-"}


def ascii_clean(s: str) -> str:
    """The firmware uses an ASCII-only font, so fold everything down to ASCII."""
    s = html.unescape(s or "")
    s = "".join(_REPL.get(c, c) for c in s)
    s = unicodedata.normalize("NFKD", s).encode("ascii", "ignore").decode("ascii")
    return re.sub(r"\s+", " ", s).strip()


def age_str(pub: str) -> str:
    try:
        secs = time.time() - parsedate_to_datetime(pub).timestamp()
    except Exception:
        return ""
    if secs < 90:
        return "now"
    if secs < 3600:
        return f"{int(secs // 60)}m"
    if secs < 86400:
        return f"{int(secs // 3600)}h"
    return f"{int(secs // 86400)}d"


# --------------------------------------------------------------------------- images
OG_RE = [re.compile(p, re.I) for p in (
    r'<meta[^>]+(?:property|name)=["\']og:image(?::secure_url)?["\'][^>]*?content=["\']([^"\']+)',
    r'<meta[^>]+content=["\']([^"\']+)["\'][^>]+(?:property|name)=["\']og:image["\']',
    r'<meta[^>]+(?:property|name)=["\']twitter:image(?::src)?["\'][^>]*?content=["\']([^"\']+)',
    r'<meta[^>]+content=["\']([^"\']+)["\'][^>]+(?:property|name)=["\']twitter:image["\']',
)]


def resolve_url(gn_link: str):
    if gnewsdecoder is None:
        return None
    try:
        d = gnewsdecoder(gn_link, interval=0)
    except Exception as e:
        log("  decode failed:", type(e).__name__, str(e)[:80])
        return None
    if d.get("success") or d.get("status"):
        return d.get("decoded_url")
    return None


def read_limited(resp, limit):
    buf = io.BytesIO()
    for chunk in resp.iter_content(16384):
        buf.write(chunk)
        if buf.tell() >= limit:
            break
    return buf.getvalue()


def find_image_url(page_url: str):
    """Fetch the article page once and pull out its og:image / twitter:image tag.
    Publishers that refuse the request (401/403) are simply skipped - the CYD
    shows a placeholder for those headlines rather than working around the block."""
    r = requests.get(page_url, headers={"User-Agent": UA, "Accept-Language": "en"},
                     timeout=(5, 8), stream=True)
    r.raise_for_status()
    text = read_limited(r, 400_000).decode(r.encoding or "utf-8", errors="ignore")
    r.close()
    for rx in OG_RE:
        m = rx.search(text)
        if m:
            return urljoin(r.url, html.unescape(m.group(1)))
    return None


def make_slide(img_bytes: bytes, w: int, h: int):
    im = Image.open(io.BytesIO(img_bytes))
    im.load()
    if im.width < 160 or im.height < 90:          # logos / tracking pixels
        return None
    im = ImageOps.exif_transpose(im).convert("RGB")
    # centering y=0.3 keeps faces / headlines of tall photos in frame
    im = ImageOps.fit(im, (w, h), Image.LANCZOS, centering=(0.5, 0.3))
    # baseline (non-progressive) JPEG: TJpg_Decoder on the ESP32 can't do progressive.
    # Download buffers: 56 KB on the CYD, 128 KB (PSRAM) on the watch, so step quality down.
    limit = 48 * 1024 if w * h <= 60000 else 96 * 1024
    for q in (cfg.quality, 70, 60, 50, 40):
        out = io.BytesIO()
        im.save(out, "JPEG", quality=min(q, cfg.quality), progressive=False, optimize=False,
                subsampling=2)
        if out.tell() <= limit:
            break
    return out.getvalue()


def fetch_original(item_id: str, gn_link: str):
    """Find and download the article's lead image (full size). None if unavailable."""
    t0 = time.time()
    page = resolve_url(gn_link)
    if not page:
        log(f"  [{item_id}] no article URL")
        return None
    try:
        img_url = find_image_url(page)
        if not img_url:
            log(f"  [{item_id}] no og:image on {urlparse(page).netloc}")
            return None
        r = requests.get(img_url, headers={"User-Agent": UA, "Referer": page},
                         timeout=(5, 10), stream=True)
        r.raise_for_status()
        data = read_limited(r, 8_000_000)
        r.close()
        log(f"  [{item_id}] fetched {len(data)//1024}KB {urlparse(page).netloc} ({time.time()-t0:.1f}s)")
        return data
    except Exception as e:
        log(f"  [{item_id}] image failed: {type(e).__name__} {str(e)[:70]}")
        return None


# --------------------------------------------------------------------------- state
pool = None                      # ThreadPoolExecutor, created in main()
slides = {}                      # (item_id, w, h) -> Future[bytes|None]
slides_lock = threading.Lock()
originals = {}                   # item_id -> bytes|None, shared by every device / size
orig_locks = {}                  # item_id -> Lock, so one article is only fetched once
orig_guard = threading.Lock()
feeds = {}                       # (feed key, w, h) -> dict(time, data)
feeds_lock = threading.Lock()
MAX_SLIDES = 900
MAX_ORIGINALS = 80


def get_original(item_id: str, gn_link: str):
    with orig_guard:
        lock = orig_locks.setdefault(item_id, threading.Lock())
    with lock:
        if item_id in originals:
            return originals[item_id]
        data = fetch_original(item_id, gn_link)
        with orig_guard:
            originals[item_id] = data
            while len(originals) > MAX_ORIGINALS:             # drop oldest
                old = next(iter(originals))
                originals.pop(old, None)
                orig_locks.pop(old, None)
        return data


def build_slide(item_id: str, gn_link: str, w: int, h: int):
    data = get_original(item_id, gn_link)
    if not data:
        return None
    try:
        return make_slide(data, w, h)
    except Exception as e:
        log(f"  [{item_id}] resize failed: {type(e).__name__} {str(e)[:70]}")
        return None


def get_slide(item_id: str, gn_link: str, w: int, h: int):
    k = (item_id, w, h)
    with slides_lock:
        fut = slides.get(k)
        if fut is None:
            if len(slides) > MAX_SLIDES:                      # drop oldest
                for old in list(slides)[:60]:
                    slides.pop(old, None)
            fut = slides[k] = pool.submit(build_slide, item_id, gn_link, w, h)
        return fut


def rss_url(key: str):
    kind, _, val = key.partition(":")
    tail = f"hl={cfg.lang}&gl={cfg.country}&ceid={cfg.country}:{cfg.lang.split('-')[0]}"
    if kind == "q":
        return f"https://news.google.com/rss/search?q={quote_plus(val)}&{tail}"
    if val == "TOP":
        return f"https://news.google.com/rss?{tail}"
    return f"https://news.google.com/rss/headlines/section/topic/{val}?{tail}"


def build_feed(key: str, w: int, h: int):
    r = requests.get(rss_url(key), headers={"User-Agent": UA}, timeout=15)
    r.raise_for_status()
    channel = ET.fromstring(r.content).find("channel")
    label = ascii_clean(channel.findtext("title", "")).replace(" - Google News", "")
    # Candidate stories, in Google's order. We only keep the ones whose picture we can
    # actually load, and reach further down the list to replace the ones we can't.
    cands = []
    for it in channel.findall("item")[: min(40, max(cfg.count * 3, cfg.count))]:
        src = ascii_clean(it.findtext("source", ""))
        title = ascii_clean(it.findtext("title", ""))
        if src and title.endswith(" - " + src):
            title = title[: -(len(src) + 3)]
        link = it.findtext("link", "")
        item_id = hashlib.sha1(it.findtext("guid", link).encode()).hexdigest()[:12]
        cands.append({"id": item_id, "t": title, "s": src,
                      "a": age_str(it.findtext("pubDate", "")),
                      "img": f"/img/{item_id}.jpg?w={w}&h={h}", "_link": link})

    submitted = 0

    def submit_next():
        nonlocal submitted
        if submitted < len(cands):
            c = cands[submitted]
            c["_fut"] = get_slide(c["id"], c["_link"], w, h)
            submitted += 1

    for _ in range(cfg.count + 3):                            # start a few extra in parallel
        submit_next()

    chosen = []
    deadline = time.time() + cfg.feed_wait
    for c in cands:
        if len(chosen) >= cfg.count:
            break
        if "_fut" not in c:
            submit_next()                                      # reached on demand after failures
        try:
            ok = bool(c["_fut"].result(timeout=max(0.1, deadline - time.time())))
        except Exception:                                      # timed out
            ok = False
        if ok:
            chosen.append(c)
        else:
            submit_next()                                      # replace it with a fresh candidate
    log(f"feed {key}: {len(chosen)} stories with images from {submitted} tried")

    if not chosen:                                             # e.g. no internet / decoder missing
        log("WARNING: no images available at all - serving headlines only")
        chosen = cands[: cfg.count]
    return {"label": label or key, "updated": int(time.time()), "items": chosen}


def get_feed(key: str, w: int = DEFAULT_W, h: int = DEFAULT_H):
    fk = (key, w, h)
    with feeds_lock:
        ent = feeds.get(fk)
        if ent and time.time() - ent["time"] < cfg.refresh:
            return ent["data"]
    try:
        log(f"refreshing feed {key} ({w}x{h})")
        data = build_feed(key, w, h)
    except Exception as e:
        log("feed refresh failed:", e)
        if ent:
            return ent["data"]                                 # serve stale
        raise
    with feeds_lock:
        feeds[fk] = {"time": time.time(), "data": data}
    return data


def feed_key_from_query(qs):
    if "q" in qs and qs["q"][0].strip():
        return "q:" + qs["q"][0].strip()
    topic = (qs.get("topic", ["TOP"])[0] or "TOP").upper()
    return "t:" + (topic if topic in TOPICS else "TOP")


def size_from_query(qs):
    """Slide size a device wants (?w=410&h=250). Clamped to something sane."""
    def num(name, default):
        try:
            return max(64, min(600, int(qs.get(name, [default])[0])))
        except ValueError:
            return default
    return num("w", DEFAULT_W), num("h", DEFAULT_H)


def background_refresher():
    """Keep every feed that a client has asked for fresh, so slides are ready."""
    while True:
        time.sleep(30)
        for (key, w, h) in list(feeds):
            try:
                get_feed(key, w, h)
            except Exception:
                pass


# --------------------------------------------------------------------------- http
class Handler(BaseHTTPRequestHandler):
    server_version = "CYDNews/1.0"

    def log_message(self, fmt, *args):
        log(self.address_string(), fmt % args)

    def send_body(self, code, ctype, body, extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urlparse(self.path)
        qs = parse_qs(u.query)
        try:
            if u.path == "/feed.json":
                data = get_feed(feed_key_from_query(qs), *size_from_query(qs))
                public = {**data, "items": [{k: v for k, v in i.items() if k[0] != "_"}
                                            for i in data["items"]]}                # hide _link/_fut
                self.send_body(200, "application/json", json.dumps(public).encode())
            elif u.path.startswith("/img/") and u.path.endswith(".jpg"):
                self.serve_image(u.path[5:-4], *size_from_query(qs))
            elif u.path == "/":
                self.serve_status(qs)
            else:
                self.send_body(404, "text/plain", b"not found")
        except Exception as e:
            log("error:", type(e).__name__, e)
            self.send_body(500, "text/plain", str(e).encode())

    def serve_image(self, item_id, w, h):
        with slides_lock:
            fut = slides.get((item_id, w, h))
        if fut is None:
            return self.send_body(404, "text/plain", b"unknown id")
        try:
            jpg = fut.result(timeout=25)
        except Exception:
            jpg = None
        if not jpg:
            return self.send_body(404, "text/plain", b"no image")
        self.send_body(200, "image/jpeg", jpg, {"Cache-Control": "max-age=3600"})

    def serve_status(self, qs):
        w, h = size_from_query(qs)
        data = get_feed(feed_key_from_query(qs), w, h)
        rows = "".join(
            f"<div class=c><img src='{i['img']}' width={w} height={h} "
            f"onerror=\"this.style.background='#223';this.removeAttribute('src')\">"
            f"<b>{html.escape(i['t'])}</b><small>{html.escape(i['s'])} &middot; {i['a']}</small></div>"
            for i in data["items"])
        topics = " ".join(f"<a href='/?topic={t}&w={w}&h={h}'>{t}</a>" for t in sorted(TOPICS))
        page = (f"<!doctype html><meta charset=utf-8><title>CYD News Bridge</title>"
                "<style>body{font:14px system-ui;background:#111;color:#eee;margin:20px}"
                "a{color:#8cf;margin-right:10px}.g{display:flex;flex-wrap:wrap;gap:14px}"
                f".c{{width:{w}px;background:#1b1b22}}.c img{{display:block;width:{w}px;height:{h}px}}"
                ".c b{display:block;padding:6px 8px 2px}.c small{display:block;padding:0 8px 8px;color:#99a}"
                f"</style><h2>CYD News Bridge &mdash; {html.escape(data['label'])} ({w}x{h})</h2><p>{topics}</p>"
                f"<div class=g>{rows}</div>")
        self.send_body(200, "text/html; charset=utf-8", page.encode())


def main():
    global pool
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=9412)
    ap.add_argument("--count", type=int, default=12, help="stories per feed (default 12); only stories with a loadable image are kept")
    ap.add_argument("--feed-wait", type=float, default=15, help="max seconds to wait for images when building a feed (default 15)")
    ap.add_argument("--lang", default="en-US", help="Google News language, e.g. en-US, fr, de")
    ap.add_argument("--country", default="US", help="Google News country, e.g. US, GB, CA")
    ap.add_argument("--refresh", type=int, default=600, help="seconds between RSS refreshes")
    ap.add_argument("--quality", type=int, default=80, help="JPEG quality 1-95")
    ap.parse_args(namespace=cfg)
    if gnewsdecoder is None:
        log("WARNING: googlenewsdecoder not installed - no images. pip install -r requirements.txt")
    pool = ThreadPoolExecutor(max_workers=6)
    get_feed("t:TOP", DEFAULT_W, DEFAULT_H)                     # warm up
    threading.Thread(target=background_refresher, daemon=True).start()
    srv = ThreadingHTTPServer((cfg.host, cfg.port), Handler)
    log(f"CYD News Bridge listening on http://{cfg.host}:{cfg.port}  (open it in a browser to preview)")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
