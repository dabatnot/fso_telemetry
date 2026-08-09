#if __has_include("telemetry/entity_id_registry.h")
#include "telemetry/entity_id_registry.h"
#define FSO_HAS_TELEMETRY_ENTITY_ID_REGISTRY 1
#else
#include "telemetry/engine_adapter.h"
#define FSO_HAS_TELEMETRY_ENTITY_ID_REGISTRY 0
#endif

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace {

namespace detail = telemetry::detail;

constexpr const char* MissingRegistry =
	"WP07-C requires telemetry/entity_id_registry.h with the tracker-frozen session registry contract.";

#if !FSO_HAS_TELEMETRY_ENTITY_ID_REGISTRY

#define WP07_C_MISSING_TEST(name) \
	TEST(TelemetryEntityIdRegistryContract, name) \
	{ \
		FAIL() << MissingRegistry; \
	}

WP07_C_MISSING_TEST(StatusDefaultsAndScalarRegistryContractExist)
WP07_C_MISSING_TEST(SameKeyIsStableAndReplacementNeverReusesAnId)
WP07_C_MISSING_TEST(InvalidationAndReappearancePreserveTheMonotonicCounter)
WP07_C_MISSING_TEST(MaximumIdIsAssignableThenExhaustionNeverWraps)
WP07_C_MISSING_TEST(SessionResetAloneRestartsTheCounterAtOne)
WP07_C_MISSING_TEST(MaterializeValidCopiesExactlyAndDistinguishesNewFromExisting)
WP07_C_MISSING_TEST(MaterializeClosedFailuresResetOutputAndDoNotConsumeAnId)

#undef WP07_C_MISSING_TEST

#else

using detail::CaptureReason;
using detail::CaptureResult;
using detail::CaptureStatus;
using detail::EntityIdInvalidationStatus;
using detail::EntityIdRegistry;
using detail::EntityIdResolveStatus;
using detail::PlayerObservationDto;
using detail::PlayerObservationKey;
using detail::PlayerKinematicsSample;
using detail::PlayerSampleMaterializeStatus;

static_assert(std::is_same_v<std::underlying_type_t<EntityIdResolveStatus>, std::uint8_t>);
static_assert(std::is_same_v<std::underlying_type_t<EntityIdInvalidationStatus>, std::uint8_t>);
static_assert(std::is_same_v<std::underlying_type_t<PlayerSampleMaterializeStatus>, std::uint8_t>);
static_assert(static_cast<std::uint8_t>(EntityIdResolveStatus::Existing) == 0U);
static_assert(static_cast<std::uint8_t>(EntityIdResolveStatus::Allocated) == 1U);
static_assert(static_cast<std::uint8_t>(EntityIdResolveStatus::InvalidKey) == 2U);
static_assert(static_cast<std::uint8_t>(EntityIdResolveStatus::CounterExhausted) == 3U);
static_assert(static_cast<std::uint8_t>(EntityIdResolveStatus::Count) == 4U);
static_assert(static_cast<std::uint8_t>(EntityIdInvalidationStatus::Invalidated) == 0U);
static_assert(static_cast<std::uint8_t>(EntityIdInvalidationStatus::AlreadyInvalid) == 1U);
static_assert(static_cast<std::uint8_t>(EntityIdInvalidationStatus::Count) == 2U);
static_assert(static_cast<std::uint8_t>(PlayerSampleMaterializeStatus::MaterializedExisting) == 0U);
static_assert(static_cast<std::uint8_t>(PlayerSampleMaterializeStatus::MaterializedNew) == 1U);
static_assert(static_cast<std::uint8_t>(PlayerSampleMaterializeStatus::NoPlayer) == 2U);
static_assert(static_cast<std::uint8_t>(PlayerSampleMaterializeStatus::InvalidSource) == 3U);
static_assert(static_cast<std::uint8_t>(PlayerSampleMaterializeStatus::InvalidCapture) == 4U);
static_assert(static_cast<std::uint8_t>(PlayerSampleMaterializeStatus::EntityIdCounterExhausted) == 5U);
static_assert(static_cast<std::uint8_t>(PlayerSampleMaterializeStatus::Count) == 6U);
static_assert(std::is_final_v<EntityIdRegistry> && std::is_nothrow_default_constructible_v<EntityIdRegistry>);
static_assert(std::is_nothrow_constructible_v<EntityIdRegistry, std::uint64_t>);
static_assert(std::is_same_v<decltype(detail::EntityIdResolveResult::status), EntityIdResolveStatus>);
static_assert(std::is_same_v<decltype(detail::EntityIdResolveResult::entity_id), std::uint64_t>);
static_assert(std::is_same_v<decltype(std::declval<EntityIdRegistry&>().resolve(
	std::declval<const PlayerObservationKey&>())), detail::EntityIdResolveResult>);
static_assert(std::is_same_v<decltype(std::declval<EntityIdRegistry&>().invalidate()),
	EntityIdInvalidationStatus>);
static_assert(std::is_same_v<decltype(std::declval<EntityIdRegistry&>().reset_session()), void>);
static_assert(std::is_same_v<decltype(std::declval<const EntityIdRegistry&>().has_active_mapping()), bool>);
static_assert(std::is_same_v<decltype(std::declval<const EntityIdRegistry&>().active_object_signature()),
	std::uint32_t>);
static_assert(std::is_same_v<decltype(std::declval<const EntityIdRegistry&>().active_entity_id()),
	std::uint64_t>);
static_assert(std::is_same_v<decltype(std::declval<const EntityIdRegistry&>().last_allocated_entity_id()),
	std::uint64_t>);
static_assert(std::is_same_v<decltype(detail::materialize_player_sample(std::declval<EntityIdRegistry&>(),
	std::declval<const CaptureResult&>(),
	std::declval<const PlayerObservationDto&>(),
	std::declval<PlayerKinematicsSample&>())), PlayerSampleMaterializeStatus>);
static_assert(noexcept(std::declval<EntityIdRegistry&>().resolve(std::declval<const PlayerObservationKey&>())));
static_assert(noexcept(std::declval<EntityIdRegistry&>().invalidate()));
static_assert(noexcept(std::declval<EntityIdRegistry&>().reset_session()));
static_assert(noexcept(std::declval<const EntityIdRegistry&>().has_active_mapping()));
static_assert(noexcept(std::declval<const EntityIdRegistry&>().active_object_signature()));
static_assert(noexcept(std::declval<const EntityIdRegistry&>().active_entity_id()));
static_assert(noexcept(std::declval<const EntityIdRegistry&>().last_allocated_entity_id()));
static_assert(noexcept(detail::materialize_player_sample(std::declval<EntityIdRegistry&>(),
	std::declval<const CaptureResult&>(),
	std::declval<const PlayerObservationDto&>(),
	std::declval<PlayerKinematicsSample&>())));

PlayerObservationKey key(std::uint32_t signature) noexcept
{
	PlayerObservationKey result{};
	result.object_signature = signature;
	return result;
}

CaptureResult capture(CaptureStatus status, CaptureReason reason) noexcept
{
	CaptureResult result{};
	result.status = status;
	result.reason = reason;
	return result;
}

PlayerObservationDto observation(std::uint32_t signature, float base = 1.0f) noexcept
{
	PlayerObservationDto result{};
	result.key.object_signature = signature;
	result.value.producer_sample_time_us = 0x1020304050607080ULL;
	result.value.position_world = {base, base + 1.0f, base + 2.0f};
	result.value.orientation_local_to_world = {0.5f, 0.5f, 0.5f, 0.5f};
	result.value.velocity_world = {base + 3.0f, base + 4.0f, base + 5.0f};
	result.value.rotational_velocity_local = {base + 6.0f, base + 7.0f, base + 8.0f};
	result.value.radius = base + 9.0f;
	result.value.physics_mode_flags = 0x00000555U;
	return result;
}

void expect_default(const PlayerKinematicsSample& sample)
{
	const PlayerKinematicsSample expected{};
	EXPECT_EQ(expected.entity_id, sample.entity_id);
	EXPECT_EQ(expected.value.producer_sample_time_us, sample.value.producer_sample_time_us);
	EXPECT_FLOAT_EQ(expected.value.orientation_local_to_world.w, sample.value.orientation_local_to_world.w);
	const float zeroes[] = {sample.value.position_world.x,
		sample.value.position_world.y,
		sample.value.position_world.z,
		sample.value.orientation_local_to_world.x,
		sample.value.orientation_local_to_world.y,
		sample.value.orientation_local_to_world.z,
		sample.value.velocity_world.x,
		sample.value.velocity_world.y,
		sample.value.velocity_world.z,
		sample.value.rotational_velocity_local.x,
		sample.value.rotational_velocity_local.y,
		sample.value.rotational_velocity_local.z,
		sample.value.radius};
	for (const auto zero : zeroes) {
		EXPECT_FLOAT_EQ(0.0f, zero);
		EXPECT_FALSE(std::signbit(zero));
	}
	EXPECT_EQ(expected.value.physics_mode_flags, sample.value.physics_mode_flags);
}

void expect_exact_value(const detail::PlayerKinematicsValue& expected,
	const detail::PlayerKinematicsValue& actual)
{
	EXPECT_EQ(expected.producer_sample_time_us, actual.producer_sample_time_us);
	EXPECT_FLOAT_EQ(expected.position_world.x, actual.position_world.x);
	EXPECT_FLOAT_EQ(expected.position_world.y, actual.position_world.y);
	EXPECT_FLOAT_EQ(expected.position_world.z, actual.position_world.z);
	EXPECT_FLOAT_EQ(expected.orientation_local_to_world.w, actual.orientation_local_to_world.w);
	EXPECT_FLOAT_EQ(expected.orientation_local_to_world.x, actual.orientation_local_to_world.x);
	EXPECT_FLOAT_EQ(expected.orientation_local_to_world.y, actual.orientation_local_to_world.y);
	EXPECT_FLOAT_EQ(expected.orientation_local_to_world.z, actual.orientation_local_to_world.z);
	EXPECT_FLOAT_EQ(expected.velocity_world.x, actual.velocity_world.x);
	EXPECT_FLOAT_EQ(expected.velocity_world.y, actual.velocity_world.y);
	EXPECT_FLOAT_EQ(expected.velocity_world.z, actual.velocity_world.z);
	EXPECT_FLOAT_EQ(expected.rotational_velocity_local.x, actual.rotational_velocity_local.x);
	EXPECT_FLOAT_EQ(expected.rotational_velocity_local.y, actual.rotational_velocity_local.y);
	EXPECT_FLOAT_EQ(expected.rotational_velocity_local.z, actual.rotational_velocity_local.z);
	EXPECT_FLOAT_EQ(expected.radius, actual.radius);
	EXPECT_EQ(expected.physics_mode_flags, actual.physics_mode_flags);
}

TEST(TelemetryEntityIdRegistryContract, StatusDefaultsAndScalarRegistryContractExist)
{
	detail::EntityIdResolveResult default_result{};
	EXPECT_EQ(EntityIdResolveStatus::InvalidKey, default_result.status);
	EXPECT_EQ(0U, default_result.entity_id);
	EntityIdRegistry registry;
	EXPECT_FALSE(registry.has_active_mapping());
	EXPECT_EQ(0U, registry.active_object_signature());
	EXPECT_EQ(0U, registry.active_entity_id());
	EXPECT_EQ(0U, registry.last_allocated_entity_id());
	const auto invalid = registry.resolve(key(0U));
	EXPECT_EQ(EntityIdResolveStatus::InvalidKey, invalid.status);
	EXPECT_EQ(0U, invalid.entity_id);
	EXPECT_FALSE(registry.has_active_mapping());
}

TEST(TelemetryEntityIdRegistryContract, SameKeyIsStableAndReplacementNeverReusesAnId)
{
	EntityIdRegistry registry;
	const auto first = registry.resolve(key(10U));
	EXPECT_EQ(EntityIdResolveStatus::Allocated, first.status);
	EXPECT_EQ(1U, first.entity_id);
	const auto stable = registry.resolve(key(10U));
	EXPECT_EQ(EntityIdResolveStatus::Existing, stable.status);
	EXPECT_EQ(first.entity_id, stable.entity_id);
	const auto replacement = registry.resolve(key(20U));
	EXPECT_EQ(EntityIdResolveStatus::Allocated, replacement.status);
	EXPECT_EQ(2U, replacement.entity_id);
	EXPECT_NE(first.entity_id, replacement.entity_id);
	const auto returning = registry.resolve(key(10U));
	EXPECT_EQ(EntityIdResolveStatus::Allocated, returning.status);
	EXPECT_EQ(3U, returning.entity_id);
	EXPECT_EQ(10U, registry.active_object_signature());
	EXPECT_EQ(3U, registry.last_allocated_entity_id());
}

TEST(TelemetryEntityIdRegistryContract, InvalidationAndReappearancePreserveTheMonotonicCounter)
{
	EntityIdRegistry registry;
	EXPECT_EQ(1U, registry.resolve(key(10U)).entity_id);
	EXPECT_EQ(EntityIdInvalidationStatus::Invalidated, registry.invalidate());
	EXPECT_EQ(EntityIdInvalidationStatus::AlreadyInvalid, registry.invalidate());
	EXPECT_FALSE(registry.has_active_mapping());
	EXPECT_EQ(1U, registry.last_allocated_entity_id());
	EXPECT_EQ(2U, registry.resolve(key(10U)).entity_id);
	const auto invalid_key = registry.resolve(key(0U));
	EXPECT_EQ(EntityIdResolveStatus::InvalidKey, invalid_key.status);
	EXPECT_FALSE(registry.has_active_mapping());
	EXPECT_EQ(2U, registry.last_allocated_entity_id());
	EXPECT_EQ(3U, registry.resolve(key(10U)).entity_id);
}

TEST(TelemetryEntityIdRegistryContract, MaximumIdIsAssignableThenExhaustionNeverWraps)
{
	const auto maximum = std::numeric_limits<std::uint64_t>::max();
	EntityIdRegistry registry(maximum - 1U);
	const auto maximum_id = registry.resolve(key(1U));
	EXPECT_EQ(EntityIdResolveStatus::Allocated, maximum_id.status);
	EXPECT_EQ(maximum, maximum_id.entity_id);
	const auto stable = registry.resolve(key(1U));
	EXPECT_EQ(EntityIdResolveStatus::Existing, stable.status);
	EXPECT_EQ(maximum, stable.entity_id);
	const auto exhausted = registry.resolve(key(2U));
	EXPECT_EQ(EntityIdResolveStatus::CounterExhausted, exhausted.status);
	EXPECT_EQ(0U, exhausted.entity_id);
	EXPECT_FALSE(registry.has_active_mapping());
	EXPECT_EQ(maximum, registry.last_allocated_entity_id());
	const auto still_exhausted = registry.resolve(key(1U));
	EXPECT_EQ(EntityIdResolveStatus::CounterExhausted, still_exhausted.status);
	EXPECT_EQ(0U, still_exhausted.entity_id);
	EXPECT_EQ(maximum, registry.last_allocated_entity_id());
}

TEST(TelemetryEntityIdRegistryContract, SessionResetAloneRestartsTheCounterAtOne)
{
	EntityIdRegistry registry;
	EXPECT_EQ(1U, registry.resolve(key(1U)).entity_id);
	EXPECT_EQ(2U, registry.resolve(key(2U)).entity_id);
	EXPECT_TRUE(registry.has_active_mapping());
	EXPECT_EQ(2U, registry.last_allocated_entity_id());
	registry.reset_session();
	EXPECT_FALSE(registry.has_active_mapping());
	EXPECT_EQ(0U, registry.last_allocated_entity_id());
	const auto new_session = registry.resolve(key(2U));
	EXPECT_EQ(EntityIdResolveStatus::Allocated, new_session.status);
	EXPECT_EQ(1U, new_session.entity_id);

	const auto maximum = std::numeric_limits<std::uint64_t>::max();
	EntityIdRegistry exhausted(maximum);
	EXPECT_EQ(EntityIdResolveStatus::CounterExhausted, exhausted.resolve(key(9U)).status);
	EXPECT_EQ(maximum, exhausted.last_allocated_entity_id());
	exhausted.reset_session();
	EXPECT_FALSE(exhausted.has_active_mapping());
	EXPECT_EQ(0U, exhausted.last_allocated_entity_id());
	const auto recovered = exhausted.resolve(key(9U));
	EXPECT_EQ(EntityIdResolveStatus::Allocated, recovered.status);
	EXPECT_EQ(1U, recovered.entity_id);
}

TEST(TelemetryEntityIdRegistryContract, MaterializeValidCopiesExactlyAndDistinguishesNewFromExisting)
{
	EntityIdRegistry registry;
	auto source = observation(42U, 10.0f);
	PlayerKinematicsSample sample{};
	const auto first = detail::materialize_player_sample(
		registry, capture(CaptureStatus::Valid, CaptureReason::None), source, sample);
	EXPECT_EQ(PlayerSampleMaterializeStatus::MaterializedNew, first);
	EXPECT_EQ(1U, sample.entity_id);
	expect_exact_value(source.value, sample.value);
	const auto retained = sample;
	source.value.position_world.x = -99.0f;
	EXPECT_FLOAT_EQ(10.0f, retained.value.position_world.x);
	const auto existing = detail::materialize_player_sample(
		registry, capture(CaptureStatus::Valid, CaptureReason::None), source, sample);
	EXPECT_EQ(PlayerSampleMaterializeStatus::MaterializedExisting, existing);
	EXPECT_EQ(1U, sample.entity_id);
	expect_exact_value(source.value, sample.value);
	EXPECT_EQ(1U, registry.last_allocated_entity_id()) << "A valid materialization resolves exactly once.";
}

TEST(TelemetryEntityIdRegistryContract, MaterializeClosedFailuresResetOutputAndDoNotConsumeAnId)
{
	for (std::uint8_t status_value = 0U;
		status_value <= static_cast<std::uint8_t>(CaptureStatus::Count);
		++status_value) {
		for (std::uint8_t reason_value = 0U;
			reason_value <= static_cast<std::uint8_t>(CaptureReason::Count);
			++reason_value) {
			const auto status = static_cast<CaptureStatus>(status_value);
			const auto reason = static_cast<CaptureReason>(reason_value);
			if (status == CaptureStatus::Valid && reason == CaptureReason::None) continue;
			PlayerSampleMaterializeStatus expected = PlayerSampleMaterializeStatus::InvalidCapture;
			if (status == CaptureStatus::NoPlayer && reason >= CaptureReason::NotInMission &&
				reason <= CaptureReason::MissingPlayerShip) {
				expected = PlayerSampleMaterializeStatus::NoPlayer;
			}
			if (status == CaptureStatus::InvalidSource && reason >= CaptureReason::WrongObjectType &&
				reason < CaptureReason::Count) {
				expected = PlayerSampleMaterializeStatus::InvalidSource;
			}
			EntityIdRegistry registry;
			EXPECT_EQ(1U, registry.resolve(key(7U)).entity_id);
			PlayerKinematicsSample output{};
			output.entity_id = 999U;
			output.value.radius = 99.0f;
			const auto result = detail::materialize_player_sample(
				registry, capture(status, reason), observation(7U), output);
			EXPECT_EQ(expected, result) << "status=" << +status_value << " reason=" << +reason_value;
			expect_default(output);
			EXPECT_FALSE(registry.has_active_mapping());
			EXPECT_EQ(1U, registry.last_allocated_entity_id());
			EXPECT_EQ(2U, registry.resolve(key(7U)).entity_id);
		}
	}

	EntityIdRegistry invalid_key_registry;
	PlayerKinematicsSample output{};
	output.entity_id = 999U;
	const auto invalid_key = detail::materialize_player_sample(invalid_key_registry,
		capture(CaptureStatus::Valid, CaptureReason::None),
		observation(0U),
		output);
	EXPECT_EQ(PlayerSampleMaterializeStatus::InvalidCapture, invalid_key);
	expect_default(output);
	EXPECT_EQ(0U, invalid_key_registry.last_allocated_entity_id());

	const auto maximum = std::numeric_limits<std::uint64_t>::max();
	EntityIdRegistry exhausted(maximum);
	output.entity_id = 999U;
	const auto overflow = detail::materialize_player_sample(exhausted,
		capture(CaptureStatus::Valid, CaptureReason::None),
		observation(9U),
		output);
	EXPECT_EQ(PlayerSampleMaterializeStatus::EntityIdCounterExhausted, overflow);
	expect_default(output);
	EXPECT_EQ(maximum, exhausted.last_allocated_entity_id());
}

#endif

} // namespace
