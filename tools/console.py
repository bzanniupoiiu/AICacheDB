"""Local browser console. Connect remotely through SSH forwarding, never expose publicly."""
import argparse
import hmac
import json
import os
from pathlib import Path
import secrets
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    from .client import Client, DatabaseError
except ImportError:
    from client import Client, DatabaseError

PAGE = Path(__file__).with_name("console.html")
ALLOWED = {"PING", "INFO", "SCAN", "GET", "SET", "DEL", "TYPE", "TTL", "EXPIRE",
           "VGET", "VSET", "VSEARCH", "DBSIZE", "SAVE", "BACKUP"}


def make_handler(host, port, password, token, webport):
    class Handler(BaseHTTPRequestHandler):
        def setup(self):
            super().setup()
            self.connection.settimeout(10)

        def log_message(self, *args):
            pass  # Never log queries, tokens or cached content.

        def send(self, status, data, mime="application/json; charset=utf-8"):
            self.send_response(status)
            self.send_header("Content-Type", mime)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("X-Frame-Options", "DENY")
            self.send_header("Referrer-Policy", "no-referrer")
            self.send_header("Content-Security-Policy", "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'")
            self.end_headers()
            self.wfile.write(data)

        def host_ok(self):
            return self.headers.get("Host") in {f"127.0.0.1:{webport}", f"localhost:{webport}"}

        def do_GET(self):
            if not self.host_ok():
                return self.send(403, b'{"error":"Invalid host"}')
            if self.path not in ("/", "/favicon.ico"):
                return self.send(404, b'{}')
            if self.path == "/favicon.ico":
                return self.send(204, b"")
            self.send(200, PAGE.read_bytes(), "text/html; charset=utf-8")

        def do_POST(self):
            origin = self.headers.get("Origin")
            if (not self.host_ok() or (origin and origin not in
                    {f"http://127.0.0.1:{webport}", f"http://localhost:{webport}"}) or
                    not hmac.compare_digest(self.headers.get("Authorization", ""), "Bearer " + token)):
                return self.send(403, b'{"error":"Open the console using its startup link."}')
            if self.path != "/api/command":
                return self.send(404, b'{}')
            try:
                n = int(self.headers.get("Content-Length", "0"))
                if n < 1 or n > 1024 * 1024:
                    raise ValueError("Request size exceeds limit")
                data = json.loads(self.rfile.read(n))
                if (not isinstance(data, list) or not data or len(data) > 16 or
                        any(not isinstance(v, (str, int, float)) for v in data) or
                        str(data[0]).upper() not in ALLOWED):
                    raise ValueError("Unsupported command")
                with Client(host, port, password) as db:
                    value = db.command(*data)
                self.send(200, json.dumps({"result": value}, ensure_ascii=False).encode())
            except (OSError, DatabaseError, ValueError, ConnectionError) as exc:
                self.send(400, json.dumps({"error": str(exc)}, ensure_ascii=False).encode())
    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1", help="Database host")
    parser.add_argument("--port", type=int, default=8888, help="Database port")
    parser.add_argument("--web-port", type=int, default=8099)
    args = parser.parse_args()
    token = secrets.token_urlsafe(32)
    handler = make_handler(args.host, args.port, os.getenv("AICACHEDB_PASSWORD"), token, args.web_port)
    with ThreadingHTTPServer(("127.0.0.1", args.web_port), handler) as server:
        print(f"Open http://127.0.0.1:{args.web_port}/#{token}", flush=True)
        print("Console is local only. Press Ctrl+C to stop.", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
