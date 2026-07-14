#include "telemetry/protocol/telemetry_clock.h"
#include "telemetry/protocol/telemetry_control_messages.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace {

using namespace telemetry::protocol;

TEST(TelemetryProtocolClock, ComputesFourTimestampSampleAndPreservesOutputOnInvalidSequences)
{
	FourTimestampExchange exchange;
	exchange.initiator_send_t0_us = 100U;
	exchange.responder_receive_t1_us = 150U;
	exchange.responder_transmit_t2_us = 160U;
	exchange.initiator_receive_t3_us = 230U;
	ClockSample sample{99, 88U};
	ASSERT_EQ(ClockSampleResult::Valid, compute_clock_sample(exchange, sample));
	EXPECT_EQ(-10, sample.responder_minus_initiator_offset_us);
	EXPECT_EQ(120U, sample.round_trip_time_us);

	const ClockSample canary{99, 88U};
	exchange.responder_transmit_t2_us = exchange.responder_receive_t1_us - 1U;
	sample = canary;
	EXPECT_EQ(ClockSampleResult::ResponderClockMovedBackward, compute_clock_sample(exchange, sample));
	EXPECT_EQ(canary.responder_minus_initiator_offset_us, sample.responder_minus_initiator_offset_us);
	EXPECT_EQ(canary.round_trip_time_us, sample.round_trip_time_us);

	exchange = FourTimestampExchange{100U, 100U, 100U, 99U};
	sample = canary;
	EXPECT_EQ(ClockSampleResult::InitiatorClockMovedBackward, compute_clock_sample(exchange, sample));
	EXPECT_EQ(canary.responder_minus_initiator_offset_us, sample.responder_minus_initiator_offset_us);

	exchange = FourTimestampExchange{100U, 100U, 201U, 200U};
	sample = canary;
	EXPECT_EQ(ClockSampleResult::NegativeRoundTrip, compute_clock_sample(exchange, sample));
	EXPECT_EQ(canary.round_trip_time_us, sample.round_trip_time_us);
}

TEST(TelemetryProtocolClock, SupportsFullUint64RttAndExactSignedOffsetBoundaries)
{
	const auto int64_max = std::numeric_limits<std::int64_t>::max();
	const auto int64_min = std::numeric_limits<std::int64_t>::min();
	const auto uint64_max = std::numeric_limits<std::uint64_t>::max();
	const auto two_to_63 = std::uint64_t{1} << 63U;
	ClockSample sample;

	FourTimestampExchange exchange{0U,
		static_cast<std::uint64_t>(int64_max),
		static_cast<std::uint64_t>(int64_max),
		0U};
	ASSERT_EQ(ClockSampleResult::Valid, compute_clock_sample(exchange, sample));
	EXPECT_EQ(int64_max, sample.responder_minus_initiator_offset_us);
	EXPECT_EQ(0U, sample.round_trip_time_us);

	exchange = FourTimestampExchange{two_to_63, 0U, 0U, two_to_63};
	ASSERT_EQ(ClockSampleResult::Valid, compute_clock_sample(exchange, sample));
	EXPECT_EQ(int64_min, sample.responder_minus_initiator_offset_us);
	EXPECT_EQ(0U, sample.round_trip_time_us);

	exchange = FourTimestampExchange{0U, two_to_63, two_to_63, 0U};
	sample = ClockSample{17, 19U};
	EXPECT_EQ(ClockSampleResult::OffsetOutOfRange, compute_clock_sample(exchange, sample));
	EXPECT_EQ(17, sample.responder_minus_initiator_offset_us);
	EXPECT_EQ(19U, sample.round_trip_time_us);

	exchange = FourTimestampExchange{uint64_max, 0U, 0U, uint64_max};
	EXPECT_EQ(ClockSampleResult::OffsetOutOfRange, compute_clock_sample(exchange, sample));

	exchange = FourTimestampExchange{0U, uint64_max / 2U, uint64_max / 2U, uint64_max};
	ASSERT_EQ(ClockSampleResult::Valid, compute_clock_sample(exchange, sample));
	EXPECT_EQ(0, sample.responder_minus_initiator_offset_us);
	EXPECT_EQ(uint64_max, sample.round_trip_time_us);
}

TEST(TelemetryProtocolClock, OrientsGenericOffsetWithoutSilentlyNegatingInt64Min)
{
	ClockSample sample{25, 100U};
	OrientedClockSample oriented{77, 88U};
	ASSERT_EQ(ClockOffsetConversionResult::Converted, orient_clock_sample(sample, LocalClockRole::Initiator, oriented));
	EXPECT_EQ(25, oriented.peer_minus_local_offset_us);
	EXPECT_EQ(100U, oriented.round_trip_time_us);

	ASSERT_EQ(ClockOffsetConversionResult::Converted, orient_clock_sample(sample, LocalClockRole::Responder, oriented));
	EXPECT_EQ(-25, oriented.peer_minus_local_offset_us);

	oriented = OrientedClockSample{77, 88U};
	EXPECT_EQ(ClockOffsetConversionResult::InvalidRole,
		orient_clock_sample(sample, static_cast<LocalClockRole>(0xffU), oriented));
	EXPECT_EQ(77, oriented.peer_minus_local_offset_us);
	EXPECT_EQ(88U, oriented.round_trip_time_us);

	sample.responder_minus_initiator_offset_us = std::numeric_limits<std::int64_t>::min();
	EXPECT_EQ(ClockOffsetConversionResult::OffsetOutOfRange,
		orient_clock_sample(sample, LocalClockRole::Responder, oriented));
	EXPECT_EQ(77, oriented.peer_minus_local_offset_us);
}

TEST(TelemetryProtocolClock, FilterUsesMinimumRttSmoothsByOneEighthAndPurgesOnInvalidate)
{
	ClockFilter filter;
	std::int64_t offset = 111;
	std::uint64_t rtt = 222U;
	std::uint64_t peer_time = 333U;
	EXPECT_FALSE(filter.smoothed_offset_us(offset));
	EXPECT_EQ(111, offset);
	EXPECT_FALSE(filter.minimum_round_trip_time_us(rtt));
	EXPECT_EQ(222U, rtt);
	EXPECT_EQ(PeerTimeEstimateResult::FilterInvalid, filter.estimate_peer_time_us(10U, peer_time));
	EXPECT_EQ(333U, peer_time);

	filter.add_sample(OrientedClockSample{80, 20U});
	ASSERT_TRUE(filter.smoothed_offset_us(offset));
	EXPECT_EQ(80, offset);
	ASSERT_TRUE(filter.minimum_round_trip_time_us(rtt));
	EXPECT_EQ(20U, rtt);

	// A higher-RTT sample leaves the minimum candidate at 80, so smoothing is
	// stable. The next lower-RTT candidate moves 1/8 of the 80-us distance.
	filter.add_sample(OrientedClockSample{800, 30U});
	ASSERT_TRUE(filter.smoothed_offset_us(offset));
	EXPECT_EQ(80, offset);
	filter.add_sample(OrientedClockSample{0, 10U});
	ASSERT_TRUE(filter.smoothed_offset_us(offset));
	EXPECT_EQ(70, offset);
	ASSERT_TRUE(filter.minimum_round_trip_time_us(rtt));
	EXPECT_EQ(10U, rtt);

	// Equal minimum RTT deliberately selects the newest sample.
	filter.add_sample(OrientedClockSample{-10, 10U});
	ASSERT_TRUE(filter.smoothed_offset_us(offset));
	EXPECT_EQ(60, offset);

	filter.invalidate();
	EXPECT_FALSE(filter.valid());
	EXPECT_EQ(0U, filter.sample_count());
	offset = 444;
	EXPECT_FALSE(filter.smoothed_offset_us(offset));
	EXPECT_EQ(444, offset);
	filter.add_sample(OrientedClockSample{-7, 5U});
	ASSERT_TRUE(filter.smoothed_offset_us(offset));
	EXPECT_EQ(-7, offset);
}

TEST(TelemetryProtocolClock, FilterWindowIsBoundedAndHandlesExtremeSignedSmoothing)
{
	ClockFilter filter;
	filter.add_sample(OrientedClockSample{std::numeric_limits<std::int64_t>::min(), 1U});
	filter.add_sample(OrientedClockSample{std::numeric_limits<std::int64_t>::max(), 0U});
	std::int64_t offset = 0;
	ASSERT_TRUE(filter.smoothed_offset_us(offset));
	const auto expected = std::numeric_limits<std::int64_t>::min() +
						  static_cast<std::int64_t>(std::numeric_limits<std::uint64_t>::max() / 8U);
	EXPECT_EQ(expected, offset);

	filter.reset();
	for (std::size_t index = 0; index < ClockFilter::Capacity + 1U; ++index) {
		filter.add_sample(
			OrientedClockSample{static_cast<std::int64_t>(index), static_cast<std::uint64_t>(index + 1U)});
	}
	EXPECT_EQ(ClockFilter::Capacity, filter.sample_count());
	std::uint64_t minimum_rtt = 0;
	ASSERT_TRUE(filter.minimum_round_trip_time_us(minimum_rtt));
	// The ninth insert evicts the original RTT=1 sample.
	EXPECT_EQ(2U, minimum_rtt);
}

TEST(TelemetryProtocolClock, PeerTimeEstimateChecksPositiveAndNegativeRangeBoundaries)
{
	ClockFilter filter;
	filter.add_sample(OrientedClockSample{5, 1U});
	std::uint64_t estimate = 99U;
	EXPECT_EQ(PeerTimeEstimateResult::Estimated, filter.estimate_peer_time_us(10U, estimate));
	EXPECT_EQ(15U, estimate);
	estimate = 99U;
	EXPECT_EQ(PeerTimeEstimateResult::OutOfRange,
		filter.estimate_peer_time_us(std::numeric_limits<std::uint64_t>::max() - 4U, estimate));
	EXPECT_EQ(99U, estimate);

	filter.reset();
	filter.add_sample(OrientedClockSample{-5, 1U});
	EXPECT_EQ(PeerTimeEstimateResult::Estimated, filter.estimate_peer_time_us(10U, estimate));
	EXPECT_EQ(5U, estimate);
	estimate = 99U;
	EXPECT_EQ(PeerTimeEstimateResult::OutOfRange, filter.estimate_peer_time_us(4U, estimate));
	EXPECT_EQ(99U, estimate);

	filter.reset();
	filter.add_sample(OrientedClockSample{std::numeric_limits<std::int64_t>::min(), 1U});
	const auto magnitude = std::uint64_t{1} << 63U;
	EXPECT_EQ(PeerTimeEstimateResult::Estimated, filter.estimate_peer_time_us(magnitude, estimate));
	EXPECT_EQ(0U, estimate);
}

TEST(TelemetryProtocolClock, TimeoutRulesEnforceNegotiatedBoundsAndProtocolMinima)
{
	std::uint32_t timeout = 77U;
	EXPECT_EQ(ClockTimeoutResult::HeartbeatIntervalOutOfRange,
		compute_clock_stale_timeout_ms(MinHeartbeatIntervalMs - 1U, timeout));
	EXPECT_EQ(77U, timeout);
	EXPECT_EQ(ClockTimeoutResult::HeartbeatIntervalOutOfRange,
		compute_session_disconnect_timeout_ms(MaxHeartbeatIntervalMs + 1U, timeout));
	EXPECT_EQ(77U, timeout);

	ASSERT_EQ(ClockTimeoutResult::Computed, compute_clock_stale_timeout_ms(MinHeartbeatIntervalMs, timeout));
	EXPECT_EQ(3000U, timeout);
	ASSERT_EQ(ClockTimeoutResult::Computed, compute_session_disconnect_timeout_ms(MinHeartbeatIntervalMs, timeout));
	EXPECT_EQ(10000U, timeout);
	ASSERT_EQ(ClockTimeoutResult::Computed, compute_clock_stale_timeout_ms(1000U, timeout));
	EXPECT_EQ(3000U, timeout);
	ASSERT_EQ(ClockTimeoutResult::Computed, compute_session_disconnect_timeout_ms(1000U, timeout));
	EXPECT_EQ(10000U, timeout);
	ASSERT_EQ(ClockTimeoutResult::Computed, compute_clock_stale_timeout_ms(MaxHeartbeatIntervalMs, timeout));
	EXPECT_EQ(15000U, timeout);
	ASSERT_EQ(ClockTimeoutResult::Computed, compute_session_disconnect_timeout_ms(MaxHeartbeatIntervalMs, timeout));
	EXPECT_EQ(50000U, timeout);
}

} // namespace
