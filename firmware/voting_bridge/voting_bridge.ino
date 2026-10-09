/*****************************************************************
 *  Fingerprint Voting System - NodeMCU hardware bridge  (firmware v4)
 *  Board  : ESP8266 (NodeMCU v1.0 / ESP-12E)
 *  Sensor : R307 / R305 optical fingerprint module
 *
 *  The NodeMCU only does the hardware work. The voter database, vote
 *  counting, SMS (CircuitDigest) and the web dashboard all run on the PC
 *  (server/voting_server.py), so the ESP's small memory never fills up.
 *  WiFi is switched OFF: no TLS, no web server, no flash database.
 *
 *  ---------------------------- PIN MAP (unchanged) ----------------
 *   Fingerprint sensor (3.3 V logic UART, 57600 baud)
 *     Sensor TX -> D1 (GPIO5)   Sensor RX -> D2 (GPIO4)  (swap is auto-detected)
 *     Sensor VCC -> VIN (5 V)   Sensor GND -> GND
 *   Vote buttons, no external resistors
 *     Button 1/2/3 -> D5 / D6 / D7 (GPIO14/12/13), other leg to GND, pressed = LOW
 *     Button 4     -> D8 (GPIO15), other leg to 3V3, pressed = HIGH
 *   LCD 16x2 + I2C backpack (0x27 / 0x3F auto-detected)
 *     SDA -> D3 (GPIO0)   SCL -> D4 (GPIO2)   VCC -> VIN   GND -> GND
 *   Buzzer (active)  + -> D0 (GPIO16), - to GND
 *
 *  ---------------------------- USB SERIAL PROTOCOL ----------------
 *  115200 baud, one text line per message.
 *
 *  PC -> NodeMCU                      NodeMCU -> PC
 *  ---------------------------------  --------------------------------------
 *  PING                               PONG
 *  INFO                               INFO sensor=1 capacity=N count=M sec=S fw=4
 *  SEC <1-5>                          OK
 *  SCAN 1 | SCAN 0                    OK   then FP MATCH <id> <conf>
 *                                          | FP NOMATCH | FP ERR <why>
 *  ENROLL <id> <rescan 0|1> <minconf> ENR PLACE1 | ENR REMOVE <0|1> | ENR PLACE2
 *                                     ENR VERIFY | ENR NOTE <why>
 *                                     ENR RETRY <n> <why>
 *                                     ENR OK <id> <conf> | ENR FAIL <why>
 *  CANCEL                             ENR FAIL cancelled
 *  DEL <id>                           DEL OK <id> | DEL FAIL <id>
 *  EMPTY                              EMPTY OK | EMPTY FAIL
 *  MAP <max>                          SLOT <id> <1 present|0 empty|2 unknown> ...
 *                                     MAP DONE <template count>
 *  ARM <ms> | DISARM                  OK   then BTN <1-4> | ARM TIMEOUT
 *  BEEP OK|ERR|LONG|DONE              OK
 *  LCD <line1>|<line2>                (no reply)
 *  RESET                              (restarts the board)
 *                                     READY
 *                                     SENSOR OK <capacity> | SENSOR FAIL | SENSOR LOST
 *                                     HB <sensor 0|1>   (every 2 s)
 *                                     SLOT <id> <1|0>   (template stored / removed)
 *
 *  Everything is non-blocking except the sensor search at boot and the
 *  occasional reconnect probe (<= 250 ms). Do not add delay() calls.
 *
 *  Arduino IDE: board "NodeMCU 1.0", Library: Adafruit Fingerprint Sensor Library
 *****************************************************************/

#include <ESP8266WiFi.h>
#include <SoftwareSerial.h>
#include <Wire.h>
#include <Adafruit_Fingerprint.h>

#define FW_VERSION 4

const uint8_t  BUTTON_PINS[4] = {14, 12, 13, 15};   // D5 D6 D7 D8
const uint8_t  BUZZER_PIN     = 16;                 // D0
const uint8_t  MAX_ID         = 127;
const uint32_t PHASE_TIMEOUT  = 40000;              // enroll: wait for a finger
const uint32_t ENROLL_TIMEOUT = 120000;             // enroll: whole procedure
const uint32_t PC_SILENCE_MS  = 7000;               // no PC traffic for this long = "PC offline"

const uint8_t FP_PIN_A = 5, FP_PIN_B = 4;           // D1, D2 (auto-detected which one is sensor TX)
SoftwareSerial sensorSerial(FP_PIN_A, FP_PIN_B);
Adafruit_Fingerprint finger(&sensorSerial);

enum Mode : uint8_t { MODE_IDLE, MODE_SCAN, MODE_ENROLL };
Mode mode = MODE_IDLE;
bool sensorOk = false;
uint8_t secLevel = 2, commErr = 0;
uint32_t lastPcRx = 0;

/* ================= non-blocking buzzer ================= */
const uint16_t P_OK[]   = {100, 50, 100};
const uint16_t P_ERR[]  = {200, 100, 200, 100, 200};
const uint16_t P_LONG[] = {500};
const uint16_t P_DONE[] = {100, 60, 100, 60, 100};
#define BEEP(p) beep(p, sizeof(p) / sizeof(p[0]))

const uint16_t* buzSteps = nullptr;
uint8_t  buzLen = 0, buzIdx = 0;
uint32_t buzNext = 0;

void beep(const uint16_t* steps, uint8_t len) {
  buzSteps = steps; buzLen = len; buzIdx = 0;
  digitalWrite(BUZZER_PIN, HIGH);
  buzNext = millis() + steps[0];
}

void buzzerTick() {
  if (!buzSteps) return;
  if ((int32_t)(millis() - buzNext) < 0) return;
  buzIdx++;
  if (buzIdx >= buzLen) { digitalWrite(BUZZER_PIN, LOW); buzSteps = nullptr; return; }
  digitalWrite(BUZZER_PIN, (buzIdx % 2 == 0) ? HIGH : LOW);
  buzNext = millis() + buzSteps[buzIdx];
}

/* ================= 16x2 LCD (I2C backpack, minimal driver) =================
 * PCF8574 bits: P0=RS P1=RW P2=EN P3=Backlight P4..P7=D4..D7. Only changed lines are rewritten. */
const uint8_t LCD_SDA = 0, LCD_SCL = 2;             // D3, D4 (D1/D2 belong to the fingerprint sensor)
uint8_t lcdAddr = 0;
char lcdCur[2][17] = {"", ""};
char lcdPc[2][17]  = {"Voting System", "Waiting for PC"};   // text sent by the PC

void lcdNib(uint8_t n, uint8_t rs) {
  uint8_t v = (n << 4) | 0x08 | rs;
  Wire.beginTransmission(lcdAddr); Wire.write(v | 4); Wire.write(v); Wire.endTransmission();
}
void lcdByte(uint8_t b, uint8_t rs) {
  uint8_t hi = (b & 0xF0) | 0x08 | rs, lo = ((b << 4) & 0xF0) | 0x08 | rs;
  Wire.beginTransmission(lcdAddr);
  Wire.write(hi | 4); Wire.write(hi); Wire.write(lo | 4); Wire.write(lo);
  Wire.endTransmission();
}

void lcdBegin() {
  Wire.begin(LCD_SDA, LCD_SCL);
  Wire.setClock(100000);
  const uint8_t tryAddr[] = {0x27, 0x3F, 0x26, 0x20, 0x38};
  for (uint8_t a : tryAddr) { Wire.beginTransmission(a); if (Wire.endTransmission() == 0) { lcdAddr = a; break; } }
  if (!lcdAddr) return;                              // no LCD: carry on without it
  delay(50);
  lcdNib(3, 0); delay(5); lcdNib(3, 0); delay(1); lcdNib(3, 0); delay(1); lcdNib(2, 0); delay(1);
  lcdByte(0x28, 0); lcdByte(0x0C, 0); lcdByte(0x06, 0); lcdByte(0x01, 0); delay(3);
}

void lcdShow(const char* a, const char* b) {
  if (!lcdAddr) return;
  const char* in[2] = {a, b};
  for (int r = 0; r < 2; r++) {
    char line[17];
    size_t i = 0;
    for (; i < 16 && in[r][i]; i++) { uint8_t c = (uint8_t)in[r][i]; line[i] = (c < 32 || c > 126) ? '?' : (char)c; }
    for (; i < 16; i++) line[i] = ' ';
    line[16] = 0;
    if (memcmp(line, lcdCur[r], 17) == 0) continue;
    memcpy(lcdCur[r], line, 17);
    lcdByte(r ? 0xC0 : 0x80, 0);
    for (int k = 0; k < 16; k++) lcdByte((uint8_t)line[k], 1);
  }
}

uint32_t lcdLast = 0;
void lcdTick() {
  uint32_t now = millis();
  if (now - lcdLast < 250) return;
  lcdLast = now;
  if (lastPcRx && now - lastPcRx < PC_SILENCE_MS) lcdShow(lcdPc[0], lcdPc[1]);
  else lcdShow("PC not connected", "Start the server");
}

/* ================= sensor ================= */
bool isCommErr(uint8_t r) {
  return r == FINGERPRINT_PACKETRECIEVEERR || r == FINGERPRINT_TIMEOUT || r == FINGERPRINT_BADPACKET;
}

// Raw handshake: send the "verify password" packet and see whether ANY valid reply (EF 01 ...) comes back.
// Returns 0 = silence, 1 = valid reply, 2 = valid reply but password rejected.
bool echoSeen = false;
uint8_t sensorProbe(uint32_t baud, bool swapped) {
  sensorSerial.end();
  sensorSerial.begin(baud, SWSERIAL_8N1, swapped ? FP_PIN_B : FP_PIN_A, swapped ? FP_PIN_A : FP_PIN_B, false, 128);
  delay(5);
  while (sensorSerial.available()) sensorSerial.read();
  static const uint8_t pkt[] = {0xEF,0x01,0xFF,0xFF,0xFF,0xFF,0x01,0x00,0x07,0x13,0x00,0x00,0x00,0x00,0x00,0x1B};
  sensorSerial.write(pkt, sizeof(pkt));
  uint8_t buf[12]; uint8_t n = 0; uint32_t t = millis();
  while (millis() - t < 250 && n < 12) { if (sensorSerial.available()) buf[n++] = sensorSerial.read(); else yield(); }
  if (n >= 10 && buf[0] == 0xEF && buf[1] == 0x01) {
    if (buf[6] == 0x01) { echoSeen = true; return 0; }          // our own command echoed back = TX/RX shorted, not a sensor
    if (buf[6] == 0x07) return buf[9] == 0x00 ? 1 : 2;           // genuine acknowledge packet
  }
  return 0;
}

const uint32_t FP_BAUDS[] = {57600, 9600, 115200, 38400, 19200};
const uint8_t FP_COMBOS = 10;                       // 5 baud rates x 2 wire orientations
uint8_t fpCombo = 0; bool fpKnown = false;          // fpKnown: fpCombo holds the last working setting

bool sensorTry(uint8_t combo) {
  uint32_t baud = FP_BAUDS[combo % 5]; bool sw = combo >= 5;
  uint8_t p = sensorProbe(baud, sw);
  if (p == 0 || p == 2) return false;
  fpCombo = combo; fpKnown = true;
  return finger.verifyPassword();
}

// full = try every baud/orientation (boot); otherwise the last good setting, or the next combo in turn
bool sensorConnect(bool full) {
  static uint8_t next = 0;
  bool ok = false;
  if (full) { for (uint8_t i = 0; i < FP_COMBOS && !ok; i++) { ok = sensorTry((fpCombo + i) % FP_COMBOS); yield(); } }
  else if (fpKnown) ok = sensorTry(fpCombo);
  else { ok = sensorTry(next); next = (next + 1) % FP_COMBOS; }
  if (!ok) { sensorOk = false; return false; }
  sensorOk = true; commErr = 0;
  finger.setSecurityLevel(secLevel);
  finger.getParameters();
  finger.getTemplateCount();
  Serial.print(F("SENSOR OK ")); Serial.println(finger.capacity);
  return true;
}

void sensorLost(const __FlashStringHelper* why) {
  if (!sensorOk) return;
  sensorOk = false;
  Serial.print(F("SENSOR LOST ")); Serial.println(why);
  BEEP(P_ERR);
}

void noteResult(uint8_t r) {
  if (isCommErr(r)) { if (++commErr >= 8) sensorLost(F("no_response")); }
  else commErr = 0;
}

uint32_t lastSensorTry = 0;
void sensorWatch() {
  if (sensorOk || mode == MODE_ENROLL || millis() - lastSensorTry < (fpKnown ? 3000UL : 1500UL)) return;
  lastSensorTry = millis();
  if (sensorConnect(false)) BEEP(P_OK);
}

void sendInfo() {
  if (sensorOk) finger.getTemplateCount();
  Serial.print(F("INFO sensor="));  Serial.print(sensorOk ? 1 : 0);
  Serial.print(F(" capacity="));    Serial.print(sensorOk ? finger.capacity : 0);
  Serial.print(F(" count="));       Serial.print(sensorOk ? finger.templateCount : 0);
  Serial.print(F(" sec="));         Serial.print(secLevel);
  Serial.print(F(" fw="));          Serial.println(FW_VERSION);
}

/* ================= scanning (voting) ================= */
bool     armed = false;
bool     fingerDown = false;
uint32_t scanNext = 0;
bool     mapRun = false;

void scanTick() {
  if (mode != MODE_SCAN || !sensorOk || armed || mapRun) return;
  uint32_t now = millis();
  if ((int32_t)(now - scanNext) < 0) return;
  scanNext = now + 120;

  uint8_t r = finger.getImage();
  noteResult(r);
  if (r == FINGERPRINT_NOFINGER) { fingerDown = false; return; }
  if (r != FINGERPRINT_OK || fingerDown) return;       // wait until the finger is lifted
  fingerDown = true;

  if (finger.image2Tz(1) != FINGERPRINT_OK) { Serial.println(F("FP ERR image")); return; }
  uint8_t s = finger.fingerFastSearch();
  noteResult(s);
  if (s == FINGERPRINT_OK) {
    Serial.print(F("FP MATCH "));
    Serial.print(finger.fingerID);
    Serial.print(' ');
    Serial.println(finger.confidence);
  } else if (s == FINGERPRINT_NOTFOUND) {
    Serial.println(F("FP NOMATCH"));
  } else {
    Serial.println(F("FP ERR search"));
  }
}

/* ================= template map (which slots hold a template?) ================= */
uint8_t  mapPos = 1, mapMax = MAX_ID, mapRetry = 0;
uint32_t mapLast = 0;

void mapTick() {
  if (!mapRun) return;
  if (!sensorOk) { mapRun = false; Serial.println(F("MAP ABORT")); return; }
  if (mode == MODE_ENROLL || millis() - mapLast < 30) return;
  mapLast = millis();
  uint8_t r = finger.loadModel(mapPos);
  if (isCommErr(r) && ++mapRetry < 3) return;
  Serial.print(F("SLOT ")); Serial.print(mapPos); Serial.print(' ');
  Serial.println(r == FINGERPRINT_OK ? 1 : (isCommErr(r) ? 2 : 0));
  mapRetry = 0;
  if (++mapPos > mapMax) {
    mapRun = false;
    finger.getTemplateCount();
    Serial.print(F("MAP DONE ")); Serial.println(finger.templateCount);
  }
}

/* ================= enrollment ================= */
enum EnrState : uint8_t { E_WAIT1, E_REMOVE, E_WAIT2, E_WAIT3 };
EnrState enr = E_WAIT1, enrAfterRemove = E_WAIT2;
uint8_t  enrId = 0, enrTries = 0, enrPhase = 0, enrMinConf = 40;
bool     enrRescan = false, enrRemoveSeen = false;
uint32_t enrDeadline = 0, enrStarted = 0, enrNext = 0, enrRemovedAt = 0;

void enrollFail(const __FlashStringHelper* why) {
  Serial.print(F("ENR FAIL ")); Serial.println(why);
  BEEP(P_ERR);
  mode = MODE_IDLE;
}

// (takes uint8_t, not EnrState: the IDE's auto-generated prototypes cannot see types declared in the sketch)
void enrollGoRemove(uint8_t next, uint8_t phase) {
  enr = E_REMOVE; enrAfterRemove = (EnrState)next; enrPhase = phase;
  enrRemoveSeen = false;
  enrDeadline = millis() + PHASE_TIMEOUT;
  Serial.print(F("ENR REMOVE ")); Serial.println(phase);
}

// weak or mismatching scans: lift the finger and start over (at most 3 times)
void enrollRetry(const __FlashStringHelper* why) {
  if (++enrTries >= 3) { enrollFail(why); return; }
  Serial.print(F("ENR RETRY ")); Serial.print(enrTries); Serial.print(' '); Serial.println(why);
  BEEP(P_ERR);
  enrollGoRemove(E_WAIT1, 0);
}

void enrollStart(int id, bool rescan, int minConf) {
  if (!sensorOk)             { Serial.println(F("ENR FAIL sensor"));  return; }
  if (id < 1 || id > MAX_ID) { Serial.println(F("ENR FAIL bad_id")); return; }
  enrId = id; enrRescan = rescan; enrMinConf = constrain(minConf, 10, 200);
  enrTries = 0; enrPhase = 0; enr = E_WAIT1;
  enrStarted = millis(); enrDeadline = enrStarted + PHASE_TIMEOUT; enrNext = 0;
  mode = MODE_ENROLL; armed = false;
  Serial.println(F("ENR PLACE1"));
  BEEP(P_LONG);
}

void enrollTick() {
  if (mode != MODE_ENROLL) return;
  uint32_t now = millis();
  if ((int32_t)(now - enrNext) < 0) return;
  enrNext = now + 70;
  if (!sensorOk)                                  { enrollFail(F("sensor"));  return; }
  if ((int32_t)(now - enrDeadline) > 0)           { enrollFail(enr == E_REMOVE ? F("timeout") : F("no_finger")); return; }
  if ((int32_t)(now - (enrStarted + ENROLL_TIMEOUT)) > 0) { enrollFail(F("timeout")); return; }

  uint8_t r;
  switch (enr) {
    case E_WAIT1:
      r = finger.getImage(); noteResult(r);
      if (r != FINGERPRINT_OK) return;
      if (finger.image2Tz(1) != FINGERPRINT_OK) { BEEP(P_ERR); Serial.println(F("ENR NOTE poor_image")); return; }
      // is this finger already enrolled? (a re-scan may match its own slot)
      if (finger.fingerFastSearch() == FINGERPRINT_OK && !(enrRescan && finger.fingerID == enrId)) {
        Serial.print(F("ENR FAIL duplicate ")); Serial.println(finger.fingerID);
        BEEP(P_ERR); mode = MODE_IDLE; return;
      }
      BEEP(P_OK);
      enrollGoRemove(E_WAIT2, 0);
      break;

    case E_REMOVE:
      if (finger.getImage() == FINGERPRINT_NOFINGER) {
        if (!enrRemoveSeen) { enrRemoveSeen = true; enrRemovedAt = now; }
        else if (now - enrRemovedAt > 700) {
          enr = enrAfterRemove;
          enrDeadline = now + PHASE_TIMEOUT;
          if (enr == E_WAIT1) Serial.println(F("ENR PLACE1"));
          else if (enr == E_WAIT2) Serial.println(F("ENR PLACE2"));
          else Serial.println(F("ENR VERIFY"));
          BEEP(P_LONG);
        }
      } else {
        enrRemoveSeen = false;
      }
      break;

    case E_WAIT2:
      r = finger.getImage(); noteResult(r);
      if (r != FINGERPRINT_OK) return;
      if (finger.image2Tz(2) != FINGERPRINT_OK) { BEEP(P_ERR); Serial.println(F("ENR NOTE poor_image")); return; }
      r = finger.createModel();
      if (r == FINGERPRINT_ENROLLMISMATCH) { enrollRetry(F("mismatch")); return; }
      if (r != FINGERPRINT_OK) { Serial.print(F("ENR FAIL model ")); Serial.println(r); BEEP(P_ERR); mode = MODE_IDLE; return; }
      if (finger.storeModel(enrId) != FINGERPRINT_OK) { enrollFail(F("store")); return; }
      Serial.print(F("SLOT ")); Serial.print(enrId); Serial.println(F(" 1"));
      BEEP(P_OK);
      enrollGoRemove(E_WAIT3, 1);                    // then verify the stored template with a fresh scan
      break;

    case E_WAIT3:
      r = finger.getImage(); noteResult(r);
      if (r != FINGERPRINT_OK) return;
      if (finger.image2Tz(1) != FINGERPRINT_OK) { BEEP(P_ERR); Serial.println(F("ENR NOTE poor_image")); return; }
      r = finger.fingerFastSearch();
      if (r == FINGERPRINT_OK && finger.fingerID == enrId && finger.confidence >= enrMinConf) {
        Serial.print(F("ENR OK ")); Serial.print(enrId); Serial.print(' '); Serial.println(finger.confidence);
        BEEP(P_DONE);
        mode = MODE_IDLE;
      } else {                                       // weak template: drop it and scan again
        finger.deleteModel(enrId);
        Serial.print(F("SLOT ")); Serial.print(enrId); Serial.println(F(" 0"));
        enrollRetry(F("weak_template"));
      }
      break;
  }
}

/* ================= vote buttons ================= */
bool     armWaitRelease = false;
uint32_t armUntil = 0, btnSince = 0;
int8_t   btnCand = -1;

// buttons 1-3 are active-LOW (internal pull-up), button 4 is active-HIGH (GPIO15)
bool buttonDown(uint8_t i) { return digitalRead(BUTTON_PINS[i]) == (i < 3 ? LOW : HIGH); }

void disarm() { armed = false; fingerDown = true; }   // a finger still on the glass must not count as a new scan

void buttonsTick() {
  if (!armed) return;
  uint32_t now = millis();
  if ((int32_t)(now - armUntil) >= 0) {
    disarm();
    Serial.println(F("ARM TIMEOUT"));
    BEEP(P_ERR);
    return;
  }
  int8_t down = -1;
  for (uint8_t i = 0; i < 4; i++) if (buttonDown(i)) { down = i; break; }
  if (armWaitRelease) {                       // ignore a button that was already held
    if (down < 0) armWaitRelease = false;
    return;
  }
  if (down < 0) { btnCand = -1; return; }
  if (down != btnCand) { btnCand = down; btnSince = now; return; }
  if (now - btnSince >= 40) {                 // stable for 40 ms = real press
    disarm();
    Serial.print(F("BTN "));
    Serial.println(down + 1);
  }
}

/* ================= command parser ================= */
void handleCmd(char* c) {
  if (!strcmp(c, "PING")) {
    Serial.println(F("PONG"));
  } else if (!strcmp(c, "INFO")) {
    sendInfo();
  } else if (!strncmp(c, "SEC ", 4)) {
    int s = atoi(c + 4);
    if (s >= 1 && s <= 5) { secLevel = s; if (sensorOk) finger.setSecurityLevel(secLevel); }
    Serial.println(F("OK"));
  } else if (!strncmp(c, "SCAN ", 5)) {
    if (atoi(c + 5)) {
      if (mode != MODE_ENROLL) { mode = MODE_SCAN; fingerDown = true; scanNext = 0; }   // finger must be lifted first
    } else if (mode == MODE_SCAN) {
      mode = MODE_IDLE;
    }
    Serial.println(F("OK"));
  } else if (!strncmp(c, "ENROLL ", 7)) {
    int id = 0, rescan = 0, minConf = 40;
    sscanf(c + 7, "%d %d %d", &id, &rescan, &minConf);
    if (mode == MODE_ENROLL) Serial.println(F("ENR FAIL busy"));
    else enrollStart(id, rescan != 0, minConf);
  } else if (!strcmp(c, "CANCEL")) {
    if (mode == MODE_ENROLL) { mode = MODE_IDLE; Serial.println(F("ENR FAIL cancelled")); }
  } else if (!strncmp(c, "DEL ", 4)) {
    int id = atoi(c + 4);
    bool ok = sensorOk && id >= 1 && id <= MAX_ID && finger.deleteModel(id) == FINGERPRINT_OK;
    Serial.print(ok ? F("DEL OK ") : F("DEL FAIL "));
    Serial.println(id);
  } else if (!strcmp(c, "EMPTY")) {
    bool ok = sensorOk && finger.emptyDatabase() == FINGERPRINT_OK;
    Serial.println(ok ? F("EMPTY OK") : F("EMPTY FAIL"));
  } else if (!strncmp(c, "MAP", 3)) {
    int m = atoi(c + 3);
    if (!sensorOk || mode == MODE_ENROLL) { Serial.println(F("MAP ABORT")); return; }
    mapMax = (m >= 1 && m <= MAX_ID) ? m : MAX_ID;
    mapPos = 1; mapRetry = 0; mapRun = true;
  } else if (!strncmp(c, "ARM ", 4)) {
    uint32_t ms = strtoul(c + 4, nullptr, 10);
    if (ms < 1000 || ms > 120000) ms = 30000;
    armed = true; armWaitRelease = true; btnCand = -1;
    armUntil = millis() + ms;
    Serial.println(F("OK"));
  } else if (!strcmp(c, "DISARM")) {
    disarm();
    Serial.println(F("OK"));
  } else if (!strncmp(c, "BEEP ", 5)) {
    const char* t = c + 5;
    if      (!strcmp(t, "OK"))   BEEP(P_OK);
    else if (!strcmp(t, "ERR"))  BEEP(P_ERR);
    else if (!strcmp(t, "LONG")) BEEP(P_LONG);
    else if (!strcmp(t, "DONE")) BEEP(P_DONE);
    Serial.println(F("OK"));
  } else if (!strncmp(c, "LCD ", 4)) {
    char* bar = strchr(c + 4, '|');
    if (bar) *bar = 0;
    strlcpy(lcdPc[0], c + 4, 17);
    strlcpy(lcdPc[1], bar ? bar + 1 : "", 17);
    lcdLast = 0;
  } else if (!strcmp(c, "RESET")) {
    Serial.flush();
    ESP.restart();
  } else {
    Serial.println(F("ERR unknown_cmd"));
  }
}

char    cmdBuf[64];
uint8_t cmdLen = 0;

void pollSerial() {
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      cmdBuf[cmdLen] = 0;
      if (cmdLen) { lastPcRx = millis() ? millis() : 1; handleCmd(cmdBuf); }
      cmdLen = 0;
    } else if (cmdLen < sizeof(cmdBuf) - 1) {
      cmdBuf[cmdLen++] = ch;
    } else {
      cmdLen = 0;                              // over-long line: drop it
    }
  }
}

/* ================= setup / loop ================= */
uint32_t lastHb = 0;

void setup() {
  WiFi.mode(WIFI_OFF);                          // the PC does all networking
  WiFi.forceSleepBegin();
  delay(1);

  Serial.begin(115200);
  for (uint8_t i = 0; i < 4; i++) pinMode(BUTTON_PINS[i], i < 3 ? INPUT_PULLUP : INPUT);   // GPIO15 has no internal pull-up
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  lcdBegin();
  lcdShow("Voting System", "Starting...");
  delay(600);                                   // let the sensor finish booting
  Serial.println();
  Serial.println(F("READY"));
  if (!sensorConnect(true)) {
    Serial.println(echoSeen ? F("SENSOR FAIL echo") : F("SENSOR FAIL silent"));
    BEEP(P_ERR);
  } else {
    BEEP(P_DONE);
  }
  lastSensorTry = millis();
}

void loop() {
  pollSerial();
  buzzerTick();
  buttonsTick();
  sensorWatch();
  scanTick();
  enrollTick();
  mapTick();
  lcdTick();

  uint32_t now = millis();
  if (now - lastHb >= 2000) {
    lastHb = now;
    Serial.print(F("HB ")); Serial.println(sensorOk ? 1 : 0);
  }
  yield();
}
