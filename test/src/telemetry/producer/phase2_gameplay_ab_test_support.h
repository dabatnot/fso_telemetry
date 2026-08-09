#pragma once

#include "telemetry/phase2_observation.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace telemetry::detail {

struct GameplayAbSnapshot {
	std::uint64_t controls = 0U;
	std::int32_t gameplay_return = 0;
	std::uint64_t gameplay_mutations = 0U;
	std::uint64_t cargo_advance_count = 0U;
	std::uint64_t cargo_reset_count = 0U;
	std::uint64_t cargo_reveal_count = 0U;
	std::uint64_t hud_side_effect_count = 0U;
	std::uint64_t allocation_count = 0U;
	std::size_t socket_count = 0U;
};

using GameplayAbInvoke = void (*)(void* context) noexcept;
using GameplayAbCapture =
	GameplayAbSnapshot (*)(void* context) noexcept;

struct GameplayAbRun {
	std::array<GameplayAbSnapshot, 3U> snapshots{};
};

inline GameplayAbRun run_phase2_gameplay_repeated(
	GameplayAbInvoke invoke,
	GameplayAbCapture capture,
	void* context) noexcept
{
	GameplayAbRun run;
	if (invoke == nullptr || capture == nullptr) {
		return run;
	}
	for (std::size_t index = 0U; index < run.snapshots.size(); ++index) {
		invoke(context);
		run.snapshots[index] = capture(context);
	}
	return run;
}

} // namespace telemetry::detail
