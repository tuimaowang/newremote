@echo off
setlocal
set "ROOT=%~dp0"
if not exist "%ROOT%dist\FSRemoteMessageServer.exe" (
  echo 请先使用 Qt Creator 以 Release 配置编译 FSRemoteMessageServer。
  pause
  exit /b 1
)
echo 正在启动实际消息服务器，固定监听 0.0.0.0:62843...
"%ROOT%dist\FSRemoteMessageServer.exe"
pause
