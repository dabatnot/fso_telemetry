#pragma once

#include "telemetry/cockpit_sensors_state_image.h"
#include "telemetry/protocol/telemetry_protocol_constants.h"

#include <cstdint>

namespace telemetry {

enum class CockpitCoverageMutationSource : std::uint8_t {
	Delta = 0,
	CapabilityUpdate,
	Count,
};

enum class Phase2SessionSlotState : std::uint8_t {
	ActiveSession = 0,
	FaultedSession,
	Count,
};

struct CockpitCoverageMutationResult {
	protocol::ValidationError error = protocol::ValidationError::None;
	protocol::SessionEndReason session_end_reason = protocol::SessionEndReason::Normal;
	std::uint8_t session_end_flags = protocol::SessionEndFlagNone;
	Phase2SessionSlotState slot_state = Phase2SessionSlotState::ActiveSession;
};

CockpitCoverageMutationResult reject_cockpit_coverage_mutation(
	std::uint64_t requested_coverage,
	CockpitCoverageMutationSource source) noexcept;

} // namespace telemetry
