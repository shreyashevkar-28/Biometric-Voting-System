// ============================================================================================
// Voting terminal firmware: Arduino UNO R3 + R307S optical fingerprint sensor.
// The UNO stays a dumb I/O device; the PC backend does all authentication, state and storage.
//
// Host protocol, newline-terminated, 9600 baud:
//   voting      UNO -> PC : SCAN:<slotId>     PC -> UNO : ALLOW | DENY
//               UNO -> PC : VOTE:<NAME>       PC -> UNO : ALLOW | DENY
//   enrolment   PC  -> UNO: ENROLL:<slot>     UNO -> PC : EN:READY:<slot> EN:PLACE:1 EN:LIFT EN:PLACE:2
//                                                        EN:PROC EN:STORED T:<n>:<hex> ... T:END:<bytes>:<crc16>
//                                                        or EN:ERR:<TIMEOUT|IMAGE|MISMATCH|DUPLICATE|STORE|
//                                                                  DOWNLOAD|SENSOR|SLOT_USED|BUSY>
//               PC  -> UNO: DEL:<slot> (UNO answers EN:DELETED)      PC -> UNO: CANCEL
//   verify      PC  -> UNO: VERIFY:<bytes>  UNO -> PC : VF:READY
//               PC  -> UNO: D:<n>:<hex> ...  UNO -> PC : VF:D:<n>   (the voter's template FROM THE DATABASE, 24 B/line)
//               PC  -> UNO: D:END:<crc16>    UNO -> PC : VF:PLACE   (template received + CRC ok)
//                                            UNO -> PC : VF:OK:<score> | VF:ERR:<TIMEOUT|IMAGE|NOMATCH|SENSOR|DOWNLOAD|BUSY>
//               The UNO captures a LIVE finger, loads the database template into the sensor and runs the sensor's
//               1:1 Match. No finger -> TIMEOUT; wrong finger -> NOMATCH. The PC alone decides whether to open a session.
//
// NOTE: the voting half was rebuilt from the documented SCAN/VOTE protocol. If you already have
// a working sketch, keep it and paste in only the ENROLMENT section (everything between the
// "ENROLMENT" banners) plus the three lines in loop() that call pollHost().
// Edit the PIN block to match your wiring. No String objects, no EEPROM, static buffers only.
// ============================================================================================
#include <Adafruit_Fingerprint.h>
#include <SoftwareSerial.h>

// ---------------------------------- PINS (edit to match your wiring) ------------------------
const uint8_t PIN_SENSOR_RX = 2;               // UNO RX  <- sensor TX
const uint8_t PIN_SENSOR_TX = 3;               // UNO TX  -> sensor RX
const uint8_t BTN_PINS[3]   = {4, 5, 6};       // candidate buttons, INPUT_PULLUP (pressed = LOW)
const uint8_t LED_GREEN     = 8;
const uint8_t LED_RED       = 9;
// Must match the candidate names in the backend ("VOTE:ALICE_SMITH" matches candidate "Alice Smith").
const char* const CANDIDATE_NAMES[3] = {"ALICE", "BOB", "CAROL"};

// ---------------------------------- TUNABLES ------------------------------------------------
const uint32_t HOST_BAUD      = 9600;
const uint32_t SENSOR_BAUD    = 57600;
const uint32_t FINGER_WAIT_MS = 30000;         // enrolment: time allowed per finger placement
const uint32_t SESSION_MS     = 35000;         // mirrors the backend session timeout
const uint16_t TPL_MAX        = 768;           // R307S character file size; raise only if RAM allows
const uint8_t  CHUNK_BYTES    = 24;            // 24 bytes -> 48 hex chars, keeps lines < 64 (host buffer)

SoftwareSerial sensorSerial(PIN_SENSOR_RX, PIN_SENSOR_TX);
Adafruit_Fingerprint finger(&sensorSerial);

enum State : uint8_t { ST_IDLE, ST_VOTING };
State state = ST_IDLE;
uint32_t sessionDeadline = 0;

static char    rxBuf[64];                      // must hold "D:<n>:<48 hex chars>" template lines
static uint8_t rxLen = 0;
static uint8_t tpl[TPL_MAX];                   // template staging buffer (the UNO cannot relay while the sensor streams)

// ---------------------------------- host line reader (non-blocking) -------------------------
// Returns true when a complete line is in rxBuf (NUL-terminated). Overlong lines are dropped.
static bool readHostLine() {
  static bool overflow = false;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      bool ok = !overflow && rxLen > 0;
      rxBuf[rxLen] = '\0';
      rxLen = 0;
      overflow = false;
      if (ok) return true;
      continue;
    }
    if (rxLen < sizeof(rxBuf) - 1) rxBuf[rxLen++] = c; else overflow = true;
  }
  return false;
}

static void ledsOff()   { digitalWrite(LED_GREEN, LOW); digitalWrite(LED_RED, LOW); }
static void flash(uint8_t pin, uint16_t ms) { digitalWrite(pin, HIGH); delay(ms); digitalWrite(pin, LOW); }

// Waits for ALLOW/DENY. Returns 1 = ALLOW, 0 = DENY or timeout.
static uint8_t waitVerdict(uint32_t timeoutMs) {
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (readHostLine()) {
      if (strcmp(rxBuf, "ALLOW") == 0) return 1;
      if (strcmp(rxBuf, "DENY") == 0) return 0;
    }
  }
  return 0;
}

// =================================== ENROLMENT (begin) ======================================
static uint16_t crc16(const uint8_t* d, uint16_t n) {          // CRC-16/CCITT-FALSE, same as the backend
  uint16_t crc = 0xFFFF;
  while (n--) {
    crc ^= (uint16_t)(*d++) << 8;
    for (uint8_t i = 0; i < 8; i++) crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
  }
  return crc;
}

static void enErr(const __FlashStringHelper* code) {
  Serial.print(F("EN:ERR:"));
  Serial.println(code);
  flash(LED_RED, 600);
}

// True if the host sent CANCEL while we were waiting.
static bool cancelled() {
  if (readHostLine() && strcmp(rxBuf, "CANCEL") == 0) return true;
  return false;
}

// Capture one image into char buffer `bufId` (1 or 2). 0 = ok, 1 = timeout, 2 = bad image, 3 = cancelled.
static uint8_t captureInto(uint8_t bufId) {
  uint32_t t0 = millis();
  while (millis() - t0 < FINGER_WAIT_MS) {
    if (cancelled()) return 3;
    uint8_t p = finger.getImage();
    if (p == FINGERPRINT_OK) {
      return finger.image2Tz(bufId) == FINGERPRINT_OK ? 0 : 2;
    }
    if (p != FINGERPRINT_NOFINGER && p != FINGERPRINT_IMAGEFAIL && p != FINGERPRINT_PACKETRECIEVEERR) return 2;
    delay(40);
  }
  return 1;
}

static bool waitFingerLifted() {
  uint32_t t0 = millis();
  uint8_t clear = 0;
  while (millis() - t0 < 10000) {
    if (finger.getImage() == FINGERPRINT_NOFINGER) { if (++clear >= 3) return true; } else clear = 0;
    delay(60);
  }
  return false;
}

// Blocking read of exactly n bytes from the sensor with a per-byte timeout.
static bool sensorRead(uint8_t* dst, uint16_t n, uint16_t perByteMs = 600) {
  for (uint16_t i = 0; i < n; i++) {
    uint32_t t0 = millis();
    while (!sensorSerial.available()) if (millis() - t0 > perByteMs) return false;
    dst[i] = (uint8_t)sensorSerial.read();
  }
  return true;
}

// Reads the template of `slot` out of sensor flash into tpl[]. Returns bytes read, 0 on any failure.
// Uses the documented R30x packets directly: LoadChar (via the library) then UpChar (0x08).
static uint16_t readTemplate(uint16_t slot) {
  if (finger.loadModel(slot) != FINGERPRINT_OK) return 0;      // flash slot -> char buffer 1
  static const uint8_t upChar[] = {0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x04, 0x08, 0x01, 0x00, 0x0E};
  while (sensorSerial.available()) sensorSerial.read();
  sensorSerial.write(upChar, sizeof(upChar));

  uint8_t h[9];
  uint8_t ack[3];
  if (!sensorRead(h, 9) || h[0] != 0xEF || h[1] != 0x01 || h[6] != 0x07) return 0;   // ACK packet
  if (!sensorRead(ack, 3) || ack[0] != 0x00) return 0;

  uint16_t total = 0;
  for (uint8_t pkt = 0; pkt < 16; pkt++) {                      // data packets until the end-of-data packet (PID 0x08)
    if (!sensorRead(h, 9) || h[0] != 0xEF || h[1] != 0x01) return 0;
    uint8_t pid = h[6];
    uint16_t len = ((uint16_t)h[7] << 8) | h[8];                // payload + 2 checksum bytes
    if ((pid != 0x02 && pid != 0x08) || len < 2) return 0;
    uint16_t dataLen = len - 2;
    if (total + dataLen > TPL_MAX) return 0;
    if (!sensorRead(tpl + total, dataLen)) return 0;
    uint8_t cs[2];
    if (!sensorRead(cs, 2)) return 0;
    uint16_t sum = pid + h[7] + h[8];
    for (uint16_t i = 0; i < dataLen; i++) sum += tpl[total + i];
    if (sum != (((uint16_t)cs[0] << 8) | cs[1])) return 0;
    total += dataLen;
    if (pid == 0x08) return total;
  }
  return 0;
}

static void sendTemplate(uint16_t n) {
  uint8_t idx = 0;
  for (uint16_t off = 0; off < n; off += CHUNK_BYTES, idx++) {
    Serial.print(F("T:"));
    Serial.print(idx);
    Serial.print(':');
    for (uint8_t i = 0; i < CHUNK_BYTES && off + i < n; i++) {
      uint8_t b = tpl[off + i];
      if (b < 16) Serial.print('0');
      Serial.print(b, HEX);
    }
    Serial.println();
  }
  Serial.print(F("T:END:"));
  Serial.print(n);
  Serial.print(':');
  Serial.println(crc16(tpl, n), HEX);
}

static void runEnroll(uint16_t slot) {
  ledsOff();
  if (!finger.verifyPassword()) { enErr(F("SENSOR")); return; }
  if (finger.loadModel(slot) == FINGERPRINT_OK) { enErr(F("SLOT_USED")); return; }   // never overwrite

  Serial.print(F("EN:READY:")); Serial.println(slot);
  digitalWrite(LED_GREEN, HIGH);

  Serial.println(F("EN:PLACE:1"));
  uint8_t r = captureInto(1);
  if (r == 3) { ledsOff(); return; }
  if (r) { ledsOff(); enErr(r == 1 ? F("TIMEOUT") : F("IMAGE")); return; }

  // Same finger already enrolled under another slot? Refuse: that would give one person two ballots.
  if (finger.fingerFastSearch() == FINGERPRINT_OK) { ledsOff(); enErr(F("DUPLICATE")); return; }

  Serial.println(F("EN:LIFT"));
  waitFingerLifted();
  Serial.println(F("EN:PLACE:2"));
  r = captureInto(2);
  if (r == 3) { ledsOff(); return; }
  if (r) { ledsOff(); enErr(r == 1 ? F("TIMEOUT") : F("IMAGE")); return; }

  Serial.println(F("EN:PROC"));
  if (finger.createModel() != FINGERPRINT_OK) { ledsOff(); enErr(F("MISMATCH")); return; }
  if (finger.storeModel(slot) != FINGERPRINT_OK) { ledsOff(); enErr(F("STORE")); return; }
  Serial.println(F("EN:STORED"));

  uint16_t n = readTemplate(slot);
  if (n == 0) {                                                  // keep sensor and database consistent
    finger.deleteModel(slot);
    ledsOff();
    enErr(F("DOWNLOAD"));
    return;
  }
  sendTemplate(n);
  memset(tpl, 0, sizeof(tpl));
  ledsOff();
  flash(LED_GREEN, 800);
}

// ----- Portal fingerprint verification: LIVE finger vs the template stored in the DATABASE ------------------------
static void vfErr(const __FlashStringHelper* code) {
  Serial.print(F("VF:ERR:"));
  Serial.println(code);
  memset(tpl, 0, sizeof(tpl));
  flash(LED_RED, 600);
}

static int8_t hexNib(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// Receives n template bytes from the host into tpl[]. 0 = ok, 1 = error/timeout, 2 = host cancelled.
static uint8_t recvTemplate(uint16_t n) {
  uint16_t got = 0;
  int nextIdx = 0;
  Serial.println(F("VF:READY"));
  uint32_t t0 = millis();
  while (millis() - t0 < 20000) {
    if (!readHostLine()) continue;
    t0 = millis();
    if (strcmp(rxBuf, "CANCEL") == 0) return 2;
    if (strncmp(rxBuf, "D:END:", 6) == 0) {
      if (got != n) return 1;
      return crc16(tpl, n) == (uint16_t)strtoul(rxBuf + 6, NULL, 16) ? 0 : 1;
    }
    if (strncmp(rxBuf, "D:", 2) != 0) continue;
    int idx = atoi(rxBuf + 2);
    char* p = strchr(rxBuf + 2, ':');
    if (!p || idx != nextIdx) return 1;
    for (p++; p[0] && p[1]; p += 2) {
      int8_t hi = hexNib(p[0]), lo = hexNib(p[1]);
      if (hi < 0 || lo < 0 || got >= n) return 1;
      tpl[got++] = (uint8_t)((hi << 4) | lo);
    }
    Serial.print(F("VF:D:"));
    Serial.println(idx);
    nextIdx++;
  }
  return 1;
}

// DownChar (0x09): writes `n` template bytes from RAM into sensor char buffer `bufId`, in data packets of `pl` bytes.
static bool sensorDownChar(uint8_t bufId, uint16_t n, uint16_t pl) {
  uint8_t cmd[13] = {0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x04, 0x09, bufId, 0x00, 0x00};
  uint16_t sum = 0x01 + 0x04 + 0x09 + bufId;
  cmd[11] = sum >> 8;
  cmd[12] = sum & 0xFF;
  while (sensorSerial.available()) sensorSerial.read();
  sensorSerial.write(cmd, sizeof(cmd));
  uint8_t h[9], ack[3];
  if (!sensorRead(h, 9) || h[0] != 0xEF || h[1] != 0x01 || h[6] != 0x07) return false;
  if (!sensorRead(ack, 3) || ack[0] != 0x00) return false;     // sensor ready to receive
  for (uint16_t off = 0; off < n; off += pl) {
    uint16_t len = (n - off < pl) ? (n - off) : pl;
    uint8_t pid = (off + len >= n) ? 0x08 : 0x02;                // 0x08 = last data packet
    uint16_t plen = len + 2;
    uint16_t cs = pid + (plen >> 8) + (plen & 0xFF);
    uint8_t hd[9] = {0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, pid, (uint8_t)(plen >> 8), (uint8_t)(plen & 0xFF)};
    sensorSerial.write(hd, 9);
    for (uint16_t i = 0; i < len; i++) { sensorSerial.write(tpl[off + i]); cs += tpl[off + i]; }
    sensorSerial.write((uint8_t)(cs >> 8));
    sensorSerial.write((uint8_t)(cs & 0xFF));
    delay(15);
  }
  return true;
}

// Match (0x03): compares char buffer 1 (live finger) with char buffer 2 (database template).
// Returns 1 = matched (score filled), 0 = not matched, -1 = sensor/communication error.
static int8_t sensorMatch(uint16_t* score) {
  static const uint8_t m[] = {0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x03, 0x03, 0x00, 0x07};
  delay(30);
  while (sensorSerial.available()) sensorSerial.read();
  sensorSerial.write(m, sizeof(m));
  uint8_t h[9], a[8];
  if (!sensorRead(h, 9, 2000) || h[0] != 0xEF || h[1] != 0x01 || h[6] != 0x07) return -1;
  uint16_t len = ((uint16_t)h[7] << 8) | h[8];                   // confirmation(1) [+ score(2)] + checksum(2)
  if (len < 3 || len > sizeof(a) || !sensorRead(a, len, 2000)) return -1;
  if (a[0] != 0x00) return 0;                                    // 0x08 = fingerprints do not match
  *score = (len >= 5) ? (((uint16_t)a[1] << 8) | a[2]) : 0;
  return 1;
}

static void runVerify(uint16_t n) {
  ledsOff();
  if (n < 64 || n > TPL_MAX) { vfErr(F("DOWNLOAD")); return; }
  if (!finger.verifyPassword()) { vfErr(F("SENSOR")); return; }

  uint8_t rc = recvTemplate(n);                                  // 1) database template -> RAM
  if (rc == 2) { memset(tpl, 0, sizeof(tpl)); return; }
  if (rc) { vfErr(F("DOWNLOAD")); return; }

  Serial.println(F("VF:PLACE"));                                 // 2) LIVE finger: nothing proceeds without it
  digitalWrite(LED_GREEN, HIGH);
  uint8_t r = captureInto(1);
  ledsOff();
  if (r == 3) { memset(tpl, 0, sizeof(tpl)); return; }
  if (r) { vfErr(r == 1 ? F("TIMEOUT") : F("IMAGE")); return; }

  finger.getParameters();                                        // 3) database template -> char buffer 2
  uint16_t pl = finger.packet_len;
  if (pl <= 3) pl = 32u << pl;                                   // library may hold the raw 0..3 code
  if (pl != 32 && pl != 64 && pl != 128 && pl != 256) pl = 128;
  bool sent = sensorDownChar(2, n, pl);
  memset(tpl, 0, sizeof(tpl));
  if (!sent) { vfErr(F("DOWNLOAD")); return; }

  uint16_t score = 0;                                            // 4) sensor compares buffer 1 vs buffer 2
  int8_t m = sensorMatch(&score);
  if (m < 0) { vfErr(F("SENSOR")); return; }
  if (m == 0) { vfErr(F("NOMATCH")); return; }
  Serial.print(F("VF:OK:"));
  Serial.println(score);
  flash(LED_GREEN, 400);
}

static void deleteSlot(uint16_t slot) {
  finger.deleteModel(slot);
  Serial.println(F("EN:DELETED"));
}
// =================================== ENROLMENT (end) ========================================

// Host commands accepted only while idle. Returns true if a command was consumed.
static bool pollHost() {
  if (!readHostLine()) return false;
  if (strncmp(rxBuf, "ENROLL:", 7) == 0) {
    if (state != ST_IDLE) { enErr(F("BUSY")); return true; }
    int slot = atoi(rxBuf + 7);
    if (slot < 1 || slot > 127) { enErr(F("STORE")); return true; }
    runEnroll((uint16_t)slot);
    return true;
  }
  if (strncmp(rxBuf, "VERIFY:", 7) == 0) {
    if (state != ST_IDLE) { vfErr(F("BUSY")); return true; }
    long n = atol(rxBuf + 7);                                     // template size in bytes
    if (n < 64 || n > TPL_MAX) { vfErr(F("DOWNLOAD")); return true; }
    runVerify((uint16_t)n);
    return true;
  }
  if (strncmp(rxBuf, "DEL:", 4) == 0) {
    int slot = atoi(rxBuf + 4);
    if (slot >= 1 && slot <= 127) deleteSlot((uint16_t)slot);
    return true;
  }
  return true;   // CANCEL / unknown chatter outside an enrolment is ignored
}

// ---------------------------------- voting ---------------------------------------------------
static void tryScan() {
  if (finger.getImage() != FINGERPRINT_OK) return;
  if (finger.image2Tz() != FINGERPRINT_OK) return;
  if (finger.fingerFastSearch() != FINGERPRINT_OK) { flash(LED_RED, 400); return; }
  Serial.print(F("SCAN:"));
  Serial.println(finger.fingerID);
  if (waitVerdict(3000)) {
    state = ST_VOTING;
    sessionDeadline = millis() + SESSION_MS;
    digitalWrite(LED_GREEN, HIGH);
  } else {
    flash(LED_RED, 800);
  }
}

static void tryVote() {
  if (millis() > sessionDeadline) { state = ST_IDLE; ledsOff(); return; }
  for (uint8_t i = 0; i < 3; i++) {
    if (digitalRead(BTN_PINS[i]) == LOW) {
      delay(30);
      if (digitalRead(BTN_PINS[i]) != LOW) continue;             // debounce
      Serial.print(F("VOTE:"));
      Serial.println(CANDIDATE_NAMES[i]);
      uint8_t ok = waitVerdict(3000);
      state = ST_IDLE;
      ledsOff();
      flash(ok ? LED_GREEN : LED_RED, 1200);
      while (digitalRead(BTN_PINS[i]) == LOW) delay(10);         // wait for release
      return;
    }
  }
}

void setup() {
  Serial.begin(HOST_BAUD);
  for (uint8_t i = 0; i < 3; i++) pinMode(BTN_PINS[i], INPUT_PULLUP);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  finger.begin(SENSOR_BAUD);
  if (!finger.verifyPassword()) flash(LED_RED, 1500);            // sensor not found; enrolment will report EN:ERR:SENSOR
}

void loop() {
  if (state == ST_IDLE) {
    pollHost();
    tryScan();
  } else {
    if (readHostLine() && strncmp(rxBuf, "VERIFY:", 7) == 0) vfErr(F("BUSY"));   // a ballot window is open
    tryVote();
  }
}