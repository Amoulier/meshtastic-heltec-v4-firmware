#!/usr/bin/env python3
"""Exercise production battery cache and early-boot policy with deterministic ADC failures."""

import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def body(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for position in range(brace, len(source)):
        depth += (source[position] == "{") - (source[position] == "}")
        if depth == 0:
            return source[start : position + 1]
    raise AssertionError(signature)


def run(name, source, flags):
    with tempfile.TemporaryDirectory(prefix="battery-pipeline-") as directory:
        cpp = Path(directory) / "test.cpp"
        executable = Path(directory) / "test"
        cpp.write_text(source)
        subprocess.run(
            [
                "g++",
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-O1",
                "-g",
                "-fsanitize=address,undefined,float-cast-overflow",
                "-fno-omit-frame-pointer",
                "-fno-pie",
                "-no-pie",
                f"-I{ROOT / 'src'}",
                *flags,
                str(cpp),
                "-o",
                str(executable),
            ],
            check=True,
        )
        subprocess.run(
            [str(executable)],
            env=dict(
                os.environ,
                ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
                UBSAN_OPTIONS="halt_on_error=1",
            ),
            check=True,
            timeout=30,
        )
    print(
        f"{name}: PASS (production bodies, ASan/UBSan/float-cast-overflow)", flush=True
    )


cache = (
    r"""
#include <cassert>
#include <cstdint>
uint32_t nowMs = 0;
uint32_t millis() { return nowMs; }
namespace Throttle { bool hasElapsed(uint32_t last, uint32_t interval) { return uint32_t(nowMs-last) >= interval; } }
float activeMultiplier = 5.0f;
float getActiveHeltecV4AdcMultiplier() { return activeMultiplier; }
int readCalls = 0;
uint16_t pinMillivolts = 800;
bool adcValid = true;
bool readHeltecV4BatteryMillivolts(float multiplier, uint16_t &mv) {
    ++readCalls;
    mv = adcValid ? static_cast<uint16_t>(pinMillivolts * multiplier) : 60000;
    return adcValid;
}
struct AnalogBatteryLevel {
    bool initial_read_done = false, read_attempted = false, latest_read_valid = false, forceFreshRawRead = false;
    float last_adc_multiplier = 0, last_read_value = 3100;
    uint32_t last_read_time_ms = 0;
    uint16_t latest_raw_read_value = 0;
    uint16_t getHeltecBattVoltage();
};
"""
    + "\nuint16_t AnalogBatteryLevel::"
    + body("src/Power.cpp", "uint16_t getHeltecBattVoltage()").removeprefix("uint16_t ")
    + r"""
int main() {
    AnalogBatteryLevel battery;
    assert(battery.getHeltecBattVoltage() == 4000 && readCalls == 1);
    nowMs = 4999;
    pinMillivolts = 600;
    assert(battery.getHeltecBattVoltage() == 4000 && readCalls == 1);
    nowMs = 5000;
    assert(battery.getHeltecBattVoltage() == 3500 && readCalls == 2);
    assert(battery.latest_raw_read_value == 3000);
    activeMultiplier = 6.0f;
    assert(battery.getHeltecBattVoltage() == 3600 && readCalls == 3);
    adcValid = false;
    battery.forceFreshRawRead = true;
    assert(battery.getHeltecBattVoltage() == 3600);
    assert(!battery.latest_read_valid && battery.latest_raw_read_value == 0);
    battery.forceFreshRawRead = false;
    const int failedCalls = readCalls;
    assert(battery.getHeltecBattVoltage() == 3600 && readCalls == failedCalls);
    activeMultiplier = 5.0f;
    assert(battery.getHeltecBattVoltage() == 0 && !battery.latest_read_valid);
    const int changedFailedCalls = readCalls;
    assert(battery.getHeltecBattVoltage() == 0 && readCalls == changedFailedCalls);
    adcValid = true;
    nowMs += 5000;
    assert(battery.getHeltecBattVoltage() == 3000 && battery.latest_read_valid);
    nowMs = UINT32_MAX - 1000;
    battery.forceFreshRawRead = true;
    battery.getHeltecBattVoltage();
    battery.forceFreshRawRead = false;
    const int wrapCalls = readCalls;
    nowMs = 3998;
    battery.getHeltecBattVoltage();
    assert(readCalls == wrapCalls);
    nowMs = 3999;
    battery.getHeltecBattVoltage();
    assert(readCalls == wrapCalls + 1);
    AnalogBatteryLevel absent;
    pinMillivolts = 440;
    assert(absent.getHeltecBattVoltage() == 2200);
    pinMillivolts = 0;
    absent.forceFreshRawRead = true;
    absent.getHeltecBattVoltage();
    assert(absent.latest_read_valid && absent.latest_raw_read_value == 0);
}
"""
)

early = (
    r"""
#include <cassert>
#include <cstdint>
#include "power/BatteryCriticalPolicy.h"
#include "power/DeepSleepPolicy.h"
#include "power/HeltecV4BatteryCalibration.h"
constexpr float ADC_MULTIPLIER = 5.1205f;
constexpr int ADC_CTRL = 37, BATTERY_PIN = 1, LED_POWER = 35, HIGH = 1;
constexpr int BATTERY_BOOT_GUARD_MIN_MILLIVOLTS = 2500, BATTERY_CRITICAL_MILLIVOLTS = 3500;
constexpr int BATTERY_CRITICAL_RECOVERY_MILLIVOLTS = 3650, ESP_SLEEP_WAKEUP_TIMER = 4;
HeltecV4BatteryCalibration stored{HeltecV4BatteryCalibrationStatus::MISSING, ADC_MULTIPLIER};
float active = 0, readMultiplier = 0;
bool batteryCriticalLatched = false, usb = false, readingValid = true;
int releases = 0, reads = 0, sleeps = 0;
uint16_t readMv = 4000;
HeltecV4BatteryCalibration readHeltecV4BatteryCalibration(float fallback) { assert(fallback == ADC_MULTIPLIER); return stored; }
void setActiveHeltecV4AdcMultiplier(float factor) { active = factor; }
void invalidateHeltecV4AdcCalibration() { active = 0; }
bool readHeltecV4BatteryMillivolts(float factor, uint16_t &mv) { ++reads; readMultiplier = factor; mv = readMv; return readingValid; }
void releaseEarlyPinHold(int) { ++releases; }
bool hasActiveUsbDataHost() { return usb; }
int esp_sleep_get_wakeup_cause() { return ESP_SLEEP_WAKEUP_TIMER; }
void enterBatteryRecoverySleep(bool) { ++sleeps; throw 1; }
void releaseBatteryRecoveryHolds() { ++releases; }
void digitalWrite(int, int) {}
"""
    + body("variants/esp32s3/heltec_v4/variant.cpp", "void earlyInitVariant()")
    + r"""
bool boots() { try { earlyInitVariant(); return true; } catch (int) { return false; } }
int main() {
    assert(boots() && active == ADC_MULTIPLIER);
    stored = {HeltecV4BatteryCalibrationStatus::VALID, 5.5f};
    assert(boots() && active == 5.5f);
#if defined(HELTEC_V4_SOLAR_ROUTER_PROFILE)
    assert(readMultiplier == active && reads == 2);
    readMv = 3500;
    assert(!boots());
    readMv = 3649;
    assert(!boots());
    readMv = 3650;
    assert(boots() && !batteryCriticalLatched);
    readMv = 2499;
    assert(boots());
    readingValid = false;
    assert(!boots());
    usb = true;
    assert(boots());
    usb = false;
    readingValid = true;
    readMv = 4000;
    stored.status = HeltecV4BatteryCalibrationStatus::INVALID;
    assert(!boots());
    usb = true;
    assert(boots());
    usb = false;
    stored.status = HeltecV4BatteryCalibrationStatus::UNAVAILABLE;
    assert(!boots());
    stored.status = HeltecV4BatteryCalibrationStatus::MISSING;
    assert(boots());
#else
    assert(reads == 0 && sleeps == 0);
    stored.status = HeltecV4BatteryCalibrationStatus::INVALID;
    assert(boots() && active == 0);
    stored.status = HeltecV4BatteryCalibrationStatus::UNAVAILABLE;
    assert(boots() && active == 0);
    stored.status = HeltecV4BatteryCalibrationStatus::VALID;
    assert(boots() && active == 5.5f);
#endif
}
"""
)

for profile, flags in (
    ("standard", ["-DHELTEC_V4_OLED=1"]),
    ("solar", ["-DHELTEC_V4_OLED=1", "-DHELTEC_V4_SOLAR_ROUTER_PROFILE=1"]),
):
    run(f"{profile}-battery-cache", cache, flags)
    run(f"{profile}-early-calibration", early, flags)
