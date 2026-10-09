# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

Fingerprint voting system: an ESP8266 NodeMCU with an R307/R305 fingerprint sensor, 4 vote buttons, a 16x2 I2C LCD and a buzzer, plus a Python server on a PC that holds the database, runs the vote logic, sends SMS through the CircuitDigest API and serves a React web dashboard over the LAN.

| Part | Files | Role |
|---|---|---|
| **Firmware (current)** | `firmware/voting_bridge/voting_bridge.ino` | Hardware only. WiFi off. Talks to the PC over USB serial (115200). No database, web server or TLS on the ESP. |
| **Server (current)** | `server/` | `voting_server.py` entry point, package `vs/` (config, store, bridge, sms, core, web), React dashboard in `static/`, tests in `tests/` |
| Legacy | `legacy/standalone_esp8266/` | Old all-on-the-ESP v3 (web console, LittleFS, TLS on the ESP). It ran out of memory, which is why the work moved to the PC. |
| Legacy | `legacy/voting_bridge/`, `legacy/voting_station.py` | First split design with a Tkinter GUI. Old protocol, not compatible with the current server. |

Unless told otherwise, "the firmware" means `firmware/voting_bridge/voting_bridge.ino` and "the server" means `server/`.

## Build / run

- `setup.bat` (Windows): Python + `.venv` + pyserial, arduino-cli + ESP8266 core 3.1.2 + Adafruit Fingerprint library, compile + upload the firmware, write `server/config.json` (via `scripts/make-config.ps1`), firewall rule for port 8080, start the server. `start.bat` starts the server with the `.venv` Python.
- Firmware build check (what was used): `"E:/ardiuno/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe" compile --fqbn esp8266:esp8266:nodemcuv2:eesz=4M1M --libraries "C:/Users/ombha/OneDrive/Documents/Arduino/libraries" firmware/voting_bridge`. Latest: flash 258 KB (24%), static RAM 29.7 KB (37%), IRAM 95% (fixed by the core). Compiled, **not run on hardware yet**.
- The Arduino build auto-generates prototypes ahead of the sketch body, so a type declared in the sketch (e.g. the `EnrState` enum) cannot appear in a function signature. Use `uint8_t` parameters or a header.
- Server: Python 3.8+ (the machine has 3.8.0, so no `match`, no `X | Y` types), only dependency `pyserial`. `python server/voting_server.py [--sim] [--port COM3] [--http-port N] [--config path]`. `VS_DEBUG=1` prints every HTTP request.
- Tests: `cd server && python -m unittest discover -s tests -v`. `test_sim.py` = full system against the built-in firmware simulator; `test_serial.py` = real `SerialBridge` through a fake serial port (unplug/reconnect).
- The dashboard has no build step: React 18 + `htm` are vendored in `server/static/js/vendor/` and loaded as UMD globals; app code is ES modules (`js/main.js`, `js/pages/*.js`). There is no Node.js on this machine. `lib.js` maps `class`/`for` to `className`/`htmlFor`.
- Headless Chrome screenshots: Chrome refuses `fetch()` when the page URL contains `user:pass@`, so use a small proxy that adds the Authorization header.

## Architecture

- Threads: the bridge reader thread calls `Core.on_line()`; a ticker thread (`Core.tick`, every 0.25 s) handles timeouts, keeps the ESP's `SCAN` state in sync with what the server wants, and drives the LCD text; the SMS worker thread sends messages; `ThreadingHTTPServer` handles web requests. All shared state is behind `Core.lock` (an RLock).
- **Never wait for the hardware while holding `Core.lock`.** Use `Core.rpc(cmd, ok_prefix, fail_prefix)` outside the lock (see `voter_delete`, `factory`, `fp_clear`).
- Firmware state machines: scanning, enrollment (PLACE1, REMOVE 0, PLACE2, store, REMOVE 1, VERIFY), template map (`MAP`), button arming. The PC decides everything else (who may vote, timeouts, what to show).
- The serial protocol is documented in the header comment of `voting_bridge.ino` and mirrored by `SimBridge` in `server/vs/bridge.py`. Change all three (firmware `handleCmd`, `Core._rx_*`, `SimBridge._cmd`) together.
- The ESP shows "PC not connected" on the LCD after 7 s without any line from the PC (the server PINGs every 2 s). The server marks the link down after 7 s without a line (the ESP sends `HB` every 2 s) and `SerialBridge` reconnects after 8 s.

## Data

- `server/data/voting_data.json` (git-ignored): voters, tally, settings, SMS log, vote-count history. Written atomically (temp + fsync + rename). The voted flag and the tally go into the same write.
- `server/data/voting_log.txt`: audit log. `--sim` uses `server/data/sim/` so simulated data never mixes with real data.
- An unreadable data file is copied to `voting_data.json.corrupt-<time>` and a fresh database starts (logged as an error).
- Fingerprint templates live in the sensor (slot = voter ID, 1..`max_id`, default 127). Always delete a voter via `/api/voter/delete` so the template goes too.

## Ballot secrecy and ethics (keep these properties)

- Only per-candidate totals are stored. No voter record, log entry, SMS or history sample may contain the candidate a voter chose. `history` stores totals only. `test_2_full_vote_flow` checks this.
- The admin SMS includes the candidate only when `seeC` is on; the UI warns that this breaks secrecy.
- Registration requires an explicit consent checkbox; phones are masked by default.

## SMS

CircuitDigest: `POST https://www.circuitdigest.cloud/api/v1/send_sms?ID=<template>` with `Authorization: <key>` and JSON `{mobiles, var1, var2}`. Templates 111 (voter), 101 (admin), 107 (alert). Variables are cleaned to ASCII and 30 characters. Up to 5 attempts, HTTP 400/401/403/404 fail immediately. Free plan: 100 SMS/month, India only, max 5 linked numbers (the usual reason voter SMS fails). The PC verifies TLS certificates.

## JSON API (Basic auth on everything, POSTs need `X-Req`)

- GET: `/api/state`, `/api/settings`, `/api/voters`, `/api/fp`, `/api/log`, `/api/sms`, `/api/results`, `/api/export.csv`, `/api/results.csv`
- POST: `/api/election`, `/api/enroll`, `/api/enroll/cancel`, `/api/voter/update`, `/api/voter/delete`, `/api/votes/reset`, `/api/factory`, `/api/fp/verify`, `/api/fp/identify`, `/api/fp/delete`, `/api/fp/clear`, `/api/settings`, `/api/sms/test`, `/api/sms/resend`, `/api/sms/clear`, `/api/reboot`; with `--sim` also `/api/sim/finger`, `/api/sim/button`.
- `/api/state.rev` holds change counters (`v`, `s`, `l`, `f`, `c`); the dashboard refetches a list only when its counter moves. JSON key names are shared between `core.py` and `static/js`.

## Pin connections (unchanged)

Sensor TX D1 (GPIO5), RX D2 (GPIO4), VCC to VIN 5 V (swap auto-detected). LCD SDA D3 (GPIO0), SCL D4 (GPIO2). Buttons 1-3 on D5/D6/D7 to GND (INPUT_PULLUP, pressed = LOW); button 4 on D8 (GPIO15) to 3V3 (pressed = HIGH). Buzzer D0 (GPIO16).

## Secrets

`server/config.json` (git-ignored; template `config.example.json`): dashboard login, CircuitDigest key, admin phone, serial port, HTTP port, bind address. `legacy/voting_station.py` contains an old hard-coded API key that is already in git history; it should be rotated.
