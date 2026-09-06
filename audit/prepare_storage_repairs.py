from pathlib import Path
r=Path('candidate')
(r/'src/PreferenceRecoveryPolicy.h').write_text('''#pragma once

// Compile the board-independent Heltec preference checks on the native audit
// target as well. This is not an ESP32, NVS, FreeRTOS or low-voltage emulator.
#if defined(HELTEC_V4_NATIVE_STORAGE_AUDIT) && (!defined(PIO_UNIT_TESTING) || !defined(ARCH_PORTDUINO))
#error "HELTEC_V4_NATIVE_STORAGE_AUDIT is restricted to native unit tests"
#endif

#if defined(HELTEC_V4_OLED) || defined(HELTEC_V4_NATIVE_STORAGE_AUDIT)
#define HAS_STRICT_PREFERENCE_RECOVERY 1
#else
#define HAS_STRICT_PREFERENCE_RECOVERY 0
#endif
''')
p=r/'src/mesh/NodeDB.cpp';s=p.read_text()
s=s.replace('#include "RadioInterface.h"','#include "RadioInterface.h"\n#include "PreferenceRecoveryPolicy.h"',1)
def patch(old,new):
 global s
 if s.count(old)!=1:raise RuntimeError(f'expected1, got{s.count(old)}: {old[:100]}')
 s=s.replace(old,new,1)
start=s.index('static void forceHeltecLocalRecoveryConfiguration()');end=s.index('\n#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN',start)
s=s[:start]+s[start:end].replace('#if defined(HELTEC_V4_OLED)','#if HAS_STRICT_PREFERENCE_RECOVERY')+s[end:]
start=s.index('        const size_t privateKeySize = config.security.private_key.size;')
start=s.rfind('#if defined(HELTEC_V4_OLED)',0,start);end=s.index('\n#else\n        const bool restoringLegacyIdentity',start)
s=s[:start]+s[start:end].replace('#if defined(HELTEC_V4_OLED)','#if HAS_STRICT_PREFERENCE_RECOVERY',1)+s[end:]
start=s.index('    bool persistedCoreGenerationPresent = false;');end=s.index('\n#if defined(HELTEC_V4_OLED) && defined(FSCom)\n    const HeltecResetPendingKind pendingReset',start)
s=s[:start]+s[start:end].replace('defined(HELTEC_V4_OLED)','HAS_STRICT_PREFERENCE_RECOVERY')+s[end:]
start=s.index('    const bool nodeDatabaseVersionTooNew');start=s.rfind('#if defined(HELTEC_V4_OLED)',0,start)
end=s.index('\n    state = loadProto(uiconfigFileName',start)
s=s[:start]+s[start:end].replace('defined(HELTEC_V4_OLED)','HAS_STRICT_PREFERENCE_RECOVERY')+s[end:]
patch('#if defined(HELTEC_V4_OLED)\n    if (incompleteConfigResetDetected)\n        configDecodeFailed = true;', '#if HAS_STRICT_PREFERENCE_RECOVERY\n    if (incompleteConfigResetDetected)\n        configDecodeFailed = true;')
for function,condition in [('bool NodeDB::saveProto(', 'if (preferenceSegment != 0 && requiresConfigRecovery() && !authorizedRecoveryWriter)'), ('bool NodeDB::saveNodeDatabaseToDisk()', 'if ((requiresConfigRecovery() || (unreadablePreferenceSegments & SEGMENT_NODEDATABASE) != 0) && !authorizedRecoveryWriter)'), ('bool NodeDB::saveToDisk(int saveWhat)', 'if (requiresConfigRecovery() && !authorizedRecoveryWriter)')]:
 f=s.index(function);decl=s.index('    const bool authorizedRecoveryWriter =',f);tail=s.index('\n    if ',decl);decl_text=s[decl:tail]
 s=s[:decl]+'#endif\n#if HAS_STRICT_PREFERENCE_RECOVERY\n#if defined(HELTEC_V4_OLED)\n'+decl_text+'\n#else\n    constexpr bool authorizedRecoveryWriter = false;\n#endif'+s[tail:]
 a=s.index('    '+condition,f);brace=s.index('{',a);depth=0
 for b in range(brace,len(s)):
  if s[b]=='{':depth+=1
  elif s[b]=='}':
   depth-=1
   if not depth:break
 s=s[:b+1]+'\n#endif\n#if defined(HELTEC_V4_OLED)'+s[b+1:]
patch('''#if defined(HELTEC_V4_OLED)
    if (shouldDeferBootPersistence(bootInitializationInProgress, configLoadComplete, configDecodeFailed)) {
        bootDeferredPreferenceSegments |= saveWhat;
        LOG_DEBUG("NodeDB: defer core save 0x%x until complete boot scan", saveWhat);
        return true;
    }
    PreferenceStorageWriteGuard storageWrite(*this);''','''#if HAS_STRICT_PREFERENCE_RECOVERY
    if (shouldDeferBootPersistence(bootInitializationInProgress, configLoadComplete, configDecodeFailed)) {
        bootDeferredPreferenceSegments |= saveWhat;
        LOG_DEBUG("NodeDB: defer core save 0x%x until complete boot scan", saveWhat);
        return true;
    }
#endif
#if defined(HELTEC_V4_OLED)
    PreferenceStorageWriteGuard storageWrite(*this);''')
s=s.replace('#if defined(HELTEC_V4_OLED)\n#endif\n#if HAS_STRICT_PREFERENCE_RECOVERY','#if HAS_STRICT_PREFERENCE_RECOVERY')
f=s.index('bool NodeDB::saveToDisk(int saveWhat)');a=s.index('    if (!shouldUseFilesystemPersistence(fsIsMounted()))',f)
s=s[:a]+'''    // This is a known read-only rejection, not a failed flash operation. The
    // lower saveProto() boundary rejects it too; do not retry or record a
    // spurious FLASH_CORRUPTION error (which terminates a native audit).
    if ((unreadablePreferenceSegments & saveWhat) != 0) {
        LOG_WARN("NodeDB: reject save of an unreadable preference segment");
        return false;
    }
'''+s[a:];p.write_text(s)
p=r/'test/test_nodedb_boot_recovery/test_main.cpp';s=p.read_text();s=s.replace('#include "FSCommon.h" // defines FSCom; must precede the feature guard below','''#include "FSCommon.h" // defines FSCom; must precede the feature guard below
#if defined(ARCH_PORTDUINO) && !defined(HELTEC_V4_NATIVE_STORAGE_AUDIT)
#error "Run this suite with the coverage-storage audit environment"
#endif''');s=s.replace('    TEST_ASSERT_FALSE(nodeDB->saveToDisk(SEGMENT_CONFIG));\n    TEST_ASSERT_EQUAL_UINT64(fpGarbage, fileFingerprint(configFileName));','    const auto priorError = error_code;\n    TEST_ASSERT_FALSE(nodeDB->saveToDisk(SEGMENT_CONFIG));\n    TEST_ASSERT_EQUAL(priorError, error_code);\n    TEST_ASSERT_EQUAL_UINT64(fpGarbage, fileFingerprint(configFileName));');p.write_text(s)
(r/'test/host/heltec_storage_audit.ini').write_text('''; Installed only inside the native CI workspace, never shipped as a hardware target.
[env:coverage-storage]
extends = env:coverage
build_flags = ${env:coverage.build_flags} -DHELTEC_V4_NATIVE_STORAGE_AUDIT=1
test_testing_command =
  ${platformio.src_dir}/../bin/pio-test-isolate.sh
  ${platformio.build_dir}/${this.__env__}/meshtasticd
  -s
''')
p=r/'.github/workflows/audit_native_heltec.yml';s=p.read_text();s=s.replace('    timeout-minutes: 45','''    timeout-minutes: 45
    strategy:
      fail-fast: false
      matrix:
        group: [shared, storage]''');s=s.replace('          rm -rf _native_fixture','''          cat test/host/heltec_storage_audit.ini >> variants/native/portduino/platformio.ini
          rm -rf _native_fixture''');s=s.replace('heltec-native-audit-v1-${{ runner.os }}-', 'heltec-native-audit-v2-${{ matrix.group }}-${{ runner.os }}-');s=s.replace('          restore-keys: heltec-native-audit-v2-${{ matrix.group }}-${{ runner.os }}-','''          restore-keys: |
            heltec-native-audit-v2-${{ matrix.group }}-${{ runner.os }}-
            heltec-native-audit-v1-${{ runner.os }}-''');s=s.replace('run: python3 bin/run-heltec-native-audit.py','run: python3 bin/run-heltec-native-audit.py --group ${{ matrix.group }}');s=s.replace('name: heltec-native-audit-${{ github.sha }}','name: heltec-native-audit-${{ matrix.group }}-${{ github.sha }}');p.write_text(s)
p=r/'bin/run-heltec-native-audit.py';s=p.read_text();s=s.replace('import json','import argparse\nimport json',1);s=s.replace("xml = report / 'native.xml'", '''parser = argparse.ArgumentParser()
parser.add_argument('--group', choices=('shared', 'storage'), required=True)
group = parser.parse_args().group
# Run each whole suite once, with the storage suite on the exact board-independent
# Heltec persistence policy. ESP32 GPIO/NVS/timing remain hardware-only checks.
storage_suites = {'test_nodedb_boot_recovery'}
suites = [s for s in suites if (s in storage_suites) == (group == 'storage')]
expected_count = 1 if group == 'storage' else 24
if len(suites) != expected_count:
    raise SystemExit('Incomplete required native audit group')
environment = 'coverage-storage' if group == 'storage' else 'coverage'
xml = report / 'native.xml' ''');s=s.replace("xml = report / 'native.xml' \n", "xml = report / 'native.xml'\n");s=s.replace("'-e', 'coverage'","'-e', environment");s=s.replace("'-f', 'test_module_config', '--without-testing'", "'-f', suites[0], '--without-testing'");s=s.replace("by_name = {s.attrib['name'].split(':')[-1]: s for s in ET.parse(xml).getroot().iter('testsuite')}","by_name = {s.attrib['name'].split(':')[-1]: s for s in ET.parse(xml).getroot().iter('testsuite')\n           if s.attrib['name'].startswith(environment + ':')}");s=s.replace("summary = {'status': 'PASS', 'source_commit':", "summary = {'status': 'PASS', 'group': group, 'environment': environment, 'source_commit':");s=s.replace("    print((report / 'native.log').read_text()[-14000:])", '''    # Preserve actionable failures, not only a long tail of SKIPPED environments.
    failures = []
    if xml.exists():
        for suite in ET.parse(xml).getroot().iter('testsuite'):
            for case in suite.iter('testcase'):
                for kind in ('failure', 'error'):
                    for item in case.findall(kind):
                        failures.append({'suite': suite.get('name'), 'test': case.get('name'),
                                         'kind': kind, 'message': item.get('message', ''),
                                         'details': item.text or ''})
    payload = {'status': 'FAIL', 'group': group, 'exit_code': result.returncode, 'failures': failures}
    (report / 'native-failures.json').write_text(json.dumps(payload, indent=2) + '\\n')
    print(json.dumps(payload, indent=2))
    print((report / 'native.log').read_text()[-4000:])''');p.write_text(s)
