#pragma once

#include "telemetry/protocol/telemetry_protocol_constants.h"
#include "telemetry/protocol/telemetry_protocol_types.h"
#include "telemetry/protocol/telemetry_sha256.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace telemetry::protocol {

// Metadata which has already been decoded from a bounded ManifestPartPayload
// or FullSnapshotPartPayload. Record framing and semantic validation happen
// above this generic transaction layer.
struct TransactionPart {
	std::uint64_t session_id = 0;
	MessageType message_type = MessageType::Invalid;
	std::uint32_t transaction_id = 0;
	std::uint32_t message_id = 0;
	std::uint16_t part_index = 0;
	std::uint16_t part_count = 0;
	std::uint32_t transaction_size = 0;
	Sha256Digest transaction_sha256{};
	std::uint64_t producer_sample_time_us = 0;
	std::uint32_t frame_id = 0;
	std::int64_t mission_time_us = 0;

	// For Manifest this is the ManifestKind value and
	// required_manifest_id must be zero. For FullSnapshot it is the
	// SnapshotFlags value and required_manifest_id carries its wire field.
	std::uint16_t kind_or_flags = 0;
	std::uint32_t required_manifest_id = 0;

	std::uint16_t record_count = 0;
	ByteView records;
};

struct CompletedTransactionPart {
	std::uint32_t message_id = 0;
	std::uint16_t record_count = 0;
	std::vector<std::uint8_t> records;

	ByteView records_view() const noexcept
	{
		return ByteView{records.empty() ? nullptr : records.data(), records.size()};
	}
};

struct CompletedTransaction {
	std::uint64_t session_id = 0;
	MessageType message_type = MessageType::Invalid;
	std::uint32_t transaction_id = 0;
	std::uint32_t transaction_size = 0;
	Sha256Digest transaction_sha256{};
	std::uint64_t producer_sample_time_us = 0;
	std::uint32_t frame_id = 0;
	std::int64_t mission_time_us = 0;
	std::uint16_t kind_or_flags = 0;
	std::uint32_t required_manifest_id = 0;

	// Ordered by part_index. Keeping the independently bounded part buffers
	// avoids allocating a second transaction-sized copy at publication time;
	// records never cross a part boundary in FSTL 1.0.
	std::vector<CompletedTransactionPart> parts;
};

enum class TransactionAssemblyResult : std::uint8_t {
	Accepted,
	Duplicate,
	Completed,
	StaleTransaction,
	InvalidPart,
	CandidateBusy,
	QuotaExceeded,
	SemanticValidationFailed,
	TransactionSizeMismatch,
	TransactionHashMismatch,
	AllocationFailed,
};

struct TransactionExpirationSummary {
	std::size_t count = 0;
	bool manifest_expired = false;
	bool full_snapshot_expired = false;
	std::uint32_t manifest_id = 0;
	std::uint32_t snapshot_id = 0;
};

struct TransactionIngestOutcome {
	TransactionAssemblyResult result = TransactionAssemblyResult::InvalidPart;
	TransactionExpirationSummary expiration;

	// Keep the status convenient for callers that do not need to branch on an
	// expiration, while still returning the expiration details from every
	// ingest operation.
	constexpr operator TransactionAssemblyResult() const noexcept
	{
		return result;
	}
};

// One instance owns the bounded paged-transaction state for one client. It
// deliberately stops at byte-level transaction validation: record parsing,
// cross-record validation, baseline selection and application ACKs remain the
// responsibility of the state layer.
class TelemetryTransactionAssembler {
  public:
	TelemetryTransactionAssembler() = default;

	// now_ms is an injected monotonic timestamp. completed is left unchanged
	// unless Completed is returned. Any candidate expired by this call is
	// reported in the outcome before the incoming part is considered.
	TransactionIngestOutcome
	ingest(const TransactionPart& part, std::uint64_t now_ms, CompletedTransaction& completed);

	// Candidates expire exactly TransactionAssemblyTimeoutMs after their first
	// accepted part. A backwards-moving injected clock does not expire them.
	// State/session callers use the detailed form so a Manifest expiry can
	// trigger a resync that explicitly requests both Manifest and Snapshot.
	std::size_t expire(std::uint64_t now_ms) noexcept;
	TransactionExpirationSummary expire_with_details(std::uint64_t now_ms) noexcept;

	bool discard(MessageType message_type) noexcept;
	void clear() noexcept;

	std::size_t active_candidates() const noexcept
	{
		return m_reserved_candidates;
	}
	std::size_t reserved_bytes() const noexcept
	{
		return m_reserved_bytes;
	}

  private:
	struct PartSlot {
		bool received = false;
		std::uint32_t message_id = 0;
		std::uint16_t record_count = 0;
		std::vector<std::uint8_t> records;
	};

	struct Candidate {
		std::uint64_t session_id = 0;
		MessageType message_type = MessageType::Invalid;
		std::uint32_t transaction_id = 0;
		std::uint16_t part_count = 0;
		std::uint32_t transaction_size = 0;
		Sha256Digest transaction_sha256{};
		std::uint64_t producer_sample_time_us = 0;
		std::uint32_t frame_id = 0;
		std::int64_t mission_time_us = 0;
		std::uint16_t kind_or_flags = 0;
		std::uint32_t required_manifest_id = 0;
		std::uint64_t first_part_time_ms = 0;
		std::size_t received_count = 0;
		std::size_t received_bytes = 0;
		std::vector<PartSlot> parts;
	};

	std::size_t find_candidate(MessageType message_type) const noexcept;
	void erase_candidate(std::size_t index) noexcept;
	TransactionAssemblyResult complete_candidate(std::size_t index, CompletedTransaction& completed);

	std::vector<Candidate> m_candidates;
	std::size_t m_reserved_candidates = 0;
	std::size_t m_reserved_bytes = 0;
	std::uint32_t m_expired_manifest_id = 0;
	std::uint32_t m_expired_snapshot_id = 0;
};

} // namespace telemetry::protocol
