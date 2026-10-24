"""Dependency-free RESP client. Also usable as a command-line client.

    from tools.client import Client
    with Client() as db:
        db.set("hello", "你好", ttl=60)
        print(db.get("hello"))
"""
import argparse
import json
import os
import socket
import sys


class DatabaseError(Exception):
    """The server rejected a command; the connection remains usable."""


def encode(*parts):
    values = [p if isinstance(p, bytes) else str(p).encode("utf-8") for p in parts]
    data = b"*%d\r\n" % len(values) + b"".join(
        b"$%d\r\n" % len(p) + p + b"\r\n" for p in values
    )
    if len(data) > 1024 * 1024:
        raise ValueError("Request exceeds 1 MiB")
    return data


class Client:
    """One connection; not shared between threads. Writes are never retried automatically."""

    def __init__(self, host="127.0.0.1", port=8888, password=None, timeout=10, decode=True):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.stream = self.sock.makefile("rb")
        self.decode = decode
        try:
            if password:
                self.command("AUTH", password)
        except Exception:
            self.close()
            raise

    def close(self):
        self.stream.close()
        self.sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    def _read(self):
        line = self.stream.readline(1024)
        if not line.endswith(b"\r\n"):
            raise ConnectionError("Incomplete server reply")
        kind, body = line[:1], line[1:-2]
        if kind == b"-":
            raise DatabaseError(body.decode("utf-8", "replace"))
        if kind == b"+":
            return body.decode() if self.decode else body
        if kind == b":":
            return int(body)
        if kind == b"$":
            n = int(body)
            if n == -1:
                return None
            if n < 0 or n > 4 * 1024 * 1024:
                raise ConnectionError("Invalid reply length")
            data = self.stream.read(n + 2)
            if len(data) != n + 2 or data[-2:] != b"\r\n":
                raise ConnectionError("Incomplete bulk reply")
            data = data[:-2]
            return data.decode("utf-8", "replace") if self.decode else data
        if kind == b"*":
            n = int(body)
            if n < 0 or n > 10000:
                raise ConnectionError("Invalid array length")
            return [self._read() for _ in range(n)]
        raise ConnectionError("Unknown server reply")

    def command(self, *parts):
        if not parts:
            raise ValueError("A command is required")
        self.sock.sendall(encode(*parts))
        return self._read()

    def set(self, key, value, ttl=None):
        return self.command("SET", key, value, *(('EX', ttl) if ttl is not None else ()))

    def get(self, key):
        return self.command("GET", key)

    def delete(self, key):
        return self.command("DEL", key)

    def vset(self, key, answer, vector, ttl=None):
        return self.command("VSET", key, answer, len(vector), ",".join(map(str, vector)),
                            *(('EX', ttl) if ttl is not None else ()))

    def search(self, vector, topk=5, threshold=0.8, prefix=None):
        rows = self.command("VSEARCH", len(vector), ",".join(map(str, vector)), topk, threshold,
                            *(('PREFIX', prefix) if prefix is not None else ()))
        return [{"key": key, "answer": answer, "score": float(score)} for key, answer, score in rows]


def main():
    parser = argparse.ArgumentParser(description="AICacheDB command-line client")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8888)
    parser.add_argument("command", nargs="+", help='e.g. SET greeting "hello" EX 60')
    args = parser.parse_args()
    try:
        with Client(args.host, args.port, os.getenv("AICACHEDB_PASSWORD")) as db:
            print(json.dumps(db.command(*args.command), ensure_ascii=False, indent=2))
    except (OSError, DatabaseError, ValueError, ConnectionError) as exc:
        print(str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
