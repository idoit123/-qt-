#!/usr/bin/env python3
"""Native/ARM wire tests. Remote mode requires a dedicated disposable test server."""
import argparse
import hashlib
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import uuid
from integration_test import packet, receive_frame

CHUNK = 65536

def payload(*paths):
    return b''.join(p.encode('utf-8') + b'\0' for p in paths)

class Client:
    def __init__(self, host, port, name, register=True):
        self.socket = socket.create_connection((host, port), 5)
        self.socket.settimeout(10)
        self.name = name
        if register:
            self.socket.sendall(packet(1, name.encode(), b'pw'))
            kind, fields, _ = receive_frame(self.socket)
            assert kind == 2 and fields.startswith(b'regtist ok'), fields
        self.socket.sendall(packet(3, name.encode(), b'pw'))
        kind, fields, _ = receive_frame(self.socket)
        assert kind == 4 and fields.startswith(b'login ok'), fields

    def reply(self, operation, status='ok'):
        kind, fields, body = receive_frame(self.socket)
        assert kind == 101 and fields[:32].split(b'\0')[0] == operation.encode(), (kind, fields)
        actual = fields[32:].split(b'\0')[0].decode()
        assert actual == status, (operation, actual, body)
        return body

    def request(self, operation, body=b'', argument='', status='ok'):
        time.sleep(0.012)  # Respect the server's 100 requests/s token bucket.
        self.socket.sendall(packet(100, operation.encode(), str(argument).encode(), body))
        return self.reply(operation, status)

    def listing(self, path=''):
        body = self.request('list', payload(path))
        return {item[1:].decode('utf-8'): chr(item[0]) for item in body.split(b'\0') if item}

    def upload(self, path, data):
        self.request('upload', payload(path), len(data))
        for start in range(0, len(data), CHUNK):
            self.request('chunk', data[start:start + CHUNK])
        self.request('commit')

    def download(self, path):
        size = int(self.request('download', payload(path)))
        data = bytearray()
        while len(data) < size:
            data.extend(self.request('read', status='more'))
        self.request('read', status='done')
        assert len(data) == size
        return bytes(data)

def exercise(host, port, storage=None):
    tag = uuid.uuid4().hex[:12]
    a = Client(host, port, 'file_a_' + tag)
    b = Client(host, port, 'file_b_' + tag)
    try:
        with socket.create_connection((host, port), 5) as anonymous:
            anonymous.sendall(packet(100, b'list', body=payload('')))
            assert anonymous.recv(1) == b'', 'unauthenticated file access was accepted'
        assert a.listing() == {}
        # Byte-by-byte fragmentation and multiple commands in one TCP write.
        frame = packet(100, b'list', body=payload(''))
        for byte in frame:
            a.socket.sendall(bytes([byte]))
        a.reply('list')
        a.socket.sendall(packet(100, b'mkdir', body=payload('one')) + packet(100, b'list', body=payload('')))
        a.reply('mkdir'); assert b'Done\0' in a.reply('list')
        a.request('mkdir', payload('one'), status='error')
        folder = 'one/中文 目录'
        a.request('mkdir', payload(folder))
        filename = folder + '/空 格 文件.bin'
        data = bytes(range(256)) * (5 * 1024 * 1024 // 256) + b'\x00\xfftail'
        a.upload(filename, data)
        assert hashlib.sha256(a.download(filename)).digest() == hashlib.sha256(data).digest()
        a.request('upload', payload(filename), '0', status='error')
        assert b.listing() == {}
        b.request('download', payload(filename), status='error')
        a.upload(folder + '/empty', b''); assert a.download(folder + '/empty') == b''
        a.request('rmdir', payload(folder), status='error')
        a.request('rename', payload(folder + '/empty', filename), status='error')
        a.request('rename', payload(filename, folder + '/renamed.bin'))
        a.request('download', payload(filename), status='error')
        assert a.listing(folder)['renamed.bin'] == 'F'
        # Transfers stay framed: chat can be sent between file chunks.
        a.request('upload', payload('interleaved'), '3')
        a.socket.sendall(packet(18, a.name.encode(), b.name.encode(), b'hello\0'))
        kind, _, body = receive_frame(b.socket); assert kind == 18 and body == b'hello\0'
        a.request('chunk', b'abc'); a.request('commit')
        assert a.download('interleaved') == b'abc'
        # Cancellation, incomplete commits, excess bytes and bad metadata.
        a.request('upload', payload('cancelled'), '100'); a.request('chunk', b'123'); a.request('cancel')
        assert 'cancelled' not in a.listing()
        a.request('upload', payload('short'), '10'); a.request('chunk', b'123'); a.request('commit', status='error')
        a.request('upload', payload('long'), '1'); a.request('chunk', b'12', status='error')
        a.request('upload', payload('oversize'), str(256 * 1024 * 1024 + 1), status='error')
        a.request('upload', payload('negative'), '-1', status='error')
        for path in ('../escape', '/etc/passwd', 'one/../../escape', 'one\\escape', '.upload-reserved', 'one//bad'):
            a.request('mkdir', payload(path), status='error')
        a.request('list', b'not terminated', status='error')
        a.request('remove', payload(''), status='error')
        a.request('download', payload('one'), status='error')
        a.request('download', payload('missing'), status='error')
        # Reader pulls only one bounded block; pausing does not disconnect it.
        a.request('download', payload(folder + '/renamed.bin'))
        first = a.request('read', status='more'); assert len(first) == CHUNK
        a.socket.settimeout(0.1)
        try:
            unexpected = a.socket.recv(1)
            raise AssertionError(('unsolicited data/connection closure', unexpected))
        except socket.timeout:
            pass
        finally:
            a.socket.settimeout(10)
        a.request('cancel')
        # Local-only adversarial filesystem setup: symlinks must not be followed.
        if storage:
            root = Path(storage) / ('u-' + a.name.encode().hex())
            outside = Path(storage).parent / 'outside'; outside.mkdir()
            (outside / 'secret').write_bytes(b'keep')
            (root / 'link').symlink_to(outside, target_is_directory=True)
            (root / 'filelink').symlink_to(outside / 'secret')
            a.request('list', payload('link'), status='error')
            a.request('download', payload('filelink'), status='error')
            a.request('upload', payload('link/escape'), '0', status='error')
            a.request('remove', payload('filelink'), status='error')
            assert (outside / 'secret').read_bytes() == b'keep'
            assert not list(root.rglob('.upload-*')), 'aborted upload leaked temporary files'
        # Disconnect mid-upload: no partial destination and a new session still works.
        a.request('upload', payload('interrupted'), '1000'); a.request('chunk', b'partial')
        a.socket.close(); time.sleep(0.2)
        a = Client(host, port, a.name, register=False)
        assert 'interrupted' not in a.listing()
        if storage:
            assert not list(root.rglob('.upload-*'))
        a.request('remove', payload(folder + '/renamed.bin'))
        a.request('remove', payload(folder + '/empty'))
        a.request('rmdir', payload(folder)); a.request('rmdir', payload('one'))
        a.request('remove', payload('interleaved'))
        assert a.listing() == {}
        print('PASS: files 5 MiB SHA-256 round trip, empty/Unicode names, directories, isolation, traversal, cancel, disconnect and chat interleaving')
    finally:
        a.socket.close(); b.socket.close()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('server', nargs='?')
    parser.add_argument('--remote', nargs=2, metavar=('HOST', 'PORT'))
    args = parser.parse_args()
    if args.remote:
        exercise(args.remote[0], int(args.remote[1])); return
    if not args.server: parser.error('provide server executable or --remote HOST PORT')
    with tempfile.TemporaryDirectory() as tmp:
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0)); port = probe.getsockname()[1]
        env = dict(os.environ); env.pop('CHAT_REDIS_HOST', None)
        env['CHAT_FILE_ROOT'] = str(Path(tmp) / 'files')
        server = subprocess.Popen([args.server, str(port), str(Path(tmp) / 'db.sqlite'), '2'], env=env)
        try:
            for _ in range(100):
                try:
                    with socket.create_connection(('127.0.0.1', port), 0.1): break
                except OSError: time.sleep(0.05)
            exercise('127.0.0.1', port, env['CHAT_FILE_ROOT'])
        finally:
            server.terminate()
            try: server.wait(5)
            except subprocess.TimeoutExpired: server.kill(); server.wait()

if __name__ == '__main__': main()
