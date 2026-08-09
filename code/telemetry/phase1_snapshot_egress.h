#pragma once

#include "telemetry/protocol/telemetry_protocol_types.h"
#include "telemetry/protocol/telemetry_replication.h"
#include "telemetry/protocol/telemetry_reliable_window.h"
#include "telemetry/protocol/telemetry_state_messages.h"
#include "telemetry/phase1_allocation_observer.h"
#include "telemetry/phase2_manifest_builder.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace telemetry::detail {

enum class Phase1SnapshotEgressResult : std::uint8_t {
	Queued = 0,
	InvalidArgument,
	ReliableCapacity,
	OutputCapacity,
	AllocationFailure,
};

// A retained FULL_SNAPSHOT is a State-class logical message.  The Phase 0/1
// wire contract allows one part of exactly MaxStatePartSize bytes (including
// the FullSnapshotPart payload prefix), and the fragmenter turns that logical
// message into at most MaxStateFragments datagrams.  Reserve the complete
// logical message before Ready: reserving only one datagram/small fixture
// would make valid fragmented snapshots fail after the runtime is live.
constexpr std::size_t Phase1ReplicationScratchBytes = protocol::MaxStatePartSize;
constexpr std::size_t Phase1SnapshotRecordScratchBytes =
	Phase1ReplicationScratchBytes - protocol::FullSnapshotPartPayloadPrefixSize;
// With 1 MiB parts, an adversarial sequence of maximum-sized whole records
// can waste less than one 65,541-byte envelope per boundary. Eighteen parts
// therefore cover every valid <=16 MiB image while remaining below the
// protocol ceiling of 64 parts.
constexpr std::size_t Phase2SnapshotMaximumParts = 18U;
constexpr std::size_t Phase2ReplicationPartBytes =
	Phase1ReplicationScratchBytes;

struct Phase1SnapshotEgressLimits {
	std::size_t reliable_item_capacity = 8U;
	std::size_t output_datagram_capacity = 8U;
	std::size_t transaction_record_capacity =
		Phase1SnapshotRecordScratchBytes;
	std::size_t part_payload_capacity =
		Phase1ReplicationScratchBytes;
};

struct Phase1SnapshotDatagram {
	protocol::EndpointKey endpoint;
	std::array<std::uint8_t, protocol::MaxDatagramSize> bytes{};
	std::size_t size = 0U;
};

// The initial snapshot's egress half.  One ReliableSendWindow is the sole
// reliable owner; local payload metadata merely supplies the canonical first
// transmission fragments and is never an independent retry queue.
class Phase1SnapshotEgress final {
  public:
	static std::size_t startup_heap_bytes(const Phase1SnapshotEgressLimits& limits) noexcept;
	bool configure(const Phase1SnapshotEgressLimits& limits) noexcept;
	void set_allocation_observer(Phase1AllocationObserver* observer) noexcept { m_allocation_observer = observer; }
	bool set_next_message_id(std::uint32_t message_id) noexcept;
	Phase1SnapshotEgressResult queue_initial_snapshot(std::uint64_t session_id,
		const protocol::EndpointKey& endpoint,
		std::uint32_t snapshot_id,
		std::uint64_t producer_sample_time_us,
		const protocol::StateImage& image,
		std::uint64_t now_us,
		std::uint32_t required_manifest_id = 0U) noexcept;
	Phase1SnapshotEgressResult queue_snapshot(std::uint64_t session_id,
		const protocol::EndpointKey& endpoint,
		std::uint32_t snapshot_id,
		std::uint64_t producer_sample_time_us,
		const protocol::StateImage& image,
		std::uint16_t snapshot_flags,
		std::uint64_t now_us,
		std::uint32_t required_manifest_id = 0U) noexcept;
	Phase1SnapshotEgressResult queue_manifest(
		std::uint64_t session_id,
		const protocol::EndpointKey& endpoint,
		const Phase2ManifestCandidate& manifest,
		std::uint64_t now_us) noexcept;

	// Produces at most one datagram while an output is outstanding.  The caller
	// must complete it before a following fragment can be exposed.
	std::size_t service(std::size_t datagram_budget) noexcept;
	std::size_t service(std::size_t datagram_budget,
		std::uint32_t packet_sequence,
		std::uint64_t sent_time_us) noexcept;
	bool peek_output(Phase1SnapshotDatagram& output) const noexcept;
	void complete_output() noexcept;
	protocol::ReliableResponseResult acknowledge_candidate_part(const protocol::AckPayload& ack,
		std::uint64_t now_us) noexcept;
	protocol::ReliableResponseResult reject_candidate_part(const protocol::NackPayload& nack,
		std::uint64_t now_us,
		protocol::ReliableNackDecision& decision) noexcept;
	protocol::ReliablePullResult pull_reliability(std::uint64_t now_us,
		protocol::ReliableWindowAction& action) noexcept;
	bool queue_retransmission(const protocol::ReliableWindowAction& action,
		std::uint32_t packet_sequence,
		std::uint64_t sent_time_us) noexcept;
	// Continues the fragment selection already consumed from the sole reliable
	// window. Each successful call exposes exactly one following fragment.
	bool queue_next_retransmission(std::uint32_t packet_sequence) noexcept;
	void rollback_candidate() noexcept;

	bool has_candidate() const noexcept { return m_has_candidate; }
	protocol::MessageType candidate_message_type() const noexcept {
		return m_candidate_message_type;
	}
	bool configured() const noexcept { return m_configured; }
	bool has_retransmission_pending() const noexcept { return m_retransmission_pending; }
	std::size_t retained_item_count() const noexcept { return m_window.entry_count(); }
	std::size_t queued_datagram_count() const noexcept { return m_has_output ? 1U : 0U; }
	std::size_t owned_heap_bytes() const noexcept;
	std::uint32_t next_message_id() const noexcept {
		return m_next_message_id;
	}
	const std::vector<protocol::SnapshotCandidatePart>& candidate_parts() const noexcept { return m_parts; }

  private:
	void clear_candidate() noexcept;
	void release_candidate_storage() noexcept;
	std::size_t service_initial(std::size_t datagram_budget,
		std::uint32_t packet_sequence,
		std::uint64_t sent_time_us) noexcept;
	bool queue_retransmission_fragment(std::uint32_t packet_sequence) noexcept;
	bool load_part_payload(std::size_t part_index) noexcept;
	struct PartDescriptor {
		std::size_t records_offset = 0U;
		std::size_t records_size = 0U;
		std::uint16_t record_count = 0U;
		std::uint16_t fragment_count = 0U;
		std::uint32_t message_id = 0U;
		std::uint32_t message_crc32 = 0U;
	};

	Phase1SnapshotEgressLimits m_limits{};
	protocol::ReliableSendWindow m_window;
	std::vector<protocol::SnapshotCandidatePart> m_parts;
	std::vector<std::uint8_t> m_records;
	std::vector<std::uint8_t> m_payload;
	std::array<PartDescriptor, protocol::MaxTransactionParts>
		m_part_descriptors{};
	protocol::EndpointKey m_endpoint;
	std::uint64_t m_session_id = 0U;
	std::uint64_t m_sent_time_us = 0U;
	std::uint32_t m_snapshot_id = 0U;
	std::uint32_t m_message_id = 0U;
	std::uint32_t m_next_message_id = 1U;
	std::uint32_t m_next_packet_sequence = 1U;
	std::uint16_t m_next_fragment_index = 0U;
	std::uint16_t m_next_part_index = 0U;
	std::uint16_t m_retransmission_part_index = 0U;
	std::uint16_t m_snapshot_flags = protocol::SnapshotFlagNone;
	std::uint32_t m_required_manifest_id = 0U;
	std::uint64_t m_producer_sample_time_us = 0U;
	protocol::Sha256Digest m_transaction_digest{};
	protocol::MessageType m_candidate_message_type =
		protocol::MessageType::FullSnapshot;
	protocol::ReliableFragmentSelection m_retransmission_fragments;
	std::uint16_t m_next_retransmission_fragment_index = 0U;
	std::uint64_t m_retransmission_mission_time_us = 0U;
	std::uint64_t m_retransmission_sent_time_us = 0U;
	bool m_configured = false;
	bool m_has_candidate = false;
	bool m_has_output = false;
	bool m_output_is_retransmission = false;
	bool m_retransmission_pending = false;
	Phase1SnapshotDatagram m_output{};
	Phase1AllocationObserver* m_allocation_observer = nullptr;
};

} // namespace telemetry::detail
