#pragma once

#include <cstdint>

bool resolveHeltecV4AdcMultiplier(float overrideValue, float &resolved);
float getActiveHeltecV4AdcMultiplier();
void setActiveHeltecV4AdcMultiplier(float resolved);
void invalidateHeltecV4AdcCalibration();

bool readHeltecV4BatteryMillivolts(float resolvedMultiplier, uint16_t &millivolts);
