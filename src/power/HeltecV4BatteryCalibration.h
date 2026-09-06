#pragma once

#include <cstdint>

enum class HeltecV4BatteryCalibrationStatus : uint8_t { VALID, MISSING, INVALID, UNAVAILABLE };

struct HeltecV4BatteryCalibration {
    HeltecV4BatteryCalibrationStatus status;
    float multiplier;
};

HeltecV4BatteryCalibration readHeltecV4BatteryCalibration(float fallbackMultiplier);

// The caller serializes NVS mutations and supplies fresh power authorization; this helper never initializes or erases NVS.
bool writeHeltecV4BatteryCalibration(float resolvedMultiplier, bool (*powerIsSafe)());
