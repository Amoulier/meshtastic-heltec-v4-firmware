# Meshtastic Heltec V4 Power-Optimized Firmware

A focused Meshtastic firmware distribution for the **Heltec WiFi LoRa 32 V4 OLED** PlatformIO target (`heltec-v4`), developed and hardware-tested on the **Heltec V4.3 OLED**.

The purpose of this repository is to improve unattended battery operation, protect configuration data during deep discharge, and provide predictable display, GPS, Bluetooth, and MQTT behavior without reducing LoRa reception or changing regional radio limits.

> This is an independent, hardware-specific distribution based on the official [Meshtastic firmware](https://github.com/meshtastic/firmware). It is not an official Meshtastic release.

## Hardware scope

This repository intentionally supports one firmware target:

```text
heltec-v4
```

It does **not** build or publish firmware for:

- Heltec V4 TFT
- Heltec V4 R8
- Muziworks Superbase
- Any other ESP32, nRF52, RP2040, STM32, Portduino, or Linux target

The source tree, PlatformIO configuration, GitHub Actions workflows, and releases are kept specific to the Heltec V4 OLED.

## What this fork changes

### Persistent OLED power control

A dedicated **Display Options → Disable Display** command provides a true persistent display-off mode.

- Turns off the OLED power rail and holds the OLED reset line low.
- Prevents messages, waypoints, notifications, UI rendering, and automatic wake events from powering the display.
- Keeps incoming packets stored and available to the phone.
- Persists across reboots and OTA updates.
- Holding **PRG for approximately one second** restores the display.

This is different from **Sleep Screen**, which is a temporary screen sleep and may wake on normal events.

### GPS power behavior

- GPS is disabled by default after a clean installation.
- GPS probing and initialization are skipped while GPS is disabled.
- OLED and GPS controls remain independent.
- Users can enable GPS normally from Meshtastic when location services are required.

### Battery reporting and storage protection

- Uses a voltage curve calibrated for the usable discharge range observed on the Heltec V4.3 test node.
- Smooths the published battery percentage so brief LoRa transmission voltage sag does not create large temporary jumps.
- The displayed and telemetered percentage changes by no more than one percentage point per minute.
- Low-voltage protection continues to use the **raw, unsmoothed voltage**.
- At sustained readings of **3.50 V or below**, the node enters protective deep sleep.
- Normal boot is held until the battery recovers to approximately **3.65 V**.
- Optional flash writes are skipped during critical-voltage shutdown to reduce the risk of configuration corruption or loss.

Battery percentage remains an estimate derived from voltage; load, temperature, battery chemistry, cell condition, and charging state can affect the reading.

### Bluetooth and CPU power

- Enables dynamic CPU scaling between **40 and 80 MHz** for lower idle consumption.
- Automatic light sleep remains disabled so Bluetooth advertising and reconnection stay available.
- Disabling the OLED does **not** disconnect Bluetooth.
- A phone can discover, connect, disconnect, and reconnect without pressing PRG.

### LED behavior

While persistent display-off mode is active, the status and pairing LED remain dark. This reduces unnecessary consumption without changing Bluetooth state or packet handling.

### MQTT acknowledgement correction

The fork preserves successful MQTT implicit acknowledgement state without cancelling the independent LoRa retry path. If LoRa retries later expire, the firmware does not overwrite an already successful MQTT delivery result with a contradictory `MAX_RETRANSMIT` failure.

Normal LoRa acknowledgements and routing behavior remain intact.

### Radio behavior intentionally unchanged

The power optimizations do not change:

- Regional LoRa limits
- Configured transmit power
- RX Boosted Gain behavior
- LoRa FEM power-control behavior
- Normal packet reception, retransmission, or mesh participation

## Validated behavior

The current implementation has been hardware-tested for:

- Persistent OLED disable and PRG restoration
- Public-channel and private-message reception while the OLED remains off
- Bluetooth discovery and reconnection with the OLED disabled
- MQTT implicit ACK behavior
- Configuration persistence through OTA installation
- GPS-disabled operation
- Battery telemetry under real LoRa traffic
- Stable temperature and absence of unexpected reboots during the validation period

## Installation

Download the latest package from [Releases](https://github.com/Amoulier/meshtastic-heltec-v4-firmware/releases/latest).

### OTA update — recommended

Use:

```text
firmware-heltec-v4-*.bin
```

An OTA or normal firmware update is recommended because it preserves the node configuration, cryptographic identity, keys, and persistent display setting.

### Clean installation

Use:

```text
firmware-heltec-v4-*.factory.bin
```

The factory image is intended for recovery or a deliberately clean installation. Erasing flash may remove configuration, keys, Bluetooth bonds, and node identity.

Always connect the correct regional antenna before operating the LoRa radio.

## Build

Build locally with PlatformIO:

```bash
pio run -e heltec-v4
```

GitHub Actions compiles only `heltec-v4`. A release is published only from an explicit commit whose message begins with:

```text
release(heltec-v4):
```

Each release contains the normal OTA firmware, factory image, LittleFS image, manifest, installation scripts, ELF file, and ESP32-S3 unified OTA component.

## Upstream synchronization

The fork is periodically audited and synchronized with the official Meshtastic `develop` branch. Upstream changes are reviewed specifically for conflicts with:

- OLED persistence and wake suppression
- Bluetooth availability
- Battery percentage filtering
- Critical-voltage storage protection
- GPS initialization
- MQTT and LoRa acknowledgement routing
- Radio and FEM power control

The current firmware line incorporates official Meshtastic changes through:

```text
7239fe886a30fa13cd35946fa5ae1a46a2807eeb
```

Because this is a standalone repository rather than a formal GitHub fork, the exact upstream source commit is recorded in synchronization commits and release notes.

## License and trademark

This project retains the upstream Meshtastic license and applicable third-party licenses. Meshtastic is a registered trademark of Meshtastic LLC. This repository and its releases are independently maintained and are not endorsed as official Meshtastic builds.
