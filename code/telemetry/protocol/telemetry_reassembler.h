#pragma once

#include "telemetry/protocol/telemetry_datagram.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace telemetry::protocol {

class GlobalReassemblyBudget;

enum class ReassemblyResult : std::uint8_t {
	Accepted,
	Duplicate,
	Completed,
	InvalidLayout,
	QuotaExceeded,
	InconsistentFragment,
	MessageCrcMismatch,
	AllocationFailed,
};

struct ReassembledMessage {
	// The common logical-message fields are authoritative. Datagram-local
	// fields (sequence, send time, fragment index/offset and datagram CRC) are
	// retained from the first accepted fragment only.
	TelemetryDatagramHeader header;
	MessageSizeClass message_class = MessageSizeClass::Invalid;
	std::vector<std::uint8_t> payload;

	ByteView payload_view() const noexcept {
		return ByteView{payload.empty() ? nullptr : payload.data(), payload.size()};
	}
};

// One instance owns the bounded reassembly state of one authenticated client
// and one receive direction. Session, endpoint, direction and contextual
// header validation happen before this layer; DatagramView must therefore
// originate from the validated receive pipeline.
class TelemetryReassembler {
  public:
	TelemetryReassembler() noexcept = default;
	explicit TelemetryReassembler(GlobalReassemblyBudget& global_budget) noexcept;
	~TelemetryReassembler();

	TelemetryReassembler(const TelemetryReassembler&) = delete;
	TelemetryReassembler& operator=(const TelemetryReassembler&) = delete;
	TelemetryReassembler(TelemetryReassembler&&) = delete;
	TelemetryReassembler& operator=(TelemetryReassembler&&) = delete;

	// completed is left unchanged unless Completed is returned.
	ReassemblyResult ingest(const DatagramView& fragment, ReassembledMessage& completed);

	bool discard(std::uint64_t session_id, std::uint32_t message_id) noexcept;
	void clear() noexcept;

	std::size_t active_reassemblies(MessageSizeClass message_class) const noexcept;
	std::size_t reserved_bytes(MessageSizeClass message_class) const noexcept;
	bool global_client_admitted() const noexcept
	{
		return m_global_budget == nullptr || m_global_client_admitted;
	}

  private:
	struct Entry {
		TelemetryDatagramHeader header;
		MessageSizeClass message_class = MessageSizeClass::Invalid;
		std::vector<std::uint8_t> payload;
		std::vector<std::uint8_t> received;
		std::size_t received_count = 0;
		bool global_budget_reserved = false;
	};

	std::size_t find_entry(std::uint64_t session_id, std::uint32_t message_id) const noexcept;
	void erase_entry(std::size_t index) noexcept;

	std::vector<Entry> m_entries;
	std::size_t m_state_reassemblies = 0;
	std::size_t m_video_reassemblies = 0;
	std::size_t m_state_reserved_bytes = 0;
	std::size_t m_video_reserved_bytes = 0;
	GlobalReassemblyBudget* m_global_budget = nullptr;
	bool m_global_client_admitted = false;
};

} // namespace telemetry::protocol
