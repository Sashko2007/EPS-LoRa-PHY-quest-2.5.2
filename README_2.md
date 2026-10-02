# EPS · LoRa PHY Quest

![version](https://img.shields.io/badge/version-2.5.2-blue)
![LoRa](https://img.shields.io/badge/LoRa-868.3%20MHz-orange)
![ESP32](https://img.shields.io/badge/ESP32-Arduino-00979D)
![Python](https://img.shields.io/badge/Python-3.8+-3776AB)

Вимірювання якості LoRa-зв'язку (RSSI, SNR, PDR, Time-on-Air) між Base і Mobile вузлами.
Порівнюємо профілі FAST / RANGE / ROBUST і три антени на відстанях до 450 м у міській забудові.

## Стенд

| Вузол | Плата | Радіо |
|---|---|---|
| Base | Heltec WiFi LoRa 32 V3 | SX1262 |
| Mobile #1 | LILYGO TTGO LoRa32 V2.1 | SX1276 |
| Mobile #2 | Heltec WiFi LoRa 32 V3 | SX1262 |

Mobile передає пакети → Base приймає і рахує RSSI, SNR, втрати → ноутбук записує все в `raw.csv`.

| Профіль | BW, кГц | SF | CR | TX, dBm | ToA (20 Б) |
|---|---|---|---|---|---|
| BENCH | 125 | 7 | 4/5 | 2 | 57 мс |
| FAST | 250 | 7 | 4/5 | 5 | 28 мс |
| RANGE | 125 | 10 | 4/5 | 5 | 371 мс |
| ROBUST | 125 | 12 | 4/8 | 5 | 1712 мс |

**Антени:** штатна (`_sa`), кастомна спіраль (`_sp`), вертикальний диполь (`_bp`) — фото в `antennas/`, карти точок у `measurement locations/`.

## Структура

```
firmware/                прошивки Base і Mobile (Arduino)
logger/                  Python-logger → raw.csv
data/raw/                сирі логи вимірювань
antennas/                фото антен
measurement locations/   карти точок
tests/                   smoke-тести плат, Ping-Pong
```

Назва папки з даними: `raw_1b_1m_100m_ic_rangecfg_bp` → 1 Base, 1 Mobile, 100 м, у місті, RANGE, диполь.

## Встановлення

```bash
git clone <посилання на репозиторій>
pip install -r logger/requirements.txt
```

Arduino IDE: плата **esp32** (Espressif), бібліотеки **RadioLib** і **ESP8266 and ESP32 OLED driver for SSD1306**.

| Прошивка | Плата в Arduino IDE |
|---|---|
| `base_heltec`, `mobile_heltec` | Heltec WiFi LoRa 32(V3) |
| `mobile_TTGO` | ESP32 Dev Module |

> Не вмикайте передачу без антени.

## Використання

```bash
python logger/LoRa_logger.py --port COM7 --out raw.csv
```

| Де | Команда | Дія |
|---|---|---|
| logger | `loc P100 100 1.5 dipole` | точка, відстань, висота, нотатка |
| logger | `!cfg 2` | переключити Base на RANGE |
| Mobile (Serial) | `cfg 2`, `run 5`, `status` | профіль, номер прогону, стан |

Профіль на Base і Mobile має бути однаковим.

## Результати (PDR)

| Відстань | Диполь RANGE | Диполь ROBUST | Штатна RANGE | Спіраль RANGE |
|---|---|---|---|---|
| 50 м | 100 % | 100 % | 100 % | 100 % |
| 100 м | 100 % | 100 % | 100 % | 100 % |
| 270 м | 61 % | 89 % | 18 % | 35 % |
| 330 м | 96 % | 100 % | — | 35 % |
| 450 м | 29 % | 53 % | — | — |

- **ROBUST тримає зв'язок там, де RANGE вже губить пакети**, але займає ефір у 4,6 раза довше.
- **Антена важить не менше за профіль:** диполь помітно кращий за штатну і спіраль.
- **FAST** — для частої телеметрії на коротких відстанях, **RANGE** — для рідких повідомлень, **ROBUST** — коли важливо доставити за будь-яку ціну.

## Обмеження

- Інтервал передачі 3 с для всіх профілів — для RANGE і ROBUST це більше за 1 % duty cycle.
- `raw15.csv` (ROBUST, 270 м, штатна) — копія `raw12.csv` (RANGE).
- Відстані оцінені за картою.

## Ліцензія

MIT
