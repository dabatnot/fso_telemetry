#include "telemetry/phase4_entity_image.h"

#include <gtest/gtest.h>

#include <atomic>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <new>
#include <vector>

namespace {
std::atomic<bool> observe_allocations{false};
std::atomic<std::size_t> observed_allocations{0U};
}

void* operator new(std::size_t size)
{
	if (observe_allocations.load(std::memory_order_relaxed))
		observed_allocations.fetch_add(1U, std::memory_order_relaxed);
	if (auto* value = std::malloc(size == 0U ? 1U : size)) return value;
	throw std::bad_alloc();
}

void* operator new[](std::size_t size)
{
	return ::operator new(size);
}

void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

namespace {

using telemetry::detail::Phase4EntityImageStatus;
using telemetry::detail::Phase4EntityProjectionInput;
using telemetry::detail::Phase4StateImagePool;
using telemetry::detail::build_phase4_entity_image;
using telemetry::detail::build_phase4_entity_image_preallocated;
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

TEST(TelemetryPhase4EntityImage,
	PreallocatedPoolBuildsWithoutAllocationAndFailsClosedAtBounds)
{
	Phase4StateImagePool pool;
	ASSERT_TRUE(pool.provision(3U));
	ASSERT_TRUE(pool.ready());
	EXPECT_GT(pool.owned_backing_bytes(), 0U);
	std::vector<Phase4EntityProjectionInput> input;
	input.reserve(4U);
	input.push_back(entity(3U, 1U));
	input.push_back(entity(1U));
	input.push_back(entity(2U, 1U));
	std::array<telemetry::protocol::StateImage,
		Phase4StateImagePool::SlotCount> retained{};

	for (auto& image : retained) {
		observed_allocations.store(0U, std::memory_order_relaxed);
		observe_allocations.store(true, std::memory_order_relaxed);
		const auto status = build_phase4_entity_image_preallocated(
			input, pool, image);
		observe_allocations.store(false, std::memory_order_relaxed);
		EXPECT_EQ(Phase4EntityImageStatus::Created, status);
		EXPECT_EQ(0U, observed_allocations.load(std::memory_order_relaxed));
		EXPECT_EQ(6U, image.records().size());
	}

	telemetry::protocol::StateImage unchanged;
	ASSERT_EQ(Phase4EntityImageStatus::Created,
		build_phase4_entity_image({entity(20U)}, unchanged));
	const auto original = unchanged.records().size();
	observed_allocations.store(0U, std::memory_order_relaxed);
	observe_allocations.store(true, std::memory_order_relaxed);
	const auto exhausted = build_phase4_entity_image_preallocated(
		input, pool, unchanged);
	observe_allocations.store(false, std::memory_order_relaxed);
	EXPECT_EQ(Phase4EntityImageStatus::AllocationFailure, exhausted);
	EXPECT_EQ(0U, observed_allocations.load(std::memory_order_relaxed));
	EXPECT_EQ(original, unchanged.records().size());

	retained[0] = {};
	EXPECT_EQ(Phase4EntityImageStatus::Created,
		build_phase4_entity_image_preallocated(input, pool, unchanged));
	input.push_back(entity(4U));
	EXPECT_EQ(Phase4EntityImageStatus::TooManyEntities,
		build_phase4_entity_image_preallocated(input, pool, unchanged));
	EXPECT_EQ(6U, unchanged.records().size());
}

} // namespace
