@echo off
rem ============================================================
rem  Fingerprint Voting System - one-click setup (Windows)
rem  1. Python            2. server packages
rem  3. arduino-cli + ESP8266 core + library
rem  4. compile the NodeMCU firmware   5. upload it
rem  6. write server\config.json, start the server, open the dashboard
rem  Re-run any time; finished steps are skipped.
rem ============================================================
setlocal EnableExtensions
title Voting System Setup
cd /d "%~dp0"

set "ROOT=%~dp0"
set "SKETCH=%ROOT%firmware\voting_bridge"
set "CONFIG=%ROOT%server\config.json"
set "VENV=%ROOT%.venv"
set "TOOLS=%ROOT%tools"
set "FQBN=esp8266:esp8266:nodemcuv2:eesz=4M1M"
set "CORE=esp8266:esp8266@3.1.2"
set "ESP_URL=https://arduino.esp8266.com/stable/package_esp8266com_index.json"
set "CLI="
set "PYEXE="
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%PS%" set "PS=pwsh"

echo.
echo  ===== Fingerprint Voting System : setup =====
echo   The NodeMCU only reads the sensor and buttons.
echo   This PC runs the database, SMS and the web dashboard.
echo.

if not exist "%SKETCH%\voting_bridge.ino" (
  echo [X] Cannot find %SKETCH%\voting_bridge.ino
  echo     Run this file from the repository root.
  goto :fail
)

rem ---------- 1. Python ----------
echo [1/6] Looking for Python 3.8 or newer...
call :findpython
if not defined PYEXE (
  echo       Python was not found.
  where winget >nul 2>&1
  if errorlevel 1 (
    echo [X] Install Python from https://www.python.org/downloads/ ^(tick "Add python.exe to PATH"^) and run setup.bat again.
    goto :fail
  )
  set "ANS=Y"
  set /p "ANS=      Install Python 3.12 now with winget? [Y/n]: "
  if /i "%ANS%"=="N" goto :fail
  winget install -e --id Python.Python.3.12 --accept-package-agreements --accept-source-agreements
  call :findpython
  if not defined PYEXE (
    echo.
    echo       Python is installed, but this window cannot see it yet.
    echo       Close this window and run setup.bat again.
    goto :fail
  )
)
echo       Using: %PYEXE%

rem ---------- 2. server packages ----------
echo.
echo [2/6] Installing the server packages ^(pyserial^)...
if not exist "%VENV%\Scripts\python.exe" (
  "%PYEXE%" -m venv "%VENV%"
  if errorlevel 1 (
    echo [X] Could not create the Python environment.
    goto :fail
  )
)
"%VENV%\Scripts\python.exe" -m pip install --disable-pip-version-check -q -r "%ROOT%server\requirements.txt"
if errorlevel 1 (
  echo [X] pip install failed. Check your internet connection.
  goto :fail
)
echo       Done.

rem ---------- 3. arduino-cli, ESP8266 core, library ----------
echo.
echo [3/6] Looking for arduino-cli...
where arduino-cli >nul 2>&1 && set "CLI=arduino-cli"
if not defined CLI if exist "%TOOLS%\arduino-cli.exe" set "CLI=%TOOLS%\arduino-cli.exe"
if not defined CLI if exist "%LOCALAPPDATA%\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe" set "CLI=%LOCALAPPDATA%\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
if not defined CLI (
  echo       Not found. Downloading the official build from arduino.cc ...
  if not exist "%TOOLS%" mkdir "%TOOLS%"
  "%PS%" -NoProfile -ExecutionPolicy Bypass -Command "[Net.ServicePointManager]::SecurityProtocol='Tls12'; Invoke-WebRequest -UseBasicParsing -Uri 'https://downloads.arduino.cc/arduino-cli/arduino-cli_latest_Windows_64bit.zip' -OutFile '%TOOLS%\cli.zip'; Expand-Archive -Force '%TOOLS%\cli.zip' '%TOOLS%'; Remove-Item '%TOOLS%\cli.zip'"
  if exist "%TOOLS%\arduino-cli.exe" set "CLI=%TOOLS%\arduino-cli.exe"
)
if not defined CLI (
  echo [X] Could not get arduino-cli. Check your internet connection, or install it from
  echo     https://arduino.github.io/arduino-cli/ and run this file again.
  goto :fail
)
echo       Using: %CLI%
echo       Installing the ESP8266 board package ^(first time is about 300 MB^)...
"%CLI%" config add board_manager.additional_urls %ESP_URL% >nul 2>&1
"%CLI%" core update-index
if errorlevel 1 goto :fail
"%CLI%" core install %CORE%
if errorlevel 1 goto :fail
"%CLI%" lib install "Adafruit Fingerprint Sensor Library"
if errorlevel 1 goto :fail

rem ---------- 4. compile ----------
echo.
echo [4/6] Compiling the NodeMCU firmware ^(firmware\voting_bridge^)...
"%CLI%" compile --fqbn %FQBN% "%SKETCH%"
if errorlevel 1 (
  echo [X] Compile failed. Read the first error above.
  goto :fail
)

rem ---------- 5. upload ----------
echo.
echo [5/6] Upload to the NodeMCU
echo       Plug it in with a data USB cable. Detected ports:
echo.
"%CLI%" board list
echo.
set "PORT="
set /p "PORT=      Type the COM port (for example COM3), or press Enter to skip the upload: "
if not "%PORT%"=="" (
  "%CLI%" upload -p %PORT% --fqbn %FQBN% "%SKETCH%"
  if errorlevel 1 (
    echo [X] Upload failed. Check the cable and the COM port, and close the Serial Monitor.
    goto :fail
  )
  echo       Uploaded.
) else (
  echo       Skipped. Upload later with:  "%CLI%" upload -p COMx --fqbn %FQBN% firmware\voting_bridge
  echo       The server finds the NodeMCU on USB by itself.
)

rem ---------- 6. config, firewall, start ----------
echo.
echo [6/6] Server settings ^(stored only in server\config.json, which git ignores^)
if not exist "%CONFIG%" goto :ask_config
set "REPLACE=N"
set /p "REPLACE=      config.json already exists. Replace it? [y/N]: "
if /i not "%REPLACE%"=="Y" goto :after_config

:ask_config
set "VS_ADMIN=admin"
set /p "VS_ADMIN=      Dashboard username [admin]  : "
set /p "VS_ADMINPASS=      Dashboard password          : "
set /p "VS_APIKEY=      CircuitDigest API key       : "
set /p "VS_PHONE=      Admin phone (10 digits)     : "
set "VS_PORT=%PORT%"
"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\make-config.ps1" -Out "%CONFIG%"
if errorlevel 1 goto :fail

:after_config
echo.
set "FW=Y"
set /p "FW=      Let phones and laptops on your WiFi open the dashboard? (adds a Windows Firewall rule) [Y/n]: "
if /i not "%FW%"=="N" (
  "%PS%" -NoProfile -ExecutionPolicy Bypass -Command "Start-Process netsh -Verb RunAs -Wait -ArgumentList 'advfirewall firewall add rule name=VotingDashboard dir=in action=allow protocol=TCP localport=8080 profile=private'"
)

echo.
echo       Starting the server in a new window...
start "Voting Server" "%ROOT%start.bat"
timeout /t 4 /nobreak >nul
start "" "http://localhost:8080"

echo.
echo  ===== Done =====
echo  Dashboard : http://localhost:8080   ^(other devices: the address shown in the Voting Server window^)
echo  Login     : the dashboard username and password you entered
echo  Next time : double-click start.bat
echo  Plug the NodeMCU into this PC with the USB cable and keep it plugged in while voting.
echo.
pause
exit /b 0

:findpython
set "PYEXE="
for %%P in (python py) do (
  if not defined PYEXE (
    %%P -c "import sys; sys.exit(0 if sys.version_info[:2] >= (3, 8) else 1)" >nul 2>&1
    if not errorlevel 1 set "PYEXE=%%P"
  )
)
exit /b 0

:fail
echo.
echo  Setup stopped. Fix the problem above and run setup.bat again.
echo.
pause
exit /b 1
