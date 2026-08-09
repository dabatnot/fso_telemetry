#include "events/events.h"
#include "telemetry/runtime.h"
#include "telemetry/telemetry.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace telemetry::test_seam {

void reset_observation_counts() noexcept;
std::uint64_t registration_attempts(std::size_t event_index) noexcept;
std::uint64_t callback_invocations(std::size_t event_index) noexcept;
void reset_runtime_adapter_observations() noexcept;
std::uint64_t runtime_adapter_factory_requests() noexcept;
std::uint64_t runtime_adapter_main_thread_captures() noexcept;
std::uint64_t runtime_adapter_main_thread_checks() noexcept;
std::uint64_t runtime_adapter_config_loads() noexcept;
std::uint64_t runtime_adapter_post_config_calls() noexcept;
std::uint64_t runtime_adapter_diagnostic_calls() noexcept;
std::uint64_t runtime_adapter_lifecycle_calls() noexcept;
detail::RuntimeState runtime_state() noexcept;
detail::RuntimeTerminalReason runtime_terminal_reason() noexcept;

} // namespace telemetry::test_seam

namespace {

constexpr std::size_t EngineUpdateIndex = 0;
constexpr std::size_t EngineShutdownIndex = 1;
constexpr std::size_t GameMissionLoadIndex = 2;
constexpr std::size_t GameEnterStateIndex = 3;
constexpr std::size_t GameLeaveStateIndex = 4;
constexpr std::size_t EventCount = 5;
constexpr std::size_t RepeatedInitializeCount = 8;

using Counts = std::array<std::uint64_t, EventCount>;

Counts registration_counts()
{
	return {telemetry::test_seam::registration_attempts(EngineUpdateIndex),
		telemetry::test_seam::registration_attempts(EngineShutdownIndex),
		telemetry::test_seam::registration_attempts(GameMissionLoadIndex),
		telemetry::test_seam::registration_attempts(GameEnterStateIndex),
		telemetry::test_seam::registration_attempts(GameLeaveStateIndex)};
}

Counts invocation_counts()
{
	return {telemetry::test_seam::callback_invocations(EngineUpdateIndex),
		telemetry::test_seam::callback_invocations(EngineShutdownIndex),
		telemetry::test_seam::callback_invocations(GameMissionLoadIndex),
		telemetry::test_seam::callback_invocations(GameEnterStateIndex),
		telemetry::test_seam::callback_invocations(GameLeaveStateIndex)};
}

void emit_each_event_once()
{
	events::EngineUpdate();
	events::EngineShutdown();
	events::GameMissionLoad("test-mission.fs2");
	events::GameEnterState(1, 2);
	events::GameLeaveState(2, 1);
}

TEST(TelemetryProducerInitializeInstrumented, RegistersExactlyOnceAndRepeatedInitializeDoesNotDuplicateCallbacks)
{
	telemetry::test_seam::reset_observation_counts();
	telemetry::test_seam::reset_runtime_adapter_observations();

	telemetry::initialize();
	telemetry::initialize();

	const Counts one_registration_each{1, 1, 1, 1, 1};
	EXPECT_EQ(one_registration_each, registration_counts());
	EXPECT_EQ(1U, telemetry::test_seam::runtime_adapter_factory_requests());
	EXPECT_EQ(1U, telemetry::test_seam::runtime_adapter_main_thread_captures());
	EXPECT_EQ(0U, telemetry::test_seam::runtime_adapter_main_thread_checks());
	EXPECT_EQ(0U, telemetry::test_seam::runtime_adapter_config_loads());
	EXPECT_EQ(0U, telemetry::test_seam::runtime_adapter_post_config_calls());
	EXPECT_EQ(0U, telemetry::test_seam::runtime_adapter_diagnostic_calls());
	EXPECT_EQ(0U, telemetry::test_seam::runtime_adapter_lifecycle_calls());
	EXPECT_EQ(telemetry::detail::RuntimeState::Cold, telemetry::test_seam::runtime_state());
	EXPECT_EQ(telemetry::detail::RuntimeTerminalReason::None,
		telemetry::test_seam::runtime_terminal_reason());

	for (std::size_t i = 0; i < RepeatedInitializeCount; ++i) {
		telemetry::initialize();
	}

	EXPECT_EQ(one_registration_each, registration_counts());
	EXPECT_EQ(1U, telemetry::test_seam::runtime_adapter_factory_requests());
	EXPECT_EQ(1U, telemetry::test_seam::runtime_adapter_main_thread_captures());
	EXPECT_EQ(0U, telemetry::test_seam::registration_attempts(EventCount));
	EXPECT_EQ(0U, telemetry::test_seam::callback_invocations(EventCount));

	events::EngineUpdate();

	const Counts first_update_only{1, 0, 0, 0, 0};
	EXPECT_EQ(first_update_only, invocation_counts());
	EXPECT_EQ(telemetry::detail::RuntimeState::Disabled, telemetry::test_seam::runtime_state());
	EXPECT_EQ(telemetry::detail::RuntimeTerminalReason::ConfigAbsent,
		telemetry::test_seam::runtime_terminal_reason());
	EXPECT_EQ(1U, telemetry::test_seam::runtime_adapter_main_thread_checks());
	EXPECT_EQ(1U, telemetry::test_seam::runtime_adapter_config_loads());
	EXPECT_EQ(0U, telemetry::test_seam::runtime_adapter_post_config_calls());
	const auto startup_diagnostic_calls = telemetry::test_seam::runtime_adapter_diagnostic_calls();
	EXPECT_LE(startup_diagnostic_calls, 1U);
	EXPECT_EQ(0U, telemetry::test_seam::runtime_adapter_lifecycle_calls());

	events::EngineUpdate();

	const Counts two_updates_only{2, 0, 0, 0, 0};
	EXPECT_EQ(two_updates_only, invocation_counts());
	EXPECT_EQ(telemetry::detail::RuntimeState::Disabled, telemetry::test_seam::runtime_state());
	EXPECT_EQ(1U, telemetry::test_seam::runtime_adapter_main_thread_checks());
	EXPECT_EQ(1U, telemetry::test_seam::runtime_adapter_config_loads());
	EXPECT_EQ(0U, telemetry::test_seam::runtime_adapter_post_config_calls());
	EXPECT_EQ(startup_diagnostic_calls, telemetry::test_seam::runtime_adapter_diagnostic_calls());

	events::EngineShutdown();
	events::GameMissionLoad("test-mission.fs2");
	events::GameEnterState(1, 2);
	events::GameLeaveState(2, 1);

	const Counts two_updates_and_one_lifecycle{2, 1, 1, 1, 1};
	EXPECT_EQ(two_updates_and_one_lifecycle, invocation_counts());
	EXPECT_EQ(telemetry::detail::RuntimeState::Stopped, telemetry::test_seam::runtime_state());
	EXPECT_EQ(7U, telemetry::test_seam::runtime_adapter_lifecycle_calls());

	for (std::size_t i = 0; i < RepeatedInitializeCount; ++i) {
		telemetry::initialize();
	}
	emit_each_event_once();

	EXPECT_EQ(one_registration_each, registration_counts());
	EXPECT_EQ(1U, telemetry::test_seam::runtime_adapter_factory_requests());
	EXPECT_EQ(1U, telemetry::test_seam::runtime_adapter_main_thread_captures());
	EXPECT_EQ(1U, telemetry::test_seam::runtime_adapter_main_thread_checks());
	EXPECT_EQ(1U, telemetry::test_seam::runtime_adapter_config_loads());
	EXPECT_EQ(0U, telemetry::test_seam::runtime_adapter_post_config_calls());
	EXPECT_EQ(startup_diagnostic_calls, telemetry::test_seam::runtime_adapter_diagnostic_calls());
	EXPECT_EQ(7U, telemetry::test_seam::runtime_adapter_lifecycle_calls());
	const Counts final_invocations{3, 2, 2, 2, 2};
	EXPECT_EQ(final_invocations, invocation_counts());
}

} // namespace
