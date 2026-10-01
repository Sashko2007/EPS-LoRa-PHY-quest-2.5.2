#include <RadioLib.h>
#include <Wire.h>
#include <SPI.h>
#include "SSD1306Wire.h"

// РОЛЬ ПЛАТИ 
#define IS_NODE_A false   // true = Node A (PING), false = Node B (PONG)

// Піни Heltec V3 
#define OLED_SDA   17
#define OLED_SCL   18
#define OLED_RST   21
#define VEXT_PIN   36    // LOW = живлення OLED увімкнено

#define LORA_SCK   9
#define LORA_MISO  11
#define LORA_MOSI  10
#define LORA_NSS   8
#define LORA_DIO1  14
#define LORA_RST   12
#define LORA_BUSY  13

// Параметри радіо
#define LORA_FREQ      868.3   // МГц
#define LORA_BW        125.0   // кГц
#define LORA_SF        9       // Spreading Factor
#define LORA_CR        7       // Coding Rate 4/7
#define LORA_SYNCWORD  RADIOLIB_SX126X_SYNC_WORD_PRIVATE
#define LORA_POWER     10      // дБм
#define LORA_PREAMBLE  8
#define LORA_TCXO_V    1.8     // напруга TCXO на V3

// Таймінги (Node A) 
#define PING_INTERVAL_MS  3000  // Як часто надсилати PING
#define PONG_TIMEOUT_MS   2000  // Скільки чекати PONG

SSD1306Wire display(0x3c, OLED_SDA, OLED_SCL);
SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);

// Прапорець отримання пакета (виставляється в перериванні від DIO1)
volatile bool rxFlag = false;
void IRAM_ATTR onRadioEvent() {
  rxFlag = true;
}

// Лічильники
uint32_t sentCount = 0;      // Node A: надіслано PING / Node B: не використовується
uint32_t okCount = 0;        // Node A: отримано PONG / Node B: отримано PING
uint32_t lostCount = 0;      // Node A: PONG не дочекались
float lastRssi = 0;
float lastSnr = 0;
uint32_t lastPingTime = 0;
String statusLine = "Init...";

// Оновлення екрана
void updateDisplay() {
  display.clear();
  display.drawString(0, 0, IS_NODE_A ? "NODE A (PING)" : "NODE B (PONG)");
  if (IS_NODE_A) {
    display.drawString(0, 14, "Sent:" + String(sentCount) + " OK:" + String(okCount) + " Lost:" + String(lostCount));
  } else {
    display.drawString(0, 14, "Received PING: " + String(okCount));
  }
  display.drawString(0, 28, "RSSI:" + String(lastRssi, 0) + " SNR:" + String(lastSnr, 1));
  display.drawString(0, 44, statusLine);
  display.display();
}

// Ініціалізація радіо; повертає true при успіху
bool initRadio() {
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  int state = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR,
                          LORA_SYNCWORD, LORA_POWER, LORA_PREAMBLE, LORA_TCXO_V);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("Radio init FAIL (%d)\n", state);
    return false;
  }
  radio.setDio2AsRfSwitch(true);       // DIO2 керує RF-перемикачем
  radio.setDio1Action(onRadioEvent);   // Переривання при події радіо
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Живлення дисплея та його скидання
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

  Serial.println(IS_NODE_A ? "\n=== NODE A (PING) ===" : "\n=== NODE B (PONG) ===");

  if (!initRadio()) {
    statusLine = "RADIO INIT FAIL";
    updateDisplay();
    while (true) delay(1000);  // Далі працювати немає сенсу
  }
  Serial.println("Radio init OK");

  // Node B одразу переходить у режим прослуховування
  if (!IS_NODE_A) {
    rxFlag = false;
    radio.startReceive();
    statusLine = "Listening...";
  } else {
    statusLine = "Ready";
  }
  updateDisplay();
}

// Логіка Node A
void loopNodeA() {
  if (millis() - lastPingTime < PING_INTERVAL_MS) return;
  lastPingTime = millis();

  // 1. Надсилаємо PING
  sentCount++;
  int state = radio.transmit("PING");
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("TX error: %d\n", state);
    statusLine = "TX error";
    updateDisplay();
    return;
  }
  Serial.printf("[%lu] PING sent\n", (unsigned long)sentCount);

  // 2. Переходимо у прийом і чекаємо PONG
  rxFlag = false;
  radio.startReceive();
  uint32_t start = millis();
  while (!rxFlag && millis() - start < PONG_TIMEOUT_MS) {
    delay(1);
  }

  // 3. Аналізуємо результат
  if (rxFlag) {
    rxFlag = false;
    String reply;
    int rs = radio.readData(reply);
    if (rs == RADIOLIB_ERR_NONE && reply == "PONG") {
      okCount++;
      lastRssi = radio.getRSSI();
      lastSnr = radio.getSNR();
      statusLine = "PONG received";
      Serial.printf("  PONG OK  RSSI=%.1f dBm  SNR=%.1f dB\n", lastRssi, lastSnr);
    } else {
      lostCount++;
      statusLine = "Bad reply";
      Serial.printf("  Bad reply (err=%d, data='%s')\n", rs, reply.c_str());
    }
  } else {
    lostCount++;
    statusLine = "PONG timeout";
    Serial.println("  PONG timeout");
  }
  radio.standby();
  updateDisplay();
}

void loopNodeB() {
  String msg;
  int rs = radio.receive(msg);   // блокуюче очікування пакета

  if (rs == RADIOLIB_ERR_RX_TIMEOUT) {
    return;                      // просто нічого не прийшло, слухаємо далі
  }

  if (rs == RADIOLIB_ERR_NONE) {
    lastRssi = radio.getRSSI();
    lastSnr = radio.getSNR();
    Serial.printf("RX: '%s'  RSSI=%.1f  SNR=%.1f\n", msg.c_str(), lastRssi, lastSnr);

    if (msg == "PING") {
      okCount++;
      delay(50);                 // даємо Node A час перейти у прийом
      int ts = radio.transmit("PONG");
      Serial.printf("PONG sent, code=%d\n", ts);
      statusLine = (ts == RADIOLIB_ERR_NONE) ? "PONG sent" : "TX error";
    } else {
      statusLine = "Unknown packet";
    }
  } else {
    Serial.printf("RX error: %d\n", rs);   // наприклад -7 = помилка CRC
    statusLine = "RX error";
  }
  updateDisplay();
}

void loop() {
  if (IS_NODE_A) {
    loopNodeA();
  } else {
    loopNodeB();
  }
}
