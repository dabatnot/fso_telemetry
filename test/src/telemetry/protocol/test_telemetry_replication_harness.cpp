#include "telemetry/protocol/telemetry_fragmenter.h"
#include "telemetry/protocol/telemetry_reliable_receive.h"
#include "telemetry/protocol/telemetry_reliable_window.h"
#include "telemetry/protocol/telemetry_replication.h"
#include "telemetry/protocol/telemetry_sha256.h"
#include "telemetry/protocol/telemetry_state_messages.h"
#include "telemetry/protocol/telemetry_transaction.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace {

using namespace telemetry::protocol;

template <typename Container>
ByteView byte_view(const Container& bytes)
{
	return ByteView{static_cast<const std::uint8_t*>(static_cast<const void*>(bytes.data())), bytes.size()};
}

StateAtom atom(std::uint16_t type,
	std::vector<std::uint8_t> key,
	std::vector<std::uint8_t> value,
	StateRecordLifecycle lifecycle = StateRecordLifecycle::UpsertOnly)
{
	StateAtom result;
	result.key.record_type = type;
	result.key.identity = std::move(key);
	result.lifecycle = lifecycle;
	result.value = std::move(value);
	return result;
}

StateImage image(std::vector<StateAtom> atoms)
{
	StateImage result;
	EXPECT_EQ(StateImageResult::Created, StateImage::create(std::move(atoms), result));
	return result;
}

StateImageResult state_image_from_transaction(const CompletedTransaction& transaction, StateImage& state)
{
	if (transaction.message_type != MessageType::FullSnapshot || transaction.parts.empty()) {
		return StateImageResult::InvalidRecord;
	}
	std::size_t total_records = 0;
	for (const auto& part : transaction.parts) {
		if (part.record_count > MaxReplicationStateAtomCount - total_records ||
			validate_record_region(part.records_view(), part.record_count, RecordFlagPolicy::RequireNone) !=
				ValidationError::None) {
			return StateImageResult::InvalidRecord;
		}
		total_records += part.record_count;
	}

	std::vector<StateAtom> records;
	try {
		records.reserve(total_records);
		for (const auto& part : transaction.parts) {
			RecordEnvelopeIterator iterator(part.records_view(), part.record_count, RecordFlagPolicy::RequireNone);
			for (;;) {
				RecordEnvelopeView envelope;
				bool has_value = false;
				if (iterator.next(envelope, has_value) != ValidationError::None) {
					return StateImageResult::InvalidRecord;
				}
				if (!has_value) {
					break;
				}
				StateAtom decoded;
				decoded.key.record_type = envelope.raw_record_type;
				decoded.record_version = envelope.record_version;
				decoded.value.assign(envelope.payload.begin(), envelope.payload.end());
				// This deliberately tiny harness schema exercises the generic P0.6
				// state boundary without pre-implementing the full P0.7 registry.
				if (envelope.raw_record_type == 10U) {
					if (decoded.value.empty()) {
						return StateImageResult::InvalidRecord;
					}
					decoded.key.identity.push_back(decoded.value.front());
					decoded.lifecycle = StateRecordLifecycle::ExplicitCreateDelete;
				}
				records.push_back(std::move(decoded));
			}
		}
	} catch (const std::bad_alloc&) {
		return StateImageResult::AllocationFailed;
	}
	return StateImage::create(std::move(records), state);
}

std::vector<std::uint8_t> record(std::uint16_t type, const std::vector<std::uint8_t>& payload)
{
	std::vector<std::uint8_t> result;
	result.reserve(RecordEnvelopeHeaderSize + payload.size());
	result.push_back(static_cast<std::uint8_t>(type & 0xffU));
	result.push_back(static_cast<std::uint8_t>(type >> 8U));
	result.push_back(1U);
	result.push_back(RecordFlagNone);
	result.push_back(static_cast<std::uint8_t>(payload.size() & 0xffU));
	result.push_back(static_cast<std::uint8_t>(payload.size() >> 8U));
	result.insert(result.end(), payload.begin(), payload.end());
	return result;
}

std::vector<std::uint8_t> mutation_record(const StateMutation& mutation)
{
	const auto& payload = mutation.kind == StateMutationKind::Delete ? mutation.atom.key.identity : mutation.atom.value;
	auto result = record(mutation.atom.key.record_type, payload);
	result[3] = mutation.kind == StateMutationKind::Create
					? RecordFlagCreate
					: (mutation.kind == StateMutationKind::Delete ? RecordFlagDelete : RecordFlagNone);
	return result;
}

bool delta_wire_round_trip(const CumulativeStateDelta& model, CumulativeStateDelta& decoded_model)
{
	std::vector<std::uint8_t> records;
	try {
		for (const auto& mutation : model.mutations) {
			const auto encoded_record = mutation_record(mutation);
			records.insert(records.end(), encoded_record.begin(), encoded_record.end());
		}
	} catch (const std::bad_alloc&) {
		return false;
	}

	DeltaPayload payload;
	payload.baseline_snapshot_id = model.baseline_snapshot_id;
	payload.delta_sequence = model.delta_sequence;
	payload.producer_sample_time_us = model.producer_sample_time_us;
	payload.record_count = static_cast<std::uint16_t>(model.mutations.size());
	payload.records = byte_view(records);
	std::vector<std::uint8_t> encoded(DeltaPayloadPrefixSize + records.size());
	std::size_t written = 0;
	if (encode_delta_payload(payload, MutableByteView{encoded.data(), encoded.size()}, written) !=
			ValidationError::None ||
		written != encoded.size()) {
		return false;
	}
	DeltaPayload decoded;
	if (decode_delta_payload(byte_view(encoded), decoded) != ValidationError::None) {
		return false;
	}

	CumulativeStateDelta candidate;
	candidate.baseline_snapshot_id = decoded.baseline_snapshot_id;
	candidate.delta_sequence = decoded.delta_sequence;
	candidate.producer_sample_time_us = decoded.producer_sample_time_us;
	try {
		candidate.mutations.reserve(decoded.record_count);
		RecordEnvelopeIterator iterator(decoded.records, decoded.record_count, RecordFlagPolicy::AllowV1Mutations);
		for (;;) {
			RecordEnvelopeView envelope;
			bool has_value = false;
			if (iterator.next(envelope, has_value) != ValidationError::None) {
				return false;
			}
			if (!has_value) {
				break;
			}
			StateMutation mutation;
			mutation.kind = (envelope.record_flags & RecordFlagCreate) != 0
								? StateMutationKind::Create
								: ((envelope.record_flags & RecordFlagDelete) != 0 ? StateMutationKind::Delete
																				   : StateMutationKind::Upsert);
			mutation.atom.key.record_type = envelope.raw_record_type;
			mutation.atom.record_version = envelope.record_version;
			if (mutation.kind == StateMutationKind::Delete) {
				mutation.atom.key.identity.assign(envelope.payload.begin(), envelope.payload.end());
				mutation.atom.lifecycle = StateRecordLifecycle::ExplicitCreateDelete;
			} else {
				mutation.atom.value.assign(envelope.payload.begin(), envelope.payload.end());
				if (envelope.raw_record_type == 10U) {
					if (mutation.atom.value.empty()) {
						return false;
					}
					mutation.atom.key.identity.push_back(mutation.atom.value.front());
					mutation.atom.lifecycle = StateRecordLifecycle::ExplicitCreateDelete;
				}
			}
			candidate.mutations.push_back(std::move(mutation));
		}
	} catch (const std::bad_alloc&) {
		return false;
	}
	if (validate_cumulative_state_delta(candidate) != StateDeltaValidationResult::Valid) {
		return false;
	}
	decoded_model = std::move(candidate);
	return true;
}

SnapshotCandidatePart retained_part(std::uint32_t message_id, ByteView logical_payload)
{
	TelemetryFragmenter fragmenter;
	EXPECT_TRUE(TelemetryFragmenter::create(logical_payload, MessageSizeClass::State, fragmenter));
	SnapshotCandidatePart result;
	result.target.message_id = message_id;
	result.target.message_type = MessageType::FullSnapshot;
	result.target.fragment_count = fragmenter.fragment_count();
	result.target.message_crc32 = fragmenter.message_crc32();
	return result;
}

AckPayload applied_ack(const SnapshotCandidatePart& part)
{
	AckPayload result;
	result.target_message_id = part.target.message_id;
	result.target_message_type = part.target.message_type;
	result.ack_flags = KnownAckFlags;
	result.target_fragment_count = part.target.fragment_count;
	result.target_message_crc32 = part.target.message_crc32;
	return result;
}

TransactionPart
transaction_part(const FullSnapshotPartPayload& payload, std::uint32_t message_id, std::uint64_t session_id = 42U)
{
	TransactionPart result;
	result.session_id = session_id;
	result.message_type = MessageType::FullSnapshot;
	result.transaction_id = payload.snapshot_id;
	result.message_id = message_id;
	result.part_index = payload.part_index;
	result.part_count = payload.part_count;
	result.transaction_size = payload.transaction_size;
	result.transaction_sha256 = payload.transaction_sha256;
	result.producer_sample_time_us = payload.producer_sample_time_us;
	result.frame_id = 77U;
	result.mission_time_us = 500'000;
	result.kind_or_flags = payload.snapshot_flags;
	result.required_manifest_id = payload.required_manifest_id;
	result.record_count = payload.record_count;
	result.records = payload.records;
	return result;
}

TransactionPart
transaction_part(const ManifestPartPayload& payload, std::uint32_t message_id, std::uint64_t session_id = 42U)
{
	TransactionPart result;
	result.session_id = session_id;
	result.message_type = MessageType::Manifest;
	result.transaction_id = payload.manifest_id;
	result.message_id = message_id;
	result.part_index = payload.part_index;
	result.part_count = payload.part_count;
	result.transaction_size = payload.transaction_size;
	result.transaction_sha256 = payload.transaction_sha256;
	result.producer_sample_time_us = payload.producer_sample_time_us;
	result.frame_id = 0U;
	result.mission_time_us = 0;
	result.kind_or_flags = static_cast<std::uint16_t>(payload.manifest_kind);
	result.record_count = payload.record_count;
	result.records = payload.records;
	return result;
}

EndpointKey endpoint()
{
	return EndpointKey::from_ipv4({127U, 0U, 0U, 42U}, 7808U);
}

SnapshotCommitResult commit_snapshot(ClientReplicationModel& client,
	std::uint32_t snapshot_id,
	const StateImage& snapshot,
	std::uint64_t now_us)
{
	ProtocolRateLimiter rate_limiter;
	EXPECT_EQ(ValidationError::None, ProtocolRateLimiter::configure({}, now_us, rate_limiter));
	ResyncRequestPayload request;
	ClientResyncChannel channel{42U, endpoint(), rate_limiter, request};
	return client.commit_snapshot(snapshot_id, 0U, snapshot, now_us, channel);
}

TEST(TelemetryProtocolReplicationHarness, ReorderedSnapshotLostAckAndLostDeltaStillConverge)
{
	constexpr std::uint64_t SessionId = 42U;
	const auto initial =
		image({atom(1U, {}, {0x10U}), atom(10U, {1U}, {1U, 0x20U}, StateRecordLifecycle::ExplicitCreateDelete)});
	const auto captured =
		image({atom(1U, {}, {0x11U}), atom(10U, {1U}, {1U, 0x20U}, StateRecordLifecycle::ExplicitCreateDelete)});
	const auto during_ack =
		image({atom(1U, {}, {0x12U}), atom(10U, {2U}, {2U, 0x30U}, StateRecordLifecycle::ExplicitCreateDelete)});
	const auto final_state =
		image({atom(1U, {}, {0x13U}), atom(10U, {2U}, {2U, 0x31U}, StateRecordLifecycle::ExplicitCreateDelete)});

	ProducerBaselineTracker producer;
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.initialize(initial));
	const auto initial_part_payload = record(1U, {0x10U});
	const auto initial_part = retained_part(100U, byte_view(initial_part_payload));
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.capture_snapshot(1U, 0U, initial, {initial_part}, 0U));
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.acknowledge_snapshot_part(applied_ack(initial_part), 1U));
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.replace_current(captured));

	ClientReplicationModel client;
	ASSERT_EQ(SnapshotCandidateResult::Known, client.note_snapshot_candidate(1U, 0U, 0U, 10'000'000U));
	ASSERT_EQ(SnapshotCommitResult::Committed, commit_snapshot(client, 1U, initial, 1U));
	auto rate_limiter = std::make_unique<ProtocolRateLimiter>();
	ASSERT_EQ(ValidationError::None, ProtocolRateLimiter::configure({}, 0U, *rate_limiter));
	ResyncRequestPayload resync;

	const std::vector<std::vector<std::uint8_t>> regions{record(1U, {0x11U}), record(10U, {1U, 0x20U})};
	Sha256 calculator;
	ASSERT_TRUE(calculator.update(byte_view(regions[0])));
	ASSERT_TRUE(calculator.update(byte_view(regions[1])));
	Sha256Digest digest{};
	ASSERT_TRUE(calculator.finalize(digest));
	const auto transaction_size = static_cast<std::uint32_t>(regions[0].size() + regions[1].size());

	std::vector<std::vector<std::uint8_t>> encoded_parts(2U);
	std::vector<SnapshotCandidatePart> retained_parts;
	for (std::uint16_t index = 0U; index < 2U; ++index) {
		FullSnapshotPartPayload payload;
		payload.snapshot_id = 2U;
		payload.part_index = index;
		payload.part_count = 2U;
		payload.transaction_size = transaction_size;
		payload.transaction_sha256 = digest;
		payload.producer_sample_time_us = 100U;
		payload.snapshot_flags = SnapshotFlagPeriodicKeyframe;
		payload.record_count = 1U;
		payload.records = byte_view(regions[index]);
		encoded_parts[index].resize(FullSnapshotPartPayloadPrefixSize + regions[index].size());
		std::size_t written = 0;
		ASSERT_EQ(ValidationError::None,
			encode_full_snapshot_part_payload(payload,
				MutableByteView{encoded_parts[index].data(), encoded_parts[index].size()},
				written));
		ASSERT_EQ(encoded_parts[index].size(), written);
		retained_parts.emplace_back(retained_part(200U + index, byte_view(encoded_parts[index])));
	}
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.capture_snapshot(2U, 0U, captured, retained_parts, 10U));

	ReliableWindowLimits snapshot_window_limits;
	snapshot_window_limits.jitter_seed = 0xabcdef1234567890ULL;
	ReliableSendWindow snapshot_window;
	ASSERT_EQ(ValidationError::None, ReliableSendWindow::configure(snapshot_window_limits, snapshot_window));
	std::array<ReliableReceiveKey, 2U> receive_keys{};
	for (std::size_t index = 0; index < encoded_parts.size(); ++index) {
		ReliableMessageToRetain retained;
		retained.session_id = SessionId;
		retained.endpoint = endpoint();
		retained.message_type = MessageType::FullSnapshot;
		retained.base_flags = static_cast<std::uint8_t>(MessageFlagAckRequired | MessageFlagKeyframe);
		retained.frame_id = 77U;
		retained.mission_time_us = 500'000;
		retained.message_id = static_cast<std::uint32_t>(200U + index);
		retained.fragment_count = retained_parts[index].target.fragment_count;
		retained.message_crc32 = retained_parts[index].target.message_crc32;
		retained.transaction_id = 2U;
		retained.transaction_sha256 = digest;
		retained.logical_payload = byte_view(encoded_parts[index]);
		retained.required_ack = RequiredAckLevel::Applied;
		retained.message_class = ReliableMessageClass::Transaction;
		ASSERT_EQ(ReliableRetainResult::Retained, snapshot_window.retain(retained, 10U));
		receive_keys[index] = ReliableReceiveKey{SessionId,
			endpoint(),
			MessageType::FullSnapshot,
			retained.message_id,
			retained.message_crc32,
			retained.fragment_count};
	}
	ReliableReceiveCache snapshot_receive_cache(8U);

	TelemetryTransactionAssembler assembler;
	CompletedTransaction completed;
	FullSnapshotPartPayload decoded_second;
	ASSERT_EQ(ValidationError::None, decode_full_snapshot_part_payload(byte_view(encoded_parts[1]), decoded_second));
	ReliableReceiveOutcome receive_outcome;
	ASSERT_EQ(ReliableReceiveReserveResult::Reserved,
		snapshot_receive_cache.reserve(receive_keys[1], ReliableMessageClass::Transaction, 0U, receive_outcome));
	const auto second_part_assembly =
		assembler.ingest(transaction_part(decoded_second, 201U, SessionId), 0U, completed);
	ASSERT_EQ(TransactionAssemblyResult::Accepted, second_part_assembly.result);
	EXPECT_EQ(0U, second_part_assembly.expiration.count);
	ASSERT_EQ(ReliableReceiveUpdateResult::Updated, snapshot_receive_cache.record_validated(receive_keys[1]));
	ASSERT_EQ(SnapshotCandidateResult::Known, client.note_snapshot_candidate(2U, 0U, 0U, 10'000'000U));

	// State changes while the candidate is reliable but not common. The
	// producer continues emitting a cumulative Delta for the old baseline.
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.replace_current(during_ack));
	CumulativeStateDelta old_baseline_delta;
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.emit_cumulative_delta(110U, old_baseline_delta));
	CumulativeStateDelta decoded_old_baseline_delta;
	ASSERT_TRUE(delta_wire_round_trip(old_baseline_delta, decoded_old_baseline_delta));
	EXPECT_EQ(old_baseline_delta, decoded_old_baseline_delta);
	ASSERT_EQ(ClientDeltaResult::Applied,
		client.receive_delta(decoded_old_baseline_delta, 100U, SessionId, endpoint(), *rate_limiter, resync));
	EXPECT_EQ(during_ack, client.published());

	FullSnapshotPartPayload decoded_first;
	ASSERT_EQ(ValidationError::None, decode_full_snapshot_part_payload(byte_view(encoded_parts[0]), decoded_first));
	ASSERT_EQ(ReliableReceiveReserveResult::Reserved,
		snapshot_receive_cache.reserve(receive_keys[0], ReliableMessageClass::Transaction, 1U, receive_outcome));
	const auto first_part_assembly = assembler.ingest(transaction_part(decoded_first, 200U, SessionId), 1U, completed);
	ASSERT_EQ(TransactionAssemblyResult::Completed, first_part_assembly.result);
	EXPECT_EQ(0U, first_part_assembly.expiration.count);
	ASSERT_EQ(ReliableReceiveUpdateResult::Updated, snapshot_receive_cache.record_validated(receive_keys[0]));
	ASSERT_EQ(2U, completed.parts.size());
	EXPECT_EQ(200U, completed.parts[0].message_id);
	EXPECT_EQ(201U, completed.parts[1].message_id);
	auto semantic_conflict = completed;
	semantic_conflict.parts[1].records[0] = 1U;
	semantic_conflict.parts[1].records[1] = 0U;
	StateImage unpublished = initial;
	EXPECT_EQ(StateImageResult::DuplicateKey, state_image_from_transaction(semantic_conflict, unpublished));
	EXPECT_EQ(initial, unpublished);

	StateImage assembled_snapshot;
	ASSERT_EQ(StateImageResult::Created, state_image_from_transaction(completed, assembled_snapshot));
	ASSERT_EQ(captured, assembled_snapshot);
	std::uint32_t snapshot_publish_count = 0U;
	ASSERT_EQ(SnapshotCommitResult::Committed, commit_snapshot(client, 2U, assembled_snapshot, 101U));
	++snapshot_publish_count;
	for (const auto& key : receive_keys) {
		ASSERT_EQ(ReliableReceiveUpdateResult::Updated, snapshot_receive_cache.record_applied(key));
	}
	EXPECT_EQ(captured, client.published());

	// One APPLIED ACK is lost. The reliable sender retransmits that exact part;
	// the committed receive cache re-ACKs it without parsing or publishing twice.
	AckPayload first_applied_ack;
	ASSERT_EQ(ValidationError::None, snapshot_receive_cache.cached_ack(receive_keys[0], 102U, first_applied_ack));
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.acknowledge_snapshot_part(first_applied_ack, 102U));
	ASSERT_EQ(ReliableResponseResult::Released,
		snapshot_window.acknowledge(SessionId, endpoint(), first_applied_ack, 102U));
	EXPECT_EQ(1U, producer.active_snapshot_id());
	const auto snapshot_retry_at =
		10U +
		reliable_retry_delay_us(snapshot_window.base_rto_us(), snapshot_window_limits.jitter_seed, SessionId, 201U, 0U);
	ReliableWindowAction snapshot_retry;
	ASSERT_EQ(ReliablePullResult::Action, snapshot_window.pull_next_action(snapshot_retry_at, snapshot_retry));
	ASSERT_EQ(ReliableWindowActionKind::Retransmit, snapshot_retry.kind);
	EXPECT_EQ(201U, snapshot_retry.target.message_id);
	ASSERT_EQ(encoded_parts[1].size(), snapshot_retry.retransmission.logical_payload.size);
	EXPECT_TRUE(std::equal(encoded_parts[1].begin(),
		encoded_parts[1].end(),
		snapshot_retry.retransmission.logical_payload.begin()));
	FullSnapshotPartPayload retransmitted_second;
	ASSERT_EQ(ValidationError::None,
		decode_full_snapshot_part_payload(snapshot_retry.retransmission.logical_payload, retransmitted_second));
	EXPECT_EQ(2U, retransmitted_second.snapshot_id);
	EXPECT_EQ(1U, retransmitted_second.part_index);
	EXPECT_EQ(2U, retransmitted_second.part_count);
	EXPECT_EQ(digest, retransmitted_second.transaction_sha256);
	ASSERT_EQ(ReliableReceiveReserveResult::Duplicate,
		snapshot_receive_cache.reserve(receive_keys[1],
			ReliableMessageClass::Transaction,
			snapshot_retry_at,
			receive_outcome));
	EXPECT_EQ(ReliableReceiveOutcomeKind::Applied, receive_outcome.kind);
	EXPECT_EQ(1U, snapshot_publish_count);
	EXPECT_EQ(0U, assembler.active_candidates());
	AckPayload replayed_applied_ack;
	ASSERT_EQ(ValidationError::None,
		snapshot_receive_cache.cached_ack(receive_keys[1], snapshot_retry_at, replayed_applied_ack));
	ASSERT_EQ(ProducerBaselineResult::Applied,
		producer.acknowledge_snapshot_part(replayed_applied_ack, snapshot_retry_at));
	ASSERT_EQ(ReliableResponseResult::Released,
		snapshot_window.acknowledge(SessionId, endpoint(), replayed_applied_ack, snapshot_retry_at));
	EXPECT_EQ(0U, snapshot_window.entry_count());
	EXPECT_EQ(2U, producer.active_snapshot_id());

	CumulativeStateDelta lost_new_baseline_delta;
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.emit_cumulative_delta(120U, lost_new_baseline_delta));
	EXPECT_EQ(1U, lost_new_baseline_delta.delta_sequence);
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.replace_current(final_state));
	CumulativeStateDelta latest;
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.emit_cumulative_delta(121U, latest));
	EXPECT_EQ(2U, latest.delta_sequence);
	CumulativeStateDelta decoded_latest;
	ASSERT_TRUE(delta_wire_round_trip(latest, decoded_latest));
	EXPECT_EQ(latest, decoded_latest);
	ASSERT_EQ(ClientDeltaResult::Applied,
		client.receive_delta(decoded_latest, 105U, SessionId, endpoint(), *rate_limiter, resync));
	EXPECT_EQ(final_state, client.published());
	EXPECT_EQ(producer.current(), client.published());

	EXPECT_EQ(ClientDeltaResult::IgnoredOldBaseline,
		client.receive_delta(decoded_old_baseline_delta, 106U, SessionId, endpoint(), *rate_limiter, resync));
	EXPECT_EQ(final_state, client.published());
}

TEST(TelemetryProtocolReplicationHarness, RejectedTransactionPartNeverProducesAValidatedReceiveOutcome)
{
	const ReliableReceiveKey key{42U, endpoint(), MessageType::Manifest, 900U, 0x12345678U, 1U};
	ReliableReceiveCache receive_cache(2U);
	ReliableReceiveOutcome reserve_outcome{ReliableReceiveOutcomeKind::Applied, ValidationError::None};
	ASSERT_EQ(ReliableReceiveReserveResult::Reserved,
		receive_cache.reserve(key, ReliableMessageClass::Transaction, 0U, reserve_outcome));

	TransactionPart invalid_part;
	invalid_part.session_id = key.session_id;
	invalid_part.message_type = MessageType::Manifest;
	invalid_part.transaction_id = 1U;
	invalid_part.message_id = key.message_id;
	invalid_part.part_count = 1U;
	invalid_part.transaction_size = 1U;
	invalid_part.record_count = 0U;
	TelemetryTransactionAssembler assembler;
	CompletedTransaction completed;
	const auto assembly = assembler.ingest(invalid_part, 0U, completed);
	ASSERT_EQ(TransactionAssemblyResult::InvalidPart, assembly.result);
	EXPECT_EQ(0U, assembly.expiration.count);
	ASSERT_EQ(ReliableReceiveUpdateResult::Updated,
		receive_cache.record_error(key, ValidationError::InvalidStateTransition));

	ReliableReceiveOutcome cached_outcome;
	ASSERT_EQ(ReliableReceiveLookupResult::Found, receive_cache.lookup(key, 1U, cached_outcome));
	EXPECT_EQ(ReliableReceiveOutcomeKind::Error, cached_outcome.kind);
	EXPECT_EQ(ValidationError::InvalidStateTransition, cached_outcome.error);
	ReliableReceiveOutcome duplicate_outcome;
	ASSERT_EQ(ReliableReceiveReserveResult::Duplicate,
		receive_cache.reserve(key, ReliableMessageClass::Transaction, 2U, duplicate_outcome));
	EXPECT_EQ(ReliableReceiveOutcomeKind::Error, duplicate_outcome.kind);
	EXPECT_EQ(ValidationError::InvalidStateTransition, duplicate_outcome.error);
	AckPayload ack;
	EXPECT_EQ(ValidationError::InvalidStateTransition, receive_cache.cached_ack(key, 2U, ack));
}

TEST(TelemetryProtocolReplicationHarness, ManifestAndSnapshotOneTwoAndSixtyFourPartMatrixCommitsAtomically)
{
	for (const std::uint16_t part_count : std::array<std::uint16_t, 3U>{1U, 2U, 64U}) {
		std::vector<std::vector<std::uint8_t>> regions;
		regions.reserve(part_count);
		Sha256 calculator;
		std::uint32_t transaction_size = 0U;
		for (std::uint16_t index = 0U; index < part_count; ++index) {
			regions.emplace_back(
				record(static_cast<std::uint16_t>(index + 1U), {static_cast<std::uint8_t>(index + 1U)}));
			ASSERT_TRUE(calculator.update(byte_view(regions.back())));
			transaction_size += static_cast<std::uint32_t>(regions.back().size());
		}
		Sha256Digest digest{};
		ASSERT_TRUE(calculator.finalize(digest));

		const auto manifest_id = static_cast<std::uint32_t>(100U + part_count);
		std::vector<std::vector<std::uint8_t>> encoded_manifest_parts(part_count);
		for (std::uint16_t index = 0U; index < part_count; ++index) {
			ManifestPartPayload payload;
			payload.manifest_id = manifest_id;
			payload.part_index = index;
			payload.part_count = part_count;
			payload.transaction_size = transaction_size;
			payload.transaction_sha256 = digest;
			payload.producer_sample_time_us = 10U;
			payload.manifest_kind = ManifestKind::FullRequired;
			payload.record_count = 1U;
			payload.records = byte_view(regions[index]);
			auto& encoded = encoded_manifest_parts[index];
			encoded.resize(ManifestPartPayloadPrefixSize + regions[index].size());
			std::size_t written = 0U;
			ASSERT_EQ(ValidationError::None,
				encode_manifest_part_payload(payload, MutableByteView{encoded.data(), encoded.size()}, written));
			ASSERT_EQ(encoded.size(), written);
		}

		TelemetryTransactionAssembler manifest_assembler;
		CompletedTransaction completed_manifest;
		ClientReplicationModel manifest_client;
		for (std::uint16_t reverse = part_count; reverse > 0U; --reverse) {
			const auto index = static_cast<std::uint16_t>(reverse - 1U);
			ManifestPartPayload decoded;
			ASSERT_EQ(ValidationError::None,
				decode_manifest_part_payload(byte_view(encoded_manifest_parts[index]), decoded));
			const auto result = manifest_assembler.ingest(transaction_part(decoded, 1000U + index),
				part_count - reverse,
				completed_manifest);
			EXPECT_EQ(index == 0U ? TransactionAssemblyResult::Completed : TransactionAssemblyResult::Accepted, result);
			if (index != 0U) {
				EXPECT_EQ(0U, manifest_client.installed_manifest_id());
			}
		}
		ASSERT_EQ(part_count, completed_manifest.parts.size());
		for (const auto& part : completed_manifest.parts) {
			ASSERT_EQ(ValidationError::None,
				validate_record_region(part.records_view(), part.record_count, RecordFlagPolicy::RequireNone));
		}
		ASSERT_EQ(ManifestInstallResult::Installed, manifest_client.install_manifest(manifest_id));
		EXPECT_EQ(manifest_id, manifest_client.installed_manifest_id());

		const auto snapshot_id = static_cast<std::uint32_t>(part_count);
		std::vector<std::vector<std::uint8_t>> encoded_snapshot_parts(part_count);
		for (std::uint16_t index = 0U; index < part_count; ++index) {
			FullSnapshotPartPayload payload;
			payload.snapshot_id = snapshot_id;
			payload.part_index = index;
			payload.part_count = part_count;
			payload.transaction_size = transaction_size;
			payload.transaction_sha256 = digest;
			payload.producer_sample_time_us = 20U;
			payload.snapshot_flags = SnapshotFlagInitial;
			payload.record_count = 1U;
			payload.records = byte_view(regions[index]);
			auto& encoded = encoded_snapshot_parts[index];
			encoded.resize(FullSnapshotPartPayloadPrefixSize + regions[index].size());
			std::size_t written = 0U;
			ASSERT_EQ(ValidationError::None,
				encode_full_snapshot_part_payload(payload, MutableByteView{encoded.data(), encoded.size()}, written));
			ASSERT_EQ(encoded.size(), written);
		}

		TelemetryTransactionAssembler snapshot_assembler;
		CompletedTransaction completed_snapshot;
		ClientReplicationModel snapshot_client;
		ASSERT_EQ(SnapshotCandidateResult::Known,
			snapshot_client.note_snapshot_candidate(snapshot_id, 0U, 0U, 10'000'000U));
		for (std::uint16_t reverse = part_count; reverse > 0U; --reverse) {
			const auto index = static_cast<std::uint16_t>(reverse - 1U);
			FullSnapshotPartPayload decoded;
			ASSERT_EQ(ValidationError::None,
				decode_full_snapshot_part_payload(byte_view(encoded_snapshot_parts[index]), decoded));
			const auto result = snapshot_assembler.ingest(transaction_part(decoded, 2000U + index),
				part_count - reverse,
				completed_snapshot);
			EXPECT_EQ(index == 0U ? TransactionAssemblyResult::Completed : TransactionAssemblyResult::Accepted, result);
			if (index != 0U) {
				EXPECT_TRUE(snapshot_client.published().empty());
			}
		}
		StateImage decoded_snapshot;
		ASSERT_EQ(StateImageResult::Created, state_image_from_transaction(completed_snapshot, decoded_snapshot));
		ASSERT_EQ(SnapshotCommitResult::Committed, commit_snapshot(snapshot_client, snapshot_id, decoded_snapshot, 1U));
		EXPECT_EQ(part_count, snapshot_client.published().records().size());
	}
}

TEST(TelemetryProtocolReplicationHarness, PartialManifestExpiryRequestsManifestAndSnapshotThenRenews)
{
	const std::vector<std::vector<std::uint8_t>> regions{record(1U, {0x10U}), record(2U, {0x20U})};
	Sha256 calculator;
	ASSERT_TRUE(calculator.update(byte_view(regions[0])));
	ASSERT_TRUE(calculator.update(byte_view(regions[1])));
	Sha256Digest digest{};
	ASSERT_TRUE(calculator.finalize(digest));

	ManifestPartPayload first;
	first.manifest_id = 7U;
	first.part_index = 0U;
	first.part_count = 2U;
	first.transaction_size = static_cast<std::uint32_t>(regions[0].size() + regions[1].size());
	first.transaction_sha256 = digest;
	first.producer_sample_time_us = 1U;
	first.manifest_kind = ManifestKind::FullRequired;
	first.record_count = 1U;
	first.records = byte_view(regions[0]);
	std::vector<std::uint8_t> encoded(ManifestPartPayloadPrefixSize + regions[0].size());
	std::size_t written = 0U;
	ASSERT_EQ(ValidationError::None,
		encode_manifest_part_payload(first, MutableByteView{encoded.data(), encoded.size()}, written));
	ManifestPartPayload decoded;
	ASSERT_EQ(ValidationError::None, decode_manifest_part_payload(byte_view(encoded), decoded));

	TelemetryTransactionAssembler assembler;
	CompletedTransaction completed;
	ASSERT_EQ(TransactionAssemblyResult::Accepted, assembler.ingest(transaction_part(decoded, 700U), 0U, completed));
	const auto before_deadline = assembler.expire_with_details(TransactionAssemblyTimeoutMs - 1U);
	EXPECT_EQ(0U, before_deadline.count);
	const auto late_part =
		assembler.ingest(transaction_part(decoded, 700U), TransactionAssemblyTimeoutMs, completed);
	ASSERT_EQ(TransactionAssemblyResult::StaleTransaction, late_part.result);
	ASSERT_EQ(1U, late_part.expiration.count);
	EXPECT_TRUE(late_part.expiration.manifest_expired);
	EXPECT_FALSE(late_part.expiration.full_snapshot_expired);
	EXPECT_EQ(7U, late_part.expiration.manifest_id);
	EXPECT_EQ(0U, assembler.active_candidates());
	EXPECT_EQ(0U, assembler.reserved_bytes());

	ClientReplicationModel client;
	ASSERT_EQ(ManifestInstallResult::Installed, client.install_manifest(5U));
	ProtocolRateLimiter rate_limiter;
	ASSERT_EQ(ValidationError::None, ProtocolRateLimiter::configure({}, 0U, rate_limiter));
	ResyncRequestPayload request;
	ClientResyncChannel channel{42U, endpoint(), rate_limiter, request};
	ASSERT_EQ(ClientResyncResult::Requested,
		client.notify_manifest_transaction_expired(static_cast<std::uint64_t>(TransactionAssemblyTimeoutMs) * 1000ULL,
			channel));
	EXPECT_EQ(ResyncReason::ReassemblyTimeout, request.reason);
	EXPECT_EQ(KnownResyncRequestFlags, request.request_flags);

	ASSERT_EQ(ManifestInstallResult::Installed, client.install_manifest(7U));
	const auto renewed = image({atom(1U, {}, {0x30U})});
	const auto first_part_time_us = static_cast<std::uint64_t>(TransactionAssemblyTimeoutMs) * 1000ULL + 1U;
	ASSERT_EQ(SnapshotCandidateResult::Known,
		client.note_snapshot_candidate(1U, 7U, first_part_time_us, first_part_time_us + 10'000'000U));
	ASSERT_EQ(SnapshotCommitResult::Committed,
		client.commit_snapshot(1U, 7U, renewed, first_part_time_us + 1U, channel));
	EXPECT_EQ(ClientResyncResult::NotRequested, channel.disposition);
	EXPECT_EQ(renewed, client.published());
}

TEST(TelemetryProtocolReplicationHarness, LostResyncAckRetransmitsSameRequestAndNeverCreatesSecondCandidate)
{
	constexpr std::uint64_t SessionId = 42U;
	constexpr std::uint32_t MessageId = 500U;
	const auto peer = endpoint();
	ProtocolRateLimiter rate_limiter;
	ASSERT_EQ(ValidationError::None, ProtocolRateLimiter::configure({}, 0U, rate_limiter));
	ClientReplicationModel client;
	ResyncRequestPayload request;
	ClientResyncChannel channel{SessionId, peer, rate_limiter, request};
	ASSERT_EQ(ClientResyncResult::Requested,
		client.request_resynchronization(ResyncReason::Manual, 0U, ClientResyncScope::FullSnapshot, channel));
	ASSERT_EQ(1U, request.request_id);

	std::array<std::uint8_t, ResyncRequestPayloadSize> encoded{};
	std::size_t written = 0;
	ASSERT_EQ(ValidationError::None,
		encode_resync_request_payload(request, MutableByteView{encoded.data(), encoded.size()}, written));
	ASSERT_EQ(encoded.size(), written);
	TelemetryFragmenter fragmenter;
	ASSERT_TRUE(TelemetryFragmenter::create(byte_view(encoded), MessageSizeClass::State, fragmenter));

	ReliableWindowLimits limits;
	limits.jitter_seed = 0x12345678U;
	ReliableSendWindow window;
	ASSERT_EQ(ValidationError::None, ReliableSendWindow::configure(limits, window));
	ReliableMessageToRetain retained;
	retained.session_id = SessionId;
	retained.endpoint = peer;
	retained.message_type = MessageType::ResyncRequest;
	retained.base_flags = MessageFlagAckRequired;
	retained.message_id = MessageId;
	retained.fragment_count = fragmenter.fragment_count();
	retained.message_crc32 = fragmenter.message_crc32();
	retained.logical_payload = byte_view(encoded);
	retained.required_ack = RequiredAckLevel::Validated;
	retained.message_class = ReliableMessageClass::ControlRequestResync;
	ASSERT_EQ(ReliableRetainResult::Retained, window.retain(retained, 0U));

	ProducerResyncTracker producer;
	std::uint32_t candidate_start_count = 0U;
	const auto first = producer.accept(request, 0U);
	ASSERT_TRUE(producer_resync_result_requires_validated_ack(first));
	if (producer_resync_result_starts_candidate(first)) {
		++candidate_start_count;
	}
	ASSERT_EQ(1U, candidate_start_count);

	// The first VALIDATED ACK is lost. P0.5 retries the exact retained bytes.
	const auto retry_at = reliable_retry_delay_us(window.base_rto_us(), limits.jitter_seed, SessionId, MessageId, 0U);
	ReliableWindowAction action;
	ASSERT_EQ(ReliablePullResult::Action, window.pull_next_action(retry_at, action));
	ASSERT_EQ(ReliableWindowActionKind::Retransmit, action.kind);
	EXPECT_EQ(encoded.size(), action.retransmission.logical_payload.size);
	EXPECT_TRUE(std::equal(encoded.begin(), encoded.end(), action.retransmission.logical_payload.begin()));
	ResyncRequestPayload replayed;
	ASSERT_EQ(ValidationError::None, decode_resync_request_payload(action.retransmission.logical_payload, replayed));
	const auto duplicate = producer.accept(replayed, retry_at);
	EXPECT_EQ(ProducerResyncResult::AcceptedDuplicate, duplicate);
	EXPECT_TRUE(producer_resync_result_requires_validated_ack(duplicate));
	EXPECT_FALSE(producer_resync_result_starts_candidate(duplicate));
	EXPECT_EQ(1U, candidate_start_count);

	AckPayload validated;
	validated.target_message_id = MessageId;
	validated.target_message_type = MessageType::ResyncRequest;
	validated.ack_flags = static_cast<std::uint8_t>(AckFlag::Validated);
	validated.target_fragment_count = fragmenter.fragment_count();
	validated.target_message_crc32 = fragmenter.message_crc32();
	ASSERT_EQ(ReliableResponseResult::Released, window.acknowledge(SessionId, peer, validated, retry_at));
	EXPECT_EQ(0U, window.entry_count());

	// A newer request while the accepted resync transaction is alive is ACKed
	// but coalesced into that one candidate.
	ASSERT_EQ(ClientResyncResult::Requested,
		client.request_resynchronization(ResyncReason::SessionStale,
			retry_at,
			ClientResyncScope::FullSnapshot,
			channel));
	EXPECT_EQ(2U, request.request_id);
	const auto coalesced = producer.accept(request, retry_at);
	EXPECT_EQ(ProducerResyncResult::AcceptedCoalesced, coalesced);
	EXPECT_TRUE(producer_resync_result_requires_validated_ack(coalesced));
	EXPECT_FALSE(producer_resync_result_starts_candidate(coalesced));
	EXPECT_EQ(1U, candidate_start_count);
	EXPECT_EQ(1U, producer.candidate_request_id());
}

} // namespace
