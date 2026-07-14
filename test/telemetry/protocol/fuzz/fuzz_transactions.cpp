#include "fuzz_common.h"

#include "telemetry/protocol/telemetry_business_records.h"
#include "telemetry/protocol/telemetry_event_messages.h"
#include "telemetry/protocol/telemetry_rate_limiter.h"
#include "telemetry/protocol/telemetry_replication.h"
#include "telemetry/protocol/telemetry_session_context.h"
#include "telemetry/protocol/telemetry_state_messages.h"
#include "telemetry/protocol/telemetry_transaction.h"

#include <array>

using namespace telemetry::protocol;

namespace {

struct StatefulHarness {
	TelemetryTransactionAssembler transactions;
	ClientReplicationModel replication;
	ProtocolRateLimiter rate_limiter;
	EndpointKey endpoint = EndpointKey::from_ipv4(std::array<std::uint8_t, 4>{{127U, 0U, 0U, 1U}}, 7808U);
	ResyncRequestPayload resync;
	std::uint64_t now_us = 1U;

	StatefulHarness()
	{
		ProtocolRateLimiterConfig config;
		static_cast<void>(ProtocolRateLimiter::configure(config, now_us, rate_limiter));
	}
};

std::uint32_t stable_part_message_id(std::uint32_t transaction_id, std::uint16_t part_index) noexcept
{
	const auto mixed = transaction_id ^ (0x9e3779b9U * (static_cast<std::uint32_t>(part_index) + 1U));
	return mixed == 0 ? 1U : mixed;
}

void publish_completed(const CompletedTransaction& completed, StatefulHarness& harness)
{
	if (completed.message_type == MessageType::Manifest) {
		static_cast<void>(harness.replication.install_manifest(completed.transaction_id));
		return;
	}
	if (completed.message_type != MessageType::FullSnapshot) {
		return;
	}

	StateImage snapshot;
	if (completed.parts.size() != 1U ||
		decode_business_snapshot_region(completed.parts[0].records_view(), completed.parts[0].record_count, snapshot) !=
			ValidationError::None) {
		return;
	}
	if (completed.required_manifest_id != 0) {
		static_cast<void>(harness.replication.install_manifest(completed.required_manifest_id));
	}
	static_cast<void>(harness.replication.note_snapshot_candidate(completed.transaction_id,
		completed.required_manifest_id,
		harness.now_us,
		harness.now_us + static_cast<std::uint64_t>(TransactionAssemblyTimeoutMs) * 1000U));
	ClientResyncChannel channel{1U, harness.endpoint, harness.rate_limiter, harness.resync};
	static_cast<void>(harness.replication.commit_snapshot(completed.transaction_id,
		completed.required_manifest_id,
		snapshot,
		harness.now_us,
		channel));
}

void ingest_transaction_part(const ManifestPartPayload& payload, StatefulHarness& harness)
{
	TransactionPart part;
	part.session_id = 1U;
	part.message_type = MessageType::Manifest;
	part.transaction_id = payload.manifest_id;
	part.message_id = stable_part_message_id(payload.manifest_id, payload.part_index);
	part.part_index = payload.part_index;
	part.part_count = payload.part_count;
	part.transaction_size = payload.transaction_size;
	part.transaction_sha256 = payload.transaction_sha256;
	part.producer_sample_time_us = payload.producer_sample_time_us;
	part.kind_or_flags = static_cast<std::uint16_t>(payload.manifest_kind);
	part.record_count = payload.record_count;
	part.records = payload.records;
	CompletedTransaction completed;
	const auto outcome = harness.transactions.ingest(part, harness.now_us / 1000U, completed);
	if (outcome.result == TransactionAssemblyResult::Completed) {
		publish_completed(completed, harness);
	}
}

void ingest_transaction_part(const FullSnapshotPartPayload& payload, StatefulHarness& harness)
{
	TransactionPart part;
	part.session_id = 1U;
	part.message_type = MessageType::FullSnapshot;
	part.transaction_id = payload.snapshot_id;
	part.message_id = stable_part_message_id(payload.snapshot_id, payload.part_index);
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
	CompletedTransaction completed;
	const auto outcome = harness.transactions.ingest(part, harness.now_us / 1000U, completed);
	if (outcome.result == TransactionAssemblyResult::Completed) {
		publish_completed(completed, harness);
	}
}

void process_message(ByteView input, StatefulHarness& harness)
{
	if (input.empty() || input.data == nullptr) {
		return;
	}
	const auto payload = input.subview(1U, input.size - 1U);
	switch (static_cast<MessageType>(input.data[0])) {
	case MessageType::Manifest: {
		ManifestPartPayload decoded;
		if (decode_manifest_part_payload(payload, decoded) == ValidationError::None) {
			ingest_transaction_part(decoded, harness);
		}
		break;
	}
	case MessageType::FullSnapshot: {
		FullSnapshotPartPayload decoded;
		if (decode_full_snapshot_part_payload(payload, decoded) == ValidationError::None) {
			ingest_transaction_part(decoded, harness);
		}
		break;
	}
	case MessageType::Delta: {
		DeltaPayload decoded;
		if (decode_delta_payload(payload, decoded) == ValidationError::None) {
			CumulativeStateDelta delta;
			if (decode_business_delta(decoded, delta) == ValidationError::None) {
				static_cast<void>(harness.replication.receive_delta(delta,
					harness.now_us,
					1U,
					harness.endpoint,
					harness.rate_limiter,
					harness.resync));
			}
		}
		break;
	}
	case MessageType::EventBatch: {
		EventBatchPayload decoded;
		static_cast<void>(decode_event_batch_payload(payload, false, decoded));
		static_cast<void>(decode_event_batch_payload(payload, true, decoded));
		break;
	}
	default:
		break;
	}
	harness.now_us += 1000U + static_cast<std::uint64_t>(input.data[0]);
	static_cast<void>(harness.transactions.expire(harness.now_us / 1000U));
	static_cast<void>(harness.replication.expire_pending_delta(harness.now_us));
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
	StatefulHarness harness;
	process_message(fuzz::bytes(data, size), harness);

	std::size_t offset = 0;
	std::size_t command_count = 0;
	while (offset <= size && size - offset >= 2U && command_count < MaxTransactionParts * 2U) {
		const auto length = static_cast<std::size_t>(fuzz::read_u16(data, size, offset));
		offset += 2U;
		if (length == 0 || length > size - offset) {
			break;
		}
		process_message(fuzz::bytes(data + offset, length), harness);
		offset += length;
		++command_count;
	}
	return 0;
}
