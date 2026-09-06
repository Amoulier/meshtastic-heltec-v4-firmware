#!/usr/bin/env bash

set -euo pipefail

PYTHON=${PYTHON:-}
ESPTOOL_PORT=${ESPTOOL_PORT:-}
BPS_RESET=false
FILENAME=""

RESET_BAUD=1200
FIRMWARE_OFFSET=0x00
EXPECTED_SPIFFS_OFFSET=0xc90000
EXPECTED_OTA_OFFSET=0x650000
EXPECTED_OTA_SHA256=3e62c5451afda604bac372444f058fc689dd588a87772ad4be90c228e04c1995

show_help() {
    cat <<EOF
Usage: $(basename "$0") [-h] -p ESPTOOL_PORT [-P PYTHON] -f FILENAME [--1200bps-reset]
Erase and install one complete Heltec V4 firmware bundle.

    -h               Display this help and exit.
    -p ESPTOOL_PORT  Select the explicit local serial device (required).
    -P PYTHON        Python interpreter used for python -m esptool.
    -f FILENAME      Profile-specific firmware-*.factory.bin from this bundle.
    --1200bps-reset  Only request bootloader mode; do not erase or flash.
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
    --1200bps-reset)
        BPS_RESET=true
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
    ESPTOOL_ERASE_FLASH=erase-flash
    ESPTOOL_READ_FLASH_STATUS=read-flash-status
    ESPTOOL_CHIP_ID=chip-id
    ESPTOOL_FLASH_ID=flash-id
    ESPTOOL_NO_RESET=no-reset
else
    ESPTOOL_WRITE_FLASH=write_flash
    ESPTOOL_ERASE_FLASH=erase_flash
    ESPTOOL_READ_FLASH_STATUS=read_flash_status
    ESPTOOL_CHIP_ID=chip_id
    ESPTOOL_FLASH_ID=flash_id
    ESPTOOL_NO_RESET=no_reset
fi

ESPTOOL_CMD+=(--port "$ESPTOOL_PORT")

if [[ $BPS_RESET == true ]]; then
    "${ESPTOOL_CMD[@]}" --baud "$RESET_BAUD" --chip esp32s3 --after "$ESPTOOL_NO_RESET" \
        "$ESPTOOL_READ_FLASH_STATUS"
    exit 0
fi

if [[ -z "$FILENAME" || ! -f "$FILENAME" || ! -s "$FILENAME" ]]; then
    echo "Error: factory firmware is missing, unreadable, or empty: ${FILENAME:-<unset>}" >&2
    exit 1
fi

FACTORY_BASENAME=$(basename "$FILENAME")
case "$FACTORY_BASENAME" in
firmware-heltec-v4-standard-*.factory.bin | firmware-heltec-v4-solar-router-*.factory.bin) ;;
*)
    echo "Error: expected a Heltec V4 profile firmware-*.factory.bin file." >&2
    exit 1
    ;;
esac

FIRMWARE_DIR=$(cd "$(dirname "$FILENAME")" && pwd)
PROGNAME=${FACTORY_BASENAME%.factory.bin}
METAFILE="$FIRMWARE_DIR/$PROGNAME.mt.json"
if [[ ! -s "$METAFILE" ]]; then
    echo "Error: required metadata is missing or empty: $METAFILE" >&2
    exit 1
fi
if ! command -v jq >/dev/null 2>&1; then
    echo "Error: jq is required to validate the release manifest." >&2
    exit 1
fi

MCU=$(jq -er '.mcu | select(. == "esp32s3")' "$METAFILE")
TARGET=$(jq -er '.platformioTarget |
    select(. == "heltec-v4-standard" or . == "heltec-v4-solar-router")' "$METAFILE")
VERSION=$(jq -er '.version | select(type == "string" and test("^[0-9A-Za-z][0-9A-Za-z._-]*$"))' "$METAFILE")
EXPECTED_FACTORY_BASENAME="firmware-${TARGET}-${VERSION}.factory.bin"
if [[ "$FACTORY_BASENAME" != "$EXPECTED_FACTORY_BASENAME" ]]; then
    echo "Error: factory filename does not match the manifest target and version." >&2
    exit 1
fi
jq -e --arg target "$TARGET" --arg version "$VERSION" '
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
    (.files | type == "array" and length == 5) and ([.files[].name] | unique | length == 5)' "$METAFILE" >/dev/null

OTAFILE="$FIRMWARE_DIR/mt-${MCU}-ota.bin"
SPIFFSFILE="$FIRMWARE_DIR/littlefs-${PROGNAME#firmware-}.bin"

file_size() {
    if stat -c '%s' "$1" >/dev/null 2>&1; then
        stat -c '%s' "$1"
    else
        stat -f '%z' "$1"
    fi
}

file_md5() {
    if command -v md5sum >/dev/null 2>&1; then
        md5sum "$1" | awk '{print $1}'
    elif command -v md5 >/dev/null 2>&1; then
        md5 -q "$1"
    elif command -v openssl >/dev/null 2>&1; then
        openssl dgst -md5 "$1" | awk '{print $NF}'
    else
        echo "Error: md5sum, md5, or openssl is required." >&2
        exit 1
    fi
}

file_sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    elif command -v openssl >/dev/null 2>&1; then
        openssl dgst -sha256 "$1" | awk '{print $NF}'
    else
        echo "Error: sha256sum, shasum, or openssl is required." >&2
        exit 1
    fi
}

verify_manifest_file() {
    local path=$1
    local expected_name=$2
    local expected_part=$3
    local maximum_bytes=$4
    local row expected_md5 expected_bytes actual_bytes
    if [[ ! -s "$path" || "$(basename "$path")" != "$expected_name" ]]; then
        echo "Error: required bundle file is missing or empty: $path" >&2
        exit 1
    fi
    row=$(jq -er --arg name "$expected_name" --arg part "$expected_part" '
        [.files[] | select(.name == $name)] as $matches |
        if ($matches | length) != 1 then error("manifest filename is absent or duplicated")
        else $matches[0] |
            select((.md5 | type) == "string" and (.md5 | test("^[0-9a-f]{32}$")) and
                   (.bytes | type) == "number" and .bytes > 0 and (.bytes | floor) == .bytes) |
            if $part == "" then select(has("part_name") | not)
            else select(.part_name == $part) end |
            [.md5, (.bytes | tostring)] | @tsv
        end' "$METAFILE")
    IFS=$'\t' read -r expected_md5 expected_bytes <<<"$row"
    actual_bytes=$(file_size "$path")
    if [[ "$actual_bytes" != "$expected_bytes" || "$(file_md5 "$path")" != "$expected_md5" ]]; then
        echo "Error: size or MD5 mismatch for $path; refusing to erase flash." >&2
        exit 1
    fi
    if ((actual_bytes > maximum_bytes)); then
        echo "Error: $path exceeds its physical flash partition; refusing to erase flash." >&2
        exit 1
    fi
}

verify_manifest_file "$FIRMWARE_DIR/$FACTORY_BASENAME" "$FACTORY_BASENAME" "" "$((0x650000))"
verify_manifest_file "$OTAFILE" "$(basename "$OTAFILE")" app1 "$((0x640000))"
verify_manifest_file "$SPIFFSFILE" "$(basename "$SPIFFSFILE")" spiffs "$((0x360000))"
if [[ "$(file_sha256 "$OTAFILE")" != "$EXPECTED_OTA_SHA256" ]]; then
    echo "Error: unified OTA loader does not match the pinned release payload; refusing to erase flash." >&2
    exit 1
fi

verify_connected_device() {
    local flash_info
    echo "Verifying ESP32-S3 and 16MB flash on $ESPTOOL_PORT"
    "${ESPTOOL_CMD[@]}" --baud 115200 --chip esp32s3 "$ESPTOOL_CHIP_ID"
    if ! flash_info=$("${ESPTOOL_CMD[@]}" --baud 115200 --chip esp32s3 "$ESPTOOL_FLASH_ID" 2>&1); then
        printf '%s\n' "$flash_info" >&2
        echo "Error: unable to read flash identity; refusing to erase." >&2
        exit 1
    fi
    printf '%s\n' "$flash_info"
    if ! grep -Eiq 'flash size:[[:space:]]*16[[:space:]]*MB' <<<"$flash_info"; then
        echo "Error: selected device does not report the required 16MB flash; refusing to erase." >&2
        exit 1
    fi
}

echo "Validated $TARGET complete bundle; erasing and installing $FACTORY_BASENAME"
verify_connected_device
"${ESPTOOL_CMD[@]}" --baud 115200 --chip esp32s3 "$ESPTOOL_ERASE_FLASH"
"${ESPTOOL_CMD[@]}" --baud 115200 --chip esp32s3 "$ESPTOOL_WRITE_FLASH" \
    "$FIRMWARE_OFFSET" "$FIRMWARE_DIR/$FACTORY_BASENAME" \
    "$EXPECTED_OTA_OFFSET" "$OTAFILE" \
    "$EXPECTED_SPIFFS_OFFSET" "$SPIFFSFILE"

echo "Heltec V4 installation complete."
