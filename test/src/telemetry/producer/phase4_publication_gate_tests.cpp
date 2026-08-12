#include "telemetry/phase4_publication_gate.h"

#include <gtest/gtest.h>

namespace {

using telemetry::detail::Phase4PublicationGate;
using telemetry::detail::Phase4PublicationResult;
using telemetry::detail::Phase4PublicationState;

TEST(TelemetryPhase4PublicationGate, RequiresManifestThenSnapshotAckBeforeDeltas)
{
	Phase4PublicationGate gate;
	EXPECT_EQ(Phase4PublicationResult::NotReady, gate.permit_delta());
	EXPECT_EQ(Phase4PublicationResult::Accepted, gate.require_manifest(7U));
	EXPECT_EQ(Phase4PublicationResult::NotReady, gate.begin_snapshot(1U));
	EXPECT_EQ(Phase4PublicationResult::InvalidManifest, gate.on_manifest_applied(8U));
	EXPECT_EQ(Phase4PublicationResult::Accepted, gate.on_manifest_applied(7U));
	EXPECT_EQ(Phase4PublicationState::ReadyForSnapshot, gate.state());
	EXPECT_EQ(Phase4PublicationResult::Accepted, gate.begin_snapshot(11U));
	EXPECT_EQ(Phase4PublicationResult::NotReady, gate.permit_delta());
	EXPECT_EQ(Phase4PublicationResult::InvalidSnapshot, gate.on_snapshot_applied(12U, 7U));
	EXPECT_EQ(Phase4PublicationResult::InvalidManifest, gate.on_snapshot_applied(11U, 8U));
	EXPECT_EQ(Phase4PublicationResult::Accepted, gate.on_snapshot_applied(11U, 7U));
	EXPECT_EQ(Phase4PublicationState::ReadyForDeltas, gate.state());
	EXPECT_EQ(Phase4PublicationResult::Accepted, gate.permit_delta());
}

TEST(TelemetryPhase4PublicationGate, NewManifestAndFaultCloseThePreviousPublicationEpoch)
{
	Phase4PublicationGate gate;
	ASSERT_EQ(Phase4PublicationResult::Accepted, gate.require_manifest(1U));
	ASSERT_EQ(Phase4PublicationResult::Accepted, gate.on_manifest_applied(1U));
	ASSERT_EQ(Phase4PublicationResult::Accepted, gate.begin_snapshot(1U));
	ASSERT_EQ(Phase4PublicationResult::Accepted, gate.on_snapshot_applied(1U, 1U));
	ASSERT_EQ(Phase4PublicationResult::Accepted, gate.permit_delta());
	EXPECT_EQ(Phase4PublicationResult::Accepted, gate.require_manifest(2U));
	EXPECT_EQ(Phase4PublicationResult::NotReady, gate.permit_delta());
	gate.fault();
	EXPECT_EQ(Phase4PublicationState::Faulted, gate.state());
	EXPECT_EQ(Phase4PublicationResult::Faulted, gate.on_manifest_applied(2U));
	gate.reset();
	EXPECT_EQ(Phase4PublicationState::AwaitingManifest, gate.state());
}

} // namespace
