#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../release"
OTA_FILE=mt-esp32s3-ota.bin
OTA_URL=https://github.com/meshtastic/esp32-unified-ota/releases/download/v1.0.1/mt-esp32s3-ota.bin
OTA_SHA256=3e62c5451afda604bac372444f058fc689dd588a87772ad4be90c228e04c1995
curl --fail --show-error --location --retry 4 --retry-all-errors \
	--connect-timeout 20 --output "$OTA_FILE" "$OTA_URL"
echo "$OTA_SHA256  $OTA_FILE" | sha256sum --check --strict

test -s "$OTA_FILE"
test "$(stat -c%s "$OTA_FILE")" -eq 636544

OTA_MD5=$(md5sum "$OTA_FILE" | cut -d' ' -f1)
OTA_SIZE=$(stat -c%s "$OTA_FILE")

shopt -s nullglob
manifests=(firmware-*.mt.json)
if [[ ${#manifests[@]} -ne 1 ]]; then
	echo "Expected exactly one firmware manifest, found ${#manifests[@]}" >&2
	exit 1
fi

manifest="${manifests[0]}"
echo "Updating $manifest with $OTA_FILE (md5: $OTA_MD5, size: $OTA_SIZE)"
jq -e '.files | type == "array"' "$manifest" >/dev/null
# Replace any stale OTA entry so the manifest always matches the pinned payload.
jq --arg name "$OTA_FILE" --arg md5 "$OTA_MD5" --arg sha256 "$OTA_SHA256" --argjson bytes "$OTA_SIZE" --arg part "app1" \
	'.files = ([.files[] | select(.name != $name)] +
    [{"name": $name, "md5": $md5, "sha256": $sha256, "bytes": $bytes, "part_name": $part}])' \
	"$manifest" >"${manifest}.tmp"
mv "${manifest}.tmp" "$manifest"
