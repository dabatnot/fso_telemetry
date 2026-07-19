#include "telemetry/protocol/telemetry_replication.h"
#include "telemetry/protocol/telemetry_reliable_window.h"
#include "telemetry/phase1_state_image.h"

#if __has_include("telemetry/phase1_snapshot_slot.h")
#include "telemetry/phase1_snapshot_slot.h"
#else
namespace telemetry::detail {

enum class Phase1SnapshotProgress : std::uint8_t { Synchronizing = 0, Live };

class Phase1SnapshotSlot final {
  public:
	protocol::ProducerBaselineResult start_initial_candidate(std::uint32_t,
		const protocol::StateImage&,
		const std::vector<protocol::SnapshotCandidatePart>&,
		std::uint64_t) noexcept
	{
		return protocol::ProducerBaselineResult::InvalidArgument;
	}
	protocol::ProducerBaselineResult acknowledge_candidate_part(
		const protocol::AckPayload&, std::uint64_t) noexcept
	{
		return protocol::ProducerBaselineResult::InvalidArgument;
	}
	void rollback_candidate() noexcept {}
	bool has_candidate() const noexcept { return false; }
	bool has_active_baseline() const noexcept { return false; }
	std::uint32_t candidate_snapshot_id() const noexcept { return 0U; }
	std::uint32_t active_snapshot_id() const noexcept { return 0U; }
	protocol::RequiredAckLevel candidate_required_ack() const noexcept
	{
		return protocol::RequiredAckLevel::None;
	}
	protocol::ReliableMessageClass candidate_message_class() const noexcept
	{
		return protocol::ReliableMessageClass::ControlDrop;
	}
	Phase1SnapshotProgress progress() const noexcept { return Phase1SnapshotProgress::Synchronizing; }
	bool session_state_dirty() const noexcept { return false; }
	bool consume_session_state_dirty() noexcept { return false; }
};

} // namespace telemetry::detail
#endif

#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace detail = telemetry::detail;
namespace protocol = telemetry::protocol;

// SESSION_BEGIN advertises snapshot id zero; this per-slot candidate is the
// deliberately separate transaction announced only after that handshake.

protocol::StateImage image(std::uint8_t value)
{
	protocol::StateAtom atom;
	atom.key.record_type = 1U;
	atom.value = {value};
	protocol::StateImage result;
	EXPECT_EQ(protocol::StateImageResult::Created,
		protocol::StateImage::create(std::vector<protocol::StateAtom>{std::move(atom)}, result));
	return result;
}

protocol::SnapshotCandidatePart part(std::uint32_t message_id, std::uint32_t crc)
{
	protocol::SnapshotCandidatePart result;
	result.target.message_type = protocol::MessageType::FullSnapshot;
	result.target.message_id = message_id;
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

template <typename Slot, typename = void>
struct has_p8_3_delta_seams : std::false_type {
};

template <typename Slot>
struct has_p8_3_delta_seams<Slot,
	std::void_t<decltype(std::declval<Slot&>().replace_current(std::declval<const protocol::StateImage&>())),
		decltype(std::declval<Slot&>().emit_cumulative_delta(std::declval<std::uint64_t>(),
			std::declval<protocol::CumulativeStateDelta&>())),
		decltype(std::declval<Slot&>().start_replacement_candidate(std::declval<std::uint32_t>(),
			std::declval<const protocol::StateImage&>(), std::declval<const std::vector<protocol::SnapshotCandidatePart>&>(),
			std::declval<std::uint64_t>()))>> : std::true_type {
};

template <typename Slot, typename = void>
struct has_p8_3_keyframe_intent_seams : std::false_type {
};

template <typename Slot>
struct has_p8_3_keyframe_intent_seams<Slot,
	std::void_t<decltype(std::declval<const Slot&>().keyframe_intent()),
		decltype(std::declval<Slot&>().consume_keyframe_intent())>> : std::true_type {
};

protocol::StateImage image_pair(std::uint8_t first, std::uint8_t second)
{
	protocol::StateAtom left;
	left.key.record_type = 1U;
	left.value = {first};
	protocol::StateAtom right;
	right.key.record_type = 2U;
	right.value = {second};
	protocol::StateImage result;
	EXPECT_EQ(protocol::StateImageResult::Created,
		protocol::StateImage::create(std::vector<protocol::StateAtom>{std::move(left), std::move(right)}, result));
	return result;
}

protocol::StateImage canonical_player_image(std::uint64_t entity_id)
{
	detail::Phase1StateImageInput input{};
	input.producer_id = 1U;
	input.negotiated_capability_generation = 1U;
	input.mission.producer_sample_time_us = 1'000U;
	input.mission.mission_generation = 1U;
	input.mission.phase = protocol::MissionPhase::Active;
	input.mission.time_compression = 1.0F;
	input.player_capture = {detail::CaptureStatus::Valid, detail::CaptureReason::None};
	input.player.entity_id = entity_id;
	input.player.value.producer_sample_time_us = 1'000U;
	input.player.value.orientation_local_to_world = {1.0F, 0.0F, 0.0F, 0.0F};
	input.player.value.radius = 1.0F;
	protocol::StateImage result;
	EXPECT_EQ(detail::Phase1StateImageBuildStatus::Created, detail::build_phase1_state_image(input, result));
	return result;
}

protocol::StateImage canonical_invalid_player_image()
{
	detail::Phase1StateImageInput input{};
	input.producer_id = 1U;
	input.negotiated_capability_generation = 1U;
	input.mission.producer_sample_time_us = 1'000U;
	input.mission.mission_generation = 1U;
	input.mission.phase = protocol::MissionPhase::Active;
	input.mission.time_compression = 1.0F;
	input.player_capture = {detail::CaptureStatus::InvalidSource, detail::CaptureReason::WrongObjectType};
	protocol::StateImage result;
	EXPECT_EQ(detail::Phase1StateImageBuildStatus::Created, detail::build_phase1_state_image(input, result));
	return result;
}

TEST(TelemetryPhase1SnapshotSlotContract, InitialCandidateIsASeparateReliableFullSnapshotTransaction)
{
	detail::Phase1SnapshotSlot slot;
	const auto captured = image(0x11U);
	const std::vector<protocol::SnapshotCandidatePart> parts{part(41U, 0x11111111U), part(42U, 0x22222222U)};

	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		slot.start_initial_candidate(1U, captured, parts, 1'000U));
	EXPECT_TRUE(slot.has_candidate());
	EXPECT_EQ(1U, slot.candidate_snapshot_id());
	EXPECT_EQ(protocol::MessageType::FullSnapshot, parts[0].target.message_type);
	EXPECT_EQ(protocol::MessageType::FullSnapshot, parts[1].target.message_type);
	EXPECT_EQ(protocol::RequiredAckLevel::Applied, slot.candidate_required_ack());
	EXPECT_EQ(protocol::ReliableMessageClass::Transaction, slot.candidate_message_class());
	EXPECT_FALSE(slot.has_active_baseline());
	EXPECT_EQ(detail::Phase1SnapshotProgress::Synchronizing, slot.progress());
	EXPECT_FALSE(slot.session_state_dirty());
}

TEST(TelemetryPhase1SnapshotSlotContract, ValidatedAndPartialAppliedAcksNeverPublishTheCandidate)
{
	detail::Phase1SnapshotSlot slot;
	const auto captured = image(0x22U);
	const std::vector<protocol::SnapshotCandidatePart> parts{part(51U, 0x33333333U), part(52U, 0x44444444U)};
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		slot.start_initial_candidate(7U, captured, parts, 2'000U));

	EXPECT_EQ(protocol::ProducerBaselineResult::NoChange,
		slot.acknowledge_candidate_part(
			ack(parts[0], static_cast<std::uint8_t>(protocol::AckFlag::Validated)), 2'001U));
	EXPECT_EQ(protocol::ProducerBaselineResult::Applied,
		slot.acknowledge_candidate_part(ack(parts[1]), 2'002U));
	EXPECT_TRUE(slot.has_candidate());
	EXPECT_FALSE(slot.has_active_baseline());
	EXPECT_EQ(detail::Phase1SnapshotProgress::Synchronizing, slot.progress());
	EXPECT_FALSE(slot.session_state_dirty());
}

TEST(TelemetryPhase1SnapshotSlotContract, FinalAppliedAckCommitsOnceAndRaisesOneBoundedSessionStateIntent)
{
	detail::Phase1SnapshotSlot slot;
	const auto captured = image(0x33U);
	const std::vector<protocol::SnapshotCandidatePart> parts{part(61U, 0x55555555U), part(62U, 0x66666666U)};
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		slot.start_initial_candidate(9U, captured, parts, 3'000U));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		slot.acknowledge_candidate_part(ack(parts[0]), 3'001U));
	ASSERT_FALSE(slot.has_active_baseline());

	EXPECT_EQ(protocol::ProducerBaselineResult::Applied,
		slot.acknowledge_candidate_part(ack(parts[1]), 3'002U));
	EXPECT_FALSE(slot.has_candidate());
	EXPECT_TRUE(slot.has_active_baseline());
	EXPECT_EQ(9U, slot.active_snapshot_id());
	EXPECT_EQ(detail::Phase1SnapshotProgress::Live, slot.progress());
	EXPECT_TRUE(slot.session_state_dirty());
	EXPECT_TRUE(slot.consume_session_state_dirty());
	EXPECT_FALSE(slot.consume_session_state_dirty()) << "The P8.3 intent is a bounded one-bit latch, not a queue.";
}

TEST(TelemetryPhase1SnapshotSlotContract, RollbackReleasesCandidateWithoutInstallingABaseline)
{
	detail::Phase1SnapshotSlot slot;
	const auto captured = image(0x44U);
	const std::vector<protocol::SnapshotCandidatePart> parts{part(71U, 0x77777777U)};
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		slot.start_initial_candidate(11U, captured, parts, 4'000U));

	slot.rollback_candidate();
	EXPECT_FALSE(slot.has_candidate());
	EXPECT_FALSE(slot.has_active_baseline());
	EXPECT_EQ(0U, slot.candidate_snapshot_id());
	EXPECT_EQ(0U, slot.active_snapshot_id());
	EXPECT_EQ(detail::Phase1SnapshotProgress::Synchronizing, slot.progress());
	EXPECT_FALSE(slot.session_state_dirty());
	EXPECT_FALSE(slot.consume_session_state_dirty());
}

TEST(TelemetryPhase1DeltaContract, NoDeltaExistsBeforeAnAppliedBaselineAndEachEmissionIsCumulative)
{
	detail::Phase1SnapshotSlot slot;
	const std::vector<protocol::SnapshotCandidatePart> parts{part(81U, 0x81818181U)};
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied, slot.start_initial_candidate(1U, image(0x11U), parts, 1'000U));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied, slot.replace_current(image(0x12U)));
	protocol::CumulativeStateDelta delta;
	EXPECT_EQ(protocol::ProducerBaselineResult::NoActiveBaseline, slot.emit_cumulative_delta(1'001U, delta));
	EXPECT_FALSE(slot.has_active_baseline());

	ASSERT_EQ(protocol::ProducerBaselineResult::Applied, slot.acknowledge_candidate_part(ack(parts[0]), 1'002U));
	ASSERT_TRUE(slot.has_active_baseline());
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied, slot.emit_cumulative_delta(1'003U, delta));
	ASSERT_EQ(1U, delta.baseline_snapshot_id);
	ASSERT_EQ(1U, delta.delta_sequence);
	ASSERT_EQ(1U, delta.mutations.size());
	ASSERT_EQ(std::vector<std::uint8_t>{static_cast<std::uint8_t>(0x12U)}, delta.mutations[0].atom.value);

	ASSERT_EQ(protocol::ProducerBaselineResult::Applied, slot.replace_current(image(0x13U)));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied, slot.emit_cumulative_delta(1'004U, delta));
	EXPECT_EQ(1U, delta.baseline_snapshot_id)
		<< "Every emission remains relative to the immutable APPLIED snapshot, never the preceding delta.";
	EXPECT_EQ(2U, delta.delta_sequence);
	ASSERT_EQ(1U, delta.mutations.size());
	EXPECT_EQ(std::vector<std::uint8_t>{static_cast<std::uint8_t>(0x13U)}, delta.mutations[0].atom.value);
}

TEST(TelemetryPhase1DeltaContract, MutationDuringReplacementCandidateAppearsAfterAppliedAndOnlyLatestDeltaIsRetained)
{
	// The replacement path is exercised at the baseline owner, rather than via
	// the unused P8.3 slot convenience method: runtime integration owns when a
	// future replacement transaction may be started.
	protocol::ProducerBaselineTracker tracker;
	const std::vector<protocol::SnapshotCandidatePart> initial_parts{part(82U, 0x82828282U)};
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied, tracker.initialize(image(0x20U)));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.capture_snapshot(1U, 0U, image(0x20U), initial_parts, 2'000U));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied, tracker.acknowledge_snapshot_part(ack(initial_parts[0]), 2'001U));
	ASSERT_EQ(1U, tracker.active_snapshot_id());

	const std::vector<protocol::SnapshotCandidatePart> replacement_parts{part(83U, 0x83838383U)};
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied, tracker.replace_current(image(0x21U)));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.capture_snapshot(2U, 0U, image(0x21U), replacement_parts, 2'002U));
	ASSERT_TRUE(tracker.has_candidate());
	ASSERT_EQ(1U, tracker.active_snapshot_id()) << "The old APPLIED baseline remains live during candidate transit.";
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied, tracker.replace_current(image(0x22U)));
	EXPECT_EQ(1U, tracker.candidate_dirty_record_count()) << "The transit mutation is retained against the captured candidate.";

	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		tracker.acknowledge_snapshot_part(ack(replacement_parts[0]), 2'003U));
	EXPECT_FALSE(tracker.has_candidate());
	EXPECT_TRUE(tracker.has_active_baseline());
	EXPECT_EQ(2U, tracker.active_snapshot_id());
	protocol::CumulativeStateDelta delta;
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied, tracker.emit_cumulative_delta(2'004U, delta));
	EXPECT_EQ(2U, delta.baseline_snapshot_id);
	EXPECT_EQ(1U, delta.delta_sequence) << "Promotion begins a new baseline-local delta sequence.";
	ASSERT_EQ(1U, delta.mutations.size());
	EXPECT_EQ(std::vector<std::uint8_t>{static_cast<std::uint8_t>(0x22U)}, delta.mutations[0].atom.value)
		<< "The first post-APPLIED delta contains the mutation made while the candidate was in transit.";
}

TEST(TelemetryPhase1DeltaContract, IncompatibleRecordSetCoalescesTypedKeyframeIntentWithoutPublishingP84State)
{
	EXPECT_TRUE(has_p8_3_keyframe_intent_seams<detail::Phase1SnapshotSlot>::value)
		<< "P8.3 RED: incompatible record sets and unrepresentable cumulative deltas require a bounded typed "
			   "keyframe intent (RecordSetDiscontinuity or CumulativeDeltaNotRepresentable), with coalescence and purge. "
			   "P8.3 must not emit FULL_SNAPSHOT or start/change a candidate for this intent; periodic keyframes remain P8.4.";
}

TEST(TelemetryPhase1DeltaContract, RecordSetDiscontinuityCoalescesWithoutCandidateOrFullSnapshotState)
{
	detail::Phase1SnapshotSlot slot;
	const auto baseline = image_pair(0x10U, 0x20U);
	const std::vector<protocol::SnapshotCandidatePart> parts{part(101U, 0xa1a1a1a1U)};
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied, slot.start_initial_candidate(1U, baseline, parts, 1'000U));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied, slot.acknowledge_candidate_part(ack(parts[0]), 1'001U));
	ASSERT_TRUE(slot.has_active_baseline());
	ASSERT_FALSE(slot.has_candidate());
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied, slot.replace_current(image(0x30U)));
	EXPECT_FALSE(slot.current_record_set_compatible_with_active_baseline());
	EXPECT_EQ(detail::Phase1KeyframeIntent::RecordSetDiscontinuity, slot.keyframe_intent());
	EXPECT_FALSE(slot.has_candidate()) << "P8.3 intent must not start a P8.4 snapshot candidate.";
	EXPECT_EQ(1U, slot.active_snapshot_id());
	EXPECT_FALSE(slot.current_record_set_compatible_with_active_baseline());
	EXPECT_EQ(detail::Phase1KeyframeIntent::RecordSetDiscontinuity, slot.consume_keyframe_intent())
		<< "First cause wins while the bounded intent is pending.";
	EXPECT_EQ(detail::Phase1KeyframeIntent::None, slot.keyframe_intent());
	slot.rollback_candidate();
	EXPECT_EQ(detail::Phase1KeyframeIntent::None, slot.keyframe_intent()) << "Purge clears the intent.";
}

TEST(TelemetryPhase1DeltaContract, ChangedPlayerIdentityRequestsOneDiscontinuityIntentWithoutDeltaOrCandidate)
{
	detail::Phase1SnapshotSlot slot;
	const auto baseline = canonical_player_image(1U);
	const std::vector<protocol::SnapshotCandidatePart> parts{part(102U, 0xa2a2a2a2U)};
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied, slot.start_initial_candidate(1U, baseline, parts, 1'000U));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied, slot.acknowledge_candidate_part(ack(parts[0]), 1'001U));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied, slot.replace_current(canonical_player_image(2U)));
	EXPECT_FALSE(slot.current_record_set_compatible_with_active_baseline());
	EXPECT_EQ(detail::Phase1KeyframeIntent::RecordSetDiscontinuity, slot.keyframe_intent());
	protocol::CumulativeStateDelta delta;
	EXPECT_EQ(protocol::ProducerBaselineResult::KeyframeRequired, slot.emit_cumulative_delta(1'002U, delta));
	EXPECT_FALSE(slot.has_candidate());
	EXPECT_EQ(1U, slot.active_snapshot_id());
	EXPECT_EQ(detail::Phase1KeyframeIntent::RecordSetDiscontinuity, slot.consume_keyframe_intent());
	EXPECT_EQ(detail::Phase1KeyframeIntent::None, slot.keyframe_intent());
}

TEST(TelemetryPhase1DeltaContract, InvalidPlayerSourceAfterValidPlayerCoalescesDiscontinuityWithoutDelta)
{
	detail::Phase1SnapshotSlot slot;
	const std::vector<protocol::SnapshotCandidatePart> parts{part(103U, 0xa3a3a3a3U)};
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		slot.start_initial_candidate(1U, canonical_player_image(1U), parts, 1'000U));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied, slot.acknowledge_candidate_part(ack(parts[0]), 1'001U));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied, slot.replace_current(canonical_invalid_player_image()));
	EXPECT_FALSE(slot.current_record_set_compatible_with_active_baseline());
	protocol::CumulativeStateDelta delta;
	EXPECT_EQ(protocol::ProducerBaselineResult::KeyframeRequired, slot.emit_cumulative_delta(1'002U, delta));
	EXPECT_EQ(detail::Phase1KeyframeIntent::RecordSetDiscontinuity, slot.keyframe_intent());
	EXPECT_FALSE(slot.has_candidate());
	EXPECT_EQ(1U, slot.active_snapshot_id());
	EXPECT_EQ(detail::Phase1KeyframeIntent::RecordSetDiscontinuity, slot.consume_keyframe_intent());
	slot.rollback_candidate();
	EXPECT_EQ(detail::Phase1KeyframeIntent::None, slot.keyframe_intent());
}

TEST(TelemetryPhase1DeltaContract, OversizeCumulativeDifferenceRequestsBoundedRepresentabilityKeyframeIntent)
{
	detail::Phase1SnapshotSlot slot;
	const std::vector<protocol::SnapshotCandidatePart> parts{part(104U, 0xa4a4a4a4U)};
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied, slot.start_initial_candidate(1U, image(0x10U), parts, 1'000U));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied, slot.acknowledge_candidate_part(ack(parts[0]), 1'001U));
	std::vector<protocol::StateAtom> oversized_atoms;
	for (std::uint16_t record_type = 1U; record_type <= 18U; ++record_type) {
		protocol::StateAtom atom;
		atom.key.record_type = record_type;
		atom.value.resize(60'000U, static_cast<std::uint8_t>(record_type));
		oversized_atoms.push_back(std::move(atom));
	}
	protocol::StateImage oversized;
	ASSERT_EQ(protocol::StateImageResult::Created,
		protocol::StateImage::create(std::move(oversized_atoms), oversized));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied, slot.replace_current(oversized));
	protocol::CumulativeStateDelta delta;
	EXPECT_EQ(protocol::ProducerBaselineResult::KeyframeRequired, slot.emit_cumulative_delta(1'002U, delta));
	EXPECT_EQ(detail::Phase1KeyframeIntent::CumulativeDeltaNotRepresentable, slot.keyframe_intent());
	EXPECT_EQ(protocol::ProducerBaselineResult::KeyframeRequired, slot.emit_cumulative_delta(1'003U, delta));
	EXPECT_EQ(detail::Phase1KeyframeIntent::CumulativeDeltaNotRepresentable, slot.consume_keyframe_intent());
	EXPECT_FALSE(slot.has_candidate());
	EXPECT_EQ(1U, slot.active_snapshot_id());
	slot.rollback_candidate();
	EXPECT_EQ(detail::Phase1KeyframeIntent::None, slot.keyframe_intent());
}

} // namespace
