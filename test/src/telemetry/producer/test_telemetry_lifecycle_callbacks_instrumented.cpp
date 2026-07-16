#include "events/events.h"
#include "gamesequence/gamesequence.h"
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
void reset_ready_runtime_adapter() noexcept;
std::uint64_t ready_runtime_factory_requests() noexcept;
std::uint64_t ready_runtime_service_calls() noexcept;
std::uint64_t ready_runtime_lifecycle_calls() noexcept;
std::uint64_t ready_runtime_diagnostic_calls() noexcept;
detail::RuntimeState runtime_state() noexcept;
detail::RuntimeTerminalReason runtime_terminal_reason() noexcept;
std::uint32_t runtime_mission_generation() noexcept;
bool runtime_mission_publication_allowed() noexcept;

} // namespace telemetry::test_seam

namespace {

constexpr std::size_t EngineUpdateIndex = 0U;
constexpr std::size_t EngineShutdownIndex = 1U;
constexpr std::size_t GameMissionLoadIndex = 2U;
constexpr std::size_t GameEnterStateIndex = 3U;
constexpr std::size_t GameLeaveStateIndex = 4U;
constexpr std::size_t EventCount = 5U;

using Counts = std::array<std::uint64_t, EventCount>;

Counts registrations()
{
	return {telemetry::test_seam::registration_attempts(EngineUpdateIndex),
		telemetry::test_seam::registration_attempts(EngineShutdownIndex),
		telemetry::test_seam::registration_attempts(GameMissionLoadIndex),
		telemetry::test_seam::registration_attempts(GameEnterStateIndex),
		telemetry::test_seam::registration_attempts(GameLeaveStateIndex)};
}

Counts invocations()
{
	return {telemetry::test_seam::callback_invocations(EngineUpdateIndex),
		telemetry::test_seam::callback_invocations(EngineShutdownIndex),
		telemetry::test_seam::callback_invocations(GameMissionLoadIndex),
		telemetry::test_seam::callback_invocations(GameEnterStateIndex),
		telemetry::test_seam::callback_invocations(GameLeaveStateIndex)};
}

TEST(TelemetryLifecycleCallbacksInstrumented, AllFiveProcessLifetimeWrappersRouteToOneRuntimeAndBecomeInertAfterStop)
{
	telemetry::test_seam::reset_observation_counts();
	telemetry::test_seam::reset_ready_runtime_adapter();
	telemetry::initialize();
	telemetry::initialize();

	EXPECT_EQ((Counts{1U, 1U, 1U, 1U, 1U}), registrations());
	EXPECT_EQ(1U, telemetry::test_seam::ready_runtime_factory_requests());
	EXPECT_EQ(telemetry::detail::RuntimeState::Cold, telemetry::test_seam::runtime_state());

	events::EngineUpdate();
	ASSERT_EQ(telemetry::detail::RuntimeState::Ready, telemetry::test_seam::runtime_state());
	ASSERT_EQ(telemetry::detail::RuntimeTerminalReason::None,
		telemetry::test_seam::runtime_terminal_reason());
	ASSERT_EQ(0U, telemetry::test_seam::runtime_mission_generation());
	ASSERT_FALSE(telemetry::test_seam::runtime_mission_publication_allowed());

	const auto* poison_filename = reinterpret_cast<const char*>(static_cast<std::uintptr_t>(1U));
	const auto calls_before_load = telemetry::test_seam::ready_runtime_service_calls();
	events::GameMissionLoad(poison_filename);
	EXPECT_EQ(calls_before_load, telemetry::test_seam::ready_runtime_service_calls())
		<< "The wrapper may only set a value marker; it must not read, copy, log or forward the filename.";
	EXPECT_EQ(telemetry::detail::RuntimeState::Ready, telemetry::test_seam::runtime_state());
	EXPECT_EQ(0U, telemetry::test_seam::runtime_mission_generation());

	events::EngineUpdate();
	ASSERT_EQ(telemetry::detail::RuntimeState::MissionLoading, telemetry::test_seam::runtime_state());
	ASSERT_EQ(1U, telemetry::test_seam::runtime_mission_generation());

	const auto calls_before_enter = telemetry::test_seam::ready_runtime_service_calls();
	events::GameEnterState(GS_STATE_BRIEFING, GS_STATE_GAME_PLAY);
	EXPECT_EQ(calls_before_enter, telemetry::test_seam::ready_runtime_service_calls());
	EXPECT_EQ(telemetry::detail::RuntimeState::MissionLoading, telemetry::test_seam::runtime_state());
	events::EngineUpdate();
	ASSERT_EQ(telemetry::detail::RuntimeState::MissionActive, telemetry::test_seam::runtime_state());
	ASSERT_TRUE(telemetry::test_seam::runtime_mission_publication_allowed());

	const auto calls_before_second_load = telemetry::test_seam::ready_runtime_service_calls();
	events::GameMissionLoad(poison_filename);
	EXPECT_EQ(calls_before_second_load, telemetry::test_seam::ready_runtime_service_calls());
	EXPECT_FALSE(telemetry::test_seam::runtime_mission_publication_allowed());
	events::EngineUpdate();
	ASSERT_EQ(telemetry::detail::RuntimeState::MissionLoading, telemetry::test_seam::runtime_state());
	ASSERT_EQ(2U, telemetry::test_seam::runtime_mission_generation());

	events::GameEnterState(GS_STATE_BRIEFING, GS_STATE_GAME_PLAY);
	events::EngineUpdate();
	ASSERT_EQ(telemetry::detail::RuntimeState::MissionActive, telemetry::test_seam::runtime_state());

	const auto calls_before_leave = telemetry::test_seam::ready_runtime_service_calls();
	events::GameLeaveState(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);
	EXPECT_EQ(calls_before_leave, telemetry::test_seam::ready_runtime_service_calls());
	EXPECT_FALSE(telemetry::test_seam::runtime_mission_publication_allowed());
	events::EngineUpdate();
	ASSERT_EQ(telemetry::detail::RuntimeState::Ready, telemetry::test_seam::runtime_state());
	ASSERT_EQ(2U, telemetry::test_seam::runtime_mission_generation());

	events::EngineShutdown();
	ASSERT_EQ(telemetry::detail::RuntimeState::Stopped, telemetry::test_seam::runtime_state());
	ASSERT_EQ(0U, telemetry::test_seam::ready_runtime_diagnostic_calls());
	const auto calls_after_stop = telemetry::test_seam::ready_runtime_service_calls();
	const auto lifecycle_after_stop = telemetry::test_seam::ready_runtime_lifecycle_calls();

	events::EngineUpdate();
	events::EngineShutdown();
	events::GameMissionLoad(poison_filename);
	events::GameEnterState(GS_STATE_MAIN_MENU, GS_STATE_GAME_PLAY);
	events::GameLeaveState(GS_STATE_GAME_PLAY, GS_STATE_MAIN_MENU);

	EXPECT_EQ(calls_after_stop, telemetry::test_seam::ready_runtime_service_calls());
	EXPECT_EQ(lifecycle_after_stop, telemetry::test_seam::ready_runtime_lifecycle_calls());
	EXPECT_EQ(telemetry::detail::RuntimeState::Stopped, telemetry::test_seam::runtime_state());
	EXPECT_EQ((Counts{7U, 2U, 3U, 3U, 2U}), invocations());
	EXPECT_EQ((Counts{1U, 1U, 1U, 1U, 1U}), registrations());
}

} // namespace
