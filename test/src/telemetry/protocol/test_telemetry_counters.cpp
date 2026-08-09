#include "telemetry/protocol/telemetry_counters.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>

namespace {

using namespace telemetry::protocol;

TEST(TelemetryProtocolCounters, SerialU32OrderingHandlesWrapAndExactHalfRange)
{
	EXPECT_EQ(SerialU32Order::Equal, compare_serial_u32(0U, 0U));
	EXPECT_EQ(SerialU32Order::Newer, compare_serial_u32(1U, 0U));
	EXPECT_EQ(SerialU32Order::Older, compare_serial_u32(0U, 1U));
	EXPECT_EQ(SerialU32Order::Newer, compare_serial_u32(0U, std::numeric_limits<std::uint32_t>::max()));
	EXPECT_EQ(SerialU32Order::Older, compare_serial_u32(std::numeric_limits<std::uint32_t>::max(), 0U));
	EXPECT_EQ(SerialU32Order::Ambiguous, compare_serial_u32(0x80000000U, 0U));
	EXPECT_EQ(SerialU32Order::Ambiguous, compare_serial_u32(0U, 0x80000000U));
	EXPECT_TRUE(serial_u32_newer(0U, std::numeric_limits<std::uint32_t>::max()));
	EXPECT_FALSE(serial_u32_newer(0x80000000U, 0U));
}

TEST(TelemetryProtocolCounters, NoWrapCounterEmitsMaxOnceThenPreservesOutputForever)
{
	NoWrapCounterU32 counter;
	std::uint32_t value = 99U;
	ASSERT_EQ(CounterNextResult::Value, counter.take_next(value));
	EXPECT_EQ(1U, value);
	ASSERT_EQ(CounterNextResult::Value, counter.take_next(value));
	EXPECT_EQ(2U, value);

	EXPECT_FALSE(counter.reset(0U));
	ASSERT_EQ(CounterNextResult::Value, counter.take_next(value));
	EXPECT_EQ(3U, value);

	const auto maximum = std::numeric_limits<std::uint32_t>::max();
	ASSERT_TRUE(counter.reset(maximum));
	ASSERT_EQ(CounterNextResult::Value, counter.take_next(value));
	EXPECT_EQ(maximum, value);
	EXPECT_TRUE(counter.exhausted());
	value = 77U;
	EXPECT_EQ(CounterNextResult::Exhausted, counter.take_next(value));
	EXPECT_EQ(77U, value);
	EXPECT_EQ(CounterNextResult::Exhausted, counter.take_next(value));
	EXPECT_EQ(77U, value);

	NoWrapCounterU64 counter64;
	std::uint64_t value64 = 0U;
	ASSERT_TRUE(counter64.reset(std::numeric_limits<std::uint64_t>::max()));
	ASSERT_EQ(CounterNextResult::Value, counter64.take_next(value64));
	EXPECT_EQ(std::numeric_limits<std::uint64_t>::max(), value64);
	EXPECT_TRUE(counter64.exhausted());
}

TEST(TelemetryProtocolCounters, ProbeTrackerRequiresSessionAndPreservesTokenAtCapacity)
{
	ProbeTracker tracker;
	ProbeToken token{91U, 92U, 93U};
	EXPECT_EQ(ProbeStartResult::NoSession, tracker.begin_probe(10U, token));
	EXPECT_EQ(91U, token.session_id);
	EXPECT_EQ(92U, token.probe_id);
	EXPECT_EQ(93U, token.origin_t0_us);

	ASSERT_TRUE(tracker.reset_session(0x1122334455667788ULL));
	std::array<ProbeToken, MaxInFlightProbes> tokens{};
	for (std::size_t index = 0; index < tokens.size(); ++index) {
		ASSERT_EQ(ProbeStartResult::Started, tracker.begin_probe(100U + index, tokens[index]));
		EXPECT_EQ(tracker.session_id(), tokens[index].session_id);
		EXPECT_EQ(index + 1U, tokens[index].probe_id);
	}
	EXPECT_TRUE(tracker.full());
	EXPECT_EQ(MaxInFlightProbes, tracker.in_flight_count());
	token = ProbeToken{91U, 92U, 93U};
	EXPECT_EQ(ProbeStartResult::CapacityReached, tracker.begin_probe(999U, token));
	EXPECT_EQ(91U, token.session_id);
	EXPECT_EQ(92U, token.probe_id);
	EXPECT_EQ(93U, token.origin_t0_us);

	ASSERT_EQ(ProbeResponseResult::Matched,
		tracker.correlate_response(tokens[3].session_id, tokens[3].probe_id, tokens[3].origin_t0_us));
	EXPECT_EQ(MaxInFlightProbes - 1U, tracker.in_flight_count());
	ASSERT_EQ(ProbeStartResult::Started, tracker.begin_probe(1000U, token));
	EXPECT_EQ(9U, token.probe_id);
}

TEST(TelemetryProtocolCounters, ProbeCorrelationOnlyConsumesAnExactSessionIdAndTimestampMatch)
{
	ProbeTracker tracker;
	ASSERT_TRUE(tracker.reset_session(55U));
	ProbeToken token;
	ASSERT_EQ(ProbeStartResult::Started, tracker.begin_probe(1234U, token));

	EXPECT_EQ(ProbeResponseResult::SessionMismatch,
		tracker.correlate_response(56U, token.probe_id, token.origin_t0_us));
	EXPECT_EQ(1U, tracker.in_flight_count());
	EXPECT_EQ(ProbeResponseResult::UnknownProbeId,
		tracker.correlate_response(55U, token.probe_id + 1U, token.origin_t0_us));
	EXPECT_EQ(1U, tracker.in_flight_count());
	EXPECT_EQ(ProbeResponseResult::OriginTimestampMismatch,
		tracker.correlate_response(55U, token.probe_id, token.origin_t0_us + 1U));
	EXPECT_EQ(1U, tracker.in_flight_count());
	EXPECT_FALSE(tracker.discard_probe(56U, token.probe_id, token.origin_t0_us));
	EXPECT_FALSE(tracker.discard_probe(55U, token.probe_id, token.origin_t0_us + 1U));
	EXPECT_EQ(1U, tracker.in_flight_count());

	EXPECT_EQ(ProbeResponseResult::Matched, tracker.correlate_response(55U, token.probe_id, token.origin_t0_us));
	EXPECT_EQ(0U, tracker.in_flight_count());
	EXPECT_EQ(ProbeResponseResult::UnknownProbeId, tracker.correlate_response(55U, token.probe_id, token.origin_t0_us));

	ASSERT_EQ(ProbeStartResult::Started, tracker.begin_probe(999U, token));
	EXPECT_TRUE(tracker.discard_probe(55U, token.probe_id, token.origin_t0_us));
	EXPECT_EQ(0U, tracker.in_flight_count());
}

TEST(TelemetryProtocolCounters, ProbePreviewMatchesWithoutConsumingUntilCommit)
{
	ProbeTracker tracker;
	ASSERT_TRUE(tracker.reset_session(77U));
	ProbeToken token;
	ASSERT_EQ(ProbeStartResult::Started, tracker.begin_probe(4321U, token));

	EXPECT_EQ(ProbeResponseResult::Matched,
		tracker.preview_response(token.session_id, token.probe_id, token.origin_t0_us));
	EXPECT_EQ(1U, tracker.in_flight_count());
	EXPECT_EQ(ProbeResponseResult::OriginTimestampMismatch,
		tracker.preview_response(token.session_id, token.probe_id, token.origin_t0_us + 1U));
	EXPECT_EQ(1U, tracker.in_flight_count());

	EXPECT_EQ(ProbeResponseResult::Matched,
		tracker.correlate_response(token.session_id, token.probe_id, token.origin_t0_us));
	EXPECT_EQ(0U, tracker.in_flight_count());
}

TEST(TelemetryProtocolCounters, ProbeIdsSkipZeroAtWrapAndSessionResetPurgesOldSlots)
{
	ProbeTracker tracker;
	const auto maximum = std::numeric_limits<std::uint32_t>::max();
	ASSERT_TRUE(tracker.reset_session(7U, maximum));
	ProbeToken maximum_token;
	ProbeToken wrapped_token;
	ASSERT_EQ(ProbeStartResult::Started, tracker.begin_probe(10U, maximum_token));
	ASSERT_EQ(ProbeStartResult::Started, tracker.begin_probe(20U, wrapped_token));
	EXPECT_EQ(maximum, maximum_token.probe_id);
	EXPECT_EQ(1U, wrapped_token.probe_id);
	EXPECT_NE(0U, wrapped_token.probe_id);

	EXPECT_FALSE(tracker.reset_session(8U, 0U));
	EXPECT_EQ(7U, tracker.session_id());
	EXPECT_EQ(2U, tracker.in_flight_count());

	ASSERT_TRUE(tracker.reset_session(8U, 5U));
	EXPECT_EQ(8U, tracker.session_id());
	EXPECT_EQ(0U, tracker.in_flight_count());
	EXPECT_EQ(ProbeResponseResult::SessionMismatch,
		tracker.correlate_response(7U, maximum_token.probe_id, maximum_token.origin_t0_us));
	ProbeToken next;
	ASSERT_EQ(ProbeStartResult::Started, tracker.begin_probe(30U, next));
	EXPECT_EQ(5U, next.probe_id);

	ASSERT_TRUE(tracker.reset_session(0U));
	EXPECT_EQ(0U, tracker.session_id());
	EXPECT_EQ(0U, tracker.in_flight_count());
	EXPECT_EQ(ProbeStartResult::NoSession, tracker.begin_probe(40U, next));
	EXPECT_EQ(ProbeResponseResult::NoSession, tracker.correlate_response(0U, 1U, 40U));
}

} // namespace
