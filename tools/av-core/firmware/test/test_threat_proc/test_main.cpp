#include <unity.h>

#include "threat_proc_logic.h"

using namespace avcore;

void setUp() {}
void tearDown() {}

void test_sectors_and_acquired_lock_flash_fast() {
    ThreatProcLogic indicator;
    indicator.receive_lighting(30, LampTestTarget::None, 0xFF, 0);
    indicator.receive_threat(0x85, 2, 0);
    const auto on = indicator.render(0);
    TEST_ASSERT_EQUAL_UINT8(30, on.brightness_percent);
    TEST_ASSERT_EQUAL_UINT8(255, on.pixels[0].r);
    TEST_ASSERT_EQUAL_UINT8(255, on.pixels[2].r);
    TEST_ASSERT_EQUAL_UINT8(255, on.pixels[7].r);
    TEST_ASSERT_EQUAL_UINT8(255, on.pixels[8].r);
    const auto off = indicator.render(125);
    TEST_ASSERT_EQUAL_UINT8(0, off.pixels[0].r);
    TEST_ASSERT_EQUAL_UINT8(0, off.pixels[8].r);
}

void test_attempt_lock_uses_slow_rate() {
    ThreatProcLogic indicator;
    indicator.receive_lighting(30, LampTestTarget::None, 0xFF, 0);
    indicator.receive_threat(0, 1, 0);
    TEST_ASSERT_EQUAL_UINT8(255, indicator.render(125).pixels[8].r);
    TEST_ASSERT_EQUAL_UINT8(0, indicator.render(500).pixels[8].r);
}

void test_global_module_and_individual_tests() {
    ThreatProcLogic indicator;
    indicator.receive_threat(0, 0, 0);
    indicator.receive_lighting(20, LampTestTarget::ThreatProc, 0xFF, 0);
    auto rendered = indicator.render(0);
    for (const auto& pixel : rendered.pixels) {
        TEST_ASSERT_EQUAL_UINT8(255, pixel.r);
    }
    indicator.receive_lighting(20, LampTestTarget::Lamp, THREAT_LAMP_FIRST_ID + 8, 1);
    rendered = indicator.render(1);
    TEST_ASSERT_EQUAL_UINT8(255, rendered.pixels[8].r);
    TEST_ASSERT_EQUAL_UINT8(0, rendered.pixels[0].r);
}

void test_stale_inputs_clear_and_only_lighting_degrades() {
    ThreatProcLogic indicator;
    indicator.receive_lighting(30, LampTestTarget::None, 0xFF, 500);
    indicator.receive_threat(0xFF, 2, 0);
    const auto stale_threat = indicator.render(501);
    TEST_ASSERT_FALSE(stale_threat.lighting_degraded);
    TEST_ASSERT_EQUAL_UINT8(0, stale_threat.pixels[0].r);

    ThreatProcLogic stale_lighting;
    stale_lighting.receive_lighting(30, LampTestTarget::None, 0xFF, 0);
    stale_lighting.receive_threat(0xFF, 2, 500);
    const auto rendered = stale_lighting.render(501);
    TEST_ASSERT_TRUE(rendered.lighting_degraded);
    TEST_ASSERT_EQUAL_UINT8(0, rendered.brightness_percent);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_sectors_and_acquired_lock_flash_fast);
    RUN_TEST(test_attempt_lock_uses_slow_rate);
    RUN_TEST(test_global_module_and_individual_tests);
    RUN_TEST(test_stale_inputs_clear_and_only_lighting_degrades);
    return UNITY_END();
}
