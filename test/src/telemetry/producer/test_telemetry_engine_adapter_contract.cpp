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

namespace telemetry::detail {
struct EnginePlayerKinematicsRead;
class FsoEngineReadView;
FsoEngineReadView make_fso_engine_read_view() noexcept;
}

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

template <typename T, typename = void>
struct is_complete_type : std::false_type {};

template <typename T>
struct is_complete_type<T, std::void_t<decltype(sizeof(T))>> : std::true_type {};

template <typename View, typename Raw, typename FsoView>
struct EngineCollectorContract {
	class CountedView final : public View {
	  public:
		std::array<bool, 8U> gates{{true, true, true, true, true, true, true, true}};
		Raw raw{};
		mutable std::array<std::size_t, 9U> calls{};
		mutable std::array<std::size_t, 9U> order{};
		mutable std::size_t order_size = 0U;
		mutable std::size_t total_calls = 0U;
		mutable bool order_overflow = false;

		bool in_mission() const noexcept override { return predicate(0U); }
		bool player_exists() const noexcept override { return predicate(1U); }
		bool player_object_exists() const noexcept override { return predicate(2U); }
		bool player_ship_exists() const noexcept override { return predicate(3U); }
		bool player_object_is_ship() const noexcept override { return predicate(4U); }
		bool player_object_ship_instance_in_range() const noexcept override { return predicate(5U); }
		bool player_object_matches_player() const noexcept override { return predicate(6U); }
		bool player_ship_matches_object() const noexcept override { return predicate(7U); }
		void read_player_kinematics(Raw& output) const noexcept override
		{
			record(8U);
			output = raw;
		}

	  private:
		bool predicate(std::size_t index) const noexcept
		{
			record(index);
			return gates[index];
		}
		void record(std::size_t index) const noexcept
		{
			++calls[index];
			++total_calls;
			if (order_size < order.size()) {
				order[order_size++] = index;
			} else {
				order_overflow = true;
			}
		}
	};

	static Raw valid_raw() noexcept
	{
		Raw raw{};
		raw.object_signature = 123;
		raw.position_world.x = 1.0f;
		raw.position_world.y = 2.0f;
		raw.position_world.z = 3.0f;
		raw.velocity_world.x = 4.0f;
		raw.velocity_world.y = 5.0f;
		raw.velocity_world.z = 6.0f;
		raw.rotational_velocity_local.x = 7.0f;
		raw.rotational_velocity_local.y = 8.0f;
		raw.rotational_velocity_local.z = 9.0f;
		raw.radius = 10.0f;
		const auto root_half = std::sqrt(0.5f);
		const auto inverse_axis_norm = 1.0f / std::sqrt(14.0f);
		raw.orientation = basis_from_quaternion(quat(root_half,
			root_half * inverse_axis_norm,
			2.0f * root_half * inverse_axis_norm,
			3.0f * root_half * inverse_axis_norm));
		raw.physics.raw_physics_flags = PF_AFTERBURNER_ON | PF_WARP_OUT;
		raw.physics.object_orientation_locked = true;
		return raw;
	}

	static void expect_default(const detail::PlayerObservationDto& output)
	{
		const detail::PlayerObservationDto expected{};
		EXPECT_EQ(expected.key.object_signature, output.key.object_signature);
		EXPECT_EQ(expected.value.producer_sample_time_us, output.value.producer_sample_time_us);
		const std::array<float, 14U> actual{{output.value.position_world.x,
			output.value.position_world.y,
			output.value.position_world.z,
			output.value.orientation_local_to_world.w,
			output.value.orientation_local_to_world.x,
			output.value.orientation_local_to_world.y,
			output.value.orientation_local_to_world.z,
			output.value.velocity_world.x,
			output.value.velocity_world.y,
			output.value.velocity_world.z,
			output.value.rotational_velocity_local.x,
			output.value.rotational_velocity_local.y,
			output.value.rotational_velocity_local.z,
			output.value.radius}};
		const std::array<float, 14U> defaults{{expected.value.position_world.x,
			expected.value.position_world.y,
			expected.value.position_world.z,
			expected.value.orientation_local_to_world.w,
			expected.value.orientation_local_to_world.x,
			expected.value.orientation_local_to_world.y,
			expected.value.orientation_local_to_world.z,
			expected.value.velocity_world.x,
			expected.value.velocity_world.y,
			expected.value.velocity_world.z,
			expected.value.rotational_velocity_local.x,
			expected.value.rotational_velocity_local.y,
			expected.value.rotational_velocity_local.z,
			expected.value.radius}};
		for (std::size_t field = 0U; field < actual.size(); ++field) {
			EXPECT_FLOAT_EQ(defaults[field], actual[field]);
			if (defaults[field] == 0.0f) EXPECT_FALSE(std::signbit(actual[field]));
		}
		EXPECT_EQ(0U, output.value.physics_mode_flags);
	}

	static detail::CaptureResult collect(CountedView& view, detail::PlayerObservationDto& output)
	{
		return detail::collect_player_kinematics(view, 0x1020304050607080ULL, output);
	}

	static void api_and_real_view()
	{
		static_assert(std::has_virtual_destructor_v<View> && std::is_abstract_v<View>);
		static_assert(std::is_final_v<FsoView> && std::is_base_of_v<View, FsoView>);
		static_assert(std::is_nothrow_default_constructible_v<FsoView>);
		static_assert(std::is_standard_layout_v<Raw> && std::is_trivially_copyable_v<Raw>);
		static_assert(std::is_same_v<decltype(std::declval<Raw>().object_signature), std::int32_t>);
		static_assert(std::is_same_v<decltype(std::declval<Raw>().position_world), CaptureVec3f>);
		static_assert(std::is_same_v<decltype(std::declval<Raw>().orientation), CaptureOrientationBasis>);
		static_assert(std::is_same_v<decltype(std::declval<Raw>().velocity_world), CaptureVec3f>);
		static_assert(std::is_same_v<decltype(std::declval<Raw>().rotational_velocity_local), CaptureVec3f>);
		static_assert(std::is_same_v<decltype(std::declval<Raw>().radius), float>);
		static_assert(std::is_same_v<decltype(std::declval<Raw>().physics), detail::EnginePhysicsFlagInput>);
		static_assert(noexcept(detail::make_fso_engine_read_view()));
		static_assert(std::is_same_v<decltype(detail::make_fso_engine_read_view()), FsoView>);
		const Raw defaults{};
		EXPECT_EQ(0, defaults.object_signature);
		expect_vec_near(defaults.position_world, vec(0.0f, 0.0f, 0.0f), 0.0f);
		expect_vec_near(defaults.orientation.right_world, vec(1.0f, 0.0f, 0.0f), 0.0f);
		expect_vec_near(defaults.orientation.up_world, vec(0.0f, 1.0f, 0.0f), 0.0f);
		expect_vec_near(defaults.orientation.forward_world, vec(0.0f, 0.0f, 1.0f), 0.0f);
		expect_vec_near(defaults.velocity_world, vec(0.0f, 0.0f, 0.0f), 0.0f);
		expect_vec_near(defaults.rotational_velocity_local, vec(0.0f, 0.0f, 0.0f), 0.0f);
		EXPECT_FLOAT_EQ(0.0f, defaults.radius);
		EXPECT_EQ(0U, defaults.physics.raw_physics_flags);
		EXPECT_FALSE(defaults.physics.object_immobile);
		EXPECT_FALSE(defaults.physics.object_position_locked);
		EXPECT_FALSE(defaults.physics.object_orientation_locked);
		const FsoView real = detail::make_fso_engine_read_view();
		(void)real;
	}

	static void preconditions()
	{
		constexpr std::array<detail::CaptureReason, 8U> reasons{{detail::CaptureReason::NotInMission,
			detail::CaptureReason::MissingPlayer,
			detail::CaptureReason::MissingPlayerObject,
			detail::CaptureReason::MissingPlayerShip,
			detail::CaptureReason::WrongObjectType,
			detail::CaptureReason::ShipInstanceOutOfRange,
			detail::CaptureReason::PlayerObjectMismatch,
			detail::CaptureReason::PlayerShipMismatch}};
		for (std::size_t failure = 0U; failure < reasons.size(); ++failure) {
			CountedView view;
			view.gates[failure] = false;
			detail::PlayerObservationDto output{};
			output.key.object_signature = 999U;
			const auto result = collect(view, output);
			EXPECT_EQ(failure < 4U ? detail::CaptureStatus::NoPlayer : detail::CaptureStatus::InvalidSource,
				result.status);
			EXPECT_EQ(reasons[failure], result.reason);
			for (std::size_t call = 0U; call < view.calls.size(); ++call) {
				EXPECT_EQ(call <= failure ? 1U : 0U, view.calls[call]);
			}
			ASSERT_EQ(failure + 1U, view.order_size);
			EXPECT_EQ(failure + 1U, view.total_calls);
			EXPECT_FALSE(view.order_overflow);
			for (std::size_t call = 0U; call <= failure; ++call) EXPECT_EQ(call, view.order[call]);
			expect_default(output);
		}
	}

	static void exact_snapshot()
	{
		CountedView view;
		view.raw = valid_raw();
		detail::PlayerObservationDto output{};
		const auto result = collect(view, output);
		EXPECT_EQ(detail::CaptureStatus::Valid, result.status);
		EXPECT_EQ(detail::CaptureReason::None, result.reason);
		for (const auto count : view.calls) EXPECT_EQ(1U, count);
		ASSERT_EQ(9U, view.order_size);
		EXPECT_EQ(9U, view.total_calls);
		EXPECT_FALSE(view.order_overflow);
		for (std::size_t call = 0U; call < view.order_size; ++call) EXPECT_EQ(call, view.order[call]);
		EXPECT_EQ(123U, output.key.object_signature);
		EXPECT_EQ(0x1020304050607080ULL, output.value.producer_sample_time_us);
		EXPECT_FLOAT_EQ(1.0f, output.value.position_world.x);
		EXPECT_FLOAT_EQ(2.0f, output.value.position_world.y);
		EXPECT_FLOAT_EQ(3.0f, output.value.position_world.z);
		EXPECT_FLOAT_EQ(4.0f, output.value.velocity_world.x);
		EXPECT_FLOAT_EQ(8.0f, output.value.rotational_velocity_local.y);
		EXPECT_FLOAT_EQ(10.0f, output.value.radius);
		const auto root_half = std::sqrt(0.5f);
		const auto inverse_axis_norm = 1.0f / std::sqrt(14.0f);
		EXPECT_NEAR(root_half, output.value.orientation_local_to_world.w, 2.0e-5f);
		EXPECT_NEAR(root_half * inverse_axis_norm, output.value.orientation_local_to_world.x, 2.0e-5f);
		EXPECT_NEAR(2.0f * root_half * inverse_axis_norm,
			output.value.orientation_local_to_world.y,
			2.0e-5f);
		EXPECT_NEAR(3.0f * root_half * inverse_axis_norm,
			output.value.orientation_local_to_world.z,
			2.0e-5f);
		expect_vec_near(rotate_local_axis(output.value.orientation_local_to_world, vec(1.0f, 0.0f, 0.0f)),
			view.raw.orientation.right_world);
		expect_vec_near(rotate_local_axis(output.value.orientation_local_to_world, vec(0.0f, 1.0f, 0.0f)),
			view.raw.orientation.up_world);
		expect_vec_near(rotate_local_axis(output.value.orientation_local_to_world, vec(0.0f, 0.0f, 1.0f)),
			view.raw.orientation.forward_world);
		EXPECT_EQ(protocol::PhysicsModeFlagAfterburner | protocol::PhysicsModeFlagWarpOut |
				protocol::PhysicsModeFlagOrientationLocked,
			output.value.physics_mode_flags);
	}

	static void validation_ladder()
	{
		CountedView view;
		view.raw = valid_raw();
		const auto infinity = std::numeric_limits<float>::infinity();
		view.raw.object_signature = 0;
		view.raw.position_world.x = infinity;
		view.raw.orientation.right_world = {};
		view.raw.velocity_world.x = infinity;
		view.raw.rotational_velocity_local.x = infinity;
		view.raw.radius = -1.0f;
		constexpr std::array<detail::CaptureReason, 6U> reasons{{detail::CaptureReason::InvalidObservationKey,
			detail::CaptureReason::InvalidPosition,
			detail::CaptureReason::InvalidOrientation,
			detail::CaptureReason::InvalidVelocity,
			detail::CaptureReason::InvalidRotationalVelocity,
			detail::CaptureReason::InvalidRadius}};
		for (std::size_t stage = 0U; stage < reasons.size(); ++stage) {
			detail::PlayerObservationDto output{};
			output.value.radius = 99.0f;
			const auto result = collect(view, output);
			EXPECT_EQ(detail::CaptureStatus::InvalidSource, result.status);
			EXPECT_EQ(reasons[stage], result.reason);
			expect_default(output);
			if (stage == 0U) view.raw.object_signature = 123;
			if (stage == 1U) view.raw.position_world.x = 1.0f;
			if (stage == 2U) view.raw.orientation = {};
			if (stage == 3U) view.raw.velocity_world.x = 1.0f;
			if (stage == 4U) view.raw.rotational_velocity_local.x = 1.0f;
		}
	}

	static detail::CaptureReason reason_for_group(std::size_t group) noexcept
	{
		constexpr std::array<detail::CaptureReason, 4U> reasons{{detail::CaptureReason::InvalidPosition,
			detail::CaptureReason::InvalidVelocity,
			detail::CaptureReason::InvalidRotationalVelocity,
			detail::CaptureReason::InvalidRadius}};
		return reasons[group];
	}

	static float* component(Raw& raw, std::size_t group, std::size_t index) noexcept
	{
		if (group == 0U) return index == 0U ? &raw.position_world.x : index == 1U ? &raw.position_world.y : &raw.position_world.z;
		if (group == 1U) return index == 0U ? &raw.velocity_world.x : index == 1U ? &raw.velocity_world.y : &raw.velocity_world.z;
		return index == 0U ? &raw.rotational_velocity_local.x : index == 1U ? &raw.rotational_velocity_local.y : &raw.rotational_velocity_local.z;
	}
	static const float* output_component(const detail::PlayerKinematicsValue& value,
		std::size_t group,
		std::size_t index) noexcept
	{
		if (group == 0U) return index == 0U ? &value.position_world.x : index == 1U ? &value.position_world.y : &value.position_world.z;
		if (group == 1U) return index == 0U ? &value.velocity_world.x : index == 1U ? &value.velocity_world.y : &value.velocity_world.z;
		return index == 0U ? &value.rotational_velocity_local.x : index == 1U ? &value.rotational_velocity_local.y : &value.rotational_velocity_local.z;
	}

	static void bounds_and_nonfinite()
	{
		constexpr std::array<float, 3U> limits{{1.0e12f, 1.0e9f, 1.0e6f}};
		const auto infinity = std::numeric_limits<float>::infinity();
		const std::array<float, 3U> hostile{{std::numeric_limits<float>::quiet_NaN(), infinity, -infinity}};
		for (std::size_t group = 0U; group < limits.size(); ++group) {
			for (std::size_t index = 0U; index < 3U; ++index) {
				for (const auto edge : {-limits[group], limits[group]}) {
					CountedView view;
					view.raw = valid_raw();
					*component(view.raw, group, index) = edge;
					detail::PlayerObservationDto output{};
					EXPECT_EQ(detail::CaptureStatus::Valid, collect(view, output).status);
					EXPECT_FLOAT_EQ(edge, *output_component(output.value, group, index));
				}
				for (const auto outside : {std::nextafter(-limits[group], -infinity),
						 std::nextafter(limits[group], infinity)}) {
					CountedView view;
					view.raw = valid_raw();
					*component(view.raw, group, index) = outside;
					detail::PlayerObservationDto output{};
					EXPECT_EQ(reason_for_group(group), collect(view, output).reason);
					expect_default(output);
				}
				for (const auto value : hostile) {
					CountedView view;
					view.raw = valid_raw();
					*component(view.raw, group, index) = value;
					detail::PlayerObservationDto output{};
					EXPECT_EQ(reason_for_group(group), collect(view, output).reason);
					expect_default(output);
				}
			}
		}
		for (const auto value : hostile) {
			for (std::size_t coefficient = 0U; coefficient < 9U; ++coefficient) {
				CountedView view;
				view.raw = valid_raw();
				float* fields[] = {&view.raw.orientation.right_world.x,
					&view.raw.orientation.right_world.y,
					&view.raw.orientation.right_world.z,
					&view.raw.orientation.up_world.x,
					&view.raw.orientation.up_world.y,
					&view.raw.orientation.up_world.z,
					&view.raw.orientation.forward_world.x,
					&view.raw.orientation.forward_world.y,
					&view.raw.orientation.forward_world.z};
				*fields[coefficient] = value;
				detail::PlayerObservationDto output{};
				EXPECT_EQ(detail::CaptureReason::InvalidOrientation, collect(view, output).reason);
				expect_default(output);
			}
		}
		for (const auto edge : {0.0f, 1.0e9f}) {
			CountedView view;
			view.raw = valid_raw();
			view.raw.radius = edge;
			detail::PlayerObservationDto output{};
			EXPECT_EQ(detail::CaptureStatus::Valid, collect(view, output).status);
			EXPECT_FLOAT_EQ(edge, output.value.radius);
		}
		for (const auto value : {-std::numeric_limits<float>::denorm_min(),
				 std::nextafter(1.0e9f, infinity),
				 std::numeric_limits<float>::quiet_NaN(),
				 infinity,
				 -infinity}) {
			CountedView view;
			view.raw = valid_raw();
			view.raw.radius = value;
			detail::PlayerObservationDto output{};
			EXPECT_EQ(detail::CaptureReason::InvalidRadius, collect(view, output).reason);
			expect_default(output);
		}
	}

	static void subnormal_and_positive_zero()
	{
		CountedView view;
		view.raw = valid_raw();
		const auto subnormal = std::numeric_limits<float>::denorm_min();
		view.raw.position_world.x = subnormal;
		view.raw.position_world.y = -subnormal;
		view.raw.position_world.z = subnormal;
		view.raw.velocity_world.x = -subnormal;
		view.raw.velocity_world.y = subnormal;
		view.raw.velocity_world.z = -subnormal;
		view.raw.rotational_velocity_local.x = subnormal;
		view.raw.rotational_velocity_local.y = -subnormal;
		view.raw.rotational_velocity_local.z = subnormal;
		view.raw.radius = subnormal;
		view.raw.orientation = {};
		view.raw.orientation.right_world.y = -0.0f;
		view.raw.orientation.right_world.z = -0.0f;
		view.raw.orientation.up_world.x = -0.0f;
		view.raw.orientation.up_world.z = -0.0f;
		view.raw.orientation.forward_world.x = -0.0f;
		view.raw.orientation.forward_world.y = -0.0f;
		detail::PlayerObservationDto output{};
		ASSERT_EQ(detail::CaptureStatus::Valid, collect(view, output).status);
		EXPECT_FLOAT_EQ(subnormal, output.value.position_world.x);
		EXPECT_FLOAT_EQ(-subnormal, output.value.position_world.y);
		EXPECT_FLOAT_EQ(subnormal, output.value.position_world.z);
		EXPECT_FLOAT_EQ(-subnormal, output.value.velocity_world.x);
		EXPECT_FLOAT_EQ(subnormal, output.value.velocity_world.y);
		EXPECT_FLOAT_EQ(-subnormal, output.value.velocity_world.z);
		EXPECT_FLOAT_EQ(subnormal, output.value.rotational_velocity_local.x);
		EXPECT_FLOAT_EQ(-subnormal, output.value.rotational_velocity_local.y);
		EXPECT_FLOAT_EQ(subnormal, output.value.rotational_velocity_local.z);
		EXPECT_FLOAT_EQ(subnormal, output.value.radius);
		const std::array<float, 3U> zeroes{{output.value.orientation_local_to_world.x,
			output.value.orientation_local_to_world.y,
			output.value.orientation_local_to_world.z}};
		for (const auto zero : zeroes) {
			EXPECT_FLOAT_EQ(0.0f, zero);
			EXPECT_FALSE(std::signbit(zero));
		}

		CountedView signed_zero;
		signed_zero.raw = valid_raw();
		signed_zero.raw.orientation = {};
		signed_zero.raw.position_world.x = -0.0f;
		signed_zero.raw.position_world.y = -0.0f;
		signed_zero.raw.position_world.z = -0.0f;
		signed_zero.raw.velocity_world.x = -0.0f;
		signed_zero.raw.velocity_world.y = -0.0f;
		signed_zero.raw.velocity_world.z = -0.0f;
		signed_zero.raw.rotational_velocity_local.x = -0.0f;
		signed_zero.raw.rotational_velocity_local.y = -0.0f;
		signed_zero.raw.rotational_velocity_local.z = -0.0f;
		signed_zero.raw.radius = -0.0f;
		detail::PlayerObservationDto canonical{};
		ASSERT_EQ(detail::CaptureStatus::Valid, collect(signed_zero, canonical).status);
		const std::array<float, 13U> canonical_zeroes{{canonical.value.position_world.x,
			canonical.value.position_world.y,
			canonical.value.position_world.z,
			canonical.value.orientation_local_to_world.x,
			canonical.value.orientation_local_to_world.y,
			canonical.value.orientation_local_to_world.z,
			canonical.value.velocity_world.x,
			canonical.value.velocity_world.y,
			canonical.value.velocity_world.z,
			canonical.value.rotational_velocity_local.x,
			canonical.value.rotational_velocity_local.y,
			canonical.value.rotational_velocity_local.z,
			canonical.value.radius}};
		for (const auto zero : canonical_zeroes) {
			EXPECT_FLOAT_EQ(0.0f, zero);
			EXPECT_FALSE(std::signbit(zero));
		}
	}

	static void reused_output_resets()
	{
		CountedView valid;
		valid.raw = valid_raw();
		detail::PlayerObservationDto output{};
		ASSERT_EQ(detail::CaptureStatus::Valid, collect(valid, output).status);
		CountedView absent;
		absent.gates[1U] = false;
		EXPECT_EQ(detail::CaptureStatus::NoPlayer, collect(absent, output).status);
		expect_default(output);
		CountedView invalid;
		invalid.raw = valid_raw();
		invalid.raw.position_world.x = std::numeric_limits<float>::infinity();
		output.key.object_signature = 999U;
		EXPECT_EQ(detail::CaptureStatus::InvalidSource, collect(invalid, output).status);
		expect_default(output);
	}
};

enum class CollectorScenario { Api, Preconditions, Snapshot, Validation, Numeric, Subnormal, Reset };

template <CollectorScenario Scenario,
	typename View = detail::EngineReadView,
	typename Raw = detail::EnginePlayerKinematicsRead,
	typename FsoView = detail::FsoEngineReadView>
void run_engine_collector_contract()
{
	if constexpr (!is_complete_type<View>::value || !is_complete_type<Raw>::value ||
		!is_complete_type<FsoView>::value) {
		FAIL() << "WP07-B requires the tracker-frozen EnginePlayerKinematicsRead, eight-predicate "
			  "EngineReadView and stateless FsoEngineReadView collector contract.";
	} else {
		using Contract = EngineCollectorContract<View, Raw, FsoView>;
		if constexpr (Scenario == CollectorScenario::Api) Contract::api_and_real_view();
		if constexpr (Scenario == CollectorScenario::Preconditions) Contract::preconditions();
		if constexpr (Scenario == CollectorScenario::Snapshot) Contract::exact_snapshot();
		if constexpr (Scenario == CollectorScenario::Validation) Contract::validation_ladder();
		if constexpr (Scenario == CollectorScenario::Numeric) Contract::bounds_and_nonfinite();
		if constexpr (Scenario == CollectorScenario::Subnormal) Contract::subnormal_and_positive_zero();
		if constexpr (Scenario == CollectorScenario::Reset) Contract::reused_output_resets();
	}
}

TEST(TelemetryEngineCollectorContract, EngineReadViewAndRealFsoViewContractExist)
{
	run_engine_collector_contract<CollectorScenario::Api>();
}
TEST(TelemetryEngineCollectorContract, PreconditionsShortCircuitInExactOrder)
{
	run_engine_collector_contract<CollectorScenario::Preconditions>();
}
TEST(TelemetryEngineCollectorContract, OneSnapshotCopiesExactValuesFlagsAndTimestamp)
{
	run_engine_collector_contract<CollectorScenario::Snapshot>();
}
TEST(TelemetryEngineCollectorContract, PostCopyValidationOrderIsTransactional)
{
	run_engine_collector_contract<CollectorScenario::Validation>();
}
TEST(TelemetryEngineCollectorContract, InclusiveBoundsOutsideValuesAndNonFiniteInputsAreClosed)
{
	run_engine_collector_contract<CollectorScenario::Numeric>();
}
TEST(TelemetryEngineCollectorContract, SubnormalsArePreservedAndAllZeroesArePositive)
{
	run_engine_collector_contract<CollectorScenario::Subnormal>();
}
TEST(TelemetryEngineCollectorContract, InvalidAndNoPlayerResetAReusedOutput)
{
	run_engine_collector_contract<CollectorScenario::Reset>();
}

#endif

} // namespace
