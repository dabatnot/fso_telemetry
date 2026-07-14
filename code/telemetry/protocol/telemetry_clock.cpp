#include "telemetry/protocol/telemetry_clock.h"

#include "telemetry/protocol/telemetry_control_messages.h"

#include <limits>

namespace telemetry::protocol {

namespace {

struct Unsigned65 {
	std::uint64_t low = 0;
	std::uint8_t high = 0;
};

Unsigned65 add_u64(std::uint64_t a, std::uint64_t b) noexcept
{
	const auto low = static_cast<std::uint64_t>(a + b);
	return Unsigned65{low, static_cast<std::uint8_t>(low < a ? 1U : 0U)};
}

int compare(const Unsigned65& a, const Unsigned65& b) noexcept
{
	if (a.high != b.high) {
		return a.high < b.high ? -1 : 1;
	}
	if (a.low == b.low) {
		return 0;
	}
	return a.low < b.low ? -1 : 1;
}

Unsigned65 subtract(const Unsigned65& larger, const Unsigned65& smaller) noexcept
{
	const auto borrow = static_cast<std::uint8_t>(larger.low < smaller.low ? 1U : 0U);
	return Unsigned65{static_cast<std::uint64_t>(larger.low - smaller.low),
		static_cast<std::uint8_t>(larger.high - smaller.high - borrow)};
}

std::uint64_t divide_unsigned65_by_two(const Unsigned65& value) noexcept
{
	return (value.high != 0U ? (std::uint64_t{1} << 63U) : std::uint64_t{0}) | (value.low >> 1U);
}

ClockSampleResult compute_offset(const FourTimestampExchange& exchange, std::int64_t& offset_us) noexcept
{
	// ((t1 - t0) + (t2 - t3)) / 2 is evaluated as
	// ((t1 + t2) - (t0 + t3)) / 2 using explicit 65-bit unsigned sums.
	const auto responder_sum = add_u64(exchange.responder_receive_t1_us, exchange.responder_transmit_t2_us);
	const auto initiator_sum = add_u64(exchange.initiator_send_t0_us, exchange.initiator_receive_t3_us);
	const auto ordering = compare(responder_sum, initiator_sum);
	if (ordering == 0) {
		offset_us = 0;
		return ClockSampleResult::Valid;
	}

	const bool negative = ordering < 0;
	const auto magnitude = negative ? subtract(initiator_sum, responder_sum) : subtract(responder_sum, initiator_sum);
	const auto half_magnitude = divide_unsigned65_by_two(magnitude);
	const auto positive_limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
	const auto negative_limit = positive_limit + 1U;

	if ((!negative && half_magnitude > positive_limit) || (negative && half_magnitude > negative_limit)) {
		return ClockSampleResult::OffsetOutOfRange;
	}

	if (!negative) {
		offset_us = static_cast<std::int64_t>(half_magnitude);
	} else if (half_magnitude == negative_limit) {
		offset_us = std::numeric_limits<std::int64_t>::min();
	} else {
		offset_us = -static_cast<std::int64_t>(half_magnitude);
	}
	return ClockSampleResult::Valid;
}

ClockTimeoutResult compute_timeout(std::uint32_t heartbeat_interval_ms,
	std::uint32_t multiplier,
	std::uint32_t minimum_ms,
	std::uint32_t& timeout_ms) noexcept
{
	if (heartbeat_interval_ms < MinHeartbeatIntervalMs || heartbeat_interval_ms > MaxHeartbeatIntervalMs) {
		return ClockTimeoutResult::HeartbeatIntervalOutOfRange;
	}

	const auto interval = static_cast<std::uint64_t>(heartbeat_interval_ms);
	const auto factor = static_cast<std::uint64_t>(multiplier);
	if (factor != 0U && interval > std::numeric_limits<std::uint64_t>::max() / factor) {
		return ClockTimeoutResult::ArithmeticOverflow;
	}

	const auto scaled = interval * factor;
	const auto selected = scaled < minimum_ms ? static_cast<std::uint64_t>(minimum_ms) : scaled;
	if (selected > std::numeric_limits<std::uint32_t>::max()) {
		return ClockTimeoutResult::ArithmeticOverflow;
	}

	timeout_ms = static_cast<std::uint32_t>(selected);
	return ClockTimeoutResult::Computed;
}

} // namespace

ClockSampleResult compute_clock_sample(const FourTimestampExchange& exchange, ClockSample& sample) noexcept
{
	if (exchange.responder_transmit_t2_us < exchange.responder_receive_t1_us) {
		return ClockSampleResult::ResponderClockMovedBackward;
	}
	if (exchange.initiator_receive_t3_us < exchange.initiator_send_t0_us) {
		return ClockSampleResult::InitiatorClockMovedBackward;
	}

	const auto initiator_span = exchange.initiator_receive_t3_us - exchange.initiator_send_t0_us;
	const auto responder_span = exchange.responder_transmit_t2_us - exchange.responder_receive_t1_us;
	if (responder_span > initiator_span) {
		return ClockSampleResult::NegativeRoundTrip;
	}

	ClockSample computed;
	const auto offset_result = compute_offset(exchange, computed.responder_minus_initiator_offset_us);
	if (offset_result != ClockSampleResult::Valid) {
		return offset_result;
	}

	computed.round_trip_time_us = initiator_span - responder_span;

	sample = computed;
	return ClockSampleResult::Valid;
}

ClockOffsetConversionResult
orient_clock_sample(const ClockSample& sample, LocalClockRole local_role, OrientedClockSample& oriented) noexcept
{
	OrientedClockSample converted;
	converted.round_trip_time_us = sample.round_trip_time_us;
	switch (local_role) {
	case LocalClockRole::Initiator:
		converted.peer_minus_local_offset_us = sample.responder_minus_initiator_offset_us;
		break;
	case LocalClockRole::Responder:
		if (sample.responder_minus_initiator_offset_us == std::numeric_limits<std::int64_t>::min()) {
			return ClockOffsetConversionResult::OffsetOutOfRange;
		}
		converted.peer_minus_local_offset_us = -sample.responder_minus_initiator_offset_us;
		break;
	default:
		return ClockOffsetConversionResult::InvalidRole;
	}

	oriented = converted;
	return ClockOffsetConversionResult::Converted;
}

void ClockFilter::add_sample(const OrientedClockSample& sample) noexcept
{
	if (m_sample_count < Capacity) {
		m_samples[m_sample_count] = sample;
		++m_sample_count;
	} else {
		for (std::size_t i = 1; i < Capacity; ++i) {
			m_samples[i - 1] = m_samples[i];
		}
		m_samples[Capacity - 1] = sample;
	}

	const auto& candidate = minimum_rtt_sample();
	m_minimum_rtt_us = candidate.round_trip_time_us;
	if (!m_valid) {
		m_smoothed_offset_us = candidate.peer_minus_local_offset_us;
		m_valid = true;
	} else {
		m_smoothed_offset_us = smooth_towards(m_smoothed_offset_us, candidate.peer_minus_local_offset_us);
	}
}

void ClockFilter::invalidate() noexcept
{
	reset();
}

void ClockFilter::reset() noexcept
{
	m_samples = {};
	m_sample_count = 0;
	m_smoothed_offset_us = 0;
	m_minimum_rtt_us = 0;
	m_valid = false;
}

bool ClockFilter::smoothed_offset_us(std::int64_t& offset_us) const noexcept
{
	if (!m_valid) {
		return false;
	}
	offset_us = m_smoothed_offset_us;
	return true;
}

bool ClockFilter::minimum_round_trip_time_us(std::uint64_t& round_trip_time_us) const noexcept
{
	if (!m_valid) {
		return false;
	}
	round_trip_time_us = m_minimum_rtt_us;
	return true;
}

PeerTimeEstimateResult ClockFilter::estimate_peer_time_us(std::uint64_t local_time_us,
	std::uint64_t& peer_time_us) const noexcept
{
	if (!m_valid) {
		return PeerTimeEstimateResult::FilterInvalid;
	}

	std::uint64_t estimated = 0;
	if (m_smoothed_offset_us >= 0) {
		const auto addition = static_cast<std::uint64_t>(m_smoothed_offset_us);
		if (addition > std::numeric_limits<std::uint64_t>::max() - local_time_us) {
			return PeerTimeEstimateResult::OutOfRange;
		}
		estimated = local_time_us + addition;
	} else {
		const auto magnitude = std::uint64_t{0} - static_cast<std::uint64_t>(m_smoothed_offset_us);
		if (magnitude > local_time_us) {
			return PeerTimeEstimateResult::OutOfRange;
		}
		estimated = local_time_us - magnitude;
	}

	peer_time_us = estimated;
	return PeerTimeEstimateResult::Estimated;
}

const OrientedClockSample& ClockFilter::minimum_rtt_sample() const noexcept
{
	std::size_t best = 0;
	for (std::size_t i = 1; i < Capacity && i < m_sample_count; ++i) {
		// <= deliberately gives an equal-RTT tie to the newer sample.
		if (m_samples[i].round_trip_time_us <= m_samples[best].round_trip_time_us) {
			best = i;
		}
	}
	return m_samples[best < Capacity ? best : 0];
}

std::int64_t ClockFilter::smooth_towards(std::int64_t current, std::int64_t candidate) noexcept
{
	if (candidate == current) {
		return current;
	}

	// Unsigned subtraction yields the exact mathematical distance for ordered
	// signed endpoints, even when that distance is wider than INT64_MAX.
	if (candidate > current) {
		const auto distance = static_cast<std::uint64_t>(candidate) - static_cast<std::uint64_t>(current);
		const auto step = static_cast<std::int64_t>(distance / 8U);
		return current + step;
	}

	const auto distance = static_cast<std::uint64_t>(current) - static_cast<std::uint64_t>(candidate);
	const auto step = static_cast<std::int64_t>(distance / 8U);
	return current - step;
}

ClockTimeoutResult compute_clock_stale_timeout_ms(std::uint32_t heartbeat_interval_ms,
	std::uint32_t& timeout_ms) noexcept
{
	return compute_timeout(heartbeat_interval_ms, 3U, 3000U, timeout_ms);
}

ClockTimeoutResult compute_session_disconnect_timeout_ms(std::uint32_t heartbeat_interval_ms,
	std::uint32_t& timeout_ms) noexcept
{
	return compute_timeout(heartbeat_interval_ms, 10U, 10000U, timeout_ms);
}

} // namespace telemetry::protocol
