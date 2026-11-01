@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start-video-server.ps1" %1
if errorlevel 1 (
    pause
    exit /b 1
)
