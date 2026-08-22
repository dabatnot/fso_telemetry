#pragma once

#include "ship/support_work.h"
#include "telemetry/phase2_observation.h"

#include <cstdint>

namespace telemetry::detail {

SupportWorkStatus evaluate_support_work(
	const SupportWorkInput& input, SupportWorkEvaluation& output) noexcept;
SupportWorkStatus evaluate_support_work(
	object* repaired_object, SupportWorkEvaluation& output) noexcept;

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

#if defined(FSO_TELEMETRY_TEST_SEAMS)
using Phase2StaticExtractorTestInput = Phase2StaticAuthorityInput;
#endif

QuaternionConversionStatus convert_fso_orientation_to_local_to_world(
	const CaptureOrientationBasis& input, CaptureQuaternionf& output) noexcept;
std::uint32_t map_player_physics_mode_flags(const EnginePhysicsFlagInput& input) noexcept;

bool normalize_engine_weapon_bank_selection(
	int selection, int bank_count, int& output) noexcept;

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

#if defined(FSO_TELEMETRY_TEST_SEAMS)
class FsoStaticAuthorityTestReadView final
	: public EngineReadView,
	  public Phase2EngineReadView {
  public:
	FsoStaticAuthorityTestReadView() noexcept = default;
	bool current_thread_is_main() const noexcept override { return false; }
	bool in_mission() const noexcept override { return false; }
	bool player_exists() const noexcept override { return false; }
	bool player_object_exists() const noexcept override { return false; }
	bool player_ship_exists() const noexcept override { return false; }
	bool player_source_is_consistent() const noexcept override { return false; }
	SourceReadResult read_player_root_key(
		EngineEntityKey& output) const noexcept override
	{
		output = {};
		return {Phase2SourceReadStatus::InvalidSource};
	}
	SourceReadResult read_discovery_node(
		EngineEntityKey, Phase2DiscoveryNode& output) const noexcept override
	{
		output = {};
		return {Phase2SourceReadStatus::InvalidSource};
	}
	SourceReadResult resolve_capture_local_key(
		Phase2CaptureLocalKey, EngineEntityKey& output) const noexcept override
	{
		output = {};
		return {Phase2SourceReadStatus::InvalidSource};
	}
	SourceReadResult read_ship(
		EngineEntityKey, Phase2ShipSource& output) const noexcept override
	{
		output = {};
		return {Phase2SourceReadStatus::InvalidSource};
	}
	bool read_player_controls(
		PlayerControlObservation& output) const noexcept override
	{
		output = {};
		return false;
	}
	bool read_player_cargo_scan(
		PlayerCargoScanObservation& output) const noexcept override
	{
		output = {};
		return false;
	}
	SourceReadResult extract_static_authorities_for_test(
		const Phase2StaticExtractorTestInput& input,
		Phase2ObservationDto& output) const noexcept
	{
		return map_phase2_static_authorities(
			input, output.raw_static_catalog);
	}
	bool player_object_is_ship() const noexcept override { return false; }
	bool player_object_ship_instance_in_range() const noexcept override
	{
		return false;
	}
	bool player_object_matches_player() const noexcept override { return false; }
	bool player_ship_matches_object() const noexcept override { return false; }
	void read_player_kinematics(
		EnginePlayerKinematicsRead& output) const noexcept override
	{
		output = {};
	}
};

using FsoEngineReadView = FsoStaticAuthorityTestReadView;
#else
class FsoEngineReadView final : public EngineReadView, public Phase2EngineReadView {
  public:
	FsoEngineReadView() noexcept;
	bool current_thread_is_main() const noexcept override;
	bool in_mission() const noexcept override;
	bool player_exists() const noexcept override;
	bool player_object_exists() const noexcept override;
	bool player_ship_exists() const noexcept override;
	bool player_source_is_consistent() const noexcept override;
	SourceReadResult read_player_root_key(EngineEntityKey& output) const noexcept override;
	SourceReadResult read_discovery_node(
		EngineEntityKey key, Phase2DiscoveryNode& output) const noexcept override;
	SourceReadResult resolve_capture_local_key(
		Phase2CaptureLocalKey key, EngineEntityKey& output) const noexcept override;
	SourceReadResult read_ship(
		EngineEntityKey key, Phase2ShipSource& output) const noexcept override;
	SourceReadResult read_ship_flight(
		EngineEntityKey key,
		ShipFlightObservation& output) const noexcept override;
	SourceReadResult read_core_gate_ship(
		EngineEntityKey key,
		Phase2ShipSource& output) const noexcept override;
	SourceReadResult read_ship_diagnosed(EngineEntityKey key,
		Phase2ShipSource& output,
		Phase2CaptureDiagnostics& diagnostics) const noexcept override;
	SourceReadResult read_core_gate_ship_diagnosed(
		EngineEntityKey key, Phase2ShipSource& output,
		Phase2CaptureDiagnostics& diagnostics) const noexcept override;
	bool read_player_controls(PlayerControlObservation& output) const noexcept override;
	bool read_player_cargo_scan(PlayerCargoScanObservation& output) const noexcept override;
	bool player_object_is_ship() const noexcept override;
	bool player_object_ship_instance_in_range() const noexcept override;
	bool player_object_matches_player() const noexcept override;
	bool player_ship_matches_object() const noexcept override;
	void read_player_kinematics(EnginePlayerKinematicsRead& output) const noexcept override;

  private:
	SourceReadResult read_ship_for_projection(
		EngineEntityKey key, Phase2ShipSource& output,
		bool core_gate) const noexcept;
};
#endif

FsoEngineReadView make_fso_engine_read_view() noexcept;
CaptureResult collect_player_kinematics(const EngineReadView& view,
	std::uint64_t now_us,
	PlayerObservationDto& output) noexcept;

} // namespace telemetry::detail
