import json
from pathlib import Path
import sys
import threading
import unittest
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.console import make_handler


class ConsoleTest(unittest.TestCase):
    def setUp(self):
        # Connection failures are handled without requiring a second database process.
        self.http = ThreadingHTTPServer(("127.0.0.1", 0), lambda *a: None)
        self.port = self.http.server_address[1]
        self.http.RequestHandlerClass = make_handler("127.0.0.1", 1, None, "test-token", self.port)
        self.thread = threading.Thread(target=self.http.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.http.shutdown()
        self.http.server_close()
        self.thread.join()

    def request(self, headers=None, data=None):
        req = urllib.request.Request(f"http://127.0.0.1:{self.port}/" + ("api/command" if data else ""),
                                     data=data, headers=headers or {})
        try:
            return urllib.request.urlopen(req)
        except urllib.error.HTTPError as e:
            return e

    def test_static_page_security_headers(self):
        with self.request() as r:
            self.assertEqual(r.status, 200)
            self.assertIn(b"AICacheDB", r.read())
            self.assertEqual(r.headers["X-Frame-Options"], "DENY")
            self.assertEqual(r.headers["Cache-Control"], "no-store")

    def test_unauthorized_origin_and_rebinding_rejected(self):
        for headers in [{}, {"Authorization": "Bearer test-token", "Origin": "https://evil.example"},
                        {"Authorization": "Bearer test-token", "Host": "evil.example"}]:
            with self.request(headers, b'["PING"]') as r:
                self.assertEqual(r.status, 403)

    def test_authenticated_errors_are_readable(self):
        headers = {"Authorization": "Bearer test-token"}
        with self.request(headers, b'["FLUSHALL"]') as r:
            self.assertEqual(r.status, 400)
            self.assertIn("Unsupported", json.load(r)["error"])
        with self.request(headers, b'["PING"]') as r:
            self.assertEqual(r.status, 400)
            self.assertIn("error", json.load(r))
