#pragma once

#include <cstdint>

namespace telemetry::detail {

struct CaptureVec3f {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

struct CaptureQuaternionf {
	float w = 1.0f;
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

struct CaptureOrientationBasis {
	CaptureVec3f right_world{1.0f, 0.0f, 0.0f};
	CaptureVec3f up_world{0.0f, 1.0f, 0.0f};
	CaptureVec3f forward_world{0.0f, 0.0f, 1.0f};
};

struct PlayerObservationKey {
	std::uint32_t object_signature = 0U;
};

struct PlayerKinematicsValue {
	std::uint64_t producer_sample_time_us = 0U;
	CaptureVec3f position_world{};
	CaptureQuaternionf orientation_local_to_world{};
	CaptureVec3f velocity_world{};
	CaptureVec3f rotational_velocity_local{};
	float radius = 0.0f;
	std::uint32_t physics_mode_flags = 0U;
};

struct PlayerObservationDto {
	PlayerObservationKey key{};
	PlayerKinematicsValue value{};
};

struct PlayerKinematicsSample {
	std::uint64_t entity_id = 0U;
	PlayerKinematicsValue value{};
};

enum class CaptureStatus : std::uint8_t {
	Valid = 0,
	NoPlayer,
	InvalidSource,
	Count,
};

enum class CaptureReason : std::uint8_t {
	None = 0,
	NotInMission,
	MissingPlayer,
	MissingPlayerObject,
	MissingPlayerShip,
	WrongObjectType,
	ShipInstanceOutOfRange,
	PlayerObjectMismatch,
	PlayerShipMismatch,
	InvalidObservationKey,
	InvalidPosition,
	InvalidOrientation,
	InvalidVelocity,
	InvalidRotationalVelocity,
	InvalidRadius,
	Count,
};

struct CaptureResult {
	CaptureStatus status = CaptureStatus::InvalidSource;
	CaptureReason reason = CaptureReason::InvalidObservationKey;
};

enum class QuaternionConversionStatus : std::uint8_t {
	Converted = 0,
	NonFiniteInput,
	DegenerateInput,
	NonFiniteResult,
	QuantizedNormOutOfRange,
	Count,
};

struct EnginePhysicsFlagInput {
	std::uint32_t raw_physics_flags = 0U;
	bool object_immobile = false;
	bool object_position_locked = false;
	bool object_orientation_locked = false;
};

struct EnginePlayerKinematicsRead {
	std::int32_t object_signature = 0;
	CaptureVec3f position_world{};
	CaptureOrientationBasis orientation{};
	CaptureVec3f velocity_world{};
	CaptureVec3f rotational_velocity_local{};
	float radius = 0.0f;
	EnginePhysicsFlagInput physics{};
};

QuaternionConversionStatus convert_fso_orientation_to_local_to_world(
	const CaptureOrientationBasis& input, CaptureQuaternionf& output) noexcept;
std::uint32_t map_player_physics_mode_flags(const EnginePhysicsFlagInput& input) noexcept;

class EngineReadView {
  public:
	virtual ~EngineReadView() noexcept = default;
	virtual bool in_mission() const noexcept = 0;
	virtual bool player_exists() const noexcept = 0;
	virtual bool player_object_exists() const noexcept = 0;
	virtual bool player_ship_exists() const noexcept = 0;
	virtual bool player_object_is_ship() const noexcept = 0;
	virtual bool player_object_ship_instance_in_range() const noexcept = 0;
	virtual bool player_object_matches_player() const noexcept = 0;
	virtual bool player_ship_matches_object() const noexcept = 0;
	virtual void read_player_kinematics(EnginePlayerKinematicsRead& output) const noexcept = 0;
};

class FsoEngineReadView final : public EngineReadView {
  public:
	FsoEngineReadView() noexcept = default;
	bool in_mission() const noexcept override;
	bool player_exists() const noexcept override;
	bool player_object_exists() const noexcept override;
	bool player_ship_exists() const noexcept override;
	bool player_object_is_ship() const noexcept override;
	bool player_object_ship_instance_in_range() const noexcept override;
	bool player_object_matches_player() const noexcept override;
	bool player_ship_matches_object() const noexcept override;
	void read_player_kinematics(EnginePlayerKinematicsRead& output) const noexcept override;
};

FsoEngineReadView make_fso_engine_read_view() noexcept;
CaptureResult collect_player_kinematics(const EngineReadView& view,
	std::uint64_t now_us,
	PlayerObservationDto& output) noexcept;

} // namespace telemetry::detail
