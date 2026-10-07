@echo off
rem ============================================================
rem  Fingerprint Voting System - one-click setup (Windows)
rem  1. gets arduino-cli   2. installs ESP8266 core + libraries
rem  3. writes secrets.h   4. compiles   5. uploads   6. opens the console
rem  Re-run any time; finished steps are skipped.
rem ============================================================
setlocal EnableExtensions
title Voting System Setup
cd /d "%~dp0"

set "ROOT=%~dp0"
set "SKETCH=%ROOT%firmware\voting_with_sms"
set "SECRETS=%SKETCH%\secrets.h"
set "TOOLS=%ROOT%tools"
set "FQBN=esp8266:esp8266:nodemcuv2:eesz=4M1M"
set "CORE=esp8266:esp8266@3.1.2"
set "ESP_URL=https://arduino.esp8266.com/stable/package_esp8266com_index.json"
set "CLI="
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%PS%" set "PS=pwsh"

echo.
echo  ===== Fingerprint Voting System : setup =====
echo.

if not exist "%SKETCH%\voting_with_sms.ino" (
  echo [X] Cannot find %SKETCH%\voting_with_sms.ino
  echo     Run this file from the repository root.
  goto :fail
)

rem ---------- 1. arduino-cli ----------
echo [1/6] Looking for arduino-cli...
where arduino-cli >nul 2>&1 && set "CLI=arduino-cli"
if not defined CLI if exist "%TOOLS%\arduino-cli.exe" set "CLI=%TOOLS%\arduino-cli.exe"
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

rem ---------- 2. ESP8266 core + libraries ----------
echo.
echo [2/6] Installing the ESP8266 board package (first time is about 300 MB)...
"%CLI%" config add board_manager.additional_urls %ESP_URL% >nul 2>&1
"%CLI%" core update-index
if errorlevel 1 goto :fail
"%CLI%" core install %CORE%
if errorlevel 1 goto :fail

echo.
echo       Installing libraries...
"%CLI%" lib install "Adafruit Fingerprint Sensor Library"
if errorlevel 1 goto :fail

rem ---------- 3. secrets.h ----------
echo.
echo [3/6] Credentials (stored only in secrets.h, which git ignores)
if not exist "%SECRETS%" goto :ask_secrets
set "WRITE_SECRETS=N"
set /p "WRITE_SECRETS=      secrets.h already exists. Replace it? [y/N]: "
if /i not "%WRITE_SECRETS%"=="Y" goto :after_secrets

:ask_secrets
set "VS_ADMIN=admin"
set /p "VS_SSID=      WiFi name (2.4 GHz)        : "
set /p "VS_WIFIPASS=      WiFi password              : "
set /p "VS_ADMIN=      Console username [admin]   : "
set /p "VS_ADMINPASS=      Console password           : "
set /p "VS_APIKEY=      CircuitDigest API key      : "
set /p "VS_PHONE=      Admin phone (10 digits)    : "
"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\make-secrets.ps1" -Out "%SECRETS%"
if errorlevel 1 goto :fail
goto :after_secrets

:after_secrets
echo       secrets.h is ready.

rem ---------- 4. compile ----------
echo.
echo [4/6] Compiling the firmware...
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
set /p "PORT=      Type the COM port (for example COM3), or press Enter to skip: "
if not "%PORT%"=="" (
  "%CLI%" upload -p %PORT% --fqbn %FQBN% "%SKETCH%"
  if errorlevel 1 (
    echo [X] Upload failed. Check the cable, the COM port, and close the Serial Monitor.
    goto :fail
  )
  echo       Uploaded. Waiting 20 s for the board to join WiFi...
  timeout /t 20 /nobreak >nul
) else (
  echo       Skipped. Upload later with:  "%CLI%" upload -p COMx --fqbn %FQBN% firmware\voting_with_sms
)

rem ---------- optional: legacy Python app ----------
echo.
set "PY="
set /p "PY=      Also install the older laptop-GUI version (Python)? [y/N]: "
if /i "%PY%"=="Y" (
  where python >nul 2>&1
  if errorlevel 1 (
    echo       Python not found. Install it from https://www.python.org/downloads/ and re-run.
  ) else (
    python -m pip install --upgrade pyserial requests
  )
)

rem ---------- 6. open the system ----------
echo.
echo [6/6] Opening the admin console and the setup guide...
start "" "%ROOT%docs\circuitdigest-setup.html"
if not "%PORT%"=="" (
  echo       Console: http://voting.local   ^(or the IP shown on the LCD / serial monitor^)
  start "" "http://voting.local"
)

echo.
echo  ===== Done =====
echo  Console login is the username and password you entered.
echo  If voting.local does not open, use the IP address shown on the LCD.
echo.
pause
exit /b 0

:fail
echo.
echo  Setup stopped. Fix the problem above and run setup.bat again.
echo.
pause
exit /b 1
