//  Heltec WiFi LoRa 32 V3 (ESP32-S3) - 10-секундний тест
//  OLED SSD1306 + радіомодуль SX1262

#include <RadioLib.h>
#include <Wire.h>
#include <SPI.h>
#include "SSD1306Wire.h"

//Піни Heltec WiFi LoRa 32 V3
#define OLED_SDA   17   // I2C дані дисплея
#define OLED_SCL   18   // I2C тактування дисплея
#define OLED_RST   21   // Лінія скидання (RST) дисплея
#define VEXT_PIN   36   // Керування живленням периферії Vext (LOW = увімкнено)

// Піни SPI для радіомодуля SX1262
#define LORA_SCK   9
#define LORA_MISO  11
#define LORA_MOSI  10
#define LORA_NSS   8
#define LORA_DIO1  14
#define LORA_RST   12
#define LORA_BUSY  13

//Об'єкти
// Дисплей (адреса 0x3C, SDA=17, SCL=18)
SSD1306Wire display(0x3c, OLED_SDA, OLED_SCL);

// Радіомодуль SX1262: Module(NSS, DIO1, RST, BUSY)
SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);

// Прапорці стану
bool isRadioOK = false;    // Результат ініціалізації радіо
bool isDisplayOK = false;  // Результат апаратної перевірки дисплея (ACK по I2C)

void setup() {
  Serial.begin(115200);
  delay(1000);

  // 1. Подача живлення на периферію (Vext: LOW = увімкнено)
  pinMode(VEXT_PIN, OUTPUT);
  digitalWrite(VEXT_PIN, LOW);
  delay(200);  // Даємо живленню стабілізуватися

  // 2. Апаратне скидання (Reset) OLED-дисплея
  pinMode(OLED_RST, OUTPUT);
  digitalWrite(OLED_RST, HIGH);
  delay(10);
  digitalWrite(OLED_RST, LOW);
  delay(20);
  digitalWrite(OLED_RST, HIGH);
  delay(100);

  // 3. Ініціалізація шини I2C
  Wire.begin(OLED_SDA, OLED_SCL);

  // 4. Низькорівнева перевірка дисплея (пінгуємо адресу 0x3C)
  Wire.beginTransmission(0x3C);
  isDisplayOK = (Wire.endTransmission() == 0);  // 0 = дисплей відповів ACK

  // 5. Ініціалізація бібліотеки дисплея
  display.init();
  display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);

  Serial.println("\n==================================");
  Serial.println(" OLED + SX1262 10 second smoke test ");
  Serial.println("==================================");

  // Вивід апаратного статусу дисплея у Serial
  if (isDisplayOK) {
    Serial.println("OLED Hardware Check: SUCCESS (ACK received at 0x3C)");
  } else {
    Serial.println("OLED Hardware Check: FAIL (No I2C response)");
  }

  // 6. Ініціалізація SPI для радіомодуля (на V3 піни нестандартні)
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);

  // 7. Ініціалізація LoRa
  //    Параметри: частота, смуга, SF, CR, sync word, потужність,
  //    довжина преамбули, напруга TCXO (на V3 TCXO живиться через DIO3, 1.8 В)
  int state = radio.begin(868.0, 125.0, 9, 7,
                          RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 10, 8, 1.8);
  if (state == RADIOLIB_ERR_NONE) {
    isRadioOK = true;
    radio.setDio2AsRfSwitch(true);  // DIO2 керує радіочастотним перемикачем
    Serial.println("SX1262 Radio Init: SUCCESS");
  } else {
    isRadioOK = false;
    Serial.printf("SX1262 Radio Init: FAIL (%d)\n", state);
  }
}

void loop() {
  static uint32_t seconds = 0;
  String scrollText = "   *** HELTEC V3 DISPLAY TEST ***   ";

  if (seconds < 10) {
    seconds++;

    // Анімація на дисплеї: біжучий рядок, тривалість ~1 секунда
    for (int offset = 0; offset < 10; offset++) {
      display.clear();

      display.drawString(0, 0, "MCU & SX1262: RUNNING");
      display.drawString(0, 16, "Radio Status: " + String(isRadioOK ? "OK" : "FAIL"));

      int xPos = 128 - (offset * 14);  // Зсув тексту вліво
      display.drawString(xPos, 36, scrollText);
      display.drawString(0, 52, "Time left: " + String(11 - seconds) + "s");

      display.display();
      delay(100);
    }

    // Статус у Serial з урахуванням стану обох модулів
    Serial.printf("[%lu/10s] Response: MCU Alive | OLED ACK: %s | Radio: %s\n",
                  (unsigned long)seconds,
                  isDisplayOK ? "OK" : "FAIL",
                  isRadioOK ? "OK" : "FAIL");

  } else if (seconds == 10) {
    seconds++;

    Serial.println("\n==================================");
    Serial.println(" TEST COMPLETED");
    Serial.println("==================================");

    // Фінальне повідомлення на дисплеї
    display.clear();
    display.drawString(0, 20, "TEST FINISHED");
    display.drawString(0, 36, "Powering off...");
    display.display();
    delay(1500);

    // Вимкнення радіо та дисплея
    radio.sleep();
    display.displayOff();
    digitalWrite(VEXT_PIN, HIGH);  // Вимикаємо Vext (HIGH = живлення знято)
  }
}