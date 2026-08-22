#pragma once

#include "telemetry/protocol/telemetry_business_records.h"
#include "telemetry/phase1_allocation_observer.h"
#include "telemetry/protocol/packet_writer.h"
#include "telemetry/protocol/telemetry_datagram.h"
#include "telemetry/protocol/telemetry_fragmenter.h"
#include "telemetry/protocol/telemetry_replication.h"
#include "telemetry/protocol/telemetry_state_messages.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace telemetry::detail {

constexpr std::size_t Phase1DeltaScratchBytes = 512U;
constexpr std::size_t Phase2CompleteShipDeltaBytes =
	protocol::MaxStateMessageSize;
// CockpitSensors keeps the FSTL one-MiB logical delta ceiling. A larger
// cumulative change is represented by a keyframe instead of a larger delta.
constexpr std::size_t Phase3CockpitSensorsDeltaBytes =
	protocol::MaxStateMessageSize;

enum class Phase1DeltaReplaceResult : std::uint8_t {
	Replaced = 0,
	InvalidState,
	InvalidDelta,
	CapacityExceeded,
	EncodingFailed,
	AllocationFailed,
	Count,
};

struct Phase1DeltaDatagram {
	protocol::EndpointKey endpoint;
	std::array<std::uint8_t, protocol::MaxDatagramSize> bytes{};
	std::size_t size = 0U;
};

// One replaceable, non-reliable Delta per client. It deliberately owns no
// ReliableSendWindow: a newer cumulative delta supersedes every unsent or
// partially sent fragment of the previous logical message.
class Phase1DeltaEgress final {
  public:
	static constexpr std::size_t StartupHeapBytes = Phase1DeltaScratchBytes * 2U;
	static std::size_t startup_heap_bytes(
		std::size_t payload_capacity) noexcept
	{
		return payload_capacity <=
			std::numeric_limits<std::size_t>::max() / 2U
			? payload_capacity * 2U
			: 0U;
	}
	void set_allocation_observer(Phase1AllocationObserver* observer) noexcept { m_allocation_observer = observer; }
	bool provisioned() const noexcept { return m_provisioned; }
	std::size_t owned_heap_bytes() const noexcept { return m_records.capacity() + m_payload.capacity(); }
	bool provision(
		std::size_t payload_capacity =
			Phase1DeltaScratchBytes) noexcept
	{
		if (payload_capacity == 0U ||
			payload_capacity > protocol::MaxStateMessageSize)
			return false;
		if (m_provisioned)
			return m_capacity == payload_capacity;
		try {
			m_records.reserve(payload_capacity);
			m_payload.reserve(payload_capacity);
			m_capacity = payload_capacity;
			m_provisioned = true;
			return true;
		} catch (const std::bad_alloc&) {
			return false;
		}
	}
	bool replace(std::uint64_t session_id,
		const protocol::EndpointKey& endpoint,
		std::uint32_t message_id,
		const protocol::CumulativeStateDelta& delta) noexcept
	{
		return replace_checked(
			session_id, endpoint, message_id, delta) ==
			Phase1DeltaReplaceResult::Replaced;
	}

	Phase1DeltaReplaceResult replace_checked(
		std::uint64_t session_id,
		const protocol::EndpointKey& endpoint,
		std::uint32_t message_id,
		const protocol::CumulativeStateDelta& delta) noexcept
	{
		return replace_impl(session_id, endpoint, message_id,
			delta, false);
	}

	Phase1DeltaReplaceResult replace_prevalidated_checked(
		std::uint64_t session_id,
		const protocol::EndpointKey& endpoint,
		std::uint32_t message_id,
		const protocol::CumulativeStateDelta& delta,
		const protocol::DeltaBuildChanges& changes) noexcept
	{
		return replace_impl(session_id, endpoint, message_id,
			delta, true, &changes);
	}

  private:
	Phase1DeltaReplaceResult replace_impl(
		std::uint64_t session_id,
		const protocol::EndpointKey& endpoint,
		std::uint32_t message_id,
		const protocol::CumulativeStateDelta& delta,
		bool delta_prevalidated,
		const protocol::DeltaBuildChanges* changes = nullptr) noexcept
	{
		if (m_has_output || m_has_started || session_id == 0U ||
			message_id == 0U || !m_provisioned)
			return Phase1DeltaReplaceResult::InvalidState;
		if (delta.baseline_snapshot_id == 0U ||
			(!delta_prevalidated &&
			 protocol::validate_cumulative_state_delta(delta) !=
				 protocol::StateDeltaValidationResult::Valid))
			return Phase1DeltaReplaceResult::InvalidDelta;
		const auto encoded_size = delta.encoded_size();
		if (encoded_size > m_capacity)
			return Phase1DeltaReplaceResult::CapacityExceeded;
		auto& records = m_records;
		auto& payload = m_payload;
		const auto records_capacity_before = records.capacity();
		const auto payload_capacity_before = payload.capacity();
		auto incremental_payload_valid =
			delta_prevalidated && changes != nullptr &&
			!changes->layout_changed &&
			m_cached_layout_valid &&
			m_cached_baseline_snapshot_id ==
				delta.baseline_snapshot_id &&
			m_cached_mutation_count == delta.mutation_count() &&
			records.size() ==
				encoded_size -
					protocol::DeltaPayloadPrefixSize &&
			payload.size() == encoded_size;
		if (incremental_payload_valid) {
			std::size_t next_change = 0U;
			std::size_t record_offset = 0U;
			for (std::size_t mutation_index = 0U;
				 mutation_index < delta.mutation_count();
				 ++mutation_index) {
				const auto& mutation =
					delta.mutations[mutation_index];
				const auto& source =
					mutation.kind ==
							protocol::StateMutationKind::Delete
						? mutation.atom.key.identity
						: mutation.atom.value;
				const auto record_size =
					protocol::RecordEnvelopeHeaderSize +
					source.size();
				if (record_offset >
						records.size() ||
					record_size >
						records.size() - record_offset) {
					incremental_payload_valid = false;
					break;
				}
				if (next_change < changes->count &&
					changes->mutation_indices[next_change] ==
						mutation_index) {
					const auto flags =
						mutation.kind ==
								protocol::StateMutationKind::
									Create
							? protocol::RecordFlagCreate
							: (mutation.kind ==
									  protocol::
										  StateMutationKind::
											  Delete
								   ? protocol::
										 RecordFlagDelete
								   : protocol::
										 RecordFlagNone);
					const protocol::RecordEnvelopeView
						record{
							mutation.atom.key.record_type,
							mutation.atom.record_version,
							flags,
							{source.data(), source.size()}};
					std::size_t written = 0U;
					if (protocol::encode_business_record(
							record,
							protocol::
								BusinessRecordContainer::
									Delta,
							protocol::VersionMinor,
							{records.data() +
								 record_offset,
							 record_size},
							written) !=
							protocol::ValidationError::None ||
						written != record_size) {
						incremental_payload_valid = false;
						break;
					}
					std::copy_n(
						records.data() + record_offset,
						record_size,
						payload.data() +
							protocol::
								DeltaPayloadPrefixSize +
							record_offset);
					++next_change;
				} else if (next_change <
						changes->count &&
					changes->mutation_indices[next_change] <
						mutation_index) {
					incremental_payload_valid = false;
					break;
				}
				record_offset += record_size;
			}
			if (record_offset != records.size() ||
				next_change != changes->count)
				incremental_payload_valid = false;
			if (incremental_payload_valid) {
				protocol::PacketWriter writer(
					{payload.data(),
					 protocol::DeltaPayloadPrefixSize});
				incremental_payload_valid =
					writer.write_u32(
						delta.baseline_snapshot_id) &&
					writer.write_u32(
						delta.delta_sequence) &&
					writer.write_u64(
						delta.producer_sample_time_us) &&
					writer.write_u16(static_cast<
						std::uint16_t>(
						delta.mutation_count())) &&
					writer.write_u16(0U) &&
					writer.remaining() == 0U;
			}
		}
		try {
			if (!incremental_payload_valid) {
				// Incremental patching may already have touched a subset of the
				// cached envelopes. Invalidate before rebuilding so an early
				// encoding failure can never make that partial cache reusable.
				m_cached_layout_valid = false;
				records.clear();
				payload.clear();
				records.reserve(
					encoded_size -
						protocol::DeltaPayloadPrefixSize);
				for (std::size_t mutation_index = 0U;
					 mutation_index < delta.mutation_count();
					 ++mutation_index) {
					const auto& mutation =
						delta.mutations[mutation_index];
					const auto flags =
						mutation.kind ==
								protocol::StateMutationKind::Create
							? protocol::RecordFlagCreate
							: (mutation.kind ==
									  protocol::StateMutationKind::Delete
								   ? protocol::RecordFlagDelete
								   : protocol::RecordFlagNone);
					const auto& source =
						mutation.kind ==
								protocol::StateMutationKind::Delete
							? mutation.atom.key.identity
							: mutation.atom.value;
					if (source.size() >
						static_cast<std::size_t>(
							std::numeric_limits<
								std::uint16_t>::max())) {
						return Phase1DeltaReplaceResult::
							EncodingFailed;
					}
					protocol::RecordEnvelopeView record;
					record.raw_record_type =
						mutation.atom.key.record_type;
					record.record_version =
						mutation.atom.record_version;
					record.record_flags = flags;
					record.payload =
						{source.data(), source.size()};
					const auto offset = records.size();
					records.resize(offset +
						protocol::RecordEnvelopeHeaderSize +
						source.size());
					std::size_t written = 0U;
					if (protocol::encode_business_record(
							record,
							protocol::
								BusinessRecordContainer::Delta,
							protocol::VersionMinor,
							{records.data() + offset,
							 records.size() - offset},
							written) !=
							protocol::ValidationError::None ||
						written != records.size() - offset) {
						return Phase1DeltaReplaceResult::
							EncodingFailed;
					}
				}
				protocol::DeltaPayload wire;
				wire.baseline_snapshot_id =
					delta.baseline_snapshot_id;
				wire.delta_sequence = delta.delta_sequence;
				wire.producer_sample_time_us =
					delta.producer_sample_time_us;
				wire.record_count = static_cast<std::uint16_t>(
					delta.mutation_count());
				wire.records = {records.data(), records.size()};
				payload.resize(protocol::DeltaPayloadPrefixSize +
					records.size());
				std::size_t written = 0U;
				if (protocol::encode_delta_payload(wire,
						{payload.data(), payload.size()},
						written) !=
						protocol::ValidationError::None ||
					written != payload.size()) {
					return Phase1DeltaReplaceResult::
						EncodingFailed;
				}
				m_cached_layout_valid = true;
				m_cached_baseline_snapshot_id =
					delta.baseline_snapshot_id;
				m_cached_mutation_count =
					delta.mutation_count();
			}
		} catch (const std::bad_alloc&) {
			return Phase1DeltaReplaceResult::AllocationFailed;
		}

		m_endpoint = endpoint;
		m_session_id = session_id;
		m_baseline_snapshot_id = delta.baseline_snapshot_id;
		m_message_id = message_id;
		if (m_allocation_observer != nullptr) {
			m_allocation_observer->note_growth(records_capacity_before,
				records.capacity(),
				Phase1AllocationGrowthSource::DeltaEgressScratch);
			m_allocation_observer->note_growth(payload_capacity_before,
				payload.capacity(),
				Phase1AllocationGrowthSource::DeltaEgressScratch);
		}
		m_next_fragment_index = 0U;
		m_has_output = false;
		m_has_started = false;
		m_output = {};
		m_has_delta = true;
		return Phase1DeltaReplaceResult::Replaced;
	}

  public:
	bool service(std::uint32_t packet_sequence, std::uint64_t sent_time_us) noexcept
	{
		if (!m_has_delta || m_has_output) {
			return false;
		}
		protocol::TelemetryFragmenter fragmenter;
		if (!protocol::TelemetryFragmenter::create({m_payload.data(), m_payload.size()},
				protocol::MessageSizeClass::State,
				fragmenter) || m_next_fragment_index >= fragmenter.fragment_count()) {
			return false;
		}
		protocol::FragmentSlice slice;
		if (!fragmenter.fragment(m_next_fragment_index, slice)) {
			return false;
		}
		protocol::TelemetryDatagramHeader header;
		header.version_minor = protocol::VersionMinor;
		header.message_type = protocol::MessageType::Delta;
		header.flags = fragmenter.fragment_count() > 1U ? protocol::MessageFlagFragmented : protocol::MessageFlagNone;
		header.session_id = m_session_id;
		header.packet_sequence = packet_sequence;
		header.frame_id = m_baseline_snapshot_id;
		header.sent_time_us = sent_time_us;
		header.message_id = m_message_id;
		header.fragment_index = slice.fragment_index;
		header.fragment_count = slice.fragment_count;
		header.message_size = slice.message_size;
		header.fragment_offset = slice.fragment_offset;
		header.message_crc32 = slice.message_crc32;
		Phase1DeltaDatagram output;
		output.endpoint = m_endpoint;
		if (protocol::encode_datagram(header,
				protocol::SupportedMinorRange,
				slice.payload,
				{output.bytes.data(), output.bytes.size()},
				output.size) != protocol::ValidationError::None) {
			return false;
		}
		m_output = output;
		m_has_output = true;
		return true;
	}

	bool peek_output(Phase1DeltaDatagram& output) const noexcept
	{
		if (!m_has_output) {
			return false;
		}
		output = m_output;
		return true;
	}

	void complete_output() noexcept
	{
		if (!m_has_output) {
			return;
		}
		m_has_output = false;
		m_output = {};
		m_has_started = true;
		if (m_next_fragment_index == std::numeric_limits<std::uint16_t>::max()) {
			discard();
			return;
		}
		++m_next_fragment_index;
		protocol::TelemetryFragmenter fragmenter;
		if (!protocol::TelemetryFragmenter::create({m_payload.data(), m_payload.size()},
				protocol::MessageSizeClass::State,
				fragmenter) || m_next_fragment_index >= fragmenter.fragment_count()) {
			discard();
		}
	}

	// The controller may preempt an unsent Delta with control or heartbeat
	// traffic. Keep the logical cumulative payload intact so it can be
	// serialized again later (or replaced by a newer capture).
	void release_output_for_preemption() noexcept
	{
		m_has_output = false;
		m_output = {};
	}

	void discard() noexcept
	{
		m_endpoint = {};
		m_session_id = 0U;
		m_baseline_snapshot_id = 0U;
		m_message_id = 0U;
		m_next_fragment_index = 0U;
		m_has_output = false;
		m_has_started = false;
		m_has_delta = false;
		m_output = {};
	}

	bool has_delta() const noexcept { return m_has_delta; }
	bool has_output() const noexcept { return m_has_output; }
	bool can_replace() const noexcept { return !m_has_output && !m_has_started; }
	std::size_t capacity_bytes() const noexcept { return m_capacity; }

  private:
	std::vector<std::uint8_t> m_records;
	std::vector<std::uint8_t> m_payload;
	protocol::EndpointKey m_endpoint;
	std::uint64_t m_session_id = 0U;
	std::uint32_t m_baseline_snapshot_id = 0U;
	std::uint32_t m_message_id = 0U;
	std::uint16_t m_next_fragment_index = 0U;
	bool m_has_delta = false;
	bool m_has_output = false;
	bool m_has_started = false;
	Phase1DeltaDatagram m_output{};
	Phase1AllocationObserver* m_allocation_observer = nullptr;
	std::size_t m_capacity = 0U;
	bool m_provisioned = false;
	bool m_cached_layout_valid = false;
	std::uint32_t m_cached_baseline_snapshot_id = 0U;
	std::size_t m_cached_mutation_count = 0U;
};

} // namespace telemetry::detail
