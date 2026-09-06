#include "configuration.h"
#if !MESHTASTIC_EXCLUDE_GPS
#include "GPS.h"
#endif
#include "../detect/ScanI2C.h"
#include "Channels.h"
#include "CryptoEngine.h"
#include "Default.h"
#include "FSCommon.h"
#include "MeshRadio.h"
#include "MeshService.h"
#include "MessageStore.h"
#include "NodeDB.h"
#include "PacketHistory.h"
#include "Power.h"
#include "PowerFSM.h"
#include "PowerStatus.h"
#include "RadioInterface.h"
#include "Router.h"
#include "SPILock.h"
#include "SafeFile.h"
#include "Throttle.h"
#include "TransmitHistory.h"
#include "TypeConversions.h"
#include "UptimeClock.h"
#if HAS_SCREEN && !MESHTASTIC_EXCLUDE_WAYPOINT
#include "WaypointStore.h"
#endif
#include "error.h"
#include "gps/RTC.h"
#include "main.h"
#include "memory/MemAudit.h"
#include "mesh-pb-constants.h"
#include "mesh/generated/meshtastic/deviceonly_legacy.pb.h"
#include "meshUtils.h"
#include "modules/NeighborInfoModule.h"
#include "target_specific.h"
#if HAS_VARIABLE_HOPS
#include "modules/HopScalingModule.h"
#endif
#if HAS_TRAFFIC_MANAGEMENT
#include "modules/TrafficManagementModule.h"
#endif
#include "xmodem.h"
#include <ErriezCRC32.h>
#include <algorithm>
#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)
#include <Curve25519.h>
#endif
#include <pb_decode.h>
#include <pb_encode.h>
#include <power/PowerHAL.h>
#include <vector>

void disableBluetooth();

#if defined(HELTEC_V4_OLED)
concurrency::Lock heltecV4NvsMutationLock;
#endif

#ifdef MESHTASTIC_ENCRYPTED_STORAGE
#include "security/EncryptedStorage.h"
#include "security/SecureZero.h"
#endif

namespace
{
bool isCompleteChannelFile(const meshtastic_ChannelFile &candidate)
{
    if (candidate.channels_count != MAX_NUM_CHANNELS)
        return false;
    size_t primaryCount = 0;
    for (pb_size_t i = 0; i < candidate.channels_count; ++i) {
        const meshtastic_Channel &channel = candidate.channels[i];
        if (channel.role != meshtastic_Channel_Role_DISABLED && channel.role != meshtastic_Channel_Role_PRIMARY &&
            channel.role != meshtastic_Channel_Role_SECONDARY)
            return false;
        if ((channel.role == meshtastic_Channel_Role_PRIMARY || channel.role == meshtastic_Channel_Role_SECONDARY) &&
            !channel.has_settings)
            return false;
        if (channel.role == meshtastic_Channel_Role_PRIMARY)
            ++primaryCount;
    }
    return primaryCount == 1;
}
} // namespace

#if defined(HELTEC_V4_OLED) && defined(FSCom)
namespace
{
enum class HeltecResetPendingKind : uint8_t { NONE, EDIT, NODEDB_RESET, CONFIG_ONLY, RESTORE, FULL, FULL_OR_UNKNOWN };
bool removeHeltecPreferenceResidualsChecked(bool retainNodeDatabase, bool retainLegacyMarker);
bool removeHeltecPreferenceTreeChecked(const char *directory);
bool removeHeltecPersistedUserContentChecked();
bool writeHeltecResetPendingMarker(HeltecResetPendingKind kind, bool requireDestructivePower);
bool clearHeltecResetPendingMarker(HeltecResetPendingKind expectedKind, bool requireDestructivePower);
HeltecResetPendingKind readHeltecResetPendingMarker();
bool eraseHeltecNvsExceptRecoveryMarker();
} // namespace
#endif

#ifdef ARCH_ESP32
#if HAS_WIFI
#include "mesh/wifi/WiFiAPClient.h"
#endif
#include "SPILock.h"
#include "modules/StoreForwardModule.h"
#include <Preferences.h>
#include <nvs.h>
#include <nvs_flash.h>
#endif

#ifdef ARCH_PORTDUINO
#include "modules/StoreForwardModule.h"
#include "platform/portduino/PortduinoGlue.h"
#endif

#ifdef ARCH_NRF52
#include <bluefruit.h>
#include <utility/bonding.h>
#endif

#ifdef ARCH_RP2040
#include <hardware/watchdog.h>
#endif

#if defined(ARCH_ESP32) && !MESHTASTIC_EXCLUDE_WIFI
#include <MeshtasticOTA.h>
#endif

NodeDB *nodeDB = nullptr;

// we have plenty of ram so statically alloc this tempbuf (for now)
EXT_RAM_BSS_ATTR meshtastic_DeviceState devicestate;
meshtastic_MyNodeInfo &myNodeInfo = devicestate.my_node;
meshtastic_NodeDatabase nodeDatabase;
meshtastic_LocalConfig config;
meshtastic_DeviceUIConfig uiconfig{.screen_brightness = 153, .screen_timeout = 30};
meshtastic_LocalModuleConfig moduleConfig;
meshtastic_ChannelFile channelFile;

static void forceHeltecLocalRecoveryConfiguration()
{
#if defined(HELTEC_V4_OLED)
    config.lora.region = meshtastic_Config_LoRaConfig_RegionCode_UNSET;
    config.lora.tx_enabled = false;
    config.position.gps_mode = meshtastic_Config_PositionConfig_GpsMode_DISABLED;
    config.network.wifi_enabled = false;
    config.network.eth_enabled = false;
    config.network.enabled_protocols = meshtastic_Config_NetworkConfig_ProtocolFlags_NO_BROADCAST;
    config.bluetooth.enabled = true;
#endif
}

#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)
static bool derivePublicKeyWithoutInstalling(const uint8_t *privateKey, uint8_t *publicKey)
{
    if (memfll(privateKey, 0, 32))
        return false;
    Curve25519::eval(publicKey, privateKey, 0);
    if (Curve25519::isWeakPoint(publicKey)) {
        memset(publicKey, 0, 32);
        return false;
    }
    return true;
}
#endif

#ifdef USERPREFS_USE_ADMIN_KEY_0
static unsigned char userprefs_admin_key_0[] = USERPREFS_USE_ADMIN_KEY_0;
#endif
#ifdef USERPREFS_USE_ADMIN_KEY_1
static unsigned char userprefs_admin_key_1[] = USERPREFS_USE_ADMIN_KEY_1;
#endif
#ifdef USERPREFS_USE_ADMIN_KEY_2
static unsigned char userprefs_admin_key_2[] = USERPREFS_USE_ADMIN_KEY_2;
#endif

// Weak empty variant initialization function.
// May be redefined by variant files.
// noinline: weak default and call site share this TU, so LTO would inline the empty body and
// never link the variant's strong override. Same guard as earlyInitVariant() in main.cpp.
__attribute__((noinline)) void variantDefaultConfig() __attribute__((weak));
__attribute__((noinline)) void variantDefaultConfig() {}

__attribute__((noinline)) void variantDefaultModuleConfig() __attribute__((weak));
__attribute__((noinline)) void variantDefaultModuleConfig() {}

#if defined(HELTEC_MESH_NODE_T114) || defined(TFT_NV3001B_DETECT)

uint32_t read8(uint8_t bits, uint8_t dummy, uint8_t cs, uint8_t sck, uint8_t mosi, uint8_t dc, uint8_t rst)
{
    uint32_t ret = 0;
    uint8_t SDAPIN = mosi;
    pinMode(SDAPIN, INPUT_PULLUP);
    digitalWrite(dc, HIGH);
    for (int i = 0; i < dummy; i++) { // any dummy clocks
        digitalWrite(sck, HIGH);
        delay(1);
        digitalWrite(sck, LOW);
        delay(1);
    }
    for (int i = 0; i < bits; i++) { // read results
        ret <<= 1;
        delay(1);
        if (digitalRead(SDAPIN))
            ret |= 1;
        ;
        digitalWrite(sck, HIGH);
        delay(1);
        digitalWrite(sck, LOW);
    }
    return ret;
}

void write9(uint8_t val, uint8_t dc_val, uint8_t cs, uint8_t sck, uint8_t mosi, uint8_t dc, uint8_t rst)
{
    pinMode(mosi, OUTPUT);
    digitalWrite(dc, dc_val);
    for (int i = 0; i < 8; i++) { // send command
        digitalWrite(mosi, (val & 0x80) != 0);
        delay(1);
        digitalWrite(sck, HIGH);
        delay(1);
        digitalWrite(sck, LOW);
        val <<= 1;
    }
}

uint32_t readwrite8(uint8_t cmd, uint8_t bits, uint8_t dummy, uint8_t cs, uint8_t sck, uint8_t mosi, uint8_t dc, uint8_t rst)
{
    digitalWrite(cs, LOW);
    write9(cmd, 0, cs, sck, mosi, dc, rst);
    uint32_t ret = read8(bits, dummy, cs, sck, mosi, dc, rst);
    digitalWrite(cs, HIGH);
    return ret;
}

#endif

#ifdef HELTEC_MESH_NODE_T114

uint32_t get_st7789_id(uint8_t cs, uint8_t sck, uint8_t mosi, uint8_t dc, uint8_t rst)
{
    pinMode(cs, OUTPUT);
    digitalWrite(cs, HIGH);
    pinMode(cs, OUTPUT);
    pinMode(sck, OUTPUT);
    pinMode(mosi, OUTPUT);
    pinMode(dc, OUTPUT);
    pinMode(rst, OUTPUT);
    digitalWrite(rst, LOW); // Hardware Reset
    delay(10);
    digitalWrite(rst, HIGH);
    delay(10);

    readwrite8(0x04, 24, 1, cs, sck, mosi, dc, rst);
    uint32_t ID = readwrite8(0x04, 24, 1, cs, sck, mosi, dc, rst); // ST7789 needs twice
    return ID;
}

#endif

#ifdef TFT_NV3001B_DETECT

// The NV3001B panel is an add-on module on these boards, so probe for it before assuming a screen.
static constexpr uint32_t NV3001B_PANEL_ID = 0x300101;
static constexpr uint32_t NV3001B_RESET_DELAY_MS = 120; // NV3001B_RST_DELAY, per the Arduino_GFX driver

bool nv3001bPanelPresent(uint8_t cs, uint8_t sck, uint8_t mosi, uint8_t dc, uint8_t rst, uint8_t en, uint8_t bl)
{
    pinMode(en, OUTPUT);
    digitalWrite(en, TFT_EN_ON);
    pinMode(bl, OUTPUT);
    digitalWrite(bl, TFT_BACKLIGHT_ON);
    delay(NV3001B_RESET_DELAY_MS);

    pinMode(cs, OUTPUT);
    digitalWrite(cs, HIGH);
    pinMode(sck, OUTPUT);
    digitalWrite(sck, LOW);
    pinMode(mosi, OUTPUT);
    pinMode(dc, OUTPUT);
    pinMode(rst, OUTPUT);
    digitalWrite(rst, HIGH);
    delay(NV3001B_RESET_DELAY_MS);
    digitalWrite(rst, LOW); // Hardware Reset
    delay(NV3001B_RESET_DELAY_MS);
    digitalWrite(rst, HIGH);
    delay(NV3001B_RESET_DELAY_MS);

    // 0x04 reports the whole 24-bit display ID; 0xDA/0xDB/0xDC report it one byte at a time.
    // A panel that answers either way is present.
    uint32_t rddid = readwrite8(0x04, 24, 1, cs, sck, mosi, dc, rst);
    uint32_t rdid = (readwrite8(0xDA, 8, 0, cs, sck, mosi, dc, rst) << 16) |
                    (readwrite8(0xDB, 8, 0, cs, sck, mosi, dc, rst) << 8) | readwrite8(0xDC, 8, 0, cs, sck, mosi, dc, rst);
    LOG_INFO("NV3001B probe RDDID=0x%06x RDID=0x%06x", (unsigned int)rddid, (unsigned int)rdid);

    if (rddid == NV3001B_PANEL_ID || rdid == NV3001B_PANEL_ID) {
        LOG_INFO("NV3001B panel detected");
        return true;
    }

    // All ones means the data line floated, all zeroes means something held it low; either way no panel
    // answered, so drop the rail again rather than leave an empty header powered.
    LOG_INFO("NV3001B panel not detected");
    digitalWrite(bl, TFT_BACKLIGHT_OFF);
    digitalWrite(en, TFT_EN_OFF);
    pinMode(en, INPUT);
    return false;
}

#endif

// When armed by loadFromDisk, the decode callback writes satellite entries
// straight into these maps instead of the temp vectors. Nullptr = legacy
// push_back-to-vector path for backup/restore and other decoders.
namespace
{
#if !MESHTASTIC_EXCLUDE_POSITIONDB
std::map<NodeNum, meshtastic_PositionLite> *s_decodePositionsTarget = nullptr;
#endif
#if !MESHTASTIC_EXCLUDE_TELEMETRYDB
std::map<NodeNum, meshtastic_DeviceMetrics> *s_decodeTelemetryTarget = nullptr;
#endif
#if !MESHTASTIC_EXCLUDE_ENVIRONMENTDB
std::map<NodeNum, meshtastic_EnvironmentMetrics> *s_decodeEnvironmentTarget = nullptr;
#endif
#if !MESHTASTIC_EXCLUDE_STATUSDB
std::map<NodeNum, meshtastic_StatusMessage> *s_decodeStatusTarget = nullptr;
#endif
} // namespace

bool meshtastic_NodeDatabase_callback(pb_istream_t *istream, pb_ostream_t *ostream, const pb_field_t *field)
{
    const auto *iter = reinterpret_cast<const pb_field_iter_t *>(field);
    switch (iter->tag) {
    case meshtastic_NodeDatabase_nodes_tag: {
        if (ostream) {
            const auto *vec = static_cast<const std::vector<meshtastic_NodeInfoLite> *>(iter->pData);
            for (auto item : *vec) {
                // Round rather than truncate: truncation wiped any |SNR| < 0.25 dB to exactly
                // 0, which collided with the "never stored" sentinel below.
                item.snr_q4 = (int32_t)lroundf(item.snr * 4.0f);
                item.snr = 0.0f;
                if (!pb_encode_tag_for_field(ostream, iter))
                    return false;
                if (!pb_encode_submessage(ostream, meshtastic_NodeInfoLite_fields, &item))
                    return false;
            }
        }
        if (istream && istream->bytes_left) {
            meshtastic_NodeInfoLite node = meshtastic_NodeInfoLite_init_zero;
            auto *vec = static_cast<std::vector<meshtastic_NodeInfoLite> *>(iter->pData);
            if (pb_decode(istream, meshtastic_NodeInfoLite_fields, &node)) {
                // snr_q4 = 0 is byte-identical to "field never written" but 0 dB is valid.
                // NODEINFO_BITFIELD_HAS_SNR_MASK disambiguates going forward; legacy
                // records (bit clear) treat this as unknown.
                if (nodeInfoLiteHasSnr(&node)) {
                    node.snr = node.snr_q4 / 4.0f;
                } else if (node.snr_q4) {
                    node.snr = node.snr_q4 / 4.0f;
                }
                node.snr_q4 = 0;
                vec->push_back(node);
            }
        }
        return true;
    }
    case meshtastic_NodeDatabase_positions_tag: {
        if (ostream) {
            const auto *vec = static_cast<const std::vector<meshtastic_NodePositionEntry> *>(iter->pData);
            for (auto item : *vec) {
                if (!pb_encode_tag_for_field(ostream, iter))
                    return false;
                if (!pb_encode_submessage(ostream, meshtastic_NodePositionEntry_fields, &item))
                    return false;
            }
        }
        if (istream && istream->bytes_left) {
            meshtastic_NodePositionEntry entry = meshtastic_NodePositionEntry_init_zero;
            if (pb_decode(istream, meshtastic_NodePositionEntry_fields, &entry)) {
#if !MESHTASTIC_EXCLUDE_POSITIONDB
                if (s_decodePositionsTarget) {
                    if (entry.has_position)
                        (*s_decodePositionsTarget)[entry.num] = entry.position;
                    return true;
                }
#endif
                auto *vec = static_cast<std::vector<meshtastic_NodePositionEntry> *>(iter->pData);
                vec->push_back(entry);
            }
        }
        return true;
    }
    case meshtastic_NodeDatabase_telemetry_tag: {
        if (ostream) {
            const auto *vec = static_cast<const std::vector<meshtastic_NodeTelemetryEntry> *>(iter->pData);
            for (auto item : *vec) {
                if (!pb_encode_tag_for_field(ostream, iter))
                    return false;
                if (!pb_encode_submessage(ostream, meshtastic_NodeTelemetryEntry_fields, &item))
                    return false;
            }
        }
        if (istream && istream->bytes_left) {
            meshtastic_NodeTelemetryEntry entry = meshtastic_NodeTelemetryEntry_init_zero;
            if (pb_decode(istream, meshtastic_NodeTelemetryEntry_fields, &entry)) {
#if !MESHTASTIC_EXCLUDE_TELEMETRYDB
                if (s_decodeTelemetryTarget) {
                    if (entry.has_device_metrics)
                        (*s_decodeTelemetryTarget)[entry.num] = entry.device_metrics;
                    return true;
                }
#endif
                auto *vec = static_cast<std::vector<meshtastic_NodeTelemetryEntry> *>(iter->pData);
                vec->push_back(entry);
            }
        }
        return true;
    }
    case meshtastic_NodeDatabase_status_tag: {
        if (ostream) {
            const auto *vec = static_cast<const std::vector<meshtastic_NodeStatusEntry> *>(iter->pData);
            for (auto item : *vec) {
                if (!pb_encode_tag_for_field(ostream, iter))
                    return false;
                if (!pb_encode_submessage(ostream, meshtastic_NodeStatusEntry_fields, &item))
                    return false;
            }
        }
        if (istream && istream->bytes_left) {
            meshtastic_NodeStatusEntry entry = meshtastic_NodeStatusEntry_init_zero;
            if (pb_decode(istream, meshtastic_NodeStatusEntry_fields, &entry)) {
#if !MESHTASTIC_EXCLUDE_STATUSDB
                if (s_decodeStatusTarget) {
                    if (entry.has_status)
                        (*s_decodeStatusTarget)[entry.num] = entry.status;
                    return true;
                }
#endif
                auto *vec = static_cast<std::vector<meshtastic_NodeStatusEntry> *>(iter->pData);
                vec->push_back(entry);
            }
        }
        return true;
    }
    case meshtastic_NodeDatabase_environment_tag: {
        if (ostream) {
            const auto *vec = static_cast<const std::vector<meshtastic_NodeEnvironmentEntry> *>(iter->pData);
            for (auto item : *vec) {
                if (!pb_encode_tag_for_field(ostream, iter))
                    return false;
                if (!pb_encode_submessage(ostream, meshtastic_NodeEnvironmentEntry_fields, &item))
                    return false;
            }
        }
        if (istream && istream->bytes_left) {
            meshtastic_NodeEnvironmentEntry entry = meshtastic_NodeEnvironmentEntry_init_zero;
            if (pb_decode(istream, meshtastic_NodeEnvironmentEntry_fields, &entry)) {
#if !MESHTASTIC_EXCLUDE_ENVIRONMENTDB
                if (s_decodeEnvironmentTarget) {
                    if (entry.has_environment_metrics)
                        (*s_decodeEnvironmentTarget)[entry.num] = entry.environment_metrics;
                    return true;
                }
#endif
                auto *vec = static_cast<std::vector<meshtastic_NodeEnvironmentEntry> *>(iter->pData);
                vec->push_back(entry);
            }
        }
        return true;
    }
    default:
        return true;
    }
}

void NodeDB::armNodeDatabaseDecodeTargets()
{
#if !MESHTASTIC_EXCLUDE_POSITIONDB
    nodePositions.clear();
    s_decodePositionsTarget = &nodePositions;
#endif
#if !MESHTASTIC_EXCLUDE_TELEMETRYDB
    nodeTelemetry.clear();
    s_decodeTelemetryTarget = &nodeTelemetry;
#endif
#if !MESHTASTIC_EXCLUDE_ENVIRONMENTDB
    nodeEnvironment.clear();
    s_decodeEnvironmentTarget = &nodeEnvironment;
#endif
#if !MESHTASTIC_EXCLUDE_STATUSDB
    nodeStatus.clear();
    s_decodeStatusTarget = &nodeStatus;
#endif
}

void NodeDB::disarmNodeDatabaseDecodeTargets()
{
#if !MESHTASTIC_EXCLUDE_POSITIONDB
    s_decodePositionsTarget = nullptr;
#endif
#if !MESHTASTIC_EXCLUDE_TELEMETRYDB
    s_decodeTelemetryTarget = nullptr;
#endif
#if !MESHTASTIC_EXCLUDE_ENVIRONMENTDB
    s_decodeEnvironmentTarget = nullptr;
#endif
#if !MESHTASTIC_EXCLUDE_STATUSDB
    s_decodeStatusTarget = nullptr;
#endif
}

/** The current change # for radio settings.  Starts at 0 on boot and any time the radio settings
 * might have changed is incremented.  Allows others to detect they might now be on a new channel.
 */
uint32_t radioGeneration;

// getMacAddr() and getDeviceId() are the per-architecture hooks declared in target_specific.h.

/**
 *
 * Normally userids are unique and start with +country code to look like Signal phone numbers.
 * But there are some special ids used when we haven't yet been configured by a user.  In that case
 * we use !macaddr (no colons).
 */
meshtastic_User &owner = devicestate.owner;

// The slim NodeInfoLite header defines the local long_name cap; the wire-facing
// meshtastic_User stays wider so names from senders built against the older
// 39-byte limit still decode (nanopb halts on string overflow).
static_assert(MAX_LONG_NAME_BYTES + 1 == sizeof(meshtastic_NodeInfoLite::long_name),
              "MAX_LONG_NAME_BYTES must match the NodeInfoLite storage width");
static_assert(sizeof(meshtastic_User::long_name) > MAX_LONG_NAME_BYTES,
              "wire User.long_name must be wider than the local cap so clampLongName stays in bounds");

meshtastic_Position localPosition = meshtastic_Position_init_default;
meshtastic_CriticalErrorCode error_code =
    meshtastic_CriticalErrorCode_NONE; // For the error code, only show values from this boot (discard value from flash)
uint32_t error_address = 0;

static uint8_t ourMacAddr[6];

NodeDB::NodeDB()
{
    LOG_INFO("Init NodeDB");
    loadFromDisk();
    cleanupMeshDB();

    uint32_t devicestateCRC = crc32Buffer(&devicestate, sizeof(devicestate));
    uint32_t nodeDatabaseCRC = crc32Buffer(&nodeDatabase, sizeof(nodeDatabase));
    uint32_t configCRC = crc32Buffer(&config, sizeof(config));
    uint32_t channelFileCRC = crc32Buffer(&channelFile, sizeof(channelFile));

    int saveWhat = 0;
    // Re-read the device id from silicon each boot via the per-arch getDeviceId(); clear the
    // disk-loaded value first so a failed/empty derivation leaves it unset rather than stale.
    myNodeInfo.device_id.size = 0;
    memset(myNodeInfo.device_id.bytes, 0, sizeof(myNodeInfo.device_id.bytes));
    if (getDeviceId(myNodeInfo.device_id.bytes)) {
        myNodeInfo.device_id.size = sizeof(myNodeInfo.device_id.bytes);
    }

    // likewise - we always want the app requirements to come from the running appload
    myNodeInfo.min_app_version = 30200; // format is Mmmss (where M is 1+the numeric major number. i.e. 30200 means 2.2.00

    // likewise the edition: it lives in persisted devicestate, so a vanilla install must
    // overwrite the previous event build's value. Before the CRC compare, so the change persists.
#ifdef USERPREFS_FIRMWARE_EDITION
    myNodeInfo.firmware_edition = USERPREFS_FIRMWARE_EDITION;
#else
    myNodeInfo.firmware_edition = meshtastic_FirmwareEdition_VANILLA;
#endif
    pickNewNodeNum();

    // Set our board type so we can share it with others
    owner.hw_model = HW_VENDOR;
    // Ensure user (nodeinfo) role is set to whatever we're configured to
    owner.role = config.device.role;
    // Ensure macaddr is set to our macaddr as it will be copied in our info below
    memcpy(owner.macaddr, ourMacAddr, sizeof(owner.macaddr));
    // Ensure owner.id is always derived from the node number
    snprintf(owner.id, sizeof(owner.id), "!%08x", getNodeNum());

    if (!config.has_security) {
        config.has_security = true;
        config.security = meshtastic_Config_SecurityConfig_init_default;
        config.security.serial_enabled = config.device.serial_enabled;
        config.security.is_managed = config.device.is_managed;
    }

#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)
    // Generate crypto keys if needed using consolidated function
    // Set my node num uint32 value to bytes from the public key (if we have one)
    // Generate identity and crypto keys if needed; this will create a new identity if one does not exist.
    // Skip on a degraded boot: the keypair isn't in RAM, so minting one would change our NodeNum.
    if (!configDecodeFailed
#ifdef MESHTASTIC_ENCRYPTED_STORAGE
        && !encryptedStorageLockedPlaceholder
#endif
    ) {
#if defined(HELTEC_V4_OLED)
        const size_t privateKeySize = config.security.private_key.size;
        const size_t configPublicKeySize = config.security.public_key.size;
        const size_t ownerPublicKeySize = owner.public_key.size;
        bool persistedIdentityInvalid = (privateKeySize != 0 && privateKeySize != 32) ||
                                        (configPublicKeySize != 0 && configPublicKeySize != 32) ||
                                        (ownerPublicKeySize != 0 && ownerPublicKeySize != 32) ||
                                        (privateKeySize == 0 && (configPublicKeySize != 0 || ownerPublicKeySize != 0));
        if (!persistedIdentityInvalid && privateKeySize == 32) {
            uint8_t derivedPublicKey[32];
            persistedIdentityInvalid =
                !derivePublicKeyWithoutInstalling(config.security.private_key.bytes, derivedPublicKey) ||
                (configPublicKeySize == 32 &&
                 memcmp(config.security.public_key.bytes, derivedPublicKey, sizeof(derivedPublicKey)) != 0) ||
                (ownerPublicKeySize == 32 && memcmp(owner.public_key.bytes, derivedPublicKey, sizeof(derivedPublicKey)) != 0);
        }
        if (persistedIdentityInvalid) {
            configDecodeFailed = true;
            unreadablePreferenceSegments |= SEGMENT_CONFIG;
            forceHeltecLocalRecoveryConfiguration();
            LOG_ERROR("Persisted PKI identity is invalid; preserving config for local recovery");
        }
        const bool identityReady = !persistedIdentityInvalid && generateCryptoKeyPair(nullptr);
        if (!persistedIdentityInvalid && privateKeySize == 32 && !identityReady) {
            unreadablePreferenceSegments |= SEGMENT_CONFIG;
            configDecodeFailed = true;
            forceHeltecLocalRecoveryConfiguration();
            LOG_ERROR("Persisted PKI identity could not be restored; preserving config for local recovery");
        }
#else
        const bool restoringLegacyIdentity = legacyPreferencesPendingCleanup && config.security.private_key.size == 32;
        const bool identityReady = generateCryptoKeyPair(nullptr);
        (void)restoringLegacyIdentity;
        (void)identityReady;
#endif
    }
#elif !(MESHTASTIC_EXCLUDE_PKI)
    // Calculate Curve25519 public and private keys
    if (config.security.private_key.size == 32 && config.security.public_key.size == 32) {
        owner.public_key.size = config.security.public_key.size;
        memcpy(owner.public_key.bytes, config.security.public_key.bytes, config.security.public_key.size);
        crypto->setDHPrivateKey(config.security.private_key.bytes);
        // Set my node num uint32 value to bytes from the new public key
        myNodeInfo.my_node_num = crc32Buffer(config.security.public_key.bytes, config.security.public_key.size);
    }
#endif
    // Identity is now established, so run the self-care pass on the store
    // loadFromDisk() deliberately left untrimmed: confirm self, trim/demote only
    // non-self overflow, pin self to index 0, rewrite once if healed.
    nodeDBSelfCare();

    // If we migrated from legacy during loadFromDisk(), persist the migrated DB
    // only after identity and self-care are established.
    if (migrationSavePending) {
        migrationSavePending = !saveNodeDatabaseToDisk();
    }

    // If node database has not been saved for the first time, save it now
#ifdef FSCom
    if (shouldUseFilesystemPersistence(fsIsMounted()) && !FSCom.exists(nodeDatabaseFileName)) {
        saveNodeDatabaseToDisk();
    }
#endif

#ifdef ARCH_ESP32
    Preferences preferences;
    preferences.begin("meshtastic", false);
    myNodeInfo.reboot_count = preferences.getUInt("rebootCounter", 0);
    preferences.end();
    LOG_DEBUG("Device reboots: %d", myNodeInfo.reboot_count);
#endif

    // UA_868 is obsolete; migrate to EU_868 before resetRadioConfig() below validates the region.
    if (config.lora.region == meshtastic_Config_LoRaConfig_RegionCode_UA_868) {
        LOG_INFO("UA_868 obsolete, migrating config to EU_868");
        config.lora.region = meshtastic_Config_LoRaConfig_RegionCode_EU_868;
    }

    resetRadioConfig(); // If bogus settings got saved, then fix them
    // nodeDB->LOG_DEBUG("region=%d, NODENUM=0x%x, dbsize=%d", config.lora.region, myNodeInfo.my_node_num, numMeshNodes);

    // Uncomment below to always enable UDP broadcasts
    // config.network.enabled_protocols = meshtastic_Config_NetworkConfig_ProtocolFlags_UDP_BROADCAST;

    // If we are setup to broadcast on any default channel slot (with default frequency slot semantics),
    // ensure that the telemetry intervals are coerced to the role-aware minimum value.
    if (channels.hasDefaultChannel()) {
        LOG_DEBUG("Coerce telemetry to role-aware min on defaults");
        moduleConfig.telemetry.device_update_interval = Default::getConfiguredOrMinimumValue(
            moduleConfig.telemetry.device_update_interval, min_default_telemetry_interval_secs);
        moduleConfig.telemetry.environment_update_interval = Default::getConfiguredOrMinimumValue(
            moduleConfig.telemetry.environment_update_interval, min_default_telemetry_interval_secs);
        moduleConfig.telemetry.air_quality_interval = Default::getConfiguredOrMinimumValue(
            moduleConfig.telemetry.air_quality_interval, min_default_telemetry_interval_secs);
        moduleConfig.telemetry.power_update_interval = Default::getConfiguredOrMinimumValue(
            moduleConfig.telemetry.power_update_interval, min_default_telemetry_interval_secs);
        moduleConfig.telemetry.health_update_interval = Default::getConfiguredOrMinimumValue(
            moduleConfig.telemetry.health_update_interval, min_default_telemetry_interval_secs);
    }
    // Enforce position broadcast minimums if we would send positions over a default channel
    // Check channels the same way PositionModule::sendOurPosition() does - first channel with position_precision set
    bool positionUsesDefaultChannel = false;
    for (uint8_t i = 0; i < channels.getNumChannels(); i++) {
        if (channels.getByIndex(i).settings.has_module_settings &&
            channels.getByIndex(i).settings.module_settings.position_precision != 0) {
            positionUsesDefaultChannel = channels.isDefaultChannel(i);
            break;
        }
    }
    if (positionUsesDefaultChannel) {
        LOG_DEBUG("Coerce position broadcasts to role-aware min and smart broadcast min of 5 min on defaults");
        config.position.position_broadcast_secs =
            Default::getConfiguredOrMinimumValue(config.position.position_broadcast_secs, min_default_broadcast_interval_secs);
        config.position.broadcast_smart_minimum_interval_secs = Default::getConfiguredOrMinimumValue(
            config.position.broadcast_smart_minimum_interval_secs, min_default_broadcast_smart_minimum_interval_secs);
    }
    // FIXME: UINT32_MAX intervals overflows Apple clients until they are fully patched
    if (config.device.node_info_broadcast_secs > MAX_INTERVAL)
        config.device.node_info_broadcast_secs = MAX_INTERVAL;
    if (config.position.position_broadcast_secs > MAX_INTERVAL)
        config.position.position_broadcast_secs = MAX_INTERVAL;
    if (config.position.gps_update_interval > MAX_INTERVAL)
        config.position.gps_update_interval = MAX_INTERVAL;
    if (config.position.gps_attempt_time > MAX_INTERVAL)
        config.position.gps_attempt_time = MAX_INTERVAL;
    if (config.position.position_flags > MAX_INTERVAL)
        config.position.position_flags = MAX_INTERVAL;
    if (config.position.rx_gpio > MAX_INTERVAL)
        config.position.rx_gpio = MAX_INTERVAL;
    if (config.position.tx_gpio > MAX_INTERVAL)
        config.position.tx_gpio = MAX_INTERVAL;
    if (config.position.broadcast_smart_minimum_distance > MAX_INTERVAL)
        config.position.broadcast_smart_minimum_distance = MAX_INTERVAL;
    if (config.position.broadcast_smart_minimum_interval_secs > MAX_INTERVAL)
        config.position.broadcast_smart_minimum_interval_secs = MAX_INTERVAL;
    if (config.position.gps_en_gpio > MAX_INTERVAL)
        config.position.gps_en_gpio = MAX_INTERVAL;
    if (moduleConfig.neighbor_info.update_interval > MAX_INTERVAL)
        moduleConfig.neighbor_info.update_interval = MAX_INTERVAL;
    if (moduleConfig.telemetry.device_update_interval > MAX_INTERVAL)
        moduleConfig.telemetry.device_update_interval = MAX_INTERVAL;
    if (moduleConfig.telemetry.environment_update_interval > MAX_INTERVAL)
        moduleConfig.telemetry.environment_update_interval = MAX_INTERVAL;
    if (moduleConfig.telemetry.air_quality_interval > MAX_INTERVAL)
        moduleConfig.telemetry.air_quality_interval = MAX_INTERVAL;
    if (moduleConfig.telemetry.health_update_interval > MAX_INTERVAL)
        moduleConfig.telemetry.health_update_interval = MAX_INTERVAL;

    if (moduleConfig.mqtt.has_map_report_settings &&
        moduleConfig.mqtt.map_report_settings.publish_interval_secs < default_map_publish_interval_secs) {
        moduleConfig.mqtt.map_report_settings.publish_interval_secs = default_map_publish_interval_secs;
    }

    // If a fixed position is configured, restore the persisted position into localPosition at boot.
    // This keeps position broadcasts / MQTT map reports working after reboot on GPS-less nodes.
    if (config.position.fixed_position) {
        meshtastic_PositionLite fixedPos;
        if (copyNodePosition(getNodeNum(), fixedPos) && (fixedPos.latitude_i != 0 || fixedPos.longitude_i != 0)) {
            setLocalPosition(TypeConversions::ConvertToPosition(fixedPos));
            LOG_INFO("Restored fixed position to localPosition: lat=%d lon=%d", fixedPos.latitude_i, fixedPos.longitude_i);
        }
    }

    // Ensure that the neighbor info update interval is coerced to the minimum
    moduleConfig.neighbor_info.update_interval =
        Default::getConfiguredOrMinimumValue(moduleConfig.neighbor_info.update_interval, min_neighbor_info_broadcast_secs);

    // Don't let licensed users to rebroadcast encrypted packets
    if (owner.is_licensed) {
        config.device.rebroadcast_mode = meshtastic_Config_DeviceConfig_RebroadcastMode_LOCAL_ONLY;
    }

#if !HAS_TFT
    if (config.display.displaymode == meshtastic_Config_DisplayConfig_DisplayMode_COLOR) {
        // On a device without MUI, this display mode makes no sense, and will break logic.
        config.display.displaymode = meshtastic_Config_DisplayConfig_DisplayMode_DEFAULT;
        config.bluetooth.enabled = true;
    }
#endif

    if (devicestateCRC != crc32Buffer(&devicestate, sizeof(devicestate)))
        saveWhat |= SEGMENT_DEVICESTATE;
    if (nodeDatabaseCRC != crc32Buffer(&nodeDatabase, sizeof(nodeDatabase)))
        saveWhat |= SEGMENT_NODEDATABASE;
    // Don't persist on a degraded boot: it would overwrite an unreadable-but-maybe-transient radio profile
    // with no-key/default values. Ordinary runtime mutation remains blocked until explicit recovery.
    if (!configDecodeFailed && configCRC != crc32Buffer(&config, sizeof(config)))
        saveWhat |= SEGMENT_CONFIG;
    if (channelFileCRC != crc32Buffer(&channelFile, sizeof(channelFile)))
        saveWhat |= SEGMENT_CHANNELS;

    // loadFromDisk() may discover and normalize migrations before it has
    // inspected a later core file. Those writes are accumulated, never
    // executed during the scan, and are eligible only after the complete
    // generation proved healthy.
    if (!requiresConfigRecovery())
        saveWhat |= bootDeferredPreferenceSegments;
    bootDeferredPreferenceSegments = 0;

    if (config.position.gps_enabled) {
        config.position.gps_mode = meshtastic_Config_PositionConfig_GpsMode_ENABLED;
        config.position.gps_enabled = 0;
    }
#ifdef USERPREFS_FIXED_GPS
    if (myNodeInfo.reboot_count == 1) { // Check if First boot ever or after Factory Reset.
        meshtastic_Position fixedGPS = meshtastic_Position_init_default;
#ifdef USERPREFS_FIXED_GPS_LAT
        fixedGPS.latitude_i = (int32_t)(USERPREFS_FIXED_GPS_LAT * 1e7);
        fixedGPS.has_latitude_i = true;
#endif
#ifdef USERPREFS_FIXED_GPS_LON
        fixedGPS.longitude_i = (int32_t)(USERPREFS_FIXED_GPS_LON * 1e7);
        fixedGPS.has_longitude_i = true;
#endif
#ifdef USERPREFS_FIXED_GPS_ALT
        fixedGPS.altitude = USERPREFS_FIXED_GPS_ALT;
        fixedGPS.has_altitude = true;
#endif
#if defined(USERPREFS_FIXED_GPS_LAT) && defined(USERPREFS_FIXED_GPS_LON)
        fixedGPS.location_source = meshtastic_Position_LocSource_LOC_MANUAL;
        config.has_position = true;
#if !MESHTASTIC_EXCLUDE_POSITIONDB
        {
            concurrency::LockGuard guard(&satelliteMutex);
            nodePositions[info->num] = TypeConversions::ConvertToPositionLite(fixedGPS);
        }
#endif
        nodeDB->setLocalPosition(fixedGPS);
        config.position.fixed_position = true;
#endif
    }
#endif
    sortMeshDB();

    const bool legacyMigrationWasPending = legacyPreferencesPendingCleanup;
    const bool legacyXModemExclusive = !legacyMigrationWasPending || xModem.beginExclusiveStorageMutation();
    bool canCommitLegacyMigration = legacyPreferencesPendingCleanup && !configDecodeFailed && legacyXModemExclusive;
#ifdef MESHTASTIC_ENCRYPTED_STORAGE
    canCommitLegacyMigration &= !(EncryptedStorage::isLockdownActive() && !EncryptedStorage::isUnlocked());
#endif
    const bool legacyNodeDatabaseRequired = canCommitLegacyMigration && (owner.public_key.size == 32 || owner.is_licensed);
    const bool abortLegacyMigration = legacyPreferencesPendingCleanup && configDecodeFailed;
    const int finalSaveWhat = abortLegacyMigration ? 0
                              : canCommitLegacyMigration
                                  ? SEGMENT_CONFIG | SEGMENT_MODULECONFIG | SEGMENT_DEVICESTATE | SEGMENT_CHANNELS |
                                        (legacyNodeDatabaseRequired ? SEGMENT_NODEDATABASE : 0)
                                  : saveWhat;
    const bool finalSaveSucceeded = finalSaveWhat == 0 || saveToDisk(finalSaveWhat);

#if defined(HELTEC_V4_OLED) && defined(FSCom)
    if (canCommitLegacyMigration && finalSaveSucceeded) {
        bool requiredFilesPresent = false;
        {
            concurrency::LockGuard guard(spiLock);
            requiredFilesPresent = FSCom.exists(configFileName) && FSCom.exists(moduleConfigFileName) &&
                                   FSCom.exists(deviceStateFileName) && FSCom.exists(channelFileName) &&
                                   (!legacyNodeDatabaseRequired || FSCom.exists(nodeDatabaseFileName));
        }

        // Historical legacy migration cleared every auxiliary preference, not
        // just the node cache. Remove and verify all of them only after the
        // identity/config-bearing replacements exist, retaining db.proto as
        // the final commit marker so a power loss always retries safely.
        const bool residualsRemoved =
            requiredFilesPresent && removeHeltecPreferenceResidualsChecked(legacyNodeDatabaseRequired, true);
        bool markerRemoved = false;
        if (residualsRemoved) {
            concurrency::LockGuard guard(spiLock);
            if (FSCom.exists(legacyPrefFileName))
                FSCom.remove(legacyPrefFileName);
            markerRemoved = !FSCom.exists(legacyPrefFileName);
        }
        if (markerRemoved) {
            legacyPreferencesPendingCleanup = false;
            incompleteLegacyMigrationDetected = false;
            LOG_INFO("Legacy preferences migrated without erasing /prefs");
        } else {
            LOG_ERROR("Legacy preference migration incomplete; preserving marker for retry");
        }
    }
#else
    (void)finalSaveSucceeded;
#endif
#if defined(HELTEC_V4_OLED) && defined(FSCom)
    if (legacyPreferencesPendingCleanup) {
        incompleteLegacyMigrationDetected = true;
        forceHeltecLocalRecoveryConfiguration();
        LOG_ERROR("Legacy preference migration did not commit - local recovery only");
    }
#endif
#ifdef FSCom
    if (legacyMigrationWasPending && legacyXModemExclusive)
        xModem.endExclusiveStorageMutation();
#endif
    bootInitializationInProgress = false;
}

/**
 * Most (but not always) of the time we want to treat packets 'from' the local phone (where from == 0), as if they originated on
 * the local node. If from is zero this function returns our node number instead
 */
NodeNum getFrom(const meshtastic_MeshPacket *p)
{
    return (p->from == 0) ? nodeDB->getNodeNum() : p->from;
}

// Returns true if the packet originated from the local node
bool isFromUs(const meshtastic_MeshPacket *p)
{
    return p->from == 0 || p->from == nodeDB->getNodeNum();
}

// Returns true if the packet is destined to us
bool isToUs(const meshtastic_MeshPacket *p)
{
    return p->to == nodeDB->getNodeNum();
}

bool isBroadcast(uint32_t dest)
{
    return dest == NODENUM_BROADCAST || dest == NODENUM_BROADCAST_NO_LORA;
}

namespace
{
template <typename Map, typename Value> bool copySatelliteEntry(const Map &map, NodeNum n, Value &out)
{
    auto it = map.find(n);
    if (it == map.end())
        return false;
    out = it->second;
    return true;
}

template <typename Map> std::vector<NodeNum> snapshotSatelliteNodeNums(const Map &map, NodeNum exclude)
{
    std::vector<NodeNum> result;
    result.reserve(map.size());
    for (const auto &kv : map) {
        if (kv.first != exclude)
            result.push_back(kv.first);
    }
    return result;
}

// Drop the stalest entry of `map` (staleness proxied via the owner's
// last_heard; 0 = owner evicted, i.e. an orphan - first out). Never evicts our
// own node's entry. Caller holds satelliteMutex. Returns false if nothing
// could be evicted.
template <typename Map> bool evictStalestSatellite(NodeDB &db, Map &map)
{
    auto victim = map.end();
    uint32_t victimTs = UINT32_MAX;
    for (auto it = map.begin(); it != map.end(); ++it) {
        if (it->first == db.getNodeNum())
            continue;
        uint32_t ts = db.hotNodeLastHeard(it->first);
        if (ts < victimTs) {
            victimTs = ts;
            victim = it;
        }
    }
    if (victim == map.end())
        return false;
    map.erase(victim);
    return true;
}

// Keep `map` within MAX_SATELLITE_NODES ahead of inserting `incoming` (the
// tier-1/tier-2 split: only the freshest MAX_SATELLITE_NODES nodes carry
// satellite payloads). Caller holds satelliteMutex.
template <typename Map> void evictSatelliteOverCap(NodeDB &db, Map &map, NodeNum incoming)
{
    if (map.size() < MAX_SATELLITE_NODES || map.count(incoming))
        return;
    evictStalestSatellite(db, map);
}
} // namespace

void NodeDB::resetRadioConfig(bool is_fresh_install, bool activateRuntime)
{
    if (is_fresh_install) {
        radioGeneration++;
    }

    if (channelFile.channels_count != MAX_NUM_CHANNELS) {
        LOG_INFO("Set default channel and radio prefs");

        channels.initDefaults();
        // Defaults ship the public PSK, so strip it again before onConfigChanged() publishes hashes;
        // loadFromDisk's sanitation is a no-op when the channel file was absent or corrupt.
        if (owner.is_licensed)
            channels.ensureLicensedOperation();
    }

    channels.onConfigChanged(activateRuntime);

    // Update the global myRegion
    initRegion();
}

#if defined(HELTEC_V4_OLED) && defined(FSCom)
namespace
{
constexpr const char *HELTEC_RESET_PENDING_NAMESPACE = "heltec-reset";
constexpr const char *HELTEC_RESET_PENDING_KEY = "pending";
constexpr char HELTEC_EDIT_PENDING_PAYLOAD[] = "heltec-v4-settings-edit-v1\n";
constexpr char HELTEC_NODEDB_RESET_PENDING_PAYLOAD[] = "heltec-v4-nodedb-reset-v1\n";
constexpr char HELTEC_CONFIG_RESET_PENDING_PAYLOAD[] = "heltec-v4-config-reset-v1\n";
constexpr char HELTEC_RESTORE_PENDING_PAYLOAD[] = "heltec-v4-restore-v1\n";
constexpr char HELTEC_FULL_RESET_PENDING_PAYLOAD[] = "heltec-v4-full-reset-v1\n";
constexpr const char *HELTEC_NVS_REBUILD_PENDING_FILE = "/heltec-nvs-reset.pending";
concurrency::Lock heltecPreferencesTransactionLock;

void scheduleHeltecRecoveryReboot()
{
    rebootAtMsec = millis() + 1000;
}

bool parkHeltecRadioForStorageMutation()
{
    RadioInterface *const radio = router ? router->getRadioIface() : nullptr;
    if (!radio)
        return true;

    constexpr uint32_t radioQuiesceWaitMs = 5000;
    const auto waitForIdle = [&]() {
        const uint32_t started = millis();
        while (!radio->canParkForConfig()) {
            if (!Throttle::isWithinTimespanMs(started, radioQuiesceWaitMs))
                return false;
            delay(1);
        }
        return true;
    };

    // A destructive operation may discard queued work at reboot, but it must
    // never truncate an on-air TX and report that packet as successful. Let
    // the admitted hardware operation finish before cutting the rail.
    return waitForIdle() && radio->sleep() && waitForIdle();
}

class HeltecXModemStorageGuard
{
  public:
    explicit HeltecXModemStorageGuard(bool cancelActiveTransfer = false)
        : acquired(xModem.beginExclusiveStorageMutation(cancelActiveTransfer))
    {
    }
    ~HeltecXModemStorageGuard()
    {
        if (acquired)
            xModem.endExclusiveStorageMutation();
    }
    explicit operator bool() const { return acquired; }

  private:
    bool acquired;
};

bool heltecNvsRebuildPendingFileExists()
{
    concurrency::LockGuard guard(spiLock);
    return FSCom.exists(HELTEC_NVS_REBUILD_PENDING_FILE);
}

bool heltecNvsRebuildPendingFileIsValid()
{
    concurrency::LockGuard guard(spiLock);
    File verify = FSCom.open(HELTEC_NVS_REBUILD_PENDING_FILE, FILE_O_READ);
    constexpr size_t payloadSize = sizeof(HELTEC_FULL_RESET_PENDING_PAYLOAD) - 1;
    char payload[sizeof(HELTEC_FULL_RESET_PENDING_PAYLOAD)] = {};
    const size_t bytesRead = verify ? verify.readBytes(payload, payloadSize) : 0;
    const bool valid = verify && verify.size() == payloadSize && bytesRead == payloadSize &&
                       memcmp(payload, HELTEC_FULL_RESET_PENDING_PAYLOAD, payloadSize) == 0;
    if (verify)
        verify.close();
    return valid;
}

void clearHeltecNvsRebuildTemporaryFile()
{
    concurrency::LockGuard guard(spiLock);
    String temporaryPath = HELTEC_NVS_REBUILD_PENDING_FILE;
    temporaryPath += ".tmp";
    if (FSCom.exists(temporaryPath.c_str()))
        FSCom.remove(temporaryPath.c_str());
}

bool writeHeltecNvsRebuildPendingFile()
{
    SafeFile file(HELTEC_NVS_REBUILD_PENDING_FILE, true, true);
    const size_t payloadSize = sizeof(HELTEC_FULL_RESET_PENDING_PAYLOAD) - 1;
    size_t bytesWritten = 0;
    {
        concurrency::LockGuard guard(spiLock);
        bytesWritten = file.write(reinterpret_cast<const uint8_t *>(HELTEC_FULL_RESET_PENDING_PAYLOAD), payloadSize);
    }
    if (bytesWritten != payloadSize || !file.close())
        return false;

    concurrency::LockGuard guard(spiLock);
    File verify = FSCom.open(HELTEC_NVS_REBUILD_PENDING_FILE, FILE_O_READ);
    char payload[sizeof(HELTEC_FULL_RESET_PENDING_PAYLOAD)] = {};
    const size_t bytesRead = verify ? verify.readBytes(payload, payloadSize) : 0;
    const bool valid = verify && verify.size() == payloadSize && bytesRead == payloadSize &&
                       memcmp(payload, HELTEC_FULL_RESET_PENDING_PAYLOAD, payloadSize) == 0;
    if (verify)
        verify.close();
    return valid;
}

bool clearHeltecNvsRebuildPendingFile()
{
    concurrency::LockGuard guard(spiLock);
    String temporaryPath = HELTEC_NVS_REBUILD_PENDING_FILE;
    temporaryPath += ".tmp";
    const bool fileRemoved = !FSCom.exists(HELTEC_NVS_REBUILD_PENDING_FILE) || FSCom.remove(HELTEC_NVS_REBUILD_PENDING_FILE);
    const bool temporaryRemoved = !FSCom.exists(temporaryPath.c_str()) || FSCom.remove(temporaryPath.c_str());
    return fileRemoved && temporaryRemoved && !FSCom.exists(HELTEC_NVS_REBUILD_PENDING_FILE) &&
           !FSCom.exists(temporaryPath.c_str());
}

const char *heltecResetPendingPayload(HeltecResetPendingKind kind)
{
    switch (kind) {
    case HeltecResetPendingKind::EDIT:
        return HELTEC_EDIT_PENDING_PAYLOAD;
    case HeltecResetPendingKind::NODEDB_RESET:
        return HELTEC_NODEDB_RESET_PENDING_PAYLOAD;
    case HeltecResetPendingKind::CONFIG_ONLY:
        return HELTEC_CONFIG_RESET_PENDING_PAYLOAD;
    case HeltecResetPendingKind::RESTORE:
        return HELTEC_RESTORE_PENDING_PAYLOAD;
    case HeltecResetPendingKind::FULL:
        return HELTEC_FULL_RESET_PENDING_PAYLOAD;
    default:
        return nullptr;
    }
}

bool writeHeltecResetPendingMarker(HeltecResetPendingKind kind, bool requireDestructivePower)
{
    const auto markerPowerIsSafe = [requireDestructivePower]() {
        return requireDestructivePower ? heltecDestructiveStoragePowerIsSafe() : heltecPreferenceStoragePowerIsSafe();
    };
    if (!markerPowerIsSafe()) {
        LOG_ERROR("Refusing Heltec recovery marker write while fresh power is unsafe");
        return false;
    }
    const char *payload = heltecResetPendingPayload(kind);
    if (!payload) {
        LOG_ERROR("Invalid Heltec recovery marker kind");
        return false;
    }
    const HeltecResetPendingKind currentKind = readHeltecResetPendingMarker();
    const bool transitionAllowed =
        currentKind == HeltecResetPendingKind::NONE || currentKind == kind || kind == HeltecResetPendingKind::FULL ||
        (currentKind == HeltecResetPendingKind::EDIT &&
         (kind == HeltecResetPendingKind::CONFIG_ONLY || kind == HeltecResetPendingKind::RESTORE)) ||
        (currentKind == HeltecResetPendingKind::CONFIG_ONLY && kind == HeltecResetPendingKind::RESTORE);
    const bool fullResetRecoveryActive = currentKind == HeltecResetPendingKind::FULL ||
                                         currentKind == HeltecResetPendingKind::FULL_OR_UNKNOWN ||
                                         heltecNvsRebuildPendingFileExists();
    if (!transitionAllowed || (kind != HeltecResetPendingKind::FULL && fullResetRecoveryActive)) {
        LOG_ERROR("Refusing unsafe Heltec recovery transaction transition");
        return false;
    }
    if (currentKind == kind)
        return true;
    const size_t payloadSize = strlen(payload);

    // This transaction intent deliberately lives in NVS, not LittleFS. A full
    // reset can explicitly format LittleFS, and a power cut between that format
    // and a filesystem marker rewrite would otherwise boot without knowing a
    // destructive operation was interrupted.
    nvs_handle_t handle;
    esp_err_t result = nvs_open(HELTEC_RESET_PENDING_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        LOG_ERROR("Heltec recovery marker could not open NVS: %d", result);
        return false;
    }
    // Replacing this one key preserves the old committed marker until the new
    // value commits; clearing the namespace first creates a power-loss gap.
    result = nvs_set_blob(handle, HELTEC_RESET_PENDING_KEY, payload, payloadSize);
    if (result == ESP_OK) {
        if (markerPowerIsSafe())
            result = nvs_commit(handle);
        else
            result = ESP_ERR_INVALID_STATE;
    }
    nvs_close(handle);
    if (result != ESP_OK) {
        LOG_ERROR("Heltec recovery marker could not persist to NVS: %d", result);
        return false;
    }

    const bool valid = readHeltecResetPendingMarker() == kind;
    if (!valid)
        LOG_ERROR("Heltec recovery marker failed NVS readback");
    return valid;
}

bool prepareHeltecNvsRebuildIntent(HeltecResetPendingKind initialKind)
{
    const bool hasStrongFullResetIntent =
        initialKind == HeltecResetPendingKind::FULL || initialKind == HeltecResetPendingKind::FULL_OR_UNKNOWN;
    if (!fsIsMounted()) {
        if (!hasStrongFullResetIntent || !heltecDestructiveStoragePowerIsSafe() || !fsFormat())
            return false;
    }
    // A prior interrupted rebuild may have left this as the only durable FULL
    // intent. Accept it without attempting a replace that could consume the
    // final free block or weaken the marker on failure.
    if (heltecNvsRebuildPendingFileIsValid() || writeHeltecNvsRebuildPendingFile())
        return true;

    // SafeFile may leave only a staging file after a failed refresh. Never
    // delete the committed target here: even malformed content is interpreted
    // fail-closed at boot and may be the sole surviving reset intent.
    clearHeltecNvsRebuildTemporaryFile();
    if (hasStrongFullResetIntent) {
        return heltecDestructiveStoragePowerIsSafe() && fsFormat() && writeHeltecNvsRebuildPendingFile();
    }

    // Without an existing FULL NVS intent, deleting user data or formatting
    // merely to make room for the secondary marker would itself be an
    // insufficiently guarded destructive transaction. Fail closed; a clean USB
    // install remains the recovery path for the rare combination of full
    // LittleFS and unusable NVS.
    return false;
}

bool rebuildHeltecNvsForFullReset(HeltecResetPendingKind initialKind, bool &rebootRequired)
{
    // Quiesce first: SafeFile and the guarded fsFormat fallback must not race
    // network/XMODEM/radio tasks that may still hold filesystem or NVS state.
#if HAS_WIFI
    deinitWifi();
#endif
    disableBluetooth();
    forceHeltecLocalRecoveryConfiguration();
#if !MESHTASTIC_EXCLUDE_GPS
    if (gps)
        gps->disable();
#endif
    if (!parkHeltecRadioForStorageMutation()) {
        LOG_ERROR("NVS rebuild could not safely drain/park LoRa");
        return false;
    }
    rebootRequired = true;
    if (!prepareHeltecNvsRebuildIntent(initialKind)) {
        LOG_ERROR("Could not persist secondary full-reset intent before rebuilding NVS");
        return false;
    }
    const esp_err_t deinitResult = nvs_flash_deinit();
    if (deinitResult != ESP_OK && deinitResult != ESP_ERR_NVS_NOT_INITIALIZED) {
        LOG_ERROR("Could not deinitialize corrupt NVS: %d", deinitResult);
        return false;
    }
    esp_err_t result = nvs_flash_erase();
    if (result == ESP_OK)
        result = nvs_flash_init();
    if (result != ESP_OK) {
        LOG_ERROR("Could not rebuild NVS for explicit full reset: %d", result);
        return false;
    }
    return writeHeltecResetPendingMarker(HeltecResetPendingKind::FULL, true);
}

bool clearHeltecResetPendingMarker(HeltecResetPendingKind expectedKind, bool requireDestructivePower)
{
    const auto markerPowerIsSafe = [requireDestructivePower]() {
        return requireDestructivePower ? heltecDestructiveStoragePowerIsSafe() : heltecPreferenceStoragePowerIsSafe();
    };
    if (!markerPowerIsSafe()) {
        LOG_ERROR("Refusing Heltec recovery marker clear while fresh power is unsafe");
        return false;
    }
    if (readHeltecResetPendingMarker() != expectedKind) {
        LOG_ERROR("Refusing to clear a different Heltec recovery transaction");
        return false;
    }
    nvs_handle_t handle;
    esp_err_t result = nvs_open(HELTEC_RESET_PENDING_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK)
        return false;
    result = nvs_erase_key(handle, HELTEC_RESET_PENDING_KEY);
    if (result == ESP_ERR_NVS_NOT_FOUND)
        result = ESP_OK;
    if (result == ESP_OK) {
        if (markerPowerIsSafe())
            result = nvs_commit(handle);
        else
            result = ESP_ERR_INVALID_STATE;
    }
    nvs_close(handle);
    return result == ESP_OK && readHeltecResetPendingMarker() == HeltecResetPendingKind::NONE;
}

HeltecResetPendingKind readHeltecResetPendingMarker()
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(HELTEC_RESET_PENDING_NAMESPACE, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND)
        return HeltecResetPendingKind::NONE;
    if (result != ESP_OK) {
        LOG_ERROR("Heltec recovery marker NVS is unreadable: %d", result);
        return HeltecResetPendingKind::FULL_OR_UNKNOWN;
    }
    char payload[64] = {};
    size_t size = 0;
    result = nvs_get_blob(handle, HELTEC_RESET_PENDING_KEY, nullptr, &size);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return HeltecResetPendingKind::NONE;
    }
    if (result != ESP_OK || size > sizeof(payload)) {
        nvs_close(handle);
        return HeltecResetPendingKind::FULL_OR_UNKNOWN;
    }
    size_t bytesRead = size;
    result = nvs_get_blob(handle, HELTEC_RESET_PENDING_KEY, payload, &bytesRead);
    nvs_close(handle);
    if (result != ESP_OK)
        return HeltecResetPendingKind::FULL_OR_UNKNOWN;

    constexpr size_t editPayloadSize = sizeof(HELTEC_EDIT_PENDING_PAYLOAD) - 1;
    if (size == editPayloadSize && bytesRead == size && memcmp(payload, HELTEC_EDIT_PENDING_PAYLOAD, editPayloadSize) == 0) {
        return HeltecResetPendingKind::EDIT;
    }
    constexpr size_t nodeDbResetPayloadSize = sizeof(HELTEC_NODEDB_RESET_PENDING_PAYLOAD) - 1;
    if (size == nodeDbResetPayloadSize && bytesRead == size &&
        memcmp(payload, HELTEC_NODEDB_RESET_PENDING_PAYLOAD, nodeDbResetPayloadSize) == 0) {
        return HeltecResetPendingKind::NODEDB_RESET;
    }
    constexpr size_t configPayloadSize = sizeof(HELTEC_CONFIG_RESET_PENDING_PAYLOAD) - 1;
    if (size == configPayloadSize && bytesRead == size &&
        memcmp(payload, HELTEC_CONFIG_RESET_PENDING_PAYLOAD, configPayloadSize) == 0) {
        return HeltecResetPendingKind::CONFIG_ONLY;
    }
    constexpr size_t restorePayloadSize = sizeof(HELTEC_RESTORE_PENDING_PAYLOAD) - 1;
    if (size == restorePayloadSize && bytesRead == size &&
        memcmp(payload, HELTEC_RESTORE_PENDING_PAYLOAD, restorePayloadSize) == 0) {
        return HeltecResetPendingKind::RESTORE;
    }
    constexpr size_t fullPayloadSize = sizeof(HELTEC_FULL_RESET_PENDING_PAYLOAD) - 1;
    if (size == fullPayloadSize && bytesRead == size &&
        memcmp(payload, HELTEC_FULL_RESET_PENDING_PAYLOAD, fullPayloadSize) == 0) {
        return HeltecResetPendingKind::FULL;
    }
    // A corrupt or unrecognized marker is treated as the more conservative
    // full-reset state; only another explicit full reset may clear it.
    return HeltecResetPendingKind::FULL_OR_UNKNOWN;
}

bool eraseHeltecNvsExceptRecoveryMarker()
{
    if (readHeltecResetPendingMarker() != HeltecResetPendingKind::FULL) {
        LOG_ERROR("Refusing NVS cleanup without a verified full-reset marker");
        return false;
    }

    std::vector<std::string> namespaces;
    std::vector<std::string> recoveryCompanionKeys;
    nvs_iterator_t iterator = nullptr;
    esp_err_t result = nvs_entry_find("nvs", nullptr, NVS_TYPE_ANY, &iterator);
    while (result == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(iterator, &info);
        if (strcmp(info.namespace_name, HELTEC_RESET_PENDING_NAMESPACE) == 0) {
            if (strcmp(info.key, HELTEC_RESET_PENDING_KEY) != 0)
                recoveryCompanionKeys.emplace_back(info.key);
        } else if (std::find(namespaces.begin(), namespaces.end(), info.namespace_name) == namespaces.end()) {
            namespaces.emplace_back(info.namespace_name);
        }
        result = nvs_entry_next(&iterator);
    }
    if (iterator)
        nvs_release_iterator(iterator);
    if (result != ESP_ERR_NVS_NOT_FOUND) {
        LOG_ERROR("Could not enumerate NVS namespaces: %d", result);
        return false;
    }

    for (const auto &namespaceName : namespaces) {
        nvs_handle_t handle;
        result = nvs_open(namespaceName.c_str(), NVS_READWRITE, &handle);
        if (result != ESP_OK) {
            LOG_ERROR("Could not open NVS namespace %s: %d", namespaceName.c_str(), result);
            return false;
        }
        result = nvs_erase_all(handle);
        if (result == ESP_OK)
            result = nvs_commit(handle);
        nvs_close(handle);
        if (result != ESP_OK) {
            LOG_ERROR("Could not erase NVS namespace %s: %d", namespaceName.c_str(), result);
            return false;
        }
    }

    if (!recoveryCompanionKeys.empty()) {
        nvs_handle_t handle;
        result = nvs_open(HELTEC_RESET_PENDING_NAMESPACE, NVS_READWRITE, &handle);
        if (result != ESP_OK)
            return false;
        for (const auto &key : recoveryCompanionKeys) {
            result = nvs_erase_key(handle, key.c_str());
            if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND)
                break;
        }
        if (result == ESP_OK || result == ESP_ERR_NVS_NOT_FOUND)
            result = nvs_commit(handle);
        nvs_close(handle);
        if (result != ESP_OK)
            return false;
    }

    bool onlyRecoveryMarkerRemains = true;
    iterator = nullptr;
    result = nvs_entry_find("nvs", nullptr, NVS_TYPE_ANY, &iterator);
    while (result == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(iterator, &info);
        if (strcmp(info.namespace_name, HELTEC_RESET_PENDING_NAMESPACE) != 0 || strcmp(info.key, HELTEC_RESET_PENDING_KEY) != 0) {
            LOG_ERROR("Unexpected NVS entry remained after full reset: %s/%s", info.namespace_name, info.key);
            onlyRecoveryMarkerRemains = false;
        }
        result = nvs_entry_next(&iterator);
    }
    if (iterator)
        nvs_release_iterator(iterator);
    if (result != ESP_ERR_NVS_NOT_FOUND)
        onlyRecoveryMarkerRemains = false;

    return onlyRecoveryMarkerRemains && readHeltecResetPendingMarker() == HeltecResetPendingKind::FULL;
}

bool isHeltecRetainedPreferenceFile(const char *path, bool retainNodeDatabase, bool retainLegacyMarker)
{
    if (!path) {
        return false;
    }
    const bool activeFile = strcmp(path, configFileName) == 0 || strcmp(path, moduleConfigFileName) == 0 ||
                            strcmp(path, channelFileName) == 0 || strcmp(path, deviceStateFileName) == 0 ||
                            (retainNodeDatabase && strcmp(path, nodeDatabaseFileName) == 0) ||
                            (retainLegacyMarker && strcmp(path, legacyPrefFileName) == 0);
#if USERPREFS_EVENT_MODE
    // An event build writes its active profile under separate names. Legacy
    // migration must not erase the inactive standard profile or its backup.
    return activeFile || strcmp(path, STANDARD_CONFIG_FILE_NAME) == 0 || strcmp(path, STANDARD_CHANNEL_FILE_NAME) == 0 ||
           strcmp(path, STANDARD_BACKUP_FILE_NAME) == 0;
#else
    return activeFile;
#endif
}

bool removeHeltecPreferenceTreeChecked(const char *directory)
{
    File root = FSCom.open(directory, FILE_O_READ);
    if (!root || !root.isDirectory()) {
        LOG_ERROR("Config reset cleanup could not open %s", directory);
        return false;
    }

    bool success = true;
    File file = root.openNextFile();
    while (file && file.name()[0]) {
        const char *sourcePath = file.path();
        char path[255];
        const bool pathValid = sourcePath && strlcpy(path, sourcePath, sizeof(path)) < sizeof(path);
        const bool isDirectory = file.isDirectory();
        file.close();

        if (!pathValid) {
            LOG_ERROR("Config reset cleanup skipped an invalid preference path");
            success = false;
        } else if (isDirectory) {
            success &= removeHeltecPreferenceTreeChecked(path);
        } else if (!FSCom.remove(path) && FSCom.exists(path)) {
            LOG_ERROR("Config reset cleanup could not remove %s", path);
            success = false;
        }

        file = root.openNextFile();
    }
    root.close();

    if (!FSCom.rmdir(directory) && FSCom.exists(directory)) {
        LOG_ERROR("Config reset cleanup could not remove directory %s", directory);
        success = false;
    }
    return success && !FSCom.exists(directory);
}

bool removeHeltecFileChecked(const char *path)
{
    if (FSCom.exists(path) && !FSCom.remove(path)) {
        LOG_ERROR("Factory reset could not remove %s", path);
        return false;
    }
    return !FSCom.exists(path);
}

bool removeHeltecPersistedUserContentChecked()
{
    constexpr const char *paths[] = {"/Messages_default.msgs", "/Messages_default.msgs.tmp", "/Waypoints_default.wpts",
                                     "/Waypoints_default.wpts.tmp"};
    bool success = true;
    for (const char *path : paths)
        success &= removeHeltecFileChecked(path);
    return success;
}

// Called only after every required default file has been committed
// successfully. This restores the traditional reset cleanup without deleting
// the previous identity/config generation before its atomic replacement is
// known good.
bool removeHeltecPreferenceResidualsChecked(bool retainNodeDatabase, bool retainLegacyMarker)
{
    concurrency::LockGuard guard(spiLock);
    File root = FSCom.open("/prefs", FILE_O_READ);
    if (!root || !root.isDirectory()) {
        LOG_ERROR("Config reset cleanup could not open /prefs");
        return false;
    }

    bool success = true;
    File file = root.openNextFile();
    while (file && file.name()[0]) {
        const char *sourcePath = file.path();
        char path[255];
        const bool pathValid = sourcePath && strlcpy(path, sourcePath, sizeof(path)) < sizeof(path);
        const bool isDirectory = file.isDirectory();
        file.close();

        if (!pathValid) {
            LOG_ERROR("Config reset cleanup skipped an invalid preference path");
            success = false;
        } else if (isDirectory) {
            success &= removeHeltecPreferenceTreeChecked(path);
        } else if (!isHeltecRetainedPreferenceFile(path, retainNodeDatabase, retainLegacyMarker) && !FSCom.remove(path) &&
                   FSCom.exists(path)) {
            LOG_ERROR("Config reset cleanup could not remove %s", path);
            success = false;
        }

        file = root.openNextFile();
    }
    root.close();

    // Re-open and verify: deleting entries while iterating can expose
    // backend-specific cursor behavior, so success requires an explicit final
    // inventory containing only the files explicitly retained by this reset or
    // migration transaction.
    root = FSCom.open("/prefs", FILE_O_READ);
    if (!root || !root.isDirectory()) {
        LOG_ERROR("Config reset cleanup could not verify /prefs");
        return false;
    }
    file = root.openNextFile();
    while (file && file.name()[0]) {
        const char *path = file.path();
        if (file.isDirectory() || !isHeltecRetainedPreferenceFile(path, retainNodeDatabase, retainLegacyMarker)) {
            LOG_ERROR("Config reset cleanup left residual %s", path ? path : "<invalid>");
            success = false;
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();
    return success;
}
} // namespace
#endif

bool NodeDB::factoryReset(bool eraseBleBonds)
{
    LOG_INFO("Factory reset");
#if defined(HELTEC_V4_OLED)
    if (rebootAtMsec != 0 || shutdownAtMsec != 0) {
        LOG_ERROR("Factory reset refused while reboot/shutdown is pending");
        return false;
    }
    bool expectedInactive = false;
    if (!destructiveStorageMutationActive.compare_exchange_strong(expectedInactive, true, std::memory_order_acq_rel)) {
        LOG_ERROR("Factory reset refused while another destructive storage operation is active");
        return false;
    }
    destructiveStorageOwnerTask.store(reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle()), std::memory_order_release);
    bool keepDestructiveFenceUntilReboot = false;
    struct ActivityReset {
        std::atomic<bool> &flag;
        std::atomic<uintptr_t> &owner;
        bool &keepUntilReboot;
        ~ActivityReset()
        {
            owner.store(0, std::memory_order_release);
            if (!keepUntilReboot)
                flag.store(false, std::memory_order_release);
        }
    } activityReset{destructiveStorageMutationActive, destructiveStorageOwnerTask, keepDestructiveFenceUntilReboot};
    // A store autosave can pass its first gate immediately before the CAS
    // above. Drain all persistence locks before any format/remove operation;
    // the raised destructive fence prevents a new save from passing its
    // second gate. Keep one fixed order for the two independent stores.
    if (transmitHistory)
        transmitHistory->drainPersistenceWrites();
#if HAS_SCREEN
    messageStore.drainPersistenceWrites();
#endif
#if HAS_SCREEN && !MESHTASTIC_EXCLUDE_WAYPOINT
    waypointStore.drainPersistenceWrites();
#endif
    if (!waitForExternalStateReaders()) {
        LOG_ERROR("Factory reset refused while a PhoneAPI/HTTP state reader is active");
        return false;
    }
    concurrency::LockGuard transactionGuard(&heltecPreferencesTransactionLock);
    if (!eraseBleBonds && isPreferenceEditTransactionActive()) {
        LOG_ERROR("Config reset refused while a settings edit is open");
        return false;
    }
    // Keep Screen::saveDisplayDisabled() and any in-flight PRG event away
    // from Preferences while FULL may deinit/erase/reinitialize NVS.
    concurrency::LockGuard nvsMutationGuard(&heltecV4NvsMutationLock);
    HeltecXModemStorageGuard xmodemGuard(eraseBleBonds);
    // saveNodeDatabaseToDisk() deliberately skips writes while XMODEM owns the
    // filesystem. A reset in that window could otherwise report success while
    // retaining the previous nodes.proto. Reject before mutating RAM instead.
    if (!xmodemGuard) {
        LOG_ERROR("Factory reset refused while XMODEM transfer is active");
        return false;
    }

    if (!eraseBleBonds && incompletePreferenceRestoreDetected) {
        LOG_ERROR("Config reset refused: interrupted backup restore must be retried or fully reset");
        return false;
    }

    const HeltecResetPendingKind initialMarkerKind = readHeltecResetPendingMarker();
    const bool recoveringInterruptedEdit = initialMarkerKind == HeltecResetPendingKind::EDIT;

    if (!eraseBleBonds && (unreadablePreferenceSegments & SEGMENT_CONFIG) != 0) {
        // A config-only reset promises to retain the keypair. When the mounted
        // file exists but could not be decoded, the in-memory defaults contain
        // no authoritative key to preserve. Leave the file untouched for a
        // clean reboot/recovery or require an explicitly destructive full reset.
        LOG_ERROR("Config reset refused: persisted identity is unavailable");
        return false;
    }

    const bool hasValidPrivateKey = config.has_security && config.security.private_key.size == 32;
    const bool hasPriorPublicIdentity =
        owner.public_key.size != 0 || (config.has_security && config.security.public_key.size != 0);
    if (!eraseBleBonds &&
        ((config.has_security && config.security.private_key.size != 0 && config.security.private_key.size != 32) ||
         (config.has_security && config.security.public_key.size != 0 && config.security.public_key.size != 32) ||
         (owner.public_key.size != 0 && owner.public_key.size != 32) || (!hasValidPrivateKey && hasPriorPublicIdentity))) {
        LOG_ERROR("Config reset refused: persisted identity length is invalid");
        return false;
    }

    uint32_t preservedNodeNum = myNodeInfo.my_node_num;
    meshtastic_User_public_key_t preservedPublicKey = owner.public_key;
#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)
    if (!eraseBleBonds && hasValidPrivateKey) {
        uint8_t derivedPublicKey[32];
        meshtastic_Config_SecurityConfig_public_key_t derivedKey = {};
        derivedKey.size = sizeof(derivedPublicKey);
        if (!derivePublicKeyWithoutInstalling(config.security.private_key.bytes, derivedPublicKey)) {
            LOG_ERROR("Config reset refused: private key cannot produce a valid identity");
            return false;
        }
        memcpy(derivedKey.bytes, derivedPublicKey, sizeof(derivedPublicKey));
        if (checkLowEntropyPublicKey(derivedKey) ||
            (config.security.public_key.size == 32 &&
             memcmp(config.security.public_key.bytes, derivedPublicKey, sizeof(derivedPublicKey)) != 0) ||
            (!recoveringInterruptedEdit && owner.public_key.size == 32 &&
             memcmp(owner.public_key.bytes, derivedPublicKey, sizeof(derivedPublicKey)) != 0)) {
            LOG_ERROR("Config reset refused: persisted identity is inconsistent");
            return false;
        }
        const uint32_t derivedNodeNum = crc32Buffer(derivedPublicKey, sizeof(derivedPublicKey));
        if (preservedNodeNum != derivedNodeNum) {
            if (!incompleteConfigResetDetected && (unreadablePreferenceSegments & SEGMENT_DEVICESTATE) == 0) {
                LOG_ERROR("Config reset refused: persisted NodeNum does not match its key");
                return false;
            }
            LOG_WARN("Recovering config-reset NodeNum from its preserved private key");
            preservedNodeNum = derivedNodeNum;
        }
        preservedPublicKey.size = sizeof(derivedPublicKey);
        memcpy(preservedPublicKey.bytes, derivedPublicKey, sizeof(derivedPublicKey));
    }
#endif

    // Never remove the last persistent generation unless flash writes are
    // currently safe. Refresh the physical battery sample at each destructive
    // boundary instead of relying on a stale periodic status value.
    if (!heltecDestructiveStoragePowerIsSafe()) {
        LOG_ERROR("Factory reset refused: connect stable power or charge battery "
                  "to at least %umV",
                  HELTEC_V4_DESTRUCTIVE_STORAGE_MIN_MILLIVOLTS);
        return false;
    }

    // Mount failures never auto-format on this board. A factory reset is the
    // explicit destructive recovery path. Only the full device reset may erase
    // the identity; the config-only reset promises to preserve it and therefore
    // must fail while the persisted key is unavailable.
    if (!fsIsMounted()) {
        if (!eraseBleBonds) {
            LOG_ERROR("Config reset refused: filesystem unavailable and identity cannot be preserved");
            return false;
        }
    }

    const HeltecResetPendingKind resetKind = eraseBleBonds ? HeltecResetPendingKind::FULL : HeltecResetPendingKind::CONFIG_ONLY;
    bool markerReady = writeHeltecResetPendingMarker(resetKind, true);
    bool nvsRebuildRequiresReboot = false;
    if (!markerReady && eraseBleBonds && heltecDestructiveStoragePowerIsSafe()) {
        LOG_WARN("Rebuilding unreadable NVS for explicit full reset");
        markerReady = rebuildHeltecNvsForFullReset(initialMarkerKind, nvsRebuildRequiresReboot);
    }
    if (!markerReady) {
        LOG_ERROR("Factory reset refused: pending marker could not be verified");
        if (nvsRebuildRequiresReboot) {
            // Rebuild preparation already quiesced transports and may have
            // changed NVS or installed the secondary FULL intent. Do not
            // reopen PhoneAPI/input against that transitional state.
            keepDestructiveFenceUntilReboot = true;
            scheduleHeltecRecoveryReboot();
        }
        return false;
    }
    // A verified reset marker supersedes any in-process settings edit. This is
    // needed for the physical factory-reset recovery path, which deliberately
    // remains available even if a client abandoned a bulk import.
    preferenceEditOwnerTask.store(0, std::memory_order_release);
    preferenceEditOwnerClient.store(0, std::memory_order_release);
    preferenceEditRequiresDestructivePower.store(false, std::memory_order_release);
    preferenceEditRadioParked.store(false, std::memory_order_release);
    preferenceEditState.store(PreferenceEditState::NONE, std::memory_order_release);
    keepDestructiveFenceUntilReboot = true;
    if (nvsRebuildRequiresReboot) {
        // nvs_flash_deinit()/erase()/init() invalidates handles owned by other
        // components. Reboot with both recovery markers committed instead of
        // continuing a multi-store reset in the same process.
        LOG_WARN("NVS rebuilt; rebooting into guarded factory-reset recovery");
        scheduleHeltecRecoveryReboot();
        return false;
    }
    {
        // Quiesce every transport and radio reader while the original config is
        // still intact, before defaults can make availability predicates return
        // false or replace NodeDB/config storage observed by another task.
#if HAS_WIFI
        deinitWifi();
#endif
        disableBluetooth();
        config.lora.tx_enabled = false;
#if !MESHTASTIC_EXCLUDE_GPS
        if (gps)
            gps->disable();
#endif
        if (!parkHeltecRadioForStorageMutation()) {
            LOG_ERROR("Factory reset could not safely drain/park LoRa");
            configDecodeFailed = true;
            incompleteConfigResetDetected = !eraseBleBonds;
            unreadablePreferenceSegments =
                SEGMENT_CONFIG | SEGMENT_MODULECONFIG | SEGMENT_DEVICESTATE | SEGMENT_CHANNELS | SEGMENT_NODEDATABASE;
            forceHeltecLocalRecoveryConfiguration();
            scheduleHeltecRecoveryReboot();
            return false;
        }
    }
    const auto abortHeltecReset = [&]() {
        configDecodeFailed = true;
        incompleteConfigResetDetected = !eraseBleBonds;
        incompletePreferenceRestoreDetected = false;
        incompleteNodeDatabaseResetDetected = false;
        unreadablePreferenceSegments =
            SEGMENT_CONFIG | SEGMENT_MODULECONFIG | SEGMENT_DEVICESTATE | SEGMENT_CHANNELS | SEGMENT_NODEDATABASE;
        forceHeltecLocalRecoveryConfiguration();
#if !MESHTASTIC_EXCLUDE_GPS
        if (gps)
            gps->disable();
#endif
        if (router && router->getRadioIface())
            router->getRadioIface()->sleep();
        // A typed reset marker has already committed and transports are
        // quiesced. Schedule a reboot independently of the caller while still
        // reporting failure so it cannot acknowledge an incomplete reset.
        scheduleHeltecRecoveryReboot();
        return false;
    };
    if (eraseBleBonds || !fsIsMounted()) {
        // A full factory reset is also a privacy boundary: XMODEM and modules
        // can create data outside /prefs, so enumerating only today's known
        // paths could leave identity exports or PSKs for the next owner. The
        // durable NVS FULL marker survives this LittleFS format.
        LOG_WARN(eraseBleBonds ? "Full factory reset formatting filesystem"
                               : "Factory reset explicitly formatting unavailable filesystem");
        if (!heltecDestructiveStoragePowerIsSafe() || !fsFormat()) {
            LOG_ERROR("Factory reset aborted: filesystem format/remount failed");
            return abortHeltecReset();
        }
    }
#endif
    // A full device reset remains explicitly destructive. On Heltec, a
    // config-only reset leaves the old core files in place until SafeFile has
    // atomically replaced every one, preserving identity if a write fails. Other
    // targets retain their established reset sequence.
    bool resetCleanupSucceeded = true;
#if defined(HELTEC_V4_OLED)
    if (!heltecDestructiveStoragePowerIsSafe()) {
        LOG_ERROR("Factory reset refused: power changed before preference cleanup");
        return abortHeltecReset();
    }
#endif
    spiLock->lock();
#ifdef FSCom
    // Clear unrelated files before touching the sole preference generation.
    // A failure here can still abort with identity/config fully intact.
    if (FSCom.exists("/static/rangetest.csv") && !FSCom.remove("/static/rangetest.csv")) {
        LOG_ERROR("Can't remove rangetest.csv");
#if defined(HELTEC_V4_OLED)
        resetCleanupSucceeded = false;
#endif
    }
#if defined(HELTEC_V4_OLED)
    // Full reset removes every recoverable copy before deleting the active
    // preference generation.
    if (resetCleanupSucceeded && eraseBleBonds && FSCom.exists("/backups")) {
        resetCleanupSucceeded = removeHeltecPreferenceTreeChecked("/backups") && !FSCom.exists("/backups");
        if (!resetCleanupSucceeded)
            LOG_ERROR("Factory reset could not remove and verify preference backups");
    }
    if (resetCleanupSucceeded && eraseBleBonds)
        resetCleanupSucceeded = removeHeltecPersistedUserContentChecked();
#endif
#endif

    if (resetCleanupSucceeded && shouldRemovePreferencesBeforeFactoryDefaults(
#if defined(HELTEC_V4_OLED)
                                     true,
#else
                                     false,
#endif
                                     eraseBleBonds)) {
        rmDir("/prefs"); // caller holds spiLock; rmDir/listDir deliberately do not
                         // lock internally
#if defined(HELTEC_V4_OLED) && defined(FSCom)
        resetCleanupSucceeded = !FSCom.exists("/prefs");
#endif
    }
    spiLock->unlock();
#if defined(HELTEC_V4_OLED)
    if (!resetCleanupSucceeded && eraseBleBonds) {
        // The typed FULL marker is already durable, so a malformed managed
        // path (for example /backups created as a file) must not make recovery
        // permanently unrepeatable. Finish the explicitly destructive reset
        // with a verified format whether cleanup failed before or during the
        // /prefs removal.
        LOG_WARN("Factory reset preference cleanup incomplete; formatting filesystem");
        resetCleanupSucceeded = heltecDestructiveStoragePowerIsSafe() && fsFormat();
        if (resetCleanupSucceeded) {
            // The NVS marker deliberately survives a LittleFS format.
            resetCleanupSucceeded = readHeltecResetPendingMarker() == HeltecResetPendingKind::FULL;
        }
    }
    if (!resetCleanupSucceeded) {
        LOG_ERROR("Factory reset aborted: preference cleanup incomplete");
        return abortHeltecReset();
    }
#endif

    // rmDir above nuked the .dat file, but TransmitHistory's in-memory
    // cache auto-flushes every 5 min and would resurrect it.
    const bool transmitHistoryCleared = !transmitHistory || transmitHistory->clear(true);
#if !defined(HELTEC_V4_OLED)
    (void)transmitHistoryCleared;
#endif
#if HAS_SCREEN
    const bool messagesCleared = messageStore.clearAllMessages(true);
#else
    const bool messagesCleared = true;
#endif
#if HAS_SCREEN && !MESHTASTIC_EXCLUDE_WAYPOINT
    const bool waypointsCleared = waypointStore.clearAllWaypoints(true);
#else
    const bool waypointsCleared = true;
#endif
#if defined(HELTEC_V4_OLED)
    if (!transmitHistoryCleared || !messagesCleared || !waypointsCleared) {
        LOG_ERROR("Factory reset failed to verify empty runtime stores");
        return abortHeltecReset();
    }
#endif

#if WARM_NODE_COUNT > 0
    // On nRF52840 the warm tier lives in raw flash outside /prefs, so rmDir
    // didn't touch it; clear it and persist the empty store.
    warmStore.clear();
    warmStore.saveIfDirty(true);
#endif
#if HAS_TRAFFIC_MANAGEMENT
    // Factory reset forgets everything; TMM's RAM caches must not survive to resurrect
    // identities (the device usually reboots after this, but don't rely on it).
    if (trafficManagementModule)
        trafficManagementModule->purgeAll();
#endif

    // The old preference tree has now been removed or is about to be replaced
    // atomically. Full reset may replace an unreadable identity; config-only
    // reached here only with a valid identity and can repair channels/modules.
    configDecodeFailed = false;

    unreadablePreferenceSegments = 0; // this explicit reset is the recovery authorization
    incompleteConfigResetDetected = false;
    incompletePreferenceRestoreDetected = false;
    incompleteNodeDatabaseResetDetected = false;
    incompleteLegacyMigrationDetected = false;
    localPosition = meshtastic_Position_init_default;
    localPositionUpdatedSinceBoot = false;
#if defined(HELTEC_V4_OLED)
    // Reset every non-identity DeviceState field. Full reset starts with no
    // identity at all; config-only restores only the NodeNum/public key pair it
    // explicitly promises to retain, not old messages, remote pins or flags.
    devicestate = meshtastic_DeviceState_init_default;
    myNodeInfo.device_id.size = 0;
    memset(myNodeInfo.device_id.bytes, 0, sizeof(myNodeInfo.device_id.bytes));
    if (getDeviceId(myNodeInfo.device_id.bytes))
        myNodeInfo.device_id.size = sizeof(myNodeInfo.device_id.bytes);
    if (!eraseBleBonds) {
        myNodeInfo.my_node_num = preservedNodeNum;
        owner.public_key = preservedPublicKey;
    }
#endif

    // second, install default state (this will deal with the duplicate mac address issue)
    installDefaultNodeDatabase();
    installDefaultDeviceState();
#if defined(HELTEC_V4_OLED)
    // installDefaultDeviceState() normally retains a nonzero NodeNum, but make
    // the config-reset identity invariant explicit at the final pre-save
    // boundary so later default-state changes cannot silently undo it.
    if (!eraseBleBonds) {
        myNodeInfo.my_node_num = preservedNodeNum;
        owner.public_key = preservedPublicKey;
        snprintf(owner.id, sizeof(owner.id), "!%08x", preservedNodeNum);
    }
#endif
    // A reset requested in the running application must not re-import network
    // credentials merely because this boot followed an OTA update.
    installDefaultConfig(!eraseBleBonds, false); // Also preserve the private key if we're not erasing BLE bonds
    installDefaultModuleConfig();
    installDefaultChannels();
    // third, write everything to disk
#if defined(HELTEC_V4_OLED)
    if (!saveToDisk()) {
        LOG_ERROR("Factory reset failed to persist defaults");
        return abortHeltecReset();
    }
    if (!eraseBleBonds) {
        const bool nodeDatabaseRequired = owner.public_key.size == 32 || owner.is_licensed;
        bool requiredFilesPresent = false;
        {
            concurrency::LockGuard guard(spiLock);
            requiredFilesPresent = FSCom.exists(configFileName) && FSCom.exists(moduleConfigFileName) &&
                                   FSCom.exists(deviceStateFileName) && FSCom.exists(channelFileName) &&
                                   (!nodeDatabaseRequired || FSCom.exists(nodeDatabaseFileName));
        }
        if (!requiredFilesPresent || !removeHeltecPreferenceResidualsChecked(nodeDatabaseRequired, false)) {
            LOG_ERROR("Config reset incomplete: auxiliary preferences remain");
            return abortHeltecReset();
        }
    }
#else
    saveToDisk();
#endif
    if (eraseBleBonds) {
#if defined(HELTEC_V4_OLED)
        if (!heltecDestructiveStoragePowerIsSafe()) {
            LOG_ERROR("Factory reset stopped: power changed before NVS erase");
            return abortHeltecReset();
        }
#endif
        LOG_INFO("Erase BLE bonds");
#ifdef ARCH_ESP32
#if defined(HELTEC_V4_OLED)
        // Keep the committed full-reset marker intact while every other NVS
        // namespace (BLE bonds, credentials and persistent variables) is
        // erased and verified. A brownout therefore cannot erase the intent
        // before the rest of the transaction has completed.
#if HAS_WIFI
        deinitWifi();
#endif
        disableBluetooth();
        if (!eraseHeltecNvsExceptRecoveryMarker()) {
            LOG_ERROR("Factory reset failed to erase and verify NVS");
            return abortHeltecReset();
        }
#else
        const esp_err_t nvsEraseResult = nvs_flash_erase();
        if (nvsEraseResult != ESP_OK) {
            LOG_ERROR("Factory reset failed to erase NVS: %d", nvsEraseResult);
            return false;
        }
#endif
#endif

#ifdef ARCH_NRF52
        LOG_INFO("Clear bluetooth bonds");
        bond_print_list(BLE_GAP_ROLE_PERIPH);
        bond_print_list(BLE_GAP_ROLE_CENTRAL);
        Bluefruit.Periph.clearBonds();
        Bluefruit.Central.clearBonds();
#endif
    }
#if defined(HELTEC_V4_OLED)
    if (!heltecDestructiveStoragePowerIsSafe()) {
        LOG_ERROR("Factory reset stopped: power changed before final commit");
        return abortHeltecReset();
    }
    if (eraseBleBonds && !clearHeltecNvsRebuildPendingFile()) {
        LOG_ERROR("Factory reset completed but secondary pending marker could not be cleared");
        return abortHeltecReset();
    }
    if (!clearHeltecResetPendingMarker(resetKind, true)) {
        LOG_ERROR("Factory reset completed but pending marker could not be cleared");
        // The replacement preferences and (for a full reset) NVS cleanup are
        // already committed. Reboot into marker-controlled recovery instead
        // of continuing to run with a half-finalized transaction.
        return abortHeltecReset();
    }
#endif
    return true;
}

void NodeDB::installDefaultNodeDatabase()
{
    LOG_DEBUG("Install default NodeDatabase");
    nodeDatabase.version = DEVICESTATE_CUR_VER;
#if defined(HELTEC_V4_OLED)
    if (nodeDatabase.nodes.size() == MAX_NUM_NODES)
        std::fill(nodeDatabase.nodes.begin(), nodeDatabase.nodes.end(), meshtastic_NodeInfoLite());
    else
        nodeDatabase.nodes.assign(MAX_NUM_NODES, meshtastic_NodeInfoLite());
#else
    nodeDatabase.nodes = std::vector<meshtastic_NodeInfoLite>(MAX_NUM_NODES);
#endif
    numMeshNodes = 0;
    meshNodes = &nodeDatabase.nodes;
    concurrency::LockGuard satelliteGuard(&satelliteMutex);
#if !MESHTASTIC_EXCLUDE_POSITIONDB
    nodePositions.clear();
#endif

#if !MESHTASTIC_EXCLUDE_TELEMETRYDB
    nodeTelemetry.clear();
#endif

#if !MESHTASTIC_EXCLUDE_ENVIRONMENTDB
    nodeEnvironment.clear();
#endif

#if !MESHTASTIC_EXCLUDE_STATUSDB
    nodeStatus.clear();
#endif
}

void NodeDB::installDefaultConfig(bool preserveKey, bool recoverOtaNetwork)
{
    uint8_t private_key_temp[32];
    bool shouldPreserveKey = preserveKey && config.has_security && config.security.private_key.size == 32;
    if (shouldPreserveKey) {
        memcpy(private_key_temp, config.security.private_key.bytes, config.security.private_key.size);
    }
    LOG_INFO("Install default LocalConfig");
    memset(&config, 0, sizeof(meshtastic_LocalConfig));
    config.version = DEVICESTATE_CUR_VER;
    config.has_device = true;
    config.has_display = true;
    config.has_lora = true;
    config.has_position = true;
    config.has_power = true;
    config.has_network = true;
    config.has_bluetooth = (HAS_BLUETOOTH ? true : false);
    config.has_security = true;
    config.device.rebroadcast_mode = meshtastic_Config_DeviceConfig_RebroadcastMode_ALL;

    config.lora.sx126x_rx_boosted_gain = true;
    config.lora.tx_enabled =
        true; // FIXME: maybe false in the future, and setting region to enable it. (unset region forces it off)
    config.lora.override_duty_cycle = false;
    config.lora.config_ok_to_mqtt = false;
#if HAS_LORA_FEM
    config.lora.fem_lna_mode = meshtastic_Config_LoRaConfig_FEM_LNA_Mode_ENABLED;
#else
    config.lora.fem_lna_mode = meshtastic_Config_LoRaConfig_FEM_LNA_Mode_NOT_PRESENT;
#endif

#if HAS_TFT // For the devices that support MUI, default to that
    config.display.displaymode = meshtastic_Config_DisplayConfig_DisplayMode_COLOR;
#endif

#if defined(TFT_WIDTH) && defined(TFT_HEIGHT) && (TFT_WIDTH >= 200 || TFT_HEIGHT >= 200)
    config.display.enable_message_bubbles = true;
#endif

#ifdef USERPREFS_CONFIG_DEVICE_ROLE
    // Restrict ROUTER*, LOST AND FOUND roles for security reasons
    if (IS_ONE_OF(USERPREFS_CONFIG_DEVICE_ROLE, meshtastic_Config_DeviceConfig_Role_ROUTER,
                  meshtastic_Config_DeviceConfig_Role_ROUTER_LATE, meshtastic_Config_DeviceConfig_Role_LOST_AND_FOUND)) {
        LOG_WARN("ROUTER roles restricted, fall back to CLIENT");
        config.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT;
    } else {
        config.device.role = USERPREFS_CONFIG_DEVICE_ROLE;
    }
#else
    config.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT; // Default to client.
#endif

#ifdef USERPREFS_CONFIG_LORA_REGION
    config.lora.region = USERPREFS_CONFIG_LORA_REGION;
#else
    config.lora.region = meshtastic_Config_LoRaConfig_RegionCode_UNSET;
#endif

#ifdef USERPREFS_LORACONFIG_TX_POWER
    config.lora.tx_power = USERPREFS_LORACONFIG_TX_POWER;
#endif

#ifdef USERPREFS_LORACONFIG_MODEM_PRESET
    config.lora.modem_preset = USERPREFS_LORACONFIG_MODEM_PRESET;
#else
    config.lora.modem_preset = meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;
#endif

#ifdef USERPREFS_LORACONFIG_USE_PRESET
    config.lora.use_preset = USERPREFS_LORACONFIG_USE_PRESET;
#else
    config.lora.use_preset = true;
#endif

#ifdef USERPREFS_LORACONFIG_BANDWIDTH
    config.lora.bandwidth = USERPREFS_LORACONFIG_BANDWIDTH;
#endif

#ifdef USERPREFS_LORACONFIG_SPREAD_FACTOR
    config.lora.spread_factor = USERPREFS_LORACONFIG_SPREAD_FACTOR;
#endif

#ifdef USERPREFS_LORACONFIG_CODING_RATE
    config.lora.coding_rate = USERPREFS_LORACONFIG_CODING_RATE;
#endif

#ifdef USERPREFS_LORACONFIG_OVERRIDE_FREQUENCY
    config.lora.override_frequency = USERPREFS_LORACONFIG_OVERRIDE_FREQUENCY;
#endif

#if USERPREFS_EVENT_MODE
    config.lora.hop_limit = Default::eventModeHopLimit;
#else
    config.lora.hop_limit = HOP_RELIABLE;
#endif
#ifdef USERPREFS_CONFIG_LORA_IGNORE_MQTT
    config.lora.ignore_mqtt = USERPREFS_CONFIG_LORA_IGNORE_MQTT;
#else
    config.lora.ignore_mqtt = false;
#endif
#ifdef USERPREFS_CONFIG_LORA_CONFIG_OK_TO_MQTT
    config.lora.config_ok_to_mqtt = USERPREFS_CONFIG_LORA_CONFIG_OK_TO_MQTT;
#endif

    // Initialize admin_key_count to zero
    byte numAdminKeys = 0;

#ifdef USERPREFS_USE_ADMIN_KEY_0
    // Check if USERPREFS_ADMIN_KEY_0 is non-empty
    if (sizeof(userprefs_admin_key_0) > 0) {
        memcpy(config.security.admin_key[0].bytes, userprefs_admin_key_0, 32);
        config.security.admin_key[0].size = 32;
        numAdminKeys++;
    }
#endif

#ifdef USERPREFS_USE_ADMIN_KEY_1
    // Check if USERPREFS_ADMIN_KEY_1 is non-empty
    if (sizeof(userprefs_admin_key_1) > 0) {
        memcpy(config.security.admin_key[1].bytes, userprefs_admin_key_1, 32);
        config.security.admin_key[1].size = 32;
        numAdminKeys++;
    }
#endif

#ifdef USERPREFS_USE_ADMIN_KEY_2
    // Check if USERPREFS_ADMIN_KEY_2 is non-empty
    if (sizeof(userprefs_admin_key_2) > 0) {
        memcpy(config.security.admin_key[2].bytes, userprefs_admin_key_2, 32);
        config.security.admin_key[2].size = 32;
        numAdminKeys++;
    }
#endif

    config.security.admin_key_count = numAdminKeys;

#ifdef USERPREFS_CONFIG_SECURITY_IS_MANAGED
    // is_managed is the supported way for a vendor to lock configuration, but without an admin key
    // it locks the vendor out too and only a factory reset recovers it.
    if (USERPREFS_CONFIG_SECURITY_IS_MANAGED && numAdminKeys == 0) {
        LOG_WARN("USERPREFS is_managed needs an admin key, ignored");
    } else {
        config.security.is_managed = USERPREFS_CONFIG_SECURITY_IS_MANAGED;
    }
#endif

    // Left at COMPATIBLE when signature checking is compiled out, so we never report a policy
    // nothing enforces (mirrors the set-config guard in AdminModule).
#if defined(USERPREFS_CONFIG_SECURITY_PACKET_SIGNATURE_POLICY) && !(MESHTASTIC_EXCLUDE_PKI) && !(MESHTASTIC_EXCLUDE_XEDDSA)
    config.security.packet_signature_policy = USERPREFS_CONFIG_SECURITY_PACKET_SIGNATURE_POLICY;
#endif

    if (shouldPreserveKey) {
        config.security.private_key.size = 32;
        memcpy(config.security.private_key.bytes, private_key_temp, config.security.private_key.size);
        // Never log the key bytes: debug logs get pasted into public bug reports.
        LOG_DEBUG("Restored preserved private key");
    } else {
        config.security.private_key.size = 0;
    }
    config.security.public_key.size = 0;

#ifdef PIN_GPS_EN
    config.position.gps_en_gpio = PIN_GPS_EN;
#endif

#if defined(USERPREFS_CONFIG_GPS_MODE)
    config.position.gps_mode = USERPREFS_CONFIG_GPS_MODE;
#elif !HAS_GPS || GPS_DEFAULT_NOT_PRESENT
    config.position.gps_mode = meshtastic_Config_PositionConfig_GpsMode_NOT_PRESENT;
#elif defined(GPS_DEFAULT_DISABLED)
    config.position.gps_mode = meshtastic_Config_PositionConfig_GpsMode_DISABLED;
#elif !defined(GPS_RX_PIN)
    if (config.position.rx_gpio == 0)
        config.position.gps_mode = meshtastic_Config_PositionConfig_GpsMode_NOT_PRESENT;
    else
        config.position.gps_mode = meshtastic_Config_PositionConfig_GpsMode_DISABLED;
#else
    config.position.gps_mode = meshtastic_Config_PositionConfig_GpsMode_ENABLED;
#endif

#ifdef USERPREFS_CONFIG_SMART_POSITION_ENABLED
    config.position.position_broadcast_smart_enabled = USERPREFS_CONFIG_SMART_POSITION_ENABLED;
#else
    config.position.position_broadcast_smart_enabled = true;
#endif

    config.position.broadcast_smart_minimum_distance = 100;
    config.position.broadcast_smart_minimum_interval_secs = default_broadcast_smart_minimum_interval_secs;
    if (config.device.role != meshtastic_Config_DeviceConfig_Role_ROUTER &&
        config.device.role != meshtastic_Config_DeviceConfig_Role_ROUTER_LATE)
        config.device.node_info_broadcast_secs = default_node_info_broadcast_secs;
    config.security.serial_enabled = true;
    config.security.admin_channel_enabled = false;
    resetRadioConfig(true); // This also triggers NodeInfo/Position requests since we're fresh
    strncpy(config.network.ntp_server, "meshtastic.pool.ntp.org", 32);

#if (defined(T_DECK) || defined(T_WATCH_S3) || defined(UNPHONE) || defined(PICOMPUTER_S3) || defined(SENSECAP_INDICATOR) ||      \
     defined(ELECROW_PANEL) || defined(HELTEC_V4_TFT) || defined(HELTEC_V4_R8_TFT) || defined(RAK_WISMESH_TAP_V2) ||             \
     defined(ELECROW_ThinkNode_M9) || defined(SEEED_WIO_TRACKER_L2) || defined(T_WATCH_ULTRA)) &&                                \
    HAS_TFT
    // switch BT off by default; use TFT programming mode or hotkey to enable
    config.bluetooth.enabled = false;
#else
    // default to bluetooth capability of platform as default
    config.bluetooth.enabled = true;
#endif

    config.bluetooth.fixed_pin = defaultBLEPin;

#if defined(USE_EINK) || defined(HAS_SPI_TFT) || defined(USE_SPISSD1306)
    bool hasScreen = true;
#if defined(TFT_NV3001B_DETECT)
    hasScreen = nv3001bPanelPresent(TFT_CS, TFT_SCL, TFT_SDA, TFT_RS, TFT_RST, TFT_EN, TFT_BL);
#elif defined(HELTEC_MESH_NODE_T114)
    uint32_t st7789_id = get_st7789_id(ST7789_NSS, ST7789_SCK, ST7789_SDA, ST7789_RS, ST7789_RESET);
    if (st7789_id == 0xFFFFFF) {
        hasScreen = false;
    }
#endif // TFT_NV3001B_DETECT / HELTEC_MESH_NODE_T114
#elif ARCH_PORTDUINO
    bool hasScreen = false;
    if (portduino_config.displayPanel)
        hasScreen = true;
    else
        hasScreen = screen_found.port != ScanI2C::I2CPort::NO_I2C;
#elif MESHTASTIC_INCLUDE_NICHE_GRAPHICS // See "src/graphics/niche"
    bool hasScreen = true; // Use random pin for Bluetooth pairing
#else
    bool hasScreen = screen_found.port != ScanI2C::I2CPort::NO_I2C;
#endif

#ifdef USERPREFS_FIXED_BLUETOOTH
    config.bluetooth.fixed_pin = USERPREFS_FIXED_BLUETOOTH;
    config.bluetooth.mode = meshtastic_Config_BluetoothConfig_PairingMode_FIXED_PIN;
#else
    config.bluetooth.mode = hasScreen ? meshtastic_Config_BluetoothConfig_PairingMode_RANDOM_PIN
                                      : meshtastic_Config_BluetoothConfig_PairingMode_FIXED_PIN;
#endif

    // for backward compat, default position flags are ALT+MSL
    config.position.position_flags =
        (meshtastic_Config_PositionConfig_PositionFlags_ALTITUDE | meshtastic_Config_PositionConfig_PositionFlags_ALTITUDE_MSL |
         meshtastic_Config_PositionConfig_PositionFlags_SPEED | meshtastic_Config_PositionConfig_PositionFlags_HEADING |
         meshtastic_Config_PositionConfig_PositionFlags_DOP | meshtastic_Config_PositionConfig_PositionFlags_SATINVIEW);

// Set default value for 'Mesh via UDP'
#if HAS_UDP_MULTICAST
#ifdef USERPREFS_NETWORK_ENABLED_PROTOCOLS
    config.network.enabled_protocols = USERPREFS_NETWORK_ENABLED_PROTOCOLS;
#else
    config.network.enabled_protocols = 0;
#endif // Network enabled protocols
#endif // UDP Multicast

#ifdef USERPREFS_NETWORK_WIFI_ENABLED
    config.network.wifi_enabled = USERPREFS_NETWORK_WIFI_ENABLED;
#endif

#if USE_ETHERNET_DEFAULT
    config.network.eth_enabled = true;
#endif

#ifdef USERPREFS_NETWORK_WIFI_SSID
    strncpy(config.network.wifi_ssid, USERPREFS_NETWORK_WIFI_SSID, sizeof(config.network.wifi_ssid));
#endif

#ifdef USERPREFS_NETWORK_WIFI_PSK
    strncpy(config.network.wifi_psk, USERPREFS_NETWORK_WIFI_PSK, sizeof(config.network.wifi_psk));
#endif

#if defined(USERPREFS_NETWORK_IPV6_ENABLED)
    config.network.ipv6_enabled = USERPREFS_NETWORK_IPV6_ENABLED;
#else
    config.network.ipv6_enabled = default_network_ipv6_enabled;
#endif

#ifdef DISPLAY_FLIP_SCREEN
    config.display.flip_screen = true;
#endif

#ifdef RAK4630
    config.display.wake_on_tap_or_motion = true;
#endif

#if defined(T_WATCH_S3) || defined(SENSECAP_INDICATOR) || defined(T_WATCH_ULTRA)
    config.display.screen_on_secs = 30;
    config.display.wake_on_tap_or_motion = true;
#endif

#if defined(T_ECHO_CARD)
    config.display.screen_on_secs = 60;
#endif

#ifdef COMPASS_ORIENTATION
    config.display.compass_orientation = COMPASS_ORIENTATION;
#endif

#if defined(ARCH_ESP32) && !MESHTASTIC_EXCLUDE_WIFI
    if (recoverOtaNetwork && MeshtasticOTA::isUpdated()) {
        MeshtasticOTA::recoverConfig(&config.network);
    }
#endif

#ifdef USERPREFS_CONFIG_DEVICE_ROLE
    // Apply role-specific defaults when role is set via user preferences
    // Boot-time defaults must not reinterpret an explicitly configured owner
    // flag as residue from a role transition. Passing the same role preserves
    // the persisted owner semantics; real role changes pass the old role.
    installRoleDefaults(config.device.role, config.device.role);
#endif

#ifdef USERPREFS_CONFIG_DEVICE_REBROADCAST_MODE
    config.device.rebroadcast_mode = USERPREFS_CONFIG_DEVICE_REBROADCAST_MODE;
    // Same restriction AdminModule enforces on a set-config; apply it here so a vendor build can't
    // ship a combination the device would silently refuse later.
    if (config.device.rebroadcast_mode == meshtastic_Config_DeviceConfig_RebroadcastMode_NONE &&
        IS_ONE_OF(config.device.role, meshtastic_Config_DeviceConfig_Role_ROUTER,
                  meshtastic_Config_DeviceConfig_Role_ROUTER_LATE)) {
        LOG_WARN("Rebroadcast mode can't be NONE for a router role, use ALL");
        config.device.rebroadcast_mode = meshtastic_Config_DeviceConfig_RebroadcastMode_ALL;
    }
#endif
#ifdef USERPREFS_CONFIG_DEVICE_NODE_INFO_BROADCAST_SECS
    // Clamped to the same window AdminModule enforces on a set-config
    config.device.node_info_broadcast_secs = clamp((uint32_t)USERPREFS_CONFIG_DEVICE_NODE_INFO_BROADCAST_SECS,
                                                   (uint32_t)min_node_info_broadcast_secs, (uint32_t)MAX_INTERVAL);
#endif

    initConfigIntervals();
    variantDefaultConfig();
    variantDefaultModuleConfig();
}

void NodeDB::initConfigIntervals()
{
#ifdef USERPREFS_CONFIG_GPS_UPDATE_INTERVAL
    config.position.gps_update_interval = USERPREFS_CONFIG_GPS_UPDATE_INTERVAL;
#else
    config.position.gps_update_interval = default_gps_update_interval;
#endif

#ifdef USERPREFS_CONFIG_POSITION_BROADCAST_INTERVAL
    config.position.position_broadcast_secs = USERPREFS_CONFIG_POSITION_BROADCAST_INTERVAL;
#else
    config.position.position_broadcast_secs = default_broadcast_interval_secs;
#endif

    config.power.ls_secs = default_ls_secs;
    config.power.min_wake_secs = default_min_wake_secs;
    config.power.sds_secs = default_sds_secs;
    config.power.wait_bluetooth_secs = default_wait_bluetooth_secs;

    config.display.screen_on_secs = default_screen_on_secs;

#if defined(USE_POWERSAVE)
    config.power.is_power_saving = true;
    config.display.screen_on_secs = 30;
    config.power.wait_bluetooth_secs = 30;
#endif
}

// Always-on traffic management defaults. Only booleans are written; every
// numeric field stays 0 and resolves to its default_traffic_mgmt_* macro at
// use (e.g. position dedup precision/interval), so fork-wide tuning changes
// take effect without another migration. Rate limiting and the features that
// exhaust or reshape relayed traffic (exhaust_hop_*, drop_unknown_enabled,
// nodeinfo_direct_response) stay opt-in.
static void installTrafficManagementDefaults(meshtastic_LocalModuleConfig &mc)
{
    mc.has_traffic_management = true;
    mc.traffic_management = meshtastic_ModuleConfig_TrafficManagementConfig_init_zero;
#if HAS_TRAFFIC_MANAGEMENT
    // Position dedup ships enabled at the 5-hour default window on all supported targets.
    // STM32WL is excluded at compile time (HAS_TRAFFIC_MANAGEMENT=0 in mesh-pb-constants.h).
    // Set position_min_interval_secs=0 at runtime to disable dedup.
    mc.traffic_management.position_min_interval_secs = default_traffic_mgmt_position_min_interval_secs;
#endif
}

// --- 2.8 position/telemetry opt-in migration helpers -------------------------------------------------
// Pure field mutators (no I/O), so the native test suite can exercise them directly. The version gate
// and saveToDisk live in loadFromDisk() below.

void optInDisablePositionSharing(meshtastic_ChannelFile &cf)
{
    for (pb_size_t i = 0; i < cf.channels_count; i++) {
        // Only flip PUBLIC / default-PSK channels. A channel with a real private key is a deliberate
        // trusted-group setup where the "leak location to strangers" concern doesn't apply, so its
        // configured precision (including full precision) is preserved.
        if (!channelFileUsesPublicKey(cf, (ChannelIndex)i))
            continue;
        cf.channels[i].settings.has_module_settings = true;
        cf.channels[i].settings.module_settings.position_precision = 0;
    }
}

void optInDisableTelemetryBroadcast(meshtastic_LocalModuleConfig &mc)
{
    // Every mesh-broadcast telemetry enable flag (each gates its module's sendTelemetry() to the mesh).
    mc.telemetry.device_telemetry_enabled = false;
    mc.telemetry.environment_measurement_enabled = false;
    mc.telemetry.air_quality_enabled = false;
    mc.telemetry.power_measurement_enabled = false;
    mc.telemetry.health_measurement_enabled = false;
    // Position leak via the public MQTT map. Leave map_reporting_enabled alone (anonymous presence is
    // still allowed) and strip only the location component.
    mc.mqtt.map_report_settings.should_report_location = false;
}

void NodeDB::installDefaultModuleConfig()
{
    LOG_INFO("Install default ModuleConfig");
    memset(&moduleConfig, 0, sizeof(meshtastic_LocalModuleConfig));

    moduleConfig.version = DEVICESTATE_CUR_VER;
    moduleConfig.has_mqtt = true;
    moduleConfig.has_range_test = true;
    moduleConfig.has_serial = true;
    moduleConfig.has_store_forward = true;
    moduleConfig.has_telemetry = true;
    moduleConfig.has_external_notification = true;
#if defined(LED_NOTIFICATION) || defined(PCA_LED_NOTIFICATION)
#define HAS_NOTIFICATION_LED
#endif
#if defined(PIN_BUZZER) || defined(PIN_VIBRATION) || defined(HAS_NOTIFICATION_LED) ||                                            \
    defined(NEOPIXEL_STATUS_NOTIFICATION_PIN) || defined(HAS_I2S_SPEAKER_NRF52)
    moduleConfig.external_notification.enabled = true;
#endif

#if defined(PIN_BUZZER)
    moduleConfig.external_notification.output_buzzer = PIN_BUZZER;
    moduleConfig.external_notification.use_pwm = true;
    moduleConfig.external_notification.alert_message_buzzer = true;
#elif defined(HAS_I2S_SPEAKER_NRF52)
    // No PWM piezo pin - alert playback goes through NRF52RtttlPlayer/I2S instead,
    // gated only on alert_message_buzzer + canBuzz(), not output_buzzer/use_pwm.
    moduleConfig.external_notification.alert_message_buzzer = true;
#endif

#if defined(PIN_VIBRATION)
    moduleConfig.external_notification.output_vibra = PIN_VIBRATION;
    moduleConfig.external_notification.alert_message_vibra = true;
    moduleConfig.external_notification.output_ms = 500;
#endif

#if defined(LED_NOTIFICATION)
    moduleConfig.external_notification.output = LED_NOTIFICATION;
    moduleConfig.external_notification.active = LED_STATE_ON;
    moduleConfig.external_notification.alert_message = true;
    moduleConfig.external_notification.output_ms = 1000;
#endif

#if HAS_TFT
    if (moduleConfig.external_notification.nag_timeout == default_ringtone_nag_secs)
        moduleConfig.external_notification.nag_timeout = 0;
#elif defined(PIN_VIBRATION)
    moduleConfig.external_notification.nag_timeout = 2;
#elif defined(PIN_BUZZER) || defined(LED_NOTIFICATION) || defined(NEOPIXEL_STATUS_NOTIFICATION_PIN) ||                           \
    defined(HAS_I2S_SPEAKER_NRF52)
    moduleConfig.external_notification.nag_timeout = default_ringtone_nag_secs;
#endif

#ifdef HAS_I2S
    // Don't worry about the other settings for T-Watch, we'll also use the DRV2056 behavior for notifications
    moduleConfig.external_notification.enabled = true;
    moduleConfig.external_notification.use_i2s_as_buzzer = true;
    moduleConfig.external_notification.alert_message_buzzer = true;
#endif // HAS_I2S

#ifdef NANO_G2_ULTRA
    moduleConfig.external_notification.enabled = true;
    moduleConfig.external_notification.alert_message = true;
    moduleConfig.external_notification.output_ms = 100;
    moduleConfig.external_notification.active = true;
#endif // NANO_G2_ULTRA

#ifdef ELECROW_ThinkNode_M8
    moduleConfig.canned_message.rotary1_enabled = true;
    moduleConfig.canned_message.inputbroker_pin_a = PIN_BUTTON_EC04_A;
    moduleConfig.canned_message.inputbroker_pin_b = PIN_BUTTON_EC04_B;
    moduleConfig.canned_message.inputbroker_pin_press = PIN_BUTTON_EC04;
    moduleConfig.canned_message.inputbroker_event_cw = meshtastic_ModuleConfig_CannedMessageConfig_InputEventChar_RIGHT;
    moduleConfig.canned_message.inputbroker_event_ccw = meshtastic_ModuleConfig_CannedMessageConfig_InputEventChar_LEFT;
    moduleConfig.canned_message.inputbroker_event_press = meshtastic_ModuleConfig_CannedMessageConfig_InputEventChar_SELECT;
#endif
#ifdef T_LORA_PAGER
    moduleConfig.canned_message.updown1_enabled = true;
    moduleConfig.canned_message.inputbroker_pin_a = ROTARY_A;
    moduleConfig.canned_message.inputbroker_pin_b = ROTARY_B;
    moduleConfig.canned_message.inputbroker_pin_press = ROTARY_PRESS;
    moduleConfig.canned_message.inputbroker_event_cw = meshtastic_ModuleConfig_CannedMessageConfig_InputEventChar(28);
    moduleConfig.canned_message.inputbroker_event_ccw = meshtastic_ModuleConfig_CannedMessageConfig_InputEventChar(29);
    moduleConfig.canned_message.inputbroker_event_press = meshtastic_ModuleConfig_CannedMessageConfig_InputEventChar_SELECT;
#endif

    moduleConfig.has_canned_message = true;

#if USERPREFS_MQTT_ENABLED && !MESHTASTIC_EXCLUDE_MQTT
    moduleConfig.mqtt.enabled = true;
#endif

#ifdef USERPREFS_MQTT_ADDRESS
    strncpy(moduleConfig.mqtt.address, USERPREFS_MQTT_ADDRESS, sizeof(moduleConfig.mqtt.address));
#else
    strncpy(moduleConfig.mqtt.address, default_mqtt_address, sizeof(moduleConfig.mqtt.address));
#endif

#ifdef USERPREFS_MQTT_USERNAME
    strncpy(moduleConfig.mqtt.username, USERPREFS_MQTT_USERNAME, sizeof(moduleConfig.mqtt.username));
#else
    strncpy(moduleConfig.mqtt.username, default_mqtt_username, sizeof(moduleConfig.mqtt.username));
#endif

#ifdef USERPREFS_MQTT_PASSWORD
    strncpy(moduleConfig.mqtt.password, USERPREFS_MQTT_PASSWORD, sizeof(moduleConfig.mqtt.password));
#else
    strncpy(moduleConfig.mqtt.password, default_mqtt_password, sizeof(moduleConfig.mqtt.password));
#endif

#ifdef USERPREFS_MQTT_ROOT_TOPIC
    strncpy(moduleConfig.mqtt.root, USERPREFS_MQTT_ROOT_TOPIC, sizeof(moduleConfig.mqtt.root));
#else
    strncpy(moduleConfig.mqtt.root, default_mqtt_root, sizeof(moduleConfig.mqtt.root));
#endif

#ifdef USERPREFS_MQTT_ENCRYPTION_ENABLED
    moduleConfig.mqtt.encryption_enabled = USERPREFS_MQTT_ENCRYPTION_ENABLED;
#else
    moduleConfig.mqtt.encryption_enabled = default_mqtt_encryption_enabled;
#endif

#ifdef USERPREFS_MQTT_TLS_ENABLED
    moduleConfig.mqtt.tls_enabled = USERPREFS_MQTT_TLS_ENABLED;
#else
    moduleConfig.mqtt.tls_enabled = default_mqtt_tls_enabled;
#endif

    moduleConfig.has_neighbor_info = true;
    moduleConfig.neighbor_info.enabled = false;

    installTrafficManagementDefaults(moduleConfig);

    moduleConfig.has_detection_sensor = true;
    moduleConfig.detection_sensor.enabled = false;
    moduleConfig.detection_sensor.detection_trigger_type = meshtastic_ModuleConfig_DetectionSensorConfig_TriggerType_LOGIC_HIGH;
    moduleConfig.detection_sensor.minimum_broadcast_secs = 45;

    moduleConfig.has_ambient_lighting = true;
    moduleConfig.ambient_lighting.current = 10;
    // Default to a color based on our node number
    moduleConfig.ambient_lighting.red = (myNodeInfo.my_node_num & 0xFF0000) >> 16;
    moduleConfig.ambient_lighting.green = (myNodeInfo.my_node_num & 0x00FF00) >> 8;
    moduleConfig.ambient_lighting.blue = myNodeInfo.my_node_num & 0x0000FF;

#if !MESHTASTIC_EXCLUDE_BEACON
    moduleConfig.has_mesh_beacon = true;
    // Default flags: listen on, broadcast off, legacy split on.
    moduleConfig.mesh_beacon.flags = meshtastic_ModuleConfig_MeshBeaconConfig_Flags_FLAG_LISTEN_ENABLED |
                                     meshtastic_ModuleConfig_MeshBeaconConfig_Flags_FLAG_LEGACY_SPLIT;
// Set or clear a single beacon flag bit from a USERPREFS boolean.
#define BEACON_APPLY_FLAG(enabled, flag)                                                                                         \
    do {                                                                                                                         \
        if (enabled)                                                                                                             \
            moduleConfig.mesh_beacon.flags |= (uint32_t)(flag);                                                                  \
        else                                                                                                                     \
            moduleConfig.mesh_beacon.flags &= ~(uint32_t)(flag);                                                                 \
    } while (0)
#ifdef USERPREFS_MESH_BEACON_LISTEN_ENABLED
    BEACON_APPLY_FLAG(USERPREFS_MESH_BEACON_LISTEN_ENABLED, meshtastic_ModuleConfig_MeshBeaconConfig_Flags_FLAG_LISTEN_ENABLED);
#endif
#ifdef USERPREFS_MESH_BEACON_BROADCAST_ENABLED
    BEACON_APPLY_FLAG(USERPREFS_MESH_BEACON_BROADCAST_ENABLED,
                      meshtastic_ModuleConfig_MeshBeaconConfig_Flags_FLAG_BROADCAST_ENABLED);
#endif
#ifdef USERPREFS_MESH_BEACON_MESSAGE
    strncpy(moduleConfig.mesh_beacon.broadcast_message, USERPREFS_MESH_BEACON_MESSAGE,
            sizeof(moduleConfig.mesh_beacon.broadcast_message) - 1);
    moduleConfig.mesh_beacon.broadcast_message[sizeof(moduleConfig.mesh_beacon.broadcast_message) - 1] = '\0';
#endif
#ifdef USERPREFS_MESH_BEACON_INTERVAL_SECS
    moduleConfig.mesh_beacon.broadcast_interval_secs =
        (USERPREFS_MESH_BEACON_INTERVAL_SECS != 0 &&
         USERPREFS_MESH_BEACON_INTERVAL_SECS < default_mesh_beacon_min_broadcast_interval_secs)
            ? default_mesh_beacon_min_broadcast_interval_secs
            : USERPREFS_MESH_BEACON_INTERVAL_SECS;
#endif
#ifdef USERPREFS_MESH_BEACON_OFFER_PRESET
    moduleConfig.mesh_beacon.has_broadcast_offer_preset = true;
    moduleConfig.mesh_beacon.broadcast_offer_preset = USERPREFS_MESH_BEACON_OFFER_PRESET;
#endif
#ifdef USERPREFS_MESH_BEACON_OFFER_REGION
    moduleConfig.mesh_beacon.broadcast_offer_region = USERPREFS_MESH_BEACON_OFFER_REGION;
#endif
#ifdef USERPREFS_MESH_BEACON_OFFER_CHANNEL_NAME
    moduleConfig.mesh_beacon.has_broadcast_offer_channel = true;
    strncpy(moduleConfig.mesh_beacon.broadcast_offer_channel.name, USERPREFS_MESH_BEACON_OFFER_CHANNEL_NAME,
            sizeof(moduleConfig.mesh_beacon.broadcast_offer_channel.name) - 1);
    moduleConfig.mesh_beacon.broadcast_offer_channel.name[sizeof(moduleConfig.mesh_beacon.broadcast_offer_channel.name) - 1] =
        '\0';
#endif
#ifdef USERPREFS_MESH_BEACON_OFFER_CHANNEL_PSK
    moduleConfig.mesh_beacon.has_broadcast_offer_channel = true;
    static const uint8_t beaconOfferPsk[] = USERPREFS_MESH_BEACON_OFFER_CHANNEL_PSK;
    static_assert(sizeof(beaconOfferPsk) <= sizeof(moduleConfig.mesh_beacon.broadcast_offer_channel.psk.bytes),
                  "USERPREFS_MESH_BEACON_OFFER_CHANNEL_PSK exceeds the 32-byte channel PSK buffer");
    memcpy(moduleConfig.mesh_beacon.broadcast_offer_channel.psk.bytes, beaconOfferPsk, sizeof(beaconOfferPsk));
    moduleConfig.mesh_beacon.broadcast_offer_channel.psk.size = sizeof(beaconOfferPsk);
#endif
// The USERPREFS_MESH_BEACON_ON_* keys were removed with the broadcast_on_* config fields. Fail the
// build rather than silently dropping a preconfigured beacon channel: define the equivalent
// USERPREFS_MESH_BEACON_TARGET_0_{PRESET,REGION,CHANNEL_INDEX} keys instead. CHANNEL_INDEX names a
// slot in the device's channel table, so the channel must also be provisioned on the node.
#if defined(USERPREFS_MESH_BEACON_ON_PRESET) || defined(USERPREFS_MESH_BEACON_ON_REGION) ||                                      \
    defined(USERPREFS_MESH_BEACON_ON_CHANNEL_NAME) || defined(USERPREFS_MESH_BEACON_ON_CHANNEL_PSK) ||                           \
    defined(USERPREFS_MESH_BEACON_ON_CHANNEL_NUM)
#error "USERPREFS_MESH_BEACON_ON_* removed; use USERPREFS_MESH_BEACON_TARGET_0_* (channel must be in the channel table)"
#endif
#ifdef USERPREFS_MESH_BEACON_LEGACY_SPLIT
    BEACON_APPLY_FLAG(USERPREFS_MESH_BEACON_LEGACY_SPLIT, meshtastic_ModuleConfig_MeshBeaconConfig_Flags_FLAG_LEGACY_SPLIT);
#endif
#undef BEACON_APPLY_FLAG
// Per-preset broadcast targets (up to 4). Each TARGET_<N>_* key bumps broadcast_targets_count as needed.
#define BEACON_TARGET_PRESET(N, VAL)                                                                                             \
    do {                                                                                                                         \
        if (moduleConfig.mesh_beacon.broadcast_targets_count < (N) + 1)                                                          \
            moduleConfig.mesh_beacon.broadcast_targets_count = (N) + 1;                                                          \
        moduleConfig.mesh_beacon.broadcast_targets[(N)].has_preset = true;                                                       \
        moduleConfig.mesh_beacon.broadcast_targets[(N)].preset = (VAL);                                                          \
    } while (0)
#define BEACON_TARGET_REGION(N, VAL)                                                                                             \
    do {                                                                                                                         \
        if (moduleConfig.mesh_beacon.broadcast_targets_count < (N) + 1)                                                          \
            moduleConfig.mesh_beacon.broadcast_targets_count = (N) + 1;                                                          \
        moduleConfig.mesh_beacon.broadcast_targets[(N)].region = (VAL);                                                          \
    } while (0)
// Target channel is referenced by index into the device's channel table (0..MAX_NUM_CHANNELS-1).
#define BEACON_TARGET_CH_INDEX(N, VAL)                                                                                           \
    do {                                                                                                                         \
        if (moduleConfig.mesh_beacon.broadcast_targets_count < (N) + 1)                                                          \
            moduleConfig.mesh_beacon.broadcast_targets_count = (N) + 1;                                                          \
        moduleConfig.mesh_beacon.broadcast_targets[(N)].has_channel_index = true;                                                \
        moduleConfig.mesh_beacon.broadcast_targets[(N)].channel_index = (VAL);                                                   \
    } while (0)
#ifdef USERPREFS_MESH_BEACON_TARGET_0_PRESET
    BEACON_TARGET_PRESET(0, USERPREFS_MESH_BEACON_TARGET_0_PRESET);
#endif
#ifdef USERPREFS_MESH_BEACON_TARGET_0_REGION
    BEACON_TARGET_REGION(0, USERPREFS_MESH_BEACON_TARGET_0_REGION);
#endif
#ifdef USERPREFS_MESH_BEACON_TARGET_0_CHANNEL_INDEX
    BEACON_TARGET_CH_INDEX(0, USERPREFS_MESH_BEACON_TARGET_0_CHANNEL_INDEX);
#endif
#ifdef USERPREFS_MESH_BEACON_TARGET_1_PRESET
    BEACON_TARGET_PRESET(1, USERPREFS_MESH_BEACON_TARGET_1_PRESET);
#endif
#ifdef USERPREFS_MESH_BEACON_TARGET_1_REGION
    BEACON_TARGET_REGION(1, USERPREFS_MESH_BEACON_TARGET_1_REGION);
#endif
#ifdef USERPREFS_MESH_BEACON_TARGET_1_CHANNEL_INDEX
    BEACON_TARGET_CH_INDEX(1, USERPREFS_MESH_BEACON_TARGET_1_CHANNEL_INDEX);
#endif
#ifdef USERPREFS_MESH_BEACON_TARGET_2_PRESET
    BEACON_TARGET_PRESET(2, USERPREFS_MESH_BEACON_TARGET_2_PRESET);
#endif
#ifdef USERPREFS_MESH_BEACON_TARGET_2_REGION
    BEACON_TARGET_REGION(2, USERPREFS_MESH_BEACON_TARGET_2_REGION);
#endif
#ifdef USERPREFS_MESH_BEACON_TARGET_2_CHANNEL_INDEX
    BEACON_TARGET_CH_INDEX(2, USERPREFS_MESH_BEACON_TARGET_2_CHANNEL_INDEX);
#endif
#ifdef USERPREFS_MESH_BEACON_TARGET_3_PRESET
    BEACON_TARGET_PRESET(3, USERPREFS_MESH_BEACON_TARGET_3_PRESET);
#endif
#ifdef USERPREFS_MESH_BEACON_TARGET_3_REGION
    BEACON_TARGET_REGION(3, USERPREFS_MESH_BEACON_TARGET_3_REGION);
#endif
#ifdef USERPREFS_MESH_BEACON_TARGET_3_CHANNEL_INDEX
    BEACON_TARGET_CH_INDEX(3, USERPREFS_MESH_BEACON_TARGET_3_CHANNEL_INDEX);
#endif
#undef BEACON_TARGET_PRESET
#undef BEACON_TARGET_REGION
#undef BEACON_TARGET_CH_INDEX
#endif // !MESHTASTIC_EXCLUDE_BEACON

    initModuleConfigIntervals();
}

void NodeDB::installRoleDefaults(meshtastic_Config_DeviceConfig_Role role, meshtastic_Config_DeviceConfig_Role previousRole)
{
    // Role presets are not user preferences: when leaving a role, undo only
    // values that still equal that role's forced value. A field customized
    // after the role was selected is preserved byte-for-byte.
    if (role != previousRole) {
        const auto resetIfStillForced = [](auto &field, const auto forcedValue, const auto replacement) {
            if (field == forcedValue)
                field = replacement;
        };

#ifdef USERPREFS_CONFIG_GPS_UPDATE_INTERVAL
        constexpr uint32_t routerGpsUpdateInterval = USERPREFS_CONFIG_GPS_UPDATE_INTERVAL;
        constexpr uint32_t normalGpsUpdateInterval = USERPREFS_CONFIG_GPS_UPDATE_INTERVAL;
#else
        constexpr uint32_t routerGpsUpdateInterval = ONE_DAY;
        constexpr uint32_t normalGpsUpdateInterval = 2 * 60;
#endif
#ifdef USERPREFS_CONFIG_POSITION_BROADCAST_INTERVAL
        constexpr uint32_t routerPositionBroadcastInterval = USERPREFS_CONFIG_POSITION_BROADCAST_INTERVAL;
        constexpr uint32_t normalPositionBroadcastInterval = USERPREFS_CONFIG_POSITION_BROADCAST_INTERVAL;
#else
        constexpr uint32_t routerPositionBroadcastInterval = ONE_DAY / 2;
        constexpr uint32_t normalPositionBroadcastInterval = 60 * 60;
#endif
#ifdef USERPREFS_CONFIG_SMART_POSITION_ENABLED
        constexpr bool normalSmartPositionEnabled = USERPREFS_CONFIG_SMART_POSITION_ENABLED;
#else
        constexpr bool normalSmartPositionEnabled = true;
#endif
#ifdef USERPREFS_CONFIG_DEVICE_TELEM_UPDATE_INTERVAL
        constexpr uint32_t normalDeviceTelemetryInterval = USERPREFS_CONFIG_DEVICE_TELEM_UPDATE_INTERVAL;
#else
        constexpr uint32_t normalDeviceTelemetryInterval = MAX_INTERVAL;
#endif
#ifdef USERPREFS_CONFIG_ENVIRONMENT_MEASUREMENT_ENABLED
        constexpr bool normalEnvironmentMeasurementEnabled = USERPREFS_CONFIG_ENVIRONMENT_MEASUREMENT_ENABLED;
#else
        constexpr bool normalEnvironmentMeasurementEnabled = false;
#endif
#ifdef USERPREFS_CONFIG_ENV_TELEM_UPDATE_INTERVAL
        constexpr uint32_t normalEnvironmentTelemetryInterval = USERPREFS_CONFIG_ENV_TELEM_UPDATE_INTERVAL;
#else
        constexpr uint32_t normalEnvironmentTelemetryInterval = 0;
#endif
#ifdef USERPREFS_CONFIG_AQ_TELEM_UPDATE_INTERVAL
        constexpr uint32_t normalAirQualityTelemetryInterval = USERPREFS_CONFIG_AQ_TELEM_UPDATE_INTERVAL;
#else
        constexpr uint32_t normalAirQualityTelemetryInterval = 0;
#endif
#ifdef USE_POWERSAVE
        constexpr uint32_t routerWaitBluetoothInterval = 30;
        constexpr uint32_t routerScreenOnInterval = 30;
        constexpr uint32_t normalWaitBluetoothInterval = 30;
        constexpr uint32_t normalScreenOnInterval = 30;
#else
        constexpr uint32_t routerWaitBluetoothInterval = 1;
        constexpr uint32_t routerScreenOnInterval = 1;
        constexpr uint32_t normalWaitBluetoothInterval = 60;
        constexpr uint32_t normalScreenOnInterval = 60 * 10;
#endif
        constexpr uint32_t normalPositionFlags =
            meshtastic_Config_PositionConfig_PositionFlags_ALTITUDE |
            meshtastic_Config_PositionConfig_PositionFlags_ALTITUDE_MSL | meshtastic_Config_PositionConfig_PositionFlags_SPEED |
            meshtastic_Config_PositionConfig_PositionFlags_HEADING | meshtastic_Config_PositionConfig_PositionFlags_DOP |
            meshtastic_Config_PositionConfig_PositionFlags_SATINVIEW;
        constexpr uint32_t takPositionFlags =
            meshtastic_Config_PositionConfig_PositionFlags_ALTITUDE | meshtastic_Config_PositionConfig_PositionFlags_SPEED |
            meshtastic_Config_PositionConfig_PositionFlags_HEADING | meshtastic_Config_PositionConfig_PositionFlags_DOP;

        switch (previousRole) {
        case meshtastic_Config_DeviceConfig_Role_ROUTER:
            resetIfStillForced(config.device.rebroadcast_mode, meshtastic_Config_DeviceConfig_RebroadcastMode_CORE_PORTNUMS_ONLY,
                               meshtastic_Config_DeviceConfig_RebroadcastMode_ALL);
            resetIfStillForced(config.position.gps_update_interval, routerGpsUpdateInterval, normalGpsUpdateInterval);
            resetIfStillForced(config.position.position_broadcast_secs, routerPositionBroadcastInterval,
                               normalPositionBroadcastInterval);
            resetIfStillForced(config.power.ls_secs, static_cast<uint32_t>(ONE_DAY), uint32_t{5 * 60});
            resetIfStillForced(config.power.sds_secs, static_cast<uint32_t>(ONE_DAY), UINT32_MAX);
            resetIfStillForced(config.power.wait_bluetooth_secs, routerWaitBluetoothInterval, normalWaitBluetoothInterval);
            resetIfStillForced(config.display.screen_on_secs, routerScreenOnInterval, normalScreenOnInterval);
            resetIfStillForced(moduleConfig.telemetry.device_update_interval, static_cast<uint32_t>(ONE_DAY / 2),
                               normalDeviceTelemetryInterval);
            break;
        case meshtastic_Config_DeviceConfig_Role_ROUTER_LATE:
            resetIfStillForced(moduleConfig.telemetry.device_update_interval, static_cast<uint32_t>(ONE_DAY),
                               normalDeviceTelemetryInterval);
            break;
        case meshtastic_Config_DeviceConfig_Role_SENSOR:
            resetIfStillForced(moduleConfig.telemetry.device_update_interval, uint32_t{60 * 60}, normalDeviceTelemetryInterval);
            resetIfStillForced(moduleConfig.telemetry.environment_measurement_enabled, true, normalEnvironmentMeasurementEnabled);
            resetIfStillForced(moduleConfig.telemetry.environment_update_interval, uint32_t{300},
                               normalEnvironmentTelemetryInterval);
            break;
        case meshtastic_Config_DeviceConfig_Role_TRACKER:
            resetIfStillForced(moduleConfig.telemetry.device_update_interval, uint32_t{60 * 60}, normalDeviceTelemetryInterval);
            break;
        case meshtastic_Config_DeviceConfig_Role_TAK_TRACKER:
            resetIfStillForced(config.device.node_info_broadcast_secs, static_cast<uint32_t>(ONE_DAY), uint32_t{3 * 60 * 60});
            resetIfStillForced(config.position.position_broadcast_smart_enabled, true, normalSmartPositionEnabled);
            resetIfStillForced(config.position.position_broadcast_secs, uint32_t{3 * 60}, normalPositionBroadcastInterval);
            resetIfStillForced(config.position.broadcast_smart_minimum_distance, uint32_t{20}, uint32_t{100});
            resetIfStillForced(config.position.broadcast_smart_minimum_interval_secs, uint32_t{15}, uint32_t{5 * 60});
            resetIfStillForced(config.position.position_flags, takPositionFlags, normalPositionFlags);
            resetIfStillForced(moduleConfig.telemetry.device_update_interval, static_cast<uint32_t>(ONE_DAY),
                               normalDeviceTelemetryInterval);
            break;
        case meshtastic_Config_DeviceConfig_Role_TAK:
            resetIfStillForced(config.device.node_info_broadcast_secs, static_cast<uint32_t>(ONE_DAY), uint32_t{3 * 60 * 60});
            resetIfStillForced(config.position.position_broadcast_smart_enabled, false, normalSmartPositionEnabled);
            resetIfStillForced(config.position.position_broadcast_secs, static_cast<uint32_t>(ONE_DAY),
                               normalPositionBroadcastInterval);
            resetIfStillForced(config.position.position_flags, takPositionFlags, normalPositionFlags);
            resetIfStillForced(moduleConfig.telemetry.device_update_interval, static_cast<uint32_t>(ONE_DAY),
                               normalDeviceTelemetryInterval);
            break;
        case meshtastic_Config_DeviceConfig_Role_LOST_AND_FOUND:
            resetIfStillForced(config.position.position_broadcast_smart_enabled, false, normalSmartPositionEnabled);
            resetIfStillForced(config.position.position_broadcast_secs, uint32_t{300}, normalPositionBroadcastInterval);
            break;
        case meshtastic_Config_DeviceConfig_Role_CLIENT_HIDDEN:
            resetIfStillForced(config.device.rebroadcast_mode, meshtastic_Config_DeviceConfig_RebroadcastMode_LOCAL_ONLY,
                               meshtastic_Config_DeviceConfig_RebroadcastMode_ALL);
            resetIfStillForced(config.device.node_info_broadcast_secs, static_cast<uint32_t>(MAX_INTERVAL),
                               uint32_t{3 * 60 * 60});
            resetIfStillForced(config.position.position_broadcast_smart_enabled, false, normalSmartPositionEnabled);
            resetIfStillForced(config.position.position_broadcast_secs, static_cast<uint32_t>(MAX_INTERVAL),
                               normalPositionBroadcastInterval);
            resetIfStillForced(moduleConfig.neighbor_info.update_interval, static_cast<uint32_t>(MAX_INTERVAL), uint32_t{0});
            resetIfStillForced(moduleConfig.telemetry.device_update_interval, static_cast<uint32_t>(MAX_INTERVAL),
                               normalDeviceTelemetryInterval);
            resetIfStillForced(moduleConfig.telemetry.environment_update_interval, static_cast<uint32_t>(MAX_INTERVAL),
                               normalEnvironmentTelemetryInterval);
            resetIfStillForced(moduleConfig.telemetry.air_quality_interval, static_cast<uint32_t>(MAX_INTERVAL),
                               normalAirQualityTelemetryInterval);
            resetIfStillForced(moduleConfig.telemetry.health_update_interval, static_cast<uint32_t>(MAX_INTERVAL), uint32_t{0});
            break;
        default:
            break;
        }
    }

    const bool previousRoleForcedUnmessagable =
        IS_ONE_OF(previousRole, meshtastic_Config_DeviceConfig_Role_ROUTER, meshtastic_Config_DeviceConfig_Role_ROUTER_LATE,
                  meshtastic_Config_DeviceConfig_Role_SENSOR, meshtastic_Config_DeviceConfig_Role_TRACKER,
                  meshtastic_Config_DeviceConfig_Role_TAK_TRACKER);
    const bool newRoleSupportsMessaging =
        IS_ONE_OF(role, meshtastic_Config_DeviceConfig_Role_CLIENT, meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE,
                  meshtastic_Config_DeviceConfig_Role_CLIENT_BASE, meshtastic_Config_DeviceConfig_Role_CLIENT_HIDDEN,
                  meshtastic_Config_DeviceConfig_Role_LOST_AND_FOUND, meshtastic_Config_DeviceConfig_Role_TAK);

    // Infrastructure/tracker defaults mark the owner as unavailable for
    // messaging. Clear that derived value when the same device returns to a
    // messaging role; otherwise the old role silently remains sticky. Do this
    // only on an actual transition, never on boot, so an explicit user choice
    // in a messaging role remains intact.
    if (role != previousRole && previousRoleForcedUnmessagable && newRoleSupportsMessaging && owner.has_is_unmessagable &&
        owner.is_unmessagable) {
        owner.has_is_unmessagable = true;
        owner.is_unmessagable = false;
    }

    if (role == meshtastic_Config_DeviceConfig_Role_ROUTER) {
        initConfigIntervals();
        initModuleConfigIntervals();
        moduleConfig.telemetry.device_update_interval = default_telemetry_broadcast_interval_secs;
        config.device.rebroadcast_mode = meshtastic_Config_DeviceConfig_RebroadcastMode_CORE_PORTNUMS_ONLY;
        owner.has_is_unmessagable = true;
        owner.is_unmessagable = true;
    } else if (role == meshtastic_Config_DeviceConfig_Role_ROUTER_LATE) {
        moduleConfig.telemetry.device_update_interval = ONE_DAY;
        owner.has_is_unmessagable = true;
        owner.is_unmessagable = true;
    } else if (role == meshtastic_Config_DeviceConfig_Role_SENSOR) {
        owner.has_is_unmessagable = true;
        owner.is_unmessagable = true;
        moduleConfig.telemetry.device_update_interval = default_telemetry_broadcast_interval_secs;
        moduleConfig.telemetry.environment_measurement_enabled = true;
        moduleConfig.telemetry.environment_update_interval = 300;
    } else if (role == meshtastic_Config_DeviceConfig_Role_LOST_AND_FOUND) {
        config.position.position_broadcast_smart_enabled = false;
        config.position.position_broadcast_secs = 300; // Every 5 minutes
    } else if (role == meshtastic_Config_DeviceConfig_Role_TAK) {
        config.device.node_info_broadcast_secs = ONE_DAY;
        config.position.position_broadcast_smart_enabled = false;
        config.position.position_broadcast_secs = ONE_DAY;
        // Remove Altitude MSL from flags since CoTs use HAE (height above ellipsoid)
        config.position.position_flags =
            (meshtastic_Config_PositionConfig_PositionFlags_ALTITUDE | meshtastic_Config_PositionConfig_PositionFlags_SPEED |
             meshtastic_Config_PositionConfig_PositionFlags_HEADING | meshtastic_Config_PositionConfig_PositionFlags_DOP);
        moduleConfig.telemetry.device_update_interval = ONE_DAY;
    } else if (role == meshtastic_Config_DeviceConfig_Role_TRACKER) {
        owner.has_is_unmessagable = true;
        owner.is_unmessagable = true;
        moduleConfig.telemetry.device_update_interval = default_telemetry_broadcast_interval_secs;
    } else if (role == meshtastic_Config_DeviceConfig_Role_TAK_TRACKER) {
        owner.has_is_unmessagable = true;
        owner.is_unmessagable = true;
        config.device.node_info_broadcast_secs = ONE_DAY;
        config.position.position_broadcast_smart_enabled = true;
        config.position.position_broadcast_secs = 3 * 60; // Every 3 minutes
        config.position.broadcast_smart_minimum_distance = 20;
        config.position.broadcast_smart_minimum_interval_secs = 15;
        // Remove Altitude MSL from flags since CoTs use HAE (height above ellipsoid)
        config.position.position_flags =
            (meshtastic_Config_PositionConfig_PositionFlags_ALTITUDE | meshtastic_Config_PositionConfig_PositionFlags_SPEED |
             meshtastic_Config_PositionConfig_PositionFlags_HEADING | meshtastic_Config_PositionConfig_PositionFlags_DOP);
        moduleConfig.telemetry.device_update_interval = ONE_DAY;
    } else if (role == meshtastic_Config_DeviceConfig_Role_CLIENT_HIDDEN) {
        config.device.rebroadcast_mode = meshtastic_Config_DeviceConfig_RebroadcastMode_LOCAL_ONLY;
        config.device.node_info_broadcast_secs = MAX_INTERVAL;
        config.position.position_broadcast_smart_enabled = false;
        config.position.position_broadcast_secs = MAX_INTERVAL;
        moduleConfig.neighbor_info.update_interval = MAX_INTERVAL;
        moduleConfig.telemetry.device_update_interval = MAX_INTERVAL;
        moduleConfig.telemetry.environment_update_interval = MAX_INTERVAL;
        moduleConfig.telemetry.air_quality_interval = MAX_INTERVAL;
        moduleConfig.telemetry.health_update_interval = MAX_INTERVAL;
    }

    if (role != previousRole) {
        // owner.role and the local slim-node cache are what NodeInfo sends.
        // Updating only config.device.role leaves the old role visible until a
        // reboot (and can persist it when a bulk transaction writes NodeDB).
        owner.role = role;
        if (configLoadComplete)
            updateUser(getNodeNum(), owner, 0, false, false);
    }
}

void NodeDB::initModuleConfigIntervals()
{
    // Zero out telemetry intervals so that they coalesce to defaults in Default.h
#ifdef USERPREFS_CONFIG_DEVICE_TELEM_UPDATE_INTERVAL
    moduleConfig.telemetry.device_update_interval = USERPREFS_CONFIG_DEVICE_TELEM_UPDATE_INTERVAL;
#else
    moduleConfig.telemetry.device_update_interval = MAX_INTERVAL;
#endif

#ifdef USERPREFS_CONFIG_ENVIRONMENT_MEASUREMENT_ENABLED
    moduleConfig.telemetry.environment_measurement_enabled = USERPREFS_CONFIG_ENVIRONMENT_MEASUREMENT_ENABLED;
#endif

#ifdef USERPREFS_CONFIG_ENV_TELEM_UPDATE_INTERVAL
    moduleConfig.telemetry.environment_update_interval = USERPREFS_CONFIG_ENV_TELEM_UPDATE_INTERVAL;
#else
    moduleConfig.telemetry.environment_update_interval = 0;
#endif

#ifdef USERPREFS_CONFIG_ENV_SCREEN_SCREEN_ENABLED
    moduleConfig.telemetry.environment_screen_enabled = USERPREFS_CONFIG_ENV_SCREEN_SCREEN_ENABLED;
#endif

#ifdef USERPREFS_CONFIG_AQ_TELEM_UPDATE_INTERVAL
    moduleConfig.telemetry.air_quality_interval = USERPREFS_CONFIG_AQ_TELEM_UPDATE_INTERVAL;
#else
    moduleConfig.telemetry.air_quality_interval = 0;
#endif

#ifdef USERPREFS_CONFIG_AQ_MEASUREMENT_ENABLED
    moduleConfig.telemetry.air_quality_enabled = USERPREFS_CONFIG_AQ_MEASUREMENT_ENABLED;
#endif

#ifdef USERPREFS_CONFIG_AQ_SCREEN_ENABLED
    moduleConfig.telemetry.air_quality_screen_enabled = USERPREFS_CONFIG_AQ_SCREEN_ENABLED;
#endif

    moduleConfig.telemetry.power_update_interval = 0;
    moduleConfig.telemetry.health_update_interval = 0;
    moduleConfig.neighbor_info.update_interval = 0;
    moduleConfig.paxcounter.paxcounter_update_interval = 0;
}

void NodeDB::installDefaultChannels()
{
    LOG_INFO("Install default ChannelFile");
    memset(&channelFile, 0, sizeof(meshtastic_ChannelFile));
    channelFile.version = DEVICESTATE_CUR_VER;
}

bool NodeDB::resetNodes(bool keepFavorites)
{
#if defined(HELTEC_V4_OLED) && defined(FSCom)
    if (rebootAtMsec != 0 || shutdownAtMsec != 0) {
        LOG_ERROR("Node database reset refused while reboot/shutdown is pending");
        return false;
    }
    bool expectedInactive = false;
    if (!destructiveStorageMutationActive.compare_exchange_strong(expectedInactive, true, std::memory_order_acq_rel)) {
        LOG_ERROR("Node database reset refused while another destructive operation is active");
        return false;
    }
    destructiveStorageOwnerTask.store(reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle()), std::memory_order_release);
    bool keepDestructiveFenceUntilReboot = false;
    struct NodeResetActivityGuard {
        std::atomic<bool> &flag;
        std::atomic<uintptr_t> &owner;
        bool &keepUntilReboot;
        ~NodeResetActivityGuard()
        {
            owner.store(0, std::memory_order_release);
            if (!keepUntilReboot)
                flag.store(false, std::memory_order_release);
        }
    } activityGuard{destructiveStorageMutationActive, destructiveStorageOwnerTask, keepDestructiveFenceUntilReboot};
    if (!waitForExternalStateReaders()) {
        LOG_ERROR("Node database reset refused while a PhoneAPI/HTTP state reader is active");
        return false;
    }
    concurrency::LockGuard transactionGuard(&heltecPreferencesTransactionLock);
    if (isPreferenceEditTransactionActive()) {
        LOG_ERROR("Node database reset refused while a settings edit is open");
        return false;
    }
    HeltecXModemStorageGuard xmodemGuard;
    if (!xmodemGuard) {
        LOG_ERROR("Node database reset refused while XMODEM transfer is active");
        return false;
    }
    if (requiresConfigRecovery() && !canResetNodesForRecovery()) {
        LOG_ERROR("Node database reset refused during unrelated recovery");
        return false;
    }
    const HeltecResetPendingKind pendingKind = readHeltecResetPendingMarker();
    if (pendingKind != HeltecResetPendingKind::NONE && pendingKind != HeltecResetPendingKind::NODEDB_RESET) {
        LOG_ERROR("Node database reset refused while another recovery transaction is active");
        return false;
    }
    if (!heltecDestructiveStoragePowerIsSafe()) {
        LOG_ERROR("Node database reset refused: connect stable power or charge the battery");
        return false;
    }
    if (!writeHeltecResetPendingMarker(HeltecResetPendingKind::NODEDB_RESET, true)) {
        LOG_ERROR("Node database reset refused: durable intent could not be verified");
        return false;
    }
    keepDestructiveFenceUntilReboot = true;
    incompleteNodeDatabaseResetDetected = true;
#if HAS_WIFI
    deinitWifi();
#endif
    disableBluetooth();
#if !MESHTASTIC_EXCLUDE_GPS
    if (gps)
        gps->disable();
#endif
    if (!parkHeltecRadioForStorageMutation()) {
        LOG_ERROR("Node database reset could not safely drain/park LoRa");
        incompleteNodeDatabaseResetDetected = true;
        unreadablePreferenceSegments |= SEGMENT_NODEDATABASE;
        forceHeltecLocalRecoveryConfiguration();
        scheduleHeltecRecoveryReboot();
        return false;
    }
#endif
    const auto abortNodeDatabaseReset = [&]() {
#if defined(HELTEC_V4_OLED)
        incompleteNodeDatabaseResetDetected = true;
        unreadablePreferenceSegments |= SEGMENT_NODEDATABASE;
        forceHeltecLocalRecoveryConfiguration();
        (void)parkHeltecRadioForStorageMutation();
        scheduleHeltecRecoveryReboot();
#endif
        return false;
    };
    const int previousUnreadableSegments = unreadablePreferenceSegments;
    // This command is explicit authorization to replace only nodes.proto. Any
    // independently unreadable DeviceState remains protected below.
    unreadablePreferenceSegments &= ~SEGMENT_NODEDATABASE;
    if (!config.position.fixed_position)
        clearLocalPosition();
    NodeNum ourNum = getNodeNum();
    numMeshNodes = 1;
    if (keepFavorites) {
        LOG_INFO("Clear node database, keep favorites");
        // Compact favorites into contiguous low slots: zeroing in place leaves one above
        // numMeshNodes, invisible to every `i < numMeshNodes` scan yet still serialized to flash.
        for (size_t i = 1; i < meshNodes->size(); i++) {
            const meshtastic_NodeInfoLite &node = meshNodes->at(i);
            if (nodeInfoLiteIsFavorite(&node)) {
                if (numMeshNodes != i)
                    meshNodes->at(numMeshNodes) = node;
                numMeshNodes += 1;
            } else if (node.num) {
                eraseNodeSatellites(node.num);
            }
        }
        std::fill(nodeDatabase.nodes.begin() + numMeshNodes, nodeDatabase.nodes.end(), meshtastic_NodeInfoLite());
    } else {
        LOG_INFO("Clear node database, remove favorites");
        for (size_t i = 1; i < meshNodes->size(); i++) {
            const NodeNum gone = meshNodes->at(i).num;
            if (gone)
                eraseNodeSatellites(gone);
        }
        std::fill(nodeDatabase.nodes.begin() + 1, nodeDatabase.nodes.end(), meshtastic_NodeInfoLite());
    }
    (void)ourNum;
#if WARM_NODE_COUNT > 0
    warmStore.clear(); // warm entries are never favorites; a DB reset clears them too
#endif
#if HAS_TRAFFIC_MANAGEMENT
    // A user-initiated DB reset forgets everything; TMM's caches must not resurrect it.
    if (trafficManagementModule)
        trafficManagementModule->purgeAll();
#endif

    devicestate.has_rx_waypoint = false;
    bool nodeDatabaseSaved = false;
#if defined(HELTEC_V4_OLED) && defined(FSCom)
    if (owner.public_key.size == 32 || owner.is_licensed) {
        nodeDatabaseSaved = saveNodeDatabaseToDisk();
    } else {
        concurrency::LockGuard guard(spiLock);
        String temporaryPath = nodeDatabaseFileName;
        temporaryPath += ".tmp";
        nodeDatabaseSaved = removeHeltecFileChecked(nodeDatabaseFileName) && removeHeltecFileChecked(temporaryPath.c_str()) &&
                            !FSCom.exists(nodeDatabaseFileName) && !FSCom.exists(temporaryPath.c_str());
    }
#else
    nodeDatabaseSaved = saveNodeDatabaseToDisk();
#endif
    const bool deviceStateSaved = saveDeviceStateToDisk();
#if WARM_NODE_COUNT > 0
    // Covers the no-key path above, where nodes.proto is removed rather than
    // saveNodeDatabaseToDisk() being called.
    const bool warmStoreSaved = warmStore.saveIfDirty(true);
#else
    const bool warmStoreSaved = true;
#endif
    if (!nodeDatabaseSaved) {
        unreadablePreferenceSegments = previousUnreadableSegments;
    }
    if (neighborInfoModule && moduleConfig.neighbor_info.enabled)
        neighborInfoModule->resetNeighbors();
    const bool resetSaved = nodeDatabaseSaved && deviceStateSaved && warmStoreSaved;
#if defined(HELTEC_V4_OLED) && defined(FSCom)
    if (!resetSaved || !heltecDestructiveStoragePowerIsSafe() ||
        !clearHeltecResetPendingMarker(HeltecResetPendingKind::NODEDB_RESET, true)) {
        LOG_ERROR("Node database reset did not reach its final commit point");
        return abortNodeDatabaseReset();
    }
    incompleteNodeDatabaseResetDetected = false;
    // The reset is now durably committed and its marker is gone. Arm the
    // ordinary reboot only at this final point; failure paths independently
    // schedule the marker-controlled recovery reboot.
    rebootAtMsec = millis() + DEFAULT_REBOOT_SECONDS * 1000;
#endif
    return resetSaved;
}

bool NodeDB::canResetNodesForRecovery() const
{
#if defined(HELTEC_V4_OLED) && defined(FSCom)
    const HeltecResetPendingKind pendingKind = readHeltecResetPendingMarker();
    const bool noUnrelatedUnreadableSegments = (unreadablePreferenceSegments & ~SEGMENT_NODEDATABASE) == 0;
    const bool retryingInterruptedReset =
        incompleteNodeDatabaseResetDetected && pendingKind == HeltecResetPendingKind::NODEDB_RESET;
    const bool repairingIsolatedNodeDatabase =
        unreadablePreferenceSegments == SEGMENT_NODEDATABASE && pendingKind == HeltecResetPendingKind::NONE;
    return noUnrelatedUnreadableSegments && !incompleteConfigResetDetected && !incompletePreferenceRestoreDetected &&
           (retryingInterruptedReset || repairingIsolatedNodeDatabase);
#else
    return true;
#endif
}

bool NodeDB::removeNodeByNum(NodeNum nodeNum, bool persist)
{
    int newPos = 0, removed = 0;
    for (int i = 0; i < numMeshNodes; i++) {
        if (meshNodes->at(i).num != nodeNum)
            meshNodes->at(newPos++) = meshNodes->at(i);
        else
            removed++;
    }
    numMeshNodes -= removed;
    if (removed) {
        // Clear exactly the slots compaction vacated. Sizing this from `removed` (rather than a
        // fixed one) keeps it inside the vector when nothing matched and the store is full.
        const size_t first = numMeshNodes;
        const size_t last = std::min(first + removed, nodeDatabase.nodes.size());
        std::fill(nodeDatabase.nodes.begin() + first, nodeDatabase.nodes.begin() + last, meshtastic_NodeInfoLite());
    }
    // Drop the node's satellite stores and warm-tier copy regardless of which tier it lived in, so
    // an explicit removal fully forgets it.
    eraseNodeSatellites(nodeNum);
#if WARM_NODE_COUNT > 0
    // Explicit user removal: don't let the warm tier resurrect the node
    warmStore.remove(nodeNum);
#endif
#if HAS_TRAFFIC_MANAGEMENT
    // Explicit removal is full removal: the TrafficManagement caches (unified slot +
    // NodeInfo identity cache) must not keep serving or resurrect the node either.
    if (trafficManagementModule)
        trafficManagementModule->purgeNode(nodeNum);
#endif

    LOG_DEBUG("NodeDB::removeNodeByNum purged %d entries%s", removed, persist ? ", saving" : "");
    return !persist || saveNodeDatabaseToDisk();
}

void NodeDB::clearLocalPosition()
{
#if !MESHTASTIC_EXCLUDE_POSITIONDB
    concurrency::LockGuard guard(&satelliteMutex);
    nodePositions.erase(getNodeNum());
#endif
    setLocalPosition(meshtastic_Position_init_default);
    localPositionUpdatedSinceBoot = false;
}

bool NodeDB::copyNodePosition(NodeNum n, meshtastic_PositionLite &out) const
{
#if MESHTASTIC_EXCLUDE_POSITIONDB
    (void)n;
    (void)out;
    return false;
#else
    concurrency::LockGuard guard(&satelliteMutex);
    return copySatelliteEntry(nodePositions, n, out);
#endif
}

bool NodeDB::copyNodeTelemetry(NodeNum n, meshtastic_DeviceMetrics &out) const
{
#if MESHTASTIC_EXCLUDE_TELEMETRYDB
    (void)n;
    (void)out;
    return false;
#else
    concurrency::LockGuard guard(&satelliteMutex);
    return copySatelliteEntry(nodeTelemetry, n, out);
#endif
}

bool NodeDB::copyNodeEnvironment(NodeNum n, meshtastic_EnvironmentMetrics &out) const
{
#if MESHTASTIC_EXCLUDE_ENVIRONMENTDB
    (void)n;
    (void)out;
    return false;
#else
    concurrency::LockGuard guard(&satelliteMutex);
    return copySatelliteEntry(nodeEnvironment, n, out);
#endif
}

bool NodeDB::copyNodeStatus(NodeNum n, meshtastic_StatusMessage &out) const
{
#if MESHTASTIC_EXCLUDE_STATUSDB
    (void)n;
    (void)out;
    return false;
#else
    concurrency::LockGuard guard(&satelliteMutex);
    return copySatelliteEntry(nodeStatus, n, out);
#endif
}

std::vector<NodeNum> NodeDB::snapshotPositionNodeNums(NodeNum exclude) const
{
#if MESHTASTIC_EXCLUDE_POSITIONDB
    (void)exclude;
    return {};
#else
    concurrency::LockGuard guard(&satelliteMutex);
    return snapshotSatelliteNodeNums(nodePositions, exclude);
#endif
}

std::vector<NodeNum> NodeDB::snapshotTelemetryNodeNums(NodeNum exclude) const
{
#if MESHTASTIC_EXCLUDE_TELEMETRYDB
    (void)exclude;
    return {};
#else
    concurrency::LockGuard guard(&satelliteMutex);
    return snapshotSatelliteNodeNums(nodeTelemetry, exclude);
#endif
}

std::vector<NodeNum> NodeDB::snapshotEnvironmentNodeNums(NodeNum exclude) const
{
#if MESHTASTIC_EXCLUDE_ENVIRONMENTDB
    (void)exclude;
    return {};
#else
    concurrency::LockGuard guard(&satelliteMutex);
    return snapshotSatelliteNodeNums(nodeEnvironment, exclude);
#endif
}

std::vector<NodeNum> NodeDB::snapshotStatusNodeNums(NodeNum exclude) const
{
#if MESHTASTIC_EXCLUDE_STATUSDB
    (void)exclude;
    return {};
#else
    concurrency::LockGuard guard(&satelliteMutex);
    return snapshotSatelliteNodeNums(nodeStatus, exclude);
#endif
}

void NodeDB::setNodeStatus(NodeNum n, const meshtastic_StatusMessage &status)
{
#if MESHTASTIC_EXCLUDE_STATUSDB
    (void)n;
    (void)status;
#else
    concurrency::LockGuard guard(&satelliteMutex);
    evictSatelliteOverCap(*this, nodeStatus, n);
    nodeStatus[n] = status;
#endif
}

void NodeDB::touchNodePositionTime(NodeNum n, uint32_t time)
{
#if MESHTASTIC_EXCLUDE_POSITIONDB
    (void)n;
    (void)time;
#else
    concurrency::LockGuard guard(&satelliteMutex);
    evictSatelliteOverCap(*this, nodePositions, n);
    nodePositions[n].time = time;
#endif
}

void NodeDB::eraseNodeSatellites(NodeNum n)
{
    concurrency::LockGuard guard(&satelliteMutex);
#if !MESHTASTIC_EXCLUDE_POSITIONDB
    nodePositions.erase(n);
#endif

#if !MESHTASTIC_EXCLUDE_TELEMETRYDB
    nodeTelemetry.erase(n);
#endif

#if !MESHTASTIC_EXCLUDE_ENVIRONMENTDB
    nodeEnvironment.erase(n);
#endif

#if !MESHTASTIC_EXCLUDE_STATUSDB
    nodeStatus.erase(n);
#endif
}

bool NodeDB::enforceSatelliteCaps()
{
    concurrency::LockGuard guard(&satelliteMutex);
    bool trimmedAny = false;
    auto trim = [this, &trimmedAny](auto &map, const char *name) {
        const size_t before = map.size();
        while (map.size() > MAX_SATELLITE_NODES) {
            if (!evictStalestSatellite(*this, map))
                break;
        }
        if (map.size() != before) {
            trimmedAny = true;
            LOG_MIGRATION("Trimmed %s satellites %u -> %u (cap %d)", name, (unsigned)before, (unsigned)map.size(),
                          MAX_SATELLITE_NODES);
        }
    };
#if !MESHTASTIC_EXCLUDE_POSITIONDB
    trim(nodePositions, "position");
#endif

#if !MESHTASTIC_EXCLUDE_TELEMETRYDB
    trim(nodeTelemetry, "telemetry");
#endif

#if !MESHTASTIC_EXCLUDE_ENVIRONMENTDB
    trim(nodeEnvironment, "environment");
#endif

#if !MESHTASTIC_EXCLUDE_STATUSDB
    trim(nodeStatus, "status");
#endif

    (void)trim; // all four maps may be compiled out

    // Approximate satellite heap usage: each std::map entry is one rb-tree node,
    // value_type plus ~44 B of node overhead (parent/left/right pointers, color,
    // allocator rounding on 32-bit targets - an estimate, not exact bookkeeping).
    size_t satBytes = 0;
#if !MESHTASTIC_EXCLUDE_POSITIONDB
    satBytes += nodePositions.size() * (sizeof(decltype(nodePositions)::value_type) + 44);
#endif
#if !MESHTASTIC_EXCLUDE_TELEMETRYDB
    satBytes += nodeTelemetry.size() * (sizeof(decltype(nodeTelemetry)::value_type) + 44);
#endif
#if !MESHTASTIC_EXCLUDE_ENVIRONMENTDB
    satBytes += nodeEnvironment.size() * (sizeof(decltype(nodeEnvironment)::value_type) + 44);
#endif
#if !MESHTASTIC_EXCLUDE_STATUSDB
    satBytes += nodeStatus.size() * (sizeof(decltype(nodeStatus)::value_type) + 44);
#endif
    memaudit::set("satmaps", satBytes);

    return trimmedAny;
}

#if WARM_NODE_COUNT > 0
// Classify an evicted node's hop-protected category for the warm tier. Favorite/ignored/
// verified are local flags (rarely reach warm - they're eviction-protected - but classify
// them if they do); otherwise tracker/sensor/tak_tracker are role-protected.
static uint8_t warmProtectedCategory(const meshtastic_NodeInfoLite &n)
{
    if (nodeInfoLiteHasXeddsaSigned(&n))
        return static_cast<uint8_t>(WarmProtected::XeddsaSigner);
    if (n.bitfield & (NODEINFO_BITFIELD_IS_FAVORITE_MASK | NODEINFO_BITFIELD_IS_IGNORED_MASK |
                      NODEINFO_BITFIELD_IS_KEY_MANUALLY_VERIFIED_MASK))
        return static_cast<uint8_t>(WarmProtected::Flag);
    if (IS_ONE_OF(n.role, meshtastic_Config_DeviceConfig_Role_TRACKER, meshtastic_Config_DeviceConfig_Role_SENSOR,
                  meshtastic_Config_DeviceConfig_Role_TAK_TRACKER))
        return static_cast<uint8_t>(WarmProtected::Role);
    return static_cast<uint8_t>(WarmProtected::None);
}

// The warm tier packs the device role into a 4-bit field (WARM_ROLE_MASK). Fail the build
// loudly if a new role outgrows it, rather than silently truncating role on eviction.
static_assert(_meshtastic_Config_DeviceConfig_Role_MAX <= WARM_ROLE_MASK,
              "device role no longer fits the 4-bit warm metadata field");
#endif // WARM_NODE_COUNT > 0

void NodeDB::cleanupMeshDB()
{
    int newPos = 0, removed = 0;
    for (int i = 0; i < numMeshNodes; i++) {
        meshtastic_NodeInfoLite &n = meshNodes->at(i);
        // Keep ignored (blocked) nodes even without user info: a block set by
        // bare node ID has no NodeInfo and would otherwise be purged here,
        // silently dropping the block.
        if (nodeInfoLiteHasUser(&n) || nodeInfoLiteIsIgnored(&n)) {
            if (n.public_key.size > 0) {
                if (memfll(n.public_key.bytes, 0, n.public_key.size)) {
                    n.public_key.size = 0;
                }
            }
            if (newPos != i)
                meshNodes->at(newPos++) = n;
            else
                newPos++;
        } else {
            // No user info - drop this node and its satellites
            const NodeNum gone = n.num;
            if (gone) {
#if WARM_NODE_COUNT > 0
                // Keep any key we learned (e.g. via a DM before the NodeInfo
                // exchange completed) rather than losing it with the purge.
                if (n.public_key.size == 32)
                    warmStore.absorb(gone, n.last_heard, n.public_key.bytes, n.role, warmProtectedCategory(n),
                                     nodeInfoLiteHasXeddsaSigned(&n));
#endif

                eraseNodeSatellites(gone);
            }
            removed++;
        }
    }
    numMeshNodes -= removed;
    std::fill(nodeDatabase.nodes.begin() + numMeshNodes, nodeDatabase.nodes.begin() + numMeshNodes + removed,
              meshtastic_NodeInfoLite());
    LOG_DEBUG("cleanupMeshDB purged %d entries", removed);
}

void NodeDB::installDefaultDeviceState()
{
    LOG_INFO("Install default DeviceState");
    // memset(&devicestate, 0, sizeof(meshtastic_DeviceState));

    // init our devicestate with valid flags so protobuf writing/reading will work
    devicestate.has_my_node = true;
    devicestate.has_owner = true;
    devicestate.version = DEVICESTATE_CUR_VER;
    devicestate.receive_queue_count = 0; // Not yet implemented FIXME
    devicestate.has_rx_waypoint = false;

    generatePacketId(); // FIXME - ugly way to init current_packet_id;

    // Set default owner name
    pickNewNodeNum(); // based on macaddr now
#ifdef USERPREFS_CONFIG_OWNER_LONG_NAME
    snprintf(owner.long_name, sizeof(owner.long_name), (const char *)USERPREFS_CONFIG_OWNER_LONG_NAME);
#else
    snprintf(owner.long_name, sizeof(owner.long_name), "Meshtastic %04x", getNodeNum() & 0x0ffff);
#endif

    clampLongName(owner.long_name); // vendor userprefs may exceed the local cap

#ifdef USERPREFS_CONFIG_OWNER_SHORT_NAME
    snprintf(owner.short_name, sizeof(owner.short_name), (const char *)USERPREFS_CONFIG_OWNER_SHORT_NAME);
#else
    snprintf(owner.short_name, sizeof(owner.short_name), "%04x", getNodeNum() & 0x0ffff);
#endif

    snprintf(owner.id, sizeof(owner.id), "!%08x", getNodeNum()); // Default node ID now based on nodenum
    memcpy(owner.macaddr, ourMacAddr, sizeof(owner.macaddr));
    owner.has_is_unmessagable = true;
    owner.is_unmessagable = false;

#ifdef HAS_HAM_2M_ONLY
    // Ham-band-only hardware defaults to licensed operation. The user can still flip this off later
    // (e.g. a commercial operator on an adjacent allocation who wants to keep encryption on) - we
    // only set the default here, not on every boot.
    owner.is_licensed = true;
#endif
}

// We reserve a few nodenums for future use
#define NUM_RESERVED 4

/**
 * get our starting (provisional) nodenum from flash.
 */
void NodeDB::pickNewNodeNum()
{
    NodeNum nodeNum = myNodeInfo.my_node_num;
    getMacAddr(ourMacAddr); // Make sure ourMacAddr is set
    if (nodeNum == 0) {
        // Pick an initial nodenum based on the macaddr
        nodeNum = (ourMacAddr[2] << 24) | (ourMacAddr[3] << 16) | (ourMacAddr[4] << 8) | ourMacAddr[5];
    }

    // Identity check via public key (or "empty slot?" when no keys yet);
    // macaddr no longer lives on the slim header.
    auto isOurOwnEntry = [&](const meshtastic_NodeInfoLite *n) -> bool {
        if (!n)
            return false;
        if (owner.public_key.size == 32 && n->public_key.size == 32)
            return memcmp(n->public_key.bytes, owner.public_key.bytes, 32) == 0;
        return !nodeInfoLiteHasUser(n);
    };

    meshtastic_NodeInfoLite *found;
    while (((found = getMeshNode(nodeNum)) && !isOurOwnEntry(found)) ||
           (nodeNum == NODENUM_BROADCAST || nodeNum < NUM_RESERVED)) {
        NodeNum candidate = random(NUM_RESERVED, LONG_MAX); // try a new random choice
        if (found)
            LOG_WARN("NOTE! Desired nodenum 0x%08x invalid or in use, picking 0x%08x", nodeNum, candidate);
        nodeNum = candidate;
    }
    LOG_DEBUG("Use nodenum 0x%08x ", nodeNum);

    myNodeInfo.my_node_num = nodeNum;
}

/** Load a protobuf from a file, return LoadFileResult */
LoadFileResult NodeDB::loadProto(const char *filename, size_t protoSize, size_t objSize, const pb_msgdesc_t *fields,
                                 void *dest_struct)
{
    if (!shouldUseFilesystemPersistence(fsIsMounted())) {
        LOG_ERROR("Filesystem unavailable while loading %s", filename);
        return LoadFileResult::NO_FILESYSTEM;
    }

    LoadFileResult state = LoadFileResult::OTHER_FAILURE;

#ifdef MESHTASTIC_ENCRYPTED_STORAGE
    // check if the file is encrypted and decrypt before protobuf decode
    if (EncryptedStorage::isEncrypted(filename)) {
        // ZeroizingArrayPtr wipes the decrypted plaintext (which contains config
        // secrets - channel PSKs, security private_key, etc.) before delete[],
        // so it isn't recoverable from the heap after this function returns.
        auto decBuf = meshtastic_security::make_zeroizing_array(protoSize);
        if (!decBuf) {
            LOG_ERROR("OOM decrypting %s", filename);
            storageCorruptThisLoad = true;
            return LoadFileResult::OTHER_FAILURE;
        }
        size_t decLen = 0;
        if (EncryptedStorage::readAndDecrypt(filename, decBuf.get(), protoSize, decLen)) {
            LOG_INFO("Load encrypted %s", filename);
            pb_istream_t stream = pb_istream_from_buffer(decBuf.get(), decLen);
            if (fields != &meshtastic_NodeDatabase_msg)
                memset(dest_struct, 0, objSize);
            if (!pb_decode(&stream, fields, dest_struct)) {
                LOG_ERROR("Can't decode protobuf %s", PB_GET_ERROR(&stream));
                state = LoadFileResult::DECODE_FAILED;
                storageCorruptThisLoad = true;
            } else {
                LOG_INFO("Loaded encrypted %s", filename);
                state = LoadFileResult::LOAD_SUCCESS;
            }
        } else {
            LOG_ERROR("Decrypt failed for %s, treating as corrupt", filename);
            state = LoadFileResult::DECODE_FAILED;
            storageCorruptThisLoad = true;
        }
        return state;
    }
#endif

#ifdef FSCom
    concurrency::LockGuard g(spiLock);

    if (!FSCom.exists(filename)) {
        LOG_INFO("File not found: %s", filename);
        return LoadFileResult::NOT_FOUND;
    }

    auto f = FSCom.open(filename, FILE_O_READ);

    if (f) {
        LOG_INFO("Load %s", filename);
        pb_istream_t stream = {&readcb, &f, protoSize};
        if (fields != &meshtastic_NodeDatabase_msg &&
            fields != &meshtastic_NodeDatabase_Legacy_msg) // both NodeDatabase descriptors contain std::vector members
            memset(dest_struct, 0, objSize);
        if (!pb_decode(&stream, fields, dest_struct)) {
            LOG_ERROR("Can't decode protobuf %s", PB_GET_ERROR(&stream));
            state = LoadFileResult::DECODE_FAILED;
        } else {
            LOG_INFO("Loaded %s", filename);
            state = LoadFileResult::LOAD_SUCCESS;
        }
        f.close();
    } else {
        LOG_ERROR("Can't open/read %s", filename);
    }
#else
    LOG_ERROR("Filesystem not implemented");
    state = LoadFileResult::NO_FILESYSTEM;
#endif
    return state;
}

#if WARM_NODE_COUNT > 0
void NodeDB::demoteOldestHotNodesToWarm()
{
    const int keep = MAX_NUM_NODES;
    if (numMeshNodes <= keep)
        return;

    // Protected nodes (favorite/ignored/verified) outrank recency and are demoted
    // only when the store is full of them; within a class, most-recently-heard
    // wins. Index 0 is self and stays put (sort from +1), as in runtime eviction.
    std::sort(meshNodes->begin() + 1, meshNodes->begin() + numMeshNodes,
              [](const meshtastic_NodeInfoLite &a, const meshtastic_NodeInfoLite &b) {
                  const bool ka = nodeInfoLiteIsProtected(&a);
                  const bool kb = nodeInfoLiteIsProtected(&b);
                  if (ka != kb)
                      return ka;
                  return a.last_heard > b.last_heard;
              });

    int demoted = 0;
    for (int i = keep; i < numMeshNodes; i++) {
        const meshtastic_NodeInfoLite &n = (*meshNodes)[i];
        if (n.num == 0)
            continue;
        // Warm entries carry no key length, so a partial key would be indistinguishable
        // from a full one. nullptr keeps the keyless placeholder that restores last_heard.
        warmStore.absorb(n.num, n.last_heard, n.public_key.size == 32 ? n.public_key.bytes : nullptr, n.role,
                         warmProtectedCategory(n), nodeInfoLiteHasXeddsaSigned(&n));
        // Demotion drops the node from the header table, so drop its satellites
        // too (the eviction chokepoint) - they'd otherwise orphan until the next
        // enforceSatelliteCaps pass.
        eraseNodeSatellites(n.num);
        demoted++;
    }
    numMeshNodes = keep; // the resize() in loadFromDisk reclaims the demoted tail
    LOG_MIGRATION("NodeDB migration: demoted %d node(s) over %d into the warm tier (keepers preferred)", demoted, keep);
}
#endif

void NodeDB::nodeDBSelfCare(bool persistRepair)
{
    if (!meshNodes)
        return;

    const NodeNum self = getNodeNum();
    bool nodesOverCap = numMeshNodes > MAX_NUM_NODES;
    bool selfHealed = false;

    // Confirm self is present and its key matches what we just (re)derived. A
    // non-empty DB that doesn't contain us means a foreign/over-cap or corrupt
    // nodes.proto was loaded; an empty DB is just a fresh device (no warning).
    meshtastic_NodeInfoLite *selfNode = getMeshNode(self);
    if (!selfNode && numMeshNodes > 0) {
        LOG_WARN("NodeDB self-care: self 0x%08x absent from DB, re-adding", (unsigned)self);
    } else if (selfNode && owner.public_key.size == 32 && selfNode->public_key.size == 32 &&
               memcmp(selfNode->public_key.bytes, owner.public_key.bytes, 32) != 0) {
        LOG_WARN("NodeDB self-care: self 0x%08x key mismatch, refreshing", (unsigned)self);
    }

    // Self has absolute priority over every remote, including a malformed
    // legacy database filled entirely with protected remotes. Append one
    // temporary overflow slot directly (getOrCreateMeshNode correctly refuses
    // to evict protected nodes), pin it at index zero, then let the normal warm
    // demotion logic choose the remote that leaves the hot tier.
    if (!selfNode) {
        if (static_cast<size_t>(numMeshNodes) >= meshNodes->size())
            meshNodes->resize(static_cast<size_t>(numMeshNodes) + 1);
        selfNode = &meshNodes->at(numMeshNodes++);
        *selfNode = meshtastic_NodeInfoLite_init_default;
        selfNode->num = self;
        selfHealed = true;
        nodesOverCap = numMeshNodes > MAX_NUM_NODES;
    }

    // Maintenance that must never touch self. Pin self to index 0 first so
    // the positional demote/eviction scans (which skip index 0) provably exclude
    // us, wherever the loaded file happened to place our row.
    if (selfNode && numMeshNodes > 0 && selfNode != &meshNodes->at(0)) {
        std::swap(meshNodes->at(0), *selfNode);
        selfHealed = true;
    }

#if WARM_NODE_COUNT > 0
    if (nodesOverCap)
        demoteOldestHotNodesToWarm(); // demotes oldest NON-self overflow; index 0 (us) left in place
#endif

    if (numMeshNodes > MAX_NUM_NODES) {
        LOG_WARN("NodeDB self-care: %d over cap %d, truncating", numMeshNodes, MAX_NUM_NODES);
        numMeshNodes = MAX_NUM_NODES;
    }
    // Normalise the backing store to the hot cap so getOrCreateMeshNode always
    // has spare slots to append into (it indexes meshNodes->at(numMeshNodes++)).
    meshNodes->resize(MAX_NUM_NODES);
    memaudit::set("nodedb", MAX_NUM_NODES * sizeof(meshtastic_NodeInfoLite));

    const bool satsTrimmed = enforceSatelliteCaps();

    // Ensure self exists, sits at index 0, and carries current owner info - after
    // any demotion has freed a slot. Covers the foreign/fixture case where the
    // loaded file did not contain us at all.
    meshtastic_NodeInfoLite *info = getMeshNode(self);
    if (info) {
        if (owner.public_key.size == 32 &&
            (info->public_key.size != 32 || memcmp(info->public_key.bytes, owner.public_key.bytes, 32) != 0))
            selfHealed = true;
        TypeConversions::CopyUserToNodeInfoLite(info, owner);
        if (info != &meshNodes->at(0))
            std::swap(meshNodes->at(0), *info);
    }

    // One-shot rewrite: only when we healed something, and never while storage
    // is locked - a locked boot loads placeholder defaults that must not be written
    // over the encrypted store; reloadFromDisk() re-runs self-care once unlocked.
#ifdef MESHTASTIC_ENCRYPTED_STORAGE
    const bool storageLocked = EncryptedStorage::isLockdownActive() && !EncryptedStorage::isUnlocked();
#else
    const bool storageLocked = false;
#endif

    if (persistRepair && (nodesOverCap || satsTrimmed || selfHealed) && !storageLocked) {
        LOG_MIGRATION("NodeDB self-care: healed store (nodes-over-cap:%s sats-trimmed:%s self:%s); rewriting nodes.proto once",
                      nodesOverCap ? "yes" : "no", satsTrimmed ? "yes" : "no", selfHealed ? "yes" : "no");
        const bool nodeDatabaseSaved = saveNodeDatabaseToDisk();
#if WARM_NODE_COUNT > 0 && !defined(HELTEC_V4_OLED)
        // Heltec commits the hot and warm tiers together inside
        // saveNodeDatabaseToDisk(); do not flush warm.dat a second time after
        // a rejected/failed hot-generation save.
        if (nodeDatabaseSaved)
            warmStore.saveIfDirty();
#else
        (void)nodeDatabaseSaved;
#endif
    }
}

void NodeDB::loadFromDisk()
{
    bootInitializationInProgress = true;
    bootDeferredPreferenceSegments = 0;
    struct BootPreferenceScanGuard {
        bool &active;
        ~BootPreferenceScanGuard() { active = false; }
    } bootPreferenceScanGuard{bootInitializationInProgress};

    // Mark the current device state as completely unusable, so that if we fail reading the entire file from
    // disk we will still factoryReset to restore things.
    devicestate.version = 0;
#ifdef MESHTASTIC_ENCRYPTED_STORAGE
    // Reset the per-load decrypt-failure tracker. Set by loadProto on any
    // encrypted file that fails to decrypt or proto-decode; consumed by
    // reloadFromDisk to surface storage corruption to the operator instead
    // of silently falling back to defaults.
    storageCorruptThisLoad = false;
    encryptedStorageLockedPlaceholder = false;
#endif

    migrationSavePending = false;
    legacyPreferencesPendingCleanup = false;
    incompleteLegacyMigrationDetected = false;
    configDecodeFailed = false;
    unreadablePreferenceSegments = 0;
    incompleteConfigResetDetected = false;
    incompletePreferenceRestoreDetected = false;
    incompleteNodeDatabaseResetDetected = false;
    configLoadComplete = false;

    if (!shouldUseFilesystemPersistence(fsIsMounted())) {
        // Keep the on-disk identity and preferences untouched after a transient mount failure. In-memory
        // defaults leave BLE available for recovery, while UNSET prevents any radio traffic under a
        // provisional identity. A later clean boot gets a fresh mount attempt and can load the real prefs.
        LOG_ERROR("NodeDB: filesystem unavailable - boot degraded without persistence or radio");
        configDecodeFailed = true;
        unreadablePreferenceSegments =
            SEGMENT_CONFIG | SEGMENT_MODULECONFIG | SEGMENT_DEVICESTATE | SEGMENT_CHANNELS | SEGMENT_NODEDATABASE;
        installDefaultNodeDatabase();
        installDefaultDeviceState();
        installDefaultConfig(false, false);
        installDefaultModuleConfig();
        installDefaultChannels();
        forceHeltecLocalRecoveryConfiguration();
        configLoadComplete = true;
        return;
    }

    // A missing file is a valid first-boot condition only when no core
    // generation exists at all. If any peer file (or an interrupted SafeFile
    // temporary) remains, defaulting a missing config/channel could silently
    // replace identity or transmit on the public default PSK.
    bool persistedCoreGenerationPresent = false;
#if defined(HELTEC_V4_OLED) && defined(FSCom)
    bool nodeDatabaseMissingFromPersistedGeneration = false;
#endif
#if USERPREFS_EVENT_MODE
    // Kept through the later config/channel reads. A clean first event boot
    // legitimately has neither active event file; all other missing-member
    // combinations are a torn generation and remain fail-closed.
    bool eventProfileFirstUse = false;
#endif
#if defined(HELTEC_V4_OLED) && defined(FSCom)
    {
        concurrency::LockGuard guard(spiLock);
        const auto activeFileExists = [](const char *path) { return FSCom.exists(path); };
        const auto generationEvidenceExists = [&activeFileExists](const char *path) {
            String temporaryPath = path;
            temporaryPath += ".tmp";
            return activeFileExists(path) || FSCom.exists(temporaryPath.c_str());
        };

        bool configPresent = activeFileExists(configFileName);
        bool channelsPresent = activeFileExists(channelFileName);
#if USERPREFS_EVENT_MODE
        String eventConfigTemporary = configFileName;
        eventConfigTemporary += ".tmp";
        String eventChannelsTemporary = channelFileName;
        eventChannelsTemporary += ".tmp";
        String eventBackupTemporary = backupFileName;
        eventBackupTemporary += ".tmp";
        eventProfileFirstUse = isCleanEventProfileFirstUse(
            configPresent, channelsPresent, activeFileExists(backupFileName), activeFileExists(eventConfigTemporary.c_str()),
            activeFileExists(eventChannelsTemporary.c_str()), activeFileExists(eventBackupTemporary.c_str()),
            activeFileExists(STANDARD_CONFIG_FILE_NAME));

        // First use of an event build deliberately seeds config from the
        // standard profile and creates fresh event channels. Treat the pair as
        // absent together only in that exact clean state. Either member (or a
        // .tmp) on its own is evidence of a torn event generation and must
        // remain fail-closed.
        if (eventProfileFirstUse) {
            configPresent = true;
            channelsPresent = true;
        }
#endif
        const bool moduleConfigPresent = activeFileExists(moduleConfigFileName);
        const bool deviceStatePresent = activeFileExists(deviceStateFileName);
        const bool nodeDatabasePresent = activeFileExists(nodeDatabaseFileName);

        // An interrupted SafeFile temporary proves this is not a pristine
        // first boot, but it is not an active segment and cannot satisfy the
        // per-file completeness check below.
        persistedCoreGenerationPresent =
            generationEvidenceExists(configFileName) || generationEvidenceExists(moduleConfigFileName) ||
            generationEvidenceExists(deviceStateFileName) || generationEvidenceExists(channelFileName) ||
            generationEvidenceExists(nodeDatabaseFileName) || generationEvidenceExists(backupFileName);
#if USERPREFS_EVENT_MODE
        persistedCoreGenerationPresent = persistedCoreGenerationPresent || generationEvidenceExists(STANDARD_CONFIG_FILE_NAME);
#endif

        // nodes.proto is deliberately absent on a keyless, unlicensed device.
        // Record its absence now, but decide whether it is required only after
        // DeviceState and config have been decoded. The other four core files
        // are unconditionally required and establish the early write fence.
        nodeDatabaseMissingFromPersistedGeneration = persistedCoreGenerationPresent && !nodeDatabasePresent;

        if (persistedCoreGenerationPresent) {
            if (!configPresent)
                unreadablePreferenceSegments |= SEGMENT_CONFIG;
            if (!moduleConfigPresent)
                unreadablePreferenceSegments |= SEGMENT_MODULECONFIG;
            if (!deviceStatePresent)
                unreadablePreferenceSegments |= SEGMENT_DEVICESTATE;
            if (!channelsPresent)
                unreadablePreferenceSegments |= SEGMENT_CHANNELS;
        }
    }
    if (unreadablePreferenceSegments != 0) {
        // Establish the write fence before loading any individual segment.
        // Several migration paths save earlier segments while later files have
        // not yet been read; without this early inventory a late missing file
        // could still allow a partial generation to be overwritten at boot.
        configDecodeFailed = true;
        LOG_ERROR("Partial preference generation detected before load - automatic core writes disabled");
    }
#endif

#if defined(HELTEC_V4_OLED) && defined(FSCom)
    const HeltecResetPendingKind pendingReset =
        heltecNvsRebuildPendingFileExists() ? HeltecResetPendingKind::FULL_OR_UNKNOWN : readHeltecResetPendingMarker();
    if (pendingReset == HeltecResetPendingKind::EDIT) {
        // A settings import or local multi-segment edit was interrupted. Some
        // files may belong to each generation, so keep radio/GPS silent and
        // expose only local restore/reset/OTA recovery.
        incompleteConfigResetDetected = true;
        configDecodeFailed = true;
        LOG_ERROR("Incomplete settings commit detected - local recovery only");
    } else if (pendingReset == HeltecResetPendingKind::NODEDB_RESET) {
        // Config/identity files remain authoritative, but the hot/warm node
        // stores and DeviceState may span generations. Keep radio/GPS silent
        // and permit only an explicit node-db reset retry (or full reset).
        incompleteNodeDatabaseResetDetected = true;
        LOG_ERROR("Incomplete node database reset detected - retry locally");
    } else if (pendingReset == HeltecResetPendingKind::CONFIG_ONLY) {
        // Load and validate the preserved identity so the operator can safely
        // retry config-only reset. All automatic core writes remain blocked,
        // and the radio is forced silent at the end of this load.
        incompleteConfigResetDetected = true;
        configDecodeFailed = true;
        LOG_ERROR("Incomplete config reset detected - local recovery only");
    } else if (pendingReset == HeltecResetPendingKind::RESTORE) {
        // A verified backup remains available outside /prefs. Load the current
        // files only to expose diagnostics, keep all automatic writes/radio
        // activity blocked, and allow an explicit full restore to retry the
        // same transaction through the local BLE/USB recovery channel.
        incompleteConfigResetDetected = true;
        incompletePreferenceRestoreDetected = true;
        configDecodeFailed = true;
        LOG_ERROR("Incomplete preferences restore detected - retry restore locally");
    } else if (pendingReset == HeltecResetPendingKind::FULL || pendingReset == HeltecResetPendingKind::FULL_OR_UNKNOWN) {
        // A prior multi-file reset did not reach its final commit point. Never
        // accept an arbitrary mix of old and new generations as authoritative.
        // Keep local BLE/USB recovery available while radio and GPS stay off.
        LOG_ERROR("Incomplete factory reset detected - boot degraded for local recovery");
        configDecodeFailed = true;
        unreadablePreferenceSegments =
            SEGMENT_CONFIG | SEGMENT_MODULECONFIG | SEGMENT_DEVICESTATE | SEGMENT_CHANNELS | SEGMENT_NODEDATABASE;
        installDefaultNodeDatabase();
        installDefaultDeviceState();
        installDefaultConfig(false, false);
        installDefaultModuleConfig();
        installDefaultChannels();
        forceHeltecLocalRecoveryConfiguration();
        configLoadComplete = true;
        return;
    }
#endif

    // Standard and event radio profiles are deliberately isolated. Keep the
    // inactive event generation when booting standard firmware, just as event
    // firmware keeps the standard generation; deleting it here could destroy
    // the only valid event profile during a locked or degraded boot.

#if USERPREFS_EVENT_MODE
    // Seed only a missing event config; never overwrite normal files after a corrupt event config.
    bool eventConfigMissing = false;
    eventProfileStorageUnavailable = false;
#ifdef FSCom
    spiLock->lock();
    eventConfigMissing = !FSCom.exists(configFileName);
    if (eventConfigMissing) {
        const size_t totalBytes = fsTotalBytes();
        const size_t usedBytes = fsUsedBytes();
        eventProfileStorageUnavailable = !hasEventProfileStorageSpace(totalBytes, usedBytes);
        if (eventProfileStorageUnavailable) {
            LOG_ERROR("Event profile needs %u bytes free; only %u available. Changes won't persist",
                      static_cast<unsigned>(EVENT_PROFILE_STORAGE_RESERVATION_BYTES),
                      static_cast<unsigned>(totalBytes >= usedBytes ? totalBytes - usedBytes : 0));
        }
    }
    spiLock->unlock();
#endif
    bool initializedEventConfig = false;
#endif

    meshtastic_Config_SecurityConfig backupSecurity = meshtastic_Config_SecurityConfig_init_zero;

#ifdef ARCH_ESP32
    spiLock->lock();
    // If the legacy deviceState exists, start over with a factory reset
    if (FSCom.exists("/static/static"))
        rmDir("/static/static"); // Remove bad static web files bundle from initial 2.5.13 release
    spiLock->unlock();
#endif

#ifdef FSCom
#if defined(FACTORY_INSTALL) && !defined(ARCH_PORTDUINO) && !defined(HELTEC_V4_OLED)
    spiLock->lock();
    if (!FSCom.exists("/prefs/" xstr(BUILD_EPOCH))) {
        LOG_WARN("Factory Install Reset");
        rmDir("/prefs");
        FSCom.mkdir("/prefs");
        File f2 = FSCom.open("/prefs/" xstr(BUILD_EPOCH), FILE_O_WRITE);
        if (f2) {
            f2.flush();
            f2.close();
        }
    }
    spiLock->unlock();
#endif // FACTORY_INSTALL, not PORTDUINO, and not HELTEC_V4_OLED
    spiLock->lock();
    if (FSCom.exists(legacyPrefFileName)) {
        const char *legacyIdentityConfigFileName = configFileName;
        bool legacyConfigPresent = FSCom.exists(legacyIdentityConfigFileName);
#if USERPREFS_EVENT_MODE
        if (!legacyConfigPresent && FSCom.exists(STANDARD_CONFIG_FILE_NAME)) {
            legacyIdentityConfigFileName = STANDARD_CONFIG_FILE_NAME;
            legacyConfigPresent = true;
        }
#endif
        spiLock->unlock();
        LOG_WARN("Legacy preferences detected");
        if (legacyConfigPresent) {
            const LoadFileResult legacyConfigState =
                loadProto(legacyIdentityConfigFileName, meshtastic_LocalConfig_size, sizeof(meshtastic_LocalConfig),
                          &meshtastic_LocalConfig_msg, &config);
            if (legacyConfigState == LoadFileResult::LOAD_SUCCESS) {
                if (config.has_security && config.security.private_key.size == 32) {
                    LOG_DEBUG("Backup security config and keys");
                    backupSecurity = config.security;
                } else if (config.has_security &&
                           (config.security.private_key.size != 0 || config.security.public_key.size != 0)) {
#if defined(HELTEC_V4_OLED)
                    configDecodeFailed = true;
                    unreadablePreferenceSegments |= SEGMENT_CONFIG;
                    LOG_ERROR("Legacy private key length is invalid; preserving files for recovery");
#endif
                }
#if defined(HELTEC_V4_OLED)
            } else {
                // The legacy file still exists, so a read/decode failure may
                // be transient. Never replace a potentially recoverable key
                // with defaults or remove the migration marker this boot.
                configDecodeFailed = true;
                unreadablePreferenceSegments |= SEGMENT_CONFIG;
                LOG_ERROR("Legacy config unavailable; preserving identity files for retry");
#endif
            }
        }
        if (shouldAutoEraseLegacyPreferences()) {
            spiLock->lock();
            rmDir("/prefs");
            spiLock->unlock();
        } else {
            // Keep every old file until current core files have been written
            // successfully later in the constructor. Only the obsolete marker
            // is removed at that commit point.
            legacyPreferencesPendingCleanup = true;
            LOG_WARN("Legacy preferences found; staging non-destructive migration");
        }
    } else {
        spiLock->unlock();
    }

#endif // FSCom

#ifdef MESHTASTIC_ENCRYPTED_STORAGE
    // Only take the locked-boot defaults path when lockdown is ACTIVE (the
    // device is provisioned) AND storage is still locked. A lockdown-capable
    // build that has never been provisioned - or that was disabled - falls
    // through to the normal plaintext load below and behaves like stock.
    if (EncryptedStorage::isLockdownActive() && !EncryptedStorage::isUnlocked()) {
        // Encrypted storage is locked. Install defaults and wait for the
        // passphrase over BLE/serial; PhoneAPI::handleLockdownAuthInline
        // calls reloadFromDisk() once the storage is unlocked.
        LOG_WARN("NodeDB: Encrypted storage locked, default config until unlocked");
        installDefaultNodeDatabase();
        installDefaultDeviceState();
        installDefaultConfig(false, false);
        installDefaultModuleConfig();
        installDefaultChannels();

        // Hold the radio silent until the operator unlocks. installDefaultConfig
        // would otherwise honour USERPREFS_CONFIG_LORA_REGION (the common shape
        // for managed deployments) and the LongFast default channel synthesised
        // by installDefaultChannels, so the device would beacon nodeinfo /
        // telemetry on the public default PSK before any unlock - and process
        // incoming default-channel packets the same way. Forcing region=UNSET
        // gates both TX and RX in RadioLibInterface (see the region==UNSET
        // checks in startSend and readData); tx_enabled=false is belt-and-
        // suspenders for any code path that does not consult region directly.
        // reloadFromDisk() restores the persisted lora config when the
        // operator unlocks.
        config.lora.region = meshtastic_Config_LoRaConfig_RegionCode_UNSET;
        config.lora.tx_enabled = false;
        // Do not mint and expose a throw-away PKI identity in the remainder of
        // the constructor. reloadFromDisk() restores the persisted key into
        // CryptoEngine after the operator unlocks storage.
        encryptedStorageLockedPlaceholder = true;
        return;
    }
#endif

#if defined(HELTEC_V4_OLED) && defined(FSCom)
    if (legacyPreferencesPendingCleanup) {
        // Preserve the historical migration semantics (legacy marker means
        // factory defaults) without deleting the only durable generation at
        // boot. Build every replacement in RAM, retain the cryptographic
        // identity, then let the constructor atomically save and verify all
        // current core files before it removes only the legacy marker.
        installDefaultNodeDatabase();
        installDefaultDeviceState();
        installDefaultConfig(false, false);
        installDefaultModuleConfig();
        installDefaultChannels();
        const bool legacyIdentityUnavailable = (unreadablePreferenceSegments & SEGMENT_CONFIG) != 0;
        if (legacyIdentityUnavailable) {
            config.lora.region = meshtastic_Config_LoRaConfig_RegionCode_UNSET;
            config.lora.tx_enabled = false;
            configLoadComplete = true;
            return;
        }
        if (backupSecurity.private_key.size == 32) {
            config.has_security = true;
            config.security = backupSecurity;
        }
        if (incompleteConfigResetDetected) {
            configDecodeFailed = true;
            forceHeltecLocalRecoveryConfiguration();
        }
        configLoadComplete = true;
        return;
    }
#endif

    // Arm the direct-into-map decode so satellite entries skip the temp vectors.
    {
        concurrency::LockGuard guard(&satelliteMutex);
        armNodeDatabaseDecodeTargets();
    }
    struct Disarm {
        NodeDB &self;
        ~Disarm() { self.disarmNodeDatabaseDecodeTargets(); }
    } disarm{*this};

    // Avoid push_back's power-of-2 capacity growth wasting RAM at small N.
    // loadProto() deliberately does not memset this C++ object because it owns
    // vectors; clear every repeated field explicitly instead. This is vital
    // for runtime encrypted-storage reloads, where the locked placeholder has
    // already sized nodes to MAX_NUM_NODES and the nanopb callback appends.
    nodeDatabase.version = 0;
    nodeDatabase.nodes.clear();
    nodeDatabase.positions.clear();
    nodeDatabase.telemetry.clear();
    nodeDatabase.status.clear();
    nodeDatabase.environment.clear();
    nodeDatabase.nodes.reserve(MAX_NUM_NODES);

    auto state = loadProto(nodeDatabaseFileName, getMaxNodesAllocatedSize(), sizeof(meshtastic_NodeDatabase),
                           &meshtastic_NodeDatabase_msg, &nodeDatabase);
#if defined(HELTEC_V4_OLED)
    const bool nodeDatabaseVersionTooNew = state == LoadFileResult::LOAD_SUCCESS && nodeDatabase.version > DEVICESTATE_CUR_VER;
#else
    const bool nodeDatabaseVersionTooNew = false;
#endif
    if (state == LoadFileResult::DECODE_FAILED || state == LoadFileResult::OTHER_FAILURE) {
        unreadablePreferenceSegments |= SEGMENT_NODEDATABASE;
    }
#if defined(HELTEC_V4_OLED)
    if (nodeDatabaseVersionTooNew) {
        unreadablePreferenceSegments |= SEGMENT_NODEDATABASE;
        LOG_ERROR("Node database version %u is newer than supported %u; preserve file without downgrade",
                  static_cast<unsigned>(nodeDatabase.version), static_cast<unsigned>(DEVICESTATE_CUR_VER));
    }
#endif
    if (state != LoadFileResult::LOAD_SUCCESS || nodeDatabase.version < DEVICESTATE_MIN_VER || nodeDatabaseVersionTooNew) {
        LOG_WARN("NodeDatabase %d is old, discard", nodeDatabase.version);
        installDefaultNodeDatabase();
    } else if (nodeDatabase.version < DEVICESTATE_CUR_VER) {
        if (migrateLegacyNodeDatabase())
            migrationSavePending = true;
        else
            installDefaultNodeDatabase();
    } else {
        meshNodes = &nodeDatabase.nodes;
        numMeshNodes = nodeDatabase.nodes.size();
        // Counts computed outside LOG_INFO() so cppcheck doesn't choke on #if in macro args.
        const unsigned posCount =
#if !MESHTASTIC_EXCLUDE_POSITIONDB
            (unsigned)nodePositions.size();
#else
            0u;
#endif

        const unsigned telCount =
#if !MESHTASTIC_EXCLUDE_TELEMETRYDB
            (unsigned)nodeTelemetry.size();
#else
            0u;
#endif

        const unsigned envCount =
#if !MESHTASTIC_EXCLUDE_ENVIRONMENTDB
            (unsigned)nodeEnvironment.size();
#else
            0u;
#endif

        const unsigned statusCount =
#if !MESHTASTIC_EXCLUDE_STATUSDB
            (unsigned)nodeStatus.size();
#else
            0u;
#endif

        LOG_INFO("Loaded saved nodedatabase v%d: %d nodes, %u pos, %u tel, %u env, %u status", nodeDatabase.version,
                 nodeDatabase.nodes.size(), posCount, telCount, envCount, statusCount);
    }

    // Left UNTRIMMED on purpose: trim/demote/satellite-cap/self-pin/rewrite all
    // run in nodeDBSelfCare() once getNodeNum() is valid (still 0 here on a cold
    // boot, so we could only assume index 0 == self - the very bug being fixed).
#if WARM_NODE_COUNT > 0
    // Load the warm tier so its on-disk snapshot is available before the node DB
    // is exercised (and before nodeDBSelfCare() demotes any overflow into it).
    warmStore.load();
#endif

    // static DeviceState scratch; We no longer read into a tempbuf because this structure is 15KB of valuable RAM
    state = loadProto(deviceStateFileName, meshtastic_DeviceState_size, sizeof(meshtastic_DeviceState),
                      &meshtastic_DeviceState_msg, &devicestate);

#if defined(HELTEC_V4_OLED)
    const bool deviceStateVersionTooNew = state == LoadFileResult::LOAD_SUCCESS && devicestate.version > DEVICESTATE_CUR_VER;
#else
    const bool deviceStateVersionTooNew = false;
#endif

    if (state == LoadFileResult::DECODE_FAILED || state == LoadFileResult::OTHER_FAILURE) {
        unreadablePreferenceSegments |= SEGMENT_DEVICESTATE;
    }
#if defined(HELTEC_V4_OLED)
    if (deviceStateVersionTooNew) {
        unreadablePreferenceSegments |= SEGMENT_DEVICESTATE;
        configDecodeFailed = true;
        LOG_ERROR("Device state version %u is newer than supported %u; preserve identity for newer firmware",
                  static_cast<unsigned>(devicestate.version), static_cast<unsigned>(DEVICESTATE_CUR_VER));
    }
#endif
#if defined(HELTEC_V4_OLED)
    if (state == LoadFileResult::NOT_FOUND && persistedCoreGenerationPresent) {
        unreadablePreferenceSegments |= SEGMENT_DEVICESTATE;
        configDecodeFailed = true;
        LOG_ERROR("Partial preference generation: device state is missing; preserving remaining files for recovery");
    }
#endif

    // See https://github.com/meshtastic/firmware/issues/4184#issuecomment-2269390786
    // It is very important to try and use the saved prefs even if we fail to read meshtastic_DeviceState.  Because most of our
    // critical config may still be valid (in the other files - loaded next).
    // Also, if we did fail on reading we probably failed on the enormous (and non critical) nodeDB.  So DO NOT install default
    // device state.
    // if (state != LoadFileResult::LOAD_SUCCESS) {
    //    installDefaultDeviceState(); // Our in RAM copy might now be corrupt
    //} else {
    if ((state != LoadFileResult::LOAD_SUCCESS) || (devicestate.version < DEVICESTATE_MIN_VER) || deviceStateVersionTooNew) {
        LOG_WARN("Devicestate %d is old or invalid, discard", devicestate.version);
        installDefaultDeviceState();

        // Attempt recovery of owner fields from our own NodeDB entry if available.
        const meshtastic_NodeInfoLite *us = getMeshNode(getNodeNum());
        if (nodeInfoLiteHasUser(us)) {
            LOG_WARN("Restore owner fields (long_name/short_name/is_licensed/is_unmessagable) from NodeDB for node 0x%08x",
                     us->num);
            // owner.long_name (40) is wider than the lite source (25); bound by the source
            memcpy(owner.long_name, us->long_name, sizeof(us->long_name));
            owner.long_name[sizeof(us->long_name) - 1] = '\0';
            memcpy(owner.short_name, us->short_name, sizeof(owner.short_name));
            owner.short_name[sizeof(owner.short_name) - 1] = '\0';
            owner.is_licensed = nodeInfoLiteIsLicensed(us);
            owner.has_is_unmessagable = nodeInfoLiteHasIsUnmessagable(us);
            owner.is_unmessagable = nodeInfoLiteIsUnmessagable(us);

            // Save the recovered owner to device state on disk
            saveToDisk(SEGMENT_DEVICESTATE);
        }
    } else {
        LOG_INFO("Loaded saved devicestate v%d", devicestate.version);
    }

    // Devicestate saved by firmware that allowed 39-byte names gets clamped on
    // first load; from here on owner never carries more than the local cap.
    clampLongName(owner.long_name);

    state = loadProto(configFileName, meshtastic_LocalConfig_size, sizeof(meshtastic_LocalConfig), &meshtastic_LocalConfig_msg,
                      &config);
#if USERPREFS_EVENT_MODE
#if defined(HELTEC_V4_OLED)
    if (canSeedEventProfile(eventProfileFirstUse, !eventProfileStorageUnavailable) && state != LoadFileResult::LOAD_SUCCESS) {
#else
    if (eventConfigMissing && state != LoadFileResult::LOAD_SUCCESS) {
#endif
        const LoadFileResult eventConfigState = state;
        const LoadFileResult standardConfigState =
            loadProto(STANDARD_CONFIG_FILE_NAME, meshtastic_LocalConfig_size, sizeof(meshtastic_LocalConfig),
                      &meshtastic_LocalConfig_msg, &config);
        if (standardConfigState == LoadFileResult::LOAD_SUCCESS) {
            // Preserve the user's identity and non-radio preferences, then
            // replace only LoRa with this event build's compiled defaults.
            const meshtastic_LocalConfig standardConfig = config;
            installDefaultConfig(true);
            const meshtastic_Config_LoRaConfig eventLora = config.lora;
            config = standardConfig;
            config.has_lora = true;
            config.lora = eventLora;
            state = LoadFileResult::LOAD_SUCCESS;
            initializedEventConfig = true;
            LOG_INFO("Init event config without modifying %s", STANDARD_CONFIG_FILE_NAME);
        } else {
            // Keep the event load outcome because loadProto() clears config before decoding.
            // Any present-but-unreadable standard config must not create a replacement identity.
            state = (standardConfigState == LoadFileResult::DECODE_FAILED || standardConfigState == LoadFileResult::OTHER_FAILURE)
                        ? standardConfigState
                        : eventConfigState;
        }
    }
#endif
    const bool loadedValidPrivateKey = config.has_security && config.security.private_key.size == 32;
#if defined(HELTEC_V4_OLED) && defined(FSCom)
    const bool persistedIdentityNeedsNodeDatabase = persistedIdentityRequiresNodeDatabase(
        config.has_security ? config.security.private_key.size : 0, config.has_security ? config.security.public_key.size : 0,
        owner.public_key.size, owner.is_licensed);
#endif
    const bool invalidPrivateKeyLength = state == LoadFileResult::LOAD_SUCCESS && config.has_security &&
                                         config.security.private_key.size != 0 && !loadedValidPrivateKey;
    const bool missingPrivateKeyForKnownIdentity =
        state == LoadFileResult::LOAD_SUCCESS && !loadedValidPrivateKey &&
        (owner.public_key.size != 0 || (config.has_security && config.security.public_key.size != 0));
    bool loadedPublicKeyMismatch = false;
#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)
    if (state == LoadFileResult::LOAD_SUCCESS && loadedValidPrivateKey) {
        uint8_t derivedPublicKey[32];
        loadedPublicKeyMismatch =
            !derivePublicKeyWithoutInstalling(config.security.private_key.bytes, derivedPublicKey) ||
            (config.security.public_key.size != 0 &&
             (config.security.public_key.size != sizeof(derivedPublicKey) ||
              memcmp(config.security.public_key.bytes, derivedPublicKey, sizeof(derivedPublicKey)) != 0)) ||
            (owner.public_key.size != 0 && (owner.public_key.size != sizeof(derivedPublicKey) ||
                                            memcmp(owner.public_key.bytes, derivedPublicKey, sizeof(derivedPublicKey)) != 0));
    }
#endif
    const bool missingConfigForKnownIdentity =
        state == LoadFileResult::NOT_FOUND && (owner.public_key.size != 0 || persistedCoreGenerationPresent);
    const bool invalidLoadedRegionPolicy = state == LoadFileResult::LOAD_SUCCESS && config.has_lora &&
                                           config.lora.region != meshtastic_Config_LoRaConfig_RegionCode_UA_868 &&
                                           !RadioInterface::checkConfigRegion(config.lora);
#if defined(HELTEC_V4_OLED)
    const bool configVersionTooNew = state == LoadFileResult::LOAD_SUCCESS && config.version > DEVICESTATE_CUR_VER;
#else
    const bool configVersionTooNew = false;
#endif
    if (state == LoadFileResult::DECODE_FAILED || state == LoadFileResult::OTHER_FAILURE || invalidPrivateKeyLength ||
        missingPrivateKeyForKnownIdentity || loadedPublicKeyMismatch || missingConfigForKnownIdentity ||
        invalidLoadedRegionPolicy || configVersionTooNew) {
        // Config file present but unreadable this boot (corruption / torn write
        // / transient open, allocation, or decrypt fail). loadProto() already
        // zeroed `config`, so the keypair is gone from RAM; minting a new one
        // would change our NodeNum (== crc32(public_key)) and orphan us on the
        // mesh. configDecodeFailed freezes identity and skips persisting (see
        // ctor), so a transient failure self-heals on the next clean boot. A
        // genuinely absent config returns NOT_FOUND, so this never fires on
        // first boot. Boot degraded + radio-silent.
        LOG_ERROR("Config read/identity/region validation failed - freeze "
                  "identity, boot degraded (radio silent until restored)");
        configDecodeFailed = true;
        unreadablePreferenceSegments |= SEGMENT_CONFIG;
        installDefaultConfig(true, false);
        config.lora.region = meshtastic_Config_LoRaConfig_RegionCode_UNSET;
        config.lora.tx_enabled = false;
    } else if (state != LoadFileResult::LOAD_SUCCESS) {
        // No config generation exists to protect (first boot), or this platform
        // has no filesystem.
        installDefaultConfig();
    } else if (config.version < DEVICESTATE_MIN_VER) {
        LOG_WARN("config %d is old, discard", config.version);
        installDefaultConfig(true, false);
    } else {
        LOG_INFO("Loaded saved config v%d", config.version);
    }
#if defined(HELTEC_V4_OLED) && defined(FSCom)
    if (nodeDatabaseMissingFromPersistedGeneration &&
        persistedIdentityNeedsNodeDatabase) {
        unreadablePreferenceSegments |= SEGMENT_NODEDATABASE;
        configDecodeFailed = true;
        LOG_ERROR("Partial preference generation: identity exists but node "
                  "database is missing; preserving remaining files for "
                  "recovery");
    }
#endif
    configLoadComplete = true;

    // Coerce LoRa config fields derived from presets while bootstrapping.
    // Some clients/UI components display bandwidth/spread_factor directly from config even in preset mode.
    if (config.has_lora && config.lora.use_preset) {
        RadioInterface::clampConfigLora(config.lora);
    }

#if defined(USERPREFS_LORA_TX_DISABLED) && USERPREFS_LORA_TX_DISABLED
    config.lora.tx_enabled = false;
#endif

    // Always-apply LoRa overrides: applied after loading saved config so they
    // take effect even when NVS already has a valid config (e.g. region-locked
    // dev boards with no BLE/serial to set the region at runtime).
#ifdef USERPREFS_CONFIG_LORA_REGION
    // Skip on a degraded boot to keep the radio silent (identity is already protected by the keygen gate).
    if (!configDecodeFailed)
        config.lora.region = USERPREFS_CONFIG_LORA_REGION;
#endif

#ifdef USERPREFS_LORACONFIG_USE_PRESET
    config.lora.use_preset = USERPREFS_LORACONFIG_USE_PRESET;
#endif

#ifdef USERPREFS_LORACONFIG_BANDWIDTH
    config.lora.bandwidth = USERPREFS_LORACONFIG_BANDWIDTH;
#endif

#ifdef USERPREFS_LORACONFIG_SPREAD_FACTOR
    config.lora.spread_factor = USERPREFS_LORACONFIG_SPREAD_FACTOR;
#endif

#ifdef USERPREFS_LORACONFIG_CODING_RATE
    config.lora.coding_rate = USERPREFS_LORACONFIG_CODING_RATE;
#endif

#ifdef USERPREFS_LORACONFIG_OVERRIDE_FREQUENCY
    config.lora.override_frequency = USERPREFS_LORACONFIG_OVERRIDE_FREQUENCY;
#endif

    if (backupSecurity.private_key.size == 32) {
        LOG_DEBUG("Restore security config backup");
        config.security = backupSecurity;
        saveToDisk(SEGMENT_CONFIG);
    }

    // Make sure we load hard coded admin keys even when the configuration file has none.
    // Initialize admin_key_count to zero
    byte numAdminKeys = 0;
#if defined(USERPREFS_USE_ADMIN_KEY_0) || defined(USERPREFS_USE_ADMIN_KEY_1) || defined(USERPREFS_USE_ADMIN_KEY_2)
    uint16_t sum = 0;
#endif

#ifdef USERPREFS_USE_ADMIN_KEY_0

    for (uint8_t b = 0; b < 32; b++) {
        sum += config.security.admin_key[0].bytes[b];
    }
    if (sum == 0) {
        numAdminKeys += 1;
        LOG_INFO("Admin 0 key zero. Load hard coded key from user prefs");
        memcpy(config.security.admin_key[0].bytes, userprefs_admin_key_0, 32);
        config.security.admin_key[0].size = 32;
    }
#endif

#ifdef USERPREFS_USE_ADMIN_KEY_1
    sum = 0;
    for (uint8_t b = 0; b < 32; b++) {
        sum += config.security.admin_key[1].bytes[b];
    }
    if (sum == 0) {
        numAdminKeys += 1;
        LOG_INFO("Admin 1 key zero. Load hard coded key from user prefs");
        memcpy(config.security.admin_key[1].bytes, userprefs_admin_key_1, 32);
        config.security.admin_key[1].size = 32;
    }
#endif

#ifdef USERPREFS_USE_ADMIN_KEY_2
    sum = 0;
    for (uint8_t b = 0; b < 32; b++) {
        sum += config.security.admin_key[2].bytes[b];
    }
    if (sum == 0) {
        numAdminKeys += 1;
        LOG_INFO("Admin 2 key zero. Load hard coded key from user prefs");
        memcpy(config.security.admin_key[2].bytes, userprefs_admin_key_2, 32);
        config.security.admin_key[2].size = 32;
    }
#endif

    if (numAdminKeys > 0) {
        LOG_INFO("Saving %d hard coded admin keys", numAdminKeys);
        config.security.admin_key_count = numAdminKeys;
        saveToDisk(SEGMENT_CONFIG);
    }

    state = loadProto(moduleConfigFileName, meshtastic_LocalModuleConfig_size, sizeof(meshtastic_LocalModuleConfig),
                      &meshtastic_LocalModuleConfig_msg, &moduleConfig);
#if defined(HELTEC_V4_OLED)
    const bool moduleConfigVersionTooNew =
        state == LoadFileResult::LOAD_SUCCESS && moduleConfig.version > POSITION_TELEMETRY_OPTIN_VER;
#else
    const bool moduleConfigVersionTooNew = false;
#endif
    if (state == LoadFileResult::DECODE_FAILED || state == LoadFileResult::OTHER_FAILURE) {
        unreadablePreferenceSegments |= SEGMENT_MODULECONFIG;
    }
#if defined(HELTEC_V4_OLED)
    if (moduleConfigVersionTooNew) {
        unreadablePreferenceSegments |= SEGMENT_MODULECONFIG;
        configDecodeFailed = true;
        LOG_ERROR("Module config version %u is newer than supported %u; preserve file without downgrade",
                  static_cast<unsigned>(moduleConfig.version), static_cast<unsigned>(POSITION_TELEMETRY_OPTIN_VER));
    }
    if (state == LoadFileResult::NOT_FOUND && persistedCoreGenerationPresent) {
        unreadablePreferenceSegments |= SEGMENT_MODULECONFIG;
        configDecodeFailed = true;
        LOG_ERROR("Partial preference generation: module config is missing; preserving remaining files for recovery");
    }
#endif
    if (state != LoadFileResult::LOAD_SUCCESS || moduleConfigVersionTooNew) {
        installDefaultModuleConfig(); // Our in RAM copy might now be corrupt
    } else {
        if (moduleConfig.version < DEVICESTATE_MIN_VER) {
            LOG_WARN("moduleConfig %d is old, discard", moduleConfig.version);
            installDefaultModuleConfig();
        } else {
            LOG_INFO("Loaded saved moduleConfig v%d", moduleConfig.version);
        }
    }

    // Always-on traffic management: a device that has NEVER configured TMM
    // (has_traffic_management false - AdminModule always sets the has_ flag on
    // write, even when disabling) gets the fork defaults. Explicitly configured
    // devices keep their exact settings.
    if (!moduleConfig.has_traffic_management) {
        LOG_INFO("Traffic management never configured, installing always-on defaults");
        installTrafficManagementDefaults(moduleConfig);
        saveToDisk(SEGMENT_MODULECONFIG);
    }

    state = loadProto(channelFileName, meshtastic_ChannelFile_size, sizeof(meshtastic_ChannelFile), &meshtastic_ChannelFile_msg,
                      &channelFile);
#if defined(HELTEC_V4_OLED)
    const bool channelVersionTooNew = state == LoadFileResult::LOAD_SUCCESS && channelFile.version > POSITION_TELEMETRY_OPTIN_VER;
#else
    const bool channelVersionTooNew = false;
#endif
    if (state == LoadFileResult::DECODE_FAILED || state == LoadFileResult::OTHER_FAILURE) {
        // Channel PSKs are part of the radio identity. A present-but-unreadable
        // file may be recoverable on the next clean boot; do not replace it with
        // defaults or permit the radio to use those defaults in the meantime.
        installDefaultChannels();
        configDecodeFailed = true;
        unreadablePreferenceSegments |= SEGMENT_CHANNELS;
        config.lora.region = meshtastic_Config_LoRaConfig_RegionCode_UNSET;
        config.lora.tx_enabled = false;
        LOG_ERROR("Channel profile read failed - boot degraded (radio silent until restored)");
    } else if (channelVersionTooNew) {
        installDefaultChannels();
        configDecodeFailed = true;
        unreadablePreferenceSegments |= SEGMENT_CHANNELS;
        config.lora.region = meshtastic_Config_LoRaConfig_RegionCode_UNSET;
        config.lora.tx_enabled = false;
        LOG_ERROR("Channel version is newer than supported; preserve future profile and disable radio");
    } else if (state != LoadFileResult::LOAD_SUCCESS) {
        installDefaultChannels(); // Safe only on an actually empty first boot.
#if defined(HELTEC_V4_OLED)
        const bool expectedMissingEventChannels =
#if USERPREFS_EVENT_MODE
            canInitializeMissingEventChannels(eventProfileFirstUse, initializedEventConfig, state == LoadFileResult::NOT_FOUND);
#else
            false;
#endif
        if (persistedCoreGenerationPresent && !expectedMissingEventChannels) {
            configDecodeFailed = true;
            unreadablePreferenceSegments |= SEGMENT_CHANNELS;
            config.lora.region = meshtastic_Config_LoRaConfig_RegionCode_UNSET;
            config.lora.tx_enabled = false;
            LOG_ERROR("Partial preference generation: channels are missing - radio disabled for local recovery");
        }
#endif
    } else {
#if defined(HELTEC_V4_OLED)
        if (!isCompleteChannelFile(channelFile)) {
            // A syntactically valid but truncated channel file is still an
            // identity/radio-profile failure. Do not let resetRadioConfig()
            // silently install LongFast and transmit with the retained region.
            installDefaultChannels();
            configDecodeFailed = true;
            unreadablePreferenceSegments |= SEGMENT_CHANNELS;
            config.lora.region = meshtastic_Config_LoRaConfig_RegionCode_UNSET;
            config.lora.tx_enabled = false;
            LOG_ERROR("Channel profile is incomplete - boot degraded (radio silent until restored)");
        } else
#endif
            if (channelFile.version < DEVICESTATE_MIN_VER) {
            LOG_WARN("channelFile %d is old, discard", channelFile.version);
            installDefaultChannels();
        } else {
            LOG_INFO("Loaded saved channelFile v%d", channelFile.version);
        }
    }

#if USERPREFS_EVENT_MODE
    const int eventProfileInitialSave = eventProfileFirstUseSaveSegments(initializedEventConfig, configDecodeFailed);
    if (eventProfileInitialSave != 0) {
        // Publish the two active event-profile members as one deferred boot
        // generation. Persisting config alone makes the next boot see a torn
        // event profile, while relying on the channel version migration leaves
        // a current-version default channel entirely unwritten.
        if (!saveToDisk(eventProfileInitialSave))
            LOG_ERROR("Can't persist initial event radio profile");
    }
#endif

    state = loadProto(uiconfigFileName, meshtastic_DeviceUIConfig_size, sizeof(meshtastic_DeviceUIConfig),
                      &meshtastic_DeviceUIConfig_msg, &uiconfig);
    if (state == LoadFileResult::LOAD_SUCCESS) {
        LOG_INFO("Loaded UIConfig");
    }

#ifdef MESHTASTIC_ENCRYPTED_STORAGE
    // Ensure all config segments are persisted to encrypted storage.
    // installDefaultConfig/installDefaultModuleConfig only set in-memory structs
    // without saving to disk, so we force a save here to ensure encrypted files exist.
    //
    // Only when lockdown is ACTIVE. A capable-but-off device must leave its
    // files as plaintext - encryptAndWrite would fail anyway (no DEK), but
    // skipping the whole block avoids the wasted attempts and error logs.
    if (EncryptedStorage::isLockdownActive()) {
        const char *filesToCheck[] = {configFileName, moduleConfigFileName, channelFileName, deviceStateFileName,
                                      nodeDatabaseFileName};
        const int segments[] = {SEGMENT_CONFIG, SEGMENT_MODULECONFIG, SEGMENT_CHANNELS, SEGMENT_DEVICESTATE,
                                SEGMENT_NODEDATABASE};
        int toSave = 0;
        for (size_t i = 0; i < sizeof(segments) / sizeof(segments[0]); i++) {
            if (!EncryptedStorage::isEncrypted(filesToCheck[i])) {
                toSave |= segments[i];
            }
        }
        if (toSave) {
            LOG_INFO("Lockdown: Saving unencrypted segments to encrypted storage (mask=0x%x)", toSave);
            saveToDisk(toSave);
        }

        // Migrate any remaining plaintext proto files (from standard firmware upgrade)
        for (const char *fn : filesToCheck) {
            bool exists = false;
#ifdef FSCom
            {
                concurrency::LockGuard guard(spiLock);
                exists = FSCom.exists(fn);
            }
#endif
            if (exists && !EncryptedStorage::isEncrypted(fn)) {
                LOG_INFO("Migrating %s to encrypted storage", fn);
                if (!EncryptedStorage::migrateFile(fn)) {
                    LOG_ERROR("Can't migrate %s to encrypted storage", fn);
                    storageCorruptThisLoad = true;
                }
            }
        }

        // Backups are outside saveToDisk(), but can contain radio profile PSKs. Only event builds
        // introduce a second backup file, so leave normal-firmware backup handling unchanged.
#if USERPREFS_EVENT_MODE
#ifdef FSCom
        spiLock->lock();
        const bool activeBackupExists = FSCom.exists(backupFileName);
        spiLock->unlock();
        if (activeBackupExists && !EncryptedStorage::isEncrypted(backupFileName)) {
            LOG_INFO("Migrating %s to encrypted storage", backupFileName);
            if (!EncryptedStorage::migrateFile(backupFileName)) {
                LOG_ERROR("Can't migrate %s to encrypted storage", backupFileName);
                storageCorruptThisLoad = true;
            }
        }
#endif
#endif

        // Event firmware keeps the normal radio profile inactive, so migrate it separately.
#if USERPREFS_EVENT_MODE
#ifdef FSCom
        const char *inactiveRadioProfileFiles[] = {STANDARD_CONFIG_FILE_NAME, STANDARD_CHANNEL_FILE_NAME,
                                                   STANDARD_BACKUP_FILE_NAME};
        for (const char *fn : inactiveRadioProfileFiles) {
            spiLock->lock();
            const bool exists = FSCom.exists(fn);
            spiLock->unlock();
            if (exists && !EncryptedStorage::isEncrypted(fn)) {
                LOG_INFO("Migrating inactive radio profile %s to encrypted storage", fn);
                if (!EncryptedStorage::migrateFile(fn)) {
                    LOG_ERROR("Can't migrate %s to encrypted storage", fn);
                    storageCorruptThisLoad = true;
                }
            }
        }
#endif
#endif
    }
#endif

    // 2.4.X - configuration migration to update new default intervals
    if (moduleConfig.version < 23) {
        LOG_DEBUG("ModuleConfig v%d stale, upgrade to new default intervals", moduleConfig.version);
        moduleConfig.version = DEVICESTATE_CUR_VER;
        if (moduleConfig.telemetry.device_update_interval == 900)
            moduleConfig.telemetry.device_update_interval = 0;
        if (moduleConfig.telemetry.environment_update_interval == 900)
            moduleConfig.telemetry.environment_update_interval = 0;
        if (moduleConfig.telemetry.air_quality_interval == 900)
            moduleConfig.telemetry.air_quality_interval = 0;
        if (moduleConfig.telemetry.power_update_interval == 900)
            moduleConfig.telemetry.power_update_interval = 0;
        if (moduleConfig.neighbor_info.update_interval == 900)
            moduleConfig.neighbor_info.update_interval = 0;
        if (moduleConfig.paxcounter.paxcounter_update_interval == 900)
            moduleConfig.paxcounter.paxcounter_update_interval = 0;

        saveToDisk(SEGMENT_MODULECONFIG);
    }

    // 2.8 - privacy: one-time flip of position sharing and device telemetry to OPT-IN for nodes upgrading
    // from a build that shipped them on-by-default. Gated on a dedicated watermark (POSITION_TELEMETRY_OPTIN_VER)
    // so it runs exactly once and does NOT re-clobber a user who later re-enables sharing (ordinary saves never
    // re-stamp .version, so a re-enabled node stays at the watermark and skips this block on the next boot).
    // Position is disabled only on public/default-PSK channels; private-PSK channels are preserved.
    if (channelFile.version < POSITION_TELEMETRY_OPTIN_VER) {
        LOG_INFO("Opt-in migration: disabling position broadcast on public channels");
        optInDisablePositionSharing(channelFile);
        channelFile.version = POSITION_TELEMETRY_OPTIN_VER;
        saveToDisk(SEGMENT_CHANNELS);
    }
    if (moduleConfig.version < POSITION_TELEMETRY_OPTIN_VER) {
        LOG_INFO("Opt-in migration: forcing device telemetry broadcast to opt-in");
        optInDisableTelemetryBroadcast(moduleConfig);
        moduleConfig.version = POSITION_TELEMETRY_OPTIN_VER;
        saveToDisk(SEGMENT_MODULECONFIG);
    }

    if (channels.ensureLicensedOperation()) {
        LOG_WARN("Licensed operation removed persisted channel encryption/admin access");
        saveToDisk(SEGMENT_CHANNELS);
    }
#if defined(HELTEC_V4_OLED)
    if (incompleteConfigResetDetected)
        configDecodeFailed = true;
    if (requiresConfigRecovery())
        forceHeltecLocalRecoveryConfiguration();
#endif
#if ARCH_PORTDUINO
    // set any config overrides
    if (portduino_config.has_configDisplayMode) {
        config.display.displaymode = (_meshtastic_Config_DisplayConfig_DisplayMode)portduino_config.configDisplayMode;
    }
    if (portduino_config.has_statusMessage) {
        moduleConfig.has_statusmessage = true;
        strncpy(moduleConfig.statusmessage.node_status, portduino_config.statusMessage.c_str(),
                sizeof(moduleConfig.statusmessage.node_status));
        moduleConfig.statusmessage.node_status[sizeof(moduleConfig.statusmessage.node_status) - 1] = '\0';
    }
    if (portduino_config.enable_UDP) {
        config.network.enabled_protocols = meshtastic_Config_NetworkConfig_ProtocolFlags_UDP_BROADCAST;
    }

#endif
}

#ifdef MESHTASTIC_ENCRYPTED_STORAGE
// Serializes reloadFromDisk against itself. Other readers of config /
// channelFile / nodeDatabase don't take this lock today, so this only
// prevents reload-vs-reload races (e.g. fast successive unlocks). It is
// not a full data-race fix for those structs - that would require
// thread-shared locking discipline across the whole codebase, beyond
// the audit's M7 scope. The radio standby+reconfigure below keeps the
// radio out of the window where SX12xx registers are mid-swap.
static concurrency::Lock g_reloadFromDiskMutex;

/**
 * Re-run loadFromDisk() after encrypted storage is unlocked at runtime.
 * Holds the radio in standby across the file IO + proto decode so the
 * SX12xx is not mid-RX/TX when config.lora is overwritten, then calls
 * reconfigure() to push the now-real settings to the chip.
 *
 * Returns true iff every encrypted file decrypted and decoded cleanly.
 * On false the caller MUST treat storage as corrupt - see header.
 */
bool NodeDB::reloadFromDisk()
{
    concurrency::LockGuard guard(&g_reloadFromDiskMutex);
    LOG_INFO("NodeDB: Reloading config from encrypted storage after unlock");

    if (!shouldUseFilesystemPersistence(fsIsMounted())) {
        LOG_ERROR("NodeDB: reload refused while filesystem is unavailable");
        return false;
    }

    RadioInterface *rIface = router ? router->getRadioIface() : nullptr;

    // Park the radio while config.lora / channelFile swap. Without this,
    // a concurrent send or receive can read half-old / half-new state
    // (channel keys, region, modem preset) and the SX12xx ends up in
    // an inconsistent register set that only a reboot recovers from.
    if (rIface && !rIface->sleep()) {
        LOG_ERROR("NodeDB: encrypted reload could not park the radio");
        return false;
    }

    loadFromDisk();

    if (storageCorruptThisLoad || requiresConfigRecovery()) {
        LOG_ERROR("NodeDB: reload failed validation - treat preference generation as corrupt");
        // Leave the radio sleeping. Caller will lock storage and emit
        // a LOCKED(storage_corrupt) status; we must not reconfigure
        // the chip with the locked-default placeholder values still
        // sitting in config.lora.
        bootDeferredPreferenceSegments = 0;
        return false;
    }

    int deferredBootSaves = bootDeferredPreferenceSegments;
    bootDeferredPreferenceSegments = 0;

#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)
    // The locked boot deliberately never installs a placeholder key. Loading
    // the protobufs restores the bytes but not CryptoEngine's Curve25519/XEdDSA
    // state, so activate and re-verify the exact persisted key before radio RX
    // is allowed to resume.
    if (config.security.private_key.size == 32) {
        const bool normalizeConfigPublicKey = config.security.public_key.size == 0;
        const bool normalizeOwnerPublicKey = owner.public_key.size == 0;
        uint8_t derivedPublicKey[32];
        if (!derivePublicKeyWithoutInstalling(config.security.private_key.bytes, derivedPublicKey) ||
            (config.security.public_key.size == 32 &&
             memcmp(config.security.public_key.bytes, derivedPublicKey, sizeof(derivedPublicKey)) != 0) ||
            (owner.public_key.size == 32 && memcmp(owner.public_key.bytes, derivedPublicKey, sizeof(derivedPublicKey)) != 0) ||
            crc32Buffer(derivedPublicKey, sizeof(derivedPublicKey)) != myNodeInfo.my_node_num || !crypto ||
            !crypto->regeneratePublicKey(derivedPublicKey, config.security.private_key.bytes)) {
            LOG_ERROR("NodeDB: encrypted reload identity validation failed");
            configDecodeFailed = true;
            unreadablePreferenceSegments |= SEGMENT_CONFIG;
            forceHeltecLocalRecoveryConfiguration();
            return false;
        }
        config.security.public_key.size = sizeof(derivedPublicKey);
        memcpy(config.security.public_key.bytes, derivedPublicKey, sizeof(derivedPublicKey));
        owner.public_key.size = sizeof(derivedPublicKey);
        memcpy(owner.public_key.bytes, derivedPublicKey, sizeof(derivedPublicKey));
        if (normalizeConfigPublicKey || normalizeOwnerPublicKey)
            deferredBootSaves |= SEGMENT_CONFIG | SEGMENT_DEVICESTATE | SEGMENT_NODEDATABASE;
    } else if (config.security.public_key.size != 0 || owner.public_key.size != 0 ||
               config.lora.region != meshtastic_Config_LoRaConfig_RegionCode_UNSET) {
        LOG_ERROR("NodeDB: encrypted reload lacks the private key required by the persisted identity");
        configDecodeFailed = true;
        unreadablePreferenceSegments |= SEGMENT_CONFIG;
        forceHeltecLocalRecoveryConfiguration();
        return false;
    }
#endif

    // loadFromDisk() replaced DeviceState after the locked constructor had
    // stamped fields owned by this running binary and silicon. Reapply those
    // fields before self-care copies owner into the local NodeInfo row.
    meshtastic_MyNodeInfo_device_id_t runtimeDeviceId{};
    if (getDeviceId(runtimeDeviceId.bytes))
        runtimeDeviceId.size = sizeof(runtimeDeviceId.bytes);
    if (myNodeInfo.device_id.size != runtimeDeviceId.size ||
        memcmp(myNodeInfo.device_id.bytes, runtimeDeviceId.bytes, sizeof(runtimeDeviceId.bytes)) != 0) {
        myNodeInfo.device_id = runtimeDeviceId;
        deferredBootSaves |= SEGMENT_DEVICESTATE;
    }

    if (myNodeInfo.min_app_version != 30200) {
        myNodeInfo.min_app_version = 30200;
        deferredBootSaves |= SEGMENT_DEVICESTATE;
    }
#ifdef USERPREFS_FIRMWARE_EDITION
    constexpr meshtastic_FirmwareEdition runtimeFirmwareEdition = USERPREFS_FIRMWARE_EDITION;
#else
    constexpr meshtastic_FirmwareEdition runtimeFirmwareEdition = meshtastic_FirmwareEdition_VANILLA;
#endif
    if (myNodeInfo.firmware_edition != runtimeFirmwareEdition) {
        myNodeInfo.firmware_edition = runtimeFirmwareEdition;
        deferredBootSaves |= SEGMENT_DEVICESTATE;
    }
#ifdef ARCH_ESP32
    Preferences preferences;
    preferences.begin("meshtastic", true);
    const uint32_t runtimeRebootCount = preferences.getUInt("rebootCounter", 0);
    preferences.end();
    if (myNodeInfo.reboot_count != runtimeRebootCount) {
        myNodeInfo.reboot_count = runtimeRebootCount;
        deferredBootSaves |= SEGMENT_DEVICESTATE;
    }
#endif

    getMacAddr(ourMacAddr);
    char runtimeOwnerId[sizeof(owner.id)]{};
    snprintf(runtimeOwnerId, sizeof(runtimeOwnerId), "!%08x", getNodeNum());
    const bool ownerRuntimeFieldsChanged = owner.hw_model != HW_VENDOR || owner.role != config.device.role ||
                                           memcmp(owner.macaddr, ourMacAddr, sizeof(owner.macaddr)) != 0 ||
                                           memcmp(owner.id, runtimeOwnerId, sizeof(owner.id)) != 0;
    if (ownerRuntimeFieldsChanged) {
        owner.hw_model = HW_VENDOR;
        owner.role = config.device.role;
        memcpy(owner.macaddr, ourMacAddr, sizeof(owner.macaddr));
        memcpy(owner.id, runtimeOwnerId, sizeof(owner.id));
        deferredBootSaves |= SEGMENT_DEVICESTATE | SEGMENT_NODEDATABASE;
    }

    cleanupMeshDB();

    // loadFromDisk() leaves the store untrimmed; run self-care now (getNodeNum()
    // is valid at runtime) to trim/demote non-self overflow, pin self to index 0
    // and normalise the backing store before the node DB is exercised again.
    nodeDBSelfCare();

    // Preserve constructor ordering: persist migrations and runtime-owned
    // metadata only after identity and self-care are established.
    const bool nodeDatabaseMigrationPending = migrationSavePending;
    if (nodeDatabaseMigrationPending)
        deferredBootSaves |= SEGMENT_NODEDATABASE;
    if (deferredBootSaves != 0 && !saveToDisk(deferredBootSaves)) {
        LOG_ERROR("NodeDB: deferred boot migrations failed after storage reload");
        return false;
    }
    if (nodeDatabaseMigrationPending)
        migrationSavePending = false;

    // Rebuild channel hashes and the region pointer before pushing the real
    // persisted generation to hardware. The locked placeholder deliberately
    // left myRegion at UNSET.
    resetRadioConfig(false);
    if (rIface) {
        if (!rIface->reconfigure()) {
            LOG_ERROR("NodeDB: encrypted reload could not activate the persisted radio configuration");
            return false;
        }
    }
    return true;
}

bool NodeDB::disableLockdownToPlaintext()
{
    concurrency::LockGuard guard(&g_reloadFromDiskMutex);
    if (!EncryptedStorage::isUnlocked()) {
        LOG_ERROR("NodeDB: disable requested but storage not unlocked");
        return false;
    }
    LOG_INFO("NodeDB: reverting encrypted prefs to plaintext for lockdown disable");

    // Decrypt each encrypted pref back to plaintext IN PLACE. Mirror of the
    // plaintext->encrypted migrate loop above. Order does not matter here;
    // EncryptedStorage::removeLockdownArtifacts() (which deletes the DEK,
    // the commit point) only runs after every file is confirmed plaintext.
    const char *filesToCheck[] = {STANDARD_CONFIG_FILE_NAME, STANDARD_CHANNEL_FILE_NAME, STANDARD_BACKUP_FILE_NAME,
                                  EVENT_CONFIG_FILE_NAME,    EVENT_CHANNEL_FILE_NAME,    EVENT_BACKUP_FILE_NAME,
                                  moduleConfigFileName,      deviceStateFileName,        nodeDatabaseFileName};
    for (const char *fn : filesToCheck) {
        if (!EncryptedStorage::migrateFileToPlaintext(fn)) {
            LOG_ERROR("NodeDB: revert %s to plaintext failed; abort disable (stays in lockdown)", fn);
            return false;
        }
    }

    // All files are plaintext now - remove the lockdown artifacts. Deleting
    // /prefs/.dek is the atomic commit: after it, isLockdownActive() is false.
    EncryptedStorage::removeLockdownArtifacts();
    return true;
}
#endif

/** Save a protobuf from a file, return true for success */
bool NodeDB::saveProto(const char *filename, size_t protoSize, const pb_msgdesc_t *fields, const void *dest_struct,
                       bool fullAtomic)
{
#if defined(HELTEC_V4_OLED)
    PreferenceStorageWriteGuard storageWrite(*this);
    if (!storageWrite) {
        LOG_WARN("NodeDB: refusing protobuf write during destructive storage mutation");
        return false;
    }
#endif

    if (!shouldUseFilesystemPersistence(fsIsMounted())) {
        LOG_ERROR("NodeDB: refusing write to %s while filesystem is unavailable", filename);
        return false;
    }

    const int preferenceSegment = preferenceSegmentForFile(filename);
#if defined(HELTEC_V4_OLED)
    const bool authorizedRecoveryWriter =
        destructiveStorageMutationActive.load(std::memory_order_acquire) &&
        destructiveStorageOwnerTask.load(std::memory_order_acquire) == reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle());
    if (preferenceSegment != 0 && requiresConfigRecovery() && !authorizedRecoveryWriter) {
        LOG_ERROR("NodeDB: refusing core write while another segment requires recovery: %s", filename);
        return false;
    }
    if (preferenceSegment != 0 && !preferenceWriteAllowedDuringEdit()) {
        LOG_WARN("NodeDB: defer external preference write during settings edit: %s", filename);
        return false;
    }
    if (preferenceSegment == 0 && isPreferenceEditTransactionActive() && !isPreferenceEditOwnerCurrentTask()) {
        LOG_WARN("NodeDB: reject auxiliary preference write from non-owner during settings edit: %s", filename);
        return false;
    }
#endif
    if (incompleteConfigResetDetected && preferenceSegment != 0) {
        LOG_ERROR("NodeDB: refusing write during incomplete config-reset recovery: %s", filename);
        return false;
    }
#if defined(HELTEC_V4_OLED) && defined(FSCom)
    if (incompleteNodeDatabaseResetDetected && preferenceSegment != 0) {
        const bool authorizedResetWriter = destructiveStorageMutationActive.load(std::memory_order_acquire) &&
                                           destructiveStorageOwnerTask.load(std::memory_order_acquire) ==
                                               reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle()) &&
                                           readHeltecResetPendingMarker() == HeltecResetPendingKind::NODEDB_RESET;
        if (!authorizedResetWriter) {
            LOG_ERROR("NodeDB: refusing core write during incomplete node-db reset: %s", filename);
            return false;
        }
    }
#endif
    if (incompleteLegacyMigrationDetected && preferenceSegment != 0) {
        LOG_ERROR("NodeDB: refusing write during incomplete legacy migration: %s", filename);
        return false;
    }
    if ((unreadablePreferenceSegments & preferenceSegment) != 0) {
        LOG_ERROR("NodeDB: refusing write to unreadable existing file %s until recovery", filename);
        return false;
    }

#if defined(HELTEC_V4_OLED)
    // Every protobuf path, including UI/auxiliary files that bypass
    // saveToDisk(), gets a fresh sample immediately before SafeFile opens.
    // Reset and explicitly destructive settings transactions retain their
    // stronger 3.65 V threshold.
    const bool preferenceEditActive = isPreferenceEditTransactionActive();
    const bool destructiveMutationActiveNow = destructiveStorageMutationActive.load(std::memory_order_acquire);
    const bool destructivePowerRequired =
        destructiveMutationActiveNow ||
        (preferenceEditActive && preferenceEditRequiresDestructivePower.load(std::memory_order_acquire));
    const bool heltecPowerSafe =
        destructivePowerRequired ? heltecDestructiveStoragePowerIsSafe() : heltecPreferenceStoragePowerIsSafe();
    if (!heltecPowerSafe) {
        if (preferenceSegment != 0 &&
            shouldQueueHeltecPreferenceWriteRetry(false, preferenceEditActive, destructiveMutationActiveNow)) {
            powerDeferredPreferenceSegments.fetch_or(preferenceSegment, std::memory_order_acq_rel);
        }
        LOG_ERROR("NodeDB: refusing preference write to %s while fresh power is unsafe", filename);
        return false;
    }
#endif

    // Identity and channel files must never replace a present-but-undecodable
    // generation, during boot or later. A reboot can retry a transient read;
    // XMODEM/BLE OTA or an explicit full reset provide recovery paths. Other
    // stores may still persist independent runtime repair state.
    if (isRadioProfileFile(filename) && configDecodeFailed) {
        LOG_ERROR("NodeDB: refusing write to %s until config recovery completes", filename);
        return false;
    }

    // do not try to save anything if power level is not safe. In many cases flash will be lock-protected
    // and all writes will fail anyway. Device should be sleeping at this point anyway.
    if (!powerHAL_isPowerLevelSafe()) {
        LOG_ERROR("saveProto() on unsafe device power level");
        return false;
    }

#ifdef MESHTASTIC_ENCRYPTED_STORAGE
    // Encrypt all files except uiconfig (no secrets) and the DEK file (self-encrypted).
    // Only when lockdown is ACTIVE (provisioned). A lockdown-capable but DISABLED
    // device has no DEK, so encryptAndWrite would fail and config would never
    // persist - it must save plaintext exactly like stock firmware. Once enabled,
    // the reloadFromDisk migrate pass re-saves these plaintext files encrypted.
    if (EncryptedStorage::isLockdownActive() && strcmp(filename, uiconfigFileName) != 0) {
        // ZeroizingArrayPtr wipes the unencrypted protobuf encoding (which contains
        // config secrets - channel PSKs, security private_key, etc.) before delete[],
        // so plaintext copies aren't left in heap memory after encryption completes.
        auto pbBuf = meshtastic_security::make_zeroizing_array(protoSize);
        if (!pbBuf) {
            LOG_ERROR("OOM encoding %s for encryption", filename);
            return false;
        }

        pb_ostream_t stream = pb_ostream_from_buffer(pbBuf.get(), protoSize);
        if (!pb_encode(&stream, fields, dest_struct)) {
            LOG_ERROR("Can't encode protobuf %s", PB_GET_ERROR(&stream));
            return false;
        }

        size_t encodedSize = stream.bytes_written;
#if defined(HELTEC_V4_OLED)
        bool ok = EncryptedStorage::encryptAndWrite(filename, pbBuf.get(), encodedSize, fullAtomic, destructivePowerRequired);
#else
        bool ok = EncryptedStorage::encryptAndWrite(filename, pbBuf.get(), encodedSize, fullAtomic);
#endif

        if (!ok) {
            LOG_ERROR("EncryptedStorage: encrypt+write %s failed", filename);
        }
        return ok;
    }
#endif

#ifdef FSCom
#if defined(HELTEC_V4_OLED)
    auto f = SafeFile(filename, fullAtomic, destructivePowerRequired);
#else
    auto f = SafeFile(filename, fullAtomic);
#endif

    LOG_INFO("Save %s", filename);
    pb_ostream_t stream = {&writecb, static_cast<Print *>(&f), protoSize};

    if (!pb_encode(&stream, fields, dest_struct)) {
        LOG_ERROR("Can't encode protobuf %s", PB_GET_ERROR(&stream));
        return false;
    }

    if (!f.close()) {
        LOG_ERROR("Can't write prefs");
        return false;
    }

    return true;
#else
    LOG_ERROR("Filesystem not implemented");
    return false;
#endif
}

bool NodeDB::saveChannelsToDisk()
{
#if defined(HELTEC_V4_OLED)
    PreferenceStorageWriteGuard storageWrite(*this);
    if (!storageWrite)
        return false;
#endif

    // do not try to save anything if power level is not safe. In many cases flash will be lock-protected
    // and all writes will fail anyway.
    if (!powerHAL_isPowerLevelSafe()) {
        LOG_ERROR("saveChannelsToDisk() on unsafe device power level");
        return false;
    }

#ifdef FSCom
    spiLock->lock();
    FSCom.mkdir("/prefs");
    spiLock->unlock();
#endif

    return saveProto(channelFileName, meshtastic_ChannelFile_size, &meshtastic_ChannelFile_msg, &channelFile, true);
}

bool NodeDB::saveDeviceStateToDisk()
{
#if defined(HELTEC_V4_OLED)
    PreferenceStorageWriteGuard storageWrite(*this);
    if (!storageWrite)
        return false;
#endif

    // do not try to save anything if power level is not safe. In many cases flash will be lock-protected
    // and all writes will fail anyway. Device should be sleeping at this point anyway.
    if (!powerHAL_isPowerLevelSafe()) {
        LOG_ERROR("saveDeviceStateToDisk() on unsafe device power level");
        return false;
    }

#ifdef FSCom
    spiLock->lock();
    FSCom.mkdir("/prefs");
    spiLock->unlock();
#endif
    // Device state is small enough to preserve the previous generation until the replacement verifies.
    return saveProto(deviceStateFileName, meshtastic_DeviceState_size, &meshtastic_DeviceState_msg, &devicestate, true);
}

bool NodeDB::saveNodeDatabaseToDisk()
{
#if defined(HELTEC_V4_OLED)
    PreferenceStorageWriteGuard storageWrite(*this);
    if (!storageWrite)
        return false;
    const bool authorizedRecoveryWriter =
        destructiveStorageMutationActive.load(std::memory_order_acquire) &&
        destructiveStorageOwnerTask.load(std::memory_order_acquire) == reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle());
    if ((requiresConfigRecovery() || (unreadablePreferenceSegments & SEGMENT_NODEDATABASE) != 0) && !authorizedRecoveryWriter) {
        // Return before projecting satellite maps or flushing warm.dat. A
        // corrupt/missing node generation must remain byte-for-byte available
        // for recovery, and critical config recovery fences every node tier.
        LOG_WARN("NodeDB: reject node/warm save while persisted generation requires recovery");
        return false;
    }
#if defined(FSCom)
    if (incompleteNodeDatabaseResetDetected && !(destructiveStorageMutationActive.load(std::memory_order_acquire) &&
                                                 destructiveStorageOwnerTask.load(std::memory_order_acquire) ==
                                                     reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle()) &&
                                                 readHeltecResetPendingMarker() == HeltecResetPendingKind::NODEDB_RESET)) {
        LOG_ERROR("NodeDB: refusing node/warm write during incomplete node-db reset");
        return false;
    }
#endif
    // Return before warm.dat or nodes.proto can be touched by an autosave
    // racing an open Admin settings transaction.
    if (!preferenceWriteAllowedDuringEdit()) {
        LOG_WARN("NodeDB: reject node database save while a settings edit is open");
        return false;
    }
#endif
    if (!shouldUseFilesystemPersistence(fsIsMounted())) {
        LOG_ERROR("NodeDB: refusing node database write while filesystem is unavailable");
        return false;
    }

    // Don't persist the node DB until this device has a PKI keypair
    // TODO: revisit when https://github.com/meshtastic/firmware/pull/10478 lands
#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)
    if (owner.public_key.size != 32 && !owner.is_licensed) {
        LOG_DEBUG("Skip NodeDB without key");
        return true;
    }
#endif

    // do not try to save anything if power level is not safe. In many cases flash will be lock-protected
    // and all writes will fail anyway. Device should be sleeping at this point anyway.
    if (!powerHAL_isPowerLevelSafe()) {
        LOG_ERROR("saveNodeDatabaseToDisk() on unsafe device power level");
        return false;
    }

    // Defer (don't fail) while xmodem holds the prefs file handle. A retry cannot
    // succeed until the transfer releases it.
#ifdef FSCom
    if (xModem.isBusy()) {
        LOG_DEBUG("Defer NodeDB save: xmodem in progress");
        return true;
    }
#endif

#ifdef FSCom
    spiLock->lock();
    FSCom.mkdir("/prefs");
    spiLock->unlock();
#endif

    // Project the maps into the on-disk vectors just before encoding; cleared
    // again on the way out so we don't carry duplicate state.
    concurrency::LockGuard guard(&satelliteMutex);
#if !MESHTASTIC_EXCLUDE_POSITIONDB
    nodeDatabase.positions.clear();
    nodeDatabase.positions.reserve(nodePositions.size());
    for (const auto &kv : nodePositions) {
        meshtastic_NodePositionEntry entry = meshtastic_NodePositionEntry_init_default;
        entry.num = kv.first;
        entry.has_position = true;
        entry.position = kv.second;
        nodeDatabase.positions.push_back(entry);
    }
#else
    nodeDatabase.positions.clear();
#endif

#if !MESHTASTIC_EXCLUDE_TELEMETRYDB
    nodeDatabase.telemetry.clear();
    nodeDatabase.telemetry.reserve(nodeTelemetry.size());
    for (const auto &kv : nodeTelemetry) {
        meshtastic_NodeTelemetryEntry entry = meshtastic_NodeTelemetryEntry_init_default;
        entry.num = kv.first;
        entry.has_device_metrics = true;
        entry.device_metrics = kv.second;
        nodeDatabase.telemetry.push_back(entry);
    }
#else
    nodeDatabase.telemetry.clear();
#endif

#if !MESHTASTIC_EXCLUDE_ENVIRONMENTDB
    nodeDatabase.environment.clear();
    nodeDatabase.environment.reserve(nodeEnvironment.size());
    for (const auto &kv : nodeEnvironment) {
        meshtastic_NodeEnvironmentEntry entry = meshtastic_NodeEnvironmentEntry_init_default;
        entry.num = kv.first;
        entry.has_environment_metrics = true;
        entry.environment_metrics = kv.second;
        nodeDatabase.environment.push_back(entry);
    }
#else
    nodeDatabase.environment.clear();
#endif

#if !MESHTASTIC_EXCLUDE_STATUSDB
    nodeDatabase.status.clear();
    nodeDatabase.status.reserve(nodeStatus.size());
    for (const auto &kv : nodeStatus) {
        meshtastic_NodeStatusEntry entry = meshtastic_NodeStatusEntry_init_default;
        entry.num = kv.first;
        entry.has_status = true;
        entry.status = kv.second;
        nodeDatabase.status.push_back(entry);
    }
#else
    nodeDatabase.status.clear();
#endif

    size_t nodeDatabaseSize;
    pb_get_encoded_size(&nodeDatabaseSize, meshtastic_NodeDatabase_fields, &nodeDatabase);
    // The node database can consume most of small LittleFS partitions, so it cannot
    // keep two generations during a save.
    // Heltec V4 has a dedicated 0x360000 LittleFS partition, large enough to
    // retain the old node database until the replacement passes readback. This
    // also makes v24 migration retryable after a failed write.
#if defined(HELTEC_V4_OLED)
    constexpr bool keepPreviousGeneration = true;
#else
    constexpr bool keepPreviousGeneration = false;
#endif
    bool ok =
        saveProto(nodeDatabaseFileName, nodeDatabaseSize, &meshtastic_NodeDatabase_msg, &nodeDatabase, keepPreviousGeneration);

    nodeDatabase.positions.clear();
    nodeDatabase.positions.shrink_to_fit();
    nodeDatabase.telemetry.clear();
    nodeDatabase.telemetry.shrink_to_fit();
    nodeDatabase.environment.clear();
    nodeDatabase.environment.shrink_to_fit();
    nodeDatabase.status.clear();
    nodeDatabase.status.shrink_to_fit();
#if defined(HELTEC_V4_OLED)
    if (!ok) {
        // The hot tier is the commit prerequisite for warm.dat. Never publish
        // a new warm generation after nodes.proto failed verification.
        return false;
    }
#endif
#if WARM_NODE_COUNT > 0
#ifdef ARCH_RP2040
    // nodes.proto + warm.dat are written back-to-back without the loop running between them;
    // reset the 8s HW watchdog so the second write gets a full budget (issue #10746).
    watchdog_update();
#endif
#if defined(HELTEC_V4_OLED)
    // Encoding and committing nodes.proto can take long enough for the supply
    // to change. Re-sample immediately before the independent warm.dat write;
    // never carry the hot-tier authorization across that boundary.
    const bool warmPreferenceEditActive = isPreferenceEditTransactionActive();
    const bool warmDestructiveMutationActive = destructiveStorageMutationActive.load(std::memory_order_acquire);
    const bool warmNeedsDestructivePower =
        warmDestructiveMutationActive ||
        (warmPreferenceEditActive && preferenceEditRequiresDestructivePower.load(std::memory_order_acquire));
    const bool warmPowerSafe =
        warmNeedsDestructivePower ? heltecDestructiveStoragePowerIsSafe() : heltecPreferenceStoragePowerIsSafe();
    if (!warmPowerSafe) {
        if (shouldQueueHeltecPreferenceWriteRetry(false, warmPreferenceEditActive, warmDestructiveMutationActive))
            powerDeferredPreferenceSegments.fetch_or(SEGMENT_NODEDATABASE, std::memory_order_acq_rel);
        LOG_WARN("NodeDB: deferring warm node store write until power recovers");
        return false;
    }
#endif
    // The Heltec warm tier is part of the node database generation. Propagate a
    // failed warm.dat commit so a node reset cannot acknowledge success and
    // reboot only to resurrect the supposedly removed identities.
#if defined(HELTEC_V4_OLED)
    const bool warmSaved = warmStore.saveIfDirty(warmNeedsDestructivePower);
#else
    const bool warmSaved = warmStore.saveIfDirty();
#endif
#if defined(HELTEC_V4_OLED)
    ok &= warmSaved;
#else
    (void)warmSaved;
#endif
#endif
    return ok;
}

bool NodeDB::saveToDiskNoRetry(int saveWhat)
{

    // do not try to save anything if power level is not safe. In many cases flash will be lock-protected
    // and all writes will fail anyway. Device should be sleeping at this point anyway.
    if (!powerHAL_isPowerLevelSafe()) {
        LOG_ERROR("saveToDiskNoRetry() on unsafe device power level");
        return false;
    }

#ifdef MESHTASTIC_ENCRYPTED_STORAGE
    // When lockdown is ACTIVE but storage is still locked, encryptAndWrite()
    // returns false for every file. Return true here: nothing can be saved until
    // unlock, and this is not a filesystem error.
    //
    // Gate on isLockdownActive(): a lockdown-capable but DISABLED device (never
    // provisioned) also has isUnlocked()==false, but it must persist plaintext
    // normally - skipping here would silently drop every config write (e.g. the
    // LoRa region) until the device is provisioned.
    if (EncryptedStorage::isLockdownActive() && !EncryptedStorage::isUnlocked()) {
        LOG_WARN("NodeDB: saveToDisk skipped - encrypted storage locked");
        return true;
    }
#endif

    bool success = true;
#ifdef FSCom
    spiLock->lock();
    FSCom.mkdir("/prefs");
    spiLock->unlock();
#endif

#if USERPREFS_EVENT_MODE
    if (eventProfileStorageUnavailable) {
        if (saveWhat & SEGMENT_CONFIG) {
            LOG_WARN("Skip event config write: insufficient profile storage at boot");
            saveWhat &= ~SEGMENT_CONFIG;
        }
        if (saveWhat & SEGMENT_CHANNELS) {
            LOG_WARN("Skip event channel write: insufficient profile storage at boot");
            saveWhat &= ~SEGMENT_CHANNELS;
        }
    }
#endif

    if (saveWhat & SEGMENT_CONFIG) {
        config.has_device = true;
        config.has_display = true;
        config.has_lora = true;
        config.has_position = true;
        config.has_power = true;
        config.has_network = true;
        config.has_bluetooth = true;
        config.has_security = true;

        success &= saveProto(configFileName, meshtastic_LocalConfig_size, &meshtastic_LocalConfig_msg, &config, true);
    }

    if (saveWhat & SEGMENT_MODULECONFIG) {
        moduleConfig.has_canned_message = true;
        moduleConfig.has_external_notification = true;
        moduleConfig.has_mqtt = true;
        moduleConfig.has_range_test = true;
        moduleConfig.has_serial = true;
        moduleConfig.has_store_forward = true;
        moduleConfig.has_telemetry = true;
        moduleConfig.has_neighbor_info = true;
        moduleConfig.has_detection_sensor = true;
        moduleConfig.has_ambient_lighting = true;
        moduleConfig.has_audio = true;
        moduleConfig.has_paxcounter = true;
        moduleConfig.has_statusmessage = true;
        moduleConfig.has_traffic_management = true;
        moduleConfig.has_tak = true;
#if !MESHTASTIC_EXCLUDE_BEACON
        moduleConfig.has_mesh_beacon = true;
#endif

        success &= saveProto(moduleConfigFileName, meshtastic_LocalModuleConfig_size, &meshtastic_LocalModuleConfig_msg,
                             &moduleConfig, true);
    }

    if (saveWhat & SEGMENT_CHANNELS) {
        success &= saveChannelsToDisk();
    }

    if (saveWhat & SEGMENT_DEVICESTATE) {
        success &= saveDeviceStateToDisk();
    }

    if (saveWhat & SEGMENT_NODEDATABASE) {
        success &= saveNodeDatabaseToDisk();
    }

    return success;
}

bool NodeDB::saveToDisk(int saveWhat)
{
    LOG_DEBUG("Save to disk %d", saveWhat);

#if defined(HELTEC_V4_OLED)
    if (shouldDeferBootPersistence(bootInitializationInProgress, configLoadComplete, configDecodeFailed)) {
        bootDeferredPreferenceSegments |= saveWhat;
        LOG_DEBUG("NodeDB: defer core save 0x%x until complete boot scan", saveWhat);
        return true;
    }
    PreferenceStorageWriteGuard storageWrite(*this);
    if (!storageWrite) {
        LOG_WARN("NodeDB: reject save during destructive storage mutation");
        return false;
    }
    const bool authorizedRecoveryWriter =
        destructiveStorageMutationActive.load(std::memory_order_acquire) &&
        destructiveStorageOwnerTask.load(std::memory_order_acquire) == reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle());
    if (requiresConfigRecovery() && !authorizedRecoveryWriter) {
        LOG_WARN("NodeDB: reject automatic/core save while configuration recovery is required");
        return false;
    }
    if (!preferenceWriteAllowedDuringEdit()) {
        LOG_WARN("NodeDB: reject external save while a settings edit is open");
        return false;
    }
#endif

    if (!shouldUseFilesystemPersistence(fsIsMounted())) {
        LOG_ERROR("NodeDB: refusing preference write while filesystem is unavailable");
#if defined(HELTEC_V4_OLED)
        // Preserve the degraded-boot recovery channel and the board's GPS-off default even if a
        // physical/UI caller mutated RAM before discovering that the change cannot be persisted.
        config.bluetooth.enabled = true;
        config.position.gps_mode = meshtastic_Config_PositionConfig_GpsMode_DISABLED;
        config.lora.region = meshtastic_Config_LoRaConfig_RegionCode_UNSET;
        config.lora.tx_enabled = false;
#endif
        return false;
    }

#if defined(HELTEC_V4_OLED)
    const bool preferenceEditActive = isPreferenceEditTransactionActive();
    const bool destructiveMutationActiveNow = destructiveStorageMutationActive.load(std::memory_order_acquire);
    const bool destructivePowerRequired =
        destructiveMutationActiveNow ||
        (preferenceEditActive && preferenceEditRequiresDestructivePower.load(std::memory_order_acquire));
    const bool heltecPowerSafe =
        destructivePowerRequired ? heltecDestructiveStoragePowerIsSafe() : heltecPreferenceStoragePowerIsSafe();
    if (!heltecPowerSafe) {
        if (shouldQueueHeltecPreferenceWriteRetry(false, preferenceEditActive, destructiveMutationActiveNow))
            powerDeferredPreferenceSegments.fetch_or(saveWhat, std::memory_order_acq_rel);
        LOG_ERROR("NodeDB: refusing core preference save while fresh power is unsafe");
        return false;
    }
#endif

    // do not try to save anything if power level is not safe. In many cases flash will be lock-protected
    // and all writes will fail anyway. Device should be sleeping at this point anyway.
    if (!powerHAL_isPowerLevelSafe()) {
        LOG_ERROR("saveToDisk() on unsafe device power level");
        return false;
    }

    bool success = saveToDiskNoRetry(saveWhat);

    if (!success) {
#if defined(HELTEC_V4_OLED)
        // A supply that sagged between files is not flash corruption. Leave
        // ordinary segments dirty and wait for the periodic power thread.
        const bool retryPowerSafe =
            destructivePowerRequired ? heltecDestructiveStoragePowerIsSafe() : heltecPreferenceStoragePowerIsSafe();
        if (!retryPowerSafe) {
            if (shouldQueueHeltecPreferenceWriteRetry(false, preferenceEditActive, destructiveMutationActiveNow))
                powerDeferredPreferenceSegments.fetch_or(saveWhat, std::memory_order_acq_rel);
            LOG_WARN("NodeDB: deferring failed save until power recovers");
            return false;
        }
#endif
        LOG_ERROR("Save to disk failed, retry without formatting");
        success = saveToDiskNoRetry(saveWhat);

        RECORD_CRITICALERROR(success ? meshtastic_CriticalErrorCode_FLASH_CORRUPTION_RECOVERABLE
                                     : meshtastic_CriticalErrorCode_FLASH_CORRUPTION_UNRECOVERABLE);
    }

#if defined(HELTEC_V4_OLED)
    if (success)
        powerDeferredPreferenceSegments.fetch_and(~saveWhat, std::memory_order_acq_rel);
#endif

    return success;
}

#if defined(HELTEC_V4_OLED)
void NodeDB::retryPowerDeferredPreferenceWrites()
{
    const int saveWhat = powerDeferredPreferenceSegments.load(std::memory_order_acquire);
    if (saveWhat == 0 || bootInitializationInProgress || requiresConfigRecovery() || rebootAtMsec != 0 || shutdownAtMsec != 0 ||
        isPreferenceEditTransactionActive() || destructiveStorageMutationActive.load(std::memory_order_acquire))
        return;
    if (!heltecPreferenceStoragePowerIsSafe())
        return;

    LOG_INFO("NodeDB: retrying low-voltage deferred preference segments 0x%x", saveWhat);
    (void)saveToDisk(saveWhat);
}
#endif

bool NodeDB::beginPreferenceEdit(bool requireDestructivePower)
{
#if defined(HELTEC_V4_OLED) && defined(FSCom)
    if (rebootAtMsec != 0 || shutdownAtMsec != 0) {
        LOG_WARN("Settings edit refused while reboot/shutdown is pending");
        return false;
    }
    if (destructiveStorageMutationActive.load(std::memory_order_acquire)) {
        LOG_WARN("Settings edit refused during destructive storage mutation");
        return false;
    }
    concurrency::LockGuard transactionGuard(&heltecPreferencesTransactionLock);
    if (destructiveStorageMutationActive.load(std::memory_order_acquire)) {
        LOG_WARN("Settings edit refused during destructive storage mutation");
        return false;
    }
    HeltecXModemStorageGuard xmodemGuard;
    if (!xmodemGuard) {
        LOG_ERROR("Settings edit refused while XMODEM transfer is active");
        return false;
    }
    if (requiresConfigRecovery() || readHeltecResetPendingMarker() != HeltecResetPendingKind::NONE) {
        LOG_ERROR("Settings edit refused while configuration recovery is active");
        return false;
    }
    const bool powerIsSafe =
        requireDestructivePower ? heltecDestructiveStoragePowerIsSafe() : heltecPreferenceStoragePowerIsSafe();
    if (!powerIsSafe) {
        LOG_ERROR("Settings edit refused: power is not safe for a multi-file transaction");
        return false;
    }

    PreferenceEditState expected = PreferenceEditState::NONE;
    if (!preferenceEditState.compare_exchange_strong(expected, PreferenceEditState::QUIESCING, std::memory_order_acq_rel)) {
        LOG_WARN("Settings edit refused: another edit is already open");
        return false;
    }
    preferenceEditOwnerTask.store(reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle()), std::memory_order_release);
    preferenceEditOwnerClient.store(currentExternalStateClientToken(), std::memory_order_release);
    preferenceEditRequiresDestructivePower.store(requireDestructivePower, std::memory_order_release);
    preferenceEditRadioParked.store(false, std::memory_order_release);

    RadioInterface *const radio = router ? router->getRadioIface() : nullptr;
    const auto resumeTrafficAfterFence = [&]() {
        // Release-store NONE before waking either worker. They must never
        // observe the wake while the generation fence is still active.
        if (radio)
            radio->resumeQueuedTransmissions();
        if (router)
            router->setReceivedMessage();
    };
    const auto cancelBegin = [&]() {
        // Once radioParked is latched, QUIESCING may already have suppressed
        // the post-RX rearm even if sleep itself was never needed. Reapply the
        // unchanged durable generation before releasing either queue.
        if (preferenceEditRadioParked.load(std::memory_order_acquire)) {
            preferenceEditState.store(PreferenceEditState::ACTIVATING, std::memory_order_release);
        }
        if (preferenceEditRadioParked.load(std::memory_order_acquire) && (!radio || !radio->reconfigure())) {
            preferenceEditState.store(PreferenceEditState::OPEN, std::memory_order_release);
            scheduleHeltecRecoveryReboot();
            return false;
        }
        preferenceEditOwnerTask.store(0, std::memory_order_release);
        preferenceEditOwnerClient.store(0, std::memory_order_release);
        preferenceEditRequiresDestructivePower.store(false, std::memory_order_release);
        preferenceEditRadioParked.store(false, std::memory_order_release);
        preferenceEditState.store(PreferenceEditState::NONE, std::memory_order_release);
        resumeTrafficAfterFence();
        return false;
    };

    // QUIESCING blocks new mesh egress and new Router decoding, but permits a
    // TX/RX admitted under the committed generation to finish. Never call
    // sleep() while a TX is active: forced standby is not a successful TX and
    // must not be reported as one. A bounded refusal leaves the active radio
    // operation untouched and lets the client retry BEGIN.
    const auto waitForRadioQuiesce = [&]() {
        if (!radio)
            return true;
        constexpr uint32_t radioQuiesceWaitMs = 5000;
        const uint32_t started = millis();
        while (!radio->canParkForConfig()) {
            if (!Throttle::isWithinTimespanMs(started, radioQuiesceWaitMs)) {
                LOG_WARN("Settings edit refused: LoRa did not quiesce in time");
                return false;
            }
            delay(1);
        }
        return true;
    };
    if (!waitForRadioQuiesce())
        return cancelBegin();
    if (radio) {
        // Once the first stable handoff is observed, a just-completed RX/TX
        // may already have suppressed its normal rearm. Every later exit must
        // therefore reapply the unchanged committed generation.
        preferenceEditRadioParked.store(true, std::memory_order_release);
    }

    // Drain operations admitted just before the CAS while they can still nest
    // old-generation scopes and enqueue their resulting ACK/relay packet. The
    // TX worker remains fenced, so those packets stay queued and make BEGIN
    // retry rather than crossing a key/radio generation.
    if (!waitForExternalStateReaders()) {
        LOG_ERROR("Settings edit refused: state readers/writers did not quiesce");
        return cancelBegin();
    }
    if (destructiveStorageMutationActive.load(std::memory_order_acquire) || rebootAtMsec != 0 || shutdownAtMsec != 0) {
        LOG_WARN("Settings edit cancelled while reset/shutdown became pending");
        return cancelBegin();
    }

    // A finishing admitted reader may have been the last producer of radio
    // work. Revalidate hardware idleness after the reader fence drains.
    if (!waitForRadioQuiesce())
        return cancelBegin();

    if (radio) {

        // Queued ciphertext belongs to the currently committed channel keys.
        // Do not carry it across a settings generation; release the fence and
        // let its existing owner transmit/decode it before BEGIN is retried.
        if (radio->hasPendingTransmissionsForConfig() || (router && router->hasPendingRadioPacketsForConfig())) {
            LOG_INFO("Settings edit deferred until committed-generation LoRa queues drain");
            return cancelBegin();
        }
    }

    if (radio) {
        if (!radio->sleep()) {
            LOG_ERROR("Settings edit could not park LoRa safely");
            return cancelBegin();
        }
        // A terminal ISR worker can become runnable immediately after the
        // pre-sleep snapshot. Driver SPI operations are serialized and
        // QUIESCING forbids rearm; wait until that worker has fully handed off
        // its packet before inspecting queues or permitting RAM mutation.
        if (!waitForRadioQuiesce()) {
            LOG_ERROR("Settings edit could not verify the parked LoRa handoff");
            return cancelBegin();
        }
        // sleep() first services any already-pending terminal IRQ. If that
        // captured a frame, decode it under the old keys before allowing the
        // caller to mutate RAM.
        if (radio->hasPendingTransmissionsForConfig() || (router && router->hasPendingRadioPacketsForConfig())) {
            LOG_INFO("Settings edit deferred for LoRa work completed at the quiesce boundary");
            return cancelBegin();
        }
    }

    // No core file is allowed to write while OPEN, so an interruption before
    // commit leaves the previous on-disk generation intact. The durable EDIT
    // marker is written immediately before the first commit write, avoiding a
    // false recovery state when a client disconnects just after BEGIN.
    preferenceEditState.store(PreferenceEditState::OPEN, std::memory_order_release);
    return true;
#else
    (void)requireDestructivePower;
    return true;
#endif
}

bool NodeDB::cancelPreferenceEdit()
{
#if defined(HELTEC_V4_OLED) && defined(FSCom)
    concurrency::LockGuard transactionGuard(&heltecPreferencesTransactionLock);
    if (preferenceEditState.load(std::memory_order_acquire) != PreferenceEditState::OPEN || !isPreferenceEditOwnerCurrentTask() ||
        requiresConfigRecovery() || readHeltecResetPendingMarker() != HeltecResetPendingKind::NONE) {
        LOG_ERROR("Settings edit cancellation refused from non-owner or unsafe state");
        return false;
    }

    // Keep ingress/egress fenced while the caller-proven-unchanged durable
    // generation is re-applied.
    // The radio driver permits only this owner task to rearm RX in ACTIVATING.
    preferenceEditState.store(PreferenceEditState::ACTIVATING, std::memory_order_release);
    if (preferenceEditRadioParked.load(std::memory_order_acquire) &&
        (!router || !router->getRadioIface() || !router->getRadioIface()->reconfigure())) {
        preferenceEditState.store(PreferenceEditState::OPEN, std::memory_order_release);
        LOG_ERROR("Settings edit cancellation could not restore radio configuration");
        scheduleHeltecRecoveryReboot();
        return false;
    }

    RadioInterface *const radio = router ? router->getRadioIface() : nullptr;
    preferenceEditOwnerTask.store(0, std::memory_order_release);
    preferenceEditOwnerClient.store(0, std::memory_order_release);
    preferenceEditRequiresDestructivePower.store(false, std::memory_order_release);
    preferenceEditRadioParked.store(false, std::memory_order_release);
    preferenceEditState.store(PreferenceEditState::NONE, std::memory_order_release);
    if (radio)
        radio->resumeQueuedTransmissions();
    if (router)
        router->setReceivedMessage();
#endif
    return true;
}

#if defined(HELTEC_V4_OLED)
bool NodeDB::isPreferenceEditTransactionActive() const
{
    return preferenceEditState.load(std::memory_order_acquire) != PreferenceEditState::NONE;
}

bool NodeDB::isPreferenceEditQuiescing() const
{
    return preferenceEditState.load(std::memory_order_acquire) == PreferenceEditState::QUIESCING;
}

bool NodeDB::isPreferenceEditRadioActivationAllowed() const
{
    return preferenceEditState.load(std::memory_order_acquire) == PreferenceEditState::ACTIVATING &&
           isPreferenceEditOwnerCurrentTask();
}

bool NodeDB::isPreferenceEditOwnerCurrentTask() const
{
    if (preferenceEditState.load(std::memory_order_acquire) == PreferenceEditState::NONE ||
        preferenceEditOwnerTask.load(std::memory_order_acquire) != reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle()))
        return false;
    const uintptr_t ownerClient = preferenceEditOwnerClient.load(std::memory_order_acquire);
    return ownerClient == 0 || ownerClient == currentExternalStateClientToken();
}

bool NodeDB::isDestructiveStorageMutationOwnerCurrentTask() const
{
    return destructiveStorageMutationActive.load(std::memory_order_acquire) &&
           destructiveStorageOwnerTask.load(std::memory_order_acquire) ==
               reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle());
}

bool NodeDB::adoptAbandonedPreferenceEdit()
{
    if (preferenceEditState.load(std::memory_order_acquire) != PreferenceEditState::OPEN)
        return false;
    preferenceEditOwnerTask.store(reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle()), std::memory_order_release);
    preferenceEditOwnerClient.store(currentExternalStateClientToken(), std::memory_order_release);
    return true;
}

bool NodeDB::preferenceWriteAllowedDuringEdit() const
{
    const PreferenceEditState state = preferenceEditState.load(std::memory_order_acquire);
    if (state == PreferenceEditState::NONE)
        return true;
    if (state != PreferenceEditState::COMMITTING)
        return false;

    // Only the task executing commitPreferenceEdit() may write while the
    // transaction is in its commit phase. Background autosaves on another
    // task must not join the generation accidentally.
    return preferenceEditOwnerTask.load(std::memory_order_acquire) == reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle());
}
#else
bool NodeDB::isPreferenceEditTransactionActive() const
{
    return false;
}
bool NodeDB::isPreferenceEditQuiescing() const
{
    return false;
}
bool NodeDB::isPreferenceEditOwnerCurrentTask() const
{
    return true;
}
bool NodeDB::isDestructiveStorageMutationOwnerCurrentTask() const
{
    return false;
}
bool NodeDB::adoptAbandonedPreferenceEdit()
{
    return true;
}
bool NodeDB::isPreferenceEditRadioActivationAllowed() const
{
    return true;
}
#endif

#if defined(HELTEC_V4_OLED)
uintptr_t NodeDB::currentExternalStateClientToken() const
{
    const uintptr_t task = reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle());
    concurrency::LockGuard guard(&externalStateReaderLock);
    uintptr_t token = 0;
    for (const auto &slot : externalStateReaderSlots) {
        if (slot.task != task || slot.depth == 0 || slot.clientToken == 0)
            continue;
        if (token != 0 && token != slot.clientToken)
            return 0;
        token = slot.clientToken;
    }
    return token;
}

bool NodeDB::isCurrentExternalStateStatelessClient() const
{
    // PhoneAPI reserves bit zero for request-scoped transports such as HTTP;
    // process-unique connection/session tokens are always even.
    return (currentExternalStateClientToken() & uintptr_t{1}) != 0;
}

bool NodeDB::hasCurrentExternalStateAccess() const
{
    const uintptr_t task = reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle());
    concurrency::LockGuard guard(&externalStateReaderLock);
    for (const auto &slot : externalStateReaderSlots) {
        if (slot.task == task && slot.depth != 0)
            return true;
    }
    return false;
}
#else
bool NodeDB::hasCurrentExternalStateAccess() const
{
    return false;
}
bool NodeDB::isCurrentExternalStateStatelessClient() const
{
    return false;
}
#endif

bool NodeDB::beginPreferenceStorageWrite()
{
#if defined(HELTEC_V4_OLED)
    const uintptr_t task = reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle());
    const auto ownedByCurrentTask = [&]() { return destructiveStorageOwnerTask.load(std::memory_order_acquire) == task; };
    if (destructiveStorageMutationActive.load(std::memory_order_acquire) && !ownedByCurrentTask())
        return false;
    preferenceStorageWriters.fetch_add(1, std::memory_order_acq_rel);
    if (destructiveStorageMutationActive.load(std::memory_order_acquire) && !ownedByCurrentTask()) {
        preferenceStorageWriters.fetch_sub(1, std::memory_order_acq_rel);
        return false;
    }
#endif
    return true;
}

void NodeDB::endPreferenceStorageWrite()
{
#if defined(HELTEC_V4_OLED)
    preferenceStorageWriters.fetch_sub(1, std::memory_order_acq_rel);
#endif
}

bool NodeDB::beginExternalStateAccess(uintptr_t clientToken)
{
#if defined(HELTEC_V4_OLED)
    if (destructiveStorageMutationActive.load(std::memory_order_acquire))
        return false;

    const uintptr_t task = reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle());
    {
        concurrency::LockGuard guard(&externalStateReaderLock);
        if (destructiveStorageMutationActive.load(std::memory_order_acquire))
            return false;

        const PreferenceEditState editState = preferenceEditState.load(std::memory_order_acquire);
        if (editState != PreferenceEditState::NONE) {
            const uintptr_t ownerTask = preferenceEditOwnerTask.load(std::memory_order_acquire);
            if (ownerTask != task) {
                // A Router/MQTT operation admitted before NONE->QUIESCING may
                // nest more token-zero scopes while it finishes under the old
                // generation. Do not admit a new request merely because its
                // executor task was reused.
                bool continuingAdmittedReader = false;
                for (const auto &slot : externalStateReaderSlots) {
                    if (slot.task == task && slot.depth != 0 && (clientToken == 0 || slot.clientToken == clientToken)) {
                        continuingAdmittedReader = true;
                        break;
                    }
                }
                if (!continuingAdmittedReader)
                    return false;
            }
            const uintptr_t ownerClient = preferenceEditOwnerClient.load(std::memory_order_acquire);
            if (ownerTask == task && ownerClient != 0) {
                bool ownerSessionStillOnStack = false;
                for (const auto &slot : externalStateReaderSlots) {
                    if (slot.task == task && slot.depth != 0 && slot.clientToken == ownerClient) {
                        ownerSessionStillOnStack = true;
                        break;
                    }
                }
                // Nested Router/MeshService scopes use token zero; allow them
                // only while the owning PhoneAPI request is visibly on this
                // same stack. A later HTTP request or reconnected client must
                // not inherit the transaction merely by reusing a task.
                if (!ownerSessionStillOnStack && clientToken != ownerClient)
                    return false;
            }
        }
        ExternalStateReaderSlot *available = nullptr;
        for (auto &slot : externalStateReaderSlots) {
            if (slot.task == task && slot.clientToken == clientToken && slot.depth != 0) {
                available = &slot;
                break;
            }
            if (!available && slot.depth == 0)
                available = &slot;
        }
        if (!available || available->depth == UINT16_MAX) {
            LOG_ERROR("No PhoneAPI/HTTP reader slot available");
            return false;
        }
        available->task = task;
        available->clientToken = clientToken;
        available->depth++;
        externalStateReaders.fetch_add(1, std::memory_order_release);
    }
    if (destructiveStorageMutationActive.load(std::memory_order_acquire)) {
        endExternalStateAccess(clientToken);
        return false;
    }
#endif
    return true;
}

void NodeDB::endExternalStateAccess(uintptr_t clientToken)
{
#if defined(HELTEC_V4_OLED)
    const uintptr_t task = reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle());
    concurrency::LockGuard guard(&externalStateReaderLock);
    for (auto &slot : externalStateReaderSlots) {
        if (slot.task != task || slot.clientToken != clientToken || slot.depth == 0)
            continue;
        slot.depth--;
        if (slot.depth == 0) {
            slot.task = 0;
            slot.clientToken = 0;
        }
        externalStateReaders.fetch_sub(1, std::memory_order_release);
        return;
    }
    LOG_ERROR("Unbalanced PhoneAPI/HTTP state reader release");
#endif
}

bool NodeDB::waitForExternalStateReaders()
{
#if defined(HELTEC_V4_OLED)
    constexpr uint32_t waitLimitMs = 5000;
    const uint32_t started = millis();
    const uintptr_t currentTask = reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle());
    while (true) {
        uint32_t currentTaskReaders = 0;
        uint32_t totalReaders = 0;
        {
            concurrency::LockGuard guard(&externalStateReaderLock);
            totalReaders = externalStateReaders.load(std::memory_order_acquire);
            for (const auto &slot : externalStateReaderSlots) {
                if (slot.task == currentTask)
                    currentTaskReaders += slot.depth;
            }
        }
        // A destructive Admin request may be dispatched synchronously inside
        // its own PhoneAPI callback. That stack cannot race itself; wait only
        // for readers owned by other tasks before replacing shared state.
        const uint32_t storageWriters = preferenceStorageWriters.load(std::memory_order_acquire);
        if (totalReaders <= currentTaskReaders && storageWriters == 0)
            return true;
        if (!Throttle::isWithinTimespanMs(started, waitLimitMs)) {
            LOG_ERROR("Timed out waiting for PhoneAPI/HTTP readers or preference writers");
            return false;
        }
        delay(1);
    }
#endif
    return true;
}

bool NodeDB::commitPreferenceEdit(int saveWhat, bool commitOpenEdit)
{
#if defined(HELTEC_V4_OLED) && defined(FSCom)
    if (destructiveStorageMutationActive.load(std::memory_order_acquire)) {
        LOG_WARN("Settings commit refused during destructive storage mutation");
        return false;
    }
    concurrency::LockGuard transactionGuard(&heltecPreferencesTransactionLock);
    if (destructiveStorageMutationActive.load(std::memory_order_acquire)) {
        LOG_WARN("Settings commit refused during destructive storage mutation");
        return false;
    }
    PreferenceEditState initialState = preferenceEditState.load(std::memory_order_acquire);
    if (initialState == PreferenceEditState::QUIESCING || initialState == PreferenceEditState::COMMITTING ||
        initialState == PreferenceEditState::ACTIVATING) {
        LOG_ERROR("Settings commit refused: another commit is already active");
        return false;
    }
    if (initialState == PreferenceEditState::NONE) {
        // Opening here is too late: reloadConfig() is called after setters have
        // already changed shared RAM. Every Heltec mutation must establish
        // BEGIN/QUIESCING before touching that state.
        LOG_ERROR("Settings commit refused without a pre-mutation transaction fence");
        return false;
    }
    if (initialState == PreferenceEditState::OPEN && !commitOpenEdit) {
        // A menu/autosave racing a client bulk import must not commit one
        // segment and clear the client's durable EDIT marker.
        LOG_WARN("One-shot settings commit refused while a bulk edit is open");
        return false;
    }
    if (initialState == PreferenceEditState::OPEN && commitOpenEdit && !isPreferenceEditOwnerCurrentTask()) {
        LOG_WARN("Settings commit refused from a different local transport task");
        return false;
    }

    const auto failBeforeCommit = []() { return false; };

    HeltecXModemStorageGuard xmodemGuard;
    if (!xmodemGuard) {
        LOG_ERROR("Settings commit refused while XMODEM transfer is active");
        return failBeforeCommit();
    }
    const HeltecResetPendingKind pendingKind = readHeltecResetPendingMarker();
    if (pendingKind != HeltecResetPendingKind::NONE) {
        LOG_ERROR("Settings commit refused while another recovery transaction is active");
        return failBeforeCommit();
    }
    const bool editRequiresDestructivePower = preferenceEditRequiresDestructivePower.load(std::memory_order_acquire);
    const auto commitPowerIsSafe = [editRequiresDestructivePower]() {
        return editRequiresDestructivePower ? heltecDestructiveStoragePowerIsSafe() : heltecPreferenceStoragePowerIsSafe();
    };
    if (!commitPowerIsSafe()) {
        LOG_ERROR("Settings commit refused: power is not safe for a multi-file transaction");
        return failBeforeCommit();
    }
    if (!writeHeltecResetPendingMarker(HeltecResetPendingKind::EDIT, editRequiresDestructivePower)) {
        LOG_ERROR("Settings commit refused: durable intent could not be verified");
        return failBeforeCommit();
    }

    preferenceEditOwnerTask.store(reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle()), std::memory_order_release);
    preferenceEditState.store(PreferenceEditState::COMMITTING, std::memory_order_release);
    const bool saved = saveToDisk(saveWhat);
    const bool powerStillSafe = commitPowerIsSafe();
    const bool markerCleared =
        saved && powerStillSafe && clearHeltecResetPendingMarker(HeltecResetPendingKind::EDIT, editRequiresDestructivePower);
    if (markerCleared) {
        // Keep mesh traffic fenced until MeshService has applied the committed
        // channel/radio generation to hardware. The owner task alone may rearm
        // RX during that activation window.
        preferenceEditState.store(PreferenceEditState::ACTIVATING, std::memory_order_release);
        return true;
    }

    // Keep every unrelated preference writer blocked until the guarded reboot.
    preferenceEditOwnerTask.store(0, std::memory_order_release);
    preferenceEditOwnerClient.store(0, std::memory_order_release);
    preferenceEditState.store(PreferenceEditState::OPEN, std::memory_order_release);

    LOG_ERROR("Settings commit incomplete - entering local recovery");
    configDecodeFailed = true;
    incompleteConfigResetDetected = true;
    unreadablePreferenceSegments |= saveWhat;
    forceHeltecLocalRecoveryConfiguration();
#if !MESHTASTIC_EXCLUDE_GPS
    if (gps)
        gps->disable();
#endif
    if (router && router->getRadioIface())
        router->getRadioIface()->sleep();
    scheduleHeltecRecoveryReboot();
    return false;
#else
    (void)commitOpenEdit;
    return saveToDisk(saveWhat);
#endif
}

bool NodeDB::activatePreferenceEditRadio(bool radioConfigChanged)
{
#if defined(HELTEC_V4_OLED) && defined(FSCom)
    concurrency::LockGuard transactionGuard(&heltecPreferencesTransactionLock);
    if (!isPreferenceEditRadioActivationAllowed()) {
        LOG_ERROR("Settings radio activation refused from non-owner task/state");
        return false;
    }
    const bool mustReconfigure = radioConfigChanged || preferenceEditRadioParked.load(std::memory_order_acquire);
    if (mustReconfigure && (!router || !router->getRadioIface() || !router->getRadioIface()->reconfigure())) {
        LOG_ERROR("Committed settings could not restore LoRa RX");
        scheduleHeltecRecoveryReboot();
        return false;
    }
    preferenceEditRadioParked.store(false, std::memory_order_release);
#else
    (void)radioConfigChanged;
#endif
    return true;
}

bool NodeDB::finishPreferenceEditActivation()
{
#if defined(HELTEC_V4_OLED) && defined(FSCom)
    concurrency::LockGuard transactionGuard(&heltecPreferencesTransactionLock);
    if (!isPreferenceEditRadioActivationAllowed()) {
        LOG_ERROR("Settings activation finish refused from non-owner task/state");
        return false;
    }
    if (preferenceEditRadioParked.load(std::memory_order_acquire)) {
        LOG_ERROR("Settings activation cannot finish while LoRa remains parked");
        scheduleHeltecRecoveryReboot();
        return false;
    }
    preferenceEditOwnerTask.store(0, std::memory_order_release);
    preferenceEditOwnerClient.store(0, std::memory_order_release);
    preferenceEditRequiresDestructivePower.store(false, std::memory_order_release);
    preferenceEditRadioParked.store(false, std::memory_order_release);
    preferenceEditState.store(PreferenceEditState::NONE, std::memory_order_release);
    if (router && router->getRadioIface())
        router->getRadioIface()->resumeQueuedTransmissions();
    if (router)
        router->setReceivedMessage();
#endif
    return true;
}

const meshtastic_NodeInfoLite *NodeDB::readNextMeshNode(uint32_t &readIndex)
{
    if (readIndex < numMeshNodes)
        return &meshNodes->at(readIndex++);
    else
        return NULL;
}

/// Given a node, return how many seconds in the past (vs now) that we last heard from it
uint32_t sinceLastSeen(const meshtastic_NodeInfoLite *n)
{
    uint32_t now = getTime();

    int delta = (int)(now - n->last_heard);
    if (delta < 0) // our clock must be slightly off still - not set from GPS yet
        delta = 0;

    return delta;
}

uint32_t sinceReceived(const meshtastic_MeshPacket *p)
{
    // rx_time may be an uptime-seconds placeholder while has_rx_time is false - don't age it as
    // wall-clock, and don't pass it off as "just now" either.
    if (!p->has_rx_time)
        return SINCE_UNKNOWN;

    uint32_t now = getTime();

    int delta = (int)(now - p->rx_time);
    if (delta < 0) // our clock must be slightly off still - not set from GPS yet
        delta = 0;

    return delta;
}

HopStartStatus classifyHopStart(const meshtastic_MeshPacket &p)
{
    // Guard against invalid values.
    if (p.hop_start < p.hop_limit)
        return HopStartStatus::INVALID;

    if (p.hop_start == 0) {
        // hop_start == 0 is either a modern zero-hop broadcast (e.g. beacon) or pre-2.3.0 firmware (585805c)
        // that never populated hop_start. Firmware 2.5.0 (bf34329) introduced a bitfield that is always
        // present; use it to tell the two apart. The bitfield is encrypted under the channel key, so this can
        // only be resolved for decoded packets - until then the status stays MISSING_OR_UNKNOWN. Callers
        // acting before decode must therefore not treat UNKNOWN as a drop (the bitfield isn't readable yet);
        // the verdict is re-checked post-decode in Router::handleReceived.
        if (p.which_payload_variant == meshtastic_MeshPacket_decoded_tag && p.decoded.has_bitfield)
            return HopStartStatus::VALID;
        return HopStartStatus::MISSING_OR_UNKNOWN;
    }

    return HopStartStatus::VALID;
}

int8_t getHopsAway(const meshtastic_MeshPacket &p, int8_t defaultIfUnknown)
{
    // Firmware prior to 2.3.0 (585805c) lacked a hop_start field. Firmware version 2.5.0 (bf34329) introduced a
    // bitfield that is always present. Use the presence of the bitfield to determine if the origin's firmware
    // version is guaranteed to have hop_start populated. Note that this can only be done for decoded packets as
    // the bitfield is encrypted under the channel encryption key. For encrypted packets, this returns
    // defaultIfUnknown when hop_start is 0.
    if (p.hop_start == 0 && !(p.which_payload_variant == meshtastic_MeshPacket_decoded_tag && p.decoded.has_bitfield))
        return defaultIfUnknown; // Cannot reliably determine the number of hops.

    // Guard against invalid values.
    if (p.hop_start < p.hop_limit)
        return defaultIfUnknown;

    return p.hop_start - p.hop_limit;
}

#define NUM_ONLINE_SECS (60 * 60 * 2) // 2 hrs to consider someone offline

size_t NodeDB::getNumOnlineMeshNodes(bool localOnly)
{
    size_t numseen = 0;

    // FIXME this implementation is kinda expensive
    for (int i = 0; i < numMeshNodes; i++) {
        if (localOnly && nodeInfoLiteViaMqtt(&meshNodes->at(i)))
            continue;
        if (sinceLastSeen(&meshNodes->at(i)) < NUM_ONLINE_SECS)
            numseen++;
    }

    return numseen;
}

#include "MeshModule.h"

// Minimum spacing between evictions once the node database is full.
#define NODEDB_FULL_EVICTION_INTERVAL_MS (2 * 1000UL)

static constexpr uint32_t HOPSTART_DROP_LOG_INTERVAL_MS = 15000;

void logHopStartDrop(const meshtastic_MeshPacket &p, const char *context)
{
    static uint32_t lastLogMs = 0;
    if (Throttle::isWithinTimespanMs(lastLogMs, HOPSTART_DROP_LOG_INTERVAL_MS)) {
        return;
    }
    lastLogMs = millis();
    const bool decoded = (p.which_payload_variant == meshtastic_MeshPacket_decoded_tag);
    const bool hasBitfield = decoded && p.decoded.has_bitfield;
    LOG_DEBUG(
        "Drop packet (%s): hop_start invalid/missing (from=0x%08x id=%u hop_start=%u hop_limit=%u decoded=%d has_bitfield=%d)",
        context ? context : "unknown", p.from, p.id, p.hop_start, p.hop_limit, decoded, hasBitfield);
}

/** Update position info for this node based on received position data
 */
void NodeDB::updatePosition(uint32_t nodeId, const meshtastic_Position &p, RxSource src)
{
    meshtastic_NodeInfoLite *info = getOrCreateMeshNode(nodeId);
    if (!info) {
        return;
    }

#if MESHTASTIC_EXCLUDE_POSITIONDB
    // Build flag opted out: header still tracks last_heard via updateFrom; we
    // simply don't cache the position payload anywhere on this device.
    if (src == RX_SRC_LOCAL) {
        LOG_INFO("updatePosition LOCAL (PositionDB excluded) time=%u lat=%d lon=%d alt=%d", p.time, p.latitude_i, p.longitude_i,
                 p.altitude);
        setLocalPosition(p);
    }
    (void)nodeId;
    updateGUIforNode = info;
    notifyObservers(true);
#else
    {
        concurrency::LockGuard guard(&satelliteMutex);
        evictSatelliteOverCap(*this, nodePositions, nodeId);
        meshtastic_PositionLite &slot = nodePositions[nodeId]; // creates default-zero entry if missing

        if (src == RX_SRC_LOCAL) {
            // Local packet, fully authoritative
            LOG_INFO("updatePosition LOCAL pos@%x time=%u lat=%d lon=%d alt=%d", p.timestamp, p.time, p.latitude_i, p.longitude_i,
                     p.altitude);

            setLocalPosition(p);
            slot = TypeConversions::ConvertToPositionLite(p);
        } else if ((p.time > 0) && !p.latitude_i && !p.longitude_i && !p.timestamp && !p.location_source) {
            // FIXME SPECIAL TIME SETTING PACKET FROM EUD TO RADIO
            // (stop-gap fix for issue #900)
            LOG_DEBUG("updatePosition SPECIAL time setting time=%u", p.time);
            slot.time = p.time;
        } else {
            // Be careful to only update fields that have been set by the REMOTE sender
            // A lot of position reports don't have time populated.  In that case, be careful to not blow away the time we
            // recorded based on the packet rxTime
            //
            // FIXME perhaps handle RX_SRC_USER separately?
            LOG_INFO("updatePosition REMOTE node=0x%08x time=%u lat=%d lon=%d", nodeId, p.time, p.latitude_i, p.longitude_i);

            // First, back up fields that we want to protect from overwrite
            uint32_t tmp_time = slot.time;

            // Next, update atomically
            slot = TypeConversions::ConvertToPositionLite(p);

            // Last, restore any fields that may have been overwritten
            if (!slot.time)
                slot.time = tmp_time;
        }
    }
    updateGUIforNode = info;
    notifyObservers(true); // Force an update whether or not our node counts have changed
#endif
}

/** Update telemetry info for this node based on received metrics. Stores
 *  device_metrics and environment_metrics into their respective satellite
 *  maps; other variants (air_quality, power, local_stats, health) are
 *  intentionally not retained per-node.
 */
void NodeDB::updateTelemetry(uint32_t nodeId, const meshtastic_Telemetry &t, RxSource src)
{
    meshtastic_NodeInfoLite *info = getOrCreateMeshNode(nodeId);
    if (!info)
        return;

    if (t.which_variant == meshtastic_Telemetry_device_metrics_tag) {
        if (src == RX_SRC_LOCAL) {
            LOG_TRACE("updateTelemetry LOCAL device");
        } else {
            LOG_TRACE("updateTelemetry REMOTE device node=0x%08x", nodeId);
        }
#if !MESHTASTIC_EXCLUDE_TELEMETRYDB
        concurrency::LockGuard guard(&satelliteMutex);
        evictSatelliteOverCap(*this, nodeTelemetry, nodeId);
        nodeTelemetry[nodeId] = t.variant.device_metrics;
#endif

    } else if (t.which_variant == meshtastic_Telemetry_environment_metrics_tag) {
        if (src == RX_SRC_LOCAL) {
            LOG_TRACE("updateTelemetry LOCAL env");
        } else {
            LOG_TRACE("updateTelemetry REMOTE env node=0x%08x", nodeId);
        }
#if !MESHTASTIC_EXCLUDE_ENVIRONMENTDB
        concurrency::LockGuard guard(&satelliteMutex);
        evictSatelliteOverCap(*this, nodeEnvironment, nodeId);
        nodeEnvironment[nodeId] = t.variant.environment_metrics;
#endif

    } else {
        return; // air_quality / power / local_stats / health: not stored per-node
    }
    updateGUIforNode = info;
    notifyObservers(true);
}

/**
 * Update the node database with a new contact
 */
bool NodeDB::addFromContact(meshtastic_SharedContact contact, bool persist)
{
    // Validate before getOrCreateMeshNode(), which may evict another entry.
    // Every false return below must be a proven no-op so a surrounding edit
    // transaction can safely cancel.
    if (!contact.has_user)
        return false;
    meshtastic_NodeInfoLite *info = getOrCreateMeshNode(contact.node_num);
    if (!info)
        return false;
    // If the local node has this node marked as manually verified
    // and the client does not, do not allow the client to update the
    // saved public key.
    if (nodeInfoLiteIsKeyManuallyVerified(info) && !contact.manually_verified) {
        if (contact.user.public_key.size != info->public_key.size ||
            memcmp(contact.user.public_key.bytes, info->public_key.bytes, info->public_key.size) != 0) {
            return false;
        }
    }
    info->num = contact.node_num;
    // CopyUserToNodeInfoLite assigns public_key unconditionally, and clients send add_contact before every
    // DM - often from an entry that carries no key at all. A contact may still supply or update a full
    // 32-byte key (that's what add_contact is for), but it must never *erase* a key we already hold, which
    // would be persisted below and break subsequent DMs with PKI_SEND_FAIL_PUBLIC_KEY.
    const meshtastic_NodeInfoLite_public_key_t storedKey = info->public_key;
    TypeConversions::CopyUserToNodeInfoLite(info, contact.user);
    if (storedKey.size == 32 && info->public_key.size != 32) {
        LOG_INFO("Contact 0x%08x has no key, keep the stored one", contact.node_num);
        info->public_key = storedKey;
    }
    if (contact.should_ignore) {
        // Block the contact and drop its rich satellite data, but keep the
        // public key copied above - an ignored peer keeps a usable identity
        // (a verifiable target) rather than a bare node number.
        if (!setProtectedFlag(info, NODEINFO_BITFIELD_IS_IGNORED_MASK, true))
            LOG_WARN(PROTECTED_CAP_WARN_FMT, "ignore", contact.node_num, MAX_NUM_NODES - 2);
        nodeInfoLiteSetBit(info, NODEINFO_BITFIELD_IS_FAVORITE_MASK, false);
        eraseNodeSatellites(contact.node_num);
#if HAS_SCREEN || defined(MESHTASTIC_INCLUDE_NICHE_GRAPHICS)
        // History lives in a separate file and must follow, never precede,
        // the durable ignored flag. AdminModule applies it after COMMIT; direct
        // persistent callers do so below only after nodes.proto verifies.
#endif
    } else {
        /* Clients are sending add_contact before every text message DM (because clients may hold a larger node database with
         * public keys than the radio holds). However, we don't want to update last_heard just because we sent someone a DM!
         */

        /* "Boring old nodes" are the first to be evicted out of the node database when full. This includes a newly-zeroed
         * nodeinfo because it has: !is_favorite && last_heard==0. To keep this from happening when we addFromContact, we set the
         * new node as a favorite, and we leave last_heard alone (even if it's zero).
         */
        if (config.device.role == meshtastic_Config_DeviceConfig_Role_CLIENT_BASE) {
            // Special case for CLIENT_BASE: is_favorite has special meaning, and we don't want to automatically set it
            // without the user doing so deliberately. We don't normally expect users to use a CLIENT_BASE to send DMs or to add
            // contacts, but we should make sure it doesn't auto-favorite in case they do. Instead, as a workaround, we'll
            // stamp the contact as heard now, so that the add_contact node doesn't immediately get evicted.
            stampContactHeardNow(info);
        } else {
            // Normal case: set is_favorite to prevent expiration.
            // last_heard will remain as-is (or remain 0 if this entry wasn't in the nodeDB).
            // If the protected cap refuses the favorite, fall back to a heard-now stamp so the
            // contact still isn't the first eviction victim.
            if (!setProtectedFlag(info, NODEINFO_BITFIELD_IS_FAVORITE_MASK, true)) {
                LOG_WARN(PROTECTED_CAP_WARN_FMT, "favorite", contact.node_num, MAX_NUM_NODES - 2);
                stampContactHeardNow(info);
            }
        }

        // As the clients will begin sending the contact with DMs, we want to strictly check if the node is manually verified
        if (contact.manually_verified) {
            if (!setProtectedFlag(info, NODEINFO_BITFIELD_IS_KEY_MANUALLY_VERIFIED_MASK, true))
                LOG_WARN(PROTECTED_CAP_WARN_FMT, "verify", contact.node_num, MAX_NUM_NODES - 2);
        }
        // Mark the node's key as manually verified to indicate trustworthiness.
        updateGUIforNode = info;
        sortMeshDB();
        notifyObservers(true); // Force an update whether or not our node counts have changed
    }
    const bool saved = !persist || saveNodeDatabaseToDisk();
#if HAS_SCREEN || defined(MESHTASTIC_INCLUDE_NICHE_GRAPHICS)
    if (saved && persist && contact.should_ignore)
        messageStore.deleteAllMessagesFromNode(contact.node_num);
#endif
    return saved;
}

/** Update user info and channel for this node based on received user data
 */
bool NodeDB::updateUser(uint32_t nodeId, meshtastic_User &p, uint8_t channelIndex, bool xeddsaSigned, bool persist)
{
    // Only a signed update may change the identity of a proven signer; our own record is exempt.
    // Checked before getOrCreateMeshNode so a refusal cannot evict; isKnownXeddsaSigner covers the warm tier.
    if (nodeId != getNodeNum() && isKnownXeddsaSigner(nodeId) && !xeddsaSigned) {
        LOG_WARN("Refuse unsigned identity update for 0x%08x that previously signed", nodeId);
        return false;
    }

    meshtastic_NodeInfoLite *info = getOrCreateMeshNode(nodeId);
    if (!info) {
        return false;
    }

#if !(MESHTASTIC_EXCLUDE_PKI)
    if (p.public_key.size == 32 && nodeId != nodeDB->getNodeNum()) {
        printBytes("Incoming Pubkey: ", p.public_key.bytes, 32);

        // Alert the user if a remote node is advertising public key that matches our own
        if (owner.public_key.size == 32 && memcmp(p.public_key.bytes, owner.public_key.bytes, 32) == 0) {
            if (!duplicateWarned) {
                duplicateWarned = true;
                // Sanitize before embedding long_name in the phone-facing ClientNotification string
                // (defense-in-depth vs PB_VALIDATE_UTF8).
                char safeName[sizeof(p.long_name)];
                strncpy(safeName, p.long_name, sizeof(safeName));
                safeName[sizeof(safeName) - 1] = '\0';
                sanitizeUtf8(safeName, sizeof(safeName));
                char warning[] =
                    "Remote device %s has advertised your public key. This may indicate a compromised key. You may need "
                    "to regenerate your public keys.";
                LOG_WARN(warning, safeName);
                meshtastic_ClientNotification *cn = clientNotificationPool.allocZeroed();
                if (cn) {
                    cn->level = meshtastic_LogRecord_Level_WARNING;
                    cn->time = getValidTime(RTCQualityFromNet);
                    snprintf(cn->message, sizeof(cn->message), warning, safeName);
                    service->sendClientNotification(cn);
                }
            }
            return false;
        }
    }
    if (info->public_key.size == 32) { // if we have a key for this user already, don't overwrite with a new one
        // if the key doesn't match, don't update nodeDB at all.
        if (p.public_key.size != 32 || (memcmp(p.public_key.bytes, info->public_key.bytes, 32) != 0)) {
            LOG_WARN("Public Key mismatch, drop NodeInfo");
            return false;
        }
        LOG_INFO("Public Key set, not updating");
    } else if (p.public_key.size == 32) {
        LOG_INFO("Update Node Pubkey");
    }
#endif

    // Always ensure user.id is derived from nodeId, regardless of what was received
    snprintf(p.id, sizeof(p.id), "!%08x", nodeId);

    meshtastic_NodeInfoLite before = *info;
    TypeConversions::CopyUserToNodeInfoLite(info, p);
    bool changed =
        (memcmp(before.long_name, info->long_name, sizeof(info->long_name)) != 0) ||
        (memcmp(before.short_name, info->short_name, sizeof(info->short_name)) != 0) || (before.hw_model != info->hw_model) ||
        (before.role != info->role) || (before.public_key.size != info->public_key.size) ||
        (info->public_key.size > 0 && memcmp(before.public_key.bytes, info->public_key.bytes, info->public_key.size) != 0) ||
        (before.bitfield != info->bitfield) || (info->channel != channelIndex);

    if (info->public_key.size == 32) {
        printBytes("Saved Pubkey: ", info->public_key.bytes, 32);
    }
    if (nodeId != getNodeNum())
        info->channel = channelIndex; // Set channel we need to use to reach this node (but don't set our own channel)
    LOG_DEBUG("Update changed=%d user %s/%s, id=0x%08x, channel=%d", changed, info->long_name, info->short_name, nodeId,
              info->channel);

    if (changed) {
        updateGUIforNode = info;
        notifyObservers(true); // Force an update whether or not our node counts have changed

        // We just changed something about a User,
        // store our DB unless we just did so less than a minute ago

        if (persist && !Throttle::isWithinTimespanMs(lastNodeDbSave, ONE_MINUTE_MS)) {
            saveToDisk(SEGMENT_NODEDATABASE);
            lastNodeDbSave = millis();
        } else if (persist) {
            LOG_DEBUG("Defer NodeDB saveToDisk");
        }
    }

#if HAS_TRAFFIC_MANAGEMENT
    // Write-through: every accepted remote-identity commit lands here (NodeInfoModule,
    // MeshService, and TMM's requester learning all funnel through updateUser; the two
    // key-write sites that bypass it call onNodeKeyCommitted instead), so TMM's NodeInfo
    // cache reflects the commit immediately rather than at the next reconcile pass. Runs on
    // acceptance, not on `changed`: an identical update still proves the identity is
    // current. `p` is the post-hygiene payload; signerKnown transfers only key-matched
    // verified-signer status (isVerifiedSignerForKey semantics), never a bare node flag.
    if (nodeId != getNodeNum() && trafficManagementModule) {
        const bool signerKnown = p.public_key.size == 32 && isVerifiedSignerForKey(nodeId, p.public_key.bytes);
        trafficManagementModule->onNodeIdentityCommitted(nodeId, p, signerKnown);
    }
#endif

    return changed;
}

/// given a subpacket sniffed from the network, update our DB state
/// we updateGUI and updateGUIforNode if we think our this change is big enough for a redraw
void NodeDB::updateFrom(const meshtastic_MeshPacket &mp)
{
    if (mp.from == getNodeNum()) {
        LOG_DEBUG("Ignore update from self");
        return;
    }
    if (mp.which_payload_variant == meshtastic_MeshPacket_decoded_tag && mp.from) {
        LOG_TRACE("Update DB node 0x%08x, rx_time=%u", mp.from, mp.rx_time);

        // mp.from is unauthenticated, so rate-limit admission once the database is full: otherwise
        // invented node numbers churn it at packet rate and push real neighbours out.
        meshtastic_NodeInfoLite *info = getMeshNode(getFrom(&mp));
        if (!info) {
            if (isFull()) {
                if (Throttle::isWithinTimespanMs(lastFullEvictionMs, NODEDB_FULL_EVICTION_INTERVAL_MS)) {
                    LOG_DEBUG("Node database full, defer admitting 0x%08x", mp.from);
                    return;
                }
                lastFullEvictionMs = millis();
            }
            info = getOrCreateMeshNode(getFrom(&mp));
        }
        if (!info) {
            return;
        }

        // Gate on has_rx_time, not truthiness - rx_time may hold an uptime-seconds placeholder.
        if (mp.has_rx_time)
            info->last_heard = mp.rx_time;
        else
            // rx_time is the arrival instant in uptime seconds. It goes to the RAM sidecar, not
            // last_heard, which only ever holds a real epoch or 0.
            recordHeardWhileClockUntrusted(getFrom(&mp), mp.rx_time);

        // Gate on the packet actually having been received over our own radio, not on rx_snr being
        // truthy, because 0 dB is valid. TRANSPORT_LORA is set only on the real over-the-air RX path
        // (RadioInterface.cpp); it excludes TRANSPORT_INTERNAL and TRANSPORT_MQTT, while still accepting
        // an MQTT-origin packet that a gateway rebroadcasts onto LoRa - we genuinely measured that one
        // ourselves. Mirrors hop histogram below.
        // Belt-and-braces: also require has_rx_rssi, which every genuine RF-reception site sets
        // unconditionally alongside rx_snr - unlike PhoneAPI's replay packets, which set TRANSPORT_LORA
        // too (so the client treats restored history as if heard over the air) but never has_rx_rssi.
        // Replay packets don't reach updateFrom() today; this check guards against a future change that
        // routes them back through this path silently recording a replayed rx_snr as a fresh measurement.
        if (mp.transport_mechanism == meshtastic_MeshPacket_TransportMechanism_TRANSPORT_LORA && mp.has_rx_rssi) {
            info->snr = mp.rx_snr; // keep the most recent SNR we received for this node.
            nodeInfoLiteSetBit(info, NODEINFO_BITFIELD_HAS_SNR_MASK, true);
        }

        nodeInfoLiteSetBit(info, NODEINFO_BITFIELD_VIA_MQTT_MASK,
                           mp.via_mqtt); // Store if we received this packet via MQTT

#if HAS_VARIABLE_HOPS
        // Only sample genuine RF-origin packets. The transport check excludes packets received
        // directly from the broker (TRANSPORT_MQTT), but an MQTT-origin packet rebroadcast onto
        // LoRa by a gateway arrives as TRANSPORT_LORA with via_mqtt set - count those would
        // inflate the local mesh-size estimate with non-RF nodes (and they usually carry
        // hop_start==0, landing in the hop-0 bucket that pulls the recommendation lowest), so
        // exclude via_mqtt too.
        //
        // The std::max clamp below is deliberate, not a bug: Counting an unproven-but-real neighbor as 0 hops is the
        // conservative direction. This intentionally does not agree with the `hopsAway >= 0`
        // gate below, which rejects the same -1 rather than storing it as a fabricated 0 -
        // the histogram and the stored hops_away serve different purposes.
        if (mp.transport_mechanism == meshtastic_MeshPacket_TransportMechanism_TRANSPORT_LORA && !mp.via_mqtt &&
            hopScalingModule) {
            uint8_t hopCount = std::max(int8_t(0), getHopsAway(mp));
            hopScalingModule->samplePacketForHistogram(mp.from, hopCount);
        }
#endif

        // If hopStart was set and there wasn't someone messing with the limit in the middle, add hopsAway
        const int8_t hopsAway = getHopsAway(mp);
        if (hopsAway >= 0) {
            info->has_hops_away = true;
            info->hops_away = hopsAway;
        }
        sortMeshDB();
    }
}

int NodeDB::numProtectedNodes() const
{
    int count = 0;
    for (int i = 0; i < numMeshNodes; i++)
        if (nodeInfoLiteIsProtected(&meshNodes->at(i)))
            count++;
    return count;
}

bool NodeDB::setProtectedFlag(meshtastic_NodeInfoLite *node, uint32_t mask, bool on)
{
    if (!node)
        return false;
    if (!on) {
        nodeInfoLiteSetBit(node, mask, false);
        return true;
    }
    // Adding a flag to a node that is already protected doesn't grow the
    // protected set, so it's always allowed. A newly-protected node is refused
    // once the protected set has reached MAX_NUM_NODES-2, leaving two evictable
    // slots so getOrCreateMeshNode can always make room.
    if (nodeInfoLiteIsProtected(node) || numProtectedNodes() < MAX_NUM_NODES - 2) {
        nodeInfoLiteSetBit(node, mask, true);
        return true;
    }
    return false;
}

bool NodeDB::set_favorite(bool is_favorite, uint32_t nodeId, bool persist)
{
    meshtastic_NodeInfoLite *lite = getMeshNode(nodeId);
    if (!lite)
        return false;
    if (nodeInfoLiteIsFavorite(lite) == is_favorite)
        return true; // already in the requested state
    if (setProtectedFlag(lite, NODEINFO_BITFIELD_IS_FAVORITE_MASK, is_favorite)) {
        sortMeshDB();
        return !persist || saveNodeDatabaseToDisk();
    }
    LOG_WARN(PROTECTED_CAP_WARN_FMT, "favorite", nodeId, MAX_NUM_NODES - 2);
    return false;
}

bool NodeDB::isFavorite(uint32_t nodeId)
{
    // returns true if nodeId is_favorite; false if not or not found

    // NODENUM_BROADCAST will never be in the DB
    if (nodeId == NODENUM_BROADCAST)
        return false;

    const meshtastic_NodeInfoLite *lite = getMeshNode(nodeId);

    if (lite) {
        return nodeInfoLiteIsFavorite(lite);
    }
    return false;
}

bool NodeDB::isFromOrToFavoritedNode(const meshtastic_MeshPacket &p)
{
    // This method is logically equivalent to:
    //   return isFavorite(p.from) || isFavorite(p.to);
    // but is more efficient by:
    //   1. doing only one pass through the database, instead of two
    //   2. exiting early when a favorite is found, or if both from and to have been seen

    if (p.to == NODENUM_BROADCAST)
        return isFavorite(p.from); // we never store NODENUM_BROADCAST in the DB, so we only need to check p.from

    meshtastic_NodeInfoLite *lite = NULL;

    bool seenFrom = false;
    bool seenTo = false;

    for (int i = 0; i < numMeshNodes; i++) {
        lite = &meshNodes->at(i);

        if (lite->num == p.from) {
            if (nodeInfoLiteIsFavorite(lite))
                return true;

            seenFrom = true;
        }

        if (lite->num == p.to) {
            if (nodeInfoLiteIsFavorite(lite))
                return true;

            seenTo = true;
        }

        if (seenFrom && seenTo)
            return false; // we've seen both, and neither is a favorite, so we can stop searching early

        // Note: if we knew that sortMeshDB was always called after any change to is_favorite, we could exit early after searching
        // all favorited nodes first.
    }

    return false;
}

void NodeDB::pause_sort(bool paused)
{
    sortingIsPaused = paused;
}

void NodeDB::sortMeshDB()
{
    if (!sortingIsPaused && (lastSort == 0 || !Throttle::isWithinTimespanMs(lastSort, 1000 * 5))) {
        lastSort = millis();
        bool changed = true;
        while (changed) { // dumb reverse bubble sort, but probably not bad for what we're doing
            changed = false;
            for (int i = numMeshNodes - 1; i > 0; i--) { // lowest case this should examine is i == 1
                if (meshNodes->at(i - 1).num == getNodeNum()) {
                    // noop
                } else if (meshNodes->at(i).num ==
                           getNodeNum()) { // in the oddball case our own node num is not at location 0, put it there
                    // TODO: Look for at(i-1) also matching own node num, and throw the DB in the trash
                    std::swap(meshNodes->at(i), meshNodes->at(i - 1));
                    changed = true;
                } else if (nodeInfoLiteIsFavorite(&meshNodes->at(i)) && !nodeInfoLiteIsFavorite(&meshNodes->at(i - 1))) {
                    std::swap(meshNodes->at(i), meshNodes->at(i - 1));
                    changed = true;
                } else if (!nodeInfoLiteIsFavorite(&meshNodes->at(i)) && nodeInfoLiteIsFavorite(&meshNodes->at(i - 1))) {
                    // noop
                } else if (meshNodes->at(i).last_heard > meshNodes->at(i - 1).last_heard) {
                    std::swap(meshNodes->at(i), meshNodes->at(i - 1));
                    changed = true;
                }
            }
        }
        LOG_INFO("Sort took %u ms", millis() - lastSort);
    }
}

uint8_t NodeDB::getMeshNodeChannel(NodeNum n)
{
    const meshtastic_NodeInfoLite *info = getMeshNode(n);
    if (!info) {
        return 0; // defaults to PRIMARY
    }
    return info->channel;
}

std::string NodeDB::getNodeId() const
{
    char nodeId[16];
    snprintf(nodeId, sizeof(nodeId), "!%08x", myNodeInfo.my_node_num);
    return std::string(nodeId);
}

/// Find a node in our DB, return null for missing
/// NOTE: This function might be called from an ISR
meshtastic_NodeInfoLite *NodeDB::getMeshNode(NodeNum n)
{
    for (int i = 0; i < numMeshNodes; i++)
        if (meshNodes->at(i).num == n)
            return &meshNodes->at(i);

    return NULL;
}

ResolvedNode NodeDB::resolveLastByte(uint8_t lastByte, bool requireDirectNeighbor)
{
    ResolvedNode result; // defaults to {None, 0}

    // 0 is the NO_RELAY_NODE / NO_NEXT_HOP_PREFERENCE sentinel (also what MQTT-sourced packets carry
    // when hop_start==0). getLastByteOfNodeNum() never yields 0, so nothing can legitimately match.
    if (lastByte == 0)
        return result;

    const NodeNum self = getNodeNum();
    NodeNum firstMatch = 0;
    uint8_t matches = 0;

    for (size_t i = 0; i < numMeshNodes; i++) {
        const meshtastic_NodeInfoLite *node = &meshNodes->at(i);

        // Candidate gate: never resolve to ourselves, the sentinels, or an ignored node.
        if (node->num == self || node->num == 0 || node->num == NODENUM_BROADCAST)
            continue;
        if (nodeInfoLiteIsIgnored(node))
            continue;
        if (getLastByteOfNodeNum(node->num) != lastByte) // cheapest discriminator last
            continue;

        // Relevance gate: is this node a plausible relay for the requested scope?
        bool relevant;
        if (requireDirectNeighbor) {
            relevant = node->has_hops_away && node->hops_away == 0 && sinceLastSeen(node) < NEXTHOP_NEIGHBOR_FRESH_SECS;
        } else {
            const bool directNeighbor = node->has_hops_away && node->hops_away == 0;
            const bool routerRole =
                IS_ONE_OF(node->role, meshtastic_Config_DeviceConfig_Role_ROUTER, meshtastic_Config_DeviceConfig_Role_ROUTER_LATE,
                          meshtastic_Config_DeviceConfig_Role_CLIENT_BASE);
            relevant = directNeighbor || nodeInfoLiteIsFavorite(node) || routerRole;
        }
        if (!relevant)
            continue;

        if (++matches == 1) {
            firstMatch = node->num;
        } else {
            // A second relevant candidate shares this byte: ambiguous. No further scanning can
            // change that, so stop early and report the collision.
            result.status = LastByteResolution::Ambiguous;
            result.num = 0;
            return result;
        }
    }

    if (matches == 1) {
        result.status = LastByteResolution::Unique;
        result.num = firstMatch;
    }
    return result;
}

bool NodeDB::resolveUniqueLastByte(uint8_t lastByte, bool requireDirectNeighbor, NodeNum *outNum)
{
    ResolvedNode r = resolveLastByte(lastByte, requireDirectNeighbor);
    if (r.status == LastByteResolution::Unique) {
        if (outNum)
            *outNum = r.num;
        return true;
    }
    return false;
}

// returns true if the maximum number of nodes is reached or we are running low on memory
bool NodeDB::isFull()
{
    return (numMeshNodes >= MAX_NUM_NODES) || (memGet.getFreeHeap() < MINIMUM_SAFE_FREE_HEAP);
}

uint32_t NodeDB::hotNodeLastHeard(NodeNum n) const
{
    for (int i = 0; i < numMeshNodes; i++)
        if (meshNodes->at(i).num == n)
            return meshNodes->at(i).last_heard;
    return 0;
}

bool NodeDB::copyPublicKeyAuthoritative(NodeNum n, meshtastic_NodeInfoLite_public_key_t &out)
{
    const meshtastic_NodeInfoLite *info = getMeshNode(n);
    if (info && info->public_key.size == 32) {
        out = info->public_key;
        return true;
    }
#if WARM_NODE_COUNT > 0
    if (warmStore.copyKey(n, out.bytes)) {
        out.size = 32;
        return true;
    }
#endif
    return false;
}

bool NodeDB::copyPublicKey(NodeNum n, meshtastic_NodeInfoLite_public_key_t &out)
{
    if (copyPublicKeyAuthoritative(n, out))
        return true;
#if HAS_TRAFFIC_MANAGEMENT
    // Last resort: a key the TrafficManagement NodeInfo cache learned from an observed frame
    // for a node no longer in either NodeDB tier. This extends the pool of peers we can
    // encrypt to. Keys here may be trust-on-first-use (see copyPublicKey's keyProven), the
    // same first-contact trust NodeDB itself applies via updateUser().
    if (trafficManagementModule && trafficManagementModule->copyPublicKey(n, out.bytes)) {
        out.size = 32;
        return true;
    }
#endif
    return false;
}

bool NodeDB::copyPublicKeyForDecrypt(NodeNum n, meshtastic_NodeInfoLite_public_key_t &out)
{
    if (copyPublicKeyAuthoritative(n, out))
        return true;
#if HAS_TRAFFIC_MANAGEMENT
    // A cold-tier cache key backs an authenticated decrypt only when key-proven; unverified TOFU
    // cache keys must not. Outbound encryption still uses the opportunistic copyPublicKey().
    bool keyProven = false;
    if (trafficManagementModule && trafficManagementModule->copyPublicKey(n, out.bytes, &keyProven) && keyProven) {
        out.size = 32;
        return true;
    }
#endif
    return false;
}

bool NodeDB::isVerifiedSignerForKey(NodeNum n, const uint8_t *key32)
{
    if (!key32)
        return false;
    // Hot store is authoritative when present; a node lives in the hot XOR warm tier, so if the
    // hot store holds it the warm tier does not, and we decide entirely from the hot entry.
    const meshtastic_NodeInfoLite *info = getMeshNode(n);
    if (info)
        return info->public_key.size == 32 && nodeInfoLiteHasXeddsaSigned(info) && memcmp(info->public_key.bytes, key32, 32) == 0;
#if WARM_NODE_COUNT > 0
    uint8_t warmKey[32];
    if (warmStore.copyKey(n, warmKey) && memcmp(warmKey, key32, 32) == 0)
        return warmStore.hasXeddsaSigned(n);
#endif
    return false;
}

bool NodeDB::isKnownXeddsaSigner(NodeNum n)
{
    // A node lives in the hot XOR warm tier, so the hot verdict is final when present.
    const meshtastic_NodeInfoLite *info = getMeshNode(n);
    if (info)
        return nodeInfoLiteHasXeddsaSigned(info);
#if WARM_NODE_COUNT > 0
    return warmStore.hasXeddsaSigned(n);
#else
    return false;
#endif
}

void NodeDB::commitRemoteKey(NodeNum n, const uint8_t key32[32], KeyCommitTrust trust)
{
    if (!key32 || n == 0)
        return;
    // Local copy first: callers may pass the node's own key bytes back in (e.g. manual
    // verification re-committing an already-stored key), and memcpy forbids overlap.
    uint8_t key[32];
    memcpy(key, key32, 32);

    meshtastic_NodeInfoLite *info = getOrCreateMeshNode(n);
    if (!info)
        return;
    // Unconditional overwrite - deliberately NOT updateUser()'s "don't replace a known key" pin.
    // That pin protects against unauthenticated NodeInfo broadcasts; the only callers here are
    // possession/authority-proven (ManuallyVerified = user confirmed the key; AdminChannelProven =
    // decrypted via the admin key with p->from bound into the AEAD nonce), i.e. exactly the paths
    // meant to establish or rotate a key. Keep new call sites to that same trust bar.
    memcpy(info->public_key.bytes, key, 32);
    info->public_key.size = 32;

#if HAS_TRAFFIC_MANAGEMENT
    // Write-through, mirroring updateUser()'s identity hook: without it the TrafficManagement
    // NodeInfo cache diverges until the next hourly reconcile.
    if (trafficManagementModule)
        trafficManagementModule->onNodeKeyCommitted(n, key, trust == KeyCommitTrust::ManuallyVerified);
#endif
}

meshtastic_Config_DeviceConfig_Role NodeDB::getNodeRole(NodeNum n)
{
    const meshtastic_NodeInfoLite *info = getMeshNode(n);
    if (nodeInfoLiteHasUser(info))
        return info->role;
#if WARM_NODE_COUNT > 0
    // Hot-store miss: fall back to the role the warm tier cached at eviction.
    uint8_t role = 0, prot = 0;
    if (warmStore.lookupMeta(n, role, prot))
        return static_cast<meshtastic_Config_DeviceConfig_Role>(role);
#endif
    return meshtastic_Config_DeviceConfig_Role_CLIENT;
}

void NodeDB::recordHeardWhileClockUntrusted(NodeNum num, uint32_t heardAtUptime)
{
    // Update in place if the node already has a stamp.
    for (auto &h : heardAt) {
        if (h.num == num) {
            h.heardAtUptimeSecs = heardAtUptime;
            return;
        }
    }
    // Otherwise take an empty slot, or reuse the oldest stamp.
    NodeHeardAt *victim = &heardAt[0];
    for (auto &h : heardAt) {
        if (h.num == 0) {
            victim = &h;
            break;
        }
        if (h.heardAtUptimeSecs < victim->heardAtUptimeSecs)
            victim = &h;
    }
    victim->num = num;
    victim->heardAtUptimeSecs = heardAtUptime;
}

bool NodeDB::getHeardAtUptimeSecs(NodeNum num, uint32_t &stamp) const
{
    for (const auto &h : heardAt) {
        if (h.num == num) {
            stamp = h.heardAtUptimeSecs;
            return true;
        }
    }
    return false;
}

NodeDB::EvictionRecency NodeDB::evictionRecency(const meshtastic_NodeInfoLite *n) const
{
    uint32_t stamp = 0;
    const bool heardThisBoot = getHeardAtUptimeSecs(n->num, stamp);
    return {heardThisBoot ? stamp : n->last_heard, heardThisBoot};
}

bool NodeDB::evictionRecencyOlder(EvictionRecency candidate, EvictionRecency incumbent)
{
    if (candidate.heardThisBoot != incumbent.heardThisBoot)
        return !candidate.heardThisBoot;
    return candidate.value < incumbent.value;
}

void NodeDB::stampContactHeardNow(meshtastic_NodeInfoLite *info)
{
    const uint32_t nowEpoch = getValidTime(RTCQualityFromNet);
    if (nowEpoch)
        info->last_heard = nowEpoch;
    else
        recordHeardWhileClockUntrusted(info->num, Time::getUptimeSecs());
}

void NodeDB::backfillHeardAt()
{
    const uint32_t nowEpoch = getValidTime(RTCQualityFromNet);
    if (nowEpoch == 0) // called before the clock was actually valid - nothing to date against
        return;
    const uint32_t nowUptimeSecs = Time::getUptimeSecs();
    for (auto &h : heardAt) {
        if (h.num == 0)
            continue;
        meshtastic_NodeInfoLite *info = getMeshNode(h.num);
        if (info) {
            // Both stamps are monotonic uptime seconds, so the elapsed term is exact at any age.
            // Never move last_heard backwards: the node may since have been re-heard on a good clock.
            const uint32_t elapsedSecs = nowUptimeSecs - h.heardAtUptimeSecs;
            if (elapsedSecs < nowEpoch && nowEpoch - elapsedSecs > info->last_heard)
                info->last_heard = nowEpoch - elapsedSecs;
        }
        h = {}; // evicted or converted either way, the stamp's job is done
    }
}

/// Find a node in our DB, create an empty NodeInfo if missing
meshtastic_NodeInfoLite *NodeDB::getOrCreateMeshNode(NodeNum n)
{
    meshtastic_NodeInfoLite *lite = getMeshNode(n);

    if (!lite) {
        if (isFull()) {
            LOG_INFO("Node database full: %i nodes, %u bytes free. Erase oldest", numMeshNodes, memGet.getFreeHeap());
            // look for oldest node and erase it
            // Newest-possible sentinel: a zeroed init ranks older than every candidate, so nothing
            // would ever be selected. Keep it maximal even though the index guards below also cover it.
            EvictionRecency oldest = {UINT32_MAX, true};
            EvictionRecency oldestBoring = {UINT32_MAX, true};
            int oldestIndex = -1;
            int oldestBoringIndex = -1;
            for (int i = 1; i < numMeshNodes; i++) {
                const meshtastic_NodeInfoLite *cand = &meshNodes->at(i);
                const bool isFavoriteNode = nodeInfoLiteIsFavorite(cand);
                const bool isIgnored = nodeInfoLiteIsIgnored(cand);
                const bool isVerified = nodeInfoLiteIsKeyManuallyVerified(cand);
                // last_heard, except that nodes heard this boot before the clock became trusted
                // rank by their RAM arrival stamp instead of the 0 in the stored field.
                const EvictionRecency candRecency = evictionRecency(cand);
                // Simply the oldest non-favorite, non-ignored, non-verified node
                if (!isFavoriteNode && !isIgnored && !isVerified &&
                    (oldestIndex == -1 || evictionRecencyOlder(candRecency, oldest))) {
                    oldest = candRecency;
                    oldestIndex = i;
                }
                // The oldest "boring" node
                if (!isFavoriteNode && !isIgnored && cand->public_key.size == 0 &&
                    (oldestBoringIndex == -1 || evictionRecencyOlder(candRecency, oldestBoring))) {
                    oldestBoring = candRecency;
                    oldestBoringIndex = i;
                }
            }
            // if we found a "boring" node, evict it
            if (oldestBoringIndex != -1) {
                oldestIndex = oldestBoringIndex;
            }

            if (oldestIndex != -1) {
                const meshtastic_NodeInfoLite &evicted = meshNodes->at(oldestIndex);
#if WARM_NODE_COUNT > 0
                // Demote to the warm tier so the identity (and crucially the
                // PKI key) outlives the hot-store slot.
                warmStore.absorb(evicted.num, evicted.last_heard, evicted.public_key.size == 32 ? evicted.public_key.bytes : NULL,
                                 evicted.role, warmProtectedCategory(evicted), nodeInfoLiteHasXeddsaSigned(&evicted));
#endif
                eraseNodeSatellites(evicted.num);
                // Shove the remaining nodes down the chain
                for (int i = oldestIndex; i < numMeshNodes - 1; i++) {
                    meshNodes->at(i) = meshNodes->at(i + 1);
                }
                (numMeshNodes)--;
            }
        }
        // Don't append past the end of the vector. The protected-node cap
        // (numProtectedNodes() <= MAX_NUM_NODES-2) means the eviction above frees
        // a slot in normal operation; this guards the legacy case of a pre-cap
        // database that is full of protected nodes - refuse rather than overrun.
        if (numMeshNodes >= MAX_NUM_NODES)
            return NULL;
        // Pre-size before append when run before nodeDBSelfCare() (boot keygen); else at() aborts on nRF52.
        if (static_cast<size_t>(numMeshNodes) >= meshNodes->size())
            meshNodes->resize(numMeshNodes + 1);
        // add the node at the end
        lite = &meshNodes->at((numMeshNodes)++);

        // everything is missing except the nodenum
        memset(lite, 0, sizeof(*lite));
        lite->num = n;
#if WARM_NODE_COUNT > 0
        // Re-admission: restore what the warm tier kept for this node
        WarmNodeEntry warm;
        if (warmStore.take(n, warm)) {
            lite->last_heard = warmTimeOf(warm); // mask off the stolen metadata bits
            // Restore the role the warm tier cached, so re-admission isn't stuck at CLIENT
            // until the next NodeInfo arrives.
            lite->role = static_cast<meshtastic_Config_DeviceConfig_Role>(warmRoleOf(warm));
            // Restore the XEdDSA-signed bit too: it is learned from verified traffic, not from
            // NodeInfo, so a round trip through the warm tier must not relearn it from zero.
            nodeInfoLiteSetBit(lite, NODEINFO_BITFIELD_HAS_XEDDSA_SIGNED_MASK, warmXeddsaSignedOf(warm));
            if (!memfll(warm.public_key, 0, sizeof(warm.public_key))) {
                lite->public_key.size = 32;
                memcpy(lite->public_key.bytes, warm.public_key, 32);
            }
            if (warmProtOf(warm) == static_cast<uint8_t>(WarmProtected::XeddsaSigner))
                nodeInfoLiteSetBit(lite, NODEINFO_BITFIELD_HAS_XEDDSA_SIGNED_MASK, true);
            LOG_MIGRATION("Rehydrated node 0x%08x from warm tier (key=%d)", n, lite->public_key.size == 32);
        }
#endif
#if HAS_TRAFFIC_MANAGEMENT
        // Name rehydration: the warm tier keeps a node's key but not its name, so a re-admitted
        // long-tail node is nameless until its next NodeInfo. The TrafficManagement NodeInfo
        // cache is much larger and often still holds the full User. Restore it - but only when
        // its cached key matches the key we just restored from warm, so a name never attaches to
        // a different identity than the one we encrypt to. No-op without the TMM NodeInfo cache
        // or when no key is present (key-matched by design). CopyUserToNodeInfoLite sets only the
        // user-related bits, so the warm-restored XEdDSA-signed bit survives.
        if (lite->public_key.size == 32 && !nodeInfoLiteHasUser(lite) && trafficManagementModule) {
            meshtastic_User tmmUser = meshtastic_User_init_zero;
            if (trafficManagementModule->copyUser(n, tmmUser) && tmmUser.public_key.size == 32 &&
                memcmp(tmmUser.public_key.bytes, lite->public_key.bytes, 32) == 0) {
                TypeConversions::CopyUserToNodeInfoLite(lite, tmmUser);
                LOG_INFO("Rehydrated node 0x%08x identity from TMM NodeInfo cache", n);
            }
        }
#endif
        LOG_INFO("Add node to database: %i nodes, %u bytes free", numMeshNodes, memGet.getFreeHeap());
    }

    return lite;
}

/// Sometimes we will have Position objects that only have a time, so check for
/// valid lat/lon
bool NodeDB::hasValidPosition(const meshtastic_NodeInfoLite *n)
{
    if (!n)
        return false;
    if (n->num == getNodeNum()) {
        return localPosition.latitude_i != 0 || localPosition.longitude_i != 0;
    }
    meshtastic_PositionLite pos;
    return copyNodePosition(n->num, pos) && (pos.latitude_i != 0 || pos.longitude_i != 0);
}

/// If we have a node / user and they report is_licensed = true
/// we consider them licensed
UserLicenseStatus NodeDB::getLicenseStatus(uint32_t nodeNum)
{
    const meshtastic_NodeInfoLite *info = getMeshNode(nodeNum);
    if (!nodeInfoLiteHasUser(info))
        return UserLicenseStatus::NotKnown;
    return nodeInfoLiteIsLicensed(info) ? UserLicenseStatus::Licensed : UserLicenseStatus::NotLicensed;
}

#if !defined(MESHTASTIC_EXCLUDE_PKI)
bool NodeDB::checkLowEntropyPublicKey(const meshtastic_Config_SecurityConfig_public_key_t &keyToTest)
{
    if (keyToTest.size == 32) {
        uint8_t keyHash[32] = {0};
        memcpy(keyHash, keyToTest.bytes, keyToTest.size);
        crypto->hash(keyHash, 32);
        for (uint16_t i = 0; i < sizeof(LOW_ENTROPY_HASHES) / sizeof(LOW_ENTROPY_HASHES[0]); i++) {
            if (memcmp(keyHash, LOW_ENTROPY_HASHES[i], sizeof(LOW_ENTROPY_HASHES[0])) == 0) {
                return true;
            }
        }
    }
    return false;
}
#endif

#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)
// A freshly minted keypair must not itself land on the blacklist. Fail with no key rather than persist
// a known-weak identity: only a broken entropy source can land here, and retrying would not fix that.
bool NodeDB::generateBlacklistCheckedKeyPair()
{
    crypto->generateKeyPair(config.security.public_key.bytes, config.security.private_key.bytes);
    if (!checkLowEntropyPublicKey(config.security.public_key))
        return true;
    LOG_ERROR("PKI keygen produced a known low-entropy key; entropy source is broken");
    config.security.public_key.size = 0;
    config.security.private_key.size = 0;
    crypto->restoreIdentity(nullptr);
    return false;
}

// Derive the public key from the stored private key and vet it. The entry check cannot see a weak key
// when the stored public key is absent, and a failed derivation must not leave sizes claiming a pair.
bool NodeDB::derivePublicKeyFromPrivate()
{
    config.security.public_key.size = 32;
    if (!crypto->regeneratePublicKey(config.security.public_key.bytes, config.security.private_key.bytes)) {
        LOG_ERROR("Can't generate public key from private key");
        config.security.public_key.size = 0;
        config.security.private_key.size = 0;
        return false;
    }
    if (!checkLowEntropyPublicKey(config.security.public_key))
        return true;
    keyIsLowEntropy = true;
    LOG_WARN("Private key derives a known low-entropy public key; generating a new keypair");
    return generateBlacklistCheckedKeyPair();
}
#endif

bool NodeDB::generateCryptoKeyPair(const uint8_t *privateKey)
{
#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)
    if (!shouldUseFilesystemPersistence(fsIsMounted())) {
        LOG_ERROR("NodeDB: refusing PKI key generation while filesystem is unavailable");
        return false;
    }

    // Mint a new identity only after a LoRa region is selected. Restoring an
    // existing private key is safe while region is UNSET: it enables no radio
    // traffic and prevents a temporary MAC-derived Node ID during migrations.
    // Licensed operation still needs the identity key for plaintext
    // signatures, even though the key is never used for PKI encryption.
    bool regionBlocksKeygen = config.lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET;
#if ARCH_PORTDUINO
    if (portduino_config.lora_module == use_simradio)
        regionBlocksKeygen = false;
#endif
    const bool canRestoreExistingKey = privateKey != nullptr || config.security.private_key.size == 32;
    if (regionBlocksKeygen && !canRestoreExistingKey) {
        return false;
    }

    bool keygenSuccess = false;
    // Record whether the stored key is a known compromised/low-entropy key so main.cpp can warn the
    // user. A detected low-entropy key is regenerated below, but the flag stays set so the
    // "Compromised keys were detected and regenerated" notification still fires.
    keyIsLowEntropy = checkLowEntropyPublicKey(config.security.public_key);

    // If a specific private key was provided, use it
    if (privateKey != nullptr) {
        LOG_INFO("Using provided private key for PKI");
        memcpy(config.security.private_key.bytes, privateKey, 32);
        config.security.private_key.size = 32;

        if (!derivePublicKeyFromPrivate())
            return false;
        keygenSuccess = true;
    }
    // Try to regenerate public key from existing private key if it's valid and not low entropy
    else if (config.security.private_key.size == 32 && !keyIsLowEntropy) {
        LOG_DEBUG("Regenerate PKI public key from private key");
        if (!derivePublicKeyFromPrivate())
            return false;
        keygenSuccess = true;
    } else {
        // Generate a new key pair
        LOG_INFO("Generate new PKI keys");
        config.security.public_key.size = 32;
        config.security.private_key.size = 32;
        if (!generateBlacklistCheckedKeyPair())
            return false;
        keygenSuccess = true;
    }

    // Update sizes and copy to owner if successful
    if (keygenSuccess) {
        owner.public_key.size = 32;
        memcpy(owner.public_key.bytes, config.security.public_key.bytes, 32);

        // Set the DH private key for crypto operations
        LOG_DEBUG("Set DH private key for crypto operations");
        crypto->setDHPrivateKey(config.security.private_key.bytes);

        if (createNewIdentity() && owner.is_licensed)
            licensedIdentityMigrationPending = true;
    }
    return keygenSuccess;
#else
    return false;
#endif
}

bool NodeDB::derivePublicKeyForValidation(const uint8_t privateKey[32], uint8_t publicKey[32]) const
{
#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)
    return privateKey && publicKey && derivePublicKeyWithoutInstalling(privateKey, publicKey);
#else
    (void)privateKey;
    (void)publicKey;
    return false;
#endif
}

bool NodeDB::notifyPendingLicensedIdentityMigration()
{
    if (!licensedIdentityMigrationPending || !service)
        return false;
    meshtastic_ClientNotification *notification = clientNotificationPool.allocZeroed();
    if (!notification)
        return false;
    notification->level = meshtastic_LogRecord_Level_WARNING;
    notification->time = getValidTime(RTCQualityFromNet);
    snprintf(notification->message, sizeof(notification->message), "%s", LICENSED_IDENTITY_MIGRATION_WARNING);
    service->sendClientNotification(notification);
    licensedIdentityMigrationPending = false;
    return true;
}

bool NodeDB::createNewIdentity()
{
    uint32_t oldNodeNum = getNodeNum();
    uint32_t newNodeNum = crc32Buffer(config.security.public_key.bytes, config.security.public_key.size);

    // If the key hasn't changed, nothing to do
    if (newNodeNum == oldNodeNum)
        return false;

    // Remove the old node entry entirely rather than retiring it in place. Flagging the old identity as
    // ignored (the previous behavior) left a keyless ghost of ourselves that survived cleanup/eviction, was
    // still streamed to clients, and made any DM/admin aimed at it fail forever with PKI_SEND_FAIL_PUBLIC_KEY.
    // removeNodeByNum() drops the lite entry, its satellite stores, and the warm-tier copy.
    if (getMeshNode(oldNodeNum) != NULL) {
        LOG_DEBUG("Old node num 0x%08x now 0x%08x, remove stale identity", oldNodeNum, newNodeNum);
        removeNodeByNum(oldNodeNum);
    } else {
        // Lite entry already absent: drop any orphaned satellite-store entries directly.
        eraseNodeSatellites(oldNodeNum);
    }

    myNodeInfo.my_node_num = newNodeNum;
    snprintf(owner.id, sizeof(owner.id), "!%08x", newNodeNum);

    // The number has moved, so the caller must persist it whatever happens next. Returning false here
    // would leave the new key saved against the old number, which is the break this exists to prevent.
    // Identity rotation can run inside an OPEN settings transaction. Repair
    // the hot-store invariant in RAM without attempting an out-of-generation
    // write; the caller persists NODEDATABASE with the rest of the identity.
    nodeDBSelfCare(false);
    meshtastic_NodeInfoLite *info = getMeshNode(getNodeNum());
    if (info)
        TypeConversions::CopyUserToNodeInfoLite(info, owner);
    else {
        LOG_ERROR("No room for our own node 0x%08x, identity moved without a self record", newNodeNum);
        return false;
    }

    return true;
}

bool NodeDB::ensurePkiIdentity()
{
#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)
    if (!shouldUsePersistentConfiguration(shouldUseFilesystemPersistence(fsIsMounted()), requiresConfigRecovery())) {
        LOG_ERROR("NodeDB: refusing PKI identity creation while persistent configuration is unavailable");
        return false;
    }

    // A failed or declined keygen leaves the existing key, and so the existing node num, untouched.
    if (!crypto || !crypto->ensurePkiKeys(config.security, owner))
        return false;

    // ensurePkiKeys() writes key material only, so my_node_num is still the stale MAC-derived value.
    // createNewIdentity() early-returns when the key, and so the node num, did not actually change.
    return createNewIdentity();
#else
    return false;
#endif
}

bool NodeDB::backupPreferences(meshtastic_AdminMessage_BackupLocation location)
{
#if defined(HELTEC_V4_OLED) && defined(FSCom)
    if (destructiveStorageMutationActive.load(std::memory_order_acquire)) {
        LOG_ERROR("Backup refused during destructive storage mutation");
        return false;
    }
    concurrency::LockGuard transactionGuard(&heltecPreferencesTransactionLock);
    if (destructiveStorageMutationActive.load(std::memory_order_acquire)) {
        LOG_ERROR("Backup refused during destructive storage mutation");
        return false;
    }
    if (isPreferenceEditTransactionActive()) {
        LOG_ERROR("Backup refused while a settings edit is open");
        return false;
    }
    HeltecXModemStorageGuard xmodemGuard;
    if (!xmodemGuard) {
        LOG_ERROR("Backup refused while XMODEM transfer is active");
        return false;
    }
    if (readHeltecResetPendingMarker() != HeltecResetPendingKind::NONE) {
        LOG_ERROR("Backup refused while a recovery transaction is active");
        return false;
    }
#endif
    bool success = false;
    lastBackupAttempt = millis();
#ifdef FSCom
    if (location == meshtastic_AdminMessage_BackupLocation_FLASH) {
        meshtastic_BackupPreferences backup = meshtastic_BackupPreferences_init_zero;
        backup.version = DEVICESTATE_CUR_VER;
        backup.timestamp = getValidTime(RTCQuality::RTCQualityDevice, false);
        backup.has_config = true;
        backup.config = config;
        backup.has_module_config = true;
        backup.module_config = moduleConfig;
        backup.has_channels = true;
        backup.channels = channelFile;
        backup.has_owner = true;
        backup.owner = owner;

        size_t backupSize;
        pb_get_encoded_size(&backupSize, meshtastic_BackupPreferences_fields, &backup);

        spiLock->lock();
        FSCom.mkdir("/backups");
        spiLock->unlock();
        success = saveProto(backupFileName, backupSize, &meshtastic_BackupPreferences_msg, &backup, true);

        if (success) {
            LOG_INFO("Saved backup preferences");
        } else {
            LOG_ERROR("Save backup prefs to file failed");
        }
    } else if (location == meshtastic_AdminMessage_BackupLocation_SD) {
        // TODO: After more mainline SD card support
    }
#endif
    return success;
}

bool NodeDB::removeBackupPreferences(meshtastic_AdminMessage_BackupLocation location)
{
#if defined(HELTEC_V4_OLED) && defined(FSCom)
    if (destructiveStorageMutationActive.load(std::memory_order_acquire)) {
        LOG_ERROR("Backup removal refused during destructive storage mutation");
        return false;
    }
    concurrency::LockGuard transactionGuard(&heltecPreferencesTransactionLock);
    if (destructiveStorageMutationActive.load(std::memory_order_acquire)) {
        LOG_ERROR("Backup removal refused during destructive storage mutation");
        return false;
    }
    if (isPreferenceEditTransactionActive()) {
        LOG_ERROR("Backup removal refused while a settings edit is open");
        return false;
    }
    HeltecXModemStorageGuard xmodemGuard;
    if (!xmodemGuard) {
        LOG_ERROR("Backup removal refused while XMODEM transfer is active");
        return false;
    }
    if (readHeltecResetPendingMarker() != HeltecResetPendingKind::NONE || requiresConfigRecovery()) {
        LOG_ERROR("Backup removal refused while configuration recovery is active");
        return false;
    }
#endif
#ifdef FSCom
    if (location == meshtastic_AdminMessage_BackupLocation_FLASH) {
        concurrency::LockGuard guard(spiLock);
#if defined(HELTEC_V4_OLED)
        String temporaryPath = backupFileName;
        temporaryPath += ".tmp";
        return removeHeltecFileChecked(backupFileName) && removeHeltecFileChecked(temporaryPath.c_str()) &&
               !FSCom.exists(backupFileName) && !FSCom.exists(temporaryPath.c_str());
#else
        return !FSCom.exists(backupFileName) || FSCom.remove(backupFileName);
#endif
    }
#endif
    return false;
}

bool NodeDB::restorePreferences(meshtastic_AdminMessage_BackupLocation location, int restoreWhat)
{
#if defined(HELTEC_V4_OLED) && defined(FSCom)
    if (rebootAtMsec != 0 || shutdownAtMsec != 0) {
        LOG_ERROR("Restore refused while reboot/shutdown is pending");
        return false;
    }
    bool expectedInactive = false;
    if (!destructiveStorageMutationActive.compare_exchange_strong(expectedInactive, true, std::memory_order_acq_rel)) {
        LOG_ERROR("Restore refused while another destructive operation is active");
        return false;
    }
    destructiveStorageOwnerTask.store(reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle()), std::memory_order_release);
    bool keepDestructiveFenceUntilReboot = false;
    struct RestoreActivityGuard {
        std::atomic<bool> &flag;
        std::atomic<uintptr_t> &owner;
        bool &keepUntilReboot;
        ~RestoreActivityGuard()
        {
            owner.store(0, std::memory_order_release);
            if (!keepUntilReboot)
                flag.store(false, std::memory_order_release);
        }
    } activityGuard{destructiveStorageMutationActive, destructiveStorageOwnerTask, keepDestructiveFenceUntilReboot};
    if (!waitForExternalStateReaders()) {
        LOG_ERROR("Restore refused while a PhoneAPI/HTTP state reader is active");
        return false;
    }
    concurrency::LockGuard transactionGuard(&heltecPreferencesTransactionLock);
    if (isPreferenceEditTransactionActive()) {
        LOG_ERROR("Restore refused while a settings edit is open");
        return false;
    }
    HeltecXModemStorageGuard xmodemGuard;
    if (!xmodemGuard) {
        LOG_ERROR("Restore refused while XMODEM transfer is active");
        return false;
    }
#endif
#ifdef FSCom
    if (location == meshtastic_AdminMessage_BackupLocation_FLASH) {
        spiLock->lock();
        if (!FSCom.exists(backupFileName)) {
            spiLock->unlock();
            LOG_WARN("Can't restore, no backup file");
            return false;
        } else {
            spiLock->unlock();
        }
        meshtastic_BackupPreferences backup = meshtastic_BackupPreferences_init_zero;
        const LoadFileResult loadResult =
            loadProto(backupFileName, meshtastic_BackupPreferences_size, sizeof(meshtastic_BackupPreferences),
                      &meshtastic_BackupPreferences_msg, &backup);
        if (loadResult != LoadFileResult::LOAD_SUCCESS) {
            LOG_ERROR("Restore prefs from backup failed");
            return false;
        }

        constexpr int restorableSegments = SEGMENT_CONFIG | SEGMENT_MODULECONFIG | SEGMENT_DEVICESTATE | SEGMENT_CHANNELS;
        if (restoreWhat == 0 || (restoreWhat & ~restorableSegments) != 0 || backup.version < DEVICESTATE_MIN_VER ||
            backup.version > DEVICESTATE_CUR_VER || ((restoreWhat & SEGMENT_CONFIG) && !backup.has_config) ||
            ((restoreWhat & SEGMENT_MODULECONFIG) && !backup.has_module_config) ||
            ((restoreWhat & SEGMENT_DEVICESTATE) && !backup.has_owner) ||
            ((restoreWhat & SEGMENT_CHANNELS) && !backup.has_channels)) {
            LOG_ERROR("Restore prefs rejected: backup is incomplete or incompatible");
            return false;
        }
        if (((restoreWhat & SEGMENT_CONFIG) &&
             (backup.config.version < DEVICESTATE_MIN_VER || backup.config.version > DEVICESTATE_CUR_VER)) ||
            ((restoreWhat & SEGMENT_MODULECONFIG) && (backup.module_config.version < DEVICESTATE_MIN_VER ||
                                                      backup.module_config.version > POSITION_TELEMETRY_OPTIN_VER)) ||
            ((restoreWhat & SEGMENT_CHANNELS) &&
             (backup.channels.version < DEVICESTATE_MIN_VER || backup.channels.version > POSITION_TELEMETRY_OPTIN_VER ||
              !isCompleteChannelFile(backup.channels)))) {
            LOG_ERROR("Restore prefs rejected: nested backup data is invalid");
            return false;
        }

        meshtastic_LocalConfig candidateConfig = (restoreWhat & SEGMENT_CONFIG) ? backup.config : config;
        meshtastic_User candidateOwner = (restoreWhat & SEGMENT_DEVICESTATE) ? backup.owner : owner;
        if (restoreWhat & SEGMENT_CONFIG) {
            if (!candidateConfig.has_lora) {
                LOG_ERROR("Restore prefs rejected: backup config has no LoRa section");
                return false;
            }
            if (candidateConfig.lora.region == meshtastic_Config_LoRaConfig_RegionCode_UA_868) {
                LOG_INFO("Restore prefs: migrate obsolete UA_868 region to EU_868");
                candidateConfig.lora.region = meshtastic_Config_LoRaConfig_RegionCode_EU_868;
            } else if (getRegion(candidateConfig.lora.region)->code != candidateConfig.lora.region) {
                LOG_ERROR("Restore prefs rejected: backup contains an unknown LoRa region");
                return false;
            }

            const RegionInfo *candidateRegion = getRegion(candidateConfig.lora.region);
            if (candidateRegion->profile->licensedOnly && !candidateOwner.is_licensed) {
                LOG_ERROR("Restore prefs rejected: licensed LoRa region requires a licensed backup owner");
                return false;
            }
#if defined(HELTEC_V4_OLED)
            // Heltec V4 carries a sub-GHz SX1262. Keep the check exact even
            // in an unusual recovery path where RadioLibInterface::instance
            // has not yet been constructed.
            if (candidateRegion->wideLora) {
                LOG_ERROR("Restore prefs rejected: 2.4 GHz LoRa is unsupported by Heltec V4");
                return false;
            }
#endif
            char regionError[160] = {};
            if (!RadioInterface::checkConfigRegion(candidateConfig.lora, regionError, sizeof(regionError),
                                                   candidateOwner.is_licensed) ||
                !RadioInterface::validateConfigLora(candidateConfig.lora)) {
                LOG_ERROR("Restore prefs rejected: LoRa settings are not usable on this hardware (%s)", regionError);
                return false;
            }
        }
        const size_t privateKeySize = candidateConfig.has_security ? candidateConfig.security.private_key.size : 0;
        const size_t configPublicKeySize = candidateConfig.has_security ? candidateConfig.security.public_key.size : 0;
        const size_t ownerPublicKeySize = candidateOwner.public_key.size;
        const bool currentHasPersistentIdentity =
            (config.has_security && (config.security.private_key.size != 0 || config.security.public_key.size != 0)) ||
            owner.public_key.size != 0;
        if ((privateKeySize != 0 && privateKeySize != 32) || (configPublicKeySize != 0 && configPublicKeySize != 32) ||
            (ownerPublicKeySize != 0 && ownerPublicKeySize != 32) ||
            (privateKeySize == 0 && (configPublicKeySize != 0 || ownerPublicKeySize != 0)) ||
            (privateKeySize == 0 && currentHasPersistentIdentity)) {
            LOG_ERROR("Restore prefs rejected: backup identity lengths are inconsistent");
            return false;
        }

        uint32_t candidateNodeNum = myNodeInfo.my_node_num;
#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)
        if (privateKeySize == 32) {
            uint8_t derivedPublicKey[32];
            if (!derivePublicKeyWithoutInstalling(candidateConfig.security.private_key.bytes, derivedPublicKey) ||
                (configPublicKeySize == 32 &&
                 memcmp(candidateConfig.security.public_key.bytes, derivedPublicKey, sizeof(derivedPublicKey)) != 0) ||
                (ownerPublicKeySize == 32 &&
                 memcmp(candidateOwner.public_key.bytes, derivedPublicKey, sizeof(derivedPublicKey)) != 0)) {
                LOG_ERROR("Restore prefs rejected: backup keypair is inconsistent");
                return false;
            }
            candidateConfig.has_security = true;
            candidateConfig.security.public_key.size = sizeof(derivedPublicKey);
            memcpy(candidateConfig.security.public_key.bytes, derivedPublicKey, sizeof(derivedPublicKey));
            if (checkLowEntropyPublicKey(candidateConfig.security.public_key)) {
                LOG_ERROR("Restore prefs rejected: backup identity uses a compromised key");
                return false;
            }
            candidateOwner.public_key.size = sizeof(derivedPublicKey);
            memcpy(candidateOwner.public_key.bytes, derivedPublicKey, sizeof(derivedPublicKey));
            candidateNodeNum = crc32Buffer(derivedPublicKey, sizeof(derivedPublicKey));
        }
#endif

#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)
        if (privateKeySize == 32 && (!crypto || !cryptLock)) {
            LOG_ERROR("Restore prefs rejected: crypto engine is unavailable");
            return false;
        }
#endif

#if defined(HELTEC_V4_OLED)
        const bool resolvingLegacyMigration = legacyPreferencesPendingCleanup || incompleteLegacyMigrationDetected;
        const HeltecResetPendingKind pendingKind = readHeltecResetPendingMarker();
        const bool replacingInterruptedCoreTransaction =
            pendingKind == HeltecResetPendingKind::EDIT || pendingKind == HeltecResetPendingKind::CONFIG_ONLY;
        if (pendingKind == HeltecResetPendingKind::NODEDB_RESET || incompleteNodeDatabaseResetDetected) {
            LOG_ERROR("Restore prefs rejected: retry the interrupted node-db reset first");
            return false;
        }
        if (incompletePreferenceRestoreDetected && restoreWhat != restorableSegments) {
            LOG_ERROR("Restore prefs rejected: interrupted restore requires every backed-up segment");
            return false;
        }
        if (replacingInterruptedCoreTransaction && restoreWhat != restorableSegments) {
            LOG_ERROR("Restore prefs rejected: interrupted settings/reset recovery requires every backed-up segment");
            return false;
        }
        if (resolvingLegacyMigration && restoreWhat != restorableSegments) {
            LOG_ERROR("Restore prefs rejected: incomplete legacy migration requires every backed-up segment");
            return false;
        }
        if (!heltecDestructiveStoragePowerIsSafe()) {
            LOG_ERROR("Restore prefs refused: connect stable power or charge the battery");
            return false;
        }
        const bool restoredNodeDatabaseRequired = privateKeySize == 32 || candidateOwner.is_licensed;
        int saveWhat = restoreWhat;
        if (candidateNodeNum != myNodeInfo.my_node_num || (resolvingLegacyMigration && restoredNodeDatabaseRequired))
            saveWhat |= SEGMENT_DEVICESTATE | SEGMENT_NODEDATABASE;
        if ((unreadablePreferenceSegments & ~saveWhat) != 0) {
            LOG_ERROR("Restore prefs rejected: an unreadable segment is not repaired by this backup");
            return false;
        }
        if (!writeHeltecResetPendingMarker(HeltecResetPendingKind::RESTORE, true)) {
            LOG_ERROR("Restore prefs refused: pending marker could not be verified");
            return false;
        }
        keepDestructiveFenceUntilReboot = true;

        // A validated, explicit restore is authorization to replace exactly
        // these segments. Keep unrelated unreadable data protected.
        unreadablePreferenceSegments &= ~saveWhat;
        incompleteConfigResetDetected = false;
        incompletePreferenceRestoreDetected = false;
        incompleteLegacyMigrationDetected = false;
        if (restoreWhat & SEGMENT_CONFIG)
            configDecodeFailed = false;
#if HAS_WIFI
        deinitWifi();
#endif
        disableBluetooth();
#if !MESHTASTIC_EXCLUDE_GPS
        if (gps)
            gps->disable();
#endif
        if (!parkHeltecRadioForStorageMutation()) {
            LOG_ERROR("Restore could not safely drain/park LoRa");
            incompleteConfigResetDetected = true;
            incompletePreferenceRestoreDetected = true;
            incompleteLegacyMigrationDetected = resolvingLegacyMigration;
            configDecodeFailed = true;
            unreadablePreferenceSegments |= saveWhat;
            forceHeltecLocalRecoveryConfiguration();
            scheduleHeltecRecoveryReboot();
            return false;
        }
        const auto abortRestore = [&]() {
            incompleteConfigResetDetected = true;
            incompletePreferenceRestoreDetected = true;
            incompleteLegacyMigrationDetected = resolvingLegacyMigration;
            configDecodeFailed = true;
            unreadablePreferenceSegments |= saveWhat;
            forceHeltecLocalRecoveryConfiguration();
#if !MESHTASTIC_EXCLUDE_GPS
            if (gps)
                gps->disable();
#endif
            if (router && router->getRadioIface())
                router->getRadioIface()->sleep();
            scheduleHeltecRecoveryReboot();
            return false;
        };
#else
        int saveWhat = restoreWhat;
#endif

        if (restoreWhat & SEGMENT_CONFIG) {
            config = candidateConfig;
            LOG_DEBUG("Restored config");
        }
        if (restoreWhat & SEGMENT_MODULECONFIG) {
            moduleConfig = backup.module_config;
            LOG_DEBUG("Restored module config");
        }
        if (restoreWhat & SEGMENT_DEVICESTATE) {
            devicestate.owner = candidateOwner;
            LOG_DEBUG("Restored device state");
        }
        if (restoreWhat & SEGMENT_CHANNELS) {
            channelFile = backup.channels;
            LOG_DEBUG("Restored channels");
        }

#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)
        if (candidateNodeNum != myNodeInfo.my_node_num) {
            if (createNewIdentity())
                saveWhat |= SEGMENT_DEVICESTATE | SEGMENT_NODEDATABASE;
        } else if (owner.public_key.size == 32) {
            snprintf(owner.id, sizeof(owner.id), "!%08x", candidateNodeNum);
        }
#endif

        if (owner.is_licensed && channels.ensureLicensedOperation()) {
            saveWhat |= SEGMENT_CHANNELS;
            LOG_WARN("Licensed operation sanitized restored channel encryption/admin access");
        }
        if (saveWhat & SEGMENT_CHANNELS)
            channels.onConfigChanged(false); // normalize only; restore remains fenced until its reboot

#if defined(HELTEC_V4_OLED)
        if (!heltecDestructiveStoragePowerIsSafe()) {
            LOG_ERROR("Restore prefs stopped: power changed before persistent commit");
            return abortRestore();
        }
#endif
        if (!saveToDisk(saveWhat)) {
            LOG_ERROR("Save restored prefs to flash failed");
#if defined(HELTEC_V4_OLED)
            return abortRestore();
#else
            config.lora.region = meshtastic_Config_LoRaConfig_RegionCode_UNSET;
            config.lora.tx_enabled = false;
            if (router && router->getRadioIface())
                router->getRadioIface()->sleep();
            return false;
#endif
        }
#if defined(HELTEC_V4_OLED) && defined(FSCom)
        if (resolvingLegacyMigration) {
            bool requiredFilesPresent = false;
            {
                concurrency::LockGuard guard(spiLock);
                requiredFilesPresent = FSCom.exists(configFileName) && FSCom.exists(moduleConfigFileName) &&
                                       FSCom.exists(deviceStateFileName) && FSCom.exists(channelFileName) &&
                                       (!restoredNodeDatabaseRequired || FSCom.exists(nodeDatabaseFileName));
            }
            const bool residualsRemoved =
                requiredFilesPresent && removeHeltecPreferenceResidualsChecked(restoredNodeDatabaseRequired, true);
            bool legacyMarkerRemoved = false;
            if (residualsRemoved) {
                concurrency::LockGuard guard(spiLock);
                legacyMarkerRemoved = !FSCom.exists(legacyPrefFileName) || FSCom.remove(legacyPrefFileName);
                legacyMarkerRemoved &= !FSCom.exists(legacyPrefFileName);
            }
            if (!legacyMarkerRemoved) {
                LOG_ERROR("Restore committed but legacy migration marker could not be removed");
                return abortRestore();
            }
            legacyPreferencesPendingCleanup = false;
            incompleteLegacyMigrationDetected = false;
        }
#endif
#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)
        if (privateKeySize == 32) {
            uint8_t installedPublicKey[32];
            bool installed = false;
            concurrency::LockGuard guard(cryptLock);
            installed = crypto->regeneratePublicKey(installedPublicKey, config.security.private_key.bytes) &&
                        memcmp(installedPublicKey, config.security.public_key.bytes, sizeof(installedPublicKey)) == 0;
            if (!installed) {
                LOG_ERROR("Restored identity could not be installed in the crypto engine");
#if defined(HELTEC_V4_OLED)
                return abortRestore();
#else
                config.lora.region = meshtastic_Config_LoRaConfig_RegionCode_UNSET;
                config.lora.tx_enabled = false;
                if (router && router->getRadioIface())
                    router->getRadioIface()->sleep();
                return false;
#endif
            }
        }
#endif
#if defined(HELTEC_V4_OLED)
        if (!heltecDestructiveStoragePowerIsSafe()) {
            LOG_ERROR("Restore prefs stopped: power changed before final marker commit");
            return abortRestore();
        }
        if (!clearHeltecResetPendingMarker(HeltecResetPendingKind::RESTORE, true)) {
            LOG_ERROR("Restore prefs committed but pending marker could not be cleared");
            return abortRestore();
        }
#endif
        LOG_INFO("Restored prefs from backup");
        return true;
    } else if (location == meshtastic_AdminMessage_BackupLocation_SD) {
        // TODO: After more mainline SD card support
    }
#endif
    return false;
}

/// Record an error that should be reported via analytics
void recordCriticalError(meshtastic_CriticalErrorCode code, uint32_t address, const char *filename)
{
    if (filename) {
        LOG_ERROR("NOTE! Record critical error %d at %s:%lu", code, filename, address);
    } else {
        LOG_ERROR("NOTE! Record critical error %d, address=0x%lx", code, address);
    }

    // Record error to DB
    error_code = code;
    error_address = address;

    // Currently portuino is mostly used for simulation.  Make sure the user notices something really bad happened
#ifdef ARCH_PORTDUINO
    LOG_ERROR("Critical failure");
    // TODO: Determine if other critical errors should also cause an immediate exit
    if (code == meshtastic_CriticalErrorCode_FLASH_CORRUPTION_RECOVERABLE ||
        code == meshtastic_CriticalErrorCode_FLASH_CORRUPTION_UNRECOVERABLE)
        exit(2);
#endif
}
