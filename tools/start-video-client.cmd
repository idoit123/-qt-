@echo off
setlocal
if not defined QT_VIDEO_ROOT set "QT_VIDEO_ROOT=F:\Qt\Qt5.14.2"
set "PATH=%QT_VIDEO_ROOT%\5.14.2\mingw73_64\bin;%QT_VIDEO_ROOT%\Tools\mingw730_64\bin;%PATH%"
set "QT_QPA_PLATFORM=windows"
if not exist "%~dp0..\build-video-client\release\TcpClient.exe" (
    echo Build first: powershell -ExecutionPolicy Bypass -File tools\build-video-client.ps1
    pause
    exit /b 1
)
cd /d "%~dp0.."
"%~dp0..\build-video-client\release\TcpClient.exe" --monitor
