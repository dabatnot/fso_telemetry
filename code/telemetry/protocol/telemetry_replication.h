#pragma once

#include "telemetry/protocol/telemetry_rate_limiter.h"
#include "telemetry/protocol/telemetry_records.h"
#include "telemetry/protocol/telemetry_reliability_messages.h"
#include "telemetry/protocol/telemetry_state_messages.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace telemetry::protocol {

constexpr std::uint64_t PendingDeltaLifetimeUs = 2'000'000ULL;
constexpr std::uint64_t ResyncRequestDeduplicationWindowUs = 7'000'000ULL;
// The ResyncRequest bucket allows an initial burst of two and one request per
// second afterwards. Nine slots conservatively cover every identity that can
// coexist in the seven-second reliable receive/deduplication window.
constexpr std::size_t ResyncRequestDeduplicationCapacity = 9U;

constexpr std::uint32_t next_resync_request_id(std::uint32_t emitted_request_id) noexcept
{
	return emitted_request_id == 0 || emitted_request_id == std::numeric_limits<std::uint32_t>::max()
			   ? 1U
			   : emitted_request_id + 1U;
}

enum class StateRecordLifecycle : std::uint8_t {
	UpsertOnly = 0,
	ExplicitCreateDelete = 1,
};

struct StateAtomKey {
	std::uint16_t record_type = 0;
	// Exact key prefix used by a DELETE record. Empty is valid for a global
	// singleton whose RecordType is its complete key.
	std::vector<std::uint8_t> identity;

	friend bool operator==(const StateAtomKey& left, const StateAtomKey& right) noexcept;
	friend bool operator!=(const StateAtomKey& left, const StateAtomKey& right) noexcept
	{
		return !(left == right);
	}
	friend bool operator<(const StateAtomKey& left, const StateAtomKey& right) noexcept;
};

struct StateAtom {
	StateAtomKey key;
	std::uint8_t record_version = 1;
	StateRecordLifecycle lifecycle = StateRecordLifecycle::UpsertOnly;
	// Some records are removed implicitly when an explicitly deletable owner
	// disappears (for example, entity-scoped atoms under EntityLifecycle).
	// P0.7 supplies this engine-neutral relation from the record schema.
	bool has_cascade_owner = false;
	StateAtomKey cascade_owner;
	// Complete record payload for a snapshot or full-record upsert. For a
	// keyed atom this begins with key.identity; P0.7 owns that schema check.
	std::vector<std::uint8_t> value;

	friend bool operator==(const StateAtom& left, const StateAtom& right) noexcept;
	friend bool operator!=(const StateAtom& left, const StateAtom& right) noexcept
	{
		return !(left == right);
	}
};

constexpr std::size_t MaxReplicationStateAtomCount = MaxTransactionSize / RecordEnvelopeHeaderSize;
constexpr std::size_t MaxReplicationStateImageRetainedBytes =
	MaxTransactionSize * 3U + MaxReplicationStateAtomCount * sizeof(StateAtom);

enum class StateImageResult : std::uint8_t {
	Created = 0,
	InvalidRecord = 1,
	DuplicateKey = 2,
	SizeLimitExceeded = 3,
	AllocationFailed = 4,
};

// A canonical, immutable-by-interface image. Records are sorted by their
// atomic key, which makes comparisons and cumulative delta construction
// deterministic on every platform.
class StateImage final {
  public:
	StateImage() = default;

	// records is passed by value so successful construction can publish by
	// move. image is unchanged on failure.
	static StateImageResult create(std::vector<StateAtom> records, StateImage& image) noexcept;

	const std::vector<StateAtom>& records() const noexcept;
	std::size_t encoded_snapshot_records_size() const noexcept
	{
		return m_encoded_snapshot_records_size;
	}
	std::size_t retained_payload_bytes() const noexcept
	{
		return m_retained_payload_bytes;
	}
	bool empty() const noexcept
	{
		return m_records == nullptr || m_records->empty();
	}

	friend bool operator==(const StateImage& left, const StateImage& right) noexcept;
	friend bool operator!=(const StateImage& left, const StateImage& right) noexcept
	{
		return !(left == right);
	}

  private:
	// State images are immutable and frequently held simultaneously as current,
	// active, candidate and published views. Sharing the canonical backing store
	// keeps those logical copies O(1) and prevents baseline bookkeeping from
	// multiplying the retained payload budget.
	std::shared_ptr<const std::vector<StateAtom>> m_records;
	std::size_t m_encoded_snapshot_records_size = 0;
	std::size_t m_retained_payload_bytes = 0;
};

enum class StateMutationKind : std::uint8_t {
	Upsert = 0,
	Create = 1,
	Delete = 2,
};

struct StateMutation {
	StateMutationKind kind = StateMutationKind::Upsert;
	StateAtom atom;

	friend bool operator==(const StateMutation& left, const StateMutation& right) noexcept;
};

constexpr std::size_t MaxReplicationDeltaMutationCount = std::numeric_limits<std::uint16_t>::max();
constexpr std::size_t MaxReplicationDeltaRetainedBytes =
	MaxStateMessageSize * 3U + MaxReplicationDeltaMutationCount * sizeof(StateMutation);

struct CumulativeStateDelta {
	std::uint32_t baseline_snapshot_id = 0;
	std::uint32_t delta_sequence = 0;
	std::uint64_t producer_sample_time_us = 0;
	std::vector<StateMutation> mutations;

	// Exact v1 logical payload size: 20-byte Delta prefix plus each six-byte
	// Record envelope and its full or deletion-key payload.
	std::size_t encoded_size() const noexcept;

	friend bool operator==(const CumulativeStateDelta& left, const CumulativeStateDelta& right) noexcept;
	friend bool operator!=(const CumulativeStateDelta& left, const CumulativeStateDelta& right) noexcept
	{
		return !(left == right);
	}
};

enum class StateDeltaValidationResult : std::uint8_t {
	Valid = 0,
	InvalidIdentity = 1,
	InvalidMutation = 2,
	DuplicateKey = 3,
	SizeLimitExceeded = 4,
};

StateDeltaValidationResult validate_cumulative_state_delta(const CumulativeStateDelta& delta) noexcept;

class StateImageValidator {
  public:
	virtual ~StateImageValidator() = default;
	virtual ValidationError validate(const StateImage& image) const noexcept = 0;
};

enum class StateDeltaApplyResult : std::uint8_t {
	Applied = 0,
	InvalidDelta = 1,
	InvalidTransition = 2,
	ValidationFailed = 3,
	AllocationFailed = 4,
};

// Always reconstructs from baseline and publishes to applied only after the
// whole candidate passes structural and caller-supplied semantic validation.
// applied is unchanged on failure.
StateDeltaApplyResult apply_cumulative_state_delta(const StateImage& baseline,
	const CumulativeStateDelta& delta,
	const StateImageValidator* validator,
	StateImage& applied) noexcept;

struct SnapshotCandidatePart {
	ReliabilityTargetTuple target;
};

enum class ProducerBaselineResult : std::uint8_t {
	Applied = 0,
	NoChange = 1,
	InvalidArgument = 2,
	CandidateBusy = 3,
	UnknownPart = 4,
	InvalidAck = 5,
	Expired = 6,
	NoActiveBaseline = 7,
	KeyframeRequired = 8,
	SequenceExhausted = 9,
	AllocationFailed = 10,
};

enum class ProducerResyncResult : std::uint8_t {
	AcceptedNewCandidate = 0,
	AcceptedDuplicate = 1,
	AcceptedCoalesced = 2,
	Stale = 3,
	Invalid = 4,
	Expired = 5,
	Completed = 6,
	NoChange = 7,
	ResourceLimit = 8,
};

constexpr bool producer_resync_result_requires_validated_ack(ProducerResyncResult result) noexcept
{
	return result == ProducerResyncResult::AcceptedNewCandidate || result == ProducerResyncResult::AcceptedDuplicate ||
		   result == ProducerResyncResult::AcceptedCoalesced;
}

constexpr bool producer_resync_result_starts_candidate(ProducerResyncResult result) noexcept
{
	return result == ProducerResyncResult::AcceptedNewCandidate;
}

// Bounded semantic request-id deduplication around the reliable receive cache.
// Request IDs have no ordering rule: every accepted identity is retained for
// the exact seven-second deduplication window, after which its ID may be reused.
class ProducerResyncTracker final {
  public:
	ProducerResyncResult accept(const ResyncRequestPayload& request, std::uint64_t now_us) noexcept;
	ProducerResyncResult expire(std::uint64_t now_us) noexcept;
	ProducerResyncResult complete() noexcept;
	void clear() noexcept;

	bool has_candidate() const noexcept
	{
		return m_has_candidate;
	}
	std::uint32_t candidate_request_id() const noexcept
	{
		return m_has_candidate ? m_candidate_request.request_id : 0;
	}
	std::size_t deduplication_entry_count() const noexcept
	{
		return m_deduplication_entry_count;
	}
	std::uint64_t candidate_deadline_us() const noexcept
	{
		return m_has_candidate ? m_candidate_deadline_us : 0;
	}

  private:
	struct DeduplicationEntry {
		ResyncRequestPayload request;
		std::uint64_t deadline_us = 0;
		bool occupied = false;
	};

	void expire_deduplication_entries(std::uint64_t now_us) noexcept;

	ResyncRequestPayload m_candidate_request;
	std::array<DeduplicationEntry, ResyncRequestDeduplicationCapacity> m_deduplication_entries{};
	std::uint64_t m_candidate_deadline_us = 0;
	std::size_t m_deduplication_entry_count = 0;
	bool m_has_candidate = false;
};

class ProducerBaselineTracker final {
  public:
	ProducerBaselineTracker() = default;

	// Starts a new per-client tracker with a current immutable engine-neutral
	// image and no common baseline. The first snapshot becomes active only
	// after all of its exact part identities receive VALIDATED|APPLIED.
	ProducerBaselineResult initialize(const StateImage& current) noexcept;
	ProducerBaselineResult replace_current(const StateImage& current) noexcept;

	ProducerBaselineResult capture_snapshot(std::uint32_t snapshot_id,
		std::uint32_t required_manifest_id,
		const StateImage& captured,
		const std::vector<SnapshotCandidatePart>& parts,
		std::uint64_t now_us) noexcept;
	ProducerBaselineResult acknowledge_snapshot_part(const AckPayload& ack, std::uint64_t now_us) noexcept;
	ProducerBaselineResult expire_candidate(std::uint64_t now_us) noexcept;

	// Builds the complete net difference from the immutable active baseline.
	// No sequence is consumed on NoChange or failure. If the exact encoded
	// delta would exceed one MiB, KeyframeRequired is returned.
	ProducerBaselineResult emit_cumulative_delta(std::uint64_t producer_sample_time_us,
		CumulativeStateDelta& delta) noexcept;

	void clear() noexcept;

	bool has_active_baseline() const noexcept
	{
		return m_active_snapshot_id != 0;
	}
	bool has_candidate() const noexcept
	{
		return m_candidate_snapshot_id != 0;
	}
	std::uint32_t active_snapshot_id() const noexcept
	{
		return m_active_snapshot_id;
	}
	std::uint32_t candidate_snapshot_id() const noexcept
	{
		return m_candidate_snapshot_id;
	}
	std::uint32_t active_required_manifest_id() const noexcept
	{
		return m_active_required_manifest_id;
	}
	std::uint32_t next_delta_sequence() const noexcept
	{
		return m_next_delta_sequence;
	}
	std::size_t active_dirty_record_count() const noexcept;
	std::size_t candidate_dirty_record_count() const noexcept;
	const StateImage& current() const noexcept
	{
		return m_current;
	}
	const StateImage& active_baseline() const noexcept
	{
		return m_active_baseline;
	}

  private:
	StateImage m_current;
	StateImage m_active_baseline;
	StateImage m_candidate_baseline;
	std::vector<SnapshotCandidatePart> m_candidate_parts;
	std::vector<bool> m_candidate_parts_applied;
	std::uint32_t m_active_snapshot_id = 0;
	std::uint32_t m_active_required_manifest_id = 0;
	std::uint32_t m_candidate_snapshot_id = 0;
	std::uint32_t m_candidate_required_manifest_id = 0;
	std::uint32_t m_highest_snapshot_id = 0;
	std::uint32_t m_next_delta_sequence = 1;
	std::uint64_t m_candidate_deadline_us = 0;
	bool m_emitted_delta_for_active_baseline = false;
};

enum class ManifestInstallResult : std::uint8_t {
	Installed = 0,
	AlreadyInstalled = 1,
	Stale = 2,
	InvalidId = 3,
};

enum class SnapshotCandidateResult : std::uint8_t {
	Known = 0,
	AlreadyKnown = 1,
	AlreadyCommitted = 2,
	Stale = 3,
	CandidateBusy = 4,
	InvalidArgument = 5,
};

enum class SnapshotCommitResult : std::uint8_t {
	Committed = 0,
	CommittedAndPendingDeltaApplied = 1,
	CommittedPendingDeltaRejected = 2,
	AlreadyCommitted = 3,
	UnknownCandidate = 4,
	MissingManifest = 5,
	Expired = 6,
	ValidationFailed = 7,
	AllocationFailed = 8,
};

enum class ClientResyncResult : std::uint8_t {
	NotRequested = 0,
	Requested = 1,
	RateLimited = 2,
	Unavailable = 3,
};

enum class ClientResyncScope : std::uint8_t {
	FullSnapshot = 0,
	ManifestAndFullSnapshot = 1,
};

struct ClientResyncChannel {
	std::uint64_t session_id;
	EndpointKey endpoint;
	ProtocolRateLimiter& rate_limiter;
	ResyncRequestPayload& request;
	ClientResyncResult disposition = ClientResyncResult::NotRequested;
};

enum class ClientDeltaResult : std::uint8_t {
	Applied = 0,
	IgnoredOldSequence = 1,
	IgnoredOldBaseline = 2,
	QueuedForCandidate = 3,
	ReplacedQueuedDelta = 4,
	IgnoredOlderQueuedDelta = 5,
	UnknownBaselineResyncRequested = 6,
	UnknownBaselineRateLimited = 7,
	UnknownBaselineResyncUnavailable = 8,
	InvalidDelta = 9,
	ValidationFailed = 10,
	AllocationFailed = 11,
	ResyncRequested = 12,
	ResyncRateLimited = 13,
	ResyncUnavailable = 14,
};

class ClientReplicationModel final {
  public:
	ClientReplicationModel() = default;

	ManifestInstallResult install_manifest(std::uint32_t manifest_id) noexcept;
	SnapshotCandidateResult note_snapshot_candidate(std::uint32_t snapshot_id,
		std::uint32_t required_manifest_id,
		std::uint64_t first_part_time_us,
		std::uint64_t transaction_deadline_us) noexcept;
	SnapshotCommitResult commit_snapshot(std::uint32_t snapshot_id,
		std::uint32_t required_manifest_id,
		const StateImage& snapshot,
		std::uint64_t now_us,
		ClientResyncChannel& resync_channel,
		const StateImageValidator* validator = nullptr) noexcept;

	// Unknown baselines are never copied or queued. If the existing per-session
	// bucket allows it, resync is populated with a reliable UnknownBaseline
	// request; otherwise it is unchanged.
	ClientDeltaResult receive_delta(const CumulativeStateDelta& delta,
		std::uint64_t now_us,
		std::uint64_t session_id,
		const EndpointKey& endpoint,
		ProtocolRateLimiter& rate_limiter,
		ResyncRequestPayload& resync,
		const StateImageValidator* validator = nullptr) noexcept;

	ClientResyncResult request_resynchronization(ResyncReason reason,
		std::uint64_t now_us,
		ClientResyncScope scope,
		ClientResyncChannel& channel) noexcept;
	ClientResyncResult notify_manifest_transaction_expired(std::uint64_t now_us, ClientResyncChannel& channel) noexcept;
	bool expire_snapshot_candidate(std::uint64_t now_us, ClientResyncChannel& resync_channel) noexcept;
	bool expire_pending_delta(std::uint64_t now_us) noexcept;
	void clear() noexcept;

	std::uint32_t installed_manifest_id() const noexcept
	{
		return m_installed_manifest_id;
	}
	std::uint32_t active_snapshot_id() const noexcept
	{
		return m_active_snapshot_id;
	}
	std::uint32_t last_delta_sequence() const noexcept
	{
		return m_last_delta_sequence;
	}
	std::uint32_t candidate_snapshot_id() const noexcept
	{
		return m_candidate_snapshot_id;
	}
	bool has_pending_delta() const noexcept
	{
		return m_has_pending_delta;
	}
	std::uint32_t pending_delta_sequence() const noexcept
	{
		return m_has_pending_delta ? m_pending_delta.delta_sequence : 0;
	}
	const StateImage& active_baseline() const noexcept
	{
		return m_active_baseline;
	}
	const StateImage& published() const noexcept
	{
		return m_published;
	}

  private:
	ClientDeltaResult apply_active_delta(const CumulativeStateDelta& delta,
		const StateImageValidator* validator) noexcept;
	void clear_candidate() noexcept;

	StateImage m_active_baseline;
	StateImage m_published;
	CumulativeStateDelta m_pending_delta;
	std::uint32_t m_installed_manifest_id = 0;
	std::uint32_t m_active_snapshot_id = 0;
	std::uint32_t m_highest_snapshot_id_seen = 0;
	std::uint32_t m_last_delta_sequence = 0;
	std::uint32_t m_candidate_snapshot_id = 0;
	std::uint32_t m_candidate_required_manifest_id = 0;
	std::uint32_t m_next_resync_request_id = 1;
	std::uint64_t m_candidate_first_part_time_us = 0;
	std::uint64_t m_candidate_deadline_us = 0;
	std::uint64_t m_pending_delta_deadline_us = 0;
	bool m_has_pending_delta = false;
};

} // namespace telemetry::protocol
