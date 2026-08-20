#pragma once

// Test-only friend seam.  It lives with the owned type so production sources
// never include a test-tree header.  No runtime path calls this API.
#include "telemetry/session_controller.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace telemetry::detail {

class SessionControllerTestAccess final {
  public:
	static bool mark_stale(SessionController& controller,
		std::size_t slot_index) noexcept
	{
		if (!controller.m_ready ||
			slot_index >= controller.m_config.max_clients ||
			!controller.m_slots ||
			controller.m_slots[slot_index].progress !=
				ProducerSessionProgress::ReadyForState) {
			return false;
		}
		controller.m_slots[slot_index].progress =
			ProducerSessionProgress::Stale;
		return true;
	}

	static bool defer_due_keyframe(SessionController& controller,
		std::size_t slot_index,
		std::uint64_t now_us,
		std::uint64_t duration_us) noexcept
	{
		if (!controller.m_ready ||
			slot_index >= controller.m_config.max_clients ||
			!controller.m_slots ||
			now_us >
				std::numeric_limits<std::uint64_t>::max() -
					duration_us) {
			return false;
		}
		auto& slot = controller.m_slots[slot_index];
		if (slot.progress !=
			ProducerSessionProgress::ReadyForState)
			return false;
		slot.keyframe_due = false;
		slot.next_keyframe_due_us = now_us + duration_us;
		return true;
	}

	static bool seed_last_allocated_entity_id(SessionController& controller,
		std::size_t slot_index,
		std::uint64_t last_id) noexcept
	{
		if (!controller.m_ready || slot_index >= controller.m_config.max_clients || !controller.m_slots) {
			return false;
		}
		auto& slot = controller.m_slots[slot_index];
		if (slot.progress != ProducerSessionProgress::ReadyForState && slot.progress != ProducerSessionProgress::Stale) {
			return false;
		}
		slot.player_entity_ids = EntityIdRegistry(last_id);
		slot.latest_player_sample = {};
		slot.latest_player_sample_status = PlayerSampleMaterializeStatus::InvalidCapture;
		slot.has_latest_player_sample = false;
		return true;
	}
};

} // namespace telemetry::detail
