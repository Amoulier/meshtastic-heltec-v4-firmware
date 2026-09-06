from pathlib import Path
root=Path('candidate')
def edit(path,old,new):
 p=root/path; s=p.read_text(); n=s.count(old)
 if n!=1: raise RuntimeError(f'{path}: expected unique block, found {n}: {old[:80]}')
 p.write_text(s.replace(old,new))
def replace_function(path,name,new):
 p=root/path;s=p.read_text(); start=s.index(name); brace=s.index('{',start); depth=0
 for i in range(brace,len(s)):
  if s[i]=='{':depth+=1
  elif s[i]=='}':
   depth-=1
   if not depth:break
 p.write_text(s[:start]+new+s[i+1:])

p='test/test_admin_radio/test_main.cpp'
edit(p,'#include "graphics/draw/MenuHandler.h"','#include "graphics/draw/MenuHandler.h"\n#include "main.h"')
edit(p,'static meshtastic_ChannelFile savedChannelFile;','static meshtastic_ChannelFile savedChannelFile;\nstatic meshtastic_LocalModuleConfig savedModuleConfig;\nstatic uint32_t savedRebootAtMsec;\nstatic uint32_t savedShutdownAtMsec;')
edit(p,'    savedChannelFile = channelFile;\n    replacementNodeDB = new NodeDB();','    savedChannelFile = channelFile;\n    savedModuleConfig = moduleConfig;\n    savedRebootAtMsec = rebootAtMsec;\n    savedShutdownAtMsec = shutdownAtMsec;\n    // A scheduled lifecycle transition belongs to one test, not the next.\n    rebootAtMsec = 0;\n    shutdownAtMsec = 0;\n    replacementNodeDB = new NodeDB();')
edit(p,'static void dropRestoreCryptoStub();','static void dropRestoreCryptoStub();\nstatic void dropConfigChangedCounter();')
edit(p,'    channelFile = savedChannelFile;\n    if (crypto)','    channelFile = savedChannelFile;\n    moduleConfig = savedModuleConfig;\n    rebootAtMsec = savedRebootAtMsec;\n    shutdownAtMsec = savedShutdownAtMsec;\n    if (crypto)')
replace_function(p,'static void test_restorePreferences_sanitizesLicensedBackupBeforeReturn()', '''static void test_restorePreferences_sanitizesLicensedBackupBeforeReturn()
{
    // Use the fixture-owned database/router so Unity longjmp cannot leak them.
    // Restore validates and installs a key under the same crypto lock as boot.
    hamMockRouter = new HamModeMockRouter();
    router = hamMockRouter;
    owner.is_licensed = true;
    installEncryptedAndAdminChannels();
    const auto priorPrivateKey = config.security.private_key;
    const auto priorPublicKey = config.security.public_key;
    const NodeNum priorNodeNum = nodeDB->getNodeNum();
    TEST_ASSERT_TRUE(nodeDB->backupPreferences(meshtastic_AdminMessage_BackupLocation_FLASH));

    owner.is_licensed = false;
    channels.initDefaults();
    TEST_ASSERT_TRUE(
        nodeDB->restorePreferences(meshtastic_AdminMessage_BackupLocation_FLASH, SEGMENT_DEVICESTATE | SEGMENT_CHANNELS));
    TEST_ASSERT_TRUE(owner.is_licensed);
    assertLicensedChannelsSanitized();
    TEST_ASSERT_FALSE_MESSAGE(channels.ensureLicensedOperation(), "restored licensed channels must remain sanitized");
    TEST_ASSERT_EQUAL_UINT32(priorNodeNum, nodeDB->getNodeNum());
    TEST_ASSERT_EQUAL_MEMORY(priorPrivateKey.bytes, config.security.private_key.bytes, 32);
    TEST_ASSERT_EQUAL_MEMORY(priorPublicKey.bytes, owner.public_key.bytes, 32);
    TEST_ASSERT_TRUE(FSCom.remove(backupFileName));
}''')
edit(p,'    testAdmin->handleReceivedProtobuf(mp, &m);\n}\n\nstatic void sendSetChannel', '''    testAdmin->drainReply();
    testAdmin->handleReceivedProtobuf(mp, &m);
    // These helpers model successful requests. Do not let an unexpected
    // rejection masquerade as a clean "no warning" result.
    if (testAdmin->reply() && testAdmin->reply()->decoded.portnum == meshtastic_PortNum_ROUTING_APP) {
        meshtastic_Routing_Error err = meshtastic_Routing_Error_BAD_REQUEST;
        TEST_ASSERT_TRUE(decodeRoutingError(testAdmin->reply(), err));
        TEST_ASSERT_EQUAL(meshtastic_Routing_Error_NONE, err);
    }
    testAdmin->drainReply();
}

static void sendSetChannel''')
needle='static void test_warn_singleChannel_variantName_oneSpecificMessage()'
s=(root/p).read_text(); idx=s.index(needle)
new='''static void test_pendingLifecycle_rejectsMutationWithoutChangingChannel()
{
    usePresetLongFast();
    const meshtastic_Channel before = channels.getByIndex(0);
    meshtastic_AdminMessage message = meshtastic_AdminMessage_init_zero;
    message.which_payload_variant = meshtastic_AdminMessage_set_channel_tag;
    message.set_channel = makeChannel(0, meshtastic_Channel_Role_PRIMARY, "blocked", DEFAULT_KEY, 1);
    meshtastic_MeshPacket packet = meshtastic_MeshPacket_init_zero;
    packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    packet.decoded.want_response = true;
    for (bool shutdown : {false, true}) {
        rebootAtMsec = shutdown ? 0 : millis() + 10000;
        shutdownAtMsec = shutdown ? millis() + 10000 : 0;
        testAdmin->handleReceivedProtobuf(packet, &message);
        meshtastic_Routing_Error err = meshtastic_Routing_Error_NONE;
        TEST_ASSERT_TRUE(decodeRoutingError(testAdmin->reply(), err));
        TEST_ASSERT_EQUAL(meshtastic_Routing_Error_BAD_REQUEST, err);
        TEST_ASSERT_EQUAL_STRING(before.settings.name, channels.getByIndex(0).settings.name);
        TEST_ASSERT_EQUAL_MEMORY(before.settings.psk.bytes, channels.getByIndex(0).settings.psk.bytes,
                                 before.settings.psk.size);
        testAdmin->drainReply();
    }
}

'''
(root/p).write_text(s[:idx]+new+s[idx:])
edit(p,'static const NodeNum TEST_NODE_NUM = 0x12345678;', '''// Heap-owned by tearDown: Unity assertions use longjmp, which skips C++
// stack destructors and would leave a registered stack observer dangling.
static ConfigChangedCounter *activeConfigChangedCounter = nullptr;
static ConfigChangedCounter &installConfigChangedCounter()
{
    activeConfigChangedCounter = new ConfigChangedCounter();
    activeConfigChangedCounter->observe(&service->configChanged);
    return *activeConfigChangedCounter;
}
static void dropConfigChangedCounter()
{
    delete activeConfigChangedCounter;
    activeConfigChangedCounter = nullptr;
}

static const NodeNum TEST_NODE_NUM = 0x12345678;''')
s=(root/p).read_text();s=s.replace('    ConfigChangedCounter counter;\n    counter.observe(&service->configChanged);','    ConfigChangedCounter &counter = installConfigChangedCounter();'); (root/p).write_text(s)
s=(root/p).read_text();start=s.index('// CHARACTERIZATION OF A KNOWN DEFECT');end=s.index('// -----------------------------------------------------------------------\n// BaseUI region chooser',start)
s=s[:start]+'''// A node metadata edit must not rewrite identity, channels or module settings.
static void test_toggleNodeMuted_persistsOnlyNodeDatabase()
{
    auto *node = nodeDB->getOrCreateMeshNode(TEST_NODE_NUM);
    TEST_ASSERT_NOT_NULL(node);
    nodeInfoLiteSetBit(node, NODEINFO_BITFIELD_HAS_USER_MASK, true);
    TEST_ASSERT_TRUE(nodeDB->saveToDisk(SEGMENT_CONFIG | SEGMENT_MODULECONFIG | SEGMENT_DEVICESTATE |
                                       SEGMENT_CHANNELS | SEGMENT_NODEDATABASE));
    const char *otherFiles[] = {configFileName, moduleConfigFileName, deviceStateFileName, channelFileName};
    std::vector<std::string> before;
    const auto readBytes = [](const char *path) {
        File file = FSCom.open(path, FILE_O_READ);
        std::string bytes;
        while (file && file.available())
            bytes.push_back(static_cast<char>(file.read()));
        file.close();
        return bytes;
    };
    for (const char *path : otherFiles) {
        TEST_ASSERT_TRUE_MESSAGE(FSCom.exists(path), path);
        before.push_back(readBytes(path));
    }
    TEST_ASSERT_TRUE(FSCom.remove(nodeDatabaseFileName));

    graphics::menuHandler::toggleNodeMuted(TEST_NODE_NUM);
    TEST_ASSERT_TRUE(nodeInfoLiteIsMuted(nodeDB->getMeshNode(TEST_NODE_NUM)));
    TEST_ASSERT_TRUE(FSCom.exists(nodeDatabaseFileName));
    for (size_t i = 0; i < before.size(); ++i) {
        TEST_ASSERT_TRUE_MESSAGE(FSCom.exists(otherFiles[i]), otherFiles[i]);
        TEST_ASSERT_TRUE_MESSAGE(before[i] == readBytes(otherFiles[i]), otherFiles[i]);
    }
}

'''+s[end:];s=s.replace('RUN_TEST(test_toggleNodeMuted_currentlyRewritesEverySegment);','RUN_TEST(test_toggleNodeMuted_persistsOnlyNodeDatabase);');(root/p).write_text(s)
edit(p,'void tearDown(void)\n{\n    restoreAdminRadioGlobals();','void tearDown(void)\n{\n    testAdmin->drainReply();\n    dropConfigChangedCounter();\n    restoreAdminRadioGlobals();')
edit(p,'    RUN_TEST(test_warn_singleChannel_variantName_oneSpecificMessage);','    RUN_TEST(test_pendingLifecycle_rejectsMutationWithoutChangingChannel);\n    RUN_TEST(test_warn_singleChannel_variantName_oneSpecificMessage);')
p='test/test_xmodem/test_main.cpp'
edit(p,'// today; the two tests marked "documents current behaviour" pin known state-confusion edges so a\n// deliberate fix has to update them consciously.','// today, including state-confusion regressions: completed uploads and download sources must\n// survive cancellation, and a misplaced EOT cannot acknowledge or wedge a download.')
replace_function(p,'void test_xmodem_can_after_eot_removes_completed_file(void)', '''void test_xmodem_can_after_eot_preserves_completed_file(void)
{
    uint8_t payload[8], readback[8];
    fillPattern(payload, sizeof(payload), 5);
    startReceive();
    xm->handlePacket(makeData(1, payload, sizeof(payload)));
    xm->handlePacket(makeControl(meshtastic_XModem_Control_EOT));
    TEST_ASSERT_FALSE(xm->isBusy());
    xm->handlePacket(makeControl(meshtastic_XModem_Control_CAN));
    TEST_ASSERT_EQUAL(meshtastic_XModem_Control_ACK, xm->getForPhone().control);
    TEST_ASSERT_FALSE(xm->isBusy());
    TEST_ASSERT_TRUE(FSCom.exists(kRxPath));
    TEST_ASSERT_EQUAL_size_t(sizeof(payload), readAll(kRxPath, readback, sizeof(readback)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(payload, readback, sizeof(payload));
}''')
replace_function(p,'void test_xmodem_eot_mid_transmit_leaves_state_busy(void)', '''void test_xmodem_eot_mid_transmit_rejects_and_resumes(void)
{
    uint8_t payload[300], readback[300];
    fillPattern(payload, sizeof(payload), 13);
    startTransmit(payload, sizeof(payload));
    xm->handlePacket(makeControl(meshtastic_XModem_Control_EOT));
    TEST_ASSERT_EQUAL(meshtastic_XModem_Control_NAK, xm->getForPhone().control);
    TEST_ASSERT_TRUE(xm->isBusy());
    // Rejection leaves the valid sending session and its file offset intact.
    xm->handlePacket(makeControl(meshtastic_XModem_Control_ACK));
    meshtastic_XModem next = xm->getForPhone();
    TEST_ASSERT_EQUAL(meshtastic_XModem_Control_SOH, next.control);
    TEST_ASSERT_EQUAL_UINT16(2, next.seq);
    TEST_ASSERT_EQUAL_UINT16(kChunk, next.buffer.size);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(payload + kChunk, next.buffer.bytes, kChunk);
    xm->handlePacket(makeControl(meshtastic_XModem_Control_CAN));
    TEST_ASSERT_FALSE(xm->isBusy());
    TEST_ASSERT_EQUAL_size_t(sizeof(payload), readAll(kTxPath, readback, sizeof(readback)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(payload, readback, sizeof(payload));
}''')
s=(root/p).read_text().replace('RUN_TEST(test_xmodem_can_after_eot_removes_completed_file);','RUN_TEST(test_xmodem_can_after_eot_preserves_completed_file);').replace('RUN_TEST(test_xmodem_eot_mid_transmit_leaves_state_busy);','RUN_TEST(test_xmodem_eot_mid_transmit_rejects_and_resumes);');(root/p).write_text(s)
p='test/test_nodedb_boot_recovery/test_main.cpp'
edit(p,'    TEST_ASSERT_NOT_NULL(nodeDB->getOrCreateMeshNode(diskOnlyNode));','''    auto *diskNode = nodeDB->getOrCreateMeshNode(diskOnlyNode);
    TEST_ASSERT_NOT_NULL(diskNode);
    // Empty discoveries are intentionally purged on boot; use a real user
    // record so this tests replacement of repeated fields, not cleanup policy.
    nodeInfoLiteSetBit(diskNode, NODEINFO_BITFIELD_HAS_USER_MASK, true);''')
