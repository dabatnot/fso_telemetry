#include "telemetry/protocol/telemetry_counters.h"

#include <limits>

namespace telemetry::protocol {

namespace {

constexpr std::size_t NoProbe = MaxInFlightProbes;

} // namespace

bool ProbeTracker::reset_session(std::uint64_t session_id, std::uint32_t next_probe_id) noexcept
{
	if (next_probe_id == 0) {
		return false;
	}

	m_slots = {};
	m_session_id = session_id;
	m_next_probe_id = next_probe_id;
	m_in_flight_count = 0;
	return true;
}

ProbeStartResult ProbeTracker::begin_probe(std::uint64_t origin_t0_us, ProbeToken& token) noexcept
{
	if (m_session_id == 0) {
		return ProbeStartResult::NoSession;
	}
	if (full()) {
		return ProbeStartResult::CapacityReached;
	}

	std::uint32_t candidate = m_next_probe_id;
	// At most eight ids can be occupied, so one of nine consecutive non-zero
	// candidates is guaranteed to be available.
	for (std::size_t attempt = 0; attempt <= MaxInFlightProbes; ++attempt) {
		if (!id_in_flight(candidate)) {
			for (auto& slot : m_slots) {
				if (!slot.active) {
					slot.active = true;
					slot.probe_id = candidate;
					slot.origin_t0_us = origin_t0_us;
					++m_in_flight_count;

					m_next_probe_id = next_nonzero_id(candidate);
					token = ProbeToken{m_session_id, candidate, origin_t0_us};
					return ProbeStartResult::Started;
				}
			}
			return ProbeStartResult::CapacityReached;
		}
		candidate = next_nonzero_id(candidate);
	}

	return ProbeStartResult::CapacityReached;
}

ProbeResponseResult
ProbeTracker::preview_response(std::uint64_t session_id, std::uint32_t probe_id, std::uint64_t origin_t0_us) const noexcept
{
	if (m_session_id == 0) {
		return ProbeResponseResult::NoSession;
	}
	if (session_id != m_session_id) {
		return ProbeResponseResult::SessionMismatch;
	}

	const auto slot_index = find_probe(probe_id);
	if (slot_index == NoProbe) {
		return ProbeResponseResult::UnknownProbeId;
	}
	if (m_slots[slot_index].origin_t0_us != origin_t0_us) {
		return ProbeResponseResult::OriginTimestampMismatch;
	}

	return ProbeResponseResult::Matched;
}

ProbeResponseResult
ProbeTracker::correlate_response(std::uint64_t session_id, std::uint32_t probe_id, std::uint64_t origin_t0_us) noexcept
{
	const auto result = preview_response(session_id, probe_id, origin_t0_us);
	if (result != ProbeResponseResult::Matched) {
		return result;
	}
	release(find_probe(probe_id));
	return ProbeResponseResult::Matched;
}

bool ProbeTracker::discard_probe(std::uint64_t session_id, std::uint32_t probe_id, std::uint64_t origin_t0_us) noexcept
{
	if (m_session_id == 0 || session_id != m_session_id) {
		return false;
	}

	const auto slot_index = find_probe(probe_id);
	if (slot_index == NoProbe || m_slots[slot_index].origin_t0_us != origin_t0_us) {
		return false;
	}

	release(slot_index);
	return true;
}

std::size_t ProbeTracker::find_probe(std::uint32_t probe_id) const noexcept
{
	if (probe_id == 0) {
		return NoProbe;
	}
	for (std::size_t i = 0; i < m_slots.size(); ++i) {
		if (m_slots[i].active && m_slots[i].probe_id == probe_id) {
			return i;
		}
	}
	return NoProbe;
}

bool ProbeTracker::id_in_flight(std::uint32_t probe_id) const noexcept
{
	return find_probe(probe_id) != NoProbe;
}

std::uint32_t ProbeTracker::next_nonzero_id(std::uint32_t probe_id) noexcept
{
	return probe_id == std::numeric_limits<std::uint32_t>::max() ? 1U : probe_id + 1U;
}

void ProbeTracker::release(std::size_t slot_index) noexcept
{
	m_slots[slot_index] = {};
	--m_in_flight_count;
}

} // namespace telemetry::protocol
