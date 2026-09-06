#!/usr/bin/env python3
"""Fail closed if the Heltec-only build or release contract drifts."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def function_body(source: str, signature: str) -> str:
    """Return one C++ function body, including its braces."""
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : index + 1]
    raise AssertionError(f"unterminated function body: {signature}")


release = read(".github/workflows/release_heltec_v4_power.yml")
build = read(".github/workflows/build_firmware.yml")

# Only the two explicit profiles and the documented Solar Router compatibility
# alias may be concrete PlatformIO environments in this single-target fork.
ini_text = read("platformio.ini")
for path in (ROOT / "variants").rglob("*.ini"):
    ini_text += "\n" + path.read_text(encoding="utf-8")
configured_envs = set(re.findall(r"^\[env:([^]]+)]", ini_text, re.MULTILINE))
require(
    configured_envs == {"heltec-v4-standard", "heltec-v4-solar-router", "heltec-v4"},
    f"unexpected concrete PlatformIO environments: {sorted(configured_envs)}",
)

release_envs = re.findall(r"^\s+pio_env:\s*([^\s#]+)", release, re.MULTILINE)
require(
    release_envs == ["heltec-v4-standard", "heltec-v4-solar-router"],
    f"release workflow targets changed: {release_envs}",
)
for expected in ("heltec-v4-standard", "heltec-v4-solar-router"):
    require(expected in build, f"build workflow does not allow {expected}")
for forbidden in ("superbase", "heltec-v4-r8", "heltec-v4-tft"):
    require(forbidden not in release.lower(), f"release workflow contains forbidden target {forbidden}")

# Publication is intentionally four assets: two normal OTA images and two
# complete clean-install bundles. Factory images must stay inside the bundles.
require(
    'test "$(find publish -maxdepth 1 -type f | wc -l)" -eq 4' in release,
    "release asset-count gate is not exactly four",
)
require("files: publish/*" in release, "release action is not limited to the verified publish directory")
copy_lines = [line for line in release.splitlines() if re.match(r"\s*cp\s", line)]
require(not any("factory.bin" in line or ".mt.json" in line for line in copy_lines), "standalone recovery assets are published")

# Capture ZIP listings before matching permissions. Under `set -o pipefail`, a
# producer piped to an early-exiting matcher can turn an otherwise valid bundle
# into a SIGPIPE-dependent result.
require(
    "standard_zip_listing=$(zipinfo -l publish/heltec-v4-standard-complete-v2.8.0.8.zip)" in release,
    "Standard ZIP listing is not captured before validation",
)
require(
    "solar_zip_listing=$(zipinfo -l publish/heltec-v4-solar-router-complete-v2.8.0.8.zip)" in release,
    "Solar Router ZIP listing is not captured before validation",
)
require(re.search(r"zipinfo[^\n]*\|\s*grep", release) is None, "ZIP permission validation reintroduced a producer pipeline")
for listing in ("standard_zip_listing", "solar_zip_listing"):
    require(
        f'grep -E \'^-rwxr-xr-x .* device-install\\.sh$\' <<<"${listing}"' in release,
        f"{listing} does not verify device-install.sh permissions",
    )
    require(
        f'grep -E \'^-rwxr-xr-x .* device-update\\.sh$\' <<<"${listing}"' in release,
        f"{listing} does not verify device-update.sh permissions",
    )

# A release must be a deliberate main-branch commit, and an old draft with the
# same tag must block publication instead of being silently adopted.
require("github.ref == 'refs/heads/main'" in release, "release is not gated to main")
require(
    "startsWith(github.event.head_commit.message, 'release(heltec-v4):')" in release,
    "release commit-prefix gate is missing",
)
require(release.count("verify_release_tag_absent_including_drafts") == 4, "draft-inclusive tag gate must run twice")
require(release.count("/releases?per_page=100&page=${page}") == 2, "draft-inclusive release inventory must run twice")
require("/releases/tags/" not in release, "draft-blind release lookup was reintroduced")
require(release.count("if ! remote_tag=$(git ls-remote") == 2, "tag-ref checks must fail closed on transport errors")
require(release.count('if [[ -n "$remote_tag" ]]') == 2, "tag-ref checks must reject an existing tag")
require(
    release.count('type == "array" and all(.[]; (type == "object") and (.tag_name | type == "string"))') == 2,
    "release inventories must validate every release entry",
)
require(release.count('case "$match_status" in') == 2, "release inventory jq status must be handled explicitly")
require("overwrite_files: false" in release, "release assets may overwrite an existing payload")
require("cuts the shared OLED/VEXT rail" not in release, "release overstates VEXT shutdown behavior")
require(
    "VEXT/QuickLink rail is also turned off when the completed boot scan found no other I2C accessory" in release,
    "release does not explain accessory-aware VEXT preservation",
)
menu_handler = read("src/graphics/draw/MenuHandler.cpp")
require("Disable OLED?\\nVEXT off if no I2C\\nHold PRG to restore" in menu_handler, "OLED confirmation misstates VEXT behavior")

# Keep every human-facing package version synchronized, while the firmware's
# own 2.8.0.<git-sha> version remains independently derived by buildinfo.py.
package_versions = set(re.findall(r"v\d+\.\d+\.\d+\.\d+", release))
require(len(package_versions) == 1, f"release package versions disagree: {sorted(package_versions)}")
require(package_versions == {"v2.8.0.8"}, f"unexpected planned package version: {sorted(package_versions)}")
require(
    release.count("heltec-v4-profiles-v2.8.0.8") == 3,
    "release tag must match in both provenance gates and the publisher",
)
require('firmware_version="2.8.0.${GITHUB_SHA::7}"' in release, "internal firmware version contract changed")
base_version = read("version.properties")
for component, value in (("major", "2"), ("minor", "8"), ("build", "0")):
    require(
        re.search(rf"^\s*{component}\s*=\s*{value}\s*$", base_version, re.MULTILINE) is not None,
        f"version.properties {component} no longer matches the 2.8.0 release base",
    )

# External actions and the build container must remain immutable references.
for line in release.splitlines() + build.splitlines():
    match = re.match(r"\s*(?:-\s*)?uses:\s*([^\s#]+)", line)
    if match and not match.group(1).startswith("./"):
        require(re.search(r"@[0-9a-f]{40}$", match.group(1)) is not None, f"GitHub Action is not SHA-pinned: {match.group(1)}")
require(
    re.search(r"ghcr\.io/meshtastic/gh-action-firmware@sha256:[0-9a-f]{64}", build) is not None,
    "firmware build container is not digest-pinned",
)
esp32_platform = read("variants/esp32/esp32-common.ini")
require("archive/refs/heads/" not in esp32_platform, "ESP32 build platform uses a mutable branch archive")
require(
    "pioarduino-platform-espressif32/archive/f89295f6a617a8ec611f4bd4eb2f998799dc5dc8.zip" in esp32_platform,
    "ESP32 build platform is not pinned to the audited revision",
)
require("persist-credentials: false" in build, "checkout credentials remain exposed to the build container")
require("< <(" not in build + release, "workflow masks a producer failure behind process substitution")

# Installer accept-lists must match the same two profiles and exclude lookalike
# board names. This checks release inputs, not just build selection.
for relative in ("bin/device-install.sh", "bin/device-update.sh", "bin/device-install.bat", "bin/device-update.bat"):
    installer = read(relative).lower()
    require("heltec-v4-standard" in installer, f"{relative} does not accept Standard")
    require("heltec-v4-solar-router" in installer, f"{relative} does not accept Solar Router")
    require("4.5.1" in installer, f"{relative} does not enforce the minimum supported esptool version")
    for forbidden in ("superbase", "heltec-v4-r8", "heltec-v4-tft"):
        require(forbidden not in installer, f"{relative} accepts or references forbidden target {forbidden}")

for relative, exact_name in (
    ("bin/device-install.sh", 'EXPECTED_FACTORY_BASENAME="firmware-${TARGET}-${VERSION}.factory.bin"'),
    ("bin/device-update.sh", 'EXPECTED_FIRMWARE_BASENAME="firmware-${TARGET}-${VERSION}.bin"'),
):
    installer = read(relative)
    require(exact_name in installer, f"{relative} does not bind the selected filename to manifest target/version")
    require("ESPTOOL_VERSION_MAJOR" in installer, f"{relative} does not parse and compare the esptool version")
    require('grep --quiet write-flash <<<"$ESPTOOL_HELP"' in installer, f"{relative} lost its esptool v4/v5 syntax probe")

for relative in ("bin/device-install.bat", "bin/device-update.bat"):
    raw = (ROOT / relative).read_bytes()
    require(b"\r\n" in raw and raw.replace(b"\r\n", b"").find(b"\n") == -1, f"{relative} must use consistent CRLF")
    require(b"[Version 2.7.0]" not in raw, f"{relative} reports the obsolete upstream script version")

# A settings generation is changed only after the radio and Router have
# handed off all old-generation work. These source-order checks pin the
# ownership barriers that cannot be exercised by the release-policy host.
radio_lib = read("src/mesh/RadioLibInterface.cpp")
radio_lib_header = read("src/mesh/RadioLibInterface.h")
router_source = read("src/mesh/Router.cpp")
router_header = read("src/mesh/Router.h")
nodedb_source = read("src/mesh/NodeDB.cpp")
nodedb_header = read("src/mesh/NodeDB.h")

on_notify = function_body(radio_lib, "void RadioLibInterface::onNotify(")
start_send = function_body(radio_lib, "bool RadioLibInterface::startSend(")
can_park = function_body(radio_lib, "bool RadioLibInterface::canParkForConfig(")
radio_send = function_body(radio_lib, "ErrorCode RadioLibInterface::send(")
pending_tx = function_body(radio_lib, "bool RadioLibInterface::hasPendingTransmissionsForConfig(")
maintenance = function_body(radio_lib, "void RadioLibInterface::periodicRadioMaintenance(")
begin_edit = function_body(nodedb_source, "bool NodeDB::beginPreferenceEdit(")
cancel_edit = function_body(nodedb_source, "bool NodeDB::cancelPreferenceEdit(")
finish_activation = function_body(nodedb_source, "bool NodeDB::finishPreferenceEditActivation(")
park_destructive = function_body(nodedb_source, "bool parkHeltecRadioForStorageMutation(")
router_run = function_body(router_source, "int32_t Router::runOnce(")

require(
    "enum class PreferenceEditState : uint8_t { NONE, QUIESCING, OPEN, COMMITTING, ACTIVATING }" in nodedb_header,
    "settings edit no longer has an explicit quiescing generation fence",
)
require(
    on_notify.index("radioNotificationDepth.fetch_add") < on_notify.index("startSend(txp)")
    < on_notify.rindex("radioNotificationDepth.fetch_sub"),
    "radio worker ownership no longer spans dequeue through hardware start",
)
require(on_notify.count("startSend(txp)") == 1, "unexpected startSend call count inside the radio worker")
dequeue = on_notify.index("txp = txQueue.dequeue()")
require(
    on_notify.rfind("isPreferenceEditTransactionActive()", 0, dequeue) >= 0,
    "radio worker no longer rechecks the transaction immediately before dequeue",
)
require(
    start_send.index("isPreferenceEditTransactionActive()") < start_send.index("configDeferredPacket = txp")
    < start_send.index("configHardwareForSend()"),
    "a dequeued packet can cross the settings fence or be lost before hardware start",
)
require(
    can_park.count("radioNotificationDepth.load") >= 2
    and can_park.count("isSending()") >= 2
    and can_park.count("isIRQPending()") >= 2,
    "radio parking lost its stable post-probe ownership recheck",
)
require(
    ("hasConfigDeferredPacket()" in pending_tx or "configDeferredPacket != nullptr" in pending_tx)
    and "!txQueue.empty()" in pending_tx
    and "std::atomic<bool> configResumeRequested" in radio_lib_header,
    "deferred TX ownership or durable resume tracking is missing",
)
require(
    radio_send.index("sendAdmissionDepth.fetch_add") < radio_send.index("isPreferenceEditTransactionActive()")
    < radio_send.index("txQueue.enqueue"),
    "a producer can enqueue old-generation ciphertext outside the radio admission depth",
)
require(
    "sendAdmissionDepth.load" in can_park and can_park.count("sendAdmissionDepth.load") >= 2,
    "radio parking can pass while a pre-fence producer still owns send admission",
)
require(
    "isPreferenceEditQuiescing()" in radio_send and "hasCurrentExternalStateAccess()" in radio_send,
    "QUIESCING does not distinguish an admitted old-generation producer from new work",
)
require(
    "isPreferenceEditTransactionActive() || nodeDB->isDestructiveStorageMutationActive()" in maintenance,
    "radio maintenance can rearm hardware while storage owns the generation",
)

quiescing = begin_edit.index("PreferenceEditState::QUIESCING")
first_wait = begin_edit.index("waitForRadioQuiesce()", quiescing)
sleep = begin_edit.index("radio->sleep()", first_wait)
post_sleep_wait = begin_edit.index("waitForRadioQuiesce()", sleep)
open_state = begin_edit.rindex("PreferenceEditState::OPEN")
require(
    quiescing < first_wait < sleep < post_sleep_wait < open_state,
    "settings edit no longer drains radio ownership before exposing mutable RAM",
)
require(
    begin_edit.count("hasPendingTransmissionsForConfig()") >= 2
    and begin_edit.count("hasPendingRadioPacketsForConfig()") >= 2,
    "settings edit no longer rejects old-generation work at both quiesce boundaries",
)
for body, operation in ((cancel_edit, "cancel"), (finish_activation, "finish")):
    none = body.rindex("PreferenceEditState::NONE")
    require(
        none < body.index("resumeQueuedTransmissions()", none) < body.index("setReceivedMessage()", none),
        f"settings {operation} wakes queues before publishing the NONE generation",
    )

require(
    router_run.index("radioPacketRunDepth.fetch_add") < router_run.index("fromRadioQueue.dequeuePtr")
    and router_run.index("fromRadioQueue.dequeuePtr") < router_run.index("configDeferredReceivedPacket.compare_exchange_strong"),
    "Router no longer fences or preserves a packet dequeued across QUIESCING",
)
require(
    router_run.index("perhapsHandleReceived(mp)")
    < router_run.index("configDeferredReceivedPacket.load", router_run.index("perhapsHandleReceived(mp)")),
    "Router can dequeue a second packet after reader admission filled its sole deferred slot",
)
require(
    "radioPacketRunDepth.load" in router_source
    and "radioPacketRunOwnerTask.load" in router_source
    and "configDeferredReceivedPacket.load" in router_source
    and "!fromRadioQueue.isEmpty()" in router_source,
    "Router parking predicate omits an old-generation packet owner",
)
require(
    router_source.index("radioPacketRunOwnerTask.store", router_source.index("int32_t Router::runOnce("))
    < router_source.index("perhapsHandleReceived(mp)", router_source.index("int32_t Router::runOnce(")),
    "remote admin cannot identify its own in-flight Router worker during BEGIN",
)
router_send = function_body(router_source, "ErrorCode Router::send(")
require(
    router_source.count("iface->send(p)") == 1
    and router_send.index("ExternalStateAccessScope stateAccess") < router_send.index("iface->send(p)"),
    "a direct or periodic LoRa producer bypasses the external-state admission fence",
)
require(
    park_destructive.count("waitForIdle()") >= 2
    and park_destructive.index("waitForIdle()") < park_destructive.index("radio->sleep()")
    < park_destructive.rindex("waitForIdle()"),
    "destructive storage can truncate an active radio operation",
)

# Brownout protection is an end-to-end contract: a destructive transaction
# must retain its 3.65 V requirement through encryption, temporary-file
# verification, and the final rename. The warm tier and transmit history must
# also preserve their previous generation until readback succeeds.
safe_file_header = read("src/SafeFile.h")
safe_file_source = read("src/SafeFile.cpp")
encrypted_storage = read("src/security/EncryptedStorage.cpp")
warm_store = read("src/mesh/WarmNodeStore.cpp")
transmit_history = read("src/mesh/TransmitHistory.cpp")
transmit_history_header = read("src/mesh/TransmitHistory.h")
message_store = read("src/MessageStore.cpp")
canned_messages = read("src/modules/CannedMessageModule.cpp")
require(
    "bool requireDestructivePower = false" in safe_file_header,
    "SafeFile no longer carries the destructive-power requirement",
)
require(
    safe_file_source.count("requireDestructivePower ? heltecDestructiveStoragePowerIsSafe()") >= 2,
    "SafeFile must enforce destructive power both at open and final commit",
)
require(
    "SafeFile(filename, fullAtomic, destructivePowerRequired)" in nodedb_source,
    "NodeDB does not propagate destructive power into SafeFile",
)
require(
    "SafeFile sf(filename, fullAtomic, requireDestructivePower)" in encrypted_storage,
    "encrypted preference writes lose the destructive-power requirement",
)
require(
    "SafeFile(warmFileName, keepPreviousGeneration, requireDestructivePower)" in warm_store,
    "warm.dat does not preserve its generation/power contract",
)
require(
    "SafeFile file(FILENAME, true);" in transmit_history
    and "atomic write/readback failed; keeping history dirty" in transmit_history,
    "TransmitHistory is not using fail-closed atomic persistence",
)
require(
    "mutable concurrency::Lock stateLock" in transmit_history_header
    and "concurrency::Lock persistenceLock" in transmit_history_header
    and "snapshot = history" in transmit_history
    and "mutationGeneration == savedGeneration" in transmit_history,
    "TransmitHistory lost its concurrent snapshot/generation fence",
)
require(
    "writeHeltecResetPendingMarker(HeltecResetPendingKind::EDIT, editRequiresDestructivePower)" in nodedb_source,
    "the settings transaction marker lost its profile-aware power requirement",
)
require(
    "it->sender != localNode || it->packetId != packetId" in message_store
    and "sm.packetId = this->lastRequestId" in canned_messages
    and "updateOwnMessageAck(nodeDB->getNodeNum(), mp.decoded.request_id, storedStatus)" in canned_messages,
    "canned-message ACK status is no longer matched to its exact outgoing packet",
)

# Required complete native suites and real-body fault injection are publication gates.
require("- native-audit" in release, "release does not depend on native integration audit")
require("test/host/heltec_audit_regression.py" in build, "real-body fault injection is not a build gate")
audit_workflow = read(".github/workflows/audit_native_heltec.yml")
require("run-heltec-native-audit.py" in audit_workflow, "native suites are not executed")
require("continue-on-error" not in audit_workflow, "native audit is allowed to fail")
print("Heltec V4 release policy: PASS")
