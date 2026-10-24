"""Run manually after docker build; isolated container is removed after verification."""
import argparse
import os
from pathlib import Path
import secrets
import socket
import subprocess
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.client import Client


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", default="aicachedb:daily")
    args = parser.parse_args()
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    name = "aicache-smoke-" + uuid.uuid4().hex[:12]
    env = dict(os.environ, AICACHEDB_PASSWORD=secrets.token_urlsafe(24))
    def docker(*parts):
        return subprocess.run(["docker", *parts], check=True, capture_output=True, env=env).stdout
    def connect():
        for _ in range(100):
            try:
                return Client(port=port, password=env["AICACHEDB_PASSWORD"])
            except OSError:
                time.sleep(.05)
        raise RuntimeError("Container did not become ready")
    try:
        docker("run", "-d", "--name", name, "-e", "AICACHEDB_PASSWORD", "-p", f"127.0.0.1:{port}:8888",
               "--memory", "256m", "--cap-drop", "ALL", "--security-opt", "no-new-privileges", "--read-only", args.image)
        with connect() as db:
            assert db.set("container:check", "persisted") == "OK"
            db.vset("container:vector", "answer", [1, 0])
            assert db.command("BACKUP") == "OK"
        docker("restart", name)
        with connect() as db:
            assert db.get("container:check") == "persisted"
            assert db.search([1, 0], prefix="container:")[0]["answer"] == "answer"
        assert docker("exec", name, "id", "-u").strip() == b"10001"
        print("Container: non-root, read-only root filesystem, durable volume and restart recovery passed.")
    finally:
        subprocess.run(["docker", "rm", "-f", "-v", name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == "__main__":
    main()
