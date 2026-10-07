/*****************************************************************
 *  Fingerprint Voting - NodeMCU "sensor bridge"  (firmware v3)
 *
 *  The NodeMCU only does the hardware work:
 *    - R307/R305 fingerprint sensor (scan / enroll / delete)
 *    - 4 vote buttons
 *    - buzzer
 *  Everything else (voter database, vote counting, SMS, GUI) runs
 *  on the laptop (voting_station.py) over the USB cable.
 *
 *  WiFi is switched OFF on purpose -> no hotspot problems, no
 *  HTTPS blocking, nothing that can freeze the loop.
 *
 *  Serial: 115200 baud, one command per line.
 *
 *  PC -> NodeMCU                     NodeMCU -> PC
 *  --------------------------------  ---------------------------------
 *  PING                              PONG
 *  INFO                              INFO sensor=1 capacity=127 count=N
 *  SCAN 1 | SCAN 0                   OK      (then FP ... events)
 *  ENROLL <id>                       ENR PLACE1 / REMOVE / PLACE2
 *                                    ENR RETRY <n> <reason>
 *                                    ENR OK <id> | ENR FAIL <reason>
 *  CANCEL                            ENR FAIL cancelled
 *  DEL <id>                          DEL OK <id> | DEL FAIL <id>
 *  EMPTY                             EMPTY OK | EMPTY FAIL
 *  ARM <ms> | DISARM                 OK  (then BTN <1-4> or ARM TIMEOUT)
 *  BEEP OK|ERR|LONG|DONE             OK
 *                                    FP MATCH <id> <confidence>
 *                                    FP NOMATCH | FP ERR <why>
 *                                    HB   (heartbeat every 2 s)
 *                                    READY / SENSOR OK <cap> | SENSOR FAIL
 *
 *  Wiring (unchanged from your old sketch)
 *    Sensor TX -> D1 (GPIO5)    Sensor RX -> D2 (GPIO4)
 *    Buttons   -> D5 D6 D7 D8   (pressed = HIGH, needs a pull-down
 *                                resistor to GND on each button)
 *    Buzzer    -> D0
 *****************************************************************/

#include <ESP8266WiFi.h>
#include <SoftwareSerial.h>
#include <Adafruit_Fingerprint.h>

const uint8_t  BUTTON_PINS[4] = {14, 12, 13, 15};  // D5 D6 D7 D8
const uint8_t  BUZZER_PIN     = 16;                // D0
const uint8_t  MAX_ID         = 127;
const uint32_t PHASE_TIMEOUT  = 30000;             // enroll: per-step timeout

SoftwareSerial sensorSerial(5, 4);                 // RX=D1, TX=D2
Adafruit_Fingerprint finger(&sensorSerial);

enum Mode : uint8_t { MODE_IDLE, MODE_SCAN, MODE_ENROLL };
Mode mode = MODE_IDLE;
bool sensorOk = false;

/* ---------------- non-blocking buzzer ---------------- */
// Patterns alternate ON, OFF, ON, ... durations in ms
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
  if (buzIdx >= buzLen) {
    digitalWrite(BUZZER_PIN, LOW);
    buzSteps = nullptr;
    return;
  }
  digitalWrite(BUZZER_PIN, (buzIdx % 2 == 0) ? HIGH : LOW);
  buzNext = millis() + buzSteps[buzIdx];
}

/* ---------------- sensor ---------------- */
bool sensorInit() {
  finger.begin(57600);
  sensorOk = finger.verifyPassword();
  if (sensorOk) {
    finger.getParameters();
    finger.setSecurityLevel(2);
    Serial.print(F("SENSOR OK "));
    Serial.println(finger.capacity);
  } else {
    Serial.println(F("SENSOR FAIL"));
  }
  return sensorOk;
}

void sendInfo() {
  if (sensorOk) finger.getTemplateCount();
  Serial.print(F("INFO sensor="));  Serial.print(sensorOk ? 1 : 0);
  Serial.print(F(" capacity="));    Serial.print(sensorOk ? finger.capacity : 0);
  Serial.print(F(" count="));       Serial.println(sensorOk ? finger.templateCount : 0);
}

/* ---------------- scanning (voting) ---------------- */
bool     scanWaitRemove = true;
uint32_t scanNext = 0;

void scanTick() {
  if (!sensorOk) return;
  uint32_t now = millis();
  if ((int32_t)(now - scanNext) < 0) return;
  scanNext = now + 80;

  uint8_t r = finger.getImage();
  if (scanWaitRemove) {                       // finger must be lifted between scans
    if (r == FINGERPRINT_NOFINGER) scanWaitRemove = false;
    return;
  }
  if (r != FINGERPRINT_OK) return;            // no finger / packet glitch -> keep polling

  scanWaitRemove = true;
  if (finger.image2Tz(1) != FINGERPRINT_OK) {
    Serial.println(F("FP ERR convert"));
    return;
  }
  uint8_t s = finger.fingerFastSearch();
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

/* ---------------- enrollment ---------------- */
enum EnrState : uint8_t { E_WAIT1, E_REMOVE, E_WAIT2 };
EnrState enr = E_WAIT1;
EnrState enrAfterRemove = E_WAIT2;
uint8_t  enrId = 0, enrTries = 0;
uint32_t enrDeadline = 0, enrNext = 0, enrRemovedAt = 0;
bool     enrRemoveSeen = false;

void enrollFail(const __FlashStringHelper* why) {
  Serial.print(F("ENR FAIL "));
  Serial.println(why);
  BEEP(P_ERR);
  mode = MODE_IDLE;
}

void enrollGoRemove(EnrState next) {
  enr = E_REMOVE;
  enrAfterRemove = next;
  enrRemoveSeen = false;
  enrDeadline = millis() + PHASE_TIMEOUT;
  Serial.println(F("ENR REMOVE"));
}

void enrollRetry(const __FlashStringHelper* why) {
  enrTries++;
  if (enrTries >= 3) { enrollFail(F("too_many_retries")); return; }
  Serial.print(F("ENR RETRY "));
  Serial.print(enrTries);
  Serial.print(' ');
  Serial.println(why);
  BEEP(P_ERR);
  enrollGoRemove(E_WAIT1);
}

void enrollStart(int id) {
  if (!sensorOk)                    { Serial.println(F("ENR FAIL sensor")); return; }
  if (id < 1 || id > MAX_ID)        { Serial.println(F("ENR FAIL bad_id")); return; }
  enrId = id; enrTries = 0;
  enr = E_WAIT1;
  enrDeadline = millis() + PHASE_TIMEOUT;
  enrNext = 0;
  mode = MODE_ENROLL;
  Serial.println(F("ENR PLACE1"));
  BEEP(P_LONG);
}

void enrollTick() {
  uint32_t now = millis();
  if ((int32_t)(now - enrNext) < 0) return;
  enrNext = now + 60;
  if ((int32_t)(now - enrDeadline) > 0) { enrollFail(F("timeout")); return; }

  uint8_t r;
  switch (enr) {
    case E_WAIT1:
      if (finger.getImage() != FINGERPRINT_OK) return;
      if (finger.image2Tz(1) != FINGERPRINT_OK) return;   // bad image, just wait for another
      // Reject a finger that is already enrolled under a different ID
      if (finger.fingerFastSearch() == FINGERPRINT_OK && finger.fingerID != enrId) {
        Serial.print(F("ENR FAIL duplicate "));
        Serial.println(finger.fingerID);
        BEEP(P_ERR);
        mode = MODE_IDLE;
        return;
      }
      BEEP(P_OK);
      enrollGoRemove(E_WAIT2);
      break;

    case E_REMOVE:
      if (finger.getImage() == FINGERPRINT_NOFINGER) {
        if (!enrRemoveSeen) { enrRemoveSeen = true; enrRemovedAt = now; }
        else if (now - enrRemovedAt > 1000) {
          enr = enrAfterRemove;
          enrDeadline = now + PHASE_TIMEOUT;
          Serial.println(enr == E_WAIT1 ? F("ENR PLACE1") : F("ENR PLACE2"));
          BEEP(P_LONG);
        }
      } else {
        enrRemoveSeen = false;
      }
      break;

    case E_WAIT2:
      if (finger.getImage() != FINGERPRINT_OK) return;
      if (finger.image2Tz(2) != FINGERPRINT_OK) { enrollRetry(F("bad_image")); return; }
      r = finger.createModel();
      if (r == FINGERPRINT_ENROLLMISMATCH) { enrollRetry(F("mismatch")); return; }
      if (r != FINGERPRINT_OK)             { enrollFail(F("model"));     return; }
      if (finger.storeModel(enrId) != FINGERPRINT_OK) { enrollFail(F("store")); return; }
      Serial.print(F("ENR OK "));
      Serial.println(enrId);
      BEEP(P_DONE);
      mode = MODE_IDLE;
      break;
  }
}

/* ---------------- vote buttons ---------------- */
bool     armed = false, armWaitRelease = false;
uint32_t armUntil = 0, btnSince = 0;
int8_t   btnCand = -1;

void buttonsTick() {
  if (!armed) return;
  uint32_t now = millis();
  if ((int32_t)(now - armUntil) >= 0) {
    armed = false;
    Serial.println(F("ARM TIMEOUT"));
    BEEP(P_ERR);
    return;
  }
  int8_t down = -1;
  for (uint8_t i = 0; i < 4; i++) {
    if (digitalRead(BUTTON_PINS[i]) == HIGH) { down = i; break; }
  }
  if (armWaitRelease) {                       // ignore a button that was already held
    if (down < 0) armWaitRelease = false;
    return;
  }
  if (down < 0) { btnCand = -1; return; }
  if (down != btnCand) { btnCand = down; btnSince = now; return; }
  if (now - btnSince >= 40) {                 // stable for 40 ms = real press
    armed = false;
    Serial.print(F("BTN "));
    Serial.println(down + 1);
  }
}

/* ---------------- command parser ---------------- */
void handleCmd(char* c) {
  if (!strcmp(c, "PING")) {
    Serial.println(F("PONG"));
  } else if (!strcmp(c, "INFO")) {
    sendInfo();
  } else if (!strncmp(c, "SCAN ", 5)) {
    if (atoi(c + 5)) {
      if (mode != MODE_ENROLL) { mode = MODE_SCAN; scanWaitRemove = true; scanNext = 0; }
    } else if (mode == MODE_SCAN) {
      mode = MODE_IDLE;
    }
    Serial.println(F("OK"));
  } else if (!strncmp(c, "ENROLL ", 7)) {
    enrollStart(atoi(c + 7));
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
  } else if (!strncmp(c, "ARM ", 4)) {
    uint32_t ms = strtoul(c + 4, nullptr, 10);
    if (ms < 1000 || ms > 120000) ms = 30000;
    armed = true; armWaitRelease = true; btnCand = -1;
    armUntil = millis() + ms;
    Serial.println(F("OK"));
  } else if (!strcmp(c, "DISARM")) {
    armed = false;
    Serial.println(F("OK"));
  } else if (!strncmp(c, "BEEP ", 5)) {
    const char* t = c + 5;
    if      (!strcmp(t, "OK"))   BEEP(P_OK);
    else if (!strcmp(t, "ERR"))  BEEP(P_ERR);
    else if (!strcmp(t, "LONG")) BEEP(P_LONG);
    else if (!strcmp(t, "DONE")) BEEP(P_DONE);
    Serial.println(F("OK"));
  } else {
    Serial.println(F("ERR unknown_cmd"));
  }
}

char    cmdBuf[48];
uint8_t cmdLen = 0;

void pollSerial() {
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      cmdBuf[cmdLen] = 0;
      if (cmdLen) handleCmd(cmdBuf);
      cmdLen = 0;
    } else if (cmdLen < sizeof(cmdBuf) - 1) {
      cmdBuf[cmdLen++] = ch;
    }
  }
}

/* ---------------- setup / loop ---------------- */
uint32_t lastHb = 0, lastSensorTry = 0;

void setup() {
  // No WiFi needed - the laptop does all networking.
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();
  delay(1);

  Serial.begin(115200);
  for (uint8_t i = 0; i < 4; i++) pinMode(BUTTON_PINS[i], INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  delay(300);
  Serial.println();
  Serial.println(F("READY"));
  sensorInit();
  BEEP(sensorOk ? P_DONE : P_ERR);
  lastSensorTry = millis();
}

void loop() {
  pollSerial();
  buzzerTick();
  buttonsTick();

  if (mode == MODE_SCAN)        scanTick();
  else if (mode == MODE_ENROLL) enrollTick();

  uint32_t now = millis();
  if (now - lastHb >= 2000) { lastHb = now; Serial.println(F("HB")); }

  // If the sensor was not found at boot, keep retrying quietly
  if (!sensorOk && mode == MODE_IDLE && now - lastSensorTry >= 5000) {
    lastSensorTry = now;
    sensorInit();
  }
  yield();
}
