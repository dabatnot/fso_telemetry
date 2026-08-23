#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <SPI.h>
#include <mcp2515.h>

#include "av_can_protocol.h"
#include "threat_proc_logic.h"

namespace {

constexpr uint8_t CAN_CS_PIN = 5;
constexpr uint8_t CAN_INT_PIN = 4;
constexpr uint8_t PIXEL_PIN = 13;
constexpr uint32_t HEARTBEAT_PERIOD_MS = 1000;

MCP2515 can_controller(CAN_CS_PIN);
Adafruit_NeoPixel pixels(avcore::THREAT_PIXEL_COUNT, PIXEL_PIN, NEO_GRB + NEO_KHZ800);
avcore::ThreatProcLogic indicator;
uint32_t next_heartbeat_ms = 0;
uint32_t next_reconnect_ms = 0;
bool can_online = false;

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
    if (frame.can_id == avcore::THREAT_STATE_ID) {
        if (frame.data[2] <= 2 && frame.data[3] == 0 && frame.data[4] == 0
            && frame.data[5] == 0 && frame.data[6] == 0 && frame.data[7] == 0) {
            indicator.receive_threat(frame.data[1], frame.data[2], now);
        }
    } else if (frame.can_id == avcore::LIGHTING_COMMAND_ID) {
        const auto opcode = static_cast<avcore::LightingOpcode>(frame.data[1]);
        if (opcode == avcore::LightingOpcode::WarningColor) {
            indicator.set_warning_color({frame.data[2], frame.data[3], frame.data[4]});
        } else if (opcode == avcore::LightingOpcode::Limits) {
            indicator.set_rates(read_u16(&frame.data[3]), read_u16(&frame.data[5]));
        }
    } else if (frame.can_id == avcore::LIGHTING_STATE_ID) {
        const uint8_t target = frame.data[2];
        const bool valid_target = target <= static_cast<uint8_t>(avcore::LampTestTarget::ThreatProc)
            || target == static_cast<uint8_t>(avcore::LampTestTarget::None);
        const bool valid_lamp = frame.data[3] == 0xFF || frame.data[3] < avcore::GLOBAL_LAMP_COUNT;
        if (frame.data[1] <= 100 && valid_target && valid_lamp && frame.data[4] <= 1) {
            indicator.receive_lighting(
                frame.data[1], static_cast<avcore::LampTestTarget>(target), frame.data[3], now);
        }
    }
}

void publish_heartbeat(uint32_t now) {
    const uint64_t efuse = ESP.getEfuseMac();
    const uint8_t data[8] = {
        avcore::PROTOCOL_VERSION,
        static_cast<uint8_t>(can_online && indicator.lighting_fresh(now) ? 0 : 1),
        0, 1, 0,
        static_cast<uint8_t>(efuse),
        static_cast<uint8_t>(efuse >> 8),
        static_cast<uint8_t>(efuse >> 16),
    };
    send_frame(avcore::THREAT_PROC_STATUS_ID, data);
}

void draw(const avcore::RenderedThreat& rendered) {
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
    draw(indicator.render(now));
    if (static_cast<int32_t>(now - next_heartbeat_ms) >= 0) {
        publish_heartbeat(now);
        next_heartbeat_ms = now + HEARTBEAT_PERIOD_MS;
    }
    delay(5);
}
