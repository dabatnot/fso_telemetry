#include "telemetry/phase1_delta_egress.h"
#include "telemetry/phase1_snapshot_slot.h"
#include "telemetry/phase1_state_image.h"
#include "telemetry/phase2_runtime.h"
#include "telemetry/protocol/telemetry_datagram.h"
#include "telemetry/protocol/telemetry_records.h"
#include "telemetry/protocol/telemetry_replication.h"
#include "telemetry/protocol/telemetry_state_messages.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

namespace detail = telemetry::detail;
namespace protocol = telemetry::protocol;

protocol::Sha256Digest digest(std::uint8_t value)
{
	protocol::Sha256Digest result{};
	result[0] = value;
	return result;
}

protocol::StateAtom atom(std::uint16_t type, std::uint8_t identity,
	std::initializer_list<std::uint8_t> value,
	protocol::StateRecordLifecycle lifecycle =
		protocol::StateRecordLifecycle::UpsertOnly)
{
	protocol::StateAtom result;
	result.key.record_type = type;
	static_cast<void>(identity);
	result.record_version = 1U;
	result.lifecycle = lifecycle;
	result.value.assign(value);
	return result;
}

protocol::StateImage image(std::initializer_list<protocol::StateAtom> atoms)
{
	protocol::StateImage result;
	EXPECT_EQ(protocol::StateImageResult::Created,
		protocol::StateImage::create(
			std::vector<protocol::StateAtom>(atoms), result));
	return result;
}

protocol::StateImage canonical_image(float radius)
{
	detail::Phase1StateImageInput input{};
	input.producer_id = 1U;
	input.negotiated_capability_generation = 1U;
	input.mission.producer_sample_time_us = 100U;
	input.mission.mission_generation = 1U;
	input.mission.phase = protocol::MissionPhase::Active;
	input.mission.time_compression = 1.0F;
	input.player_capture = {
		detail::CaptureStatus::Valid,
		detail::CaptureReason::None};
	input.player.entity_id = 1U;
	input.player.value.producer_sample_time_us = 100U;
	input.player.value.orientation_local_to_world =
		{1.0F, 0.0F, 0.0F, 0.0F};
	input.player.value.radius = radius;
	protocol::StateImage result;
	EXPECT_EQ(detail::Phase1StateImageBuildStatus::Created,
		detail::build_phase1_state_image(input, result));
	return result;
}

protocol::SnapshotCandidatePart part(std::uint32_t id, std::uint32_t crc)
{
	protocol::SnapshotCandidatePart result;
	result.target.message_type = protocol::MessageType::FullSnapshot;
	result.target.message_id = id;
	result.target.fragment_count = 1U;
	result.target.message_crc32 = crc;
	return result;
}

protocol::AckPayload ack(const protocol::SnapshotCandidatePart& candidate,
	std::uint8_t flags = protocol::KnownAckFlags)
{
	protocol::AckPayload result;
	result.target_message_type = candidate.target.message_type;
	result.target_message_id = candidate.target.message_id;
	result.target_fragment_count = candidate.target.fragment_count;
	result.target_message_crc32 = candidate.target.message_crc32;
	result.ack_flags = flags;
	return result;
}

void install_manifest(detail::Phase2RuntimeSlot& slot, std::uint32_t id,
	std::uint8_t fingerprint)
{
	ASSERT_EQ(detail::Phase2RuntimeResult::ManifestRequired,
		slot.stage_manifest(id, digest(fingerprint)));
	ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		slot.on_manifest_applied(id));
}

detail::Phase2RuntimeSnapshotPlan start_plan(detail::Phase2RuntimeSlot& slot)
{
	detail::Phase2RuntimeSnapshotPlan plan;
	EXPECT_EQ(detail::Phase2RuntimeResult::Applied,
		slot.next_snapshot_plan(plan));
	EXPECT_EQ(detail::Phase2RuntimeResult::Applied,
		slot.on_snapshot_started(plan));
	return plan;
}

void apply_plan(detail::Phase2RuntimeSlot& slot,
	const detail::Phase2RuntimeSnapshotPlan& plan)
{
	ASSERT_EQ(detail::Phase2RuntimeResult::Applied,
		slot.on_snapshot_applied(plan.snapshot_id,
			plan.required_manifest_id));
}

TEST(Phase2Replication, TST050SnapshotReasonsUseExactFlagsAndAppliedIsAtomic)
{
	detail::Phase2RuntimeSlot runtime;
	ASSERT_TRUE(runtime.configure(
		telemetry::Phase2Profile::CompleteShip, 0U));
	install_manifest(runtime, 7U, 0x71U);

	auto initial = start_plan(runtime);
	EXPECT_EQ(protocol::SnapshotFlagInitial, initial.flags);
	EXPECT_EQ(detail::Phase2RuntimeSnapshotCause::Initial, initial.cause);

	detail::Phase1SnapshotSlot baseline;
	const auto captured = image({atom(1U, 0U, {0x11U})});
	const std::vector<protocol::SnapshotCandidatePart> parts{
		part(10U, 0x10101010U), part(11U, 0x11111111U)};
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		baseline.start_initial_candidate(
			initial.snapshot_id, captured, parts, 100U));
	EXPECT_EQ(protocol::ProducerBaselineResult::NoChange,
		baseline.acknowledge_candidate_part(
			ack(parts[0], static_cast<std::uint8_t>(
				protocol::AckFlag::Validated)), 101U));
	EXPECT_FALSE(baseline.has_active_baseline());
	EXPECT_EQ(protocol::ProducerBaselineResult::Applied,
		baseline.acknowledge_candidate_part(ack(parts[0]), 102U));
	EXPECT_FALSE(baseline.has_active_baseline());
	EXPECT_EQ(protocol::ProducerBaselineResult::Applied,
		baseline.acknowledge_candidate_part(ack(parts[1]), 103U));
	EXPECT_TRUE(baseline.has_active_baseline());
	apply_plan(runtime, initial);

	const std::array<std::pair<detail::Phase2RuntimeSnapshotCause,
			std::uint16_t>, 7U>
		cases{{
			{detail::Phase2RuntimeSnapshotCause::Periodic,
				protocol::SnapshotFlagPeriodicKeyframe},
			{detail::Phase2RuntimeSnapshotCause::Topology,
				protocol::SnapshotFlagPeriodicKeyframe},
			{detail::Phase2RuntimeSnapshotCause::Catalog,
				protocol::SnapshotFlagPeriodicKeyframe},
			{detail::Phase2RuntimeSnapshotCause::Lifecycle,
				protocol::SnapshotFlagPeriodicKeyframe},
			{detail::Phase2RuntimeSnapshotCause::SupportTerminal,
				protocol::SnapshotFlagPeriodicKeyframe},
			{detail::Phase2RuntimeSnapshotCause::DeltaCapacity,
				protocol::SnapshotFlagPeriodicKeyframe},
			{detail::Phase2RuntimeSnapshotCause::Resync,
				protocol::SnapshotFlagResync},
		}};
	for (const auto& test_case : cases) {
		ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
			runtime.request_snapshot(test_case.first));
		auto plan = start_plan(runtime);
		EXPECT_EQ(test_case.second, plan.flags);
		EXPECT_EQ(test_case.first, plan.cause);
		apply_plan(runtime, plan);
	}
}

TEST(Phase2Replication, TST051CandidateIsImmutableAndMembershipDefersToKeyframe)
{
	protocol::ProducerBaselineTracker tracker;
	const auto base = image({
		atom(1U, 0U, {1U, 2U}),
		atom(2U, 0U, {2U, 3U})});
	const std::vector<protocol::SnapshotCandidatePart> first{
		part(20U, 0x20202020U)};
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.initialize(base));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.capture_snapshot(1U, 1U, base, first, 100U));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.acknowledge_snapshot_part(ack(first[0]), 101U));

	const auto captured = image({
		atom(1U, 0U, {9U, 2U}),
		atom(2U, 0U, {2U, 3U})});
	const std::vector<protocol::SnapshotCandidatePart> replacement{
		part(21U, 0x21212121U)};
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.replace_current(captured));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.capture_snapshot(2U, 1U, captured, replacement, 102U));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.replace_current(image({
			atom(1U, 0U, {10U, 2U}),
			atom(3U, 0U, {3U, 4U})})));
	EXPECT_EQ(1U, tracker.active_snapshot_id());
	EXPECT_EQ(2U, tracker.candidate_snapshot_id());
	EXPECT_EQ(3U, tracker.candidate_dirty_record_count())
		<< "One value mutation plus one removal and one addition are "
		   "retained relative to the immutable candidate.";

	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.acknowledge_snapshot_part(
			ack(replacement[0]), 103U));
	EXPECT_EQ(2U, tracker.active_snapshot_id());
	protocol::CumulativeStateDelta delta;
	EXPECT_EQ(protocol::ProducerBaselineResult::KeyframeRequired,
		tracker.emit_cumulative_delta(104U, delta));
	EXPECT_EQ(0U, delta.mutation_count());
}

TEST(Phase2Replication, TST052LostIntermediateDeltaStillConverges)
{
	protocol::ProducerBaselineTracker tracker;
	const auto base = image({atom(1U, 0U, {1U, 2U})});
	const auto first = std::vector<protocol::SnapshotCandidatePart>{
		part(30U, 0x30303030U)};
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.initialize(base));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.capture_snapshot(1U, 1U, base, first, 100U));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.acknowledge_snapshot_part(ack(first[0]), 101U));

	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.replace_current(image({atom(1U, 0U, {2U, 2U})})));
	protocol::CumulativeStateDelta lost;
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.emit_cumulative_delta(102U, lost));
	const auto expected =
		image({atom(1U, 0U, {3U, 4U, 5U})});
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.replace_current(expected));
	protocol::CumulativeStateDelta latest;
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.emit_cumulative_delta(103U, latest));

	protocol::StateImage applied;
	ASSERT_EQ(protocol::StateDeltaApplyResult::Applied,
		protocol::apply_cumulative_state_delta(
			base, latest, nullptr, applied));
	EXPECT_EQ(expected, applied);
	EXPECT_EQ(lost.baseline_snapshot_id, latest.baseline_snapshot_id);
	EXPECT_LT(lost.delta_sequence, latest.delta_sequence);
}

TEST(Phase2Replication, TST053ExactBaselineReturnRemovesTheMutation)
{
	protocol::ProducerBaselineTracker tracker;
	const auto base = image({
		atom(1U, 0U, {1U}), atom(2U, 0U, {2U})});
	const auto parts = std::vector<protocol::SnapshotCandidatePart>{
		part(40U, 0x40404040U)};
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.initialize(base));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.capture_snapshot(1U, 1U, base, parts, 100U));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.acknowledge_snapshot_part(ack(parts[0]), 101U));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.replace_current(image({
			atom(1U, 0U, {9U}), atom(2U, 0U, {3U})})));
	protocol::CumulativeStateDelta first_delta;
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.emit_cumulative_delta(102U, first_delta));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.replace_current(image({
			atom(1U, 0U, {1U}), atom(2U, 0U, {3U})})));
	protocol::CumulativeStateDelta returned;
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.emit_cumulative_delta(103U, returned));
	ASSERT_EQ(1U, returned.mutation_count());
	EXPECT_EQ(2U, returned.mutations[0].atom.key.record_type);
	protocol::StateImage applied;
	ASSERT_EQ(protocol::StateDeltaApplyResult::Applied,
		protocol::apply_cumulative_state_delta(
			base, returned, nullptr, applied));
	EXPECT_EQ(tracker.current(), applied);
}

TEST(Phase2Replication,
	IncrementalDirtyIndicesRemainCumulativeAgainstTheImmutableBaseline)
{
	protocol::ProducerBaselineTracker tracker;
	const auto base = image({
		atom(1U, 0U, {1U}), atom(2U, 0U, {2U}),
		atom(3U, 0U, {3U}), atom(4U, 0U, {4U})});
	const std::vector<protocol::SnapshotCandidatePart> parts{
		part(41U, 0x41414141U)};
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.initialize(base));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.capture_snapshot(1U, 1U, base, parts, 100U));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.acknowledge_snapshot_part(ack(parts[0]), 101U));

	const std::array<std::uint16_t, 2U> first_dirty{{1U, 3U}};
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.replace_current_incremental(
			image({
				atom(1U, 0U, {1U}), atom(2U, 0U, {20U}),
				atom(3U, 0U, {3U}), atom(4U, 0U, {40U})}),
			first_dirty.data(), first_dirty.size()));
	EXPECT_TRUE(tracker.incremental_record_set_compatible());
	EXPECT_EQ(2U, tracker.active_dirty_record_count());
	protocol::CumulativeStateDelta first;
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.emit_cumulative_delta(102U, first));
	ASSERT_EQ(2U, first.mutation_count());
	EXPECT_EQ(2U, first.mutations[0].atom.key.record_type);
	EXPECT_EQ(4U, first.mutations[1].atom.key.record_type);

	const std::array<std::uint16_t, 1U> second_dirty{{1U}};
	const auto expected = image({
		atom(1U, 0U, {1U}), atom(2U, 0U, {2U}),
		atom(3U, 0U, {3U}), atom(4U, 0U, {40U})});
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.replace_current_incremental(
			expected, second_dirty.data(), second_dirty.size()));
	EXPECT_EQ(1U, tracker.active_dirty_record_count());
	protocol::CumulativeStateDelta latest;
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.emit_cumulative_delta(103U, latest));
	ASSERT_EQ(1U, latest.mutation_count());
	EXPECT_EQ(4U, latest.mutations[0].atom.key.record_type);
	protocol::StateImage applied;
	ASSERT_EQ(protocol::StateDeltaApplyResult::Applied,
		protocol::apply_cumulative_state_delta(
			base, latest, nullptr, applied));
	EXPECT_EQ(expected, applied);
}

TEST(Phase2Replication,
	IncrementalDirtyIndicesTrackChangesWhileCandidateAwaitsApplied)
{
	protocol::ProducerBaselineTracker tracker;
	const auto base = image({
		atom(1U, 0U, {1U}), atom(2U, 0U, {2U})});
	const std::vector<protocol::SnapshotCandidatePart> initial{
		part(42U, 0x42424242U)};
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.initialize(base));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.capture_snapshot(1U, 1U, base, initial, 100U));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.acknowledge_snapshot_part(
			ack(initial[0]), 101U));

	const auto captured = image({
		atom(1U, 0U, {10U}), atom(2U, 0U, {2U})});
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.replace_current(captured));
	const std::vector<protocol::SnapshotCandidatePart> replacement{
		part(43U, 0x43434343U)};
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.capture_snapshot(
			2U, 1U, captured, replacement, 102U));
	const std::array<std::uint16_t, 1U> dirty{{1U}};
	const auto current = image({
		atom(1U, 0U, {10U}), atom(2U, 0U, {22U})});
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.replace_current_incremental(
			current, dirty.data(), dirty.size()));
	EXPECT_EQ(1U, tracker.candidate_dirty_record_count());
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.acknowledge_snapshot_part(
			ack(replacement[0]), 103U));
	EXPECT_TRUE(tracker.incremental_record_set_compatible());
	protocol::CumulativeStateDelta delta;
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.emit_cumulative_delta(104U, delta));
	ASSERT_EQ(1U, delta.mutation_count());
	EXPECT_EQ(2U, delta.mutations[0].atom.key.record_type);
}

TEST(Phase2Replication, TST054ListAndScalarChangesReplaceWholeAtomsWithoutPartial)
{
	const auto base = image({
		atom(1001U, 0U, {1U, 2U, 3U}),
		atom(1002U, 0U, {4U, 5U}),
		atom(1003U, 0U, {6U, 7U, 8U}),
		atom(1004U, 0U, {9U, 10U})});
	const auto current = image({
		atom(1001U, 0U, {1U, 22U, 3U}),
		atom(1002U, 0U, {4U, 55U, 56U}),
		atom(1003U, 0U, {66U, 7U, 8U}),
		atom(1004U, 0U, {9U, 100U})});
	protocol::ProducerBaselineTracker tracker;
	const auto parts = std::vector<protocol::SnapshotCandidatePart>{
		part(50U, 0x50505050U)};
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.initialize(base));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.capture_snapshot(1U, 1U, base, parts, 100U));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.acknowledge_snapshot_part(ack(parts[0]), 101U));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.replace_current(current));
	protocol::CumulativeStateDelta delta;
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.emit_cumulative_delta(102U, delta));
	ASSERT_EQ(current.records().size(), delta.mutation_count());
	for (std::size_t index = 0U; index < delta.mutation_count();
			++index) {
		EXPECT_EQ(protocol::StateMutationKind::Upsert,
			delta.mutations[index].kind);
		EXPECT_EQ(current.records()[index].value,
			delta.mutations[index].atom.value);
	}

	protocol::ProducerBaselineTracker wire_tracker;
	const auto wire_base = canonical_image(1.0F);
	const auto wire_parts =
		std::vector<protocol::SnapshotCandidatePart>{
			part(51U, 0x51515151U)};
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		wire_tracker.initialize(wire_base));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		wire_tracker.capture_snapshot(
			1U, 1U, wire_base, wire_parts, 100U));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		wire_tracker.acknowledge_snapshot_part(
			ack(wire_parts[0]), 101U));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		wire_tracker.replace_current(canonical_image(2.0F)));
	protocol::CumulativeStateDelta wire_delta;
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		wire_tracker.emit_cumulative_delta(102U, wire_delta));

	detail::Phase1DeltaEgress egress;
	ASSERT_TRUE(egress.provision(protocol::MaxStateMessageSize));
	const auto endpoint =
		protocol::EndpointKey::from_ipv4({127U, 0U, 0U, 1U}, 4242U);
	ASSERT_EQ(detail::Phase1DeltaReplaceResult::Replaced,
		egress.replace_checked(1U, endpoint, 1U, wire_delta));
	ASSERT_TRUE(egress.service(1U, 103U));
	detail::Phase1DeltaDatagram datagram;
	ASSERT_TRUE(egress.peek_output(datagram));
	protocol::DatagramView decoded;
	ASSERT_EQ(protocol::ValidationError::None,
		protocol::decode_and_validate_datagram(
			{datagram.bytes.data(), datagram.size},
			{protocol::VersionMinor,
				protocol::VersionMinor}, decoded));
	protocol::DeltaPayload payload;
	ASSERT_EQ(protocol::ValidationError::None,
		protocol::decode_delta_payload(decoded.payload, payload));
	protocol::RecordEnvelopeIterator records(payload.records,
		payload.record_count,
		protocol::RecordFlagPolicy::AllowV1Mutations);
	for (;;) {
		protocol::RecordEnvelopeView record;
		bool present = false;
		ASSERT_EQ(protocol::ValidationError::None,
			records.next(record, present));
		if (!present)
			break;
		EXPECT_EQ(protocol::RecordFlagNone, record.record_flags);
	}
}

TEST(Phase2Replication,
	IncrementalDeltaPayloadCacheMatchesFullEncoding)
{
	protocol::ProducerBaselineTracker tracker;
	const auto baseline = canonical_image(1.0F);
	const auto parts =
		std::vector<protocol::SnapshotCandidatePart>{
			part(52U, 0x52525252U)};
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.initialize(baseline));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.capture_snapshot(
			1U, 1U, baseline, parts, 100U));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.acknowledge_snapshot_part(
			ack(parts[0]), 101U));

	const auto first_current = canonical_image(2.0F);
	std::vector<std::uint16_t> dirty_indices;
	for (std::size_t index = 0U;
		 index < first_current.records().size(); ++index) {
		dirty_indices.push_back(
			static_cast<std::uint16_t>(index));
	}
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.replace_current_incremental(first_current,
			dirty_indices.data(), dirty_indices.size()));

	protocol::CumulativeStateDelta delta;
	protocol::DeltaBuildChanges changes;
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.emit_cumulative_delta(
			102U, delta, &changes));
	EXPECT_TRUE(changes.layout_changed);

	detail::Phase1DeltaEgress cached;
	ASSERT_TRUE(cached.provision(protocol::MaxStateMessageSize));
	const auto endpoint =
		protocol::EndpointKey::from_ipv4(
			{127U, 0U, 0U, 1U}, 4343U);
	ASSERT_EQ(detail::Phase1DeltaReplaceResult::Replaced,
		cached.replace_prevalidated_checked(
			1U, endpoint, 1U, delta, changes));
	ASSERT_TRUE(cached.service(1U, 103U));
	cached.complete_output();
	ASSERT_FALSE(cached.has_delta());

	const auto second_current = canonical_image(3.0F);
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.replace_current_incremental(second_current,
			dirty_indices.data(), dirty_indices.size()));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.emit_cumulative_delta(
			104U, delta, &changes));
	ASSERT_FALSE(changes.layout_changed);
	ASSERT_GT(changes.count, 0U);

	detail::Phase1DeltaEgress reference;
	ASSERT_TRUE(reference.provision(
		protocol::MaxStateMessageSize));
	ASSERT_EQ(detail::Phase1DeltaReplaceResult::Replaced,
		cached.replace_prevalidated_checked(
			1U, endpoint, 2U, delta, changes));
	ASSERT_EQ(detail::Phase1DeltaReplaceResult::Replaced,
		reference.replace_checked(
			1U, endpoint, 2U, delta));
	ASSERT_TRUE(cached.service(2U, 105U));
	ASSERT_TRUE(reference.service(2U, 105U));

	detail::Phase1DeltaDatagram cached_datagram;
	detail::Phase1DeltaDatagram reference_datagram;
	ASSERT_TRUE(cached.peek_output(cached_datagram));
	ASSERT_TRUE(reference.peek_output(reference_datagram));
	ASSERT_EQ(reference_datagram.size, cached_datagram.size);
	EXPECT_TRUE(std::equal(
		reference_datagram.bytes.begin(),
		reference_datagram.bytes.begin() +
			static_cast<std::ptrdiff_t>(
				reference_datagram.size),
		cached_datagram.bytes.begin()));
}

TEST(Phase2Replication, TST055AckIdentityIsExactAndDuplicatesCannotRegress)
{
	protocol::ProducerBaselineTracker tracker;
	const auto baseline = image({atom(1U, 0U, {1U})});
	const std::vector<protocol::SnapshotCandidatePart> parts{
		part(60U, 0x60606060U), part(61U, 0x61616161U)};
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.initialize(baseline));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.capture_snapshot(1U, 1U, baseline, parts, 100U));
	auto future = ack(parts[0]);
	++future.target_message_id;
	EXPECT_EQ(protocol::ProducerBaselineResult::InvalidAck,
		tracker.acknowledge_snapshot_part(future, 101U));
	EXPECT_EQ(1U, tracker.candidate_snapshot_id());
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.acknowledge_snapshot_part(ack(parts[0]), 102U));
	EXPECT_EQ(protocol::ProducerBaselineResult::NoChange,
		tracker.acknowledge_snapshot_part(ack(parts[0]), 103U));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.acknowledge_snapshot_part(ack(parts[1]), 104U));
	EXPECT_EQ(1U, tracker.active_snapshot_id());
	EXPECT_EQ(protocol::ProducerBaselineResult::UnknownPart,
		tracker.acknowledge_snapshot_part(ack(parts[1]), 105U));
	EXPECT_EQ(1U, tracker.active_snapshot_id());
}

TEST(Phase2Replication, TST056UnknownAndStaleBaselinesNeverPublish)
{
	protocol::ClientReplicationModel client;
	ASSERT_EQ(protocol::ManifestInstallResult::Installed,
		client.install_manifest(1U));
	ASSERT_EQ(protocol::SnapshotCandidateResult::Known,
		client.note_snapshot_candidate(1U, 1U, 100U,
			100U + static_cast<std::uint64_t>(
				protocol::TransactionAssemblyTimeoutMs) * 1'000U));
	protocol::ProtocolRateLimiter limiter;
	ASSERT_EQ(protocol::ValidationError::None,
		protocol::ProtocolRateLimiter::configure({}, 0U, limiter));
	protocol::ResyncRequestPayload request{};
	const auto endpoint =
		protocol::EndpointKey::from_ipv4({127U, 0U, 0U, 1U}, 4242U);
	protocol::ClientResyncChannel channel{
		1U, endpoint, limiter, request};
	const auto baseline = image({atom(1U, 0U, {1U})});
	ASSERT_EQ(protocol::SnapshotCommitResult::Committed,
		client.commit_snapshot(1U, 1U, baseline, 101U, channel));
	ASSERT_EQ(protocol::SnapshotCandidateResult::Known,
		client.note_snapshot_candidate(2U, 1U, 102U,
			102U + static_cast<std::uint64_t>(
				protocol::TransactionAssemblyTimeoutMs) * 1'000U));
	ASSERT_EQ(protocol::SnapshotCommitResult::Committed,
		client.commit_snapshot(2U, 1U, baseline, 103U, channel));
	const auto sentinel = client.published();

	protocol::CumulativeStateDelta unknown;
	unknown.baseline_snapshot_id = 99U;
	unknown.delta_sequence = 1U;
	unknown.producer_sample_time_us = 102U;
	unknown.mutations.push_back(
		{protocol::StateMutationKind::Upsert,
			atom(1U, 0U, {9U})});
	EXPECT_EQ(protocol::ClientDeltaResult::UnknownBaselineResyncRequested,
		client.receive_delta(unknown, 102U, 1U,
			endpoint, limiter, request));
	EXPECT_EQ(sentinel, client.published());
	bool rate_limited = false;
	for (std::uint64_t now = 103U; now < 120U; ++now) {
		const auto repeated = client.receive_delta(
			unknown, now, 1U, endpoint, limiter, request);
		if (repeated ==
			protocol::ClientDeltaResult::
				UnknownBaselineRateLimited) {
			rate_limited = true;
			break;
		}
		ASSERT_EQ(protocol::ClientDeltaResult::
				UnknownBaselineResyncRequested,
			repeated);
		EXPECT_EQ(sentinel, client.published());
	}
	EXPECT_TRUE(rate_limited);
	EXPECT_EQ(sentinel, client.published());

	auto stale = unknown;
	stale.baseline_snapshot_id = 1U;
	EXPECT_EQ(protocol::ClientDeltaResult::IgnoredOldBaseline,
		client.receive_delta(stale, 104U, 1U,
			endpoint, limiter, request));
	EXPECT_EQ(sentinel, client.published());
}

TEST(Phase2Replication, TST057CapacityFallbackKeepsTheActiveBaseline)
{
	detail::Phase2RuntimeSlot runtime;
	ASSERT_TRUE(runtime.configure(
		telemetry::Phase2Profile::CompleteShip, 0U));
	install_manifest(runtime, 1U, 0x11U);
	auto initial = start_plan(runtime);
	apply_plan(runtime, initial);
	EXPECT_TRUE(runtime.can_emit_delta());
	EXPECT_EQ(detail::Phase2RuntimeResult::Applied,
		runtime.classify_delta_size(protocol::MaxStateMessageSize));
	EXPECT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		runtime.classify_delta_size(
			protocol::MaxStateMessageSize + 1U));
	EXPECT_EQ(initial.snapshot_id, runtime.active_snapshot_id());
	EXPECT_FALSE(runtime.can_emit_delta());
	detail::Phase2RuntimeSnapshotPlan fallback;
	ASSERT_EQ(detail::Phase2RuntimeResult::Applied,
		runtime.next_snapshot_plan(fallback));
	EXPECT_EQ(detail::Phase2RuntimeSnapshotCause::DeltaCapacity,
		fallback.cause);
	EXPECT_EQ(protocol::SnapshotFlagPeriodicKeyframe,
		fallback.flags);
}

TEST(Phase2Replication,
	IncrementalMetadataMatchesExhaustiveAdoptionAndRejectsInvalidMappings)
{
	auto backing =
		std::make_shared<std::vector<protocol::StateAtom>>(
			std::initializer_list<protocol::StateAtom>{
				atom(1U, 0U, {1U, 2U}),
				atom(2U, 0U, {3U})});
	protocol::StateImage incremental;
	protocol::StateImageInvalidRecordReason reason =
		protocol::StateImageInvalidRecordReason::None;
	ASSERT_EQ(protocol::StateImageResult::Created,
		protocol::StateImage::adopt_preallocated(
			backing, incremental, reason));
	const auto previous = *backing;
	auto* mutable_records =
		incremental.mutable_preallocated_records_if_unique();
	ASSERT_NE(nullptr, mutable_records);
	(*mutable_records)[1].value = {3U, 4U, 5U, 6U};
	const std::array<std::uint16_t, 1U> canonical_indices{{1U}};
	const std::array<std::uint16_t, 1U> previous_indices{{1U}};
	ASSERT_EQ(protocol::StateImageResult::Created,
		incremental.refresh_preallocated_metadata_incremental(
			previous, canonical_indices.data(),
			previous_indices.data(), canonical_indices.size(),
			reason));

	protocol::StateImage exhaustive;
	ASSERT_EQ(protocol::StateImageResult::Created,
		protocol::StateImage::adopt_preallocated(
			backing, exhaustive, reason));
	EXPECT_EQ(exhaustive.encoded_snapshot_records_size(),
		incremental.encoded_snapshot_records_size());
	EXPECT_EQ(exhaustive.retained_payload_bytes(),
		incremental.retained_payload_bytes());

	const auto encoded_before =
		incremental.encoded_snapshot_records_size();
	const auto retained_before =
		incremental.retained_payload_bytes();
	const std::array<std::uint16_t, 2U> duplicate_indices{{1U, 1U}};
	const std::array<std::uint16_t, 2U> duplicate_previous{{1U, 1U}};
	EXPECT_EQ(protocol::StateImageResult::InvalidRecord,
		incremental.refresh_preallocated_metadata_incremental(
			previous, duplicate_indices.data(),
			duplicate_previous.data(), duplicate_indices.size(),
			reason));
	EXPECT_EQ(encoded_before,
		incremental.encoded_snapshot_records_size());
	EXPECT_EQ(retained_before,
		incremental.retained_payload_bytes());
}

} // namespace
