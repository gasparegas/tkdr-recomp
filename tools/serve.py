#!/usr/bin/env python3
"""Serve the web build with the COOP/COEP headers wasm threads need.

usage: serve.py DIR [PORT] [--iso PATH] [--eboot PATH] [--zip PATH]
  --iso exposes PATH at /game.iso, --eboot at /eboot.bin, --zip at /game.zip (all with
  Range support): local testing (?iso=/game.iso&eboot=/eboot.bin, or config.json
  {"pickFiles": false, "gameZip": "/game.zip"}). Never put these online.
"""
import http.server
import os
import sys

ISO = None
EBOOT = None
ZIP = None


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map,
                      ".mjs": "text/javascript", ".js": "text/javascript", ".wasm": "application/wasm"}

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def do_GET(self):
        path = self.path.split("?")[0]
        for url, file in (("/game.iso", ISO), ("/eboot.bin", EBOOT), ("/game.zip", ZIP)):
            if file and path == url:
                return self.send_file(file)
        return super().do_GET()

    def send_file(self, name):
        size = os.path.getsize(name)
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
        with open(name, "rb") as f:  # in chunks: these files are large
            f.seek(start)
            left = end - start + 1
            try:
                while left > 0:
                    chunk = f.read(min(left, 1 << 20))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    left -= len(chunk)
            except (BrokenPipeError, ConnectionResetError):
                pass  # the page stopped reading

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
    if "--zip" in args:
        i = args.index("--zip")
        ZIP = os.path.abspath(args[i + 1])
        del args[i:i + 2]
    directory = args[0] if args else "."
    port = int(args[1]) if len(args) > 1 else 8080
    handler = lambda *a, **k: Handler(*a, directory=directory, **k)
    print(f"serving {directory} on http://localhost:{port}" + (f" (ISO at /game.iso)" if ISO else ""))
    http.server.ThreadingHTTPServer(("127.0.0.1", port), handler).serve_forever()
