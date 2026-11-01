"""Exercise a dedicated board test server seeded with user0..user19 / pw.

The test database must contain friendships from user0 to user1, user2, user3.
Never run this against a production/user database.
"""
import socket
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'EpollServer' / 'tests'))
from integration_test import packet, receive_frame, drain_frames

host, port = sys.argv[1], int(sys.argv[2])
clients = []
try:
    with socket.create_connection((host, port), timeout=5) as client:
        client.sendall(packet(1, b'board_registration_probe', b'pw'))
        kind, fields, _ = receive_frame(client)
        if kind != 2 or not fields.startswith(b'regtist ok'):
            raise RuntimeError(f'Registration failed: {kind}, {fields!r}')
    for i in range(20):
        client = socket.create_connection((host, port), timeout=5)
        clients.append(client)
        client.sendall(packet(3, f'user{i}'.encode(), b'pw'))
        kind, fields, _ = receive_frame(client)
        if kind != 4 or not fields.startswith(b'login ok'):
            raise RuntimeError(f'Login failed for user{i}')

    expected = set()
    for sender, client in enumerate(clients):
        recipient = (sender + 1) % len(clients)
        batch = bytearray()
        for sequence in range(50):
            body = f'{sender}:{sequence}'.encode()
            batch += packet(18, f'user{sender}'.encode(), f'user{recipient}'.encode(), body + b'\0')
            expected.add((recipient, body))
        client.sendall(batch)
    for client in clients:
        client.setblocking(False)
    buffers = [bytearray() for _ in clients]
    received = set()
    for item in drain_frames(clients, buffers, 20):
        if item is not None:
            index, kind, body = item
            if kind == 18:
                received.add((index, body.rstrip(b'\0')))
        if received == expected:
            break
    if received != expected:
        raise RuntimeError(f'Private messages: expected {len(expected)}, received {len(received)}')

    clients[0].sendall(packet(20, b'user0', body=b'group\0'))
    recipients = set()
    for item in drain_frames(clients, buffers, 5):
        if item is not None:
            index, kind, body = item
            if kind == 21:
                if body.rstrip(b'\0') != b'group':
                    raise RuntimeError('Corrupted group message')
                recipients.add(index)
        if recipients == {1, 2, 3}:
            break
    if recipients != {1, 2, 3}:
        raise RuntimeError(f'Group recipients mismatch: {recipients}')
    print('PASS: board registration, 20 logins, 1000 private messages, 3 group recipients')
finally:
    for client in clients:
        client.close()
