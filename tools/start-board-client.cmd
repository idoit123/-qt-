@echo off
setlocal
set "CHAT_SERVER_IP=%~1"
if not defined CHAT_SERVER_IP set "CHAT_SERVER_IP=192.168.31.73"
set "QT_QPA_PLATFORM=windows"
set "PATH=F:\Qt\Qt5.14.2\5.14.2\mingw73_64\bin;F:\Qt\Qt5.14.2\Tools\mingw730_64\bin;%PATH%"
if not exist "%~dp0..\build-client-fsmp1a\release\TcpClient.exe" (
    echo Client executable not found. Build build-client-fsmp1a first.
    pause
    exit /b 1
)
cd /d "%~dp0..\build-client-fsmp1a"
"%~dp0..\build-client-fsmp1a\release\TcpClient.exe"
if errorlevel 1 (
    echo Client failed to start. Please report the error shown above.
    pause
    exit /b 1
)
