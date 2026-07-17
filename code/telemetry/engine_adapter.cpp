#include "telemetry/engine_adapter.h"

#include "physics/physics.h"
#include "telemetry/protocol/telemetry_protocol_constants.h"

#include <array>
#include <cmath>
#include <limits>

namespace telemetry::detail {
namespace {

constexpr double MinimumBasisSquaredNorm = 1.0e-24;
constexpr double MinimumRelativeDeterminant = 1.0e-8;
constexpr double MinimumQuaternionSquaredNorm = 1.0e-24;
constexpr double QuantizedNormMinimum = 0.9999;
constexpr double QuantizedNormMaximum = 1.0001;

bool finite_basis(const CaptureOrientationBasis& input) noexcept
{
	const std::array<float, 9U> coefficients{{input.right_world.x,
		input.right_world.y,
		input.right_world.z,
		input.up_world.x,
		input.up_world.y,
		input.up_world.z,
		input.forward_world.x,
		input.forward_world.y,
		input.forward_world.z}};
	for (const auto coefficient : coefficients) {
		if (!std::isfinite(coefficient)) {
			return false;
		}
	}
	return true;
}

double squared_norm(const CaptureVec3f& value) noexcept
{
	const auto x = static_cast<double>(value.x);
	const auto y = static_cast<double>(value.y);
	const auto z = static_cast<double>(value.z);
	return x * x + y * y + z * z;
}

double determinant(const CaptureOrientationBasis& input) noexcept
{
	const auto r0 = static_cast<double>(input.right_world.x);
	const auto r1 = static_cast<double>(input.right_world.y);
	const auto r2 = static_cast<double>(input.right_world.z);
	const auto u0 = static_cast<double>(input.up_world.x);
	const auto u1 = static_cast<double>(input.up_world.y);
	const auto u2 = static_cast<double>(input.up_world.z);
	const auto f0 = static_cast<double>(input.forward_world.x);
	const auto f1 = static_cast<double>(input.forward_world.y);
	const auto f2 = static_cast<double>(input.forward_world.z);
	return r0 * (u1 * f2 - u2 * f1) - u0 * (r1 * f2 - r2 * f1) +
		f0 * (r1 * u2 - r2 * u1);
}

bool should_flip_sign(const CaptureQuaternionf& value) noexcept
{
	if (value.w < 0.0f) {
		return true;
	}
	if (value.w != 0.0f) {
		return false;
	}
	for (const auto component : {value.x, value.y, value.z}) {
		if (component != 0.0f) {
			return component < 0.0f;
		}
	}
	return false;
}

void canonicalize(CaptureQuaternionf& value) noexcept
{
	if (should_flip_sign(value)) {
		value.w = -value.w;
		value.x = -value.x;
		value.y = -value.y;
		value.z = -value.z;
	}
	if (value.w == 0.0f) value.w = 0.0f;
	if (value.x == 0.0f) value.x = 0.0f;
	if (value.y == 0.0f) value.y = 0.0f;
	if (value.z == 0.0f) value.z = 0.0f;
}

} // namespace

QuaternionConversionStatus convert_fso_orientation_to_local_to_world(
	const CaptureOrientationBasis& input, CaptureQuaternionf& output) noexcept
{
	output = {};
	if (!finite_basis(input)) {
		return QuaternionConversionStatus::NonFiniteInput;
	}

	const auto right_norm = squared_norm(input.right_world);
	const auto up_norm = squared_norm(input.up_world);
	const auto forward_norm = squared_norm(input.forward_world);
	if (!std::isfinite(right_norm) || !std::isfinite(up_norm) || !std::isfinite(forward_norm) ||
		right_norm > std::numeric_limits<float>::max() || up_norm > std::numeric_limits<float>::max() ||
		forward_norm > std::numeric_limits<float>::max()) {
		return QuaternionConversionStatus::NonFiniteResult;
	}
	if (right_norm <= MinimumBasisSquaredNorm || up_norm <= MinimumBasisSquaredNorm ||
		forward_norm <= MinimumBasisSquaredNorm) {
		return QuaternionConversionStatus::DegenerateInput;
	}
	const auto basis_determinant = determinant(input);
	const auto determinant_scale = std::sqrt(right_norm * up_norm * forward_norm);
	if (!std::isfinite(basis_determinant) || !std::isfinite(determinant_scale)) {
		return QuaternionConversionStatus::NonFiniteResult;
	}
	if (basis_determinant <= 0.0 || determinant_scale <= 0.0 ||
		basis_determinant / determinant_scale <= MinimumRelativeDeterminant) {
		return QuaternionConversionStatus::DegenerateInput;
	}

	// The input members are FSO local axes expressed in world coordinates.
	// They are therefore the columns of the logical local-to-world matrix.
	const auto m00 = static_cast<double>(input.right_world.x);
	const auto m01 = static_cast<double>(input.up_world.x);
	const auto m02 = static_cast<double>(input.forward_world.x);
	const auto m10 = static_cast<double>(input.right_world.y);
	const auto m11 = static_cast<double>(input.up_world.y);
	const auto m12 = static_cast<double>(input.forward_world.y);
	const auto m20 = static_cast<double>(input.right_world.z);
	const auto m21 = static_cast<double>(input.up_world.z);
	const auto m22 = static_cast<double>(input.forward_world.z);

	double w = 0.0;
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
	const auto trace = m00 + m11 + m22;
	if (trace > 0.0) {
		const auto scale = 2.0 * std::sqrt(trace + 1.0);
		w = 0.25 * scale;
		x = (m21 - m12) / scale;
		y = (m02 - m20) / scale;
		z = (m10 - m01) / scale;
	} else if (m00 > m11 && m00 > m22) {
		const auto scale = 2.0 * std::sqrt(1.0 + m00 - m11 - m22);
		w = (m21 - m12) / scale;
		x = 0.25 * scale;
		y = (m01 + m10) / scale;
		z = (m02 + m20) / scale;
	} else if (m11 > m22) {
		const auto scale = 2.0 * std::sqrt(1.0 + m11 - m00 - m22);
		w = (m02 - m20) / scale;
		x = (m01 + m10) / scale;
		y = 0.25 * scale;
		z = (m12 + m21) / scale;
	} else {
		const auto scale = 2.0 * std::sqrt(1.0 + m22 - m00 - m11);
		w = (m10 - m01) / scale;
		x = (m02 + m20) / scale;
		y = (m12 + m21) / scale;
		z = 0.25 * scale;
	}
	if (!std::isfinite(w) || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
		return QuaternionConversionStatus::NonFiniteResult;
	}
	const auto squared_quaternion_norm = w * w + x * x + y * y + z * z;
	if (!std::isfinite(squared_quaternion_norm)) {
		return QuaternionConversionStatus::NonFiniteResult;
	}
	if (squared_quaternion_norm <= MinimumQuaternionSquaredNorm) {
		return QuaternionConversionStatus::DegenerateInput;
	}
	const auto inverse_norm = 1.0 / std::sqrt(squared_quaternion_norm);
	CaptureQuaternionf candidate{static_cast<float>(w * inverse_norm),
		static_cast<float>(x * inverse_norm),
		static_cast<float>(y * inverse_norm),
		static_cast<float>(z * inverse_norm)};
	if (!std::isfinite(candidate.w) || !std::isfinite(candidate.x) || !std::isfinite(candidate.y) ||
		!std::isfinite(candidate.z)) {
		return QuaternionConversionStatus::NonFiniteResult;
	}
	canonicalize(candidate);
	const auto quantized_squared_norm = static_cast<double>(candidate.w) * candidate.w +
		static_cast<double>(candidate.x) * candidate.x + static_cast<double>(candidate.y) * candidate.y +
		static_cast<double>(candidate.z) * candidate.z;
	const auto quantized_norm = std::sqrt(quantized_squared_norm);
	if (!std::isfinite(quantized_norm)) {
		return QuaternionConversionStatus::NonFiniteResult;
	}
	if (quantized_norm < QuantizedNormMinimum || quantized_norm > QuantizedNormMaximum) {
		return QuaternionConversionStatus::QuantizedNormOutOfRange;
	}
	output = candidate;
	return QuaternionConversionStatus::Converted;
}

std::uint32_t map_player_physics_mode_flags(const EnginePhysicsFlagInput& input) noexcept
{
	std::uint32_t output = protocol::PhysicsModeFlagNone;
	const auto has = [&](std::uint32_t flag) noexcept { return (input.raw_physics_flags & flag) != 0U; };
	if (has(PF_AFTERBURNER_ON)) output |= protocol::PhysicsModeFlagAfterburner;
	if (has(PF_BOOSTER_ON)) output |= protocol::PhysicsModeFlagBooster;
	if (has(PF_GLIDING)) output |= protocol::PhysicsModeFlagGlideActive;
	if (has(PF_FORCE_GLIDE)) output |= protocol::PhysicsModeFlagGlideForced;
	if (has(PF_NEWTONIAN_DAMP)) output |= protocol::PhysicsModeFlagNewtonianDamping;
	if (has(PF_SLIDE_ENABLED)) output |= protocol::PhysicsModeFlagLateralTranslation;
	if (has(PF_WARP_IN) || has(PF_SUPERCAP_WARP_IN)) output |= protocol::PhysicsModeFlagWarpIn;
	if (has(PF_WARP_OUT) || has(PF_SUPERCAP_WARP_OUT)) output |= protocol::PhysicsModeFlagWarpOut;
	if (has(PF_SCRIPTED_VELOCITY)) output |= protocol::PhysicsModeFlagScripted;
	if (has(PF_IN_SHOCKWAVE)) output |= protocol::PhysicsModeFlagShockwave;
	if (input.object_immobile || input.object_position_locked) output |= protocol::PhysicsModeFlagImmobile;
	if (input.object_immobile || input.object_orientation_locked) {
		output |= protocol::PhysicsModeFlagOrientationLocked;
	}
	return output & protocol::KnownPhysicsModeFlags;
}

} // namespace telemetry::detail
