// LILYGO TTGO LoRa32 V2.1 (T3 V1.6.1, ESP32 + SX1276) - 10-секундний smoke test
// OLED SSD1306 + радіомодуль SX1276
// Arduino IDE: Board = "ESP32 Dev Module", Serial Monitor 115200.
// НЕ вмикайте передачу без підключеної антени (цей тест сам нічого в ефір не передає).

#include <RadioLib.h>
#include <Wire.h>
#include <SPI.h>
#include "SSD1306Wire.h"

// ---------- Піни TTGO LoRa32 V2.1 / T3 V1.6.1 ----------
#define OLED_SDA   21   // I2C дані дисплея
#define OLED_SCL   22   // I2C тактування дисплея
// УВАГА: GPIO16/17 на ESP32-PICO-D4 зайняті вбудованою flash-пам'яттю, їх чіпати не можна.
// Reset-піна для OLED на цій платі немає, апаратний reset дисплея не потрібен.
// Vext на цій платі немає: дисплей живиться постійно.

// Піни SPI для радіомодуля SX1276
#define LORA_SCK    5
#define LORA_MISO  19
#define LORA_MOSI  27
#define LORA_CS    18
#define LORA_RST   23
#define LORA_DIO0  26   // IRQ
#define LORA_DIO1  33

// ---------- Об'єкти ----------
SSD1306Wire display(0x3c, OLED_SDA, OLED_SCL);

// Радіомодуль SX1276: Module(CS, DIO0, RST, DIO1)
SX1276 radio = new Module(LORA_CS, LORA_DIO0, LORA_RST, LORA_DIO1);

// ---------- Прапорці стану ----------
bool isRadioOK = false;    // результат ініціалізації радіо
bool isDisplayOK = false;  // результат перевірки дисплея (ACK по I2C)

void setup() {
  Serial.begin(115200);
  delay(1000);

  // 1. Ініціалізація шини I2C
  Wire.begin(OLED_SDA, OLED_SCL);

  // 2. Низькорівнева перевірка дисплея (пінгуємо адресу 0x3C)
  Wire.beginTransmission(0x3C);
  isDisplayOK = (Wire.endTransmission() == 0);  // 0 = дисплей відповів ACK

  // 3. Ініціалізація бібліотеки дисплея
  display.init();
  display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);

  Serial.println("\n==================================");
  Serial.println(" OLED + SX1276 10 second smoke test ");
  Serial.println("==================================");

  if (isDisplayOK) {
    Serial.println("OLED Hardware Check: SUCCESS (ACK received at 0x3C)");
  } else {
    Serial.println("OLED Hardware Check: FAIL (No I2C response)");
  }

  // 4. Ініціалізація SPI для радіомодуля
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);

  // 5. Ініціалізація LoRa
  // Параметри: частота, смуга, SF, CR, sync word, потужність, довжина преамбули.
  // 0x12 = private sync word (сумісний з Base на SX1262). TCXO і DIO2-перемикача на SX1276 немає.
  int state = radio.begin(868.3, 125.0, 9, 7, 0x12, 10, 8);
  if (state == RADIOLIB_ERR_NONE) {
    isRadioOK = true;
    Serial.println("SX1276 Radio Init: SUCCESS");
  } else {
    isRadioOK = false;
    Serial.printf("SX1276 Radio Init: FAIL (%d)\n", state);
  }
}

void loop() {
  static uint32_t seconds = 0;
  String scrollText = " *** TTGO DISPLAY TEST *** ";

  if (seconds < 10) {
    seconds++;

    // Анімація на дисплеї: біжучий рядок, тривалість ~1 секунда
    for (int offset = 0; offset < 10; offset++) {
      display.clear();

      display.drawString(0, 0, "MCU & SX1276: RUNNING");
      display.drawString(0, 16, "Radio Status: " + String(isRadioOK ? "OK" : "FAIL"));

      int xPos = 128 - (offset * 14);  // Зсув тексту вліво
      display.drawString(xPos, 36, scrollText);
      display.drawString(0, 52, "Time left: " + String(11 - seconds) + "s");

      display.display();
      delay(100);
    }

    Serial.printf("[%lu/10s] Response: MCU Alive | OLED ACK: %s | Radio: %s\n",
                  (unsigned long)seconds,
                  isDisplayOK ? "OK" : "FAIL",
                  isRadioOK ? "OK" : "FAIL");

  } else if (seconds == 10) {
    seconds++;

    Serial.println("\n==================================");
    Serial.println(" TEST COMPLETED");
    Serial.println("==================================");

    display.clear();
    display.drawString(0, 0, "TEST COMPLETED");
    display.drawString(0, 16, String("OLED: ") + (isDisplayOK ? "OK" : "FAIL"));
    display.drawString(0, 32, String("Radio: ") + (isRadioOK ? "OK" : "FAIL"));
    display.display();

  } else {
    delay(1000);  // тест завершено, результат лишається на екрані
  }
}
