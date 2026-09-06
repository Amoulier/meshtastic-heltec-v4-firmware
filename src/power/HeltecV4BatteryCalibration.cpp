#include "HeltecV4BatteryCalibration.h"

#if defined(HELTEC_V4_OLED) && defined(ARDUINO_ARCH_ESP32)
#include <cmath>
#include <cstring>
#include <nvs.h>

namespace
{
constexpr const char *calibrationNamespace = "heltec_adc";
constexpr const char *calibrationKey = "calibration";
constexpr uint32_t calibrationMagic = 0x4d544143;
constexpr uint32_t calibrationVersion = 1;
constexpr size_t calibrationSize = 16;
static_assert(sizeof(float) == sizeof(uint32_t));

uint32_t readWord(const uint8_t *bytes)
{
    return uint32_t{bytes[0]} | (uint32_t{bytes[1]} << 8) | (uint32_t{bytes[2]} << 16) | (uint32_t{bytes[3]} << 24);
}

void writeWord(uint8_t *bytes, uint32_t value)
{
    for (size_t i = 0; i < sizeof(value); ++i)
        bytes[i] = static_cast<uint8_t>(value >> (i * 8));
}

uint32_t calibrationChecksum(const uint8_t *bytes, size_t size)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

bool validMultiplier(float multiplier)
{
    return std::isfinite(multiplier) && multiplier > 0;
}
} // namespace

HeltecV4BatteryCalibration readHeltecV4BatteryCalibration(float fallbackMultiplier)
{
    HeltecV4BatteryCalibration result{HeltecV4BatteryCalibrationStatus::UNAVAILABLE, fallbackMultiplier};
    nvs_handle_t handle;
    esp_err_t error = nvs_open(calibrationNamespace, NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        result.status = HeltecV4BatteryCalibrationStatus::MISSING;
        return result;
    }
    if (error != ESP_OK)
        return result;

    uint8_t bytes[calibrationSize];
    size_t size = 0;
    error = nvs_get_blob(handle, calibrationKey, nullptr, &size);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        result.status = HeltecV4BatteryCalibrationStatus::MISSING;
    } else if (error == ESP_OK && size != sizeof(bytes)) {
        result.status = HeltecV4BatteryCalibrationStatus::INVALID;
    } else if (error == ESP_OK) {
        error = nvs_get_blob(handle, calibrationKey, bytes, &size);
        if (error == ESP_OK) {
            result.status = HeltecV4BatteryCalibrationStatus::INVALID;
            if (size == sizeof(bytes) && readWord(bytes) == calibrationMagic && readWord(bytes + 4) == calibrationVersion &&
                readWord(bytes + 12) == calibrationChecksum(bytes, 12)) {
                const uint32_t bits = readWord(bytes + 8);
                float multiplier;
                memcpy(&multiplier, &bits, sizeof(multiplier));
                if (validMultiplier(multiplier)) {
                    result.status = HeltecV4BatteryCalibrationStatus::VALID;
                    result.multiplier = multiplier;
                }
            }
        }
    }
    if (error == ESP_ERR_NVS_TYPE_MISMATCH || error == ESP_ERR_NVS_INVALID_LENGTH)
        result.status = HeltecV4BatteryCalibrationStatus::INVALID;
    nvs_close(handle);
    return result;
}

bool writeHeltecV4BatteryCalibration(float resolvedMultiplier, bool (*powerIsSafe)())
{
    if (!validMultiplier(resolvedMultiplier) || !powerIsSafe)
        return false;
    const HeltecV4BatteryCalibration current = readHeltecV4BatteryCalibration(resolvedMultiplier);
    if (current.status == HeltecV4BatteryCalibrationStatus::VALID && current.multiplier == resolvedMultiplier)
        return true;
    if (!powerIsSafe())
        return false;

    uint8_t bytes[calibrationSize];
    uint32_t bits;
    memcpy(&bits, &resolvedMultiplier, sizeof(bits));
    writeWord(bytes, calibrationMagic);
    writeWord(bytes + 4, calibrationVersion);
    writeWord(bytes + 8, bits);
    writeWord(bytes + 12, calibrationChecksum(bytes, 12));

    nvs_handle_t handle;
    esp_err_t error = nvs_open(calibrationNamespace, NVS_READWRITE, &handle);
    if (error != ESP_OK)
        return false;
    if (powerIsSafe()) {
        error = nvs_set_blob(handle, calibrationKey, bytes, sizeof(bytes));
        if (error == ESP_OK)
            error = powerIsSafe() ? nvs_commit(handle) : ESP_ERR_INVALID_STATE;
    } else {
        error = ESP_ERR_INVALID_STATE;
    }
    nvs_close(handle);
    if (error != ESP_OK)
        return false;

    const HeltecV4BatteryCalibration persisted = readHeltecV4BatteryCalibration(resolvedMultiplier);
    return persisted.status == HeltecV4BatteryCalibrationStatus::VALID && persisted.multiplier == resolvedMultiplier;
}
#endif
