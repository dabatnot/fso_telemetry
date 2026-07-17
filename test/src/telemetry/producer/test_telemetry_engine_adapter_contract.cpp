#if __has_include("telemetry/engine_adapter.h")
#include "telemetry/engine_adapter.h"

#include "physics/physics.h"
#include "telemetry/protocol/telemetry_protocol_constants.h"
#define FSO_HAS_TELEMETRY_ENGINE_ADAPTER 1
#else
#define FSO_HAS_TELEMETRY_ENGINE_ADAPTER 0
#endif

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

namespace {

constexpr const char* MissingEngineAdapter =
	"WP07-A requires telemetry/engine_adapter.h with the tracker-approved value, quaternion and flag contracts.";

#if !FSO_HAS_TELEMETRY_ENGINE_ADAPTER

#define WP07_A_MISSING_TEST(name, contract) \
	TEST(TelemetryEngineAdapterContract, name) \
	{ \
		FAIL() << MissingEngineAdapter << " Contract: " << contract; \
	}

WP07_A_MISSING_TEST(ValueDtoAndCaptureResultContractsExist,
	"CaptureVec3f, CaptureQuaternionf, CaptureOrientationBasis, PlayerObservationKey, "
	"PlayerKinematicsValue, PlayerObservationDto, PlayerKinematicsSample, "
	"CaptureStatus/Reason/Result and QuaternionConversionStatus")
WP07_A_MISSING_TEST(FsoOrientationConversionIsCanonicalAndTransactional,
	"finite FSO basis to canonical local-to-world w,x,y,z float quaternion, invalid input resets output to identity")
WP07_A_MISSING_TEST(EnginePhysicsFlagsMapToTheClosedFstlBitmap,
	"real PF_* inputs and three object-lock booleans map only to KnownPhysicsModeFlags")
WP07_A_MISSING_TEST(Wp07ABoundariesExcludeSessionWireRuntimeAndOwnership,
	"shared observation DTO only; no entity registry, wire image, cadence, Runtime, allocation or owning fields")

#undef WP07_A_MISSING_TEST

#else

namespace detail = telemetry::detail;
namespace protocol = telemetry::protocol;

using detail::CaptureQuaternionf;
using detail::CaptureVec3f;
using detail::CaptureOrientationBasis;

constexpr float QuaternionNormMinimum = 0.9999f;
constexpr float QuaternionNormMaximum = 1.0001f;

static_assert(sizeof(float) == 4U && std::numeric_limits<float>::is_iec559);

static_assert(std::is_standard_layout_v<CaptureVec3f> && std::is_trivially_copyable_v<CaptureVec3f>);
static_assert(std::is_standard_layout_v<CaptureQuaternionf> &&
	std::is_trivially_copyable_v<CaptureQuaternionf>);
static_assert(std::is_standard_layout_v<CaptureOrientationBasis> &&
	std::is_trivially_copyable_v<CaptureOrientationBasis>);
static_assert(std::is_standard_layout_v<detail::PlayerObservationKey> &&
	std::is_trivially_copyable_v<detail::PlayerObservationKey>);
static_assert(std::is_standard_layout_v<detail::PlayerKinematicsValue> &&
	std::is_trivially_copyable_v<detail::PlayerKinematicsValue>);
static_assert(std::is_standard_layout_v<detail::PlayerObservationDto> &&
	std::is_trivially_copyable_v<detail::PlayerObservationDto>);
static_assert(std::is_standard_layout_v<detail::PlayerKinematicsSample> &&
	std::is_trivially_copyable_v<detail::PlayerKinematicsSample>);
static_assert(std::is_standard_layout_v<detail::CaptureResult> &&
	std::is_trivially_copyable_v<detail::CaptureResult>);
static_assert(std::is_standard_layout_v<detail::EnginePhysicsFlagInput> &&
	std::is_trivially_copyable_v<detail::EnginePhysicsFlagInput>);
static_assert(std::is_enum_v<detail::CaptureStatus> && std::is_enum_v<detail::CaptureReason> &&
	std::is_enum_v<detail::QuaternionConversionStatus>);
static_assert(std::is_same_v<std::underlying_type_t<detail::CaptureStatus>, std::uint8_t> &&
	std::is_same_v<std::underlying_type_t<detail::CaptureReason>, std::uint8_t> &&
	std::is_same_v<std::underlying_type_t<detail::QuaternionConversionStatus>, std::uint8_t>);
static_assert(std::is_same_v<decltype(CaptureVec3f::x), float> &&
	std::is_same_v<decltype(CaptureVec3f::y), float> && std::is_same_v<decltype(CaptureVec3f::z), float>);
static_assert(std::is_same_v<decltype(CaptureQuaternionf::w), float> &&
	std::is_same_v<decltype(CaptureQuaternionf::x), float> &&
	std::is_same_v<decltype(CaptureQuaternionf::y), float> &&
	std::is_same_v<decltype(CaptureQuaternionf::z), float>);
static_assert(std::is_same_v<decltype(CaptureOrientationBasis::right_world), CaptureVec3f> &&
	std::is_same_v<decltype(CaptureOrientationBasis::up_world), CaptureVec3f> &&
	std::is_same_v<decltype(CaptureOrientationBasis::forward_world), CaptureVec3f>);
static_assert(std::is_same_v<decltype(detail::PlayerObservationKey::object_signature), std::uint32_t>);
static_assert(std::is_same_v<decltype(detail::PlayerKinematicsValue::producer_sample_time_us), std::uint64_t> &&
	std::is_same_v<decltype(detail::PlayerKinematicsValue::position_world), CaptureVec3f> &&
	std::is_same_v<decltype(detail::PlayerKinematicsValue::orientation_local_to_world), CaptureQuaternionf> &&
	std::is_same_v<decltype(detail::PlayerKinematicsValue::velocity_world), CaptureVec3f> &&
	std::is_same_v<decltype(detail::PlayerKinematicsValue::rotational_velocity_local), CaptureVec3f> &&
	std::is_same_v<decltype(detail::PlayerKinematicsValue::radius), float> &&
	std::is_same_v<decltype(detail::PlayerKinematicsValue::physics_mode_flags), std::uint32_t>);
static_assert(std::is_same_v<decltype(detail::PlayerObservationDto::key), detail::PlayerObservationKey> &&
	std::is_same_v<decltype(detail::PlayerObservationDto::value), detail::PlayerKinematicsValue>);
static_assert(std::is_same_v<decltype(detail::PlayerKinematicsSample::entity_id), std::uint64_t> &&
	std::is_same_v<decltype(detail::PlayerKinematicsSample::value), detail::PlayerKinematicsValue>);
static_assert(std::is_same_v<decltype(detail::CaptureResult::status), detail::CaptureStatus> &&
	std::is_same_v<decltype(detail::CaptureResult::reason), detail::CaptureReason>);
static_assert(std::is_same_v<decltype(detail::EnginePhysicsFlagInput::raw_physics_flags), std::uint32_t> &&
	std::is_same_v<decltype(detail::EnginePhysicsFlagInput::object_immobile), bool> &&
	std::is_same_v<decltype(detail::EnginePhysicsFlagInput::object_position_locked), bool> &&
	std::is_same_v<decltype(detail::EnginePhysicsFlagInput::object_orientation_locked), bool>);
template <typename T>
constexpr bool is_nothrow_value_contract = std::is_nothrow_default_constructible_v<T> &&
	std::is_nothrow_copy_constructible_v<T> && std::is_nothrow_copy_assignable_v<T>;
static_assert(is_nothrow_value_contract<CaptureVec3f> &&
	is_nothrow_value_contract<CaptureQuaternionf> &&
	is_nothrow_value_contract<CaptureOrientationBasis> &&
	is_nothrow_value_contract<detail::PlayerObservationKey> &&
	is_nothrow_value_contract<detail::PlayerKinematicsValue> &&
	is_nothrow_value_contract<detail::PlayerObservationDto> &&
	is_nothrow_value_contract<detail::PlayerKinematicsSample> &&
	is_nothrow_value_contract<detail::EnginePhysicsFlagInput> &&
	is_nothrow_value_contract<detail::CaptureResult>);
static_assert(noexcept(detail::convert_fso_orientation_to_local_to_world(
	std::declval<const CaptureOrientationBasis&>(), std::declval<CaptureQuaternionf&>())));
static_assert(noexcept(detail::map_player_physics_mode_flags(
	std::declval<const detail::EnginePhysicsFlagInput&>())));

CaptureVec3f vec(float x, float y, float z) noexcept
{
	CaptureVec3f result{};
	result.x = x;
	result.y = y;
	result.z = z;
	return result;
}

CaptureQuaternionf quat(float w, float x, float y, float z) noexcept
{
	CaptureQuaternionf result{};
	result.w = w;
	result.x = x;
	result.y = y;
	result.z = z;
	return result;
}

CaptureOrientationBasis basis(CaptureVec3f right, CaptureVec3f up, CaptureVec3f forward) noexcept
{
	CaptureOrientationBasis result{};
	result.right_world = right;
	result.up_world = up;
	result.forward_world = forward;
	return result;
}

float quaternion_norm(CaptureQuaternionf value) noexcept
{
	return std::sqrt(value.w * value.w + value.x * value.x + value.y * value.y + value.z * value.z);
}

CaptureQuaternionf normalized(CaptureQuaternionf value) noexcept
{
	const auto norm = quaternion_norm(value);
	value.w /= norm;
	value.x /= norm;
	value.y /= norm;
	value.z /= norm;
	return value;
}

CaptureOrientationBasis basis_from_quaternion(CaptureQuaternionf input) noexcept
{
	const auto q = normalized(input);
	const auto xx = q.x * q.x;
	const auto yy = q.y * q.y;
	const auto zz = q.z * q.z;
	const auto xy = q.x * q.y;
	const auto xz = q.x * q.z;
	const auto yz = q.y * q.z;
	const auto wx = q.w * q.x;
	const auto wy = q.w * q.y;
	const auto wz = q.w * q.z;
	return basis(vec(1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz), 2.0f * (xz - wy)),
		vec(2.0f * (xy - wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx)),
		vec(2.0f * (xz + wy), 2.0f * (yz - wx), 1.0f - 2.0f * (xx + yy)));
}

CaptureVec3f rotate_local_axis(CaptureQuaternionf input, CaptureVec3f axis) noexcept
{
	const auto q = normalized(input);
	const CaptureVec3f vector_part = vec(q.x, q.y, q.z);
	const auto uv = vec(vector_part.y * axis.z - vector_part.z * axis.y,
		vector_part.z * axis.x - vector_part.x * axis.z,
		vector_part.x * axis.y - vector_part.y * axis.x);
	const auto uuv = vec(vector_part.y * uv.z - vector_part.z * uv.y,
		vector_part.z * uv.x - vector_part.x * uv.z,
		vector_part.x * uv.y - vector_part.y * uv.x);
	return vec(axis.x + 2.0f * (q.w * uv.x + uuv.x),
		axis.y + 2.0f * (q.w * uv.y + uuv.y),
		axis.z + 2.0f * (q.w * uv.z + uuv.z));
}

void expect_vec_near(CaptureVec3f actual, CaptureVec3f expected, float tolerance = 2.0e-5f)
{
	EXPECT_NEAR(expected.x, actual.x, tolerance);
	EXPECT_NEAR(expected.y, actual.y, tolerance);
	EXPECT_NEAR(expected.z, actual.z, tolerance);
}

void expect_canonical_quaternion(CaptureQuaternionf actual)
{
	const std::array<float, 4U> canonical_components{{actual.w, actual.x, actual.y, actual.z}};
	for (const auto component : canonical_components) {
		if (component == 0.0f) {
			EXPECT_FALSE(std::signbit(component));
		}
	}
	EXPECT_TRUE(actual.w > 0.0f || actual.w == 0.0f);
	if (actual.w == 0.0f) {
		for (std::size_t component = 1U; component < canonical_components.size(); ++component) {
			if (canonical_components[component] != 0.0f) {
				EXPECT_GT(canonical_components[component], 0.0f);
				break;
			}
		}
	}
	EXPECT_GE(quaternion_norm(actual), QuaternionNormMinimum);
	EXPECT_LE(quaternion_norm(actual), QuaternionNormMaximum);
}

void expect_quaternion_equivalent(CaptureQuaternionf actual,
	CaptureQuaternionf expected,
	float tolerance = 2.0e-5f)
{
	expect_canonical_quaternion(actual);
	actual = normalized(actual);
	expected = normalized(expected);
	const auto alignment = actual.w * expected.w + actual.x * expected.x + actual.y * expected.y +
		actual.z * expected.z;
	EXPECT_NEAR(1.0f, std::fabs(alignment), tolerance);
	EXPECT_GE(quaternion_norm(actual), QuaternionNormMinimum);
	EXPECT_LE(quaternion_norm(actual), QuaternionNormMaximum);
}

detail::QuaternionConversionStatus convert(const CaptureOrientationBasis& input,
	CaptureQuaternionf& output) noexcept
{
	return detail::convert_fso_orientation_to_local_to_world(input, output);
}

detail::CaptureResult capture_result(detail::CaptureStatus status, detail::CaptureReason reason) noexcept
{
	detail::CaptureResult result{};
	result.status = status;
	result.reason = reason;
	return result;
}

TEST(TelemetryEngineAdapterContract, ValueDtoAndCaptureResultContractsExist)
{
	using detail::CaptureReason;
	using detail::CaptureStatus;
	using detail::QuaternionConversionStatus;
	static_assert(static_cast<std::uint8_t>(CaptureStatus::Valid) == 0U);
	static_assert(static_cast<std::uint8_t>(CaptureStatus::NoPlayer) == 1U);
	static_assert(static_cast<std::uint8_t>(CaptureStatus::InvalidSource) == 2U);
	static_assert(static_cast<std::uint8_t>(CaptureStatus::Count) == 3U);
	static_assert(static_cast<std::uint8_t>(CaptureReason::None) == 0U);
	static_assert(static_cast<std::uint8_t>(CaptureReason::NotInMission) == 1U);
	static_assert(static_cast<std::uint8_t>(CaptureReason::MissingPlayer) == 2U);
	static_assert(static_cast<std::uint8_t>(CaptureReason::MissingPlayerObject) == 3U);
	static_assert(static_cast<std::uint8_t>(CaptureReason::MissingPlayerShip) == 4U);
	static_assert(static_cast<std::uint8_t>(CaptureReason::WrongObjectType) == 5U);
	static_assert(static_cast<std::uint8_t>(CaptureReason::ShipInstanceOutOfRange) == 6U);
	static_assert(static_cast<std::uint8_t>(CaptureReason::PlayerObjectMismatch) == 7U);
	static_assert(static_cast<std::uint8_t>(CaptureReason::PlayerShipMismatch) == 8U);
	static_assert(static_cast<std::uint8_t>(CaptureReason::InvalidObservationKey) == 9U);
	static_assert(static_cast<std::uint8_t>(CaptureReason::InvalidPosition) == 10U);
	static_assert(static_cast<std::uint8_t>(CaptureReason::InvalidOrientation) == 11U);
	static_assert(static_cast<std::uint8_t>(CaptureReason::InvalidVelocity) == 12U);
	static_assert(static_cast<std::uint8_t>(CaptureReason::InvalidRotationalVelocity) == 13U);
	static_assert(static_cast<std::uint8_t>(CaptureReason::InvalidRadius) == 14U);
	static_assert(static_cast<std::uint8_t>(CaptureReason::Count) == 15U);
	static_assert(static_cast<std::uint8_t>(QuaternionConversionStatus::Converted) == 0U);
	static_assert(static_cast<std::uint8_t>(QuaternionConversionStatus::NonFiniteInput) == 1U);
	static_assert(static_cast<std::uint8_t>(QuaternionConversionStatus::DegenerateInput) == 2U);
	static_assert(static_cast<std::uint8_t>(QuaternionConversionStatus::NonFiniteResult) == 3U);
	static_assert(static_cast<std::uint8_t>(QuaternionConversionStatus::QuantizedNormOutOfRange) == 4U);
	static_assert(static_cast<std::uint8_t>(QuaternionConversionStatus::Count) == 5U);

	const CaptureVec3f default_vector{};
	EXPECT_FLOAT_EQ(0.0f, default_vector.x);
	EXPECT_FLOAT_EQ(0.0f, default_vector.y);
	EXPECT_FLOAT_EQ(0.0f, default_vector.z);
	EXPECT_FALSE(std::signbit(default_vector.x));
	EXPECT_FALSE(std::signbit(default_vector.y));
	EXPECT_FALSE(std::signbit(default_vector.z));
	const CaptureQuaternionf default_quaternion{};
	EXPECT_FLOAT_EQ(1.0f, default_quaternion.w);
	EXPECT_FLOAT_EQ(0.0f, default_quaternion.x);
	EXPECT_FLOAT_EQ(0.0f, default_quaternion.y);
	EXPECT_FLOAT_EQ(0.0f, default_quaternion.z);
	EXPECT_FALSE(std::signbit(default_quaternion.x));
	EXPECT_FALSE(std::signbit(default_quaternion.y));
	EXPECT_FALSE(std::signbit(default_quaternion.z));
	const CaptureOrientationBasis default_basis{};
	expect_vec_near(default_basis.right_world, vec(1.0f, 0.0f, 0.0f), 0.0f);
	expect_vec_near(default_basis.up_world, vec(0.0f, 1.0f, 0.0f), 0.0f);
	expect_vec_near(default_basis.forward_world, vec(0.0f, 0.0f, 1.0f), 0.0f);
	const detail::CaptureResult default_result{};
	EXPECT_EQ(CaptureStatus::InvalidSource, default_result.status);
	EXPECT_EQ(CaptureReason::InvalidObservationKey, default_result.reason);
	const detail::PlayerObservationDto default_observation{};
	EXPECT_EQ(0U, default_observation.key.object_signature);
	EXPECT_EQ(0U, default_observation.value.producer_sample_time_us);
	EXPECT_FLOAT_EQ(0.0f, default_observation.value.position_world.x);
	EXPECT_FLOAT_EQ(1.0f, default_observation.value.orientation_local_to_world.w);
	EXPECT_FLOAT_EQ(0.0f, default_observation.value.velocity_world.x);
	EXPECT_FLOAT_EQ(0.0f, default_observation.value.rotational_velocity_local.x);
	EXPECT_FLOAT_EQ(0.0f, default_observation.value.radius);
	EXPECT_EQ(0U, default_observation.value.physics_mode_flags);
	const detail::PlayerKinematicsSample default_sample{};
	EXPECT_EQ(0U, default_sample.entity_id);
	EXPECT_EQ(0U, default_sample.value.producer_sample_time_us);
	const detail::EnginePhysicsFlagInput default_flags{};
	EXPECT_EQ(0U, default_flags.raw_physics_flags);
	EXPECT_FALSE(default_flags.object_immobile);
	EXPECT_FALSE(default_flags.object_position_locked);
	EXPECT_FALSE(default_flags.object_orientation_locked);

	const std::array<detail::CaptureResult, 15U> allowed_results{{
		capture_result(CaptureStatus::Valid, CaptureReason::None),
		capture_result(CaptureStatus::NoPlayer, CaptureReason::NotInMission),
		capture_result(CaptureStatus::NoPlayer, CaptureReason::MissingPlayer),
		capture_result(CaptureStatus::NoPlayer, CaptureReason::MissingPlayerObject),
		capture_result(CaptureStatus::NoPlayer, CaptureReason::MissingPlayerShip),
		capture_result(CaptureStatus::InvalidSource, CaptureReason::WrongObjectType),
		capture_result(CaptureStatus::InvalidSource, CaptureReason::ShipInstanceOutOfRange),
		capture_result(CaptureStatus::InvalidSource, CaptureReason::PlayerObjectMismatch),
		capture_result(CaptureStatus::InvalidSource, CaptureReason::PlayerShipMismatch),
		capture_result(CaptureStatus::InvalidSource, CaptureReason::InvalidObservationKey),
		capture_result(CaptureStatus::InvalidSource, CaptureReason::InvalidPosition),
		capture_result(CaptureStatus::InvalidSource, CaptureReason::InvalidOrientation),
		capture_result(CaptureStatus::InvalidSource, CaptureReason::InvalidVelocity),
		capture_result(CaptureStatus::InvalidSource, CaptureReason::InvalidRotationalVelocity),
		capture_result(CaptureStatus::InvalidSource, CaptureReason::InvalidRadius)}};
	for (const auto& result : allowed_results) {
		EXPECT_LT(static_cast<std::uint8_t>(result.status), static_cast<std::uint8_t>(CaptureStatus::Count));
		EXPECT_LT(static_cast<std::uint8_t>(result.reason), static_cast<std::uint8_t>(CaptureReason::Count));
		EXPECT_EQ(result.status == CaptureStatus::Valid, result.reason == CaptureReason::None);
		EXPECT_EQ(result.status == CaptureStatus::NoPlayer,
			result.reason >= CaptureReason::NotInMission && result.reason <= CaptureReason::MissingPlayerShip);
		EXPECT_EQ(result.status == CaptureStatus::InvalidSource,
			result.reason >= CaptureReason::WrongObjectType && result.reason < CaptureReason::Count);
	}

	detail::PlayerObservationDto observation{};
	observation.key.object_signature = 0x12345678U;
	observation.value.producer_sample_time_us = 42U;
	observation.value.position_world = vec(1.0f, 2.0f, 3.0f);
	observation.value.orientation_local_to_world = quat(1.0f, 0.0f, 0.0f, 0.0f);
	observation.value.velocity_world = vec(4.0f, 5.0f, 6.0f);
	observation.value.rotational_velocity_local = vec(7.0f, 8.0f, 9.0f);
	observation.value.radius = 10.0f;
	observation.value.physics_mode_flags = protocol::PhysicsModeFlagAfterburner;
	const auto copy = observation;
	observation.key.object_signature = 0U;
	observation.value.position_world.x = -1.0f;
	EXPECT_EQ(0x12345678U, copy.key.object_signature);
	EXPECT_EQ(42U, copy.value.producer_sample_time_us);
	EXPECT_FLOAT_EQ(1.0f, copy.value.position_world.x);
	EXPECT_FLOAT_EQ(10.0f, copy.value.radius);
	EXPECT_EQ(protocol::PhysicsModeFlagAfterburner, copy.value.physics_mode_flags);

	detail::PlayerKinematicsSample sample{};
	sample.entity_id = 7U;
	sample.value = copy.value;
	EXPECT_EQ(7U, sample.entity_id);
	EXPECT_EQ(copy.value.producer_sample_time_us, sample.value.producer_sample_time_us);
}

TEST(TelemetryEngineAdapterContract, IdentityAndQuarterTurnsCoverTraceAndAxisConventions)
{
	const auto root_half = std::sqrt(0.5f);
	const std::array<CaptureQuaternionf, 7U> cases{{quat(1.0f, 0.0f, 0.0f, 0.0f),
		quat(root_half, root_half, 0.0f, 0.0f),
		quat(root_half, -root_half, 0.0f, 0.0f),
		quat(root_half, 0.0f, root_half, 0.0f),
		quat(root_half, 0.0f, -root_half, 0.0f),
		quat(root_half, 0.0f, 0.0f, root_half),
		quat(root_half, 0.0f, 0.0f, -root_half)}};
	for (const auto expected : cases) {
		const auto input = basis_from_quaternion(expected);
		CaptureQuaternionf output{};
		ASSERT_EQ(detail::QuaternionConversionStatus::Converted, convert(input, output));
		expect_quaternion_equivalent(output, expected);
		expect_vec_near(rotate_local_axis(output, vec(1.0f, 0.0f, 0.0f)), input.right_world);
		expect_vec_near(rotate_local_axis(output, vec(0.0f, 1.0f, 0.0f)), input.up_world);
		expect_vec_near(rotate_local_axis(output, vec(0.0f, 0.0f, 1.0f)), input.forward_world);
	}
}

TEST(TelemetryEngineAdapterContract, HalfTurnsCoverEveryDominantDiagonalBranch)
{
	const std::array<CaptureQuaternionf, 3U> cases{{quat(0.0f, 1.0f, 0.0f, 0.0f),
		quat(0.0f, 0.0f, 1.0f, 0.0f),
		quat(0.0f, 0.0f, 0.0f, 1.0f)}};
	for (const auto expected : cases) {
		CaptureQuaternionf output{};
		ASSERT_EQ(detail::QuaternionConversionStatus::Converted,
			convert(basis_from_quaternion(expected), output));
		expect_quaternion_equivalent(output, expected);
		EXPECT_FALSE(std::signbit(output.w));
		const std::array<float, 3U> vector{{output.x, output.y, output.z}};
		for (const auto component : vector) {
			if (component != 0.0f) {
				EXPECT_GT(component, 0.0f);
				break;
			}
		}
	}
}

TEST(TelemetryEngineAdapterContract, SlightNoiseNormalizesAndPreservesLocalToWorldAxes)
{
	const std::array<CaptureQuaternionf, 4U> cases{{quat(0.8f, 0.2f, -0.3f, 0.45f),
		quat(0.1f, 0.92f, 0.2f, -0.1f),
		quat(0.1f, -0.1f, 0.94f, 0.2f),
		quat(0.1f, 0.2f, -0.1f, 0.94f)}};
	for (const auto expected : cases) {
		auto input = basis_from_quaternion(expected);
		input.right_world.x += 1.0e-6f;
		input.up_world.y -= 1.0e-6f;
		input.forward_world.z += 1.0e-6f;
		CaptureQuaternionf output{};
		ASSERT_EQ(detail::QuaternionConversionStatus::Converted, convert(input, output));
		expect_quaternion_equivalent(output, expected, 5.0e-5f);
		EXPECT_GE(quaternion_norm(output), QuaternionNormMinimum);
		EXPECT_LE(quaternion_norm(output), QuaternionNormMaximum);
	}
}

TEST(TelemetryEngineAdapterContract, SignTieNegativeZeroAndEquivalentInputsCanonicalizeIdentically)
{
	const auto positive = normalized(quat(-0.0f, -1.0f, -0.0f, 0.0f));
	const auto negative = quat(-positive.w, -positive.x, -positive.y, -positive.z);
	CaptureQuaternionf first{};
	CaptureQuaternionf second{};
	ASSERT_EQ(detail::QuaternionConversionStatus::Converted,
		convert(basis_from_quaternion(positive), first));
	ASSERT_EQ(detail::QuaternionConversionStatus::Converted,
		convert(basis_from_quaternion(negative), second));
	expect_canonical_quaternion(first);
	expect_canonical_quaternion(second);
	EXPECT_FLOAT_EQ(first.w, second.w);
	EXPECT_FLOAT_EQ(first.x, second.x);
	EXPECT_FLOAT_EQ(first.y, second.y);
	EXPECT_FLOAT_EQ(first.z, second.z);
	EXPECT_FALSE(std::signbit(first.w));
	EXPECT_GT(first.x, 0.0f);
	EXPECT_GE(quaternion_norm(first), QuaternionNormMinimum);
	EXPECT_LE(quaternion_norm(first), QuaternionNormMaximum);
}

bool same_quaternion(CaptureQuaternionf left, CaptureQuaternionf right) noexcept
{
	return left.w == right.w && left.x == right.x && left.y == right.y && left.z == right.z;
}

TEST(TelemetryEngineAdapterContract, EveryNonFiniteCoefficientIsRejectedTransactionally)
{
	const std::array<float, 3U> hostile{{std::numeric_limits<float>::quiet_NaN(),
		std::numeric_limits<float>::infinity(),
		-std::numeric_limits<float>::infinity()}};
	for (const auto value : hostile) {
		for (std::size_t coefficient = 0U; coefficient < 9U; ++coefficient) {
			auto input = basis_from_quaternion(quat(1.0f, 0.0f, 0.0f, 0.0f));
			float* fields[] = {&input.right_world.x,
				&input.right_world.y,
				&input.right_world.z,
				&input.up_world.x,
				&input.up_world.y,
				&input.up_world.z,
				&input.forward_world.x,
				&input.forward_world.y,
				&input.forward_world.z};
			*fields[coefficient] = value;
			const auto sentinel = quat(0.25f, 0.5f, 0.75f, 1.0f);
			auto output = sentinel;
			EXPECT_EQ(detail::QuaternionConversionStatus::NonFiniteInput, convert(input, output));
			EXPECT_TRUE(same_quaternion(CaptureQuaternionf{}, output));
		}
	}
}

TEST(TelemetryEngineAdapterContract, DegenerateCollinearAndOverflowBasesAreRejectedTransactionally)
{
	const auto maximum = std::numeric_limits<float>::max();
	const std::array<CaptureOrientationBasis, 3U> degenerate{{basis(vec(0.0f, 0.0f, 0.0f),
		vec(0.0f, 0.0f, 0.0f),
		vec(0.0f, 0.0f, 0.0f)),
		basis(vec(1.0f, 0.0f, 0.0f), vec(2.0f, 0.0f, 0.0f), vec(0.0f, 0.0f, 1.0f)),
		basis(vec(1.0f, 0.0f, 0.0f), vec(0.0f, 1.0f, 0.0f), vec(1.0f, 1.0f, 0.0f))}};
	for (const auto& input : degenerate) {
		const auto sentinel = quat(0.25f, 0.5f, 0.75f, 1.0f);
		auto output = sentinel;
		EXPECT_EQ(detail::QuaternionConversionStatus::DegenerateInput, convert(input, output));
		EXPECT_TRUE(same_quaternion(CaptureQuaternionf{}, output));
	}
	const auto overflowing = basis(vec(maximum, maximum, maximum),
		vec(maximum, -maximum, maximum),
		vec(-maximum, maximum, maximum));
	auto output = quat(0.25f, 0.5f, 0.75f, 1.0f);
	EXPECT_EQ(detail::QuaternionConversionStatus::NonFiniteResult, convert(overflowing, output));
	EXPECT_TRUE(same_quaternion(CaptureQuaternionf{}, output));
}

std::uint32_t mapped(std::uint32_t raw,
	bool immobile = false,
	bool position_locked = false,
	bool orientation_locked = false) noexcept
{
	detail::EnginePhysicsFlagInput input{};
	input.raw_physics_flags = raw;
	input.object_immobile = immobile;
	input.object_position_locked = position_locked;
	input.object_orientation_locked = orientation_locked;
	return detail::map_player_physics_mode_flags(input);
}

TEST(TelemetryEngineAdapterContract, EveryRecognizedEnginePhysicsFlagMapsToOneClosedFstlBit)
{
	struct Mapping { std::uint32_t engine; std::uint32_t fstl; };
	const std::array<Mapping, 12U> mappings{{{PF_AFTERBURNER_ON, protocol::PhysicsModeFlagAfterburner},
		{PF_BOOSTER_ON, protocol::PhysicsModeFlagBooster},
		{PF_GLIDING, protocol::PhysicsModeFlagGlideActive},
		{PF_FORCE_GLIDE, protocol::PhysicsModeFlagGlideForced},
		{PF_NEWTONIAN_DAMP, protocol::PhysicsModeFlagNewtonianDamping},
		{PF_SLIDE_ENABLED, protocol::PhysicsModeFlagLateralTranslation},
		{PF_WARP_IN, protocol::PhysicsModeFlagWarpIn},
		{PF_SUPERCAP_WARP_IN, protocol::PhysicsModeFlagWarpIn},
		{PF_WARP_OUT, protocol::PhysicsModeFlagWarpOut},
		{PF_SUPERCAP_WARP_OUT, protocol::PhysicsModeFlagWarpOut},
		{PF_SCRIPTED_VELOCITY, protocol::PhysicsModeFlagScripted},
		{PF_IN_SHOCKWAVE, protocol::PhysicsModeFlagShockwave}}};
	for (const auto& mapping : mappings) {
		EXPECT_EQ(mapping.fstl, mapped(mapping.engine));
	}
}

TEST(TelemetryEngineAdapterContract, KnownCombinationsLocksAndUnknownBitsNeverEscapeTheClosedBitmap)
{
	const auto all_known_engine = static_cast<std::uint32_t>(PF_AFTERBURNER_ON | PF_BOOSTER_ON | PF_GLIDING |
		PF_FORCE_GLIDE | PF_NEWTONIAN_DAMP | PF_SLIDE_ENABLED | PF_WARP_IN | PF_SUPERCAP_WARP_IN |
		PF_WARP_OUT | PF_SUPERCAP_WARP_OUT | PF_SCRIPTED_VELOCITY | PF_IN_SHOCKWAVE);
	EXPECT_EQ(protocol::KnownPhysicsModeFlags,
		mapped(all_known_engine, true, true, true));
	EXPECT_EQ(protocol::PhysicsModeFlagImmobile | protocol::PhysicsModeFlagOrientationLocked,
		mapped(0U, true, false, false));
	EXPECT_EQ(protocol::PhysicsModeFlagImmobile, mapped(0U, false, true, false));
	EXPECT_EQ(protocol::PhysicsModeFlagOrientationLocked, mapped(0U, false, false, true));
	EXPECT_EQ(protocol::PhysicsModeFlagImmobile | protocol::PhysicsModeFlagOrientationLocked,
		mapped(0U, false, true, true));

	const std::array<std::uint32_t, 8U> unmapped_engine_bits{{PF_ACCELERATES,
		PF_USE_VEL,
		PF_REDUCED_DAMP,
		PF_DEAD_DAMP,
		PF_AFTERBURNER_WAIT,
		PF_CONST_VEL,
		PF_MANEUVER_NO_DAMP,
		PF_BALLISTIC}};
	constexpr auto known_engine = static_cast<std::uint32_t>(PF_AFTERBURNER_ON | PF_GLIDING | PF_WARP_OUT);
	constexpr auto known_output = protocol::PhysicsModeFlagAfterburner | protocol::PhysicsModeFlagGlideActive |
		protocol::PhysicsModeFlagWarpOut;
	for (const auto unmapped_bit : unmapped_engine_bits) {
		EXPECT_EQ(protocol::PhysicsModeFlagNone, mapped(unmapped_bit));
		EXPECT_EQ(known_output, mapped(known_engine | unmapped_bit));
	}
	EXPECT_EQ(0U, mapped(std::numeric_limits<std::uint32_t>::max()) & protocol::ReservedPhysicsModeFlags);
}

template <typename T, typename = void>
struct has_encode_api : std::false_type {};

template <typename T>
struct has_encode_api<T, std::void_t<decltype(std::declval<T&>().encode())>> : std::true_type {};

template <typename T, typename = void>
struct has_capture_snapshot_api : std::false_type {};

template <typename T>
struct has_capture_snapshot_api<T, std::void_t<decltype(std::declval<T&>().capture_snapshot())>>
	: std::true_type {};

template <typename T, typename = void>
struct has_set_current_state_api : std::false_type {};

template <typename T>
struct has_set_current_state_api<T, std::void_t<decltype(std::declval<T&>().set_current_state())>>
	: std::true_type {};

template <typename T, typename = void>
struct has_session_ownership_surface : std::false_type {};

template <typename T>
struct has_session_ownership_surface<T, std::void_t<decltype(std::declval<T&>().session_id)>>
	: std::true_type {};

TEST(TelemetryEngineAdapterContract, Wp07ABoundariesExcludeSessionWireRuntimeAndOwnership)
{
	constexpr auto has_forbidden_surface = has_encode_api<detail::PlayerObservationDto>::value ||
		has_capture_snapshot_api<detail::PlayerObservationDto>::value ||
		has_set_current_state_api<detail::PlayerObservationDto>::value ||
		has_session_ownership_surface<detail::PlayerObservationDto>::value;
	EXPECT_FALSE(has_encode_api<detail::PlayerObservationDto>::value);
	EXPECT_FALSE(has_capture_snapshot_api<detail::PlayerObservationDto>::value);
	EXPECT_FALSE(has_set_current_state_api<detail::PlayerObservationDto>::value);
	EXPECT_FALSE(has_session_ownership_surface<detail::PlayerObservationDto>::value);
	EXPECT_FALSE(has_forbidden_surface);
	EXPECT_TRUE(std::is_nothrow_default_constructible_v<detail::PlayerObservationDto>);
	EXPECT_TRUE(std::is_nothrow_copy_constructible_v<detail::PlayerObservationDto>);
	EXPECT_TRUE(std::is_nothrow_copy_assignable_v<detail::PlayerObservationDto>);
}

#endif

} // namespace
