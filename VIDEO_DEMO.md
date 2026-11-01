# 远程视频监控：Qt 摄像头上传 → Linux OpenCV 运动检测 → Qt 显示

这是在现有 Qt/TCP 项目上新增的视频入口。无参数启动仍打开原聊天客户端；
`TcpClient.exe --monitor` 打开视频界面。首版采集和观看位于同一个 Qt 窗口，
服务端只把处理结果返回上传连接，尚未实现其他客户端订阅观看、多路视频或录像。

## 本机启动

Windows 端使用现有 Qt 5.14.2 / MinGW 7.3，新增 Qt Multimedia 模块读取摄像头。
OpenCV 实际运行在 Linux 服务端，Windows 不需要 OpenCV DLL。

Ubuntu WSL 首次安装依赖：

```bash
sudo apt-get update
sudo apt-get install --no-install-recommends build-essential cmake libsqlite3-dev libopencv-dev python3-opencv
```

1. 双击 `tools/start-video-server.cmd`。它在 WSL 中构建视频服务端并监听 18888；保持窗口运行。
2. Windows PowerShell 中执行 `powershell -ExecutionPolicy Bypass -File tools/build-video-client.ps1`。
3. 双击 `tools/start-video-client.cmd`。
4. 服务器填 `127.0.0.1`，端口 `18888`，自行填写用户名、密码（各 1～31 个 UTF-8 字节）。
   第一次点击“注册并连接”，以后点击“连接”。不自动生成或保存密码。
5. 点击“运动演示”，先发送静止的合成场景学习背景，再发送一起移动的三块分离色块，
   右侧应把它们合成一个稳定的运动框。
   左侧为原图，右侧会出现绿色运动框，状态依次为“背景学习中”“暂无运动”“检测到运动”。
   演示不打开摄像头，约 9 秒后自动停止；网络慢时耗时增加。
6. 点击“启动摄像头”，固定摄像头并等待前 20 个返回帧完成背景学习，通常需要约 2 秒或更久。
   学习结束后挥手或移动物体，右侧显示彩色画面和绿色运动框。“停止采集”会停止摄像头或合成演示，
   “断开”关闭网络和摄像头。
7. 调整摄像头角度或更换场景后点击“重置背景”，从下一帧重新学习。
   启动摄像头、启动演示、重连及图像分辨率变化也会重新学习。

“发送彩色测试图”保留用于单帧网络检查，返回彩色图像并显示背景学习进度；
单击只发送一帧，不能单靠它完成 20 帧背景学习。切换到测试图会停止当前采集并重置背景。

如 WSL localhost 转发不可用，用 `wsl -d Ubuntu -- hostname -I` 查询其地址并填入服务器栏。
Windows 相机权限需允许桌面应用使用摄像头；相机被其他程序独占时先关闭占用程序。
启动应用不会自动打开摄像头。默认尝试 640×480，上传图像缩放到该边界内，目标 10 FPS；
实际帧率受摄像头、处理速度和网络影响，尚不承诺性能指标。

数据库和文件数据保存在 `build-video-runtime/<端口>/`，编译输出位于 `build-video-client/`
和 `build-video-server/`，均被现有 `build-*/` Git 规则忽略。
本地端口与旧服务端 8888 区分，原有数据库不迁移、不覆盖。

本机 Ubuntu 已经用户同意迁到 `F:\codex-qt\build-wsl-ubuntu`，解决 C 盘空间不足。
**该目录是实际的 WSL 发行版数据，不是编译输出，不要删除，也不要批量清理所有 `build-*` 目录。**

## 通信和线程

复用现有 76 字节小端 PDU 头，类型 200（JPEG 上传）、201（带运动框的彩色 JPEG）、202（错误文本）。
`caData` 前 32 字节放十进制帧序号；后 32 字节在请求中可写 `reset`，在成功响应中返回
`warmup:N/20`、`idle:0` 或 `motion:N`（N 为保留的运动框数，最多 16）。
正文长度取 `uiMsgLen`，必须等于包长减 76。
仍依赖已有注册/登录认证，不允许匿名上传。

- Qt 摄像头 surface 兼容 RGB、常见 YUV 和 JPEG 格式；映射期间复制像素，解映射后不引用摄像头内存。
- 摄像头只保留一张最新预览图；GUI 以 100 ms 周期取帧，只有上一张收到响应才上传下一张。
- Qt 工作线程负责 JPEG 编码、Socket 收发和 JPEG 解码；主线程更新界面。
- epoll I/O 线程只负责网络和任务投递；独立 OpenCV 工作线程处理解码、运动检测、标框和 JPEG 编码。
- 每连接独立持有背景状态，只有 OpenCV 工作线程操作模型；断连后随连接/最后一个在途任务释放，
  不把不同连接的背景混在一起，不使用永久增长的全局模型表。
- 检测输入保持比例缩小到不超过 320×240，灰度化、高斯平滑后执行 MOG2（关闭阴影标记），
  用前 20 帧学习背景且不告警，再执行开/闭运算和轮廓面积过滤，将框坐标映射回彩色原图。
- 微小区域被过滤；剩余区域按面积取前 16 个，绿色框表示运动区域，不代表人数或物体类别。
- 后处理只使用传统图像处理和几何规则：合并邻近碎片后过滤小区域，连续 2 帧确认才显示；
  用坐标/尺寸指数平滑减少抖动，容忍 2 个收到的视频帧漏检，第 3 个漏检帧移除。
  该策略增加约一个帧间隔的首次确认等待，并短暂保留刚消失的框，以减少闪烁。
- 原始碎片最多取 128 个参与合并，短期关联记录最多 32 条，显示最多 16 个框；
  分辨率变化或重置背景时同时清空关联状态。未引入神经网络、训练流程、模型权重或额外推理框架。
- 每连接只允许一个视频任务在途，处理队列最多 16 个待办；视频正文最多 512 KiB。
- 解码前检查 JPEG SOF 的像素尺寸，最高 1280×720 / 8 位；失败返回错误，不终止工作线程。
- 工作线程通过 Reactor::post/eventfd 将结果交回连接所属 I/O 线程；关闭连接后丢弃迟到结果。
- Qt 请求/连接超时 5 秒；不会无限排队或自动重连。状态栏“往返耗时”从 JPEG 编码后开始，
  到返回图像解码完成为止，既不是纯 OpenCV 耗时，也不是完整采集到屏幕显示延迟。

## 验证命令

Linux（视频选项默认 OFF，不影响原 ARM 构建）：

```bash
cmake -S EpollServer -B build-video-server -DCMAKE_BUILD_TYPE=Release -DCHAT_WITH_VIDEO=ON -DCHAT_WITH_REDIS=OFF
cmake --build build-video-server -j2
ctest --test-dir build-video-server --output-on-failure
```

`video_integration_test.py` 用临时数据库和随机端口启动真实服务端，验证不同背景的两客户端
彩色回传、20 帧学习、静止和微噪声不触发、移动物体的真实绿色框、运动消失、
手动重置、分辨率变化、跨连接模型隔离，以及帧序号、TCP 拆包、未登录拒绝、非法 JPEG、
正文/尺寸上限、错误恢复和停服。
新增 `motion_regions_test` 覆盖碎片合并、远目标分离、单帧闪动过滤、坐标和尺寸平滑、
漏检容忍、过期清理及重置；网络测试也验证实际 JPEG 的合并框和双目标分离结果。
需要 Ubuntu 的 `python3-opencv`，不打开摄像头。

Windows：`TcpClient/tests/camerasurface_test.pro` 验证 RGB 拷贝所有权、YUYV 转换、最新帧提取；
设置 `VIDEO_TEST_CAMERA=1` 后可额外短暂读取实际摄像头 5 帧，仅验证采集、不保存图像。
在视频服务端启动后执行 `TcpClient.exe --monitor-smoke --host 127.0.0.1 --port 18888 --user <唯一测试名>`，
会注册临时测试账号，使用真实 Qt 编码/网络/解码连续校验 30 帧合成场景的学习、静止和运动状态，
成功输出 `QT_MOTION_SMOKE_PASS frames=30`，不访问摄像头。

`TcpClient/tests/monitor_test.pro` 测试空密码后的界面恢复；设置 `VIDEO_TEST_PORT=18888`
后还验证真实 Qt 窗口经工作线程注册、彩色回显、运动演示的学习/静止/运动状态、背景重置、
停止采集及断开。测试截图 `build-video-ui-test/monitor-motion-ui.png` 只使用合成场景。

本机 2026-10-31 验证：Qt 5.14.2 / MinGW 7.3 客户端 Release 编译通过；
Ubuntu 22.04 / GCC 11.4 / OpenCV 4.5.4 视频构建及 4 项 CTest 通过；
首版摄像头测试通过（Integrated Camera 实际读取 5 帧后关闭）；
运动检测用合成场景验证，不自动上传或保存真实摄像头画面。
运动版 Windows 原生 GUI 测试通过，Qt → WSL 连续 30 帧状态校验通过，
截图见 `build-video-ui-test/monitor-motion-ui.png`，详细记录见 `notes/2026-10-31-motion-verification.md`。
碎框优化后 CTest 增至 5 项且通过，新增记录见 `notes/2026-10-31-motion-stability-verification.md`。

## 后续边界

目前是 JPEG/TCP 局域网原型，尚无独立上传端与观看端订阅、H.264、音频、录像、自动重连、TLS。
运动检测面向固定摄像头；光照突变、摄像头晃动可能造成误报，长期静止物体可能融入背景。
它不识别人、车等类别，也不能把运动框数量当作人数。
距离很近的不同目标仍可能合并，同一个目标的变化区域如果间隔很远仍可能分开；
区域合并与几何关联不能保证“一人一个框”。
已有账号协议明文传输密码，数据库沿用原明文密码格式。接入公网云服务器前应先完善
安全传输和部署方式；当前只做本地 WSL 验证，无需提供云服务器凭据。
