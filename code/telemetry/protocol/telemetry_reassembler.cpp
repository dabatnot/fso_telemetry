#include "telemetry/protocol/telemetry_reassembler.h"

#include "telemetry/protocol/telemetry_protocol_constants.h"
#include "telemetry/protocol/telemetry_security.h"

#include <algorithm>
#include <limits>
#include <new>

namespace telemetry::protocol {

namespace {

constexpr std::size_t NoEntry = std::numeric_limits<std::size_t>::max();

struct ReassemblyLimits {
	std::size_t max_message_size = 0;
	std::size_t max_fragment_count = 0;
	std::size_t max_reassemblies = 0;
	std::size_t max_reserved_bytes = 0;
};

class QuotaReservation {
  public:
	QuotaReservation(std::size_t& count, std::size_t& bytes, std::size_t message_size) noexcept
	    : m_count(count), m_bytes(bytes), m_message_size(message_size) {
		++m_count;
		m_bytes += m_message_size;
	}

	~QuotaReservation() {
		if (!m_committed) {
			--m_count;
			m_bytes -= m_message_size;
		}
	}

	QuotaReservation(const QuotaReservation&) = delete;
	QuotaReservation& operator=(const QuotaReservation&) = delete;

	void commit() noexcept { m_committed = true; }

  private:
	std::size_t& m_count;
	std::size_t& m_bytes;
	std::size_t m_message_size;
	bool m_committed = false;
};

class GlobalQuotaReservation {
  public:
	GlobalQuotaReservation(GlobalReassemblyBudget* budget,
		MessageSizeClass message_class,
		std::size_t message_size) noexcept
		: m_budget(budget), m_message_class(message_class), m_message_size(message_size)
	{
	}

	~GlobalQuotaReservation()
	{
		if (!m_committed && m_budget != nullptr) {
			m_budget->release(m_message_class, m_message_size);
		}
	}

	GlobalQuotaReservation(const GlobalQuotaReservation&) = delete;
	GlobalQuotaReservation& operator=(const GlobalQuotaReservation&) = delete;

	void commit() noexcept
	{
		m_committed = true;
	}

  private:
	GlobalReassemblyBudget* m_budget = nullptr;
	MessageSizeClass m_message_class = MessageSizeClass::Invalid;
	std::size_t m_message_size = 0;
	bool m_committed = false;
};

bool limits_for(MessageSizeClass message_class, ReassemblyLimits& limits) noexcept {
	switch (message_class) {
	case MessageSizeClass::State:
		limits = ReassemblyLimits{MaxStateMessageSize,
		                          MaxStateFragments,
		                          MaxStateReassembliesPerClient,
		                          MaxStateReassemblyBytesPerClient};
		return true;
	case MessageSizeClass::Video:
		limits = ReassemblyLimits{MaxVideoMessageSize,
		                          MaxVideoFragments,
		                          MaxVideoReassembliesPerClient,
		                          MaxVideoReassemblyBytesPerClient};
		return true;
	default:
		return false;
	}
}

bool common_fields_match(const TelemetryDatagramHeader& first,
                         const TelemetryDatagramHeader& next) noexcept {
	constexpr auto CommonFlagMask = static_cast<std::uint8_t>(~MessageFlagRetransmission);
	return first.version_major == next.version_major && first.version_minor == next.version_minor &&
	       first.message_type == next.message_type && (first.flags & CommonFlagMask) == (next.flags & CommonFlagMask) &&
	       first.session_id == next.session_id && first.frame_id == next.frame_id &&
	       first.mission_time_us == next.mission_time_us && first.message_id == next.message_id &&
	       first.fragment_count == next.fragment_count && first.message_size == next.message_size &&
	       first.message_crc32 == next.message_crc32;
}

bool same_bytes(ByteView left, const std::vector<std::uint8_t>& right, std::size_t offset) noexcept {
	if (left.size == 0) {
		return true;
	}
	return std::equal(left.begin(), left.end(), right.begin() + static_cast<std::ptrdiff_t>(offset));
}

} // namespace

TelemetryReassembler::TelemetryReassembler(GlobalReassemblyBudget& global_budget) noexcept
	: m_global_budget(&global_budget), m_global_client_admitted(global_budget.try_register_client())
{
}

TelemetryReassembler::~TelemetryReassembler()
{
	clear();
	if (m_global_budget != nullptr && m_global_client_admitted) {
		m_global_budget->release_client();
		m_global_client_admitted = false;
	}
}

ReassemblyResult TelemetryReassembler::ingest(const DatagramView& fragment, ReassembledMessage& completed) {
	const auto& header = fragment.header;
	const auto existing_index = find_entry(header.session_id, header.message_id);

	if ((fragment.payload.size != 0 && fragment.payload.data == nullptr) ||
	    fragment.payload.size != header.payload_size ||
	    validate_fragment_layout(header) != ValidationError::None) {
		if (existing_index != NoEntry) {
			erase_entry(existing_index);
		}
		return ReassemblyResult::InvalidLayout;
	}

	const auto message_class = message_size_class(header.message_type);
	ReassemblyLimits limits;
	std::uint16_t canonical_fragment_count = 0;
	if (!limits_for(message_class, limits) || header.message_id == 0 ||
	    header.message_size > limits.max_message_size || header.fragment_count > limits.max_fragment_count ||
	    expected_fragment_count(header.message_type, header.message_size, canonical_fragment_count) !=
	        ValidationError::None ||
	    canonical_fragment_count != header.fragment_count) {
		if (existing_index != NoEntry) {
			erase_entry(existing_index);
		}
		return ReassemblyResult::InvalidLayout;
	}

	std::size_t entry_index = existing_index;
	if (entry_index != NoEntry) {
		auto& entry = m_entries[entry_index];
		if (entry.message_class != message_class || !common_fields_match(entry.header, header)) {
			erase_entry(entry_index);
			return ReassemblyResult::InconsistentFragment;
		}
	} else {
		const auto current_count = active_reassemblies(message_class);
		const auto current_bytes = reserved_bytes(message_class);
		if (current_count >= limits.max_reassemblies || header.message_size > limits.max_reserved_bytes - current_bytes) {
			return ReassemblyResult::QuotaExceeded;
		}

		auto& reassembly_count =
		    message_class == MessageSizeClass::Video ? m_video_reassemblies : m_state_reassemblies;
		auto& reserved_byte_count =
		    message_class == MessageSizeClass::Video ? m_video_reserved_bytes : m_state_reserved_bytes;
		if (m_global_budget != nullptr && !m_global_client_admitted) {
			return ReassemblyResult::QuotaExceeded;
		}
		if (m_global_budget != nullptr &&
			m_global_budget->try_reserve(message_class, header.message_size) != GlobalBudgetResult::Reserved) {
			return ReassemblyResult::QuotaExceeded;
		}
		GlobalQuotaReservation global_reservation(m_global_budget, message_class, header.message_size);
		QuotaReservation reservation(reassembly_count, reserved_byte_count, header.message_size);

		try {
			Entry candidate;
			candidate.header = header;
			candidate.message_class = message_class;
			candidate.global_budget_reserved = m_global_budget != nullptr;
			candidate.payload.resize(header.message_size);
			candidate.received.resize(header.fragment_count, 0);
			m_entries.emplace_back(std::move(candidate));
		} catch (const std::bad_alloc&) {
			return ReassemblyResult::AllocationFailed;
		}
		reservation.commit();
		global_reservation.commit();

		entry_index = m_entries.size() - 1;
	}

	auto& entry = m_entries[entry_index];
	const auto fragment_index = static_cast<std::size_t>(header.fragment_index);
	const auto fragment_offset = static_cast<std::size_t>(header.fragment_offset);
	if (entry.received[fragment_index] != 0) {
		if (same_bytes(fragment.payload, entry.payload, fragment_offset)) {
			return ReassemblyResult::Duplicate;
		}
		erase_entry(entry_index);
		return ReassemblyResult::InconsistentFragment;
	}

	if (!fragment.payload.empty()) {
		std::copy(fragment.payload.begin(), fragment.payload.end(),
		          entry.payload.begin() + static_cast<std::ptrdiff_t>(fragment_offset));
	}
	entry.received[fragment_index] = 1;
	++entry.received_count;

	if (entry.received_count != entry.received.size()) {
		return ReassemblyResult::Accepted;
	}

	const auto logical_payload = ByteView{entry.payload.empty() ? nullptr : entry.payload.data(), entry.payload.size()};
	if (validate_message_crc(entry.header, logical_payload) != ValidationError::None) {
		erase_entry(entry_index);
		return ReassemblyResult::MessageCrcMismatch;
	}

	ReassembledMessage published;
	published.header = entry.header;
	published.message_class = entry.message_class;
	published.payload = std::move(entry.payload);
	erase_entry(entry_index);
	completed = std::move(published);
	return ReassemblyResult::Completed;
}

bool TelemetryReassembler::discard(std::uint64_t session_id, std::uint32_t message_id) noexcept {
	const auto index = find_entry(session_id, message_id);
	if (index == NoEntry) {
		return false;
	}
	erase_entry(index);
	return true;
}

void TelemetryReassembler::clear() noexcept {
	if (m_global_budget != nullptr) {
		for (const auto& entry : m_entries) {
			if (entry.global_budget_reserved) {
				m_global_budget->release(entry.message_class, entry.header.message_size);
			}
		}
	}
	m_entries.clear();
	m_state_reassemblies = 0;
	m_video_reassemblies = 0;
	m_state_reserved_bytes = 0;
	m_video_reserved_bytes = 0;
}

std::size_t TelemetryReassembler::active_reassemblies(MessageSizeClass message_class) const noexcept {
	switch (message_class) {
	case MessageSizeClass::State:
		return m_state_reassemblies;
	case MessageSizeClass::Video:
		return m_video_reassemblies;
	default:
		return 0;
	}
}

std::size_t TelemetryReassembler::reserved_bytes(MessageSizeClass message_class) const noexcept {
	switch (message_class) {
	case MessageSizeClass::State:
		return m_state_reserved_bytes;
	case MessageSizeClass::Video:
		return m_video_reserved_bytes;
	default:
		return 0;
	}
}

std::size_t TelemetryReassembler::find_entry(std::uint64_t session_id, std::uint32_t message_id) const noexcept {
	for (std::size_t i = 0; i < m_entries.size(); ++i) {
		if (m_entries[i].header.session_id == session_id && m_entries[i].header.message_id == message_id) {
			return i;
		}
	}
	return NoEntry;
}

void TelemetryReassembler::erase_entry(std::size_t index) noexcept {
	const auto& entry = m_entries[index];
	if (entry.global_budget_reserved && m_global_budget != nullptr) {
		m_global_budget->release(entry.message_class, entry.header.message_size);
	}
	if (entry.message_class == MessageSizeClass::Video) {
		--m_video_reassemblies;
		m_video_reserved_bytes -= entry.header.message_size;
	} else {
		--m_state_reassemblies;
		m_state_reserved_bytes -= entry.header.message_size;
	}
	m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(index));
}

} // namespace telemetry::protocol
