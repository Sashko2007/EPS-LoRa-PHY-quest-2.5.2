// ============================================================
//  LoRa PHY Quest - BASE NODE (Heltec WiFi LoRa 32 V3)
//
//  Що робить (розділ 6 гайда, "Base node"):
//    - приймає пакети (від кількох Mobile nodes)
//    - зчитує RSSI і SNR на приймачі
//    - бачить node_id, run_id, seq_num, config_id
//    - рахує втрати за пропусками seq_num
//    - рахує live статистику received, lost, PDR
//    - друкує machine-readable CSV-рядки в Serial
//    - live status на OLED
//
//  Формат Serial (115200):
//    - рядки, що починаються з '#'  = debug/службові (logger їх відкидає)
//    - решта рядків = ДАНІ, один успішний прийом = один рядок:
//      run_id,node_id,role,seq_num,config_id,freq_hz,sf,bw_hz,cr,tx_power_dbm,
//      payload_len,tx_interval_ms,toa_us,rssi_dbm,snr_db,rx_ok,received,lost,pdr
//    (timestamp_utc, team_id, location_tag... додає ноутбучний logger)
//
//  Команди Serial (Enter в кінці):
//    help | status | cfg <id> | reset
//    cfg <id> змінює LoRa profile Base (має збігатися з Mobile!)
//
//  Правила лічильників:
//    - лічильники ведуться окремо для кожного node_id
//    - новий run_id (або seq_num нижче за попередній) => лічильники цього вузла скидаються
//    - якщо перший побачений seq_num < START_TOLERANCE, вважаємо що run стартував з 0
//      і рахуємо втрати з самого початку; інакше базова лінія = перший побачений seq
// ============================================================

#include <RadioLib.h>
#include <Wire.h>
#include <SPI.h>
#include "SSD1306Wire.h"
#include "lora_common.h"

// ---------- Піни Heltec WiFi LoRa 32 V3 (див. схему) ----------
#define OLED_SDA   17
#define OLED_SCL   18
#define OLED_RST   21
#define VEXT_PIN   36    // LOW = живлення OLED увімкнено
#define LORA_SCK    9
#define LORA_MISO  11
#define LORA_MOSI  10
#define LORA_NSS    8
#define LORA_DIO1  14
#define LORA_RST   12
#define LORA_BUSY  13
#define LORA_TCXO_V 1.8f

#define DEFAULT_CONFIG_ID   1     // профіль Base при старті
#define MAX_NODES           8
#define START_TOLERANCE     10

SSD1306Wire display(0x3c, OLED_SDA, OLED_SCL);
SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);

volatile bool rxFlag = false;
void IRAM_ATTR onRadioEvent() { rxFlag = true; }

struct NodeStat {
  bool     used;
  uint8_t  node_id;
  uint16_t run_id;
  uint32_t last_seq;
  uint32_t received;
  uint32_t lost;
};
NodeStat nodes[MAX_NODES];

uint8_t  baseCfgId = DEFAULT_CONFIG_ID;
uint32_t crcErrors = 0;
uint32_t badPackets = 0;
uint32_t totalRx = 0;

// Для OLED
uint8_t  lastNode = 0;
uint32_t lastSeq = 0;
float    lastRssi = 0, lastSnr = 0;
String   statusLine = "Init...";
bool     displayDirty = true;
uint32_t lastDisplayMs = 0;

// ------------------------------------------------------------
// Повертає індекс у nodes[] (або -1). Повертаємо int, а не NodeStat*, бо Arduino IDE
// генерує прототипи функцій ДО struct-ів скетча, і повернення NodeStat* не скомпілюється.
int findNode(uint8_t id) {
  for (int i = 0; i < MAX_NODES; i++) if (nodes[i].used && nodes[i].node_id == id) return i;
  for (int i = 0; i < MAX_NODES; i++) if (!nodes[i].used) { nodes[i] = {}; nodes[i].used = true; nodes[i].node_id = id; return i; }
  return -1;
}

void resetStats() {
  for (int i = 0; i < MAX_NODES; i++) nodes[i] = {};
  crcErrors = badPackets = totalRx = 0;
  lastNode = 0; lastSeq = 0; lastRssi = lastSnr = 0;
  Serial.println("# STATS RESET");
  displayDirty = true;
}

float pdrOf(uint32_t rx, uint32_t lost) {
  uint32_t tot = rx + lost;
  return tot ? (100.0f * rx / tot) : 100.0f;
}

void totals(uint32_t& rx, uint32_t& lost) {
  rx = lost = 0;
  for (int i = 0; i < MAX_NODES; i++) if (nodes[i].used) { rx += nodes[i].received; lost += nodes[i].lost; }
}

// ------------------------------------------------------------
void updateDisplay() {
  const LoraProfile* p = findProfile(baseCfgId);
  uint32_t rx, lost; totals(rx, lost);
  display.clear();
  display.drawString(0, 0,  String("BASE cfg ") + baseCfgId + " " + (p ? p->name : "?") +
                            " SF" + (p ? p->sf : 0) + " BW" + (p ? (int)p->bw_khz : 0));
  display.drawString(0, 12, String("RX:") + rx + " Lost:" + lost + " CRC:" + crcErrors);
  display.drawString(0, 24, String("PDR: ") + String(pdrOf(rx, lost), 1) + "%");
  if (lastNode) {
    display.drawString(0, 36, String("n") + lastNode + " #" + lastSeq + " " + String(lastRssi, 0) + "dBm " + String(lastSnr, 1) + "dB");
  } else {
    display.drawString(0, 36, "no packets yet");
  }
  display.drawString(0, 48, statusLine);
  display.display();
  displayDirty = false;
  lastDisplayMs = millis();
}

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
  if (st != RADIOLIB_ERR_NONE) { Serial.printf("# ERR applyProfile(%u) code=%d\n", id, st); return false; }
  baseCfgId = id;
  rxFlag = false;
  radio.startReceive();
  Serial.printf("# CFG %u %s freq=%.1f bw=%.0f sf=%u cr=4/%u\n", p->id, p->name, p->freq_mhz, p->bw_khz, p->sf, p->cr);
  return true;
}

bool initRadio() {
  const LoraProfile* p = findProfile(baseCfgId);
  if (!p) p = &PROFILES[0];
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  int st = radio.begin(p->freq_mhz, p->bw_khz, p->sf, p->cr, LORA_SYNCWORD,
                       p->tx_dbm, LORA_PREAMBLE, LORA_TCXO_V);
  if (st != RADIOLIB_ERR_NONE) { Serial.printf("# Radio init FAIL (%d)\n", st); return false; }
  radio.setDio2AsRfSwitch(true);
  radio.setDio1Action(onRadioEvent);
  baseCfgId = p->id;
  return true;
}

// ------------------------------------------------------------
//  Обробка прийнятого пакета (F5, F6, F7)
// ------------------------------------------------------------
void handleRx() {
  uint8_t buf[PKT_MAX_LEN];
  size_t n = radio.getPacketLength();
  if (n > PKT_MAX_LEN) n = PKT_MAX_LEN;
  int st = radio.readData(buf, n);

  if (st == RADIOLIB_ERR_CRC_MISMATCH) {
    crcErrors++;
    statusLine = "CRC error";
    Serial.printf("# RX CRC_ERROR count=%lu\n", (unsigned long)crcErrors);
    displayDirty = true;
    return;
  }
  if (st != RADIOLIB_ERR_NONE) {
    badPackets++;
    Serial.printf("# RX ERROR code=%d\n", st);
    return;
  }

  // RSSI/SNR читаємо ПІСЛЯ readData - це метрики щойно прийнятого пакета
  float rssi = radio.getRSSI();
  float snr  = radio.getSNR();

  LoraPacket pkt;
  if (n < PKT_HEADER_LEN) { badPackets++; Serial.printf("# RX SHORT len=%u\n", (unsigned)n); return; }
  memcpy(&pkt, buf, sizeof(pkt));
  if (pkt.magic != PKT_MAGIC) { badPackets++; Serial.printf("# RX FOREIGN len=%u\n", (unsigned)n); return; }
  if (pkt.payload_len != n) Serial.printf("# WARN payload_len field=%u but received=%u\n", pkt.payload_len, (unsigned)n);
  if (pkt.config_id != baseCfgId) Serial.printf("# WARN config mismatch: packet cfg=%u, base cfg=%u\n", pkt.config_id, baseCfgId);

  int si = findNode(pkt.node_id);
  if (si < 0) { Serial.println("# WARN too many nodes"); return; }
  NodeStat* s = &nodes[si];

  // ----- втрати за пропусками seq_num -----
  bool newRun = (s->received == 0 && s->lost == 0) || s->run_id != pkt.run_id;
  if (newRun) {
    s->run_id = pkt.run_id;
    s->received = 1;
    s->lost = (pkt.seq_num < START_TOLERANCE) ? pkt.seq_num : 0;
    s->last_seq = pkt.seq_num;
    Serial.printf("# NEW_RUN node=%u run=%u first_seq=%lu\n", pkt.node_id, pkt.run_id, (unsigned long)pkt.seq_num);
  } else if (pkt.seq_num > s->last_seq) {
    s->lost += pkt.seq_num - s->last_seq - 1;
    s->last_seq = pkt.seq_num;
    s->received++;
  } else if (pkt.seq_num == s->last_seq) {
    Serial.printf("# DUPLICATE node=%u seq=%lu\n", pkt.node_id, (unsigned long)pkt.seq_num);
    return;                                   // дубль не пишемо в дані
  } else {
    // seq пішов назад без зміни run_id => Mobile перезавантажився, починаємо заново
    Serial.printf("# SEQ_RESET node=%u last=%lu got=%lu\n", pkt.node_id, (unsigned long)s->last_seq, (unsigned long)pkt.seq_num);
    s->received = 1;
    s->lost = (pkt.seq_num < START_TOLERANCE) ? pkt.seq_num : 0;
    s->last_seq = pkt.seq_num;
  }
  totalRx++;

  // ----- машинозчитуваний рядок (ОДИН прийом = ОДИН рядок) -----
  const LoraProfile* p = findProfile(baseCfgId);
  uint32_t toaUs = (uint32_t)radio.getTimeOnAir(n);
  Serial.printf("%u,%u,mobile,%lu,%u,%lu,%u,%lu,%u,%d,%u,%lu,%lu,%.1f,%.2f,1,%lu,%lu,%.2f\n",
                pkt.run_id, pkt.node_id, (unsigned long)pkt.seq_num, pkt.config_id,
                (unsigned long)(p->freq_mhz * 1e6f + 0.5f), p->sf,
                (unsigned long)(p->bw_khz * 1000.0f + 0.5f), p->cr, p->tx_dbm,
                (unsigned)n, (unsigned long)pkt.tx_interval_ms, (unsigned long)toaUs,
                rssi, snr,
                (unsigned long)s->received, (unsigned long)s->lost, pdrOf(s->received, s->lost));

  lastNode = pkt.node_id; lastSeq = pkt.seq_num; lastRssi = rssi; lastSnr = snr;
  statusLine = "RX ok";
  displayDirty = true;
}

// ------------------------------------------------------------
void printStatus() {
  uint32_t rx, lost; totals(rx, lost);
  Serial.printf("# STATUS cfg=%u total_rx=%lu total_lost=%lu pdr=%.2f crc_err=%lu bad=%lu\n",
                baseCfgId, (unsigned long)rx, (unsigned long)lost, pdrOf(rx, lost),
                (unsigned long)crcErrors, (unsigned long)badPackets);
  for (int i = 0; i < MAX_NODES; i++) if (nodes[i].used)
    Serial.printf("# NODE %u run=%u rx=%lu lost=%lu pdr=%.2f last_seq=%lu\n", nodes[i].node_id, nodes[i].run_id,
                  (unsigned long)nodes[i].received, (unsigned long)nodes[i].lost,
                  pdrOf(nodes[i].received, nodes[i].lost), (unsigned long)nodes[i].last_seq);
}

void handleCommand(char* line) {
  char cmd[16] = {0};
  long arg = 0;
  int n = sscanf(line, "%15s %ld", cmd, &arg);
  if (n < 1) return;
  if (!strcmp(cmd, "help")) {
    Serial.println("# Commands: help | status | cfg N | reset");
    Serial.print("# Profiles:");
    for (uint8_t i = 0; i < NUM_PROFILES; i++) Serial.printf(" %u=%s", PROFILES[i].id, PROFILES[i].name);
    Serial.println();
  }
  else if (!strcmp(cmd, "status")) printStatus();
  else if (!strcmp(cmd, "reset"))  resetStats();
  else if (n == 2 && !strcmp(cmd, "cfg")) {
    if (arg < 0 || arg > 255 || !applyProfile((uint8_t)arg)) Serial.println("# ERR unknown cfg");
    else resetStats();                        // статистика різних профілів не змішується
  }
  else Serial.println("# ERR unknown command (help)");
  displayDirty = true;
}

void pollSerial() {
  static char line[64];
  static uint8_t len = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (len > 0) { line[len] = 0; handleCommand(line); len = 0; }
    } else if (len < sizeof(line) - 1) line[len++] = c;
  }
}

// ------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(VEXT_PIN, OUTPUT);
  digitalWrite(VEXT_PIN, LOW);
  delay(200);
  pinMode(OLED_RST, OUTPUT);
  digitalWrite(OLED_RST, HIGH); delay(10);
  digitalWrite(OLED_RST, LOW);  delay(20);
  digitalWrite(OLED_RST, HIGH); delay(100);
  display.init();
  display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);

  Serial.println("\n# === LoRa PHY Quest BASE ===");
  if (!initRadio()) {
    statusLine = "RADIO INIT FAIL";
    updateDisplay();
    while (true) delay(1000);
  }
  Serial.println("# Radio init OK");
  Serial.println("# HEADER run_id,node_id,role,seq_num,config_id,freq_hz,sf,bw_hz,cr,tx_power_dbm,payload_len,tx_interval_ms,toa_us,rssi_dbm,snr_db,rx_ok,received,lost,pdr");
  const LoraProfile* p = findProfile(baseCfgId);
  Serial.printf("# CFG %u %s freq=%.1f bw=%.0f sf=%u cr=4/%u\n", p->id, p->name, p->freq_mhz, p->bw_khz, p->sf, p->cr);

  rxFlag = false;
  radio.startReceive();
  statusLine = "Listening...";
  updateDisplay();
}

void loop() {
  pollSerial();
  if (rxFlag) {
    rxFlag = false;
    handleRx();
    radio.startReceive();
  }
  if (displayDirty && millis() - lastDisplayMs > 300) updateDisplay();
  else if (millis() - lastDisplayMs > 2000) updateDisplay();
}
