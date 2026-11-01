# TCP 报文分帧测试

在安装了 Qt 5（含 Qt Test）和 MinGW 的 Windows 环境中，从项目根目录运行：

```powershell
New-Item -ItemType Directory -Force build-pduframer-test
Set-Location build-pduframer-test
qmake ..\TcpClient\tests\pduframer_test.pro
mingw32-make
.\release\pduframer_test.exe -o test-results.txt,txt
Get-Content test-results.txt
```

测试覆盖逐字节拆包、多条报文合包、非法报文总长度和非法消息长度。当前协议沿用项目原有的本机字节序与 `PDU` 内存布局；跨平台传输需要另行定义稳定的序列化格式。
