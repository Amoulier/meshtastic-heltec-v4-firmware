#!/usr/bin/env python3
"""Run whole selected suites with per-suite isolation and reject false-green reports."""
from pathlib import Path
import argparse
import json
import os
import subprocess
import sys
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
os.chdir(root)
report = root / 'audit-results'
report.mkdir(exist_ok=True)
suites = (root / 'test/host/heltec_native_suites.txt').read_text().split()
if len(suites) != 25 or len(suites) != len(set(suites)):
    raise SystemExit('Expected the complete audited set of 25 distinct native suites')
for suite in suites:
    if not (root / 'test' / suite).is_dir():
        raise SystemExit(f'Missing required suite: {suite}')
parser = argparse.ArgumentParser()
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
xml = report / 'native.xml'
xml.unlink(missing_ok=True)
args = ['pio', 'test', '-e', environment, '-v', '--junit-output-path', str(xml)]
for suite in suites:
    args += ['-f', suite]
# Compile one complete suite first, so a broken host adapter fails once rather
# than repeating the same compilation error 25 times. This does not count as a
# test pass: every suite is still built and executed by the full command below.
smoke = ['pio', 'test', '-e', environment, '-f', suites[0], '--without-testing', '-v']
with (report / 'native-preflight.log').open('w') as log:
    preflight = subprocess.run(smoke, stdout=log, stderr=subprocess.STDOUT)
if preflight.returncode:
    print((report / 'native-preflight.log').read_text()[-18000:])
    raise SystemExit(preflight.returncode)
with (report / 'native.log').open('w') as log:
    result = subprocess.run(args, stdout=log, stderr=subprocess.STDOUT)
if result.returncode:
    # Preserve actionable failures, not only a long tail of SKIPPED environments.
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
    (report / 'native-failures.json').write_text(json.dumps(payload, indent=2) + '\n')
    print(json.dumps(payload, indent=2))
    print((report / 'native.log').read_text()[-4000:])
    raise SystemExit(result.returncode)
subprocess.run([sys.executable, 'bin/check-test-attribution.py', '--expect', ' '.join(suites), str(xml)], check=True)
by_name = {s.attrib['name'].split(':')[-1]: s for s in ET.parse(xml).getroot().iter('testsuite')
           if s.attrib['name'].startswith(environment + ':')}
counts = {}
for name in suites:
    suite = by_name[name]
    cases = list(suite.iter('testcase'))
    if not cases or any(c.find(x) is not None for c in cases for x in ('failure','error','skipped')):
        raise SystemExit(f'{name}: empty, failing or skipped test cases')
    counts[name] = len(cases)
summary = {'status': 'PASS', 'group': group, 'environment': environment, 'source_commit': subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
           'suites': counts, 'tests': sum(counts.values()), 'hardware_tested': False}
(report / 'native-summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary,indent=2))
