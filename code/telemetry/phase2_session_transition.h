#pragma once

#include "telemetry/phase2_profile_gate.h"
#include "telemetry/protocol/telemetry_protocol_constants.h"

#include <cstdint>

namespace telemetry {

enum class Phase2ProfileMutationSource : std::uint8_t {
	Delta = 0,
	CapabilityUpdate,
	Count,
};

enum class Phase2SessionSlotState : std::uint8_t {
	ActiveSession = 0,
	FaultedSession,
	Count,
};

struct Phase2ProfileMutationResult {
	protocol::ValidationError error = protocol::ValidationError::None;
	protocol::SessionEndReason session_end_reason = protocol::SessionEndReason::Normal;
	std::uint8_t session_end_flags = protocol::SessionEndFlagNone;
	Phase2SessionSlotState slot_state = Phase2SessionSlotState::ActiveSession;
};

Phase2ProfileMutationResult reject_phase2_profile_mutation(Phase2Profile current_profile,
	std::uint64_t requested_coverage,
	Phase2ProfileMutationSource source) noexcept;

} // namespace telemetry
