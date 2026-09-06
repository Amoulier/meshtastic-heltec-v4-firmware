#!/usr/bin/env bash

set -euo pipefail

PYTHON=${PYTHON:-}
ESPTOOL_PORT=${ESPTOOL_PORT:-}
CHANGE_MODE=false
FILENAME=""

FLASH_BAUD=115200
RESET_BAUD=1200
UPDATE_OFFSET=0x10000

show_help() {
    cat <<EOF
Usage: $(basename "$0") [-h] -p ESPTOOL_PORT [-P PYTHON] -f FILENAME [--change-mode]
Flash a normal Heltec V4 profile image while preserving device data.

    -h               Display this help and exit.
    -p ESPTOOL_PORT  Select the explicit local serial device (required).
    -P PYTHON        Python interpreter used for python -m esptool.
    -f FILENAME      Profile-specific normal firmware .bin file.
    --change-mode    Only request bootloader mode; do not flash.
EOF
}

require_option_value() {
    if [[ $# -lt 2 || -z "$2" ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
    -h | --help)
        show_help
        exit 0
        ;;
    -p | --port)
        require_option_value "$@"
        ESPTOOL_PORT=$2
        shift 2
        ;;
    -P)
        require_option_value "$@"
        PYTHON=$2
        shift 2
        ;;
    -f)
        require_option_value "$@"
        FILENAME=$2
        shift 2
        ;;
    --change-mode)
        CHANGE_MODE=true
        shift
        ;;
    --)
        shift
        if [[ $# -gt 1 || ($# -eq 1 && -n "$FILENAME") ]]; then
            echo "Error: unexpected arguments after --." >&2
            exit 1
        fi
        if [[ $# -eq 1 ]]; then
            FILENAME=$1
            shift
        fi
        break
        ;;
    -*)
        echo "Error: unknown argument: $1" >&2
        exit 1
        ;;
    *)
        if [[ -n "$FILENAME" ]]; then
            echo "Error: unexpected extra filename: $1" >&2
            exit 1
        fi
        FILENAME=$1
        shift
        ;;
    esac
done

if [[ -z "$ESPTOOL_PORT" ]]; then
    echo "Error: an explicit -p serial device is required; automatic port selection is disabled." >&2
    exit 1
fi
if [[ ! -c "$ESPTOOL_PORT" ]]; then
    echo "Error: serial port is not a character device: $ESPTOOL_PORT" >&2
    exit 1
fi

ESPTOOL_CMD=()
if [[ -n "$PYTHON" ]]; then
    if ! "$PYTHON" -m esptool version >/dev/null 2>&1; then
        echo "Error: $PYTHON cannot run the esptool module." >&2
        exit 1
    fi
    ESPTOOL_CMD=("$PYTHON" -m esptool)
else
    for candidate in python3 python; do
        if command -v "$candidate" >/dev/null 2>&1 && "$candidate" -m esptool version >/dev/null 2>&1; then
            ESPTOOL_CMD=("$candidate" -m esptool)
            break
        fi
    done
    if [[ ${#ESPTOOL_CMD[@]} -eq 0 ]]; then
        if command -v esptool >/dev/null 2>&1; then
            ESPTOOL_CMD=(esptool)
        elif command -v esptool.py >/dev/null 2>&1; then
            ESPTOOL_CMD=(esptool.py)
        else
            echo "Error: esptool not found." >&2
            exit 1
        fi
    fi
fi

if ! ESPTOOL_VERSION_OUTPUT=$("${ESPTOOL_CMD[@]}" version 2>&1); then
    echo "Error: unable to query esptool version." >&2
    exit 1
fi
if [[ ! $ESPTOOL_VERSION_OUTPUT =~ [Ee][Ss][Pp][Tt][Oo][Oo][Ll](\.py)?[[:space:]]+[vV]?([0-9]+)\.([0-9]+)(\.([0-9]+))? ]]; then
    echo "Error: unable to parse esptool version; esptool 4.5.1 or newer is required." >&2
    exit 1
fi
ESPTOOL_VERSION_MAJOR=$((10#${BASH_REMATCH[2]}))
ESPTOOL_VERSION_MINOR=$((10#${BASH_REMATCH[3]}))
ESPTOOL_VERSION_PATCH=$((10#${BASH_REMATCH[5]:-0}))
if ((ESPTOOL_VERSION_MAJOR < 4 ||
     (ESPTOOL_VERSION_MAJOR == 4 && ESPTOOL_VERSION_MINOR < 5) ||
     (ESPTOOL_VERSION_MAJOR == 4 && ESPTOOL_VERSION_MINOR == 5 && ESPTOOL_VERSION_PATCH < 1))); then
    echo "Error: esptool 4.5.1 or newer is required." >&2
    exit 1
fi

if ! ESPTOOL_HELP=$("${ESPTOOL_CMD[@]}" --help 2>&1); then
    echo "Error: unable to query esptool command syntax." >&2
    exit 1
fi
if grep --quiet write-flash <<<"$ESPTOOL_HELP"; then
    ESPTOOL_WRITE_FLASH=write-flash
    ESPTOOL_READ_FLASH_STATUS=read-flash-status
    ESPTOOL_CHIP_ID=chip-id
    ESPTOOL_FLASH_ID=flash-id
    ESPTOOL_NO_RESET=no-reset
else
    ESPTOOL_WRITE_FLASH=write_flash
    ESPTOOL_READ_FLASH_STATUS=read_flash_status
    ESPTOOL_CHIP_ID=chip_id
    ESPTOOL_FLASH_ID=flash_id
    ESPTOOL_NO_RESET=no_reset
fi

ESPTOOL_CMD+=(--port "$ESPTOOL_PORT")

if [[ $CHANGE_MODE == true ]]; then
    "${ESPTOOL_CMD[@]}" --baud "$RESET_BAUD" --chip esp32s3 --after "$ESPTOOL_NO_RESET" \
        "$ESPTOOL_READ_FLASH_STATUS"
    exit 0
fi

if [[ -z "$FILENAME" || ! -f "$FILENAME" || ! -s "$FILENAME" ]]; then
    echo "Error: update firmware is missing, unreadable, or empty: ${FILENAME:-<unset>}" >&2
    exit 1
fi

FIRMWARE_BASENAME=$(basename "$FILENAME")
case "$FIRMWARE_BASENAME" in
*.factory.bin)
    echo "Error: use device-install.sh for factory images." >&2
    exit 1
    ;;
firmware-heltec-v4-standard-*.bin | firmware-heltec-v4-solar-router-*.bin) ;;
*)
    echo "Error: expected a normal Heltec V4 profile firmware .bin file." >&2
    exit 1
    ;;
esac

FIRMWARE_DIR=$(cd "$(dirname "$FILENAME")" && pwd)
PROGNAME=${FIRMWARE_BASENAME%.bin}
METAFILE="$FIRMWARE_DIR/$PROGNAME.mt.json"
if [[ ! -s "$METAFILE" ]]; then
    echo "Error: required metadata is missing or empty: $METAFILE" >&2
    exit 1
fi
if ! command -v jq >/dev/null 2>&1; then
    echo "Error: jq is required to validate the release manifest." >&2
    exit 1
fi

TARGET=$(jq -er '.platformioTarget |
    select(. == "heltec-v4-standard" or . == "heltec-v4-solar-router")' "$METAFILE")
VERSION=$(jq -er '.version | select(type == "string" and test("^[0-9A-Za-z][0-9A-Za-z._-]*$"))' "$METAFILE")
EXPECTED_FIRMWARE_BASENAME="firmware-${TARGET}-${VERSION}.bin"
if [[ "$FIRMWARE_BASENAME" != "$EXPECTED_FIRMWARE_BASENAME" ]]; then
    echo "Error: firmware filename does not match the manifest target and version." >&2
    exit 1
fi
jq -e --arg target "$TARGET" --arg version "$VERSION" --arg name "$FIRMWARE_BASENAME" '
    .mcu == "esp32s3" and .hwModelSlug == "HELTEC_V4" and .platformioTarget == $target and .version == $version and
    .partitionScheme == "16MB" and .repo == "Amoulier/meshtastic-heltec-v4-firmware" and
    (.part | type == "array" and length == 6) and
    ([.part[].name] | unique | length == 6) and ([.part[].offset] | unique | length == 6) and
    ([.part[] | select(.subtype == "nvs" and .name == "nvs" and .type == "data" and
        .offset == "0x9000" and .size == "0x5000" and .flags == "")] | length == 1) and
    ([.part[] | select(.subtype == "ota" and .name == "otadata" and .type == "data" and
        .offset == "0xe000" and .size == "0x2000" and .flags == "")] | length == 1) and
    ([.part[] | select(.subtype == "ota_0" and .name == "app0" and .type == "app" and
        .offset == "0x10000" and .size == "0x640000" and .flags == "")] | length == 1) and
    ([.part[] | select(.subtype == "ota_1" and .name == "app1" and .type == "app" and
        .offset == "0x650000" and .size == "0x640000" and .flags == "")] | length == 1) and
    ([.part[] | select(.subtype == "spiffs" and .name == "spiffs" and .type == "data" and
        .offset == "0xc90000" and .size == "0x360000" and .flags == "")] | length == 1) and
    ([.part[] | select(.subtype == "coredump" and .name == "coredump" and .type == "data" and
        .offset == "0xFF0000" and .size == "0x10000" and .flags == "")] | length == 1) and
    ([.files[] | select(.name == $name and .part_name == "app0" and
        (.md5 | type) == "string" and (.md5 | test("^[0-9a-f]{32}$")) and
        (.bytes | type) == "number" and .bytes > 0 and (.bytes | floor) == .bytes)] | length == 1)' \
    "$METAFILE" >/dev/null

EXPECTED_MD5=$(jq -er --arg name "$FIRMWARE_BASENAME" '.files[] | select(.name == $name) | .md5' "$METAFILE")
EXPECTED_BYTES=$(jq -er --arg name "$FIRMWARE_BASENAME" '.files[] | select(.name == $name) | .bytes' "$METAFILE")
if stat -c '%s' "$FILENAME" >/dev/null 2>&1; then
    ACTUAL_BYTES=$(stat -c '%s' "$FILENAME")
else
    ACTUAL_BYTES=$(stat -f '%z' "$FILENAME")
fi
if command -v md5sum >/dev/null 2>&1; then
    ACTUAL_MD5=$(md5sum "$FILENAME" | awk '{print $1}')
elif command -v md5 >/dev/null 2>&1; then
    ACTUAL_MD5=$(md5 -q "$FILENAME")
elif command -v openssl >/dev/null 2>&1; then
    ACTUAL_MD5=$(openssl dgst -md5 "$FILENAME" | awk '{print $NF}')
else
    echo "Error: md5sum, md5, or openssl is required." >&2
    exit 1
fi
if [[ "$ACTUAL_BYTES" != "$EXPECTED_BYTES" || "$ACTUAL_MD5" != "$EXPECTED_MD5" || $ACTUAL_BYTES -gt $((0x640000)) ]]; then
    echo "Error: firmware size/MD5 is invalid or exceeds app0; refusing to flash." >&2
    exit 1
fi

verify_connected_device() {
    local flash_info
    echo "Verifying ESP32-S3 and 16MB flash on $ESPTOOL_PORT"
    "${ESPTOOL_CMD[@]}" --baud "$FLASH_BAUD" --chip esp32s3 "$ESPTOOL_CHIP_ID"
    if ! flash_info=$("${ESPTOOL_CMD[@]}" --baud "$FLASH_BAUD" --chip esp32s3 "$ESPTOOL_FLASH_ID" 2>&1); then
        printf '%s\n' "$flash_info" >&2
        echo "Error: unable to read flash identity; refusing to write." >&2
        exit 1
    fi
    printf '%s\n' "$flash_info"
    if ! grep -Eiq 'flash size:[[:space:]]*16[[:space:]]*MB' <<<"$flash_info"; then
        echo "Error: selected device does not report the required 16MB flash; refusing to write." >&2
        exit 1
    fi
}

echo "Validated $TARGET update; flashing $FIRMWARE_BASENAME at $UPDATE_OFFSET"
verify_connected_device
"${ESPTOOL_CMD[@]}" --baud "$FLASH_BAUD" --chip esp32s3 "$ESPTOOL_WRITE_FLASH" "$UPDATE_OFFSET" "$FILENAME"
echo "Heltec V4 update complete."
