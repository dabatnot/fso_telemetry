#include "telemetry/phase2_allocation_tracker.h"
#include "telemetry/phase3_identity_registry.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <type_traits>

namespace {

using telemetry::detail::Phase3IdentityProvisionStatus;
using telemetry::detail::Phase3IdentityReconcileStatus;
using telemetry::detail::Phase3IdentityRegistry;
using telemetry::detail::Phase3IdentityResolveStatus;
using telemetry::detail::Phase3SensorIdentityKey;
using telemetry::test::phase2test::GlobalAllocationScope;

static_assert(Phase3IdentityRegistry::Capacity == 65'536U);
static_assert(std::is_final_v<Phase3IdentityRegistry>);
static_assert(std::is_nothrow_default_constructible_v<Phase3IdentityRegistry>);
static_assert(static_cast<std::uint8_t>(Phase3IdentityResolveStatus::Existing) == 0U);
static_assert(static_cast<std::uint8_t>(Phase3IdentityResolveStatus::Allocated) == 1U);
static_assert(static_cast<std::uint8_t>(Phase3IdentityResolveStatus::InvalidKey) == 2U);
static_assert(static_cast<std::uint8_t>(Phase3IdentityResolveStatus::NotReady) == 3U);
static_assert(static_cast<std::uint8_t>(Phase3IdentityResolveStatus::CapacityExhausted) == 4U);
static_assert(static_cast<std::uint8_t>(Phase3IdentityResolveStatus::CounterExhausted) == 5U);
static_assert(static_cast<std::uint8_t>(Phase3IdentityResolveStatus::Count) == 6U);
static_assert(static_cast<std::uint8_t>(Phase3IdentityReconcileStatus::Existing) == 0U);
static_assert(static_cast<std::uint8_t>(Phase3IdentityReconcileStatus::Bound) == 1U);
static_assert(static_cast<std::uint8_t>(Phase3IdentityReconcileStatus::InvalidKey) == 2U);
static_assert(static_cast<std::uint8_t>(Phase3IdentityReconcileStatus::InvalidEntityId) == 3U);
static_assert(static_cast<std::uint8_t>(Phase3IdentityReconcileStatus::NotReady) == 4U);
static_assert(static_cast<std::uint8_t>(Phase3IdentityReconcileStatus::CapacityExhausted) == 5U);
static_assert(static_cast<std::uint8_t>(Phase3IdentityReconcileStatus::KeyConflict) == 6U);
static_assert(static_cast<std::uint8_t>(Phase3IdentityReconcileStatus::EntityIdConflict) == 7U);
static_assert(static_cast<std::uint8_t>(Phase3IdentityReconcileStatus::Count) == 8U);

Phase3SensorIdentityKey key(std::uint32_t signature, std::uint8_t type = 1U) noexcept
{
	return {signature, type};
}

TEST(TelemetryPhase3Identity, RequiresProvisionAndRejectsInvalidKeysWithoutMutation)
{
	Phase3IdentityRegistry registry;
	EXPECT_FALSE(registry.ready());
	EXPECT_EQ(Phase3IdentityResolveStatus::InvalidKey, registry.resolve(key(0U)).status);
	EXPECT_EQ(Phase3IdentityResolveStatus::InvalidKey, registry.resolve(key(1U, 0U)).status);
	EXPECT_EQ(Phase3IdentityResolveStatus::NotReady, registry.resolve(key(1U)).status);
	EXPECT_EQ(Phase3IdentityReconcileStatus::InvalidEntityId,
		registry.reconcile_phase2_binding(key(1U), 0U));
	EXPECT_EQ(Phase3IdentityReconcileStatus::NotReady,
		registry.reconcile_phase2_binding(key(1U), 1U));
	EXPECT_EQ(0U, registry.identity_count());
	EXPECT_EQ(0U, registry.last_allocated_entity_id());
	EXPECT_EQ(0U, registry.provisioned_bytes());
}

TEST(TelemetryPhase3Identity, StableCompositeKeysReceiveStrictlyIncreasingNonZeroIds)
{
	Phase3IdentityRegistry registry;
	ASSERT_EQ(Phase3IdentityProvisionStatus::Ready, registry.provision());
	EXPECT_EQ(Phase3IdentityProvisionStatus::AlreadyReady, registry.provision());
	EXPECT_GT(registry.provisioned_bytes(), 0U);

	const auto first = registry.resolve(key(17U, 1U));
	ASSERT_EQ(Phase3IdentityResolveStatus::Allocated, first.status);
	EXPECT_EQ(1U, first.entity_id);
	const auto stable = registry.resolve(key(17U, 1U));
	EXPECT_EQ(Phase3IdentityResolveStatus::Existing, stable.status);
	EXPECT_EQ(first.entity_id, stable.entity_id);

	const auto same_signature_different_type = registry.resolve(key(17U, 2U));
	EXPECT_EQ(Phase3IdentityResolveStatus::Allocated, same_signature_different_type.status);
	EXPECT_EQ(2U, same_signature_different_type.entity_id);
	const auto next = registry.resolve(key(18U, 1U));
	EXPECT_EQ(Phase3IdentityResolveStatus::Allocated, next.status);
	EXPECT_EQ(3U, next.entity_id);
	EXPECT_EQ(3U, registry.identity_count());
	EXPECT_EQ(3U, registry.last_allocated_entity_id());
}

TEST(TelemetryPhase3Identity, ResetIsTheOnlyOperationThatPermitsIdsToRestart)
{
	Phase3IdentityRegistry registry;
	ASSERT_EQ(Phase3IdentityProvisionStatus::Ready, registry.provision());
	const auto first = registry.resolve(key(7U));
	ASSERT_EQ(1U, first.entity_id);
	EXPECT_EQ(first.entity_id, registry.resolve(key(7U)).entity_id);

	registry.reset_session();
	EXPECT_TRUE(registry.ready());
	EXPECT_EQ(0U, registry.identity_count());
	EXPECT_EQ(0U, registry.last_allocated_entity_id());
	EXPECT_EQ(1U, registry.resolve(key(8U)).entity_id);
}

TEST(TelemetryPhase3Identity, Phase2BindingsAreReusedAndAdvanceTheAllocationHighWater)
{
	Phase3IdentityRegistry registry;
	ASSERT_EQ(Phase3IdentityProvisionStatus::Ready, registry.provision());
	EXPECT_EQ(Phase3IdentityReconcileStatus::Bound,
		registry.reconcile_phase2_binding(key(42U, 1U), 91U));
	EXPECT_EQ(Phase3IdentityReconcileStatus::Existing,
		registry.reconcile_phase2_binding(key(42U, 1U), 91U));
	EXPECT_EQ(Phase3IdentityReconcileStatus::KeyConflict,
		registry.reconcile_phase2_binding(key(42U, 1U), 92U));
	EXPECT_EQ(Phase3IdentityReconcileStatus::EntityIdConflict,
		registry.reconcile_phase2_binding(key(43U, 1U), 91U));

	const auto reused = registry.resolve(key(42U, 1U));
	EXPECT_EQ(Phase3IdentityResolveStatus::Existing, reused.status);
	EXPECT_EQ(91U, reused.entity_id);
	const auto allocated = registry.resolve(key(43U, 1U));
	EXPECT_EQ(Phase3IdentityResolveStatus::Allocated, allocated.status);
	EXPECT_EQ(92U, allocated.entity_id);
}

TEST(TelemetryPhase3Identity, FailedTransactionRestoresBindingsAndIdHighWater)
{
	Phase3IdentityRegistry registry;
	ASSERT_EQ(Phase3IdentityProvisionStatus::Ready, registry.provision());
	ASSERT_EQ(1U, registry.resolve(key(10U)).entity_id);
	ASSERT_TRUE(registry.begin_transaction());
	EXPECT_TRUE(registry.transaction_active());
	ASSERT_EQ(Phase3IdentityReconcileStatus::Bound,
		registry.reconcile_phase2_binding(key(20U, 2U), 50U));
	ASSERT_EQ(51U, registry.resolve(key(30U, 3U)).entity_id);
	EXPECT_EQ(3U, registry.identity_count());

	registry.rollback_transaction();
	EXPECT_FALSE(registry.transaction_active());
	EXPECT_EQ(1U, registry.identity_count());
	EXPECT_EQ(1U, registry.last_allocated_entity_id());
	EXPECT_EQ(Phase3IdentityResolveStatus::Allocated,
		registry.resolve(key(20U, 2U)).status);
	EXPECT_EQ(2U, registry.last_allocated_entity_id());

	ASSERT_TRUE(registry.begin_transaction());
	ASSERT_EQ(3U, registry.resolve(key(40U, 4U)).entity_id);
	registry.commit_transaction();
	EXPECT_FALSE(registry.transaction_active());
	EXPECT_EQ(3U, registry.identity_count());
	EXPECT_EQ(3U, registry.resolve(key(40U, 4U)).entity_id);
}

TEST(TelemetryPhase3Identity, FixedCapacityIsReportedWithoutTruncationOrReuse)
{
	Phase3IdentityRegistry registry;
	ASSERT_EQ(Phase3IdentityProvisionStatus::Ready, registry.provision());
	for (std::size_t index = 0U; index < Phase3IdentityRegistry::Capacity; ++index) {
		const auto resolved = registry.resolve(key(static_cast<std::uint32_t>(index + 1U), 1U));
		ASSERT_EQ(Phase3IdentityResolveStatus::Allocated, resolved.status) << index;
		ASSERT_EQ(index + 1U, resolved.entity_id) << index;
	}
	EXPECT_EQ(Phase3IdentityRegistry::Capacity, registry.identity_count());
	EXPECT_EQ(Phase3IdentityResolveStatus::CapacityExhausted,
		registry.resolve(key(static_cast<std::uint32_t>(Phase3IdentityRegistry::Capacity + 1U))).status);
	EXPECT_EQ(Phase3IdentityRegistry::Capacity, registry.identity_count());
	EXPECT_EQ(1U, registry.resolve(key(1U)).entity_id);
}

TEST(TelemetryPhase3Identity, ResolveReconcileAndResetAllocateNothingAfterReady)
{
	Phase3IdentityRegistry registry;
	ASSERT_EQ(Phase3IdentityProvisionStatus::Ready, registry.provision());
	ASSERT_EQ(Phase3IdentityReconcileStatus::Bound,
		registry.reconcile_phase2_binding(key(50U, 1U), 100U));

	GlobalAllocationScope allocations;
	const auto existing = registry.resolve(key(50U, 1U));
	EXPECT_TRUE(registry.begin_transaction());
	const auto fresh = registry.resolve(key(51U, 2U));
	const auto bound = registry.reconcile_phase2_binding(key(52U, 3U), 200U);
	registry.rollback_transaction();
	EXPECT_TRUE(registry.begin_transaction());
	const auto committed = registry.resolve(key(54U, 2U));
	registry.commit_transaction();
	registry.reset_session();
	const auto after_reset = registry.resolve(key(53U, 4U));
	const auto allocation_count = allocations.finish();

	EXPECT_EQ(Phase3IdentityResolveStatus::Existing, existing.status);
	EXPECT_EQ(100U, existing.entity_id);
	EXPECT_EQ(Phase3IdentityResolveStatus::Allocated, fresh.status);
	EXPECT_EQ(101U, fresh.entity_id);
	EXPECT_EQ(Phase3IdentityReconcileStatus::Bound, bound);
	EXPECT_EQ(Phase3IdentityResolveStatus::Allocated, committed.status);
	EXPECT_EQ(101U, committed.entity_id);
	EXPECT_EQ(Phase3IdentityResolveStatus::Allocated, after_reset.status);
	EXPECT_EQ(1U, after_reset.entity_id);
	EXPECT_EQ(0U, allocation_count);
}

TEST(TelemetryPhase3Identity, MaximumReconciledIdClosesFurtherAllocationWithoutWrap)
{
	Phase3IdentityRegistry registry;
	ASSERT_EQ(Phase3IdentityProvisionStatus::Ready, registry.provision());
	const auto maximum = std::numeric_limits<std::uint64_t>::max();
	ASSERT_EQ(Phase3IdentityReconcileStatus::Bound,
		registry.reconcile_phase2_binding(key(70U), maximum));
	EXPECT_EQ(Phase3IdentityResolveStatus::Existing, registry.resolve(key(70U)).status);
	const auto exhausted = registry.resolve(key(71U));
	EXPECT_EQ(Phase3IdentityResolveStatus::CounterExhausted, exhausted.status);
	EXPECT_EQ(0U, exhausted.entity_id);
	EXPECT_EQ(maximum, registry.last_allocated_entity_id());
}

} // namespace
