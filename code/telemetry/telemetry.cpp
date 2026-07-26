#include "telemetry/telemetry.h"

#include "events/events.h"
#include "telemetry/phase2_observation.h"
#include "telemetry/runtime_adapter.h"

#if defined(FSO_TELEMETRY_TEST_SEAMS)
#include <array>
#include <cstddef>
#include <cstdint>
#endif

namespace {

bool telemetry_initialized = false;
bool telemetry_callbacks_ready = false;
telemetry::detail::Runtime* telemetry_runtime = nullptr;

telemetry::detail::Runtime& shared_runtime() noexcept
{
	static telemetry::detail::Runtime runtime(telemetry::detail::runtime_startup_services());
	telemetry_runtime = &runtime;
	return runtime;
}

#if defined(FSO_TELEMETRY_TEST_SEAMS)
constexpr std::size_t EngineUpdateIndex    = 0;
constexpr std::size_t EngineShutdownIndex  = 1;
constexpr std::size_t GameMissionLoadIndex = 2;
constexpr std::size_t GameEnterStateIndex  = 3;
constexpr std::size_t GameLeaveStateIndex  = 4;
constexpr std::size_t EventCount           = 5;

std::array<std::uint64_t, EventCount> registration_attempt_counts{};
std::array<std::uint64_t, EventCount> callback_invocation_counts{};
#endif

void on_engine_update() noexcept
{
#if defined(FSO_TELEMETRY_TEST_SEAMS)
	++callback_invocation_counts[EngineUpdateIndex];
#endif

	if (telemetry_callbacks_ready && telemetry_runtime != nullptr) {
		telemetry_runtime->on_engine_update();
	}
}

void on_engine_shutdown() noexcept
{
#if defined(FSO_TELEMETRY_TEST_SEAMS)
	++callback_invocation_counts[EngineShutdownIndex];
#endif

	telemetry::detail::reset_phase2_mission_observation_state();
	if (telemetry_callbacks_ready && telemetry_runtime != nullptr) {
		telemetry_runtime->on_engine_shutdown();
	}
}

void on_game_mission_load(const char*) noexcept
{
#if defined(FSO_TELEMETRY_TEST_SEAMS)
	++callback_invocation_counts[GameMissionLoadIndex];
#endif

	telemetry::detail::reset_phase2_mission_observation_state();
	if (telemetry_callbacks_ready && telemetry_runtime != nullptr) {
		telemetry_runtime->on_game_mission_load();
	}
}

void on_game_enter_state(int old_state, int new_state) noexcept
{
#if defined(FSO_TELEMETRY_TEST_SEAMS)
	++callback_invocation_counts[GameEnterStateIndex];
#endif

	if (telemetry_callbacks_ready && telemetry_runtime != nullptr) {
		telemetry_runtime->on_game_enter_state(old_state, new_state);
	}
}

void on_game_leave_state(int old_state, int new_state) noexcept
{
#if defined(FSO_TELEMETRY_TEST_SEAMS)
	++callback_invocation_counts[GameLeaveStateIndex];
#endif

	if (telemetry_callbacks_ready && telemetry_runtime != nullptr) {
		telemetry_runtime->on_game_leave_state(old_state, new_state);
	}
}

} // namespace

namespace telemetry {

void initialize() noexcept
{
	// Engine initialization and all event emissions are main-thread operations.
	// Register process-lifetime callbacks once; util::event has no individual removal.
	if (telemetry_initialized) {
		return;
	}
	telemetry_initialized = true;
	auto& runtime = shared_runtime();
	runtime.capture_main_thread();
	detail::capture_phase2_main_thread_authority();

	try {
#if defined(FSO_TELEMETRY_TEST_SEAMS)
		++registration_attempt_counts[EngineUpdateIndex];
#endif
		events::EngineUpdate.add(on_engine_update);

#if defined(FSO_TELEMETRY_TEST_SEAMS)
		++registration_attempt_counts[EngineShutdownIndex];
#endif
		events::EngineShutdown.add(on_engine_shutdown);

#if defined(FSO_TELEMETRY_TEST_SEAMS)
		++registration_attempt_counts[GameMissionLoadIndex];
#endif
		events::GameMissionLoad.add(on_game_mission_load);

#if defined(FSO_TELEMETRY_TEST_SEAMS)
		++registration_attempt_counts[GameEnterStateIndex];
#endif
		events::GameEnterState.add(on_game_enter_state);

#if defined(FSO_TELEMETRY_TEST_SEAMS)
		++registration_attempt_counts[GameLeaveStateIndex];
#endif
		events::GameLeaveState.add(on_game_leave_state);
	} catch (...) {
		// A partial util::event registration cannot be rolled back safely. Keep
		// telemetry disabled and prevent a retry from duplicating listeners.
		return;
	}
	telemetry_callbacks_ready = true;
}

} // namespace telemetry

#if defined(FSO_TELEMETRY_TEST_SEAMS)
namespace telemetry::test_seam {

void reset_observation_counts() noexcept
{
	registration_attempt_counts.fill(0);
	callback_invocation_counts.fill(0);
}

std::uint64_t registration_attempts(std::size_t event_index) noexcept
{
	if (event_index >= EventCount) {
		return 0;
	}

	return registration_attempt_counts[event_index];
}

std::uint64_t callback_invocations(std::size_t event_index) noexcept
{
	if (event_index >= EventCount) {
		return 0;
	}

	return callback_invocation_counts[event_index];
}

detail::RuntimeState runtime_state() noexcept
{
	return telemetry_runtime == nullptr ? detail::RuntimeState::Cold : telemetry_runtime->state();
}

detail::RuntimeTerminalReason runtime_terminal_reason() noexcept
{
	return telemetry_runtime == nullptr ? detail::RuntimeTerminalReason::None : telemetry_runtime->terminal_reason();
}

std::uint32_t runtime_mission_generation() noexcept
{
	return telemetry_runtime == nullptr ? 0U : telemetry_runtime->mission_generation();
}

bool runtime_mission_publication_allowed() noexcept
{
	return telemetry_runtime != nullptr && telemetry_runtime->mission_publication_allowed();
}

} // namespace telemetry::test_seam
#endif
