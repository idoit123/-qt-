# Qt/TCP 即时通信系统

新增 [Qt + OpenCV 远程视频监控首版](VIDEO_DEMO.md)：电脑摄像头上传到 Linux 服务端，
服务端使用 MOG2 检测运动并在彩色原图上标框后回传，Qt 并排显示本地原图和服务器结果；
支持背景学习状态、重置背景和无需摄像头的运动演示，通过 `--monitor` 进入。
运动区域加入邻近碎片合并、连续帧确认及坐标平滑，仅使用传统 OpenCV 与几何规则，不引入神经网络或训练流程。

基于 C++11、Qt 5 Widgets、Qt Network 和 SQLite 的客户端／服务端练习项目。当前首版聚焦注册登录、在线状态、好友关系、私聊与群聊。文件传输相关界面和协议仍在仓库中，但服务端处理尚未完成，不属于当前验收范围。

仓库同时新增了 [Linux epoll 多 Reactor 服务端](EpollServer/README.md)，保留 Qt 服务端作为稳定基线，用于后续比较一连接一线程与固定 I/O 线程多路复用模型。

## 当前架构

- 服务端为每个 TCP 连接创建一个 `QThread`，Socket 在所属线程处理收发与数据库访问。
- 收包使用 PDU 长度字段和接收缓冲区处理半包、粘包，拒绝非法长度；客户端分帧逻辑可独立测试。
- 在线用户映射由服务端主线程管理，转发消息通过 Qt 队列投递到目标 Socket 所属线程。
- 服务端将登录身份绑定到 TCP 连接；未登录连接不能访问业务接口，聊天和好友操作会校验报文身份，拒绝冒用其他用户。
- 所有已启用的发送路径统一经过 1 MiB 高水位背压保护；每个连接使用令牌桶限制持续消息洪泛，并限制单条聊天消息大小，过载连接会被隔离。
- SQLite 连接按线程创建并在断连时释放；写操作受互斥锁保护，用户输入通过预编译语句绑定。首次启动会自动创建所需表。
- 停服时停止监听、关闭活动连接，并等待工作线程退出。

## 构建和运行

在安装 Qt 5（包含 Widgets、Network、SQL、Test）及 MinGW 的 Windows 环境中，可分别用 Qt Creator 打开 `TcpServer/TcpServer.pro`、`TcpClient/TcpClient.pro`。也可在项目根目录运行：

```powershell
New-Item -ItemType Directory -Force build-server,build-client
Set-Location build-server
qmake ..\TcpServer\TcpServer.pro
mingw32-make
Set-Location ..\build-client
qmake ..\TcpClient\TcpClient.pro
mingw32-make
```

先运行服务端，再运行客户端。默认地址为 `127.0.0.1:8888`，分别写在 `TcpServer/server.config` 和 `TcpClient/client.config` 中，并打包进 Qt 资源；修改后需要重新构建。服务端默认在其可执行文件目录创建 `cloud.db`，`server.config` 的可选第三行可以指定其他数据库路径。

## 验证

- [客户端分帧测试](TcpClient/tests/README.md)：逐字节拆包、合包及非法长度。
- [服务端回归测试](TcpServer/tests/README.md)：连接生命周期、数据库查询与 20 客户端聊天集成测试。

本机 Qt 5.14.2 / MinGW 7.3 验证了 20 个客户端同时登录、累计 1000 条私聊消息正确送达，以及一条群聊消息送达 3 个好友；集成测试还覆盖未登录访问、伪造发送者、超大消息、消息洪泛和慢消费者隔离。这是本机功能验收结果，不代表生产环境吞吐能力。

## 当前限制

协议直接传输本机 `PDU` 结构体，依赖双方一致的字节序和内存布局。数据库现有密码字段仍为明文格式，且尚未加入传输加密。文件传输留待后续实现与测试。

## Linux 服务端 Redis 优化

Linux 多 Reactor 服务端新增可选 Redis 好友关系缓存，覆盖好友列表刷新与好友群发查询；包含双方缓存失效、TTL、故障回源、恢复隔离和查询统计，并补充 SQLite 双向好友索引。运行时默认关闭，通过 `CHAT_REDIS_HOST=127.0.0.1` 启用。构建、运行、真实 Redis 回归及微基准见 [Redis 使用说明](EpollServer/README.md#redis-好友缓存可选)。
