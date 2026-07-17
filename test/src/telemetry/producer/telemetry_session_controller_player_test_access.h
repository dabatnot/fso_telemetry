#pragma once

#include "telemetry/session_controller.h"

#include <cstddef>
#include <cstdint>

namespace telemetry::detail {

class SessionControllerPlayerTestAccess final {
  public:
	static bool seed_last_allocated_entity_id(SessionController& controller,
		std::size_t slot_index,
		std::uint64_t last_id) noexcept
	{
		if (!controller.m_ready || slot_index >= controller.m_config.max_clients || !controller.m_slots) {
			return false;
		}
		auto& slot = controller.m_slots[slot_index];
		if (slot.progress != ProducerSessionProgress::ReadyForState &&
			slot.progress != ProducerSessionProgress::Stale) {
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
