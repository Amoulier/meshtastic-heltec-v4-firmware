#!/usr/bin/env python3
"""Exercise production IRQ and preference-fence bodies with deterministic hardware doubles."""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def function_body(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        depth += (source[index] == "{") - (source[index] == "}")
        if depth == 0:
            return source[start : index + 1]
    raise AssertionError(signature)


PREFIX = r"""
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <vector>
#define HELTEC_V4_OLED 1
#define FSCom 1
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
#define LOG_TRACE(...) ((void)0)
uint32_t clockMs = 10000, delayCalls = 0, rebootAtMsec = 0, shutdownAtMsec = 0;
uint32_t millis() { return clockMs; }
void delay(uint32_t ms) { clockMs += ms; ++delayCalls; }
struct Throttle {
 static bool isWithinTimespanMs(uint32_t start, uint32_t duration) { return uint32_t(millis()-start) < duration; }
};
constexpr uint16_t RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED = 0x0004;
constexpr uint16_t RADIOLIB_SX126X_IRQ_HEADER_VALID = 0x0010;
constexpr uint16_t RX_DONE = 0x0002, TX_DONE = 0x0001, HEADER_ERR = 0x0020, CRC_ERR = 0x0040, TIMEOUT = 0x0200;
constexpr uint32_t meshtastic_Constants_DATA_PAYLOAD_LEN = 233;
struct PacketHeader { uint8_t data[16]; };
struct Chip {
 uint16_t flags = 0;
 int reads = 0, clearResult = 0;
 std::vector<uint16_t> cleared;
 std::function<void(int)> afterRead;
 std::function<void()> afterClear;
 uint16_t getIrqFlags() {
  uint16_t result = flags;
  ++reads;
  if (afterRead) afterRead(reads);
  return result;
 }
 int clearIrqFlags(uint16_t mask) {
  cleared.push_back(mask);
  if (!clearResult) flags &= ~mask;
  if (afterClear) afterClear();
  return clearResult;
 }
};
struct RadioInterface {
 virtual ~RadioInterface() = default;
 virtual bool canParkForConfig() = 0;
 virtual bool sleep() = 0;
 virtual bool reconfigure() = 0;
 virtual bool hasPendingTransmissionsForConfig() = 0;
 virtual void resumeQueuedTransmissions() = 0;
};
struct RadioLibInterface : RadioInterface {
 std::atomic<uint32_t> sendAdmissionDepth{0}, radioNotificationDepth{0};
 bool isReceiving = true, sending = false;
 uint32_t activeReceiveStart = 0, preambleTimeMsec = 100;
 bool isSending() { return sending; }
 uint32_t getPacketTime(uint32_t, bool = false) { return 1000; }
 virtual bool isIRQPending() = 0;
 virtual bool isActivelyReceiving() = 0;
 virtual bool isActivelyReceivingForConfig(uint32_t &);
 virtual bool isIRQPendingForConfig(uint32_t);
 bool canParkForConfig() override;
 bool receiveDetected(uint16_t, unsigned long, unsigned long, bool = true);
};
template <typename T> struct SX126xInterface : RadioLibInterface {
 T lora;
 std::atomic<bool> radioHardwareParked{false};
 std::atomic<uint32_t> configHeaderDetectedAt{0};
 bool sleepResult = true, reconfigureResult = true, pendingTx = false;
 int sleeps = 0, reconfigures = 0, resumes = 0;
 std::function<void()> afterSleep, onResume;
 bool isActivelyReceiving() override;
 bool isActivelyReceivingForConfig(uint32_t &) override;
 bool isIRQPendingForConfig(uint32_t) override;
 IRQ_PENDING_BODY
 bool sleep() override {
  assert(!sending);
  ++sleeps;
  radioHardwareParked = true;
  isReceiving = false;
  if (afterSleep) afterSleep();
  return sleepResult;
 }
 bool reconfigure() override {
  ++reconfigures;
  if (reconfigureResult) {
   radioHardwareParked = false;
   isReceiving = true;
  }
  return reconfigureResult;
 }
 bool hasPendingTransmissionsForConfig() override { return pendingTx; }
 void resumeQueuedTransmissions() override { ++resumes; if (onResume) onResume(); }
};
using Radio = SX126xInterface<Chip>;
struct Router {
 RadioInterface *radio = nullptr;
 bool pendingRx = false;
 int wakes = 0;
 RadioInterface *getRadioIface() { return radio; }
 bool hasPendingRadioPacketsForConfig() { return pendingRx; }
 void setReceivedMessage() { ++wakes; }
};
Router *router = nullptr;
namespace concurrency {
 struct Lock { bool held = false; };
 struct LockGuard {
  Lock *lock;
  explicit LockGuard(Lock *value) : lock(value) { assert(!lock->held); lock->held = true; }
  ~LockGuard() { lock->held = false; }
 };
}
concurrency::Lock heltecPreferencesTransactionLock;
bool xmodemAvailable = true, xmodemHeld = false, preferencePower = true, destructivePower = true;
struct HeltecXModemStorageGuard {
 bool acquired;
 HeltecXModemStorageGuard() : acquired(xmodemAvailable && !xmodemHeld) { if (acquired) xmodemHeld = true; }
 ~HeltecXModemStorageGuard() { if (acquired) xmodemHeld = false; }
 explicit operator bool() const { return acquired; }
};
enum class HeltecResetPendingKind { NONE, EDIT };
HeltecResetPendingKind pendingMarker = HeltecResetPendingKind::NONE;
HeltecResetPendingKind readHeltecResetPendingMarker() { return pendingMarker; }
bool heltecDestructiveStoragePowerIsSafe() { return destructivePower; }
bool heltecPreferenceStoragePowerIsSafe() { return preferencePower; }
void *xTaskGetCurrentTaskHandle() { return reinterpret_cast<void *>(uintptr_t{42}); }
int recoveryReboots = 0;
void scheduleHeltecRecoveryReboot() { ++recoveryReboots; }
enum class PreferenceEditState { NONE, QUIESCING, OPEN, COMMITTING, ACTIVATING };
struct NodeDB {
 std::atomic<bool> destructiveStorageMutationActive{false}, preferenceEditRequiresDestructivePower{false}, preferenceEditRadioParked{false};
 std::atomic<uintptr_t> preferenceEditOwnerTask{0}, preferenceEditOwnerClient{0};
 std::atomic<PreferenceEditState> preferenceEditState{PreferenceEditState::NONE};
 bool recovery = false, readersQuiescent = true;
 std::function<void()> afterReaders;
 bool requiresConfigRecovery() { return recovery; }
 uintptr_t currentExternalStateClientToken() { return 24; }
 bool waitForExternalStateReaders() { if (afterReaders) afterReaders(); return readersQuiescent; }
 bool beginPreferenceEdit(bool requireDestructivePower = true);
};
"""

TESTS = r"""
struct Fixture {
 Radio radio;
 Router routing;
 NodeDB db;
 Fixture() {
  clockMs = 10000;
  delayCalls = rebootAtMsec = shutdownAtMsec = 0;
  recoveryReboots = 0;
  preferencePower = destructivePower = xmodemAvailable = true;
  assert(!heltecPreferencesTransactionLock.held && !xmodemHeld);
  pendingMarker = HeltecResetPendingKind::NONE;
  routing.radio = &radio;
  router = &routing;
  radio.onResume = [&]() { assert(db.preferenceEditState == PreferenceEditState::NONE); };
 }
 ~Fixture() { router = nullptr; assert(!heltecPreferencesTransactionLock.held && !xmodemHeld); }
 void assertReleased() {
  assert(db.preferenceEditState == PreferenceEditState::NONE);
  assert(db.preferenceEditOwnerTask == 0 && db.preferenceEditOwnerClient == 0);
  assert(!db.preferenceEditRadioParked && !db.preferenceEditRequiresDestructivePower);
  assert(recoveryReboots == 0 && delayCalls == 0);
 }
};
void test_noise_expiry_preserves_flags_and_survives_repeated_probes() {
 constexpr uint16_t preamble = RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED;
 constexpr uint16_t header = RADIOLIB_SX126X_IRQ_HEADER_VALID;
 for (uint16_t flags : {preamble, header, uint16_t(preamble|header)}) {
  Fixture f;
  f.radio.lora.flags = flags;
  f.radio.activeReceiveStart = clockMs - 1;
  assert(!f.radio.canParkForConfig());
  assert(f.radio.lora.cleared.empty());
  clockMs += 2000;
  for (int probe = 0; probe < 3; ++probe) assert(f.radio.canParkForConfig());
  assert(f.radio.activeReceiveStart != 0);
  assert(f.radio.lora.cleared.empty() && f.radio.lora.flags == flags);
  assert(f.db.beginPreferenceEdit(false));
  assert(f.db.preferenceEditState == PreferenceEditState::OPEN && f.radio.sleeps == 1);
  assert(f.radio.lora.cleared.empty() && f.radio.lora.flags == flags);
 }
 {
  Fixture f;
  clockMs = 200;
  f.radio.activeReceiveStart = UINT32_MAX - 2000;
  f.radio.configHeaderDetectedAt = UINT32_MAX - 2000;
  f.radio.lora.flags = header;
  assert(f.radio.canParkForConfig());
  assert(f.radio.lora.flags == header && f.radio.lora.cleared.empty());
 }
 {
  Fixture f;
  f.radio.activeReceiveStart = 1;
  f.radio.lora.flags = preamble;
  f.radio.lora.clearResult = -1;
  assert(f.radio.canParkForConfig());
  assert(f.radio.lora.flags == preamble && f.radio.lora.cleared.empty());
 }
}
void test_terminal_irqs_and_new_receive_are_preserved() {
 for (uint16_t flags : {RX_DONE,TX_DONE,HEADER_ERR,CRC_ERR,TIMEOUT,uint16_t(0xffff),uint16_t(RX_DONE|HEADER_ERR|0x10)}) {
  Fixture f;
  f.radio.activeReceiveStart = 1;
  f.radio.lora.flags = flags;
  assert(!f.radio.canParkForConfig());
  assert(f.radio.lora.cleared.empty() && f.radio.lora.flags == flags);
 }
 {
  Fixture f;
  f.radio.lora.afterRead = [&](int count) { if (count == 1) f.radio.lora.flags = 0x10; };
  assert(!f.radio.canParkForConfig());
  assert(f.radio.lora.cleared.empty() && f.radio.lora.flags == 0x10);
 }
 {
  Fixture f;
  f.radio.activeReceiveStart = 1;
  f.radio.lora.flags = 0x04;
  f.radio.lora.afterRead = [&](int count) { if (count == 1) f.radio.lora.flags |= 0x10; };
  assert(!f.radio.canParkForConfig());
  assert(f.radio.lora.cleared.empty());
  assert(f.radio.lora.flags == 0x14);
 }
 {
  Fixture f;
  f.radio.activeReceiveStart = 1;
  f.radio.lora.flags = 0x04;
  f.radio.lora.afterRead = [&](int count) { if (count == 1) f.radio.lora.flags |= RX_DONE; };
  assert(!f.radio.canParkForConfig());
  assert(f.radio.lora.flags == (RX_DONE|0x04) && f.radio.lora.cleared.empty());
 }
 {
  Fixture f;
  f.radio.activeReceiveStart = 1;
  f.radio.configHeaderDetectedAt = 1;
  f.radio.lora.flags = 0x10;
  f.radio.lora.afterRead = [&](int count) { if (count == 1) f.radio.lora.flags |= RX_DONE|HEADER_ERR; };
  assert(!f.radio.canParkForConfig());
  assert(f.radio.lora.flags == (RX_DONE|HEADER_ERR|0x10) && f.radio.lora.cleared.empty());
  // RadioLib readData accepts HEADER_ERR when HEADER_VALID is also present.
  assert(!(f.radio.lora.flags & HEADER_ERR) || (f.radio.lora.flags & 0x10));
 }
}
void test_new_header_between_config_probes_gets_its_own_window() {
 Fixture f;
 f.radio.activeReceiveStart = 1;
 f.radio.lora.flags = 0x04;
 f.db.afterReaders = [&]() { f.radio.lora.flags |= 0x10; };
 assert(!f.db.beginPreferenceEdit(false));
 f.assertReleased();
 assert(f.radio.sleeps == 0 && f.radio.reconfigures == 0);
 assert(f.radio.configHeaderDetectedAt == clockMs);
 assert(f.radio.lora.flags == 0x14 && f.radio.lora.cleared.empty());
 clockMs += 999;
 assert(!f.radio.canParkForConfig());
 ++clockMs;
 assert(f.radio.canParkForConfig());
 assert(f.db.beginPreferenceEdit(false));
 assert(f.radio.sleeps == 1 && f.radio.lora.cleared.empty());
}
void test_normal_receive_detection_retains_its_existing_expiry() {
 Fixture f;
 f.radio.activeReceiveStart = 1;
 f.radio.lora.flags = 0x04;
 assert(!f.radio.isActivelyReceiving());
 assert(f.radio.activeReceiveStart == 0 && f.radio.lora.cleared.empty());
 assert(f.radio.isActivelyReceiving());
 assert(f.radio.activeReceiveStart == clockMs);
}
void test_other_radio_drivers_keep_conservative_irq_policy() {
 struct ConservativeRadio : Radio {
  bool isActivelyReceiving() override { return false; }
  bool isActivelyReceivingForConfig(uint32_t &flags) override {
   return RadioLibInterface::isActivelyReceivingForConfig(flags);
  }
  bool isIRQPendingForConfig(uint32_t flags) override {
   return RadioLibInterface::isIRQPendingForConfig(flags);
  }
 } radio;
 for (uint16_t flags : {uint16_t(0x04),uint16_t(0x10),RX_DONE,uint16_t(0xffff)}) {
  radio.lora.flags = flags;
  assert(!radio.canParkForConfig());
  assert(radio.lora.flags == flags && radio.lora.cleared.empty());
 }
 radio.lora.flags = 0;
 assert(radio.canParkForConfig());
}
void test_software_owners_are_rechecked() {
 for (int raceRead = 1; raceRead <= 2; ++raceRead) {
 for (int owner = 0; owner < 3; ++owner) {
  Fixture f;
  f.radio.lora.afterRead = [&](int count) {
   if (count != raceRead) return;
   if (owner == 0) f.radio.sendAdmissionDepth = 1;
   if (owner == 1) f.radio.radioNotificationDepth = 1;
   if (owner == 2) f.radio.sending = true;
  };
  assert(!f.radio.canParkForConfig());
 }
 }
 {
  Fixture f;
  f.radio.sending = true;
  assert(!f.radio.canParkForConfig() && f.radio.lora.reads == 0);
 }
 {
  Fixture f;
  f.radio.radioHardwareParked = true;
  assert(f.radio.canParkForConfig() && f.radio.lora.reads == 0);
 }
}
void test_busy_returns_to_worker_and_retry_succeeds() {
 Fixture f;
 f.radio.lora.flags = RX_DONE;
 const uint32_t started = clockMs;
 assert(!f.db.beginPreferenceEdit(false));
 f.assertReleased();
 assert(clockMs == started && f.radio.sleeps == 0 && f.radio.reconfigures == 0);
 assert(f.radio.resumes == 1 && f.routing.wakes == 1 && f.radio.lora.flags == RX_DONE);
 f.radio.lora.flags = 0; // The normal worker runs only after the rejected caller returns.
 assert(f.db.beginPreferenceEdit(false));
 assert(f.db.preferenceEditState == PreferenceEditState::OPEN && f.radio.sleeps == 1);
 assert(pendingMarker == HeltecResetPendingKind::NONE);
}
void test_pre_sleep_cancellation_never_reconfigures_new_work() {
 for (int race = 0; race < 5; ++race) {
  Fixture f;
  f.db.afterReaders = [&]() {
   if (race == 0) f.radio.sending = true;
   if (race == 1) { f.radio.lora.flags = 0x10; f.radio.activeReceiveStart = clockMs; }
   if (race == 2) f.radio.pendingTx = true;
   if (race == 3) f.routing.pendingRx = true;
   if (race == 4) f.db.readersQuiescent = false;
  };
  assert(!f.db.beginPreferenceEdit(false));
  f.assertReleased();
  assert(f.radio.sleeps == 0 && f.radio.reconfigures == 0);
  if (race == 0) assert(f.radio.sending);
  if (race == 1) assert(f.radio.lora.flags == 0x10 && f.radio.lora.cleared.empty());
  if (race == 2) assert(f.radio.pendingTx);
  if (race == 3) assert(f.routing.pendingRx);
 }
}
void test_sleep_attempt_cancellation_restores_or_fences_recovery() {
 for (int failure = 0; failure < 4; ++failure) {
  Fixture f;
  if (failure == 0) f.radio.sleepResult = false;
  f.radio.afterSleep = [&]() {
   if (failure == 1) f.routing.pendingRx = true;
   if (failure == 2) f.radio.pendingTx = true;
   if (failure == 3) f.radio.radioNotificationDepth = 1;
  };
  assert(!f.db.beginPreferenceEdit(false));
  f.assertReleased();
  assert(f.radio.sleeps == 1 && f.radio.reconfigures == 1 && !f.radio.radioHardwareParked);
  if (failure == 1) assert(f.routing.pendingRx);
  if (failure == 2) assert(f.radio.pendingTx);
 }
 {
  Fixture f;
  f.radio.sleepResult = f.radio.reconfigureResult = false;
  assert(!f.db.beginPreferenceEdit(false));
  assert(f.db.preferenceEditState == PreferenceEditState::OPEN && f.db.preferenceEditRadioParked);
  assert(recoveryReboots == 1 && f.radio.resumes == 0 && f.routing.wakes == 0);
 }
}
void test_existing_guards_reject_before_radio_work() {
 for (int gate = 0; gate < 6; ++gate) {
  Fixture f;
  if (gate == 0) rebootAtMsec = 1;
  if (gate == 1) f.db.destructiveStorageMutationActive = true;
  if (gate == 2) f.db.recovery = true;
  if (gate == 3) pendingMarker = HeltecResetPendingKind::EDIT;
  if (gate == 4) preferencePower = false;
  if (gate == 5) xmodemAvailable = false;
  assert(!f.db.beginPreferenceEdit(false));
  assert(f.radio.lora.reads == 0 && f.radio.sleeps == 0 && f.radio.reconfigures == 0);
 }
}
void test_storage_park_never_waits_for_its_own_worker() {
 {
  Fixture f;
  f.radio.lora.flags = RX_DONE;
  assert(!parkHeltecRadioForStorageMutation());
  assert(delayCalls == 0 && f.radio.sleeps == 0 && f.radio.lora.flags == RX_DONE);
  f.radio.lora.flags = 0;
  assert(parkHeltecRadioForStorageMutation());
  assert(f.radio.sleeps == 1);
 }
 {
  Fixture f;
  f.radio.afterSleep = [&]() { f.radio.sendAdmissionDepth = 1; };
  assert(!parkHeltecRadioForStorageMutation());
  assert(f.radio.sleeps == 1 && delayCalls == 0);
 }
 router = nullptr;
 assert(parkHeltecRadioForStorageMutation());
}
int main() {
 test_noise_expiry_preserves_flags_and_survives_repeated_probes();
 test_terminal_irqs_and_new_receive_are_preserved();
 test_new_header_between_config_probes_gets_its_own_window();
 test_normal_receive_detection_retains_its_existing_expiry();
 test_other_radio_drivers_keep_conservative_irq_policy();
 test_software_owners_are_rechecked();
 test_busy_returns_to_worker_and_retry_succeeds();
 test_pre_sleep_cancellation_never_reconfigures_new_work();
 test_sleep_attempt_cancellation_restores_or_fences_recovery();
 test_existing_guards_reject_before_radio_work();
 test_storage_park_never_waits_for_its_own_worker();
 puts("Radio quiesce regression: PASS (production IRQ, receive, preference-begin and storage-park bodies)");
}
"""


def main():
    radio = (ROOT / "src/mesh/RadioLibInterface.cpp").read_text()
    sx = (ROOT / "src/mesh/SX126xInterface.cpp").read_text()
    sx_header = (ROOT / "src/mesh/SX126xInterface.h").read_text()
    nodedb = (ROOT / "src/mesh/NodeDB.cpp").read_text()
    prefix = PREFIX.replace(
        "IRQ_PENDING_BODY", function_body(sx_header, "bool isIRQPending() override")
    )
    methods = "\n".join(
        [
            function_body(radio, "bool RadioLibInterface::canParkForConfig()"),
            function_body(radio, "bool RadioLibInterface::receiveDetected("),
            function_body(
                radio, "bool RadioLibInterface::isActivelyReceivingForConfig("
            ),
            function_body(radio, "bool RadioLibInterface::isIRQPendingForConfig("),
            function_body(
                sx,
                "template <typename T> bool SX126xInterface<T>::isActivelyReceiving()",
            ),
            function_body(
                sx,
                "template <typename T> bool SX126xInterface<T>::isActivelyReceivingForConfig(",
            ),
            function_body(
                sx,
                "template <typename T> bool SX126xInterface<T>::isIRQPendingForConfig(",
            ),
            function_body(nodedb, "bool NodeDB::beginPreferenceEdit("),
            function_body(nodedb, "bool parkHeltecRadioForStorageMutation()"),
        ]
    )
    with tempfile.TemporaryDirectory(prefix="radio-quiesce-") as directory:
        cpp = Path(directory) / "test.cpp"
        executable = Path(directory) / ("test.exe" if os.name == "nt" else "test")
        cpp.write_text(prefix + methods + TESTS)
        command = shlex.split(os.environ.get("CXX", "g++"))
        command += ["-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror"]
        command += ["-Wno-unused-parameter"]
        sanitizer_flags = (
            "-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie"
            if os.name == "posix"
            else ""
        )
        command += shlex.split(os.environ.get("RADIO_TEST_CXXFLAGS", sanitizer_flags))
        subprocess.run(command + [str(cpp), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == "__main__":
    main()
