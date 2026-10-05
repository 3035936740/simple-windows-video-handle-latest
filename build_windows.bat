@echo off
chcp 65001 >nul
setlocal
python scripts\build_local.py --package %*
if errorlevel 1 exit /b 1
echo 编译和打包完成: bin\SimpleVideoHandle.exe
pause
