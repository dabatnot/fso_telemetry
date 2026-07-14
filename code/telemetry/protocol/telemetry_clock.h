#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace telemetry::protocol {

struct FourTimestampExchange {
	std::uint64_t initiator_send_t0_us = 0;
	std::uint64_t responder_receive_t1_us = 0;
	std::uint64_t responder_transmit_t2_us = 0;
	std::uint64_t initiator_receive_t3_us = 0;
};

struct ClockSample {
	// This core sign is role-based, not producer/client based. A caller must
	// explicitly orient it before estimating the peer's clock.
	std::int64_t responder_minus_initiator_offset_us = 0;
	std::uint64_t round_trip_time_us = 0;
};

enum class ClockSampleResult : std::uint8_t {
	Valid,
	ResponderClockMovedBackward,
	InitiatorClockMovedBackward,
	NegativeRoundTrip,
	OffsetOutOfRange,
};

// Computes the exact four-timestamp result without requiring a compiler
// extension wider than 64 bits. sample is unchanged unless Valid is returned.
ClockSampleResult compute_clock_sample(const FourTimestampExchange& exchange, ClockSample& sample) noexcept;

enum class LocalClockRole : std::uint8_t {
	Initiator,
	Responder,
};

struct OrientedClockSample {
	std::int64_t peer_minus_local_offset_us = 0;
	std::uint64_t round_trip_time_us = 0;
};

enum class ClockOffsetConversionResult : std::uint8_t {
	Converted,
	InvalidRole,
	OffsetOutOfRange,
};

// Converts the generic responder-minus-initiator sign into peer-minus-local.
// Mapping producer/client to Initiator/Responder deliberately remains a call-
// site decision, because either peer may initiate a post-handshake heartbeat.
ClockOffsetConversionResult
orient_clock_sample(const ClockSample& sample, LocalClockRole local_role, OrientedClockSample& oriented) noexcept;

enum class PeerTimeEstimateResult : std::uint8_t {
	Estimated,
	FilterInvalid,
	OutOfRange,
};

class ClockFilter final {
  public:
	static constexpr std::size_t Capacity = 8;

	// A successful first add revalidates the filter and initializes the
	// smoothed offset.
	void add_sample(const OrientedClockSample& sample) noexcept;

	// Both operations purge the sample window so stale samples cannot affect a
	// later session or a filter rebuilt after a monotonic discontinuity.
	void invalidate() noexcept;
	void reset() noexcept;

	bool valid() const noexcept
	{
		return m_valid;
	}
	std::size_t sample_count() const noexcept
	{
		return m_sample_count;
	}

	// Outputs are unchanged when the filter is invalid.
	bool smoothed_offset_us(std::int64_t& offset_us) const noexcept;
	bool minimum_round_trip_time_us(std::uint64_t& round_trip_time_us) const noexcept;
	PeerTimeEstimateResult estimate_peer_time_us(std::uint64_t local_time_us,
		std::uint64_t& peer_time_us) const noexcept;

  private:
	const OrientedClockSample& minimum_rtt_sample() const noexcept;
	static std::int64_t smooth_towards(std::int64_t current, std::int64_t candidate) noexcept;

	// Samples remain oldest-to-newest. For equal minimum RTTs the most recent
	// sample wins. That deterministic tie-break is a wire-neutral, revisable
	// implementation choice because the protocol does not prescribe one.
	std::array<OrientedClockSample, Capacity> m_samples{};
	std::size_t m_sample_count = 0;
	std::int64_t m_smoothed_offset_us = 0;
	std::uint64_t m_minimum_rtt_us = 0;
	bool m_valid = false;
};

enum class ClockTimeoutResult : std::uint8_t {
	Computed,
	HeartbeatIntervalOutOfRange,
	ArithmeticOverflow,
};

// H is accepted only in the negotiated 200..5000 ms range. Outputs are
// unchanged on failure and multiplication is performed in checked u64 space.
ClockTimeoutResult compute_clock_stale_timeout_ms(std::uint32_t heartbeat_interval_ms,
	std::uint32_t& timeout_ms) noexcept;
ClockTimeoutResult compute_session_disconnect_timeout_ms(std::uint32_t heartbeat_interval_ms,
	std::uint32_t& timeout_ms) noexcept;

} // namespace telemetry::protocol
