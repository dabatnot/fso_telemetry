#include "telemetry/metrics.h"
#include "telemetry/startup_budget.h"

#include <gtest/gtest.h>

#include <limits>
#include <type_traits>

namespace {

namespace detail = telemetry::detail;

constexpr auto counter_index(detail::TelemetryMetricCounter counter)
{
	return static_cast<std::size_t>(counter);
}

constexpr auto histogram_index(detail::TelemetryMetricHistogram histogram)
{
	return static_cast<std::size_t>(histogram);
}

TEST(TelemetryP91MetricsContract, FixedCardinalitySnapshotAndClosedLabelCatalogAreExact)
{
	static_assert(std::is_trivially_copyable_v<detail::TelemetryMetricsSnapshot>);
	EXPECT_EQ(5U, static_cast<std::size_t>(detail::TelemetryCallbackKind::Count));
	EXPECT_EQ(2U, static_cast<std::size_t>(detail::TelemetryDirection::Count));
	EXPECT_EQ(4U, static_cast<std::size_t>(detail::TelemetryIoResult::Count));
	EXPECT_EQ(3U, static_cast<std::size_t>(detail::TelemetryCaptureResult::Count));
	EXPECT_EQ(3U, static_cast<std::size_t>(detail::TelemetrySnapshotKind::Count));
	EXPECT_EQ(6U, static_cast<std::size_t>(detail::TelemetryPendingKind::Count));
	EXPECT_EQ(5U, static_cast<std::size_t>(detail::TelemetryDeltaDropReason::Count));
	EXPECT_EQ(6U, static_cast<std::size_t>(detail::TelemetrySessionEndReason::Count));
	EXPECT_EQ(7U, static_cast<std::size_t>(detail::TelemetryRuntimeFaultReason::Count));
	EXPECT_EQ(9U, detail::TelemetryMetricHistogramBucketCount);
	EXPECT_EQ(4U, detail::TelemetryMetricsMaxClients);

	detail::TelemetryMetrics metrics;
	ASSERT_TRUE(metrics.provision());
	const auto snapshot = metrics.snapshot();
	EXPECT_TRUE(snapshot.provisioned);
	EXPECT_EQ(detail::TelemetryMetricsMaxClients, snapshot.sessions.size());
	EXPECT_EQ(static_cast<std::size_t>(detail::TelemetryMetricCounter::Count), snapshot.process_counters.size());
	EXPECT_EQ(static_cast<std::size_t>(detail::TelemetryMetricHistogram::Count), snapshot.process_histograms.size());
	EXPECT_EQ(detail::TelemetryMetrics::StorageBytes, metrics.owned_bytes());
}

TEST(TelemetryP91MetricsContract, ProcessTotalsSurviveWhileSessionAndMissionScopesReset)
{
	detail::TelemetryMetrics metrics;
	ASSERT_TRUE(metrics.provision());
	metrics.activate_session(0U, 7U);
	metrics.increment_session(0U, detail::TelemetryMetricCounter::HeartbeatProbes, 3U);
	metrics.observe_session(0U, detail::TelemetryMetricHistogram::SerializationDuration, 25U);
	metrics.observe_mission(detail::TelemetryMetricHistogram::CaptureDuration, 10U);
	metrics.set_current_player_entity_id(42U);

	const auto before = metrics.snapshot();
	EXPECT_EQ(3U, before.process_counters[counter_index(detail::TelemetryMetricCounter::HeartbeatProbes)]);
	EXPECT_EQ(3U, before.sessions[0].counters[counter_index(detail::TelemetryMetricCounter::HeartbeatProbes)]);
	EXPECT_EQ(1U, before.mission_histograms[histogram_index(detail::TelemetryMetricHistogram::CaptureDuration)].count);
	EXPECT_EQ(42U, before.current_player_entity_id);

	metrics.reset_session(0U);
	metrics.reset_mission();
	const auto after = metrics.snapshot();
	EXPECT_EQ(3U, after.process_counters[counter_index(detail::TelemetryMetricCounter::HeartbeatProbes)]);
	EXPECT_FALSE(after.sessions[0].active);
	EXPECT_EQ(0U, after.sessions[0].counters[counter_index(detail::TelemetryMetricCounter::HeartbeatProbes)]);
	EXPECT_EQ(0U, after.sessions[0].histograms[histogram_index(detail::TelemetryMetricHistogram::SerializationDuration)].count);
	EXPECT_EQ(0U, after.mission_histograms[histogram_index(detail::TelemetryMetricHistogram::CaptureDuration)].count);
	EXPECT_EQ(0U, after.current_player_entity_id);
}

TEST(TelemetryP91MetricsContract, ScalarDiagnosticsDoNotRequireCopyingTheMetricsSnapshot)
{
	detail::TelemetryMetrics metrics;
	ASSERT_TRUE(metrics.provision());
	metrics.increment_process(detail::TelemetryMetricCounter::SessionsEnded, 3U);
	metrics.set_phase2_memory(detail::TelemetryPhase2MemoryScope::ProcessTotal, 64U);
	metrics.set_phase2_memory(detail::TelemetryPhase2MemoryScope::ProcessTotal, 16U);

	EXPECT_EQ(3U, metrics.process_counter(detail::TelemetryMetricCounter::SessionsEnded));
	EXPECT_EQ(64U, metrics.phase2_memory_high_water(
		detail::TelemetryPhase2MemoryScope::ProcessTotal));
}

TEST(TelemetryP91MetricsContract, CounterAndHistogramSumsSaturateAndAccountForOverflow)
{
	detail::TelemetryMetrics metrics;
	ASSERT_TRUE(metrics.provision());
	constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
	metrics.increment_process(detail::TelemetryMetricCounter::Datagrams, maximum);
	metrics.increment_process(detail::TelemetryMetricCounter::Datagrams, 1U);
	metrics.observe_process(detail::TelemetryMetricHistogram::TickDuration, maximum);
	metrics.observe_process(detail::TelemetryMetricHistogram::TickDuration, 1U);

	const auto snapshot = metrics.snapshot();
	EXPECT_EQ(maximum, snapshot.process_counters[counter_index(detail::TelemetryMetricCounter::Datagrams)]);
	EXPECT_GE(snapshot.process_counters[counter_index(detail::TelemetryMetricCounter::CounterOverflow)], 2U);
	const auto& histogram = snapshot.process_histograms[histogram_index(detail::TelemetryMetricHistogram::TickDuration)];
	EXPECT_EQ(2U, histogram.count);
	EXPECT_EQ(maximum, histogram.sum_us);
	EXPECT_EQ(1U, histogram.buckets.front());
	EXPECT_EQ(1U, histogram.buckets.back());
}

TEST(TelemetryP91MetricsContract, MetricsBudgetClearsFinalDeferralOnlyAfterProvisionedStorageMatches)
{
	const auto wp08 = detail::calculate_wp06_startup_budget(
		detail::calculate_wp04_startup_budget(detail::make_wp03_known_budget_request(1U)), 1U);
	ASSERT_EQ(detail::StartupBudgetError::None, wp08.error);
	ASSERT_TRUE(detail::startup_budget_category_is_deferred(wp08, detail::DeferredStartupBudgetCategory::Metrics));
	ASSERT_FALSE(wp08.is_complete);

	detail::TelemetryMetrics metrics;
	const auto unprovisioned_budget = detail::calculate_wp09_startup_budget(wp08, metrics);
	EXPECT_TRUE(detail::startup_budget_category_is_deferred(
		unprovisioned_budget, detail::DeferredStartupBudgetCategory::Metrics));
	EXPECT_FALSE(unprovisioned_budget.is_complete);
	EXPECT_FALSE(detail::wp09_budget_matches_metrics(unprovisioned_budget, metrics));

	ASSERT_TRUE(metrics.provision());
	const auto wp09 = detail::calculate_wp09_startup_budget(wp08, metrics);
	ASSERT_EQ(detail::StartupBudgetError::None, wp09.error);
	EXPECT_EQ(detail::TelemetryMetrics::StorageBytes, wp09.metrics_bytes);
	EXPECT_FALSE(detail::startup_budget_category_is_deferred(wp09, detail::DeferredStartupBudgetCategory::Metrics));
	EXPECT_EQ(0U, wp09.deferred_categories);
	EXPECT_TRUE(wp09.is_complete);

	EXPECT_TRUE(detail::wp09_budget_matches_metrics(wp09, metrics));
}

} // namespace
