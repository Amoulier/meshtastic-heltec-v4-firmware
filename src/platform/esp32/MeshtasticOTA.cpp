// First, in its own block so the include sorter keeps it there: configuration.h supplies the
// variant defines mesh-pb-constants.h needs (portduino resolves MAX_NUM_NODES at runtime).
#include "configuration.h"

#include "MeshtasticOTA.h"
#if defined(HELTEC_V4_OLED)
#include "Power.h"
#endif
#ifdef ESP_PLATFORM
#include <Preferences.h>
#include <esp_ota_ops.h>
#endif

namespace MeshtasticOTA
{

static const char *nvsNamespace = "MeshtasticOTA";
static const char *combinedAppProjectName = "MeshtasticOTA";
static const char *bleOnlyAppProjectName = "MeshtasticOTA-BLE";
static const char *wifiOnlyAppProjectName = "MeshtasticOTA-WiFi";

static bool updated = false;

bool isUpdated()
{
    return updated;
}

void initialize()
{
    Preferences prefs;
    prefs.begin(nvsNamespace);
    if (prefs.getBool("updated")) {
        LOG_INFO("First boot after OTA update");
        updated = true;
#if defined(HELTEC_V4_OLED)
        if (heltecPreferenceStoragePowerIsSafe())
            prefs.putBool("updated", false);
        else
            LOG_WARN("OTA completion marker retained while fresh power is unsafe");
#else
        prefs.putBool("updated", false);
#endif
    }
    prefs.end();
}

void recoverConfig(meshtastic_Config_NetworkConfig *network)
{
    if (!network) {
        LOG_ERROR("OTA settings recovery: missing network config");
        return;
    }

    Preferences prefs;
    if (!prefs.begin(nvsNamespace, true)) {
        LOG_ERROR("OTA settings recovery: cannot open NVS namespace");
        return;
    }

    const uint8_t method = prefs.getUChar("method", UINT8_MAX);
    if (method != METHOD_OTA_WIFI) {
        // BLE OTA must not revive credentials left by an earlier WiFi OTA.
        // In particular, degraded storage recovery deliberately keeps WiFi
        // off while exposing only local BLE/USB administration.
        LOG_INFO("Non-WiFi OTA completed; leaving WiFi settings unchanged");
        prefs.end();
        return;
    }

    LOG_INFO("Recovering WiFi settings after WiFi OTA update");
    String ssid = prefs.getString("ssid");
    String psk = prefs.getString("psk");
    prefs.end();

    network->wifi_enabled = true;
    strlcpy(network->wifi_ssid, ssid.c_str(), sizeof(network->wifi_ssid));
    strlcpy(network->wifi_psk, psk.c_str(), sizeof(network->wifi_psk));
}

bool saveConfig(const meshtastic_Config_NetworkConfig *network, meshtastic_OTAMode method, const uint8_t *ota_hash)
{
    LOG_INFO("Saving verified settings for upcoming OTA update");

    if (!network || !ota_hash || (method != METHOD_OTA_BLE && method != METHOD_OTA_WIFI)) {
        LOG_ERROR("OTA settings: invalid arguments");
        return false;
    }

#if defined(HELTEC_V4_OLED)
    // OTA crosses both NVS and boot-partition commit boundaries. Use the
    // reset/restore margin, not the ordinary preference floor.
    if (!heltecDestructiveStoragePowerIsSafe()) {
        LOG_ERROR("OTA settings refused while fresh power is below the safe update threshold");
        return false;
    }
#endif

    Preferences prefs;
    if (!prefs.begin(nvsNamespace, false)) {
        LOG_ERROR("OTA settings: cannot open NVS namespace");
        return false;
    }

    prefs.putUChar("method", method);
    prefs.putBytes("ota_hash", ota_hash, 32);
    if (method == METHOD_OTA_WIFI) {
        prefs.putString("ssid", network->wifi_ssid);
        prefs.putString("psk", network->wifi_psk);
    } else {
        // Remove stale credentials from a prior WiFi OTA. Recovery is already
        // method-gated, but clearing them prevents future code from treating
        // unrelated historical values as current configuration.
        prefs.remove("ssid");
        prefs.remove("psk");
    }
    prefs.putBool("updated", false);

    uint8_t savedHash[32] = {};
    const bool coreVerified = prefs.getUChar("method", UINT8_MAX) == method &&
                              prefs.getBytesLength("ota_hash") == sizeof(savedHash) &&
                              prefs.getBytes("ota_hash", savedHash, sizeof(savedHash)) == sizeof(savedHash) &&
                              memcmp(savedHash, ota_hash, sizeof(savedHash)) == 0 && !prefs.getBool("updated", true);
    bool networkVerified = true;
    if (method == METHOD_OTA_WIFI) {
        const String savedSsid = prefs.getString("ssid", "");
        const String savedPsk = prefs.getString("psk", "");
        networkVerified = strcmp(savedSsid.c_str(), network->wifi_ssid) == 0 && strcmp(savedPsk.c_str(), network->wifi_psk) == 0;
    } else {
        networkVerified = !prefs.isKey("ssid") && !prefs.isKey("psk");
    }
    prefs.end();

    if (!coreVerified || !networkVerified) {
        LOG_ERROR("OTA settings failed NVS readback verification");
        return false;
    }
    return true;
}

const esp_partition_t *getAppPartition()
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
}

bool getAppDesc(const esp_partition_t *part, esp_app_desc_t *app_desc)
{
    if (esp_ota_get_partition_description(part, app_desc) != ESP_OK) {
        LOG_INFO("esp_ota_get_partition_description failed");
        return false;
    }
    return true;
}

bool checkOTACapability(const esp_app_desc_t *app_desc, uint8_t method)
{
    // Combined loader supports all (both) transports, BLE and WiFi
    if (strcmp(app_desc->project_name, combinedAppProjectName) == 0) {
        LOG_INFO("OTA partition contains combined BLE/WiFi OTA Loader");
        return true;
    }
    if (method == METHOD_OTA_BLE && strcmp(app_desc->project_name, bleOnlyAppProjectName) == 0) {
        LOG_INFO("OTA partition contains BLE-only OTA Loader");
        return true;
    }
    if (method == METHOD_OTA_WIFI && strcmp(app_desc->project_name, wifiOnlyAppProjectName) == 0) {
        LOG_INFO("OTA partition contains WiFi-only OTA Loader");
        return true;
    }
    LOG_INFO("OTA partition does not contain a known OTA loader");
    return false;
}

bool trySwitchToOTA()
{
#if defined(HELTEC_V4_OLED)
    // Re-sample after NVS persistence: a BLE/WiFi request can outlive a load
    // sag, and selecting the loader must never rely on stale authorization.
    if (!heltecDestructiveStoragePowerIsSafe()) {
        LOG_ERROR("OTA partition switch refused while fresh power is below the safe update threshold");
        return false;
    }
#endif
    const esp_partition_t *part = getAppPartition();

    if (part == NULL) {
        LOG_WARN("Can't get app partition in preparation of OTA reboot");
        return false;
    }

    uint8_t result = esp_ota_set_boot_partition(part);
    // Partition and app checks should now be done in the AdminModule before this is called
    if (result != ESP_OK) {
        LOG_WARN("Can't switch to OTA partition (reason %d)", result);
        return false;
    }

    return true;
}

const char *getVersion()
{
    const esp_partition_t *part = getAppPartition();
    static esp_app_desc_t app_desc;
    if (!getAppDesc(part, &app_desc))
        return "";
    return app_desc.version;
}

} // namespace MeshtasticOTA
