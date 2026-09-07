#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

python3 test/host/version_identity_test.py
python3 test/host/ci_environment_test.py
python3 test/host/battery_calibration_regression.py
python3 test/host/battery_adc_regression.py
python3 test/host/battery_pipeline_regression.py
python3 test/host/heltec_release_policy_test.py
python3 test/host/heltec_audit_regression.py
bash -n bin/device-install.sh bin/device-update.sh bin/heltec-ci.sh bin/test-native-docker.sh

scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
for profile in generic standard solar-router; do
	flags=()
	if [[ $profile != generic ]]; then flags+=(-DHELTEC_V4_OLED=1); fi
	if [[ $profile == solar-router ]]; then flags+=(-DHELTEC_V4_SOLAR_ROUTER_PROFILE=1); fi
	g++ -std=c++17 -Wall -Wextra -Werror -pedantic -Isrc "${flags[@]}" \
		test/host/deep_sleep_policy_test.cpp -o "$scratch/$profile"
	"$scratch/$profile"
done
git ls-files --cached --others --exclude-standard -z '*.cpp' '*.h' >"$scratch/sources"
sources=()
while IFS= read -r -d '' path; do
	if [[ -f $path ]]; then sources+=("$path"); fi
done <"$scratch/sources"
test "${#sources[@]}" -gt 0
bash bin/lint-ifdef-complexity.sh "${sources[@]}"
bash bin/lint-node-id-format.sh "${sources[@]}"
bash bin/lint-unity-exit.sh test/host/deep_sleep_policy_test.cpp
bash bin/test-lint-unity-exit.sh
echo 'Host checks: PASS'
