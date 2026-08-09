#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace telemetry::protocol {

enum class SerialU32Order : std::uint8_t {
	Equal,
	Newer,
	Older,
	Ambiguous,
};

// RFC-1982-style ordering for the two wrapping FSTL u32 counters. Exactly
// half of the serial space has no direction and is reported as Ambiguous.
constexpr SerialU32Order compare_serial_u32(std::uint32_t a, std::uint32_t b) noexcept
{
	const auto difference = static_cast<std::uint32_t>(a - b);
	if (difference == 0U) {
		return SerialU32Order::Equal;
	}
	if (difference == 0x80000000U) {
		return SerialU32Order::Ambiguous;
	}
	return difference < 0x80000000U ? SerialU32Order::Newer : SerialU32Order::Older;
}

constexpr bool serial_u32_newer(std::uint32_t a, std::uint32_t b) noexcept
{
	return compare_serial_u32(a, b) == SerialU32Order::Newer;
}

enum class CounterNextResult : std::uint8_t {
	Value,
	Exhausted,
};

// A non-zero monotonically increasing counter which never wraps. The maximum
// value can be emitted once; the next call reports Exhausted without writing
// zero (or changing the caller's output).
template <typename UInt>
class NoWrapCounter final {
	static_assert(std::is_integral<UInt>::value, "NoWrapCounter requires an integer type");
	static_assert(std::is_unsigned<UInt>::value, "NoWrapCounter requires an unsigned type");

  public:
	NoWrapCounter() = default;

	// A non-zero first value is useful when restoring a counter or exercising
	// its exhaustion boundary. A rejected zero leaves the counter unchanged.
	bool reset(UInt first_value = UInt{1}) noexcept
	{
		if (first_value == UInt{0}) {
			return false;
		}
		m_next = first_value;
		m_exhausted = false;
		return true;
	}

	CounterNextResult take_next(UInt& value) noexcept
	{
		if (m_exhausted) {
			return CounterNextResult::Exhausted;
		}

		value = m_next;
		if (m_next == std::numeric_limits<UInt>::max()) {
			m_exhausted = true;
		} else {
			++m_next;
		}
		return CounterNextResult::Value;
	}

	bool exhausted() const noexcept
	{
		return m_exhausted;
	}

  private:
	UInt m_next = UInt{1};
	bool m_exhausted = false;
};

using NoWrapCounterU32 = NoWrapCounter<std::uint32_t>;
using NoWrapCounterU64 = NoWrapCounter<std::uint64_t>;

constexpr std::size_t MaxInFlightProbes = 8;

struct ProbeToken {
	std::uint64_t session_id = 0;
	std::uint32_t probe_id = 0;
	std::uint64_t origin_t0_us = 0;
};

enum class ProbeStartResult : std::uint8_t {
	Started,
	NoSession,
	CapacityReached,
};

enum class ProbeResponseResult : std::uint8_t {
	Matched,
	NoSession,
	SessionMismatch,
	UnknownProbeId,
	OriginTimestampMismatch,
};

// Tracks the probes initiated by one local endpoint. Storage is fixed, probe
// zero is always skipped, and a wrapped id is not reused while it is in flight.
class ProbeTracker final {
  public:
	ProbeTracker() = default;

	// On success, purges the previous session, including when session_id is
	// zero. A zero session disables begin_probe until a non-zero session is
	// installed.
	// next_probe_id is injectable so wrap behavior can be exercised without
	// billions of calls. Production session setup uses the default value 1.
	// A rejected zero leaves the tracker unchanged.
	bool reset_session(std::uint64_t session_id, std::uint32_t next_probe_id = 1) noexcept;

	// token is unchanged on failure.
	ProbeStartResult begin_probe(std::uint64_t origin_t0_us, ProbeToken& token) noexcept;

	// Non-mutating correlation seam. Call this before validating response
	// semantics so malformed timestamps cannot consume a live probe.
	ProbeResponseResult
	preview_response(std::uint64_t session_id, std::uint32_t probe_id, std::uint64_t origin_t0_us) const noexcept;

	// A response consumes a slot only when session, probe id and the echoed t0
	// all match. Call only after preview_response and semantic validation.
	ProbeResponseResult
	correlate_response(std::uint64_t session_id, std::uint32_t probe_id, std::uint64_t origin_t0_us) noexcept;

	// Used by a timeout owner to release an exact in-flight probe.
	bool discard_probe(std::uint64_t session_id, std::uint32_t probe_id, std::uint64_t origin_t0_us) noexcept;

	std::uint64_t session_id() const noexcept
	{
		return m_session_id;
	}
	std::size_t in_flight_count() const noexcept
	{
		return m_in_flight_count;
	}
	bool full() const noexcept
	{
		return m_in_flight_count == MaxInFlightProbes;
	}

  private:
	struct Slot {
		bool active = false;
		std::uint32_t probe_id = 0;
		std::uint64_t origin_t0_us = 0;
	};

	std::size_t find_probe(std::uint32_t probe_id) const noexcept;
	bool id_in_flight(std::uint32_t probe_id) const noexcept;
	static std::uint32_t next_nonzero_id(std::uint32_t probe_id) noexcept;
	void release(std::size_t slot_index) noexcept;

	std::array<Slot, MaxInFlightProbes> m_slots{};
	std::uint64_t m_session_id = 0;
	std::uint32_t m_next_probe_id = 1;
	std::size_t m_in_flight_count = 0;
};

} // namespace telemetry::protocol
