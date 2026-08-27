#!/usr/bin/env python3
"""Serve one of the WASM harnesses to a browser, and collect what it reports.

    python3 serve.py spike          # the GX -> WGSL -> WebGPU spike
    python3 serve.py bench          # the guest-code benchmark
    python3 serve.py bench --lan    # listen on every interface, for a phone

Pages POST their results back to /report; they are appended to reports.jsonl
next to this file and printed here. A PNG in the payload is written out as
frame.png, which is how spike/frame.png was produced.

--lan exposes the harness to your local network. It serves only this directory
and only for as long as it runs, but do not leave it running on a network you
do not trust.
"""
import base64
import datetime
import http.server
import json
import os
import socket
import socketserver
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = int(os.environ.get("PORT", "8712"))


def make_handler(root):
    class Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **k):
            super().__init__(*a, directory=root, **k)

        def end_headers(self):
            # Cross-origin isolation, so SharedArrayBuffer and wasm threads are
            # available to test. WebKit ignores these on custom schemes, which
            # is why the harness is served over HTTP rather than from a bundle.
            self.send_header("Cross-Origin-Opener-Policy", "same-origin")
            self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
            self.send_header("Cache-Control", "no-store")
            super().end_headers()

        def do_POST(self):
            n = int(self.headers.get("Content-Length", 0))
            rec = json.loads(self.rfile.read(n))
            png = rec.pop("png", None)
            if png and png.startswith("data:image/png;base64,"):
                out = os.path.join(HERE, "frame.png")
                with open(out, "wb") as f:
                    f.write(base64.b64decode(png.split(",", 1)[1]))
                rec["png_saved"] = out
            rec["received"] = datetime.datetime.now().isoformat()
            with open(os.path.join(HERE, "reports.jsonl"), "a") as f:
                f.write(json.dumps(rec) + "\n")
            print("\n=== REPORT ===")
            print(json.dumps(rec, indent=1))
            sys.stdout.flush()
            self.send_response(204)
            self.end_headers()

        def log_message(self, *a):
            pass

    return Handler


def lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    finally:
        s.close()


def main(argv):
    which = argv[1] if len(argv) > 1 else "spike"
    if which not in ("spike", "bench"):
        print(__doc__)
        return 2
    root = os.path.join(HERE, which, "web")
    if not os.path.isdir(root):
        print(f"no such harness: {root} (run {which}/build.sh first)", file=sys.stderr)
        return 1

    lan = "--lan" in argv
    host = "" if lan else "127.0.0.1"
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer((host, PORT), make_handler(root)) as httpd:
        shown = lan_ip() if lan else "127.0.0.1"
        print(f"serving {which} at  http://{shown}:{PORT}/")
        if lan:
            print("(reachable from any device on this network)")
        sys.stdout.flush()
        httpd.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
