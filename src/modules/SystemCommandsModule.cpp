#include "SystemCommandsModule.h"
#include "input/InputBroker.h"
#include "meshUtils.h"

#if HAS_SCREEN
#include "MessageStore.h"
#include "graphics/Screen.h"
#include "graphics/SharedUIDisplay.h"
#endif

#include "FSCommon.h"
#include "GPS.h"
#include "MeshService.h"
#include "Module.h"
#include "NodeDB.h"
#include "main.h"
#include "modules/AdminModule.h"
#include "modules/ExternalNotificationModule.h"
#include "power/PowerFSMPolicy.h"
#if !MESHTASTIC_EXCLUDE_GPS
#include "buzz.h"
#endif

SystemCommandsModule *systemCommandsModule;

SystemCommandsModule::SystemCommandsModule()
{
    if (inputBroker)
        inputObserver.observe(inputBroker);
}

int SystemCommandsModule::handleInputEvent(const InputEvent *event)
{
    LOG_INPUT("SystemCommands Input event %u! kb %u", event->inputEvent, event->kbchar);
    const auto runSerializedOperation = [](const auto &operation) {
#if defined(HELTEC_V4_OLED)
        if (adminModule)
            return adminModule->runExternalConfigMutation(operation);
#endif
        operation();
        return true;
    };
    const auto runConfigMutation = [](int saveWhat, const auto &operation) {
#if defined(HELTEC_V4_OLED)
        if (adminModule)
            return adminModule->runExternalConfigMutation(saveWhat, operation);
        if (!nodeDB || !nodeDB->beginPreferenceEdit(false))
            return false;
        operation();
        return service->reloadConfig(saveWhat, true);
#else
        operation();
        return service->reloadConfig(saveWhat);
#endif
    };
    // System commands (all others fall through)
    switch (event->kbchar) {
    // Fn key symbols
    case INPUT_BROKER_MSG_FN_SYMBOL_ON:
    case INPUT_BROKER_MSG_FN_SYMBOL_OFF:
        return 0;
    // Brightness
    case INPUT_BROKER_MSG_BRIGHTNESS_UP:
        IF_SCREEN(screen->increaseBrightness());
        LOG_DEBUG("Increase Screen Brightness");
        return 0;
    case INPUT_BROKER_MSG_BRIGHTNESS_DOWN:
        IF_SCREEN(screen->decreaseBrightness());
        LOG_DEBUG("Decrease Screen Brightness");
        return 0;
    // Mute
    case INPUT_BROKER_MSG_MUTE_TOGGLE:
        if (moduleConfig.external_notification.enabled && externalNotificationModule) {
            externalNotificationModule->setMute(!externalNotificationModule->getMute());
            IF_SCREEN(if (!externalNotificationModule->getMute()) externalNotificationModule->stopNow(); screen->showSimpleBanner(
                externalNotificationModule->getMute() ? "Notifications\nDisabled" : "Notifications\nEnabled", 3000);)
        }
        return 0;
    // Bluetooth
    case INPUT_BROKER_MSG_BLUETOOTH_TOGGLE: {
#if defined(HELTEC_V4_OLED)
        if (!shouldUsePersistentConfiguration(shouldUseFilesystemPersistence(fsIsMounted()), nodeDB->requiresConfigRecovery())) {
            LOG_WARN("Ignore Bluetooth toggle while persistent configuration is unavailable");
            return 0;
        }
#endif
        bool bluetoothEnabled = false;
        const bool applied = runConfigMutation(SEGMENT_CONFIG, [&]() {
            // Read and derive under the same Admin/NodeDB fence that commits
            // the result, so a just-finished phone edit cannot be overwritten
            // by a stale physical-button snapshot.
            bluetoothEnabled = !config.bluetooth.enabled;
            config.bluetooth.enabled = bluetoothEnabled;
            LOG_INFO("User toggled Bluetooth");
        });
        if (applied) {
#if defined(ARDUINO_ARCH_NRF52)
            if (!bluetoothEnabled) {
                disableBluetooth();
                IF_SCREEN(screen->showSimpleBanner("Bluetooth OFF\nRebooting", 3000));
                rebootAtMsec = millis() + DEFAULT_REBOOT_SECONDS * 2000;
            } else {
                IF_SCREEN(screen->showSimpleBanner("Bluetooth ON\nRebooting", 3000));
                rebootAtMsec = millis() + DEFAULT_REBOOT_SECONDS * 1000;
            }
#else
            if (!bluetoothEnabled) {
                disableBluetooth();
                if (shouldRestartAfterBluetoothDisable(bluetoothEnabled)) {
                    IF_SCREEN(screen->showSimpleBanner("Bluetooth OFF\nRebooting", 3000));
                    rebootAtMsec = millis() + DEFAULT_REBOOT_SECONDS * 1000;
                } else {
                    IF_SCREEN(screen->showSimpleBanner("Bluetooth OFF", 3000));
                }
            } else {
                IF_SCREEN(screen->showSimpleBanner("Bluetooth ON\nRebooting", 3000));
                rebootAtMsec = millis() + DEFAULT_REBOOT_SECONDS * 1000;
            }
#endif
        }
#if defined(HELTEC_V4_OLED)
        if (!applied) {
            LOG_WARN("Bluetooth change could not be committed");
            IF_SCREEN(screen->showSimpleBanner("Bluetooth change\nnot saved", 3000));
        }
#else
        (void)applied;
#endif
        return 0;
    }
    case INPUT_BROKER_MSG_REBOOT: {
        const bool applied = runSerializedOperation([&]() {
            IF_SCREEN(screen->showSimpleBanner("Rebooting...", 0));
            nodeDB->saveToDisk();
#if HAS_SCREEN
            messageStore.saveToFlash();
#endif
            rebootAtMsec = millis() + DEFAULT_REBOOT_SECONDS * 1000;
        });
#if defined(HELTEC_V4_OLED)
        if (!applied) {
            LOG_WARN("Ignore reboot input while an Admin settings transaction is active");
            IF_SCREEN(screen->showSimpleBanner("Settings import\nin progress", 3000));
        }
#else
        (void)applied;
#endif
        // runState = CANNED_MESSAGE_RUN_STATE_INACTIVE;
        return true;
    }
    }

    switch (event->inputEvent) {
        // GPS, on its own or together with the buzzer
    case INPUT_BROKER_GPS_TOGGLE:
    case INPUT_BROKER_PRIVACY_TOGGLE:
#if !MESHTASTIC_EXCLUDE_GPS
#if defined(HELTEC_V4_OLED)
        if (!shouldUseFilesystemPersistence(fsIsMounted()) || nodeDB->requiresConfigRecovery()) {
            LOG_WARN("Ignore GPS toggle while persistent configuration is unavailable");
            return true;
        }
#endif
        {
            if (!gps)
                return true;

            bool validGpsMode = false;
            bool withBuzzer = false;
            bool nowEnabled = false;
            // Conservatively include NODEDATABASE; whether the old position
            // needs clearing is derived only after the transaction owns the
            // shared generation.
            const bool applied = runConfigMutation(SEGMENT_CONFIG | SEGMENT_NODEDATABASE, [&]() {
                const bool wasEnabled = config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED;
                const bool wasDisabled = config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_DISABLED;
                // The physical toggle only moves between ENABLED and DISABLED. Leave other modes unchanged.
                if (!wasEnabled && !wasDisabled)
                    return;
                validGpsMode = true;
                withBuzzer = event->inputEvent == INPUT_BROKER_PRIVACY_TOGGLE;
                nowEnabled = !wasEnabled;
                if (wasEnabled && !config.position.fixed_position)
                    nodeDB->clearLocalPosition();
                config.position.gps_mode = nowEnabled ? meshtastic_Config_PositionConfig_GpsMode_ENABLED
                                                      : meshtastic_Config_PositionConfig_GpsMode_DISABLED;
                if (withBuzzer)
                    config.device.buzzer_mode = nowEnabled ? meshtastic_Config_DeviceConfig_BuzzerMode_ALL_ENABLED
                                                           : meshtastic_Config_DeviceConfig_BuzzerMode_DISABLED;
            });
            if (applied && validGpsMode) {
                LOG_INFO("User toggled GpsMode. Now %s", nowEnabled ? "ENABLED" : "DISABLED");
                if (nowEnabled) {
                    playGPSEnableBeep();
                    if (!gps->isEnabled())
                        gps->enable();
                } else {
                    // The combined privacy toggle persists DISABLED, but the
                    // confirmation tone must still be audible before taking
                    // effect in RAM.
                    if (withBuzzer)
                        config.device.buzzer_mode = meshtastic_Config_DeviceConfig_BuzzerMode_ALL_ENABLED;
                    playGPSDisableBeep();
                    if (withBuzzer)
                        config.device.buzzer_mode = meshtastic_Config_DeviceConfig_BuzzerMode_DISABLED;
                    if (gps->isEnabled())
                        gps->disable();
                }
                const char *msg = withBuzzer ? (nowEnabled ? "GPS + Buzzer\nEnabled" : "GPS + Buzzer\nDisabled")
                                             : (nowEnabled ? "GPS Enabled" : "GPS Disabled");
                IF_SCREEN(screen->forceDisplay(); screen->showSimpleBanner(msg, 3000);)
            }
#if defined(HELTEC_V4_OLED)
            if (!applied) {
                LOG_WARN("GPS change could not be committed");
                IF_SCREEN(screen->showSimpleBanner("GPS change\nnot saved", 3000));
            }
#else
            (void)applied;
#endif
        }
#endif
        return true;
    // Mesh ping
    case INPUT_BROKER_SEND_PING:
#if defined(HELTEC_V4_OLED)
        if (!shouldUseFilesystemPersistence(fsIsMounted()) || nodeDB->requiresConfigRecovery()) {
            LOG_WARN("Ignore mesh ping while persistent configuration is unavailable");
            return true;
        }
#endif
        service->refreshLocalMeshNode();
        if (service->trySendPosition(NODENUM_BROADCAST, true)) {
            IF_SCREEN(screen->showSimpleBanner("Position\nSent", 3000));
        } else {
            IF_SCREEN(screen->showSimpleBanner("Node Info\nSent", 3000));
        }
        return true;
    // Power control
    case INPUT_BROKER_SHUTDOWN: {
        const bool applied = runSerializedOperation([&]() { shutdownAtMsec = millis(); });
#if defined(HELTEC_V4_OLED)
        if (!applied) {
            LOG_WARN("Ignore shutdown input while an Admin settings transaction is active");
            IF_SCREEN(screen->showSimpleBanner("Settings import\nin progress", 3000));
        }
#else
        (void)applied;
#endif
        return true;
    }
    // factory reset
    case INPUT_BROKER_FACTORY_RST: {
        const auto resetOperation = [&]() {
            LOG_INFO("Initiate full factory reset");
            if (nodeDB->factoryReset(true)) {
                disableBluetooth();
                // reboot(DEFAULT_REBOOT_SECONDS);
                LOG_INFO("Reboot in %d seconds", DEFAULT_REBOOT_SECONDS);
                if (screen)
                    screen->showSimpleBanner("Rebooting...", 0); // stays on screen
                rebootAtMsec = (DEFAULT_REBOOT_SECONDS < 0) ? 0 : (millis() + DEFAULT_REBOOT_SECONDS * 1000);
                return true;
            } else {
                LOG_ERROR("Factory reset failed; keeping recovery interfaces available");
                IF_SCREEN(screen->showSimpleBanner("Factory reset\nfailed", 3000);)
                return false;
            }
        };
#if defined(HELTEC_V4_OLED)
        if (adminModule)
            adminModule->runExternalRecoveryMutation(resetOperation);
        else
            resetOperation();
#else
        resetOperation();
#endif
        return true;
    }

    default:
        // No other input events handled here
        break;
    }
    return false;
}
