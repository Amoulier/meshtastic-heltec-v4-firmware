#pragma once

#include <cstdint>

enum class ChannelScanAction : uint8_t { TRANSMIT, DEFER, RECOVER_AND_RETRY };

constexpr ChannelScanAction channelScanAction(bool channelDetected, bool channelFree, bool recoveryAttempted)
{
    if (channelDetected) {
        return ChannelScanAction::DEFER;
    }
    if (channelFree) {
        return ChannelScanAction::TRANSMIT;
    }
    return recoveryAttempted ? ChannelScanAction::DEFER : ChannelScanAction::RECOVER_AND_RETRY;
}
