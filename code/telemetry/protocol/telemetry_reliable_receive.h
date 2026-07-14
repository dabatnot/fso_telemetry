#pragma once

#include "telemetry/protocol/telemetry_control_messages.h"
#include "telemetry/protocol/telemetry_reassembler.h"
#include "telemetry/protocol/telemetry_reliable_window.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace telemetry::protocol {

constexpr std::size_t ReliableReceiveMaximumEntries = 4096;
constexpr std::uint64_t ReliableReceiveDeduplicationGraceUs = 2'000'000;
constexpr std::uint64_t SelectiveNackMaximumDelayUs = 25'000;
constexpr std::uint64_t SelectiveNackMinimumIntervalUs = 50'000;
constexpr std::uint64_t SelectiveNackReliableRetentionUs =
	static_cast<std::uint64_t>(ReliableReassemblyTimeoutV1Ms) * 1000U;
constexpr std::uint64_t ReplaceableStateReassemblyRetentionUs = 500'000;
constexpr std::uint64_t VideoInterframeReassemblyRetentionUs = 200'000;
constexpr std::size_t SelectiveNackMaximumVideoEntries = 1;
constexpr std::size_t SelectiveNackMaximumEntries = MaxStateReassembliesPerClient + SelectiveNackMaximumVideoEntries;
constexpr std::size_t ReceivePipelineMaximumEntries = MaxStateReassembliesPerClient + MaxVideoReassembliesPerClient;

// The receiver retains a result for the matching sender window plus the
// normative two-second grace period. SessionEnd therefore naturally leaves a
// seven-second tombstone through its ordinary five-second send window.
std::uint64_t reliable_receive_retention_us(ReliableMessageClass message_class) noexcept;
bool reliable_receive_class_matches_type(ReliableMessageClass message_class, MessageType message_type) noexcept;

struct ReliableReceiveKey {
	std::uint64_t session_id = 0;
	EndpointKey endpoint;
	MessageType message_type = MessageType::Invalid;
	std::uint32_t message_id = 0;
	std::uint32_t message_crc32 = 0;
	std::uint16_t fragment_count = 0;
};

enum class ReliableReceiveOutcomeKind : std::uint8_t {
	Pending = 0,
	Validated = 1,
	Applied = 2,
	Error = 3,
};

// Pending exists only between reservation and the caller recording a terminal
// parse/publication result. A duplicate Pending reservation is still consumed
// by the cache and must not be parsed concurrently a second time.
struct ReliableReceiveOutcome {
	ReliableReceiveOutcomeKind kind = ReliableReceiveOutcomeKind::Pending;
	ValidationError error = ValidationError::None;
};

enum class ReliableReceiveReserveResult : std::uint8_t {
	Reserved,
	Duplicate,
	IdentityConflict,
	ResourceLimit,
	InvalidKey,
};

enum class ReliableReceiveUpdateResult : std::uint8_t {
	Updated,
	Unchanged,
	NotFound,
	IdentityConflict,
	InvalidOutcome,
};

enum class ReliableReceiveLookupResult : std::uint8_t {
	NotFound,
	Found,
	IdentityConflict,
	InvalidKey,
};

// Fixed-capacity, per-client receive cache. No operation allocates. Time is
// caller-injected and clamped monotonically so a regressing clock can neither
// resurrect an expired entry nor extend its retention.
class ReliableReceiveCache final {
  public:
	explicit ReliableReceiveCache(std::size_t maximum_entries = ReliableReceiveMaximumEntries) noexcept;

	// Reserves before parsing. On Duplicate, outcome is the last cached result
	// and the caller must neither parse nor publish the message again. outcome is
	// unchanged for every other result.
	ReliableReceiveReserveResult reserve(const ReliableReceiveKey& key,
		ReliableMessageClass message_class,
		std::uint64_t now_us,
		ReliableReceiveOutcome& outcome) noexcept;

	// Looks up without reserving. Exact identity includes message_crc32; a
	// retained base identity with another CRC is an explicit contradiction.
	ReliableReceiveLookupResult
	lookup(const ReliableReceiveKey& key, std::uint64_t now_us, ReliableReceiveOutcome& outcome) noexcept;

	ReliableReceiveUpdateResult record_validated(const ReliableReceiveKey& key) noexcept;
	ReliableReceiveUpdateResult record_applied(const ReliableReceiveKey& key) noexcept;
	ReliableReceiveUpdateResult record_error(const ReliableReceiveKey& key, ValidationError error) noexcept;
	ValidationError cached_ack(const ReliableReceiveKey& key, std::uint64_t now_us, AckPayload& ack) noexcept;

	std::size_t expire(std::uint64_t now_us) noexcept;
	// SessionEnd publication tears down every other session resource but must
	// keep its APPLIED tombstone for seven seconds so a lost ACK can be replayed
	// without publishing the terminal transition twice.
	std::size_t purge_session_preserving_end_tombstone(std::uint64_t session_id) noexcept;
	std::size_t purge_session_preserving_end_tombstone(std::uint64_t session_id, const EndpointKey& endpoint) noexcept;
	// Full purge, including any SessionEnd tombstone (administrative reset or
	// final cache destruction).
	std::size_t purge_session(std::uint64_t session_id) noexcept;
	std::size_t purge_session(std::uint64_t session_id, const EndpointKey& endpoint) noexcept;
	void clear() noexcept;

	std::size_t entry_count() const noexcept
	{
		return m_entry_count;
	}
	std::size_t maximum_entries() const noexcept
	{
		return m_maximum_entries;
	}
	std::uint64_t monotonic_time_us() const noexcept
	{
		return m_monotonic_time_us;
	}

  private:
	struct Entry {
		bool occupied = false;
		ReliableReceiveKey key;
		ReliableReceiveOutcome outcome;
		std::uint64_t expires_at_us = 0;
	};

	std::uint64_t advance_time(std::uint64_t now_us) noexcept;
	std::size_t expire_at(std::uint64_t now_us) noexcept;
	std::size_t find_exact(const ReliableReceiveKey& key) const noexcept;
	std::size_t find_base_identity(const ReliableReceiveKey& key) const noexcept;
	std::size_t find_free() const noexcept;
	void erase(std::size_t index) noexcept;
	ReliableReceiveUpdateResult record(const ReliableReceiveKey& key, ReliableReceiveOutcome outcome) noexcept;

	std::array<Entry, ReliableReceiveMaximumEntries> m_entries{};
	std::size_t m_maximum_entries = ReliableReceiveMaximumEntries;
	std::size_t m_entry_count = 0;
	std::uint64_t m_monotonic_time_us = 0;
};

enum class SelectiveNackObserveResult : std::uint8_t {
	Accepted,
	Duplicate,
	Completed,
	IgnoredIneligible,
	InvalidFragment,
	IdentityConflict,
	ResourceLimit,
};

enum class SelectiveNackPollResult : std::uint8_t {
	None,
	Ready,
};

enum class SelectiveNackActionKind : std::uint8_t {
	SendNack,
	RequestResync,
	RequestKeyframe,
};

struct SelectiveNackAction {
	SelectiveNackActionKind kind = SelectiveNackActionKind::SendNack;
	ReliableMessageKey target;
	NackPayload nack;
};

// Owner-side evictions are deliberately separate from network recovery
// actions. Several logical messages may expire at once even though one session-
// level ResyncRequest is sufficient on the wire; every evicted identity must
// still release its reassembly bytes and quota.
struct SelectiveNackRetirements {
	std::array<ReliableMessageKey, SelectiveNackMaximumEntries> targets{};
	std::size_t count = 0;

	bool append(const ReliableMessageKey& target) noexcept
	{
		if (count >= targets.size()) {
			return false;
		}
		targets[count++] = target;
		return true;
	}
};

// True only for fragmented ACK_REQUIRED messages and fragmented video IDRs.
// The input must already have passed the contextual endpoint/session stage;
// this helper still defensively validates the canonical fragment layout.
bool is_selective_nack_candidate(const TelemetryDatagramHeader& header) noexcept;

// Fixed state mirroring the receiver's four reliable-state reassemblies plus
// the single latest recoverable video IDR. Interframes and superseded IDRs may
// still occupy the separate reassembler quota, but never consume NACK state.
// No operation allocates from peer-controlled input. Returned bitmap views
// point into the tracker and remain valid until the next mutating operation.
class SelectiveNackTracker final {
  public:
	SelectiveNackTracker() noexcept = default;

	SelectiveNackObserveResult observe_validated_fragment(const DatagramView& fragment,
		const EndpointKey& source_endpoint,
		std::uint64_t now_us,
		std::uint64_t base_rto_us,
		std::uint64_t needed_before_producer_time_us = 0,
		std::uint64_t estimated_producer_time_us = 0,
		SelectiveNackRetirements* retirements = nullptr) noexcept;

	// Advances only expiration state. This lets the owning receive pipeline free
	// every retired reassembly before it admits another peer-controlled buffer.
	void advance_expiration(std::uint64_t now_us,
		std::uint64_t estimated_producer_time_us = 0,
		SelectiveNackRetirements* retirements = nullptr) noexcept;

	// Returns at most one action. Expiration recovery is selected before a NACK;
	// estimated_producer_time_us is zero when the clock filter is invalid.
	// Repeated NACKs for one incomplete message are separated by at least 50 ms.
	SelectiveNackPollResult poll(std::uint64_t now_us,
		std::uint64_t estimated_producer_time_us,
		SelectiveNackAction& action,
		SelectiveNackRetirements* retirements = nullptr) noexcept;

	// Removes only live fragment-tracking state. Owner rollback uses this form so
	// it cannot accidentally cancel a recovery already queued by an earlier
	// expiration of the same logical identity.
	bool discard_active(std::uint64_t session_id,
		const EndpointKey& endpoint,
		MessageType message_type,
		std::uint32_t message_id) noexcept;
	// Restores the recovery obligation when owner admission or full-message
	// validation fails after the tracker accepted a new IDR and invalidated the
	// prior keyframe request.
	void queue_keyframe_recovery(const ReliableMessageKey& target) noexcept;
	bool discard(std::uint64_t session_id,
		const EndpointKey& endpoint,
		MessageType message_type,
		std::uint32_t message_id) noexcept;
	std::size_t purge_session(std::uint64_t session_id) noexcept;
	std::size_t purge_session(std::uint64_t session_id, const EndpointKey& endpoint) noexcept;
	void clear() noexcept;

	std::size_t active_entries(MessageSizeClass message_class) const noexcept;
	std::uint64_t monotonic_time_us() const noexcept
	{
		return m_monotonic_time_us;
	}

  private:
	struct Entry {
		bool occupied = false;
		std::uint64_t session_id = 0;
		EndpointKey endpoint;
		MessageType message_type = MessageType::Invalid;
		MessageSizeClass message_class = MessageSizeClass::Invalid;
		std::uint32_t message_id = 0;
		std::uint32_t message_crc32 = 0;
		std::uint32_t message_size = 0;
		std::uint16_t fragment_count = 0;
		std::uint16_t received_count = 0;
		std::uint16_t highest_received = 0;
		std::uint64_t needed_before_producer_time_us = 0;
		std::uint64_t first_receive_us = 0;
		std::uint64_t absolute_deadline_us = 0;
		std::uint64_t nack_due_us = 0;
		std::uint64_t last_nack_us = 0;
		bool has_received = false;
		bool nack_armed = false;
		bool has_sent_nack = false;
		std::array<std::uint8_t, MaxNackBitmapBytes> received{};
		std::array<std::uint8_t, MaxNackBitmapBytes> missing{};
	};
	struct PendingRecovery {
		bool occupied = false;
		SelectiveNackActionKind kind = SelectiveNackActionKind::RequestResync;
		ReliableMessageKey target;
	};

	std::uint64_t advance_time(std::uint64_t now_us) noexcept;
	std::size_t find_base(const TelemetryDatagramHeader& header, const EndpointKey& endpoint) const noexcept;
	std::size_t find_free() const noexcept;
	void erase(std::size_t index, SelectiveNackRetirements* retirements = nullptr) noexcept;
	bool has_revealed_hole(const Entry& entry) const noexcept;
	void build_missing_bitmap(Entry& entry) noexcept;
	std::size_t find_expired(std::uint64_t now_us, std::uint64_t estimated_producer_time_us) const noexcept;
	void expire_to_pending(std::uint64_t now_us,
		std::uint64_t estimated_producer_time_us,
		SelectiveNackRetirements* retirements) noexcept;
	void queue_recovery(SelectiveNackActionKind kind, const ReliableMessageKey& target) noexcept;
	void cancel_pending_keyframe(std::uint64_t session_id, const EndpointKey& endpoint) noexcept;
	bool pop_recovery(SelectiveNackAction& action) noexcept;

	std::array<Entry, SelectiveNackMaximumEntries> m_entries{};
	std::size_t m_state_entries = 0;
	std::size_t m_video_entries = 0;
	std::uint64_t m_monotonic_time_us = 0;
	std::array<PendingRecovery, SelectiveNackMaximumEntries> m_pending_recoveries{};
	std::size_t m_pending_recovery_count = 0;
	bool m_has_latest_video_idr = false;
	std::uint64_t m_latest_video_session_id = 0;
	EndpointKey m_latest_video_endpoint;
	std::uint32_t m_latest_video_message_id = 0;
};

struct ReliableReceivePipelineResult {
	SelectiveNackObserveResult tracking = SelectiveNackObserveResult::IgnoredIneligible;
	ReassemblyResult reassembly = ReassemblyResult::InvalidLayout;
	bool reassembly_attempted = false;
};

// Sole owner of all fragmented-message reassembly for one authenticated client
// and receive direction. Selective NACK tracking is conditional, but reliable
// state, replaceable state, IDRs, and interframes share the same normative
// reassembly quotas and are expired through this one API.
class ReliableReceivePipeline final {
  public:
	ReliableReceivePipeline() noexcept = default;
	explicit ReliableReceivePipeline(GlobalReassemblyBudget& global_budget) noexcept : m_reassembler(global_budget) {}

	// estimated_producer_time_us is zero while the clock filter is invalid. A
	// nonzero IDR recovery deadline is checked before any allocation or payload
	// publication, including for an unfragmented IDR.
	ReliableReceivePipelineResult ingest_validated_fragment(const DatagramView& fragment,
		const EndpointKey& source_endpoint,
		std::uint64_t now_us,
		std::uint64_t base_rto_us,
		ReassembledMessage& completed,
		std::uint64_t needed_before_producer_time_us = 0,
		std::uint64_t estimated_producer_time_us = 0);

	// A recovery action is published only after the matching reassembly bytes
	// and quota have been released. SendNack leaves the incomplete message live.
	SelectiveNackPollResult
	poll(std::uint64_t now_us, std::uint64_t estimated_producer_time_us, SelectiveNackAction& action) noexcept;

	bool discard(std::uint64_t session_id,
		const EndpointKey& endpoint,
		MessageType message_type,
		std::uint32_t message_id) noexcept;
	void clear() noexcept;

	std::size_t active_reassemblies(MessageSizeClass message_class) const noexcept
	{
		return m_reassembler.active_reassemblies(message_class);
	}
	std::size_t reserved_bytes(MessageSizeClass message_class) const noexcept
	{
		return m_reassembler.reserved_bytes(message_class);
	}
	std::size_t tracked_entries(MessageSizeClass message_class) const noexcept
	{
		return m_tracker.active_entries(message_class);
	}

  private:
	enum class SilentMessageClass : std::uint8_t {
		Invalid = 0,
		Delta,
		ReplaceableEvent,
		VideoInterframe,
	};
	static constexpr std::size_t SilentMessageClassCount = 3;

	struct SilentHighWatermark {
		bool occupied = false;
		std::uint64_t session_id = 0;
		EndpointKey endpoint;
		std::uint32_t message_id = 0;
	};

	struct ReassemblyLifetime {
		bool occupied = false;
		ReliableMessageKey key;
		std::uint32_t message_size = 0;
		std::uint64_t expires_at_us = 0;
		bool selectively_tracked = false;
		SilentMessageClass silent_class = SilentMessageClass::Invalid;
	};

	static ReliableMessageKey key_for(const TelemetryDatagramHeader& header, const EndpointKey& endpoint) noexcept;
	static std::uint64_t reassembly_retention_us(const TelemetryDatagramHeader& header) noexcept;
	static SilentMessageClass silent_message_class(const TelemetryDatagramHeader& header) noexcept;
	static std::size_t silent_high_watermark_index(SilentMessageClass message_class) noexcept;
	bool silent_high_watermark_blocks(SilentMessageClass message_class, const ReliableMessageKey& key) const noexcept;
	void record_silent_terminal(SilentMessageClass message_class, const ReliableMessageKey& key) noexcept;
	std::size_t find_lifetime_base(const ReliableMessageKey& key) const noexcept;
	std::size_t find_lifetime_storage_base(std::uint64_t session_id, std::uint32_t message_id) const noexcept;
	std::size_t find_free_lifetime() const noexcept;
	void erase_lifetime(std::size_t index) noexcept;
	void discard_reassembly(const ReliableMessageKey& key) noexcept;
	void discard_storage_base(std::uint64_t session_id, std::uint32_t message_id) noexcept;
	void apply_retirements(const SelectiveNackRetirements& retirements) noexcept;
	void expire_lifetimes(std::uint64_t now_us) noexcept;
	void recover_failed_idr(const ReliableMessageKey& key, bool video_idr) noexcept;

	TelemetryReassembler m_reassembler;
	SelectiveNackTracker m_tracker;
	std::array<ReassemblyLifetime, ReceivePipelineMaximumEntries> m_lifetimes{};
	std::array<SilentHighWatermark, SilentMessageClassCount> m_silent_high_watermarks{};
};

// Constructs a bitmap-free error NACK only after re-validating the exact
// endpoint/session context. It deliberately performs no rate limiting; callers
// must pass the result through the common ACK/NACK and per-target buckets.
ValidationError make_context_validated_error_nack(const TelemetryDatagramHeader& target,
	const EndpointKey& source_endpoint,
	const TelemetrySessionContext& session_context,
	NackReason reason,
	std::uint64_t needed_before_producer_time_us,
	NackPayload& nack) noexcept;

// Unknown message types cannot pass the ordinary contextual validator. This
// dedicated path requires that validator to reach UnknownMessageType only
// after matching the active session and exact endpoint, and also requires the
// peer's ACK_REQUIRED intent before producing reason 6.
ValidationError make_context_validated_unsupported_message_nack(const TelemetryDatagramHeader& target,
	const EndpointKey& source_endpoint,
	const TelemetrySessionContext& session_context,
	NackPayload& nack) noexcept;

} // namespace telemetry::protocol
