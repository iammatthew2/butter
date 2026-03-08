# Project Butter Notes

## Goal
Wire up an ESP32-C6 board (XIAO form factor) to an LCD display over I2C.

## Hardware Context
- Controller: ESP32-C6 (XIAO / "xiaostudio" board)
- Display: Adafruit I2C 16x2 RGB LCD Pi Plate
- The LCD is embedded in a Raspberry Pi HAT, but we will use the I2C interface directly from the ESP32-C6 instead of a Raspberry Pi.

## Reference Links
- Product page: https://www.adafruit.com/product/1110
- Guide: https://learn.adafruit.com/adafruit-16x2-character-lcd-plus-keypad-for-raspberry-pi
- Hardware repo (PCB files + README): https://github.com/adafruit/Adafruit-16x2-LCD-Pi-Plate

## Notes
- Primary integration path: use I2C wiring between ESP32-C6 and the Pi Plate LCD backpack/interface.
- Keep pin mapping and voltage compatibility notes here as wiring decisions are finalized.

## Board Details (from Adafruit Repo README)
- The plate exposes a 16x2 LCD plus 5 keypad buttons (up, down, left, right, select) over I2C.
- It is designed to use only SDA/SCL for LCD control, button reads, and backlight control.
- The white backlight is effectively on/off only; no PWM dimming via the I2C expander.
- Adafruit's Pi/Python examples include software debouncing for buttons. On ESP32-C6, we should implement our own debounce logic in firmware.

## Wiring (XIAO ESP32-C6 <-> Adafruit 16x2 LCD Pi Plate)
- `D4 (GPIO22, SDA)` -> `SDA`
- `D5 (GPIO23, SCL)` -> `SCL`
- `GND` -> `GND`
- `5V (VBUS)` -> `5V`

## Wiring Notes
- On XIAO ESP32-C6, default I2C pins are `D4=SDA` and `D5=SCL`.
- LCD, backlight, and buttons all use the same I2C bus; no extra button signal wires are required.
- Keep SDA/SCL logic at `3.3V` levels (ESP32-C6 GPIOs are not 5V tolerant).
- If using a separate 5V supply for the plate, still connect grounds together.

## Power Plan
- Project power source: USB connected to the XIAO ESP32-C6.
- LCD Pi Plate power: feed from XIAO `5V (VBUS)` to plate `5V`.
- Ground reference: XIAO `GND` to plate `GND` (required).
- Note: `VBUS` 5V is available when USB is connected.
