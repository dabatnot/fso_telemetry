#include "telemetry/protocol/telemetry_replication.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace {

using namespace telemetry::protocol;

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

StateAtom owned_atom(std::uint16_t type,
	std::vector<std::uint8_t> key,
	std::vector<std::uint8_t> value,
	const StateAtomKey& owner)
{
	auto result = atom(type, std::move(key), std::move(value));
	result.has_cascade_owner = true;
	result.cascade_owner = owner;
	return result;
}

StateImage image(std::vector<StateAtom> atoms)
{
	StateImage result;
	EXPECT_EQ(StateImageResult::Created, StateImage::create(std::move(atoms), result));
	return result;
}

SnapshotCandidatePart
snapshot_part(std::uint32_t message_id, std::uint16_t fragment_count = 1U, std::uint32_t message_crc32 = 0x12345678U)
{
	SnapshotCandidatePart result;
	result.target.message_id = message_id;
	result.target.message_type = MessageType::FullSnapshot;
	result.target.fragment_count = fragment_count;
	result.target.message_crc32 = message_crc32;
	return result;
}

AckPayload ack(const SnapshotCandidatePart& part, std::uint8_t flags = KnownAckFlags)
{
	AckPayload result;
	result.target_message_id = part.target.message_id;
	result.target_message_type = part.target.message_type;
	result.ack_flags = flags;
	result.target_fragment_count = part.target.fragment_count;
	result.target_message_crc32 = part.target.message_crc32;
	return result;
}

EndpointKey endpoint()
{
	return EndpointKey::from_ipv4({127U, 0U, 0U, 42U}, 7808U);
}

std::unique_ptr<ProtocolRateLimiter> limiter(std::uint64_t initial_time_us = 0U)
{
	auto result = std::make_unique<ProtocolRateLimiter>();
	EXPECT_EQ(ValidationError::None, ProtocolRateLimiter::configure({}, initial_time_us, *result));
	return result;
}

SnapshotCommitResult commit_snapshot(ClientReplicationModel& client,
	std::uint32_t snapshot_id,
	std::uint32_t required_manifest_id,
	const StateImage& snapshot,
	std::uint64_t now_us,
	const StateImageValidator* validator = nullptr)
{
	auto rate_limiter = limiter(now_us);
	ResyncRequestPayload request;
	ClientResyncChannel channel{42U, endpoint(), *rate_limiter, request};
	return client.commit_snapshot(snapshot_id, required_manifest_id, snapshot, now_us, channel, validator);
}

void establish_producer_baseline(ProducerBaselineTracker& tracker,
	const StateImage& baseline,
	std::uint32_t snapshot_id = 1U,
	std::uint32_t message_id = 100U)
{
	ASSERT_EQ(ProducerBaselineResult::Applied, tracker.initialize(baseline));
	const std::vector<SnapshotCandidatePart> parts{snapshot_part(message_id)};
	ASSERT_EQ(ProducerBaselineResult::Applied, tracker.capture_snapshot(snapshot_id, 0U, baseline, parts, 0U));
	ASSERT_EQ(ProducerBaselineResult::Applied, tracker.acknowledge_snapshot_part(ack(parts[0]), 1U));
	ASSERT_TRUE(tracker.has_active_baseline());
	ASSERT_EQ(snapshot_id, tracker.active_snapshot_id());
}

void establish_client_baseline(ClientReplicationModel& client,
	const StateImage& baseline,
	std::uint32_t snapshot_id = 1U,
	std::uint32_t required_manifest_id = 0U)
{
	if (required_manifest_id != 0U) {
		ASSERT_EQ(ManifestInstallResult::Installed, client.install_manifest(required_manifest_id));
	}
	ASSERT_EQ(SnapshotCandidateResult::Known,
		client.note_snapshot_candidate(snapshot_id, required_manifest_id, 0U, 10'000'000U));
	ASSERT_EQ(SnapshotCommitResult::Committed,
		commit_snapshot(client, snapshot_id, required_manifest_id, baseline, 1U));
}

CumulativeStateDelta delta(std::uint32_t baseline_id, std::uint32_t sequence, std::vector<StateMutation> mutations)
{
	CumulativeStateDelta result;
	result.baseline_snapshot_id = baseline_id;
	result.delta_sequence = sequence;
	result.producer_sample_time_us = 1234U + sequence;
	result.mutations = std::move(mutations);
	return result;
}

StateMutation upsert(StateAtom value)
{
	return {StateMutationKind::Upsert, std::move(value)};
}

StateMutation erase(std::uint16_t type, std::vector<std::uint8_t> key)
{
	StateMutation result;
	result.kind = StateMutationKind::Delete;
	result.atom.key.record_type = type;
	result.atom.key.identity = std::move(key);
	result.atom.lifecycle = StateRecordLifecycle::ExplicitCreateDelete;
	return result;
}

ResyncRequestPayload
resync_request(std::uint32_t request_id, std::uint64_t client_send_time_us, ResyncReason reason = ResyncReason::Manual)
{
	ResyncRequestPayload result;
	result.request_id = request_id;
	result.reason = reason;
	result.request_flags = ResyncRequestFlagRequireFullSnapshot;
	result.last_applied_snapshot_id = 1U;
	result.last_applied_delta_sequence = 2U;
	result.client_send_time_us = client_send_time_us;
	return result;
}

class RejectMarkerValidator final : public StateImageValidator {
  public:
	explicit RejectMarkerValidator(std::uint8_t marker) : m_marker(marker) {}

	ValidationError validate(const StateImage& state) const noexcept override
	{
		for (const auto& record : state.records()) {
			if (std::find(record.value.begin(), record.value.end(), m_marker) != record.value.end()) {
				return ValidationError::OutOfRange;
			}
		}
		return ValidationError::None;
	}

  private:
	std::uint8_t m_marker;
};

TEST(TelemetryProtocolReplication, StateImageCanonicalizesKeysAndRejectsInvalidOrDuplicateAtoms)
{
	const auto second = atom(2U, {2U}, {2U, 0x22U}, StateRecordLifecycle::ExplicitCreateDelete);
	const auto first = atom(1U, {}, {0x11U});
	const auto canonical = image({second, first});
	ASSERT_EQ(2U, canonical.records().size());
	EXPECT_EQ(1U, canonical.records()[0].key.record_type);
	EXPECT_EQ(2U, canonical.records()[1].key.record_type);
	EXPECT_EQ(RecordEnvelopeHeaderSize * 2U + first.value.size() + second.value.size(),
		canonical.encoded_snapshot_records_size());

	StateImage unchanged = canonical;
	EXPECT_EQ(StateImageResult::DuplicateKey, StateImage::create({first, first}, unchanged));
	EXPECT_EQ(canonical, unchanged);

	auto invalid = first;
	invalid.record_version = 2U;
	EXPECT_EQ(StateImageResult::InvalidRecord, StateImage::create({invalid}, unchanged));
	EXPECT_EQ(canonical, unchanged);
}

TEST(TelemetryProtocolReplication, RadarContactsVersionFourSupportsUpsertAndDeleteAtoms)
{
	std::vector<std::uint8_t> key(16U, 0U);
	key[0] = 1U;
	key[8] = 2U;
	auto value = key;
	value.push_back(0x42U);
	auto radar = atom(static_cast<std::uint16_t>(RecordType::RadarContacts),
		key, value, StateRecordLifecycle::ExplicitCreateDelete);
	radar.record_version = 4U;
	StateImage state;
	EXPECT_EQ(StateImageResult::Created, StateImage::create({radar}, state));

	StateMutation deletion;
	deletion.kind = StateMutationKind::Delete;
	deletion.atom.key = radar.key;
	deletion.atom.record_version = 4U;
	deletion.atom.lifecycle = StateRecordLifecycle::ExplicitCreateDelete;
	EXPECT_EQ(StateDeltaValidationResult::Valid,
		validate_cumulative_state_delta(delta(1U, 1U, {deletion})));
}

TEST(TelemetryProtocolReplication, DeltaValidationRequiresCanonicalUniqueCompleteMutationsAndExactLimit)
{
	const auto first = upsert(atom(1U, {}, {0x01U}));
	const auto second = upsert(atom(2U, {}, {0x02U}));
	auto valid = delta(7U, 9U, {first, second});
	EXPECT_EQ(StateDeltaValidationResult::Valid, validate_cumulative_state_delta(valid));
	EXPECT_EQ(DeltaPayloadPrefixSize + 2U * RecordEnvelopeHeaderSize + 2U, valid.encoded_size());

	auto out_of_order = delta(7U, 9U, {second, first});
	EXPECT_EQ(StateDeltaValidationResult::InvalidMutation, validate_cumulative_state_delta(out_of_order));
	auto duplicate = delta(7U, 9U, {first, first});
	EXPECT_EQ(StateDeltaValidationResult::DuplicateKey, validate_cumulative_state_delta(duplicate));
	auto invalid_create = first;
	invalid_create.kind = StateMutationKind::Create;
	EXPECT_EQ(StateDeltaValidationResult::InvalidMutation,
		validate_cumulative_state_delta(delta(7U, 9U, {invalid_create})));
	valid.baseline_snapshot_id = 0U;
	EXPECT_EQ(StateDeltaValidationResult::InvalidIdentity, validate_cumulative_state_delta(valid));
}

TEST(TelemetryProtocolReplication, ApplicationAlwaysClonesBaselineAndPublishesAtomically)
{
	const auto baseline =
		image({atom(1U, {}, {0x10U}), atom(10U, {1U}, {1U, 0x20U}, StateRecordLifecycle::ExplicitCreateDelete)});
	const auto newer = delta(1U,
		7U,
		{upsert(atom(1U, {}, {0x11U})),
			erase(10U, {1U}),
			{StateMutationKind::Create, atom(10U, {2U}, {2U, 0x30U}, StateRecordLifecycle::ExplicitCreateDelete)}});
	StateImage published = baseline;
	ASSERT_EQ(StateDeltaApplyResult::Applied, apply_cumulative_state_delta(baseline, newer, nullptr, published));
	EXPECT_EQ(image({atom(1U, {}, {0x11U}), atom(10U, {2U}, {2U, 0x30U}, StateRecordLifecycle::ExplicitCreateDelete)}),
		published);
	EXPECT_EQ((std::vector<std::uint8_t>{0x10U}), baseline.records()[0].value);

	const RejectMarkerValidator reject(0xeeU);
	const auto invalid = delta(1U, 8U, {upsert(atom(1U, {}, {0xeeU}))});
	const auto before = published;
	EXPECT_EQ(StateDeltaApplyResult::ValidationFailed,
		apply_cumulative_state_delta(baseline, invalid, &reject, published));
	EXPECT_EQ(before, published);
}

TEST(TelemetryProtocolReplication, ExplicitOwnerDeleteCascadesDependentAtomsWithoutExtraWireDeletes)
{
	const auto owner = atom(5U, {1U}, {1U, 0x10U}, StateRecordLifecycle::ExplicitCreateDelete);
	const auto child = owned_atom(7U, {1U}, {1U, 0x20U}, owner.key);
	const auto baseline = image({owner, child});
	ProducerBaselineTracker producer;
	establish_producer_baseline(producer, baseline);
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.replace_current(image({})));
	CumulativeStateDelta deletion;
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.emit_cumulative_delta(10U, deletion));
	ASSERT_EQ(1U, deletion.mutations.size());
	EXPECT_EQ(StateMutationKind::Delete, deletion.mutations[0].kind);
	EXPECT_EQ(owner.key, deletion.mutations[0].atom.key);

	StateImage applied = baseline;
	ASSERT_EQ(StateDeltaApplyResult::Applied, apply_cumulative_state_delta(baseline, deletion, nullptr, applied));
	EXPECT_TRUE(applied.empty());

	StateImage invalid = baseline;
	EXPECT_EQ(StateImageResult::InvalidRecord, StateImage::create({child}, invalid));
	EXPECT_EQ(baseline, invalid);

	auto nested_owner = owned_atom(8U, {1U}, {1U, 0x30U}, owner.key);
	nested_owner.lifecycle = StateRecordLifecycle::ExplicitCreateDelete;
	const auto nested_child = owned_atom(9U, {1U}, {1U, 0x40U}, nested_owner.key);
	EXPECT_EQ(StateImageResult::InvalidRecord, StateImage::create({owner, nested_owner, nested_child}, invalid));

	// Mutation ordering cannot change cascade semantics. The child sorts before
	// its owner delete, but an explicit update below that deleted root is still
	// rejected instead of being silently applied or discarded.
	const auto late_owner = atom(10U, {1U}, {1U, 0x50U}, StateRecordLifecycle::ExplicitCreateDelete);
	const auto early_child = owned_atom(1U, {1U}, {1U, 0x60U}, late_owner.key);
	const auto ordered_baseline = image({late_owner, early_child});
	const auto contradictory =
		delta(1U, 1U, {upsert(owned_atom(1U, {1U}, {1U, 0x61U}, late_owner.key)), erase(10U, {1U})});
	StateImage unchanged = ordered_baseline;
	EXPECT_EQ(StateDeltaApplyResult::InvalidTransition,
		apply_cumulative_state_delta(ordered_baseline, contradictory, nullptr, unchanged));
	EXPECT_EQ(ordered_baseline, unchanged);
}

TEST(TelemetryProtocolReplication, ExplicitLifecycleCreationCannotMasqueradeAsUpsert)
{
	const auto baseline = image({});
	const auto explicit_atom = atom(10U, {1U}, {1U, 0x20U}, StateRecordLifecycle::ExplicitCreateDelete);
	const auto invalid = delta(1U, 1U, {upsert(explicit_atom)});
	StateImage applied = baseline;
	EXPECT_EQ(StateDeltaApplyResult::InvalidTransition,
		apply_cumulative_state_delta(baseline, invalid, nullptr, applied));
	EXPECT_EQ(baseline, applied);

	const auto valid = delta(1U, 1U, {{StateMutationKind::Create, explicit_atom}});
	EXPECT_EQ(StateDeltaApplyResult::Applied, apply_cumulative_state_delta(baseline, valid, nullptr, applied));
	EXPECT_EQ(image({explicit_atom}), applied);
}

TEST(TelemetryProtocolReplication, ProducerPromotesOnlyAfterEveryExactAppliedAck)
{
	const auto baseline = image({atom(1U, {}, {0x10U})});
	ProducerBaselineTracker producer;
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.initialize(baseline));
	const std::vector<SnapshotCandidatePart> parts{snapshot_part(100U, 2U, 0xaaaaU), snapshot_part(101U, 1U, 0xbbbbU)};
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.capture_snapshot(1U, 7U, baseline, parts, 100U));
	EXPECT_FALSE(producer.has_active_baseline());

	EXPECT_EQ(ProducerBaselineResult::NoChange,
		producer.acknowledge_snapshot_part(ack(parts[0], static_cast<std::uint8_t>(AckFlag::Validated)), 101U));
	EXPECT_FALSE(producer.has_active_baseline());
	auto forged = ack(parts[0]);
	forged.target_message_crc32 ^= 1U;
	EXPECT_EQ(ProducerBaselineResult::InvalidAck, producer.acknowledge_snapshot_part(forged, 102U));
	EXPECT_FALSE(producer.has_active_baseline());
	EXPECT_EQ(ProducerBaselineResult::Applied, producer.acknowledge_snapshot_part(ack(parts[1]), 103U));
	EXPECT_FALSE(producer.has_active_baseline());
	EXPECT_EQ(ProducerBaselineResult::Applied, producer.acknowledge_snapshot_part(ack(parts[0]), 104U));
	EXPECT_TRUE(producer.has_active_baseline());
	EXPECT_FALSE(producer.has_candidate());
	EXPECT_EQ(1U, producer.active_snapshot_id());
	EXPECT_EQ(7U, producer.active_required_manifest_id());
}

TEST(TelemetryProtocolReplication, ProducerResyncRequestsAreDeduplicatedAndCoalescedWithoutGrowth)
{
	ProducerResyncTracker tracker;
	const auto first = resync_request(1U, 10U);
	EXPECT_EQ(ProducerResyncResult::AcceptedNewCandidate, tracker.accept(first, 100U));
	EXPECT_TRUE(tracker.has_candidate());
	EXPECT_EQ(1U, tracker.candidate_request_id());
	EXPECT_EQ(10'000'100U, tracker.candidate_deadline_us());
	EXPECT_TRUE(producer_resync_result_requires_validated_ack(ProducerResyncResult::AcceptedNewCandidate));
	EXPECT_TRUE(producer_resync_result_starts_candidate(ProducerResyncResult::AcceptedNewCandidate));

	EXPECT_EQ(ProducerResyncResult::AcceptedDuplicate, tracker.accept(first, 101U));
	EXPECT_FALSE(producer_resync_result_starts_candidate(ProducerResyncResult::AcceptedDuplicate));
	auto conflicting = first;
	conflicting.reason = ResyncReason::SessionStale;
	EXPECT_EQ(ProducerResyncResult::Invalid, tracker.accept(conflicting, 102U));

	const auto third = resync_request(3U, 30U, ResyncReason::UnknownBaseline);
	EXPECT_EQ(ProducerResyncResult::AcceptedCoalesced, tracker.accept(third, 103U));
	EXPECT_EQ(2U, tracker.deduplication_entry_count());
	EXPECT_EQ(1U, tracker.candidate_request_id());
	EXPECT_FALSE(producer_resync_result_starts_candidate(ProducerResyncResult::AcceptedCoalesced));
	// request_id has no numeric ordering rule; reliable reordering of 1, 3, 2
	// still coalesces every distinct request into the one candidate.
	EXPECT_EQ(ProducerResyncResult::AcceptedCoalesced, tracker.accept(resync_request(2U, 20U), 104U));
	EXPECT_EQ(3U, tracker.deduplication_entry_count());
	auto conflicting_second = resync_request(2U, 20U);
	conflicting_second.reason = ResyncReason::SessionStale;
	EXPECT_EQ(ProducerResyncResult::Invalid, tracker.accept(conflicting_second, 105U));
	EXPECT_EQ(ProducerResyncResult::AcceptedDuplicate, tracker.accept(third, 105U));

	EXPECT_EQ(ProducerResyncResult::Completed, tracker.complete());
	EXPECT_FALSE(tracker.has_candidate());
	// While the semantic ID is still protected, only the reliable receive cache
	// may replay its old ACK after the candidate has completed.
	EXPECT_EQ(ProducerResyncResult::Stale, tracker.accept(first, 106U));
	EXPECT_FALSE(tracker.has_candidate());
	EXPECT_EQ(ProducerResyncResult::AcceptedNewCandidate, tracker.accept(resync_request(4U, 40U), 107U));
	EXPECT_EQ(4U, tracker.candidate_request_id());
	EXPECT_EQ(ProducerResyncResult::NoChange, tracker.expire(10'000'106U));
	EXPECT_EQ(0U, tracker.deduplication_entry_count());
	EXPECT_EQ(ProducerResyncResult::Expired, tracker.expire(10'000'107U));
	EXPECT_FALSE(tracker.has_candidate());
	// The old request_id is reusable outside its seven-second identity window;
	// accepting it starts real work instead of ACKing a nonexistent candidate.
	EXPECT_EQ(ProducerResyncResult::AcceptedNewCandidate, tracker.accept(first, 10'000'108U));
	EXPECT_TRUE(tracker.has_candidate());

	tracker.clear();
	EXPECT_EQ(0U, tracker.deduplication_entry_count());
	EXPECT_FALSE(tracker.has_candidate());
	EXPECT_EQ(ProducerResyncResult::Invalid, tracker.accept(ResyncRequestPayload{}, 0U));
}

TEST(TelemetryProtocolReplication, ResyncRequestIdsReuseAtTheExactDedupBoundaryWithoutMovingCandidateDeadline)
{
	ProducerResyncTracker tracker;
	const auto first = resync_request(7U, 10U);
	ASSERT_EQ(ProducerResyncResult::AcceptedNewCandidate, tracker.accept(first, 100U));
	const auto candidate_deadline = tracker.candidate_deadline_us();

	auto reused = first;
	reused.reason = ResyncReason::SessionStale;
	reused.client_send_time_us = 20U;
	EXPECT_EQ(ProducerResyncResult::AcceptedDuplicate,
		tracker.accept(first, 100U + ResyncRequestDeduplicationWindowUs - 2U));
	EXPECT_EQ(ProducerResyncResult::Invalid,
		tracker.accept(reused, 100U + ResyncRequestDeduplicationWindowUs - 1U));
	EXPECT_EQ(candidate_deadline, tracker.candidate_deadline_us());
	EXPECT_EQ(1U, tracker.deduplication_entry_count());
	EXPECT_EQ(ProducerResyncResult::AcceptedCoalesced,
		tracker.accept(reused, 100U + ResyncRequestDeduplicationWindowUs));
	EXPECT_EQ(candidate_deadline, tracker.candidate_deadline_us());
	EXPECT_EQ(1U, tracker.deduplication_entry_count());
}

TEST(TelemetryProtocolReplication, ResyncRequestDeduplicationIsFixedCapacityWrapSafeAndOverflowSafe)
{
	EXPECT_EQ(std::numeric_limits<std::uint32_t>::max(),
		next_resync_request_id(std::numeric_limits<std::uint32_t>::max() - 1U));
	EXPECT_EQ(1U, next_resync_request_id(std::numeric_limits<std::uint32_t>::max()));
	EXPECT_EQ(1U, next_resync_request_id(0U));

	ProducerResyncTracker tracker;
	EXPECT_EQ(ProducerResyncResult::AcceptedNewCandidate,
		tracker.accept(resync_request(std::numeric_limits<std::uint32_t>::max() - 1U, 1U), 0U));
	EXPECT_EQ(ProducerResyncResult::AcceptedCoalesced,
		tracker.accept(resync_request(std::numeric_limits<std::uint32_t>::max(), 2U), 1U));
	EXPECT_EQ(ProducerResyncResult::AcceptedCoalesced, tracker.accept(resync_request(1U, 3U), 2U));
	for (std::uint32_t id = 2U; id <= 7U; ++id) {
		EXPECT_EQ(ProducerResyncResult::AcceptedCoalesced, tracker.accept(resync_request(id, id + 2U), id + 1U));
	}
	ASSERT_EQ(ResyncRequestDeduplicationCapacity, tracker.deduplication_entry_count());
	EXPECT_EQ(ProducerResyncResult::ResourceLimit, tracker.accept(resync_request(8U, 10U), 10U));
	EXPECT_EQ(ProducerResyncResult::ResourceLimit,
		tracker.accept(resync_request(8U, 10U), ResyncRequestDeduplicationWindowUs - 1U));
	EXPECT_EQ(ProducerResyncResult::AcceptedCoalesced,
		tracker.accept(resync_request(8U, 10U), ResyncRequestDeduplicationWindowUs));
	EXPECT_EQ(ResyncRequestDeduplicationCapacity, tracker.deduplication_entry_count());

	ProducerResyncTracker saturated_clock;
	const auto near_max_time = std::numeric_limits<std::uint64_t>::max() - 1U;
	const auto original = resync_request(9U, near_max_time);
	ASSERT_EQ(ProducerResyncResult::AcceptedNewCandidate, saturated_clock.accept(original, near_max_time));
	auto changed = original;
	changed.reason = ResyncReason::SessionStale;
	EXPECT_EQ(ProducerResyncResult::Invalid, saturated_clock.accept(changed, near_max_time));
	EXPECT_EQ(ProducerResyncResult::AcceptedNewCandidate,
		saturated_clock.accept(changed, std::numeric_limits<std::uint64_t>::max()));
	EXPECT_EQ(std::numeric_limits<std::uint64_t>::max(), saturated_clock.candidate_deadline_us());
}

TEST(TelemetryProtocolReplication, ProducerKeepsBothDirtySetsAndPreservesPostCaptureMutations)
{
	const auto initial =
		image({atom(1U, {}, {0x10U}), atom(10U, {1U}, {1U, 0x20U}, StateRecordLifecycle::ExplicitCreateDelete)});
	ProducerBaselineTracker producer;
	establish_producer_baseline(producer, initial);

	const auto at_capture =
		image({atom(1U, {}, {0x11U}), atom(10U, {1U}, {1U, 0x20U}, StateRecordLifecycle::ExplicitCreateDelete)});
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.replace_current(at_capture));
	const std::vector<SnapshotCandidatePart> parts{snapshot_part(200U), snapshot_part(201U)};
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.capture_snapshot(2U, 0U, at_capture, parts, 10U));

	const auto after_capture =
		image({atom(1U, {}, {0x12U}), atom(10U, {2U}, {2U, 0x30U}, StateRecordLifecycle::ExplicitCreateDelete)});
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.replace_current(after_capture));
	EXPECT_EQ(3U, producer.active_dirty_record_count());
	EXPECT_EQ(3U, producer.candidate_dirty_record_count());

	CumulativeStateDelta old_baseline_delta;
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.emit_cumulative_delta(100U, old_baseline_delta));
	EXPECT_EQ(1U, old_baseline_delta.baseline_snapshot_id);
	EXPECT_EQ(1U, old_baseline_delta.delta_sequence);

	ASSERT_EQ(ProducerBaselineResult::Applied, producer.acknowledge_snapshot_part(ack(parts[1]), 20U));
	EXPECT_EQ(1U, producer.active_snapshot_id());
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.acknowledge_snapshot_part(ack(parts[0]), 21U));
	EXPECT_EQ(2U, producer.active_snapshot_id());
	EXPECT_EQ(at_capture, producer.active_baseline());
	EXPECT_EQ(1U, producer.next_delta_sequence());

	CumulativeStateDelta first_new_delta;
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.emit_cumulative_delta(101U, first_new_delta));
	EXPECT_EQ(2U, first_new_delta.baseline_snapshot_id);
	EXPECT_EQ(1U, first_new_delta.delta_sequence);
	ASSERT_EQ(3U, first_new_delta.mutations.size());
	EXPECT_EQ(StateMutationKind::Upsert, first_new_delta.mutations[0].kind);
	EXPECT_EQ(StateMutationKind::Delete, first_new_delta.mutations[1].kind);
	EXPECT_EQ(StateMutationKind::Create, first_new_delta.mutations[2].kind);
	StateImage converged;
	ASSERT_EQ(StateDeltaApplyResult::Applied,
		apply_cumulative_state_delta(at_capture, first_new_delta, nullptr, converged));
	EXPECT_EQ(after_capture, converged);
}

TEST(TelemetryProtocolReplication, SnapshotPromotionUsesTheExactCapturedImageNotTheLaterCurrentState)
{
	const auto captured = image({atom(1U, {}, {0x10U})});
	const auto later = image({atom(1U, {}, {0x20U})});
	ProducerBaselineTracker producer;
	establish_producer_baseline(producer, captured);
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.replace_current(later));
	const std::vector<SnapshotCandidatePart> parts{snapshot_part(300U)};
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.capture_snapshot(2U, 0U, captured, parts, 10U));
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.acknowledge_snapshot_part(ack(parts[0]), 11U));
	EXPECT_EQ(captured, producer.active_baseline());
	EXPECT_EQ(1U, producer.active_dirty_record_count());

	CumulativeStateDelta first;
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.emit_cumulative_delta(12U, first));
	StateImage converged;
	ASSERT_EQ(StateDeltaApplyResult::Applied, apply_cumulative_state_delta(captured, first, nullptr, converged));
	EXPECT_EQ(later, converged);
}

TEST(TelemetryProtocolReplication, LatestCumulativeDeltaConvergesAfterEveryIntermediateLoss)
{
	const auto baseline = image({atom(1U, {}, {0x10U})});
	ProducerBaselineTracker producer;
	establish_producer_baseline(producer, baseline);
	ClientReplicationModel client;
	establish_client_baseline(client, baseline);
	auto rate_limiter = limiter();
	ResyncRequestPayload resync;

	ASSERT_EQ(ProducerBaselineResult::Applied, producer.replace_current(image({atom(1U, {}, {0x11U})})));
	CumulativeStateDelta lost;
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.emit_cumulative_delta(10U, lost));
	EXPECT_EQ(1U, lost.delta_sequence);

	const auto latest_state =
		image({atom(1U, {}, {0x12U}), atom(10U, {7U}, {7U, 0x44U}, StateRecordLifecycle::ExplicitCreateDelete)});
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.replace_current(latest_state));
	CumulativeStateDelta latest;
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.emit_cumulative_delta(11U, latest));
	EXPECT_EQ(2U, latest.delta_sequence);
	ASSERT_EQ(ClientDeltaResult::Applied, client.receive_delta(latest, 100U, 42U, endpoint(), *rate_limiter, resync));
	EXPECT_EQ(latest_state, client.published());
	EXPECT_EQ(2U, client.last_delta_sequence());

	EXPECT_EQ(ClientDeltaResult::IgnoredOldSequence,
		client.receive_delta(lost, 101U, 42U, endpoint(), *rate_limiter, resync));
	EXPECT_EQ(latest_state, client.published());
}

TEST(TelemetryProtocolReplication, NewerDeltaThatReturnsToBaselineRemovesTheOlderDifference)
{
	const auto baseline = image({atom(1U, {}, {0x10U})});
	ProducerBaselineTracker producer;
	establish_producer_baseline(producer, baseline);
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.replace_current(image({atom(1U, {}, {0x20U})})));
	CumulativeStateDelta changed;
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.emit_cumulative_delta(1U, changed));
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.replace_current(baseline));
	CumulativeStateDelta sentinel = changed;
	EXPECT_EQ(ProducerBaselineResult::KeyframeRequired, producer.emit_cumulative_delta(2U, sentinel));
	EXPECT_EQ(changed, sentinel);

	ClientReplicationModel client;
	establish_client_baseline(client, baseline);
	auto rate_limiter = limiter();
	ResyncRequestPayload resync;
	ASSERT_EQ(ClientDeltaResult::Applied, client.receive_delta(changed, 10U, 1U, endpoint(), *rate_limiter, resync));
	EXPECT_NE(baseline, client.published());
	// A later cumulative delta with another difference is rebuilt from the
	// baseline, so the omitted first atom returns to its baseline value.
	const auto other = delta(1U,
		2U,
		{{StateMutationKind::Create, atom(10U, {1U}, {1U, 0x33U}, StateRecordLifecycle::ExplicitCreateDelete)}});
	ASSERT_EQ(ClientDeltaResult::Applied, client.receive_delta(other, 11U, 1U, endpoint(), *rate_limiter, resync));
	EXPECT_EQ((std::vector<std::uint8_t>{0x10U}), client.published().records()[0].value);
}

TEST(TelemetryProtocolReplication, KnownCandidateKeepsOnlyHighestDeltaWithinAbsoluteTwoSecondWindow)
{
	const auto snapshot = image({atom(1U, {}, {0x10U})});
	ClientReplicationModel client;
	ASSERT_EQ(SnapshotCandidateResult::Known, client.note_snapshot_candidate(2U, 0U, 100U, 10'000'100U));
	auto rate_limiter = limiter();
	ResyncRequestPayload resync;
	const auto first = delta(2U, 1U, {upsert(atom(1U, {}, {0x11U}))});
	const auto highest = delta(2U, 5U, {upsert(atom(1U, {}, {0x15U}))});
	const auto lower = delta(2U, 4U, {upsert(atom(1U, {}, {0x14U}))});
	EXPECT_EQ(ClientDeltaResult::QueuedForCandidate,
		client.receive_delta(first, 100U, 42U, endpoint(), *rate_limiter, resync));
	EXPECT_EQ(ClientDeltaResult::ReplacedQueuedDelta,
		client.receive_delta(highest, 1'000'000U, 42U, endpoint(), *rate_limiter, resync));
	EXPECT_EQ(ClientDeltaResult::IgnoredOlderQueuedDelta,
		client.receive_delta(lower, 1'500'000U, 42U, endpoint(), *rate_limiter, resync));
	EXPECT_EQ(5U, client.pending_delta_sequence());

	EXPECT_EQ(SnapshotCommitResult::CommittedAndPendingDeltaApplied,
		commit_snapshot(client, 2U, 0U, snapshot, 1'999'999U));
	EXPECT_EQ(5U, client.last_delta_sequence());
	EXPECT_EQ((std::vector<std::uint8_t>{0x15U}), client.published().records()[0].value);
}

TEST(TelemetryProtocolReplication, PendingDeltaExpiresExactlyAndNeverOutlivesSnapshotCandidate)
{
	const auto snapshot = image({atom(1U, {}, {0x10U})});
	auto rate_limiter = limiter();
	ResyncRequestPayload resync;
	const auto early = delta(2U, 1U, {upsert(atom(1U, {}, {0x11U}))});

	ClientReplicationModel two_seconds;
	ASSERT_EQ(SnapshotCandidateResult::Known, two_seconds.note_snapshot_candidate(2U, 0U, 0U, 10'000'000U));
	ASSERT_EQ(ClientDeltaResult::QueuedForCandidate,
		two_seconds.receive_delta(early, 0U, 42U, endpoint(), *rate_limiter, resync));
	EXPECT_EQ(SnapshotCommitResult::Committed, commit_snapshot(two_seconds, 2U, 0U, snapshot, PendingDeltaLifetimeUs));
	EXPECT_EQ(0U, two_seconds.last_delta_sequence());
	EXPECT_EQ(snapshot, two_seconds.published());

	ClientReplicationModel snapshot_deadline;
	ASSERT_EQ(SnapshotCandidateResult::Known, snapshot_deadline.note_snapshot_candidate(2U, 0U, 0U, 10'000'000U));
	ASSERT_EQ(ClientDeltaResult::QueuedForCandidate,
		snapshot_deadline.receive_delta(early, 9'500'000U, 43U, endpoint(), *rate_limiter, resync));
	EXPECT_EQ(SnapshotCommitResult::Expired, commit_snapshot(snapshot_deadline, 2U, 0U, snapshot, 10'000'000U));
	EXPECT_FALSE(snapshot_deadline.has_pending_delta());
}

TEST(TelemetryProtocolReplication, TotallyUnknownBaselineDropsAndUsesExistingPerSessionResyncBucket)
{
	const auto baseline = image({atom(1U, {}, {0x10U})});
	ClientReplicationModel client;
	establish_client_baseline(client, baseline);
	auto rate_limiter = limiter();
	ResyncRequestPayload resync;
	const auto unknown = delta(99U, 1U, {upsert(atom(1U, {}, {0x20U}))});

	EXPECT_EQ(ClientDeltaResult::UnknownBaselineResyncRequested,
		client.receive_delta(unknown, 0U, 42U, endpoint(), *rate_limiter, resync));
	EXPECT_EQ(1U, resync.request_id);
	EXPECT_EQ(ResyncReason::UnknownBaseline, resync.reason);
	EXPECT_EQ(KnownResyncRequestFlags, resync.request_flags);
	EXPECT_EQ(1U, resync.last_applied_snapshot_id);
	EXPECT_EQ(0U, resync.last_applied_delta_sequence);
	EXPECT_FALSE(client.has_pending_delta());

	EXPECT_EQ(ClientDeltaResult::UnknownBaselineResyncRequested,
		client.receive_delta(unknown, 0U, 42U, endpoint(), *rate_limiter, resync));
	EXPECT_EQ(2U, resync.request_id);
	const auto unchanged = resync;
	EXPECT_EQ(ClientDeltaResult::UnknownBaselineRateLimited,
		client.receive_delta(unknown, 0U, 42U, endpoint(), *rate_limiter, resync));
	EXPECT_EQ(unchanged.request_id, resync.request_id);
	EXPECT_EQ(ClientDeltaResult::UnknownBaselineResyncRequested,
		client.receive_delta(unknown, 1'000'000U, 42U, endpoint(), *rate_limiter, resync));
	EXPECT_EQ(3U, resync.request_id);
}

TEST(TelemetryProtocolReplication, MissingManifestDefersSnapshotCommitWithoutDiscardingCandidateOrPendingDelta)
{
	const auto snapshot = image({atom(1U, {}, {0x10U})});
	ClientReplicationModel client;
	ASSERT_EQ(SnapshotCandidateResult::Known, client.note_snapshot_candidate(2U, 7U, 0U, 10'000'000U));
	auto rate_limiter = limiter();
	ResyncRequestPayload resync;
	const auto early = delta(2U, 1U, {upsert(atom(1U, {}, {0x11U}))});
	ASSERT_EQ(ClientDeltaResult::QueuedForCandidate,
		client.receive_delta(early, 1U, 42U, endpoint(), *rate_limiter, resync));
	EXPECT_EQ(SnapshotCommitResult::MissingManifest, commit_snapshot(client, 2U, 7U, snapshot, 2U));
	EXPECT_EQ(2U, client.candidate_snapshot_id());
	EXPECT_TRUE(client.has_pending_delta());
	EXPECT_EQ(ManifestInstallResult::Installed, client.install_manifest(7U));
	EXPECT_EQ(SnapshotCommitResult::CommittedAndPendingDeltaApplied, commit_snapshot(client, 2U, 7U, snapshot, 3U));
	EXPECT_EQ(2U, client.active_snapshot_id());
	EXPECT_EQ(1U, client.last_delta_sequence());
}

TEST(TelemetryProtocolReplication, OldBaselineIsSilentlyIgnoredAfterNewSnapshotCommit)
{
	const auto first = image({atom(1U, {}, {0x10U})});
	const auto second = image({atom(1U, {}, {0x20U})});
	ClientReplicationModel client;
	establish_client_baseline(client, first, 1U);
	ASSERT_EQ(SnapshotCandidateResult::Known, client.note_snapshot_candidate(2U, 0U, 2U, 10'000'002U));
	ASSERT_EQ(SnapshotCommitResult::Committed, commit_snapshot(client, 2U, 0U, second, 3U));
	auto rate_limiter = limiter();
	ResyncRequestPayload resync;
	const auto stale = delta(1U, 99U, {upsert(atom(1U, {}, {0x99U}))});
	EXPECT_EQ(ClientDeltaResult::IgnoredOldBaseline,
		client.receive_delta(stale, 4U, 42U, endpoint(), *rate_limiter, resync));
	EXPECT_EQ(second, client.published());
}

TEST(TelemetryProtocolReplication, ValidationFailureNeverPublishesOrAdvancesSequence)
{
	const auto baseline = image({atom(1U, {}, {0x10U})});
	ClientReplicationModel client;
	establish_client_baseline(client, baseline);
	auto rate_limiter = limiter();
	ResyncRequestPayload resync;
	const RejectMarkerValidator validator(0xeeU);
	const auto rejected = delta(1U, 5U, {upsert(atom(1U, {}, {0xeeU}))});
	EXPECT_EQ(ClientDeltaResult::ResyncRequested,
		client.receive_delta(rejected, 1U, 42U, endpoint(), *rate_limiter, resync, &validator));
	EXPECT_EQ(1U, resync.request_id);
	EXPECT_EQ(ResyncReason::ValidationFailed, resync.reason);
	EXPECT_EQ(KnownResyncRequestFlags, resync.request_flags);
	EXPECT_EQ(1U, resync.last_applied_snapshot_id);
	EXPECT_EQ(0U, resync.last_applied_delta_sequence);
	EXPECT_EQ(0U, client.last_delta_sequence());
	EXPECT_EQ(baseline, client.published());

	const auto accepted = delta(1U, 6U, {upsert(atom(1U, {}, {0x20U}))});
	EXPECT_EQ(ClientDeltaResult::Applied,
		client.receive_delta(accepted, 2U, 42U, endpoint(), *rate_limiter, resync, &validator));
	EXPECT_EQ(6U, client.last_delta_sequence());
}

TEST(TelemetryProtocolReplication, InvalidDeltaPathsRequestResyncWithoutPublicationAndRespectRateLimit)
{
	const auto baseline = image({atom(1U, {}, {0x10U})});
	ClientReplicationModel client;
	establish_client_baseline(client, baseline);
	auto rate_limiter = limiter();
	ResyncRequestPayload resync;

	const auto noncanonical = delta(1U, 1U, {upsert(atom(2U, {}, {0x20U})), upsert(atom(1U, {}, {0x11U}))});
	EXPECT_EQ(ClientDeltaResult::ResyncRequested,
		client.receive_delta(noncanonical, 0U, 42U, endpoint(), *rate_limiter, resync));
	EXPECT_EQ(ResyncReason::ValidationFailed, resync.reason);
	EXPECT_EQ(1U, resync.request_id);
	EXPECT_EQ(baseline, client.published());

	const auto missing_delete = delta(1U, 2U, {erase(10U, {1U})});
	EXPECT_EQ(ClientDeltaResult::ResyncRequested,
		client.receive_delta(missing_delete, 0U, 42U, endpoint(), *rate_limiter, resync));
	EXPECT_EQ(2U, resync.request_id);
	EXPECT_EQ(baseline, client.published());
	EXPECT_FALSE(client.has_pending_delta());

	const auto unchanged_request = resync;
	EXPECT_EQ(ClientDeltaResult::ResyncRateLimited,
		client.receive_delta(noncanonical, 0U, 42U, endpoint(), *rate_limiter, resync));
	EXPECT_EQ(unchanged_request.request_id, resync.request_id);
	EXPECT_EQ(unchanged_request.reason, resync.reason);
	EXPECT_EQ(unchanged_request.request_flags, resync.request_flags);
	EXPECT_EQ(unchanged_request.last_applied_snapshot_id, resync.last_applied_snapshot_id);
	EXPECT_EQ(unchanged_request.last_applied_delta_sequence, resync.last_applied_delta_sequence);
	EXPECT_EQ(unchanged_request.client_send_time_us, resync.client_send_time_us);
	EXPECT_EQ(0U, client.last_delta_sequence());
	EXPECT_EQ(baseline, client.published());
}

TEST(TelemetryProtocolReplication, SnapshotValidationFailureReleasesCandidateAndRequestsResync)
{
	const auto baseline = image({atom(1U, {}, {0x10U})});
	const auto rejected = image({atom(1U, {}, {0xeeU})});
	ClientReplicationModel client;
	establish_client_baseline(client, baseline);
	ASSERT_EQ(SnapshotCandidateResult::Known, client.note_snapshot_candidate(2U, 0U, 100U, 10'000'100U));
	auto rate_limiter = limiter();
	ResyncRequestPayload request;
	ASSERT_EQ(ClientDeltaResult::QueuedForCandidate,
		client.receive_delta(delta(2U, 1U, {upsert(atom(1U, {}, {0x20U}))}),
			101U,
			42U,
			endpoint(),
			*rate_limiter,
			request));
	ClientResyncChannel channel{42U, endpoint(), *rate_limiter, request};
	const RejectMarkerValidator validator(0xeeU);
	EXPECT_EQ(SnapshotCommitResult::ValidationFailed,
		client.commit_snapshot(2U, 0U, rejected, 102U, channel, &validator));
	EXPECT_EQ(ClientResyncResult::Requested, channel.disposition);
	EXPECT_EQ(ResyncReason::ValidationFailed, request.reason);
	EXPECT_EQ(1U, request.last_applied_snapshot_id);
	EXPECT_EQ(0U, request.last_applied_delta_sequence);
	EXPECT_EQ(0U, client.candidate_snapshot_id());
	EXPECT_FALSE(client.has_pending_delta());
	EXPECT_EQ(1U, client.active_snapshot_id());
	EXPECT_EQ(baseline, client.published());
	EXPECT_EQ(SnapshotCandidateResult::Known, client.note_snapshot_candidate(3U, 0U, 103U, 10'000'103U));
}

TEST(TelemetryProtocolReplication, RejectedPendingDeltaCommitsSnapshotAloneAndRequestsResync)
{
	const auto baseline = image({atom(1U, {}, {0x10U})});
	const auto snapshot = image({atom(1U, {}, {0x20U})});
	ClientReplicationModel client;
	establish_client_baseline(client, baseline);
	ASSERT_EQ(SnapshotCandidateResult::Known, client.note_snapshot_candidate(2U, 0U, 100U, 10'000'100U));
	auto rate_limiter = limiter();
	ResyncRequestPayload request;
	ASSERT_EQ(ClientDeltaResult::QueuedForCandidate,
		client.receive_delta(delta(2U, 1U, {erase(10U, {1U})}), 101U, 42U, endpoint(), *rate_limiter, request));
	ClientResyncChannel channel{42U, endpoint(), *rate_limiter, request};
	EXPECT_EQ(SnapshotCommitResult::CommittedPendingDeltaRejected,
		client.commit_snapshot(2U, 0U, snapshot, 102U, channel));
	EXPECT_EQ(ClientResyncResult::Requested, channel.disposition);
	EXPECT_EQ(ResyncReason::ValidationFailed, request.reason);
	EXPECT_EQ(2U, request.last_applied_snapshot_id);
	EXPECT_EQ(0U, request.last_applied_delta_sequence);
	EXPECT_EQ(2U, client.active_snapshot_id());
	EXPECT_EQ(0U, client.last_delta_sequence());
	EXPECT_EQ(snapshot, client.published());
	EXPECT_FALSE(client.has_pending_delta());
}

TEST(TelemetryProtocolReplication, CandidateExpiryAtExactDeadlineClearsStateAndRequestsResync)
{
	const auto baseline = image({atom(1U, {}, {0x10U})});
	ClientReplicationModel client;
	establish_client_baseline(client, baseline);
	ASSERT_EQ(SnapshotCandidateResult::Known, client.note_snapshot_candidate(2U, 0U, 100U, 10'000'100U));
	auto rate_limiter = limiter();
	ResyncRequestPayload request;
	ASSERT_EQ(ClientDeltaResult::QueuedForCandidate,
		client.receive_delta(delta(2U, 1U, {upsert(atom(1U, {}, {0x20U}))}),
			101U,
			42U,
			endpoint(),
			*rate_limiter,
			request));
	ClientResyncChannel channel{42U, endpoint(), *rate_limiter, request};
	EXPECT_FALSE(client.expire_snapshot_candidate(10'000'099U, channel));
	EXPECT_EQ(ClientResyncResult::NotRequested, channel.disposition);
	EXPECT_EQ(2U, client.candidate_snapshot_id());
	EXPECT_TRUE(client.has_pending_delta());
	EXPECT_TRUE(client.expire_snapshot_candidate(10'000'100U, channel));
	EXPECT_EQ(ClientResyncResult::Requested, channel.disposition);
	EXPECT_EQ(ResyncReason::ReassemblyTimeout, request.reason);
	EXPECT_EQ(1U, request.last_applied_snapshot_id);
	EXPECT_EQ(0U, request.last_applied_delta_sequence);
	EXPECT_EQ(0U, client.candidate_snapshot_id());
	EXPECT_FALSE(client.has_pending_delta());
	EXPECT_EQ(SnapshotCandidateResult::Stale, client.note_snapshot_candidate(2U, 0U, 10'000'101U, 20'000'101U));
	EXPECT_EQ(SnapshotCandidateResult::Known, client.note_snapshot_candidate(3U, 0U, 10'000'101U, 20'000'101U));
}

TEST(TelemetryProtocolReplication, ExpiredSnapshotThatNeedsANewerManifestRequestsBothDependencies)
{
	const auto baseline = image({atom(1U, {}, {0x10U})});
	ClientReplicationModel client;
	establish_client_baseline(client, baseline, 1U, 5U);
	ASSERT_EQ(SnapshotCandidateResult::Known, client.note_snapshot_candidate(2U, 7U, 0U, 10'000'000U));
	auto rate_limiter = limiter();
	ResyncRequestPayload request;
	ClientResyncChannel channel{42U, endpoint(), *rate_limiter, request};
	ASSERT_TRUE(client.expire_snapshot_candidate(10'000'000U, channel));
	ASSERT_EQ(ClientResyncResult::Requested, channel.disposition);
	EXPECT_EQ(ResyncReason::ReassemblyTimeout, request.reason);
	EXPECT_EQ(KnownResyncRequestFlags, request.request_flags);
	EXPECT_EQ(1U, request.last_applied_snapshot_id);

	ASSERT_EQ(ManifestInstallResult::Installed, client.install_manifest(7U));
	ASSERT_EQ(SnapshotCandidateResult::Known, client.note_snapshot_candidate(3U, 7U, 10'000'001U, 20'000'001U));
	EXPECT_EQ(SnapshotCommitResult::Committed, client.commit_snapshot(3U, 7U, baseline, 10'000'002U, channel));
	EXPECT_EQ(ClientResyncResult::NotRequested, channel.disposition);
	EXPECT_EQ(3U, client.active_snapshot_id());
	EXPECT_EQ(baseline, client.published());
}

TEST(TelemetryProtocolReplication, CommitAtCandidateDeadlineReportsTimeoutAndResyncDisposition)
{
	const auto baseline = image({atom(1U, {}, {0x10U})});
	ClientReplicationModel client;
	establish_client_baseline(client, baseline);
	ASSERT_EQ(SnapshotCandidateResult::Known, client.note_snapshot_candidate(2U, 0U, 0U, 10'000'000U));
	auto rate_limiter = limiter();
	ResyncRequestPayload request;
	ClientResyncChannel channel{42U, endpoint(), *rate_limiter, request};
	EXPECT_EQ(SnapshotCommitResult::Expired, client.commit_snapshot(2U, 0U, baseline, 10'000'000U, channel));
	EXPECT_EQ(ClientResyncResult::Requested, channel.disposition);
	EXPECT_EQ(ResyncReason::ReassemblyTimeout, request.reason);
	EXPECT_EQ(1U, request.last_applied_snapshot_id);
	EXPECT_EQ(0U, request.last_applied_delta_sequence);
	EXPECT_EQ(0U, client.candidate_snapshot_id());
	EXPECT_EQ(baseline, client.published());
}

TEST(TelemetryProtocolReplication, ExpiryObservedByReceiveDeltaRequestsResyncBeforeAnyApply)
{
	const auto baseline = image({atom(1U, {}, {0x10U})});
	ClientReplicationModel client;
	establish_client_baseline(client, baseline);
	ASSERT_EQ(SnapshotCandidateResult::Known, client.note_snapshot_candidate(2U, 0U, 0U, 10'000'000U));
	auto rate_limiter = limiter();
	ResyncRequestPayload request;
	const auto otherwise_valid = delta(1U, 1U, {upsert(atom(1U, {}, {0x20U}))});
	EXPECT_EQ(ClientDeltaResult::ResyncRequested,
		client.receive_delta(otherwise_valid, 10'000'000U, 42U, endpoint(), *rate_limiter, request));
	EXPECT_EQ(ResyncReason::ReassemblyTimeout, request.reason);
	EXPECT_EQ(baseline, client.published());
	EXPECT_EQ(0U, client.last_delta_sequence());
}

TEST(TelemetryProtocolReplication, OversizeCumulativeDifferenceAndUnrepresentableDeleteRequireKeyframe)
{
	std::vector<StateAtom> small_records;
	std::vector<StateAtom> large_records;
	for (std::uint16_t type = 1U; type <= 17U; ++type) {
		small_records.emplace_back(atom(type, {}, {static_cast<std::uint8_t>(type)}));
		large_records.emplace_back(atom(type,
			{},
			std::vector<std::uint8_t>(std::numeric_limits<std::uint16_t>::max(), static_cast<std::uint8_t>(type))));
	}
	ProducerBaselineTracker producer;
	establish_producer_baseline(producer, image(std::move(small_records)));
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.replace_current(image(std::move(large_records))));
	CumulativeStateDelta sentinel = delta(77U, 88U, {upsert(atom(1U, {}, {0x55U}))});
	const auto unchanged = sentinel;
	EXPECT_EQ(ProducerBaselineResult::KeyframeRequired, producer.emit_cumulative_delta(10U, sentinel));
	EXPECT_EQ(unchanged, sentinel);
	EXPECT_EQ(1U, producer.next_delta_sequence());

	ProducerBaselineTracker deletion;
	const auto stable = image({atom(1U, {}, {0x10U})});
	establish_producer_baseline(deletion, stable);
	ASSERT_EQ(ProducerBaselineResult::Applied, deletion.replace_current(image({})));
	EXPECT_EQ(ProducerBaselineResult::KeyframeRequired, deletion.emit_cumulative_delta(1U, sentinel));
}

TEST(TelemetryProtocolReplication, DeltaAcceptsTheExactOneMiBBoundaryAndRejectsOneByteMore)
{
	std::vector<StateAtom> baseline_records;
	std::vector<StateAtom> exact_records;
	for (std::uint16_t type = 1U; type <= 16U; ++type) {
		baseline_records.emplace_back(atom(type, {}, {0U}));
		const auto payload_size = type == 16U ? 65'435U : std::numeric_limits<std::uint16_t>::max();
		exact_records.emplace_back(
			atom(type, {}, std::vector<std::uint8_t>(payload_size, static_cast<std::uint8_t>(type))));
	}
	ProducerBaselineTracker producer;
	establish_producer_baseline(producer, image(std::move(baseline_records)));
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.replace_current(image(exact_records)));
	CumulativeStateDelta exact;
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.emit_cumulative_delta(1U, exact));
	EXPECT_EQ(MaxStateMessageSize, exact.encoded_size());
	EXPECT_EQ(StateDeltaValidationResult::Valid, validate_cumulative_state_delta(exact));

	exact_records.back().value.push_back(0U);
	ASSERT_EQ(ProducerBaselineResult::Applied, producer.replace_current(image(std::move(exact_records))));
	const auto sentinel = exact;
	EXPECT_EQ(ProducerBaselineResult::KeyframeRequired, producer.emit_cumulative_delta(2U, exact));
	EXPECT_EQ(sentinel, exact);
}

TEST(TelemetryProtocolReplication, DeltaAcceptsTheExactU16RecordCountAndRejectsTheNextEntry)
{
	CumulativeStateDelta counted;
	counted.baseline_snapshot_id = 1U;
	counted.delta_sequence = 1U;
	counted.mutations.reserve(std::numeric_limits<std::uint16_t>::max());
	for (std::uint32_t type = 1U; type <= std::numeric_limits<std::uint16_t>::max(); ++type) {
		counted.mutations.emplace_back(upsert(atom(static_cast<std::uint16_t>(type), {}, {})));
	}
	ASSERT_EQ(std::numeric_limits<std::uint16_t>::max(), counted.mutations.size());
	EXPECT_EQ(StateDeltaValidationResult::Valid, validate_cumulative_state_delta(counted));
	EXPECT_EQ(DeltaPayloadPrefixSize + (std::numeric_limits<std::uint16_t>::max() * RecordEnvelopeHeaderSize),
		counted.encoded_size());

	counted.mutations.emplace_back(upsert(atom(1U, {}, {})));
	EXPECT_EQ(StateDeltaValidationResult::InvalidIdentity, validate_cumulative_state_delta(counted));
}

TEST(TelemetryProtocolReplication, StateImageAcceptsExactTransactionSizeAndRejectsOneByteMore)
{
	std::vector<StateAtom> exact_records;
	for (std::uint16_t type = 1U; type <= 256U; ++type) {
		const auto payload_size = type == 256U ? 64'255U : std::numeric_limits<std::uint16_t>::max();
		exact_records.emplace_back(
			atom(type, {}, std::vector<std::uint8_t>(payload_size, static_cast<std::uint8_t>(type))));
	}
	StateImage exact;
	ASSERT_EQ(StateImageResult::Created, StateImage::create(exact_records, exact));
	EXPECT_EQ(MaxTransactionSize, exact.encoded_snapshot_records_size());

	exact_records.back().value.push_back(0U);
	StateImage unchanged = exact;
	EXPECT_EQ(StateImageResult::SizeLimitExceeded, StateImage::create(std::move(exact_records), unchanged));
	EXPECT_EQ(exact, unchanged);
}

TEST(TelemetryProtocolReplication, StateImageNormalizesHostileSpareCapacityBeforeRetention)
{
	std::vector<StateAtom> records;
	records.reserve(4096U);
	records.emplace_back(atom(1U, {}, {0x10U}));
	records[0].key.identity.reserve(4096U);
	records[0].value.reserve(std::numeric_limits<std::uint16_t>::max());
	records[0].cascade_owner.identity.reserve(4096U);
	ASSERT_GT(records.capacity(), records.size());
	ASSERT_GT(records[0].value.capacity(), records[0].value.size());

	StateImage normalized;
	ASSERT_EQ(StateImageResult::Created, StateImage::create(std::move(records), normalized));
	EXPECT_EQ(sizeof(StateAtom) + 1U, normalized.retained_payload_bytes());
	EXPECT_LT(normalized.records().capacity(), 4096U);
	EXPECT_LT(normalized.records()[0].key.identity.capacity(), 4096U);
	EXPECT_LT(normalized.records()[0].value.capacity(), std::numeric_limits<std::uint16_t>::max());
	EXPECT_LT(normalized.records()[0].cascade_owner.identity.capacity(), 4096U);
	EXPECT_EQ((std::vector<std::uint8_t>{0x10U}), normalized.records()[0].value);
}

TEST(TelemetryProtocolReplication, CandidateAndPendingStateAreBoundedAndClearedWithSession)
{
	const auto baseline = image({atom(1U, {}, {0x10U})});
	ClientReplicationModel client;
	establish_client_baseline(client, baseline);
	ASSERT_EQ(SnapshotCandidateResult::Known, client.note_snapshot_candidate(2U, 0U, 2U, 10'000'002U));
	auto rate_limiter = limiter();
	ResyncRequestPayload resync;
	ASSERT_EQ(ClientDeltaResult::QueuedForCandidate,
		client
			.receive_delta(delta(2U, 1U, {upsert(atom(1U, {}, {0x20U}))}), 3U, 42U, endpoint(), *rate_limiter, resync));
	client.clear();
	EXPECT_EQ(0U, client.active_snapshot_id());
	EXPECT_EQ(0U, client.candidate_snapshot_id());
	EXPECT_FALSE(client.has_pending_delta());
	EXPECT_TRUE(client.published().empty());
}

TEST(TelemetryProtocolReplication, RichReplicationResultsMapToStableValidationTaxonomy)
{
	EXPECT_EQ(ValidationError::None, snapshot_commit_validation_error(SnapshotCommitResult::Committed));
	EXPECT_EQ(ValidationError::MissingManifest,
		snapshot_commit_validation_error(SnapshotCommitResult::MissingManifest));
	EXPECT_EQ(ValidationError::ResourceLimit,
		snapshot_commit_validation_error(SnapshotCommitResult::AllocationFailed));
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		snapshot_commit_validation_error(SnapshotCommitResult::UnknownCandidate));

	EXPECT_EQ(ValidationError::None, client_delta_validation_error(ClientDeltaResult::QueuedForCandidate));
	EXPECT_EQ(ValidationError::StaleBaseline,
		client_delta_validation_error(ClientDeltaResult::UnknownBaselineResyncRequested));
	EXPECT_EQ(ValidationError::StaleBaseline,
		client_delta_validation_error(ClientDeltaResult::UnknownBaselineRateLimited));
	EXPECT_EQ(ValidationError::RateLimited,
		client_delta_validation_error(ClientDeltaResult::ResyncRateLimited));
	EXPECT_EQ(ValidationError::ResourceLimit,
		client_delta_validation_error(ClientDeltaResult::AllocationFailed));
}

} // namespace
