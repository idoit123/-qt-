#!/usr/bin/env bash
set -euo pipefail
repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
port="${1:-18888}"
if ! [[ "$port" =~ ^[0-9]{1,5}$ ]] || ((10#$port < 1 || 10#$port > 65535)); then
  echo "Port must be an integer in 1..65535" >&2
  exit 1
fi
build="$repo/build-video-server"
runtime="$repo/build-video-runtime/$port"
cmake -S "$repo/EpollServer" -B "$build" \
  -DCMAKE_BUILD_TYPE=Release -DCHAT_WITH_VIDEO=ON -DCHAT_WITH_REDIS=OFF
cmake --build "$build" -j2
mkdir -p "$runtime"
cd "$runtime"
echo "Video demo: port=$port; database=$runtime/cloud.db"
echo "Open tools/start-video-client.cmd, register an account, then send a test image."
unset CHAT_REDIS_HOST
export CHAT_FILE_ROOT="$runtime/files"
exec "$build/epoll_chat_server" "$port" "$runtime/cloud.db" 2
