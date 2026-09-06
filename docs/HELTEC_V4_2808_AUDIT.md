# Heltec V4 v2.8.0.8 — release audit record

Date: 2026-09-06.

## Scope and provenance

This revision replaces the withdrawn `heltec-v4-profiles-v2.8.0.7` distribution. Its old source tag remains for traceability, not as an approved installation. Production targets remain `heltec-v4-standard` and `heltec-v4-solar-router`, both OLED. GPS remains disabled by default on a clean installation. No new upstream synchronization or non-Heltec hardware target is introduced by this repair.

The pre-release candidate is `5a789d2e9fc5a1fc2a017d3fdfe42a659a572d51`. Its complete validation is recorded by [GitHub Actions run 34038938560](https://github.com/Amoulier/meshtastic-heltec-v4-firmware/actions/runs/34038938560). This release-record commit adds documentation only to that candidate. The publication workflow must rebuild both profiles and repeat all required native groups at the publication commit; an earlier green run is not substituted for those gates.

## Closed findings from v2.8.0.7

- SX1262 hardware parking is separate from confirmed chip sleep. Parking, standby, reinitialization and radio activation invalidate the sleep confirmation. Repeated failed sleep cannot report success just because the hardware is parked. Solar critical shutdown therefore retains its NRESET fallback when sleep is unconfirmed.
- MQTT implicit ACK history uses a reserved/pending state, commits only after successful local admission, and releases the reservation on allocation/admission failure. Delivery occurs outside the history mutex, so a reentrant call cannot deadlock it. Pending reservations cannot be evicted by unrelated traffic. The independent LoRa retry/fallback path remains active, and an already-recorded MQTT success is not overwritten by MAX_RETRANSMIT.
- Rejecting an incoming known-compromised private key now uses a rejection-specific warning. It does not claim that the existing identity was replaced. Boot-time replacement of a stored weak identity remains a separate operation. Valid identity keys are preserved.
- The explicitly excluded `3683566f` text-message-frame banner suppression is absent. The custom atomic `displayDisabled` protection is retained. The excluded periodic heap diagnostics from `43155f3f` remain absent.

FEM normalization remains idempotent: `limitPower()` restarts from `requestedPower`, which is populated from the normalized configured request. Fault-injection tests repeat conversions for GC1109 and KCT8103L across regional limits and requested powers. They do not measure conducted or radiated RF output.

## Native regression repairs

The older native run reported 17 assertion failures and three suite-level execution errors. These were not 17 independent firmware defects. They included obsolete expectations and test state that leaked between cases.

### Administration

Each test now saves/restores module configuration and pending reboot/shutdown deadlines. The production rejection of mutations during a pending lifecycle transition remains enabled and has a dedicated negative test. Successful-operation helpers reject unexpected routing errors instead of treating them as a successful no-warning result.

Replies and config-change observers are cleaned in tearDown, including after a Unity assertion skips automatic C++ destructors. The restore test uses fixture-owned database/router resources, supplies the real crypto locking prerequisite, and verifies unchanged NodeNum and both identity keys after restoring and sanitizing licensed channels.

Mute tests explicitly start from an unmuted node rather than inheriting a persisted muted node from a preceding test. A pure node metadata mutation must write nodes.proto without changing config.proto, device.proto, channels.proto or module.proto; those other files are compared byte for byte. Empty keys are checked without making a zero-length Unity memory comparison.

### XMODEM

The complete upload must survive a later cancel. An unexpected EOT during a download must receive NAK, allow a subsequent ACK to advance to the next block, and preserve the source file when cancelled. Tests now require these corrected semantics instead of reintroducing historical state-confusion defects.

### Storage recovery

The repeated-load test uses a real user record, not an empty discovery that boot cleanup intentionally prunes. A save of a segment already classified as unreadable is rejected before retry/error escalation. The lower write boundary already rejected that operation; the new early check prevents a deliberate read-only rejection from being misreported as fresh flash corruption.

`PreferenceRecoveryPolicy.h` lets the native audit compile the board-independent Heltec recovery guards. It does not emulate ESP32 NVS, GPIO, FreeRTOS ownership, ADC, or power timing. The test-only macro is rejected outside native unit tests. The storage suite refuses to compile accidentally against the generic policy. Native fixture files are installed only in the CI workspace; they are not added to production targets.

## Pre-release candidate results

Both ESP32-S3 profiles compiled successfully at the candidate SHA. All required native suites ran with AddressSanitizer and passed. The runner verifies nonempty suites, exact suite attribution, no skipped required cases, and no assertion or execution errors.

| Native suite | Passing cases |
| --- | ---: |
| test_admin_radio | 129 |
| test_admin_session_repro | 25 |
| test_module_config | 3 |
| test_mqtt | 74 |
| test_reliable_ack_matrix | 38 |
| test_nexthop_routing | 49 |
| test_mesh_module | 31 |
| test_radio | 35 |
| test_gps_fix_hold | 12 |
| test_gps_update_scheduling | 15 |
| test_muted_source | 14 |
| test_fscommon_getfiles | 8 |
| test_safefile | 5 |
| test_nodedb_identity_hygiene | 18 |
| test_nodedb_legacy_migration | 9 |
| test_nodedb_v25_roundtrip | 9 |
| test_message_store_text | 3 |
| test_event_profile_storage | 10 |
| test_xmodem | 21 |
| test_phone_api_config_dump | 9 |
| test_packet_signing | 78 |
| test_crypto | 13 |
| test_channel_keys | 25 |
| test_waypoint_expiry | 11 |
| test_nodedb_boot_recovery (strict storage profile) | 19 |
| **Total** | **663** |

Additional candidate checks passed:

- Real production method bodies compiled with deterministic hardware/pool doubles under ASan and UBSan: SX1262 Standard/Solar sleep failures, repeated FEM normalization, MQTT failed admission and 32 concurrent duplicate deliveries.
- Generic, Standard and Solar power policy executables.
- Mutation checks in temporary copies: reintroducing false sleep success, compounded FEM conversion or failed-ACK deduplication made the respective regression fail at runtime.
- Twelve preprocessor-only comparisons of the recovery-guard refactor, excluding the intentionally added early rejection; these are not runtime or hardware tests.
- Synthetic tests of the audit-report gate rejected empty, misattributed, skipped, failed and errored reports. These are not included in the 663 firmware cases.
- Candidate packages: manifest sizes/MD5, artifact SHA256, embedded image checksum/SHA256, embedded ELF SHA256, exact normal app inside factory, partition table agreement and pinned Unified OTA payload. `has_mui` and `has_inkhud` are false. Source scope and installer shell syntax checks passed.

## Release acceptance and remaining physical tests

No unresolved software-test failure remains in the pre-release candidate. This is evidence from code review, host execution and ESP32-S3 compilation, not a guarantee that every possible defect has been excluded. The release workflow remains fail-closed on any required test/build/package failure and must not overwrite the withdrawn tag.

Physical hardware was **not tested in this audit**. Before unattended Solar deployment, verify the complete low-voltage cutoff/recovery cycle on the actual battery, charger and board. On the target device verify preserving OTA, identity/key retention, persistent OLED-off through RX/TX and alerts, PRG restoration, Bluetooth discovery/reconnection, MQTT/LoRa ACK coexistence, and SX1262 recovery. Actual RF power requires measurement; mathematical FEM checks are not such a measurement.

Use the profile-specific normal .bin for preserving OTA. Use the complete profile ZIP and its installer only for an explicitly intended clean installation. Do not erase a configured node merely to test these fixes.
