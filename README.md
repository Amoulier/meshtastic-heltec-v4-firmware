# Meshtastic Heltec V4 Firmware Profiles

A focused Meshtastic firmware distribution for the **Heltec WiFi LoRa 32 V4 OLED**, developed and hardware-tested on the **Heltec V4.3 OLED**.

This repository keeps one maintained source tree but produces two explicit firmware profiles:

- **Standard** for regular personal, portable, mobile, or client nodes.
- **Solar Router** for fixed, unattended solar infrastructure deliberately configured as `ROUTER` or `ROUTER_LATE`.

The common goal is lower avoidable power consumption, protection against deep-discharge corruption, and predictable OLED, GPS, Bluetooth, and MQTT behavior without reducing LoRa reception or changing regional radio limits.

> This is an independent, hardware-specific distribution based on the official [Meshtastic firmware](https://github.com/meshtastic/firmware). It is not an official Meshtastic release.

## Choose the correct profile

| Profile | PlatformIO environment | Intended use | Critical-battery behavior |
| --- | --- | --- | --- |
| **Standard** | `heltec-v4-standard` | `CLIENT`, `CLIENT_MUTE`, `CLIENT_BASE`, tracker, handheld, mobile, and other regular nodes | Meshtastic's normal 3.10 V threshold, 10 confirming readings, and role-default wake behavior |
| **Solar Router** | `heltec-v4-solar-router` | Fixed, elevated, unattended solar infrastructure using `ROUTER` or `ROUTER_LATE` | 3.50 V threshold, 3 confirming readings, timer-only sleep, peripheral isolation, and a 3.65 V recovery latch |

The firmware profile does **not** change the node role automatically. Select the intended role separately in the Meshtastic app or CLI.

Do not use an advanced routing role merely because a node is stationary. Most personal and mobile nodes should remain on a client role. Use the Solar Router profile only where the node is intentionally part of fixed routing infrastructure and has an appropriate antenna, location, battery, and solar supply.

### Default and legacy build targets

Running PlatformIO without an explicit environment builds the safe regular-node profile:

```bash
pio run
```

which resolves to:

```text
heltec-v4-standard
```

The historical PlatformIO environment:

```text
heltec-v4
```

remains as an explicit compatibility alias for `heltec-v4-solar-router`. New releases publish only the explicit `standard` and `solar-router` filenames so the installed behavior is unambiguous.

## Hardware scope

This repository intentionally supports only the Heltec V4 OLED family represented by these profiles. It does **not** build or publish firmware for:

- Heltec V4 TFT
- Heltec V4 R8
- Muziworks Superbase
- Any other ESP32, nRF52, RP2040, STM32, Portduino, or Linux target

## Improvements shared by both profiles

### Persistent OLED power control

A dedicated **Display Options → Disable Display** command provides a true persistent display-off mode.

- Turns off the OLED power rail and holds the OLED reset line low.
- Prevents messages, waypoints, notifications, UI rendering, and automatic wake events from powering the display.
- Keeps incoming packets stored and available to the phone.
- Persists across reboots and OTA updates.
- Holding **PRG for approximately one second** restores the display.

This is different from **Sleep Screen**, which is temporary and may wake on normal events.

### GPS power behavior

- GPS is disabled by default after a clean installation.
- GPS probing and initialization are skipped while GPS is disabled.
- OLED and GPS controls remain independent.
- GPS can be enabled normally when location services are required.

### Battery reporting and critical-write protection

- Uses profile-aligned voltage curves with the same calibrated Heltec V4 upper range.
- The Standard curve continues through the normal discharge tail toward 3.10 V instead of displaying 0% prematurely at 3.50 V.
- The Solar Router curve deliberately reaches 0% at its protective 3.50 V cutoff.
- Smooths the published battery percentage so brief LoRa transmission voltage sag does not create large temporary jumps.
- The displayed and telemetered percentage changes by no more than one percentage point per minute.
- Critical-voltage decisions continue to use the raw, unsmoothed voltage.
- At each profile's own critical threshold, the shutdown path skips the optional NodeDB save to reduce brownout-related configuration risk.

Battery percentage remains an estimate derived from voltage. Load, temperature, battery chemistry, cell condition, and charging state can affect the reading.

### Bluetooth and CPU power

- Enables dynamic CPU scaling between **40 and 80 MHz** to reduce idle consumption.
- Automatic light sleep remains disabled so Bluetooth advertising and reconnection stay available.
- Disabling the OLED does **not** disconnect Bluetooth.
- A phone can discover, connect, disconnect, and reconnect without pressing PRG.

### LED behavior

While persistent display-off mode is active, the status and pairing LED remain dark. This reduces unnecessary consumption without changing Bluetooth state or packet handling.

### MQTT acknowledgement correction

The fork preserves successful MQTT implicit acknowledgement state without cancelling the independent LoRa retry path. If LoRa retries later expire, the firmware does not overwrite an already successful MQTT delivery result with a contradictory `MAX_RETRANSMIT` failure.

Normal LoRa acknowledgements and routing behavior remain intact.

### Radio behavior intentionally unchanged

Neither profile changes:

- Regional LoRa limits
- Configured transmit power
- RX Boosted Gain behavior
- LoRa FEM power-control behavior during normal operation
- Normal packet reception, retransmission, or mesh participation

## Solar Router safeguards

The `heltec-v4-solar-router` profile adds aggressive protection intended for an unattended node that must survive poor solar conditions:

1. Three consecutive raw readings at or below **3.50 V** trigger protective deep sleep.
2. External wake sources are disabled; recovery checks use a timer only.
3. The OLED, GPS, LED, LoRa FEM, radio state, and retained power domains are forced into their lowest safe state.
4. A bounded preflight prevents the shutdown path from hanging indefinitely on a busy subsystem.
5. After a critical shutdown, early boot checks battery voltage before starting Meshtastic.
6. The node remains in recovery sleep until the battery reaches approximately **3.65 V**, preventing rapid boot/sleep oscillation.

The elevated cutoff, timer-only wake policy, forced peripheral isolation, and boot-recovery latch are intentionally excluded from `heltec-v4-standard`. A regular node therefore retains normal button/external-wake behavior and does not enter the Solar Router recovery loop.

## Validated behavior

The common implementation has been hardware-tested for:

- Persistent OLED disable and PRG restoration
- Public-channel and private-message reception while the OLED remains off
- Bluetooth discovery and reconnection with the OLED disabled
- MQTT implicit ACK behavior
- Configuration persistence through OTA installation
- GPS-disabled operation
- Battery telemetry under real LoRa traffic
- Stable temperature and absence of unexpected reboots during the validation period

Profile isolation is additionally checked by host policy tests and by compiling the Standard and Solar Router environments independently in GitHub Actions. The Solar Router critical-discharge path remains published as a prerelease until a complete low-voltage sleep and solar-recovery cycle is validated on the target installation.

## Installation

Open the repository's [Releases](https://github.com/Amoulier/meshtastic-heltec-v4-firmware/releases) page. Profile releases are initially marked as prereleases while the complete Solar Router discharge-and-recovery cycle is validated on hardware.

### Standard node

Use files beginning with:

```text
firmware-heltec-v4-standard-
```

### Solar router

Use files beginning with:

```text
firmware-heltec-v4-solar-router-
```

### OTA update — recommended

Use the normal `.bin` image that does not contain `.factory` in its filename. An OTA or normal firmware update preserves the node configuration, cryptographic identity, keys, Bluetooth bonds, and persistent display setting.

### Clean installation

Use the `.factory.bin` image only for recovery or a deliberately clean installation. Erasing flash may remove configuration, keys, Bluetooth bonds, and node identity.

Always connect the correct regional antenna before operating the LoRa radio.

## Build

Build a regular-node image:

```bash
pio run -e heltec-v4-standard
```

Build the dedicated solar-router image:

```bash
pio run -e heltec-v4-solar-router
```

The legacy command remains available and maps to Solar Router:

```bash
pio run -e heltec-v4
```

GitHub Actions compiles both explicit profiles. A release is published only from an explicit commit whose message begins with:

```text
release(heltec-v4):
```

Each release provides directly flashable profile-specific binaries plus a complete ZIP bundle for each profile.

## Upstream synchronization

The fork is periodically audited and synchronized with the official Meshtastic `develop` branch. Upstream changes are reviewed specifically for conflicts with:

- OLED persistence and wake suppression
- Bluetooth availability
- Battery percentage filtering
- Critical-voltage storage protection
- GPS initialization
- MQTT and LoRa acknowledgement routing
- Radio and FEM power control
- Standard versus Solar Router profile isolation

The current firmware line incorporates official Meshtastic changes through:

```text
7239fe886a30fa13cd35946fa5ae1a46a2807eeb
```

Because this is a standalone repository rather than a formal GitHub fork, the exact upstream source commit is recorded in synchronization commits and release notes.

## License and trademark

This project retains the upstream Meshtastic license and applicable third-party licenses. Meshtastic is a registered trademark of Meshtastic LLC. This repository and its releases are independently maintained and are not endorsed as official Meshtastic builds.
