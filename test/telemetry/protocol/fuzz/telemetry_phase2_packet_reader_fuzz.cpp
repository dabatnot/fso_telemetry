#include "fuzz_common.h"

#include "telemetry/protocol/packet_reader.h"
#include "telemetry/protocol/telemetry_control_messages.h"
#include "telemetry/protocol/telemetry_datagram.h"
#include "telemetry/protocol/telemetry_reassembler.h"
#include "telemetry/protocol/telemetry_reliability_messages.h"
#include "telemetry/protocol/telemetry_state_messages.h"
#include "telemetry/protocol/telemetry_transaction.h"

#include <algorithm>

using namespace telemetry::protocol;

namespace {

void ingest_transaction(const DatagramView& datagram,
	TelemetryTransactionAssembler& assembler)
{
	TransactionPart part;
	part.session_id = datagram.header.session_id;
	part.message_type = datagram.header.message_type;
	part.message_id = datagram.header.message_id;
	if (datagram.header.message_type == MessageType::Manifest) {
		ManifestPartPayload payload;
		if (decode_manifest_part_payload(datagram.payload, payload) !=
			ValidationError::None)
			return;
		part.transaction_id = payload.manifest_id;
		part.part_index = payload.part_index;
		part.part_count = payload.part_count;
		part.transaction_size = payload.transaction_size;
		part.transaction_sha256 = payload.transaction_sha256;
		part.producer_sample_time_us = payload.producer_sample_time_us;
		part.kind_or_flags =
			static_cast<std::uint16_t>(payload.manifest_kind);
		part.record_count = payload.record_count;
		part.records = payload.records;
	} else if (datagram.header.message_type ==
		MessageType::FullSnapshot) {
		FullSnapshotPartPayload payload;
		if (decode_full_snapshot_part_payload(datagram.payload, payload) !=
			ValidationError::None)
			return;
		part.transaction_id = payload.snapshot_id;
		part.part_index = payload.part_index;
		part.part_count = payload.part_count;
		part.transaction_size = payload.transaction_size;
		part.transaction_sha256 = payload.transaction_sha256;
		part.producer_sample_time_us = payload.producer_sample_time_us;
		part.frame_id = payload.snapshot_id;
		part.kind_or_flags = payload.snapshot_flags;
		part.required_manifest_id = payload.required_manifest_id;
		part.record_count = payload.record_count;
		part.records = payload.records;
	} else {
		return;
	}
	CompletedTransaction completed;
	static_cast<void>(assembler.ingest(part, 0U, completed));
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(
	const std::uint8_t* data, std::size_t size)
{
	const auto input = fuzz::bytes(data, size);
	PacketReader reader(input);
	const auto operations = std::min<std::size_t>(size, 256U);
	for (std::size_t operation = 0U;
		 operation < operations && reader.ok(); ++operation) {
		const auto requested = static_cast<std::size_t>(
			fuzz::read_u16(data, size, operation));
		switch (data[operation] % 6U) {
		case 0: {
			std::uint64_t value = 0U;
			static_cast<void>(reader.read_u64(value));
			break;
		}
		case 1: {
			float value = 0.0F;
			static_cast<void>(reader.read_f32(value));
			break;
		}
		case 2: {
			std::string_view value;
			static_cast<void>(reader.read_utf8(requested, value));
			break;
		}
		case 3: {
			ByteView value;
			static_cast<void>(reader.read_bytes(requested, value));
			break;
		}
		case 4: {
			PacketReader nested;
			static_cast<void>(reader.subreader(requested, nested));
			break;
		}
		default:
			static_cast<void>(reader.skip(requested));
			break;
		}
	}

	DatagramView datagram;
	if (decode_and_validate_datagram(input, datagram) !=
		ValidationError::None)
		return 0;

	TelemetryReassembler reassembler;
	ReassembledMessage completed;
	static_cast<void>(reassembler.ingest(datagram, completed));

	TelemetryTransactionAssembler transactions;
	ingest_transaction(datagram, transactions);
	if (datagram.header.message_type == MessageType::Ack) {
		AckPayload payload;
		static_cast<void>(decode_ack_payload(datagram.payload, payload));
	} else if (datagram.header.message_type == MessageType::Nack) {
		NackPayload payload;
		static_cast<void>(decode_nack_payload(datagram.payload, payload));
	} else if (datagram.header.message_type ==
		MessageType::ResyncRequest) {
		ResyncRequestPayload payload;
		static_cast<void>(
			decode_resync_request_payload(datagram.payload, payload));
	}
	return 0;
}
