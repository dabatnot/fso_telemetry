#pragma once

#include "telemetry/phase2_observation.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace telemetry::detail {

// Test-only observation runner. The callback may invoke the real
// read_player_controls, player_inspect_cargo, ship_do_rearm_frame and
// ::ship_cleanup entry points against test-owned real globals. This seam only
// orchestrates observation off/on/off; it never wraps, simulates or substitutes
// any gameplay entry point.
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
	std::uint64_t hook_calls = 0U;
};

using GameplayAbInvoke = void (*)(void* context) noexcept;
using GameplayAbCapture =
	GameplayAbSnapshot (*)(void* context) noexcept;

struct GameplayAbRun {
	std::array<GameplayAbSnapshot, 3U> snapshots{};
};

class GameplayAbHookCounter final : public Phase2SeamTestDouble {
  public:
	explicit GameplayAbHookCounter(Phase2SeamTestDouble* delegate) noexcept
		: m_delegate(delegate)
	{
	}

	std::uint64_t calls() const noexcept { return m_calls; }
	void on_ship_cleanup(const ShipCleanupFact& fact) noexcept override
	{
		++m_calls;
		if (m_delegate != nullptr) m_delegate->on_ship_cleanup(fact);
	}
	void on_support_transition(const SupportTransitionFact& fact) noexcept override
	{
		++m_calls;
		if (m_delegate != nullptr) m_delegate->on_support_transition(fact);
	}
	void on_control_target(ControlTargetAuthority authority) noexcept override
	{
		++m_calls;
		if (m_delegate != nullptr) m_delegate->on_control_target(authority);
	}
	void on_cargo_authority(const CargoAuthorityFact& fact) noexcept override
	{
		++m_calls;
		if (m_delegate != nullptr) m_delegate->on_cargo_authority(fact);
	}

  private:
	Phase2SeamTestDouble* m_delegate = nullptr;
	std::uint64_t m_calls = 0U;
};

inline GameplayAbRun run_phase2_gameplay_ab(
	GameplayAbInvoke invoke,
	GameplayAbCapture capture,
	void* context,
	Phase2SeamTestDouble* observer = nullptr) noexcept
{
	GameplayAbRun run;
	if (invoke == nullptr || capture == nullptr) {
		return run;
	}
	telemetry::test_seam::set_phase2_seam_test_double(nullptr);
	invoke(context);
	run.snapshots[0] = capture(context);

	GameplayAbHookCounter counter(observer);
	telemetry::test_seam::set_phase2_seam_test_double(&counter);
	invoke(context);
	run.snapshots[1] = capture(context);
	run.snapshots[1].hook_calls = counter.calls();

	telemetry::test_seam::set_phase2_seam_test_double(nullptr);
	invoke(context);
	run.snapshots[2] = capture(context);
	return run;
}

} // namespace telemetry::detail
