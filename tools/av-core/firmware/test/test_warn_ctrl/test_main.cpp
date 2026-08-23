#include <unity.h>

#include "warn_ctrl_logic.h"

using namespace avcore;

void setUp() {}
void tearDown() {}

void test_mapping_and_active_states() {
    WarnCtrlLogic panel;
    panel.receive_warning(0x03, 100);
    panel.receive_caution((1U << 0) | (1U << 8), 1U << (1 * 2), 100);
    const auto rendered = panel.render(100, 4095, false);
    TEST_ASSERT_EQUAL_UINT8(30, rendered.brightness_percent);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(LampTestTarget::None), static_cast<uint8_t>(rendered.test_target));
    TEST_ASSERT_EQUAL_UINT8(255, rendered.pixels[static_cast<size_t>(Lamp::MasterWarning)].r);
    TEST_ASSERT_EQUAL_UINT8(255, rendered.pixels[static_cast<size_t>(Lamp::Fire)].r);
    TEST_ASSERT_EQUAL_UINT8(255, rendered.pixels[static_cast<size_t>(Lamp::Missile)].r);
    TEST_ASSERT_EQUAL_UINT8(255, rendered.pixels[static_cast<size_t>(Lamp::MasterCaution)].r);
    TEST_ASSERT_EQUAL_UINT8(255, rendered.pixels[static_cast<size_t>(Lamp::Engine)].r);
    TEST_ASSERT_EQUAL_UINT8(255, rendered.pixels[static_cast<size_t>(Lamp::Subsystem)].r);
    TEST_ASSERT_EQUAL_UINT8(255, rendered.pixels[static_cast<size_t>(Lamp::FlightData)].r);
}

void test_web_and_physical_test_priority_and_expiration() {
    WarnCtrlLogic panel;
    panel.receive_warning(0, 0);
    panel.receive_caution(0, 0, 0);
    panel.start_web_test(LampTestTarget::Lamp, static_cast<uint8_t>(Lamp::Hull), 2000, 100);
    auto rendered = panel.render(101, 4095, false);
    TEST_ASSERT_EQUAL_UINT8(255, rendered.pixels[static_cast<size_t>(Lamp::Hull)].r);
    TEST_ASSERT_EQUAL_UINT8(0, rendered.pixels[static_cast<size_t>(Lamp::Fire)].r);
    rendered = panel.render(101, 4095, true);
    TEST_ASSERT_EQUAL_UINT8(255, rendered.pixels[static_cast<size_t>(Lamp::Fire)].r);
    rendered = panel.render(2100, 4095, false);
    TEST_ASSERT_EQUAL_UINT8(0, rendered.pixels[static_cast<size_t>(Lamp::Hull)].r);
}

void test_stale_remote_state_clears_panel_and_flashes_av_bus() {
    WarnCtrlLogic panel;
    panel.receive_warning(0x1F, 1);
    panel.receive_caution(0x1FF, 0, 1);
    const auto stale = panel.render(1001, 4095, false);
    TEST_ASSERT_EQUAL_UINT8(0, stale.pixels[static_cast<size_t>(Lamp::Fire)].r);
    TEST_ASSERT_EQUAL_UINT8(255, stale.pixels[static_cast<size_t>(Lamp::AvBus)].r);
    TEST_ASSERT_EQUAL_UINT8(255, stale.pixels[static_cast<size_t>(Lamp::MasterCaution)].r);
}

void test_potentiometer_is_capped_by_web_limit() {
    WarnCtrlLogic panel;
    panel.set_limits(40, 100, 400);
    TEST_ASSERT_EQUAL_UINT8(0, panel.render(0, 0, true).brightness_percent);
    TEST_ASSERT_EQUAL_UINT8(20, panel.render(0, 2048, true).brightness_percent);
    TEST_ASSERT_EQUAL_UINT8(40, panel.render(0, 4095, true).brightness_percent);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_mapping_and_active_states);
    RUN_TEST(test_web_and_physical_test_priority_and_expiration);
    RUN_TEST(test_stale_remote_state_clears_panel_and_flashes_av_bus);
    RUN_TEST(test_potentiometer_is_capped_by_web_limit);
    return UNITY_END();
}
