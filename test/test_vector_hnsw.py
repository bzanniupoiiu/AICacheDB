#!/usr/bin/env python3
import os
import socket
import subprocess
import tempfile
import time
import unittest


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BIN = os.path.join(ROOT, "bin", "kvstore")


def encode_resp(*parts):
    out = [f"*{len(parts)}\r\n".encode("utf-8")]
    for part in parts:
        if not isinstance(part, bytes):
            part = str(part).encode("utf-8")
        out.append(f"${len(part)}\r\n".encode("utf-8"))
        out.append(part)
        out.append(b"\r\n")
    return b"".join(out)


class RespClient:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=3)

    def close(self):
        self.sock.close()

    def command(self, *parts):
        self.sock.sendall(encode_resp(*parts))
        return self._read_value()

    def _read_line(self):
        data = bytearray()
        while True:
            ch = self.sock.recv(1)
            if not ch:
                raise EOFError("connection closed")
            data += ch
            if data.endswith(b"\r\n"):
                return bytes(data[:-2])

    def _read_value(self):
        line = self._read_line()
        prefix = line[:1]
        body = line[1:]
        if prefix == b"+":
            return body.decode("utf-8")
        if prefix == b"-":
            raise AssertionError(body.decode("utf-8"))
        if prefix == b":":
            return int(body)
        if prefix == b"$":
            size = int(body)
            if size < 0:
                return None
            payload = b""
            while len(payload) < size + 2:
                payload += self.sock.recv(size + 2 - len(payload))
            return payload[:size].decode("utf-8")
        if prefix == b"*":
            return [self._read_value() for _ in range(int(body))]
        raise AssertionError(f"unknown RESP prefix: {prefix!r}")


class VectorHnswTest(unittest.TestCase):
    def setUp(self):
        if not os.path.exists(BIN):
            self.skipTest("kvstore binary is missing; run make first")

        self.tmp = tempfile.TemporaryDirectory()
        os.makedirs(os.path.join(self.tmp.name, "Persistence"), exist_ok=True)
        self.port = 8899
        self.config = os.path.join(self.tmp.name, "kvstore.conf")
        with open(self.config, "w", encoding="utf-8") as fp:
            fp.write(
                "bind 127.0.0.1\n"
                f"port {self.port}\n"
                "appendonly no\n"
                "appendfsync no\n"
            )

        self.proc = subprocess.Popen(
            [BIN, "-c", self.config],
            cwd=self.tmp.name,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        deadline = time.time() + 5
        while True:
            try:
                self.client = RespClient(self.port)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.05)

    def tearDown(self):
        if hasattr(self, "client"):
            self.client.close()
        if hasattr(self, "proc"):
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=3)
        if hasattr(self, "tmp"):
            self.tmp.cleanup()

    def test_vsearch_returns_topk_ranked_by_exact_cosine(self):
        self.assertEqual(
            self.client.command("VSET", "doc:apple", "apple fruit", "3", "1,0,0"),
            "OK",
        )
        self.assertEqual(
            self.client.command("VSET", "doc:banana", "banana fruit", "3", "0.9,0.1,0"),
            "OK",
        )
        self.assertEqual(
            self.client.command("VSET", "doc:car", "car engine", "3", "0,1,0"),
            "OK",
        )

        result = self.client.command("VSEARCH", "3", "1,0,0", "2", "0.0")

        self.assertEqual(len(result), 2)
        self.assertEqual(result[0][0], "doc:apple")
        self.assertEqual(result[0][1], "apple fruit")
        self.assertGreater(float(result[0][2]), float(result[1][2]))
        self.assertEqual(result[1][0], "doc:banana")


if __name__ == "__main__":
    unittest.main()
