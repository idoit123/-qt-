#!/usr/bin/env python3
"""Real TCP + OpenCV test; no webcam, real accounts, or existing DB required."""
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

import cv2
import numpy as np

HEADER = struct.Struct("<II64sI")


def packet(kind, body=b"", first=b"", second=b""):
    fields = first[:31].ljust(32, b"\0") + second[:31].ljust(32, b"\0")
    return HEADER.pack(HEADER.size + len(body), kind, fields, len(body)) + body


def receive(sock):
    def exact(size):
        result = bytearray()
        while len(result) < size:
            part = sock.recv(size - len(result))
            if not part:
                raise EOFError("connection closed")
            result.extend(part)
        return bytes(result)
    total, kind, fields, length = HEADER.unpack(exact(HEADER.size))
    assert HEADER.size <= total <= 1024 * 1024
    assert length == total - HEADER.size
    return kind, fields, exact(length)


def login(port, name):
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    sock.sendall(packet(1, first=name, second=b"pw"))
    assert receive(sock)[0] == 2
    sock.sendall(packet(3, first=name, second=b"pw"))
    kind, fields, _ = receive(sock)
    assert kind == 4 and fields.startswith(b"login ok")
    return sock


def video(sock, image, sequence="check", reset=False):
    ok, encoded = cv2.imencode(".jpg", image)
    assert ok
    sock.sendall(packet(200, encoded.tobytes(), first=sequence.encode(),
                        second=b"reset" if reset else b""))
    kind, fields, body = receive(sock)
    assert kind == 201, body
    assert fields[:32].split(b"\0")[0] == sequence.encode()
    result = cv2.imdecode(np.frombuffer(body, dtype=np.uint8), cv2.IMREAD_COLOR)
    assert result is not None and result.shape == image.shape
    return fields[32:].split(b"\0")[0].decode(), result


def main():
    executable = os.path.abspath(sys.argv[1])
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    with tempfile.TemporaryDirectory() as directory:
        env = dict(os.environ)
        env.pop("CHAT_REDIS_HOST", None)
        env["CHAT_FILE_ROOT"] = os.path.join(directory, "files")
        with tempfile.TemporaryFile() as log:
            server = subprocess.Popen([executable, str(port), os.path.join(directory, "video.db"), "2"],
                                      cwd=directory, env=env, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 5
                while True:
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                            break
                    except OSError:
                        if time.monotonic() > deadline:
                            raise RuntimeError("server not listening")
                        time.sleep(0.03)
                image = np.zeros((120, 160, 3), dtype=np.uint8)
                image[:, :50] = (0, 0, 255)
                image[:, 50:100] = (0, 255, 0)
                image[:, 100:] = (255, 0, 0)
                ok, encoded = cv2.imencode(".jpg", image)
                assert ok
                jpeg = encoded.tobytes()
                expected = cv2.imdecode(encoded, cv2.IMREAD_COLOR)
                other = np.full_like(image, (90, 40, 20))
                other_jpeg = cv2.imencode(".jpg", other)[1].tobytes()
                with socket.create_connection(("127.0.0.1", port), timeout=5) as anonymous:
                    anonymous.sendall(packet(200, jpeg))
                    assert anonymous.recv(1) == b"", "anonymous video access accepted"
                with login(port, b"video-a") as client, login(port, b"video-b") as second:
                    # Invalid data must return a recoverable error, not kill the worker.
                    for invalid in (b"not-jpeg", b"", b"X" * (512 * 1024 + 1)):
                        client.sendall(packet(200, invalid, first=b"invalid"))
                        assert receive(client)[0] == 202
                    huge = cv2.imencode(".jpg", np.zeros((721, 1280, 3), dtype=np.uint8))[1].tobytes()
                    client.sendall(packet(200, huge))
                    assert receive(client)[0] == 202, "oversized dimensions accepted"
                    # Two independent clients, partial TCP frames and exact frame IDs.
                    for sequence in range(30):
                        for sock, payload in ((client, jpeg), (second, other_jpeg)):
                            request = packet(200, payload, first=str(sequence).encode())
                            sock.sendall(request[:3])
                            sock.sendall(request[3:65])
                            sock.sendall(request[65:])
                        for sock, reference in ((client, expected), (second, other)):
                            kind, fields, body = receive(sock)
                            assert kind == 201 and fields.split(b"\0")[0] == str(sequence).encode()
                            result = cv2.imdecode(np.frombuffer(body, dtype=np.uint8), cv2.IMREAD_COLOR)
                            assert result is not None and result.shape == image.shape
                            status = fields[32:].split(b"\0")[0].decode()
                            wanted = f"warmup:{sequence + 1}/20" if sequence < 20 else "idle:0"
                            assert status == wanted, (status, wanted)
                            error = np.abs(result.astype(float) - reference.astype(float)).mean()
                            assert error < 4, error
                    # A new scene with a known moving object: assert visible green
                    # bounding boxes, not just a status string generated by the server.
                    background = np.full_like(image, (40, 30, 20))
                    for frame in range(20):
                        status, _ = video(client, background, reset=(frame == 0))
                        assert status == f"warmup:{frame + 1}/20"
                    assert video(client, background)[0] == "idle:0"
                    noisy = background.copy()
                    noisy[60, 80] = (255, 255, 255)
                    assert video(client, noisy)[0] == "idle:0", "single-pixel noise detected"
                    flash = background.copy()
                    cv2.rectangle(flash, (10, 10), (30, 30), (220, 220, 220), -1)
                    assert video(client, flash)[0] == "idle:0", "one-frame flash was reported"
                    assert video(client, background)[0] == "idle:0", "one-frame flash left a box"
                    moving = background.copy()
                    cv2.rectangle(moving, (50, 40), (100, 90), (220, 220, 220), -1)
                    assert video(client, moving)[0] == "idle:0", "motion was reported before confirmation"
                    status, marked = video(client, moving, "moving")
                    assert status.startswith("motion:") and int(status.split(":")[1]) > 0, status
                    b, g, r = cv2.split(marked.astype(np.int16))
                    green = (g > 120) & (g > r + 60) & (g > b + 60)
                    assert np.count_nonzero(green[34:97, 44:107]) > 80, "no visible motion box"
                    for _ in range(2):
                        assert video(client, background)[0].startswith("motion:"), "short miss caused flicker"
                    assert video(client, background)[0] == "idle:0", "expired motion did not clear"
                    assert video(second, other)[0] == "idle:0", "background leaked across connections"
                    # Explicit reset learns the current object as background.
                    for frame in range(20):
                        status, _ = video(client, moving, reset=(frame == 0))
                        assert status == f"warmup:{frame + 1}/20"
                    assert video(client, moving)[0] == "idle:0"
                    resized = cv2.resize(moving, (320, 240))
                    assert video(client, resized)[0] == "warmup:1/20", "resolution change did not reset"
                    scene = np.full((240, 320, 3), (40, 30, 20), dtype=np.uint8)
                    for frame in range(20):
                        assert video(client, scene, reset=(frame == 0))[0] == f"warmup:{frame + 1}/20"
                    fragments = scene.copy()
                    for a, b in (((100, 60), (124, 84)), ((134, 62), (158, 86)), ((112, 96), (142, 120))):
                        cv2.rectangle(fragments, a, b, (220, 220, 220), -1)
                    assert video(client, fragments)[0] == "idle:0"
                    status, grouped = video(client, fragments)
                    assert status == "motion:1", f"head fragments not merged: {status}"
                    b, g, r = cv2.split(grouped.astype(np.int16))
                    green = (g > 120) & (g > r + 60) & (g > b + 60)
                    assert np.count_nonzero(green[54:127, 94:165]) > 80, "no visible grouped box"
                    for frame in range(20):
                        video(client, scene, reset=(frame == 0))
                    distant = scene.copy()
                    cv2.rectangle(distant, (25, 80), (65, 130), (220, 220, 220), -1)
                    cv2.rectangle(distant, (245, 80), (285, 130), (220, 220, 220), -1)
                    assert video(client, distant)[0] == "idle:0"
                    assert video(client, distant)[0] == "motion:2", "distant targets were merged"
                    assert video(client, scene, reset=True)[0] == "warmup:1/20", "reset retained tracking boxes"
                    # Disconnect with work outstanding; a subsequent connection must work.
                    second.sendall(packet(200, jpeg))
                with login(port, b"after-disconnect") as client:
                    assert video(client, image, "final")[0] == "warmup:1/20"
                assert server.poll() is None
                print("MOTION_INTEGRATION_PASS: color video, warmup, static/noise/flash, grouped motion boxes, "
                      "separate targets, confirmation/expiry, reset, isolation, resize, auth, bounds, disconnect")
            finally:
                server.terminate()
                try:
                    server.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()
                    raise RuntimeError("server shutdown hung")
                log.seek(0)
                print(log.read().decode(errors="replace"))
            assert server.returncode == 0, server.returncode


if __name__ == "__main__":
    main()
