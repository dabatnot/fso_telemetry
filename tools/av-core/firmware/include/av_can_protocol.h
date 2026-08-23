#pragma once

#include <cstdint>

namespace avcore {

constexpr uint16_t WARNING_STATE_ID = 0x180;
constexpr uint16_t CAUTION_STATE_ID = 0x181;
constexpr uint16_t LIGHTING_COMMAND_ID = 0x182;
constexpr uint16_t LIGHTING_STATE_ID = 0x183;
constexpr uint16_t THREAT_STATE_ID = 0x184;
constexpr uint16_t WARN_CTRL_STATUS_ID = 0x700;
constexpr uint8_t PROTOCOL_VERSION = 1;

enum class LightingOpcode : uint8_t {
    WarningColor = 1,
    CautionColor = 2,
    Limits = 3,
    StartTest = 4,
};

enum class LampTestTarget : uint8_t {
    All = 0,
    WarnCtrl = 1,
    Lamp = 2,
    None = 0xFF,
};

} // namespace avcore
