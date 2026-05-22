"""tools/web/serve.py — v2.1.0 web mirror server stub.

Serves a static HTML+WASM bundle and a /events WebSocket that streams
the local bridge's snapshot/EVT lines to the browser. Browser side
renders the same LVGL scenes the device renders.

Stub: this version of the file does NOT actually compile the WASM
bundle or stream live events. It documents the contract and serves
a placeholder page. v2.1.x will:
  - emit /events JSONL from bridge subscription
  - serve build/static/lvgl.wasm (once tools/web/build_wasm.sh runs)
  - bridge the dash hello -> JSON capability advert to the browser

Usage::

    python tools/web/serve.py --bridge 127.0.0.1:7321 --listen 0.0.0.0:8765

Then point a browser at http://localhost:8765/.
"""

from __future__ import annotations

import argparse
import http.server
import json
import socketserver
import sys
from pathlib import Path


PLACEHOLDER_HTML = """\
<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8">
<title>esp32-agent-dashboard — web mirror (placeholder)</title>
<style>
body { font-family: ui-sans-serif, system-ui; background:#0b0a09; color:#f3eee2;
       margin:0; padding:48px; line-height:1.6; }
h1 { color:#2BB3B1; font-weight:600; }
code { background:#1c1814; padding:2px 6px; border-radius:4px; }
a { color:#B8431A; }
.dev { opacity:.6; font-size:.9em; margin-top:4em; }
</style></head><body>
<h1>esp32-agent-dashboard — web mirror</h1>
<p>v2.1.0 placeholder. The real mirror renders the same five LVGL
scenes the device renders, fed by your local bridge over WebSocket.</p>
<p>To get this working:</p>
<ol>
  <li>Install <a href="https://emscripten.org/">emsdk</a>.</li>
  <li><code>bash tools/web/build_wasm.sh</code> to build the LVGL
      WASM bundle.</li>
  <li><code>python tools/web/serve.py --bridge 127.0.0.1:7321</code>
      to start this server with live data.</li>
</ol>
<p class="dev">Tracking <a href="https://github.com/Caldis/esp32-agent-dashboard/issues">
the v2.1.x cycle</a> for full implementation.</p>
</body></html>
"""


class _Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path in ("/", "/index.html"):
            body = PLACEHOLDER_HTML.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path == "/healthz":
            self.send_response(200); self.end_headers()
            self.wfile.write(b'{"ok":true,"version":"v2.1.0-stub"}')
            return
        self.send_response(404); self.end_headers()

    def log_message(self, fmt, *args):
        # Quieter than default
        sys.stderr.write("[web] " + (fmt % args) + "\n")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--listen", default="127.0.0.1:8765")
    ap.add_argument("--bridge", default="127.0.0.1:7321",
                    help="local bridge to mirror (unused in stub)")
    args = ap.parse_args()
    host, _, port = args.listen.partition(":")
    port = int(port or "8765")
    print(f"[web] stub serving http://{host}:{port}/", file=sys.stderr)
    with socketserver.TCPServer((host, port), _Handler) as srv:
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
