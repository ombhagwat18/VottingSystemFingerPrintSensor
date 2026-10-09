@echo off
rem Starts the voting server (database, SMS and web dashboard). Run setup.bat first.
rem   start.bat           normal, NodeMCU on USB
rem   start.bat --sim     no hardware, built-in simulator
setlocal
title Voting Server
cd /d "%~dp0"
set "PY=%~dp0.venv\Scripts\python.exe"
if not exist "%PY%" (
  echo The Python environment is missing. Run setup.bat first.
  pause
  exit /b 1
)
"%PY%" "%~dp0server\voting_server.py" %*
echo.
echo The server stopped.
pause
