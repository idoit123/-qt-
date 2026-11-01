#!/usr/bin/env python3
import os
import select
import signal
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time

HEADER = struct.Struct("=II64sI")
LOGIN_REQUEST, LOGIN_RESPONSE = 3, 4
PRIVATE_CHAT_REQUEST, GROUP_CHAT_REQUEST, GROUP_CHAT_RESPONSE = 18, 20, 21


def packet(message_type, first=b"", second=b"", body=b""):
    fields = first[:31].ljust(32, b"\0") + second[:31].ljust(32, b"\0")
    return HEADER.pack(HEADER.size + len(body), message_type, fields, len(body)) + body


def receive_frame(sock):
    data = b""
    while len(data) < HEADER.size:
        chunk = sock.recv(HEADER.size - len(data))
        if not chunk:
            raise RuntimeError("connection closed while reading header")
        data += chunk
    length, message_type, fields, body_length = HEADER.unpack(data)
    if length < HEADER.size or body_length > length - HEADER.size:
        raise RuntimeError("invalid frame")
    body = b""
    while len(body) < length - HEADER.size:
        chunk = sock.recv(length - HEADER.size - len(body))
        if not chunk:
            raise RuntimeError("connection closed while reading body")
        body += chunk
    return message_type, fields, body[:body_length]


def drain_frames(clients, buffers, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        readable, _, _ = select.select(clients, [], [], 0.25)
        if not readable:
            yield None
            continue
        for sock in readable:
            index = clients.index(sock)
            chunk = sock.recv(65536)
            if not chunk:
                raise RuntimeError(f"client {index} disconnected")
            buffers[index] += chunk
            while len(buffers[index]) >= 4:
                length = struct.unpack_from("=I", buffers[index])[0]
                if len(buffers[index]) < length:
                    break
                frame = bytes(buffers[index][:length])
                del buffers[index][:length]
                _, message_type, _, body_length = HEADER.unpack_from(frame)
                body = frame[HEADER.size:HEADER.size + body_length]
                yield index, message_type, body


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: integration_test.py /path/to/epoll_chat_server")
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]

    with tempfile.TemporaryDirectory() as directory:
        database_path = os.path.join(directory, "chat.db")
        db = sqlite3.connect(database_path)
        db.executescript("""
            CREATE TABLE usrInfo (id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL,
                pwd TEXT NOT NULL, online INTEGER NOT NULL DEFAULT 0);
            CREATE TABLE friend (id INTEGER NOT NULL, friendId INTEGER NOT NULL);
        """)
        db.executemany("INSERT INTO usrInfo(name, pwd) VALUES(?, 'pw')",
                       [(f"user{i}",) for i in range(20)])
        db.executemany("INSERT INTO friend(id, friendId) VALUES("
                       "(SELECT id FROM usrInfo WHERE name='user0'), "
                       "(SELECT id FROM usrInfo WHERE name=?))",
                       [(f"user{i}",) for i in range(1, 4)])
        db.commit()
        db.close()

        server = subprocess.Popen([sys.argv[1], str(port), database_path, "4"])
        clients = []
        try:
            deadline = time.time() + 5
            while True:
                try:
                    test = socket.create_connection(("127.0.0.1", port), timeout=0.2)
                    test.close()
                    break
                except OSError:
                    if time.time() >= deadline:
                        raise RuntimeError("server did not start")
                    time.sleep(0.05)

            for i in range(20):
                client = socket.create_connection(("127.0.0.1", port), timeout=3)
                client.sendall(packet(LOGIN_REQUEST, f"user{i}".encode(), b"pw"))
                message_type, fields, _ = receive_frame(client)
                if message_type != LOGIN_RESPONSE or not fields.startswith(b"login ok"):
                    raise RuntimeError(f"login failed for user{i}")
                client.setblocking(False)
                clients.append(client)

            expected = set()
            for sender in range(20):
                recipient = (sender + 1) % 20
                batch = bytearray()
                for sequence in range(50):
                    body = f"{sender}:{sequence}".encode() + b"\0"
                    batch += packet(PRIVATE_CHAT_REQUEST, f"user{sender}".encode(),
                                    f"user{recipient}".encode(), body)
                    expected.add((recipient, body[:-1]))
                clients[sender].sendall(batch)

            buffers = [bytearray() for _ in clients]
            received = set()
            for item in drain_frames(clients, buffers, 15):
                if item is not None:
                    index, message_type, body = item
                    if message_type == PRIVATE_CHAT_REQUEST:
                        received.add((index, body.rstrip(b"\0")))
                if received == expected:
                    break
            if received != expected:
                raise RuntimeError(
                    f"private chat mismatch expected={len(expected)} got={len(received)}")

            clients[0].sendall(packet(GROUP_CHAT_REQUEST, b"user0", body=b"group\0"))
            recipients = set()
            for item in drain_frames(clients, buffers, 5):
                if item is not None:
                    index, message_type, _ = item
                    if message_type == GROUP_CHAT_RESPONSE:
                        recipients.add(index)
                if recipients == {1, 2, 3}:
                    break
            if recipients != {1, 2, 3}:
                raise RuntimeError(f"group recipients mismatch: {recipients}")
            print("20 clients, 1000 private messages and group routing passed")
        finally:
            for client in clients:
                client.close()
            server.send_signal(signal.SIGTERM)
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()


if __name__ == "__main__":
    main()
