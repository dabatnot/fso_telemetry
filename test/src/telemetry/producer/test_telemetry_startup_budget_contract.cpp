#include "telemetry/startup_budget.h"

#include "telemetry/protocol/telemetry_protocol_constants.h"
#include "telemetry/protocol/telemetry_reliable_window.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace {

namespace detail = telemetry::detail;
namespace protocol = telemetry::protocol;

constexpr std::size_t ProvisionalKnownCapBytes = 268'435'456U;
constexpr std::size_t SessionRegistrySlotCount = 131'072U;
constexpr std::size_t SessionRegistryBytes = SessionRegistrySlotCount * sizeof(std::uint64_t);

void expect_unpublished_failure(const detail::Wp03KnownBudgetSubtotal& result,
	detail::StartupBudgetError expected_error)
{
	EXPECT_EQ(expected_error, result.error);
	EXPECT_FALSE(result.is_complete);
	EXPECT_EQ(0U, result.known_bytes);
	EXPECT_EQ(0U, result.metric_known_bytes);
	EXPECT_EQ(0U, result.session_id_registry_bytes);
	EXPECT_EQ(0U, result.reassembly_bytes);
	EXPECT_EQ(0U, result.reliable_retention_projection_bytes);
	EXPECT_EQ(0U, result.client_slot_count);
	EXPECT_EQ(0U, result.reassembly_slot_count);
	EXPECT_EQ(0U, result.baseline_slot_count);
	EXPECT_EQ(0U, result.delta_slot_count);
}

TEST(TelemetryWp03KnownBudgetContract, ProvisionalKnownCapAndRealSessionRegistryRepresentationAreExplicit)
{
	EXPECT_EQ(ProvisionalKnownCapBytes, detail::WP03ProvisionalKnownBudgetCapBytes);
	EXPECT_EQ(SessionRegistrySlotCount, detail::SessionIdRegistryStorageSlotCount);
	EXPECT_EQ(SessionRegistryBytes, detail::SessionIdRegistryStorageBytes);
	EXPECT_EQ(1'048'576U, detail::SessionIdRegistryStorageBytes);
}

TEST(TelemetryWp03KnownBudgetContract, CheckedSizeArithmeticPublishesOnlySuccessfulAdditionsAndMultiplications)
{
	const auto maximum = std::numeric_limits<std::size_t>::max();
	std::size_t output = 0U;
	ASSERT_TRUE(detail::checked_add_size(maximum - 1U, 1U, output));
	EXPECT_EQ(maximum, output);
	output = 99U;
	EXPECT_FALSE(detail::checked_add_size(maximum, 1U, output));
	EXPECT_EQ(0U, output);

	ASSERT_TRUE(detail::checked_multiply_size(maximum, 1U, output));
	EXPECT_EQ(maximum, output);
	output = 99U;
	EXPECT_FALSE(detail::checked_multiply_size(maximum / 2U + 1U, 2U, output));
	EXPECT_EQ(0U, output);
}

TEST(TelemetryWp03KnownBudgetContract, CheckedMetricArithmeticUsesAnIndependentUint64Path)
{
	const auto maximum = std::numeric_limits<std::uint64_t>::max();
	std::uint64_t output = 0U;
	ASSERT_TRUE(detail::checked_add_metric_u64(maximum - 1U, 1U, output));
	EXPECT_EQ(maximum, output);
	output = 99U;
	EXPECT_FALSE(detail::checked_add_metric_u64(maximum, 1U, output));
	EXPECT_EQ(0U, output);

	ASSERT_TRUE(detail::checked_multiply_metric_u64(maximum, 1U, output));
	EXPECT_EQ(maximum, output);
	output = 99U;
	EXPECT_FALSE(detail::checked_multiply_metric_u64(maximum / 2U + 1U, 2U, output));
	EXPECT_EQ(0U, output);
}

TEST(TelemetryWp03KnownBudgetContract, Wp03RequestUsesInheritedKnownPerClientBounds)
{
	const auto request = detail::make_wp03_known_budget_request(1U);

	EXPECT_EQ(1U, request.max_clients);
	EXPECT_EQ(SessionRegistryBytes, request.session_id_registry_bytes);
	EXPECT_EQ(protocol::MaxStateReassemblyBytesPerClient, request.reassembly_bytes_per_client);
	EXPECT_EQ(protocol::ReliableWindowMaximumRetainedBytes,
		request.reliable_retention_projection_bytes_per_client);
}

TEST(TelemetryWp03KnownBudgetContract, KnownCountsScaleExactlyAndSharedReassemblyMemoryIsNotMultipliedBySlotCount)
{
	for (std::size_t clients = 1U; clients <= 4U; ++clients) {
		SCOPED_TRACE(clients);
		const auto request = detail::make_wp03_known_budget_request(clients);
		const auto result = detail::calculate_wp03_known_budget_subtotal(request);

		ASSERT_EQ(detail::StartupBudgetError::None, result.error);
		EXPECT_FALSE(result.is_complete);
		EXPECT_EQ(clients, result.client_slot_count);
		EXPECT_EQ(clients * 4U, result.reassembly_slot_count);
		EXPECT_EQ(clients * 2U, result.baseline_slot_count);
		EXPECT_EQ(clients * 2U, result.delta_slot_count);
		EXPECT_EQ(SessionRegistryBytes, result.session_id_registry_bytes);
		EXPECT_EQ(clients * protocol::MaxStateReassemblyBytesPerClient, result.reassembly_bytes);
		EXPECT_EQ(clients * protocol::ReliableWindowMaximumRetainedBytes,
			result.reliable_retention_projection_bytes);
		EXPECT_EQ(result.session_id_registry_bytes + result.reassembly_bytes +
				result.reliable_retention_projection_bytes,
			result.known_bytes);
		EXPECT_EQ(static_cast<std::uint64_t>(result.known_bytes), result.metric_known_bytes);
		EXPECT_LE(result.known_bytes, detail::WP03ProvisionalKnownBudgetCapBytes);
	}
}

TEST(TelemetryWp03KnownBudgetContract, FutureStorageCategoriesAreExplicitlyIncompleteInsteadOfImplicitZeroes)
{
	const auto result =
		detail::calculate_wp03_known_budget_subtotal(detail::make_wp03_known_budget_request(4U));
	ASSERT_EQ(detail::StartupBudgetError::None, result.error);
	EXPECT_FALSE(result.is_complete);
	EXPECT_EQ(8U, static_cast<std::uint8_t>(detail::DeferredStartupBudgetCategory::Count));
	EXPECT_EQ(0x00ffU, result.deferred_categories);

	const detail::DeferredStartupBudgetCategory required_open_categories[]{
		detail::DeferredStartupBudgetCategory::ClientSlotStorage,
		detail::DeferredStartupBudgetCategory::StateReassemblyStorage,
		detail::DeferredStartupBudgetCategory::TransportBuffers,
		detail::DeferredStartupBudgetCategory::ReliableWindowStorage,
		detail::DeferredStartupBudgetCategory::BaselineStorage,
		detail::DeferredStartupBudgetCategory::DeltaStorage,
		detail::DeferredStartupBudgetCategory::SerializationScratch,
		detail::DeferredStartupBudgetCategory::Metrics,
	};
	for (const auto category : required_open_categories) {
		EXPECT_TRUE(detail::startup_budget_category_is_deferred(result, category));
	}
}

TEST(TelemetryWp03KnownBudgetContract, KnownWp03ComponentsForOneAndFourClientsAreExactButNeverClaimCompleteness)
{
	const auto one =
		detail::calculate_wp03_known_budget_subtotal(detail::make_wp03_known_budget_request(1U));
	ASSERT_EQ(detail::StartupBudgetError::None, one.error);
	EXPECT_FALSE(one.is_complete);
	EXPECT_EQ(4'194'304U, one.reassembly_bytes);
	EXPECT_EQ(33'554'432U, one.reliable_retention_projection_bytes);
	EXPECT_EQ(38'797'312U, one.known_bytes);

	const auto four =
		detail::calculate_wp03_known_budget_subtotal(detail::make_wp03_known_budget_request(4U));
	ASSERT_EQ(detail::StartupBudgetError::None, four.error);
	EXPECT_FALSE(four.is_complete);
	EXPECT_EQ(16'777'216U, four.reassembly_bytes);
	EXPECT_EQ(134'217'728U, four.reliable_retention_projection_bytes);
	EXPECT_EQ(152'043'520U, four.known_bytes);
}

TEST(TelemetryWp03KnownBudgetContract, InvalidClientCountsFailWithoutClampingOrPartialPublication)
{
	const std::array<std::size_t, 3U> invalid_client_counts{
		0U, 5U, std::numeric_limits<std::size_t>::max()};
	for (const auto clients : invalid_client_counts) {
		SCOPED_TRACE(clients);
		const auto request = detail::make_wp03_known_budget_request(clients);
		expect_unpublished_failure(
			detail::calculate_wp03_known_budget_subtotal(request), detail::StartupBudgetError::InvalidClientCount);
	}
}

TEST(TelemetryWp03KnownBudgetContract, ProvisionalKnownCapMinusOneAndExactAreAcceptedButOneByteMoreIsRejected)
{
	detail::Wp03KnownBudgetRequest request;
	request.max_clients = 1U;
	request.session_id_registry_bytes = ProvisionalKnownCapBytes - 1U;
	const auto below = detail::calculate_wp03_known_budget_subtotal(request);
	ASSERT_EQ(detail::StartupBudgetError::None, below.error);
	EXPECT_FALSE(below.is_complete);
	EXPECT_EQ(ProvisionalKnownCapBytes - 1U, below.known_bytes);

	request.session_id_registry_bytes = ProvisionalKnownCapBytes;
	const auto exact = detail::calculate_wp03_known_budget_subtotal(request);
	ASSERT_EQ(detail::StartupBudgetError::None, exact.error);
	EXPECT_FALSE(exact.is_complete);
	EXPECT_EQ(ProvisionalKnownCapBytes, exact.known_bytes);

	request.reassembly_bytes_per_client = 1U;
	expect_unpublished_failure(
		detail::calculate_wp03_known_budget_subtotal(request), detail::StartupBudgetError::StaticCapExceeded);
}

TEST(TelemetryWp03KnownBudgetContract, EveryKnownComponentMultiplicationAndAdditionFailsClosedOnSizeOverflow)
{
	detail::Wp03KnownBudgetRequest reassembly_product;
	reassembly_product.max_clients = 4U;
	reassembly_product.reassembly_bytes_per_client = std::numeric_limits<std::size_t>::max() / 4U + 1U;
	expect_unpublished_failure(detail::calculate_wp03_known_budget_subtotal(reassembly_product),
		detail::StartupBudgetError::ArithmeticOverflow);

	detail::Wp03KnownBudgetRequest reliable_product;
	reliable_product.max_clients = 4U;
	reliable_product.reliable_retention_projection_bytes_per_client =
		std::numeric_limits<std::size_t>::max() / 4U + 1U;
	expect_unpublished_failure(detail::calculate_wp03_known_budget_subtotal(reliable_product),
		detail::StartupBudgetError::ArithmeticOverflow);

	detail::Wp03KnownBudgetRequest total_sum;
	total_sum.max_clients = 1U;
	total_sum.session_id_registry_bytes = std::numeric_limits<std::size_t>::max();
	total_sum.reassembly_bytes_per_client = 1U;
	expect_unpublished_failure(detail::calculate_wp03_known_budget_subtotal(total_sum),
		detail::StartupBudgetError::ArithmeticOverflow);

	detail::Wp03KnownBudgetRequest second_total_sum;
	second_total_sum.max_clients = 1U;
	second_total_sum.session_id_registry_bytes = std::numeric_limits<std::size_t>::max() - 1U;
	second_total_sum.reassembly_bytes_per_client = 1U;
	second_total_sum.reliable_retention_projection_bytes_per_client = 1U;
	expect_unpublished_failure(detail::calculate_wp03_known_budget_subtotal(second_total_sum),
		detail::StartupBudgetError::ArithmeticOverflow);
}

} // namespace
