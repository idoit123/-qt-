# 视频监控首版验证记录

2026-10-31，本地 Windows + Ubuntu WSL2 验证，未部署云服务器。

## 实际通过

- Qt 5.14.2 / MinGW 7.3，`build-video-client/release/TcpClient.exe` Release 构建。
- Ubuntu 22.04 / GCC 11.4 / OpenCV 4.5.4，视频选项开启的 Release 构建。
- CTest：`epoll_core_test`、`video_integration_test`、`file_integration_test`、`epoll_integration_test`，4/4 通过。
- 视频集成测试：两客户端共 60 帧，色彩转换与帧序号正确；覆盖分片发送、匿名拒绝、非法 JPEG、512 KiB 长度上限、1280×720 尺寸上限、错误恢复及断连在途任务。
- Qt `camerasurface_test`：RGB 图像所有权、YUYV 转换、单张最新帧，及 Integrated Camera 实际采集 5 帧后关闭；6 passed（含初始化/清理）。真实画面未保存、未上传。
- Qt `monitor_test`：无效账号可重试，原生 Windows 界面注册、上传测试图、显示灰度结果、断开连接；4 passed（含初始化/清理）。
- Qt 客户端 `--monitor-smoke`：Windows → WSL 18888，连续 30 张色条的 JPEG 编码、网络传输、OpenCV 灰度处理及 Qt JPEG 解码，输出 `QT_VIDEO_SMOKE_PASS frames=30`。
- 原生界面截图：`build-video-ui-test/monitor-ui.png`，仅合成测试色条，不含摄像头内容。

## 范围

客户端采集使用 Qt Multimedia；OpenCV 在 Linux 服务端运行。首版返回上传者自己的处理结果，
还不支持独立观看者订阅、录像、自动重连或公网安全部署。目标 10 FPS，不是已测得的性能承诺。
本地摄像头采集与测试图完整链路分别验证，真实摄像头端到端效果留给用户点击启动后查看。

## 环境处理

安装依赖期间 C 盘剩余约 34 MiB，WSL 变只读并无法启动。经用户明确授权，
使用 WSL 官方 `--manage Ubuntu --move` 迁到 `F:\codex-qt\build-wsl-ubuntu`，
保留发行版数据，恢复可写后完成被中断的 dpkg/OpenCV 安装。
该目录被 `build-*/` 忽略，但不是可删除的编译缓存。
