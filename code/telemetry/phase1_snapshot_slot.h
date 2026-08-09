#pragma once

#include "telemetry/protocol/telemetry_replication.h"
#include "telemetry/protocol/telemetry_reliable_window.h"
#include "telemetry/phase1_allocation_observer.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace telemetry::detail {

// Local producer-side replication progress.  The wire session remains
// Synchronizing until the entire reliable snapshot transaction is APPLIED.
enum class Phase1SnapshotProgress : std::uint8_t {
	Synchronizing = 0,
	Live,
};

// P8.3 records only the reason a future P8.4 keyframe must be created. It
// never starts that candidate itself; first cause wins until consumed/purged.
enum class Phase1KeyframeIntent : std::uint8_t {
	None = 0,
	RecordSetDiscontinuity,
	CumulativeDeltaNotRepresentable,
};

// Owns the per-slot initial snapshot candidate.  ProducerBaselineTracker is
// deliberately the sole owner of current, candidate and active baselines;
// this adapter adds the Phase 1 session transition and the bounded P8.3
// notification that follows its atomic commit.
class Phase1SnapshotSlot final {
  public:
	bool preallocated() const noexcept {
		return m_baseline.dirty_index_backing_ready();
	}
	bool provision() noexcept {
		return m_baseline.provision_dirty_index_backing();
	}
	void reset() noexcept;
	void set_allocation_observer(Phase1AllocationObserver* observer) noexcept { m_allocation_observer = observer; }
	protocol::ProducerBaselineResult start_initial_candidate(std::uint32_t snapshot_id,
		const protocol::StateImage& captured,
		const std::vector<protocol::SnapshotCandidatePart>& parts,
		std::uint64_t now_us) noexcept;
	// Starts the single permitted replacement candidate while retaining the
	// active baseline for cumulative deltas until ACK APPLIED promotes it.
	protocol::ProducerBaselineResult start_replacement_candidate(std::uint32_t snapshot_id,
		const protocol::StateImage& captured,
		const std::vector<protocol::SnapshotCandidatePart>& parts,
		std::uint64_t now_us) noexcept;
	protocol::ProducerBaselineResult acknowledge_candidate_part(const protocol::AckPayload& ack,
		std::uint64_t now_us) noexcept;

	// These remain thin forwarding seams: ProducerBaselineTracker is the sole
	// owner of the current image, active baseline, candidate and delta sequence.
	protocol::ProducerBaselineResult replace_current(const protocol::StateImage& current) noexcept;
	protocol::ProducerBaselineResult replace_current_incremental(
		const protocol::StateImage& current,
		const std::uint16_t* rebuilt_indices,
		std::size_t rebuilt_index_count) noexcept;
	protocol::ProducerBaselineResult take_current_for_incremental_patch(
		protocol::StateImage& current) noexcept;
	protocol::ProducerBaselineResult restore_current_after_incremental_patch(
		protocol::StateImage&& current) noexcept;
	protocol::ProducerBaselineResult commit_current_incremental_patch(
		protocol::StateImage&& current,
		const std::uint16_t* rebuilt_indices,
		std::size_t rebuilt_index_count) noexcept;
	protocol::ProducerBaselineResult emit_cumulative_delta(std::uint64_t producer_sample_time_us,
		protocol::CumulativeStateDelta& delta,
		protocol::DeltaBuildChanges* changes = nullptr) noexcept;

	// The initial candidate has no active baseline yet.  Clearing it therefore
	// cannot expose a partially published state.
	void rollback_candidate() noexcept;
	// A replacement candidate may time out after an already ACKed baseline is
	// Live.  Drop only that uncommitted candidate in this case: the active
	// baseline remains the sole source for cumulative deltas until the caller
	// starts the bounded resynchronization keyframe.
	bool abandon_replacement_candidate() noexcept;

	bool has_candidate() const noexcept { return m_baseline.has_candidate(); }
	bool has_active_baseline() const noexcept { return m_baseline.has_active_baseline(); }
	const protocol::StateImage& current_state() const noexcept { return m_baseline.current(); }
	const protocol::StateImage& active_baseline() const noexcept { return m_baseline.active_baseline(); }
	// P8.3 only transports value mutations. A player/entity composition change
	// is deferred to the next keyframe phase rather than encoded as a delta.
	bool current_record_set_compatible_with_active_baseline() noexcept;
	std::uint32_t candidate_snapshot_id() const noexcept { return m_baseline.candidate_snapshot_id(); }
	std::uint32_t active_snapshot_id() const noexcept { return m_baseline.active_snapshot_id(); }
	protocol::RequiredAckLevel candidate_required_ack() const noexcept
	{
		return protocol::RequiredAckLevel::Applied;
	}
	protocol::ReliableMessageClass candidate_message_class() const noexcept
	{
		return protocol::ReliableMessageClass::Transaction;
	}
	Phase1SnapshotProgress progress() const noexcept { return m_progress; }
	bool session_state_dirty() const noexcept { return m_session_state_dirty; }
	bool consume_session_state_dirty() noexcept;
	Phase1KeyframeIntent keyframe_intent() const noexcept { return m_keyframe_intent; }
	Phase1KeyframeIntent consume_keyframe_intent() noexcept;

  private:
	protocol::ProducerBaselineTracker m_baseline;
	// Tracker::clear() intentionally resets its own sequence scope.  A slot can
	// nevertheless abandon a sent candidate, so preserve the wire identity
	// high-water mark across that rollback.
	std::uint32_t m_highest_started_snapshot_id = 0U;
	Phase1SnapshotProgress m_progress = Phase1SnapshotProgress::Synchronizing;
	bool m_session_state_dirty = false;
	Phase1KeyframeIntent m_keyframe_intent = Phase1KeyframeIntent::None;
	Phase1AllocationObserver* m_allocation_observer = nullptr;

	void request_keyframe(Phase1KeyframeIntent intent) noexcept;
};

} // namespace telemetry::detail
