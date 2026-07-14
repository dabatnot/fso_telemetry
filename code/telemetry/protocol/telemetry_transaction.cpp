#include "telemetry/protocol/telemetry_transaction.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace telemetry::protocol {

namespace {

constexpr std::size_t NoCandidate = std::numeric_limits<std::size_t>::max();
constexpr std::size_t ManifestPartPrefixSize = 56;
constexpr std::size_t FullSnapshotPartPrefixSize = 60;

class CandidateQuotaReservation {
  public:
	explicit CandidateQuotaReservation(std::size_t& candidate_count) noexcept : m_candidate_count(candidate_count) {
		++m_candidate_count;
	}

	~CandidateQuotaReservation() {
		if (!m_committed) {
			--m_candidate_count;
		}
	}

	CandidateQuotaReservation(const CandidateQuotaReservation&) = delete;
	CandidateQuotaReservation& operator=(const CandidateQuotaReservation&) = delete;

	void commit() noexcept { m_committed = true; }

  private:
	std::size_t& m_candidate_count;
	bool m_committed = false;
};

class ByteQuotaReservation {
  public:
	ByteQuotaReservation(std::size_t& reserved_bytes, std::size_t byte_count) noexcept
	    : m_reserved_bytes(reserved_bytes), m_byte_count(byte_count) {
		m_reserved_bytes += m_byte_count;
	}

	~ByteQuotaReservation() {
		if (!m_committed) {
			m_reserved_bytes -= m_byte_count;
		}
	}

	ByteQuotaReservation(const ByteQuotaReservation&) = delete;
	ByteQuotaReservation& operator=(const ByteQuotaReservation&) = delete;

	void commit() noexcept { m_committed = true; }

  private:
	std::size_t& m_reserved_bytes;
	std::size_t m_byte_count;
	bool m_committed = false;
};

bool is_transaction_type(MessageType message_type) noexcept {
	return message_type == MessageType::Manifest || message_type == MessageType::FullSnapshot;
}

std::size_t part_prefix_size(MessageType message_type) noexcept {
	return message_type == MessageType::Manifest ? ManifestPartPrefixSize : FullSnapshotPartPrefixSize;
}

bool has_exactly_one_snapshot_kind(std::uint16_t flags) noexcept {
	return flags == SnapshotFlagInitial || flags == SnapshotFlagPeriodicKeyframe || flags == SnapshotFlagResync;
}

bool is_intrinsically_valid(const TransactionPart& part) noexcept {
	if (!is_transaction_type(part.message_type) || (part.records.size != 0 && part.records.data == nullptr) ||
	    part.session_id == 0 || part.transaction_id == 0 || part.message_id == 0 || part.part_count == 0 ||
	    part.part_count > MaxTransactionParts || part.part_index >= part.part_count ||
	    part.transaction_size == 0 || part.transaction_size > MaxTransactionSize || part.record_count == 0 ||
	    part.transaction_size < part.part_count || part.records.empty() || part.records.size > part.transaction_size ||
	    part.records.size > MaxStatePartSize - part_prefix_size(part.message_type)) {
		return false;
	}

	if (part.message_type == MessageType::Manifest) {
		return part.kind_or_flags == static_cast<std::uint16_t>(ManifestKind::FullRequired) &&
		       part.required_manifest_id == 0 && part.frame_id == 0 && part.mission_time_us == 0;
	}

	return has_exactly_one_snapshot_kind(part.kind_or_flags) && part.frame_id != 0;
}

bool same_bytes(ByteView left, const std::vector<std::uint8_t>& right) noexcept {
	return left.size == right.size() &&
	       (left.empty() || std::equal(left.begin(), left.end(), right.begin()));
}

} // namespace

TransactionAssemblyResult TelemetryTransactionAssembler::ingest(const TransactionPart& part,
	                                                              std::uint64_t now_ms,
	                                                              CompletedTransaction& completed) {
	expire(now_ms);

	if (!is_intrinsically_valid(part)) {
		return TransactionAssemblyResult::InvalidPart;
	}

	auto candidate_index = find_candidate(part.message_type);
	bool created_candidate = false;
	if (candidate_index != NoCandidate) {
		auto& candidate = m_candidates[candidate_index];
		if (candidate.session_id == part.session_id && candidate.transaction_id == part.transaction_id &&
		    candidate.transaction_sha256 != part.transaction_sha256) {
			erase_candidate(candidate_index);
			return TransactionAssemblyResult::SemanticValidationFailed;
		}
		if (candidate.session_id != part.session_id || candidate.message_type != part.message_type ||
		    candidate.transaction_id != part.transaction_id ||
		    candidate.transaction_sha256 != part.transaction_sha256) {
			// A newer transaction cannot evict an older reliable candidate
			// before the latter's absolute assembly window expires.
			return TransactionAssemblyResult::CandidateBusy;
		}
		if (candidate.part_count != part.part_count || candidate.transaction_size != part.transaction_size ||
		    candidate.producer_sample_time_us != part.producer_sample_time_us || candidate.frame_id != part.frame_id ||
		    candidate.mission_time_us != part.mission_time_us || candidate.kind_or_flags != part.kind_or_flags ||
		    candidate.required_manifest_id != part.required_manifest_id) {
			erase_candidate(candidate_index);
			return TransactionAssemblyResult::SemanticValidationFailed;
		}
	} else {
		if (m_reserved_candidates >= MaxCandidateTransactionsPerClient) {
			return TransactionAssemblyResult::QuotaExceeded;
		}

		CandidateQuotaReservation reservation(m_reserved_candidates);
		try {
			Candidate candidate;
			candidate.session_id = part.session_id;
			candidate.message_type = part.message_type;
			candidate.transaction_id = part.transaction_id;
			candidate.part_count = part.part_count;
			candidate.transaction_size = part.transaction_size;
			candidate.transaction_sha256 = part.transaction_sha256;
			candidate.producer_sample_time_us = part.producer_sample_time_us;
			candidate.frame_id = part.frame_id;
			candidate.mission_time_us = part.mission_time_us;
			candidate.kind_or_flags = part.kind_or_flags;
			candidate.required_manifest_id = part.required_manifest_id;
			candidate.first_part_time_ms = now_ms;
			candidate.parts.resize(part.part_count);
			m_candidates.emplace_back(std::move(candidate));
		} catch (const std::bad_alloc&) {
			return TransactionAssemblyResult::AllocationFailed;
		}
		reservation.commit();
		candidate_index = m_candidates.size() - 1;
		created_candidate = true;
	}

	auto& candidate = m_candidates[candidate_index];
	auto& slot = candidate.parts[part.part_index];
	if (slot.received) {
		if (slot.message_id != part.message_id || slot.record_count != part.record_count ||
		    !same_bytes(part.records, slot.records)) {
			erase_candidate(candidate_index);
			return TransactionAssemblyResult::SemanticValidationFailed;
		}
		if (candidate.received_count == candidate.part_count) {
			return complete_candidate(candidate_index, completed);
		}
		return TransactionAssemblyResult::Duplicate;
	}
	for (const auto& existing_part : candidate.parts) {
		if (existing_part.received && existing_part.message_id == part.message_id) {
			erase_candidate(candidate_index);
			return TransactionAssemblyResult::SemanticValidationFailed;
		}
	}

	if (part.records.size > candidate.transaction_size - candidate.received_bytes) {
		erase_candidate(candidate_index);
		return TransactionAssemblyResult::TransactionSizeMismatch;
	}
	if (part.records.size > MaxCandidateTransactionBytesPerClient - m_reserved_bytes) {
		if (created_candidate) {
			erase_candidate(candidate_index);
		}
		return TransactionAssemblyResult::QuotaExceeded;
	}

	ByteQuotaReservation byte_reservation(m_reserved_bytes, part.records.size);
	try {
		slot.records.assign(part.records.begin(), part.records.end());
	} catch (const std::bad_alloc&) {
		if (created_candidate) {
			erase_candidate(candidate_index);
		}
		return TransactionAssemblyResult::AllocationFailed;
	}
	byte_reservation.commit();

	slot.received = true;
	slot.message_id = part.message_id;
	slot.record_count = part.record_count;
	++candidate.received_count;
	candidate.received_bytes += part.records.size;

	if (candidate.received_count != candidate.part_count) {
		return TransactionAssemblyResult::Accepted;
	}
	return complete_candidate(candidate_index, completed);
}

std::size_t TelemetryTransactionAssembler::expire(std::uint64_t now_ms) noexcept {
	std::size_t expired = 0;
	for (std::size_t index = 0; index < m_candidates.size();) {
		const auto first_part_time_ms = m_candidates[index].first_part_time_ms;
		if (now_ms >= first_part_time_ms && now_ms - first_part_time_ms >= TransactionAssemblyTimeoutMs) {
			erase_candidate(index);
			++expired;
		} else {
			++index;
		}
	}
	return expired;
}

bool TelemetryTransactionAssembler::discard(MessageType message_type) noexcept {
	const auto index = find_candidate(message_type);
	if (index == NoCandidate) {
		return false;
	}
	erase_candidate(index);
	return true;
}

void TelemetryTransactionAssembler::clear() noexcept {
	m_candidates.clear();
	m_reserved_candidates = 0;
	m_reserved_bytes = 0;
}

std::size_t TelemetryTransactionAssembler::find_candidate(MessageType message_type) const noexcept {
	for (std::size_t index = 0; index < m_candidates.size(); ++index) {
		if (m_candidates[index].message_type == message_type) {
			return index;
		}
	}
	return NoCandidate;
}

void TelemetryTransactionAssembler::erase_candidate(std::size_t index) noexcept {
	m_reserved_bytes -= m_candidates[index].received_bytes;
	--m_reserved_candidates;
	m_candidates.erase(m_candidates.begin() + static_cast<std::ptrdiff_t>(index));
}

TransactionAssemblyResult TelemetryTransactionAssembler::complete_candidate(std::size_t index,
	                                                                         CompletedTransaction& completed) {
	auto& candidate = m_candidates[index];
	if (candidate.received_bytes != candidate.transaction_size) {
		erase_candidate(index);
		return TransactionAssemblyResult::TransactionSizeMismatch;
	}

	Sha256 hasher;
	for (const auto& part : candidate.parts) {
		const auto view = ByteView{part.records.empty() ? nullptr : part.records.data(), part.records.size()};
		if (!hasher.update(view)) {
			erase_candidate(index);
			return TransactionAssemblyResult::SemanticValidationFailed;
		}
	}

	Sha256Digest digest{};
	if (!hasher.finalize(digest)) {
		erase_candidate(index);
		return TransactionAssemblyResult::SemanticValidationFailed;
	}
	if (digest != candidate.transaction_sha256) {
		erase_candidate(index);
		return TransactionAssemblyResult::TransactionHashMismatch;
	}

	CompletedTransaction published;
	published.session_id = candidate.session_id;
	published.message_type = candidate.message_type;
	published.transaction_id = candidate.transaction_id;
	published.transaction_size = candidate.transaction_size;
	published.transaction_sha256 = candidate.transaction_sha256;
	published.producer_sample_time_us = candidate.producer_sample_time_us;
	published.frame_id = candidate.frame_id;
	published.mission_time_us = candidate.mission_time_us;
	published.kind_or_flags = candidate.kind_or_flags;
	published.required_manifest_id = candidate.required_manifest_id;
	try {
		published.parts.reserve(candidate.parts.size());
		for (const auto& part : candidate.parts) {
			CompletedTransactionPart published_part;
			published_part.message_id = part.message_id;
			published_part.record_count = part.record_count;
			published.parts.emplace_back(std::move(published_part));
		}
	} catch (const std::bad_alloc&) {
		return TransactionAssemblyResult::AllocationFailed;
	}
	for (std::size_t part_index = 0; part_index < candidate.parts.size(); ++part_index) {
		published.parts[part_index].records = std::move(candidate.parts[part_index].records);
	}

	erase_candidate(index);
	completed = std::move(published);
	return TransactionAssemblyResult::Completed;
}

} // namespace telemetry::protocol
