#!/usr/bin/env python3
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import time
from redis_fixture import Probe, Redis, clean_env


def expect(actual, expected):
    if actual != expected:
        raise AssertionError(f"expected {expected!r}, got {actual!r}")


def main():
    probe_exe, server_exe, redis_exe, cli_exe = sys.argv[1:]
    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, "cache.db")
        with Redis(redis_exe, cli_exe, directory) as redis:
            with Probe(probe_exe, path, redis.env()) as probe:
                for name in ("a", "b", "empty", "c"):
                    expect(probe.command("register " + name), "1")
                expect(probe.command("read empty"), "0")
                expect(probe.command("read empty"), "0")
                expect(probe.stats(), (1, 1, 0, 1, 1))
                expect(probe.command("read a"), "0")
                expect(probe.command("read b"), "0")
                expect(probe.command("add a b"), "1")
                expect(probe.command("read a"), "1 b")
                expect(probe.command("read b"), "1 a")
                before = probe.stats()
                expect(probe.command("read a"), "1 b")
                expect(probe.stats()[4], before[4])
                expect(probe.stats()[0], before[0] + 1)
                expect(probe.command("delete a b"), "1")
                expect(probe.command("read a"), "0")
                expect(probe.command("read b"), "0")
                expect(probe.command("race"), "1")

                # Corrupt cache values are misses, not corrupt application data.
                for key in redis.cli("KEYS", "qtchat:friends:v1:*:a").splitlines():
                    redis.cli("SET", key, "bad-payload", "EX", "30")
                before = probe.stats()
                expect(probe.command("read a"), "0")
                expect(probe.stats()[4], before[4] + 1)
                expect(probe.command("add a b"), "1")
                expect(probe.command("read a"), "1 b")
                expect(probe.command("read b"), "1 a")

                # Timeout during invalidation after DB commit; Redis retains stale
                # keys while paused. Recovery must NOT reuse them.
                before = probe.stats()
                with redis.paused():
                    started = time.monotonic()
                    expect(probe.command("delete a b"), "1")
                    expect(probe.command("read a"), "0")
                    expect(probe.command("read b"), "0")
                    expect(probe.command("add a c"), "1")
                    expect(probe.command("read a"), "1 c")
                    assert time.monotonic() - started < 1.0, "fallback is too slow"
                    expect(probe.stats()[2], before[2] + 1)
                time.sleep(1.1)
                expect(probe.command("read a"), "1 c")
                expect(probe.command("read b"), "0")
                before = probe.stats()
                expect(probe.command("read a"), "1 c")
                expect(probe.stats()[0], before[0] + 1)

                # Cache read failures also fall back, then rebuild after cooldown.
                with redis.paused():
                    expect(probe.command("read a"), "1 c")
                time.sleep(1.1)
                expect(probe.command("read a"), "1 c")

            # TTL + startup namespace isolation: deliberately change authoritative
            # data between server instances; no old cache value may leak through.
            with sqlite3.connect(path) as db:
                db.execute("DELETE FROM friend")
            with Probe(probe_exe, path, redis.env(ttl=1)) as probe:
                expect(probe.command("read a"), "0")
                expect(probe.command("read a"), "0")
                before = probe.stats()
                time.sleep(1.1)
                expect(probe.command("read a"), "0")
                expect(probe.stats()[4], before[4] + 1)

            # Real client/server chat with Redis enabled.
            subprocess.run([sys.executable, str(Path(__file__).with_name("integration_test.py")),
                            server_exe], env=redis.env(), check=True, timeout=25)
            # Database read failures must never be cached as empty friend lists.
            with Probe(probe_exe, path, redis.env()) as probe:
                with sqlite3.connect(path) as db:
                    db.execute("DROP TABLE friend")
                expect(probe.command("read a"), "0")
                expect(probe.command("read a"), "0")
                expect(probe.stats()[3], 0)

        # Offline Redis at startup, disabled cache, and password authentication.
        with Probe(probe_exe, path, redis.env()) as probe:
            expect(probe.command("read a"), "0")
            expect(probe.command("read a"), "0")
            expect(probe.stats()[2], 1)
            expect(probe.stats()[4], 2)
        with Probe(probe_exe, path, clean_env()) as probe:
            expect(probe.command("read a"), "0")
            expect(probe.stats(), (0, 0, 0, 0, 1))
        with Redis(redis_exe, cli_exe, directory, password="test-only-password") as secured:
            with Probe(probe_exe, path, secured.env()) as probe:
                expect(probe.command("read a"), "0")
                expect(probe.command("read a"), "0")
                expect(probe.stats()[0], 1)
            bad = secured.env()
            bad["CHAT_REDIS_PASSWORD"] = "wrong-test-password"
            with Probe(probe_exe, path, bad) as probe:
                expect(probe.command("read a"), "0")
                expect(probe.stats()[2], 1)
        print("Redis cache: hit/miss, empty list, invalidation, race, corruption, TTL, "
              "timeout, recovery, namespace isolation, DB failure, AUTH and chat passed")


if __name__ == "__main__":
    main()
