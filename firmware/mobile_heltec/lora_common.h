// ============================================================
//  lora_common.h - СПІЛЬНИЙ файл для Mobile і Base прошивок
//  Піни плат тут НЕ зберігаються: Base (Heltec V3) і Mobile (LILYGO TTGO SX1276)
//  мають різні піни, вони прописані на початку base.ino і mobile.ino.
//  ВАЖЛИВО: файли mobile/lora_common.h і base/lora_common.h
//  мають бути ІДЕНТИЧНИМИ (Arduino IDE вимагає копію в кожній
//  папці скетча). Змінили тут - скопіюйте в другу папку.
// ============================================================
#pragma once
#include <Arduino.h>

// ---------- Фіксовані параметри радіо (однакові у всіх профілях і на обох платах) ----------
// 0x12 = "private" sync word. RadioLib для SX126x (Base) сам перетворює 0x12 у 0x1424,
// що сумісно з SX127x (Mobile), тому SX1262 <-> SX1276 чують одне одного.
#define LORA_SYNCWORD  0x12
#define LORA_PREAMBLE  8

// ---------- F3: явна таблиця LoRa profiles (розділ 9 гайда) ----------
// cr: 5..8 означає 4/5..4/8 (так само, як у RadioLib)
struct LoraProfile {
  uint8_t     id;
  const char* name;
  float       freq_mhz;
  float       bw_khz;
  uint8_t     sf;
  uint8_t     cr;
  int8_t      tx_dbm;
};

static const LoraProfile PROFILES[] = {
  // id  name      freq    bw     sf  cr  tx   (мін. 2 dBm: SX1276 на PA_BOOST не вміє менше)
  {  0, "BENCH",  868.3f, 125.0f,  7, 5,  2 },
  {  1, "FAST",   868.3f, 250.0f,  7, 5,  5 },
  {  2, "RANGE",  868.3f, 125.0f, 10, 5,  5 },
  {  3, "ROBUST", 868.3f, 125.0f, 12, 8,  5 },   // bonus
};
static const uint8_t NUM_PROFILES = sizeof(PROFILES) / sizeof(PROFILES[0]);

static inline const LoraProfile* findProfile(uint8_t id) {
  for (uint8_t i = 0; i < NUM_PROFILES; i++)
    if (PROFILES[i].id == id) return &PROFILES[i];
  return nullptr;
}

// ---------- F4: формат пакета (little-endian, packed, 14 байт заголовок) ----------
#define PKT_MAGIC       0xA5
#define PKT_HEADER_LEN  14
#define PKT_MAX_LEN     255      // ліміт LoRa payload

struct __attribute__((packed)) LoraPacket {
  uint8_t  magic;           // 0xA5 - відсіює чужі пакети
  uint8_t  node_id;         // id мобільного вузла
  uint8_t  config_id;       // id профілю з PROFILES[]
  uint8_t  payload_len;     // ПОВНА довжина пакета в байтах (заголовок + наповнювач)
  uint16_t run_id;          // id запуску (експерименту)
  uint32_t seq_num;         // росте на кожному пакеті, у новому run з 0
  uint32_t tx_interval_ms;  // заданий інтервал передачі
};
static_assert(sizeof(LoraPacket) == PKT_HEADER_LEN, "LoraPacket must be 14 bytes");
