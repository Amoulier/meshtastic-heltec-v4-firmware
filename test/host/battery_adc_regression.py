#!/usr/bin/env python3
"""Exercise the production Heltec ADC reader with deterministic IDF failures."""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

STUBS = {
    "configuration.h": r"""
#pragma once
#ifdef TEST_NATIVE_ONLY
#define HELTEC_V4_NATIVE_STORAGE_AUDIT 1
#else
#define HELTEC_V4_OLED 1
#define ARDUINO_ARCH_ESP32 1
#define ADC_MULTIPLIER (4.9 * 1.045)
#endif
#define BATTERY_PIN 1
#define ADC_CHANNEL ADC_CHANNEL_0
#define ADC_ATTENUATION ADC_ATTEN_DB_2_5
#define ADC_CTRL 37
#define ADC_CTRL_ENABLED 1
""",
    "Arduino.h": r"""
#pragma once
#define OUTPUT 1
void pinMode(int pin, int mode);
void digitalWrite(int pin, int level);
void delay(unsigned milliseconds);
""",
    "concurrency/Lock.h": r"""
#pragma once
#include <cassert>
#include <mutex>
extern thread_local bool testAdcLockHeld;
namespace concurrency {
class Lock {
 std::mutex mutex;
public:
 void lock() {mutex.lock(); assert(!testAdcLockHeld); testAdcLockHeld = true;}
 void unlock() {assert(testAdcLockHeld); testAdcLockHeld = false; mutex.unlock();}
};
}
""",
    "concurrency/LockGuard.h": r"""
#pragma once
#include "Lock.h"
namespace concurrency {
class LockGuard {
 Lock *mutex;
public:
 explicit LockGuard(Lock *lock):mutex(lock) {mutex->lock();}
 ~LockGuard() {mutex->unlock();}
};
}
""",
    "idf_stubs.h": r"""
#pragma once
using esp_err_t = int;
using adc_unit_t = int;
using adc_channel_t = int;
using adc_atten_t = int;
using adc_bitwidth_t = int;
using adc_oneshot_unit_handle_t = void *;
using adc_cali_handle_t = void *;
constexpr int ESP_OK = 0;
constexpr int ESP_FAIL = -1;
constexpr int ADC_UNIT_1 = 7;
constexpr int ADC_CHANNEL_0 = 3;
constexpr int ADC_ATTEN_DB_2_5 = 6;
constexpr int ADC_BITWIDTH_12 = 12;
#ifndef ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
#define ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED 1
#endif
struct adc_oneshot_unit_init_cfg_t {adc_unit_t unit_id;};
struct adc_oneshot_chan_cfg_t {adc_atten_t atten; adc_bitwidth_t bitwidth;};
struct adc_cali_curve_fitting_config_t {
 adc_unit_t unit_id; adc_channel_t chan; adc_atten_t atten; adc_bitwidth_t bitwidth;
};
esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *, adc_oneshot_unit_handle_t *);
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t, adc_channel_t, const adc_oneshot_chan_cfg_t *);
esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t);
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t, adc_channel_t, int *);
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *, adc_cali_handle_t *);
esp_err_t adc_cali_delete_scheme_curve_fitting(adc_cali_handle_t);
esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t, int, int *);
""",
}

TEST_CPP = r"""
#include "configuration.h"
#include "idf_stubs.h"
#include "power/HeltecV4BatteryAdc.h"
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <thread>
#include <vector>

thread_local bool testAdcLockHeld = false;
struct Fake {
 bool enabled=false, failUnit=false, failChannel=false, failCalibrationInit=false;
 int units=0, calibrations=0, unitCreates=0, calibrationCreates=0, unitDeletes=0;
 int deleteFailures=0, enabledCount=0, settledCount=0, reads=0, conversions=0, index=0;
 int failedRead=-1, failedConversion=-1, forcedRaw=-1, forcedVoltage=-1;
 bool invalidNegativeRaw=false, invalidNegativeVoltage=false;
 int unitToken=1, calibrationToken=2;
} fake;

void pinMode(int pin,int mode) {assert(testAdcLockHeld && pin==37 && mode==1);}
void digitalWrite(int pin,int level) {
 assert(testAdcLockHeld && pin==37);
 fake.enabled=level!=0;
 if (level) {++fake.enabledCount; fake.index=0;}
}
void delay(unsigned ms) {
 assert(testAdcLockHeld && fake.enabled && ms==10);
 ++fake.settledCount;
 std::this_thread::yield();
}
esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *cfg,adc_oneshot_unit_handle_t *handle) {
 assert(testAdcLockHeld && !fake.enabled && cfg->unit_id==ADC_UNIT_1 && fake.units==0);
 ++fake.unitCreates;
 if (fake.failUnit) return ESP_FAIL;
 ++fake.units; *handle=&fake.unitToken; return ESP_OK;
}
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t handle,adc_channel_t channel,const adc_oneshot_chan_cfg_t *cfg) {
 assert(testAdcLockHeld && !fake.enabled && handle==&fake.unitToken && channel==ADC_CHANNEL_0);
 assert(cfg->atten==ADC_ATTEN_DB_2_5 && cfg->bitwidth==ADC_BITWIDTH_12);
 return fake.failChannel ? ESP_FAIL : ESP_OK;
}
esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t handle) {
 assert(testAdcLockHeld && !fake.enabled && handle==&fake.unitToken && fake.units==1);
 ++fake.unitDeletes;
 if (fake.deleteFailures) {--fake.deleteFailures; return ESP_FAIL;}
 --fake.units; return ESP_OK;
}
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t handle,adc_channel_t channel,int *raw) {
 assert(testAdcLockHeld && fake.enabled && fake.settledCount==fake.enabledCount);
 assert(handle==&fake.unitToken && channel==ADC_CHANNEL_0 && fake.units==1);
 int index=fake.index++;
 ++fake.reads;
 if (index==fake.failedRead) return ESP_FAIL;
 *raw=fake.invalidNegativeRaw ? -1 : fake.forcedRaw>=0 ? fake.forcedRaw : index;
 return ESP_OK;
}
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *cfg,adc_cali_handle_t *handle) {
 assert(testAdcLockHeld && !fake.enabled && fake.calibrations==0);
 assert(cfg->unit_id==ADC_UNIT_1 && cfg->chan==ADC_CHANNEL_0);
 assert(cfg->atten==ADC_ATTEN_DB_2_5 && cfg->bitwidth==ADC_BITWIDTH_12);
 ++fake.calibrationCreates;
 if (fake.failCalibrationInit) return ESP_FAIL;
 ++fake.calibrations; *handle=&fake.calibrationToken; return ESP_OK;
}
esp_err_t adc_cali_delete_scheme_curve_fitting(adc_cali_handle_t handle) {
 assert(testAdcLockHeld && handle==&fake.calibrationToken && fake.calibrations==1);
 --fake.calibrations; return ESP_OK;
}
esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t handle,int raw,int *voltage) {
 assert(testAdcLockHeld && fake.enabled && handle==&fake.calibrationToken && fake.calibrations==1);
 ++fake.conversions;
 if (fake.index-1==fake.failedConversion) return ESP_FAIL;
 *voltage=fake.invalidNegativeVoltage ? -1 : fake.forcedVoltage>=0 ? fake.forcedVoltage : 800+raw*raw;
 return ESP_OK;
}

static void testResolver() {
 float nominal=0, resolved=123;
 assert(resolveHeltecV4AdcMultiplier(0,nominal));
 assert(nominal==static_cast<float>(4.9*1.045));
 assert(getActiveHeltecV4AdcMultiplier()==nominal);
 assert(resolveHeltecV4AdcMultiplier(-0.0f,resolved) && resolved==nominal);
 for (float invalid : {-1.0f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()}) {
  resolved=123;
  assert(!resolveHeltecV4AdcMultiplier(invalid,resolved) && resolved==0);
  setActiveHeltecV4AdcMultiplier(invalid);
  assert(getActiveHeltecV4AdcMultiplier()==nominal);
 }
 for (float valid : {0.125f,5.1205f,12.0f,std::numeric_limits<float>::denorm_min(),std::numeric_limits<float>::max()}) {
  assert(resolveHeltecV4AdcMultiplier(valid,resolved) && resolved==valid);
 }
 setActiveHeltecV4AdcMultiplier(5.5f);
 assert(getActiveHeltecV4AdcMultiplier()==5.5f);
 setActiveHeltecV4AdcMultiplier(0);
 assert(getActiveHeltecV4AdcMultiplier()==5.5f);
 invalidateHeltecV4AdcCalibration();
 assert(getActiveHeltecV4AdcMultiplier()==0);
 setActiveHeltecV4AdcMultiplier(std::numeric_limits<float>::quiet_NaN());
 assert(getActiveHeltecV4AdcMultiplier()==0);
 setActiveHeltecV4AdcMultiplier(nominal);
 assert(getActiveHeltecV4AdcMultiplier()==nominal);
 assert(fake.reads==0 && fake.unitCreates==0);
}

#ifndef TEST_NATIVE_ONLY
static void expectFailure(float factor) {
 uint16_t value=9999;
 assert(!readHeltecV4BatteryMillivolts(factor,value));
 assert(value==0 && !fake.enabled && !testAdcLockHeld);
}
static void expectSuccess(float factor,uint16_t expected) {
 uint16_t value=9999;
 assert(readHeltecV4BatteryMillivolts(factor,value));
 assert(value==expected && !fake.enabled && !testAdcLockHeld);
}
static void testHardware(const std::string &name) {
 constexpr float nominal=static_cast<float>(4.9*1.045);
 if (name=="invalidation_and_recovery") {
  invalidateHeltecV4AdcCalibration();
  expectFailure(getActiveHeltecV4AdcMultiplier());
  assert(fake.unitCreates==0 && fake.enabledCount==0);
  setActiveHeltecV4AdcMultiplier(5.5f);
  fake.forcedVoltage=800;
  expectSuccess(getActiveHeltecV4AdcMultiplier(),4400);
  assert(fake.unitCreates==1 && fake.calibrationCreates==1);
 } else if (name=="invalid_multiplier") {
  for(float value : {0.0f,-1.0f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()}) expectFailure(value);
  assert(fake.unitCreates==0 && fake.enabledCount==0);
 } else if (name=="init_failure" || name=="channel_failure" || name=="calibration_init_failure" || name=="cleanup_retry") {
  fake.failUnit=name=="init_failure";
  fake.failChannel=name=="channel_failure" || name=="cleanup_retry";
  fake.failCalibrationInit=name=="calibration_init_failure";
  fake.deleteFailures=name=="cleanup_retry" ? 1 : 0;
  expectFailure(nominal);
  assert(fake.calibrations==0 && fake.enabledCount==0);
  assert(fake.units==(name=="cleanup_retry" ? 1 : 0));
  fake.failUnit=fake.failChannel=fake.failCalibrationInit=false;
  fake.forcedVoltage=800;
  expectSuccess(nominal,static_cast<uint16_t>(800.0*nominal));
  assert(fake.units==1 && fake.calibrations==1);
 } else if (name=="nonlinear_average_and_reuse") {
  const uint16_t expected=static_cast<uint16_t>((13015.0/15)*nominal);
  expectSuccess(nominal,expected);
  assert(expected!=static_cast<uint16_t>((800+7*7)*nominal));
  expectSuccess(nominal,expected);
  assert(fake.unitCreates==1 && fake.calibrationCreates==1 && fake.reads==30 && fake.conversions==30);
 } else if (name=="explicit_factor_changes") {
  fake.forcedVoltage=800;
  expectSuccess(nominal,static_cast<uint16_t>(800.0*nominal));
  expectSuccess(5.5f,4400);
  expectSuccess(nominal,static_cast<uint16_t>(800.0*nominal));
  assert(getActiveHeltecV4AdcMultiplier()==nominal && fake.reads==45 && fake.unitCreates==1);
 } else if (name=="read_fail_first" || name=="read_fail_middle" || name=="read_fail_last" || name=="conversion_failure") {
  fake.failedRead=name=="read_fail_first" ? 0 : name=="read_fail_middle" ? 7 : name=="read_fail_last" ? 14 : -1;
  fake.failedConversion=name=="conversion_failure" ? 7 : -1;
  expectFailure(nominal);
  fake.failedRead=fake.failedConversion=-1;
  fake.forcedVoltage=800;
  expectSuccess(nominal,static_cast<uint16_t>(800.0*nominal));
  assert(fake.unitCreates==1 && fake.calibrationCreates==1);
 } else if (name=="raw_negative" || name=="raw_overflow" || name=="voltage_negative" || name=="voltage_overflow") {
  fake.invalidNegativeRaw=name=="raw_negative";
  fake.forcedRaw=name=="raw_overflow" ? 4096 : -1;
  fake.invalidNegativeVoltage=name=="voltage_negative";
  fake.forcedVoltage=name=="voltage_overflow" ? std::numeric_limits<int>::max() : -1;
  expectFailure(nominal);
 } else if (name=="output_bounds") {
  fake.forcedVoltage=65535;
  expectSuccess(1,65535);
  expectFailure(std::nextafter(1.0f,2.0f));
  expectFailure(std::numeric_limits<float>::max());
  fake.forcedVoltage=0;
  expectSuccess(nominal,0);
  fake.forcedVoltage=800;
  expectSuccess(std::numeric_limits<float>::denorm_min(),0);
 } else if (name=="concurrent_reads") {
  fake.forcedVoltage=800;
  std::vector<std::thread> threads;
  std::atomic<bool> good{true};
  for(int i=0;i<8;++i) threads.emplace_back([&] {
   for(int j=0;j<10;++j) {
    uint16_t mv=0;
    if(!readHeltecV4BatteryMillivolts(5.5f,mv) || mv!=4400) good=false;
   }
  });
  for(auto &thread:threads) thread.join();
  assert(good && !fake.enabled && fake.unitCreates==1 && fake.calibrationCreates==1);
  assert(fake.reads==8*10*15 && fake.conversions==fake.reads);
 } else if (name=="unsupported_calibration") {
  expectFailure(nominal);
  assert(fake.units==0 && fake.calibrations==0 && fake.reads==0 && fake.enabledCount==0);
 } else assert(false);
}
#endif

int main(int argc,char **argv) {
 assert(argc==2);
 std::string name=argv[1];
 if(name=="resolver") testResolver();
#ifndef TEST_NATIVE_ONLY
 else testHardware(name);
#endif
 std::printf("PASS %s\n",name.c_str());
}
"""


def main():
    cases = [
        "resolver",
        "invalidation_and_recovery",
        "invalid_multiplier",
        "init_failure",
        "channel_failure",
        "calibration_init_failure",
        "cleanup_retry",
        "nonlinear_average_and_reuse",
        "explicit_factor_changes",
        "read_fail_first",
        "read_fail_middle",
        "read_fail_last",
        "conversion_failure",
        "raw_negative",
        "raw_overflow",
        "voltage_negative",
        "voltage_overflow",
        "output_bounds",
        "concurrent_reads",
    ]
    with tempfile.TemporaryDirectory(prefix="heltec-battery-adc-") as temporary:
        directory = Path(temporary)
        for name, contents in STUBS.items():
            target = directory / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(contents)
        for header in ("adc_oneshot.h", "adc_cali.h", "adc_cali_scheme.h"):
            target = directory / "esp_adc" / header
            target.parent.mkdir(exist_ok=True)
            target.write_text('#pragma once\n#include "idf_stubs.h"\n')
        source = directory / "test.cpp"
        source.write_text(TEST_CPP)
        compiler = shlex.split(os.environ.get("CXX", "g++"))
        compiler += [
            "-std=c++17",
            "-O1",
            "-g",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-pedantic",
            "-pthread",
        ]
        default_flags = (
            "-fsanitize=address,undefined,float-cast-overflow -fno-omit-frame-pointer -fno-pie -no-pie"
            if os.name == "posix"
            else ""
        )
        compiler += shlex.split(
            os.environ.get("BATTERY_ADC_TEST_CXXFLAGS", default_flags)
        )
        compiler += [f"-I{directory}", f"-I{ROOT / 'src'}"]
        environment = dict(
            os.environ,
            ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
            UBSAN_OPTIONS="halt_on_error=1",
        )
        for name, flags, scenarios in (
            ("hardware", [], cases),
            (
                "uncalibrated",
                ["-DADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED=0"],
                ["unsupported_calibration"],
            ),
            ("native", ["-DTEST_NATIVE_ONLY=1"], ["resolver"]),
        ):
            executable = directory / (name + (".exe" if os.name == "nt" else ""))
            command = (
                compiler
                + flags
                + [
                    str(ROOT / "src/power/HeltecV4BatteryAdc.cpp"),
                    str(source),
                    "-o",
                    str(executable),
                ]
            )
            subprocess.run(command, check=True)
            for scenario in scenarios:
                subprocess.run(
                    [str(executable), scenario], check=True, env=environment, timeout=30
                )
    print(
        "Heltec ADC: PASS (production reader/resolver; IDF fault injection; ASan, UBSan and float-cast-overflow when enabled)",
        flush=True,
    )


if __name__ == "__main__":
    main()
