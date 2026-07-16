#include "events/events.h"
#include "telemetry/telemetry.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace telemetry::test_seam {

void reset_observation_counts() noexcept;
std::uint64_t registration_attempts(std::size_t event_index) noexcept;
std::uint64_t callback_invocations(std::size_t event_index) noexcept;

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

	telemetry::initialize();
	telemetry::initialize();

	const Counts one_registration_each{1, 1, 1, 1, 1};
	EXPECT_EQ(one_registration_each, registration_counts());

	for (std::size_t i = 0; i < RepeatedInitializeCount; ++i) {
		telemetry::initialize();
	}

	EXPECT_EQ(one_registration_each, registration_counts());
	EXPECT_EQ(0U, telemetry::test_seam::registration_attempts(EventCount));
	EXPECT_EQ(0U, telemetry::test_seam::callback_invocations(EventCount));

	emit_each_event_once();

	const Counts one_invocation_each{1, 1, 1, 1, 1};
	EXPECT_EQ(one_invocation_each, invocation_counts());

	for (std::size_t i = 0; i < RepeatedInitializeCount; ++i) {
		telemetry::initialize();
	}
	emit_each_event_once();

	EXPECT_EQ(one_registration_each, registration_counts());
	const Counts two_invocations_each{2, 2, 2, 2, 2};
	EXPECT_EQ(two_invocations_each, invocation_counts());
}

} // namespace
