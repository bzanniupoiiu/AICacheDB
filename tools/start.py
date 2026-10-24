"""Build and run the database plus its local browser console. Linux / WSL only."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", default=str(ROOT / "config.example.conf"))
    parser.add_argument("--web-port", type=int, default=8099)
    args = parser.parse_args()
    if os.name != "posix":
        parser.error("Run this launcher on Linux / WSL. On Windows use the SSH workflow in README.md.")
    config = Path(args.config).resolve()
    subprocess.run(["make"], cwd=ROOT, check=True)
    subprocess.run([str(ROOT / "bin/kvstore"), "-c", str(config), "--check-config"], check=True)
    values = {}
    for line in config.read_text().splitlines():
        if line.strip() and not line.lstrip().startswith("#"):
            key, value = line.strip().split(None, 1)
            values[key] = value
    env = os.environ.copy()
    if "AICACHEDB_PASSWORD" not in env and "requirepass" in values:
        env["AICACHEDB_PASSWORD"] = values["requirepass"]
    db = subprocess.Popen([str(ROOT / "bin/kvstore"), "-c", str(config)], cwd=ROOT, env=env)
    console = None
    try:
        # Fail visibly if a conflicting listener or invalid data prevents startup.
        time.sleep(.4)
        if db.poll() is not None:
            return db.returncode or 1
        host = values.get("bind", "127.0.0.1")
        if host == "0.0.0.0":
            host = "127.0.0.1"
        console = subprocess.Popen([sys.executable, str(ROOT / "tools/console.py"), "--host", host,
                                    "--port", values.get("port", "8888"), "--web-port", str(args.web_port)],
                                   cwd=ROOT, env=env)
        print("Database and console started. Open the link below. Ctrl+C stops both.", flush=True)
        while db.poll() is None and console.poll() is None:
            time.sleep(.25)
        return 1
    except KeyboardInterrupt:
        return 0
    finally:
        for proc in (console, db):
            if proc and proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


if __name__ == "__main__":
    sys.exit(main())
