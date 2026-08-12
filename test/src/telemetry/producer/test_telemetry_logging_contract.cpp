#include "telemetry/logging.h"

#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <string_view>
#include <type_traits>

namespace {

namespace detail = telemetry::detail;

TEST(TelemetryP92LoggingContract, FixedSchemaUsesOnlyClosedEventLevelAndReasonFields)
{
	static_assert(std::is_trivially_copyable_v<detail::TelemetryLogRecord>);
	static_assert(std::is_trivially_copyable_v<detail::TelemetryLogSnapshot>);
	EXPECT_EQ(24U, static_cast<std::size_t>(detail::TelemetryLogEvent::Count));
	EXPECT_EQ(14U, static_cast<std::size_t>(detail::TelemetryLogReason::Count));
	EXPECT_EQ(3U, static_cast<std::size_t>(detail::TelemetryLogFamily::DualStack));
	EXPECT_EQ(6U, static_cast<std::size_t>(detail::TelemetryLogBudget::Count));
	EXPECT_EQ(6U, static_cast<std::size_t>(detail::TelemetryLogDrop::Count));

	detail::TelemetryStructuredLog log;
	for (std::uint64_t generation = 0U; generation <= detail::TelemetryLogRecordCapacity; ++generation) {
		log.mission_entered(generation);
	}
	const auto snapshot = log.snapshot();
	EXPECT_EQ(detail::TelemetryLogRecordCapacity, snapshot.count);
	EXPECT_EQ(1U, snapshot.dropped_records);
	EXPECT_EQ(detail::TelemetryLogEvent::MissionEntered, snapshot.records.front().event);
	EXPECT_EQ(0U, snapshot.records.front().value);
}

TEST(TelemetryP92LoggingContract, Phase3FailureCarriesOnlyClosedBlockAndReason)
{
	detail::TelemetryStructuredLog log;
	log.phase3_source_rejected(2U, detail::TelemetryPhase3Block::Radar,
		detail::TelemetryPhase3CaptureFailure::InvalidSource);

	const auto snapshot = log.snapshot();
	ASSERT_EQ(1U, snapshot.count);
	const auto& record = snapshot.records[0];
	EXPECT_EQ(detail::TelemetryLogEvent::Phase3SourceRejected, record.event);
	EXPECT_EQ(detail::TelemetryLogLevel::Error, record.level);
	EXPECT_EQ(3U, record.correlation_slot);
	EXPECT_EQ(detail::TelemetryPhase3Block::Radar, record.phase3_block);
	EXPECT_EQ(detail::TelemetryPhase3CaptureFailure::InvalidSource,
		record.phase3_capture_failure);

	std::array<char, detail::TelemetryLogLineCapacity> line{};
	ASSERT_TRUE(detail::format_telemetry_log_record(record, line));
	const std::string_view rendered{line.data()};
	EXPECT_NE(std::string_view::npos,
		rendered.find("p3_block=" + std::to_string(
			static_cast<unsigned>(detail::TelemetryPhase3Block::Radar))));
	EXPECT_NE(std::string_view::npos,
		rendered.find("p3_capture_failure=" + std::to_string(
			static_cast<unsigned>(detail::TelemetryPhase3CaptureFailure::InvalidSource))));
}

TEST(TelemetryP92LoggingContract,
	TerminalPhase3CauseSurvivesAFullDeliveryQueue)
{
	detail::TelemetryStructuredLog log;
	for (std::uint64_t generation = 0U;
		 generation < detail::TelemetryLogRecordCapacity; ++generation) {
		log.mission_entered(generation);
	}
	log.phase3_source_rejected(1U, detail::TelemetryPhase3Block::Threat,
		detail::TelemetryPhase3CaptureFailure::InvalidSource);
	log.transport_fault(detail::TelemetryLogFamily::DualStack,
		detail::TelemetryLogReason::ProtocolError, 0U,
		detail::TelemetryLogFault::Capture);

	const auto snapshot = log.snapshot();
	ASSERT_EQ(detail::TelemetryLogRecordCapacity, snapshot.count);
	EXPECT_EQ(2U, snapshot.dropped_records);
	ASSERT_EQ(detail::TelemetryTerminalLogRecordCapacity,
		snapshot.terminal_count);
	EXPECT_EQ(detail::TelemetryLogEvent::Phase3SourceRejected,
		snapshot.terminal_records[0].event);
	EXPECT_EQ(detail::TelemetryPhase3Block::Threat,
		snapshot.terminal_records[0].phase3_block);
	EXPECT_EQ(detail::TelemetryPhase3CaptureFailure::InvalidSource,
		snapshot.terminal_records[0].phase3_capture_failure);
	EXPECT_EQ(detail::TelemetryLogEvent::TransportFault,
		snapshot.terminal_records[1].event);
	EXPECT_EQ(detail::TelemetryLogFault::Capture,
		snapshot.terminal_records[1].fault);
	EXPECT_EQ(0U, snapshot.superseded_terminal_records);
}

TEST(TelemetryP92LoggingContract, AggregatesDropsAndValidationAtMostOncePerSecond)
{
	detail::TelemetryStructuredLog log;
	log.record_drop(detail::TelemetryLogDrop::Validation);
	log.record_drop(detail::TelemetryLogDrop::Validation);
	log.record_drop(detail::TelemetryLogDrop::RateLimit);
	log.flush_drop_summary(0U);
	const auto first = log.snapshot();
	ASSERT_EQ(1U, first.count);
	EXPECT_EQ(detail::TelemetryLogEvent::DropSummary, first.records[0].event);
	EXPECT_EQ(2U, first.records[0].drops[static_cast<std::size_t>(detail::TelemetryLogDrop::Validation)]);
	EXPECT_EQ(1U, first.records[0].drops[static_cast<std::size_t>(detail::TelemetryLogDrop::RateLimit)]);

	log.record_drop(detail::TelemetryLogDrop::WouldBlock);
	log.flush_drop_summary(999'999U);
	EXPECT_EQ(1U, log.snapshot().count);
	log.flush_drop_summary(1'000'000U);
	const auto second = log.snapshot();
	ASSERT_EQ(2U, second.count);
	EXPECT_EQ(1U, second.records[1].drops[static_cast<std::size_t>(detail::TelemetryLogDrop::WouldBlock)]);
}

TEST(TelemetryP92LoggingContract, RecordsAreBoundedNumericAndCannotRetainForbiddenTextualData)
{
	static_assert(!std::is_constructible_v<detail::TelemetryLogRecord, const char*>);
	static_assert(!std::is_constructible_v<detail::TelemetryLogRecord, std::string_view>);
	detail::TelemetryStructuredLog log;
	log.transport_fault(detail::TelemetryLogFamily::Ipv6, detail::TelemetryLogReason::Send, 0xC0000005U);
	const auto snapshot = log.snapshot();
	ASSERT_EQ(1U, snapshot.count);
	const auto& record = snapshot.records[0];
	EXPECT_EQ(detail::TelemetryLogEvent::TransportFault, record.event);
	EXPECT_EQ(detail::TelemetryLogReason::Send, record.reason);
	EXPECT_EQ(detail::TelemetryLogFamily::Ipv6, record.family);
	EXPECT_EQ(0xC0000005U, record.platform_code);
	EXPECT_EQ(0U, record.correlation_slot);
	EXPECT_EQ(0U, record.port);
}

TEST(TelemetryP92LoggingContract, LifecycleProducesOneBoundedStartupSessionAndShutdownSummary)
{
	detail::TelemetryStructuredLog log;
	log.config_invalid(detail::TelemetryLogReason::ConfigSecurity);
	log.config_disabled();
	log.session_opened(0U);
	log.session_opened(0U);
	log.session_closed(0U, detail::TelemetryLogReason::ProtocolError, 41U, 7U);
	log.session_closed(0U, detail::TelemetryLogReason::ProtocolError, 42U, 8U);
	log.shutdown(1'000U, 3U);
	log.shutdown(2'000U, 4U);
	const auto snapshot = log.snapshot();
	ASSERT_EQ(4U, snapshot.count);
	EXPECT_EQ(detail::TelemetryLogEvent::ConfigInvalid, snapshot.records[0].event);
	EXPECT_EQ(detail::TelemetryLogEvent::SessionOpened, snapshot.records[1].event);
	EXPECT_EQ(1U, snapshot.records[1].correlation_slot);
	EXPECT_EQ(detail::TelemetryLogEvent::SessionClosed, snapshot.records[2].event);
	EXPECT_EQ(41U, snapshot.records[2].value);
	EXPECT_EQ(detail::TelemetryLogEvent::Shutdown, snapshot.records[3].event);
	EXPECT_EQ(1'000U, snapshot.records[3].value);
}

} // namespace
