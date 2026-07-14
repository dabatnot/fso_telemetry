#include "telemetry/protocol/telemetry_capabilities.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using namespace telemetry::protocol;

constexpr std::uint64_t AllClientCapabilities =
	CapabilityCommViewLocalAssets | CapabilityTargetVideoH264 | CapabilityUpdate;
constexpr std::uint64_t AllProducerCapabilities =
	CapabilityCommViewAuthoritativeSource | CapabilityTargetVideoRemoteRender | CapabilityUpdate;
constexpr std::uint64_t AllActiveCapabilities = VisualCapabilities | CapabilityUpdate;

CapabilitySessionState all_capabilities_state()
{
	CapabilitySessionState state;
	EXPECT_EQ(ValidationError::None,
		initialize_capability_session(AllClientCapabilities, AllProducerCapabilities, AllActiveCapabilities, state));
	return state;
}

CapabilityUpdatePayload
update(std::uint32_t generation, std::uint64_t advertised, std::uint64_t active, std::uint64_t effective_time = 100U)
{
	CapabilityUpdatePayload result;
	result.capability_generation = generation;
	result.advertised_capabilities = advertised;
	result.active_capabilities = active;
	result.effective_time_us = effective_time;
	result.reason = CapabilityUpdateReason::RuntimeAvailability;
	return result;
}

TEST(TelemetryProtocolCapabilities, EmitValidationRejectsUnknownAndWrongOwnerCapabilities)
{
	EXPECT_EQ(ValidationError::None,
		validate_emittable_capability_offer(CapabilityPeerRole::Client, AllClientCapabilities));
	EXPECT_EQ(ValidationError::None,
		validate_emittable_capability_offer(CapabilityPeerRole::Producer, AllProducerCapabilities));
	EXPECT_EQ(ValidationError::CapabilityNotNegotiated,
		validate_emittable_capability_offer(CapabilityPeerRole::Client, CapabilityCommViewAuthoritativeSource));
	EXPECT_EQ(ValidationError::CapabilityNotNegotiated,
		validate_emittable_capability_offer(CapabilityPeerRole::Invalid, 0U));
	EXPECT_EQ(ValidationError::ReservedFlag,
		validate_emittable_capability_offer(CapabilityPeerRole::Client, std::uint64_t{1} << 63U));

	EXPECT_EQ(ValidationError::None, validate_emittable_active_capabilities(AllActiveCapabilities));
	EXPECT_EQ(ValidationError::CapabilityNotNegotiated,
		validate_emittable_active_capabilities(CapabilityCommViewLocalAssets));
	EXPECT_EQ(ValidationError::ReservedFlag, validate_emittable_active_capabilities(std::uint64_t{1} << 63U));
}

TEST(TelemetryProtocolCapabilities, InitialNegotiationIgnoresUnknownBitsButRejectsKnownOwnershipAndPairErrors)
{
	const auto unknown_bit = std::uint64_t{1} << 63U;
	CapabilitySessionState state;
	ASSERT_EQ(ValidationError::None,
		initialize_capability_session(AllClientCapabilities | unknown_bit,
			AllProducerCapabilities | unknown_bit,
			AllActiveCapabilities | unknown_bit,
			state));
	EXPECT_TRUE(state.initialized());
	EXPECT_EQ(AllClientCapabilities, state.client_advertised_capabilities());
	EXPECT_EQ(AllProducerCapabilities, state.producer_advertised_capabilities());
	EXPECT_EQ(AllActiveCapabilities, state.active_capabilities());
	EXPECT_TRUE(state.capability_updates_were_negotiated());

	const auto previous_active = state.active_capabilities();
	EXPECT_EQ(ValidationError::CapabilityNotNegotiated,
		initialize_capability_session(AllClientCapabilities | CapabilityCommViewAuthoritativeSource,
			AllProducerCapabilities,
			AllActiveCapabilities,
			state));
	EXPECT_EQ(previous_active, state.active_capabilities());
	EXPECT_TRUE(state.initialized());
	EXPECT_EQ(ValidationError::CapabilityNotNegotiated,
		initialize_capability_session(AllClientCapabilities,
			AllProducerCapabilities,
			CapabilityCommViewLocalAssets,
			state));
	EXPECT_EQ(previous_active, state.active_capabilities());
	EXPECT_EQ(ValidationError::CapabilityNotNegotiated,
		initialize_capability_session(CapabilityUpdate, CapabilityNone, CapabilityUpdate, state));
	EXPECT_EQ(previous_active, state.active_capabilities());
}

TEST(TelemetryProtocolCapabilities, ConvenienceNegotiationSelectsOnlyCompatibleCompletePairs)
{
	CapabilitySessionState state;
	ASSERT_EQ(ValidationError::None,
		negotiate_initial_capabilities(AllClientCapabilities, AllProducerCapabilities, CommViewCapabilityPair, state));
	EXPECT_EQ(CommViewCapabilityPair | CapabilityUpdate, state.active_capabilities());

	ASSERT_EQ(ValidationError::None,
		negotiate_initial_capabilities(AllClientCapabilities, AllProducerCapabilities, VisualCapabilities, state));
	EXPECT_EQ(AllActiveCapabilities, state.active_capabilities());
	EXPECT_EQ(ValidationError::CapabilityNotNegotiated,
		negotiate_initial_capabilities(AllClientCapabilities,
			AllProducerCapabilities,
			CapabilityCommViewLocalAssets,
			state));
	EXPECT_EQ(AllActiveCapabilities, state.active_capabilities());
	EXPECT_EQ(ValidationError::CapabilityNotNegotiated,
		negotiate_initial_capabilities(AllClientCapabilities, AllProducerCapabilities, CapabilityUpdate, state));
}

TEST(TelemetryProtocolCapabilities, ReceiveSideAcceptsFirstGenerationJumpAndPeerGenerationsAreIndependent)
{
	auto state = all_capabilities_state();
	const auto client_gen2 = update(2U, AllClientCapabilities, AllActiveCapabilities);
	auto outcome = apply_capability_update(state, CapabilityPeerRole::Client, client_gen2);
	EXPECT_EQ(CapabilityUpdateDisposition::Applied, outcome.disposition);
	EXPECT_EQ(ValidationError::None, outcome.error);
	EXPECT_EQ(AllActiveCapabilities, state.active_capabilities());

	const auto client_gen1 = update(1U, AllClientCapabilities, AllActiveCapabilities);
	outcome = apply_capability_update(state, CapabilityPeerRole::Client, client_gen1);
	EXPECT_EQ(CapabilityUpdateDisposition::Obsolete, outcome.disposition);
	EXPECT_TRUE(outcome.should_ack_applied());

	const auto producer_gen4 = update(4U, AllProducerCapabilities, AllActiveCapabilities, 200U);
	outcome = apply_capability_update(state, CapabilityPeerRole::Producer, producer_gen4);
	EXPECT_EQ(CapabilityUpdateDisposition::Applied, outcome.disposition);

	CapabilitySessionState uninitialized;
	outcome = apply_capability_update(uninitialized, CapabilityPeerRole::Client, client_gen2);
	EXPECT_EQ(CapabilityUpdateDisposition::NotNegotiated, outcome.disposition);
	EXPECT_EQ(ValidationError::CapabilityNotNegotiated, outcome.error);
	outcome = apply_capability_update(state, CapabilityPeerRole::Invalid, client_gen2);
	EXPECT_EQ(CapabilityUpdateDisposition::NotNegotiated, outcome.disposition);
}

TEST(TelemetryProtocolCapabilities, GenerationJumpsAreAcceptedAndDuplicateCollisionAndObsoleteAreDistinct)
{
	auto state = all_capabilities_state();
	const auto generation1 = update(1U, AllClientCapabilities, AllActiveCapabilities, 100U);
	auto outcome = apply_capability_update(state, CapabilityPeerRole::Client, generation1);
	ASSERT_EQ(CapabilityUpdateDisposition::Applied, outcome.disposition);

	const auto generation3 = update(3U, AllClientCapabilities, AllActiveCapabilities, 300U);
	outcome = apply_capability_update(state, CapabilityPeerRole::Client, generation3);
	EXPECT_EQ(CapabilityUpdateDisposition::Applied, outcome.disposition);
	EXPECT_EQ(ValidationError::None, outcome.error);

	outcome = apply_capability_update(state, CapabilityPeerRole::Client, generation3);
	EXPECT_EQ(CapabilityUpdateDisposition::DuplicateIdentical, outcome.disposition);
	EXPECT_TRUE(outcome.should_ack_applied());

	auto collision = generation3;
	collision.effective_time_us += 1U;
	outcome = apply_capability_update(state, CapabilityPeerRole::Client, collision);
	EXPECT_EQ(CapabilityUpdateDisposition::SameGenerationDifferent, outcome.disposition);
	EXPECT_EQ(ValidationError::InvalidStateTransition, outcome.error);
	EXPECT_FALSE(outcome.should_ack_applied());

	outcome = apply_capability_update(state, CapabilityPeerRole::Client, generation1);
	EXPECT_EQ(CapabilityUpdateDisposition::Obsolete, outcome.disposition);
	EXPECT_TRUE(outcome.should_ack_applied());
}

TEST(TelemetryProtocolCapabilities, UpdatesAreMonotoneWithdrawalsAndReportRemovedVisualPairs)
{
	auto state = all_capabilities_state();
	const auto without_video_offer = AllClientCapabilities & ~CapabilityTargetVideoH264;
	const auto without_video_active = AllActiveCapabilities & ~TargetVideoCapabilityPair;
	const auto withdrawal = update(1U, without_video_offer, without_video_active);
	auto outcome = apply_capability_update(state, CapabilityPeerRole::Client, withdrawal);
	ASSERT_EQ(CapabilityUpdateDisposition::Applied, outcome.disposition);
	EXPECT_EQ(TargetVideoCapabilityPair, outcome.removed_active_capabilities);
	EXPECT_EQ(TargetVideoCapabilityPair, outcome.removed_visual_capabilities());
	EXPECT_EQ(without_video_active, state.active_capabilities());

	const auto attempted_addition = update(2U, AllClientCapabilities, AllActiveCapabilities, 200U);
	outcome = apply_capability_update(state, CapabilityPeerRole::Client, attempted_addition);
	EXPECT_EQ(CapabilityUpdateDisposition::RequiresNewSession, outcome.disposition);
	EXPECT_EQ(ValidationError::InvalidStateTransition, outcome.error);
	EXPECT_EQ(without_video_active, state.active_capabilities());

	const auto wrong_owner =
		update(2U, without_video_offer | CapabilityCommViewAuthoritativeSource, without_video_active, 200U);
	outcome = apply_capability_update(state, CapabilityPeerRole::Client, wrong_owner);
	EXPECT_EQ(CapabilityUpdateDisposition::Rejected, outcome.disposition);
	EXPECT_EQ(ValidationError::CapabilityNotNegotiated, outcome.error);
}

TEST(TelemetryProtocolCapabilities, WithdrawingCapabilityUpdateIsFinalForThatPeer)
{
	auto state = all_capabilities_state();
	const auto final_update =
		update(1U, AllClientCapabilities & ~CapabilityUpdate, AllActiveCapabilities & ~CapabilityUpdate);
	auto outcome = apply_capability_update(state, CapabilityPeerRole::Client, final_update);
	ASSERT_EQ(CapabilityUpdateDisposition::Applied, outcome.disposition);
	EXPECT_EQ(CapabilityUpdate, outcome.removed_active_capabilities);
	EXPECT_EQ(VisualCapabilities, state.active_capabilities());

	outcome = apply_capability_update(state, CapabilityPeerRole::Client, final_update);
	EXPECT_EQ(CapabilityUpdateDisposition::DuplicateIdentical, outcome.disposition);

	const auto later =
		update(2U, AllClientCapabilities & ~CapabilityUpdate, AllActiveCapabilities & ~CapabilityUpdate, 200U);
	outcome = apply_capability_update(state, CapabilityPeerRole::Client, later);
	EXPECT_EQ(CapabilityUpdateDisposition::RequiresNewSession, outcome.disposition);
	EXPECT_EQ(ValidationError::InvalidStateTransition, outcome.error);
	EXPECT_EQ(VisualCapabilities, state.active_capabilities());
}

TEST(TelemetryProtocolCapabilities, UpdateRequiresCapabilityToHaveBeenNegotiatedInitially)
{
	CapabilitySessionState state;
	ASSERT_EQ(ValidationError::None,
		initialize_capability_session(CapabilityCommViewLocalAssets,
			CapabilityCommViewAuthoritativeSource,
			CommViewCapabilityPair,
			state));
	EXPECT_FALSE(state.capability_updates_were_negotiated());
	const auto attempted = update(1U, CapabilityCommViewLocalAssets, CommViewCapabilityPair);
	const auto outcome = apply_capability_update(state, CapabilityPeerRole::Client, attempted);
	EXPECT_EQ(CapabilityUpdateDisposition::NotNegotiated, outcome.disposition);
	EXPECT_EQ(ValidationError::CapabilityNotNegotiated, outcome.error);
	EXPECT_EQ(CommViewCapabilityPair, state.active_capabilities());
}

TEST(TelemetryProtocolCapabilities, ReceiveSideUnknownBitsAreMaskedButRemainPartOfDuplicateIdentity)
{
	auto state = all_capabilities_state();
	const auto unknown_bit = std::uint64_t{1} << 63U;
	const auto first = update(1U, AllClientCapabilities | unknown_bit, AllActiveCapabilities | unknown_bit);
	auto outcome = apply_capability_update(state, CapabilityPeerRole::Client, first);
	ASSERT_EQ(CapabilityUpdateDisposition::Applied, outcome.disposition);
	EXPECT_EQ(AllClientCapabilities, state.client_advertised_capabilities());
	EXPECT_EQ(AllActiveCapabilities, state.active_capabilities());

	outcome = apply_capability_update(state, CapabilityPeerRole::Client, first);
	EXPECT_EQ(CapabilityUpdateDisposition::DuplicateIdentical, outcome.disposition);
	auto same_generation_different_raw = first;
	same_generation_different_raw.advertised_capabilities &= ~unknown_bit;
	same_generation_different_raw.active_capabilities &= ~unknown_bit;
	outcome = apply_capability_update(state, CapabilityPeerRole::Client, same_generation_different_raw);
	EXPECT_EQ(CapabilityUpdateDisposition::SameGenerationDifferent, outcome.disposition);
}

} // namespace
