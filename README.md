# Butter

ESP32-C6 (Seeed XIAO) + Adafruit 16x2 LCD Pi Plate over I2C.

This project listens to MQTT events on the local network and shows them on the LCD.

## Hardware
- Controller: Seeed XIAO ESP32-C6
- Display: Adafruit 16x2 RGB LCD Pi Plate

## Wiring
- `D4 (GPIO22 / SDA)` -> `SDA`
- `D5 (GPIO23 / SCL)` -> `SCL`
- `GND` -> `GND`
- `5V (VBUS)` -> `5V`

Notes:
- ESP32-C6 is 3.3V logic; keep I2C pull-ups at 3.3V.
- External I2C pull-ups were required for stable bus behavior.
- Add pull-ups:
  - `3k` from `SDA` to `3.3V`
  - `3k` from `SCL` to `3.3V`
  - (`4.7k` also works if needed)

## Firmware Behavior
- Connects to Wi-Fi and local MQTT broker.
- Subscribes to topic from `src/secrets.h` (`MQTT_EVENTS_TOPIC`).
- Displays latest event:
  - Line 1: `T:<topic>`
  - Line 2: `M:<payload>`
- LCD backlight turns off after 20s of no activity.
- Any MQTT event or button press resets the light timer.

## Buttons
- `LEFT/RIGHT`: horizontal scroll for long text
- `UP/DOWN`: browse older/newer messages (history)
- `SELECT`: jump back to newest message

## Local Config
- Secrets live in `src/secrets.h` (ignored by git).
- Serial ports are pinned in `platformio.ini`:
  - `upload_port = /dev/cu.usbmodem11401`
  - `monitor_port = /dev/cu.usbmodem11401`

## Build / Flash / Monitor
```bash
pio run
pio run -t upload
pio device monitor -b 115200
```

## References
- Product: https://www.adafruit.com/product/1110
- Guide: https://learn.adafruit.com/adafruit-16x2-character-lcd-plus-keypad-for-raspberry-pi
- Hardware repo: https://github.com/adafruit/Adafruit-16x2-LCD-Pi-Plate
