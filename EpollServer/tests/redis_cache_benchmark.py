#!/usr/bin/env python3
"""Microbenchmark of the real Database::friends path, not chat/server QPS."""
import argparse
import json
import os
import platform
import sqlite3
import statistics
import subprocess
import tempfile
from redis_fixture import Probe, Redis, clean_env


def seed(path, users, links):
    with sqlite3.connect(path) as db:
        db.executescript("""
            CREATE TABLE usrInfo(id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL,
                pwd TEXT NOT NULL, online INTEGER NOT NULL DEFAULT 0);
            CREATE TABLE friend(id INTEGER NOT NULL, friendId INTEGER NOT NULL);
        """)
        db.executemany("INSERT INTO usrInfo(id,name,pwd) VALUES(?,?,'pw')",
                       [(i + 1, f"u{i}") for i in range(users)])
        db.executemany("INSERT INTO friend VALUES(?,?)",
                       ((i + 1, (i + j) % users + 1)
                        for i in range(users) for j in range(1, links + 1)))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("probe")
    parser.add_argument("--output", required=True)
    parser.add_argument("--iterations", type=int, default=500)
    args = parser.parse_args()
    if args.iterations < 1:
        parser.error("iterations must be positive")
    output = {"scope": "single-thread Database::friends, warm cache, loopback Redis; not chat QPS",
              "environment": {"kernel": platform.platform(), "sqlite": sqlite3.sqlite_version,
                              "redis": subprocess.check_output(["redis-server", "--version"], text=True).strip(),
                              "compiler": subprocess.check_output(["g++", "--version"], text=True).splitlines()[0]},
              "iterations_per_round": args.iterations, "rounds": 3, "cases": []}
    with tempfile.TemporaryDirectory() as directory:
        with Redis("redis-server", "redis-cli", directory) as redis:
            for users, links in [(20, 3), (1000, 50)]:
                path = os.path.join(directory, f"{users}.db")
                seed(path, users, links)
                case = {"users": users, "friend_rows": users * links,
                        "friends_per_query": links * 2, "friend_indexes": True,
                        "sqlite": [], "redis": []}
                for round_id in range(3):
                    # Alternate order to reduce systematic warm-up/order bias.
                    modes = ["sqlite", "redis"] if round_id % 2 == 0 else ["redis", "sqlite"]
                    for mode in modes:
                        env = redis.env(ttl=60) if mode == "redis" else clean_env()
                        with Probe(args.probe, path, env) as probe:
                            assert int(probe.command("read u0").split()[0]) == links * 2
                            values = probe.command(f"loop u0 {args.iterations}").split()
                            ms, total, queries, hits, p99 = map(float, values)
                            assert int(total) == args.iterations * links * 2
                            assert int(queries) == (0 if mode == "redis" else args.iterations)
                            assert int(hits) == (args.iterations if mode == "redis" else 0)
                            case[mode].append({"elapsed_ms": ms, "p99_us": p99,
                                               "sqlite_queries": int(queries), "cache_hits": int(hits)})
                baseline = statistics.median(item["elapsed_ms"] for item in case["sqlite"])
                cached = statistics.median(item["elapsed_ms"] for item in case["redis"])
                case["median_sqlite_ms"] = baseline
                case["median_redis_ms"] = cached
                case["elapsed_reduction_percent"] = round((1 - cached / baseline) * 100, 2)
                output["cases"].append(case)
                print(json.dumps(case), flush=True)
    with open(args.output, "w", encoding="utf-8") as file:
        json.dump(output, file, ensure_ascii=False, indent=2)
        file.write("\n")


if __name__ == "__main__":
    main()
