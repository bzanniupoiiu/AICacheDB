import concurrent.futures
import json
import math
import os
from pathlib import Path
import random
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.client import Client, DatabaseError, encode


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class DailyTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name)
        self.port = free_port()
        self.proc = None
        self.log = open(self.path / "server.log", "w+b")
        self.addCleanup(self.log.close)
        self.addCleanup(self.stop)
        self.start()

    def start(self, extra="", preexec=None):
        self.config = self.path / "daily.conf"
        self.config.write_text(f"bind 127.0.0.1\nport {self.port}\ndir {self.path}/data\n" + extra)
        self.proc = subprocess.Popen([str(ROOT / "bin/kvstore"), "-c", str(self.config)],
                                     stdout=self.log, stderr=self.log, preexec_fn=preexec)
        for _ in range(200):
            if self.proc.poll() is not None:
                self.log.seek(0)
                self.fail(self.log.read().decode(errors="replace"))
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=.1):
                    return
            except OSError:
                time.sleep(.01)
        self.fail("server did not become ready")

    def stop(self, kill=False):
        if self.proc and self.proc.poll() is None:
            self.proc.kill() if kill else self.proc.terminate()
            self.proc.wait(timeout=10)
            if not kill:
                self.assertEqual(self.proc.returncode, 0, "server exited abnormally")

    def db(self, **kwargs):
        return Client(port=self.port, **kwargs)

    def restart(self):
        self.stop(kill=True)
        self.start()

    def test_binary_unicode_empty_case_and_types(self):
        with self.db(decode=False) as db:
            for key, value in [(b"a\x00b", b"x\x00y\r\nz"), (b"", b""), ("笔记".encode(), "你好世界".encode())]:
                self.assertEqual(db.command("sEt", key, value), b"OK")
                self.assertEqual(db.get(key), value)
            db.vset("vector", "answer", [1, 0])
            with self.assertRaises(DatabaseError):
                db.get("vector")
            self.assertEqual(db.command("VGET", "vector"), [b"vector", b"answer"])

    def test_standard_setex_and_expiry_survive_restart(self):
        with self.db() as db:
            db.command("SETEX", "short", 1, "hello")
            db.command("SET", "long", "value", "PX", 10000)
            before = db.command("PTTL", "long")
        time.sleep(1.05)
        self.restart()
        with self.db() as db:
            self.assertIsNone(db.get("short"))
            self.assertEqual(db.get("long"), "value")
            self.assertLess(db.command("PTTL", "long"), before - 800)
            self.assertEqual(db.command("TTL", "missing"), -2)
            self.assertEqual(db.command("PERSIST", "long"), 1)
            self.assertEqual(db.command("TTL", "long"), -1)
            self.assertEqual(db.command("EXPIRE", "long", 0), 1)
            self.assertIsNone(db.get("long"))

    def test_nx_and_validation(self):
        with self.db() as db:
            self.assertEqual(db.command("SET", "x", "1", "NX"), "OK")
            self.assertIsNone(db.command("SET", "x", "2", "EX", 60, "NX"))
            self.assertEqual(db.get("x"), "1")
            for parts in [("SET", "x", "v", "EX", "bad"), ("EXPIRE", "x", "9" * 40),
                          ("SET", "x", "v", "NX", "NX"), ("GET",), ("VSEARCH", "0", "", 1, 0),
                          ("VSET", "q", "a", 2, "1,,0"), ("VSET", "q", "a", 2, "nan,1"),
                          ("VSET", "q", "a", 2, "0,0"), ("VSET", "q", "a", 2, "1,0,0")]:
                with self.subTest(parts=parts), self.assertRaises(DatabaseError):
                    db.command(*parts)
            self.assertEqual(db.get("x"), "1")

    def test_pipeline_fragmentation_and_half_close(self):
        with self.db() as db:
            payload = encode("SET", "x", "hello") + encode("GET", "x") + encode("PING")
            for byte in payload:
                db.sock.sendall(bytes([byte]))
            self.assertEqual([db._read(), db._read(), db._read()], ["OK", "hello", "PONG"])
            db.sock.sendall(encode("PING"))
            db.sock.shutdown(socket.SHUT_WR)
            self.assertEqual(db._read(), "PONG")

    def test_half_closed_pipeline_is_fully_drained(self):
        with self.db() as db:
            db.sock.sendall(encode("PING") * 200)
            db.sock.shutdown(socket.SHUT_WR)
            self.assertTrue(all(db._read() == "PONG" for _ in range(200)))

    def test_pipeline_fairness_and_large_value(self):
        with self.db(decode=False) as db:
            value = b"x" * (512 * 1024)
            db.set(b"large", value)
            self.assertEqual(db.get(b"large"), value)
            db.sock.sendall(encode("PING") * 300)
            self.assertTrue(all(db._read() == b"PONG" for _ in range(300)))
        with self.db() as db:
            self.assertEqual(db.command("PING"), "PONG")

    def test_invalid_protocol_does_not_harm_other_clients(self):
        for data in [b"*99999999999999999999\r\n", b"*1\r\n$-1\r\n", b"*1\r\n$99999999\r\n", b"INVALID\r\n"]:
            with self.db() as db:
                db.sock.sendall(data)
                with self.assertRaises(DatabaseError):
                    db._read()
        with self.db() as db:
            self.assertEqual(db.command("PING"), "PONG")

    def test_persistence_update_delete_vector_and_backup(self):
        with self.db(decode=False) as db:
            db.set("key", b"before\0")
            db.set("key", b"after\0")
            db.set("deleted", "x")
            db.delete("deleted")
            db.vset("model:v1:q", "答案", [1, 0, 0])
            self.assertEqual(db.command("BACKUP"), b"OK")
            self.assertTrue((self.path / "data/backup.aof").stat().st_size)
            db.set("after-backup", "not in backup")
            self.assertEqual(db.command("SAVE"), b"OK")
        self.restart()
        with self.db(decode=False) as db:
            self.assertEqual(db.get("key"), b"after\0")
            self.assertIsNone(db.get("deleted"))
            self.assertEqual(db.search([1, 0, 0], prefix="model:v1:")[0]["answer"], "答案".encode())
        self.stop()
        backup = self.path / "data/backup.aof"
        (self.path / "data/cache.aof").write_bytes(backup.read_bytes())
        self.start()
        with self.db() as db:
            self.assertIsNone(db.get("after-backup"))
            self.assertEqual(db.get("key"), "after\0")

    def test_incomplete_tail_repaired_then_future_writes_survive(self):
        with self.db() as db:
            db.set("good", "yes")
        self.stop(kill=True)
        journal = self.path / "data/cache.aof"
        good = journal.stat().st_size
        with journal.open("ab") as fp:
            fp.write(b"AIC1\x20")
        self.start()
        self.assertEqual(journal.stat().st_size, good)
        with self.db() as db:
            self.assertEqual(db.get("good"), "yes")
            db.set("new", "yes")
        self.restart()
        with self.db() as db:
            self.assertEqual(db.get("new"), "yes")

    def test_corruption_refuses_start_without_modifying_file(self):
        with self.db() as db:
            db.set("key", "value")
        self.stop()
        journal = self.path / "data/cache.aof"
        original = bytearray(journal.read_bytes())
        original[-1] ^= 1
        journal.write_bytes(original)
        result = subprocess.run([str(ROOT / "bin/kvstore"), "-c", str(self.config)], capture_output=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"corrupt", result.stderr)
        self.assertEqual(journal.read_bytes(), original)

    def test_bad_header_refuses_start(self):
        self.stop()
        (self.path / "data/cache.aof").write_bytes(b"AIC1" + b"\xff" * 12)
        result = subprocess.run([str(ROOT / "bin/kvstore"), "-c", str(self.config)], capture_output=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)

    def test_directory_lock_and_bind_failure(self):
        result = subprocess.run([str(ROOT / "bin/kvstore"), "-c", str(self.config)], capture_output=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"already in use", result.stderr)
        other = self.path / "other.conf"
        other.write_text(f"port {self.port}\nappendonly no\n")
        result = subprocess.run([str(ROOT / "bin/kvstore"), "-c", str(other)], capture_output=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)

    def test_auth_and_nonloopback_guard(self):
        self.stop()
        self.start("requirepass test-secret\n")
        with self.db() as db:
            with self.assertRaises(DatabaseError):
                db.command("PING")
            with self.assertRaises(DatabaseError):
                db.command("AUTH", "wrong")
            self.assertEqual(db.command("AUTH", "test-secret"), "OK")
            self.assertEqual(db.command("PING"), "PONG")
        with self.db(password="test-secret") as db:
            self.assertEqual(db.command("PING"), "PONG")
        cfg = self.path / "unsafe.conf"
        cfg.write_text("bind 0.0.0.0\n")
        result = subprocess.run([str(ROOT / "bin/kvstore"), "-c", str(cfg), "--check-config"], capture_output=True)
        self.assertNotEqual(result.returncode, 0)

    def test_one_command_launcher_stops_its_children(self):
        self.stop()
        webport = free_port()
        proc = subprocess.Popen([sys.executable, str(ROOT / "tools/start.py"), "--config", str(self.config),
                                 "--web-port", str(webport)], stdout=self.log, stderr=self.log)
        try:
            for _ in range(200):
                if proc.poll() is not None:
                    self.fail("launcher exited before becoming ready")
                try:
                    with socket.create_connection(("127.0.0.1", webport), timeout=.1):
                        break
                except OSError:
                    time.sleep(.02)
            else:
                self.fail("launcher console did not start")
            with self.db() as db:
                self.assertEqual(db.command("PING"), "PONG")
        finally:
            proc.send_signal(signal.SIGINT)
            proc.wait(timeout=10)
        self.assertEqual(proc.returncode, 0)
        with self.assertRaises(OSError):
            socket.create_connection(("127.0.0.1", self.port), timeout=.1)

    def test_memory_limit_preserves_previous_values(self):
        self.stop()
        self.start("maxmemory 1048576\n")
        with self.db() as db:
            db.set("a", "x" * 400000)
            db.set("b", "y" * 400000)
            with self.assertRaises(DatabaseError):
                db.set("c", "z" * 400000)
            self.assertEqual(db.get("a"), "x" * 400000)
            db.set("a", "small")
            db.set("c", "z" * 400000)
        self.restart()
        with self.db() as db:
            self.assertEqual(db.get("a"), "small")

    def test_vector_exact_ranking_prefix_and_updates(self):
        rng = random.Random(12)
        vectors = {f"model:v1:{i:03}": [rng.uniform(-1, 1) for _ in range(12)] for i in range(80)}
        query = [rng.uniform(-1, 1) for _ in range(12)]
        def cosine(v):
            return sum(a*b for a, b in zip(v, query)) / math.sqrt(sum(a*a for a in v)*sum(b*b for b in query))
        expected = sorted(vectors, key=lambda k: (-cosine(vectors[k]), k))[:10]
        with self.db() as db:
            for key, vec in vectors.items():
                db.vset(key, key, vec)
            db.vset("another:model", "must not appear", query)
            rows = db.search(query, topk=10, threshold=-1, prefix="model:v1:")
            self.assertEqual([row["key"] for row in rows], expected)
            self.assertAlmostEqual(rows[0]["score"], cosine(vectors[expected[0]]), places=6)
            baseline = int(dict(line.split(":", 1) for line in db.command("INFO").splitlines() if ":" in line)["used_memory"])
            for _ in range(100):
                db.vset("another:model", "must not appear", query)
            after = int(dict(line.split(":", 1) for line in db.command("INFO").splitlines() if ":" in line)["used_memory"])
            self.assertEqual(baseline, after)
            self.assertEqual(db.command("VDEL", "another:model"), 1)

    def test_scan_and_concurrent_clients(self):
        def worker(i):
            with self.db() as db:
                db.set(f"key:{i}", str(i))
                return db.get(f"key:{i}")
        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
            self.assertEqual(list(pool.map(worker, range(120))), list(map(str, range(120))))
        with self.db() as db:
            cursor = "0"
            seen = []
            while True:
                cursor, keys = db.command("SCAN", cursor, "COUNT", 17)
                seen.extend(keys)
                if cursor == "0":
                    break
            self.assertEqual(set(seen), {f"key:{i}" for i in range(120)})

    def test_oversized_search_reply_is_a_recoverable_error(self):
        with self.db() as db:
            for i in range(10):
                db.vset(f"big:{i}", "x" * 450000, [1, 0])
            with self.assertRaisesRegex(DatabaseError, "reduce topK"):
                db.search([1, 0], topk=10)
            self.assertEqual(len(db.search([1, 0], topk=1)), 1)
            self.assertEqual(db.command("PING"), "PONG")

    def test_automatic_compaction_limits_update_history(self):
        with self.db() as db:
            for i in range(140):
                db.set("hot-key", str(i) + "x" * 500000)
            info = dict(line.split(":", 1) for line in db.command("INFO").splitlines() if ":" in line)
            self.assertLess(int(info["aof_bytes"]), 10 * 1024 * 1024)
        self.restart()
        with self.db() as db:
            self.assertTrue(db.get("hot-key").startswith("139x"))

    @unittest.skipUnless(Path("/proc/self/status").exists(), "Linux memory accounting")
    def test_idle_clients_use_small_buffers(self):
        clients = [self.db() for _ in range(64)]
        try:
            for db in clients:
                self.assertEqual(db.command("PING"), "PONG")
            status = Path(f"/proc/{self.proc.pid}/status").read_text()
            rss = int(next(line.split()[1] for line in status.splitlines() if line.startswith("VmRSS:")))
            # Sanitizers use more metadata. This still catches 8 MiB eagerly touched per client.
            self.assertLess(rss, 128 * 1024)
        finally:
            for db in clients:
                db.close()

    def test_disk_write_failure_does_not_publish_mutation(self):
        import resource
        self.stop()
        def limit_file():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (2048, 2048))
        self.start(preexec=limit_file)
        with self.db() as db:
            db.set("existing", "before")
            with self.assertRaises(DatabaseError):
                db.set("existing", "z" * 10000)
            self.assertEqual(db.get("existing"), "before")
            with self.assertRaises(DatabaseError):
                db.set("next", "fail")
            self.assertIsNone(db.get("next"))
        self.restart()
        with self.db() as db:
            self.assertEqual(db.get("existing"), "before")
            self.assertIsNone(db.get("next"))


if __name__ == "__main__":
    unittest.main()
