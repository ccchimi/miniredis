@echo off
REM ============================================================
REM  run-server.bat - starts the miniredis server (double-click)
REM  Optional: pass a port, e.g.  run-server.bat 6500
REM ============================================================
setlocal
cd /d "%~dp0"

set PORT=%1
if "%PORT%"=="" set PORT=6400

if not exist "build\miniredis.exe" (
    echo.
    echo   ERROR: build\miniredis.exe was not found.
    echo   Build the project first, then run this again.
    echo.
    pause
    exit /b 1
)

echo ============================================
echo    miniredis server  -  port %PORT%
echo    Keep this window open.
echo    Press Ctrl+C to stop the server.
echo ============================================
echo.

build\miniredis.exe %PORT%

echo.
echo Server stopped.
pause
