#pragma once

constexpr bool shouldKeepBluetoothConnectableDuringIdle(bool heltecV4Oled, bool bluetoothEnabled)
{
    return heltecV4Oled && bluetoothEnabled;
}

constexpr bool shouldKeepBluetoothConnectableDuringIdle(bool bluetoothEnabled)
{
#if defined(HELTEC_V4_OLED)
    return shouldKeepBluetoothConnectableDuringIdle(true, bluetoothEnabled);
#else
    return shouldKeepBluetoothConnectableDuringIdle(false, bluetoothEnabled);
#endif
}

// The ESP32 power-state graph is assembled once during boot. If the user turns
// Bluetooth off from the physical menu on a Heltec V4, restart after saving so
// the graph is rebuilt with its normal Router/power-saving sleep transitions.
// Enabling Bluetooth already follows a restart path.
constexpr bool shouldRestartAfterBluetoothDisable(bool heltecV4Oled, bool bluetoothEnabled)
{
    return heltecV4Oled && !bluetoothEnabled;
}

constexpr bool shouldRestartAfterBluetoothDisable(bool bluetoothEnabled)
{
#if defined(HELTEC_V4_OLED)
    return shouldRestartAfterBluetoothDisable(true, bluetoothEnabled);
#else
    return shouldRestartAfterBluetoothDisable(false, bluetoothEnabled);
#endif
}

constexpr bool shouldUseNoBluetoothStateAfterLightSleep(bool isEsp32, bool isRouterRole, bool keepBluetoothConnectable)
{
    return isEsp32 && isRouterRole && !keepBluetoothConnectable;
}

constexpr bool shouldEnterLightSleepFromIdle(bool isEsp32, bool isRouterRole, bool isPowerSaving, bool wifiAvailable,
                                             bool isTrackerOrSensor, bool keepBluetoothConnectable)
{
    return isEsp32 && (isRouterRole || isPowerSaving) && !wifiAvailable && !isTrackerOrSensor && !keepBluetoothConnectable;
}
