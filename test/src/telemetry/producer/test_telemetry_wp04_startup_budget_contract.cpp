#include "telemetry/startup_budget.h"

#include "telemetry/protocol/telemetry_protocol_constants.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace {

namespace detail = telemetry::detail;
namespace protocol = telemetry::protocol;

constexpr std::size_t OneClientWp03KnownBytes = 38'797'312U;
constexpr std::uint16_t AllWp03DeferredCategories = 0x00ffU;
constexpr std::uint16_t Wp04DeferredCategories = 0x00fbU;

detail::Wp03KnownBudgetSubtotal one_client_wp03_subtotal()
{
	return detail::calculate_wp03_known_budget_subtotal(detail::make_wp03_known_budget_request(1U));
}

void expect_wp04_unpublished_failure(const detail::Wp03KnownBudgetSubtotal& result,
	detail::StartupBudgetError expected_error)
{
	EXPECT_EQ(expected_error, result.error);
	EXPECT_FALSE(result.is_complete);
	EXPECT_EQ(0U, result.known_bytes);
	EXPECT_EQ(0U, result.metric_known_bytes);
	EXPECT_EQ(0U, result.transport_buffer_bytes);
	EXPECT_EQ(0U, result.deferred_categories);
}

TEST(TelemetryWp04StartupBudgetContract, PricesExactlyTheTwoFixedApplicationDatagramBuffers)
{
	EXPECT_EQ(2U, detail::WP04TransportApplicationBufferCount);
	EXPECT_EQ(protocol::MaxDatagramSize, detail::WP04TransportApplicationBufferBytes);
	EXPECT_EQ(2U * protocol::MaxDatagramSize, detail::WP04TransportStorageBytes);
	EXPECT_EQ(2'400U, detail::WP04TransportStorageBytes);
}

TEST(TelemetryWp04StartupBudgetContract, CompositionPreservesTheWp03SubtotalAsHistoricalEvidence)
{
	const auto wp03 = one_client_wp03_subtotal();
	ASSERT_EQ(detail::StartupBudgetError::None, wp03.error);
	EXPECT_FALSE(wp03.is_complete);
	EXPECT_EQ(OneClientWp03KnownBytes, wp03.known_bytes);
	EXPECT_EQ(OneClientWp03KnownBytes, wp03.metric_known_bytes);
	EXPECT_EQ(0U, wp03.transport_buffer_bytes);
	EXPECT_EQ(AllWp03DeferredCategories, wp03.deferred_categories);
	EXPECT_TRUE(detail::startup_budget_category_is_deferred(
		wp03, detail::DeferredStartupBudgetCategory::TransportBuffers));
}

TEST(TelemetryWp04StartupBudgetContract, CompositionAddsSizeAndMetricBytesAndClearsOnlyTransportDeferral)
{
	const auto wp03 = one_client_wp03_subtotal();
	const auto wp04 = detail::apply_wp04_transport_budget(wp03,
		detail::WP04TransportApplicationBufferCount,
		detail::WP04TransportApplicationBufferBytes);

	ASSERT_EQ(detail::StartupBudgetError::None, wp04.error);
	EXPECT_FALSE(wp04.is_complete)
		<< "Seven storage owners remain deferred after the isolated WP04 transport tranche.";
	EXPECT_EQ(detail::WP04TransportStorageBytes, wp04.transport_buffer_bytes);
	EXPECT_EQ(OneClientWp03KnownBytes + detail::WP04TransportStorageBytes, wp04.known_bytes);
	EXPECT_EQ(static_cast<std::uint64_t>(OneClientWp03KnownBytes) + detail::WP04TransportStorageBytes,
		wp04.metric_known_bytes);
	EXPECT_EQ(Wp04DeferredCategories, wp04.deferred_categories);
	EXPECT_FALSE(detail::startup_budget_category_is_deferred(
		wp04, detail::DeferredStartupBudgetCategory::TransportBuffers));

	const std::array<detail::DeferredStartupBudgetCategory, 7> still_deferred{{
		detail::DeferredStartupBudgetCategory::ClientSlotStorage,
		detail::DeferredStartupBudgetCategory::StateReassemblyStorage,
		detail::DeferredStartupBudgetCategory::ReliableWindowStorage,
		detail::DeferredStartupBudgetCategory::BaselineStorage,
		detail::DeferredStartupBudgetCategory::DeltaStorage,
		detail::DeferredStartupBudgetCategory::SerializationScratch,
		detail::DeferredStartupBudgetCategory::Metrics,
	}};
	for (const auto category : still_deferred) {
		EXPECT_TRUE(detail::startup_budget_category_is_deferred(wp04, category));
	}
}

TEST(TelemetryWp04StartupBudgetContract, StandardWp04ReportComposesTheHistoricalRequest)
{
	const auto composed = detail::calculate_wp04_startup_budget(detail::make_wp03_known_budget_request(4U));
	ASSERT_EQ(detail::StartupBudgetError::None, composed.error);
	EXPECT_FALSE(composed.is_complete);
	EXPECT_EQ(152'043'520U + detail::WP04TransportStorageBytes, composed.known_bytes);
	EXPECT_EQ(static_cast<std::uint64_t>(152'043'520U) + detail::WP04TransportStorageBytes,
		composed.metric_known_bytes);
	EXPECT_EQ(detail::WP04TransportStorageBytes, composed.transport_buffer_bytes);
	EXPECT_EQ(Wp04DeferredCategories, composed.deferred_categories);
}

TEST(TelemetryWp04StartupBudgetContract, TransportProductAndBothAdditionDomainsFailWithoutPartialPublication)
{
	auto wp03 = one_client_wp03_subtotal();
	ASSERT_EQ(detail::StartupBudgetError::None, wp03.error);

	expect_wp04_unpublished_failure(
		detail::apply_wp04_transport_budget(
			wp03, std::numeric_limits<std::size_t>::max() / 2U + 1U, 2U),
		detail::StartupBudgetError::ArithmeticOverflow);

	auto size_overflow = wp03;
	size_overflow.known_bytes = std::numeric_limits<std::size_t>::max();
	expect_wp04_unpublished_failure(detail::apply_wp04_transport_budget(size_overflow, 1U, 1U),
		detail::StartupBudgetError::ArithmeticOverflow);

	auto metric_overflow = wp03;
	metric_overflow.metric_known_bytes = std::numeric_limits<std::uint64_t>::max();
	expect_wp04_unpublished_failure(detail::apply_wp04_transport_budget(metric_overflow, 1U, 1U),
		detail::StartupBudgetError::ArithmeticOverflow);
}

TEST(TelemetryWp04StartupBudgetContract, CompositionRetainsTheProvisionalCapAndRejectsInvalidPriorReports)
{
	auto capped = one_client_wp03_subtotal();
	capped.known_bytes = detail::WP03ProvisionalKnownBudgetCapBytes;
	capped.metric_known_bytes = detail::WP03ProvisionalKnownBudgetCapBytes;
	expect_wp04_unpublished_failure(detail::apply_wp04_transport_budget(capped, 1U, 1U),
		detail::StartupBudgetError::StaticCapExceeded);

	auto failed = one_client_wp03_subtotal();
	failed.error = detail::StartupBudgetError::InvalidClientCount;
	expect_wp04_unpublished_failure(
		detail::apply_wp04_transport_budget(failed, 2U, protocol::MaxDatagramSize),
		detail::StartupBudgetError::InvalidClientCount);
}

} // namespace
