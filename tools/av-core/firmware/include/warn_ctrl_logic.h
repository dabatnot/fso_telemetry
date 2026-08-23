#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "av_can_protocol.h"

namespace avcore {

enum class Lamp : uint8_t {
    MasterWarning,
    Fire,
    Missile,
    Blast,
    Collision,
    Emp,
    MasterCaution,
    Engine,
    Sensors,
    Shield,
    Hull,
    WeaponEnergy,
    AfterburnerFuel,
    Ammo,
    Countermeasures,
    Subsystem,
    AvCore,
    FlightData,
    AvBus,
    SensProc,
    ThreatProc,
    InstProc,
    WarnCtrl,
    Count,
};

constexpr size_t LAMP_COUNT = static_cast<size_t>(Lamp::Count);
static_assert(LAMP_COUNT == 23);

struct Rgb {
    constexpr Rgb(uint8_t red = 0, uint8_t green = 0, uint8_t blue = 0) : r(red), g(green), b(blue) {}

    uint8_t r;
    uint8_t g;
    uint8_t b;

};

struct RenderedPanel {
    std::array<Rgb, LAMP_COUNT> pixels{};
    uint8_t brightness_percent = 0;
    LampTestTarget test_target = LampTestTarget::None;
    uint8_t test_lamp = 0xFF;
    bool physical_test_active = false;
};

class WarnCtrlLogic {
  public:
    void receive_warning(uint8_t mask, uint32_t now_ms) {
        warning_mask_ = mask & 0x1F;
        warning_seen_ = true;
        warning_at_ms_ = now_ms;
    }

    void receive_caution(uint16_t cockpit_mask, uint16_t diagnostic_mask, uint32_t now_ms) {
        cockpit_mask_ = cockpit_mask & 0x01FF;
        diagnostic_mask_ = diagnostic_mask & 0x3FFF;
        caution_seen_ = true;
        caution_at_ms_ = now_ms;
    }

    void set_warning_color(Rgb color) { warning_color_ = color; }
    void set_caution_color(Rgb color) { caution_color_ = color; }

    void set_limits(uint8_t max_brightness_percent, uint16_t slow_centihz, uint16_t fast_centihz) {
        max_brightness_percent_ = max_brightness_percent <= 100 ? max_brightness_percent : 100;
        if (slow_centihz >= 25 && fast_centihz > slow_centihz) {
            slow_centihz_ = slow_centihz;
            fast_centihz_ = fast_centihz;
        }
    }

    void start_web_test(LampTestTarget target, uint8_t lamp, uint16_t duration_ms, uint32_t now_ms) {
        if (target == LampTestTarget::Lamp && lamp >= LAMP_COUNT) {
            return;
        }
        web_test_target_ = target;
        web_test_lamp_ = target == LampTestTarget::Lamp ? lamp : 0xFF;
        web_test_until_ms_ = now_ms + duration_ms;
    }

    RenderedPanel render(uint32_t now_ms, uint16_t potentiometer, bool physical_test) const {
        RenderedPanel result;
        const uint32_t bounded_pot = potentiometer > 4095 ? 4095 : potentiometer;
        result.brightness_percent = static_cast<uint8_t>((bounded_pot * max_brightness_percent_ + 2047) / 4095);
        result.physical_test_active = physical_test;

        if (physical_test) {
            result.test_target = LampTestTarget::All;
            fill_test(result.pixels);
            return result;
        }
        if (deadline_active(now_ms, web_test_until_ms_)) {
            result.test_target = web_test_target_;
            result.test_lamp = web_test_lamp_;
            if (web_test_target_ == LampTestTarget::Lamp) {
                set_test_pixel(result.pixels, web_test_lamp_);
            } else {
                fill_test(result.pixels);
            }
            return result;
        }

        const bool fresh = warning_seen_ && caution_seen_
            && elapsed(now_ms, warning_at_ms_) <= REMOTE_TIMEOUT_MS
            && elapsed(now_ms, caution_at_ms_) <= REMOTE_TIMEOUT_MS;
        if (!fresh) {
            const bool bus_visible = flash_on(now_ms, slow_centihz_);
            if (bus_visible) {
                result.pixels[index(Lamp::AvBus)] = caution_color_;
            }
            result.pixels[index(Lamp::MasterCaution)] = caution_color_;
            return result;
        }

        const bool fast_visible = flash_on(now_ms, fast_centihz_);
        if (warning_mask_ != 0 && fast_visible) {
            result.pixels[index(Lamp::MasterWarning)] = warning_color_;
        }
        for (uint8_t bit = 0; bit < 5; ++bit) {
            if ((warning_mask_ & (1U << bit)) != 0 && fast_visible) {
                result.pixels[index(static_cast<Lamp>(index(Lamp::Fire) + bit))] = warning_color_;
            }
        }

        bool master_caution = cockpit_mask_ != 0;
        for (uint8_t bit = 0; bit < 9; ++bit) {
            if ((cockpit_mask_ & (1U << bit)) != 0) {
                result.pixels[index(static_cast<Lamp>(index(Lamp::Engine) + bit))] = caution_color_;
            }
        }
        for (uint8_t bit = 0; bit < 7; ++bit) {
            const uint8_t state = static_cast<uint8_t>((diagnostic_mask_ >> (bit * 2)) & 0x03);
            const bool visible = state == 1 || (state == 2 && flash_on(now_ms, slow_centihz_));
            if (state == 1 || state == 2) {
                master_caution = true;
            }
            if (visible) {
                result.pixels[index(static_cast<Lamp>(index(Lamp::AvCore) + bit))] = caution_color_;
            }
        }
        if (master_caution) {
            result.pixels[index(Lamp::MasterCaution)] = caution_color_;
        }
        return result;
    }

    static constexpr uint32_t REMOTE_TIMEOUT_MS = 500;

  private:
    static constexpr size_t index(Lamp lamp) { return static_cast<size_t>(lamp); }
    static constexpr uint32_t elapsed(uint32_t now, uint32_t then) { return now - then; }
    static constexpr bool deadline_active(uint32_t now, uint32_t deadline) {
        return deadline != 0 && static_cast<int32_t>(deadline - now) > 0;
    }

    static bool flash_on(uint32_t now_ms, uint16_t centihz) {
        if (centihz == 0) {
            return true;
        }
        const uint32_t half_cycles = static_cast<uint32_t>((static_cast<uint64_t>(now_ms) * centihz * 2) / 100000);
        return (half_cycles & 1U) == 0;
    }

    void fill_test(std::array<Rgb, LAMP_COUNT>& pixels) const {
        for (size_t i = 0; i < pixels.size(); ++i) {
            pixels[i] = i <= index(Lamp::Emp) ? warning_color_ : caution_color_;
        }
    }

    void set_test_pixel(std::array<Rgb, LAMP_COUNT>& pixels, uint8_t lamp) const {
        pixels[lamp] = lamp <= index(Lamp::Emp) ? warning_color_ : caution_color_;
    }

    uint8_t warning_mask_ = 0;
    uint16_t cockpit_mask_ = 0;
    uint16_t diagnostic_mask_ = 0;
    bool warning_seen_ = false;
    bool caution_seen_ = false;
    uint32_t warning_at_ms_ = 0;
    uint32_t caution_at_ms_ = 0;
    Rgb warning_color_{255, 96, 0};
    Rgb caution_color_{255, 96, 0};
    uint8_t max_brightness_percent_ = 30;
    uint16_t slow_centihz_ = 100;
    uint16_t fast_centihz_ = 400;
    LampTestTarget web_test_target_ = LampTestTarget::All;
    uint8_t web_test_lamp_ = 0xFF;
    uint32_t web_test_until_ms_ = 0;
};

} // namespace avcore
