#!/usr/bin/env python3
"""Exercise the production NVS calibration cache with storage/power faults."""

import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def body(source, signature):
    start = source.index("{", source.index(signature))
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


NVS_HEADER = r"""
#pragma once
#include <cstddef>
#include <cstdint>
using esp_err_t = int;
using nvs_handle_t = uint32_t;
enum nvs_open_mode_t { NVS_READONLY, NVS_READWRITE };
constexpr int ESP_OK=0, ESP_ERR_INVALID_STATE=1, ESP_ERR_NVS_NOT_FOUND=2;
constexpr int ESP_ERR_NVS_TYPE_MISMATCH=3, ESP_ERR_NVS_INVALID_LENGTH=4;
int nvs_open(const char*, nvs_open_mode_t, nvs_handle_t*);
int nvs_get_blob(nvs_handle_t, const char*, void*, size_t*);
int nvs_set_blob(nvs_handle_t, const char*, const void*, size_t);
int nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
"""
TEST_CPP = r"""
#include "power/HeltecV4BatteryCalibration.h"
#include "nvs.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>
#include <atomic>
#include <functional>

using Status = HeltecV4BatteryCalibrationStatus;
static std::vector<uint8_t> disk, pending, replaceAfterCommit;
static bool namespaceExists=false, keyExists=false, commitFails=false, persistOnFailure=false;
static int openError=0, getError=0, setError=0, rwOpens=0, setCalls=0, commitCalls=0;
static int getCalls=0, failGetAt=0, handles=0, powerCalls=0, failPowerAt=0;
static constexpr float fallback=5.1205f;
static void testNodeDBIntegration();

int nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* out) {
 assert(std::strcmp(name,"heltec_adc")==0);
 if(openError) return openError;
 if(mode==NVS_READONLY && !namespaceExists) return ESP_ERR_NVS_NOT_FOUND;
 if(mode==NVS_READWRITE) { ++rwOpens; namespaceExists=true; }
 ++handles; *out=mode==NVS_READONLY?1:2; return ESP_OK;
}
int nvs_get_blob(nvs_handle_t handle,const char* key,void* data,size_t* size) {
 assert(handle==1); assert(std::strcmp(key,"calibration")==0); ++getCalls;
 if(getError && (!failGetAt || getCalls==failGetAt)) return getError;
 if(!keyExists) return ESP_ERR_NVS_NOT_FOUND;
 if(!data) { *size=disk.size(); return ESP_OK; }
 if(*size<disk.size()) return ESP_ERR_NVS_INVALID_LENGTH;
 std::memcpy(data,disk.data(),disk.size()); *size=disk.size(); return ESP_OK;
}
int nvs_set_blob(nvs_handle_t handle,const char* key,const void* data,size_t size) {
 assert(handle==2); assert(std::strcmp(key,"calibration")==0); ++setCalls;
 const auto* bytes=static_cast<const uint8_t*>(data);
 pending.assign(bytes,bytes+size);
 if(setError && persistOnFailure) { disk=pending; keyExists=true; }
 return setError;
}
int nvs_commit(nvs_handle_t handle) {
 assert(handle==2); ++commitCalls;
 if(!commitFails || persistOnFailure) { disk=pending; keyExists=true; }
 if(!replaceAfterCommit.empty()) disk=replaceAfterCommit;
 return commitFails?ESP_ERR_INVALID_STATE:ESP_OK;
}
void nvs_close(nvs_handle_t) { assert(handles>0); --handles; }
static bool safePower() { ++powerCalls; return !failPowerAt || powerCalls!=failPowerAt; }
static void reset(bool preserveDisk=false) {
 assert(handles==0);
 if(!preserveDisk) { disk.clear(); namespaceExists=false; keyExists=false; }
 pending.clear(); replaceAfterCommit.clear();
 commitFails=false; persistOnFailure=false;
 openError=getError=setError=rwOpens=setCalls=commitCalls=getCalls=failGetAt=powerCalls=failPowerAt=0;
}
static void expect(Status status,float value=fallback) {
 auto result=readHeltecV4BatteryCalibration(fallback);
 assert(result.status==status); assert(result.multiplier==value); assert(handles==0);
}
static void install(float value) {
 assert(writeHeltecV4BatteryCalibration(value,safePower));
 expect(Status::VALID,value); assert(handles==0);
}
static void putWord(size_t offset,uint32_t value) {
 for(size_t i=0;i<4;++i) disk[offset+i]=static_cast<uint8_t>(value>>(i*8));
}
static void reseal() {
 uint32_t crc=0xffffffffu;
 for(size_t i=0;i<12;++i) {
  crc^=disk[i];
  for(int bit=0;bit<8;++bit) crc=(crc&1)?(crc>>1)^0xedb88320u:crc>>1;
 }
 putWord(12,~crc);
}

int main() {
 reset(); expect(Status::MISSING); assert(rwOpens==0 && setCalls==0 && commitCalls==0);
 namespaceExists=true; expect(Status::MISSING);
 openError=ESP_ERR_INVALID_STATE; expect(Status::UNAVAILABLE);
 reset(); install(5.5f);
 assert(disk.size()==16); const auto good=disk;
 reset(true); expect(Status::VALID,5.5f); // Cold boot retains only the NVS bytes.
 failPowerAt=1;
 assert(writeHeltecV4BatteryCalibration(5.5f,safePower));
 assert(powerCalls==0 && rwOpens==0 && setCalls==0 && commitCalls==0);

 for(float bad: {0.0f,-0.0f,-1.0f,std::numeric_limits<float>::infinity(),
                 -std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
  reset(true); assert(!writeHeltecV4BatteryCalibration(bad,safePower));
  assert(rwOpens==0 && setCalls==0); expect(Status::VALID,5.5f);
 }
 reset(true); assert(!writeHeltecV4BatteryCalibration(6.0f,nullptr)); assert(rwOpens==0);

 for(size_t offset=0;offset<good.size();++offset) {
  reset(true); disk=good; disk[offset]^=1; expect(Status::INVALID);
  assert(rwOpens==0 && setCalls==0 && commitCalls==0);
 }
 for(size_t size: {size_t{0},size_t{1},size_t{15},size_t{17},size_t{256}}) {
  reset(true); disk.assign(size,0); expect(Status::INVALID);
 }
 reset(true); disk=good; putWord(4,2); reseal(); expect(Status::INVALID);
 for(uint32_t bits: {0u,0x80000000u,0xbf800000u,0x7f800000u,0x7fc00000u}) {
  reset(true); disk=good; putWord(8,bits); reseal(); expect(Status::INVALID);
 }
 for(int error: {ESP_ERR_INVALID_STATE,ESP_ERR_NVS_TYPE_MISMATCH,ESP_ERR_NVS_INVALID_LENGTH}) {
  for(int failureAt: {1,2}) {
   reset(true); disk=good; getError=error; failGetAt=failureAt;
   expect(error==ESP_ERR_INVALID_STATE?Status::UNAVAILABLE:Status::INVALID);
   assert(rwOpens==0 && setCalls==0);
  }
 }

 for(int powerFailure: {1,2,3}) {
  reset(true); disk=good; failPowerAt=powerFailure;
  assert(!writeHeltecV4BatteryCalibration(6.0f,safePower));
  assert(commitCalls==0 && handles==0);
  if(powerFailure<3) assert(setCalls==0);
  reset(true); expect(Status::VALID,5.5f);
 }
 reset(true); disk=good; openError=ESP_ERR_INVALID_STATE;
 assert(!writeHeltecV4BatteryCalibration(6.0f,safePower)); assert(handles==0 && setCalls==0);
 for(bool partialWritePersists: {false,true}) {
  reset(true); disk=good; setError=ESP_ERR_INVALID_STATE; persistOnFailure=partialWritePersists;
  assert(!writeHeltecV4BatteryCalibration(6.0f,safePower)); assert(commitCalls==0 && handles==0);
  reset(true); expect(Status::VALID,partialWritePersists?6.0f:5.5f);
  reset(true); disk=good; commitFails=true; persistOnFailure=partialWritePersists;
  assert(!writeHeltecV4BatteryCalibration(6.0f,safePower)); assert(handles==0);
  reset(true); expect(Status::VALID,partialWritePersists?6.0f:5.5f);
 }
 reset(true); disk=good; replaceAfterCommit=good;
 assert(!writeHeltecV4BatteryCalibration(6.0f,safePower)); expect(Status::VALID,5.5f);
 for(int failureAt: {3,4}) {
  reset(true); disk=good; getError=ESP_ERR_INVALID_STATE; failGetAt=failureAt;
  assert(!writeHeltecV4BatteryCalibration(6.0f,safePower)); assert(handles==0);
 }
 reset(true); disk=good; install(fallback); reset(true); expect(Status::VALID,fallback);
 reset(); expect(Status::MISSING); install(fallback); // Full reset removes the namespace; reseed afterwards.
 for(int i=0;i<100;++i) { reset(true); install((i&1)?5.5f:fallback); }
 assert(handles==0);
 testNodeDBIntegration();
 std::cout << "PASS: cold boot, corruption, storage faults, power loss, reset and idempotence\n";
}
"""

nodedb = (ROOT / "src/mesh/NodeDB.cpp").read_text()
adc = (ROOT / "src/power/HeltecV4BatteryAdc.cpp").read_text()
save_body = body(nodedb, "bool NodeDB::saveToDisk(int saveWhat)")
assert save_body.index("saveToDiskNoRetry(saveWhat)") < save_body.index(
    "syncHeltecBatteryCalibration(false)"
)
assert "success = syncHeltecBatteryCalibration" not in save_body
reset_body = body(nodedb, "bool NodeDB::factoryReset(")
assert reset_body.rindex("nvs_flash_erase()") < reset_body.index(
    "syncHeltecBatteryCalibration(true, true)"
)
assert reset_body.index("syncHeltecBatteryCalibration(true, true)") < reset_body.index(
    "clearHeltecResetPendingMarker(resetKind, true)"
)
restore_body = body(nodedb, "bool NodeDB::restorePreferences(")
assert restore_body.index("syncHeltecBatteryCalibration(true)") < restore_body.index(
    "clearHeltecResetPendingMarker(HeltecResetPendingKind::RESTORE"
)

INTEGRATION_CPP = r"""
#define FSCom 1
#define MESHTASTIC_EXCLUDE_GPS 1
#define MESHTASTIC_ENCRYPTED_STORAGE 1
#define LOG_WARN(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
namespace concurrency {
struct Lock { bool held=false; std::function<void()> beforeAcquire; };
struct LockGuard {
 Lock* lock;
 explicit LockGuard(Lock* value): lock(value) {
  if(lock->beforeAcquire) { auto callback=lock->beforeAcquire; lock->beforeAcquire=nullptr; callback(); }
  assert(!lock->held); lock->held=true;
 }
 ~LockGuard() { assert(lock->held); lock->held=false; }
};
}
static concurrency::Lock heltecV4NvsMutationLock, heltecPreferencesTransactionLock;
static std::atomic<bool> heltecBatteryCalibrationSyncPending{false};
static constexpr float nominalMultiplier=static_cast<float>(4.9*1.045);
static std::atomic<float> activeMultiplier{nominalMultiplier};
struct { struct { float adc_multiplier_override=0; } power; } config;
float getActiveHeltecV4AdcMultiplier() @GET_ACTIVE@
void setActiveHeltecV4AdcMultiplier(float resolved) @SET_ACTIVE@
bool resolveHeltecV4AdcMultiplier(float overrideValue, float& resolved) @RESOLVE@
static float durableMultiplier=nominalMultiplier;
static float previousActive=nominalMultiplier;
static bool powerSafe=true;
static unsigned destructivePowerChecks=0;
static bool heltecPreferenceStoragePowerIsSafe() {
 const auto cached=readHeltecV4BatteryCalibration(nominalMultiplier);
 const auto active=getActiveHeltecV4AdcMultiplier();
 // A new active scale must already have durable preference and verified NVS bytes.
 if(active!=previousActive) {
  assert(cached.status==Status::VALID && cached.multiplier==active);
  assert(durableMultiplier==active);
 }
 return powerSafe && safePower();
}
static bool heltecDestructiveStoragePowerIsSafe() {
 ++destructivePowerChecks; return heltecPreferenceStoragePowerIsSafe();
}
static bool syncHeltecBatteryCalibration(bool requireDestructivePower, bool nvsLockHeld=false) @SYNC@
enum class PreferenceEditState { NONE, QUIESCING, OPEN, COMMITTING, ACTIVATING };
enum class HeltecResetPendingKind { NONE, EDIT };
static HeltecResetPendingKind marker=HeltecResetPendingKind::NONE;
static bool failMarkerWrite=false, failMarkerClear=false;
static HeltecResetPendingKind readHeltecResetPendingMarker() { return marker; }
static bool writeHeltecResetPendingMarker(HeltecResetPendingKind kind,bool) {
 if(failMarkerWrite) return false;
 marker=kind; return true;
}
static bool clearHeltecResetPendingMarker(HeltecResetPendingKind kind,bool) {
 assert(marker==kind);
 if(failMarkerClear) return false;
 marker=HeltecResetPendingKind::NONE; return true;
}
static bool xmodemAvailable=true;
struct HeltecXModemStorageGuard { explicit operator bool() const { return xmodemAvailable; } };
static void* xTaskGetCurrentTaskHandle() { return reinterpret_cast<void*>(1); }
static unsigned rebootAtMsec=0, shutdownAtMsec=0;
static bool localRecovery=false;
static void forceHeltecLocalRecoveryConfiguration() { localRecovery=true; }
static void scheduleHeltecRecoveryReboot() { rebootAtMsec=1; }
struct Radio { bool sleep() { return true; } };
struct Router { Radio* getRadioIface() { return nullptr; } };
static Router* router=nullptr;
struct EncryptedStorage {
 static bool locked;
 static bool isLockdownActive() { return locked; }
 static bool isUnlocked() { return !locked; }
};
bool EncryptedStorage::locked=false;
constexpr int SEGMENT_CONFIG=1, SEGMENT_CHANNELS=2;
struct NodeDB {
 std::atomic<bool> destructiveStorageMutationActive{false}, preferenceEditRequiresDestructivePower{false};
 std::atomic<PreferenceEditState> preferenceEditState{PreferenceEditState::NONE};
 std::atomic<uintptr_t> preferenceEditOwnerTask{0}, preferenceEditOwnerClient{0};
 std::atomic<int> powerDeferredPreferenceSegments{0};
 bool configDecodeFailed=false, incompleteConfigResetDetected=false, bootInitializationInProgress=false;
 bool failSave=false, owner=true;
 int unreadablePreferenceSegments=0, saves=0;
 bool isPreferenceEditOwnerCurrentTask() const { return owner; }
 bool isPreferenceEditTransactionActive() const { return preferenceEditState.load()!=PreferenceEditState::NONE; }
 bool requiresConfigRecovery() const { return configDecodeFailed || incompleteConfigResetDetected; }
 bool saveToDisk(int saveWhat) {
  assert(heltecPreferencesTransactionLock.held); ++saves;
  if(failSave) return false;
  if(saveWhat&SEGMENT_CONFIG) {
   float next;
   assert(resolveHeltecV4AdcMultiplier(config.power.adc_multiplier_override,next));
   durableMultiplier=next;
  }
  powerDeferredPreferenceSegments.fetch_and(~saveWhat);
  if((saveWhat&SEGMENT_CONFIG) && !isPreferenceEditTransactionActive())
   syncHeltecBatteryCalibration(false);
  return true;
 }
 bool commitPreferenceEdit(int saveWhat,bool commitOpenEdit);
 void retryPowerDeferredPreferenceWrites();
};
bool NodeDB::commitPreferenceEdit(int saveWhat,bool commitOpenEdit) @COMMIT@
void NodeDB::retryPowerDeferredPreferenceWrites() @RETRY@

static void resetIntegration(NodeDB& db) {
 reset(); install(nominalMultiplier); reset(true);
 assert(!heltecPreferencesTransactionLock.held && !heltecV4NvsMutationLock.held);
 setActiveHeltecV4AdcMultiplier(nominalMultiplier);
 durableMultiplier=nominalMultiplier;
 previousActive=nominalMultiplier;
 config.power.adc_multiplier_override=0;
 heltecBatteryCalibrationSyncPending=false;
 marker=HeltecResetPendingKind::NONE;
 failMarkerWrite=failMarkerClear=localRecovery=false;
 powerSafe=xmodemAvailable=true;
 destructivePowerChecks=0;
 rebootAtMsec=shutdownAtMsec=0;
 EncryptedStorage::locked=false;
 db.preferenceEditState=PreferenceEditState::NONE;
 db.powerDeferredPreferenceSegments=0;
 db.preferenceEditRequiresDestructivePower=false;
 db.configDecodeFailed=db.incompleteConfigResetDetected=db.bootInitializationInProgress=false;
 db.failSave=false; db.saves=0;
}
static void testNodeDBIntegration() {
 NodeDB db;
 resetIntegration(db);
 // A commit publishes the active calibration only after both stores verify.
 config.power.adc_multiplier_override=5.5f;
 db.preferenceEditState=PreferenceEditState::OPEN;
 db.preferenceEditRequiresDestructivePower=true;
 assert(db.commitPreferenceEdit(SEGMENT_CONFIG,true));
 assert(durableMultiplier==5.5f && getActiveHeltecV4AdcMultiplier()==5.5f);
 expect(Status::VALID,5.5f);
 assert(marker==HeltecResetPendingKind::NONE && destructivePowerChecks>0);
 assert(db.preferenceEditState==PreferenceEditState::ACTIVATING && !localRecovery);
 previousActive=5.5f;
 config.power.adc_multiplier_override=0;
 db.preferenceEditState=PreferenceEditState::OPEN;
 assert(db.commitPreferenceEdit(SEGMENT_CONFIG,true));
 assert(durableMultiplier==nominalMultiplier && getActiveHeltecV4AdcMultiplier()==nominalMultiplier);
 expect(Status::VALID,nominalMultiplier);

 // Cache failure after config durability retains the EDIT marker and old active scale.
 for(int fault=0;fault<4;++fault) {
  resetIntegration(db);
  config.power.adc_multiplier_override=5.5f;
  db.preferenceEditState=PreferenceEditState::OPEN;
  if(fault==0) setError=ESP_ERR_INVALID_STATE;
  if(fault==1) commitFails=true;
  if(fault==2) { commitFails=true; persistOnFailure=true; }
  if(fault==3) replaceAfterCommit=disk;
  assert(!db.commitPreferenceEdit(SEGMENT_CONFIG,true));
  assert(durableMultiplier==5.5f && getActiveHeltecV4AdcMultiplier()==nominalMultiplier);
  assert(marker==HeltecResetPendingKind::EDIT && localRecovery && rebootAtMsec);
  assert(heltecBatteryCalibrationSyncPending && db.requiresConfigRecovery());
 }
 resetIntegration(db);
 config.power.adc_multiplier_override=5.5f;
 db.preferenceEditState=PreferenceEditState::OPEN; db.failSave=true;
 assert(!db.commitPreferenceEdit(SEGMENT_CONFIG,true));
 assert(setCalls==0 && getActiveHeltecV4AdcMultiplier()==nominalMultiplier);
 resetIntegration(db);
 config.power.adc_multiplier_override=5.5f;
 db.preferenceEditState=PreferenceEditState::OPEN; failMarkerClear=true;
 assert(!db.commitPreferenceEdit(SEGMENT_CONFIG,true));
 assert(marker==HeltecResetPendingKind::EDIT && localRecovery);
 assert(getActiveHeltecV4AdcMultiplier()==5.5f);

 // Failed cache A, then low-power deferred CONFIG B: persist B before mirroring B.
 resetIntegration(db);
 durableMultiplier=5.5f; config.power.adc_multiplier_override=5.5f;
 setError=ESP_ERR_INVALID_STATE;
 assert(!syncHeltecBatteryCalibration(false));
 reset(true); config.power.adc_multiplier_override=6.0f;
 db.powerDeferredPreferenceSegments=SEGMENT_CONFIG;
 db.failSave=true; db.retryPowerDeferredPreferenceWrites();
 assert(setCalls==0 && durableMultiplier==5.5f);
 expect(Status::VALID,nominalMultiplier);
 db.failSave=false; db.retryPowerDeferredPreferenceWrites();
 assert(durableMultiplier==6.0f && getActiveHeltecV4AdcMultiplier()==6.0f);
 expect(Status::VALID,6.0f); assert(!heltecBatteryCalibrationSyncPending);

 // Cache-only retries are fenced against an edit opening just before acquisition.
 resetIntegration(db);
 durableMultiplier=5.5f; config.power.adc_multiplier_override=5.5f;
 heltecBatteryCalibrationSyncPending=true;
 heltecPreferencesTransactionLock.beforeAcquire=[&]() {
  db.preferenceEditState=PreferenceEditState::OPEN;
  config.power.adc_multiplier_override=6.0f;
 };
 db.retryPowerDeferredPreferenceWrites();
 assert(setCalls==0 && getActiveHeltecV4AdcMultiplier()==nominalMultiplier);
 db.preferenceEditState=PreferenceEditState::NONE;
 config.power.adc_multiplier_override=5.5f;
 EncryptedStorage::locked=true;
 db.retryPowerDeferredPreferenceWrites(); assert(setCalls==0);
 EncryptedStorage::locked=false;
 db.retryPowerDeferredPreferenceWrites();
 assert(db.saves==0 && getActiveHeltecV4AdcMultiplier()==5.5f);
 expect(Status::VALID,5.5f);

 // FULL reset owns the NVS lock already; synchronization must not reacquire it.
 resetIntegration(db);
 { concurrency::LockGuard guard(&heltecV4NvsMutationLock);
   disk.clear(); namespaceExists=keyExists=false;
   assert(syncHeltecBatteryCalibration(true,true)); }
 expect(Status::VALID,nominalMultiplier);
 std::cout << "PASS: production NodeDB commit/retry fences and active-calibration publication\n";
}
"""
for marker_name, replacement in {
    "GET_ACTIVE": body(adc, "float getActiveHeltecV4AdcMultiplier()"),
    "SET_ACTIVE": body(adc, "void setActiveHeltecV4AdcMultiplier(float resolved)"),
    "RESOLVE": body(adc, "bool resolveHeltecV4AdcMultiplier("),
    "SYNC": body(nodedb, "bool syncHeltecBatteryCalibration("),
    "COMMIT": body(nodedb, "bool NodeDB::commitPreferenceEdit("),
    "RETRY": body(nodedb, "void NodeDB::retryPowerDeferredPreferenceWrites()"),
}.items():
    INTEGRATION_CPP = INTEGRATION_CPP.replace(f"@{marker_name}@", replacement)

for profile, extra in [
    ("standard", []),
    ("solar", ["-DHELTEC_V4_SOLAR_ROUTER_PROFILE=1"]),
]:
    with tempfile.TemporaryDirectory(prefix="heltec-adc-cache-") as directory:
        work = Path(directory)
        (work / "nvs.h").write_text(NVS_HEADER)
        source = work / "test.cpp"
        source.write_text(TEST_CPP + INTEGRATION_CPP)
        executable = work / "test"
        subprocess.run(
            [
                "g++",
                "-std=c++17",
                "-O1",
                "-g",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pedantic",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                "-fno-pie",
                "-no-pie",
                "-DHELTEC_V4_OLED=1",
                "-DARDUINO_ARCH_ESP32=1",
                *extra,
                f"-I{work}",
                f"-I{ROOT / 'src'}",
                str(source),
                str(ROOT / "src/power/HeltecV4BatteryCalibration.cpp"),
                "-o",
                str(executable),
            ],
            check=True,
        )
        subprocess.run(
            [str(executable)],
            check=True,
            timeout=30,
            env=dict(
                os.environ,
                ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
                UBSAN_OPTIONS="halt_on_error=1",
            ),
        )
    print(f"{profile}: PASS (production NVS cache, ASan+UBSan)", flush=True)
