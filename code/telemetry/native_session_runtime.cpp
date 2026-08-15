#include "telemetry/native_session_runtime.h"

#include "telemetry/phase1_state_image.h"
#include "telemetry/native_session_runtime_test_seam.h"
#include "telemetry/logging.h"
#include "telemetry/phase2_catalog_projection.h"
#include "telemetry/phase3_engine_collector.h"
#include "telemetry/protocol/telemetry_protocol_constants.h"
#include "telemetry/startup_budget.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace telemetry::detail {
namespace {

std::uint64_t elapsed_nanoseconds(std::chrono::steady_clock::time_point started,
	std::chrono::steady_clock::time_point ended) noexcept
{
	const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(ended - started).count();
	return elapsed <= 0 ? 0U : static_cast<std::uint64_t>(elapsed);
}

void reset_phase3_projection(Phase3Projection& projection) noexcept
{
	// Phase3Projection contains the fixed 4,096-contact and navigation
	// workspaces. Reconstruct it directly in its preallocated storage: assigning
	// a value-initialized temporary would reserve that whole workspace on the
	// engine update stack even when this reset branch is not taken.
	projection.~Phase3Projection();
	new (&projection) Phase3Projection();
}

TelemetryPhase2Profile telemetry_profile(
	Phase2Profile profile) noexcept
{
	switch (profile) {
	case Phase2Profile::CoreGate:
		return TelemetryPhase2Profile::CoreGate;
	case Phase2Profile::CompleteShip:
		return TelemetryPhase2Profile::CompleteShip;
	case Phase2Profile::CockpitSensors:
		return TelemetryPhase2Profile::CockpitSensors;
	case Phase2Profile::None:
	default:
		return TelemetryPhase2Profile::None;
	}
}

TelemetryPhase3Block telemetry_phase3_block(
	Phase3EngineCollectBlock block) noexcept
{
	switch (block) {
	case Phase3EngineCollectBlock::TargetLocks:
		return TelemetryPhase3Block::TargetLocks;
	case Phase3EngineCollectBlock::HudAlerts:
		return TelemetryPhase3Block::HudAlerts;
	case Phase3EngineCollectBlock::Radar:
		return TelemetryPhase3Block::Radar;
	case Phase3EngineCollectBlock::Threat:
		return TelemetryPhase3Block::Threat;
	case Phase3EngineCollectBlock::Cargo:
		return TelemetryPhase3Block::Cargo;
	case Phase3EngineCollectBlock::Navigation:
		return TelemetryPhase3Block::Navigation;
	case Phase3EngineCollectBlock::StateImage:
		return TelemetryPhase3Block::StateImage;
	case Phase3EngineCollectBlock::None:
	case Phase3EngineCollectBlock::Precondition:
	case Phase3EngineCollectBlock::Count:
	default:
		return TelemetryPhase3Block::Precondition;
	}
}

TelemetryPhase3CaptureFailure telemetry_phase3_failure(
	Phase3EngineCollectStatus status) noexcept
{
	switch (status) {
	case Phase3EngineCollectStatus::NoPlayer:
		return TelemetryPhase3CaptureFailure::NoPlayer;
	case Phase3EngineCollectStatus::NotMainThread:
		return TelemetryPhase3CaptureFailure::NotMainThread;
	case Phase3EngineCollectStatus::SourceLimitExceeded:
		return TelemetryPhase3CaptureFailure::SourceLimitExceeded;
	case Phase3EngineCollectStatus::IdentityFailure:
		return TelemetryPhase3CaptureFailure::IdentityFailure;
	case Phase3EngineCollectStatus::AllocationFailure:
		return TelemetryPhase3CaptureFailure::AllocationFailed;
	case Phase3EngineCollectStatus::Collected:
	case Phase3EngineCollectStatus::InvalidSource:
	case Phase3EngineCollectStatus::Count:
	default:
		return TelemetryPhase3CaptureFailure::InvalidSource;
	}
}

TelemetryPhase3CaptureFailure telemetry_phase3_image_failure(
	Phase3StateImageBuildStatus status) noexcept
{
	switch (status) {
	case Phase3StateImageBuildStatus::InvalidInput:
		return TelemetryPhase3CaptureFailure::InvalidInput;
	case Phase3StateImageBuildStatus::CapacityExceeded:
		return TelemetryPhase3CaptureFailure::CapacityExceeded;
	case Phase3StateImageBuildStatus::AllocationFailed:
		return TelemetryPhase3CaptureFailure::AllocationFailed;
	case Phase3StateImageBuildStatus::Created:
	case Phase3StateImageBuildStatus::EncodingFailed:
	case Phase3StateImageBuildStatus::Count:
	default:
		return TelemetryPhase3CaptureFailure::EncodingFailed;
	}
}

void record_phase3_failure(TelemetryMetrics* metrics,
	TelemetryStructuredLog* log, std::size_t slot,
	TelemetryPhase3Block block,
	TelemetryPhase3CaptureFailure reason) noexcept
{
	if (metrics != nullptr)
		metrics->record_phase3_capture_failure(block, reason);
	if (log != nullptr)
		log->phase3_source_rejected(slot, block, reason);
}

TelemetryPhase2ProfileRejection telemetry_profile_rejection(
	Phase2ProfileError error) noexcept
{
	switch (error) {
	case Phase2ProfileError::UnsupportedAuthority:
	case Phase2ProfileError::DedicatedNotAllowed:
	case Phase2ProfileError::HeadlessNotAllowed:
		return TelemetryPhase2ProfileRejection::UnsupportedAuthority;
	case Phase2ProfileError::UnsupportedCoverage:
	case Phase2ProfileError::UnsupportedProfile:
	default:
		return TelemetryPhase2ProfileRejection::IncompleteCoverage;
	}
}

TelemetryPhase2SupportKind telemetry_support_kind(
	SupportTransitionReason reason) noexcept
{
	switch (reason) {
	case SupportTransitionReason::Broken:
		return TelemetryPhase2SupportKind::Obstructed;
	case SupportTransitionReason::Abort:
	case SupportTransitionReason::Killed:
		return TelemetryPhase2SupportKind::Aborted;
	case SupportTransitionReason::Complete:
		return TelemetryPhase2SupportKind::CompletedPrivate;
	case SupportTransitionReason::End:
		return TelemetryPhase2SupportKind::EndedPrivate;
	case SupportTransitionReason::Queue:
	case SupportTransitionReason::OnWay:
	case SupportTransitionReason::Begin:
	default:
		return TelemetryPhase2SupportKind::Count;
	}
}

TelemetryPhase2SupportKind telemetry_observed_support_kind(
	ShipSupportPhase phase) noexcept
{
	switch (phase) {
	case ShipSupportPhase::Queued:
		return TelemetryPhase2SupportKind::Requested;
	case ShipSupportPhase::OnWay:
		return TelemetryPhase2SupportKind::Approaching;
	case ShipSupportPhase::Docking:
		return TelemetryPhase2SupportKind::Docking;
	case ShipSupportPhase::Repairing:
		return TelemetryPhase2SupportKind::Repairing;
	case ShipSupportPhase::Rearming:
		return TelemetryPhase2SupportKind::Rearming;
	case ShipSupportPhase::None:
	case ShipSupportPhase::Aborted:
	case ShipSupportPhase::Obstructed:
	case ShipSupportPhase::Count:
	default:
		return TelemetryPhase2SupportKind::Count;
	}
}

TelemetryPhase2KeyframeReason telemetry_keyframe_reason(
	Phase2RuntimeSnapshotCause cause) noexcept
{
	switch (cause) {
	case Phase2RuntimeSnapshotCause::Periodic:
		return TelemetryPhase2KeyframeReason::Periodic;
	case Phase2RuntimeSnapshotCause::Topology:
		return TelemetryPhase2KeyframeReason::Topology;
	case Phase2RuntimeSnapshotCause::Catalog:
		return TelemetryPhase2KeyframeReason::Catalog;
	case Phase2RuntimeSnapshotCause::Lifecycle:
		return TelemetryPhase2KeyframeReason::Lifecycle;
	case Phase2RuntimeSnapshotCause::SupportTerminal:
		return TelemetryPhase2KeyframeReason::Support;
	case Phase2RuntimeSnapshotCause::Resync:
	case Phase2RuntimeSnapshotCause::DeltaCapacity:
		return TelemetryPhase2KeyframeReason::Resync;
	case Phase2RuntimeSnapshotCause::Initial:
	case Phase2RuntimeSnapshotCause::None:
	case Phase2RuntimeSnapshotCause::Count:
	default:
		return TelemetryPhase2KeyframeReason::Count;
	}
}

TelemetryPhase2LifecycleKind telemetry_lifecycle_kind(
	Phase2RuntimeLifecycleEventKind kind) noexcept
{
	switch (kind) {
	case Phase2RuntimeLifecycleEventKind::Appeared:
		return TelemetryPhase2LifecycleKind::Appeared;
	case Phase2RuntimeLifecycleEventKind::Disabled:
		return TelemetryPhase2LifecycleKind::Disabled;
	case Phase2RuntimeLifecycleEventKind::DyingStarted:
		return TelemetryPhase2LifecycleKind::DyingStarted;
	case Phase2RuntimeLifecycleEventKind::Destroyed:
		return TelemetryPhase2LifecycleKind::Destroyed;
	case Phase2RuntimeLifecycleEventKind::Disappeared:
	default:
		return TelemetryPhase2LifecycleKind::Disappeared;
	}
}

bool make_controller_config(const TelemetryConfig& source,
	std::uint64_t producer_id,
	SessionControllerConfig& output) noexcept
{
	if (!source.enabled || source.max_clients < 1U || source.max_clients > 4U ||
		source.max_datagrams_per_tick < 1U || source.max_datagrams_per_tick > 256U ||
		producer_id == 0U) {
		return false;
	}

	output.max_clients = source.max_clients;
	output.producer_id = producer_id;
	output.mission_heartbeat_ms = source.mission_heartbeat_ms;
	output.idle_heartbeat_ms = source.idle_heartbeat_ms;
	output.keyframe_seconds = source.keyframe_seconds;
	output.security.enabled = true;
	output.security.port = source.bind_port;
	output.security.discovery_enabled = source.discovery_enabled;
	output.security.source_allowlist = source.allowed_clients;
	output.security.resources.max_clients = source.max_clients;
	output.security.resources.global_state_reassembly_bytes =
		static_cast<std::size_t>(source.max_clients) * protocol::MaxStateReassemblyBytesPerClient;
	output.security.resources.global_video_reassembly_bytes =
		static_cast<std::size_t>(source.max_clients) * protocol::MaxVideoReassemblyBytesPerClient;
	for (std::size_t index = 0U; index < source.bind_addresses.size(); ++index) {
		if (!source.bind_addresses[index].is_loopback()) {
			output.security.bind_mode = protocol::NetworkBindMode::LoopbackAndAllowlisted;
			break;
		}
	}
	return true;
}

TelemetryLogReason log_reason(SessionCloseReason reason) noexcept
{
	switch (reason) {
	case SessionCloseReason::Timeout:
		return TelemetryLogReason::Timeout;
	case SessionCloseReason::MissionDiscontinuity:
		return TelemetryLogReason::MissionDiscontinuity;
	case SessionCloseReason::ProtocolError:
		return TelemetryLogReason::ProtocolError;
	case SessionCloseReason::TransportError:
		return TelemetryLogReason::TransportError;
	case SessionCloseReason::Shutdown:
	default:
		return TelemetryLogReason::Shutdown;
	}
}

TelemetryLogDrop log_drop_reason(SessionIngressDropReason reason) noexcept
{
	switch (reason) {
	case SessionIngressDropReason::SourceNotAllowed:
		return TelemetryLogDrop::Allowlist;
	case SessionIngressDropReason::HelloRateLimited:
	case SessionIngressDropReason::SessionCreationRateLimited:
		return TelemetryLogDrop::RateLimit;
	case SessionIngressDropReason::AntiAmplificationLimit:
		return TelemetryLogDrop::AntiAmplification;
	case SessionIngressDropReason::None:
	case SessionIngressDropReason::DatagramEnvelopeInvalid:
	case SessionIngressDropReason::EndpointSessionMismatch:
	case SessionIngressDropReason::HandshakeCacheFull:
	case SessionIngressDropReason::PayloadInvalid:
	case SessionIngressDropReason::SessionIdUnavailable:
	case SessionIngressDropReason::NoClientSlot:
	default:
		return TelemetryLogDrop::Validation;
	}
}

std::uint64_t seconds_to_microseconds(float seconds) noexcept
{
	if (!std::isfinite(seconds) || seconds <= 0.0F) return 0U;
	const auto value = static_cast<double>(seconds) * 1'000'000.0;
	return value >= static_cast<double>(
			std::numeric_limits<std::uint64_t>::max())
		? std::numeric_limits<std::uint64_t>::max()
		: static_cast<std::uint64_t>(value);
}

WeaponFamily manifest_weapon_family(std::uint8_t value) noexcept
{
	switch (value) {
	case 0U: return WeaponFamily::None;
	case 1U: return WeaponFamily::Primary;
	case 2U: return WeaponFamily::Secondary;
	case 3U: return WeaponFamily::Tertiary;
	case 4U: return WeaponFamily::Turret;
	default: return WeaponFamily::None;
	}
}

bool project_weapon_subtype(const Phase2RawWeaponDefinition& source,
	protocol::WeaponSubtype& output) noexcept
{
	if ((source.raw_class_flags &
		 protocol::WeaponClassFlagCountermeasure) != 0U) {
		output = protocol::WeaponSubtype::Countermeasure;
		return true;
	}
	if ((source.raw_class_flags & protocol::WeaponClassFlagBeam) != 0U ||
		source.weapon_subtype_source == 2U) {
		output = protocol::WeaponSubtype::Beam;
		return true;
	}
	if (source.weapon_subtype_source == 0U) {
		output = protocol::WeaponSubtype::Primary;
		return true;
	}
	if (source.weapon_subtype_source == 1U) {
		output = protocol::WeaponSubtype::Missile;
		return true;
	}
	return false;
}

bool project_subsystem_type(std::uint8_t source,
	protocol::SubsystemType& output) noexcept
{
	switch (source) {
	case 1U:
		output = protocol::SubsystemType::Engine;
		return true;
	case 2U:
		output = protocol::SubsystemType::Turret;
		return true;
	case 3U:
		output = protocol::SubsystemType::Radar;
		return true;
	case 4U:
		output = protocol::SubsystemType::Navigation;
		return true;
	case 5U:
		output = protocol::SubsystemType::Communication;
		return true;
	case 6U:
		output = protocol::SubsystemType::Weapons;
		return true;
	case 7U:
		output = protocol::SubsystemType::Sensors;
		return true;
	case 8U:
		output = protocol::SubsystemType::Reactor;
		return true;
	case 9U:
	case 10U:
		output = protocol::SubsystemType::Other;
		return true;
	case 0U:
	case 11U:
		output = protocol::SubsystemType::Unknown;
		return true;
	default:
		return false;
	}
}

std::int32_t auxiliary_engine_index(std::uint32_t capture_key) noexcept
{
	return capture_key == 0U ||
		capture_key > static_cast<std::uint32_t>(
			std::numeric_limits<std::int32_t>::max())
		? -1
		: static_cast<std::int32_t>(capture_key - 1U);
}

float observed_subsystem_hits(const Phase2ObservationDto& observation,
	std::uint32_t class_key, std::uint32_t subsystem_key,
	float fallback) noexcept
{
	for (const auto& ship : observation.ships) {
		if (ship.raw_static_references.class_capture_key != class_key)
			continue;
		for (std::uint32_t index = 0U;
			 index < ship.subsystems.count; ++index)
			if (ship.subsystems.values[index].source_key.value ==
				subsystem_key)
				return ship.subsystems.values[index].hits_current;
	}
	return fallback;
}

Phase2CatalogProjectionStatus project_phase2_catalog_impl(
	const Phase2ObservationDto& observation,
	Phase2ManifestSource& output) noexcept
{
	const auto& raw = observation.raw_static_catalog;
	if (raw.class_count > output.ship_classes.size() ||
		raw.weapon_count > output.weapons.size() ||
		raw.auxiliary_count > output.auxiliary_entries.size())
		return Phase2CatalogProjectionStatus::SourceLimitExceeded;

	// Phase2ManifestSource is intentionally much larger than the native thread
	// stack. Reset only the bounded entries selected by this projection instead
	// of materializing a full aggregate temporary on the stack.
	output.ship_class_count = 0U;
	output.weapon_count = 0U;
	output.referenced_ship_class_count = 0U;
	output.referenced_weapon_count = 0U;
	output.auxiliary_entry_count = 0U;
	output.metadata = {};
	output.player_instance_signature = 0U;
	output.engine_index = 0U;
	output.manifest_generation = 0U;
	output.topology_fingerprint = {};
	output.referenced_ship_class_keys.fill(0U);
	output.referenced_weapon_keys.fill(0U);
	for (std::uint32_t index = 0U; index < raw.class_count; ++index)
		output.ship_classes[index] = {};
	for (std::uint32_t index = 0U; index < raw.weapon_count; ++index)
		output.weapons[index] = {};
	for (std::uint32_t index = 0U; index < raw.auxiliary_count; ++index)
		output.auxiliary_entries[index] = {};

	for (std::uint32_t index = 0U; index < raw.auxiliary_count;
		 ++index) {
		const auto& source = raw.auxiliary_entries[index];
		auto& target = output.auxiliary_entries[index];
		if (source.registry >= Phase2RawAuxiliaryRegistry::Count)
			return Phase2CatalogProjectionStatus::InvalidSource;
		target.registry = static_cast<AuxiliaryRegistry>(
			source.registry);
		target.engine_index =
			source.registry == Phase2RawAuxiliaryRegistry::Pattern
			? static_cast<std::int32_t>(
				source.firing_pattern_source_code)
			: auxiliary_engine_index(source.capture_key);
		target.name.assign(source.name.view());
	}

	for (std::uint32_t index = 0U; index < raw.weapon_count;
		 ++index) {
		const auto& source = raw.weapon_definitions[index];
		auto& target = output.weapons[index];
		target.source_key = source.weapon_capture_key;
		target.name.assign(source.internal_name.view());
		target.title.assign(source.title.view());
		target.damage_type_index =
			auxiliary_engine_index(source.damage_type_capture_key);
		if (!project_weapon_subtype(source, target.subtype))
			return Phase2CatalogProjectionStatus::InvalidSource;
		target.class_flags = source.raw_class_flags;
		target.max_speed = source.max_speed;
		target.mass = source.mass;
		target.gravity_constant = source.gravity_constant;
		target.velocity_inherit_amount =
			source.velocity_inherit_amount;
		target.lifetime_us =
			seconds_to_microseconds(source.lifetime_seconds);
		target.effect_flags = source.raw_effect_flags;
		target.guidance_type = source.guidance_type_source;
		target.has_acceleration =
			source.acceleration_time_seconds > 0.0F;
		target.has_ranges = source.maximum_range > 0.0F;
		target.has_fire = source.fire_wait_seconds > 0.0F;
		target.has_damage = source.damage > 0.0F;
		target.has_guidance = source.guidance_type_source != 0U;
		target.has_lock = source.lock_time_seconds > 0.0F;
		target.has_cargo_rearm = source.cargo_size > 0.0F;
		if (source.burst_shots < 0 ||
			source.burst_shots >= 4096 ||
			source.shots_source < 1 ||
			source.shots_source > 4096 ||
			source.swarm_count_source < -1 ||
			source.swarm_count_source > 4096)
			return Phase2CatalogProjectionStatus::SourceLimitExceeded;
		const auto burst_count = source.burst_shots + 1;
		const auto swarm_count = source.swarm_count_source > 0
			? source.swarm_count_source
			: source.shots_source;
		target.has_burst = burst_count > 1;
		target.has_swarm =
			source.swarm_count_source > 0 ||
			source.shots_source > 1;
		target.acceleration_time_us = seconds_to_microseconds(
			source.acceleration_time_seconds);
		target.fire_wait_us =
			seconds_to_microseconds(source.fire_wait_seconds);
		target.lock_time_us =
			seconds_to_microseconds(source.lock_time_seconds);
		target.rearm_time_us =
			seconds_to_microseconds(source.rearm_rate_seconds);
		target.burst_interval_us =
			seconds_to_microseconds(source.burst_delay_seconds);
		target.minimum_range = source.minimum_range;
		target.optimal_range = source.optimal_range;
		target.maximum_range = source.maximum_range;
		target.energy_consumed = source.energy_consumed;
		target.damage = source.damage;
		target.guidance_fov_rad = source.guidance_type_source == 0U
			? 0.0F
			: std::acos(source.guidance_fov_source_cosine);
		target.lock_fov_rad = source.lock_time_seconds <= 0.0F
			? 0.0F
			: std::acos(source.lock_fov_source_cosine);
		target.cargo_size = source.cargo_size;
		target.reloaded_per_batch = source.reloaded_per_batch;
		target.burst_count =
			static_cast<std::uint16_t>(burst_count);
		target.swarm_count =
			static_cast<std::uint16_t>(swarm_count);
		target.shots_per_trigger = static_cast<std::uint16_t>(
			source.shots_source);
		output.referenced_weapon_keys[index] =
			source.weapon_capture_key;
	}

	for (std::uint32_t index = 0U; index < raw.class_count;
		 ++index) {
		const auto& source = raw.class_definitions[index];
		auto& target = output.ship_classes[index];
		if (source.subsystem_count > target.subsystems.size() ||
			source.bank_count > target.banks.size() ||
			source.subsystem_offset >
				raw.subsystem_storage.size() ||
			source.subsystem_count >
				raw.subsystem_storage.size() -
					source.subsystem_offset ||
			source.bank_offset > raw.bank_storage.size() ||
			source.bank_count > raw.bank_storage.size() -
				source.bank_offset)
			return Phase2CatalogProjectionStatus::InvalidSource;
		target.source_key = source.class_capture_key;
		target.name.assign(source.internal_name.view());
		target.model_mass = source.model_mass;
		target.density_provenance = source.density;
		target.effective_mass = source.effective_mass;
		target.model_inertia = {source.model_inertia[0],
			source.model_inertia[4], source.model_inertia[8]};
		target.effective_inertia = {source.effective_inertia[0],
			source.effective_inertia[4],
			source.effective_inertia[8]};
		target.effective_inertia_matrix =
			source.effective_inertia;
		target.center_of_mass = {source.center_of_mass.x,
			source.center_of_mass.y, source.center_of_mass.z};
		target.max_velocity = {source.max_velocity.x,
			source.max_velocity.y, source.max_velocity.z};
		target.afterburner_max_velocity = {
			source.afterburner_max_velocity.x,
			source.afterburner_max_velocity.y,
			source.afterburner_max_velocity.z};
		target.booster_max_velocity = {
			source.booster_max_velocity.x,
			source.booster_max_velocity.y,
			source.booster_max_velocity.z};
		target.max_rotational_velocity = {
			source.max_rotational_velocity.x,
			source.max_rotational_velocity.y,
			source.max_rotational_velocity.z};
		target.max_rear_velocity = source.max_rear_velocity;
		target.forward_accel_time = source.forward_accel_time;
		target.afterburner_forward_accel_time =
			source.afterburner_forward_accel_time;
		target.booster_forward_accel_time =
			source.booster_forward_accel_time;
		target.forward_decel_time = source.forward_decel_time;
		target.slide_accel_time = source.slide_accel_time;
		target.slide_decel_time = source.slide_decel_time;
		target.max_hull_strength = source.max_hull_strength;
		target.max_shield_strength = source.max_shield_strength;
		target.has_afterburner = source.has_afterburner;
		target.afterburner_fuel_capacity =
			source.afterburner_fuel_capacity;
		target.afterburner_burn_rate =
			source.afterburner_burn_rate;
		target.afterburner_recover_rate =
			source.afterburner_recover_rate;
		target.afterburner_min_start_fuel =
			source.afterburner_min_start_fuel;
		target.afterburner_cooldown_us =
			seconds_to_microseconds(
				source.afterburner_cooldown_seconds);
		target.has_scan = source.has_scan;
		target.scan_required_time_us = source.scan_time_ms <= 0
			? 0U : static_cast<std::uint64_t>(
				source.scan_time_ms) * 1000U;
		target.scan_max_distance = std::max(
			source.scan_range_normal,
			source.scan_range_capital) *
			source.scanning_range_multiplier;
		target.scan_max_angle_rad =
			static_cast<float>(std::acos(0.95));
		target.has_glide = source.has_glide;
		target.glide_cap = source.glide_cap;
		target.has_autoaim = source.has_autoaim;
		target.autoaim_fov_rad = source.autoaim_fov_rad;
		target.species_index =
			auxiliary_engine_index(source.species_capture_key);
		target.ship_type_index =
			auxiliary_engine_index(source.ship_type_capture_key);
		target.iff_index =
			auxiliary_engine_index(source.iff_capture_key);
		target.wing_index =
			auxiliary_engine_index(source.wing_capture_key);
		target.armor_index =
			auxiliary_engine_index(source.armor_capture_key);
		target.damage_type_index =
			auxiliary_engine_index(
				source.damage_type_capture_key);
		target.required_model_index = 0;
		target.countermeasure_capacity =
			source.countermeasure_capacity;
		target.countermeasure_cargo_size =
			source.countermeasure_cargo_size;
		target.countermeasure_uses_capacity =
			source.countermeasure_uses_capacity;
		target.countermeasure_weapon_source_key =
			source.countermeasure_weapon_capture_key;
		target.countermeasure_firewait_ms =
			source.countermeasure_firewait_ms;
		target.subsystem_count = source.subsystem_count;
		for (std::uint32_t subsystem = 0U;
			 subsystem < source.subsystem_count; ++subsystem) {
			const auto& raw_subsystem =
				raw.subsystem_storage[
					source.subsystem_offset + subsystem];
			auto& mapped = target.subsystems[subsystem];
			mapped.source_key =
				raw_subsystem.subsystem_capture_key;
			mapped.system_info_key =
				raw_subsystem.subsystem_capture_key;
			mapped.name.assign(
				raw_subsystem.internal_name.view());
			mapped.alt_name.assign(
				raw_subsystem.alt_name.view());
			mapped.hud_name.assign(
				raw_subsystem.hud_name.view());
			mapped.max_hits = raw_subsystem.max_hits;
			const auto observed_hits = observed_subsystem_hits(
				observation, source.class_capture_key,
				raw_subsystem.subsystem_capture_key,
				raw_subsystem.max_hits);
			if (!std::isfinite(mapped.max_hits) ||
				mapped.max_hits < 0.0F ||
				!std::isfinite(observed_hits))
				return Phase2CatalogProjectionStatus::InvalidSource;
			mapped.current_hits = std::clamp(
				observed_hits, 0.0F, mapped.max_hits);
			mapped.armor_index = auxiliary_engine_index(
				raw_subsystem.armor_capture_key);
			if (!project_subsystem_type(
					raw_subsystem.subsystem_type_source,
					mapped.type))
				return Phase2CatalogProjectionStatus::InvalidSource;
			mapped.local_position = {
				raw_subsystem.local_position.x,
				raw_subsystem.local_position.y,
				raw_subsystem.local_position.z};
			mapped.radius = raw_subsystem.radius;
			mapped.static_flags =
				raw_subsystem.raw_static_flags;
		}
		target.bank_count = source.bank_count;
		for (std::uint32_t bank = 0U;
			 bank < source.bank_count; ++bank) {
			const auto& raw_bank =
				raw.bank_storage[source.bank_offset + bank];
			auto& mapped = target.banks[bank];
			if (raw_bank.family_source < 1U ||
				raw_bank.family_source > 4U ||
				raw_bank.source_family > 2U)
				return Phase2CatalogProjectionStatus::InvalidSource;
			mapped.family =
				manifest_weapon_family(raw_bank.family_source);
			mapped.source_family =
				manifest_weapon_family(raw_bank.source_family);
			mapped.bank_index = raw_bank.bank_index;
			mapped.weapon_source_key =
				raw_bank.weapon_capture_key;
			mapped.firing_pattern_source_code =
				raw_bank.firing_pattern_source_code;
			mapped.consumes_ammunition =
				raw_bank.consumes_ammunition;
			mapped.capacity = raw_bank.has_capacity
				? raw_bank.capacity : 0.0F;
			mapped.fire_point_count =
				raw_bank.fire_point_count;
			if (mapped.fire_point_count >
				mapped.fire_points.size())
				return Phase2CatalogProjectionStatus::
					SourceLimitExceeded;
			for (std::uint32_t point = 0U;
				 point < raw_bank.fire_point_count; ++point)
				mapped.fire_points[point] = {
					raw_bank.fire_points[point].x,
					raw_bank.fire_points[point].y,
					raw_bank.fire_points[point].z};
			if (raw_bank.owner_subsystem_capture_key != 0U) {
				mapped.owner_subsystem_canonical_index =
					UINT16_MAX;
				for (std::uint32_t subsystem = 0U;
					 subsystem < source.subsystem_count;
					 ++subsystem)
					if (raw.subsystem_storage[
							source.subsystem_offset +
							subsystem]
							.subsystem_capture_key ==
						raw_bank
							.owner_subsystem_capture_key)
						mapped
							.owner_subsystem_canonical_index =
							static_cast<std::uint16_t>(
								subsystem);
				if (mapped.owner_subsystem_canonical_index ==
					UINT16_MAX)
					return Phase2CatalogProjectionStatus::
						InvalidSource;
			}
		}
		output.referenced_ship_class_keys[index] =
			source.class_capture_key;
	}

	protocol::Sha256 topology;
	const auto add = [&](const auto& value) noexcept {
		return topology.update({reinterpret_cast<const std::uint8_t*>(
			&value), sizeof(value)});
	};
	if (!add(observation.player_key.value) ||
		!add(observation.discovery_count))
		return Phase2CatalogProjectionStatus::InvalidSource;
	for (std::uint32_t index = 0U;
		 index < observation.discovery_count; ++index) {
		const auto& node = observation.discovery_nodes[index];
		if (!add(node.capture_key.value) ||
			!add(node.support_capture_key.value) ||
			!add(node.direct_docking_count))
			return Phase2CatalogProjectionStatus::InvalidSource;
		for (std::uint32_t relation = 0U;
			 relation < node.direct_docking_count; ++relation)
			if (!add(node.direct_docking_capture_keys[
					relation].value))
				return Phase2CatalogProjectionStatus::
					InvalidSource;
	}
	if (!topology.finalize(output.topology_fingerprint))
		return Phase2CatalogProjectionStatus::InvalidSource;
	output.ship_class_count = raw.class_count;
	output.weapon_count = raw.weapon_count;
	output.referenced_ship_class_count = raw.class_count;
	output.referenced_weapon_count = raw.weapon_count;
	output.auxiliary_entry_count = raw.auxiliary_count;
	output.player_instance_signature =
		observation.player_key.value;
	output.manifest_generation = 1U;
	return Phase2CatalogProjectionStatus::Success;
}

bool append_phase3_manifest_weapon(Phase2ManifestSource& output,
	std::uint32_t source_key) noexcept
{
	if (source_key == 0U) return true;
	for (std::uint32_t index = 0U;
		 index < output.referenced_weapon_count; ++index)
		if (output.referenced_weapon_keys[index] == source_key)
			return true;
	if (output.referenced_weapon_count >=
		output.referenced_weapon_keys.size())
		return false;
	for (std::uint32_t index = 0U; index < output.weapon_count; ++index)
		if (output.weapons[index].source_key == source_key) {
			output.referenced_weapon_keys[
				output.referenced_weapon_count++] = source_key;
			return true;
		}
	return false;
}

bool phase3_manifest_has_weapon_definition(
	const Phase2ManifestSource& output, std::uint32_t source_key) noexcept
{
	if (source_key == 0U) return true;
	for (std::uint32_t index = 0U; index < output.weapon_count; ++index)
		if (output.weapons[index].source_key == source_key)
			return true;
	return false;
}

bool append_phase3_manifest_class(Phase2ManifestSource& output,
	std::uint32_t source_key) noexcept
{
	if (source_key == 0U) return false;
	for (std::uint32_t index = 0U;
		 index < output.referenced_ship_class_count; ++index)
		if (output.referenced_ship_class_keys[index] == source_key)
			return true;
	if (output.referenced_ship_class_count >=
		output.referenced_ship_class_keys.size())
		return false;
	const Phase2ClassSource* selected = nullptr;
	for (std::uint32_t index = 0U; index < output.ship_class_count;
		 ++index)
		if (output.ship_classes[index].source_key == source_key) {
			selected = &output.ship_classes[index];
			break;
		}
	if (selected == nullptr) return false;
	output.referenced_ship_class_keys[
		output.referenced_ship_class_count++] = source_key;
	for (std::uint32_t bank = 0U; bank < selected->bank_count; ++bank)
		if (!append_phase3_manifest_weapon(output,
			selected->banks[bank].weapon_source_key))
			return false;
	return append_phase3_manifest_weapon(output,
		selected->countermeasure_weapon_source_key);
}

bool select_phase3_manifest_references(const Phase2ManifestSource& raw,
	const Phase2ObservationDto& observation,
	const Phase2Wp05SubjectBinding* bindings,
	std::size_t binding_count,
	const Phase3CatalogDependencies& dependencies,
	Phase2ManifestSource& output) noexcept
{
	if (bindings == nullptr || binding_count != observation.ships.size() ||
		raw.ship_class_count > output.ship_classes.size() ||
		raw.weapon_count > output.weapons.size())
		return false;
	// Copying remains private and bounded.  Only referenced_* is serializable;
	// raw definitions not selected below cannot enter the client manifest.
	output = raw;
	output.referenced_ship_class_count = 0U;
	output.referenced_weapon_count = 0U;
	output.referenced_ship_class_keys.fill(0U);
	output.referenced_weapon_keys.fill(0U);
	for (std::size_t subject = 0U; subject < binding_count; ++subject) {
		if (bindings[subject].entity_id == 0U) continue;
		const auto class_key = static_cast<std::uint32_t>(
			observation.ships[subject].identity.class_source_key.value);
		if (!append_phase3_manifest_class(output, class_key))
			return false;
	}
	for (std::uint32_t index = 0U;
		 index < dependencies.ship_class_count; ++index) {
		// TARGET_STATE and CARGO_SCAN depend on independently disclosed
		// identities.  A target can legitimately belong to a class absent from
		// the Phase 2 catalog captured for this sample.  That must suppress only
		// the optional detailed identity group; failing the whole manifest here
		// used to close the live dashboard session as soon as a new target was
		// selected.
		(void)append_phase3_manifest_class(output,
			dependencies.ship_class_source_keys[index]);
	}
	for (std::uint32_t index = 0U;
		 index < dependencies.weapon_count; ++index) {
		const auto source_key = dependencies.weapon_source_keys[index];
		// Incoming ordnance can legitimately use a class outside the Phase 2
		// observer catalog.  collect_threat already omits such a missile until
		// its class is installed; manifest selection must make the same optional
		// omission instead of closing the whole cockpit stream.
		if (!phase3_manifest_has_weapon_definition(output, source_key))
			continue;
		if (!append_phase3_manifest_weapon(output, source_key))
			return false;
	}
	return true;
}

} // namespace

Phase2CatalogProjectionStatus project_phase2_catalog(
	const Phase2ObservationDto& observation,
	Phase2ManifestSource& output) noexcept
{
	if (observation.capture.status == Phase2CaptureStatus::NoPlayer)
		return Phase2CatalogProjectionStatus::
			SourceTemporarilyUnavailable;
	if (observation.capture.status != Phase2CaptureStatus::Valid ||
		observation.capture.reason != Phase2CaptureReason::None)
		return Phase2CatalogProjectionStatus::InvalidSource;
	return project_phase2_catalog_impl(observation, output);
}

void NativeOutputCompletionForwarder::complete(SessionController& controller, IoStatus status) noexcept
{
	controller.complete_output(status);
}

NativeSessionRuntime::NativeSessionRuntime(UdpSocketBackend& backend,
	NativeOutputCompletionPort& output_completion) noexcept
	: m_transport(backend), m_output_completion(output_completion)
{
}

NativeSessionRuntime::~NativeSessionRuntime() noexcept
{
	shutdown();
}

NativeSessionStartStatus NativeSessionRuntime::start(const NativeSessionStartRequest& request) noexcept
{
	if (m_state == State::Started) {
		return NativeSessionStartStatus::AlreadyStarted;
	}
	if (m_state != State::Cold || request.config == nullptr || request.session_ids == nullptr ||
		request.packet_sequences == nullptr) {
		return NativeSessionStartStatus::InvalidConfiguration;
	}
	Phase2Profile selected_phase2_profile = Phase2Profile::None;
	const auto profile_error =
		request.requested_phase2_profile != Phase2Profile::None
		? select_phase2_profile(request.phase2_eligibility,
			request.requested_phase2_profile,
			selected_phase2_profile)
		: Phase2ProfileError::None;
	if (profile_error != Phase2ProfileError::None) {
		const auto reason =
			telemetry_profile_rejection(profile_error);
		if (request.metrics != nullptr)
			request.metrics->record_phase2_profile_rejection(reason);
		if (request.log != nullptr)
			request.log->phase2_profile_rejected(reason,
				phase2_profile_coverage(
					request.requested_phase2_profile));
		// This rejection precedes controller/DTO allocation, bind and WELCOME.
		return NativeSessionStartStatus::InvalidConfiguration;
	}
	m_startup_allocation_count = 0U;
	Capture30Hz capture_cadence;
	if (!capture_cadence.configure(request.config->flight_hz)) {
		return NativeSessionStartStatus::InvalidConfiguration;
	}
	Capture30Hz systems_capture_cadence;
	if (!systems_capture_cadence.configure(
			request.config->systems_hz, 20U)) {
		return NativeSessionStartStatus::InvalidConfiguration;
	}

	SessionControllerConfig controller_config;
	if (!make_controller_config(*request.config, request.producer_id, controller_config)) {
		return NativeSessionStartStatus::InvalidConfiguration;
	}
	if (selected_phase2_profile != Phase2Profile::None)
		controller_config.delta_payload_capacity =
			Phase2CompleteShipDeltaBytes;
	controller_config.phase2_profile = selected_phase2_profile;
	if (m_fail_session_controller_provision) {
		// No controller has been published and no transport operation has begun.
		// Keep State::Cold so clearing the test-only failpoint permits a retry.
		return NativeSessionStartStatus::AllocationFailure;
	}
	++m_startup_allocation_count;
	SessionController controller;
	const auto configured = SessionController::configure(controller_config,
		*request.session_ids,
		*request.packet_sequences,
		0U,
		nullptr,
		controller);
	if (configured == SessionControllerConfigureResult::InvalidConfiguration) {
		return NativeSessionStartStatus::InvalidConfiguration;
	}
	if (configured == SessionControllerConfigureResult::AllocationFailure) {
		return NativeSessionStartStatus::AllocationFailure;
	}
	const auto phase2_mode = selected_phase2_profile == Phase2Profile::None
		? Phase2ProvisioningMode::ValidDisabled
		: Phase2ProvisioningMode::ValidEnabled;
	auto phase2_observation = std::unique_ptr<Phase2ObservationBuffer>(
		new (std::nothrow) Phase2ObservationBuffer());
	if (phase2_observation == nullptr) {
		return NativeSessionStartStatus::AllocationFailure;
	}
	++m_startup_allocation_count;
	if (phase2_mode == Phase2ProvisioningMode::ValidEnabled) {
		++m_startup_allocation_count;
	}
	if (!phase2_observation->provision(phase2_mode)) {
		return NativeSessionStartStatus::AllocationFailure;
	}
	if (phase2_mode == Phase2ProvisioningMode::ValidEnabled &&
		!phase2_observation->enter_ready()) {
		return NativeSessionStartStatus::AllocationFailure;
	}
	const auto phase2_owned_bytes = phase2_observation->owned_bytes();
	// Construct every mutable image backing before bind/Ready. A failure is
	// retryable because no transport operation has started yet.
	if (!provision_state_image_pools(request.config->max_clients)) {
		return NativeSessionStartStatus::AllocationFailure;
	}
	if (selected_phase2_profile == Phase2Profile::CoreGate &&
		!provision_phase2_core_gate_image_pools(
			request.config->max_clients)) {
		release_state_image_pools();
		return NativeSessionStartStatus::AllocationFailure;
	}
	if ((selected_phase2_profile == Phase2Profile::CompleteShip ||
		 selected_phase2_profile == Phase2Profile::CockpitSensors) &&
		!provision_phase2_image_pools(request.config->max_clients)) {
		release_state_image_pools();
		return NativeSessionStartStatus::AllocationFailure;
	}
	std::array<std::unique_ptr<Phase3Projection>, 4U>
		phase3_projections{};
	std::array<std::unique_ptr<Phase3Projection>, 4U>
		phase3_projection_scratch{};
	std::array<std::unique_ptr<Phase3IdentityRegistry>, 4U>
		phase3_identity_registries{};
	if (selected_phase2_profile == Phase2Profile::CockpitSensors) {
		for (std::size_t index = 0U;
			 index < request.config->max_clients; ++index) {
			phase3_projections[index].reset(
				new (std::nothrow) Phase3Projection());
			phase3_projection_scratch[index].reset(
				new (std::nothrow) Phase3Projection());
			phase3_identity_registries[index].reset(
				new (std::nothrow) Phase3IdentityRegistry());
			if (phase3_projections[index] == nullptr ||
				phase3_projection_scratch[index] == nullptr ||
				phase3_identity_registries[index] == nullptr ||
				phase3_identity_registries[index]->provision() !=
					Phase3IdentityProvisionStatus::Ready) {
				release_state_image_pools();
				return NativeSessionStartStatus::AllocationFailure;
			}
		}
	}
	if (selected_phase2_profile != Phase2Profile::None &&
		!(selected_phase2_profile == Phase2Profile::CockpitSensors
			? provision_phase2_catalog_source()
			: provision_phase2_manifest_state())) {
		release_state_image_pools();
		return NativeSessionStartStatus::AllocationFailure;
	}
	if (selected_phase2_profile == Phase2Profile::CockpitSensors &&
		!provision_phase3_manifest_states(request.config->max_clients)) {
		release_state_image_pools();
		return NativeSessionStartStatus::AllocationFailure;
	}
	++m_startup_allocation_count;
	std::size_t startup_owned_bytes = 0U;
	Phase2OwnedBudget phase2_budget{};
	if (selected_phase2_profile == Phase2Profile::None) {
		if (phase2_owned_bytes > Phase2SharedOwnedCapBytes ||
			m_state_image_pool_backing_bytes >
				Phase2SharedOwnedCapBytes - phase2_owned_bytes) {
			release_state_image_pools();
			return NativeSessionStartStatus::AllocationFailure;
		}
		startup_owned_bytes =
			phase2_owned_bytes + m_state_image_pool_backing_bytes;
		if (m_startup_owned_budget_test_adjustment >
			Phase2SharedOwnedCapBytes - startup_owned_bytes) {
			release_state_image_pools();
			return NativeSessionStartStatus::AllocationFailure;
		}
	} else {
		const auto owned = controller.owned_capacity();
		const auto clients = static_cast<std::size_t>(
			request.config->max_clients);
		std::size_t shared_owned = phase2_owned_bytes;
		std::size_t per_client_owned = 0U;
		std::size_t shared_controller = 0U;
		const bool valid_owned =
			checked_add_size(shared_owned,
				sizeof(m_phase2_event_batch_scratch) +
					sizeof(m_phase2_fanout_scratch) +
					sizeof(m_phase2_support_tracker) +
					sizeof(m_phase2_support_seen_scratch),
				shared_owned) &&
			checked_add_size(shared_owned,
				m_phase2_manifest_backing_bytes,
				shared_owned) &&
			checked_add_size(shared_owned,
				m_phase3_manifest_backing_bytes,
				shared_owned) &&
			(selected_phase2_profile !=
					Phase2Profile::CockpitSensors ||
				(phase3_projections[0] != nullptr &&
				 phase3_projection_scratch[0] != nullptr &&
				 phase3_identity_registries[0] != nullptr &&
				 checked_add_size(per_client_owned,
					 2U * sizeof(Phase3Projection),
					 per_client_owned) &&
				 checked_add_size(per_client_owned,
					 sizeof(Phase3IdentityRegistry),
					 per_client_owned) &&
					checked_add_size(per_client_owned,
						phase3_identity_registries[0]->
							provisioned_bytes(),
						per_client_owned))) &&
			clients != 0U &&
			owned.client_slots == clients &&
			owned.client_slot_bytes % clients == 0U &&
			owned.state_reassembly_bytes % clients == 0U &&
			owned.reliable_retention_bytes % clients == 0U &&
			owned.snapshot_egress_heap_bytes % clients == 0U &&
			owned.delta_egress_heap_bytes % clients == 0U &&
			owned.delta_scratch_heap_bytes % clients == 0U &&
			m_state_image_pool_backing_bytes % clients == 0U &&
			m_phase2_core_gate_image_pool_backing_bytes %
				clients == 0U &&
			m_phase2_image_pool_backing_bytes % clients == 0U &&
			checked_add_size(owned.rate_limiter_bytes,
				owned.handshake_cache_bytes, shared_controller) &&
			checked_add_size(shared_controller,
				owned.preproof_ledger_bytes, shared_controller) &&
			checked_add_size(shared_controller,
				owned.output_queue_bytes, shared_controller) &&
			checked_add_size(shared_owned, shared_controller,
				shared_owned) &&
			checked_add_size(shared_owned,
				request.metrics != nullptr
					? request.metrics->owned_bytes() : 0U,
				shared_owned) &&
			checked_add_size(shared_owned,
				m_startup_owned_budget_test_adjustment,
				shared_owned) &&
			checked_add_size(per_client_owned,
				owned.client_slot_bytes / clients,
				per_client_owned) &&
			checked_add_size(per_client_owned,
				owned.state_reassembly_bytes / clients,
				per_client_owned) &&
			checked_add_size(per_client_owned,
				owned.reliable_retention_bytes / clients,
				per_client_owned) &&
			checked_add_size(per_client_owned,
				owned.snapshot_egress_heap_bytes / clients,
				per_client_owned) &&
			checked_add_size(per_client_owned,
				owned.delta_egress_heap_bytes / clients,
				per_client_owned) &&
			checked_add_size(per_client_owned,
				owned.delta_scratch_heap_bytes / clients,
				per_client_owned) &&
			checked_add_size(per_client_owned,
				m_state_image_pool_backing_bytes / clients,
				per_client_owned) &&
			checked_add_size(per_client_owned,
				m_phase2_core_gate_image_pool_backing_bytes /
					clients,
				per_client_owned) &&
			checked_add_size(per_client_owned,
				m_phase2_image_pool_backing_bytes / clients,
				per_client_owned);
		if (!valid_owned) {
			release_state_image_pools();
			return NativeSessionStartStatus::AllocationFailure;
		}
		phase2_budget =
			selected_phase2_profile ==
					Phase2Profile::CockpitSensors
			? calculate_phase3_owned_budget(
				{shared_owned, per_client_owned, clients})
			: calculate_phase2_owned_budget(
				{shared_owned, per_client_owned, clients});
		if (phase2_budget.error != StartupBudgetError::None) {
			release_state_image_pools();
			return NativeSessionStartStatus::AllocationFailure;
		}
		startup_owned_bytes = phase2_budget.process_owned_bytes;
	}

	const auto opened = m_transport.open(*request.config);
	if (opened == TransportOpenStatus::InvalidConfiguration || opened == TransportOpenStatus::Disabled) {
		release_state_image_pools();
		return NativeSessionStartStatus::InvalidConfiguration;
	}
	if (opened != TransportOpenStatus::Complete) {
		release_state_image_pools();
		return NativeSessionStartStatus::TransportUnavailable;
	}

	m_controller = std::move(controller);
	m_metrics = request.metrics;
	m_log = request.log;
	if (m_metrics != nullptr) {
		m_metrics->set_udp_sockets_open(m_transport.socket_count());
		m_metrics->set_runtime_state(4U); // RuntimeState::Ready, kept local to avoid a dependency cycle.
		if (selected_phase2_profile != Phase2Profile::None) {
			m_metrics->set_phase2_memory(
				TelemetryPhase2MemoryScope::Shared,
				phase2_budget.shared_owned_bytes);
			m_metrics->set_phase2_memory(
				TelemetryPhase2MemoryScope::ClientTotal,
				phase2_budget.clients_owned_bytes);
			m_metrics->set_phase2_memory(
				TelemetryPhase2MemoryScope::ProcessTotal,
				phase2_budget.process_owned_bytes);
		}
	}
	m_controller_ready = true;
	m_maximum_attempts = request.config->max_datagrams_per_tick;
	m_producer_id = request.producer_id;
	m_scheduler.reset();
	m_capture_cadence = capture_cadence;
	m_systems_capture_cadence = systems_capture_cadence;
	m_phase2_observation = std::move(*phase2_observation);
	m_startup_owned_bytes = startup_owned_bytes;
	m_phase2_owned_budget = phase2_budget;
	m_phase2_capture_plan = {};
	m_last_phase2_capture_result = {};
	m_selected_phase2_profile = selected_phase2_profile;
	m_phase2_enabled = selected_phase2_profile != Phase2Profile::None;
	m_phase3_projections = std::move(phase3_projections);
	m_phase3_projection_scratch =
		std::move(phase3_projection_scratch);
	m_phase3_identity_registries =
		std::move(phase3_identity_registries);
	reset_phase2_support_tracker();
	clear_player_capture();
	m_fault_status = NativeSessionTickStatus::Unavailable;
	m_phase2_event_pipeline_failed_closed = false;
	m_state = State::Started;
	if (m_log != nullptr) {
		// State-image backings are fully provisioned before Ready; this is the
		// first real high-water observation, not a configured estimate.
		m_log->budget_high_water(TelemetryLogBudget::StateImage, m_state_image_pool_backing_bytes,
			m_state_image_pool_backing_bytes);
	}
	return NativeSessionStartStatus::Started;
}

NativeSessionTickStatus NativeSessionRuntime::service_tick(const NativeSessionTickContext& context,
	const EngineReadView& engine_view,
	const Phase2EngineReadView* phase2_view) noexcept
{
	m_last_phase2_failure_diagnostic = {};
	m_last_phase2_failure_diagnostic.tick_now_us = context.now_us;
	if (m_phase2_enabled && phase2_view != nullptr &&
		!phase2_view->current_thread_is_main()) {
		if (m_metrics != nullptr)
			m_metrics->record_phase2_capture_failure(
				TelemetryPhase2Block::Identity,
				TelemetryPhase2CaptureFailure::Guard);
		m_phase2_observation.reset_observation_and_clear_phase2();
		fail_capture(NativePlayerCaptureStatus::CaptureInvariantFailure);
		return NativeSessionTickStatus::PermanentCaptureFailure;
	}
	const auto measure_performance = m_performance_observation_active;
	const auto tick_started = measure_performance ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	if (measure_performance) m_last_performance_sample = {};
	const auto r2_status = service_r2_tick(context);
	if (r2_status != NativeSessionTickStatus::Complete) {
		return r2_status;
	}
	if (m_phase2_enabled) {
		auto& event_batch = m_phase2_event_batch_scratch;
		const auto prepared = prepare_phase2_global_events(
			m_phase2_observation.accepted_capture_map(), event_batch);
		if (m_metrics != nullptr) {
			m_metrics->set_phase2_ring(TelemetryPhase2Ring::Cleanup,
				phase2_cleanup_ring_depth());
			m_metrics->set_phase2_ring(TelemetryPhase2Ring::Support,
				phase2_support_ring_depth());
		}
		if (prepared == Phase2Wp07DrainStatus::RingOverflow ||
			prepared == Phase2Wp07DrainStatus::TableOverflow ||
			prepared == Phase2Wp07DrainStatus::InvalidInput) {
			if (!m_phase2_event_pipeline_failed_closed) {
				m_phase2_event_pipeline_failed_closed = true;
				if (m_metrics != nullptr) {
					if (phase2_cleanup_ring_overflowed())
						m_metrics->record_phase2_ring_overflow(
							TelemetryPhase2Ring::Cleanup);
					if (phase2_support_ring_overflowed())
						m_metrics->record_phase2_ring_overflow(
							TelemetryPhase2Ring::Support);
				}
				purge_all(SessionCloseReason::ProtocolError);
			}
			return NativeSessionTickStatus::PermanentCaptureFailure;
		}
		if (prepared == Phase2Wp07DrainStatus::Drained) {
			auto& fanout = m_phase2_fanout_scratch;
			fanout = {};
			const auto applied =
				m_controller.apply_phase2_global_events_transaction(
					event_batch, fanout);
			if (applied == Phase2RuntimeResult::InvalidInput ||
				applied == Phase2RuntimeResult::CapacityExceeded ||
				applied == Phase2RuntimeResult::CounterExhausted ||
				!commit_phase2_global_events(event_batch)) {
				if (!m_phase2_event_pipeline_failed_closed) {
					m_phase2_event_pipeline_failed_closed = true;
					purge_all(SessionCloseReason::ProtocolError);
				}
				return NativeSessionTickStatus::PermanentCaptureFailure;
			}
			if (m_metrics != nullptr) {
				m_metrics->set_phase2_ring(TelemetryPhase2Ring::Cleanup,
					phase2_cleanup_ring_depth());
				m_metrics->set_phase2_ring(TelemetryPhase2Ring::Support,
					phase2_support_ring_depth());
				for (std::size_t index = 0U;
					 index < event_batch.cleanup.count; ++index)
					m_metrics->record_phase2_lifecycle(
						event_batch.cleanup.intents[index].mode ==
								ShipCleanupMode::Destroyed
							? TelemetryPhase2LifecycleKind::Destroyed
							: TelemetryPhase2LifecycleKind::Disappeared);
				for (std::size_t index = 0U;
					 index < event_batch.support_count; ++index) {
					const auto reason = event_batch.support[index].reason;
					const auto kind = telemetry_support_kind(reason);
					if (kind != TelemetryPhase2SupportKind::Count)
						m_metrics->record_phase2_support(kind);
				}
				for (std::size_t slot = 0U;
					 slot < Phase2GlobalFanoutResult::Capacity; ++slot)
					if (fanout.targeted[slot])
						for (std::size_t kind = 0U;
							 kind < fanout.support_coalesced[slot].size();
							 ++kind)
							for (std::uint8_t count =
									0U;
								 count < fanout
										 .support_coalesced[slot][kind];
								 ++count)
								m_metrics
									->record_phase2_support_coalesced(
										slot,
										static_cast<
											TelemetryPhase2SupportCoalescedKind>(
											kind));
			}
			if (m_log != nullptr)
				for (std::size_t slot = 0U;
					 slot < Phase2GlobalFanoutResult::Capacity; ++slot) {
					if (!fanout.targeted[slot]) continue;
					for (std::size_t index = 0U;
						 index < event_batch.cleanup.count; ++index)
						m_log->phase2_lifecycle(slot,
							event_batch.cleanup.intents[index].mode ==
									ShipCleanupMode::Destroyed
								? TelemetryPhase2LifecycleKind::Destroyed
								: TelemetryPhase2LifecycleKind::Disappeared);
					for (std::size_t index = 0U;
						 index < event_batch.support_count; ++index) {
						const auto reason =
							event_batch.support[index].reason;
						const auto kind =
							telemetry_support_kind(reason);
						if (kind !=
							TelemetryPhase2SupportKind::Count)
							m_log->phase2_support_terminal(slot,
								kind,
								fanout.cause_after[slot] !=
									fanout.cause_before[slot]);
					}
				}
		}
	}

	// Network ingress, reliability and the global Phase 2 event transaction may
	// all request a replacement snapshot during this EngineUpdate. Observe the
	// coalesced intent only after those paths have run, then force one complete
	// Phase 3 sample before any keyframe candidate is materialized.
	m_capture_for_phase3_keyframe =
		m_capture_for_phase3_keyframe ||
		m_controller.phase3_complete_capture_required();
	const auto cadence =
		m_capture_cadence.poll(context.now_us, context.mission_active);
	const auto systems_cadence = m_phase2_enabled
		? m_systems_capture_cadence.poll(
			context.now_us, context.mission_active)
		: CaptureCadenceResult{CaptureCadenceStatus::NotDue, 0U};
	switch (cadence.status) {
	case CaptureCadenceStatus::Inactive:
		m_phase2_capture_plan = {};
		m_phase2_observation.reset_observation_and_clear_phase2();
		reset_phase2_support_tracker();
		m_controller.clear_player_observations();
		clear_player_capture();
		m_last_player_capture_status = NativePlayerCaptureStatus::Inactive;
		if (measure_performance) {
			m_last_performance_sample.tick_duration_ns = elapsed_nanoseconds(tick_started, std::chrono::steady_clock::now());
			refresh_performance_resource_sample();
		}
		return NativeSessionTickStatus::Complete;
	case CaptureCadenceStatus::NotDue:
		break;
	case CaptureCadenceStatus::Due:
		break;
	case CaptureCadenceStatus::InvalidRate:
	case CaptureCadenceStatus::ClockRegression:
	case CaptureCadenceStatus::DeadlineOverflow:
	case CaptureCadenceStatus::Count:
		fail_capture(NativePlayerCaptureStatus::CadenceFailure);
		return NativeSessionTickStatus::PermanentCaptureFailure;
	}
	switch (systems_cadence.status) {
	case CaptureCadenceStatus::Inactive:
	case CaptureCadenceStatus::NotDue:
	case CaptureCadenceStatus::Due:
		break;
	case CaptureCadenceStatus::InvalidRate:
	case CaptureCadenceStatus::ClockRegression:
	case CaptureCadenceStatus::DeadlineOverflow:
	case CaptureCadenceStatus::Count:
		if (m_metrics != nullptr)
			m_metrics->record_phase2_capture_failure(
				TelemetryPhase2Block::Identity,
				TelemetryPhase2CaptureFailure::Guard);
		fail_capture(NativePlayerCaptureStatus::CadenceFailure);
		return NativeSessionTickStatus::PermanentCaptureFailure;
	}

	const auto flight_controls_cadence =
		cadence.status == CaptureCadenceStatus::Due ||
		m_capture_after_ready_transition ||
		m_capture_for_phase3_keyframe;
	const auto systems_capture_due =
		systems_cadence.status == CaptureCadenceStatus::Due ||
		m_capture_after_ready_transition ||
		m_capture_for_phase3_keyframe;
	// Phase 2's All refresh necessarily refreshes flight controls as part of a
	// coherent observation. That must not promote the Phase 3 targeting cadence:
	// target and locks remain flightHz-owned even when systemsHz is higher.
	if (!flight_controls_cadence && !systems_capture_due) {
		m_phase2_capture_plan = {};
		m_last_player_materialization = {};
		m_last_player_capture_status = NativePlayerCaptureStatus::NotDue;
		if (measure_performance) {
			m_last_performance_sample.tick_duration_ns =
				elapsed_nanoseconds(tick_started,
					std::chrono::steady_clock::now());
			refresh_performance_resource_sample();
		}
		return NativeSessionTickStatus::Complete;
	}
	m_phase2_capture_plan.capture_flight_controls =
		m_phase2_enabled && flight_controls_cadence;
	m_phase2_capture_plan.capture_systems =
		m_phase2_enabled && systems_capture_due;
	m_phase2_capture_plan.force_complete_keyframe =
		m_capture_after_ready_transition ||
		m_capture_for_phase3_keyframe;
	m_phase2_capture_plan.phase3_refresh_targeting =
		m_phase2_enabled && flight_controls_cadence;
	m_phase2_capture_plan.phase3_refresh_systems =
		m_phase2_enabled && systems_capture_due;
	m_phase2_capture_plan.producer_sample_time_us = context.now_us;
	if (m_phase2_capture_plan.force_complete_keyframe) {
		prepare_phase2_keyframe(m_phase2_capture_plan);
	}
	if (m_phase2_enabled && phase2_view != nullptr &&
		(m_phase2_capture_plan.capture_flight_controls ||
		 m_phase2_capture_plan.capture_systems)) {
		const auto phase2_capture_started = measure_performance
			? std::chrono::steady_clock::now()
			: std::chrono::steady_clock::time_point{};
		const auto projection =
			m_selected_phase2_profile == Phase2Profile::CoreGate
			? Phase2ObservationProjection::CoreGate
			: Phase2ObservationProjection::CompleteShip;
		const auto phase2_capture = collect_phase2_observation(
			m_phase2_observation,
			*phase2_view,
			m_phase2_capture_plan.producer_sample_time_us,
			projection,
			m_phase2_capture_plan.capture_systems ||
					m_phase2_capture_plan.force_complete_keyframe
				? Phase2ObservationRefresh::All
				: Phase2ObservationRefresh::FlightControls);
		if (measure_performance)
			m_last_performance_sample.collect_duration_ns +=
				elapsed_nanoseconds(
					phase2_capture_started,
					std::chrono::steady_clock::now());
		m_last_phase2_capture_result = phase2_capture;
		m_last_phase2_failure_diagnostic.capture_observed_this_tick =
			true;
		m_last_phase2_failure_diagnostic
			.phase2_capture_sample_time_us =
			m_phase2_capture_plan.producer_sample_time_us;
		const auto& diagnostics =
			m_phase2_observation.capture_diagnostics();
		const auto primary_block =
			diagnostics.primary_failed_block == Phase2CaptureBlock::Count
			? TelemetryPhase2Block::Identity
			: static_cast<TelemetryPhase2Block>(
				diagnostics.primary_failed_block);
		if (m_metrics != nullptr) {
			if (diagnostics.normalization_count != 0U)
				m_metrics->increment_mission(
					TelemetryMetricCounter::
						SourceNormalizations,
					diagnostics.normalization_count);
			for (std::size_t block = 0U;
				 block < static_cast<std::size_t>(
					 Phase2CaptureBlock::Count); ++block)
				if ((diagnostics.attempted_mask &
					 static_cast<std::uint8_t>(1U << block)) != 0U)
					m_metrics->observe_phase2_capture(
						static_cast<TelemetryPhase2Block>(block),
						diagnostics.duration_ns[block] / 1000U);
		}
		switch (phase2_capture.status) {
		case Phase2CaptureStatus::Valid:
		case Phase2CaptureStatus::NoPlayer:
			break;
		case Phase2CaptureStatus::InvalidSource:
			if (m_metrics != nullptr)
				m_metrics->record_phase2_capture_failure(
					primary_block,
					phase2_capture.reason ==
						Phase2CaptureReason::
							InconsistentPlayerSource
					? TelemetryPhase2CaptureFailure::
						IncoherentTopology
					: TelemetryPhase2CaptureFailure::Guard);
			if (m_log != nullptr)
				m_log->phase2_source_rejected(primary_block,
					phase2_capture.reason ==
							Phase2CaptureReason::InconsistentPlayerSource
						? TelemetryPhase2CaptureFailure::IncoherentTopology
						: TelemetryPhase2CaptureFailure::Guard,
					context.now_us);
			[[fallthrough]];
		case Phase2CaptureStatus::SourceLimitExceeded:
			if (phase2_capture.status ==
					Phase2CaptureStatus::SourceLimitExceeded &&
				m_metrics != nullptr)
				m_metrics->record_phase2_capture_failure(
					primary_block,
					TelemetryPhase2CaptureFailure::OutOfRange);
			if (phase2_capture.status ==
					Phase2CaptureStatus::SourceLimitExceeded &&
				m_log != nullptr)
				m_log->phase2_source_rejected(primary_block,
					TelemetryPhase2CaptureFailure::OutOfRange,
					context.now_us);
			[[fallthrough]];
		case Phase2CaptureStatus::UnsupportedEngineState:
			if (phase2_capture.status ==
					Phase2CaptureStatus::UnsupportedEngineState &&
				m_metrics != nullptr)
				m_metrics->record_phase2_capture_failure(
					primary_block,
					TelemetryPhase2CaptureFailure::InvalidEnum);
			if (phase2_capture.status ==
					Phase2CaptureStatus::UnsupportedEngineState &&
				m_log != nullptr)
				m_log->phase2_source_rejected(primary_block,
					TelemetryPhase2CaptureFailure::InvalidEnum,
					context.now_us);
			m_phase2_capture_plan.capture_flight_controls = false;
			m_phase2_capture_plan.capture_systems = false;
			m_phase2_capture_plan.force_complete_keyframe = false;
			m_phase2_capture_plan.phase3_refresh_targeting = false;
			m_phase2_capture_plan.phase3_refresh_systems = false;
			break;
		default:
			m_phase2_capture_plan.capture_flight_controls = false;
			m_phase2_capture_plan.capture_systems = false;
			m_phase2_capture_plan.force_complete_keyframe = false;
			m_phase2_capture_plan.phase3_refresh_targeting = false;
			m_phase2_capture_plan.phase3_refresh_systems = false;
			break;
		}
		if ((phase2_capture.status == Phase2CaptureStatus::Valid ||
			 phase2_capture.status == Phase2CaptureStatus::NoPlayer) &&
			diagnostics.completed_refresh ==
			Phase2ObservationRefresh::Count) {
			m_phase2_observation.reset_observation_and_clear_phase2();
			m_controller.clear_player_observations();
			clear_player_capture();
			fail_capture(
				NativePlayerCaptureStatus::
					CaptureInvariantFailure);
			return NativeSessionTickStatus::
				PermanentCaptureFailure;
		}
		m_phase2_capture_plan.capture_flight_controls = true;
		if (diagnostics.completed_refresh ==
			Phase2ObservationRefresh::All)
			m_phase2_capture_plan.capture_systems = true;
		if (m_phase2_capture_plan.capture_systems) {
			if (phase2_capture.status == Phase2CaptureStatus::Valid)
				observe_phase2_support_transitions();
			else if (phase2_capture.status ==
				Phase2CaptureStatus::NoPlayer)
				reset_phase2_support_tracker();
		}
	}

	const auto capture_started = measure_performance ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	PlayerObservationDto observation;
	const auto capture = collect_player_kinematics(engine_view, context.now_us, observation);
	if (measure_performance) {
		m_last_performance_sample.collect_duration_ns += elapsed_nanoseconds(capture_started, std::chrono::steady_clock::now());
	}
	const auto diff_started = measure_performance ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	m_applying_engine_capture = true;
	const auto status = apply_collected_player_capture(capture, observation);
	m_applying_engine_capture = false;
	const auto capture_ended = std::chrono::steady_clock::now();
	const auto duration_us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
		capture_ended - capture_started).count());
	record_capture_metric(m_last_player_capture_status, duration_us);
	if (measure_performance) {
		m_last_performance_sample.diff_duration_ns = elapsed_nanoseconds(diff_started, capture_ended);
		m_last_performance_sample.tick_duration_ns = elapsed_nanoseconds(tick_started, capture_ended);
		refresh_performance_resource_sample();
	}
	return status;
}

void NativeSessionRuntime::observe_phase2_support_transitions() noexcept
{
	const auto& observation = m_phase2_observation.observation();
	const auto& diagnostics =
		m_phase2_observation.capture_diagnostics();
	if (observation.capture.status != Phase2CaptureStatus::Valid ||
		observation.ships.size() > m_phase2_support_tracker.size() ||
		diagnostics.source_count != observation.ships.size())
		return;
	m_phase2_support_seen_scratch = {};
	for (auto& entry : m_phase2_support_tracker) {
		if (!entry.active) continue;
		bool present = false;
		for (std::size_t ship_index = 0U;
			 ship_index < observation.ships.size(); ++ship_index)
			present = present ||
				diagnostics.source_signatures[ship_index] ==
					entry.signature;
		if (!present) entry = {};
	}
	for (std::size_t ship_index = 0U;
		 ship_index < observation.ships.size(); ++ship_index) {
		const auto& ship = observation.ships[ship_index];
		const auto signature =
			diagnostics.source_signatures[ship_index];
		if (ship.capture_key.value == 0U || signature == 0U) continue;
		std::size_t tracker_index = m_phase2_support_tracker.size();
		std::size_t free_index = m_phase2_support_tracker.size();
		for (std::size_t index = 0U;
			 index < m_phase2_support_tracker.size(); ++index) {
			const auto& entry = m_phase2_support_tracker[index];
			if (entry.active &&
				entry.signature == signature) {
				tracker_index = index;
				break;
			}
			if (!entry.active && free_index ==
					m_phase2_support_tracker.size())
				free_index = index;
		}
		if (tracker_index == m_phase2_support_tracker.size()) {
			if (free_index == m_phase2_support_tracker.size())
				continue;
			tracker_index = free_index;
			m_phase2_support_tracker[tracker_index] = {};
			m_phase2_support_tracker[tracker_index].capture_key =
				ship.capture_key;
			m_phase2_support_tracker[tracker_index].signature =
				signature;
			m_phase2_support_tracker[tracker_index].active = true;
		}
		m_phase2_support_tracker[tracker_index].capture_key =
			ship.capture_key;
		m_phase2_support_seen_scratch[tracker_index] = true;
		auto& entry = m_phase2_support_tracker[tracker_index];
		if (entry.previous == ship.support.phase) continue;
		entry.previous = ship.support.phase;
		const auto kind =
			telemetry_observed_support_kind(ship.support.phase);
		if (kind == TelemetryPhase2SupportKind::Count) continue;
		if (m_metrics != nullptr)
			m_metrics->record_phase2_support(kind);
	}
	for (std::size_t index = 0U;
		 index < m_phase2_support_tracker.size(); ++index)
		if (m_phase2_support_tracker[index].active &&
			!m_phase2_support_seen_scratch[index])
			m_phase2_support_tracker[index] = {};
}

void NativeSessionRuntime::reset_phase2_support_tracker() noexcept
{
	m_phase2_support_tracker = {};
	m_phase2_support_seen_scratch = {};
}

bool NativeSessionRuntime::provision_phase2_manifest_state() noexcept
{
	constexpr auto backing_bytes =
		protocol::MaxTransactionSize * 2U;
	auto backing = std::unique_ptr<std::uint8_t[]>(
		new (std::nothrow) std::uint8_t[backing_bytes]);
	auto source = std::unique_ptr<Phase2ManifestSource>(
		new (std::nothrow) Phase2ManifestSource());
	if (backing == nullptr || source == nullptr) return false;
	auto storage = std::unique_ptr<Phase2ManifestStorage>(
		new (std::nothrow) Phase2ManifestStorage(
			protocol::MutableByteView{
				backing.get(), backing_bytes}));
	if (storage == nullptr) return false;
	auto slot = std::unique_ptr<Phase2ManifestSlot>(
		new (std::nothrow) Phase2ManifestSlot(*storage));
	if (slot == nullptr) return false;
	m_phase2_manifest_backing = std::move(backing);
	m_phase2_manifest_source = std::move(source);
	m_phase2_manifest_storage = std::move(storage);
	m_phase2_manifest_slot = std::move(slot);
	m_phase2_manifest_backing_bytes =
		backing_bytes + sizeof(Phase2ManifestSource) +
		sizeof(Phase2ManifestStorage) +
		sizeof(Phase2ManifestSlot);
	return true;
}

bool NativeSessionRuntime::provision_phase2_catalog_source() noexcept
{
	auto source = std::unique_ptr<Phase2ManifestSource>(
		new (std::nothrow) Phase2ManifestSource());
	if (source == nullptr) return false;
	m_phase2_manifest_source = std::move(source);
	m_phase2_manifest_backing_bytes = sizeof(Phase2ManifestSource);
	return true;
}

bool NativeSessionRuntime::provision_phase3_manifest_states(
	std::size_t client_count) noexcept
{
	if (client_count == 0U ||
		client_count > m_phase3_manifest_states.size())
		return false;
	constexpr auto backing_bytes = protocol::MaxTransactionSize * 2U;
	auto workspace = std::unique_ptr<std::uint8_t[]>(
		new (std::nothrow) std::uint8_t[backing_bytes]);
	if (workspace == nullptr) return false;
	auto selection_source = std::unique_ptr<Phase2ManifestSource>(
		new (std::nothrow) Phase2ManifestSource());
	if (selection_source == nullptr) return false;
	std::size_t total_bytes = backing_bytes + sizeof(Phase2ManifestSource);
	for (std::size_t index = 0U; index < client_count; ++index) {
		auto storage = std::unique_ptr<Phase2ManifestStorage>(
			new (std::nothrow) Phase2ManifestStorage(
				protocol::MutableByteView{workspace.get(), backing_bytes}));
		if (storage == nullptr) {
			release_phase3_manifest_states();
			return false;
		}
		auto slot = std::unique_ptr<Phase2ManifestSlot>(
			new (std::nothrow) Phase2ManifestSlot(*storage));
		if (slot == nullptr ||
			!checked_add_size(total_bytes,
				sizeof(Phase2ManifestStorage) +
					sizeof(Phase2ManifestSlot),
				total_bytes)) {
			release_phase3_manifest_states();
			return false;
		}
		auto& state = m_phase3_manifest_states[index];
		state.storage = std::move(storage);
		state.slot = std::move(slot);
	}
	m_phase3_manifest_selection_source = std::move(selection_source);
	m_phase3_manifest_workspace = std::move(workspace);
	m_phase3_manifest_backing_bytes = total_bytes;
	return true;
}

bool NativeSessionRuntime::refresh_owned_phase2_manifest(
	const Phase2ObservationDto& observation,
	Phase2ManifestError& result) noexcept
{
	result = Phase2ManifestError::InvalidSource;
	if (m_phase2_manifest_source == nullptr ||
		(m_selected_phase2_profile != Phase2Profile::CockpitSensors &&
		 m_phase2_manifest_slot == nullptr))
		return m_phase2_manifest != nullptr;
	const auto projection = project_phase2_catalog(
		observation, *m_phase2_manifest_source);
	if (projection ==
		Phase2CatalogProjectionStatus::SourceTemporarilyUnavailable) {
		result = Phase2ManifestError::NoCatalogChange;
		return m_phase2_manifest != nullptr;
	}
	if (projection ==
		Phase2CatalogProjectionStatus::SourceLimitExceeded) {
		result = Phase2ManifestError::SourceLimitExceeded;
		return false;
	}
	if (projection != Phase2CatalogProjectionStatus::Success)
		return false;
	if (m_selected_phase2_profile == Phase2Profile::CockpitSensors) {
		result = Phase2ManifestError::NoCatalogChange;
		return true;
	}
	release_unreferenced_phase2_manifest_generation();
	result = m_phase2_manifest_slot->rebuild(
		*m_phase2_manifest_source);
	if (result == Phase2ManifestError::None) {
		const auto id =
			m_phase2_manifest_slot->staged_manifest_id();
		if (id == 0U ||
			m_phase2_manifest_slot->on_manifest_applied(id) !=
				Phase2ManifestError::None ||
			m_phase2_manifest_slot
					->on_dependent_snapshot_applied(1U, id) !=
				Phase2ManifestError::None)
			return false;
		m_phase2_manifest =
			&m_phase2_manifest_slot->active_candidate();
		m_phase2_catalog_projection_pending = false;
		return true;
	}
	if (result == Phase2ManifestError::NoCatalogChange ||
		result == Phase2ManifestError::TopologyOnly) {
		m_phase2_manifest =
			&m_phase2_manifest_slot->active_candidate();
		m_phase2_catalog_projection_pending = false;
		return m_phase2_manifest->manifest_id != 0U;
	}
	if (result == Phase2ManifestError::RebuildCoalesced) {
		m_phase2_manifest =
			&m_phase2_manifest_slot->active_candidate();
		m_phase2_catalog_projection_pending =
			!m_phase2_manifest_slot->source_catalog_matches_active(
				*m_phase2_manifest_source) ||
			m_phase2_manifest->topology_fingerprint !=
				m_phase2_manifest_source
					->topology_fingerprint;
		return m_phase2_manifest->manifest_id != 0U;
	}
	return false;
}

bool NativeSessionRuntime::refresh_phase3_manifest(
	std::size_t client_slot,
	const Phase2ObservationDto& observation,
	const Phase2Wp05SubjectBinding* bindings,
	std::size_t binding_count,
	Phase2ManifestError& result,
	Phase3EngineCollectDiagnostic& diagnostic) noexcept
{
	result = Phase2ManifestError::InvalidSource;
	diagnostic = {Phase3EngineCollectBlock::Precondition,
		Phase3EngineCollectStatus::InvalidSource};
	if (client_slot >= m_phase3_manifest_states.size() ||
		m_phase2_manifest_source == nullptr)
		return false;
	auto& state = m_phase3_manifest_states[client_slot];
	if (m_phase3_manifest_selection_source == nullptr || state.slot == nullptr)
		return false;
	const auto previous_manifest_id = state.slot->previous_manifest_id();
	if (previous_manifest_id != 0U &&
		client_slot < m_controller.owned_capacity().client_slots) {
		const auto& client = m_controller.slot(client_slot);
		const auto& manifest = client.phase2_runtime.manifest_state();
		if (client.required_manifest_id != previous_manifest_id &&
			manifest.active_id != previous_manifest_id &&
			manifest.staged_id != previous_manifest_id)
			state.slot->release_reliable_references(previous_manifest_id);
	}
	Phase3CatalogDependencies dependencies;
	const auto dependency_status =
		discover_phase3_catalog_dependencies(observation, dependencies);
	if (dependency_status != Phase3EngineCollectStatus::Collected) {
		diagnostic.status = dependency_status;
		return false;
	}
	if (!select_phase3_manifest_references(*m_phase2_manifest_source,
		observation, bindings, binding_count, dependencies,
		*m_phase3_manifest_selection_source))
		return false;
	if (m_phase3_manifest_workspace_owner !=
			m_phase3_manifest_states.size() &&
		m_phase3_manifest_workspace_owner != client_slot)
		return false;
	m_phase3_manifest_workspace_owner = client_slot;
	result = state.slot->rebuild(*m_phase3_manifest_selection_source);
	if (result == Phase2ManifestError::None) {
		const auto id = state.slot->staged_manifest_id();
		if (id == 0U ||
			state.slot->on_manifest_applied(id) !=
				Phase2ManifestError::None ||
			state.slot->on_dependent_snapshot_applied(1U, id) !=
				Phase2ManifestError::None)
			return false;
		state.manifest = &state.slot->active_candidate();
		state.catalog_projection_pending = false;
		diagnostic = {};
		return true;
	}
	if (result == Phase2ManifestError::NoCatalogChange ||
		result == Phase2ManifestError::TopologyOnly) {
		state.manifest = &state.slot->active_candidate();
		state.catalog_projection_pending = false;
		diagnostic = {};
		return state.manifest->manifest_id != 0U;
	}
	if (result == Phase2ManifestError::RebuildCoalesced) {
		state.manifest = &state.slot->active_candidate();
		state.catalog_projection_pending =
			!state.slot->source_catalog_matches_active(
				*m_phase3_manifest_selection_source) ||
			state.manifest->topology_fingerprint !=
				m_phase3_manifest_selection_source->topology_fingerprint;
		const auto ready = state.manifest->manifest_id != 0U;
		if (ready) diagnostic = {};
		return ready;
	}
	if (result == Phase2ManifestError::SourceLimitExceeded ||
		result == Phase2ManifestError::TooManySubsystemsPerShip ||
		result == Phase2ManifestError::TooManyAggregateSubsystems)
		diagnostic.status = Phase3EngineCollectStatus::SourceLimitExceeded;
	else if (result == Phase2ManifestError::AllocationFailed)
		diagnostic.status = Phase3EngineCollectStatus::AllocationFailure;
	m_phase3_manifest_workspace_owner = m_phase3_manifest_states.size();
	return false;
}

void NativeSessionRuntime::
	release_unreferenced_phase2_manifest_generation() noexcept
{
	if (m_phase2_manifest_slot == nullptr) return;
	const auto previous_id =
		m_phase2_manifest_slot->previous_manifest_id();
	if (previous_id == 0U) return;
	for (std::size_t index = 0U;
		 index < m_controller.owned_capacity().client_slots; ++index) {
		const auto& slot = m_controller.slot(index);
		const auto& state = slot.phase2_runtime.manifest_state();
		if (slot.required_manifest_id == previous_id ||
			state.active_id == previous_id ||
			state.staged_id == previous_id)
			return;
	}
	m_phase2_manifest_slot->release_reliable_references(
		previous_id);
}

void NativeSessionRuntime::release_phase2_manifest_state() noexcept
{
	m_phase2_manifest = nullptr;
	m_phase2_manifest_slot.reset();
	m_phase2_manifest_storage.reset();
	m_phase2_manifest_source.reset();
	m_phase2_manifest_backing.reset();
	m_phase2_manifest_backing_bytes = 0U;
	m_phase2_catalog_projection_pending = false;
}

void NativeSessionRuntime::release_phase3_manifest_states() noexcept
{
	for (auto& state : m_phase3_manifest_states) {
		state.manifest = nullptr;
		state.slot.reset();
		state.storage.reset();
		state.catalog_projection_pending = false;
	}
	m_phase3_manifest_selection_source.reset();
	m_phase3_manifest_workspace.reset();
	m_phase3_manifest_workspace_owner = m_phase3_manifest_states.size();
	m_phase3_manifest_backing_bytes = 0U;
}

void NativeSessionRuntime::prepare_phase2_keyframe(Phase2CapturePlan& plan) noexcept
{
	if (!m_phase2_enabled) {
		return;
	}
	plan.force_complete_keyframe = true;
	plan.capture_flight_controls = true;
	plan.capture_systems = true;
	plan.phase3_refresh_targeting = true;
	plan.phase3_refresh_systems = true;
}

void NativeSessionRuntime::stop_collection() noexcept
{
	if (m_controller_ready) {
		m_controller.clear_player_observations();
	}
	m_capture_cadence.stop();
	m_phase2_capture_plan = {};
	m_phase2_observation.reset_observation_and_clear_phase2();
	for (std::size_t index = 0U;
		 index < m_phase3_projections.size(); ++index) {
		if (m_phase3_projections[index] != nullptr)
			reset_phase3_projection(*m_phase3_projections[index]);
		if (m_phase3_projection_scratch[index] != nullptr)
			reset_phase3_projection(*m_phase3_projection_scratch[index]);
		if (m_phase3_identity_registries[index] != nullptr)
			m_phase3_identity_registries[index]->reset_session();
	}
	reset_phase2_support_tracker();
	clear_player_capture();
	m_last_player_capture_status = NativePlayerCaptureStatus::Inactive;
}

void NativeSessionRuntime::purge_all(SessionCloseReason reason) noexcept
{
	if (m_controller_ready) {
		m_controller.purge_all(reason);
	}
	const auto metric_reason = reason == SessionCloseReason::Timeout ? TelemetrySessionEndReason::Timeout :
			reason == SessionCloseReason::MissionDiscontinuity ? TelemetrySessionEndReason::MissionDiscontinuity :
			reason == SessionCloseReason::TransportError ? TelemetrySessionEndReason::TransportError :
			reason == SessionCloseReason::Shutdown ? TelemetrySessionEndReason::Shutdown : TelemetrySessionEndReason::ProtocolError;
	for (std::size_t index = 0U; index < m_metrics_session_active.size(); ++index) {
		if (m_metrics != nullptr && m_metrics_session_active[index]) {
			m_metrics->increment_session(index, TelemetryMetricCounter::SessionsEnded);
			m_metrics->record_session_end(metric_reason);
			m_metrics->deactivate_session(index);
		}
		if (m_log != nullptr) {
			const auto started_at = m_session_started_at_us[index];
			const auto duration = m_tick_context.now_us >= started_at ? m_tick_context.now_us - started_at : 0U;
			const auto total = m_metrics != nullptr
				? m_metrics->process_counter(TelemetryMetricCounter::SessionsEnded)
				: 0U;
			m_log->session_closed(index, log_reason(reason), duration, total);
		}
	}
	m_metrics_session_active = {};
	m_session_started_at_us = {};
	m_phase2_started_snapshot_sequences = {};
	m_capture_cadence.stop();
	m_phase2_capture_plan = {};
	m_phase2_observation.reset_observation_and_clear_phase2();
	for (std::size_t index = 0U;
		 index < m_phase3_projections.size(); ++index) {
		if (m_phase3_projections[index] != nullptr)
			reset_phase3_projection(*m_phase3_projections[index]);
		if (m_phase3_projection_scratch[index] != nullptr)
			reset_phase3_projection(*m_phase3_projection_scratch[index]);
		if (m_phase3_identity_registries[index] != nullptr)
			m_phase3_identity_registries[index]->reset_session();
	}
	reset_phase2_support_tracker();
	clear_player_capture();
}

void NativeSessionRuntime::shutdown() noexcept
{
	if (m_state == State::Stopped) {
		return;
	}
	purge_all(SessionCloseReason::Shutdown);
	m_transport.close();
	if (m_metrics != nullptr) {
		m_metrics->set_udp_sockets_open(0U);
	}
	m_scheduler.reset();
	m_capture_cadence.reset();
	m_phase2_capture_plan = {};
	reset_phase2_observation_buffer_in_place(m_phase2_observation);
	m_selected_phase2_profile = Phase2Profile::None;
	m_phase2_enabled = false;
	m_systems_capture_cadence.reset();
	m_phase2_owned_budget = {};
	m_phase2_event_batch_scratch = {};
	m_phase2_fanout_scratch = {};
	reset_phase2_support_tracker();
	m_startup_owned_bytes = 0U;
	m_startup_allocation_count = 0U;
	clear_player_capture();
	release_state_image_pools();
	m_state = State::Stopped;
}

std::size_t NativeSessionRuntime::socket_count() const noexcept
{
	return m_transport.socket_count();
}

std::size_t NativeSessionRuntime::active_sessions() const noexcept
{
	return m_controller_ready ? m_controller.active_slots() : 0U;
}

SessionControllerOwnedUsage NativeSessionRuntime::owned_usage() const noexcept
{
	return m_controller_ready ? m_controller.owned_usage() : SessionControllerOwnedUsage{};
}

bool NativeSessionRuntime::ingress_ready() const noexcept
{
	return m_state == State::Started && m_transport.is_open();
}

bool NativeSessionRuntime::egress_ready() const noexcept
{
	return m_state == State::Started && m_transport.is_open() && m_controller.has_output();
}

IoStatus NativeSessionRuntime::try_receive() noexcept
{
	const auto received = m_transport.try_receive();
	if (m_performance_observation_active) ++m_last_performance_sample.syscall_count;
	if (m_metrics != nullptr) {
		const auto result = received.status == IoStatus::Complete ? TelemetryIoResult::Complete :
			received.status == IoStatus::WouldBlock ? TelemetryIoResult::WouldBlock :
			received.status == IoStatus::Closed ? TelemetryIoResult::Closed : TelemetryIoResult::Error;
		m_metrics->record_datagram(TelemetryDirection::Rx, result,
			received.status == IoStatus::Complete ? received.datagram.bytes.size : 0U);
	}
	if (received.status == IoStatus::Complete &&
		received.disposition == TransportReceiveDisposition::Datagram) {
		const auto ingress = m_controller.ingest(received.datagram.endpoint,
			received.datagram.bytes,
			m_tick_context.now_us,
			m_tick_context.mission_generation,
			m_tick_context.mission_active);
		m_capture_after_ready_transition = m_capture_after_ready_transition ||
			ingress.disposition ==
				SessionIngressDisposition::WelcomeProofApplied ||
			ingress.has_phase2_manifest_applied;
		if (m_log != nullptr && m_phase2_enabled &&
			ingress.has_phase2_resync) {
			const auto result = ingress.phase2_resync_result ==
					protocol::ProducerResyncResult::AcceptedNewCandidate
				? TelemetryLogPhase2ResyncResult::AcceptedNewCandidate
				: TelemetryLogPhase2ResyncResult::AcceptedCoalesced;
			m_log->phase2_resync(ingress.phase2_resync_slot, result);
		}
		if (m_log != nullptr && ingress.disposition == SessionIngressDisposition::Dropped) {
			m_log->record_drop(log_drop_reason(ingress.drop_reason));
		}
	} else if (m_log != nullptr && received.status == IoStatus::WouldBlock) {
		m_log->record_drop(TelemetryLogDrop::WouldBlock);
	}
	return received.status;
}

IoStatus NativeSessionRuntime::try_send() noexcept
{
	SessionControllerOutput output;
	if (!m_controller.peek_output(output)) {
		return IoStatus::Error;
	}
	const auto status = m_transport.try_send(output.endpoint, {output.bytes.data(), output.size});
	if (m_performance_observation_active) ++m_last_performance_sample.syscall_count;
	if (m_metrics != nullptr) {
		const auto result = status == IoStatus::Complete ? TelemetryIoResult::Complete :
			status == IoStatus::WouldBlock ? TelemetryIoResult::WouldBlock :
			status == IoStatus::Closed ? TelemetryIoResult::Closed : TelemetryIoResult::Error;
		m_metrics->record_datagram(TelemetryDirection::Tx, result, output.size);
	}
	m_output_completion.complete(m_controller, status);
	if (m_log != nullptr && status == IoStatus::WouldBlock) m_log->record_drop(TelemetryLogDrop::WouldBlock);
	return status;
}

NativeSessionTickStatus NativeSessionRuntime::service_r2_tick(const NativeSessionTickContext& context) noexcept
{
	if (m_state == State::Faulted) {
		return m_fault_status;
	}
	if (m_state != State::Started || !m_controller_ready) {
		return NativeSessionTickStatus::Unavailable;
	}

	m_capture_after_ready_transition = false;
	m_capture_for_phase3_keyframe = false;
	m_tick_context = context;
	m_controller.expire_housekeeping(context.now_us);
	m_controller.service_timeouts(context.now_us);
	m_controller.service_reliability(context.now_us);
	m_controller.service_periodic(context.now_us);
	refresh_metrics_session_scope();
	refresh_log_budget_high_water();
	const auto measure_performance = m_performance_observation_active;
	const auto network_started = measure_performance ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	const auto result = m_scheduler.run_tick(m_maximum_attempts, *this);
	if (measure_performance) {
		m_last_performance_sample.network_duration_ns = elapsed_nanoseconds(network_started, std::chrono::steady_clock::now());
	}
	if (!result.valid_budget || result.terminal_status == IoStatus::Closed ||
		result.terminal_status == IoStatus::Error) {
		if (m_log != nullptr) m_log->flush_drop_summary(context.now_us);
		fail_transport();
		return NativeSessionTickStatus::PermanentTransportFailure;
	}
	// Ingress/control and existing reliable output are serviced first. Only the
	// idle tail may make an initial snapshot available for a later scheduler
	// pass, so state can never occupy the output slot ahead of a heartbeat or
	// other control response received in this tick.
	const auto serialization_started = measure_performance ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	auto attempts_remaining = static_cast<std::uint16_t>(
		m_maximum_attempts - result.attempts);
	const auto queue_next_state = [&]() noexcept {
		const auto queue_budget = static_cast<std::size_t>(
			attempts_remaining == 0U ? 1U : attempts_remaining);
		bool queued = m_controller.service_next_phase2_manifest_egress(
			context.now_us) != 0U;
		if (!queued)
			queued = m_controller.service_initial_snapshot_egress(
				queue_budget, context.now_us) != 0U;
		m_capture_for_phase3_keyframe =
			m_controller.phase3_complete_capture_required();
		// Deltas are the lowest-priority state traffic and are only exposed after
		// control, reliable snapshot work and heartbeat processing above.
		if (!queued && !m_capture_for_phase3_keyframe)
			queued = m_controller.service_delta_egress(
				queue_budget, context.now_us) != 0U;
		return queued;
	};
	while (attempts_remaining != 0U) {
		if (!queue_next_state())
			break;

		const auto follow_started = measure_performance
			? std::chrono::steady_clock::now()
			: std::chrono::steady_clock::time_point{};
		const auto follow = m_scheduler.run_tick(
			attempts_remaining, *this);
		if (measure_performance)
			m_last_performance_sample.network_duration_ns +=
				elapsed_nanoseconds(follow_started,
					std::chrono::steady_clock::now());
		if (!follow.valid_budget ||
			follow.terminal_status == IoStatus::Closed ||
			follow.terminal_status == IoStatus::Error) {
			if (m_log != nullptr)
				m_log->flush_drop_summary(context.now_us);
			fail_transport();
			return NativeSessionTickStatus::PermanentTransportFailure;
		}
		attempts_remaining = static_cast<std::uint16_t>(
			attempts_remaining - follow.attempts);
		if (follow.attempts == 0U || m_controller.has_output())
			break;
	}
	// A fully consumed budget still prepares one tail item for the next tick.
	// This preserves progress for maxDatagramsPerTick=1 without overspending.
	if (!m_controller.has_output())
		(void)queue_next_state();
	if (measure_performance) {
		m_last_performance_sample.serialization_duration_ns = elapsed_nanoseconds(serialization_started, std::chrono::steady_clock::now());
	}
	if (m_log != nullptr) m_log->flush_drop_summary(context.now_us);
	return NativeSessionTickStatus::Complete;
}

void NativeSessionRuntime::refresh_performance_resource_sample() noexcept
{
	if (!m_performance_observation_active || !m_controller_ready) return;
	const auto usage = m_controller.owned_usage();
	m_last_performance_sample.allocation_events = m_controller.phase1_observed_allocation_count();
	m_last_performance_sample.queue_depth = usage.reliable_items + (usage.output_queued ? 1U : 0U);
	m_last_performance_sample.baselines_active = 0U;
	m_last_performance_sample.keyframe = false;
	for (std::size_t index = 0U; index < m_controller.owned_capacity().client_slots; ++index) {
		const auto& snapshot = m_controller.slot(index).snapshot;
		if (snapshot.has_active_baseline()) ++m_last_performance_sample.baselines_active;
		if (snapshot.has_candidate()) m_last_performance_sample.keyframe = true;
	}
}

void NativeSessionRuntime::refresh_metrics_session_scope() noexcept
{
	if (!m_controller_ready) return;
	for (std::size_t index = 0U; index < m_controller.owned_capacity().client_slots; ++index) {
		const auto& slot = m_controller.slot(index);
		const bool active = slot.progress != ProducerSessionProgress::Empty;
		if (active && !m_metrics_session_active[index]) {
			if (m_metrics != nullptr) {
				m_metrics->activate_session(index, static_cast<std::uint64_t>(slot.progress));
				m_metrics->increment_session(index, TelemetryMetricCounter::SessionsStarted);
				m_metrics->set_phase2_profile(index,
					telemetry_profile(m_selected_phase2_profile));
			}
			m_session_started_at_us[index] = slot.session_start_us;
			if (m_log != nullptr) {
				m_log->session_opened(index);
				if (m_selected_phase2_profile !=
					Phase2Profile::None)
					m_log->phase2_profile_selected(index,
						telemetry_profile(
							m_selected_phase2_profile),
						phase2_profile_coverage(
							m_selected_phase2_profile));
			}
		} else if (!active && m_metrics_session_active[index]) {
			if (m_phase3_projections[index] != nullptr)
				reset_phase3_projection(*m_phase3_projections[index]);
			if (m_phase3_projection_scratch[index] != nullptr)
				reset_phase3_projection(*m_phase3_projection_scratch[index]);
			if (m_phase3_identity_registries[index] != nullptr)
				m_phase3_identity_registries[index]->reset_session();
			if (m_metrics != nullptr) {
				m_metrics->increment_session(index, TelemetryMetricCounter::SessionsEnded);
				m_metrics->deactivate_session(index);
			}
			if (m_log != nullptr) {
				const auto started_at = m_session_started_at_us[index];
				const auto duration = m_tick_context.now_us >= started_at ? m_tick_context.now_us - started_at : 0U;
				const auto total = m_metrics != nullptr
					? m_metrics->process_counter(TelemetryMetricCounter::SessionsEnded)
					: 0U;
				if (m_selected_phase2_profile !=
					Phase2Profile::None)
					m_log->phase2_summary(index, total,
						m_metrics != nullptr
							? m_metrics->phase2_memory_high_water(
								TelemetryPhase2MemoryScope::ProcessTotal)
							: 0U);
				m_log->session_closed(index, TelemetryLogReason::PeerClosed, duration, total);
			}
			m_session_started_at_us[index] = 0U;
		}
		m_metrics_session_active[index] = active;
		if (active && m_metrics != nullptr) {
			m_metrics->set_session_gauges(index,
				static_cast<std::uint64_t>(slot.progress),
				0U, 0U, 0U,
				slot.snapshot.has_candidate() ? 1U : 0U,
				slot.snapshot.has_active_baseline() ? 1U : 0U);
			if (m_selected_phase2_profile !=
					Phase2Profile::None) {
				const auto started_sequence =
					slot.phase2_runtime.started_snapshot_sequence();
				if (started_sequence !=
					m_phase2_started_snapshot_sequences[index]) {
					m_phase2_started_snapshot_sequences[index] =
						started_sequence;
					const auto reason = telemetry_keyframe_reason(
						slot.phase2_runtime
							.last_started_snapshot_cause());
					if (reason !=
						TelemetryPhase2KeyframeReason::Count)
						m_metrics->record_phase2_forced_keyframe(
							index, reason);
				}
				const auto* latches =
					m_controller.phase2_support_latches(index);
				m_metrics->set_phase2_support_latches(index,
					latches != nullptr
						? latches->size(index) : 0U);
				if (slot.phase2_runtime.active_snapshot_id() != 0U) {
					for (std::size_t block = 0U;
						 block < static_cast<std::size_t>(
							 TelemetryPhase2Block::Count);
						 ++block) {
						const auto sample = slot.phase2_runtime
							.active_block_sample(block);
						m_metrics->set_phase2_sample_age(index,
							static_cast<TelemetryPhase2Block>(block),
							m_tick_context.now_us >= sample
								? m_tick_context.now_us - sample
								: 0U);
					}
				}
			}
		}
	}
}

void NativeSessionRuntime::refresh_log_budget_high_water() noexcept
{
	if (m_log == nullptr || !m_controller_ready) return;
	const auto capacity = m_controller.owned_capacity();
	const auto usage = m_controller.owned_usage();
	if (capacity.client_slots != 0U && usage.active_slots >= capacity.client_slots) {
		m_log->budget_high_water(TelemetryLogBudget::SessionSlots, capacity.client_slots, usage.active_slots);
	}
	const auto reliable_item_limit = capacity.client_slots * protocol::ReliableWindowMaximumEntries;
	if (reliable_item_limit != 0U && usage.reliable_items != 0U) {
		m_log->budget_high_water(TelemetryLogBudget::ReliableWindow, reliable_item_limit,
			usage.reliable_items);
	}
	if (capacity.state_reassembly_bytes != 0U && usage.reassembly_bytes != 0U) {
		m_log->budget_high_water(TelemetryLogBudget::ReassemblyBytes, capacity.state_reassembly_bytes,
			usage.reassembly_bytes);
	}
}

void NativeSessionRuntime::record_capture_metric(NativePlayerCaptureStatus status, std::uint64_t duration_us) noexcept
{
	if (m_metrics == nullptr) return;
	m_metrics->observe_mission(TelemetryMetricHistogram::CaptureDuration, duration_us);
	if (status == NativePlayerCaptureStatus::CapturedValid) {
		m_metrics->increment_mission(TelemetryMetricCounter::CaptureAttempts);
		m_metrics->record_capture_result(TelemetryCaptureResult::Valid);
	} else if (status == NativePlayerCaptureStatus::CapturedNoPlayer) {
		m_metrics->increment_mission(TelemetryMetricCounter::CaptureAttempts);
		m_metrics->record_capture_result(TelemetryCaptureResult::NoPlayer);
	} else if (status == NativePlayerCaptureStatus::CapturedInvalidSource) {
		m_metrics->increment_mission(TelemetryMetricCounter::CaptureAttempts);
		m_metrics->record_capture_result(TelemetryCaptureResult::InvalidSource);
	}
}

NativeSessionTickStatus NativeSessionRuntime::apply_collected_player_capture(const CaptureResult& result,
	const PlayerObservationDto& observation) noexcept
{
	NativePlayerCaptureStatus status;
	PlayerObservationDto effective_observation;
	if (result.status == CaptureStatus::Valid && result.reason == CaptureReason::None) {
		status = NativePlayerCaptureStatus::CapturedValid;
		effective_observation = observation;
	} else if (result.status == CaptureStatus::NoPlayer && result.reason >= CaptureReason::NotInMission &&
		result.reason <= CaptureReason::MissingPlayerShip) {
		status = NativePlayerCaptureStatus::CapturedNoPlayer;
	} else if (result.status == CaptureStatus::InvalidSource && result.reason >= CaptureReason::WrongObjectType &&
		result.reason < CaptureReason::Count) {
		status = NativePlayerCaptureStatus::CapturedInvalidSource;
	} else {
		if (auto* diagnostic =
				begin_phase2_failure_diagnostic(
					NativePhase2FailureStage::
						PlayerCaptureClassification);
			diagnostic != nullptr) {
			diagnostic->player_capture_status = result.status;
			diagnostic->player_capture_reason = result.reason;
		}
		fail_capture(NativePlayerCaptureStatus::CaptureInvariantFailure);
		return NativeSessionTickStatus::PermanentCaptureFailure;
	}

	const auto materialization = m_controller.apply_player_observation(result, effective_observation);
	CurrentPlayerCapture current;
	current.available = true;
	current.result = result;
	current.observation = effective_observation;
	m_current_player_capture = current;
	m_last_player_materialization = materialization;
	m_last_player_capture_status = status;

	if (!m_applying_engine_capture || !m_tick_context.mission_active) {
		return NativeSessionTickStatus::Complete;
	}
	if (m_selected_phase2_profile != Phase2Profile::None) {
		const auto& phase2 = m_phase2_observation.observation();
		const auto phase2_valid =
			phase2.capture.status == Phase2CaptureStatus::Valid;
		const auto phase2_no_player =
			phase2.capture.status == Phase2CaptureStatus::NoPlayer &&
			phase2.capture.reason == Phase2CaptureReason::None;
		if (!phase2_valid && !phase2_no_player)
			return NativeSessionTickStatus::Complete;
		if (phase2_valid &&
			(m_phase2_capture_plan.capture_systems ||
			 m_phase2_manifest == nullptr)) {
			Phase2ManifestError manifest_result{};
			if (!refresh_owned_phase2_manifest(
					phase2, manifest_result)) {
				if (m_metrics != nullptr)
					m_metrics->record_phase2_manifest(
						0U,
						TelemetryPhase2ManifestResult::Rejected,
						0U, 0U, 0U);
				return NativeSessionTickStatus::Complete;
			}
		}
		if (m_selected_phase2_profile != Phase2Profile::CockpitSensors &&
			m_phase2_manifest == nullptr)
			return NativeSessionTickStatus::Complete;
	}
	const auto mission_generation = m_tick_context.mission_generation == 0U ? 1U : m_tick_context.mission_generation;
	for (std::size_t index = 0U; index < m_controller.owned_capacity().client_slots; ++index) {
		const auto& slot = m_controller.slot(index);
		if (slot.progress != ProducerSessionProgress::ReadyForState) {
			continue;
		}
		if (m_selected_phase2_profile ==
				Phase2Profile::CompleteShip ||
			m_selected_phase2_profile ==
				Phase2Profile::CockpitSensors) {
			const auto& phase2 =
				m_phase2_observation.observation();
			const auto complete_ship_capture_valid =
				status == NativePlayerCaptureStatus::CapturedValid &&
				phase2.capture.status == Phase2CaptureStatus::Valid;
			const auto complete_ship_no_player =
				status == NativePlayerCaptureStatus::CapturedNoPlayer &&
				phase2.capture.status == Phase2CaptureStatus::NoPlayer &&
				phase2.capture.reason == Phase2CaptureReason::None &&
				phase2.player_key.value == 0U &&
				phase2.ships.empty();
			if ((!complete_ship_capture_valid &&
				 !complete_ship_no_player) ||
				phase2.ships.size() >
					MaximumPhase2ObservationShips) {
				if (auto* diagnostic =
						begin_phase2_failure_diagnostic(
							NativePhase2FailureStage::
								Phase2Precondition);
					diagnostic != nullptr) {
					diagnostic->ship_count =
						phase2.ships.size();
					diagnostic->player_key =
						phase2.player_key.value;
				}
				fail_capture(
					NativePlayerCaptureStatus::
						CaptureInvariantFailure);
				return NativeSessionTickStatus::
					PermanentCaptureFailure;
			}
			if (m_phase2_catalog_projection_pending) {
				const auto manifest_state =
					m_controller.stage_phase2_manifest(
						index, *m_phase2_manifest,
						m_tick_context.now_us);
				if (manifest_state ==
						Phase2RuntimeResult::InvalidInput ||
					manifest_state ==
						Phase2RuntimeResult::Stale ||
					manifest_state ==
						Phase2RuntimeResult::CounterExhausted) {
					if (auto* diagnostic =
							begin_phase2_failure_diagnostic(
								NativePhase2FailureStage::
									Manifest);
						diagnostic != nullptr) {
						diagnostic->runtime_result =
							manifest_state;
						diagnostic->ship_count =
							phase2.ships.size();
						diagnostic->player_key =
							phase2.player_key.value;
						diagnostic->global_manifest_id =
							m_phase2_manifest->manifest_id;
						diagnostic->required_manifest_id =
							slot.required_manifest_id;
						diagnostic->client_slot = index;
						diagnostic->required_manifest_applied =
							slot.required_manifest_applied;
					}
					fail_capture(
						NativePlayerCaptureStatus::
							CaptureInvariantFailure);
					return NativeSessionTickStatus::
						PermanentCaptureFailure;
				}
				continue;
			}
			std::array<Phase2CaptureLocalKey,
				MaximumPhase2ObservationShips> binding_keys{};
			std::array<Phase2CaptureLocalKey,
				MaximumPhase2ObservationShips> identity_signatures{};
			std::array<std::uint64_t,
				MaximumPhase2ObservationShips> public_entity_ids{};
			std::array<Phase2Wp05SubjectBinding,
				MaximumPhase2ObservationShips> bindings{};
			for (std::size_t subject = 0U;
				 subject < phase2.ships.size(); ++subject)
				binding_keys[subject] =
					phase2.ships[subject].capture_key;
			const auto cockpit_sensors = m_selected_phase2_profile ==
				Phase2Profile::CockpitSensors;
			if (cockpit_sensors) {
				// Flight-only refreshes deliberately clear the diagnostics for the
				// current attempt while retaining the CompleteShip observation. Use
				// the accepted full-capture map that owns those ship rows.
				const auto& capture_diagnostics =
					m_phase2_observation.accepted_capture_map();
				auto* identities =
					m_phase3_identity_registries[index].get();
				if (identities == nullptr ||
					capture_diagnostics.source_count !=
						phase2.ships.size()) {
					m_last_phase2_failure_diagnostic.phase3_diagnostic = {
						Phase3EngineCollectBlock::Precondition,
						Phase3EngineCollectStatus::InvalidSource};
					record_phase3_failure(m_metrics, m_log, index,
						TelemetryPhase3Block::Precondition,
						TelemetryPhase3CaptureFailure::InvalidSource);
					fail_capture(NativePlayerCaptureStatus::
						CaptureInvariantFailure);
					return NativeSessionTickStatus::
						PermanentCaptureFailure;
				}
				for (std::size_t subject = 0U;
					 subject < phase2.ships.size(); ++subject) {
					identity_signatures[subject].value =
						capture_diagnostics.source_signatures[subject];
					const auto identity = identities->resolve({
						identity_signatures[subject].value,
						static_cast<std::uint8_t>(
							protocol::ObjectType::Ship)});
					if (identity.status !=
							Phase3IdentityResolveStatus::Existing &&
						identity.status !=
							Phase3IdentityResolveStatus::Allocated) {
						m_last_phase2_failure_diagnostic.phase3_diagnostic = {
							Phase3EngineCollectBlock::Precondition,
							Phase3EngineCollectStatus::IdentityFailure};
						record_phase3_failure(m_metrics, m_log, index,
							TelemetryPhase3Block::Precondition,
							TelemetryPhase3CaptureFailure::IdentityFailure);
						fail_capture(NativePlayerCaptureStatus::
							CaptureInvariantFailure);
						return NativeSessionTickStatus::
							PermanentCaptureFailure;
					}
					public_entity_ids[subject] = identity.entity_id;
				}
			}
			const auto manifest_started =
				std::chrono::steady_clock::now();
			const auto closure = cockpit_sensors
				? m_controller.reconcile_phase2_closure_with_public_ids(
					index, identity_signatures.data(), binding_keys.data(),
					public_entity_ids.data(), phase2.ships.size(),
					bindings.data(), bindings.size())
				: m_controller.reconcile_phase2_closure(
					index, binding_keys.data(), phase2.ships.size(),
					bindings.data(), bindings.size());
			if (m_metrics != nullptr)
				m_metrics->record_phase2_closure(
					closure == Phase2RuntimeResult::Applied
					? TelemetryPhase2ClosureResult::Created
					: closure == Phase2RuntimeResult::NoChange
					? TelemetryPhase2ClosureResult::Unchanged
					: closure ==
							Phase2RuntimeResult::SnapshotRequired
					? TelemetryPhase2ClosureResult::TopologyChanged
					: closure ==
							Phase2RuntimeResult::ManifestRequired
					? TelemetryPhase2ClosureResult::CatalogChanged
					: TelemetryPhase2ClosureResult::Rejected);
			if (closure ==
					Phase2RuntimeResult::InvalidInput ||
				closure ==
					Phase2RuntimeResult::CapacityExceeded ||
				closure ==
					Phase2RuntimeResult::CounterExhausted) {
				if (auto* diagnostic =
						begin_phase2_failure_diagnostic(
							NativePhase2FailureStage::
								Closure);
					diagnostic != nullptr) {
					diagnostic->runtime_result = closure;
					diagnostic->ship_count =
						phase2.ships.size();
					diagnostic->player_key =
						phase2.player_key.value;
				}
				fail_capture(
					NativePlayerCaptureStatus::
						CaptureInvariantFailure);
				return NativeSessionTickStatus::
					PermanentCaptureFailure;
			}
			const Phase2ManifestCandidate* phase2_manifest =
				m_phase2_manifest;
			const Phase2ManifestSlot* manifest_slot =
				m_phase2_manifest_slot.get();
			if (m_selected_phase2_profile ==
				Phase2Profile::CockpitSensors) {
				Phase2ManifestError manifest_error{};
				Phase3EngineCollectDiagnostic manifest_diagnostic;
				if (!refresh_phase3_manifest(index, phase2, bindings.data(),
						phase2.ships.size(), manifest_error,
						manifest_diagnostic)) {
					m_last_phase2_failure_diagnostic.phase3_diagnostic =
						manifest_diagnostic;
					record_phase3_failure(m_metrics, m_log, index,
						telemetry_phase3_block(manifest_diagnostic.block),
						telemetry_phase3_failure(manifest_diagnostic.status));
					if (auto* diagnostic = begin_phase2_failure_diagnostic(
							NativePhase2FailureStage::Manifest);
						diagnostic != nullptr) {
						diagnostic->ship_count = phase2.ships.size();
						diagnostic->player_key = phase2.player_key.value;
						diagnostic->runtime_result =
							Phase2RuntimeResult::InvalidInput;
						diagnostic->phase3_diagnostic =
							manifest_diagnostic;
					}
					fail_capture(NativePlayerCaptureStatus::CaptureInvariantFailure);
					return NativeSessionTickStatus::PermanentCaptureFailure;
				}
				const auto& cockpit_manifest =
					m_phase3_manifest_states[index];
				phase2_manifest = cockpit_manifest.manifest;
				manifest_slot = cockpit_manifest.slot.get();
			}
			if (phase2_manifest == nullptr || manifest_slot == nullptr)
				return NativeSessionTickStatus::Complete;
			const auto lifecycle_before =
				slot.phase2_runtime.pending_lifecycle_count();
			const auto lifecycle =
				m_controller.observe_phase2_lifecycle(
					index, phase2, bindings.data(),
					phase2.ships.size());
			if (lifecycle ==
					Phase2RuntimeResult::InvalidInput ||
				lifecycle ==
					Phase2RuntimeResult::CapacityExceeded ||
				lifecycle ==
					Phase2RuntimeResult::CounterExhausted) {
				if (auto* diagnostic =
						begin_phase2_failure_diagnostic(
							NativePhase2FailureStage::
								Lifecycle);
					diagnostic != nullptr) {
					diagnostic->runtime_result = lifecycle;
					diagnostic->ship_count =
						phase2.ships.size();
					diagnostic->player_key =
						phase2.player_key.value;
				}
				fail_capture(
					NativePlayerCaptureStatus::
						CaptureInvariantFailure);
				return NativeSessionTickStatus::
					PermanentCaptureFailure;
			}
			const auto lifecycle_after =
				slot.phase2_runtime.pending_lifecycle_count();
			for (std::size_t event = lifecycle_before;
				 event < lifecycle_after; ++event) {
				const auto kind = telemetry_lifecycle_kind(
					slot.phase2_runtime.pending_lifecycle(event).kind);
				if (m_metrics != nullptr)
					m_metrics->record_phase2_lifecycle(kind);
				if (m_log != nullptr)
					m_log->phase2_lifecycle(index, kind);
			}
			const auto rebuild_was_pending =
				slot.phase2_runtime.manifest_state().rebuild_intent;
			const auto manifest_state =
				m_controller.stage_phase2_manifest(index,
					*phase2_manifest,
					m_tick_context.now_us);
			if (m_selected_phase2_profile ==
					Phase2Profile::CockpitSensors &&
				manifest_state != Phase2RuntimeResult::CandidateBusy)
				m_phase3_manifest_workspace_owner =
					m_phase3_manifest_states.size();
			if (manifest_state == Phase2RuntimeResult::CandidateBusy &&
				!rebuild_was_pending &&
				slot.phase2_runtime.manifest_state().rebuild_intent &&
				m_metrics != nullptr)
				m_metrics->record_phase2_manifest_rebuild_coalesced(
					index);
			const auto manifest_duration_us =
				elapsed_nanoseconds(manifest_started,
					std::chrono::steady_clock::now()) / 1000U;
			if (m_performance_observation_active)
				m_last_performance_sample.state_image_build_duration_ns +=
					manifest_duration_us * 1000U;
			if (m_metrics != nullptr) {
				m_metrics->set_phase2_closure(
					phase2_manifest->class_record_count,
					phase2.ships.size(),
					phase2_manifest->weapon_record_count,
					phase2_manifest->aggregate_subsystem_count);
				if (manifest_state == Phase2RuntimeResult::Applied ||
					manifest_state == Phase2RuntimeResult::InvalidInput ||
					manifest_state == Phase2RuntimeResult::Stale ||
					manifest_state ==
						Phase2RuntimeResult::CounterExhausted)
					m_metrics->record_phase2_manifest(index,
						manifest_state ==
							Phase2RuntimeResult::Applied
						? TelemetryPhase2ManifestResult::Built
						: TelemetryPhase2ManifestResult::Rejected,
						phase2_manifest->encoded_size,
						phase2_manifest->part_count,
						manifest_duration_us);
			}
			if (m_log != nullptr &&
				(manifest_state == Phase2RuntimeResult::Applied ||
				 manifest_state == Phase2RuntimeResult::InvalidInput ||
				 manifest_state == Phase2RuntimeResult::Stale ||
				 manifest_state ==
					Phase2RuntimeResult::CounterExhausted))
				m_log->phase2_manifest(index,
					manifest_state == Phase2RuntimeResult::Applied
					? TelemetryLogEvent::Phase2ManifestBuilt
					: TelemetryLogEvent::Phase2ManifestRejected,
					phase2_manifest->manifest_id,
					phase2_manifest->class_record_count +
						phase2_manifest->weapon_record_count +
						phase2_manifest->aggregate_subsystem_count,
					phase2_manifest->part_count,
					phase2_manifest->encoded_size,
					manifest_duration_us);
			if (manifest_state ==
					Phase2RuntimeResult::InvalidInput ||
				manifest_state ==
					Phase2RuntimeResult::Stale ||
				manifest_state ==
					Phase2RuntimeResult::CounterExhausted) {
				if (auto* diagnostic =
						begin_phase2_failure_diagnostic(
							NativePhase2FailureStage::
								Manifest);
					diagnostic != nullptr) {
					diagnostic->runtime_result =
						manifest_state;
					diagnostic->ship_count =
						phase2.ships.size();
					diagnostic->player_key =
						phase2.player_key.value;
				}
				fail_capture(
					NativePlayerCaptureStatus::
						CaptureInvariantFailure);
				return NativeSessionTickStatus::
					PermanentCaptureFailure;
			}
			// A staged manifest is deliberately not treated as installed.
			// Until its reliable APPLIED transition reaches
			// apply_phase2_manifest(), no dependent snapshot is emitted.
			if (slot.required_manifest_id !=
					phase2_manifest->manifest_id ||
				!slot.required_manifest_applied)
				continue;
			const auto* installed_manifest =
				manifest_slot->candidate_for_id(
					slot.required_manifest_id);
			if (installed_manifest == nullptr ||
				installed_manifest->manifest_id !=
					slot.required_manifest_id) {
				if (auto* diagnostic =
						begin_phase2_failure_diagnostic(
							NativePhase2FailureStage::
								Manifest);
					diagnostic != nullptr)
					diagnostic->runtime_result =
						Phase2RuntimeResult::InvalidInput;
				fail_capture(
					NativePlayerCaptureStatus::
						CaptureInvariantFailure);
				return NativeSessionTickStatus::
					PermanentCaptureFailure;
			}
			std::uint64_t player_entity_id = 0U;
			for (std::size_t subject = 0U;
				 subject < phase2.ships.size(); ++subject)
				if (bindings[subject].capture_key.value ==
					phase2.player_key.value) {
					player_entity_id =
						bindings[subject].entity_id;
					break;
				}
			if (phase2.player_key.value != 0U &&
				player_entity_id == 0U) {
				if (auto* diagnostic =
						begin_phase2_failure_diagnostic(
							NativePhase2FailureStage::
								PlayerBinding);
					diagnostic != nullptr) {
					diagnostic->ship_count =
						phase2.ships.size();
					diagnostic->player_key =
						phase2.player_key.value;
					diagnostic->player_entity_id =
						player_entity_id;
					diagnostic->global_manifest_id =
						m_phase2_manifest->manifest_id;
					diagnostic->required_manifest_id =
						slot.required_manifest_id;
					diagnostic->client_slot = index;
					diagnostic->required_manifest_applied =
						slot.required_manifest_applied;
				}
				fail_capture(
					NativePlayerCaptureStatus::
						CaptureInvariantFailure);
				return NativeSessionTickStatus::
					PermanentCaptureFailure;
			}
			Phase2CompleteDomainInput phase2_input;
			phase2_input.producer_id = m_producer_id;
			phase2_input.negotiated_capability_generation =
				1U;
			phase2_input.session_phase =
				slot.snapshot.progress() ==
					Phase1SnapshotProgress::Live
				? protocol::SessionPhase::Live
				: protocol::SessionPhase::Synchronizing;
			phase2_input.mission.producer_sample_time_us =
				m_tick_context.now_us;
			phase2_input.mission.mission_generation =
				mission_generation;
			phase2_input.mission.phase =
				protocol::MissionPhase::Active;
			phase2_input.mission.time_compression = 1.0F;
			phase2_input.observation = &phase2;
			phase2_input.retained_state =
				slot.snapshot.current_state().empty()
				? nullptr
				: &slot.snapshot.current_state();
			phase2_input.refresh_flight_controls =
				m_phase2_capture_plan.capture_flight_controls ||
				m_phase2_capture_plan.force_complete_keyframe;
			phase2_input.refresh_systems =
				m_phase2_capture_plan.capture_systems ||
				m_phase2_capture_plan.force_complete_keyframe;
			phase2_input.installed_manifest =
				installed_manifest;
			phase2_input.subjects = bindings.data();
			phase2_input.subject_count =
				phase2.ships.size();
			phase2_input.player_entity_id =
				player_entity_id;
			phase2_input.episode_latches =
				m_controller.phase2_support_latches(index);
			phase2_input.cleanup_batch =
				&slot.phase2_runtime.cleanup_batch();
			phase2_input.session_slot = index;
			if (!phase2.ships.empty()) {
				const auto& sampled_ship = phase2.ships.front();
				const std::array<std::uint64_t,
					Phase2RuntimeSlot::BlockCount> block_samples{
					sampled_ship.identity.sample_time_us,
					sampled_ship.flight.sample_time_us,
					phase2.player_controls.sample_time_us,
					sampled_ship.damage.sample_time_us,
					sampled_ship.energy.sample_time_us,
					sampled_ship.weapons.sample_time_us,
					sampled_ship.subsystems.count != 0U
						? sampled_ship.subsystems.values[0]
							  .sample_time_us
						: sampled_ship.identity.sample_time_us,
					sampled_ship.support.sample_time_us};
				if (!m_controller.set_phase2_block_samples(
						index, block_samples)) {
					if (auto* diagnostic =
							begin_phase2_failure_diagnostic(
								NativePhase2FailureStage::
									BlockSamples);
						diagnostic != nullptr) {
						diagnostic->ship_count =
							phase2.ships.size();
						diagnostic->player_key =
							phase2.player_key.value;
						diagnostic->player_entity_id =
							player_entity_id;
						for (std::size_t block = 0U;
							 block < block_samples.size();
							 ++block)
							if (block_samples[block] == 0U) {
								diagnostic->first_zero_block =
									block;
								break;
							}
					}
					fail_capture(
						NativePlayerCaptureStatus::
							CaptureInvariantFailure);
					return NativeSessionTickStatus::
						PermanentCaptureFailure;
				}
			}
			protocol::StateImage image;
			Phase2StateImageBuildDiagnostic image_diagnostic{};
			Phase2StateImageRebuildSet rebuilt_atoms;
			const auto incremental_tick =
				phase2_input.retained_state != nullptr &&
				!m_phase2_capture_plan.force_complete_keyframe;
			auto used_in_place_patch = false;
			// CockpitSensors currently projects its additional records into a
			// separate immutable image. Do not loan the controller's mutable
			// Phase 2 backing in that profile: replacing it before commit would
			// otherwise make the rollback/commit path operate on the wrong
			// storage.
			if (incremental_tick &&
				m_selected_phase2_profile !=
					Phase2Profile::CockpitSensors &&
				m_controller
						.take_current_state_for_incremental_patch(
							index, image) ==
					protocol::ProducerBaselineResult::Applied) {
				phase2_input.retained_state = &image;
				used_in_place_patch = true;
			}
			const auto image_started =
				std::chrono::steady_clock::now();
			auto image_status = used_in_place_patch
				? build_phase2_complete_domain_patch_preallocated(
					  phase2_input,
					  m_phase2_image_pools[index], image,
					  rebuilt_atoms, &image_diagnostic)
				: build_phase2_complete_domain_preallocated(
					  phase2_input,
					  m_phase2_image_pools[index], image,
					  &image_diagnostic, &rebuilt_atoms);
			if (used_in_place_patch &&
				(image_status ==
					 Phase2StateImageBuildStatus::
						 AllocationFailed ||
				 image_status ==
					 Phase2StateImageBuildStatus::
						 CapacityExceeded ||
				 image_status ==
					 Phase2StateImageBuildStatus::
						 SourceMappingMissing)) {
				(void)m_controller
					.restore_current_state_after_incremental_patch(
						index, std::move(image));
				used_in_place_patch = false;
				phase2_input.retained_state =
					&slot.snapshot.current_state();
				image_diagnostic = {};
				image_status =
					build_phase2_complete_domain_preallocated(
						phase2_input,
						m_phase2_image_pools[index], image,
						&image_diagnostic, &rebuilt_atoms);
			}
			if (m_performance_observation_active)
				m_last_performance_sample.state_image_fill_duration_ns +=
					elapsed_nanoseconds(
						image_started,
						std::chrono::steady_clock::now());
			if (image_status ==
				Phase2StateImageBuildStatus::
					AllocationFailed) {
				if (used_in_place_patch)
					(void)m_controller
						.restore_current_state_after_incremental_patch(
							index, std::move(image));
				continue;
			}
			if (image_status !=
				Phase2StateImageBuildStatus::Created) {
				if (used_in_place_patch)
					(void)m_controller
						.restore_current_state_after_incremental_patch(
							index, std::move(image));
				if (auto* diagnostic =
						begin_phase2_failure_diagnostic(
							NativePhase2FailureStage::
								StateImage);
					diagnostic != nullptr) {
					diagnostic->image_status = image_status;
					diagnostic->image_diagnostic =
						image_diagnostic;
					diagnostic->ship_count =
						phase2.ships.size();
					diagnostic->player_key =
						phase2.player_key.value;
					diagnostic->player_entity_id =
						player_entity_id;
				}
				fail_capture(
					NativePlayerCaptureStatus::
						CaptureInvariantFailure);
				return NativeSessionTickStatus::
					PermanentCaptureFailure;
			}
			if (m_selected_phase2_profile ==
				Phase2Profile::CockpitSensors) {
				auto* phase3_projection =
					m_phase3_projections[index].get();
				auto* phase3_projection_scratch =
					m_phase3_projection_scratch[index].get();
				auto* phase3_identity_registry =
					m_phase3_identity_registries[index].get();
				if (phase3_projection == nullptr ||
					phase3_projection_scratch == nullptr ||
					phase3_identity_registry == nullptr) {
					m_last_phase2_failure_diagnostic.phase3_diagnostic = {
						Phase3EngineCollectBlock::Precondition,
						Phase3EngineCollectStatus::InvalidSource};
					record_phase3_failure(m_metrics, m_log, index,
						TelemetryPhase3Block::Precondition,
						TelemetryPhase3CaptureFailure::InvalidSource);
					fail_capture(NativePlayerCaptureStatus::
						CaptureInvariantFailure);
					return NativeSessionTickStatus::
						PermanentCaptureFailure;
				}
				if (player_entity_id == 0U) {
					// A no-player image contains only the inherited global
					// singletons. Purging the retained sensor identities here
					// also guarantees that a later respawn receives a fresh
					// public identity scope.
					reset_phase3_projection(*phase3_projection);
					reset_phase3_projection(*phase3_projection_scratch);
					phase3_identity_registry->reset_session();
				} else {
					const Phase3EngineCollectInput phase3_input{
						m_tick_context.now_us,
						player_entity_id,
						&phase2,
						bindings.data(),
						phase2.ships.size(),
						installed_manifest,
						m_phase2_capture_plan.phase3_refresh_targeting &&
							m_phase2_capture_plan.capture_flight_controls,
						m_phase2_capture_plan.phase3_refresh_systems &&
							m_phase2_capture_plan.capture_systems,
						identity_signatures.data(),
						phase2.ships.size()};
					Phase3EngineCollectDiagnostic phase3_diagnostic;
					const auto phase3_status = collect_phase3_engine_projection(
							phase3_input,
							*phase3_identity_registry,
							*phase3_projection,
							*phase3_projection_scratch,
							&phase3_diagnostic);
					if (phase3_status !=
						Phase3EngineCollectStatus::Collected) {
						m_last_phase2_failure_diagnostic.phase3_diagnostic =
							phase3_diagnostic;
						record_phase3_failure(m_metrics, m_log, index,
							telemetry_phase3_block(phase3_diagnostic.block),
							telemetry_phase3_failure(phase3_status));
						fail_capture(NativePlayerCaptureStatus::
							CaptureInvariantFailure);
						return NativeSessionTickStatus::
							PermanentCaptureFailure;
					}
				}
				protocol::StateImage phase3_image;
				const auto phase3_image_status =
					build_phase3_cockpit_sensor_state_image(
						image, *phase3_projection,
						phase3_image);
				if (phase3_image_status !=
					Phase3StateImageBuildStatus::Created) {
					m_last_phase2_failure_diagnostic.phase3_diagnostic = {
						Phase3EngineCollectBlock::StateImage,
						Phase3EngineCollectStatus::InvalidSource};
					m_last_phase2_failure_diagnostic.phase3_image_status =
						phase3_image_status;
					record_phase3_failure(m_metrics, m_log, index,
						TelemetryPhase3Block::StateImage,
						telemetry_phase3_image_failure(
							phase3_image_status));
					fail_capture(NativePlayerCaptureStatus::
						CaptureInvariantFailure);
					return NativeSessionTickStatus::
						PermanentCaptureFailure;
				}
				image = std::move(phase3_image);
				// The Phase 3 projection replaces and appends records after the
				// Phase 2 builder has returned its rebuild set.  Include the final
				// canonical image in the incremental dirty set so systems ticks emit
				// RADAR_CONTACTS and the other cockpit sensor records immediately,
				// rather than waiting for the periodic keyframe.
				if (image.records().size() >
					rebuilt_atoms.canonical_indices.size()) {
					m_last_phase2_failure_diagnostic.phase3_diagnostic = {
						Phase3EngineCollectBlock::StateImage,
						Phase3EngineCollectStatus::SourceLimitExceeded};
					m_last_phase2_failure_diagnostic.phase3_image_status =
						Phase3StateImageBuildStatus::CapacityExceeded;
					record_phase3_failure(m_metrics, m_log, index,
						TelemetryPhase3Block::StateImage,
						TelemetryPhase3CaptureFailure::CapacityExceeded);
					fail_capture(NativePlayerCaptureStatus::
						CaptureInvariantFailure);
					return NativeSessionTickStatus::PermanentCaptureFailure;
				}
				rebuilt_atoms.count = image.records().size();
				for (std::size_t record_index = 0U;
					 record_index < rebuilt_atoms.count; ++record_index)
					rebuilt_atoms.canonical_indices[record_index] =
						static_cast<std::uint16_t>(record_index);
				rebuilt_atoms.exhaustive = true;
			}
			if (m_metrics != nullptr)
				m_metrics->observe_phase2_image(
					elapsed_nanoseconds(image_started,
						std::chrono::steady_clock::now()) /
					1000U);
			if (m_metrics != nullptr)
				m_metrics->set_phase2_session_state(index,
					image.records().size(),
					image.encoded_snapshot_records_size(),
					0U,
					(slot.phase2_runtime.manifest_state().active_id !=
							0U
						? 1U : 0U) +
						(slot.phase2_runtime.manifest_state().staged_id !=
								0U
							? 1U : 0U));
			if (!slot.snapshot.has_candidate() &&
				!slot.snapshot.has_active_baseline()) {
				(void)m_controller.begin_phase2_snapshot(
					index, image,
					Phase2RuntimeSnapshotCause::Initial,
					m_tick_context.now_us);
				continue;
			}
			const auto delta_started = m_performance_observation_active
				? std::chrono::steady_clock::now()
				: std::chrono::steady_clock::time_point{};
			auto baseline_result = used_in_place_patch
				? m_controller.commit_current_state_incremental_patch(
					  index, std::move(image),
					  rebuilt_atoms.canonical_indices.data(), rebuilt_atoms.count)
				: m_controller.replace_current_state_incremental(
					  index, image, rebuilt_atoms.canonical_indices.data(),
					  rebuilt_atoms.count);
			if (!used_in_place_patch &&
				rebuilt_atoms.exhaustive &&
				baseline_result ==
					protocol::ProducerBaselineResult::
						InvalidArgument)
				baseline_result =
					m_controller.replace_current_state(
						index, image);
			if (baseline_result !=
				protocol::ProducerBaselineResult::Applied) {
				if (used_in_place_patch && !image.empty()) {
					(void)rollback_phase2_complete_domain_patch_preallocated(
						m_phase2_image_pools[index],
						image, rebuilt_atoms);
					(void)m_controller
						.restore_current_state_after_incremental_patch(
							index, std::move(image));
				}
				if (auto* diagnostic =
						begin_phase2_failure_diagnostic(
							NativePhase2FailureStage::
								BaselineReplace);
					diagnostic != nullptr) {
					diagnostic->baseline_result =
						baseline_result;
					diagnostic->ship_count =
						phase2.ships.size();
					diagnostic->player_key =
						phase2.player_key.value;
					diagnostic->player_entity_id =
						player_entity_id;
				}
				fail_capture(
					NativePlayerCaptureStatus::
						CaptureInvariantFailure);
				return NativeSessionTickStatus::
					PermanentCaptureFailure;
			}
			if (slot.snapshot.has_active_baseline())
				(void)m_controller.queue_cumulative_delta(
					index, m_tick_context.now_us,
					m_phase2_capture_plan.force_complete_keyframe
						? m_phase2_capture_plan
							  .producer_sample_time_us
						: 0U);
			if (m_performance_observation_active)
				m_last_performance_sample.delta_build_duration_ns +=
					elapsed_nanoseconds(
						delta_started,
						std::chrono::steady_clock::now());
			continue;
		}
		if (m_selected_phase2_profile ==
			Phase2Profile::CoreGate) {
			const auto& phase2 =
				m_phase2_observation.observation();
			const auto core_gate_capture_valid =
				status == NativePlayerCaptureStatus::CapturedValid &&
				phase2.capture.status == Phase2CaptureStatus::Valid &&
				phase2.player_key.value != 0U &&
				phase2.ships.size() == 1U &&
				phase2.ships.front().capture_key.value ==
					phase2.player_key.value;
			const auto core_gate_no_player =
				status ==
					NativePlayerCaptureStatus::CapturedNoPlayer &&
				phase2.capture.status ==
					Phase2CaptureStatus::NoPlayer &&
				phase2.capture.reason == Phase2CaptureReason::None &&
				phase2.player_key.value == 0U &&
				phase2.ships.empty();
			if (m_phase2_manifest == nullptr ||
				(!core_gate_capture_valid &&
				 !core_gate_no_player)) {
				if (auto* diagnostic =
						begin_phase2_failure_diagnostic(
							NativePhase2FailureStage::
								Phase2Precondition);
					diagnostic != nullptr) {
					diagnostic->ship_count =
						phase2.ships.size();
					diagnostic->player_key =
						phase2.player_key.value;
				}
				fail_capture(
					NativePlayerCaptureStatus::
						CaptureInvariantFailure);
				return NativeSessionTickStatus::
					PermanentCaptureFailure;
			}
			Phase2CaptureLocalKey player_key{};
			if (core_gate_capture_valid)
				player_key = phase2.player_key;
			Phase2Wp05SubjectBinding binding{};
			const auto closure =
				m_controller.reconcile_phase2_closure(
					index,
					core_gate_capture_valid ? &player_key
						: nullptr,
					core_gate_capture_valid ? 1U : 0U,
					core_gate_capture_valid ? &binding
						: nullptr,
					core_gate_capture_valid ? 1U : 0U);
			if (closure == Phase2RuntimeResult::InvalidInput ||
				closure ==
					Phase2RuntimeResult::CapacityExceeded ||
				closure ==
					Phase2RuntimeResult::CounterExhausted) {
				if (auto* diagnostic =
						begin_phase2_failure_diagnostic(
							NativePhase2FailureStage::
								Closure);
					diagnostic != nullptr) {
					diagnostic->runtime_result = closure;
					diagnostic->ship_count =
						phase2.ships.size();
					diagnostic->player_key =
						phase2.player_key.value;
				}
				fail_capture(
					NativePlayerCaptureStatus::
						CaptureInvariantFailure);
				return NativeSessionTickStatus::
					PermanentCaptureFailure;
			}
			const auto player_entity_id =
				core_gate_capture_valid ? binding.entity_id : 0U;
			if (core_gate_capture_valid &&
				(binding.capture_key.value !=
					 phase2.player_key.value ||
				 player_entity_id == 0U)) {
				if (auto* diagnostic =
						begin_phase2_failure_diagnostic(
							NativePhase2FailureStage::
								PlayerBinding);
					diagnostic != nullptr) {
					diagnostic->ship_count =
						phase2.ships.size();
					diagnostic->player_key =
						phase2.player_key.value;
					diagnostic->player_entity_id =
						player_entity_id;
				}
				fail_capture(
					NativePlayerCaptureStatus::
						CaptureInvariantFailure);
				return NativeSessionTickStatus::
					PermanentCaptureFailure;
			}
			const auto manifest_state =
				m_controller.stage_phase2_manifest(index,
					*m_phase2_manifest,
					m_tick_context.now_us);
			if (manifest_state ==
					Phase2RuntimeResult::InvalidInput ||
				manifest_state ==
					Phase2RuntimeResult::Stale ||
				manifest_state ==
					Phase2RuntimeResult::CounterExhausted) {
				if (auto* diagnostic =
						begin_phase2_failure_diagnostic(
							NativePhase2FailureStage::
								Manifest);
					diagnostic != nullptr)
					diagnostic->runtime_result =
						manifest_state;
				fail_capture(
					NativePlayerCaptureStatus::
						CaptureInvariantFailure);
				return NativeSessionTickStatus::
					PermanentCaptureFailure;
			}
			// A staged manifest is deliberately not treated as installed.
			// The CoreGate snapshot references the FullRequired generation
			// and therefore waits for its reliable APPLIED transition.
			if (!slot.required_manifest_applied)
				continue;
			const auto* installed_manifest =
				m_phase2_manifest_slot->candidate_for_id(
					slot.required_manifest_id);
			if (installed_manifest == nullptr ||
				installed_manifest->manifest_id !=
					slot.required_manifest_id) {
				if (auto* diagnostic =
						begin_phase2_failure_diagnostic(
							NativePhase2FailureStage::
								Manifest);
					diagnostic != nullptr)
					diagnostic->runtime_result =
						Phase2RuntimeResult::InvalidInput;
				fail_capture(
					NativePlayerCaptureStatus::
						CaptureInvariantFailure);
				return NativeSessionTickStatus::
					PermanentCaptureFailure;
			}
			Phase2CoreGateStateImageInput phase2_input;
			phase2_input.producer_id = m_producer_id;
			phase2_input.negotiated_capability_generation = 1U;
			phase2_input.session_phase =
				slot.snapshot.progress() ==
					Phase1SnapshotProgress::Live
				? protocol::SessionPhase::Live
				: protocol::SessionPhase::Synchronizing;
			phase2_input.mission.producer_sample_time_us =
				m_tick_context.now_us;
			phase2_input.mission.mission_generation =
				mission_generation;
			phase2_input.mission.phase =
				protocol::MissionPhase::Active;
			phase2_input.mission.paused = false;
			phase2_input.mission.time_compression = 1.0F;
			phase2_input.player_entity_id = player_entity_id;
			phase2_input.observation = &phase2;
			phase2_input.installed_manifest = installed_manifest;
			phase2_input.required_manifest_id =
				slot.required_manifest_id;
			phase2_input.manifest_applied =
				slot.required_manifest_applied;
			protocol::StateImage image;
			const auto image_status =
				build_phase2_core_gate_state_image_preallocated(
					phase2_input,
					m_phase2_core_gate_image_pools[index],
					image);
			if (image_status ==
				Phase2StateImageBuildStatus::AllocationFailed)
				continue;
			if (image_status !=
				Phase2StateImageBuildStatus::Created) {
				if (auto* diagnostic =
						begin_phase2_failure_diagnostic(
							NativePhase2FailureStage::
								StateImage);
					diagnostic != nullptr) {
					diagnostic->image_status = image_status;
					diagnostic->ship_count =
						phase2.ships.size();
					diagnostic->player_key =
						phase2.player_key.value;
					diagnostic->player_entity_id =
						player_entity_id;
				}
				fail_capture(
					NativePlayerCaptureStatus::
						CaptureInvariantFailure);
				return NativeSessionTickStatus::
					PermanentCaptureFailure;
			}
			if (m_metrics != nullptr)
				m_metrics->set_phase2_session_state(index,
					image.records().size(),
					image.encoded_snapshot_records_size(),
					0U,
					(slot.phase2_runtime.manifest_state()
							 .active_id != 0U
						? 1U : 0U) +
						(slot.phase2_runtime.manifest_state()
								 .staged_id != 0U
							? 1U : 0U));
			if (!slot.snapshot.has_candidate() &&
				!slot.snapshot.has_active_baseline()) {
				(void)m_controller.begin_phase2_snapshot(
					index, image,
					Phase2RuntimeSnapshotCause::Initial,
					m_tick_context.now_us);
				continue;
			}
			const auto baseline_result =
				m_controller.replace_current_state(index, image);
			if (baseline_result !=
				protocol::ProducerBaselineResult::Applied) {
				if (auto* diagnostic =
						begin_phase2_failure_diagnostic(
							NativePhase2FailureStage::
								BaselineReplace);
					diagnostic != nullptr) {
					diagnostic->baseline_result =
						baseline_result;
					diagnostic->ship_count =
						phase2.ships.size();
					diagnostic->player_key =
						phase2.player_key.value;
					diagnostic->player_entity_id =
						player_entity_id;
				}
				fail_capture(
					NativePlayerCaptureStatus::
						CaptureInvariantFailure);
				return NativeSessionTickStatus::
					PermanentCaptureFailure;
			}
			if (slot.snapshot.has_active_baseline())
				(void)m_controller.queue_cumulative_delta(
					index, m_tick_context.now_us);
			continue;
		}
		Phase1StateImageInput input;
		input.producer_id = m_producer_id;
		input.negotiated_capability_generation = 1U;
		input.session_phase = slot.snapshot.progress() == Phase1SnapshotProgress::Live ? protocol::SessionPhase::Live
																													 : protocol::SessionPhase::Synchronizing;
		input.mission.producer_sample_time_us = m_tick_context.now_us;
		input.mission.mission_generation = mission_generation;
		input.mission.phase = m_tick_context.mission_active ? protocol::MissionPhase::Active : protocol::MissionPhase::None;
		input.mission.paused = false;
		input.mission.time_compression = 1.0F;
		input.player_capture = result;
		input.player = slot.latest_player_sample;
		protocol::StateImage image;
		Phase1StateImageBuildTiming image_timing{};
		const auto image_build_started = m_performance_observation_active ? std::chrono::steady_clock::now()
			: std::chrono::steady_clock::time_point{};
		const auto image_status =
			build_phase1_state_image_preallocated(input, m_state_image_pools[index], image,
				m_performance_observation_active ? &image_timing : nullptr);
		if (m_performance_observation_active) {
			m_last_performance_sample.state_image_build_duration_ns +=
				elapsed_nanoseconds(image_build_started, std::chrono::steady_clock::now());
			m_last_performance_sample.state_image_fill_duration_ns += image_timing.fill_records_ns;
			m_last_performance_sample.state_image_publish_validate_duration_ns += image_timing.publish_validate_ns;
			m_last_performance_sample.state_image_adopt_duration_ns += image_timing.adopt_preallocated_ns;
			m_last_performance_sample.state_image_semantic_validate_duration_ns += image_timing.semantic_validate_ns;
		}
		if (image_status == Phase1StateImageBuildStatus::AllocationFailed) {
			// All immutable backings are still retained by current/active/candidate
			// state. Preserve them and drop this replaceable capture; the next tick
			// retries once a baseline/candidate releases an owned backing.
			continue;
		}
		if (image_status != Phase1StateImageBuildStatus::Created) {
			fail_capture(NativePlayerCaptureStatus::CaptureInvariantFailure);
			return NativeSessionTickStatus::PermanentCaptureFailure;
		}
		if (!slot.snapshot.has_candidate() && !slot.snapshot.has_active_baseline()) {
			(void)m_controller.begin_initial_snapshot(
				index, image, m_tick_context.now_us);
			continue;
		}
		const auto delta_build_started = m_performance_observation_active ? std::chrono::steady_clock::now()
			: std::chrono::steady_clock::time_point{};
		if (m_controller.replace_current_state(index, image) != protocol::ProducerBaselineResult::Applied) {
			fail_capture(NativePlayerCaptureStatus::CaptureInvariantFailure);
			return NativeSessionTickStatus::PermanentCaptureFailure;
		}
		if (slot.snapshot.has_active_baseline()) {
			(void)m_controller.queue_cumulative_delta(
				index, m_tick_context.now_us);
		}
		if (m_performance_observation_active) {
			m_last_performance_sample.delta_build_duration_ns +=
				elapsed_nanoseconds(delta_build_started, std::chrono::steady_clock::now());
		}
	}
	return NativeSessionTickStatus::Complete;
}

void NativeSessionRuntime::clear_player_capture() noexcept
{
	m_current_player_capture = {};
	m_last_player_materialization = {};
	m_last_player_capture_status = NativePlayerCaptureStatus::Unavailable;
}

bool NativeSessionRuntime::provision_state_image_pools(std::size_t client_count) noexcept
{
	if (client_count == 0U || client_count > m_state_image_pools.size()) {
		return false;
	}
	std::size_t expected_backing_bytes = 0U;
	if (!checked_multiply_size(client_count, Phase1StateImagePool::BackingBytesPerClient, expected_backing_bytes) ||
		expected_backing_bytes > WP03ProvisionalKnownBudgetCapBytes) {
		return false;
	}
	release_state_image_pools();
	for (std::size_t index = 0U; index < client_count; ++index) {
		if (!m_state_image_pools[index].provision()) {
			release_state_image_pools();
			return false;
		}
	}
	std::size_t backing_bytes = 0U;
	for (std::size_t index = 0U; index < client_count; ++index) {
		const auto owned = m_state_image_pools[index].owned_backing_bytes();
		if (owned == 0U || !checked_add_size(backing_bytes, owned, backing_bytes)) {
			release_state_image_pools();
			return false;
		}
	}
	if (backing_bytes != expected_backing_bytes) {
		release_state_image_pools();
		return false;
	}
	m_state_image_pool_backing_bytes = backing_bytes;
	const auto budget = calculate_wp06_startup_budget(
		calculate_wp04_startup_budget(make_wp03_known_budget_request(client_count)), client_count);
	if (!wp06_budget_matches_state_image_pool(budget, client_count, m_state_image_pool_backing_bytes)) {
		release_state_image_pools();
		return false;
	}
	return true;
}

bool NativeSessionRuntime::provision_phase2_image_pools(
	std::size_t client_count) noexcept
{
	if (client_count == 0U ||
		client_count > m_phase2_image_pools.size())
		return false;
	for (std::size_t index = 0U; index < client_count; ++index)
		if (!m_phase2_image_pools[index].provision(
				MaximumPhase2ObservationShips,
				Phase2ManifestLimits::MaxAggregateSubsystems,
				MaximumPhase2DockRelationsPerShip,
				Phase2Wp07EpisodeLatches::Capacity)) {
			for (auto& pool : m_phase2_image_pools)
				pool.reset();
			return false;
		}
	std::size_t backing_bytes = 0U;
	for (std::size_t index = 0U; index < client_count; ++index) {
		const auto owned =
			m_phase2_image_pools[index].owned_backing_bytes();
		if (owned == 0U ||
			!checked_add_size(backing_bytes, owned,
				backing_bytes)) {
			for (auto& pool : m_phase2_image_pools)
				pool.reset();
			return false;
		}
	}
	m_phase2_image_pool_backing_bytes = backing_bytes;
	return true;
}

bool NativeSessionRuntime::provision_phase2_core_gate_image_pools(
	std::size_t client_count) noexcept
{
	if (client_count == 0U ||
		client_count > m_phase2_core_gate_image_pools.size())
		return false;
	for (std::size_t index = 0U; index < client_count; ++index)
		if (!m_phase2_core_gate_image_pools[index].provision(
				Phase2ManifestLimits::MaxSubsystemsPerShip)) {
			for (auto& pool : m_phase2_core_gate_image_pools)
				pool.reset();
			return false;
		}
	std::size_t backing_bytes = 0U;
	for (std::size_t index = 0U; index < client_count; ++index) {
		const auto owned =
			m_phase2_core_gate_image_pools[index]
				.owned_backing_bytes();
		if (owned == 0U ||
			!checked_add_size(backing_bytes, owned,
				backing_bytes)) {
			for (auto& pool :
				 m_phase2_core_gate_image_pools)
				pool.reset();
			return false;
		}
	}
	m_phase2_core_gate_image_pool_backing_bytes = backing_bytes;
	return true;
}

void NativeSessionRuntime::release_state_image_pools() noexcept
{
	for (auto& pool : m_state_image_pools) {
		pool.reset();
	}
	for (auto& pool : m_phase2_core_gate_image_pools)
		pool.reset();
	for (auto& pool : m_phase2_image_pools)
		pool.reset();
	m_state_image_pool_backing_bytes = 0U;
	m_phase2_core_gate_image_pool_backing_bytes = 0U;
	m_phase2_image_pool_backing_bytes = 0U;
	release_phase3_manifest_states();
	release_phase2_manifest_state();
}

std::uint64_t NativeSessionRuntime::state_image_pool_allocation_count() const noexcept
{
	std::uint64_t total = 0U;
	for (const auto& pool : m_state_image_pools) {
		const auto count = pool.successful_allocation_count();
		if (std::numeric_limits<std::uint64_t>::max() - total < count) {
			return std::numeric_limits<std::uint64_t>::max();
		}
		total += count;
	}
	return total;
}

std::size_t NativeSessionRuntime::state_image_pool_backing_bytes() const noexcept
{
	return m_state_image_pool_backing_bytes;
}

void NativeSessionRuntime::fail_transport() noexcept
{
	if (m_state != State::Started) {
		return;
	}
	purge_all(SessionCloseReason::TransportError);
	m_transport.close();
	m_scheduler.reset();
	m_capture_cadence.stop();
	m_phase2_capture_plan = {};
	reset_phase2_observation_buffer_in_place(m_phase2_observation);
	m_selected_phase2_profile = Phase2Profile::None;
	m_phase2_enabled = false;
	clear_player_capture();
	release_state_image_pools();
	m_producer_id = 0U;
	m_fault_status = NativeSessionTickStatus::PermanentTransportFailure;
	m_state = State::Faulted;
}

NativePhase2FailureDiagnostic*
NativeSessionRuntime::begin_phase2_failure_diagnostic(
	NativePhase2FailureStage stage) noexcept
{
	if (stage == NativePhase2FailureStage::None ||
		stage == NativePhase2FailureStage::Count ||
		m_last_phase2_failure_diagnostic.stage !=
			NativePhase2FailureStage::None)
		return nullptr;
	m_last_phase2_failure_diagnostic.stage = stage;
	return &m_last_phase2_failure_diagnostic;
}

void NativeSessionRuntime::fail_capture(NativePlayerCaptureStatus status) noexcept
{
	if (m_state != State::Started) {
		return;
	}
	purge_all(SessionCloseReason::ProtocolError);
	m_transport.close();
	m_scheduler.reset();
	m_capture_cadence.stop();
	m_phase2_capture_plan = {};
	reset_phase2_observation_buffer_in_place(m_phase2_observation);
	m_selected_phase2_profile = Phase2Profile::None;
	m_phase2_enabled = false;
	clear_player_capture();
	release_state_image_pools();
	m_last_player_capture_status = status;
	m_fault_status = NativeSessionTickStatus::PermanentCaptureFailure;
	m_state = State::Faulted;
}

const SessionControllerSlot* NativeSessionRuntimeTestAccess::slot(const NativeSessionRuntime& runtime,
	std::size_t index) noexcept
{
	return runtime.m_controller_ready && index < runtime.m_controller.owned_capacity().client_slots
			? &runtime.m_controller.slot(index)
			: nullptr;
}

SessionController* NativeSessionRuntimeTestAccess::controller(NativeSessionRuntime& runtime) noexcept
{
	return runtime.m_controller_ready ? &runtime.m_controller : nullptr;
}

IoStatus NativeSessionRuntimeTestAccess::try_receive(
	NativeSessionRuntime& runtime) noexcept
{
	return runtime.try_receive();
}

void NativeSessionRuntimeTestAccess::set_session_controller_provision_failure(NativeSessionRuntime& runtime,
	bool fail) noexcept
{
	runtime.m_fail_session_controller_provision = fail;
}

void NativeSessionRuntimeTestAccess::set_startup_owned_budget_adjustment(
	NativeSessionRuntime& runtime, std::size_t additional_bytes) noexcept
{
	runtime.m_startup_owned_budget_test_adjustment = additional_bytes;
}

std::size_t NativeSessionRuntimeTestAccess::startup_owned_bytes(
	const NativeSessionRuntime& runtime) noexcept
{
	return runtime.m_startup_owned_bytes;
}

std::uint64_t NativeSessionRuntimeTestAccess::startup_allocation_count(
	const NativeSessionRuntime& runtime) noexcept
{
	return runtime.m_startup_allocation_count;
}

Phase2CapturePlan NativeSessionRuntimeTestAccess::prepare_phase2_keyframe_plan(
	NativeSessionRuntime& runtime) noexcept
{
	Phase2CapturePlan plan;
	const auto was_enabled = runtime.m_phase2_enabled;
	runtime.m_phase2_enabled = true;
	runtime.prepare_phase2_keyframe(plan);
	runtime.m_phase2_enabled = was_enabled;
	return plan;
}

Phase2CapturePlan NativeSessionRuntimeTestAccess::phase2_capture_plan(
	const NativeSessionRuntime& runtime) noexcept
{
	return runtime.m_phase2_capture_plan;
}

Phase2CaptureResult
NativeSessionRuntimeTestAccess::last_phase2_capture_result(
	const NativeSessionRuntime& runtime) noexcept
{
	return runtime.m_last_phase2_capture_result;
}

NativePhase2FailureDiagnostic
NativeSessionRuntimeTestAccess::last_phase2_failure_diagnostic(
	const NativeSessionRuntime& runtime) noexcept
{
	return runtime.m_last_phase2_failure_diagnostic;
}

Phase2OwnedBudget NativeSessionRuntimeTestAccess::phase2_owned_budget(
	const NativeSessionRuntime& runtime) noexcept
{
	return runtime.m_phase2_owned_budget;
}

bool NativeSessionRuntimeTestAccess::phase2_owned_scope_within_cap(
	TelemetryPhase2MemoryScope scope,
	std::size_t owned_bytes) noexcept
{
	return telemetry::detail::phase2_owned_scope_within_cap(
		scope, owned_bytes);
}

void NativeSessionRuntimeTestAccess::begin_steady_state_allocation_tracking(NativeSessionRuntime& runtime) noexcept
{
	if (runtime.m_controller_ready) {
		runtime.m_controller.begin_phase1_allocation_observation();
	}
}

std::uint64_t NativeSessionRuntimeTestAccess::steady_state_allocation_count(
	const NativeSessionRuntime& runtime) noexcept
{
	return runtime.m_controller_ready ? runtime.m_controller.phase1_observed_allocation_count() : 0U;
}

std::uint64_t NativeSessionRuntimeTestAccess::steady_state_allocation_count(
	const NativeSessionRuntime& runtime,
	Phase1AllocationGrowthSource source) noexcept
{
	return runtime.m_controller_ready
		? runtime.m_controller.phase1_observed_allocation_count(source)
		: 0U;
}

void NativeSessionRuntimeTestAccess::force_steady_state_allocation_for_tests(NativeSessionRuntime& runtime) noexcept
{
	if (!runtime.m_controller_ready) {
		return;
	}
	auto allocation = std::unique_ptr<std::uint8_t[]>(new (std::nothrow) std::uint8_t[1U]);
	if (allocation == nullptr) {
		return;
	}
	runtime.m_test_allocation_probe = std::move(allocation);
	runtime.m_controller.note_phase1_runtime_allocation_for_test();
}

void NativeSessionRuntimeTestAccess::begin_performance_observation(NativeSessionRuntime& runtime) noexcept
{
	runtime.m_performance_observation_active = runtime.m_controller_ready;
	runtime.m_last_performance_sample = {};
	if (runtime.m_performance_observation_active) runtime.m_controller.begin_phase1_allocation_observation();
}

NativeRuntimePerformanceSample NativeSessionRuntimeTestAccess::last_performance_sample(
	const NativeSessionRuntime& runtime) noexcept
{
	return runtime.m_last_performance_sample;
}

bool NativeSessionRuntimeTestAccess::seed_last_allocated_entity_id(NativeSessionRuntime& runtime,
	std::size_t index,
	std::uint64_t last_id) noexcept
{
	return runtime.m_controller_ready &&
		SessionControllerTestAccess::seed_last_allocated_entity_id(runtime.m_controller, index, last_id);
}

NativeSessionTickStatus NativeSessionRuntimeTestAccess::inject_collected_player_capture(NativeSessionRuntime& runtime,
	const CaptureResult& result,
	const PlayerObservationDto& observation) noexcept
{
	if (runtime.m_state != NativeSessionRuntime::State::Started || !runtime.m_controller_ready) {
		return NativeSessionTickStatus::Unavailable;
	}
	return runtime.apply_collected_player_capture(result, observation);
}

NativeSessionTickStatus NativeSessionRuntimeTestAccess::service_r2_tick(NativeSessionRuntime& runtime,
	const NativeSessionTickContext& context) noexcept
{
	return runtime.service_r2_tick(context);
}

TelemetryLogSnapshot NativeSessionRuntimeTestAccess::log_snapshot(const NativeSessionRuntime& runtime) noexcept
{
	return runtime.m_log != nullptr ? runtime.m_log->snapshot() : TelemetryLogSnapshot{};
}

} // namespace telemetry::detail
