#include "telemetry/phase4_trusted_image.h"

#include <gtest/gtest.h>

namespace {

using telemetry::detail::Phase4DockingRelation;
using telemetry::detail::Phase4EntityProjectionInput;
using telemetry::detail::Phase4TrustedImageStatus;
using telemetry::detail::build_phase4_trusted_image;
using telemetry::protocol::ObjectType;
using telemetry::protocol::RecordType;

Phase4EntityProjectionInput entity(std::uint64_t id, ObjectType type)
{
	Phase4EntityProjectionInput value;
	value.entity_id = id;
	value.object_type = type;
	value.class_id = (type == ObjectType::Ship || type == ObjectType::Weapon) ? 1U : 0U;
	value.sample_time_us = 50U;
	return value;
}

TEST(TelemetryPhase4TrustedImage, CombinesEntityAndDockingRecordsInOneCandidate)
{
	const Phase4DockingRelation forward{1U, 2U, 3U, 4U, "Left", "Right"};
	const Phase4DockingRelation reverse{2U, 1U, 4U, 3U, "Right", "Left"};
	telemetry::protocol::StateImage image;
	ASSERT_EQ(Phase4TrustedImageStatus::Created,
		build_phase4_trusted_image({entity(1U, ObjectType::Ship), entity(2U, ObjectType::Ship),
			entity(3U, ObjectType::Weapon)}, {forward, reverse}, 60U, image));
	EXPECT_EQ(8U, image.records().size());
	std::size_t docking_count = 0U;
	for (const auto& record : image.records()) {
		if (record.key.record_type == static_cast<std::uint16_t>(RecordType::DockingState)) ++docking_count;
	}
	EXPECT_EQ(2U, docking_count);
}

TEST(TelemetryPhase4TrustedImage, LeavesOutputUntouchedWhenDockingCannotJoinTheEntityGraph)
{
	telemetry::protocol::StateImage image;
	ASSERT_EQ(Phase4TrustedImageStatus::Created,
		build_phase4_trusted_image({entity(1U, ObjectType::Waypoint)}, {}, 1U, image));
	const auto original_size = image.records().size();
	EXPECT_EQ(Phase4TrustedImageStatus::InvalidDocking,
		build_phase4_trusted_image({entity(1U, ObjectType::Waypoint)},
			{{1U, 2U, 0U, 0U, "", ""}}, 1U, image));
	EXPECT_EQ(original_size, image.records().size());
}

} // namespace
