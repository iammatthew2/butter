# Butter

ESP32-C6 (Seeed XIAO) + Adafruit 16x2 LCD Pi Plate over I2C.

This project listens to MQTT events on the local network and shows them on the LCD.

<img width="1120" height="878" alt="Butter, the simple MQTT display" src="https://github.com/user-attachments/assets/8da0f640-beab-4236-a2ff-5c332e01d175" />

## Hardware
- Controller: Seeed XIAO ESP32-C6
- Display: Adafruit 16x2 RGB LCD Pi Plate

## Wiring
- `D4 (GPIO22 / SDA)` (pull up to 3v, 3k ohms) -> `SDA`
- `D5 (GPIO23 / SCL)` (pull up to 3v, 3k ohms) -> `SCL`
- `GND` -> `GND`
- `5V (VBUS)` -> `5V`

## Behavior
- Connects to Wi-Fi and local MQTT broker.
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
- Serial ports are pinned in `platformio.ini`:
  - `upload_port = /dev/cu.usbmodem11401`
  - `monitor_port = /dev/cu.usbmodem11401`

## References
- LCD info: https://www.adafruit.com/product/1110
