#include "telemetry/phase2_session_transition.h"

namespace telemetry {

CockpitCoverageMutationResult reject_cockpit_coverage_mutation(
	std::uint64_t requested_coverage,
	CockpitCoverageMutationSource source) noexcept
{
	(void)source;

	if (requested_coverage == Phase3CockpitSensorsCoverage) {
		return {};
	}

	CockpitCoverageMutationResult result;
	result.error = protocol::ValidationError::InvalidStateTransition;
	result.session_end_reason = protocol::SessionEndReason::ProtocolError;
	result.session_end_flags = protocol::SessionEndFlagReconnectAllowed;
	result.slot_state = Phase2SessionSlotState::FaultedSession;
	return result;
}

} // namespace telemetry
