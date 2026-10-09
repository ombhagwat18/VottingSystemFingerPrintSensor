/*****************************************************************
 *  Fingerprint Voting System with SMS  -  v3.0 (standalone)
 *  Board  : ESP8266 (NodeMCU v1.0 / ESP-12E)
 *  Sensor : R307 / R305 optical fingerprint module
 *  SMS    : CircuitDigest cloud SMS API (HTTPS)
 *
 *  ---------------------------- PIN MAP ----------------------------
 *   Fingerprint sensor (3.3 V logic UART, 57600 baud)
 *     Sensor TX (green/yellow) -> D1 (GPIO5)   [SoftwareSerial RX]
 *     Sensor RX (white)        -> D2 (GPIO4)   [SoftwareSerial TX]
 *     Sensor VCC (red)         -> VIN (5 V)  R305/R307 need 4.2-6 V; at 3V3 the LED lights but it won't talk
 *     Sensor GND (black)       -> GND
 *   Vote buttons, no external resistors
 *     Buttons 1-3 (pressed = LOW, internal pull-up): one leg to the pin, other leg to GND
 *     Button 1 / Candidate 1   -> D5 (GPIO14)
 *     Button 2 / Candidate 2   -> D6 (GPIO12)
 *     Button 3 / Candidate 3   -> D7 (GPIO13)
 *     Button 4 (pressed = HIGH, uses the pull-down already on the board for GPIO15)
 *     Button 4 / Candidate 4   -> D8 (GPIO15)  other leg to 3V3
 *   LCD 16x2 with I2C backpack (PCF8574, address 0x27 or 0x3F, auto-detected)
 *     LCD SDA -> D3 (GPIO0)   LCD SCL -> D4 (GPIO2)   LCD VCC -> VIN (5 V)   LCD GND -> GND
 *   Buzzer (active, 3.3 V)
 *     Buzzer +                 -> D0 (GPIO16)  (- to GND)
 *
 *  ---------------------------- HOW IT WORKS -----------------------
 *   1. Voters are registered from the web console: name, age, phone,
 *      address + two scans of one finger. The sensor stores the
 *      template in slot <voter id>; the voter record is stored in
 *      the ESP's LittleFS flash (survives power loss).
 *   2. Admin opens the election. The sensor is scanned continuously.
 *      A recognised, not-yet-voted voter is "authenticated" and has
 *      a configurable time to press one of the 4 buttons.
 *   3. The vote is counted, the voter is flagged as voted (both saved
 *      to flash) and SMS messages are queued for the voter / admin.
 *      Votes are anonymous: the tally stores only totals.
 *   4. SMS jobs are retried in the background with a log of every
 *      attempt, so a bad network never blocks voting.
 *
 *  Everything in loop() is non-blocking (no delay()), apart from the
 *  HTTPS call of a single SMS (1-3 s), which is deferred while an
 *  enrollment or a vote is in progress.
 *
 *  Arduino IDE: board "NodeMCU 1.0", Flash size "4MB (FS:1MB OTA:~1019KB)"
 *  Libraries  : Adafruit Fingerprint Sensor Library (ESP8266 core 3.x)
 *****************************************************************/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <WiFiClientSecure.h>
#include <SoftwareSerial.h>
#include <Wire.h>
#include <Adafruit_Fingerprint.h>
#include <LittleFS.h>
#include <time.h>
#include <stdarg.h>
#include "webui.h"
#include "types.h"

/* ================= Site configuration ================= */
// WiFi, admin login, SMS key and phone numbers live in secrets.h (git-ignored).
// Copy secrets.example.h to secrets.h and fill it in.
#include "secrets.h"

const char* AP_SSID   = "VotingStation";      // fallback hotspot if WiFi fails
const char* AP_PASS   = "vote1234";
const bool  AUTH_ENABLED = true;

// CircuitDigest SMS templates
const char* TPL_VOTER    = "111";   // "The task {var1} has been successfully completed at {var2}."
const char* TPL_ADMIN    = "101";   // "Your {var1} is currently at {var2}."
const char* TPL_ERROR    = "107";   // "Error {var1} has been detected in {var2}."
const char* BOOTH_LABEL  = "Voting Booth 1";        // shown in SMS text

const long  TZ_OFFSET_SEC = 19800;                  // IST (UTC+5:30)

/* ================= Hardware ================= */
const uint8_t BUTTON_PINS[4] = {14, 12, 13, 15};    // D5 D6 D7 D8
const uint8_t BUZZER_PIN     = 16;                  // D0
const uint8_t MAX_ID         = 40;                  // sensor slots used (1..40) - kept small to leave RAM for HTTPS/SMS
const uint32_t BUTTON_DEBOUNCE_MS = 350;

const uint8_t FP_PIN_A = 5, FP_PIN_B = 4;           // D1, D2 (auto-detected which one is sensor TX)
SoftwareSerial sensorSerial(FP_PIN_A, FP_PIN_B);    // default: RX=D1, TX=D2
Adafruit_Fingerprint finger(&sensorSerial);
ESP8266WebServer server(80);

/* ================= Tunables ================= */
const uint8_t  SMS_MAX_ATTEMPTS  = 5;
const uint32_t SMS_COOLDOWN_MS   = 750;             // min gap between API calls
const uint32_t SMS_HTTP_TIMEOUT  = 8000;
const uint32_t MIN_HEAP_FOR_TLS  = 15000;           // total free heap
const uint32_t MIN_BLOCK_FOR_TLS = 9000;            // largest contiguous block (fragmentation guard)
const uint8_t  SMS_VAR_MAX       = 30;              // CircuitDigest limit per variable
const uint16_t TLS_RX_BUF        = 6144;            // BearSSL buffers (saves RAM)
const uint16_t TLS_TX_BUF        = 512; 
const uint8_t  FAIL_ALERT_COUNT  = 5;               // unknown fingers before admin SMS
const uint32_t ALERT_COOLDOWN_MS = 300000UL;        // 5 min between alert SMS

/* ================= Data model ================= */
struct Voter {
  uint32_t votedAt;      // epoch seconds (0 if unknown)
  char     name[24];
  char     address[40];
  char     phone[11];    // 10 digits
  uint8_t  age;
  uint8_t  used;         // 1 = record exists
  uint8_t  voted;
};
Voter voters[MAX_ID + 1];                           // index = sensor slot

#define NUM_CAND 4
uint32_t votes[NUM_CAND];

struct AppConfig {
  char    cand[NUM_CAND][21];
  char    adminPhone[13];
  uint8_t sms, smsVoter, smsAdmin, smsReg, adminSeesCand;
  uint8_t electionOpen, securityLevel, minConfidence;
  uint16_t voteTimeoutSec;
};
AppConfig cfg;

uint8_t sensorMap[MAX_ID + 1];                      // 0 unknown, 1 present, 2 empty

// change counters -> the web UI only re-downloads lists when these move
uint32_t revVoters = 1, revSms = 1, revFp = 1, revCfg = 1;

/* ================= Activity log (RAM ring) ================= */
enum { EV_INFO = 0, EV_OK = 1, EV_WARN = 2, EV_ERR = 3 };
#define EV_N 20
struct Event { uint32_t id, epoch, ms; uint8_t type; char msg[64]; };
Event events[EV_N];
uint32_t evTotal = 0;

static uint32_t nowEpoch() {
  time_t t = time(nullptr);
  return (t > 1700000000L) ? (uint32_t)t : 0;
}

static void evlog(uint8_t type, const char* fmt, ...) {
  Event& e = events[evTotal % EV_N];
  va_list ap; va_start(ap, fmt);
  vsnprintf(e.msg, sizeof(e.msg), fmt, ap);
  va_end(ap);
  e.type = type; e.epoch = nowEpoch(); e.ms = millis(); e.id = ++evTotal;
  Serial.printf("[%lu.%03lu] %s\n", (unsigned long)(e.ms / 1000), (unsigned long)(e.ms % 1000), e.msg);
}

/* ================= Small helpers ================= */
static void copyClean(char* dst, size_t n, String s) {
  s.trim();
  size_t j = 0;
  for (size_t i = 0; i < s.length() && j < n - 1; i++) {
    uint8_t c = (uint8_t)s[i];
    if (c < 32 || c == 127) continue;
    dst[j++] = (char)c;
  }
  dst[j] = 0;
}

// text for SMS template variables: ASCII only, safe punctuation
static void cleanVar(char* dst, size_t n, const String& s) {
  size_t j = 0;
  for (size_t i = 0; i < s.length() && j < n - 1; i++) {
    char c = s[i];
    if (isalnum((uint8_t)c) || (c && strchr(" .,-_()#:/@&+", c))) dst[j++] = c;
  }
  while (j > 0 && dst[j - 1] == ' ') j--;
  dst[j] = 0;
}

static String jesc(const char* s) {
  String o; o.reserve(strlen(s) + 4);
  for (; *s; s++) {
    char c = *s;
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 32) o += ' ';
    else o += c;
  }
  return o;
}

static bool normPhone10(const String& in, char out[11]) {
  String d;
  for (size_t i = 0; i < in.length(); i++) if (isdigit((uint8_t)in[i])) d += in[i];
  if (d.length() > 10) d = d.substring(d.length() - 10);
  if (d.length() != 10) return false;
  strlcpy(out, d.c_str(), 11);
  return true;
}

static void fullPhone(const char* p10, char out[13]) { snprintf(out, 13, "91%s", p10); }

static void maskedPhone(const char* full, char out[16]) {
  size_t n = strlen(full);
  if (n < 6) { strlcpy(out, full, 16); return; }
  snprintf(out, 16, "%.2s******%s", full, full + n - 2);
}

static void stamp(char* out, size_t n) {
  uint32_t e = nowEpoch();
  if (!e) { out[0] = 0; return; }
  time_t t = e; struct tm tmv; localtime_r(&t, &tmv);
  strftime(out, n, "%d-%b %H:%M", &tmv);
}

static bool validId(int id) { return id >= 1 && id <= MAX_ID; }

uint8_t enrStepNow();        // 1 while a fingerprint enrollment is in its scan phase
bool voterAuthActive();      // true while an authenticated voter is choosing a candidate

static void countVoters(int& reg, int& voted) {
  reg = voted = 0;
  for (int i = 1; i <= MAX_ID; i++) if (voters[i].used) { reg++; if (voters[i].voted) voted++; }
}

/* ================= Non-blocking buzzer ================= */
const uint16_t P_OK[]   = {100, 50, 100};
const uint16_t P_ERR[]  = {200, 100, 200, 100, 200};
const uint16_t P_LONG[] = {500};
const uint16_t P_DONE[] = {100, 60, 100, 60, 100};
#define BEEP(p) beep(p, sizeof(p) / sizeof(p[0]))

const uint16_t* buzSteps = nullptr;
uint8_t  buzLen = 0, buzIdx = 0;
uint32_t buzNext = 0;

static void beep(const uint16_t* steps, uint8_t len) {
  buzSteps = steps; buzLen = len; buzIdx = 0;
  digitalWrite(BUZZER_PIN, HIGH);
  buzNext = millis() + steps[0];
}

static void buzzerTick() {
  if (!buzSteps) return;
  if ((int32_t)(millis() - buzNext) < 0) return;
  buzIdx++;
  if (buzIdx >= buzLen) { digitalWrite(BUZZER_PIN, LOW); buzSteps = nullptr; return; }
  digitalWrite(BUZZER_PIN, (buzIdx % 2 == 0) ? HIGH : LOW);
  buzNext = millis() + buzSteps[buzIdx];
}

/* ================= Persistent storage (LittleFS) =================
 *  /cfg.bin     settings
 *  /tally.bin   vote totals
 *  /voters.bin  fixed-size array of Voter records (one slot per sensor ID)
 * Each file starts with a 4-byte magic that encodes the struct size, so a
 * layout change is detected and the file is ignored instead of misread. */
static bool fsReady = false;

static uint32_t magicFor(size_t sz) { return 0x56540000UL | (uint32_t)sz; }

static bool loadBlob(const char* path, void* buf, size_t n) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  uint32_t m = 0;
  bool ok = f.read((uint8_t*)&m, 4) == 4 && m == magicFor(n) && f.read((uint8_t*)buf, n) == (int)n;
  f.close();
  return ok;
}

static bool saveBlob(const char* path, const void* buf, size_t n) {
  File f = LittleFS.open(path, "w");
  if (!f) return false;
  uint32_t m = magicFor(n);
  bool ok = f.write((const uint8_t*)&m, 4) == 4 && f.write((const uint8_t*)buf, n) == n;
  f.close();
  if (!ok) evlog(EV_ERR, "Flash write failed: %s", path);
  return ok;
}

static void setDefaults() {
  memset(&cfg, 0, sizeof(cfg));
  strlcpy(cfg.cand[0], "Candidate A", 21); strlcpy(cfg.cand[1], "Candidate B", 21);
  strlcpy(cfg.cand[2], "Candidate C", 21); strlcpy(cfg.cand[3], "Candidate D", 21);
  strlcpy(cfg.adminPhone, DEFAULT_ADMIN_PHONE, 13);
  cfg.sms = cfg.smsVoter = cfg.smsAdmin = cfg.smsReg = 1;
  cfg.adminSeesCand = 0;
  cfg.electionOpen = 0; cfg.securityLevel = 2; cfg.minConfidence = 40; cfg.voteTimeoutSec = 30;
}

static void saveCfg()   { saveBlob("/cfg.bin", &cfg, sizeof(cfg)); revCfg++; }
static void saveTally() { saveBlob("/tally.bin", votes, sizeof(votes)); }
static void saveAllVoters() { saveBlob("/voters.bin", voters, sizeof(voters)); revVoters++; revFp++; }

static void saveVoter(int id) {            // writes just one record
  File f = LittleFS.open("/voters.bin", "r+");
  if (f && f.size() == 4 + sizeof(voters) && f.seek(4 + (size_t)id * sizeof(Voter))) {
    bool ok = f.write((const uint8_t*)&voters[id], sizeof(Voter)) == sizeof(Voter);
    f.close();
    if (ok) { revVoters++; revFp++; return; }
  } else if (f) f.close();
  saveAllVoters();
}

static void storageBegin() {
  setDefaults();
  memset(voters, 0, sizeof(voters));
  memset(votes, 0, sizeof(votes));
  fsReady = LittleFS.begin();
  if (!fsReady) { LittleFS.format(); fsReady = LittleFS.begin(); }
  if (!fsReady) { evlog(EV_ERR, "LittleFS failed - data will NOT persist"); return; }
  if (!loadBlob("/cfg.bin", &cfg, sizeof(cfg))) { setDefaults(); saveCfg(); evlog(EV_INFO, "Settings initialised"); }
  if (!loadBlob("/tally.bin", votes, sizeof(votes))) { memset(votes, 0, sizeof(votes)); saveTally(); }
  if (!loadBlob("/voters.bin", voters, sizeof(voters))) { memset(voters, 0, sizeof(voters)); saveAllVoters(); }
  // sanitise anything loaded from flash
  for (int i = 0; i <= MAX_ID; i++) {
    voters[i].name[sizeof(voters[i].name) - 1] = 0;
    voters[i].address[sizeof(voters[i].address) - 1] = 0;
    voters[i].phone[sizeof(voters[i].phone) - 1] = 0;
  }
  voters[0].used = 0;
  cfg.adminPhone[sizeof(cfg.adminPhone) - 1] = 0;
  for (int i = 0; i < NUM_CAND; i++) cfg.cand[i][20] = 0;
  int reg, v; countVoters(reg, v);
  unsigned long total = votes[0] + votes[1] + votes[2] + votes[3];
  evlog(EV_INFO, "Loaded %d voters (%d voted), %lu votes", reg, v, total);
  if (total != (unsigned long)v) evlog(EV_WARN, "Tally mismatch: %lu votes vs %d voted flags", total, v);
}

/* ================= SMS queue + log ================= */
#define SMS_N 8
SmsJob sms[SMS_N];
uint32_t smsSeq = 0, smsSentCount = 0, smsFailCount = 0, lastSmsSendMs = 0;
char smsLastResp[96] = "";

static bool smsPending(const SmsJob& j) { return j.status == SMS_QUEUED || j.status == SMS_RETRY; }

static int smsPendingCount() { int n = 0; for (int i = 0; i < SMS_N; i++) if (smsPending(sms[i])) n++; return n; }

static SmsJob* smsAlloc() {
  for (int i = 0; i < SMS_N; i++) if (sms[i].status == SMS_FREE) return &sms[i];
  SmsJob* oldest = nullptr;                       // recycle the oldest finished entry
  for (int i = 0; i < SMS_N; i++)
    if (!smsPending(sms[i]) && (!oldest || sms[i].id < oldest->id)) oldest = &sms[i];
  return oldest;
}

static bool smsEnqueue(const char* kind, const char* phone, const char* tpl, const String& v1, const String& v2) {
  SmsJob* j = smsAlloc();
  if (!j) { evlog(EV_ERR, "SMS queue full - %s message dropped", kind); return false; }
  memset(j, 0, sizeof(*j));
  j->id = ++smsSeq; j->epoch = nowEpoch(); j->ms = millis(); j->nextMs = millis();
  strlcpy(j->phone, phone, sizeof(j->phone)); strlcpy(j->tpl, tpl, sizeof(j->tpl)); strlcpy(j->kind, kind, sizeof(j->kind));
  cleanVar(j->var1, SMS_VAR_MAX + 1, v1); cleanVar(j->var2, SMS_VAR_MAX + 1, v2);   // API accepts at most 30 chars
  j->status = cfg.sms ? SMS_QUEUED : SMS_SKIPPED;
  revSms++;
  if (!cfg.sms) evlog(EV_INFO, "SMS disabled - %s message not sent", kind);
  return true;
}

static void smsOnRegister(int id) {
  char ph[13]; fullPhone(voters[id].phone, ph);
  smsEnqueue("VOTER", ph, TPL_VOTER, "Voter registration", String(BOOTH_LABEL) + " ID " + id);
}

static void smsOnVote(int id, int cand) {
  int reg, voted; countVoters(reg, voted);
  char ts[16]; stamp(ts, sizeof(ts));
  if (cfg.smsVoter) {
    char ph[13]; fullPhone(voters[id].phone, ph);
    String v2 = BOOTH_LABEL; if (ts[0]) { v2 += " "; v2 += ts; }
    smsEnqueue("VOTER", ph, TPL_VOTER, "Your vote", v2);
  }
  if (cfg.smsAdmin && cfg.adminPhone[0]) {
    String v1 = String("voter ") + voters[id].name + " #" + id;
    String v2 = "VOTED";
    if (cfg.adminSeesCand) { v2 += " for "; v2 += cfg.cand[cand]; }
    v2 += " (" + String(voted) + "/" + String(reg) + ")";
    smsEnqueue("ADMIN", cfg.adminPhone, TPL_ADMIN, v1, v2);
  }
}

static uint32_t lastAlertMs[2] = {0, 0};
static void smsAlert(uint8_t slot, const char* what) {
  if (lastAlertMs[slot] && millis() - lastAlertMs[slot] < ALERT_COOLDOWN_MS) return;
  lastAlertMs[slot] = millis() ? millis() : 1;
  if (cfg.adminPhone[0]) smsEnqueue("ALERT", cfg.adminPhone, TPL_ERROR, what, BOOTH_LABEL);
}

static void smsAfterAttempt(SmsJob& j, int code, const char* resp, bool ok, bool fatal) {
  j.http = (int16_t)code; j.attempts++;
  strlcpy(smsLastResp, resp, sizeof(smsLastResp));
  for (char* p = smsLastResp; *p; p++) if ((uint8_t)*p < 32) *p = ' ';
  lastSmsSendMs = millis();
  if (ok) {
    j.status = SMS_SENT; smsSentCount++;
    evlog(EV_OK, "SMS sent (%s, try %u)", j.kind, j.attempts);
  } else if (fatal || j.attempts >= SMS_MAX_ATTEMPTS) {
    j.status = SMS_FAILED; smsFailCount++;
    evlog(EV_ERR, "SMS %s failed (HTTP %d)", j.kind, code);
  } else {
    j.status = SMS_RETRY; j.nextMs = millis() + 2000UL * j.attempts;
    evlog(EV_WARN, "SMS %s try %u failed (HTTP %d), retrying", j.kind, j.attempts, code);
  }
  revSms++;
}

static void smsSend(SmsJob& j) {
  // local conditions: wait without burning one of the retry attempts
  bool lowHeap = ESP.getFreeHeap() < MIN_HEAP_FOR_TLS || ESP.getMaxFreeBlockSize() < MIN_BLOCK_FOR_TLS;
  if (WiFi.status() != WL_CONNECTED || lowHeap) {
    j.nextMs = millis() + 3000;
    static uint32_t lastWarn = 0;
    if (lowHeap && millis() - lastWarn > 60000UL) { lastWarn = millis(); evlog(EV_WARN, "SMS waiting: low memory (%u free, %u largest block)", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize()); }
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();                          // no certificate pinning (see docs)
  client.setBufferSizes(TLS_RX_BUF, TLS_TX_BUF);
  HTTPClient http;
  http.setTimeout(SMS_HTTP_TIMEOUT);
  String url = String("https://www.circuitdigest.cloud/api/v1/send_sms?ID=") + j.tpl;
  if (!http.begin(client, url)) { smsAfterAttempt(j, 0, "http.begin failed", false, false); return; }
  http.addHeader("Authorization", SMS_API_KEY);
  http.addHeader("Content-Type", "application/json");
  String body = String("{\"mobiles\":\"") + j.phone + "\",\"var1\":\"" + j.var1 + "\",\"var2\":\"" + j.var2 + "\"}";
  int code = http.POST(body);
  String resp;
  if (code > 0) resp = http.getString();
  else {                                         // transport failure: add the TLS error to the log
    char se[80] = ""; int e = client.getLastSSLError(se, sizeof(se));
    resp = http.errorToString(code);
    if (e) resp += String(" / TLS ") + e + " " + se;
  }
  http.end();
  bool ok = (code == 200);
  bool fatal = (code == 400 || code == 401 || code == 403 || code == 404);   // retrying cannot help
  smsAfterAttempt(j, code, resp.c_str(), ok, fatal);
}

static void smsTick() {
  if (enrStepNow() != 0 || voterAuthActive()) return;       // keep sensor steps responsive
  if (millis() - lastSmsSendMs < SMS_COOLDOWN_MS) return;
  SmsJob* pick = nullptr;
  for (int i = 0; i < SMS_N; i++)
    if (smsPending(sms[i]) && (int32_t)(millis() - sms[i].nextMs) >= 0 && (!pick || sms[i].id < pick->id)) pick = &sms[i];
  if (pick) smsSend(*pick);
}

/* ================= Fingerprint sensor ================= */
bool sensorOk = false;
uint8_t commErr = 0;
uint32_t lastSensorTry = 0;

static bool isCommErr(uint8_t r) {
  return r == FINGERPRINT_PACKETRECIEVEERR || r == FINGERPRINT_TIMEOUT || r == FINGERPRINT_BADPACKET;
}

// Raw handshake: send the "verify password" packet and see whether ANY valid reply (EF 01 ...) comes back.
// Returns 0 = silence, 1 = valid reply, 2 = valid reply but password rejected.
static bool echoSeen = false;
static uint8_t sensorProbe(uint32_t baud, bool swapped) {
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

static const uint32_t FP_BAUDS[] = {57600, 9600, 115200, 38400, 19200};
const uint8_t FP_COMBOS = 10;                       // 5 baud rates x 2 wire orientations
uint8_t fpCombo = 0; bool fpKnown = false;          // fpKnown: fpCombo holds the last working setting

static bool sensorTry(uint8_t combo) {
  uint32_t baud = FP_BAUDS[combo % 5]; bool sw = combo >= 5;
  uint8_t p = sensorProbe(baud, sw);
  if (p == 0) return false;
  if (p == 2) { evlog(EV_ERR, "Sensor answers at %lu baud but rejects password 0 (changed password?)", (unsigned long)baud); return false; }
  fpCombo = combo; fpKnown = true;
  if (!finger.verifyPassword()) return false;
  if (sw) evlog(EV_WARN, "Sensor works only with TX/RX swapped: sensor TX is on D2, sensor RX on D1");
  evlog(EV_INFO, "Sensor link: %lu baud%s", (unsigned long)baud, sw ? " (swapped)" : "");
  return true;
}

// full = try every baud/orientation (boot); otherwise the last good setting, or the next combo in turn
static bool sensorConnect(bool full) {
  static uint8_t next = 0;
  bool ok = false;
  if (full) { for (uint8_t i = 0; i < FP_COMBOS && !ok; i++) { ok = sensorTry((fpCombo + i) % FP_COMBOS); yield(); } }
  else if (fpKnown) ok = sensorTry(fpCombo);
  else { ok = sensorTry(next); next = (next + 1) % FP_COMBOS; }
  if (!ok) { sensorOk = false; return false; }
  sensorOk = true; commErr = 0;
  finger.setSecurityLevel(cfg.securityLevel);
  finger.getParameters();
  finger.getTemplateCount();
  return true;
}

static void sensorFault(const char* why) {
  if (!sensorOk) return;
  sensorOk = false; revFp++;
  evlog(EV_ERR, "Sensor lost: %s", why);
  BEEP(P_ERR);
  smsAlert(1, "fingerprint sensor offline");
}

static void sensorWatch() {
  if (sensorOk || millis() - lastSensorTry < (fpKnown ? 3000UL : 1500UL)) return;
  lastSensorTry = millis();
  if (sensorConnect(false)) {
    evlog(EV_OK, "Sensor online (capacity %u, %u templates)", finger.capacity, finger.templateCount);
    BEEP(P_OK); revFp++;
  }
}

static void noteSensorResult(uint8_t r) {
  if (isCommErr(r)) { if (++commErr >= 8) sensorFault("no response"); }
  else commErr = 0;
}

/* ---- sensor verification: which slots really hold a template? ---- */
bool verifyRun = false; uint8_t verifyPos = 1, verifyRetry = 0; uint32_t verifyLast = 0;

static void verifyStart() {
  if (!sensorOk) return;
  verifyRun = true; verifyPos = 1; verifyRetry = 0;
  memset(sensorMap, 0, sizeof(sensorMap));
  revFp++;
}

static void verifyFinish() {
  verifyRun = false;
  finger.getTemplateCount();
  int miss = 0, orph = 0;
  for (int i = 1; i <= MAX_ID; i++) {
    if (voters[i].used && sensorMap[i] == 2) miss++;
    if (!voters[i].used && sensorMap[i] == 1) orph++;
  }
  evlog(miss || orph ? EV_WARN : EV_OK, "Sensor check: %u templates, %d missing, %d orphan", finger.templateCount, miss, orph);
  revFp++;
}

/* ================= Enrollment state machine ================= */
struct Enroll {
  EnrStep step; uint8_t id, retries, phase; bool rescan;
  uint32_t seq, startedAt, t0, rmSeen, lastPoll, doneAt;
  char msg[64];
  Voter v;
} enr = {};

uint8_t enrStepNow() { return enr.step >= EN_WAIT1 && enr.step <= EN_WAIT3 ? 1 : 0; }

static void enrSet(EnrStep s, const char* m) { enr.step = s; strlcpy(enr.msg, m, sizeof(enr.msg)); enr.t0 = millis(); }

static void enrFinish(bool ok, const char* m) {
  enr.step = ok ? EN_OK : EN_FAIL; strlcpy(enr.msg, m, sizeof(enr.msg)); enr.doneAt = millis();
  if (ok) BEEP(P_DONE); else BEEP(P_ERR);
  evlog(ok ? EV_OK : EV_ERR, "Enrollment ID %u %s: %s", enr.id, ok ? "done" : "failed", m);
}

static void enrStart(int id, bool rescan) {
  enr.step = EN_WAIT1; enr.id = id; enr.rescan = rescan; enr.retries = 0; enr.phase = 0; enr.seq++;
  enr.startedAt = millis(); enr.lastPoll = 0; enr.rmSeen = 0;
  strlcpy(enr.msg, "Place finger on the sensor", sizeof(enr.msg)); enr.t0 = millis();
  BEEP(P_LONG);
  evlog(EV_INFO, "Enrollment started for ID %d (%s)", id, rescan ? "re-scan" : enr.v.name);
}

static void enrTick() {
  uint32_t now = millis();
  if (enr.step == EN_OK || enr.step == EN_FAIL) { if (now - enr.doneAt > 8000) enr.step = EN_IDLE; return; }
  if (enr.step == EN_IDLE) return;
  if (!sensorOk) { enrFinish(false, "Sensor offline"); return; }
  if (now - enr.startedAt > 120000UL) { enrFinish(false, "Timed out"); return; }
  if ((enr.step == EN_WAIT1 || enr.step == EN_WAIT2 || enr.step == EN_WAIT3) && now - enr.t0 > 40000UL) { enrFinish(false, "No finger detected"); return; }
  if (now - enr.lastPoll < 70) return;
  enr.lastPoll = now;

  uint8_t r;
  switch (enr.step) {
    case EN_WAIT1:
      r = finger.getImage();
      noteSensorResult(r);
      if (r != FINGERPRINT_OK) return;
      if ((r = finger.image2Tz(1)) != FINGERPRINT_OK) { BEEP(P_ERR); strlcpy(enr.msg, "Poor image - try again", sizeof(enr.msg)); return; }
      r = finger.fingerFastSearch();                         // is this finger already enrolled?
      if (r == FINGERPRINT_OK && !(enr.rescan && finger.fingerID == enr.id)) {
        char b[64]; snprintf(b, sizeof(b), "Finger already enrolled (ID %u)", finger.fingerID);
        enrFinish(false, b); return;
      }
      BEEP(P_OK);
      enr.phase = 0;
      enrSet(EN_REMOVE, "Lift your finger"); enr.rmSeen = 0;
      break;

    case EN_REMOVE:
      r = finger.getImage();
      if (r == FINGERPRINT_NOFINGER) {
        if (!enr.rmSeen) enr.rmSeen = now;
        else if (now - enr.rmSeen > 700) {
          BEEP(P_LONG);
          if (enr.phase == 0) enrSet(EN_WAIT2, "Place the SAME finger again");
          else enrSet(EN_WAIT3, "Place the same finger once more to verify");
        }
      } else enr.rmSeen = 0;
      break;

    case EN_WAIT2:
      r = finger.getImage();
      if (r != FINGERPRINT_OK) return;
      if (finger.image2Tz(2) != FINGERPRINT_OK) { BEEP(P_ERR); strlcpy(enr.msg, "Poor image - try again", sizeof(enr.msg)); return; }
      r = finger.createModel();
      if (r == FINGERPRINT_ENROLLMISMATCH) {
        if (++enr.retries >= 3) { enrFinish(false, "Fingers did not match"); return; }
        BEEP(P_ERR); enrSet(EN_WAIT1, "No match - start again, same finger"); return;
      }
      if (r != FINGERPRINT_OK) { char b[64]; snprintf(b, sizeof(b), "Model error (code %u)", r); enrFinish(false, b); return; }
      if (finger.storeModel(enr.id) != FINGERPRINT_OK) { enrFinish(false, "Sensor could not store template"); return; }
      sensorMap[enr.id] = 1;
      enr.phase = 1;                                         // verify the stored template with a fresh scan
      BEEP(P_OK); enrSet(EN_REMOVE, "Saved - lift your finger"); enr.rmSeen = 0;
      break;

    case EN_WAIT3:
      r = finger.getImage();
      if (r != FINGERPRINT_OK) return;
      if (finger.image2Tz(1) != FINGERPRINT_OK) { BEEP(P_ERR); strlcpy(enr.msg, "Poor image - place finger again", sizeof(enr.msg)); return; }
      r = finger.fingerFastSearch();
      if (r == FINGERPRINT_OK && finger.fingerID == enr.id && finger.confidence >= cfg.minConfidence) {
        finger.getTemplateCount();
        if (!enr.rescan) {                                   // commit the voter record
          voters[enr.id] = enr.v;
          voters[enr.id].used = 1; voters[enr.id].voted = 0; voters[enr.id].votedAt = 0;
        }
        saveVoter(enr.id);
        evlog(EV_OK, "%s: %s (ID %u, verify conf %u)", enr.rescan ? "Finger updated" : "Voter registered", voters[enr.id].name, enr.id, finger.confidence);
        if (!enr.rescan && cfg.smsReg) smsOnRegister(enr.id);
        enrFinish(true, enr.rescan ? "Fingerprint updated" : "Voter enrolled");
      } else {                                               // weak template: drop it and scan again
        finger.deleteModel(enr.id); sensorMap[enr.id] = 2;
        finger.getTemplateCount();
        if (++enr.retries >= 3) { enrFinish(false, "Could not get a reliable scan - clean finger/sensor"); return; }
        BEEP(P_ERR); enr.phase = 0; enrSet(EN_WAIT1, "Verification failed - scan again, press firmly and flat");
      }
      break;

    default: break;
  }
}

/* ================= 16x2 LCD (I2C backpack, own minimal driver) =================
 * PCF8574 bits: P0=RS P1=RW P2=EN P3=Backlight P4..P7=D4..D7. Only changed lines are rewritten. */
const uint8_t LCD_SDA = 0, LCD_SCL = 2;             // D3, D4 (D1/D2 belong to the fingerprint sensor)
uint8_t lcdAddr = 0;
char lcdCur[2][17] = {"", ""};
char lcdFlashBuf[2][17] = {"", ""};
uint32_t lcdFlashUntil = 0, lcdLastTick = 0;

static void lcdNib(uint8_t n, uint8_t rs) {
  uint8_t v = (n << 4) | 0x08 | rs;
  Wire.beginTransmission(lcdAddr); Wire.write(v | 4); Wire.write(v); Wire.endTransmission();
}
static void lcdByte(uint8_t b, uint8_t rs) {
  uint8_t hi = (b & 0xF0) | 0x08 | rs, lo = ((b << 4) & 0xF0) | 0x08 | rs;
  Wire.beginTransmission(lcdAddr);
  Wire.write(hi | 4); Wire.write(hi); Wire.write(lo | 4); Wire.write(lo);
  Wire.endTransmission();
}

static void lcdBegin() {
  Wire.begin(LCD_SDA, LCD_SCL);
  Wire.setClock(100000);
  const uint8_t tryAddr[] = {0x27, 0x3F, 0x26, 0x20, 0x38};
  for (uint8_t a : tryAddr) { Wire.beginTransmission(a); if (Wire.endTransmission() == 0) { lcdAddr = a; break; } }
  if (!lcdAddr) { Serial.println("LCD not found on D3/D4 (SDA/SCL) - continuing without it"); return; }
  delay(50);
  lcdNib(3, 0); delay(5); lcdNib(3, 0); delay(1); lcdNib(3, 0); delay(1); lcdNib(2, 0); delay(1);
  lcdByte(0x28, 0); lcdByte(0x0C, 0); lcdByte(0x06, 0); lcdByte(0x01, 0); delay(3);
  Serial.printf("LCD found at 0x%02X\n", lcdAddr);
}

static void lcdShow(const char* a, const char* b) {
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

// show two lines for a few seconds, overriding the normal status screen
static void lcdFlash(const char* a, const char* b, uint32_t ms) {
  strlcpy(lcdFlashBuf[0], a, 17); strlcpy(lcdFlashBuf[1], b, 17);
  lcdFlashUntil = millis() + ms; lcdLastTick = 0;
}

/* ================= Voting station ================= */
enum Station : uint8_t { ST_CLOSED = 0, ST_SCAN, ST_AUTH };
uint8_t  authId = 0;
uint32_t authStart = 0, lastScan = 0, lastButtonMs = 0, lastFailBeepMs = 0;
bool     fingerDown = false;
uint8_t  failStreak = 0;

// transient message shown on the console
char     stMsg[72] = ""; uint8_t stMsgType = 0; uint32_t stMsgUntil = 0;
// "test a finger" mode (identify without voting)
bool     identOn = false; uint32_t identUntil = 0; char identMsg[80] = "";

bool voterAuthActive() { return authId != 0; }

static void setStMsg(uint8_t type, const char* fmt, ...) {
  va_list ap; va_start(ap, fmt); vsnprintf(stMsg, sizeof(stMsg), fmt, ap); va_end(ap);
  stMsgType = type; stMsgUntil = millis() + 5000;
  char l2[17] = ""; if (strlen(stMsg) > 16) strlcpy(l2, stMsg + 16, 17);
  char l1[17]; strlcpy(l1, stMsg, 17);
  lcdFlash(l1, l2, 3000);
}

static void failBeep() {
  if (millis() - lastFailBeepMs > 1200) { lastFailBeepMs = millis(); BEEP(P_ERR); }
}

static void castVote(int cand) {
  int id = authId;
  authId = 0;
  votes[cand]++;
  voters[id].voted = 1; voters[id].votedAt = nowEpoch();
  saveVoter(id); saveTally();   // flag first: a power cut can then never allow a second vote
  BEEP(P_DONE);
  setStMsg(EV_OK, "Vote recorded for %s - thank you!", voters[id].name);
  lcdFlash("Vote recorded", "Thank you!", 4000);
  evlog(EV_OK, "VOTE cast by %s (ID %d)", voters[id].name, id);   // candidate is NOT logged (secret ballot)
  smsOnVote(id, cand);
}

static void handleFinger() {
  uint8_t t = finger.image2Tz(1);
  if (t != FINGERPRINT_OK) { failBeep(); if (!identOn) setStMsg(EV_WARN, "Poor image - lift and try again"); return; }
  uint8_t s = finger.fingerFastSearch();
  noteSensorResult(s);
  if (s == FINGERPRINT_NOTFOUND) {
    failBeep();
    if (identOn) { identOn = false; strlcpy(identMsg, "Not enrolled: no matching template", sizeof(identMsg)); return; }
    setStMsg(EV_ERR, "Fingerprint not recognised");
    evlog(EV_WARN, "Unrecognised fingerprint (%u in a row)", failStreak + 1);
    if (++failStreak >= FAIL_ALERT_COUNT) { failStreak = 0; smsAlert(0, "repeated unknown fingerprints"); evlog(EV_ERR, "Repeated unknown fingerprints - admin alerted"); }
    return;
  }
  if (s != FINGERPRINT_OK) { failBeep(); return; }

  int id = finger.fingerID, conf = finger.confidence;
  if (identOn) {
    identOn = false;
    if (validId(id) && voters[id].used)
      snprintf(identMsg, sizeof(identMsg), "Match: %s (ID %d), confidence %d, %s", voters[id].name, id, conf, voters[id].voted ? "already voted" : "not voted yet");
    else snprintf(identMsg, sizeof(identMsg), "Template %d matched but has no voter record (orphan)", id);
    BEEP(P_OK); return;
  }
  failStreak = 0;
  if (!validId(id) || conf < cfg.minConfidence) { failBeep(); setStMsg(EV_WARN, "Low confidence (%d) - try again", conf); return; }
  if (!voters[id].used) { failBeep(); setStMsg(EV_ERR, "Fingerprint has no voter record"); evlog(EV_WARN, "Orphan template matched (ID %d)", id); return; }
  if (voters[id].voted) {
    failBeep(); setStMsg(EV_ERR, "%s has already voted", voters[id].name);
    evlog(EV_WARN, "Repeat vote attempt by %s (ID %d)", voters[id].name, id);
    return;
  }
  authId = id; authStart = millis(); lastButtonMs = millis();
  BEEP(P_OK);
  stMsg[0] = 0;
  evlog(EV_INFO, "%s (ID %d) authenticated, conf %d", voters[id].name, id, conf);
}

static void buttonsTick() {
  if (millis() - lastButtonMs < BUTTON_DEBOUNCE_MS) return;
  for (int i = 0; i < NUM_CAND; i++)
    if (digitalRead(BUTTON_PINS[i]) == (i < 3 ? LOW : HIGH)) { lastButtonMs = millis(); castVote(i); return; }
}

static void stationTick() {
  uint32_t now = millis();
  if (enrStepNow() || verifyRun || !sensorOk) {
    if (authId && (enrStepNow() || !sensorOk)) authId = 0;
    return;
  }
  if (identOn && now > identUntil) { identOn = false; strlcpy(identMsg, "Test timed out", sizeof(identMsg)); }

  if (authId) {
    if (!cfg.electionOpen) { authId = 0; return; }
    if (now - authStart > (uint32_t)cfg.voteTimeoutSec * 1000UL) {
      evlog(EV_WARN, "Vote timeout for %s", voters[authId].name);
      authId = 0; BEEP(P_ERR); setStMsg(EV_WARN, "Voting time expired");
    } else buttonsTick();
    return;
  }
  if (!cfg.electionOpen && !identOn) return;
  if (now - lastScan < 120) return;
  lastScan = now;
  uint8_t r = finger.getImage();
  noteSensorResult(r);
  if (r == FINGERPRINT_NOFINGER) { fingerDown = false; return; }
  if (r != FINGERPRINT_OK || fingerDown) return;       // wait until the finger is lifted
  fingerDown = true;
  handleFinger();
}

static void verifyTick() {
  if (!verifyRun) return;
  if (!sensorOk) { verifyRun = false; return; }
  if (enrStepNow() || authId) return;
  if (millis() - verifyLast < 30) return;
  verifyLast = millis();
  uint8_t last = (finger.capacity > 0 && finger.capacity < MAX_ID) ? finger.capacity : MAX_ID;
  uint8_t r = finger.loadModel(verifyPos);
  if (isCommErr(r) && ++verifyRetry < 3) return;
  sensorMap[verifyPos] = (r == FINGERPRINT_OK) ? 1 : (isCommErr(r) ? 0 : 2);
  verifyRetry = 0;
  if (++verifyPos > last) { for (int i = last + 1; i <= MAX_ID; i++) sensorMap[i] = 2; verifyFinish(); }
}

bool apOn = false;                                       // fallback hotspot active
/* ================= LCD status screen ================= */
static void lcdTick() {
  if (!lcdAddr) return;
  uint32_t now = millis();
  if (now - lcdLastTick < 250) return;
  lcdLastTick = now;
  if ((int32_t)(lcdFlashUntil - now) > 0) { lcdShow(lcdFlashBuf[0], lcdFlashBuf[1]); return; }
  char a[24], b[24];
  if (enr.step != EN_IDLE) {
    snprintf(a, sizeof(a), "Enroll ID %u", enr.id);
    switch (enr.step) {
      case EN_WAIT1: strcpy(b, "Place finger"); break;
      case EN_REMOVE: strcpy(b, "Lift finger"); break;
      case EN_WAIT2: strcpy(b, "Same finger again"); break;
      case EN_WAIT3: strcpy(b, "Verify: place"); break;
      case EN_OK: strcpy(b, "Enrolled OK!"); break;
      default: strcpy(b, "Failed"); break;
    }
    lcdShow(a, b); return;
  }
  if (!sensorOk) { lcdShow("Sensor offline", "Check wiring"); return; }
  if (identOn || identMsg[0]) { lcdShow("Finger test", identMsg); return; }
  if (authId) {
    int left = max(0, (int)(cfg.voteTimeoutSec - (now - authStart) / 1000));
    lcdShow(voters[authId].name, (String("Press 1-4  ") + left + "s").c_str()); return;
  }
  int reg, voted; countVoters(reg, voted);
  char ip[20];                                           // what to tell the admin about the network
  if (WiFi.status() == WL_CONNECTED) strlcpy(ip, WiFi.localIP().toString().c_str(), sizeof(ip));
  else if (apOn) strlcpy(ip, WiFi.softAPIP().toString().c_str(), sizeof(ip));
  else ip[0] = 0;
  if (cfg.electionOpen) {
    bool showIp = ip[0] && ((now / 4000) % 2 == 1);      // alternate every 4 s: votes <-> address
    snprintf(b, sizeof(b), "Voted %d/%d", voted, reg);
    lcdShow("Place finger", showIp ? ip : b);
  } else if (ip[0]) {
    lcdShow(apOn && WiFi.status() != WL_CONNECTED ? "Hotspot VotingSta" : "Election closed", ip);
  } else {
    lcdShow("Election closed", "WiFi connecting.");
  }
}

/* ================= Web API helpers ================= */
const char* STATION_NAMES[] = {"closed", "scan", "auth"};
const char* ENR_NAMES[]     = {"idle", "wait1", "remove", "wait2", "wait3", "ok", "fail"};
const char* SMS_NAMES[]     = {"free", "queued", "retry", "sent", "failed", "skipped"};

static bool guard(bool post) {
  if (AUTH_ENABLED && !server.authenticate(ADMIN_USER, ADMIN_PASS)) { server.requestAuthentication(); return false; }
  if (post && !server.hasHeader("X-Req")) {           // cheap CSRF defence (forces a CORS preflight)
    server.send(403, "application/json", "{\"ok\":0,\"msg\":\"Forbidden\"}"); return false;
  }
  return true;
}

static void reply(int code, bool ok, const String& msg) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", String("{\"ok\":") + (ok ? 1 : 0) + ",\"msg\":\"" + jesc(msg.c_str()) + "\"}");
}

struct JsonOut {                                        // chunked response writer
  String b;
  void begin() {
    server.sendHeader("Cache-Control", "no-store");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json", "");
    b.reserve(1200);
  }
  void add(const String& s) { b += s; if (b.length() > 1000) { server.sendContent(b); b = ""; } }
  void end() { if (b.length()) server.sendContent(b); server.sendContent(""); }
};

static char slotCode(int id) {
  uint8_t s = sensorMap[id];
  if (voters[id].used) return s == 1 ? 'O' : s == 2 ? 'M' : 'U';
  return s == 1 ? 'X' : '.';
}

static void clearStationMsgIfExpired() {
  if (stMsg[0] && (int32_t)(millis() - stMsgUntil) > 0) stMsg[0] = 0;
}

/* ================= Web handlers ================= */
static void hIndex() { if (!guard(false)) return; server.send_P(200, "text/html", INDEX_HTML); }

static void hState() {
  if (!guard(false)) return;
  clearStationMsgIfExpired();
  int reg, voted; countVoters(reg, voted);
  FSInfo fs = {}; if (fsReady) LittleFS.info(fs);
  String j; j.reserve(1500);
  j += "{\"up\":" + String(millis()) + ",\"ep\":" + String(nowEpoch());
  j += ",\"heap\":" + String(ESP.getFreeHeap()) + ",\"blk\":" + String(ESP.getMaxFreeBlockSize()) + ",\"frag\":" + String(ESP.getHeapFragmentation());
  j += ",\"rssi\":" + String(WiFi.RSSI()) + ",\"wifi\":" + String(WiFi.status() == WL_CONNECTED ? 1 : 0);
  j += ",\"ip\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString()) + "\"";
  j += ",\"fsUsed\":" + String((unsigned long)fs.usedBytes) + ",\"fsTotal\":" + String((unsigned long)fs.totalBytes);
  j += ",\"sensor\":{\"ok\":" + String(sensorOk ? 1 : 0) + ",\"cap\":" + String(finger.capacity) + ",\"cnt\":" + String(finger.templateCount) + ",\"sec\":" + String(cfg.securityLevel) + "}";
  j += ",\"open\":" + String(cfg.electionOpen ? 1 : 0) + ",\"cand\":[";
  for (int i = 0; i < NUM_CAND; i++) { if (i) j += ","; j += "\"" + jesc(cfg.cand[i]) + "\""; }
  j += "],\"votes\":[";
  for (int i = 0; i < NUM_CAND; i++) { if (i) j += ","; j += String((unsigned long)votes[i]); }
  j += "],\"reg\":" + String(reg) + ",\"voted\":" + String(voted);
  uint8_t st = authId ? ST_AUTH : (cfg.electionOpen ? ST_SCAN : ST_CLOSED);
  int left = authId ? max(0, (int)(cfg.voteTimeoutSec - (millis() - authStart) / 1000)) : 0;
  j += ",\"st\":{\"s\":\"" + String(STATION_NAMES[st]) + "\",\"id\":" + String(authId) + ",\"name\":\"" + (authId ? jesc(voters[authId].name) : String("")) +
       "\",\"left\":" + String(left) + ",\"msg\":\"" + jesc(stMsg) + "\",\"mt\":\"" + String(stMsgType == EV_OK ? "ok" : stMsgType == EV_ERR ? "err" : "warn") + "\"}";
  j += ",\"en\":{\"s\":\"" + String(ENR_NAMES[enr.step]) + "\",\"id\":" + String(enr.id) + ",\"msg\":\"" + jesc(enr.msg) + "\",\"try\":" + String(enr.retries) +
       ",\"seq\":" + String((unsigned long)enr.seq) + ",\"resc\":" + String(enr.rescan ? 1 : 0) + ",\"ph\":" + String(enr.phase) + "}";
  j += ",\"idn\":{\"on\":" + String(identOn ? 1 : 0) + ",\"msg\":\"" + jesc(identMsg) + "\"}";
  j += ",\"ver\":{\"run\":" + String(verifyRun ? 1 : 0) + ",\"pos\":" + String(verifyPos) + ",\"max\":" + String(MAX_ID) + "}";
  j += ",\"sms\":{\"pend\":" + String(smsPendingCount()) + ",\"sent\":" + String((unsigned long)smsSentCount) + ",\"fail\":" + String((unsigned long)smsFailCount) +
       ",\"last\":\"" + jesc(smsLastResp) + "\"}";
  j += ",\"rev\":{\"v\":" + String((unsigned long)revVoters) + ",\"s\":" + String((unsigned long)revSms) + ",\"l\":" + String((unsigned long)evTotal) +
       ",\"f\":" + String((unsigned long)revFp) + ",\"c\":" + String((unsigned long)revCfg) + "}}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

static void hSettingsGet() {
  if (!guard(false)) return;
  String j = "{\"cand\":[";
  for (int i = 0; i < NUM_CAND; i++) { if (i) j += ","; j += "\"" + jesc(cfg.cand[i]) + "\""; }
  j += "],\"admin\":\"" + jesc(cfg.adminPhone) + "\",\"sms\":" + cfg.sms + ",\"smsV\":" + cfg.smsVoter + ",\"smsA\":" + cfg.smsAdmin +
       ",\"smsR\":" + cfg.smsReg + ",\"seeC\":" + cfg.adminSeesCand + ",\"vt\":" + cfg.voteTimeoutSec + ",\"mc\":" + cfg.minConfidence +
       ",\"sec\":" + cfg.securityLevel + "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

static void hSettingsPost() {
  if (!guard(true)) return;
  char tmp[NUM_CAND][21];
  for (int i = 0; i < NUM_CAND; i++) {
    copyClean(tmp[i], 21, server.arg(String("c") + i));
    if (strlen(tmp[i]) < 1) return reply(400, false, "Candidate names cannot be empty");
  }
  int vt = server.arg("vt").toInt(), mc = server.arg("mc").toInt(), sec = server.arg("sec").toInt();
  if (vt < 10 || vt > 120) return reply(400, false, "Vote timeout must be 10-120 s");
  if (mc < 10 || mc > 200) return reply(400, false, "Confidence must be 10-200");
  if (sec < 1 || sec > 5)  return reply(400, false, "Security level must be 1-5");
  String ap = server.arg("admin"); ap.trim();
  char phone12[13] = "";
  if (ap.length()) {
    char p10[11];
    if (!normPhone10(ap, p10)) return reply(400, false, "Admin phone must be 10 digits");
    fullPhone(p10, phone12);
  }
  memcpy(cfg.cand, tmp, sizeof(tmp));
  strlcpy(cfg.adminPhone, phone12, sizeof(cfg.adminPhone));
  cfg.voteTimeoutSec = vt; cfg.minConfidence = mc;
  cfg.sms = server.arg("sms") == "1"; cfg.smsVoter = server.arg("smsV") == "1"; cfg.smsAdmin = server.arg("smsA") == "1";
  cfg.smsReg = server.arg("smsR") == "1"; cfg.adminSeesCand = server.arg("seeC") == "1";
  if (sec != cfg.securityLevel) { cfg.securityLevel = sec; if (sensorOk) finger.setSecurityLevel(sec); }
  saveCfg();
  evlog(EV_INFO, "Settings updated");
  reply(200, true, "Settings saved");
}

static void hElection() {
  if (!guard(true)) return;
  bool open = server.arg("open") == "1";
  if (open && !sensorOk) return reply(503, false, "Fingerprint sensor is offline");
  cfg.electionOpen = open; saveCfg();
  if (!open) authId = 0;
  evlog(EV_INFO, "Election %s", open ? "OPENED" : "CLOSED");
  BEEP(P_OK);
  reply(200, true, open ? "Election opened" : "Election closed");
}

static void hEnroll() {
  if (!guard(true)) return;
  if (!sensorOk) return reply(503, false, "Fingerprint sensor is offline");
  if (enrStepNow()) return reply(409, false, "An enrollment is already running");
  if (authId) return reply(409, false, "A voter is voting right now");
  int id = server.arg("id").toInt();
  bool rescan = server.arg("rescan") == "1";
  if (!validId(id)) return reply(400, false, "Voter ID must be 1-40");
  if (rescan) {
    if (!voters[id].used) return reply(404, false, "No voter with that ID");
  } else {
    if (voters[id].used) return reply(409, false, String("ID ") + id + " is already used by " + voters[id].name);
    Voter v; memset(&v, 0, sizeof(v));
    copyClean(v.name, sizeof(v.name), server.arg("name"));
    copyClean(v.address, sizeof(v.address), server.arg("address"));
    int age = server.arg("age").toInt();
    if (strlen(v.name) < 2) return reply(400, false, "Name is too short");
    if (strlen(v.address) < 3) return reply(400, false, "Address is too short");
    if (age < 18 || age > 120) return reply(400, false, "Voter must be 18 or older");
    if (!normPhone10(server.arg("phone"), v.phone)) return reply(400, false, "Phone must be 10 digits");
    v.age = age;
    enr.v = v;
  }
  identOn = false; verifyRun = false;
  enrStart(id, rescan);
  reply(200, true, "Enrollment started - follow the instructions on screen");
}

static void hEnrollCancel() {
  if (!guard(true)) return;
  if (enrStepNow()) { enrFinish(false, "Cancelled"); }
  reply(200, true, "Cancelled");
}

static void hVoterUpdate() {
  if (!guard(true)) return;
  int id = server.arg("id").toInt();
  if (!validId(id) || !voters[id].used) return reply(404, false, "No such voter");
  Voter v = voters[id];
  copyClean(v.name, sizeof(v.name), server.arg("name"));
  copyClean(v.address, sizeof(v.address), server.arg("address"));
  int age = server.arg("age").toInt();
  if (strlen(v.name) < 2 || strlen(v.address) < 3) return reply(400, false, "Name or address too short");
  if (age < 18 || age > 120) return reply(400, false, "Voter must be 18 or older");
  if (!normPhone10(server.arg("phone"), v.phone)) return reply(400, false, "Phone must be 10 digits");
  v.age = age;
  voters[id] = v; saveVoter(id);
  evlog(EV_INFO, "Voter %d updated", id);
  reply(200, true, "Voter updated");
}

static void hVoterDelete() {
  if (!guard(true)) return;
  int id = server.arg("id").toInt();
  if (!validId(id) || !voters[id].used) return reply(404, false, "No such voter");
  if (voters[id].voted) return reply(409, false, "This voter has already voted. Reset the votes first.");
  if (!sensorOk) return reply(503, false, "Sensor offline - cannot delete the fingerprint");
  if (enrStepNow() || authId) return reply(409, false, "Busy - try again in a moment");
  uint8_t r = finger.deleteModel(id);
  if (r != FINGERPRINT_OK && sensorMap[id] != 2) return reply(500, false, String("Sensor refused to delete (code ") + r + ")");
  evlog(EV_INFO, "Voter %s (ID %d) deleted", voters[id].name, id);
  memset(&voters[id], 0, sizeof(Voter)); saveVoter(id);
  sensorMap[id] = 2; finger.getTemplateCount();
  reply(200, true, "Voter deleted");
}

static void hVotesReset() {
  if (!guard(true)) return;
  memset(votes, 0, sizeof(votes)); saveTally();
  for (int i = 1; i <= MAX_ID; i++) { voters[i].voted = 0; voters[i].votedAt = 0; }
  saveAllVoters(); authId = 0;
  evlog(EV_WARN, "ALL VOTES RESET by admin");
  reply(200, true, "All votes cleared; voters can vote again");
}

static void hFactory() {
  if (!guard(true)) return;
  if (server.arg("confirm") != "ERASE") return reply(400, false, "Confirmation missing");
  if (!sensorOk) return reply(503, false, "Sensor offline - cannot clear templates");
  if (finger.emptyDatabase() != FINGERPRINT_OK) return reply(500, false, "Sensor refused to clear templates");
  memset(voters, 0, sizeof(voters)); memset(votes, 0, sizeof(votes)); memset(sensorMap, 2, sizeof(sensorMap));
  cfg.electionOpen = 0; authId = 0; saveCfg(); saveTally(); saveAllVoters();
  finger.getTemplateCount();
  evlog(EV_WARN, "ALL DATA ERASED by admin");
  reply(200, true, "All voters, votes and templates erased");
}

static void hVoters() {
  if (!guard(false)) return;
  JsonOut o; o.begin(); o.add("[");
  bool first = true;
  for (int i = 1; i <= MAX_ID; i++) {
    if (!voters[i].used) continue;
    const Voter& v = voters[i];
    o.add(String(first ? "" : ",") + "{\"id\":" + i + ",\"n\":\"" + jesc(v.name) + "\",\"a\":" + v.age + ",\"p\":\"" + jesc(v.phone) +
          "\",\"ad\":\"" + jesc(v.address) + "\",\"v\":" + v.voted + ",\"vt\":" + String((unsigned long)v.votedAt) + ",\"fp\":\"" + slotCode(i) + "\"}");
    first = false;
  }
  o.add("]"); o.end();
}

static void hFp() {
  if (!guard(false)) return;
  int miss = 0, orph = 0; String slots; slots.reserve(MAX_ID);
  for (int i = 1; i <= MAX_ID; i++) {
    char c = slotCode(i); slots += c;
    if (c == 'M') miss++; else if (c == 'X') orph++;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", String("{\"slots\":\"") + slots + "\",\"cnt\":" + finger.templateCount + ",\"cap\":" + finger.capacity +
              ",\"ok\":" + (sensorOk ? 1 : 0) + ",\"sec\":" + cfg.securityLevel + ",\"miss\":" + miss + ",\"orph\":" + orph + "}");
}

static void hFpVerify() {
  if (!guard(true)) return;
  if (!sensorOk) return reply(503, false, "Sensor offline");
  if (enrStepNow() || authId) return reply(409, false, "Busy - try again in a moment");
  verifyStart();
  reply(200, true, "Verifying sensor templates...");
}

static void hFpIdentify() {
  if (!guard(true)) return;
  if (!sensorOk) return reply(503, false, "Sensor offline");
  if (enrStepNow() || authId) return reply(409, false, "Busy - try again in a moment");
  identOn = true; identUntil = millis() + 20000UL; strlcpy(identMsg, "Place a finger on the sensor...", sizeof(identMsg));
  reply(200, true, "Test mode on for 20 s");
}

static void hFpDelete() {
  if (!guard(true)) return;
  int id = server.arg("id").toInt();
  if (!validId(id)) return reply(400, false, "Bad ID");
  if (voters[id].used) return reply(409, false, "Slot belongs to a voter - delete the voter instead");
  if (!sensorOk) return reply(503, false, "Sensor offline");
  if (finger.deleteModel(id) != FINGERPRINT_OK) return reply(500, false, "Sensor refused to delete");
  sensorMap[id] = 2; finger.getTemplateCount(); revFp++;
  evlog(EV_INFO, "Orphan template %d deleted", id);
  reply(200, true, "Template deleted");
}

static void hFpClear() {
  if (!guard(true)) return;
  if (!sensorOk) return reply(503, false, "Sensor offline");
  if (enrStepNow() || authId) return reply(409, false, "Busy - try again in a moment");
  if (finger.emptyDatabase() != FINGERPRINT_OK) return reply(500, false, "Sensor refused");
  memset(sensorMap, 2, sizeof(sensorMap)); finger.getTemplateCount(); revFp++; revVoters++;
  evlog(EV_WARN, "All sensor templates cleared");
  reply(200, true, "All templates cleared from the sensor");
}

static void hLog() {
  if (!guard(false)) return;
  JsonOut o; o.begin(); o.add("[");
  uint32_t n = evTotal < EV_N ? evTotal : EV_N;
  for (uint32_t k = 0; k < n; k++) {
    const Event& e = events[(evTotal - 1 - k) % EV_N];
    o.add(String(k ? "," : "") + "{\"i\":" + e.id + ",\"t\":" + String((unsigned long)e.epoch) + ",\"ms\":" + String((unsigned long)e.ms) + ",\"y\":" + e.type + ",\"m\":\"" + jesc(e.msg) + "\"}");
  }
  o.add("]"); o.end();
}

static void hSms() {
  if (!guard(false)) return;
  JsonOut o; o.begin(); o.add("[");
  uint32_t below = 0xFFFFFFFFUL; bool first = true;
  for (int k = 0; k < SMS_N; k++) {                      // newest first
    SmsJob* best = nullptr;
    for (int i = 0; i < SMS_N; i++) if (sms[i].status != SMS_FREE && sms[i].id < below && (!best || sms[i].id > best->id)) best = &sms[i];
    if (!best) break;
    below = best->id;
    char mp[16]; maskedPhone(best->phone, mp);
    o.add(String(first ? "" : ",") + "{\"i\":" + best->id + ",\"t\":" + String((unsigned long)best->epoch) + ",\"ms\":" + String((unsigned long)best->ms) +
          ",\"k\":\"" + best->kind + "\",\"to\":\"" + mp + "\",\"tp\":\"" + best->tpl + "\",\"v1\":\"" + jesc(best->var1) + "\",\"v2\":\"" + jesc(best->var2) +
          "\",\"s\":\"" + SMS_NAMES[best->status] + "\",\"a\":" + best->attempts + ",\"h\":" + best->http + "}");
    first = false;
  }
  o.add("]"); o.end();
}

static void hSmsTest() {
  if (!guard(true)) return;
  if (!cfg.adminPhone[0]) return reply(400, false, "Set an admin phone in Settings first");
  char ts[16]; stamp(ts, sizeof(ts));
  smsEnqueue("TEST", cfg.adminPhone, TPL_ADMIN, "SMS test", String("OK ") + ts);
  reply(200, true, cfg.sms ? "Test SMS queued" : "SMS is disabled in Settings");
}

static void hSmsResend() {
  if (!guard(true)) return;
  uint32_t id = (uint32_t)server.arg("id").toInt();
  for (int i = 0; i < SMS_N; i++) {
    if (sms[i].id == id && sms[i].status != SMS_FREE) {
      if (smsPending(sms[i])) return reply(409, false, "Already queued");
      SmsJob copy = sms[i];
      if (!smsEnqueue(copy.kind, copy.phone, copy.tpl, copy.var1, copy.var2)) return reply(500, false, "Queue full");
      return reply(200, true, cfg.sms ? "Message re-queued" : "SMS is disabled in Settings");
    }
  }
  reply(404, false, "Message not found");
}

static void hSmsClear() {
  if (!guard(true)) return;
  for (int i = 0; i < SMS_N; i++) if (!smsPending(sms[i])) sms[i].status = SMS_FREE;
  revSms++;
  reply(200, true, "Finished messages cleared");
}

static String csvQ(const char* s) {             // quoted CSV field, neutralises spreadsheet formulas
  String o = "\"";
  if (s[0] && strchr("=+-@", s[0])) o += '\'';
  for (; *s; s++) { if (*s == '"') o += '"'; o += *s; }
  return o + "\"";
}

static void hExport() {
  if (!guard(false)) return;
  server.sendHeader("Content-Disposition", "attachment; filename=voters.csv");
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv", "");
  server.sendContent("id,name,age,phone,address,voted,voted_at_epoch,fingerprint\r\n");
  for (int i = 1; i <= MAX_ID; i++) {
    if (!voters[i].used) continue;
    const Voter& v = voters[i];
    server.sendContent(String(i) + "," + csvQ(v.name) + "," + v.age + "," + v.phone + "," + csvQ(v.address) + "," + (v.voted ? "yes" : "no") + "," +
                       String((unsigned long)v.votedAt) + "," + slotCode(i) + "\r\n");
  }
  server.sendContent("");
}

static uint32_t restartAt = 0;
static void hReboot() {
  if (!guard(true)) return;
  reply(200, true, "Restarting...");
  restartAt = millis() + 600;
}

static void hNotFound() { server.send(404, "text/plain", "Not found"); }

/* ================= Wi-Fi / housekeeping ================= */
bool wasConnected = false;
uint32_t wifiSince = 0;

static void wifiTick() {
  bool c = WiFi.status() == WL_CONNECTED;
  if (c && !wasConnected) {
    wasConnected = true;
    evlog(EV_OK, "WiFi connected, IP %s", WiFi.localIP().toString().c_str());
    MDNS.begin("voting");
    MDNS.addService("http", "tcp", 80);
    configTime(TZ_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");
    BEEP(P_OK);
    lcdFlash("WiFi connected", WiFi.localIP().toString().c_str(), 6000);
    if (apOn) { WiFi.softAPdisconnect(true); WiFi.mode(WIFI_STA); apOn = false; }
  } else if (!c && wasConnected) {
    wasConnected = false; wifiSince = millis();
    evlog(EV_WARN, "WiFi lost");
  }
  if (!c && !apOn && millis() - wifiSince > 25000UL) {      // fallback hotspot
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASS);
    apOn = true;
    lcdFlash("Hotspot VotingStat", WiFi.softAPIP().toString().c_str(), 6000);
    evlog(EV_WARN, "No WiFi - hotspot '%s' at %s", AP_SSID, WiFi.softAPIP().toString().c_str());
  }
}

/* ================= SETUP / LOOP ================= */
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("\n\n=== Fingerprint Voting System v3.0 ===");

  for (int i = 0; i < NUM_CAND; i++) pinMode(BUTTON_PINS[i], i < 3 ? INPUT_PULLUP : INPUT);  // GPIO15 has no internal pull-up
  pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, LOW);
  memset(sensorMap, 0, sizeof(sensorMap));
  memset(sms, 0, sizeof(sms));

  lcdBegin();
  lcdShow("Voting System", "Starting...");
  storageBegin();
  cfg.electionOpen = cfg.electionOpen ? 1 : 0;

  delay(600);                                 // let the sensor finish booting
  if (sensorConnect(true)) {
    evlog(EV_OK, "Sensor online (capacity %u, %u templates)", finger.capacity, finger.templateCount);
    verifyStart();
  } else {
    if (echoSeen) evlog(EV_ERR, "D1 and D2 hear each other (echo): wires shorted or sensor TX/RX on one pin");
    else evlog(EV_ERR, "Sensor silent at every baud/orientation - check power (3.3V/5V + GND) and D1/D2 wires");
    BEEP(P_ERR);
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  wifiSince = millis();

  server.collectHeaders("X-Req");
  server.on("/", HTTP_GET, hIndex);
  server.on("/api/state", HTTP_GET, hState);
  server.on("/api/settings", HTTP_GET, hSettingsGet);
  server.on("/api/settings", HTTP_POST, hSettingsPost);
  server.on("/api/voters", HTTP_GET, hVoters);
  server.on("/api/fp", HTTP_GET, hFp);
  server.on("/api/log", HTTP_GET, hLog);
  server.on("/api/sms", HTTP_GET, hSms);
  server.on("/api/export.csv", HTTP_GET, hExport);
  server.on("/api/election", HTTP_POST, hElection);
  server.on("/api/enroll", HTTP_POST, hEnroll);
  server.on("/api/enroll/cancel", HTTP_POST, hEnrollCancel);
  server.on("/api/voter/update", HTTP_POST, hVoterUpdate);
  server.on("/api/voter/delete", HTTP_POST, hVoterDelete);
  server.on("/api/votes/reset", HTTP_POST, hVotesReset);
  server.on("/api/factory", HTTP_POST, hFactory);
  server.on("/api/fp/verify", HTTP_POST, hFpVerify);
  server.on("/api/fp/identify", HTTP_POST, hFpIdentify);
  server.on("/api/fp/delete", HTTP_POST, hFpDelete);
  server.on("/api/fp/clear", HTTP_POST, hFpClear);
  server.on("/api/sms/test", HTTP_POST, hSmsTest);
  server.on("/api/sms/resend", HTTP_POST, hSmsResend);
  server.on("/api/sms/clear", HTTP_POST, hSmsClear);
  server.on("/api/reboot", HTTP_POST, hReboot);
  server.onNotFound(hNotFound);
  server.begin();

  evlog(EV_INFO, "System ready. Console: http://voting.local or the IP above");
  if (cfg.electionOpen) evlog(EV_INFO, "Election was OPEN before restart - resuming");
}

static void housekeeping() {
  static bool prevIdent = false; static uint32_t identEnd = 0;
  if (prevIdent && !identOn) identEnd = millis();
  prevIdent = identOn;
  if (!identOn && identMsg[0] && identEnd && millis() - identEnd > 15000UL) { identMsg[0] = 0; identEnd = 0; }
  if (restartAt && (int32_t)(millis() - restartAt) > 0) ESP.restart();
}

void loop() {
  server.handleClient();
  MDNS.update();
  buzzerTick();
  wifiTick();
  sensorWatch();
  enrTick();
  stationTick();
  verifyTick();
  lcdTick();
  smsTick();
  housekeeping();
  yield();
}
