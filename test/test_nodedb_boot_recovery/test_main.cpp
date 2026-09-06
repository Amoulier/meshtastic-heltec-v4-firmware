// NodeDB boot-recovery contract: an undecodable config.proto must freeze
// identity (no keygen, no overwrite), an absent one takes the fresh-install
// path, and a corrupt nodes.proto does neither. The tests are a ladder
// (state=per-suite): arrange /prefs, then "reboot" a fresh NodeDB.
#include "MeshTypes.h" // Include BEFORE TestUtil.h
#include "TestUtil.h"
#include <unity.h>

#if defined(ARCH_PORTDUINO)
#define NBR_TEST_ENTRY extern "C"
#else
#define NBR_TEST_ENTRY
#endif

#include "FSCommon.h" // defines FSCom; must precede the feature guard below
#if defined(ARCH_PORTDUINO) && !defined(HELTEC_V4_NATIVE_STORAGE_AUDIT)
#error "Run this suite with the coverage-storage audit environment"
#endif

// The identity-freeze contract only exists where there is a filesystem and boot
// keygen.
#if defined(FSCom) && !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)

#include "mesh/NodeDB.h"
#include "mesh/TypeConversions.h"
#include <ErriezCRC32.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Friend seam declared in NodeDB.h (PIO_UNIT_TESTING): read the private
// degraded-boot flag. Never instantiated - constructing one would run the real
// boot sequence.
class NodeDBTestShim : public NodeDB
{
  public:
    static bool decodeFailed(const NodeDB *db) { return db->configDecodeFailed; }
    static int unreadableSegments(const NodeDB *db) { return db->unreadablePreferenceSegments; }
};

namespace
{

// --- Identity baseline captured after a healthy keyed boot ---
uint32_t baseNodeNum = 0;
uint8_t basePublicKey[32];
uint8_t basePrivateKey[32];
char baseLongName[sizeof(meshtastic_User::long_name)];
std::vector<uint8_t> goodConfigBytes; // byte-exact healthy config.proto for restore tests
std::vector<uint8_t> goodChannelBytes;
std::vector<uint8_t> goodDeviceStateBytes;
std::vector<uint8_t> goodNodeDatabaseBytes;

// --- File helpers (through FSCom so the tests stay agnostic about the
// mountpoint) ---

bool readFileBytes(const char *path, std::vector<uint8_t> &out)
{
    out.clear();
    File f = FSCom.open(path, FILE_O_READ);
    if (!f)
        return false;
    uint8_t buf[512];
    size_t n;
    while ((n = f.read(buf, sizeof(buf))) > 0)
        out.insert(out.end(), buf, buf + n);
    f.close();
    return true;
}

void writeFileBytes(const char *path, const uint8_t *data, size_t len)
{
    FSCom.remove(path); // FILE_O_WRITE is append on some backends; start clean
    File f = FSCom.open(path, FILE_O_WRITE);
    TEST_ASSERT_TRUE_MESSAGE(f, path);
    TEST_ASSERT_EQUAL_size_t(len, f.write(data, len));
    f.close();
}

// FNV-1a content fingerprint; answers only "did this file change?". 0 ==
// missing file.
uint64_t fileFingerprint(const char *path)
{
    std::vector<uint8_t> bytes;
    if (!readFileBytes(path, bytes))
        return 0;
    uint64_t h = 1469598103934665603ULL;
    for (uint8_t b : bytes) {
        h ^= b;
        h *= 1099511628211ULL;
    }
    return h;
}

// A varint tag of five 0xFF bytes overflows 32 bits, so nanopb fails
// deterministically.
const uint8_t kGarbage[32] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                              0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// --- Reboot helper ---

// Deliberately retain the process-global protobuf vectors. Runtime encrypted
// unlock uses this same in-process reload shape, so NodeDB itself must clear
// repeated fields before nanopb's callbacks append the disk generation.
void rebootNodeDB()
{
    NodeDB *rebooted = new NodeDB();
    delete nodeDB;
    nodeDB = rebooted;
}

void captureIdentityBaseline()
{
    TEST_ASSERT_EQUAL(32, config.security.public_key.size);
    TEST_ASSERT_EQUAL(32, config.security.private_key.size);
    TEST_ASSERT_EQUAL(32, owner.public_key.size);
    baseNodeNum = myNodeInfo.my_node_num;
    memcpy(basePublicKey, config.security.public_key.bytes, 32);
    memcpy(basePrivateKey, config.security.private_key.bytes, 32);
    strncpy(baseLongName, owner.long_name, sizeof(baseLongName));
    baseLongName[sizeof(baseLongName) - 1] = '\0';
    TEST_ASSERT_TRUE(readFileBytes(configFileName, goodConfigBytes));
    TEST_ASSERT_TRUE(readFileBytes(channelFileName, goodChannelBytes));
    TEST_ASSERT_TRUE(readFileBytes(deviceStateFileName, goodDeviceStateBytes));
    TEST_ASSERT_TRUE(readFileBytes(nodeDatabaseFileName, goodNodeDatabaseBytes));
    TEST_ASSERT_GREATER_THAN(1, goodConfigBytes.size());
}

// Persist a set region so boot keygen is unconditionally armed
// (generateCryptoKeyPair skips while region == UNSET unless the portduino
// sim-radio bypass applies), then reboot into the healthy keyed state every
// later test measures against.
void establishHealthyBaseline()
{
    config.lora.region = meshtastic_Config_LoRaConfig_RegionCode_US;
    TEST_ASSERT_TRUE(nodeDB->saveToDisk(SEGMENT_CONFIG));
    rebootNodeDB();
    // Reboot once more so any boot-time coercion of the freshly saved config
    // (preset clamp) has reached its fixpoint on disk before we fingerprint it as
    // the "good" file.
    rebootNodeDB();
    TEST_ASSERT_EQUAL(meshtastic_Config_LoRaConfig_RegionCode_US, config.lora.region);
    captureIdentityBaseline();
}

void assertIdentityMatchesBaseline()
{
    TEST_ASSERT_EQUAL_UINT32(baseNodeNum, myNodeInfo.my_node_num);
    TEST_ASSERT_EQUAL(32, config.security.public_key.size);
    TEST_ASSERT_EQUAL_MEMORY(basePublicKey, config.security.public_key.bytes, 32);
    TEST_ASSERT_EQUAL(32, config.security.private_key.size);
    TEST_ASSERT_EQUAL_MEMORY(basePrivateKey, config.security.private_key.bytes, 32);
    TEST_ASSERT_EQUAL(32, owner.public_key.size);
    TEST_ASSERT_EQUAL_MEMORY(basePublicKey, owner.public_key.bytes, 32);
}

} // namespace

void setUp(void) {}
void tearDown(void) {}

// --- Healthy-boot identity ---

// A real Heltec does not mint a PKI identity until the operator selects a
// region. Core autosaves can therefore create config/module/device/channels
// while saveNodeDatabaseToDisk() intentionally leaves nodes.proto absent.
// That is a complete keyless generation, not evidence of a torn write.
static void test_keylessUnsetGeneration_withoutNodeDatabaseIsHealthy(void)
{
    config.lora.region = meshtastic_Config_LoRaConfig_RegionCode_UNSET;
    config.security.private_key.size = 0;
    config.security.public_key.size = 0;
    memset(config.security.private_key.bytes, 0, sizeof(config.security.private_key.bytes));
    memset(config.security.public_key.bytes, 0, sizeof(config.security.public_key.bytes));
    owner.public_key.size = 0;
    memset(owner.public_key.bytes, 0, sizeof(owner.public_key.bytes));
    owner.is_licensed = false;

    TEST_ASSERT_TRUE(nodeDB->saveToDisk(SEGMENT_CONFIG | SEGMENT_MODULECONFIG | SEGMENT_DEVICESTATE | SEGMENT_CHANNELS));
    if (FSCom.exists(nodeDatabaseFileName))
        TEST_ASSERT_TRUE(FSCom.remove(nodeDatabaseFileName));
    TEST_ASSERT_FALSE(FSCom.exists(nodeDatabaseFileName));

    rebootNodeDB();

    TEST_ASSERT_FALSE(NodeDBTestShim::decodeFailed(nodeDB));
    TEST_ASSERT_FALSE(NodeDBTestShim::unreadableSegments(nodeDB) & SEGMENT_NODEDATABASE);
}

// The #11001 renumber family: a keyed boot must mint NodeNum ==
// crc32(public_key) once, and every subsequent reboot must reproduce the same
// NodeNum, keypair and owner identity.
static void test_firstBoot_establishesKeyedIdentity(void)
{
    TEST_MESSAGE("=== First keyed boot mints crc32(pubkey) identity ===");
    establishHealthyBaseline();

    TEST_ASSERT_EQUAL_UINT32(crc32Buffer(config.security.public_key.bytes, 32), myNodeInfo.my_node_num);
    // The minted identity is in the store of record: self entry present, carrying
    // our key.
    const meshtastic_NodeInfoLite *self = nodeDB->getMeshNode(nodeDB->getNodeNum());
    TEST_ASSERT_NOT_NULL(self);
    TEST_ASSERT_TRUE(nodeInfoLiteHasUser(self));
    TEST_ASSERT_EQUAL(32, self->public_key.size);
    TEST_ASSERT_EQUAL_MEMORY(basePublicKey, self->public_key.bytes, 32);
    TEST_ASSERT_FALSE(NodeDBTestShim::decodeFailed(nodeDB));
}

static void test_healthyReboot_preservesIdentity(void)
{
    TEST_MESSAGE("=== Plain reboot: identity byte-identical, config.proto not "
                 "rewritten ===");
    const uint64_t fpBefore = fileFingerprint(configFileName);
    TEST_ASSERT_NOT_EQUAL(0, fpBefore);

    rebootNodeDB();

    assertIdentityMatchesBaseline();
    TEST_ASSERT_EQUAL_STRING(baseLongName, owner.long_name);
    // A healthy boot has nothing to persist for config: the on-disk file is
    // already the fixpoint.
    TEST_ASSERT_EQUAL_UINT64(fpBefore, fileFingerprint(configFileName));
}

static void test_repeatedReload_replacesRatherThanAppendsNodeDatabase(void)
{
    constexpr NodeNum diskOnlyNode = 0xA55AA55A;
    auto *diskNode = nodeDB->getOrCreateMeshNode(diskOnlyNode);
    TEST_ASSERT_NOT_NULL(diskNode);
    // Empty discoveries are intentionally purged on boot; use a real user
    // record so this tests replacement of repeated fields, not cleanup policy.
    nodeInfoLiteSetBit(diskNode, NODEINFO_BITFIELD_HAS_USER_MASK, true);
    TEST_ASSERT_TRUE(nodeDB->saveToDisk(SEGMENT_NODEDATABASE));

    // Make RAM intentionally disagree with disk. If loadFromDisk appends to
    // the retained MAX-sized vector, the freshly decoded row lands past the
    // hot cap and self-care truncates it away.
    TEST_ASSERT_TRUE(nodeDB->removeNodeByNum(diskOnlyNode, false));
    TEST_ASSERT_NULL(nodeDB->getMeshNode(diskOnlyNode));

    rebootNodeDB();
    TEST_ASSERT_NOT_NULL_MESSAGE(nodeDB->getMeshNode(diskOnlyNode), "reload must replace retained repeated fields");
    rebootNodeDB();
    TEST_ASSERT_NOT_NULL_MESSAGE(nodeDB->getMeshNode(diskOnlyNode), "a second reload must not append or truncate disk rows");

    TEST_ASSERT_TRUE(nodeDB->removeNodeByNum(diskOnlyNode, true));
}

// --- Degraded boot: present-but-undecodable config ---

static void test_corruptConfig_freezesIdentity_leavesFileUntouched(void)
{
    TEST_MESSAGE("=== Corrupt config.proto: frozen identity, radio silent, file "
                 "untouched ===");
    writeFileBytes(configFileName, kGarbage, sizeof(kGarbage));
    const uint64_t fpGarbage = fileFingerprint(configFileName);
    TEST_ASSERT_NOT_EQUAL(0, fpGarbage);

    rebootNodeDB();

    TEST_ASSERT_TRUE(NodeDBTestShim::decodeFailed(nodeDB));
    // Radio silent until the operator restores a config.
    TEST_ASSERT_EQUAL(meshtastic_Config_LoRaConfig_RegionCode_UNSET, config.lora.region);
    TEST_ASSERT_FALSE(config.lora.tx_enabled);
    // Keygen skipped: no replacement keypair minted into RAM...
    TEST_ASSERT_EQUAL(0, config.security.private_key.size);
    // ...and the identity carried by devicestate is untouched, so the NodeNum
    // cannot move.
    TEST_ASSERT_EQUAL_UINT32(baseNodeNum, myNodeInfo.my_node_num);
    TEST_ASSERT_EQUAL(32, owner.public_key.size);
    TEST_ASSERT_EQUAL_MEMORY(basePublicKey, owner.public_key.bytes, 32);
    // The boot must not have overwritten the (maybe transiently) corrupt file
    // with defaults.
    TEST_ASSERT_EQUAL_UINT64(fpGarbage, fileFingerprint(configFileName));
}

// Runs against the still-degraded NodeDB from the previous test: no ordinary
// runtime save may replace the only persisted config generation. Recovery is
// deliberately limited to a clean reboot, BLE OTA/XMODEM, or explicit full
// reset.
static void test_degradedBoot_runtimeConfigSaveIsRejected(void)
{
    TEST_MESSAGE("=== Degraded boot: runtime config saves remain fail-closed ===");
    TEST_ASSERT_TRUE(NodeDBTestShim::decodeFailed(nodeDB));
    const uint64_t fpGarbage = fileFingerprint(configFileName);

    const auto priorError = error_code;
    TEST_ASSERT_FALSE(nodeDB->saveToDisk(SEGMENT_CONFIG));
    TEST_ASSERT_EQUAL(priorError, error_code);
    TEST_ASSERT_EQUAL_UINT64(fpGarbage, fileFingerprint(configFileName));
}

static void test_restoredConfig_recoversOriginalIdentity(void)
{
    TEST_MESSAGE("=== Good config bytes restored: next boot is normal with the "
                 "ORIGINAL identity ===");
    writeFileBytes(configFileName, goodConfigBytes.data(), goodConfigBytes.size());

    rebootNodeDB();

    TEST_ASSERT_FALSE(NodeDBTestShim::decodeFailed(nodeDB));
    TEST_ASSERT_EQUAL(meshtastic_Config_LoRaConfig_RegionCode_US, config.lora.region);
    TEST_ASSERT_TRUE(config.lora.tx_enabled);
    assertIdentityMatchesBaseline();
}

static void test_partialPrivateKey_freezesKnownIdentity(void)
{
    TEST_MESSAGE("=== Partial private key: known identity freezes instead of "
                 "rotating ===");
    config.security.private_key.size = 1;
    TEST_ASSERT_TRUE(nodeDB->saveToDisk(SEGMENT_CONFIG));
    const uint64_t invalidFingerprint = fileFingerprint(configFileName);

    rebootNodeDB();

    TEST_ASSERT_TRUE(NodeDBTestShim::decodeFailed(nodeDB));
    TEST_ASSERT_EQUAL_UINT32(baseNodeNum, myNodeInfo.my_node_num);
    TEST_ASSERT_EQUAL_MEMORY(basePublicKey, owner.public_key.bytes, 32);
    TEST_ASSERT_EQUAL_UINT64(invalidFingerprint, fileFingerprint(configFileName));

    writeFileBytes(configFileName, goodConfigBytes.data(), goodConfigBytes.size());
    rebootNodeDB();
    assertIdentityMatchesBaseline();
}

// --- A missing config on an established identity is not a fresh install ---

static void test_missingConfigWithKnownIdentity_failsClosed(void)
{
    TEST_MESSAGE("=== Missing config + existing public identity: no replacement "
                 "keypair ===");
    TEST_ASSERT_TRUE(FSCom.remove(configFileName));

    rebootNodeDB();

    TEST_ASSERT_TRUE(NodeDBTestShim::decodeFailed(nodeDB));
    TEST_ASSERT_EQUAL(meshtastic_Config_LoRaConfig_RegionCode_UNSET, config.lora.region);
    TEST_ASSERT_EQUAL_UINT32(baseNodeNum, myNodeInfo.my_node_num);
    TEST_ASSERT_EQUAL_MEMORY(basePublicKey, owner.public_key.bytes, 32);
    TEST_ASSERT_FALSE(FSCom.exists(configFileName));

    writeFileBytes(configFileName, goodConfigBytes.data(), goodConfigBytes.size());
    rebootNodeDB();
    assertIdentityMatchesBaseline();
}

static void test_tempOnlyConfig_isEvidenceButNotAnActiveGeneration(void)
{
    TEST_MESSAGE("=== Temp-only config: interrupted write stays fail-closed "
                 "across reboot ===");
    const std::string temporaryConfig = std::string(configFileName) + ".tmp";
    const uint64_t channelFingerprint = fileFingerprint(channelFileName);
    TEST_ASSERT_TRUE(FSCom.remove(configFileName));
    writeFileBytes(temporaryConfig.c_str(), goodConfigBytes.data(), goodConfigBytes.size());
    const uint64_t temporaryFingerprint = fileFingerprint(temporaryConfig.c_str());

    rebootNodeDB();
    rebootNodeDB();

    TEST_ASSERT_TRUE(NodeDBTestShim::decodeFailed(nodeDB));
    TEST_ASSERT_TRUE(NodeDBTestShim::unreadableSegments(nodeDB) & SEGMENT_CONFIG);
    TEST_ASSERT_EQUAL(meshtastic_Config_LoRaConfig_RegionCode_UNSET, config.lora.region);
    TEST_ASSERT_FALSE(config.lora.tx_enabled);
    TEST_ASSERT_FALSE(FSCom.exists(configFileName));
    TEST_ASSERT_EQUAL_UINT64(temporaryFingerprint, fileFingerprint(temporaryConfig.c_str()));
    TEST_ASSERT_EQUAL_UINT64(channelFingerprint, fileFingerprint(channelFileName));

    TEST_ASSERT_TRUE(FSCom.remove(temporaryConfig.c_str()));
    writeFileBytes(configFileName, goodConfigBytes.data(), goodConfigBytes.size());
    rebootNodeDB();
    assertIdentityMatchesBaseline();
}

static void test_partialGenerationWithoutDeviceState_doesNotBecomeFirstBoot(void)
{
    TEST_MESSAGE("=== Missing config + devicestate with remaining channels: no "
                 "first-boot rewrite ===");
    const uint64_t channelFingerprint = fileFingerprint(channelFileName);
    TEST_ASSERT_TRUE(FSCom.remove(configFileName));
    TEST_ASSERT_TRUE(FSCom.remove(deviceStateFileName));

    rebootNodeDB();
    rebootNodeDB();

    TEST_ASSERT_TRUE(NodeDBTestShim::decodeFailed(nodeDB));
    TEST_ASSERT_TRUE(NodeDBTestShim::unreadableSegments(nodeDB) & SEGMENT_CONFIG);
    TEST_ASSERT_TRUE(NodeDBTestShim::unreadableSegments(nodeDB) & SEGMENT_DEVICESTATE);
    TEST_ASSERT_EQUAL(meshtastic_Config_LoRaConfig_RegionCode_UNSET, config.lora.region);
    TEST_ASSERT_FALSE(config.lora.tx_enabled);
    TEST_ASSERT_FALSE(FSCom.exists(configFileName));
    TEST_ASSERT_FALSE(FSCom.exists(deviceStateFileName));
    TEST_ASSERT_EQUAL_UINT64(channelFingerprint, fileFingerprint(channelFileName));

    writeFileBytes(configFileName, goodConfigBytes.data(), goodConfigBytes.size());
    writeFileBytes(deviceStateFileName, goodDeviceStateBytes.data(), goodDeviceStateBytes.size());
    rebootNodeDB();
    assertIdentityMatchesBaseline();
}

static void test_missingChannelsFromExistingGeneration_failsClosed(void)
{
    TEST_MESSAGE("=== Missing channels from existing generation: never transmit "
                 "on Default ===");
    const uint64_t configFingerprint = fileFingerprint(configFileName);
    TEST_ASSERT_TRUE(FSCom.remove(channelFileName));

    rebootNodeDB();
    rebootNodeDB();

    TEST_ASSERT_TRUE(NodeDBTestShim::decodeFailed(nodeDB));
    TEST_ASSERT_TRUE(NodeDBTestShim::unreadableSegments(nodeDB) & SEGMENT_CHANNELS);
    TEST_ASSERT_EQUAL(meshtastic_Config_LoRaConfig_RegionCode_UNSET, config.lora.region);
    TEST_ASSERT_FALSE(config.lora.tx_enabled);
    TEST_ASSERT_FALSE(FSCom.exists(channelFileName));
    TEST_ASSERT_EQUAL_UINT64(configFingerprint, fileFingerprint(configFileName));

    writeFileBytes(channelFileName, goodChannelBytes.data(), goodChannelBytes.size());
    rebootNodeDB();
    assertIdentityMatchesBaseline();
}

static void test_corruptChannels_preservesPskGenerationAndSilencesRadio(void)
{
    TEST_MESSAGE("=== Corrupt channels.proto: preserve file and keep radio silent ===");
    writeFileBytes(channelFileName, kGarbage, sizeof(kGarbage));
    const uint64_t corruptFingerprint = fileFingerprint(channelFileName);

    rebootNodeDB();

    TEST_ASSERT_TRUE(NodeDBTestShim::decodeFailed(nodeDB));
    TEST_ASSERT_TRUE(NodeDBTestShim::unreadableSegments(nodeDB) & SEGMENT_CHANNELS);
    TEST_ASSERT_EQUAL(meshtastic_Config_LoRaConfig_RegionCode_UNSET, config.lora.region);
    TEST_ASSERT_FALSE(nodeDB->saveToDisk(SEGMENT_CHANNELS));
    TEST_ASSERT_EQUAL_UINT64(corruptFingerprint, fileFingerprint(channelFileName));

    writeFileBytes(channelFileName, goodChannelBytes.data(), goodChannelBytes.size());
    rebootNodeDB();
    assertIdentityMatchesBaseline();
}

static void test_missingNodeDatabaseWithPersistedKey_failsClosed(void)
{
    TEST_ASSERT_TRUE(FSCom.remove(nodeDatabaseFileName));
    const uint64_t configFingerprint = fileFingerprint(configFileName);

    rebootNodeDB();

    TEST_ASSERT_TRUE(NodeDBTestShim::decodeFailed(nodeDB));
    TEST_ASSERT_TRUE(NodeDBTestShim::unreadableSegments(nodeDB) & SEGMENT_NODEDATABASE);
    TEST_ASSERT_EQUAL(meshtastic_Config_LoRaConfig_RegionCode_UNSET, config.lora.region);
    TEST_ASSERT_FALSE(nodeDB->saveToDisk(SEGMENT_CONFIG));
    TEST_ASSERT_EQUAL_UINT64(configFingerprint, fileFingerprint(configFileName));

    writeFileBytes(nodeDatabaseFileName, goodNodeDatabaseBytes.data(), goodNodeDatabaseBytes.size());
    rebootNodeDB();
    assertIdentityMatchesBaseline();
}

static void test_missingNodeDatabaseWithLicensedOwner_failsClosed(void)
{
    config.lora.region = meshtastic_Config_LoRaConfig_RegionCode_UNSET;
    config.security.private_key.size = 0;
    config.security.public_key.size = 0;
    memset(config.security.private_key.bytes, 0, sizeof(config.security.private_key.bytes));
    memset(config.security.public_key.bytes, 0, sizeof(config.security.public_key.bytes));
    owner.public_key.size = 0;
    memset(owner.public_key.bytes, 0, sizeof(owner.public_key.bytes));
    owner.is_licensed = true;
    TEST_ASSERT_TRUE(nodeDB->saveToDisk(SEGMENT_CONFIG | SEGMENT_DEVICESTATE));
    TEST_ASSERT_TRUE(FSCom.remove(nodeDatabaseFileName));

    rebootNodeDB();

    TEST_ASSERT_TRUE(NodeDBTestShim::decodeFailed(nodeDB));
    TEST_ASSERT_TRUE(NodeDBTestShim::unreadableSegments(nodeDB) & SEGMENT_NODEDATABASE);
    TEST_ASSERT_FALSE(nodeDB->saveToDisk(SEGMENT_CONFIG | SEGMENT_DEVICESTATE));

    writeFileBytes(configFileName, goodConfigBytes.data(), goodConfigBytes.size());
    writeFileBytes(deviceStateFileName, goodDeviceStateBytes.data(), goodDeviceStateBytes.size());
    writeFileBytes(channelFileName, goodChannelBytes.data(), goodChannelBytes.size());
    writeFileBytes(nodeDatabaseFileName, goodNodeDatabaseBytes.data(), goodNodeDatabaseBytes.size());
    rebootNodeDB();
    assertIdentityMatchesBaseline();
}

// --- Freeze is config-scoped ---

static void test_corruptNodesDb_doesNotFreezeIdentity(void)
{
    TEST_MESSAGE("=== Corrupt nodes.proto alone: config loads, keygen runs, "
                 "NodeNum kept ===");
    const uint64_t fpConfig = fileFingerprint(configFileName);
    writeFileBytes(nodeDatabaseFileName, kGarbage, sizeof(kGarbage));
    const uint64_t corruptNodeDbFingerprint = fileFingerprint(nodeDatabaseFileName);

    rebootNodeDB();

    TEST_ASSERT_FALSE(NodeDBTestShim::decodeFailed(nodeDB));
    TEST_ASSERT_EQUAL(meshtastic_Config_LoRaConfig_RegionCode_US, config.lora.region);
    assertIdentityMatchesBaseline();
    // The store rebuilt from defaults still contains us.
    const meshtastic_NodeInfoLite *self = nodeDB->getMeshNode(nodeDB->getNodeNum());
    TEST_ASSERT_NOT_NULL(self);
    TEST_ASSERT_TRUE(nodeInfoLiteHasUser(self));
    TEST_ASSERT_EQUAL_UINT64(fpConfig, fileFingerprint(configFileName));
    TEST_ASSERT_TRUE(NodeDBTestShim::unreadableSegments(nodeDB) & SEGMENT_NODEDATABASE);
    TEST_ASSERT_FALSE(nodeDB->saveToDisk(SEGMENT_NODEDATABASE));
    TEST_ASSERT_EQUAL_UINT64(corruptNodeDbFingerprint, fileFingerprint(nodeDatabaseFileName));

    writeFileBytes(nodeDatabaseFileName, goodNodeDatabaseBytes.data(), goodNodeDatabaseBytes.size());
    rebootNodeDB();
}

// --- Devicestate-loss owner recovery ---

// The recovery block in loadFromDisk() fires when device.proto decodes but is
// below DEVICESTATE_MIN_VER: identity fields survive (my_node_num is in the
// decoded struct), the defaults overwrite the owner names, and the own-node
// entry in nodes.proto restores them.
static void test_oldDevicestate_recoversOwnerFromNodeDb(void)
{
    TEST_MESSAGE("=== Old-version devicestate: owner names recovered from own "
                 "NodeDB entry ===");
    // Put the recoverable names into the store of record...
    strncpy(owner.long_name, "Recovered Owner", sizeof(owner.long_name));
    strncpy(owner.short_name, "RCVR", sizeof(owner.short_name));
    meshtastic_NodeInfoLite *self = nodeDB->getMeshNode(nodeDB->getNodeNum());
    TEST_ASSERT_NOT_NULL(self);
    TypeConversions::CopyUserToNodeInfoLite(self, owner);
    TEST_ASSERT_TRUE(nodeDB->saveToDisk(SEGMENT_NODEDATABASE));

    // ...then persist a devicestate that is valid but too old, carrying DIFFERENT
    // names, so a recovered name can only have come from the nodes.proto entry.
    strncpy(owner.long_name, "Stale Devicestate", sizeof(owner.long_name));
    strncpy(owner.short_name, "STAL", sizeof(owner.short_name));
    devicestate.version = DEVICESTATE_MIN_VER - 1;
    TEST_ASSERT_TRUE(nodeDB->saveToDisk(SEGMENT_DEVICESTATE));

    rebootNodeDB();

    TEST_ASSERT_EQUAL_STRING("Recovered Owner", owner.long_name);
    TEST_ASSERT_EQUAL_STRING("RCVR", owner.short_name);
    // Identity survives the devicestate discard: the NodeNum in the old file is
    // carried over and keygen re-derives the same crc32(public_key) value.
    assertIdentityMatchesBaseline();

    // The recovery is re-persisted: the on-disk devicestate is current-version
    // with the recovered names, not the stale ones.
    static meshtastic_DeviceState saved;
    TEST_ASSERT_EQUAL(LoadFileResult::LOAD_SUCCESS, nodeDB->loadProto(deviceStateFileName, meshtastic_DeviceState_size,
                                                                      sizeof(saved), &meshtastic_DeviceState_msg, &saved));
    TEST_ASSERT_EQUAL(DEVICESTATE_CUR_VER, saved.version);
    TEST_ASSERT_EQUAL_STRING("Recovered Owner", saved.owner.long_name);
}

// --- loadProto classification ---

// The wipe cascade lived in the difference between these verdicts. A genuinely
// missing file must be NOT_FOUND; a present malformed file is protected as a
// read failure instead of being mistaken for a first boot.
static void test_loadProto_classifiesFailuresDistinctly(void)
{
    TEST_MESSAGE("=== loadProto: absent=NOT_FOUND, garbage/truncated=DECODE_FAILED ===");
    const char *scratchPath = "/prefs/nbr_scratch.proto";
    static meshtastic_LocalConfig scratch;

    FSCom.remove(scratchPath);
    TEST_ASSERT_EQUAL(LoadFileResult::NOT_FOUND, nodeDB->loadProto(scratchPath, meshtastic_LocalConfig_size, sizeof(scratch),
                                                                   &meshtastic_LocalConfig_msg, &scratch));

    writeFileBytes(scratchPath, kGarbage, sizeof(kGarbage));
    TEST_ASSERT_EQUAL(LoadFileResult::DECODE_FAILED, nodeDB->loadProto(scratchPath, meshtastic_LocalConfig_size, sizeof(scratch),
                                                                       &meshtastic_LocalConfig_msg, &scratch));

    // A torn write: a valid encoding minus its final byte always cuts the last
    // field short.
    TEST_ASSERT_GREATER_THAN(1, goodConfigBytes.size());
    writeFileBytes(scratchPath, goodConfigBytes.data(), goodConfigBytes.size() - 1);
    TEST_ASSERT_EQUAL(LoadFileResult::DECODE_FAILED, nodeDB->loadProto(scratchPath, meshtastic_LocalConfig_size, sizeof(scratch),
                                                                       &meshtastic_LocalConfig_msg, &scratch));

    // The unmodified bytes still decode - the failure above was the truncation,
    // nothing else.
    writeFileBytes(scratchPath, goodConfigBytes.data(), goodConfigBytes.size());
    TEST_ASSERT_EQUAL(LoadFileResult::LOAD_SUCCESS, nodeDB->loadProto(scratchPath, meshtastic_LocalConfig_size, sizeof(scratch),
                                                                      &meshtastic_LocalConfig_msg, &scratch));

    FSCom.remove(scratchPath); // leave nothing behind
}

// installDefaultModuleConfig() sized its memset to meshtastic_ModuleConfig (the
// 368-byte oneof) instead of the 1092-byte LocalModuleConfig, so submessages it
// never assigns kept their old values.
static void test_oldModuleConfig_discardClearsTailSubmessages(void)
{
    // statusmessage sits at offset 609, past the old 368-byte clear, and the
    // defaults installer never assigns it - so only the full-struct memset can
    // drop this marker.
    moduleConfig = meshtastic_LocalModuleConfig_init_zero;
    moduleConfig.version = DEVICESTATE_MIN_VER - 1;
    moduleConfig.has_statusmessage = true;
    strncpy(moduleConfig.statusmessage.node_status, "STALE-TAIL", sizeof(moduleConfig.statusmessage.node_status) - 1);

    uint8_t buf[meshtastic_LocalModuleConfig_size];
    size_t len = pb_encode_to_bytes(buf, sizeof(buf), &meshtastic_LocalModuleConfig_msg, &moduleConfig);
    TEST_ASSERT_GREATER_THAN_size_t(0, len);
    writeFileBytes(moduleConfigFileName, buf, len);

    rebootNodeDB(); // decodes, sees version < DEVICESTATE_MIN_VER, discards

    // The opt-in migration re-stamps the version after the discard, so assert the
    // floor, not equality.
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(DEVICESTATE_CUR_VER, moduleConfig.version, "discard did not reinstall defaults");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", moduleConfig.statusmessage.node_status,
                                     "statusmessage survived the moduleConfig discard");

    FSCom.remove(moduleConfigFileName); // leave the sandbox as we found it
    rebootNodeDB();
}

NBR_TEST_ENTRY void setup()
{
    initializeTestEnvironment();
    nodeDB = new NodeDB(); // first boot on the pristine per-suite sandbox

    UNITY_BEGIN();

    printf("\n=== Healthy-boot identity ===\n");
    RUN_TEST(test_keylessUnsetGeneration_withoutNodeDatabaseIsHealthy);
    RUN_TEST(test_firstBoot_establishesKeyedIdentity);
    RUN_TEST(test_healthyReboot_preservesIdentity);
    RUN_TEST(test_repeatedReload_replacesRatherThanAppendsNodeDatabase);

    printf("\n=== Degraded boot (corrupt config) ===\n");
    RUN_TEST(test_corruptConfig_freezesIdentity_leavesFileUntouched);
    RUN_TEST(test_degradedBoot_runtimeConfigSaveIsRejected);
    RUN_TEST(test_restoredConfig_recoversOriginalIdentity);
    RUN_TEST(test_partialPrivateKey_freezesKnownIdentity);

    printf("\n=== Fresh install vs freeze scoping ===\n");
    RUN_TEST(test_missingConfigWithKnownIdentity_failsClosed);
    RUN_TEST(test_tempOnlyConfig_isEvidenceButNotAnActiveGeneration);
    RUN_TEST(test_partialGenerationWithoutDeviceState_doesNotBecomeFirstBoot);
    RUN_TEST(test_missingChannelsFromExistingGeneration_failsClosed);
    RUN_TEST(test_corruptChannels_preservesPskGenerationAndSilencesRadio);
    RUN_TEST(test_missingNodeDatabaseWithPersistedKey_failsClosed);
    RUN_TEST(test_missingNodeDatabaseWithLicensedOwner_failsClosed);
    RUN_TEST(test_corruptNodesDb_doesNotFreezeIdentity);

    printf("\n=== Devicestate recovery + loadProto classification ===\n");
    RUN_TEST(test_oldDevicestate_recoversOwnerFromNodeDb);
    RUN_TEST(test_loadProto_classifiesFailuresDistinctly);

    printf("\n=== ModuleConfig discard completeness ===\n");
    RUN_TEST(test_oldModuleConfig_discardClearsTailSubmessages);

    exit(UNITY_END());
}

NBR_TEST_ENTRY void loop() {}

#else // !FSCom || PKI excluded

void setUp(void) {}
void tearDown(void) {}

NBR_TEST_ENTRY void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    exit(UNITY_END());
}

NBR_TEST_ENTRY void loop() {}

#endif
