#include "telemetry/phase4_docking_projection.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using telemetry::detail::Phase4DockingProjectionStatus;
using telemetry::detail::Phase4DockingRelation;
using telemetry::detail::project_phase4_docking_states;

Phase4DockingRelation relation(std::uint64_t local, std::uint64_t remote,
	std::uint16_t local_point, std::uint16_t remote_point)
{
	return {local, remote, local_point, remote_point, "Local", "Remote"};
}

Phase4DockingRelation reverse(const Phase4DockingRelation& value)
{
	return {value.remote_entity_id, value.local_entity_id, value.remote_dockpoint,
		value.local_dockpoint, value.remote_dock_bay_name, value.local_dock_bay_name};
}

TEST(TelemetryPhase4DockingProjection, EmitsOneValidatedStateForEachShipWithExactReciprocity)
{
	const auto forward = relation(1U, 2U, 3U, 4U);
	std::vector<telemetry::protocol::StateAtom> records;
	ASSERT_EQ(Phase4DockingProjectionStatus::Created,
		project_phase4_docking_states({1U, 2U, 3U}, {forward, reverse(forward)}, 99U, records));
	ASSERT_EQ(3U, records.size());
	for (const auto& record : records) {
		EXPECT_EQ(static_cast<std::uint16_t>(telemetry::protocol::RecordType::DockingState),
			record.key.record_type);
		EXPECT_TRUE(record.has_cascade_owner);
	}
}

TEST(TelemetryPhase4DockingProjection, LeavesOutputUntouchedForBrokenRelationGraphs)
{
	std::vector<telemetry::protocol::StateAtom> records;
	const auto forward = relation(1U, 2U, 3U, 4U);
	ASSERT_EQ(Phase4DockingProjectionStatus::Created,
		project_phase4_docking_states({1U, 2U}, {forward, reverse(forward)}, 1U, records));
	const auto original_size = records.size();
	EXPECT_EQ(Phase4DockingProjectionStatus::MissingReciprocal,
		project_phase4_docking_states({1U, 2U}, {forward}, 2U, records));
	EXPECT_EQ(original_size, records.size());
	EXPECT_EQ(Phase4DockingProjectionStatus::UnknownEndpoint,
		project_phase4_docking_states({1U, 2U}, {relation(1U, 3U, 0U, 0U)}, 2U, records));
	EXPECT_EQ(original_size, records.size());
	EXPECT_EQ(Phase4DockingProjectionStatus::SelfRelation,
		project_phase4_docking_states({1U}, {relation(1U, 1U, 0U, 0U)}, 2U, records));
	EXPECT_EQ(original_size, records.size());
}

TEST(TelemetryPhase4DockingProjection, RefusesTheSixtyFifthOutgoingRelationWithoutTruncation)
{
	std::vector<std::uint64_t> ships;
	std::vector<Phase4DockingRelation> relations;
	ships.push_back(1U);
	for (std::uint64_t index = 0U; index < 65U; ++index) {
		const auto remote = index + 2U;
		ships.push_back(remote);
		const auto forward = relation(1U, remote, static_cast<std::uint16_t>(index), 0U);
		relations.push_back(forward);
		relations.push_back(reverse(forward));
	}
	std::vector<telemetry::protocol::StateAtom> records;
	EXPECT_EQ(Phase4DockingProjectionStatus::TooManyRelations,
		project_phase4_docking_states(ships, relations, 1U, records));
	EXPECT_TRUE(records.empty());
}

} // namespace
