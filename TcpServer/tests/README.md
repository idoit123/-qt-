# 服务端回归测试

在安装 Qt 5（含 Qt Test、Qt SQL）和 MinGW 的 Windows 环境中，从项目根目录运行。三个测试项目分别构建到独立目录：

```powershell
New-Item -ItemType Directory -Force build-worker-test
Set-Location build-worker-test
qmake ..\TcpServer\tests\worker_lifecycle_test.pro
mingw32-make
.\release\worker_lifecycle_test.exe -o test-results.txt,txt
Get-Content test-results.txt
```

将上面命令中的 `build-worker-test`、`worker_lifecycle_test.pro`、`worker_lifecycle_test.exe` 换为以下对应项，即可运行另外两组测试：

| 构建目录 | qmake 项目 | 可执行文件 |
| --- | --- | --- |
| `build-opedb-test` | `opedb_test.pro` | `opedb_test.exe` |
| `build-chat-test` | `chat_integration_test.pro` | `chat_integration_test.exe` |

- 生命周期测试：10 个本机连接主动断开、带连接停服时等待工作线程退出，以及非法用户名字段触发断连；不需要数据库。
- 数据库测试：从空的临时 SQLite 数据库启动，覆盖自动建表、注册登录、带引号输入、好友关系及并发登录。
- 聊天集成测试：从空的临时 SQLite 数据库启动，20 个客户端同时登录，发送 1000 条私聊消息，并验证群聊发给 3 位好友；同时检查未登录访问、伪造发送者、超大消息、消息洪泛和慢消费者会被隔离。检查每条私聊的目标、内容及重复情况。

本机 Qt 5.14.2 / MinGW 7.3 验证：生命周期测试连续 10 次通过，数据库测试通过，聊天集成测试连续 5 次通过。以上是功能正确性验收，不代表吞吐量或生产负载性能。
