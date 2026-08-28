#!/usr/bin/env python3
"""Serve one of the WASM harnesses to a browser, and collect what it reports.

    python3 serve.py spike          # the GX -> WGSL -> WebGPU spike
    python3 serve.py bench          # the guest-code benchmark
    python3 serve.py game           # the GXRuntime client (web/)
    python3 serve.py dolphin        # Dolphin in wasm (dolphin/web/)
    python3 serve.py bench --lan    # listen on every interface, for a phone
    python3 serve.py bench --lan --access   # log every request, to time a boot

`game` also accepts --iso <path>, which mounts a disc image at /disc.iso with
byte-range support.

--lan serves over HTTPS with a self-signed certificate, and it has to: WebGPU,
OPFS, AudioWorklet and SharedArrayBuffer are all secure-context features, and a
plain-HTTP LAN address is not a secure context. On a desktop 127.0.0.1 is, which
is why this only bites on a phone -- Safari there reports no navigator.gpu at
all. Safari will warn about the certificate once; tap through it. --no-https
opts out for the cases where plain HTTP is genuinely what you want. That is a development convenience so a run can be scripted
(index.html's ?iso=/disc.iso&auto=1); it never ships, and no disc image is ever
committed.

Pages POST their results back to /report; they are appended to reports.jsonl
next to this file and printed here. A PNG in the payload is written out as
frame.png, which is how spike/frame.png was produced.

--lan exposes the harness to your local network. It serves only this directory
and only for as long as it runs, but do not leave it running on a network you
do not trust.
"""
import base64
import datetime
import gzip
import shutil
import http.server
import json
import time
import os
import socket
import socketserver
import ssl
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = int(os.environ.get("PORT", "8712"))
CACHE_DIR = os.path.join(HERE, ".gzcache")


def make_handler(root, iso_path=None):
    class Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **k):
            super().__init__(*a, directory=root, **k)

        def do_GET(self):
            if iso_path and self.path.split("?")[0] == "/disc.iso":
                return self.serve_iso()
            if self.headers.get("Range"):
                return self.serve_range()
            gz = self.gzipped_path()
            if gz:
                return self.serve_gzip(gz)
            return super().do_GET()

        # A phone fetches the module over Wi-Fi, and wasm compresses about four
        # to one, so this is the difference between a ten-second wait and a
        # forty-second one. Compressed once and cached beside the file; a
        # Content-Encoding is transparent to WebAssembly.instantiateStreaming.
        def gzipped_path(self):
            path = self.translate_path(self.path)
            if not path.endswith((".wasm", ".js", ".data")):
                return None
            if "gzip" not in self.headers.get("Accept-Encoding", ""):
                return None
            if not os.path.isfile(path):
                return None
            real = os.path.realpath(path)
            cache = os.path.join(
                CACHE_DIR, real.replace(os.sep, "_").lstrip("_") + ".gz")
            try:
                if (not os.path.exists(cache) or
                        os.path.getmtime(cache) < os.path.getmtime(real)):
                    os.makedirs(CACHE_DIR, exist_ok=True)
                    with open(real, "rb") as src, gzip.open(cache + ".tmp", "wb",
                                                            compresslevel=6) as dst:
                        shutil.copyfileobj(src, dst, 1 << 20)
                    os.replace(cache + ".tmp", cache)
            except OSError:
                return None
            return (path, cache)

        def serve_gzip(self, pair):
            path, cache = pair
            size = os.path.getsize(cache)
            # An ETag turns a reload from a 96 MB transfer into a 304. That is
            # the difference between iterating on a phone and not: without a
            # validator "no-cache" means refetch, and the A/B run reloads the
            # page on purpose halfway through. The tag is the source file's
            # mtime and size, so a rebuild invalidates it and nothing else does.
            st = os.stat(path)
            etag = '"%x-%x"' % (int(st.st_mtime), st.st_size)
            if self.headers.get("If-None-Match") == etag:
                self.send_response(304)
                self.send_header("ETag", etag)
                self.send_header("Cache-Control", "no-cache")
                self.end_headers()
                return
            self.send_response(200)
            self.send_header("Content-Type", self.guess_type(path))
            self.send_header("Content-Encoding", "gzip")
            self.send_header("Content-Length", str(size))
            self.send_header("Cache-Control", "no-cache")
            self.send_header("ETag", etag)
            self.end_headers()
            with open(cache, "rb") as f:
                try:
                    shutil.copyfileobj(f, self.wfile, 1 << 20)
                except (BrokenPipeError, ConnectionResetError):
                    return

        def do_HEAD(self):
            if iso_path and self.path.split("?")[0] == "/disc.iso":
                return self.serve_iso(head_only=True)
            if self.headers.get("Range"):
                # A HEAD asks about the resource, not about a slice of it, and
                # answering 206 leaves the client to trust that Content-Length
                # describes the whole file. Chrome does; Safari does not appear
                # to, and the difference was a 1.4 GB download per boot. A plain
                # 200 with Accept-Ranges says the same thing unambiguously.
                path = self.translate_path(self.path)
                if os.path.isfile(path):
                    self.send_response(200)
                    self.send_header("Content-Type", self.guess_type(path))
                    self.send_header("Accept-Ranges", "bytes")
                    self.send_header("Content-Length", str(os.path.getsize(path)))
                    self.end_headers()
                    return
                return self.serve_range(head_only=True)
            return super().do_HEAD()

        def serve_range(self, head_only=False):
            """Range requests for any file under the served directory.

            SimpleHTTPRequestHandler ignores Range and sends the whole file with
            a 200, which a client that asked for 64 KB of a 75 MB movie takes as
            "here are the first 64 KB" -- wrong data, no error. WASMFS's fetch
            backend is exactly that client: it is what lets a browser mount a
            1.2 GB disc without downloading it.
            """
            path = self.translate_path(self.path)
            if os.path.isdir(path) or not os.path.isfile(path):
                return super().do_GET()
            size = os.path.getsize(path)
            spec = self.headers["Range"][6:].split(",")[0]
            a, _, b = spec.partition("-")
            if a:
                start = int(a)
                end = int(b) if b else size - 1
            else:
                start = max(0, size - int(b))
                end = size - 1
            end = min(end, size - 1)
            if start >= size:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{size}")
                self.end_headers()
                return
            length = end - start + 1
            self.send_response(206)
            self.send_header("Content-Type", self.guess_type(path))
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(length))
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.end_headers()
            if head_only:
                return
            with open(path, "rb") as f:
                f.seek(start)
                remaining = length
                while remaining > 0:
                    chunk = f.read(min(1 << 20, remaining))
                    if not chunk:
                        break
                    try:
                        self.wfile.write(chunk)
                    except (BrokenPipeError, ConnectionResetError):
                        return
                    remaining -= len(chunk)

        def serve_iso(self, head_only=False):
            size = os.path.getsize(iso_path)
            start, end = 0, size - 1
            rng = self.headers.get("Range")
            partial = False
            if rng and rng.startswith("bytes="):
                spec = rng[6:].split(",")[0]
                a, _, b = spec.partition("-")
                if a:
                    start = int(a)
                    end = int(b) if b else size - 1
                else:
                    start = max(0, size - int(b))
                end = min(end, size - 1)
                partial = True
            length = max(0, end - start + 1)
            self.send_response(206 if partial else 200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(length))
            if partial:
                self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.end_headers()
            if head_only:
                return
            with open(iso_path, "rb") as f:
                f.seek(start)
                remaining = length
                while remaining > 0:
                    chunk = f.read(min(1 << 20, remaining))
                    if not chunk:
                        break
                    try:
                        self.wfile.write(chunk)
                    except (BrokenPipeError, ConnectionResetError):
                        return
                    remaining -= len(chunk)

        def end_headers(self):
            # Cross-origin isolation, so SharedArrayBuffer and wasm threads are
            # available to test. WebKit ignores these on custom schemes, which
            # is why the harness is served over HTTP rather than from a bundle.
            self.send_header("Cross-Origin-Opener-Policy", "same-origin")
            self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
            # Disc contents do not change while the harness is up, and a phone
            # that has to re-fetch them on every reload cannot be iterated on.
            # Everything else stays uncacheable, and anything that already set
            # its own Cache-Control (the gzip path, with its ETag) keeps it.
            # Emscripten's fetch backend decides between ranged reads and
            # downloading the whole file by looking for Accept-Ranges on the
            # response to its probe. SimpleHTTPRequestHandler does not send it,
            # so any game file that reaches the default path advertises no
            # range support and gets pulled down in full.
            if self.path.startswith("/game/"):
                self.send_header("Accept-Ranges", "bytes")
            if not getattr(self, "_cache_set", False):
                cacheable = (self.path.startswith("/game/") and
                             not self.path.endswith(".manifest"))
                self.send_header("Cache-Control",
                                 "public, max-age=86400" if cacheable else "no-store")
            super().end_headers()
            if ACCESS_LOG:
                print("%.3f %s %s %s %s %s"
                      % (time.time() - T_START, self.client_address[0],
                         self.command, self.path, getattr(self, "_status", "-"),
                         getattr(self, "_sent_length", "-")), file=sys.stderr)
                sys.stderr.flush()

        def do_POST(self):
            n = int(self.headers.get("Content-Length", 0))
            rec = json.loads(self.rfile.read(n))
            png = rec.pop("png", None)
            if png and png.startswith("data:image/png;base64,"):
                # A single frame.png is one arbitrary moment, and a game spends
                # plenty of them on black. A record carrying `shot` gets its own
                # numbered file so a run leaves a filmstrip.
                shot = rec.get("shot")
                name = f"frame-{int(shot):04d}.png" if shot is not None else "frame.png"
                out = os.path.join(HERE, name)
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

        # Quiet by default -- a boot is thousands of requests and the reports
        # are the interesting output. --access turns it on, which is how a
        # phone's boot gets taken apart: over Wi-Fi a round trip costs what a
        # whole file costs on this machine, so the count and the shape of the
        # requests is the thing to look at, not the bytes.
        # Content-Length is the number that matters -- a boot's cost is bytes
        # over Wi-Fi, and a request count says nothing about them when one GET
        # can be 64 KB or 85 MB. The line is written from end_headers because
        # the base class logs from send_response, before any header exists.
        def send_response_only(self, code, message=None):
            self._status = code
            super().send_response_only(code, message)

        def send_header(self, key, value):
            k = key.lower()
            if k == "content-length":
                self._sent_length = value
            if k == "cache-control":
                self._cache_set = True
            super().send_header(key, value)

        def log_message(self, *a):
            pass

    return Handler


def all_addresses():
    """Every non-loopback IPv4 address on this machine, by interface."""
    found = {}
    try:
        out = subprocess.run(["ifconfig"], capture_output=True, text=True,
                             check=True).stdout
        interface = None
        for line in out.splitlines():
            if line and not line[0].isspace():
                interface = line.split(":", 1)[0]
            elif "inet " in line and interface:
                addr = line.split("inet ", 1)[1].split()[0]
                if addr != "127.0.0.1":
                    found.setdefault(interface, addr)
    except Exception:
        pass
    return found


def lan_ip():
    """The address a phone on the same Wi-Fi can actually reach.

    Connecting a UDP socket to a public address reports whatever the default
    route uses -- which on a machine with a VPN is the tunnel, an address no
    phone on the local network can reach. Prefer a real interface (en0/en1) and
    fall back to the routed address only when there is no better answer.
    """
    addresses = all_addresses()
    for name in ("en0", "en1", "en2", "bridge100"):
        if name in addresses:
            return addresses[name]
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    finally:
        s.close()


CERT_DIR = os.path.join(HERE, ".devcert")


def dev_certificate(addresses):
    """A self-signed certificate covering every address this server answers on.

    Regenerated whenever the address set changes, so moving between networks
    does not silently serve a certificate for the wrong host.
    """
    os.makedirs(CERT_DIR, exist_ok=True)
    cert = os.path.join(CERT_DIR, "cert.pem")
    key = os.path.join(CERT_DIR, "key.pem")
    stamp = os.path.join(CERT_DIR, "addresses")
    wanted = "\n".join(sorted(set(addresses) | {"127.0.0.1", "localhost"}))

    if os.path.exists(cert) and os.path.exists(key) and os.path.exists(stamp):
        with open(stamp) as f:
            if f.read() == wanted:
                return cert, key

    sans = ",".join(
        ("IP:" + a) if a[0].isdigit() else ("DNS:" + a)
        for a in wanted.split("\n"))
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-sha256",
         "-days", "365", "-nodes",
         "-keyout", key, "-out", cert,
         "-subj", "/CN=dolweb-dev",
         "-addext", "subjectAltName=" + sans],
        check=True, capture_output=True)
    with open(stamp, "w") as f:
        f.write(wanted)
    return cert, key


ACCESS_LOG = False
T_START = time.time()


def main(argv):
    global ACCESS_LOG
    ACCESS_LOG = "--access" in argv
    which = argv[1] if len(argv) > 1 else "spike"
    if which not in ("spike", "bench", "game", "dolphin"):
        print(__doc__)
        return 2
    iso_path = None
    if "--iso" in argv:
        iso_path = os.path.abspath(argv[argv.index("--iso") + 1])
        if not os.path.isfile(iso_path):
            print(f"no such disc image: {iso_path}", file=sys.stderr)
            return 1
    root = os.path.join(HERE, "web") if which == "game" \
        else os.path.join(HERE, which, "web")
    if not os.path.isdir(root):
        print(f"no such harness: {root} (run {which}/build.sh first)", file=sys.stderr)
        return 1

    lan = "--lan" in argv
    # Secure context or nothing: see the module docstring.
    https = lan and "--no-https" not in argv
    host = "" if lan else "127.0.0.1"
    socketserver.TCPServer.allow_reuse_address = True
    socketserver.TCPServer.request_queue_size = 16
    class Server(socketserver.ThreadingTCPServer):
        daemon_threads = True
        allow_reuse_address = True
    with Server((host, PORT), make_handler(root, iso_path)) as httpd:
        shown = lan_ip() if lan else "127.0.0.1"
        scheme = "https" if https else "http"
        if https:
            cert, key = dev_certificate(list(all_addresses().values()))
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(cert, key)
            httpd.socket = context.wrap_socket(httpd.socket, server_side=True)
        print(f"serving {which} at  {scheme}://{shown}:{PORT}/")
        if iso_path:
            print(f"disc mounted at /disc.iso  ({iso_path})")
        if lan:
            print("(reachable from any device on this network)")
            # One machine can have several addresses and only some of them
            # reach a phone -- a VPN tunnel is the default route here but no
            # phone on the Wi-Fi can use it. Print them all and say which is
            # which rather than guess.
            others = [(name, addr) for name, addr in all_addresses().items()
                      if addr != shown]
            for name, addr in others:
                kind = ("tailnet" if addr.startswith("100.") else
                        "vpn tunnel" if name.startswith("utun") else name)
                print(f"  also {scheme}://{addr}:{PORT}/   ({kind})")
            if https:
                print("  (self-signed certificate: Safari warns once, "
                      "tap Show Details -> visit this website)")
        sys.stdout.flush()
        httpd.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
