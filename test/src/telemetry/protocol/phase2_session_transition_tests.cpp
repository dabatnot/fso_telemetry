#include "telemetry/phase2_session_transition.h"

#include <gtest/gtest.h>

#include <array>

namespace {

using namespace telemetry;
using namespace telemetry::protocol;

TEST(TelemetryPhase2SessionTransition, CoverageMutationFaultsOnlyTheAffectedSession)
{
	for (const auto source :
		{Phase2ProfileMutationSource::Delta, Phase2ProfileMutationSource::CapabilityUpdate}) {
		SCOPED_TRACE(static_cast<unsigned>(source));
		const auto result = reject_phase2_profile_mutation(
			Phase2Profile::CoreGate, 0x0583ULL, source);
		EXPECT_EQ(ValidationError::InvalidStateTransition, result.error);
		EXPECT_EQ(SessionEndReason::ProtocolError, result.session_end_reason);
		EXPECT_EQ(SessionEndFlagReconnectAllowed, result.session_end_flags);
		EXPECT_EQ(Phase2SessionSlotState::FaultedSession, result.slot_state);
	}
}

} // namespace
