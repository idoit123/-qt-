#!/usr/bin/env python3
"""Isolated Redis fixtures: never connect to or clear a user's Redis instance."""
import os
import select
import signal
import socket
import subprocess
import time
from contextlib import contextmanager


def clean_env():
    return {key: value for key, value in os.environ.items()
            if not key.startswith("CHAT_REDIS_") and key != "REDISCLI_AUTH"}


class Redis:
    def __init__(self, executable, cli, directory, password=""):
        self.cli_exe = cli
        self.password = password
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            self.port = probe.getsockname()[1]
        args = [executable, "--bind", "127.0.0.1", "--port", str(self.port),
                "--save", "", "--appendonly", "no", "--dir", directory]
        if password:
            args += ["--requirepass", password]
        self.process = subprocess.Popen(args, stdout=subprocess.DEVNULL,
                                        stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 5
            while self.cli("PING") != "PONG":
                if time.monotonic() > deadline or self.process.poll() is not None:
                    raise RuntimeError("test Redis failed to start")
                time.sleep(0.02)
        except BaseException:
            self.close()
            raise

    def cli(self, *args):
        env = clean_env()
        if self.password:
            env["REDISCLI_AUTH"] = self.password
        result = subprocess.run([self.cli_exe, "-h", "127.0.0.1", "-p", str(self.port),
                                 "--raw", *args], env=env, capture_output=True,
                                text=True, timeout=3)
        return result.stdout.strip()

    def env(self, ttl=30):
        result = clean_env()
        result.update(CHAT_REDIS_HOST="127.0.0.1", CHAT_REDIS_PORT=str(self.port),
                      CHAT_REDIS_TTL_SECONDS=str(ttl), CHAT_REDIS_TIMEOUT_MS="20")
        if self.password:
            result["CHAT_REDIS_PASSWORD"] = self.password
        return result

    @contextmanager
    def paused(self):
        self.process.send_signal(signal.SIGSTOP)
        try:
            yield
        finally:
            self.process.send_signal(signal.SIGCONT)

    def close(self):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGCONT)
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


class Probe:
    def __init__(self, executable, path, env):
        self.process = subprocess.Popen([executable, path], env=env, text=True,
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        bufsize=1)
        self.command("stats")  # Wait until schema initialization has completed.

    def command(self, command):
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()
        if not select.select([self.process.stdout], [], [], 15)[0]:
            raise RuntimeError("probe command timed out: " + command)
        line = self.process.stdout.readline()
        if not line:
            raise RuntimeError("probe exited while processing " + command)
        return line.strip()

    def stats(self):
        return tuple(map(int, self.command("stats").split()))

    def close(self):
        if self.process.stdin:
            self.process.stdin.close()
        try:
            self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        self.process.stdout.close()
        if self.process.returncode:
            raise RuntimeError("probe failed: " + str(self.process.returncode))

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

