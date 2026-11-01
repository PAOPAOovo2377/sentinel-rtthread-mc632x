@echo off
setlocal
title MC632X face server
cd /d "%~dp0"

set "PY=C:\Users\25012\.workbuddy\binaries\python\versions\3.13.12\python.exe"

if not exist "%PY%" goto :nopython

echo ============================================================
echo  MC632X cloud-face PC server
echo  python : %PY%
echo  folder : %~dp0
echo  board  : http://192.168.137.1:5000
echo ============================================================
echo  Close this window or press Ctrl+C to stop.
echo.

"%PY%" face_server.py %*
echo.
echo [server stopped]
pause
exit /b 0

:nopython
echo.
echo [ERROR] Bundled Python not found:
echo   %PY%
echo.
echo It may have been removed by a WorkBuddy upgrade.
echo Fix options:
echo   1. install Python 3.13 from python.org, then run:
echo        pip install opencv-contrib-python numpy
echo   2. ask WorkBuddy to re-install the bundled runtime
echo.
pause
exit /b 1
