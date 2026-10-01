// ============================================================
//  LoRa PHY Quest - MOBILE NODE #2 (Heltec WiFi LoRa 32 V3, SX1262)
//
//  Що робить (розділ 6 гайда, "Mobile node"):
//    - передає пакети через ФІКСОВАНИЙ інтервал
//    - має свій node_id
//    - збільшує seq_num на кожному пакеті
//    - вказує run_id та config_id
//    - payload_len і tx_interval_ms контрольовані
//
//  Керування:
//    - кнопка PRG (короткий тиск): старт / стоп передачі
//    - Serial Monitor 115200, команди (закінчувати Enter):
//        help                 список команд
//        status               поточні налаштування
//        start | stop         старт / стоп передачі
//        node <0..255>        встановити node_id
//        cfg  <id>            вибрати LoRa profile (0 BENCH, 1 FAST, 2 RANGE, 3 ROBUST)
//        run  <0..65535>      задати run_id (seq_num скидається в 0)
//        newrun               run_id+1 і seq_num=0
//        len  <14..255>       повна довжина пакета в байтах (payload_len)
//        int  <100..600000>   інтервал передачі, мс (tx_interval_ms)
//
//  Плата: Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262), 868 МГц.
//  Arduino IDE: Board = "Heltec WiFi LoRa 32(V3) / Wireless shell(V3)".
//  Кнопка PRG (GPIO0) = старт/стоп. Цей вузол за замовчуванням node_id = 2.
//  УВАГА: Base теж Heltec V3, тож у Tools вибирайте ПРАВИЛЬНИЙ COM-порт для кожної плати.
//
//  ВАЖЛИВО: ніколи не передавайте без підключеної антени!
//  Бібліотеки: RadioLib, "ESP8266 and ESP32 OLED driver for SSD1306" (ThingPulse)
// ============================================================

#include <RadioLib.h>
#include <Wire.h>
#include <SPI.h>
#include "SSD1306Wire.h"
#include "lora_common.h"

// ---------- Піни Heltec WiFi LoRa 32 V3 (ті самі, що в base.ino) ----------
#define OLED_SDA   17
#define OLED_SCL   18
#define OLED_RST   21
#define VEXT_PIN   36    // LOW = живлення OLED увімкнено
#define PRG_BTN     0    // кнопка PRG
#define LORA_SCK    9
#define LORA_MISO  11
#define LORA_MOSI  10
#define LORA_NSS    8
#define LORA_DIO1  14
#define LORA_RST   12
#define LORA_BUSY  13
#define LORA_TCXO_V 1.8f

// ---------- Значення за замовчуванням (можна змінити Serial-командами) ----------
#define DEFAULT_NODE_ID          2      // Mobile #2 (TTGO має 1)
#define DEFAULT_CONFIG_ID        1      // 0 BENCH, 1 FAST, 2 RANGE, 3 ROBUST
#define DEFAULT_RUN_ID           1
#define DEFAULT_PAYLOAD_LEN      20     // повна довжина пакета, байт (мін. 14)
#define DEFAULT_TX_INTERVAL_MS   3000  // FAST + 20 байт => ToA ~28 мс => >=2.8 с для 1% duty cycle
#define AUTOSTART                true   // true = починати передачу одразу після старту (на Heltec можна керувати кнопкою PRG)

SSD1306Wire display(0x3c, OLED_SDA, OLED_SCL);
SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);

// ---------- Стан ----------
uint8_t  nodeId      = DEFAULT_NODE_ID;
uint8_t  configId    = DEFAULT_CONFIG_ID;
uint16_t runId       = DEFAULT_RUN_ID;
uint8_t  payloadLen  = DEFAULT_PAYLOAD_LEN;
uint32_t txIntervalMs = DEFAULT_TX_INTERVAL_MS;
uint32_t seqNum      = 0;
uint32_t sentCount   = 0;
uint32_t txErrCount  = 0;
bool     running     = AUTOSTART;
uint32_t lastTxMs    = 0;
uint32_t lastToaUs   = 0;
bool     displayDirty = true;
uint32_t lastDisplayMs = 0;
String   statusLine  = "Init...";

// ------------------------------------------------------------
//  Допоміжне: OLED
// ------------------------------------------------------------
void updateDisplay() {
  const LoraProfile* p = findProfile(configId);
  display.clear();
  display.drawString(0, 0,  String("MOBILE n") + nodeId + "  run " + runId + (running ? "  TX" : "  STOP"));
  display.drawString(0, 12, String("cfg ") + configId + " " + (p ? p->name : "?") +
                            " SF" + (p ? p->sf : 0) + " BW" + (p ? (int)p->bw_khz : 0));
  display.drawString(0, 24, String("len ") + payloadLen + "  int " + txIntervalMs + "ms");
  display.drawString(0, 36, String("seq ") + seqNum + "  sent " + sentCount);
  display.drawString(0, 48, statusLine);
  display.display();
  displayDirty = false;
  lastDisplayMs = millis();
}

// ------------------------------------------------------------
//  Радіо: застосувати LoRa profile (F3)
// ------------------------------------------------------------
bool applyProfile(uint8_t id) {
  const LoraProfile* p = findProfile(id);
  if (!p) return false;
  radio.standby();
  int st = radio.setFrequency(p->freq_mhz);
  if (st == RADIOLIB_ERR_NONE) st = radio.setBandwidth(p->bw_khz);
  if (st == RADIOLIB_ERR_NONE) st = radio.setSpreadingFactor(p->sf);
  if (st == RADIOLIB_ERR_NONE) st = radio.setCodingRate(p->cr);
  if (st == RADIOLIB_ERR_NONE) st = radio.setOutputPower(p->tx_dbm);
  if (st != RADIOLIB_ERR_NONE) {
    Serial.printf("# ERR applyProfile(%u) code=%d\n", id, st);
    return false;
  }
  configId = id;
  lastToaUs = (uint32_t)radio.getTimeOnAir(payloadLen);
  Serial.printf("# CFG %u %s freq=%.1f bw=%.0f sf=%u cr=4/%u tx=%ddBm toa_us=%lu\n",
                p->id, p->name, p->freq_mhz, p->bw_khz, p->sf, p->cr, p->tx_dbm,
                (unsigned long)lastToaUs);
  return true;
}

bool initRadio() {
  const LoraProfile* p = findProfile(configId);
  if (!p) p = &PROFILES[0];
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  int st = radio.begin(p->freq_mhz, p->bw_khz, p->sf, p->cr, LORA_SYNCWORD,
                       p->tx_dbm, LORA_PREAMBLE, LORA_TCXO_V);
  if (st != RADIOLIB_ERR_NONE) {
    Serial.printf("# Radio init FAIL (%d)\n", st);
    return false;
  }
  radio.setDio2AsRfSwitch(true);
  configId = p->id;
  lastToaUs = (uint32_t)radio.getTimeOnAir(payloadLen);
  return true;
}

// Попередження про duty cycle: у EU868 піддіапазон 868.0-868.6 МГц має ліміт 1%
void checkDutyCycle() {
  lastToaUs = (uint32_t)radio.getTimeOnAir(payloadLen);
  float duty = (txIntervalMs > 0) ? (lastToaUs / 1000.0f) / txIntervalMs * 100.0f : 100.0f;
  if (duty > 1.0f) {
    Serial.printf("# WARN duty cycle %.2f%% > 1%% (ToA %lu us, interval %lu ms)\n",
                  duty, (unsigned long)lastToaUs, (unsigned long)txIntervalMs);
  }
}

// ------------------------------------------------------------
//  Передача одного пакета
// ------------------------------------------------------------
void sendPacket() {
  uint8_t buf[PKT_MAX_LEN];
  LoraPacket pkt;
  pkt.magic          = PKT_MAGIC;
  pkt.node_id        = nodeId;
  pkt.config_id      = configId;
  pkt.payload_len    = payloadLen;
  pkt.run_id         = runId;
  pkt.seq_num        = seqNum;
  pkt.tx_interval_ms = txIntervalMs;
  memcpy(buf, &pkt, sizeof(pkt));
  for (size_t i = PKT_HEADER_LEN; i < payloadLen; i++) buf[i] = (uint8_t)(i & 0xFF);  // наповнювач

  uint32_t t0 = micros();
  int st = radio.transmit(buf, payloadLen);
  uint32_t airUs = micros() - t0;
  lastTxMs = millis();

  if (st == RADIOLIB_ERR_NONE) {
    sentCount++;
    statusLine = "TX ok #" + String(seqNum);
    Serial.printf("# TX node=%u run=%u seq=%lu cfg=%u len=%u toa_us=%lu measured_us=%lu\n",
                  nodeId, runId, (unsigned long)seqNum, configId, payloadLen,
                  (unsigned long)lastToaUs, (unsigned long)airUs);
  } else {
    txErrCount++;
    statusLine = "TX err " + String(st);
    Serial.printf("# TX ERROR code=%d seq=%lu\n", st, (unsigned long)seqNum);
  }
  seqNum++;            // seq росте на КОЖНІЙ спробі, тож Base бачить і втрачені пакети
  displayDirty = true;
}

// ------------------------------------------------------------
//  Serial-команди
// ------------------------------------------------------------
void printStatus() {
  const LoraProfile* p = findProfile(configId);
  Serial.printf("# STATUS node=%u run=%u cfg=%u(%s) len=%u int=%lums seq=%lu sent=%lu err=%lu running=%d toa_us=%lu\n",
                nodeId, runId, configId, p ? p->name : "?", payloadLen,
                (unsigned long)txIntervalMs, (unsigned long)seqNum,
                (unsigned long)sentCount, (unsigned long)txErrCount, running, (unsigned long)lastToaUs);
}

void printHelp() {
  Serial.println("# Commands: help | status | start | stop | node N | cfg N | run N | newrun | len N | int MS");
  Serial.print("# Profiles:");
  for (uint8_t i = 0; i < NUM_PROFILES; i++) Serial.printf(" %u=%s", PROFILES[i].id, PROFILES[i].name);
  Serial.println();
}

void startTx() {
  running = true;
  lastTxMs = millis() - txIntervalMs;   // перший пакет одразу
  statusLine = "Running";
  checkDutyCycle();
  Serial.printf("# START run=%u\n", runId);
  displayDirty = true;
}

void stopTx() {
  running = false;
  statusLine = "Stopped";
  Serial.println("# STOP");
  displayDirty = true;
}

void handleCommand(char* line) {
  char cmd[16] = {0};
  long arg = 0;
  int n = sscanf(line, "%15s %ld", cmd, &arg);
  if (n < 1) return;

  if (!strcmp(cmd, "help")) { printHelp(); }
  else if (!strcmp(cmd, "status")) { printStatus(); }
  else if (!strcmp(cmd, "start")) { startTx(); }
  else if (!strcmp(cmd, "stop"))  { stopTx(); }
  else if (!strcmp(cmd, "newrun")) { runId++; seqNum = 0; sentCount = 0; txErrCount = 0; Serial.printf("# RUN %u\n", runId); }
  else if (n == 2 && !strcmp(cmd, "node")) {
    if (arg < 0 || arg > 255) { Serial.println("# ERR node 0..255"); return; }
    nodeId = (uint8_t)arg; Serial.printf("# NODE %u\n", nodeId);
  }
  else if (n == 2 && !strcmp(cmd, "cfg")) {
    if (arg < 0 || arg > 255 || !applyProfile((uint8_t)arg)) { Serial.println("# ERR unknown cfg"); return; }
    checkDutyCycle();
  }
  else if (n == 2 && !strcmp(cmd, "run")) {
    if (arg < 0 || arg > 65535) { Serial.println("# ERR run 0..65535"); return; }
    runId = (uint16_t)arg; seqNum = 0; sentCount = 0; txErrCount = 0;
    Serial.printf("# RUN %u (seq reset)\n", runId);
  }
  else if (n == 2 && !strcmp(cmd, "len")) {
    if (arg < PKT_HEADER_LEN || arg > PKT_MAX_LEN) { Serial.printf("# ERR len %d..%d\n", PKT_HEADER_LEN, PKT_MAX_LEN); return; }
    payloadLen = (uint8_t)arg; Serial.printf("# LEN %u\n", payloadLen); checkDutyCycle();
  }
  else if (n == 2 && !strcmp(cmd, "int")) {
    if (arg < 100 || arg > 600000) { Serial.println("# ERR int 100..600000"); return; }
    txIntervalMs = (uint32_t)arg; Serial.printf("# INT %lu\n", (unsigned long)txIntervalMs); checkDutyCycle();
  }
  else { Serial.println("# ERR unknown command (help)"); }
  displayDirty = true;
}

void pollSerial() {
  static char line[64];
  static uint8_t len = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (len > 0) { line[len] = 0; handleCommand(line); len = 0; }
    } else if (len < sizeof(line) - 1) {
      line[len++] = c;
    }
  }
}

// Кнопка PRG: короткий тиск = старт/стоп
void pollButton() {
  static bool lastState = HIGH;
  static uint32_t lastChange = 0;
  bool s = digitalRead(PRG_BTN);
  if (s != lastState && millis() - lastChange > 50) {
    lastChange = millis();
    lastState = s;
    if (s == LOW) { running ? stopTx() : startTx(); }
  }
}

// ------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(PRG_BTN, INPUT_PULLUP);
  pinMode(VEXT_PIN, OUTPUT);
  digitalWrite(VEXT_PIN, LOW);            // живлення OLED
  delay(100);
  pinMode(OLED_RST, OUTPUT);
  digitalWrite(OLED_RST, HIGH); delay(10);
  digitalWrite(OLED_RST, LOW);  delay(20);
  digitalWrite(OLED_RST, HIGH); delay(100);
  display.init();
  display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);

  Serial.println("\n# === LoRa PHY Quest MOBILE (Heltec V3) ===");
  if (!initRadio()) {
    statusLine = "RADIO INIT FAIL";
    updateDisplay();
    while (true) delay(1000);
  }
  Serial.println("# Radio init OK");
  printHelp();
  printStatus();
  statusLine = running ? "Running" : "Press PRG / start";
  if (running) startTx();
  updateDisplay();
}

void loop() {
  pollSerial();
  pollButton();

  if (running && (uint32_t)(millis() - lastTxMs) >= txIntervalMs) {
    sendPacket();
  }
  if (displayDirty || millis() - lastDisplayMs > 1000) updateDisplay();
}
