#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <SPI.h>
#include <mcp2515.h>

#include "av_can_protocol.h"
#include "warn_ctrl_logic.h"

namespace {

constexpr uint8_t CAN_CS_PIN = 5;
constexpr uint8_t CAN_INT_PIN = 4;
constexpr uint8_t PIXEL_PIN = 13;
constexpr uint8_t BRT_PIN = 34;
constexpr uint8_t LAMP_TEST_PIN = 27;
constexpr uint32_t LIGHTING_PERIOD_MS = 100;
constexpr uint32_t HEARTBEAT_PERIOD_MS = 1000;

MCP2515 can_controller(CAN_CS_PIN);
Adafruit_NeoPixel pixels(avcore::LAMP_COUNT, PIXEL_PIN, NEO_GRB + NEO_KHZ800);
avcore::WarnCtrlLogic panel;
uint32_t next_lighting_ms = 0;
uint32_t next_heartbeat_ms = 0;
uint32_t next_reconnect_ms = 0;
bool can_online = false;

bool physical_lamp_test(uint32_t now) {
    static bool candidate = false;
    static bool stable = false;
    static uint32_t candidate_since = 0;
    const bool raw = digitalRead(LAMP_TEST_PIN) == LOW;
    if (raw != candidate) {
        candidate = raw;
        candidate_since = now;
    } else if (now - candidate_since >= 25) {
        stable = candidate;
    }
    return stable;
}

uint16_t brightness_sample() {
    uint32_t total = 0;
    for (uint8_t sample = 0; sample < 8; ++sample) {
        total += analogRead(BRT_PIN);
    }
    return static_cast<uint16_t>((total + 4) / 8);
}

uint16_t read_u16(const uint8_t* bytes) {
    return static_cast<uint16_t>(bytes[0] | (static_cast<uint16_t>(bytes[1]) << 8));
}

bool configure_can() {
    can_controller.reset();
    if (can_controller.setBitrate(CAN_1000KBPS, MCP_8MHZ) != MCP2515::ERROR_OK) {
        return false;
    }
    return can_controller.setNormalMode() == MCP2515::ERROR_OK;
}

bool send_frame(uint16_t id, const uint8_t data[8]) {
    can_frame frame{};
    frame.can_id = id;
    frame.can_dlc = 8;
    memcpy(frame.data, data, 8);
    const bool sent = can_controller.sendMessage(&frame) == MCP2515::ERROR_OK;
    if (!sent) {
        can_online = false;
    }
    return sent;
}

void receive_frame(const can_frame& frame, uint32_t now) {
    if (frame.can_dlc != 8 || frame.data[0] != avcore::PROTOCOL_VERSION) {
        return;
    }
    if (frame.can_id == avcore::WARNING_STATE_ID && (frame.data[1] & ~0x1F) == 0) {
        panel.receive_warning(frame.data[1], now);
    } else if (frame.can_id == avcore::CAUTION_STATE_ID) {
        const uint16_t cockpit = read_u16(&frame.data[1]);
        const uint16_t diagnostics = read_u16(&frame.data[3]);
        if ((cockpit & ~0x01FF) == 0 && (diagnostics & ~0x3FFF) == 0) {
            panel.receive_caution(cockpit, diagnostics, now);
        }
    } else if (frame.can_id == avcore::LIGHTING_COMMAND_ID) {
        const auto opcode = static_cast<avcore::LightingOpcode>(frame.data[1]);
        if (opcode == avcore::LightingOpcode::WarningColor) {
            panel.set_warning_color({frame.data[2], frame.data[3], frame.data[4]});
        } else if (opcode == avcore::LightingOpcode::CautionColor) {
            panel.set_caution_color({frame.data[2], frame.data[3], frame.data[4]});
        } else if (opcode == avcore::LightingOpcode::Limits) {
            panel.set_limits(frame.data[2], read_u16(&frame.data[3]), read_u16(&frame.data[5]));
        } else if (opcode == avcore::LightingOpcode::StartTest && frame.data[2] <= 3) {
            panel.start_web_test(
                static_cast<avcore::LampTestTarget>(frame.data[2]),
                frame.data[3],
                read_u16(&frame.data[4]),
                now);
        }
    }
}

void publish_lighting(const avcore::RenderedPanel& rendered) {
    const uint8_t data[8] = {
        avcore::PROTOCOL_VERSION,
        rendered.brightness_percent,
        static_cast<uint8_t>(rendered.test_target),
        rendered.test_lamp,
        static_cast<uint8_t>(rendered.physical_test_active),
        0, 0, 0,
    };
    send_frame(avcore::LIGHTING_STATE_ID, data);
}

void publish_heartbeat() {
    const uint64_t efuse = ESP.getEfuseMac();
    const uint8_t data[8] = {
        avcore::PROTOCOL_VERSION,
        static_cast<uint8_t>(can_online ? 0 : 1),
        0, 4, 0,
        static_cast<uint8_t>(efuse),
        static_cast<uint8_t>(efuse >> 8),
        static_cast<uint8_t>(efuse >> 16),
    };
    send_frame(avcore::WARN_CTRL_STATUS_ID, data);
}

void draw(const avcore::RenderedPanel& rendered) {
    pixels.setBrightness(static_cast<uint8_t>((rendered.brightness_percent * 255U + 50U) / 100U));
    for (size_t i = 0; i < rendered.pixels.size(); ++i) {
        const auto color = rendered.pixels[i];
        pixels.setPixelColor(i, pixels.Color(color.r, color.g, color.b));
    }
    pixels.show();
}

} // namespace

void setup() {
    pinMode(CAN_INT_PIN, INPUT);
    pinMode(LAMP_TEST_PIN, INPUT_PULLUP);
    analogReadResolution(12);
    SPI.begin(18, 19, 23, CAN_CS_PIN);
    pixels.begin();
    pixels.clear();
    pixels.show();
    can_online = configure_can();
}

void loop() {
    const uint32_t now = millis();
    if (!can_online && static_cast<int32_t>(now - next_reconnect_ms) >= 0) {
        can_online = configure_can();
        next_reconnect_ms = now + 1000;
    }
    if (can_online) {
        can_frame frame{};
        while (digitalRead(CAN_INT_PIN) == LOW && can_controller.readMessage(&frame) == MCP2515::ERROR_OK) {
            receive_frame(frame, now);
        }
    }
    const bool physical_test = physical_lamp_test(now);
    const auto rendered = panel.render(now, brightness_sample(), physical_test);
    draw(rendered);
    if (static_cast<int32_t>(now - next_lighting_ms) >= 0) {
        publish_lighting(rendered);
        next_lighting_ms = now + LIGHTING_PERIOD_MS;
    }
    if (static_cast<int32_t>(now - next_heartbeat_ms) >= 0) {
        publish_heartbeat();
        next_heartbeat_ms = now + HEARTBEAT_PERIOD_MS;
    }
    delay(5);
}
