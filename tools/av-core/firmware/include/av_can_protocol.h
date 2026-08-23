#pragma once

#include <cstdint>

namespace avcore {

constexpr uint16_t WARNING_STATE_ID = 0x180;
constexpr uint16_t CAUTION_STATE_ID = 0x181;
constexpr uint16_t LIGHTING_COMMAND_ID = 0x182;
constexpr uint16_t LIGHTING_STATE_ID = 0x183;
constexpr uint16_t THREAT_STATE_ID = 0x184;
constexpr uint16_t WARN_CTRL_STATUS_ID = 0x700;
constexpr uint16_t THREAT_PROC_STATUS_ID = 0x701;
constexpr uint8_t PROTOCOL_VERSION = 1;
constexpr uint8_t WARN_CTRL_LAMP_COUNT = 23;
constexpr uint8_t THREAT_LAMP_FIRST_ID = WARN_CTRL_LAMP_COUNT;
constexpr uint8_t THREAT_LAMP_COUNT = 9;
constexpr uint8_t GLOBAL_LAMP_COUNT = WARN_CTRL_LAMP_COUNT + THREAT_LAMP_COUNT;

struct Rgb {
    constexpr Rgb(uint8_t red = 0, uint8_t green = 0, uint8_t blue = 0) : r(red), g(green), b(blue) {}

    uint8_t r;
    uint8_t g;
    uint8_t b;
};

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
    ThreatProc = 3,
    None = 0xFF,
};

} // namespace avcore
