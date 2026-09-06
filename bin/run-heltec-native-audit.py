#!/usr/bin/env python3
"""Run whole selected suites with per-suite isolation and reject false-green reports."""
from pathlib import Path
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
xml = report / 'native.xml'
xml.unlink(missing_ok=True)
args = ['pio', 'test', '-e', 'coverage', '-v', '--junit-output-path', str(xml)]
for suite in suites:
    args += ['-f', suite]
# Compile one complete suite first, so a broken host adapter fails once rather
# than repeating the same compilation error 25 times. This does not count as a
# test pass: every suite is still built and executed by the full command below.
smoke = ['pio', 'test', '-e', 'coverage', '-f', 'test_module_config', '--without-testing', '-v']
with (report / 'native-preflight.log').open('w') as log:
    preflight = subprocess.run(smoke, stdout=log, stderr=subprocess.STDOUT)
if preflight.returncode:
    print((report / 'native-preflight.log').read_text()[-18000:])
    raise SystemExit(preflight.returncode)
with (report / 'native.log').open('w') as log:
    result = subprocess.run(args, stdout=log, stderr=subprocess.STDOUT)
if result.returncode:
    print((report / 'native.log').read_text()[-14000:])
    raise SystemExit(result.returncode)
subprocess.run([sys.executable, 'bin/check-test-attribution.py', '--expect', ' '.join(suites), str(xml)], check=True)
by_name = {s.attrib['name'].split(':')[-1]: s for s in ET.parse(xml).getroot().iter('testsuite')}
counts = {}
for name in suites:
    suite = by_name[name]
    cases = list(suite.iter('testcase'))
    if not cases or any(c.find(x) is not None for c in cases for x in ('failure','error','skipped')):
        raise SystemExit(f'{name}: empty, failing or skipped test cases')
    counts[name] = len(cases)
summary = {'status': 'PASS', 'source_commit': subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
           'suites': counts, 'tests': sum(counts.values()), 'hardware_tested': False}
(report / 'native-summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary,indent=2))
