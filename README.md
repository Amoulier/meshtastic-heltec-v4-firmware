# Meshtastic Heltec V4 Firmware

Dedicated Meshtastic firmware for the **Heltec WiFi LoRa 32 V4 OLED** (`heltec-v4`).

This repository intentionally supports one hardware target only. It does not contain build variants or release workflows for Muziworks Superbase, Heltec V4 TFT, Heltec V4 R8, or any other board.

## Included improvements

- Persistent OLED disable with hardware power and reset isolation.
- Incoming messages do not wake a persistently disabled display.
- Holding PRG for approximately one second restores the OLED.
- GPS is disabled by default and is not initialized while disabled.
- Heltec-specific battery voltage curve and smoothed reported percentage.
- Raw-voltage low-battery protection at 3.50 V with 3.65 V recovery.
- Dynamic 40–80 MHz CPU operation while preserving Bluetooth discovery and reconnection.
- Status LED suppression while the display is persistently disabled.
- Correct MQTT implicit ACK handling.
- Unchanged LoRa transmit power, regional settings, and RX Boosted Gain behavior.

## Build target

```text
heltec-v4
```

Build locally with PlatformIO:

```bash
pio run -e heltec-v4
```

The GitHub Actions workflows in this repository compile and publish only this target.

## Installation

Use the normal `.bin` file for OTA updates that preserve configuration, identity, and keys. Use the `.factory.bin` file only for a clean installation when required.

## Upstream

Based on the open-source [Meshtastic firmware](https://github.com/meshtastic/firmware). Meshtastic is a registered trademark of Meshtastic LLC; this is an independent hardware-specific distribution.
