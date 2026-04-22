#!/usr/bin/env python3
"""Synthetic Atom feed server for stress-testing Elfeed2.

Run:
    python3 tests/fakefeeds.py [--port 8080] [--feeds 100]
                               [--entries-per-feed 20]
                               [--latency-min 0.05 --latency-max 1.5]
                               [--fail-rate 0.0]

It serves N synthetic feeds at /feed/0 ... /feed/N-1, with each
request optionally delayed (simulates slow upstreams) and
optionally failing with HTTP 500 at the requested rate
(exercises the failure-cap retry path). Entries are
deterministic per feed (driven by a per-feed RNG seed) so
re-fetching the same URL returns the same entries unless you
pass --rotate.

The script also prints an Elfeed2 config snippet to stdout that
you can paste into the file passed to `--config` on the
elfeed2 command line. Combine with `--db` for a fully isolated
test instance:

    python3 tests/fakefeeds.py --feeds 50 > /tmp/elfeed2-test.conf
    elfeed2 --db /tmp/elfeed2-test.db --config /tmp/elfeed2-test.conf

Press F5 inside Elfeed2 to fetch all feeds in parallel and
watch the Log panel + Downloads panel under realistic load.
"""

import argparse
import http.server
import random
import socketserver
import sys
import threading
import time
import xml.sax.saxutils as xml_escape


WORDS = (
    "alpha bravo charlie delta echo foxtrot golf hotel india juliet "
    "kilo lima mike november oscar papa quebec romeo sierra tango "
    "uniform victor whiskey xray yankee zulu metric system tabular "
    "feed channel article entry author publish update fragment "
    "newsletter podcast video audio binary stream chunk record"
).split()


def words(rng, n):
    return " ".join(rng.choice(WORDS) for _ in range(n))


def render_feed(feed_id: int, n_entries: int, rotate: bool) -> bytes:
    """Render an Atom feed as bytes. Deterministic per feed_id
    unless rotate=True (in which case content shifts by current
    minute, useful for testing 'new entries on refetch')."""
    seed = feed_id * 7919
    if rotate:
        seed += int(time.time()) // 60
    rng = random.Random(seed)
    title = "Fake Feed " + str(feed_id) + " — " + words(rng, 3)
    entries = []
    for i in range(n_entries):
        entry_id = "urn:fakefeed:%d:%d" % (feed_id, i)
        entry_title = words(rng, rng.randint(3, 10)).capitalize()
        # Entries dated within the last 30 days, deterministic
        published = (time.time() - rng.randint(0, 30 * 86400))
        # ISO-8601 UTC
        ts = time.strftime("%Y-%m-%dT%H:%M:%SZ",
                           time.gmtime(published))
        body = words(rng, rng.randint(40, 200))
        entries.append(
            "  <entry>\n"
            "    <id>%s</id>\n"
            "    <title>%s</title>\n"
            "    <updated>%s</updated>\n"
            "    <published>%s</published>\n"
            "    <link href='http://localhost/post/%d'/>\n"
            "    <content type='html'>%s</content>\n"
            "  </entry>\n"
            % (entry_id,
               xml_escape.escape(entry_title),
               ts, ts, feed_id,
               xml_escape.escape(body))
        )
    feed_xml = (
        "<?xml version='1.0' encoding='utf-8'?>\n"
        "<feed xmlns='http://www.w3.org/2005/Atom'>\n"
        "  <id>urn:fakefeed:%d</id>\n"
        "  <title>%s</title>\n"
        "  <updated>%s</updated>\n"
        "%s"
        "</feed>\n"
        % (feed_id, xml_escape.escape(title),
           time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
           "".join(entries))
    )
    return feed_xml.encode("utf-8")


class FakeFeedHandler(http.server.BaseHTTPRequestHandler):
    # Set by main() before serving
    options = None

    def log_message(self, fmt, *args):
        # Quiet the default per-request logging — too noisy under
        # stress; user can re-enable by removing this override.
        pass

    def do_GET(self):
        opt = self.options
        # Optional simulated latency.
        if opt.latency_max > 0:
            delay = random.uniform(opt.latency_min, opt.latency_max)
            time.sleep(delay)

        # Optional failure injection.
        if opt.fail_rate > 0 and random.random() < opt.fail_rate:
            self.send_response(500)
            self.end_headers()
            self.wfile.write(b"injected failure\n")
            return

        # Match /feed/N where 0 <= N < opt.feeds.
        path = self.path.strip("/")
        if not path.startswith("feed/"):
            self.send_response(404)
            self.end_headers()
            return
        try:
            feed_id = int(path[len("feed/"):])
        except ValueError:
            self.send_response(404)
            self.end_headers()
            return
        if feed_id < 0 or feed_id >= opt.feeds:
            self.send_response(404)
            self.end_headers()
            return

        body = render_feed(feed_id, opt.entries_per_feed, opt.rotate)
        self.send_response(200)
        self.send_header("Content-Type",
                         "application/atom+xml; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class ThreadingHTTPServer(socketserver.ThreadingMixIn,
                          http.server.HTTPServer):
    daemon_threads = True


def main():
    p = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--feeds", type=int, default=50,
        help="Number of synthetic feeds to serve (default 50)")
    p.add_argument("--entries-per-feed", type=int, default=20,
        help="Entries returned per feed (default 20)")
    p.add_argument("--latency-min", type=float, default=0.05,
        help="Min per-request delay seconds (default 0.05)")
    p.add_argument("--latency-max", type=float, default=1.5,
        help="Max per-request delay seconds (default 1.5)")
    p.add_argument("--fail-rate", type=float, default=0.0,
        help="Fraction of requests to fail with HTTP 500 "
             "(default 0; try 0.1 to exercise the retry path)")
    p.add_argument("--rotate", action="store_true",
        help="Roll the per-minute seed so refetches return new "
             "entries (exercises the merge / unread tag path)")
    p.add_argument("--print-config", action="store_true",
        help="Print an Elfeed2 config snippet listing every feed "
             "URL, then exit without serving.  Pipe to a file you "
             "pass via `elfeed2 --config`.")
    opt = p.parse_args()

    FakeFeedHandler.options = opt

    if opt.print_config:
        # Useful subset of global settings + every feed URL.
        print("# Generated by tests/fakefeeds.py")
        print("default-filter @1-month +unread")
        print("max-connections 16")
        print("preset h @1-month +unread")
        print("color fakefeeds #88c0d0")
        print()
        for i in range(opt.feeds):
            print("http://localhost:%d/feed/%d" % (opt.port, i))
            print("  title Fake Feed %d" % i)
            print("  tag fakefeeds")
            print()
        return

    server = ThreadingHTTPServer(("localhost", opt.port),
                                 FakeFeedHandler)
    print(("Serving %d feeds on http://localhost:%d/feed/0..%d "
           "(latency %.2f-%.2f s, fail rate %.0f%%)")
          % (opt.feeds, opt.port, opt.feeds - 1,
             opt.latency_min, opt.latency_max, opt.fail_rate * 100),
          file=sys.stderr)
    print("Generate config snippet with: --print-config",
          file=sys.stderr)
    print("Ctrl-C to stop.", file=sys.stderr)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("", file=sys.stderr)


if __name__ == "__main__":
    main()
