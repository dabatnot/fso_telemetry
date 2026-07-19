#include "telemetry/phase1_snapshot_slot.h"

#include <array>
#include <limits>

namespace telemetry::detail {

protocol::ProducerBaselineResult Phase1SnapshotSlot::start_initial_candidate(std::uint32_t snapshot_id,
	const protocol::StateImage& captured,
	const std::vector<protocol::SnapshotCandidatePart>& parts,
	std::uint64_t now_us) noexcept
{
	// P8.2 owns only the first snapshot.  A later keyframe is a P8.3 concern;
	// accepting it here could replace an ACKed baseline without its delta rules.
	if (snapshot_id == 0U || snapshot_id <= m_highest_started_snapshot_id) {
		return protocol::ProducerBaselineResult::InvalidArgument;
	}
	if (m_baseline.has_candidate() || m_baseline.has_active_baseline()) {
		return protocol::ProducerBaselineResult::CandidateBusy;
	}
	if (const auto initialized = m_baseline.initialize(captured);
		initialized != protocol::ProducerBaselineResult::Applied) {
		return initialized;
	}
	const auto result = m_baseline.capture_snapshot(snapshot_id, 0U, captured, parts, now_us);
	if (result != protocol::ProducerBaselineResult::Applied) {
		m_baseline.clear();
	} else {
		m_highest_started_snapshot_id = snapshot_id;
	}
	return result;
}

protocol::ProducerBaselineResult Phase1SnapshotSlot::start_replacement_candidate(std::uint32_t snapshot_id,
	const protocol::StateImage& captured,
	const std::vector<protocol::SnapshotCandidatePart>& parts,
	std::uint64_t now_us) noexcept
{
	if (snapshot_id == 0U || snapshot_id <= m_highest_started_snapshot_id) {
		return protocol::ProducerBaselineResult::InvalidArgument;
	}
	if (!m_baseline.has_active_baseline()) {
		return protocol::ProducerBaselineResult::NoActiveBaseline;
	}
	if (m_baseline.has_candidate()) {
		return protocol::ProducerBaselineResult::CandidateBusy;
	}
	// Current state is intentionally updated before reserving the candidate.
	// A reservation failure therefore cannot affect the active baseline, while
	// the latest canonical image remains available for its cumulative delta.
	const auto current = m_baseline.replace_current(captured);
	if (current != protocol::ProducerBaselineResult::Applied) {
		return current;
	}
	const auto result = m_baseline.capture_snapshot(snapshot_id, 0U, captured, parts, now_us);
	if (result == protocol::ProducerBaselineResult::Applied) {
		m_highest_started_snapshot_id = snapshot_id;
	}
	return result;
}

protocol::ProducerBaselineResult Phase1SnapshotSlot::acknowledge_candidate_part(const protocol::AckPayload& ack,
	std::uint64_t now_us) noexcept
{
	const auto had_candidate = m_baseline.has_candidate();
	const auto result = m_baseline.acknowledge_snapshot_part(ack, now_us);
	if (had_candidate && !m_baseline.has_candidate() && m_baseline.has_active_baseline() &&
		result == protocol::ProducerBaselineResult::Applied) {
		m_progress = Phase1SnapshotProgress::Live;
		// One bit, rather than a queued delta: P8.3 reconstructs the current
		// SESSION_STATE against the newly committed baseline.
		m_session_state_dirty = true;
	}
	return result;
}

protocol::ProducerBaselineResult Phase1SnapshotSlot::replace_current(const protocol::StateImage& current) noexcept
{
	return m_baseline.replace_current(current);
}

protocol::ProducerBaselineResult Phase1SnapshotSlot::emit_cumulative_delta(std::uint64_t producer_sample_time_us,
	protocol::CumulativeStateDelta& delta) noexcept
{
	// A discontinuity is represented only by the future keyframe. In
	// particular, never let a caller bypass the controller's record-set gate
	// and serialize a create/delete delta after the intent was latched.
	if (m_keyframe_intent != Phase1KeyframeIntent::None) {
		return protocol::ProducerBaselineResult::KeyframeRequired;
	}
	const auto mutations_capacity_before = delta.mutations.capacity();
	std::array<std::size_t, 4U> identity_capacities{};
	std::array<std::size_t, 4U> value_capacities{};
	for (std::size_t index = 0U; index < delta.mutations.size() && index < identity_capacities.size(); ++index) {
		identity_capacities[index] = delta.mutations[index].atom.key.identity.capacity();
		value_capacities[index] = delta.mutations[index].atom.value.capacity();
	}
	const auto result = m_baseline.emit_cumulative_delta(producer_sample_time_us, delta);
	if (m_allocation_observer != nullptr) {
		m_allocation_observer->note_growth(mutations_capacity_before, delta.mutations.capacity());
		for (std::size_t index = 0U; index < delta.mutations.size() && index < identity_capacities.size(); ++index) {
			m_allocation_observer->note_growth(identity_capacities[index], delta.mutations[index].atom.key.identity.capacity());
			m_allocation_observer->note_growth(value_capacities[index], delta.mutations[index].atom.value.capacity());
		}
	}
	if (result == protocol::ProducerBaselineResult::KeyframeRequired) {
		request_keyframe(Phase1KeyframeIntent::CumulativeDeltaNotRepresentable);
	}
	return result;
}

bool Phase1SnapshotSlot::current_record_set_compatible_with_active_baseline() noexcept
{
	if (!m_baseline.has_active_baseline()) {
		return false;
	}
	const auto& current = m_baseline.current().records();
	const auto& baseline = m_baseline.active_baseline().records();
	if (current.size() != baseline.size()) {
		request_keyframe(Phase1KeyframeIntent::RecordSetDiscontinuity);
		return false;
	}
	for (std::size_t index = 0U; index < current.size(); ++index) {
		if (current[index].key != baseline[index].key || current[index].record_version != baseline[index].record_version ||
			current[index].lifecycle != baseline[index].lifecycle ||
			current[index].has_cascade_owner != baseline[index].has_cascade_owner ||
			current[index].cascade_owner != baseline[index].cascade_owner) {
			request_keyframe(Phase1KeyframeIntent::RecordSetDiscontinuity);
			return false;
		}
	}
	return true;
}

void Phase1SnapshotSlot::rollback_candidate() noexcept
{
	m_baseline.clear();
	m_progress = Phase1SnapshotProgress::Synchronizing;
	m_session_state_dirty = false;
	m_keyframe_intent = Phase1KeyframeIntent::None;
}

bool Phase1SnapshotSlot::abandon_replacement_candidate() noexcept
{
	if (!m_baseline.has_active_baseline() || !m_baseline.has_candidate()) {
		return false;
	}
	// ProducerBaselineTracker owns the candidate separately from the immutable
	// active baseline.  Its expiry path is the bounded, transaction-safe way to
	// release only the candidate; max time makes this an explicit abandonment
	// rather than a second independent timeout policy.
	return m_baseline.expire_candidate(std::numeric_limits<std::uint64_t>::max()) ==
		protocol::ProducerBaselineResult::Expired;
}

bool Phase1SnapshotSlot::consume_session_state_dirty() noexcept
{
	const auto dirty = m_session_state_dirty;
	m_session_state_dirty = false;
	return dirty;
}

Phase1KeyframeIntent Phase1SnapshotSlot::consume_keyframe_intent() noexcept
{
	const auto intent = m_keyframe_intent;
	m_keyframe_intent = Phase1KeyframeIntent::None;
	return intent;
}

void Phase1SnapshotSlot::request_keyframe(Phase1KeyframeIntent intent) noexcept
{
	if (m_keyframe_intent == Phase1KeyframeIntent::None && intent != Phase1KeyframeIntent::None) {
		m_keyframe_intent = intent;
	}
}

} // namespace telemetry::detail
