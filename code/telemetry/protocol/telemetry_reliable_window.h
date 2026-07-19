#pragma once

#include "telemetry/protocol/telemetry_reliability_messages.h"
#include "telemetry/protocol/telemetry_session_context.h"
#include "telemetry/protocol/telemetry_sha256.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace telemetry::protocol {

constexpr std::uint64_t ReliableDefaultRtoUs = 250'000;
constexpr std::uint64_t ReliableMinimumRtoUs = 100'000;
constexpr std::uint64_t ReliableMaximumRtoUs = 1'000'000;
constexpr std::uint64_t ReliableOrdinaryRetentionUs = 5'000'000;
constexpr std::uint64_t ReliableTransactionRetentionUs = 10'000'000;
constexpr std::uint64_t ReliableVideoIdrRetentionUs = 500'000;

// These are implementation ceilings, not wire values. Configuration may
// lower them but cannot turn a per-client send window into an unbounded cache.
constexpr std::size_t ReliableWindowMaximumEntries = 4096;
constexpr std::size_t ReliableWindowMaximumRetainedBytes = MaxCandidateTransactionBytesPerClient;
constexpr std::size_t ReliableWindowMaximumVideoEntries = 1;
constexpr std::size_t ReliableWindowMaximumVideoRetainedBytes = MaxVideoMessageSize;
constexpr std::size_t ReliableWindowDefaultEntries = 256;
constexpr std::size_t ReliableWindowDefaultRetainedBytes = MaxCandidateTransactionBytesPerClient;

enum class RequiredAckLevel : std::uint8_t {
	None = 0,
	Validated = static_cast<std::uint8_t>(AckFlag::Validated),
	Applied = KnownAckFlags,
};

// The class fixes the retention duration, strict scheduler priority and
// terminal policy. Keeping policy in the class makes timeout behavior explicit
// at reservation time instead of guessing after the payload has expired.
enum class ReliableMessageClass : std::uint8_t {
	ControlDrop = 1,
	ControlRequestKeyframe = 2,
	ControlRequestResync = 3,
	SessionCritical = 4,
	SessionClosing = 5,
	Transaction = 6,
	ReliableEvent = 7,
	VideoIdr = 8,
	HandshakeCritical = 9,
	// HELLO is the only retained logical message with session_id == 0. It is
	// repeated until a matching WELCOME is received, then removed explicitly
	// with discard(); it is never ACKed by an AckPayload.
	HelloNegotiation = 10,
};

enum class ReliableTerminalPolicy : std::uint8_t {
	Drop = 1,
	RequestKeyframe = 2,
	RequestResync = 3,
	MarkSessionStale = 4,
	CloseSession = 5,
};

struct ReliableWindowLimits {
	// These limits apply only to reliable/control state. The last retained IDR
	// has its own fixed one-entry, MaxVideoMessageSize budget so video can never
	// consume capacity needed by a reliable message.
	std::size_t max_entries = ReliableWindowDefaultEntries;
	std::size_t max_retained_bytes = ReliableWindowDefaultRetainedBytes;
	// Optional startup-only payload pool. Zero preserves the general-purpose
	// window behaviour; a bounded runtime can reserve one fixed payload backing
	// per entry and therefore retain without allocating after Ready.
	std::size_t preallocated_payload_bytes_per_entry = 0U;
	// A local, test-injectable secret. The peer controls neither this value nor
	// the final jitter because session_id, message_id and retry number are mixed.
	std::uint64_t jitter_seed = 0x4653544c5f52544fULL;
};

struct ReliableMessageToRetain {
	std::uint64_t session_id = 0;
	EndpointKey endpoint;
	MessageType message_type = MessageType::Invalid;
	std::uint8_t base_flags = MessageFlagNone;
	std::uint32_t frame_id = 0;
	std::int64_t mission_time_us = 0;
	std::uint32_t message_id = 0;
	std::uint16_t fragment_count = 0;
	std::uint32_t message_crc32 = 0;
	// Required only for Manifest/FullSnapshot transaction parts. All parts in
	// one group repeat this wire identity and share the first part's deadline.
	std::uint32_t transaction_id = 0;
	Sha256Digest transaction_sha256{};
	ByteView logical_payload;
	RequiredAckLevel required_ack = RequiredAckLevel::None;
	ReliableMessageClass message_class = ReliableMessageClass::ControlDrop;
};

enum class ReliableRetainResult : std::uint8_t {
	Retained,
	Duplicate,
	InvalidMessage,
	IdentityConflict,
	QuotaExceeded,
	TransactionBusy,
	ClockOverflow,
	AllocationFailed,
};

enum class ReliableResponseResult : std::uint8_t {
	ValidatedRetained,
	Released,
	Duplicate,
	IgnoredUnknownOrLate,
	IgnoredIncoherent,
};

// Stable ValidationError projection used by ingress metrics and the external
// conformance-vector replay. The send window keeps its richer disposition so
// callers can distinguish benign duplicates from rejected responses.
constexpr ValidationError reliable_response_validation_error(ReliableResponseResult result) noexcept
{
	switch (result) {
	case ReliableResponseResult::ValidatedRetained:
	case ReliableResponseResult::Released:
	case ReliableResponseResult::Duplicate:
		return ValidationError::None;
	case ReliableResponseResult::IgnoredUnknownOrLate:
	case ReliableResponseResult::IgnoredIncoherent:
	default:
		return ValidationError::InvalidStateTransition;
	}
}

struct ReliableMessageKey {
	std::uint64_t session_id = 0;
	EndpointKey endpoint;
	MessageType message_type = MessageType::Invalid;
	std::uint32_t message_id = 0;
	std::uint16_t fragment_count = 0;
	std::uint32_t message_crc32 = 0;
	std::uint32_t transaction_id = 0;
	Sha256Digest transaction_sha256{};
};

enum class ReliableNackDecisionKind : std::uint8_t {
	SelectiveRetransmissionScheduled,
	FullRetransmissionScheduled,
	WaitForScheduledRetry,
	TerminalPolicy,
};

struct ReliableNackDecision {
	ReliableNackDecisionKind kind = ReliableNackDecisionKind::WaitForScheduledRetry;
	ReliableTerminalPolicy terminal_policy = ReliableTerminalPolicy::Drop;
	ReliableMessageKey target;
};

struct ReliableFragmentSelection {
	bool all_fragments = false;
	std::uint16_t fragment_count = 0;
	std::uint16_t bitmap_bytes = 0;
	std::array<std::uint8_t, MaxNackBitmapBytes> bitmap{};

	// selected is unchanged when the selection or index is invalid.
	ValidationError is_selected(std::uint16_t fragment_index, bool& selected) const noexcept;
};

struct ReliableRetransmissionView {
	ReliableMessageKey key;
	std::uint8_t base_flags = MessageFlagNone;
	std::uint32_t frame_id = 0;
	std::int64_t mission_time_us = 0;
	ByteView logical_payload;
	ReliableFragmentSelection fragments;
	std::uint32_t retry_number = 0;
	std::uint64_t absolute_deadline_us = 0;
};

enum class ReliableWindowActionKind : std::uint8_t {
	Retransmit,
	TerminalPolicy,
};

struct ReliableWindowAction {
	ReliableWindowActionKind kind = ReliableWindowActionKind::Retransmit;
	ReliableTerminalPolicy terminal_policy = ReliableTerminalPolicy::Drop;
	ReliableMessageKey target;
	ReliableRetransmissionView retransmission;
};

enum class ReliablePullResult : std::uint8_t {
	None,
	Action,
};

struct ReliableWindowCounters {
	std::uint64_t retained = 0;
	std::uint64_t quota_rejected = 0;
	std::uint64_t invalid_rejected = 0;
	std::uint64_t retransmissions = 0;
	std::uint64_t expirations = 0;
	std::uint64_t ack_validated = 0;
	std::uint64_t ack_applied = 0;
	std::uint64_t ack_ignored_unknown_or_late = 0;
	std::uint64_t ack_ignored_incoherent = 0;
	std::uint64_t nack_accepted = 0;
	std::uint64_t nack_ignored_unknown_or_late = 0;
	std::uint64_t nack_ignored_incoherent = 0;
};

// Allocation-free control-only composition for runtimes that must preallocate
// every byte before becoming Ready. It deliberately accepts only the two
// ordinary reliable control datagrams a Phase 1 client slot can retain at
// once; transaction, event and video retention remain with ReliableSendWindow.
class PreallocatedReliableControlWindow final {
  public:
	static constexpr std::size_t MaximumEntries = 2U;
	static constexpr std::size_t PayloadBytesPerEntry = MaxDatagramSize;

	void configure() noexcept;
	ReliableRetainResult retain(const ReliableMessageToRetain& message,
		std::uint64_t first_send_time_us) noexcept;
	ReliableResponseResult acknowledge(std::uint64_t session_id,
		const EndpointKey& endpoint,
		const AckPayload& ack,
		std::uint64_t now_us) noexcept;
	ReliableResponseResult reject(std::uint64_t session_id,
		const EndpointKey& endpoint,
		const NackPayload& nack,
		std::uint64_t now_us,
		ReliableNackDecision& decision) noexcept;
	ReliablePullResult pull_next_action(std::uint64_t now_us, ReliableWindowAction& action) noexcept;
	bool discard(std::uint64_t session_id, const EndpointKey& endpoint, std::uint32_t message_id) noexcept;
	void clear() noexcept;
	std::size_t entry_count() const noexcept { return m_size; }
	std::size_t retained_bytes() const noexcept { return m_retained_bytes; }

  private:
	struct Entry {
		bool used = false;
		ReliableMessageKey key;
		std::uint8_t base_flags = MessageFlagNone;
		std::uint32_t frame_id = 0U;
		std::int64_t mission_time_us = 0;
		RequiredAckLevel required_ack = RequiredAckLevel::None;
		ReliableMessageClass message_class = ReliableMessageClass::ControlDrop;
		std::array<std::uint8_t, PayloadBytesPerEntry> payload{};
		std::size_t payload_size = 0U;
		std::uint64_t absolute_deadline_us = 0U;
		std::uint64_t next_retry_at_us = 0U;
		std::uint32_t retry_number = 0U;
		bool validated = false;
		bool immediate_retry = false;
		ReliableFragmentSelection fragments;
	};

	std::size_t find(std::uint64_t session_id, const EndpointKey& endpoint, std::uint32_t message_id) const noexcept;
	std::size_t free_slot() const noexcept;
	void erase(std::size_t index) noexcept;
	std::array<Entry, MaximumEntries> m_entries{};
	std::size_t m_size = 0U;
	std::size_t m_retained_bytes = 0U;
};

// Computes clamp(2 * minimum_rtt, 100 ms, 1000 ms) without overflow. When no
// valid RTT window exists, the v1.0 default of 250 ms is returned.
std::uint64_t reliable_base_rto_us(bool has_valid_minimum_rtt, std::uint64_t minimum_rtt_us) noexcept;

// Integer-only deterministic jitter. The result applies a uniformly selected
// basis-point multiplier in the inclusive range [9000, 11000] after capping
// exponential backoff at one second.
std::uint64_t reliable_retry_delay_us(std::uint64_t base_rto_us,
	std::uint64_t jitter_seed,
	std::uint64_t session_id,
	std::uint32_t message_id,
	std::uint32_t retry_number) noexcept;

// A pure, per-client, per-direction scheduler. Callers retain ownership of
// packet_sequence and sent_time_us; every Retransmit action preserves the
// logical identity and tells the caller which canonical fragment indices to
// emit with RETRANSMISSION set.
class ReliableSendWindow final {
  public:
	ReliableSendWindow() noexcept = default;
	ReliableSendWindow(const ReliableSendWindow&) = delete;
	ReliableSendWindow& operator=(const ReliableSendWindow&) = delete;
	ReliableSendWindow(ReliableSendWindow&& other) noexcept;
	ReliableSendWindow& operator=(ReliableSendWindow&& other) noexcept;

	// On failure, window is unchanged. A successful reconfiguration clears any
	// prior entries and counters.
	static ValidationError configure(const ReliableWindowLimits& limits, ReliableSendWindow& window) noexcept;

	void set_minimum_rtt_us(bool valid, std::uint64_t minimum_rtt_us) noexcept;
	std::uint64_t base_rto_us() const noexcept;

	// first_send_time_us anchors both the first retry and an immutable absolute
	// deadline. Every field and quota is validated before payload bytes are
	// copied. Existing reliable entries are never evicted to admit a new one.
	ReliableRetainResult retain(const ReliableMessageToRetain& message, std::uint64_t first_send_time_us);

	ReliableResponseResult acknowledge(std::uint64_t session_id,
		const EndpointKey& endpoint,
		const AckPayload& ack,
		std::uint64_t now_us) noexcept;

	// decision is unchanged for ignored responses. MissingFragments stores the
	// exact current bitmap and schedules it immediately. Other reasons always
	// return an explicit full-retry, wait or terminal-policy decision.
	ReliableResponseResult reject(std::uint64_t session_id,
		const EndpointKey& endpoint,
		const NackPayload& nack,
		std::uint64_t now_us,
		ReliableNackDecision& decision) noexcept;

	// Returns at most one action. action is unchanged when None is returned.
	// A retransmission payload view stays valid only until the next non-const
	// operation on this window and must therefore be consumed immediately.
	ReliablePullResult pull_next_action(std::uint64_t now_us, ReliableWindowAction& action) noexcept;

	bool discard(std::uint64_t session_id, const EndpointKey& endpoint, std::uint32_t message_id) noexcept;
	std::size_t discard_session(std::uint64_t session_id) noexcept;
	void clear() noexcept;

	std::size_t entry_count() const noexcept
	{
		return m_entries.size();
	}
	std::size_t retained_bytes() const noexcept
	{
		return m_retained_bytes;
	}
	std::size_t video_entry_count() const noexcept
	{
		return m_video_entry_count;
	}
	std::size_t video_retained_bytes() const noexcept
	{
		return m_video_retained_bytes;
	}
	std::size_t reliable_entry_count() const noexcept
	{
		return m_entries.size() - m_video_entry_count;
	}
	std::size_t reliable_retained_bytes() const noexcept
	{
		return m_retained_bytes - m_video_retained_bytes;
	}
	const ReliableWindowLimits& limits() const noexcept
	{
		return m_limits;
	}
	const ReliableWindowCounters& counters() const noexcept
	{
		return m_counters;
	}
	std::uint64_t allocation_events() const noexcept { return m_allocation_events; }
	// Bytes in the startup-owned vector capacities. This deliberately prices
	// both metadata vectors and the payload backing retained by the free-entry
	// pool; sizeof(ReliableSendWindow) alone excludes all of these allocations.
	static std::size_t preallocated_heap_bytes(std::size_t entry_count,
		std::size_t payload_bytes_per_entry) noexcept;
	std::size_t owned_preallocated_heap_bytes() const noexcept;

  private:
	struct Entry {
		ReliableMessageKey key;
		std::uint8_t base_flags = MessageFlagNone;
		std::uint32_t frame_id = 0;
		std::int64_t mission_time_us = 0;
		RequiredAckLevel required_ack = RequiredAckLevel::None;
		ReliableMessageClass message_class = ReliableMessageClass::ControlDrop;
		std::vector<std::uint8_t> payload;
		std::uint64_t absolute_deadline_us = 0;
		std::uint64_t next_retry_at_us = 0;
		// Earliest non-zero deadline ever accepted for this retained IDR. It
		// never increases, even after an immediate retransmission is consumed.
		std::uint64_t video_recovery_deadline_us = 0;
		std::uint32_t retry_number = 0;
		bool validated = false;
		bool immediate_retry = false;
		bool retry_all_fragments = true;
		std::uint64_t retry_needed_before_us = 0;
		std::uint16_t missing_bitmap_bytes = 0;
		std::array<std::uint8_t, MaxNackBitmapBytes> missing_bitmap{};
	};

	std::size_t find(std::uint64_t session_id, const EndpointKey& endpoint, std::uint32_t message_id) const noexcept;
	void erase(std::size_t index) noexcept;
	std::size_t erase_transaction_group(std::uint64_t session_id,
		const EndpointKey& endpoint,
		MessageType message_type,
		std::uint32_t transaction_id,
		const Sha256Digest& transaction_sha256) noexcept;

	ReliableWindowLimits m_limits{};
	std::vector<Entry> m_entries;
	std::vector<Entry> m_free_entries;
	std::size_t m_retained_bytes = 0;
	std::size_t m_video_entry_count = 0;
	std::size_t m_video_retained_bytes = 0;
	bool m_has_valid_minimum_rtt = false;
	std::uint64_t m_minimum_rtt_us = 0;
	ReliableWindowCounters m_counters{};
	std::uint64_t m_allocation_events = 0U;
};

} // namespace telemetry::protocol
