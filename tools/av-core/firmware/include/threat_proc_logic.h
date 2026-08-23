#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "av_can_protocol.h"

namespace avcore {

enum class ThreatLamp : uint8_t {
    Forward,
    ForwardRight,
    Right,
    AftRight,
    Aft,
    AftLeft,
    Left,
    ForwardLeft,
    Lock,
    Count,
};

constexpr size_t THREAT_PIXEL_COUNT = static_cast<size_t>(ThreatLamp::Count);
static_assert(THREAT_PIXEL_COUNT == THREAT_LAMP_COUNT, "THREAT PROC lamp map must match the CAN contract");

struct RenderedThreat {
    std::array<Rgb, THREAT_PIXEL_COUNT> pixels{};
    uint8_t brightness_percent = 0;
    bool lighting_degraded = true;
};

class ThreatProcLogic {
  public:
    void receive_threat(uint8_t sector_mask, uint8_t lock_state, uint32_t now_ms) {
        sector_mask_ = sector_mask;
        lock_state_ = lock_state <= 2 ? lock_state : 0;
        threat_seen_ = true;
        threat_at_ms_ = now_ms;
    }

    void receive_lighting(
        uint8_t brightness_percent,
        LampTestTarget test_target,
        uint8_t test_lamp,
        uint32_t now_ms) {
        brightness_percent_ = brightness_percent <= 100 ? brightness_percent : 100;
        test_target_ = test_target;
        test_lamp_ = test_lamp;
        lighting_seen_ = true;
        lighting_at_ms_ = now_ms;
    }

    void set_warning_color(Rgb color) { warning_color_ = color; }

    void set_rates(uint16_t slow_centihz, uint16_t fast_centihz) {
        if (slow_centihz >= 25 && fast_centihz > slow_centihz) {
            slow_centihz_ = slow_centihz;
            fast_centihz_ = fast_centihz;
        }
    }

    bool lighting_fresh(uint32_t now_ms) const {
        return lighting_seen_ && elapsed(now_ms, lighting_at_ms_) <= REMOTE_TIMEOUT_MS;
    }

    RenderedThreat render(uint32_t now_ms) const {
        RenderedThreat result;
        if (!lighting_fresh(now_ms)) {
            return result;
        }
        result.brightness_percent = brightness_percent_;
        result.lighting_degraded = false;

        if (test_target_ == LampTestTarget::All || test_target_ == LampTestTarget::ThreatProc) {
            result.pixels.fill(warning_color_);
            return result;
        }
        if (test_target_ == LampTestTarget::Lamp
            && test_lamp_ >= THREAT_LAMP_FIRST_ID
            && test_lamp_ < GLOBAL_LAMP_COUNT) {
            result.pixels[test_lamp_ - THREAT_LAMP_FIRST_ID] = warning_color_;
            return result;
        }

        const bool threat_fresh = threat_seen_ && elapsed(now_ms, threat_at_ms_) <= REMOTE_TIMEOUT_MS;
        if (!threat_fresh) {
            return result;
        }
        if (flash_on(now_ms, fast_centihz_)) {
            for (uint8_t sector = 0; sector < 8; ++sector) {
                if ((sector_mask_ & (1U << sector)) != 0) {
                    result.pixels[sector] = warning_color_;
                }
            }
        }
        const uint16_t lock_rate = lock_state_ == 1 ? slow_centihz_ : fast_centihz_;
        if (lock_state_ != 0 && flash_on(now_ms, lock_rate)) {
            result.pixels[static_cast<size_t>(ThreatLamp::Lock)] = warning_color_;
        }
        return result;
    }

    static constexpr uint32_t REMOTE_TIMEOUT_MS = 500;

  private:
    static constexpr uint32_t elapsed(uint32_t now, uint32_t then) { return now - then; }

    static bool flash_on(uint32_t now_ms, uint16_t centihz) {
        if (centihz == 0) {
            return true;
        }
        const uint32_t half_cycles = static_cast<uint32_t>((static_cast<uint64_t>(now_ms) * centihz * 2) / 100000);
        return (half_cycles & 1U) == 0;
    }

    uint8_t sector_mask_ = 0;
    uint8_t lock_state_ = 0;
    bool threat_seen_ = false;
    uint32_t threat_at_ms_ = 0;
    bool lighting_seen_ = false;
    uint32_t lighting_at_ms_ = 0;
    uint8_t brightness_percent_ = 0;
    LampTestTarget test_target_ = LampTestTarget::None;
    uint8_t test_lamp_ = 0xFF;
    Rgb warning_color_{255, 96, 0};
    uint16_t slow_centihz_ = 100;
    uint16_t fast_centihz_ = 400;
};

} // namespace avcore
