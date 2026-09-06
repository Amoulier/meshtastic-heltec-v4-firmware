# Meshtastic Heltec V4 Firmware Profiles

A focused Meshtastic firmware distribution for the **Heltec WiFi LoRa 32 V4 OLED**, developed and hardware-tested on the **Heltec V4.3 OLED**.

This repository keeps one maintained source tree but produces two explicit firmware profiles:

- **Standard** for regular personal, portable, mobile, or client nodes.
- **Solar Router** for fixed, unattended solar infrastructure deliberately configured as `ROUTER` or `ROUTER_LATE`.

The common goal is lower avoidable power consumption, protection against deep-discharge corruption, and predictable OLED, GPS, Bluetooth, and MQTT behavior without reducing LoRa reception or changing regional radio limits.

> This is an independent, hardware-specific distribution based on the official [Meshtastic firmware](https://github.com/meshtastic/firmware). It is not an official Meshtastic release.

## Choose the correct profile

| Profile          | PlatformIO environment   | Intended use                                                                               | Critical-battery behavior                                                                                                                        |
| ---------------- | ------------------------ | ------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------ |
| **Standard**     | `heltec-v4-standard`     | `CLIENT`, `CLIENT_MUTE`, `CLIENT_BASE`, tracker, handheld, mobile, and other regular nodes | Meshtastic's normal 3.10 V threshold, 10 confirming readings, and role-default wake behavior                                                     |
| **Solar Router** | `heltec-v4-solar-router` | Fixed, elevated, unattended solar infrastructure using `ROUTER` or `ROUTER_LATE`           | 3.50 V threshold, 3 confirming readings, timer-only sleep, peripheral isolation, and a 3.65 V recovery latch with an active USB data-host bypass |

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
- Geofence, motion/tap, and remotely injected input events cannot create a hidden display-wake interval while disabled.
- Keeps incoming packets stored and available to the phone.
- Persists across reboots and OTA updates.
- Holding **PRG for approximately one second** restores the display.

Because the onboard OLED and the exported VEXT/QuickLink connector share the GPIO36-controlled power rail, persistent **Disable Display** always holds the OLED in reset but keeps VEXT powered when the completed boot scan detects another I²C device. If no I²C accessory is found, GPIO36 turns VEXT off to save power. Unknown, UART, analog, or power-only accessories cannot be detected automatically; do not use persistent display disable when one of those devices depends on VEXT.

This is different from **Sleep Screen**, which is temporary and may wake on normal events.

### GPS power behavior

- GPS is disabled by default after a clean installation.
- GPS probing and initialization are skipped while GPS is disabled.
- OLED and GPS controls remain independent.
- GPS can be enabled normally when location services are required.
- A valid fix seen during a GPS search cycle remains credited when that cycle ends, even if its final poll does not produce another fix.

### Battery reporting and critical-write protection

- Early boot and normal telemetry use the same calibrated 15-sample ADC reader and persisted `adc_multiplier_override`. Changing the multiplier discards readings and filters from the previous scale; invalid calibration values and failed samples cannot authorize battery-powered storage writes.
- Uses profile-aligned voltage curves with the same calibrated Heltec V4 upper range.
- The Standard curve continues through the normal discharge tail toward 3.10 V instead of displaying 0% prematurely at 3.50 V.
- The Solar Router curve deliberately reaches 0% at its protective 3.50 V cutoff.
- Smooths the published battery percentage so brief LoRa transmission voltage sag does not create large temporary jumps.
- The displayed and telemetered percentage changes by no more than one percentage point per minute.
- Solar Router critical-voltage decisions use the latest unfiltered 15-sample ADC average, independently of the displayed voltage and percentage filters.
- Once a healthy battery has been observed, an abrupt sag below the normal battery-presence threshold remains eligible for the three-sample Solar Router cutoff; an open battery input at boot remains excluded.
- At each profile's own critical threshold, the shutdown path skips the optional NodeDB save to reduce brownout-related configuration risk.
- On battery, ordinary preference writes require a fresh usable reading of at least 3.30 V on Standard. Because the Solar Router cutoff is inclusive, its writes require more than 3.50 V (at least 3.501 V at the integer-millivolt gate). An active enumerated USB data host also authorizes the write; charge-only/VBUS detection does not.
- Transaction-free core saves deferred solely by this power gate are retried after safe power returns, but never during a pending reboot, shutdown, or critical-sleep transition. Reset, preference restore, and OTA entry use the stricter 3.65 V-or-USB-data-host gate.

Battery percentage remains an estimate derived from voltage. Load, temperature, battery chemistry, cell condition, and charging state can affect the reading.

### Storage and identity resilience

- Preference saves do not report success until the temporary file passes readback and the atomic replacement completes.
- A transient write failure is retried without formatting or wiping the preferences filesystem.
- Configuration, channel, module, and device-state files preserve the previous verified generation until their replacement is committed.
- At boot, the complete core generation is inventoried before any migration or automatic save. Config, channel, module, and device-state files must form one complete generation. `nodes.proto` may be absent only for a new keyless, unlicensed node; once private/config public key material, an owner public key, or licensed identity exists, it is required as well. A missing required peer or pending temporary therefore enters local recovery instead of manufacturing and persisting a mixed generation.
- A legacy preferences marker no longer causes an early-boot directory erase. The firmware prepares the same clean defaults in RAM, preserves the cryptographic identity, verifies all four identity/config-bearing replacement files, then clears the old node cache and removes the obsolete marker last. If the old identity file cannot be decoded, migration fails closed and leaves every file for a later recovery attempt.
- A failed LittleFS mount never triggers an automatic format on Heltec V4. The node instead starts fail-closed with LoRa transmission and GPS disabled, while Bluetooth remains available for diagnosis and an explicit full factory reset.
- While storage is unavailable, settings changes, preference writes, and PKI identity generation are rejected rather than being acknowledged only in RAM.
- A present but temporarily unreadable identity/config file receives the same fail-closed treatment even when LittleFS itself mounted: LoRa remains silent, ordinary mutations cannot overwrite it after boot, and config-only reset is refused because it cannot satisfy its identity-preservation contract.
- Recovery mode accepts only local administrative traffic over Bluetooth or USB. Its bounded recovery allowlist includes reboot/shutdown, DFU, supported BLE OTA, compatible preference restore and factory reset operations, plus NodeDB reset when the surviving state permits it. Destructive recovery operations require an active USB data-host connection or an initialized raw battery reading of at least 3.65 V. The raw ADC/USB condition is refreshed immediately before each destructive boundary. A charge-only cable with no detectable battery is deliberately insufficient; use a data-capable USB connection or the complete clean-install bundle. The PRG long-press display gesture does not format storage.
- Full reset removes unrelated files before the preference generation; once an explicitly authorized `/prefs` removal has begun, an incomplete directory cleanup falls back to a verified filesystem format instead of leaving a half-deleted generation.
- A config-only reset atomically replaces each identity/config-bearing file before deleting auxiliary preferences, so a failed write cannot first erase the persisted identity it promises to preserve. It requires a fresh verified node cache when identity is initialized and otherwise explicitly removes the old cache. Because the files are committed individually, a failed multi-file reset reports an error, keeps Bluetooth available, and may require a retry.
- Normal OTA updates preserve the existing configuration and every valid keypair. A legacy visible Node ID that does not match `CRC32(public_key)` may be realigned without replacing that valid keypair; selecting the first region on a clean keyless node creates its initial identity. A restored legacy low-entropy key is rejected and replaced for compatibility and security.
- Before entering the unified OTA loader, the requested transport and firmware hash must pass NVS readback verification; a persistence failure leaves the normal application selected.
- BLE OTA never restores stale Wi-Fi credentials from an earlier Wi-Fi OTA, including while recovering from unavailable storage.
- Message and waypoint persistence is fenced and drained before a factory reset touches LittleFS. A clear/autosave operation queued immediately before reset cannot resume inside the destructive transaction, and the reset verifies the empty stores before committing.
- Preference backups and restores validate complete segment sets, identity/keypair consistency, radio legality, and backup layout before replacing live files. A durable restore marker makes interruption retry-safe and nested backup artifacts are rejected.
- Message and waypoint stores use locked snapshots and verified atomic persistence. Each saved message owns its text instead of sharing a wrapping text pool, and malformed persisted records or notification flags are rejected.
- Multi-setting Admin edits are owned by the initiating local session, fenced from concurrent UI/HTTP/background writes, and committed as one marked generation. Invalid radio candidates are rejected before commit; after a valid generation commits, mesh traffic stays fenced until radio activation succeeds, and an activation failure leaves LoRa parked and schedules a guarded reboot.
- Settings begin with a lossless quiescing phase: new radio/network work is blocked while already admitted RX/TX drains, including a packet caught after dequeue. Only then is the radio parked; maintenance cannot rearm it and queues resume only after the transaction reaches its finished state. A one-shot mutation without this pre-mutation fence is rejected.
- First use of the event profile creates its config and channel files as one generation without deleting the inactive profile or its backup.

### Administrative and local-interface hardening

- Security changes validate key lengths/counts, private/public consistency, and known compromised or low-entropy keys before mutation. Curve25519 shared-secret bytes are no longer written to debug logs.
- XMODEM file transfer is limited to local BLE/serial transports, refuses preference, backup, and recovery-marker paths, and is disabled during recovery or storage transactions. A short upload removes the partial destination; cancelling a download does not delete its source.
- HTTP uploads and deletes are confined to the static-file tree, traversal and protected paths are rejected, and state readers are fenced while reset/restore replaces shared data.
- Role transitions undo only unchanged role-imposed messaging, telemetry, and position defaults, preserve later user customizations, and clear stale `is_unmessagable` state when the new role supports messaging.
- Canned-message, ringtone, and manual key-verification changes report persistence failure instead of acknowledging RAM-only state; updating canned text no longer implicitly changes the separate module-enable setting.
- MQTT configuration validation is side-effect free and does not open a live TCP probe before the candidate settings commit.

### Bluetooth and CPU power

- Enables dynamic CPU scaling between **40 and 80 MHz** to reduce idle consumption.
- While Bluetooth is enabled, idle sleep paths that would stop advertising are suppressed so reconnection stays available.
- If Bluetooth is explicitly disabled, the normal Router or power-saving sleep path remains available instead of paying the BLE availability cost.
- Disabling Bluetooth from the physical menu verifies the saved choice before performing a controlled restart, so a write failure keeps the previous working state instead of silently reverting after reboot.
- Disabling the OLED does **not** disconnect Bluetooth.
- A phone can discover, connect, disconnect, and reconnect without pressing PRG, including when the node role is `ROUTER` or `ROUTER_LATE`.
- If a client abandons a multi-setting Admin transaction, its real idle timeout still commits the applied values and performs any restart required to rebuild Bluetooth/power state.

### LED behavior

While persistent display-off mode is active, the status and pairing LED remain dark, including on headless timer wakes. This reduces unnecessary consumption without changing Bluetooth state or packet handling.

### MQTT acknowledgement correction

The fork preserves successful MQTT implicit acknowledgement state without cancelling the independent LoRa retry path. If LoRa retries later expire, the firmware does not overwrite an already successful MQTT delivery result with a contradictory `MAX_RETRANSMIT` failure.

Normal LoRa acknowledgements and routing behavior remain intact.

### Radio recovery and unchanged operating limits

If the SX1262 loses its runtime state after a transient reset or brownout, the firmware reinitializes it in place and periodically rearms receive mode. A failed channel-activity scan is recovered and retried once, then fails closed by deferring transmission. During Solar Router critical sleep, a radio that cannot confirm sleep is held in reset. Recovery always recalculates chip power from the configured request, so repeated recovery cannot compound the GC1109/KCT8103L FEM conversion.

LoRa settings are validated before persistence and again before modem programming. Safe corrections, including a compatible EU sibling-region preset, are applied before the active region pointer is selected. An unknown or physically unsupported candidate is rejected before commit, leaving the current files and runtime unchanged rather than guessing a regulatory region.

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
6. Without an active enumerated USB data host, the node remains in recovery sleep until the battery reaches approximately **3.65 V**, preventing rapid boot/sleep oscillation. A data-host connection bypasses the latch so an operator can perform local recovery.

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

Profile isolation is additionally checked by host policy tests and by compiling the Standard and Solar Router environments independently in GitHub Actions. Before unattended deployment, validate the complete Solar Router low-voltage sleep and recovery cycle on the target board, battery, regulator, and solar supply.

## Installation

Open the repository's [Releases](https://github.com/Amoulier/meshtastic-heltec-v4-firmware/releases) page and select the latest stable profile release.

The distribution revision is `v2.8.0.9` (full tag: `heltec-v4-profiles-v2.8.0.9`). Its manifests, firmware filenames, ESP application descriptor, and Meshtastic client **About** view identify the compiled image as `2.8.0-h9g<8-character fingerprint>`. Local builds with modified sources use `h9d` instead of `h9g`; the compact OLED footer displays `2.8.0-h9`. The manifest records the full source and build-identity SHA256, source commit, and each image's SHA256. Rebuild after committing source changes; renaming a previous image does not change its embedded identity.

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

### OTA update - recommended

Use the normal `.bin` image that does not contain `.factory` in its filename. An OTA or normal firmware update preserves the node configuration, every valid keypair, Bluetooth bonds, and persistent display setting. The visible Node ID exceptions described above still apply to a legacy mismatched identity, a clean keyless node, or a rejected low-entropy key.

### Wired update

Download and extract the matching `complete` ZIP so the profile-specific normal `.bin` and its `.mt.json` manifest remain together, then run `device-update.sh` or `device-update.bat` with that normal image. The individually published `.bin` is intended for OTA clients and is not, by itself, a complete input for the manifest-verifying wired updater.

### Clean installation

Download the matching `complete` ZIP, extract every file into one directory, and run its `device-install.sh` or `device-install.bat` with the included `.factory.bin`. The installer erases flash and writes the factory image, unified OTA loader, and LittleFS image as one complete installation.

Both clean-install and wired-update scripts require esptool 4.5.1 or newer and an explicit serial port, verify the connected chip as ESP32-S3 with 16 MB flash, bind the selected profile/version filename exactly to the manifest target/version, require the exact Heltec V4 repository and partition layout, and validate file sizes and digests before erasing or writing. The POSIX scripts also require `jq`; the clean installer additionally verifies the pinned unified OTA loader.

Do **not** erase flash and then write the `.factory.bin` alone. That application image does not contain LittleFS; with automatic formatting intentionally disabled for identity protection, an incomplete installation will boot in fail-closed recovery mode. The release therefore keeps `.factory.bin` inside the complete ZIP rather than publishing it as a standalone asset.

Any clean installation removes configuration, keys, Bluetooth bonds, and node identity.

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

Each release provides one profile-specific normal `.bin` for OTA updates plus one complete clean-install ZIP for each profile.

The build uses the official Meshtastic ESP32-S3 container by immutable image digest, SHA-pinned GitHub Actions, and a SHA-256-pinned unified OTA loader. Publication requires both profile builds, native integration tests, host regressions for calibration and power policies, source-invariant linters, and exact manifest/file verification.

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

The common source base incorporates official Meshtastic changes through:

```text
7239fe886a30fa13cd35946fa5ae1a46a2807eeb
```

It also includes selected, audited stability backports from later `develop` history through `fdb67309aa8fb9a019e07160ac72024c3d25ce2d`: filesystem allocation safety, administrative BLE handling, muted-notification behavior, radio state-loss recovery, GPS search-cycle retention, and legacy low-entropy PKI rejection. This remains a 2.8.0-based selective backport and does not claim full synchronization with the in-development 2.8.1 source line.

Because this is a standalone repository rather than a formal GitHub fork, the exact upstream source commit is recorded in synchronization commits and release notes.

## License and trademark

This project retains the upstream Meshtastic license and applicable third-party licenses. Meshtastic is a registered trademark of Meshtastic LLC. This repository and its releases are independently maintained and are not endorsed as official Meshtastic builds.
