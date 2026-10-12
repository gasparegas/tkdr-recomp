#!/usr/bin/env python3
"""Serve the web build with the COOP/COEP headers wasm threads need.

usage: serve.py DIR [PORT] [--iso PATH] [--eboot PATH]
  --iso exposes PATH at /game.iso (with Range support), --eboot at /eboot.bin:
  local testing only (?iso=/game.iso&eboot=/eboot.bin), never put these online.
"""
import http.server
import os
import sys

ISO = None
EBOOT = None


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map,
                      ".mjs": "text/javascript", ".js": "text/javascript", ".wasm": "application/wasm"}

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def do_GET(self):
        if ISO and self.path.split("?")[0] == "/game.iso":
            return self.send_iso()
        if EBOOT and self.path.split("?")[0] == "/eboot.bin":
            data = open(EBOOT, "rb").read()
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        return super().do_GET()

    def send_iso(self):
        size = os.path.getsize(ISO)
        start, end = 0, size - 1
        rng = self.headers.get("Range")
        if rng and rng.startswith("bytes="):
            a, b = rng[6:].split("-")
            start = int(a)
            end = min(int(b) if b else size - 1, size - 1)
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        else:
            self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        with open(ISO, "rb") as f:
            f.seek(start)
            self.wfile.write(f.read(end - start + 1))

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    args = sys.argv[1:]
    if "--iso" in args:
        i = args.index("--iso")
        ISO = os.path.abspath(args[i + 1])
        del args[i:i + 2]
    if "--eboot" in args:
        i = args.index("--eboot")
        EBOOT = os.path.abspath(args[i + 1])
        del args[i:i + 2]
    directory = args[0] if args else "."
    port = int(args[1]) if len(args) > 1 else 8080
    handler = lambda *a, **k: Handler(*a, directory=directory, **k)
    print(f"serving {directory} on http://localhost:{port}" + (f" (ISO at /game.iso)" if ISO else ""))
    http.server.ThreadingHTTPServer(("127.0.0.1", port), handler).serve_forever()
