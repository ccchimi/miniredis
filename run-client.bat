@echo off
REM ============================================================
REM  run-client.bat - opens the interactive client (double-click)
REM  Connects to the server started by run-server.bat.
REM  Optional: pass a port, e.g.  run-client.bat 6500
REM ============================================================
setlocal
cd /d "%~dp0"

set PORT=%1
if "%PORT%"=="" set PORT=6400

echo Connecting to miniredis on port %PORT% ...
echo (make sure run-server.bat is running first)
echo.

powershell -ExecutionPolicy Bypass -File "%~dp0tools\mini-cli.ps1" -Port %PORT%

echo.
pause
