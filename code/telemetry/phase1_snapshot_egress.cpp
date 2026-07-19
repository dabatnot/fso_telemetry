#include "telemetry/phase1_snapshot_egress.h"

#include "telemetry/protocol/telemetry_datagram.h"
#include "telemetry/protocol/telemetry_fragmenter.h"
#include "telemetry/protocol/telemetry_records.h"
#include "telemetry/protocol/telemetry_sha256.h"
#include "telemetry/protocol/telemetry_state_messages.h"

#include <limits>
#include <new>
#include <utility>

namespace telemetry::detail {
namespace {

bool checked_add_size(std::size_t left, std::size_t right, std::size_t& result) noexcept
{
	if (right > std::numeric_limits<std::size_t>::max() - left) return false;
	result = left + right;
	return true;
}

bool checked_multiply_size(std::size_t left, std::size_t right, std::size_t& result) noexcept
{
	if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left) return false;
	result = left * right;
	return true;
}

bool append_snapshot_records(const protocol::StateImage& image,
	std::vector<std::uint8_t>& records,
	std::uint16_t& record_count) noexcept
{
	const auto& atoms = image.records();
	if (atoms.empty() || atoms.size() > std::numeric_limits<std::uint16_t>::max() ||
		image.encoded_snapshot_records_size() > Phase1SnapshotRecordScratchBytes) {
		return false;
	}
	try {
		records.reserve(image.encoded_snapshot_records_size());
		for (const auto& atom : atoms) {
			if (atom.value.size() > std::numeric_limits<std::uint16_t>::max()) {
				return false;
			}
			const auto type = atom.key.record_type;
			const auto size = static_cast<std::uint16_t>(atom.value.size());
			records.push_back(static_cast<std::uint8_t>(type & 0xffU));
			records.push_back(static_cast<std::uint8_t>(type >> 8U));
			records.push_back(atom.record_version);
			records.push_back(protocol::RecordFlagNone);
			records.push_back(static_cast<std::uint8_t>(size & 0xffU));
			records.push_back(static_cast<std::uint8_t>(size >> 8U));
			records.insert(records.end(), atom.value.begin(), atom.value.end());
		}
	} catch (const std::bad_alloc&) {
		return false;
	}
	record_count = static_cast<std::uint16_t>(atoms.size());
	return protocol::validate_record_region({records.data(), records.size()},
			 record_count,
			 protocol::RecordFlagPolicy::RequireNone) == protocol::ValidationError::None;
}

} // namespace

std::size_t Phase1SnapshotEgress::startup_heap_bytes(const Phase1SnapshotEgressLimits& limits) noexcept
{
	std::size_t reliable_bytes = 0U;
	std::size_t parts_bytes = 0U;
	std::size_t scratch_bytes = 0U;
	std::size_t total = 0U;
	if (limits.reliable_item_capacity != 0U) {
		reliable_bytes = protocol::ReliableSendWindow::preallocated_heap_bytes(
			limits.reliable_item_capacity, Phase1ReplicationScratchBytes);
		if (reliable_bytes == 0U) return 0U;
	}
	if (!checked_multiply_size(limits.output_datagram_capacity, sizeof(protocol::SnapshotCandidatePart), parts_bytes) ||
		!checked_add_size(Phase1SnapshotRecordScratchBytes, Phase1ReplicationScratchBytes, scratch_bytes) ||
		!checked_add_size(reliable_bytes, parts_bytes, total) ||
		!checked_add_size(total, scratch_bytes, total)) {
		return 0U;
	}
	return total;
}

std::size_t Phase1SnapshotEgress::owned_heap_bytes() const noexcept
{
	std::size_t total = 0U;
	if (!checked_multiply_size(m_parts.capacity(), sizeof(protocol::SnapshotCandidatePart), total) ||
		!checked_add_size(total, m_records.capacity(), total) ||
		!checked_add_size(total, m_payload.capacity(), total) ||
		!checked_add_size(total, m_window.owned_preallocated_heap_bytes(), total)) {
		return 0U;
	}
	return total;
}

bool Phase1SnapshotEgress::configure(const Phase1SnapshotEgressLimits& limits) noexcept
{
	// A zero reservation is valid configuration (and causes an explicit queue
	// rejection) so deployments can disable snapshot egress without allocating.
	protocol::ReliableSendWindow candidate;
	if (limits.reliable_item_capacity != 0U) {
		protocol::ReliableWindowLimits window_limits;
		window_limits.max_entries = limits.reliable_item_capacity;
		window_limits.preallocated_payload_bytes_per_entry = Phase1ReplicationScratchBytes;
		if (protocol::ReliableSendWindow::configure(window_limits, candidate) != protocol::ValidationError::None) {
			return false;
		}
	}
	m_window = std::move(candidate);
	try {
		m_parts.reserve(1U);
		m_records.reserve(Phase1SnapshotRecordScratchBytes);
		m_payload.reserve(Phase1ReplicationScratchBytes);
	} catch (const std::bad_alloc&) {
		return false;
	}
	m_limits = limits;
	m_parts.clear();
	m_records.clear();
	m_payload.clear();
	m_endpoint = {};
	m_session_id = 0U;
	m_sent_time_us = 0U;
	m_snapshot_id = 0U;
	m_message_id = 0U;
	m_next_message_id = 1U;
	m_next_packet_sequence = 1U;
	m_next_fragment_index = 0U;
	m_retransmission_fragments = {};
	m_next_retransmission_fragment_index = 0U;
	m_retransmission_mission_time_us = 0U;
	m_retransmission_sent_time_us = 0U;
	m_has_candidate = false;
	m_has_output = false;
	m_output_is_retransmission = false;
	m_retransmission_pending = false;
	m_output = {};
	m_configured = true;
	return true;
}

bool Phase1SnapshotEgress::set_next_message_id(std::uint32_t message_id) noexcept
{
	if (!m_configured || m_has_candidate || message_id == 0U) {
		return false;
	}
	m_next_message_id = message_id;
	return true;
}

Phase1SnapshotEgressResult Phase1SnapshotEgress::queue_initial_snapshot(std::uint64_t session_id,
	const protocol::EndpointKey& endpoint,
	std::uint32_t snapshot_id,
	std::uint64_t producer_sample_time_us,
	const protocol::StateImage& image,
	std::uint64_t now_us) noexcept
{
	return queue_snapshot(session_id, endpoint, snapshot_id, producer_sample_time_us, image,
		protocol::SnapshotFlagInitial, now_us);
}

Phase1SnapshotEgressResult Phase1SnapshotEgress::queue_snapshot(std::uint64_t session_id,
	const protocol::EndpointKey& endpoint,
	std::uint32_t snapshot_id,
	std::uint64_t producer_sample_time_us,
	const protocol::StateImage& image,
	std::uint16_t snapshot_flags,
	std::uint64_t now_us) noexcept
{
	if (!m_configured || session_id == 0U || snapshot_id == 0U || image.empty() || m_has_candidate ||
		m_next_message_id == 0U ||
		(snapshot_flags != protocol::SnapshotFlagInitial && snapshot_flags != protocol::SnapshotFlagPeriodicKeyframe &&
			snapshot_flags != protocol::SnapshotFlagResync)) {
		return Phase1SnapshotEgressResult::InvalidArgument;
	}
	if (m_limits.reliable_item_capacity == 0U) {
		return Phase1SnapshotEgressResult::ReliableCapacity;
	}
	if (m_limits.output_datagram_capacity == 0U) {
		return Phase1SnapshotEgressResult::OutputCapacity;
	}

	auto& records = m_records;
	records.clear();
	const auto records_capacity_before = records.capacity();
	std::uint16_t record_count = 0U;
	if (!append_snapshot_records(image, records, record_count)) {
		return Phase1SnapshotEgressResult::AllocationFailure;
	}
	protocol::Sha256Digest digest{};
	if (!protocol::sha256({records.data(), records.size()}, digest)) {
		return Phase1SnapshotEgressResult::InvalidArgument;
	}
	if (records.size() > std::numeric_limits<std::uint32_t>::max()) {
		return Phase1SnapshotEgressResult::InvalidArgument;
	}

	protocol::FullSnapshotPartPayload part_payload;
	part_payload.snapshot_id = snapshot_id;
	part_payload.part_index = 0U;
	part_payload.part_count = 1U;
	part_payload.transaction_size = static_cast<std::uint32_t>(records.size());
	part_payload.transaction_sha256 = digest;
	part_payload.producer_sample_time_us = producer_sample_time_us;
	part_payload.required_manifest_id = 0U;
	part_payload.snapshot_flags = snapshot_flags;
	part_payload.record_count = record_count;
	part_payload.records = {records.data(), records.size()};

	if (records.size() > Phase1SnapshotRecordScratchBytes) {
		return Phase1SnapshotEgressResult::ReliableCapacity;
	}
	auto& payload = m_payload;
	payload.clear();
	const auto payload_capacity_before = payload.capacity();
	const auto parts_capacity_before = m_parts.capacity();
	try {
		payload.resize(protocol::FullSnapshotPartPayloadPrefixSize + records.size());
	} catch (const std::bad_alloc&) {
		return Phase1SnapshotEgressResult::AllocationFailure;
	}
	std::size_t payload_size = 0U;
	if (protocol::encode_full_snapshot_part_payload(
			part_payload, {payload.data(), payload.size()}, payload_size) != protocol::ValidationError::None ||
		payload_size != payload.size()) {
		return Phase1SnapshotEgressResult::InvalidArgument;
	}

	protocol::TelemetryFragmenter fragmenter;
	if (!protocol::TelemetryFragmenter::create({payload.data(), payload.size()},
			protocol::MessageSizeClass::State,
			fragmenter)) {
		return Phase1SnapshotEgressResult::InvalidArgument;
	}
	protocol::SnapshotCandidatePart candidate_part;
	candidate_part.target.message_type = protocol::MessageType::FullSnapshot;
	candidate_part.target.message_id = m_next_message_id;
	candidate_part.target.fragment_count = fragmenter.fragment_count();
	candidate_part.target.message_crc32 = fragmenter.message_crc32();
	const auto base_flags = static_cast<std::uint8_t>(protocol::MessageFlagAckRequired | protocol::MessageFlagKeyframe |
		(fragmenter.fragment_count() > 1U ? protocol::MessageFlagFragmented : protocol::MessageFlagNone));

	protocol::ReliableMessageToRetain retained;
	retained.session_id = session_id;
	retained.endpoint = endpoint;
	retained.message_type = protocol::MessageType::FullSnapshot;
	retained.base_flags = base_flags;
	retained.frame_id = snapshot_id;
	retained.message_id = candidate_part.target.message_id;
	retained.fragment_count = candidate_part.target.fragment_count;
	retained.message_crc32 = candidate_part.target.message_crc32;
	retained.transaction_id = snapshot_id;
	retained.transaction_sha256 = digest;
	retained.logical_payload = {payload.data(), payload.size()};
	retained.required_ack = protocol::RequiredAckLevel::Applied;
	retained.message_class = protocol::ReliableMessageClass::Transaction;
	const auto window_allocations_before = m_window.allocation_events();
	const auto retained_result = m_window.retain(retained, now_us);
	if (m_allocation_observer != nullptr) {
		m_allocation_observer->note_growth(window_allocations_before, m_window.allocation_events());
	}
	if (retained_result != protocol::ReliableRetainResult::Retained) {
		if (retained_result == protocol::ReliableRetainResult::AllocationFailed) {
			return Phase1SnapshotEgressResult::AllocationFailure;
		}
		if (retained_result == protocol::ReliableRetainResult::InvalidMessage ||
			retained_result == protocol::ReliableRetainResult::ClockOverflow) {
			return Phase1SnapshotEgressResult::InvalidArgument;
		}
		return Phase1SnapshotEgressResult::ReliableCapacity;
	}

	try {
		m_parts.assign(1U, candidate_part);
	} catch (const std::bad_alloc&) {
		(void)m_window.discard(session_id, endpoint, candidate_part.target.message_id);
		return Phase1SnapshotEgressResult::AllocationFailure;
	}
	if (m_allocation_observer != nullptr) {
		m_allocation_observer->note_growth(records_capacity_before, records.capacity());
		m_allocation_observer->note_growth(payload_capacity_before, payload.capacity());
		m_allocation_observer->note_growth(parts_capacity_before, m_parts.capacity());
	}
	m_endpoint = endpoint;
	m_session_id = session_id;
	m_sent_time_us = now_us;
	m_snapshot_id = snapshot_id;
	m_message_id = candidate_part.target.message_id;
	++m_next_message_id;
	m_next_fragment_index = 0U;
	m_has_candidate = true;
	return Phase1SnapshotEgressResult::Queued;
}

std::size_t Phase1SnapshotEgress::service(std::size_t datagram_budget) noexcept
{
	return service_initial(datagram_budget, m_next_packet_sequence, m_sent_time_us);
}

std::size_t Phase1SnapshotEgress::service(std::size_t datagram_budget,
	std::uint32_t packet_sequence,
	std::uint64_t sent_time_us) noexcept
{
	return service_initial(datagram_budget, packet_sequence, sent_time_us);
}

std::size_t Phase1SnapshotEgress::service_initial(std::size_t datagram_budget,
	std::uint32_t packet_sequence,
	std::uint64_t sent_time_us) noexcept
{
	if (!m_has_candidate || m_has_output || m_retransmission_pending || datagram_budget == 0U || m_parts.empty()) {
		return 0U;
	}
	protocol::TelemetryFragmenter fragmenter;
	if (!protocol::TelemetryFragmenter::create({m_payload.data(), m_payload.size()},
			protocol::MessageSizeClass::State,
			fragmenter) ||
		m_next_fragment_index >= fragmenter.fragment_count()) {
		return 0U;
	}
	protocol::FragmentSlice slice;
	if (!fragmenter.fragment(m_next_fragment_index, slice)) {
		return 0U;
	}
	protocol::TelemetryDatagramHeader header;
	header.version_minor = protocol::VersionMinorV1_1;
	header.message_type = protocol::MessageType::FullSnapshot;
	header.flags = static_cast<std::uint8_t>(protocol::MessageFlagAckRequired | protocol::MessageFlagKeyframe |
		(fragmenter.fragment_count() > 1U ? protocol::MessageFlagFragmented : protocol::MessageFlagNone));
	header.session_id = m_session_id;
	header.packet_sequence = packet_sequence;
	header.frame_id = m_snapshot_id;
	header.sent_time_us = sent_time_us;
	header.message_id = m_message_id;
	header.fragment_index = slice.fragment_index;
	header.fragment_count = slice.fragment_count;
	header.message_size = slice.message_size;
	header.fragment_offset = slice.fragment_offset;
	header.message_crc32 = slice.message_crc32;
	Phase1SnapshotDatagram output;
	output.endpoint = m_endpoint;
	if (protocol::encode_datagram(header,
			protocol::ProtocolMinorRange{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1},
			slice.payload,
			{output.bytes.data(), output.bytes.size()},
			output.size) != protocol::ValidationError::None) {
		return 0U;
	}
	m_output = output;
	m_has_output = true;
	m_output_is_retransmission = false;
	return 1U;
}

bool Phase1SnapshotEgress::peek_output(Phase1SnapshotDatagram& output) const noexcept
{
	if (!m_has_output) {
		return false;
	}
	output = m_output;
	return true;
}

void Phase1SnapshotEgress::complete_output() noexcept
{
	if (!m_has_output) {
		return;
	}
	if (!m_output_is_retransmission) {
		++m_next_packet_sequence;
		++m_next_fragment_index;
	} else {
		if (m_next_retransmission_fragment_index == std::numeric_limits<std::uint16_t>::max()) {
			m_retransmission_pending = false;
		} else {
			++m_next_retransmission_fragment_index;
		}
	}
	m_has_output = false;
	m_output = {};
	m_output_is_retransmission = false;
}

protocol::ReliableResponseResult Phase1SnapshotEgress::acknowledge_candidate_part(const protocol::AckPayload& ack,
	std::uint64_t now_us) noexcept
{
	if (!m_has_candidate) {
		return protocol::ReliableResponseResult::IgnoredUnknownOrLate;
	}
	const auto result = m_window.acknowledge(m_session_id, m_endpoint, ack, now_us);
	if (result == protocol::ReliableResponseResult::Released) {
		release_candidate_storage();
	}
	return result;
}

void Phase1SnapshotEgress::rollback_candidate() noexcept
{
	release_candidate_storage();
}

protocol::ReliableResponseResult Phase1SnapshotEgress::reject_candidate_part(const protocol::NackPayload& nack,
	std::uint64_t now_us,
	protocol::ReliableNackDecision& decision) noexcept
{
	if (!m_has_candidate) {
		return protocol::ReliableResponseResult::IgnoredUnknownOrLate;
	}
	return m_window.reject(m_session_id, m_endpoint, nack, now_us, decision);
}

protocol::ReliablePullResult Phase1SnapshotEgress::pull_reliability(std::uint64_t now_us,
	protocol::ReliableWindowAction& action) noexcept
{
	return m_has_candidate ? m_window.pull_next_action(now_us, action) : protocol::ReliablePullResult::None;
}

bool Phase1SnapshotEgress::queue_retransmission(const protocol::ReliableWindowAction& action,
	std::uint32_t packet_sequence,
	std::uint64_t sent_time_us) noexcept
{
	if (!m_has_candidate || m_has_output || m_retransmission_pending ||
		action.kind != protocol::ReliableWindowActionKind::Retransmit) {
		return false;
	}
	const auto& retransmission = action.retransmission;
	if (retransmission.key.session_id != m_session_id || retransmission.key.message_id != m_message_id ||
		retransmission.key.message_type != protocol::MessageType::FullSnapshot ||
		retransmission.key.fragment_count == 0U ||
		retransmission.key.fragment_count != retransmission.fragments.fragment_count ||
		retransmission.logical_payload.size != m_payload.size()) {
		return false;
	}
	m_retransmission_fragments = retransmission.fragments;
	m_next_retransmission_fragment_index = 0U;
	m_retransmission_mission_time_us = retransmission.mission_time_us;
	m_retransmission_sent_time_us = sent_time_us;
	m_retransmission_pending = true;
	return queue_retransmission_fragment(packet_sequence);
}

bool Phase1SnapshotEgress::queue_next_retransmission(std::uint32_t packet_sequence) noexcept
{
	if (!m_has_candidate || m_has_output || !m_retransmission_pending) {
		return false;
	}
	return queue_retransmission_fragment(packet_sequence);
}

bool Phase1SnapshotEgress::queue_retransmission_fragment(std::uint32_t packet_sequence) noexcept
{
	protocol::TelemetryFragmenter fragmenter;
	if (!protocol::TelemetryFragmenter::create({m_payload.data(), m_payload.size()},
			protocol::MessageSizeClass::State,
			fragmenter) || fragmenter.fragment_count() != m_retransmission_fragments.fragment_count) {
		return false;
	}
	std::uint16_t fragment_index = m_next_retransmission_fragment_index;
	bool selected = false;
	for (; fragment_index < fragmenter.fragment_count(); ++fragment_index) {
		if (m_retransmission_fragments.is_selected(fragment_index, selected) != protocol::ValidationError::None) {
			return false;
		}
		if (selected) {
			break;
		}
	}
	if (fragment_index == fragmenter.fragment_count()) {
		m_retransmission_pending = false;
		return false;
	}
	protocol::FragmentSlice slice;
	if (!fragmenter.fragment(fragment_index, slice)) {
		return false;
	}
	protocol::TelemetryDatagramHeader header;
	header.version_minor = protocol::VersionMinorV1_1;
	header.message_type = protocol::MessageType::FullSnapshot;
	header.flags = static_cast<std::uint8_t>(protocol::MessageFlagAckRequired | protocol::MessageFlagKeyframe |
		(protocol::MessageFlagRetransmission) |
		(fragmenter.fragment_count() > 1U ? protocol::MessageFlagFragmented : protocol::MessageFlagNone));
	header.session_id = m_session_id;
	header.packet_sequence = packet_sequence;
	header.frame_id = m_snapshot_id;
	header.mission_time_us = m_retransmission_mission_time_us;
	header.sent_time_us = m_retransmission_sent_time_us;
	header.message_id = m_message_id;
	header.fragment_index = slice.fragment_index;
	header.fragment_count = slice.fragment_count;
	header.message_size = slice.message_size;
	header.fragment_offset = slice.fragment_offset;
	header.message_crc32 = slice.message_crc32;
	Phase1SnapshotDatagram output;
	output.endpoint = m_endpoint;
	if (protocol::encode_datagram(header,
			protocol::ProtocolMinorRange{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1},
			slice.payload,
			{output.bytes.data(), output.bytes.size()},
			output.size) != protocol::ValidationError::None) {
		return false;
	}
	m_output = output;
	m_has_output = true;
	m_output_is_retransmission = true;
	m_next_retransmission_fragment_index = fragment_index;
	return true;
}

void Phase1SnapshotEgress::clear_candidate() noexcept
{
	m_window.clear();
	m_parts.clear();
	m_records.clear();
	m_payload.clear();
	m_endpoint = {};
	m_session_id = 0U;
	m_sent_time_us = 0U;
	m_snapshot_id = 0U;
	m_message_id = 0U;
	m_next_fragment_index = 0U;
	m_retransmission_fragments = {};
	m_next_retransmission_fragment_index = 0U;
	m_retransmission_mission_time_us = 0U;
	m_retransmission_sent_time_us = 0U;
	m_has_candidate = false;
	m_has_output = false;
	m_output_is_retransmission = false;
	m_retransmission_pending = false;
	m_output = {};
}

void Phase1SnapshotEgress::release_candidate_storage() noexcept
{
	if (m_has_candidate) {
		(void)m_window.discard(m_session_id, m_endpoint, m_message_id);
	}
	m_parts.clear();
	m_records.clear();
	m_payload.clear();
	m_endpoint = {};
	m_session_id = 0U;
	m_sent_time_us = 0U;
	m_snapshot_id = 0U;
	m_message_id = 0U;
	m_next_fragment_index = 0U;
	m_retransmission_fragments = {};
	m_next_retransmission_fragment_index = 0U;
	m_retransmission_mission_time_us = 0U;
	m_retransmission_sent_time_us = 0U;
	m_has_candidate = false;
	m_has_output = false;
	m_output_is_retransmission = false;
	m_retransmission_pending = false;
	m_output = {};
}

} // namespace telemetry::detail
