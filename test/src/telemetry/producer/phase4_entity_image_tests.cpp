#include "telemetry/phase4_entity_image.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using telemetry::detail::Phase4EntityImageStatus;
using telemetry::detail::Phase4EntityProjectionInput;
using telemetry::detail::build_phase4_entity_image;
using telemetry::protocol::ObjectType;

Phase4EntityProjectionInput entity(std::uint64_t id, std::uint64_t parent = 0U)
{
	Phase4EntityProjectionInput result;
	result.entity_id = id;
	result.object_type = ObjectType::Waypoint;
	result.parent_entity_id = parent;
	result.sample_time_us = 10U;
	return result;
}

TEST(TelemetryPhase4EntityImage, PublishesAllEntitiesAtomicallyInEntityIdOrder)
{
	telemetry::protocol::StateImage image;
	const std::vector<Phase4EntityProjectionInput> input{{entity(3U, 1U), entity(1U), entity(2U, 1U)}};
	ASSERT_EQ(Phase4EntityImageStatus::Created, build_phase4_entity_image(input, image));
	ASSERT_EQ(6U, image.records().size());
	for (std::size_t index = 0U; index < 3U; ++index) {
		EXPECT_EQ(static_cast<std::uint16_t>(telemetry::protocol::RecordType::EntityLifecycle),
			image.records()[index].key.record_type);
	}
}

TEST(TelemetryPhase4EntityImage, RefusesUnknownParentDuplicateAndCyclesWithoutPublishing)
{
	telemetry::protocol::StateImage output;
	ASSERT_EQ(Phase4EntityImageStatus::Created,
		build_phase4_entity_image({entity(10U)}, output));
	const auto original_count = output.records().size();

	EXPECT_EQ(Phase4EntityImageStatus::UnknownParent,
		build_phase4_entity_image({entity(1U, 2U)}, output));
	EXPECT_EQ(original_count, output.records().size());
	EXPECT_EQ(Phase4EntityImageStatus::DuplicateEntityId,
		build_phase4_entity_image({entity(1U), entity(1U)}, output));
	EXPECT_EQ(original_count, output.records().size());
	EXPECT_EQ(Phase4EntityImageStatus::ParentCycle,
		build_phase4_entity_image({entity(1U, 2U), entity(2U, 1U)}, output));
	EXPECT_EQ(original_count, output.records().size());
}

} // namespace
