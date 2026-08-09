#include "telemetry/phase2_session_transition.h"

namespace telemetry {

Phase2ProfileMutationResult reject_phase2_profile_mutation(Phase2Profile current_profile,
	std::uint64_t requested_coverage,
	Phase2ProfileMutationSource source) noexcept
{
	(void)source;

	if (requested_coverage == phase2_profile_coverage(current_profile)) {
		return {};
	}

	Phase2ProfileMutationResult result;
	result.error = protocol::ValidationError::InvalidStateTransition;
	result.session_end_reason = protocol::SessionEndReason::ProtocolError;
	result.session_end_flags = protocol::SessionEndFlagReconnectAllowed;
	result.slot_state = Phase2SessionSlotState::FaultedSession;
	return result;
}

} // namespace telemetry
