#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
build="$repo/build-fsmp1a"
mkdir -p "$build/deps"
python3 - "$build/deps" <<'PY'
import hashlib
import io
from pathlib import Path
import sys
import urllib.request
import zipfile

dest = Path(sys.argv[1])
archive = dest / 'sqlite-amalgamation-3530400.zip'
expected = '628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e'
if not archive.exists():
    with urllib.request.urlopen('https://www.sqlite.org/2026/' + archive.name, timeout=60) as response:
        data = response.read()
    if hashlib.sha3_256(data).hexdigest() != expected:
        raise SystemExit('SQLite download checksum mismatch')
    archive.write_bytes(data)
data = archive.read_bytes()
if hashlib.sha3_256(data).hexdigest() != expected:
    raise SystemExit('SQLite archive checksum mismatch')
with zipfile.ZipFile(io.BytesIO(data)) as source:
    for name in ('sqlite3.c', 'sqlite3.h'):
        (dest / name).write_bytes(source.read('sqlite-amalgamation-3530400/' + name))
PY
arm-linux-gnueabihf-gcc -O2 -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard \
  -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION \
  -c "$build/deps/sqlite3.c" -o "$build/deps/sqlite3.o"
arm-linux-gnueabihf-ar rcs "$build/deps/libsqlite3.a" "$build/deps/sqlite3.o"
cmake -S "$repo/EpollServer" -B "$build" \
  -DCMAKE_TOOLCHAIN_FILE="$repo/EpollServer/cmake/arm-linux-static.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DCHAT_WITH_REDIS=OFF \
  -DSQLite3_INCLUDE_DIR="$build/deps" -DSQLite3_LIBRARY="$build/deps/libsqlite3.a"
cmake --build "$build" -j2
ctest --test-dir "$build" -R '^epoll_core_test$' --output-on-failure
file "$build/epoll_chat_server"
sha256sum "$build/epoll_chat_server"
