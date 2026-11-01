# Linux epoll 多 Reactor 服务端

这是现有 Qt 客户端的第二套服务端实现。Qt 服务端保留为稳定基线；本目录面向 Linux，使用非阻塞 Socket、epoll、eventfd 和固定数量 I/O 线程，消除“一连接一线程”的线程数量增长。

## 架构

- 主 Reactor 使用 epoll 监听服务端 Socket 和 signalfd，通过 accept4 接收非阻塞连接。
- 新连接轮询分配到固定数量的子 Reactor，每个子 Reactor 在一个线程中管理多个连接。
- eventfd 负责新连接、跨线程消息路由和停服唤醒，不进行轮询等待。
- 连接事件携带 fd 和 generation，避免文件描述符复用后处理过期事件。
- ET 模式读取持续到 EAGAIN；增量解码器处理半包、粘包和多包。
- 发送队列处理短写，只在存在积压时注册 EPOLLOUT。
- 每连接包含令牌桶、64 KiB 聊天消息上限和 1 MiB 发送高水位。
- 用户路由表保存弱引用，跨 Reactor 投递回目标连接所属线程执行。
- SQLite 使用预编译语句和串行化访问，表结构兼容原 Qt 服务端。

当前协议暂时兼容原 Qt 客户端的本机结构体布局，已验证小端 x86_64 和 ARMv7 的 76 字节协议头。后续应迁移到带魔数、版本号和网络字节序的新协议。

## FS-MP1A 开发板部署

已在 FS-MP1A V3.1、ARMv7、Linux 5.4.31 / Buildroot 2021.02.1 上验证。
WSL Ubuntu 的构建命令：

```bash
sudo apt-get install g++-arm-linux-gnueabihf qemu-user cmake python3
bash tools/build-fsmp1a.sh
```

脚本从 [SQLite 官网](https://www.sqlite.org/download.html) 下载固定版本 3.53.4，并校验 SHA3-256。
输出 `build-fsmp1a/epoll_chat_server` 为 ARM 硬浮点静态程序，包含 SQLite 和 C++ 运行库，避免依赖板子的旧 glibc；此构建关闭 Redis。
板子当前部署路径为 `/opt/codex-chat`，独立数据库为该目录下的 `cloud.db`，监听端口 8888，使用两个 I/O 线程。

`deploy/codex-chat.service` 是对应 systemd 服务，启动和查看状态：

```bash
systemctl start codex-chat
systemctl status codex-chat
```

Windows 客户端支持 `CHAT_SERVER_IP` 环境变量覆盖内置地址，并对该连接使用直连，避免系统 HTTP 代理干扰 TCP 连接；不设置时仍使用原配置。
本机已编译的客户端可通过 `tools/start-board-client.cmd` 启动，当前默认地址是 `192.168.31.73`，也可向脚本传入新 IP。

验证结果：ARM 核心测试通过；在独立测试数据库/18888 端口上，通过电脑到板子 Wi-Fi 的注册、20 客户端登录、1000 条私聊和 3 个群聊接收方测试。Windows Qt 客户端重新编译成功，连接成功信号及日志确认目标为正式 8888 端口；尚未人工操作 GUI 完成注册/聊天。测试脚本为 `tools/test-board-network.py`，其数据库种子要求见文件说明。板子的 Python 缺少 sqlite3 模块，因此完整网络测试由电脑发起。

当前运行尚未配置重启自动恢复：U-Boot 缺少 `bootcmd`，此次使用 `run bootcmd_mmc1` 手动启动；Wi-Fi 配置在板子的 `/run/codex-board-wifi.conf`，重启会丢失；聊天服务已启动但尚未 enable。DHCP 地址也可能变化。
此外，启动日志显示引导器来自 eMMC，而 Linux 根文件系统实际挂载自 SD 卡 `mmcblk1p4`，不要直接拔掉 SD 卡。

## Linux 构建

依赖：CMake 3.16+、支持 C++17 的 GCC/Clang、pthread、SQLite3 开发包和 Python 3；默认 Redis 构建还需要 hiredis 开发包与 pkg-config，完整缓存测试需要 redis-server、redis-cli。

```bash
cmake -S EpollServer -B build-epoll -DCMAKE_BUILD_TYPE=Release
cmake --build build-epoll -j
ctest --test-dir build-epoll --output-on-failure
```

启动：`./build-epoll/epoll_chat_server 8888 ./cloud.db 4`。参数依次为端口、数据库路径和 I/O Reactor 数量；SIGINT、SIGTERM 会触发停止接入并等待 Reactor 退出。

## 验证说明

- core_test.cpp 覆盖逐字节拆包、合包、非法长度、令牌桶和发送预算。
- integration_test.py 在 Linux 启动真实服务端，验证 20 个客户端登录、1000 条私聊及 3 人群聊路由。
- 已在 Ubuntu 22.04（WSL2、Linux 6.18.33.2、GCC 11.4、Release 构建）完成编译，并通过上述 2 项测试。该结果验证了协议拆包、流控和基础并发路由的正确性；吞吐量、P99 延迟和长稳能力仍需专项压测后再给出量化结论。

## Redis 好友缓存（可选）

Linux 服务端通过 hiredis 为 `Database::friends()` 增加 Cache-Aside 缓存，覆盖好友列表刷新与好友群发时的接收者查询。SQLite 仍为权威数据源，登录凭据和在线连接路由不存入 Redis。Qt 服务端无需 Redis；同一数据库应只由一套服务端写入。

### 构建与启用

默认编译 Redis 支持，但未设置 `CHAT_REDIS_HOST` 时不会连接 Redis。Ubuntu 开发依赖：

```bash
sudo apt-get install libsqlite3-dev libhiredis-dev pkg-config redis-server redis-tools
cmake -S EpollServer -B build-epoll -DCMAKE_BUILD_TYPE=Release
cmake --build build-epoll -j2
ctest --test-dir build-epoll --output-on-failure
```

先在一个终端启动本地 Redis（如果已有本地实例，直接使用它）：

```bash
redis-server --bind 127.0.0.1 --port 6379
```

另一个终端启动聊天服务端：

```bash
CHAT_REDIS_HOST=127.0.0.1 CHAT_REDIS_PORT=6379 \
  ./build-epoll/epoll_chat_server 8888 ./cloud.db 4
```

| 环境变量 | 默认值 | 含义 |
| --- | --- | --- |
| CHAT_REDIS_HOST | 未设置，禁用缓存 | 数字 IPv4/IPv6 地址，避免同步 DNS 等待 |
| CHAT_REDIS_PORT | 6379 | Redis TCP 端口 |
| CHAT_REDIS_PASSWORD | 未设置 | 可选 AUTH 密码（默认 Redis 用户），不写日志 |
| CHAT_REDIS_TIMEOUT_MS | 20 | 连接与 socket I/O 超时，范围 1～1000 ms |
| CHAT_REDIS_TTL_SECONDS | 30 | 缓存 TTL，范围 1～3600 秒 |

无需 Redis 开发库时，可用 `-DCHAT_WITH_REDIS=OFF` 编译。此构建若设置了 `CHAT_REDIS_HOST` 会明确报错，避免误以为缓存已经启用。

### 一致性与降级

- 读取先查 Redis，未命中回查 SQLite，只在查询成功完成后写入缓存；空列表也可缓存。
- 值使用带版本标识的长度编码，单值最大 1 MiB，使用 `SET ... EX` 同时写入与设置 TTL。过大的列表不缓存。
- 添加、删除好友成功后通过一次 `DEL` 失效双方缓存。读/回填与写/失效在同一个数据库互斥锁内执行，避免本进程并发回填旧值。
- Redis 连接、命令或认证失败时回退 SQLite，并进入 1 秒重连冷却期。失败时切换缓存命名空间，防止删除失败后恢复连接又使用旧值；旧命名空间的键依靠 TTL 回收，不清空其他应用数据。
- 每个 Database 实例拥有独立随机命名空间，重启或切换数据库不复用旧缓存。不支持多个服务实例/外部 SQL 写入者之间的即时缓存失效；外部写入最多需等待 TTL。
- 为 `friend(id)`、`friend(friendId)` 自动创建查询索引，改善缓存未命中与关闭缓存时的双向查询。
- 停服日志输出 hits、misses、errors、fills、sqlite_queries，程序内也可调用 `cacheStats()`。misses 只统计 Redis 返回空值或无效编码；关闭缓存和故障回源不计入 misses。

当前采用复用的同步 hiredis 连接，访问受数据库互斥锁保护，仍会占用调用它的 I/O 线程。20 ms 是每次连接/读写等待的超时设置，不是整个请求的延迟上限。此版本没有异步 Redis、连接池或分布式会话功能。

### 验证与性能范围

`redis_cache_test.py` 自动启动独立的临时 Redis，在随机回环端口测试命中、空列表、双方失效、并发读写、损坏值、TTL、暂停 Redis 导致的超时、降级后的更新与恢复、重启隔离、数据库失败、AUTH 和实际聊天。测试退出时停止临时进程，不操作已有 Redis 实例。CI 已加入依赖与测试。

复现好友查询微基准：

```bash
python3 EpollServer/tests/redis_cache_benchmark.py \
  ./build-epoll/redis_cache_probe --output redis-benchmark.json
```

2026-09-22 本机 Ubuntu 22.04 / WSL2、GCC 11.4 Release、Redis 6.0.16、SQLite 3.37.2：每组预热一次，再对同一用户查询 500 次，运行 3 轮，交替测试顺序。SQLite 两组均有索引；取总耗时中位数：

| 数据集 | SQLite | Redis 热缓存 | 解释 |
| --- | ---: | ---: | --- |
| 20 用户、60 条好友关系、返回 6 人 | 31.79 ms | 53.66 ms | 小数据集 Redis 网络开销更大 |
| 1000 用户、5 万条好友关系、返回 100 人 | 57.44 ms | 54.59 ms | 本次总耗时约降低 5%，收益较小且有波动 |

每轮含预热的 501 次查询，SQLite 好友查询从 501 次降为 1 次；预热后的 500 次读取均命中 Redis。该结果是单线程好友查询微基准，不代表聊天吞吐、并发容量或端到端 P99 改善。Redis 默认运行时关闭，小规模演示可继续使用 SQLite。完整数据见 [基准原始结果](../notes/2026-09-22-redis-benchmark.json)。

API 参考：[hiredis 官方说明](https://github.com/redis/hiredis)、[Redis SET/EX](https://redis.io/docs/latest/commands/set/)。

