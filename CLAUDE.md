# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

Fingerprint-based voting system on an ESP8266 (NodeMCU) with an R307/R305 fingerprint sensor, 4 vote buttons and a buzzer, plus SMS notifications through the CircuitDigest cloud SMS API. There is no build system, package manifest, test suite or git repo. There are two independent designs in this folder:

| Design | Files | Where the logic runs |
|---|---|---|
| **Standalone v3 (current, enhanced)** | `voting_with_sms.ino` + `webui.h` | Everything on the NodeMCU: voter DB in LittleFS flash, web console served over WiFi |
| Split bridge (older alternative) | `voting_bridge/voting_bridge.ino` + `voting_station.py` | NodeMCU is a USB serial bridge; a Tkinter app on the laptop holds the DB, SMS and GUI |

The two designs are not compatible with each other. Unless told otherwise, "the sketch" means `voting_with_sms.ino`.

## Build / run

- No CLI build is set up. Use the Arduino IDE: board **NodeMCU 1.0 (ESP-12E)**, Flash size **4MB (FS:1MB OTA:~1019KB)**. The LittleFS partition is required or the data won't persist (the sketch logs this on boot).
- ESP8266 core 3.x. Libraries: Adafruit Fingerprint Sensor Library (it pulls in Adafruit BusIO). Everything else ships with the core.
- **One sketch per folder:** `voting_bridge.ino` used to sit in this folder (it now lives in `voting_bridge/`), and the Arduino IDE concatenates every `.ino` in a folder, so building the folder fails with `redefinition of 'void loop()'`. Build `voting_with_sms.ino` from a folder that contains only it (plus `webui.h` and `types.h`), or move `voting_bridge.ino` into its own folder.
- Full build and link check (what was used): the IDE bundles a CLI at `E:/ardiuno/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe`. Run `compile --fqbn esp8266:esp8266:nodemcuv2:eesz=4M1M --libraries "C:/Users/ombha/OneDrive/Documents/Arduino/libraries"` on a folder with only the new sketch. Result (latest): flash 511 KB (48%), static RAM 44.7 KB (55%), IRAM 96% (fixed by the core).
- Syntax check without the IDE: compile `voting_with_sms.ino` (prepend `#include <Arduino.h>`, put `webui.h` on the include path) with `xtensa-lx106-elf-g++ -fsyntax-only`, using the include dirs from `~/AppData/Local/Arduino15/packages/esp8266/hardware/esp8266/3.1.2`. The last change compiled cleanly with `-Wall -Wextra`. It was **not** run on hardware.
- Check the web UI script on its own: extract the `<script>` body from `webui.h` and run `node --check` on it.
- Split design: `pip install pyserial requests`, then `python voting_station.py` (`--sim` runs without hardware).

## Pin connections (voting_with_sms.ino)

| Part | Pin on part | NodeMCU pin | GPIO | Notes |
|---|---|---|---|---|
| Fingerprint sensor | TX | D1 | 5 | SoftwareSerial RX, 57600 baud |
| Fingerprint sensor | RX | D2 | 4 | SoftwareSerial TX |
| Fingerprint sensor | VCC / GND | VIN (5V) / GND | | R305/R307 need 4.2-6V; at 3V3 the LED lights but it won't talk |
| 16x2 LCD (I2C backpack) | SDA / SCL | D3 / D4 | 0 / 2 | Address 0x27 or 0x3F auto-detected. VCC to VIN, GND to GND |
| Vote button 1 | | D5 | 14 | Active-LOW, `INPUT_PULLUP`. Other leg to GND |
| Vote button 2 | | D6 | 12 | Active-LOW, `INPUT_PULLUP`. Other leg to GND |
| Vote button 3 | | D7 | 13 | Active-LOW, `INPUT_PULLUP`. Other leg to GND |
| Vote button 4 | | D8 | 15 | Active-HIGH, plain `INPUT`. Other leg to 3V3. GPIO15 has no internal pull-up, so it relies on the board's own pull-down, which also keeps it LOW at boot |
| Buzzer (active) | + | D0 | 16 | Other leg to GND |

No external resistors. The LCD and the polarity split above are what `voting_with_sms.ino` does now (see `buttonsTick` and the `pinMode` loop in `setup`). `voting_bridge.ino` still uses the old scheme: all four buttons active-HIGH with external 10k pull-downs.

## How it works (voting_with_sms.ino)

1. **Registration.** In the web console, the admin enters ID (1-40, which is also the sensor slot; `MAX_ID`), name, age (18+), 10-digit phone and address. The sensor then guides the voter through two scans of one finger. The template goes into sensor slot = ID and the voter record into flash. Registration is refused if the finger is already enrolled under another ID.
2. **Election.** The admin opens the election with the header button. The sensor is polled every 120 ms. A recognised voter who has not yet voted is "authenticated" and has `voteTimeoutSec` (default 30 s) to press button 1-4.
3. **Vote.** The voter's `voted` flag is saved, then the tally, both to flash, then SMS jobs are queued. The flag is written first on purpose, so a power cut can never allow a second vote. Boot logs a warning if the totals and the flags disagree.
4. **SMS.** A background queue sends and retries (details below). Voting never waits on the network.
5. **Ballot secrecy.** The tally stores only per-candidate totals. Neither the log nor the voter record says who voted for whom. The admin SMS includes the candidate only if the "Include candidate" setting is turned on.

`loop()` is fully non-blocking (state machines for enrollment, scanning, sensor verification, buzzer and SMS). The one exception is a single HTTPS SMS call (1-3 s), which is deferred while an enrollment or an authenticated vote is in progress. Do not add `delay()` calls.

## Code layout

- `voting_with_sms.ino`: configuration constants at the top (WiFi, API key, template IDs, admin login, booth label, timezone), then data model, event log, buzzer, LittleFS storage, SMS queue, sensor, enrollment state machine, voting station, JSON API handlers, setup/loop.
- `types.h`: `SmsJob`, `SmsStatus` and `EnrStep`. They live in a header because the Arduino build auto-generates function prototypes ahead of the sketch body, so any type used in a function signature must be defined before it.
- `webui.h`: the whole admin console as one PROGMEM raw string (`INDEX_HTML`). Light blue and white theme (CSS variables at the top of the style block, no dark mode). Vanilla JS that polls `/api/state` every 1.5 s, or 0.5 s while something is happening.
- Any API change must be made in both files. The UI and firmware share exact JSON key names (`st`, `en`, `rev`, and so on).

## Features

- **Dashboard.** Live turnout ring, per-candidate bars with leader, live station status (idle, scan, authenticated voter with countdown, last result), recent activity and recent SMS.
- **Voters tab (database).** Search, filter (voted, not voted, fingerprint problem), phone masking, add, edit, re-scan finger, delete (blocked once the voter has voted), CSV export.
- **Fingerprints tab.** Map of all 127 slots: OK, missing template (voter registered, sensor empty), orphan template (sensor has a template with no voter), not verified, free. Actions: verify templates (incremental scan of the sensor), test a finger (identify without voting), delete an orphan, re-scan, clear the whole sensor. Sensor info: template count, capacity, security level.
- **SMS tab.** Live log of every message (type, masked number, template, variables, status, attempts, HTTP code) and the last API response. Resend failed messages, send a test, clear finished entries.
- **Activity tab.** Last 30 events (RAM ring): registrations, votes (no candidate), failed or repeat attempts, SMS results, WiFi and sensor changes.
- **Settings.** Candidate names, vote timeout, minimum match confidence, sensor security level, SMS switches (master, voter after voting, voter after registration, admin per vote, include candidate), admin phone. System panel: uptime, IP, RSSI, heap, flash use. Reset votes, erase all data, restart.
- **Safety and extras.** HTTP Basic login on every route. State-changing calls need an `X-Req` header (basic CSRF defence). Admin SMS alert after 5 unknown fingerprints in a row, or when the sensor goes offline (5-minute cooldown). The sensor auto-reconnects. Fallback hotspot `VotingStation` starts if WiFi has not connected after 25 s. mDNS name `voting.local`. NTP clock (IST) for timestamps.

## Database and persistence

Stored in LittleFS. Each file starts with a 4-byte magic derived from `sizeof(struct)`. If a struct layout changes, the old file is ignored and re-initialised, so bump or verify this when editing `Voter` or `AppConfig`.

| File | Content | Written |
|---|---|---|
| `/voters.bin` | `Voter voters[41]`, indexed by sensor slot (record = name, address, phone, age, used, voted, votedAt) | Single record, via `saveVoter(id)` |
| `/tally.bin` | `uint32_t votes[4]` | After each vote or reset |
| `/cfg.bin` | `AppConfig`: candidate names, admin phone, SMS flags, election open/closed, security level, min confidence, vote timeout | After a settings or election change |

Fingerprint templates live inside the sensor, not in flash. The voter file and the sensor must agree: always delete a voter through `/api/voter/delete`, which also removes the template. The Fingerprints tab shows any mismatch.

The event log and SMS log are RAM only and reset on reboot.

## SMS

CircuitDigest API: `POST https://www.circuitdigest.cloud/api/v1/send_sms?ID=<template>` with `Authorization: <key>` and JSON `{mobiles, var1, var2}`. Templates and wording (the variable text is built in `smsOnVote`, `smsOnRegister`, `smsAlert`):

| Purpose | Template | Text |
|---|---|---|
| Voter, registered or voted | 111 | "The task {var1} has been successfully completed at {var2}." |
| Admin, per vote | 101 | "Your {var1} is currently at {var2}." |
| Admin, alert | 107 | "Error {var1} has been detected in {var2}." |

- The queue holds 8 entries. Each job tries up to 5 times, with a longer wait after each failure. HTTP 400/401/403/404 fail immediately.
- A job waits, without using up attempts, while WiFi is down or free heap is below 18 KB.
- Variables are cleaned to ASCII and 30 characters (CircuitDigest limit). Free plan: 100 SMS/month, India only, max 5 linked numbers, which is the usual reason voter SMS fails. The DLT template text must match what is registered with the provider.
- TLS uses `setInsecure()` (no certificate check) with reduced BearSSL buffers (`TLS_RX_BUF`=6144) to save RAM. A server that sends larger TLS records would fail the handshake.

## JSON API (all need Basic auth, POSTs need `X-Req`)

- GET: `/api/state` (poll target), `/api/settings`, `/api/voters`, `/api/fp`, `/api/log`, `/api/sms`, `/api/export.csv`.
- POST: `/api/election`, `/api/enroll`, `/api/enroll/cancel`, `/api/voter/update`, `/api/voter/delete`, `/api/votes/reset`, `/api/factory`, `/api/fp/verify`, `/api/fp/identify`, `/api/fp/delete`, `/api/fp/clear`, `/api/settings`, `/api/sms/test`, `/api/sms/resend`, `/api/sms/clear`, `/api/reboot`.
- `/api/state.rev` holds change counters (`v` voters, `s` sms, `l` log, `f` fingerprints, `c` settings). The UI refetches a list only when its counter moves.

## Things to know

- Secrets live in `secrets.h` (git-ignored; template `secrets.example.h`): WiFi, SMS API key, admin phone, console login. Change the login before real use. Basic auth over plain HTTP is not encrypted, so use only on a trusted network.
- Memory is tight on the ESP8266. The voter table is about 3.4 KB (MAX_ID 40), and an HTTPS SMS needs about 16 KB of heap including a contiguous block of 9 KB (`MIN_BLOCK_FOR_TLS`). Check "Free heap" in Settings > System after changes.
- Behaviour change from the earlier sketch: there is no per-voter "Vote Now" page. The admin opens or closes the election, and the sensor scans continuously while it is open.
- Sensor library constants used: `FINGERPRINT_PACKETRECIEVEERR`, `FINGERPRINT_TIMEOUT`, `FINGERPRINT_BADPACKET` (communication errors count toward an 8-in-a-row sensor-offline trip).

## Split design (voting_bridge.ino + voting_station.py)

- The serial line protocol (115200 baud) is documented in the header comment of `voting_bridge.ino`. Changing a command or event means updating the firmware `handleCmd`, `SerialDevice` and `SimDevice` in `voting_station.py`.
- `voting_station.py` keeps voters and the tally in `voting_data.json`, saved atomically after every change, and writes `voting_log.txt`. Config constants are at the top of the file.
