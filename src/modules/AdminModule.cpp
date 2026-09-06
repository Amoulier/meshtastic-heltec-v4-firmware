#include "AdminModule.h"
#include "Channels.h"
#include "CryptoEngine.h"
#include "DisplayFormatters.h"
#include "HardwareRNG.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "PositionPrecision.h"
#include "PowerFSM.h"
#include "SPILock.h"
#include "gps/RTC.h"
#include "input/InputBroker.h"
#include "meshUtils.h"
#include <ErriezCRC32.h>
#include <FSCommon.h>
#include <Throttle.h>
#include <ctype.h> // for better whitespace handling
#if defined(ARCH_ESP32) && !MESHTASTIC_EXCLUDE_WIFI
#include "MeshtasticOTA.h"
#endif
#include "Router.h"
#include "concurrency/LockGuard.h"
#include "configuration.h"
#include "main.h"
#ifdef ARCH_NRF52
#include "main.h"
#endif
#ifdef ARCH_PORTDUINO
#include "PortduinoGlue.h"
#include "unistd.h"
#endif

#include "Default.h"
#include "MeshRadio.h"
#include "MessageStore.h"
#include "RadioInterface.h"
#include "TypeConversions.h"
#include "mesh/RadioLibInterface.h"
#ifdef MESHTASTIC_PHONEAPI_ACCESS_CONTROL
#include "mesh/PhoneAPI.h"
#endif
#ifdef MESHTASTIC_ENCRYPTED_STORAGE
#include "security/EncryptedStorage.h"
#endif
#if !MESHTASTIC_EXCLUDE_BEACON
#include "modules/MeshBeaconModule.h"
#endif

#if !MESHTASTIC_EXCLUDE_MQTT
#include "mqtt/MQTT.h"
#endif

#if !MESHTASTIC_EXCLUDE_GPS
#include "GPS.h"
#endif

#include <RNG.h>      // CryptRNG, the seeded CSPRNG used as fallback for the session passkey
#include <Throttle.h> // rollover-safe elapsed-time checks for the session passkey
#include <algorithm>

#if MESHTASTIC_EXCLUDE_GPS
#include "modules/PositionModule.h"
#endif

#if !defined(ARCH_STM32WL) && !MESHTASTIC_EXCLUDE_I2C && !MESHTASTIC_EXCLUDE_ACCELEROMETER
#include "motion/AccelerometerThread.h"
#endif
// Unguarded: serialConfigIsValid() is needed on every platform. The class stays
// guarded in the header.
#include "SerialModule.h"

AdminModule *adminModule;
static void applyCommittedConfigSideEffects();

#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)
static bool licensedIdentityWillMigrate()
{
    if (config.security.private_key.size != 32 || config.security.public_key.size != 32)
        return true;
    if (nodeDB->checkLowEntropyPublicKey(config.security.public_key))
        return true;
    return crc32Buffer(config.security.public_key.bytes, config.security.public_key.size) != nodeDB->getNodeNum();
}

static void restoreLivePkiIdentity(const meshtastic_Config_SecurityConfig &previousSecurity)
{
    if (!crypto)
        return;
    const uint8_t *previousPrivateKey = previousSecurity.private_key.size == 32 ? previousSecurity.private_key.bytes : nullptr;
    if (!crypto->restoreIdentity(previousPrivateKey))
        LOG_ERROR("Could not restore the prior live PKI identity; crypto remains cleared");
}
#endif

/// A special reserved string to indicate strings we can not share with external
/// nodes.  We will use this 'reserved' word instead. Also, to make setting work
/// correctly, if someone tries to set a string to this reserved value we assume
/// they don't really want a change.
static const char *secretReserved = "sekrit";

#if defined(HELTEC_V4_OLED)
static bool isProtectedHeltecAdminDeletePath(const char *name)
{
    if (!name)
        return true;
    // FSCom accepts relative paths. Reject alternate separators and every
    // traversal component before checking the protected top-level trees so a
    // path such as /static/../prefs/config.proto cannot bypass the guard.
    if (strchr(name, '\\'))
        return true;
    for (const char *segment = name; *segment;) {
        while (*segment == '/')
            ++segment;
        const char *separator = strchr(segment, '/');
        const size_t segmentLength = separator ? static_cast<size_t>(separator - segment) : strlen(segment);
        if (segmentLength == 2 && segment[0] == '.' && segment[1] == '.')
            return true;
        if (!separator)
            break;
        segment = separator + 1;
    }
    while ((name[0] == '.' && (name[1] == '/' || name[1] == '\\')) || name[0] == '/' || name[0] == '\\')
        name += (name[0] == '.') ? 2 : 1;
    const char *separator = strpbrk(name, "/\\");
    const size_t componentLength = separator ? static_cast<size_t>(separator - name) : strlen(name);
    const auto componentIs = [&](const char *expected) {
        return componentLength == strlen(expected) && memcmp(name, expected, componentLength) == 0;
    };
    return componentIs("prefs") || componentIs("backups") || (componentIs("heltec-nvs-reset.pending") && !separator);
}
#endif

/// If buf is the reserved secret word, replace the buffer with currentVal
static void writeSecret(char *buf, size_t bufsz, const char *currentVal)
{
    if (strcmp(buf, secretReserved) == 0) {
        strncpy(buf, currentVal, bufsz);
    }
}

/**
 * @brief Handle received protobuf message
 *
 * @param mp Received MeshPacket
 * @param r Decoded AdminMessage
 * @return bool
 */
bool AdminModule::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_AdminMessage *r)
{
    // if handled == false, then let others look at this message also if they want
    bool handled = false;
    assert(r);
    // PhoneAPI enters through an external-state reader scope. Never block on
    // this mutex while holding that scope: another handler may own the mutex
    // while draining readers for a reset/edit, which would invert the locks.
    // A client can safely retry this transient busy response.
    if (!editTransactionLock.lock(0)) {
        LOG_WARN("AdminModule busy; retry request");
        return handled;
    }
    struct EditTransactionUnlock {
        concurrency::Lock &lock;
        ~EditTransactionUnlock() { lock.unlock(); }
    } transactionGuard{editTransactionLock};
    mutationPersistenceFailed = false;

#ifdef MESHTASTIC_ENCRYPTED_STORAGE
    // While storage is locked, drop every admin payload - both local and
    // remote (PKC, mesh-relayed). Lockdown unlock is the prerequisite for
    // any admin operation: operators must authenticate via lockdown_auth
    // first. The lockdown_auth path itself is handled synchronously in
    // PhoneAPI::handleToRadioPacket before reaching here, so the real
    // unlock flow is not affected by this gate. Without this, a remote
    // PKC-authorized peer (or a USERPREFS-baked admin_key) could drive
    // factory_reset / set_config against a locked device before the
    // operator has even unlocked it.
    // Only gate when lockdown is ACTIVE. A lockdown-capable build that hasn't
    // been provisioned (or was disabled) is not unlocked either, but must
    // still serve admin normally - so check isLockdownActive() first.
    if (EncryptedStorage::isLockdownActive() && !EncryptedStorage::isUnlocked()) {
        LOG_WARN("AdminModule: dropping admin payload - storage locked");
        return handled;
    }
#endif

    bool fromOthers = !isFromUs(&mp);
    if (mp.which_payload_variant != meshtastic_MeshPacket_decoded_tag) {
        return handled;
    }
#if defined(HELTEC_V4_OLED)
    const bool localRecoveryOperation =
        mp.from == 0 && nodeDB && nodeDB->requiresConfigRecovery() &&
        (IS_ONE_OF(r->which_payload_variant, meshtastic_AdminMessage_factory_reset_device_tag,
                   meshtastic_AdminMessage_factory_reset_config_tag, meshtastic_AdminMessage_restore_preferences_tag,
                   meshtastic_AdminMessage_reboot_seconds_tag, meshtastic_AdminMessage_shutdown_seconds_tag,
                   meshtastic_AdminMessage_enter_dfu_mode_request_tag) ||
         (r->which_payload_variant == meshtastic_AdminMessage_nodedb_reset_tag && nodeDB->canResetNodesForRecovery()) ||
         shouldAllowOtaRequestWhileRecovering(r->which_payload_variant == meshtastic_AdminMessage_ota_request_tag,
                                              r->which_payload_variant == meshtastic_AdminMessage_ota_request_tag &&
                                                  r->ota_request.reboot_ota_mode == meshtastic_OTAMode_OTA_BLE));
#endif
#ifdef ARCH_PORTDUINO
    // Simulator only: honor exit_simulator unconditionally for the local client
    // (from==0). The from==0 branch below now covers pki_encrypted local packets
    // too, but is_managed can still block it. Rather than threading simulator
    // awareness through the auth gates, intercept here before any auth logic
    // runs. Local-origin + force_simradio only.
    // TODO: should a local client bypass admin auth at all? Fenced to the
    // simulator for now.
    if (portduino_config.force_simradio && mp.from == 0 &&
        r->which_payload_variant == meshtastic_AdminMessage_exit_simulator_tag) {
        LOG_INFO("Exiting simulator");
        exit(0);
    }
#endif
    meshtastic_Channel *ch = &channels.getByIndex(mp.channel);
    if (messageIsResponse(r)) {
        // Only accept a response from a remote we sent the matching request to.
        // from == 0 is a local client, which PhoneAPI has already gated.
        const pb_size_t moduleConfigTag = r->which_payload_variant == meshtastic_AdminMessage_get_module_config_response_tag
                                              ? r->get_module_config_response.which_payload_variant
                                              : 0;
        if (mp.from != 0 && !responseIsSolicited(mp, r->which_payload_variant, moduleConfigTag)) {
            LOG_INFO("Ignore admin response from 0x%08x, no outstanding request", mp.from);
            return handled;
        }
        LOG_TRACE("Allow admin response message");
    } else if (mp.from == 0) {
        // Local admin from a BLE/USB/TCP client. from == 0 cannot arrive from the
        // mesh: RF drops packets without a sender (RadioLibInterface) and MQTT
        // treats from == 0 as our own downlink and ignores it. Clients may set
        // pki_encrypted on self-addressed admin (the python CLI does), so don't use
        // it to reroute local packets into the remote-PKC key check.
        //
        // Under MESHTASTIC_PHONEAPI_ACCESS_CONTROL, the per-connection auth
        // gate lives in PhoneAPI::handleToRadioPacket - any local admin
        // payload other than lockdown_auth is dropped there if the
        // originating connection is unauthorized. By the time we reach
        // this branch the connection has already proven the passphrase,
        // so is_managed needs no additional gate here.
        //
        // Without that build flag the legacy is_managed semantics still
        // apply: refuse all plain local admin and require PKC instead.
#ifndef MESHTASTIC_PHONEAPI_ACCESS_CONTROL
        if (config.security.is_managed
#if defined(HELTEC_V4_OLED)
            && !localRecoveryOperation
#endif
        ) {
            LOG_INFO("Ignore local admin payload: is_managed");
            return handled;
        }
#endif
    } else if (strcasecmp(ch->settings.name, Channels::adminChannel) == 0) {
        if (!config.security.admin_channel_enabled) {
            LOG_INFO("Ignore admin channel, legacy admin disabled");
            myReply = allocErrorResponse(meshtastic_Routing_Error_NOT_AUTHORIZED, &mp);
            return handled;
        }
    } else if (mp.pki_encrypted) {
        if ((config.security.admin_key[0].size == 32 &&
             memcmp(mp.public_key.bytes, config.security.admin_key[0].bytes, 32) == 0) ||
            (config.security.admin_key[1].size == 32 &&
             memcmp(mp.public_key.bytes, config.security.admin_key[1].bytes, 32) == 0) ||
            (config.security.admin_key[2].size == 32 &&
             memcmp(mp.public_key.bytes, config.security.admin_key[2].bytes, 32) == 0)) {
            LOG_INFO("PKC admin payload with authorized sender key");

            // Note: PKC admin does NOT automatically authorize the
            // originating local PhoneAPI connection for content
            // redaction purposes. PKC and the per-connection lockdown
            // auth slot are independent gates - operators using PKC
            // admin from a local app should still send lockdown_auth
            // separately to unlock the redacted FromRadio stream.
            // (The previous auto-authorize path read a shared
            // g_currentContext set during synchronous PhoneAPI
            // dispatch; by the time this Router-thread handler runs
            // that pointer is unrelated, so the path was unsafe.)

            // Do not mutate the node database while authenticating. Older
            // behavior auto-favorited this sender before validating the
            // temporary session key, so a rejected packet could still leave
            // an uncommitted favorite in RAM.
        } else {
            myReply = allocErrorResponse(meshtastic_Routing_Error_ADMIN_PUBLIC_KEY_UNAUTHORIZED, &mp);
            LOG_INFO("PKC admin payload: sender public key doesn't match admin "
                     "authorized key");
            return handled;
        }
    } else {
        LOG_INFO("Ignore unauthorized admin payload %i", r->which_payload_variant);
        myReply = allocErrorResponse(meshtastic_Routing_Error_NOT_AUTHORIZED, &mp);
        return handled;
    }

    LOG_INFO("Handle admin payload %i", r->which_payload_variant);

    // all of the get and set messages, including those for other modules, flow
    // through here first. any message that changes state, we want to check the
    // passkey for
    if (mp.from != 0 && !messageIsRequest(r) && !messageIsResponse(r)) {
        if (!checkPassKey(r)) {
            LOG_WARN("Admin message without session_key");
            myReply = allocErrorResponse(meshtastic_Routing_Error_ADMIN_BAD_SESSION_KEY, &mp);
            return handled;
        }
    }

#if defined(HELTEC_V4_OLED)
    const bool mutatingAdminResponse =
        r->which_payload_variant == meshtastic_AdminMessage_get_module_config_response_tag &&
        r->get_module_config_response.which_payload_variant == meshtastic_ModuleConfig_remote_hardware_tag;
    const bool readOnlyAdminPayload = (messageIsRequest(r) || messageIsResponse(r)) && !mutatingAdminResponse;
    const bool supportedRecoveryOta =
        shouldAllowOtaRequestWhileRecovering(r->which_payload_variant == meshtastic_AdminMessage_ota_request_tag,
                                             r->which_payload_variant == meshtastic_AdminMessage_ota_request_tag &&
                                                 r->ota_request.reboot_ota_mode == meshtastic_OTAMode_OTA_BLE);
    const bool persistentConfigUsable =
        shouldUsePersistentConfiguration(shouldUseFilesystemPersistence(fsIsMounted()), nodeDB->requiresConfigRecovery());
    const bool supportedNodeDbRecovery =
        r->which_payload_variant == meshtastic_AdminMessage_nodedb_reset_tag && nodeDB->canResetNodesForRecovery();
    if (!persistentConfigUsable && !readOnlyAdminPayload && !supportedRecoveryOta && !supportedNodeDbRecovery &&
        !IS_ONE_OF(r->which_payload_variant, meshtastic_AdminMessage_factory_reset_device_tag,
                   meshtastic_AdminMessage_factory_reset_config_tag, meshtastic_AdminMessage_restore_preferences_tag,
                   meshtastic_AdminMessage_reboot_seconds_tag, meshtastic_AdminMessage_shutdown_seconds_tag,
                   meshtastic_AdminMessage_enter_dfu_mode_request_tag)) {
        // Keep BLE usable as a recovery channel, but do not acknowledge or apply
        // settings which cannot be persisted. In particular, a Bluetooth config
        // write must not strand the device by disabling BLE.
        LOG_WARN("AdminModule: reject mutation while persistent configuration is "
                 "unavailable");
        myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
        return handled;
    }
#endif

    // Before the switch, so every case below sees consistent transaction state.
    expireStaleEditTransaction();
    const bool transactionMutation = !messageIsRequest(r) && !messageIsResponse(r);
    const bool cancelsPendingLifecycle =
        (r->which_payload_variant == meshtastic_AdminMessage_reboot_seconds_tag && r->reboot_seconds < 0) ||
        (r->which_payload_variant == meshtastic_AdminMessage_shutdown_seconds_tag && r->shutdown_seconds < 0);
    if ((rebootAtMsec != 0 || shutdownAtMsec != 0) && transactionMutation && !cancelsPendingLifecycle) {
        // Once teardown is armed, a flash transaction may be cut off by the
        // main loop at any point. Permit only the explicit cancellation of the
        // pending lifecycle request until normal operation resumes.
        LOG_WARN("Reject settings mutation while reboot/shutdown is pending");
        myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
        return handled;
    }
    if (hasOpenEditTransaction && transactionMutation && !nodeDB->isPreferenceEditOwnerCurrentTask()) {
        LOG_WARN("Reject settings mutation from a different edit-transaction owner");
        myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
        return handled;
    }
    if (hasOpenEditTransaction &&
        IS_ONE_OF(r->which_payload_variant, meshtastic_AdminMessage_begin_edit_settings_tag,
                  meshtastic_AdminMessage_reboot_seconds_tag, meshtastic_AdminMessage_shutdown_seconds_tag,
                  meshtastic_AdminMessage_ota_request_tag, meshtastic_AdminMessage_factory_reset_config_tag,
                  meshtastic_AdminMessage_factory_reset_device_tag, meshtastic_AdminMessage_nodedb_reset_tag,
                  meshtastic_AdminMessage_restore_preferences_tag, meshtastic_AdminMessage_enter_dfu_mode_request_tag)) {
        // A second local transport is also normalized to from=0, so never let
        // it renew an existing transaction or interrupt it with a reboot. The
        // original client must commit (or let the idle timeout commit) first.
        LOG_WARN("Reject lifecycle operation while a settings edit is open");
        myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
        return handled;
    }
    const bool auxiliaryOrIndependentMutation =
        IS_ONE_OF(r->which_payload_variant, meshtastic_AdminMessage_store_ui_config_tag,
                  meshtastic_AdminMessage_set_canned_message_module_messages_tag,
                  meshtastic_AdminMessage_set_ringtone_message_tag, meshtastic_AdminMessage_sensor_config_tag,
                  meshtastic_AdminMessage_set_scale_tag) ||
        (r->which_payload_variant == meshtastic_AdminMessage_key_verification_tag &&
         r->key_verification.message_type == meshtastic_KeyVerificationAdmin_MessageType_DO_VERIFY);
    if (hasOpenEditTransaction && auxiliaryOrIndependentMutation) {
        // These files/workflows are not part of the five-file bulk settings
        // generation. Mixing them into OPEN would acknowledge a change that a
        // later COMMIT cannot atomically represent.
        LOG_WARN("Reject independent preference mutation while settings edit is open");
        myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
        return handled;
    }

    // A one-request setter used to mutate shared RAM first and only open the
    // durable transaction inside saveChanges(). A radio/module task could
    // therefore observe a provisional PSK, identity, role, or position in the
    // small window before persistence. Park traffic before dispatching every
    // setter that commits through saveChanges(); validation/no-op paths cancel
    // the empty transaction below and restore the durable radio generation.
    bool implicitPreferenceEdit = false;
#if defined(HELTEC_V4_OLED)
    const bool oneRequestPreferenceMutation =
        IS_ONE_OF(r->which_payload_variant, meshtastic_AdminMessage_set_owner_tag, meshtastic_AdminMessage_set_config_tag,
                  meshtastic_AdminMessage_set_module_config_tag, meshtastic_AdminMessage_set_channel_tag,
                  meshtastic_AdminMessage_set_ham_mode_tag, meshtastic_AdminMessage_set_fixed_position_tag,
                  meshtastic_AdminMessage_remove_fixed_position_tag) ||
        IS_ONE_OF(r->which_payload_variant, meshtastic_AdminMessage_set_favorite_node_tag,
                  meshtastic_AdminMessage_remove_favorite_node_tag, meshtastic_AdminMessage_set_ignored_node_tag,
                  meshtastic_AdminMessage_remove_ignored_node_tag, meshtastic_AdminMessage_toggle_muted_node_tag,
                  meshtastic_AdminMessage_remove_by_nodenum_tag, meshtastic_AdminMessage_add_contact_tag) ||
        (r->which_payload_variant == meshtastic_AdminMessage_key_verification_tag &&
         r->key_verification.message_type == meshtastic_KeyVerificationAdmin_MessageType_DO_VERIFY);
    implicitPreferenceEdit = !hasOpenEditTransaction && oneRequestPreferenceMutation;
    if (implicitPreferenceEdit && !nodeDB->beginPreferenceEdit(false)) {
        LOG_ERROR("One-request settings mutation could not open its "
                  "traffic/storage fence");
        myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
        return handled;
    }
#endif

    switch (r->which_payload_variant) {

#ifdef MESHTASTIC_ENCRYPTED_STORAGE
    // lockdown_auth is handled synchronously in
    // PhoneAPI::handleToRadioPacket - see handleLockdownAuthInline. A
    // packet should not normally reach AdminModule under that flag set,
    // but if it ever does (e.g. injected via a non-PhoneAPI path), drop
    // it silently rather than leaking a partial response.
    case meshtastic_AdminMessage_lockdown_auth_tag:
        LOG_WARN("AdminModule: lockdown_auth reached Router/AdminModule path; "
                 "ignoring (PhoneAPI handles)");
        return handled;
#endif // MESHTASTIC_ENCRYPTED_STORAGE

    /**
     * Getters
     */
    case meshtastic_AdminMessage_get_owner_request_tag:
        LOG_DEBUG("Client got owner");
        handleGetOwner(mp);
        break;

    case meshtastic_AdminMessage_get_config_request_tag:
        LOG_DEBUG("Client got config");
        handleGetConfig(mp, r->get_config_request);
        break;

    case meshtastic_AdminMessage_get_module_config_request_tag:
        LOG_DEBUG("Client got module config");
        handleGetModuleConfig(mp, r->get_module_config_request);
        break;

    case meshtastic_AdminMessage_get_channel_request_tag: {
        uint32_t i = r->get_channel_request - 1;
        LOG_DEBUG("Client got channel %u", i);
        if (i >= MAX_NUM_CHANNELS)
            myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
        else
            handleGetChannel(mp, i);
        break;
    }

    /**
     * Setters
     */
    case meshtastic_AdminMessage_set_owner_tag:
        LOG_DEBUG("Client set owner");
        // Validate names
        if (*r->set_owner.long_name) {
            const char *start = r->set_owner.long_name;
            // Skip all whitespace (space, tab, newline, etc)
            while (*start && isspace((unsigned char)*start))
                start++;
            if (*start == '\0') {
                LOG_WARN("Rejected long_name: needs 1+ non-whitespace char");
                myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
                break;
            }
        }
        if (*r->set_owner.short_name) {
            const char *start = r->set_owner.short_name;
            while (*start && isspace((unsigned char)*start))
                start++;
            if (*start == '\0') {
                LOG_WARN("Rejected short_name: needs 1+ non-whitespace char");
                myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
                break;
            }
        }
        if (!handleSetOwner(r->set_owner))
            myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
        break;

    case meshtastic_AdminMessage_set_config_tag: {
        LOG_DEBUG("Client set config");

        // Non-LoRa configs need no further validation.
        if (r->set_config.which_payload_variant != meshtastic_Config_lora_tag) {
            LOG_DEBUG("Non-LoRa config, applying directly");
            if (!handleSetConfig(r->set_config, fromOthers))
                myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
            break;
        }

        // Only LORA_24 requires hardware capability validation.
        if (r->set_config.payload_variant.lora.region != meshtastic_Config_LoRaConfig_RegionCode_LORA_24) {
            LOG_DEBUG("LoRa config, region is not LORA_24, applying directly");
            if (!handleSetConfig(r->set_config, fromOthers))
                myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
            break;
        }

        // Hardware supports 2.4 GHz - apply the config.
        // Fail closed: null instance is treated as incapable.
        if (RadioLibInterface::instance && RadioLibInterface::instance->wideLora()) {
            LOG_DEBUG("LORA_24 requested, radio hardware supports 2.4 GHz, applying");
            if (!handleSetConfig(r->set_config, fromOthers))
                myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
            break;
        }

        LOG_WARN("No 2.4 GHz radio support; rejecting LORA_24 region");
        myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
        break;
    }

    case meshtastic_AdminMessage_set_module_config_tag:
        LOG_DEBUG("Client set module config");
        if (!handleSetModuleConfig(r->set_module_config)) {
            myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
        }
        break;

    case meshtastic_AdminMessage_set_channel_tag:
        LOG_DEBUG("Client set channel %d", r->set_channel.index);
        if (r->set_channel.index < 0 || r->set_channel.index >= (int)MAX_NUM_CHANNELS)
            myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
        else
            handleSetChannel(r->set_channel);
        break;
    case meshtastic_AdminMessage_set_ham_mode_tag:
        LOG_DEBUG("Client set ham mode");
        // Without this a rejected request falls through to the generic
        // Routing_Error_NONE ack below, so a client would report ham mode as
        // enabled on a node that changed nothing.
        if (!handleSetHamMode(r->set_ham_mode))
            myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
        break;
    case meshtastic_AdminMessage_get_ui_config_request_tag: {
        LOG_DEBUG("Client is getting device-ui config");
        handleGetDeviceUIConfig(mp);
        handled = true;
        break;
    }

    /**
     * Other
     */
    case meshtastic_AdminMessage_reboot_seconds_tag: {
        reboot(r->reboot_seconds);
        break;
    }
    case meshtastic_AdminMessage_ota_request_tag: {
#if defined(ARCH_ESP32) && !MESHTASTIC_EXCLUDE_WIFI
        LOG_INFO("OTA Requested");

        if (r->ota_request.ota_hash.size != 32) {
            suppressRebootBanner = true;
            sendWarningAndLog("Cannot start OTA: Invalid `ota_hash` provided.");
            break;
        }

        meshtastic_OTAMode mode = r->ota_request.reboot_ota_mode;
        if (mode != METHOD_OTA_BLE && mode != METHOD_OTA_WIFI) {
            suppressRebootBanner = true;
            sendWarningAndLog("Cannot start OTA: Invalid transport mode.");
            break;
        }
        const char *mode_name = (mode == METHOD_OTA_BLE ? "BLE" : "WiFi");

        // Check that we have an OTA partition
        const esp_partition_t *part = MeshtasticOTA::getAppPartition();
        if (part == NULL) {
            suppressRebootBanner = true;
            sendWarningAndLog("Cannot start OTA: Cannot find OTA Loader partition.");
            break;
        }

        static esp_app_desc_t app_desc;
        if (!MeshtasticOTA::getAppDesc(part, &app_desc)) {
            suppressRebootBanner = true;
            sendWarningAndLog("Cannot start OTA: Device does have a valid OTA Loader.");
            break;
        }

        if (!MeshtasticOTA::checkOTACapability(&app_desc, mode)) {
            suppressRebootBanner = true;
            sendWarningAndLog("OTA Loader does not support %s", mode_name);
            break;
        }

        if (!MeshtasticOTA::saveConfig(&config.network, mode, r->ota_request.ota_hash.bytes)) {
            suppressRebootBanner = true;
            sendWarningAndLog("Cannot start OTA: Settings failed persistence verification.");
            break;
        }

        if (MeshtasticOTA::trySwitchToOTA()) {
            suppressRebootBanner = true;
            if (screen)
                screen->startFirmwareUpdateScreen();
            sendWarningAndLog("Rebooting to %s OTA", mode_name);
        } else {
            suppressRebootBanner = true;
            sendWarningAndLog("Unable to switch to the OTA partition.");
            break;
        }
#endif
        int s = 1; // Reboot in 1 second, hard coded
        LOG_INFO("Reboot in %d seconds", s);
        rebootAtMsec = (s < 0) ? 0 : (millis() + s * 1000);
        break;
    }
    case meshtastic_AdminMessage_shutdown_seconds_tag: {
        int32_t s = r->shutdown_seconds;
        LOG_INFO("Shutdown in %d seconds", s);
        shutdownAtMsec = (s < 0) ? 0 : (millis() + s * 1000);
        break;
    }
    case meshtastic_AdminMessage_get_device_metadata_request_tag: {
        LOG_INFO("Client got device metadata");
        handleGetDeviceMetadata(mp);
        break;
    }
    case meshtastic_AdminMessage_factory_reset_config_tag: {
        LOG_INFO("Initiate factory config reset");
        // Keep BLE active while reset cleanup performs nRF flash operations.
        if (nodeDB->factoryReset()) {
            LOG_INFO("Factory config reset finished, rebooting soon");
            disableBluetooth();
            reboot(DEFAULT_REBOOT_SECONDS);
        } else {
            myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
        }
        break;
    }
    case meshtastic_AdminMessage_factory_reset_device_tag: {
        LOG_INFO("Initiate full factory reset");
        if (nodeDB->factoryReset(true)) {
            disableBluetooth();
            reboot(DEFAULT_REBOOT_SECONDS);
        } else {
            myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
        }
        break;
    }
    case meshtastic_AdminMessage_nodedb_reset_tag: {
        LOG_INFO("Initiate node-db reset");
        //  CLIENT_BASE, ROUTER and ROUTER_LATE are able to preserve the remaining
        //  hop count when relaying a packet via a favorited node, so ensure that
        //  their favorites are kept on reset
        bool rolePreference =
            isOneOf(config.device.role, meshtastic_Config_DeviceConfig_Role_CLIENT_BASE,
                    meshtastic_Config_DeviceConfig_Role_ROUTER, meshtastic_Config_DeviceConfig_Role_ROUTER_LATE);
        if (nodeDB->resetNodes(rolePreference ? rolePreference : r->nodedb_reset)) {
            disableBluetooth();
            reboot(DEFAULT_REBOOT_SECONDS);
        } else {
            myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
        }
        break;
    }
    case meshtastic_AdminMessage_store_ui_config_tag: {
        LOG_INFO("Storing device-ui config");
        if (!handleStoreDeviceUIConfig(r->store_ui_config))
            myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
        handled = true;
        break;
    }
    case meshtastic_AdminMessage_begin_edit_settings_tag: {
        LOG_INFO("Begin settings edit transaction");
        if (mp.from != 0 || nodeDB->isCurrentExternalStateStatelessClient()) {
            // OPEN parks LoRa so live traffic cannot use a provisional PSK or
            // identity. A remote admin would lose the transport needed to
            // deliver COMMIT, while HTTP has no durable client session to own
            // the later request. BLE and serial are connection-scoped.
            LOG_WARN("Remote/stateless settings edit transactions are not supported");
            myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
            break;
        }
        if (!nodeDB->beginPreferenceEdit()) {
            LOG_ERROR("Settings edit transaction could not commit its durable intent");
            myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
            break;
        }
        deferredEditSegments = 0;
        deferredEditRequiresReboot = false;
        deferredEditOwnerChanged = false;
        deferredFixedPositionAnnouncement = false;
        deferredMessagePurgeNodes.clear();
        editTransactionOwner = mp.from;
        hasOpenEditTransaction = true;
        editTransactionActivityMs = millis();
        break;
    }
    case meshtastic_AdminMessage_commit_edit_settings_tag: {
        LOG_INFO("Commit settings edit transaction");
        if (!hasOpenEditTransaction || mp.from != editTransactionOwner || !nodeDB->isPreferenceEditOwnerCurrentTask()) {
            LOG_WARN("Reject orphaned or non-owner settings commit");
            myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
            break;
        }
        const int commitSegments =
            SEGMENT_CONFIG | SEGMENT_MODULECONFIG | SEGMENT_DEVICESTATE | SEGMENT_CHANNELS | SEGMENT_NODEDATABASE;
        if (!service->reloadConfig(commitSegments, true)) {
            LOG_ERROR("Settings transaction commit failed");
            myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
            break;
        }
        const bool requiresReboot = deferredEditRequiresReboot;
        const bool ownerChanged = deferredEditOwnerChanged;
        const bool announceFixedPosition = deferredFixedPositionAnnouncement;
        hasOpenEditTransaction = false;
        editTransactionOwner = 0;
        deferredEditSegments = 0;
        deferredEditRequiresReboot = false;
        deferredEditOwnerChanged = false;
        deferredFixedPositionAnnouncement = false;
        applyCommittedConfigSideEffects();
        flushChannelWarnings(); // one coalesced message for everything edited in
                                // this transaction
        if (ownerChanged)
            service->reloadOwner(true, false);
        if (announceFixedPosition && config.position.fixed_position && positionModule)
            positionModule->sendOurPosition();
        applyDeferredMessagePurges();
        if (requiresReboot) {
            disableBluetooth();
            reboot(DEFAULT_REBOOT_SECONDS);
        }
        break;
    }
    case meshtastic_AdminMessage_get_device_connection_status_request_tag: {
        LOG_INFO("Client got device connection status");
        handleGetDeviceConnectionStatus(mp);
        break;
    }
    case meshtastic_AdminMessage_get_module_config_response_tag: {
        LOG_INFO("Client got get_module_config response");
        // which_payload_variant is the ModuleConfig oneof tag, so compare against
        // that tag, not the AdminMessage ModuleConfigType enum (whose
        // REMOTEHARDWARE value is a different number).
        if (fromOthers && r->get_module_config_response.which_payload_variant == meshtastic_ModuleConfig_remote_hardware_tag) {
            handleGetModuleConfigResponse(mp, r);
        }
        break;
    }
    case meshtastic_AdminMessage_remove_by_nodenum_tag: {
        LOG_INFO("Client got remove_nodenum");
        if (r->remove_by_nodenum == 0 || r->remove_by_nodenum == NODENUM_BROADCAST ||
            r->remove_by_nodenum == nodeDB->getNodeNum()) {
            LOG_WARN("Refuse removal of invalid/local NodeNum 0x%08x", r->remove_by_nodenum);
            myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
        } else if (nodeDB->removeNodeByNum(r->remove_by_nodenum, false)) {
            saveChanges(SEGMENT_NODEDATABASE, false);
        }
        break;
    }
    case meshtastic_AdminMessage_add_contact_tag: {
        LOG_INFO("Client got add_contact");
        if (nodeDB->addFromContact(r->add_contact, false) && saveChanges(SEGMENT_NODEDATABASE, false) &&
            r->add_contact.should_ignore)
            deferOrApplyMessagePurge(r->add_contact.node_num);
        break;
    }
    case meshtastic_AdminMessage_set_favorite_node_tag: {
        LOG_INFO("Client got set_favorite_node");
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(r->set_favorite_node);
        if (node != NULL) {
            if (nodeDB->setProtectedFlag(node, NODEINFO_BITFIELD_IS_FAVORITE_MASK, true)) {
                saveChanges(SEGMENT_NODEDATABASE, false);
                if (screen)
                    screen->setFrames(graphics::Screen::FOCUS_PRESERVE); // <-- Rebuild screens
            } else if (mp.from == 0) {                                   // local request from the phone - tell the user
                                                                         // why it didn't take
                sendWarning(NodeDB::PROTECTED_CAP_WARN_FMT, "favorite", r->set_favorite_node, MAX_NUM_NODES - 2);
            } else {
                LOG_WARN("Remote set_favorite_node for 0x%08x refused: protected-node cap", r->set_favorite_node);
            }
        }
        break;
    }
    case meshtastic_AdminMessage_remove_favorite_node_tag: {
        LOG_INFO("Client got remove_favorite_node");
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(r->remove_favorite_node);
        if (node != NULL) {
            nodeInfoLiteSetBit(node, NODEINFO_BITFIELD_IS_FAVORITE_MASK, false);
            saveChanges(SEGMENT_NODEDATABASE, false);
            if (screen)
                screen->setFrames(graphics::Screen::FOCUS_PRESERVE); // <-- Rebuild screens
        }
        break;
    }
    case meshtastic_AdminMessage_set_ignored_node_tag: {
        LOG_INFO("Client got set_ignored_node");
        const meshtastic_NodeInfoLite *existingNode = nodeDB->getMeshNode(r->set_ignored_node);
        if (!existingNode && nodeDB->numProtectedNodes() >= MAX_NUM_NODES - 2) {
            // getOrCreateMeshNode() may evict a legitimate node before the
            // protected-node cap is checked. Reject first so a failed request
            // is a proven no-op and its implicit transaction can be cancelled.
            if (mp.from == 0)
                sendWarning(NodeDB::PROTECTED_CAP_WARN_FMT, "ignore", r->set_ignored_node, MAX_NUM_NODES - 2);
            else
                LOG_WARN("Remote set_ignored_node for 0x%08x refused: protected-node cap", r->set_ignored_node);
            break;
        }
        // Unlike the sibling node-targeted admin commands, create the entry if
        // it's absent so the block sticks for a node we've not heard from yet
        // (e.g. one a remote admin asks us to block) with no NodeInfo or key.
        meshtastic_NodeInfoLite *node = nodeDB->getOrCreateMeshNode(r->set_ignored_node);
        if (node != NULL) {
            if (nodeDB->setProtectedFlag(node, NODEINFO_BITFIELD_IS_IGNORED_MASK, true)) {
                nodeDB->eraseNodeSatellites(node->num);
                if (saveChanges(SEGMENT_NODEDATABASE, false))
                    deferOrApplyMessagePurge(node->num);
#if HAS_SCREEN
                if (screen)
                    screen->setFrames(graphics::Screen::FOCUS_PRESERVE);
#endif
            } else if (mp.from == 0) { // local request from the phone - tell the user
                                       // why it didn't take
                sendWarning(NodeDB::PROTECTED_CAP_WARN_FMT, "ignore", r->set_ignored_node, MAX_NUM_NODES - 2);
            } else {
                LOG_WARN("Remote set_ignored_node for 0x%08x refused: protected-node cap", r->set_ignored_node);
            }
        }
        break;
    }
    case meshtastic_AdminMessage_remove_ignored_node_tag: {
        LOG_INFO("Client got remove_ignored_node");
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(r->remove_ignored_node);
        if (node != NULL) {
            nodeInfoLiteSetBit(node, NODEINFO_BITFIELD_IS_IGNORED_MASK, false);
            saveChanges(SEGMENT_NODEDATABASE, false);
        }
        break;
    }
    case meshtastic_AdminMessage_toggle_muted_node_tag: {
        LOG_INFO("Client got toggle_muted_node");
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(r->toggle_muted_node);
        if (node != NULL) {
            nodeInfoLiteSetBit(node, NODEINFO_BITFIELD_IS_MUTED_MASK, !nodeInfoLiteIsMuted(node));
            saveChanges(SEGMENT_NODEDATABASE, false);
        }
        break;
    }

    case meshtastic_AdminMessage_set_fixed_position_tag: {
        LOG_INFO("Client got set_fixed_position");
        const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(nodeDB->getNodeNum());
        // Route the fixed position through updatePosition so it lands in the
        // satellite map (or, on builds with PositionDB excluded, just sets
        // localPosition for the local broadcast path).
        nodeDB->updatePosition(node->num, r->set_fixed_position, RX_SRC_LOCAL);
        nodeDB->setLocalPosition(r->set_fixed_position);
        config.position.fixed_position = true;
        const bool saved = saveChanges(SEGMENT_NODEDATABASE | SEGMENT_CONFIG, false);
#if !MESHTASTIC_EXCLUDE_GPS
        if (saved) {
            if (hasOpenEditTransaction)
                deferredFixedPositionAnnouncement = true;
            else if (positionModule)
                positionModule->sendOurPosition();
        }
#endif
        break;
    }
    case meshtastic_AdminMessage_remove_fixed_position_tag: {
        LOG_INFO("Client got remove_fixed_position");
        nodeDB->clearLocalPosition();
        config.position.fixed_position = false;
        deferredFixedPositionAnnouncement = false;
        saveChanges(SEGMENT_NODEDATABASE | SEGMENT_CONFIG, false);
        break;
    }
    case meshtastic_AdminMessage_set_time_only_tag: {
        LOG_INFO("Client got set_time_only");
        struct timeval tv;
        tv.tv_sec = r->set_time_only;
        tv.tv_usec = 0;

        perhapsSetRTC(RTCQualityNTP, &tv, false);
        break;
    }
    case meshtastic_AdminMessage_enter_dfu_mode_request_tag: {
        LOG_INFO("Client requests DFU mode");
#if HAS_SCREEN
        IF_SCREEN(screen->showSimpleBanner("Device is rebooting\ninto DFU mode.", 0));
#endif
#if defined(ARCH_NRF52) || defined(ARCH_RP2040) || defined(ARCH_STM32)
        enterDfuMode();
#endif
        break;
    }
    case meshtastic_AdminMessage_delete_file_request_tag: {
        LOG_DEBUG("Client requests delete file: %s", r->delete_file_request);

#ifdef FSCom
#if defined(HELTEC_V4_OLED)
        if (nodeDB->requiresConfigRecovery() || isProtectedHeltecAdminDeletePath(r->delete_file_request)) {
            LOG_WARN("Protected file deletion rejected");
            myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
            break;
        }
#endif
        concurrency::LockGuard guard(spiLock);
        if (FSCom.remove(r->delete_file_request) && !FSCom.exists(r->delete_file_request)) {
            LOG_DEBUG("Deleted file");
        } else {
            LOG_DEBUG("File delete failed");
            myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
        }
#endif
        break;
    }
    case meshtastic_AdminMessage_backup_preferences_tag: {
        LOG_INFO("Client requests preferences backup");
        if (nodeDB->backupPreferences(r->backup_preferences)) {
            myReply = allocErrorResponse(meshtastic_Routing_Error_NONE, &mp);
        } else {
            myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
        }
        break;
    }
    case meshtastic_AdminMessage_restore_preferences_tag: {
        LOG_INFO("Client requests preferences restore");
        if (nodeDB->restorePreferences(r->backup_preferences,
                                       SEGMENT_DEVICESTATE | SEGMENT_CONFIG | SEGMENT_MODULECONFIG | SEGMENT_CHANNELS)) {
            myReply = allocErrorResponse(meshtastic_Routing_Error_NONE, &mp);
            LOG_DEBUG("Rebooting after preferences restore");
            disableBluetooth();
            reboot(DEFAULT_REBOOT_SECONDS);
        } else {
            myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
        }
        break;
    }
    case meshtastic_AdminMessage_remove_backup_preferences_tag: {
        LOG_INFO("Client requests preferences backup removal");
        myReply = allocErrorResponse(nodeDB->removeBackupPreferences(r->remove_backup_preferences)
                                         ? meshtastic_Routing_Error_NONE
                                         : meshtastic_Routing_Error_BAD_REQUEST,
                                     &mp);
        break;
    }
    case meshtastic_AdminMessage_send_input_event_tag: {
        LOG_INFO("Client requests send input event");
        if (hasOpenEditTransaction || !handleSendInputEvent(r->send_input_event))
            myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
        break;
    }
#ifdef ARCH_PORTDUINO
    case meshtastic_AdminMessage_exit_simulator_tag:
        LOG_INFO("Exiting simulator");
        exit(0);
        break;
#endif

    default:
        handleViaModuleApi(mp, r);
        break;
    }

#if defined(HELTEC_V4_OLED)
    if (implicitPreferenceEdit && nodeDB->isPreferenceEditTransactionActive() && !mutationPersistenceFailed) {
        // The setter rejected its payload or found nothing to change, so no
        // save was attempted. There is no durable marker and RAM is still the
        // pre-request generation; safely rearm that generation now.
        if (!nodeDB->cancelPreferenceEdit()) {
            mutationPersistenceFailed = true;
            rebootAtMsec = millis() + 1000;
        }
    }
#endif

    // Allow any observers (e.g. the UI) to handle/respond
    handleViaObservers(mp, r);

    // If asked for a response and it is not yet set, generate an 'ACK' response
    if (mutationPersistenceFailed && !myReply) {
        myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
    }
    if (mp.decoded.want_response && !myReply) {
        myReply = allocErrorResponse(meshtastic_Routing_Error_NONE, &mp);
    }
    if (mp.pki_encrypted && myReply) {
        myReply->pki_encrypted = true;
    }
    return handled;
}

void AdminModule::handleViaModuleApi(const meshtastic_MeshPacket &mp, meshtastic_AdminMessage *r)
{
    meshtastic_AdminMessage res = meshtastic_AdminMessage_init_default;
    AdminMessageHandleResult handleResult = MeshModule::handleAdminMessageForAllModules(mp, r, &res);

    if (handleResult == AdminMessageHandleResult::ERROR) {
        myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
    } else if (handleResult == AdminMessageHandleResult::HANDLED_WITH_RESPONSE) {
        setPassKey(&res);
        myReply = allocDataProtobuf(res);
    } else if (mp.decoded.want_response) {
        LOG_DEBUG("Module API didn't respond to admin msg. req.variant=%d", r->which_payload_variant);
    } else if (handleResult != AdminMessageHandleResult::HANDLED) {
        // Probably a message sent by us or sent to our local node.  FIXME, we
        // should avoid scanning these messages
        LOG_DEBUG("Module API didn't handle admin msg %d", r->which_payload_variant);
    }
}

void AdminModule::handleViaObservers(const meshtastic_MeshPacket &mp, const meshtastic_AdminMessage *r)
{
    AdminMessageHandleResult observerResult = AdminMessageHandleResult::NOT_HANDLED;
    meshtastic_AdminMessage observerResponse = meshtastic_AdminMessage_init_default;
    AdminModule_ObserverData observerData = {
        .request = r,
        .response = &observerResponse,
        .result = &observerResult,
    };

    notifyObservers(&observerData);

    if (observerResult == AdminMessageHandleResult::ERROR) {
        myReply = allocErrorResponse(meshtastic_Routing_Error_BAD_REQUEST, &mp);
    } else if (observerResult == AdminMessageHandleResult::HANDLED_WITH_RESPONSE) {
        setPassKey(&observerResponse);
        myReply = allocDataProtobuf(observerResponse);
        LOG_DEBUG("Observer responded to admin message");
    } else if (observerResult == AdminMessageHandleResult::HANDLED) {
        LOG_DEBUG("Observer handled admin message");
    }
}

void AdminModule::handleGetModuleConfigResponse(const meshtastic_MeshPacket &mp, meshtastic_AdminMessage *r)
{
    // Skip if it's disabled or no pins are exposed
    if (!r->get_module_config_response.payload_variant.remote_hardware.enabled ||
        r->get_module_config_response.payload_variant.remote_hardware.available_pins_count == 0) {
        LOG_DEBUG("Remote hardware module disabled or no available_pins. Skip");
        return;
    }
    for (uint8_t i = 0; i < devicestate.node_remote_hardware_pins_count; i++) {
        if (devicestate.node_remote_hardware_pins[i].node_num == 0 || !devicestate.node_remote_hardware_pins[i].has_pin) {
            continue;
        }
        for (uint8_t j = 0; j < r->get_module_config_response.payload_variant.remote_hardware.available_pins_count; j++) {
            auto availablePin = r->get_module_config_response.payload_variant.remote_hardware.available_pins[j];
            if (i < devicestate.node_remote_hardware_pins_count) {
                devicestate.node_remote_hardware_pins[i].node_num = mp.from;
                devicestate.node_remote_hardware_pins[i].pin = availablePin;
            }
            i++;
        }
    }
}

/**
 * Setter methods
 */

bool AdminModule::handleSetOwner(const meshtastic_User &o)
{
#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)
    const meshtastic_User oldOwner = owner;
    const meshtastic_Config_SecurityConfig oldSecurity = config.security;
    const meshtastic_ChannelFile oldChannelFile = channelFile;
    const bool oldKeyIsLowEntropy = nodeDB->keyIsLowEntropy;
    const bool oldLicensedIdentityMigrationPending = nodeDB->licensedIdentityMigrationPending;
#endif
    int changed = 0;
    bool identityUpdated = false;
    bool channelsSanitized = false;
    bool licensedRegionDisabled = false;
    bool licensedIdentityMigrationWarningPending = false;

    if (*o.long_name) {
        // Apps built against the older 39-byte limit may send longer names; clamp
        // before the changed-compare so re-sending the same long name is a no-op.
        char longName[sizeof(o.long_name)];
        strncpy(longName, o.long_name, sizeof(longName));
        longName[sizeof(longName) - 1] = '\0';
        clampLongName(longName);
        changed |= strcmp(owner.long_name, longName);
        strncpy(owner.long_name, longName, sizeof(owner.long_name));
        owner.long_name[sizeof(owner.long_name) - 1] = '\0';
    }
    if (*o.short_name) {
        changed |= strcmp(owner.short_name, o.short_name);
        strncpy(owner.short_name, o.short_name, sizeof(owner.short_name));
        owner.short_name[sizeof(owner.short_name) - 1] = '\0';
        sanitizeUtf8(owner.short_name, sizeof(owner.short_name));
    }
    if (owner.is_licensed != o.is_licensed) {
        changed = 1;
#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)
        const bool identityWillMigrate =
            o.is_licensed && config.lora.region != meshtastic_Config_LoRaConfig_RegionCode_UNSET && licensedIdentityWillMigrate();
#endif
        owner.is_licensed = o.is_licensed;
        if (!owner.is_licensed) {
            const RegionInfo *activeRegion = getRegion(config.lora.region);
            if (activeRegion->code == config.lora.region && activeRegion->profile->licensedOnly) {
                // Removing the operator licence and retaining a ham-only
                // region would leave an illegal runtime tuple until the next
                // unrelated config write. Make the owner+radio transition one
                // durable generation and fail silent if that commit fails.
                LOG_WARN("Licensed mode removed while using %s; disabling LoRa until a "
                         "legal region is selected",
                         activeRegion->name);
                config.lora.region = meshtastic_Config_LoRaConfig_RegionCode_UNSET;
                config.lora.tx_enabled = false;
                config.lora.override_frequency = 0;
                config.lora.frequency_offset = 0;
                config.lora.channel_num = 0;
                licensedRegionDisabled = true;
            }
        }
        if (channels.ensureLicensedOperation()) {
            channelsSanitized = true;
        }
#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)
        if (owner.is_licensed && config.lora.region != meshtastic_Config_LoRaConfig_RegionCode_UNSET) {
            if (!nodeDB->generateCryptoKeyPair()) {
                restoreLivePkiIdentity(oldSecurity);
                owner = oldOwner;
                config.security = oldSecurity;
                channelFile = oldChannelFile;
                nodeDB->keyIsLowEntropy = oldKeyIsLowEntropy;
                nodeDB->licensedIdentityMigrationPending = oldLicensedIdentityMigrationPending;
                LOG_ERROR("Licensed-owner request rejected because PKI identity activation failed");
                return false;
            }
            identityUpdated = true;
            licensedIdentityMigrationWarningPending = identityWillMigrate;
        }
#endif
    }
    snprintf(owner.id, sizeof(owner.id), "!%08x", nodeDB->getNodeNum());
    if (owner.has_is_unmessagable != o.has_is_unmessagable ||
        (o.has_is_unmessagable && owner.is_unmessagable != o.is_unmessagable)) {
        changed = 1;
        owner.has_is_unmessagable = owner.has_is_unmessagable || o.has_is_unmessagable;
        owner.is_unmessagable = o.is_unmessagable;
    }

    if (changed) { // If nothing really changed, don't broadcast on the network or
                   // write to flash
        // Update the local cache now, but never announce an identity/name that
        // has not reached durable storage. A failed commit schedules a reboot
        // to roll the in-RAM mutation back without leaking it to the mesh.
        service->reloadOwner(false, false);
        const bool saved = saveChanges(SEGMENT_DEVICESTATE | SEGMENT_NODEDATABASE |
                                       ((identityUpdated || licensedRegionDisabled) ? SEGMENT_CONFIG : 0) |
                                       (channelsSanitized ? SEGMENT_CHANNELS : 0));
        if (saved) {
            if (licensedIdentityMigrationWarningPending) {
                warnLicensedIdentityMigration();
            }
            if (channelsSanitized)
                warnLicensedMode();
            if (hasOpenEditTransaction)
                deferredEditOwnerChanged = true;
            else
                service->reloadOwner(true, false);
        }
        return saved;
    }
    return true;
}

// Apply hardware-visible state only after its complete preference generation
// has committed. This is idempotent so one-shot, bulk, timeout, and physical
// menu commits all converge through the same path.
static void applyCommittedConfigSideEffects()
{
#if !defined(ARCH_PORTDUINO) && !defined(ARCH_STM32WL) && !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR &&                            \
    !MESHTASTIC_EXCLUDE_ACCELEROMETER
    if (accelerometerThread) {
        const bool shouldRun = config.device.double_tap_as_button_press || config.display.wake_on_tap_or_motion;
        if (shouldRun && !accelerometerThread->enabled) {
            accelerometerThread->enabled = true;
            accelerometerThread->start();
        } else if (!shouldRun && accelerometerThread->enabled && !accelerometerThread->providesHeading()) {
            accelerometerThread->disable();
        }
    }
#endif
#ifdef RF95_FAN_EN
    digitalWrite(RF95_FAN_EN, config.lora.pa_fan_disabled ? LOW : HIGH);
#endif
#if HAS_LORA_FEM
    if (loraFEMInterface.isLnaCanControl())
        loraFEMInterface.setLNAEnable(config.lora.fem_lna_mode != meshtastic_Config_LoRaConfig_FEM_LNA_Mode_DISABLED);
#endif
#if !MESHTASTIC_EXCLUDE_GPS
    if (gps) {
        const bool shouldEnable = config.lora.region != meshtastic_Config_LoRaConfig_RegionCode_UNSET &&
                                  config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED;
        if (shouldEnable && !gps->isEnabled())
            gps->enable();
        else if (!shouldEnable && gps->isEnabled())
            gps->disable();
    }
#endif
}

// A "regenerate keys" client sends a blank SecurityConfig holding only the new
// private key, rather than the config it read from us. Detect that shape - new
// private key, every other field at its proto default - so it isn't mistaken
// for "and clear everything else".
static bool isBareKeypairRotation(const meshtastic_Config_SecurityConfig &incoming,
                                  const meshtastic_Config_SecurityConfig &current)
{
    if (incoming.private_key.size != 32)
        return false;
    if (current.private_key.size == 32 && memcmp(incoming.private_key.bytes, current.private_key.bytes, 32) == 0)
        return false;

    return incoming.admin_key_count == 0 && !incoming.is_managed && !incoming.serial_enabled && !incoming.debug_log_api_enabled &&
           !incoming.admin_channel_enabled &&
           incoming.packet_signature_policy ==
               meshtastic_Config_SecurityConfig_PacketSignaturePolicy_PACKET_SIGNATURE_POLICY_COMPATIBLE;
}

bool AdminModule::handleSetConfig(const meshtastic_Config &c, bool fromOthers)
{
    auto changes = SEGMENT_CONFIG;
    auto existingRole = config.device.role;
    bool isRegionUnset = (config.lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET);
    bool requiresReboot = true;
    bool loraPresetWarnPending = false;
    bool licensedIdentityMigrationWarningPending = false;
    meshtastic_Config_LoRaConfig pendingOldLora = {}, pendingNewLora = {};

    switch (c.which_payload_variant) {
    case meshtastic_Config_device_tag: {
        LOG_INFO("Set config: Device");
        config.has_device = true;
        if (config.device.button_gpio == c.payload_variant.device.button_gpio &&
            config.device.buzzer_gpio == c.payload_variant.device.buzzer_gpio &&
            config.device.role == c.payload_variant.device.role &&
            config.device.rebroadcast_mode == c.payload_variant.device.rebroadcast_mode) {
            requiresReboot = false;
        }
        config.device = c.payload_variant.device;
        if (config.device.rebroadcast_mode == meshtastic_Config_DeviceConfig_RebroadcastMode_NONE &&
            (config.device.role == meshtastic_Config_DeviceConfig_Role_ROUTER ||
             config.device.role == meshtastic_Config_DeviceConfig_Role_ROUTER_LATE)) {
            config.device.rebroadcast_mode = meshtastic_Config_DeviceConfig_RebroadcastMode_ALL;
            const char *warning = "Rebroadcast mode can't be set to NONE for a router role";
            LOG_WARN(warning);
            sendWarning(warning);
        }
        if (config.device.node_info_broadcast_secs < min_node_info_broadcast_secs) {
            LOG_DEBUG("node_info_broadcast_secs too low, set to %d", min_node_info_broadcast_secs);
            config.device.node_info_broadcast_secs = min_node_info_broadcast_secs;
        }
        // Router Client and Repeater deprecated; Set it to client
        if (IS_ONE_OF(c.payload_variant.device.role, meshtastic_Config_DeviceConfig_Role_ROUTER_CLIENT,
                      meshtastic_Config_DeviceConfig_Role_REPEATER)) {
            config.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT;
            if (moduleConfig.store_forward.enabled && !moduleConfig.store_forward.is_server) {
                moduleConfig.store_forward.is_server = true;
                changes |= SEGMENT_MODULECONFIG;
                requiresReboot = true;
            }
        }
#if USERPREFS_EVENT_MODE
        // If we're in event mode, nobody is a Router or Router Late
        if (config.device.role == meshtastic_Config_DeviceConfig_Role_ROUTER ||
            config.device.role == meshtastic_Config_DeviceConfig_Role_ROUTER_LATE) {
            config.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT;
        }
#endif
        // Apply role-derived values only after deprecated/event roles have
        // been normalized. Otherwise a ROUTER -> deprecated ROUTER_CLIENT
        // request becomes CLIENT without clearing the infrastructure-only
        // owner semantics left by the previous role.
        if (existingRole != config.device.role) {
            nodeDB->installRoleDefaults(config.device.role, existingRole);
            // Role defaults may update owner/NodeDB plus telemetry and neighbor
            // module intervals; persist the complete derived generation.
            changes |= SEGMENT_NODEDATABASE | SEGMENT_DEVICESTATE | SEGMENT_MODULECONFIG;
        }
        break;
    } // case meshtastic_Config_device_tag
    case meshtastic_Config_position_tag:
        LOG_INFO("Set config: Position");
        config.has_position = true;
        // If we have turned off the GPS (disabled or not present) and we're not
        // using fixed position, clear the stored position since it may not get
        // updated
        if (config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED &&
            c.payload_variant.position.gps_mode != meshtastic_Config_PositionConfig_GpsMode_ENABLED &&
            config.position.fixed_position == false && c.payload_variant.position.fixed_position == false) {
            nodeDB->clearLocalPosition();
            changes |= SEGMENT_NODEDATABASE;
        }
        config.position = c.payload_variant.position;

        // Save nodedb as well in case we got a fixed position packet
        break;
    case meshtastic_Config_power_tag:
        LOG_INFO("Set config: Power");
        config.has_power = true;
        // Really just the adc override is the only thing that can change without a
        // reboot
        if (config.power.device_battery_ina_address == c.payload_variant.power.device_battery_ina_address &&
            config.power.is_power_saving == c.payload_variant.power.is_power_saving &&
            config.power.ls_secs == c.payload_variant.power.ls_secs &&
            config.power.min_wake_secs == c.payload_variant.power.min_wake_secs &&
            config.power.on_battery_shutdown_after_secs == c.payload_variant.power.on_battery_shutdown_after_secs &&
            config.power.sds_secs == c.payload_variant.power.sds_secs &&
            config.power.wait_bluetooth_secs == c.payload_variant.power.wait_bluetooth_secs) {
            requiresReboot = false;
        }
        config.power = c.payload_variant.power;
        if (c.payload_variant.power.on_battery_shutdown_after_secs > 0 &&
            c.payload_variant.power.on_battery_shutdown_after_secs < 30) {
            LOG_WARN("on_battery_shutdown_after_secs too low, set to min 30 sec");
            config.power.on_battery_shutdown_after_secs = 30;
        }
        break;
    case meshtastic_Config_network_tag: {
        LOG_INFO("Set config: WiFi");
        config.has_network = true;
        char prevPsk[sizeof(config.network.wifi_psk)];
        memcpy(prevPsk, config.network.wifi_psk, sizeof(prevPsk));
        config.network = c.payload_variant.network;
        writeSecret(config.network.wifi_psk, sizeof(config.network.wifi_psk), prevPsk);
        break;
    }
    case meshtastic_Config_display_tag:
        LOG_INFO("Set config: Display");
        config.has_display = true;
        if (config.display.screen_on_secs == c.payload_variant.display.screen_on_secs &&
            config.display.flip_screen == c.payload_variant.display.flip_screen &&
            config.display.oled == c.payload_variant.display.oled &&
            config.display.displaymode == c.payload_variant.display.displaymode) {
            requiresReboot = false;
        } else if (config.display.displaymode != meshtastic_Config_DisplayConfig_DisplayMode_COLOR &&
                   c.payload_variant.display.displaymode == meshtastic_Config_DisplayConfig_DisplayMode_COLOR) {
            config.bluetooth.enabled = false;
        }
        config.display = c.payload_variant.display;
        break;

    case meshtastic_Config_lora_tag: {
        // Wrap the entire case in a block to scope variables and avoid crossing
        // initialization
        auto oldLoraConfig = config.lora;
        auto validatedLora = c.payload_variant.lora;
        const bool oldUsesDefaultFrequencySlot = RadioInterface::uses_default_frequency_slot;
        const bool oldUsesCustomChannelName = RadioInterface::uses_custom_channel_name;
        const auto rejectLoraConfig = [&]() {
            // Validation computes slot metadata through legacy static fields.
            // A rejected request must not leave those runtime fields describing
            // a tuple that was never installed.
            config.lora = oldLoraConfig;
            RadioInterface::uses_default_frequency_slot = oldUsesDefaultFrequencySlot;
            RadioInterface::uses_custom_channel_name = oldUsesCustomChannelName;
            return false;
        };

        LOG_INFO("Set config: LoRa");

        // Local clients get the long-standing repair behavior. A remote admin
        // request must be byte-valid as sent; silently rewriting its tuple and
        // ACKing success makes the controller believe a different radio
        // generation was installed.
        if (!fromOthers && validatedLora.coding_rate != clampCodingRate(validatedLora.coding_rate)) {
            LOG_WARN("Invalid coding_rate %d, set to %d", validatedLora.coding_rate, LORA_CR_DEFAULT);
            validatedLora.coding_rate = LORA_CR_DEFAULT;
        }

        if (!fromOthers && validatedLora.spread_factor != clampSpreadFactor(validatedLora.spread_factor)) {
            LOG_WARN("Invalid spread_factor %d, set to %d", validatedLora.spread_factor, LORA_SF_DEFAULT);
            validatedLora.spread_factor = LORA_SF_DEFAULT;
        }

        // A custom (non-preset) config that leaves bandwidth at its proto
        // zero-value otherwise slips through validateConfigLora() and persists as
        // 0, while the radio silently falls back to the default
        // (config.lora.bandwidth then reads back 0 even though the radio runs at
        // 250kHz). Coerce it here like coding_rate/spread_factor so the stored
        // config matches the radio. In preset mode bandwidth 0 is expected (the
        // preset supplies it), so leave it untouched.
        const uint16_t clampedBandwidth = clampBandwidthCode(validatedLora.bandwidth);
        if (!fromOthers && !validatedLora.use_preset && validatedLora.bandwidth != clampedBandwidth) {
            LOG_WARN("Invalid bandwidth %d, set to %d", validatedLora.bandwidth, clampedBandwidth);
            validatedLora.bandwidth = clampedBandwidth;
        }

        if (fromOthers && !validatedLora.use_preset &&
            (validatedLora.coding_rate != clampCodingRate(validatedLora.coding_rate) ||
             validatedLora.spread_factor != clampSpreadFactor(validatedLora.spread_factor) ||
             validatedLora.bandwidth != clampedBandwidth)) {
            // Remote administration must be transactional and exact: do not
            // rewrite a malformed tuple and then acknowledge a configuration
            // different from the one the controller requested.
            LOG_WARN("Malformed remote LoRa tuple; rejecting changes");
            return rejectLoraConfig();
        }

        // If we're setting a new region, check the region is valid and then init
        // the region or discard the change
        if (validatedLora.region != myRegion->code) {
            //  Region has changed so check whether it is valid for e.g. licensing
            //  conditions and if the lora config is valid
            if (RadioInterface::validateConfigRegion(validatedLora) && RadioInterface::validateConfigLora(validatedLora)) {
                // If we're setting region for the first time, init the region and
                // regenerate the keys
                if (isRegionUnset && validatedLora.region > meshtastic_Config_LoRaConfig_RegionCode_UNSET) {
#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)
                    const meshtastic_Config_SecurityConfig oldSecurity = config.security;
                    const meshtastic_User oldOwner = owner;
                    const bool oldKeyIsLowEntropy = nodeDB->keyIsLowEntropy;
                    const bool oldLicensedIdentityMigrationPending = nodeDB->licensedIdentityMigrationPending;
                    const bool identityWillMigrate = owner.is_licensed && licensedIdentityWillMigrate();

                    // Key generation is region-gated. Stage only the already
                    // validated region, and reject the complete request if the
                    // identity cannot be restored or created. Persisting a
                    // first region without a usable key would enable TX under
                    // an address other nodes cannot authenticate.
                    config.lora.region = validatedLora.region;
                    if (!nodeDB->generateCryptoKeyPair()) {
                        restoreLivePkiIdentity(oldSecurity);
                        config.security = oldSecurity;
                        owner = oldOwner;
                        config.lora = oldLoraConfig;
                        nodeDB->keyIsLowEntropy = oldKeyIsLowEntropy;
                        nodeDB->licensedIdentityMigrationPending = oldLicensedIdentityMigrationPending;
                        LOG_ERROR("LoRa region request rejected because PKI identity activation failed");
                        return rejectLoraConfig();
                    }
                    changes |= SEGMENT_DEVICESTATE | SEGMENT_NODEDATABASE;
                    licensedIdentityMigrationWarningPending = identityWillMigrate;
#endif
                    // new region is valid and we're coming from an unset region, so
                    // enable tx
                    validatedLora.tx_enabled = true;
                }
                // If we're unsetting the region for some reason, disable tx
                if (!isRegionUnset && validatedLora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET) {
                    validatedLora.tx_enabled = false;
                }
                // Ensure initRegion() uses the newly validated region
                config.lora.region = validatedLora.region;
                initRegion();
                if (getEffectiveDutyCycle() < 100) {
                    validatedLora.ignore_mqtt = true; // Ignore MQTT by default if region has a duty cycle limit
                }
                if (strncmp(moduleConfig.mqtt.root, default_mqtt_root, strlen(default_mqtt_root)) == 0) {
                    //  Default root is in use, so subscribe to the appropriate MQTT topic
                    //  for this region
                    snprintf(moduleConfig.mqtt.root, sizeof(moduleConfig.mqtt.root), "%s/%s", default_mqtt_root, myRegion->name);
                }
                changes |= SEGMENT_CONFIG | SEGMENT_MODULECONFIG;
            } else {
                // No state has been committed yet. Reject explicitly so the
                // dispatcher sends BAD_REQUEST rather than saving the old
                // tuple and falsely ACKing the invalid request.
                LOG_WARN("Invalid LoRa region/config request; rejecting changes");
                return rejectLoraConfig();
            }
        } // end of new region handling

        if (!RadioInterface::validateConfigLora(validatedLora)) {
            if (fromOthers) {
                // A preset locked to a sibling EU region still swaps the region for
                // remote admin; any other invalid config is rejected outright.
                const RegionInfo *swapRegion =
                    validatedLora.use_preset
                        ? RadioInterface::regionSwapForPreset(validatedLora.region, validatedLora.modem_preset)
                        : NULL;
                if (swapRegion) {
                    validatedLora.region = swapRegion->code;
                }
                if (!swapRegion || !RadioInterface::validateConfigLora(validatedLora)) {
                    LOG_WARN("Invalid LoRa config from another node, rejecting changes");
                    return rejectLoraConfig();
                }
            } else {
                LOG_WARN("Invalid LoRa config from client, using corrected values");
                if (!RadioInterface::clampConfigLora(validatedLora) || !RadioInterface::validateConfigLora(validatedLora)) {
                    // An unknown region or one outside this board's physical
                    // range has no safe automatic replacement. Reject before
                    // changing region-derived state or persisting the tuple;
                    // applyModemConfig() must never be the first component to
                    // discover a durable invalid configuration.
                    LOG_ERROR("LoRa config from client cannot be repaired safely; reject "
                              "changes");
                    return rejectLoraConfig();
                }
            }
            // A preset locked to a sibling EU region swaps the region during the
            // clamp; apply the same housekeeping as an explicit region change.
            if (validatedLora.region != oldLoraConfig.region) {
                config.lora.region = validatedLora.region;
                initRegion();
                if (getEffectiveDutyCycle() < 100) {
                    validatedLora.ignore_mqtt = true; // Ignore MQTT by default if region has a duty cycle limit
                }
                if (strncmp(moduleConfig.mqtt.root, default_mqtt_root, strlen(default_mqtt_root)) == 0) {
                    //  Default root is in use, so subscribe to the appropriate MQTT topic
                    //  for this region
                    snprintf(moduleConfig.mqtt.root, sizeof(moduleConfig.mqtt.root), "%s/%s", default_mqtt_root, myRegion->name);
                }
                changes = SEGMENT_CONFIG | SEGMENT_MODULECONFIG;
            }
            //  use_preset and bandwidth are coerced into valid values by the check.
        }

        // All LoRa radio changes apply live via configChanged observer →
        // reconfigure(). reconfigure() puts the radio in standby, reprograms all
        // modem parameters, and restarts receive.
        requiresReboot = false;

#if defined(ARCH_PORTDUINO)
        // If running on portduino and using SimRadio, do not require reboot
        if (SimRadio::instance) {
            requiresReboot = false;
        }
#endif

#if HAS_LORA_FEM
        // Normalize before persistence; hardware is updated only after commit.
        if (!loraFEMInterface.isLnaCanControl() &&
            validatedLora.fem_lna_mode != meshtastic_Config_LoRaConfig_FEM_LNA_Mode_NOT_PRESENT) {
            // Hardware FEM does not support LNA control; normalize stored config to
            // match actual capability
            LOG_WARN("FEM LNA mode set but FEM lacks LNA control; normalizing to "
                     "NOT_PRESENT");
            validatedLora.fem_lna_mode = meshtastic_Config_LoRaConfig_FEM_LNA_Mode_NOT_PRESENT;
        }
#endif

        config.has_lora = true;
        config.lora = validatedLora; // Finally, return the validated config back to
                                     // the main config
        if (validatedLora.modem_preset != oldLoraConfig.modem_preset) {
            pendingOldLora = oldLoraConfig;
            pendingNewLora = validatedLora;
            loraPresetWarnPending = true;
        }

        break;
    }
    case meshtastic_Config_bluetooth_tag:
        LOG_INFO("Set config: Bluetooth");
        config.has_bluetooth = true;
        config.bluetooth = c.payload_variant.bluetooth;
        break;
    case meshtastic_Config_security_tag: {
        LOG_INFO("Set config: Security");
        meshtastic_Config_SecurityConfig incoming = c.payload_variant.security;
        if (!IS_ONE_OF(incoming.private_key.size, 0, 32) || !IS_ONE_OF(incoming.public_key.size, 0, 32) ||
            incoming.admin_key_count > 3) {
            LOG_WARN("Security set rejected: invalid key length/count");
            return false;
        }
        for (pb_size_t i = 0; i < incoming.admin_key_count; ++i) {
            if (incoming.admin_key[i].size != 32) {
                LOG_WARN("Security set rejected: admin key %u is not 32 bytes", static_cast<unsigned>(i));
                return false;
            }
        }
        // Preserve our keypair when a SET omits the private key but we already hold
        // one: regenerating would change our NodeNum (== crc32(public_key)) and
        // orphan us on the mesh. A SET without the key is a partial/legacy client,
        // not an identity reset (that goes through factory_reset). Done outside the
        // PKI guard so non-PKI builds keep their key bytes too.
        if (incoming.private_key.size == 0 && config.security.private_key.size == 32) {
            LOG_WARN("Security set omitted private key; keeping identity keypair");
            incoming.private_key = config.security.private_key;
            incoming.public_key = config.security.public_key;
        }
        // Rotating the keypair must not drop the admin keys - that locks the owner
        // out of remote admin with no recourse but a physical connection. Clearing
        // admin keys still works via a SET that leaves the private key alone and
        // sends an empty list.
        if (isBareKeypairRotation(incoming, config.security)) {
            LOG_INFO("Security set is bare keypair rotation; keeping other security "
                     "config");
            meshtastic_Config_SecurityConfig rotated = config.security;
            rotated.public_key = incoming.public_key; // usually empty; derived from the private key below
            rotated.private_key = incoming.private_key;
            incoming = rotated;
        }
#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)
        if (incoming.private_key.size == 32) {
            uint8_t derivedPublicKey[32];
            if (!nodeDB->derivePublicKeyForValidation(incoming.private_key.bytes, derivedPublicKey)) {
                LOG_WARN("Security set rejected: private key is invalid");
                return false;
            }
            if (incoming.public_key.size == 32 && memcmp(incoming.public_key.bytes, derivedPublicKey, 32) != 0) {
                LOG_WARN("Security set rejected: public key does not match private key");
                return false;
            }
            incoming.public_key.size = 32;
            memcpy(incoming.public_key.bytes, derivedPublicKey, 32);
            if (nodeDB->checkLowEntropyPublicKey(incoming.public_key)) {
                LOG_WARN("Security set rejected: keypair is known to be compromised");
                sendWarning(LOW_ENTROPY_RESTORE_WARNING);
                return false;
            }
        } else if (incoming.public_key.size != 0) {
            LOG_WARN("Security set rejected: public key supplied without private key");
            return false;
        }
#endif
#if MESHTASTIC_EXCLUDE_PKI || MESHTASTIC_EXCLUDE_XEDDSA
        if (incoming.packet_signature_policy !=
            meshtastic_Config_SecurityConfig_PacketSignaturePolicy_PACKET_SIGNATURE_POLICY_COMPATIBLE) {
            incoming.packet_signature_policy =
                meshtastic_Config_SecurityConfig_PacketSignaturePolicy_PACKET_SIGNATURE_POLICY_COMPATIBLE;
            const char *warning = "Packet authenticity policy is unavailable on this firmware build";
            LOG_WARN(warning);
            sendWarning(warning);
        }
#endif
        config.security = incoming;
#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN) && !(MESHTASTIC_EXCLUDE_PKI)
        // Always install a supplied/preserved key into owner, NodeNum and the
        // live DH engine. Persisting bytes alone would boot into identity
        // recovery if public/private/owner diverged.
        if (config.security.private_key.size == 32) {
            uint8_t validatedPrivateKey[32];
            memcpy(validatedPrivateKey, config.security.private_key.bytes, 32);
            if (!nodeDB->generateCryptoKeyPair(validatedPrivateKey)) {
                mutationPersistenceFailed = true;
                rebootAtMsec = millis() + 1000;
                return false;
            }
        } else if (config.lora.region != meshtastic_Config_LoRaConfig_RegionCode_UNSET && !nodeDB->generateCryptoKeyPair()) {
            mutationPersistenceFailed = true;
            rebootAtMsec = millis() + 1000;
            return false;
        }
#endif
        if (config.security.is_managed && !(config.security.admin_key[0].size == 32 || config.security.admin_key[1].size == 32 ||
                                            config.security.admin_key[2].size == 32)) {
            config.security.is_managed = false;
            const char *warning = "You must provide at least one admin public key to "
                                  "enable managed mode";
            LOG_WARN(warning);
            sendWarning(warning);
        }

        changes = SEGMENT_CONFIG | SEGMENT_DEVICESTATE | SEGMENT_NODEDATABASE;

        requiresReboot = true;

        break;
    }
    case meshtastic_Config_device_ui_tag:
        // NOOP! This is handled by handleStoreDeviceUIConfig
        break;
    }
    const bool saved = saveChanges(changes, requiresReboot);
    if (saved && requiresReboot && !hasOpenEditTransaction)
        disableBluetooth();
    if (saved && loraPresetWarnPending)
        warnOnLoraPresetChange(pendingOldLora, pendingNewLora);
    if (saved && licensedIdentityMigrationWarningPending) {
        warnLicensedIdentityMigration();
    }
    // Inside an edit transaction the queued warnings are flushed once at commit;
    // otherwise emit now.
    if (saved && !hasOpenEditTransaction)
        flushChannelWarnings();
    return saved;
} // end of handleSetConfig

bool AdminModule::handleSetModuleConfig(const meshtastic_ModuleConfig &c)
{
    bool shouldReboot = true;
    switch (c.which_payload_variant) {
    case meshtastic_ModuleConfig_mqtt_tag:
#if MESHTASTIC_EXCLUDE_MQTT
        LOG_WARN("Set module config: MESHTASTIC_EXCLUDE_MQTT defined, skip MQTT config");
        return false;
#else
        LOG_INFO("Set module config: MQTT");
        if (!MQTT::isValidConfig(c.payload_variant.mqtt)) {
            return false;
        }
        moduleConfig.has_mqtt = true;
        {
            char prevPass[sizeof(moduleConfig.mqtt.password)];
            memcpy(prevPass, moduleConfig.mqtt.password, sizeof(prevPass));
            moduleConfig.mqtt = c.payload_variant.mqtt;
            writeSecret(moduleConfig.mqtt.password, sizeof(moduleConfig.mqtt.password), prevPass);
        }
#endif
        break;
    case meshtastic_ModuleConfig_serial_tag:
        LOG_INFO("Set module config: Serial");
        // No architecture guard: the check and the store below must agree on every
        // platform.
        if (!serialConfigIsValid(c.payload_variant.serial)) {
            LOG_ERROR("Invalid serial config");
            return false;
        }
        moduleConfig.has_serial = true;
        moduleConfig.serial = c.payload_variant.serial;
        break;
    case meshtastic_ModuleConfig_external_notification_tag:
        LOG_INFO("Set module config: External Notification");
        moduleConfig.has_external_notification = true;
        moduleConfig.external_notification = c.payload_variant.external_notification;
        break;
    case meshtastic_ModuleConfig_store_forward_tag:
        LOG_INFO("Set module config: Store & Forward");
        moduleConfig.has_store_forward = true;
        moduleConfig.store_forward = c.payload_variant.store_forward;
        break;
    case meshtastic_ModuleConfig_range_test_tag:
        LOG_INFO("Set module config: Range Test");
        moduleConfig.has_range_test = true;
        moduleConfig.range_test = c.payload_variant.range_test;
        break;
    case meshtastic_ModuleConfig_telemetry_tag:
        LOG_INFO("Set module config: Telemetry");
        moduleConfig.has_telemetry = true;
        moduleConfig.telemetry = c.payload_variant.telemetry;
        break;
    case meshtastic_ModuleConfig_canned_message_tag:
        LOG_INFO("Set module config: Canned Message");
        moduleConfig.has_canned_message = true;
        moduleConfig.canned_message = c.payload_variant.canned_message;
        break;
    case meshtastic_ModuleConfig_audio_tag:
        LOG_INFO("Set module config: Audio");
        moduleConfig.has_audio = true;
        moduleConfig.audio = c.payload_variant.audio;
        break;
    case meshtastic_ModuleConfig_remote_hardware_tag:
        LOG_INFO("Set module config: Remote Hardware");
        moduleConfig.has_remote_hardware = true;
        moduleConfig.remote_hardware = c.payload_variant.remote_hardware;
        break;
    case meshtastic_ModuleConfig_neighbor_info_tag:
        LOG_INFO("Set module config: Neighbor Info");
        moduleConfig.has_neighbor_info = true;
        moduleConfig.neighbor_info = c.payload_variant.neighbor_info;
        if (moduleConfig.neighbor_info.update_interval < min_neighbor_info_broadcast_secs) {
            LOG_DEBUG("update_interval too low, set to %d", default_neighbor_info_broadcast_secs);
            moduleConfig.neighbor_info.update_interval = default_neighbor_info_broadcast_secs;
        }
        break;
    case meshtastic_ModuleConfig_detection_sensor_tag:
        LOG_INFO("Set module config: Detection Sensor");
        moduleConfig.has_detection_sensor = true;
        moduleConfig.detection_sensor = c.payload_variant.detection_sensor;
        break;
    case meshtastic_ModuleConfig_ambient_lighting_tag:
        LOG_INFO("Set module config: Ambient Lighting");
        moduleConfig.has_ambient_lighting = true;
        moduleConfig.ambient_lighting = c.payload_variant.ambient_lighting;
        break;
    case meshtastic_ModuleConfig_paxcounter_tag:
        LOG_INFO("Set module config: Paxcounter");
        moduleConfig.has_paxcounter = true;
        moduleConfig.paxcounter = c.payload_variant.paxcounter;
        break;
    case meshtastic_ModuleConfig_statusmessage_tag:
        LOG_INFO("Set module config: StatusMessage");
        moduleConfig.has_statusmessage = true;
        moduleConfig.statusmessage = c.payload_variant.statusmessage;
        shouldReboot = false;
        break;
    case meshtastic_ModuleConfig_traffic_management_tag:
        LOG_INFO("Set module config: Traffic Management");
        moduleConfig.has_traffic_management = true;
        moduleConfig.traffic_management = c.payload_variant.traffic_management;
        break;
    case meshtastic_ModuleConfig_tak_tag:
        LOG_INFO("Set module config: TAK");
        moduleConfig.has_tak = true;
        moduleConfig.tak = c.payload_variant.tak;
        break;
#if !MESHTASTIC_EXCLUDE_BEACON
    case meshtastic_ModuleConfig_mesh_beacon_tag: {
        LOG_INFO("Set module config: MeshBeacon");
        // Sanitize a local copy rather than const_cast-ing the const input (UB if a
        // truly-const object is ever passed); the validated copy is assigned into
        // moduleConfig below.
        auto beaconCfg = c.payload_variant.mesh_beacon;
        // Hard cap at 100 chars.
        beaconCfg.broadcast_message[100] = '\0';
        // Enforce interval minimum (0 means unset/use default).
        if (beaconCfg.broadcast_interval_secs != 0 &&
            beaconCfg.broadcast_interval_secs < default_mesh_beacon_min_broadcast_interval_secs)
            beaconCfg.broadcast_interval_secs = default_mesh_beacon_min_broadcast_interval_secs;
        // Validate broadcast_offer_preset against broadcast_offer_region (or
        // current region if unset).
        if (beaconCfg.has_broadcast_offer_preset) {
            meshtastic_Config_LoRaConfig probe = config.lora;
            probe.use_preset = true;
            probe.modem_preset = beaconCfg.broadcast_offer_preset;
            if (beaconCfg.broadcast_offer_region != meshtastic_Config_LoRaConfig_RegionCode_UNSET)
                probe.region = beaconCfg.broadcast_offer_region;
            if (!RadioInterface::validateConfigLora(probe)) {
                LOG_WARN("Beacon: broadcast_offer_preset %d invalid for region, clearing", beaconCfg.broadcast_offer_preset);
                beaconCfg.has_broadcast_offer_preset = false;
            }
        }
        // Validate broadcast_offer_region is a known region code.
        if (beaconCfg.broadcast_offer_region != meshtastic_Config_LoRaConfig_RegionCode_UNSET) {
            const RegionInfo *r = getRegion(beaconCfg.broadcast_offer_region);
            if (r->code != beaconCfg.broadcast_offer_region) {
                LOG_WARN("Beacon: broadcast_offer_region %d invalid, clearing", beaconCfg.broadcast_offer_region);
                beaconCfg.broadcast_offer_region = meshtastic_Config_LoRaConfig_RegionCode_UNSET;
            }
        }
        // Validate each broadcast target so a bad preset/region is cleared on write
        // rather than relying on the runtime TX drop.
        for (pb_size_t i = 0; i < beaconCfg.broadcast_targets_count; i++) {
            auto &t = beaconCfg.broadcast_targets[i];
            // Region must be a known region code (UNSET = use running config at TX
            // time).
            if (t.region != meshtastic_Config_LoRaConfig_RegionCode_UNSET) {
                const RegionInfo *r = getRegion(t.region);
                if (r->code != t.region) {
                    LOG_WARN("Beacon: broadcast_targets[%u] region %d invalid, clearing", i, t.region);
                    t.region = meshtastic_Config_LoRaConfig_RegionCode_UNSET;
                }
            }
            // Preset must be valid for the target region (or current region if
            // unset).
            if (t.has_preset) {
                meshtastic_Config_LoRaConfig probe = config.lora;
                probe.use_preset = true;
                probe.modem_preset = t.preset;
                if (t.region != meshtastic_Config_LoRaConfig_RegionCode_UNSET)
                    probe.region = t.region;
                if (!RadioInterface::validateConfigLora(probe)) {
                    LOG_WARN("Beacon: broadcast_targets[%u] preset %d invalid for "
                             "region, clearing",
                             i, t.preset);
                    t.has_preset = false;
                    t.has_channel_index = false;
                }
            }
            // channel_index must reference a real channel-table slot.
            if (t.has_channel_index && t.channel_index >= MAX_NUM_CHANNELS) {
                LOG_WARN("Beacon: broadcast_targets[%u] channel_index %u out of range, "
                         "clearing",
                         i, t.channel_index);
                t.has_channel_index = false;
            }
        }
        moduleConfig.has_mesh_beacon = true;
        moduleConfig.mesh_beacon = beaconCfg;
        shouldReboot = false;
        // Payload content changed - invalidate the broadcaster's cache.
        if (meshBeaconBroadcastModule)
            meshBeaconBroadcastModule->invalidateCache();
        break;
    }
#endif
    }
    const bool saved = saveChanges(SEGMENT_MODULECONFIG, shouldReboot);
    if (saved && shouldReboot && !hasOpenEditTransaction)
        disableBluetooth();
    return saved;
}

void AdminModule::handleSetChannel(const meshtastic_Channel &cc)
{
    channels.setChannel(cc);
    if (channels.ensureLicensedOperation()) {
        warnLicensedMode();
    }
    // Refresh derived state (primaryIndex in particular) BEFORE the precision
    // clamp below. usesPublicKey() resolves a secondary channel's key against the
    // primary, so it must see the post-update primaryIndex; running the clamp
    // first could evaluate secondaries against the previous primary and skip the
    // clamp/warning.
    channels.onConfigChanged(false); // normalize only; runtime activation follows the durable commit

    // Persist the public-key precision clamp for all channels that may be
    // affected (e.g. secondaries that inherit a now-public primary key) and warn
    // the client once if anything was coarsened.
    bool clamped = false;
    for (uint8_t i = 0; i < channels.getNumChannels(); i++) {
        meshtastic_Channel &ch = channels.getByIndex(i);
        if (ch.role == meshtastic_Channel_Role_DISABLED || !ch.settings.has_module_settings)
            continue;
        uint32_t allowed = getPositionPrecisionForChannel(i);
        if (allowed != ch.settings.module_settings.position_precision) {
            ch.settings.module_settings.position_precision = allowed;
            clamped = true;
        }
    }
    if (clamped)
        sendWarning(publicChannelPrecisionMessage);
    saveChanges(SEGMENT_CHANNELS, false);
    warnOnChannelSet(channels.getByIndex(cc.index)); // passes the saved channel
    // Inside an edit transaction the queued warnings are flushed once at commit;
    // otherwise emit now.
    if (!hasOpenEditTransaction)
        flushChannelWarnings();
}

/**
 * Getters
 */

void AdminModule::handleGetOwner(const meshtastic_MeshPacket &req)
{
    if (req.decoded.want_response) {
        // We create the reply here
        meshtastic_AdminMessage res = meshtastic_AdminMessage_init_default;
        res.get_owner_response = owner;

        res.which_payload_variant = meshtastic_AdminMessage_get_owner_response_tag;
        setPassKey(&res);
        myReply = allocDataProtobuf(res);
        if (!myReply) {
            return;
        }
        if (req.pki_encrypted) {
            myReply->pki_encrypted = true;
        }
    }
}

void AdminModule::handleGetConfig(const meshtastic_MeshPacket &req, const uint32_t configType)
{
    meshtastic_AdminMessage res = meshtastic_AdminMessage_init_default;

    if (req.decoded.want_response) {
        switch (configType) {
        case meshtastic_AdminMessage_ConfigType_DEVICE_CONFIG:
            LOG_INFO("Get config: Device");
            res.get_config_response.which_payload_variant = meshtastic_Config_device_tag;
            res.get_config_response.payload_variant.device = config.device;
            break;
        case meshtastic_AdminMessage_ConfigType_POSITION_CONFIG:
            LOG_INFO("Get config: Position");
            res.get_config_response.which_payload_variant = meshtastic_Config_position_tag;
            res.get_config_response.payload_variant.position = config.position;
            break;
        case meshtastic_AdminMessage_ConfigType_POWER_CONFIG:
            LOG_INFO("Get config: Power");
            res.get_config_response.which_payload_variant = meshtastic_Config_power_tag;
            res.get_config_response.payload_variant.power = config.power;
            break;
        case meshtastic_AdminMessage_ConfigType_NETWORK_CONFIG:
            LOG_INFO("Get config: Network");
            res.get_config_response.which_payload_variant = meshtastic_Config_network_tag;
            res.get_config_response.payload_variant.network = config.network;
            if (req.from != 0)
                strncpy(res.get_config_response.payload_variant.network.wifi_psk, secretReserved,
                        sizeof(res.get_config_response.payload_variant.network.wifi_psk));
            break;
        case meshtastic_AdminMessage_ConfigType_DISPLAY_CONFIG:
            LOG_INFO("Get config: Display");
            res.get_config_response.which_payload_variant = meshtastic_Config_display_tag;
            res.get_config_response.payload_variant.display = config.display;
            break;
        case meshtastic_AdminMessage_ConfigType_LORA_CONFIG:
            LOG_INFO("Get config: LoRa");
            res.get_config_response.which_payload_variant = meshtastic_Config_lora_tag;
            res.get_config_response.payload_variant.lora = config.lora;
            break;
        case meshtastic_AdminMessage_ConfigType_BLUETOOTH_CONFIG:
            LOG_INFO("Get config: Bluetooth");
            res.get_config_response.which_payload_variant = meshtastic_Config_bluetooth_tag;
            res.get_config_response.payload_variant.bluetooth = config.bluetooth;
            break;
        case meshtastic_AdminMessage_ConfigType_SECURITY_CONFIG:
            LOG_INFO("Get config: Security");
            res.get_config_response.which_payload_variant = meshtastic_Config_security_tag;
            res.get_config_response.payload_variant.security = config.security;
            // The device identity private key is backup material for the local owner
            // only. A local admin client sets from == 0 (BLE/USB/TCP); never return
            // the key to a remote requester, even an authorized one, since it would
            // travel over the air. public_key/admin_key are public and stay put.
            if (req.from != 0) {
                auto &sec = res.get_config_response.payload_variant.security;
                memset(sec.private_key.bytes, 0, sizeof(sec.private_key.bytes));
                sec.private_key.size = 0;
            }
            break;
        case meshtastic_AdminMessage_ConfigType_SESSIONKEY_CONFIG:
            LOG_INFO("Get config: Sessionkey");
            res.get_config_response.which_payload_variant = meshtastic_Config_sessionkey_tag;
            break;
        case meshtastic_AdminMessage_ConfigType_DEVICEUI_CONFIG:
            // NOOP! This is handled by handleGetDeviceUIConfig
            res.get_config_response.which_payload_variant = meshtastic_Config_device_ui_tag;
            break;
        }
        // NOTE: The phone app needs to know the ls_secs value so it can properly
        // expect sleep behavior. So even if we internally use 0 to represent 'use
        // default' we still need to send the value we are using to the app (so that
        // even old phone apps work with new device loads).
        // r.get_radio_response.preferences.ls_secs = getPref_ls_secs();
        // hideSecret(r.get_radio_response.preferences.wifi_ssid); // hmm - leave
        // public for now, because only minimally private and useful for users to
        // know current provisioning)
        // hideSecret(r.get_radio_response.preferences.wifi_password);
        // r.get_config_response.which_payloadVariant =
        // Config_ModuleConfig_telemetry_tag;
        res.which_payload_variant = meshtastic_AdminMessage_get_config_response_tag;
        setPassKey(&res);
        myReply = allocDataProtobuf(res);
        if (!myReply) {
            return;
        }
        if (req.pki_encrypted) {
            myReply->pki_encrypted = true;
        }
    }
}

void AdminModule::handleGetModuleConfig(const meshtastic_MeshPacket &req, const uint32_t configType)
{
    meshtastic_AdminMessage res = meshtastic_AdminMessage_init_default;

    if (req.decoded.want_response) {
        const char *configName = "?";
        switch (configType) {
        case meshtastic_AdminMessage_ModuleConfigType_MQTT_CONFIG:
            configName = "MQTT";
            res.get_module_config_response.which_payload_variant = meshtastic_ModuleConfig_mqtt_tag;
            res.get_module_config_response.payload_variant.mqtt = moduleConfig.mqtt;
            if (req.from != 0)
                strncpy(res.get_module_config_response.payload_variant.mqtt.password, secretReserved,
                        sizeof(res.get_module_config_response.payload_variant.mqtt.password));
            break;
        case meshtastic_AdminMessage_ModuleConfigType_SERIAL_CONFIG:
            configName = "Serial";
            res.get_module_config_response.which_payload_variant = meshtastic_ModuleConfig_serial_tag;
            res.get_module_config_response.payload_variant.serial = moduleConfig.serial;
            break;
        case meshtastic_AdminMessage_ModuleConfigType_EXTNOTIF_CONFIG:
            configName = "External Notification";
            res.get_module_config_response.which_payload_variant = meshtastic_ModuleConfig_external_notification_tag;
            res.get_module_config_response.payload_variant.external_notification = moduleConfig.external_notification;
            break;
        case meshtastic_AdminMessage_ModuleConfigType_STOREFORWARD_CONFIG:
            configName = "Store & Forward";
            res.get_module_config_response.which_payload_variant = meshtastic_ModuleConfig_store_forward_tag;
            res.get_module_config_response.payload_variant.store_forward = moduleConfig.store_forward;
            break;
        case meshtastic_AdminMessage_ModuleConfigType_RANGETEST_CONFIG:
            configName = "Range Test";
            res.get_module_config_response.which_payload_variant = meshtastic_ModuleConfig_range_test_tag;
            res.get_module_config_response.payload_variant.range_test = moduleConfig.range_test;
            break;
        case meshtastic_AdminMessage_ModuleConfigType_TELEMETRY_CONFIG:
            configName = "Telemetry";
            res.get_module_config_response.which_payload_variant = meshtastic_ModuleConfig_telemetry_tag;
            res.get_module_config_response.payload_variant.telemetry = moduleConfig.telemetry;
            break;
        case meshtastic_AdminMessage_ModuleConfigType_CANNEDMSG_CONFIG:
            configName = "Canned Message";
            res.get_module_config_response.which_payload_variant = meshtastic_ModuleConfig_canned_message_tag;
            res.get_module_config_response.payload_variant.canned_message = moduleConfig.canned_message;
            break;
        case meshtastic_AdminMessage_ModuleConfigType_AUDIO_CONFIG:
            configName = "Audio";
            res.get_module_config_response.which_payload_variant = meshtastic_ModuleConfig_audio_tag;
            res.get_module_config_response.payload_variant.audio = moduleConfig.audio;
            break;
        case meshtastic_AdminMessage_ModuleConfigType_REMOTEHARDWARE_CONFIG:
            configName = "Remote Hardware";
            res.get_module_config_response.which_payload_variant = meshtastic_ModuleConfig_remote_hardware_tag;
            res.get_module_config_response.payload_variant.remote_hardware = moduleConfig.remote_hardware;
            break;
        case meshtastic_AdminMessage_ModuleConfigType_NEIGHBORINFO_CONFIG:
            configName = "Neighbor Info";
            res.get_module_config_response.which_payload_variant = meshtastic_ModuleConfig_neighbor_info_tag;
            res.get_module_config_response.payload_variant.neighbor_info = moduleConfig.neighbor_info;
            break;
        case meshtastic_AdminMessage_ModuleConfigType_DETECTIONSENSOR_CONFIG:
            configName = "Detection Sensor";
            res.get_module_config_response.which_payload_variant = meshtastic_ModuleConfig_detection_sensor_tag;
            res.get_module_config_response.payload_variant.detection_sensor = moduleConfig.detection_sensor;
            break;
        case meshtastic_AdminMessage_ModuleConfigType_AMBIENTLIGHTING_CONFIG:
            configName = "Ambient Lighting";
            res.get_module_config_response.which_payload_variant = meshtastic_ModuleConfig_ambient_lighting_tag;
            res.get_module_config_response.payload_variant.ambient_lighting = moduleConfig.ambient_lighting;
            break;
        case meshtastic_AdminMessage_ModuleConfigType_PAXCOUNTER_CONFIG:
            configName = "Paxcounter";
            res.get_module_config_response.which_payload_variant = meshtastic_ModuleConfig_paxcounter_tag;
            res.get_module_config_response.payload_variant.paxcounter = moduleConfig.paxcounter;
            break;
        case meshtastic_AdminMessage_ModuleConfigType_STATUSMESSAGE_CONFIG:
            configName = "StatusMessage";
            res.get_module_config_response.which_payload_variant = meshtastic_ModuleConfig_statusmessage_tag;
            res.get_module_config_response.payload_variant.statusmessage = moduleConfig.statusmessage;
            break;
        case meshtastic_AdminMessage_ModuleConfigType_TRAFFICMANAGEMENT_CONFIG:
            configName = "Traffic Management";
            res.get_module_config_response.which_payload_variant = meshtastic_ModuleConfig_traffic_management_tag;
            res.get_module_config_response.payload_variant.traffic_management = moduleConfig.traffic_management;
            break;
        case meshtastic_AdminMessage_ModuleConfigType_TAK_CONFIG:
            configName = "TAK";
            res.get_module_config_response.which_payload_variant = meshtastic_ModuleConfig_tak_tag;
            res.get_module_config_response.payload_variant.tak = moduleConfig.tak;
            break;
#if !MESHTASTIC_EXCLUDE_BEACON
        case meshtastic_AdminMessage_ModuleConfigType_MESHBEACON_CONFIG:
            configName = "MeshBeacon";
            res.get_module_config_response.which_payload_variant = meshtastic_ModuleConfig_mesh_beacon_tag;
            res.get_module_config_response.payload_variant.mesh_beacon = moduleConfig.mesh_beacon;
            break;
#endif
        }
        LOG_INFO("Get module config: %s", configName);

        // NOTE: The phone app needs to know the ls_secsvalue so it can properly
        // expect sleep behavior. So even if we internally use 0 to represent 'use
        // default' we still need to send the value we are using to the app (so that
        // even old phone apps work with new device loads).
        // r.get_radio_response.preferences.ls_secs = getPref_ls_secs();
        // hideSecret(r.get_radio_response.preferences.wifi_ssid); // hmm - leave
        // public for now, because only minimally private and useful for users to
        // know current provisioning)
        // hideSecret(r.get_radio_response.preferences.wifi_password);
        // r.get_config_response.which_payloadVariant =
        // Config_ModuleConfig_telemetry_tag;
        res.which_payload_variant = meshtastic_AdminMessage_get_module_config_response_tag;
        setPassKey(&res);
        myReply = allocDataProtobuf(res);
        if (!myReply) {
            return;
        }
        if (req.pki_encrypted) {
            myReply->pki_encrypted = true;
        }
    }
}

void AdminModule::handleGetNodeRemoteHardwarePins(const meshtastic_MeshPacket &req)
{
    meshtastic_AdminMessage r = meshtastic_AdminMessage_init_default;
    r.which_payload_variant = meshtastic_AdminMessage_get_node_remote_hardware_pins_response_tag;
    for (uint8_t i = 0; i < devicestate.node_remote_hardware_pins_count; i++) {
        if (devicestate.node_remote_hardware_pins[i].node_num == 0 || !devicestate.node_remote_hardware_pins[i].has_pin) {
            continue;
        }
        r.get_node_remote_hardware_pins_response.node_remote_hardware_pins[i] = devicestate.node_remote_hardware_pins[i];
    }
    for (uint8_t i = 0; i < moduleConfig.remote_hardware.available_pins_count; i++) {
        if (!moduleConfig.remote_hardware.available_pins[i].gpio_pin) {
            continue;
        }
        meshtastic_NodeRemoteHardwarePin nodePin = meshtastic_NodeRemoteHardwarePin_init_default;
        nodePin.node_num = nodeDB->getNodeNum();
        nodePin.pin = moduleConfig.remote_hardware.available_pins[i];
        r.get_node_remote_hardware_pins_response.node_remote_hardware_pins[i + 12] = nodePin;
    }
    setPassKey(&r);
    myReply = allocDataProtobuf(r);
    if (!myReply) {
        return;
    }
    if (req.pki_encrypted) {
        myReply->pki_encrypted = true;
    }
}

void AdminModule::handleGetDeviceMetadata(const meshtastic_MeshPacket &req)
{
#if WARM_NODE_COUNT > 0 && MESHTASTIC_NODEDB_MIGRATION_VERBOSE
    // Debug aid: dump the warm tier to the console on a local metadata request
    // (e.g. `meshtastic --info` over USB/BLE). Gated to req.from == 0 so remote
    // or admin polling can't spam the console.
    if (nodeDB && req.from == 0)
        nodeDB->warmStore.dumpToLog("admin get_metadata");
#endif
    meshtastic_AdminMessage r = meshtastic_AdminMessage_init_default;
    r.get_device_metadata_response = getDeviceMetadata();
    r.which_payload_variant = meshtastic_AdminMessage_get_device_metadata_response_tag;
    setPassKey(&r);
    myReply = allocDataProtobuf(r);
    if (!myReply) {
        return;
    }
    if (req.pki_encrypted) {
        myReply->pki_encrypted = true;
    }
}

void AdminModule::handleGetDeviceConnectionStatus(const meshtastic_MeshPacket &req)
{
    meshtastic_AdminMessage r = meshtastic_AdminMessage_init_default;

    meshtastic_DeviceConnectionStatus conn = meshtastic_DeviceConnectionStatus_init_zero;

#if HAS_WIFI
    conn.has_wifi = true;
    conn.wifi.has_status = true;
#ifdef ARCH_PORTDUINO
    conn.wifi.status.is_connected = true;
#else
    conn.wifi.status.is_connected = WiFi.status() == WL_CONNECTED;
#endif
    strncpy(conn.wifi.ssid, config.network.wifi_ssid, 33);
    if (conn.wifi.status.is_connected) {
        conn.wifi.rssi = WiFi.RSSI();
        conn.wifi.status.ip_address = WiFi.localIP();
#ifndef MESHTASTIC_EXCLUDE_MQTT
        conn.wifi.status.is_mqtt_connected = mqtt && mqtt->isConnectedDirectly();
#endif
        conn.wifi.status.is_syslog_connected = false; // FIXME wire this up
    }
#endif

#if HAS_ETHERNET && !defined(USE_WS5500) && !defined(USE_CH390D)
    conn.has_ethernet = true;
    conn.ethernet.has_status = true;
    if (Ethernet.linkStatus() == LinkON) {
        conn.ethernet.status.is_connected = true;
        conn.ethernet.status.ip_address = Ethernet.localIP();
#if !MESHTASTIC_EXCLUDE_MQTT
        conn.ethernet.status.is_mqtt_connected = mqtt && mqtt->isConnectedDirectly();
#endif
        conn.ethernet.status.is_syslog_connected = false; // FIXME wire this up
    } else {
        conn.ethernet.status.is_connected = false;
    }
#endif

#if HAS_BLUETOOTH
    conn.has_bluetooth = true;
    conn.bluetooth.pin = config.bluetooth.fixed_pin;
#ifdef ARCH_ESP32
    if (config.bluetooth.enabled && nimbleBluetooth) {
        conn.bluetooth.is_connected = nimbleBluetooth->isConnected();
        conn.bluetooth.rssi = nimbleBluetooth->getRssi();
    }
#elif defined(ARCH_NRF52)
    if (config.bluetooth.enabled && nrf52Bluetooth) {
        conn.bluetooth.is_connected = nrf52Bluetooth->isConnected();
    }
#elif defined(ARCH_NRF54L15)
    if (config.bluetooth.enabled && nrf54l15Bluetooth) {
        conn.bluetooth.is_connected = nrf54l15Bluetooth->isConnected();
    }
#endif
#endif
    conn.has_serial = true; // No serial-less devices
#if !MESHTASTIC_EXCLUDE_POWER_FSM
    conn.serial.is_connected = powerFSM.getState() == &stateSERIAL;
#else
    conn.serial.is_connected = powerFSM.getState();
#endif
    conn.serial.baud = SERIAL_BAUD;

    r.get_device_connection_status_response = conn;
    r.which_payload_variant = meshtastic_AdminMessage_get_device_connection_status_response_tag;
    setPassKey(&r);
    myReply = allocDataProtobuf(r);
    if (!myReply) {
        return;
    }
    if (req.pki_encrypted) {
        myReply->pki_encrypted = true;
    }
}

void AdminModule::handleGetChannel(const meshtastic_MeshPacket &req, uint32_t channelIndex)
{
    if (req.decoded.want_response) {
        // We create the reply here
        meshtastic_AdminMessage r = meshtastic_AdminMessage_init_default;
        r.get_channel_response = channels.getByIndex(channelIndex);
        r.which_payload_variant = meshtastic_AdminMessage_get_channel_response_tag;
        setPassKey(&r);
        myReply = allocDataProtobuf(r);
        if (!myReply) {
            return;
        }
        if (req.pki_encrypted) {
            myReply->pki_encrypted = true;
        }
    }
}

void AdminModule::handleGetDeviceUIConfig(const meshtastic_MeshPacket &req)
{
    meshtastic_AdminMessage r = meshtastic_AdminMessage_init_default;
    r.which_payload_variant = meshtastic_AdminMessage_get_ui_config_response_tag;
    r.get_ui_config_response = uiconfig;
    myReply = allocDataProtobuf(r);
    if (!myReply) {
        return;
    }
    if (req.pki_encrypted) {
        myReply->pki_encrypted = true;
    }
}

void AdminModule::reboot(int32_t seconds)
{
    LOG_INFO("Reboot in %d seconds", seconds);
    if (screen)
        screen->showSimpleBanner("Rebooting...", 0); // stays on screen
    rebootAtMsec = (seconds < 0) ? 0 : (millis() + seconds * 1000);
}

// Without this, a commit that never arrives leaves the transaction open forever
// and every later config write from any client is applied, acknowledged, and
// then never saved.
void AdminModule::expireStaleEditTransaction()
{
    if (!hasOpenEditTransaction || Throttle::isWithinTimespanMs(editTransactionActivityMs, EDIT_TRANSACTION_IDLE_MS))
        return;

    LOG_WARN("Edit transaction abandoned for %us; committing what it applied", EDIT_TRANSACTION_IDLE_MS / 1000);
    int segments = deferredEditSegments;
    const bool requiresReboot = deferredEditRequiresReboot;
    const bool ownerChanged = deferredEditOwnerChanged;
    const bool announceFixedPosition = deferredFixedPositionAnnouncement;
    // Even an empty abandoned transaction parked LoRa at BEGIN. Persisting the
    // unchanged radio profile through MeshService clears the marker and
    // deterministically re-arms the prior configuration.
    const int commitSegments = segments | SEGMENT_CONFIG | SEGMENT_CHANNELS;
    if (!nodeDB->adoptAbandonedPreferenceEdit()) {
        LOG_ERROR("Abandoned settings transaction ownership could not be recovered");
        return;
    }
    const bool committed = service->reloadConfig(commitSegments, true);
    if (!committed) {
        LOG_ERROR("Abandoned settings transaction could not be committed");
        return;
    }
    hasOpenEditTransaction = false;
    editTransactionOwner = 0;
    deferredEditSegments = 0;
    deferredEditRequiresReboot = false;
    deferredEditOwnerChanged = false;
    deferredFixedPositionAnnouncement = false;
    // Most values are already live in RAM, but Bluetooth enablement and the
    // one-time PowerFSM graph are not. Preserve the normal restart requirement
    // even when the client abandoned the transaction.
    applyCommittedConfigSideEffects();
    flushChannelWarnings();
    if (ownerChanged)
        service->reloadOwner(true, false);
    if (announceFixedPosition && config.position.fixed_position && positionModule)
        positionModule->sendOurPosition();
    applyDeferredMessagePurges();
    if (requiresReboot)
        reboot(DEFAULT_REBOOT_SECONDS);
}

void AdminModule::serviceEditTransactionTimeout()
{
    concurrency::LockGuard transactionGuard(&editTransactionLock);
    expireStaleEditTransaction();
}

bool AdminModule::saveChanges(int saveWhat, bool shouldReboot)
{
#ifdef PIO_UNIT_TESTING
    lastSaveWhatForTest = saveWhat;
#endif
    if (externalMutationActive) {
        LOG_INFO("Coalesce save into physical/UI settings transaction");
        externalMutationSegments |= saveWhat;
        externalMutationRequiresReboot |= shouldReboot;
        return true;
    }
    if (!hasOpenEditTransaction) {
        LOG_INFO("Save changes to disk");
        const bool commitImplicitEdit = nodeDB->isPreferenceEditTransactionActive() && nodeDB->isPreferenceEditOwnerCurrentTask();
        if (!service->reloadConfig(saveWhat, commitImplicitEdit)) {
            mutationPersistenceFailed = true;
            rebootAtMsec = millis() + 1000;
            return false;
        }
        if (saveWhat & SEGMENT_CONFIG)
            applyCommittedConfigSideEffects();
    } else {
        LOG_INFO("Delay disk save until open transaction commits");
        editTransactionActivityMs = millis(); // still in use, so not the abandoned kind we time out
        deferredEditSegments |= saveWhat;
        deferredEditRequiresReboot |= shouldReboot;
    }
    if (shouldReboot && !hasOpenEditTransaction) {
        reboot(DEFAULT_REBOOT_SECONDS);
    }
    return true;
}

bool AdminModule::prepareExternalConfigMutation(int saveWhat)
{
    if (externalMutationActive || saveWhat == 0)
        return false;
    if (!nodeDB->beginPreferenceEdit(false)) {
        LOG_WARN("Physical/UI settings mutation could not open its durable fence");
        return false;
    }
    externalMutationActive = true;
    externalMutationSegments = saveWhat;
    externalMutationRequiresReboot = false;
    return true;
}

bool AdminModule::finishExternalConfigMutation()
{
    if (!externalMutationActive)
        return false;
    const int saveWhat = externalMutationSegments;
    const bool requiresReboot = externalMutationRequiresReboot;
    externalMutationActive = false;
    externalMutationSegments = 0;
    externalMutationRequiresReboot = false;

    if (!service->reloadConfig(saveWhat, true)) {
        mutationPersistenceFailed = true;
        rebootAtMsec = millis() + 1000;
        return false;
    }
    if (saveWhat & SEGMENT_CONFIG)
        applyCommittedConfigSideEffects();
    if (requiresReboot)
        reboot(DEFAULT_REBOOT_SECONDS);
    return true;
}

bool AdminModule::cancelExternalConfigMutation()
{
    if (!externalMutationActive)
        return false;
    externalMutationActive = false;
    externalMutationSegments = 0;
    externalMutationRequiresReboot = false;
    if (!nodeDB->cancelPreferenceEdit()) {
        mutationPersistenceFailed = true;
        rebootAtMsec = millis() + 1000;
    }
    return false;
}

void AdminModule::deferOrApplyMessagePurge(NodeNum nodeNum)
{
#if HAS_SCREEN || defined(MESHTASTIC_INCLUDE_NICHE_GRAPHICS)
    if (hasOpenEditTransaction) {
        if (std::find(deferredMessagePurgeNodes.begin(), deferredMessagePurgeNodes.end(), nodeNum) ==
            deferredMessagePurgeNodes.end())
            deferredMessagePurgeNodes.push_back(nodeNum);
    } else {
        messageStore.deleteAllMessagesFromNode(nodeNum);
    }
#else
    (void)nodeNum;
#endif
}

void AdminModule::applyDeferredMessagePurges()
{
#if HAS_SCREEN || defined(MESHTASTIC_INCLUDE_NICHE_GRAPHICS)
    for (NodeNum nodeNum : deferredMessagePurgeNodes) {
        // A later request in the same bulk transaction can un-ignore or remove
        // this node. Apply the irreversible history purge only from the final,
        // durably committed state rather than the first provisional request.
        const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(nodeNum);
        if (node && nodeInfoLiteIsIgnored(node))
            messageStore.deleteAllMessagesFromNode(nodeNum);
    }
#endif
    deferredMessagePurgeNodes.clear();
}

bool AdminModule::handleStoreDeviceUIConfig(const meshtastic_DeviceUIConfig &uicfg)
{
#if HAS_SCREEN
    if (!nodeDB->saveProto(uiconfigFileName, meshtastic_DeviceUIConfig_size, &meshtastic_DeviceUIConfig_msg, &uicfg))
        return false;
    uiconfig = uicfg;
    return true;
#else
    (void)uicfg;
    return false;
#endif
}

// Unset, or set to nothing but whitespace - the two ways a client can leave a
// name field empty.
static bool isBlankName(const char *start)
{
    while (*start && isspace((unsigned char)*start))
        start++;
    return *start == '\0';
}

bool AdminModule::handleSetHamMode(const meshtastic_HamParameters &p)
{
    // Validate ham parameters before setting since this would bypass validation
    // in the owner struct.

    // The call sign is the station ID the whole licensed mode is built around, so
    // it is required; without it we would license a node that never identifies
    // itself on the air.
    if (isBlankName(p.call_sign)) {
        LOG_WARN("Rejected ham call_sign: needs 1+ non-whitespace char");
        return false;
    }

    // Validate every RF value before touching identity, channels, or config.
    // In particular, protobuf floats may carry NaN/Inf and the Heltec V4
    // SX1262 cannot tune the 144-148 MHz amateur regions.
    meshtastic_Config_LoRaConfig candidateLora = config.lora;
    candidateLora.override_duty_cycle = true;
    candidateLora.tx_power = p.tx_power;
    candidateLora.override_frequency = p.frequency;
    char radioError[160] = {};
    if (!RadioInterface::checkConfigRegion(candidateLora, radioError, sizeof(radioError), true) ||
        !RadioInterface::validateConfigLora(candidateLora)) {
        LOG_WARN("Rejected ham radio settings: %s", radioError[0] ? radioError : "invalid modem/frequency values");
        return false;
    }

#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)
    const meshtastic_User oldOwner = owner;
    const meshtastic_Config_DeviceConfig oldDeviceConfig = config.device;
    const meshtastic_Config_LoRaConfig oldLoraConfig = config.lora;
    const meshtastic_Config_SecurityConfig oldSecurityConfig = config.security;
    const bool oldKeyIsLowEntropy = nodeDB->keyIsLowEntropy;
    const bool oldLicensedIdentityMigrationPending = nodeDB->licensedIdentityMigrationPending;
#endif

    // Set call sign and override lora limitations for licensed use. An optional
    // long_name rides behind the call sign with the "//" separator hams already
    // use on the air. e.g. call_sign "N0CALL" plus long_name "Attic Heltec"
    // becomes "N0CALL//Attic Heltec".
    if (!isBlankName(p.long_name))
        snprintf(owner.long_name, sizeof(owner.long_name), "%s//%s", p.call_sign, p.long_name);
    else
        strncpy(owner.long_name, p.call_sign, sizeof(owner.long_name));
    owner.long_name[sizeof(owner.long_name) - 1] = '\0';
    clampLongName(owner.long_name);
    // short_name is optional per the schema, so a blank one keeps the name the
    // node already had
    if (!isBlankName(p.short_name)) {
        strncpy(owner.short_name, p.short_name, sizeof(owner.short_name));
        owner.short_name[sizeof(owner.short_name) - 1] = '\0';
        sanitizeUtf8(owner.short_name, sizeof(owner.short_name));
    }
    owner.is_licensed = true;
    config.lora.override_duty_cycle = true;
    config.lora.tx_power = p.tx_power;
    config.lora.override_frequency = p.frequency;
    // Set node info broadcast interval to 10 minutes
    // For FCC minimum call-sign announcement
    config.device.node_info_broadcast_secs = 600;

    config.device.rebroadcast_mode = meshtastic_Config_DeviceConfig_RebroadcastMode_LOCAL_ONLY;
    // Remove PSK of primary channel for plaintext amateur usage

#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)
    bool identityWillMigrate = false;
    if (config.lora.region != meshtastic_Config_LoRaConfig_RegionCode_UNSET) {
        identityWillMigrate = licensedIdentityWillMigrate();
        if (!nodeDB->generateCryptoKeyPair()) {
            restoreLivePkiIdentity(oldSecurityConfig);
            owner = oldOwner;
            config.device = oldDeviceConfig;
            config.lora = oldLoraConfig;
            config.security = oldSecurityConfig;
            nodeDB->keyIsLowEntropy = oldKeyIsLowEntropy;
            nodeDB->licensedIdentityMigrationPending = oldLicensedIdentityMigrationPending;
            LOG_ERROR("Ham mode rejected because PKI identity activation failed");
            return false;
        }
    }
#endif

    const bool channelsSanitized = channels.ensureLicensedOperation();
    channels.onConfigChanged(false);

    if (strcmp(p.call_sign, "N0CALL") == 0) {
        config.lora.tx_enabled = false;
    }

    service->reloadOwner(false, false);
    if (!saveChanges(SEGMENT_CONFIG | SEGMENT_NODEDATABASE | SEGMENT_DEVICESTATE | SEGMENT_CHANNELS))
        return false;
#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)
    if (identityWillMigrate) {
        warnLicensedIdentityMigration();
    }
#endif
    if (channelsSanitized)
        warnLicensedMode();
    return true;
}

AdminModule::AdminModule() : ProtobufModule("Admin", meshtastic_PortNum_ADMIN_APP, &meshtastic_AdminMessage_msg)
{
    // restrict to the admin channel for rx
    // boundChannel = Channels::adminChannel;
}

void AdminModule::setPassKey(meshtastic_AdminMessage *res)
{
    // Regenerate once there is no session yet or the current key is older than
    // 150s. session_time holds millis(); the Throttle check is rollover-safe,
    // unlike the previous seconds comparison.
    if (!sessionPasskeyValid || !Throttle::isWithinTimespanMs(session_time, 150 * 1000UL)) {
        // Session passkey authenticates admin replies, so it must be unpredictable:
        // prefer the hardware RNG, falling back to the seeded CSPRNG only when no
        // hardware source exists. Hold cryptLock like the signing path does: this
        // runs on the admin receive path, which on nRF52 is the BLE task, and the
        // fill toggles the CC310 that packet crypto also uses while the CryptRNG
        // state is shared with signing.
        {
            concurrency::LockGuard g(cryptLock);
            if (!HardwareRNG::fill(session_passkey, sizeof(session_passkey)))
                CryptRNG.rand(session_passkey, sizeof(session_passkey));
        }
        session_time = millis();
        sessionPasskeyValid = true;
    }
    memcpy(res->session_passkey.bytes, session_passkey, 8);
    res->session_passkey.size = 8;
    LOG_DEBUG("Issued a new temporary admin session key");
    // if halfway to session_expire, regenerate session_passkey, reset the timeout
    // set the key in the packet
}

bool AdminModule::checkPassKey(meshtastic_AdminMessage *res)
{ // check that the key in the packet is still valid
    // Key is valid for 300s from issue; sessionPasskeyValid guards an unissued
    // session.
    return (sessionPasskeyValid && Throttle::isWithinTimespanMs(session_time, 300 * 1000UL) && res->session_passkey.size == 8 &&
            memcmp(res->session_passkey.bytes, session_passkey, 8) == 0);
}

bool AdminModule::messageIsResponse(const meshtastic_AdminMessage *r)
{
    if (r->which_payload_variant == meshtastic_AdminMessage_get_channel_response_tag ||
        r->which_payload_variant == meshtastic_AdminMessage_get_owner_response_tag ||
        r->which_payload_variant == meshtastic_AdminMessage_get_config_response_tag ||
        r->which_payload_variant == meshtastic_AdminMessage_get_module_config_response_tag ||
        r->which_payload_variant == meshtastic_AdminMessage_get_canned_message_module_messages_response_tag ||
        r->which_payload_variant == meshtastic_AdminMessage_get_device_metadata_response_tag ||
        r->which_payload_variant == meshtastic_AdminMessage_get_ringtone_response_tag ||
        r->which_payload_variant == meshtastic_AdminMessage_get_device_connection_status_response_tag ||
        r->which_payload_variant == meshtastic_AdminMessage_get_node_remote_hardware_pins_response_tag ||
        r->which_payload_variant == meshtastic_AdminMessage_get_ui_config_response_tag)
        return true;
    else
        return false;
}

// The response variant that answers a getter request, or 0 if the request has
// none.
static pb_size_t adminResponseForRequest(pb_size_t requestVariant)
{
    switch (requestVariant) {
    case meshtastic_AdminMessage_get_channel_request_tag:
        return meshtastic_AdminMessage_get_channel_response_tag;
    case meshtastic_AdminMessage_get_owner_request_tag:
        return meshtastic_AdminMessage_get_owner_response_tag;
    case meshtastic_AdminMessage_get_config_request_tag:
        return meshtastic_AdminMessage_get_config_response_tag;
    case meshtastic_AdminMessage_get_module_config_request_tag:
        return meshtastic_AdminMessage_get_module_config_response_tag;
    case meshtastic_AdminMessage_get_canned_message_module_messages_request_tag:
        return meshtastic_AdminMessage_get_canned_message_module_messages_response_tag;
    case meshtastic_AdminMessage_get_device_metadata_request_tag:
        return meshtastic_AdminMessage_get_device_metadata_response_tag;
    case meshtastic_AdminMessage_get_ringtone_request_tag:
        return meshtastic_AdminMessage_get_ringtone_response_tag;
    case meshtastic_AdminMessage_get_device_connection_status_request_tag:
        return meshtastic_AdminMessage_get_device_connection_status_response_tag;
    case meshtastic_AdminMessage_get_node_remote_hardware_pins_request_tag:
        return meshtastic_AdminMessage_get_node_remote_hardware_pins_response_tag;
    case meshtastic_AdminMessage_get_ui_config_request_tag:
        return meshtastic_AdminMessage_get_ui_config_response_tag;
    default:
        return 0;
    }
}

void AdminModule::noteOutgoingAdminRequest(const meshtastic_MeshPacket &p)
{
    if (p.which_payload_variant != meshtastic_MeshPacket_decoded_tag || p.decoded.portnum != meshtastic_PortNum_ADMIN_APP)
        return;
    // Local admin is answered in-process, and a broadcast is never a request to
    // one remote.
    if (p.to == 0 || isBroadcast(p.to) || p.to == nodeDB->getNodeNum())
        return;

    meshtastic_AdminMessage admin = meshtastic_AdminMessage_init_zero;
    if (!pb_decode_from_bytes(p.decoded.payload.bytes, p.decoded.payload.size, &meshtastic_AdminMessage_msg, &admin))
        return;
    const pb_size_t responseVariant = adminResponseForRequest(admin.which_payload_variant);
    if (!responseVariant)
        return; // not a getter whose response we can pair

    // Pin the key perhapsEncode will actually encrypt to, resolved the same way
    // it resolves it. p.public_key is NOT it: nothing populates that field on the
    // outgoing path (only perhapsDecode sets it, on RX), so reading it here
    // pinned nothing and left `from` as the sole check.
    bool keyValid = false;
    meshtastic_NodeInfoLite_public_key_t destKey = {0, {0}};
#if !(MESHTASTIC_EXCLUDE_PKI)
    const bool haveDestKey = nodeDB->copyPublicKey(p.to, destKey);
    keyValid = haveDestKey && wouldEncryptWithPKC(&p, p.channel, haveDestKey);
#endif

    // One entry per request (a client sends N indexed get_channel requests, each
    // answered once, so entries must not merge). Free slot, else evict the oldest
    // by rollover-safe elapsed time.
    OutstandingAdminRequest *slot = nullptr;
    for (auto &o : outstandingAdminRequests)
        if (o.to == 0) {
            slot = &o;
            break;
        }
    if (!slot) {
        const uint32_t now = millis();
        slot = &outstandingAdminRequests[0];
        for (auto &o : outstandingAdminRequests)
            if ((uint32_t)(now - o.sentAtMs) > (uint32_t)(now - slot->sentAtMs))
                slot = &o;
    }

    slot->to = p.to;
    slot->requestId = p.id;
    slot->sentAtMs = millis();
    slot->expectedResponse = responseVariant;
    slot->moduleConfigType = admin.which_payload_variant == meshtastic_AdminMessage_get_module_config_request_tag
                                 ? (uint8_t)admin.get_module_config_request
                                 : 0;
    slot->keyValid = keyValid;
    if (keyValid)
        memcpy(slot->key, destKey.bytes, 32);
    else
        memset(slot->key, 0, 32);
    LOG_DEBUG("Admin request sent to 0x%08x, expect response", p.to);
}

bool AdminModule::responseIsSolicited(const meshtastic_MeshPacket &mp, pb_size_t responseVariant, pb_size_t moduleConfigTag)
{
    // Scan every entry: several requests for the same variant may differ only in
    // pinning, and an unpinned one still authorizes the response even if a pinned
    // one does not.
    for (auto &o : outstandingAdminRequests) {
        if (o.to != mp.from || o.expectedResponse != responseVariant)
            continue;
        // mp.from is unauthenticated, so also require the response to echo our
        // request's packet id (setReplyTo puts it in decoded.request_id). A blind
        // injector must now guess it. Id 0 is no token at all - an omitted
        // request_id decodes to 0 - so such a slot never matches.
        if (o.requestId == 0 || mp.decoded.request_id != o.requestId)
            continue;
        if (!Throttle::isWithinTimespanMs(o.sentAtMs, kOutstandingAdminRequestMs)) {
            o.to = 0; // lapsed; free the slot and keep looking for another live match
            continue;
        }
        // A request pinned to a PKC key must be answered over PKC by that same key.
        if (o.keyValid && (!mp.pki_encrypted || mp.public_key.size != 32 || memcmp(mp.public_key.bytes, o.key, 32) != 0))
            continue;
        // remote_hardware is the only module config that mutates state (the pin
        // table), so require it to answer a request for that exact subtype, not
        // just any get_module_config_request.
        if (moduleConfigTag == meshtastic_ModuleConfig_remote_hardware_tag &&
            o.moduleConfigType != meshtastic_AdminMessage_ModuleConfigType_REMOTEHARDWARE_CONFIG)
            continue;
        o.to = 0; // consume: one request authorizes one response, no replay within
                  // the window
        return true;
    }
    return false;
}

bool AdminModule::messageIsRequest(const meshtastic_AdminMessage *r)
{
    if (r->which_payload_variant == meshtastic_AdminMessage_get_channel_request_tag ||
        r->which_payload_variant == meshtastic_AdminMessage_get_owner_request_tag ||
        r->which_payload_variant == meshtastic_AdminMessage_get_config_request_tag ||
        r->which_payload_variant == meshtastic_AdminMessage_get_module_config_request_tag ||
        r->which_payload_variant == meshtastic_AdminMessage_get_canned_message_module_messages_request_tag ||
        r->which_payload_variant == meshtastic_AdminMessage_get_device_metadata_request_tag ||
        r->which_payload_variant == meshtastic_AdminMessage_get_ringtone_request_tag ||
        r->which_payload_variant == meshtastic_AdminMessage_get_device_connection_status_request_tag ||
        r->which_payload_variant == meshtastic_AdminMessage_get_node_remote_hardware_pins_request_tag ||
        r->which_payload_variant == meshtastic_AdminMessage_get_ui_config_request_tag)
        return true;
    else
        return false;
}

bool AdminModule::handleSendInputEvent(const meshtastic_AdminMessage_InputEvent &inputEvent)
{
    LOG_TRACE("Processing input event: event_code=%u, kb_char=%u, touch_x=%u, "
              "touch_y=%u",
              inputEvent.event_code, inputEvent.kb_char, inputEvent.touch_x, inputEvent.touch_y);

    // Create InputEvent for injection.
    //
    // `.source` MUST be a non-null C string: the LOG_INFO below formats it
    // with %s, and passing NULL to the esp-log formatter crashes with
    // Guru Meditation LoadProhibited at strlen(NULL). Other InputBroker
    // sources (buttons, rotary) always set this; the admin path was the
    // only one leaving it default-null.
    InputEvent event = {.source = "admin",
                        .inputEvent = (input_broker_event)inputEvent.event_code,
                        .kbchar = (unsigned char)inputEvent.kb_char,
                        .touchX = inputEvent.touch_x,
                        .touchY = inputEvent.touch_y};

    // Log the event being injected
    LOG_INFO("Injecting input event from admin: source=%s, event=%u, "
             "char=%c(%u), touch=(%u,%u)",
             event.source, event.inputEvent, (event.kbchar >= 32 && event.kbchar <= 126) ? event.kbchar : '?', event.kbchar,
             event.touchX, event.touchY);

    // A persistently disabled Heltec display accepts only the physical PRG
    // long-press restore gesture. InputBroker will drop this remote injection
    // too; avoid waking the CPU first.
#if defined(HELTEC_V4_OLED)
    if (inputEvent.event_code == INPUT_BROKER_FACTORY_RST) {
        LOG_WARN("Reject remote factory-reset input injection; use the "
                 "authenticated reset command");
        return false;
    }
    if (screen && screen->isDisplayDisabled())
        return false;
#endif
    powerFSM.trigger(EVENT_INPUT);
#if !defined(MESHTASTIC_EXCLUDE_INPUTBROKER)
    // Queue on RTOS targets so observers run after this Admin handler releases
    // its edit-transaction lock. Synchronous injection can otherwise re-enter
    // SystemCommands and deadlock on the same non-recursive lock.
    if (inputBroker) {
#if defined(HAS_FREE_RTOS) && !defined(ARCH_RP2040)
        if (!inputBroker->tryQueueInputEvent(&event)) {
            LOG_WARN("Dropping admin input event because the input queue is full");
            return false;
        }
#else
        inputBroker->injectInputEvent(&event);
#endif
    } else {
        LOG_ERROR("No InputBroker for event injection");
        return false;
    }
#endif
    return true;
}

void AdminModule::sendWarning(const char *format, ...)
{
    meshtastic_ClientNotification *cn = clientNotificationPool.allocZeroed();
    if (!cn)
        return;

    cn->level = meshtastic_LogRecord_Level_WARNING;
    cn->time = getValidTime(RTCQualityFromNet);

    va_list args;
    va_start(args, format);
    // Format the arguments directly into the notification object
    vsnprintf(cn->message, sizeof(cn->message), format, args);
    va_end(args);

    service->sendClientNotification(cn);
}

void AdminModule::sendWarningAndLog(const char *format, ...)
{
    // We need a temporary buffer to hold the formatted text so we can log it
    // Using 250 bytes as a safe upper limit for typical text notifications
    char buf[250];

    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);

    // SECURITY NOTE: Both LOG_WARN and sendWarning are printf-style, so we pass
    // "%s", buf rather than 'buf' directly. If 'buf' contained a % symbol (e.g. a
    // user-supplied channel name like "50%"), passing it as the format string
    // would read bogus varargs and could crash. "%s" treats it purely as text.
    LOG_WARN("%s", buf);
    sendWarning("%s", buf);
}

// Strip spaces and fold to lowercase for loose preset-name comparison.
static void normalizePresetName(const char *src, char *dst, size_t dstLen)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < dstLen; i++) {
        if (src[i] != ' ')
            dst[j++] = (char)tolower((unsigned char)src[i]);
    }
    dst[j] = '\0';
}

// Record one channel-configuration warning. The first message is kept verbatim
// in case it turns out to be the only one; nameIssue/pskIssue and the channel
// bitmask feed the catch-all wording if more than one channel ends up flagged.
// flushChannelWarnings() emits the result.
void AdminModule::queueChannelWarning(uint8_t channelIndex, bool nameIssue, bool pskIssue, const char *format, ...)
{
    if (pendingWarningCount == 0) {
        va_list args;
        va_start(args, format);
        vsnprintf(pendingWarningText, sizeof(pendingWarningText), format, args);
        va_end(args);
    }
    if (channelIndex < MAX_NUM_CHANNELS)
        pendingWarningChannels |= (1u << channelIndex);
    pendingWarningNameIssue |= nameIssue;
    pendingWarningPskIssue |= pskIssue;
    pendingWarningCount++;
}

// Queue the fixed "licensed mode activated" notice, deferring it to commit
// during an edit transaction so repeated triggers collapse to a single message.
void AdminModule::warnLicensedMode()
{
    if (hasOpenEditTransaction)
        pendingLicenseWarning = true;
    else
        sendWarning(licensedModeMessage);
}

// Keep the identity-migration notice and its durable pending bit aligned with
// the preference generation that installs the key. A deferred edit is not a
// commit: emitting here would tell the operator an identity migration succeeded
// even if COMMIT later failed or never arrived.
void AdminModule::warnLicensedIdentityMigration()
{
    if (hasOpenEditTransaction) {
        pendingIdentityMigrationWarning = true;
        return;
    }
    sendWarning(licensedIdentityMigrationMessage);
    nodeDB->licensedIdentityMigrationPending = false;
}

// Emit the coalesced channel warning(s): nothing if none queued, the lone
// message verbatim if exactly one, otherwise a single catch-all naming every
// flagged channel. The licensed-mode notice, if queued, is emitted once
// alongside. Always resets state.
void AdminModule::flushChannelWarnings()
{
    if (pendingLicenseWarning)
        sendWarning(licensedModeMessage);
    if (pendingIdentityMigrationWarning) {
        sendWarning(licensedIdentityMigrationMessage);
        nodeDB->licensedIdentityMigrationPending = false;
    }

    if (pendingWarningCount == 1) {
        sendWarningAndLog("%s", pendingWarningText);
    } else if (pendingWarningCount > 1) {
        char list[48] = {};
        for (uint8_t i = 0; i < MAX_NUM_CHANNELS; i++) {
            if (!(pendingWarningChannels & (1u << i)))
                continue;
            char num[8];
            snprintf(num, sizeof(num), "%s%u", *list ? ", " : "", i);
            strncat(list, num, sizeof(list) - strlen(list) - 1);
        }
        if (pendingWarningNameIssue && pendingWarningPskIssue)
            sendWarningAndLog("There may be name and PSK issues on channels %s",
                              list); // max 60 bytes
        else if (pendingWarningNameIssue)
            sendWarningAndLog("There may be name issues on channels %s",
                              list); // max 52 bytes
        else
            sendWarningAndLog("There may be PSK issues on channels %s",
                              list); // max 51 bytes
    }
    pendingWarningText[0] = '\0';
    pendingWarningChannels = 0;
    pendingWarningCount = 0;
    pendingWarningNameIssue = false;
    pendingWarningPskIssue = false;
    pendingLicenseWarning = false;
    pendingIdentityMigrationWarning = false;
}

/**
 * @brief Emit client warnings for common misconfigurations on a newly committed
 * channel.
 *
 * Called from handleSetChannel() after the channel has been saved. The
 * following checks are performed:
 *
 * - Blank PSK (size == 0) on a non-licensed device: the channel has no
 * encryption.
 * - Blank name with a non-default key (not the default key, 0x01): the name
 * will auto-resolve to the current modem preset name, but the key mismatch
 * means other preset nodes cannot decode traffic on this channel.
 * - Named channel whose name is a case/space variant of the current modem
 * preset: the explicit name prevents auto-resolution; client should clear it.
 * - Same variant match but PSK is not the default key: looks like the preset
 * channel but is incompatible with nodes using the preset's default key.
 *
 * @param cc  The channel that was written.
 */
void AdminModule::warnOnChannelSet(const meshtastic_Channel &cc)
{
    if (cc.role == meshtastic_Channel_Role_DISABLED || !cc.has_settings) // don't check unused channels
        return;

    bool blankPsk = (cc.settings.psk.size == 0 && !owner.is_licensed);

    if (!config.lora.use_preset) { // custom or unset preset can mistype things too
        const char *mistypePreset = nullptr;
        if (*cc.settings.name) {
            char normChan[32];
            normalizePresetName(cc.settings.name, normChan, sizeof(normChan));
            for (auto preset = _meshtastic_Config_LoRaConfig_ModemPreset_MIN;
                 preset <= _meshtastic_Config_LoRaConfig_ModemPreset_MAX;
                 preset = (meshtastic_Config_LoRaConfig_ModemPreset)(preset + 1)) {
                const char *name = DisplayFormatters::getModemPresetDisplayName(preset, false, true);
                if (strcmp(name, "Invalid") == 0)
                    continue; // skip preset slots without a real display name
                if (strcmp(cc.settings.name, name) == 0)
                    break; // exact match - not a mistype, no warning
                char normPreset[32];
                normalizePresetName(name, normPreset, sizeof(normPreset));
                if (strcmp(normChan, normPreset) == 0) {
                    mistypePreset = name;
                    break;
                }
            }
        }
        // At most one warning: collapse blank-PSK + mistype into a single
        // catch-all.
        if (blankPsk && mistypePreset)
            queueChannelWarning(cc.index, true, true, "There may be name and PSK issues on channel %d", cc.index);
        else if (mistypePreset)
            queueChannelWarning(cc.index, true, false,
                                "Channel %d name '%s' looks like a mistype of '%s' - "
                                "make sure to type it exactly!", // max 90 bytes
                                cc.index, cc.settings.name, mistypePreset);
        else if (blankPsk)
            queueChannelWarning(cc.index, false, true,
                                "Channel %d '%s' has a blank PSK (no encryption) "
                                "instead of default key", // max 100 bytes
                                cc.index, cc.settings.name);
        return;
    }

    const char *presetName = DisplayFormatters::getModemPresetDisplayName(config.lora.modem_preset, false, true);
    bool isDefaultKey = (cc.settings.psk.size == 1 && cc.settings.psk.bytes[0] == 0x01);
    char normPreset[32],
        normChan[32]; // max size is 11 plus nul, but allow for future expansion
    normalizePresetName(presetName, normPreset, sizeof(normPreset));
    normalizePresetName(cc.settings.name, normChan, sizeof(normChan));

    if (!*cc.settings.name) {
        // Blank name resolves to the preset name - A and B are mutually exclusive
        // (psk.size can't be both 0 and >0)
        if (blankPsk)
            queueChannelWarning(cc.index, false, true,
                                "Channel %d '%s' has a blank PSK (no encryption) "
                                "instead of default key", // max 100 bytes
                                cc.index, cc.settings.name);
        else if (!isDefaultKey && cc.settings.psk.size > 0)
            queueChannelWarning(cc.index, false, true,
                                "Channel %d will resolve to preset '%s' but uses a non-default key - "
                                "default-key nodes can't decode it.", // max 102 bytes
                                cc.index, presetName);
        return;
    }

    if (strcmp(normChan, normPreset) != 0) { // name unrelated to preset
        if (blankPsk)
            queueChannelWarning(cc.index, false, true,
                                "Channel %d '%s' has a blank PSK (no encryption) "
                                "instead of default key", // max 100 bytes
                                cc.index, cc.settings.name);
        return;
    }

    bool variantName = (strcmp(cc.settings.name, presetName) != 0);
    bool keyMismatch = (!isDefaultKey && cc.settings.psk.size > 0);
    int issues = (int)blankPsk + (int)variantName + (int)keyMismatch;

    if (issues > 1) {
        bool hasNameIssue = variantName;
        bool hasPskIssue = blankPsk || keyMismatch;
        if (hasNameIssue && hasPskIssue)
            queueChannelWarning(cc.index, true, true, "There may be name and PSK issues on channel %d", cc.index);
        else if (hasNameIssue)
            queueChannelWarning(cc.index, true, false, "There may be name issues on channel %d", cc.index);
        else
            queueChannelWarning(cc.index, false, true, "There may be PSK issues on channel %d", cc.index);
        return;
    }

    if (blankPsk)
        queueChannelWarning(cc.index, false, true,
                            "Channel %d '%s' has a blank PSK (no encryption) "
                            "instead of default key", // max 100 bytes
                            cc.index, cc.settings.name);
    if (variantName)
        queueChannelWarning(cc.index, true, false,
                            "Channel %d name '%s' looks like a mistype of '%s' - "
                            "clear the name to use the preset name automatically.", // max 113 bytes
                            cc.index, cc.settings.name, presetName);
    if (keyMismatch)
        queueChannelWarning(cc.index, false, true,
                            "Channel %d '%s' matches preset '%s' but uses a non-default key - "
                            "default-key nodes can't decode it.", // max 108 bytes
                            cc.index, cc.settings.name, presetName);
} // warnOnChannelSet

/**
 * @brief Scan all channels for preset-name conflicts after a modem preset
 * change is committed.
 *
 * Called from handleSetConfig() after the LoRa config has been saved, and only
 * when modem_preset actually changed (rejected configs are never passed here).
 * For every named, non-disabled channel two checks are performed:
 *
 * - Name matches the *old* preset (case-insensitive, spaces stripped): the
 * channel was likely tracking the previous preset; the user should rename it if
 * it should follow the new one.
 * - Name matches the *new* preset: the channel name collides with the
 * auto-generated preset name but won't resolve automatically because the name
 * is set explicitly.
 *
 * No-ops if the new config does not use a preset.
 *
 * @param oldLora  LoRa config before the update.
 * @param newLora  LoRa config after the update.
 */
void AdminModule::warnOnLoraPresetChange(const meshtastic_Config_LoRaConfig &oldLora, const meshtastic_Config_LoRaConfig &newLora)
{
    if (!newLora.use_preset || newLora.modem_preset == oldLora.modem_preset)
        return;

    char normOld[32] = {}, normNew[32];
    if (oldLora.use_preset) {
        const char *oldName = DisplayFormatters::getModemPresetDisplayName(oldLora.modem_preset, false, true);
        normalizePresetName(oldName, normOld, sizeof(normOld));
    }
    const char *newName = DisplayFormatters::getModemPresetDisplayName(newLora.modem_preset, false, true);
    normalizePresetName(newName, normNew, sizeof(normNew));

    // Queue one (name) warning per affected channel; flushChannelWarnings()
    // collapses them into a single message - either the lone warning verbatim or
    // a catch-all listing indices.
    for (int i = 0; i < channels.getNumChannels(); i++) {
        const meshtastic_Channel &ch = channels.getByIndex(i);
        if (ch.role == meshtastic_Channel_Role_DISABLED || !ch.has_settings || !*ch.settings.name)
            continue;
        char normChan[32];
        normalizePresetName(ch.settings.name, normChan, sizeof(normChan));
        if (*normOld && strcmp(normChan, normOld) == 0)
            queueChannelWarning(i, true, false,
                                "Channel %d name '%s' matches the old preset. "
                                "Rename it manually if it should track the new preset.", // max 98
                                                                                         // bytes
                                i, ch.settings.name);
        else if (strcmp(normChan, normNew) == 0)
            queueChannelWarning(i, true, false,
                                "Channel %d '%s' looks like preset '%s' but won't auto-resolve - "
                                "clear the name to fix it.", // max 98 bytes
                                i, ch.settings.name, newName);
    }
} // warnOnLoraPresetChange

void disableBluetooth()
{
#if HAS_BLUETOOTH
#ifdef ARCH_ESP32
    if (nimbleBluetooth && nimbleBluetooth->isActive())
        nimbleBluetooth->deinit();
    esp32ReleaseBluetoothMemoryIfUnused();
#elif defined(ARCH_NRF52)
    if (nrf52Bluetooth)
        nrf52Bluetooth->shutdown();
#elif defined(ARCH_NRF54L15)
    if (nrf54l15Bluetooth)
        nrf54l15Bluetooth->shutdown();
#endif
#endif
}
