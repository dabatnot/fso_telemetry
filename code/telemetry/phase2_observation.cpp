#include "telemetry/phase2_observation.h"

#include "telemetry/protocol/telemetry_protocol_constants.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <thread>
#include <tuple>
#include <utility>

namespace telemetry::detail {
namespace {

std::atomic<Phase2SeamTestDouble*> Phase2TestDouble{nullptr};
Phase2SeamHandoffSnapshot Phase2Handoff;
std::thread::id Phase2MainThread;
bool Phase2MainThreadCaptured = false;

void reset_observation(Phase2ObservationDto& output) noexcept
{
	for (std::size_t index = 0U; index < output.discovery_count; ++index) {
		output.discovery_nodes[index] = {};
	}
	output.capture = {};
	output.discovery_capture = {};
	output.discovery_status = Phase2SourceReadStatus::Valid;
	output.discovery_count = 0U;
	output.player_key = {};
	output.producer_sample_time_us = 0U;
	output.ships.clear();
	output.raw_static_catalog.clear();
	output.raw_static_diagnostic = {};
	output.player_controls = {};
	output.player_cargo_scan = {};
}

bool validate_raw_static_catalog_bounds(
	const Phase2RawStaticCatalog& catalog) noexcept
{
	if(catalog.class_count>MaximumPhase2StaticClasses||
		catalog.weapon_count>MaximumPhase2StaticWeapons||
		catalog.auxiliary_count>MaximumPhase2StaticAuxiliaryEntries||
		catalog.aggregate_subsystem_count>MaximumPhase2StaticSubsystems)return false;
	std::array<bool, MaximumPhase2StaticSubsystems> subsystem_used{};
	const auto finite_vec = [](const Phase2RawVec3& value) noexcept {
		return std::isfinite(value.x) && std::isfinite(value.y) &&
			std::isfinite(value.z);
	};
	for(std::uint32_t index=0;index<catalog.class_count;++index) {
		const auto& value=catalog.class_definitions[index];
		if(value.subsystem_count>MaximumPhase2SubsystemsPerShip||
			value.bank_count>MaximumPhase2StaticBanksPerClass||
			value.primary_bank_count>MaximumPhase2WeaponBanksPerFamily||
			value.secondary_bank_count>MaximumPhase2WeaponBanksPerFamily||
			value.tertiary_bank_count>MaximumPhase2WeaponBanksPerFamily||
			value.turret_bank_count>MaximumPhase2WeaponBanksPerFamily||
			value.primary_bank_count+value.secondary_bank_count+
				value.tertiary_bank_count+value.turret_bank_count!=value.bank_count)
			return false;
		const std::array<float, 26U> class_scalars{{
			value.model_mass, value.density, value.effective_mass,
			value.max_rear_velocity, value.forward_accel_time,
			value.afterburner_forward_accel_time,
			value.booster_forward_accel_time, value.forward_decel_time,
			value.slide_accel_time, value.slide_decel_time,
			value.max_hull_strength, value.max_shield_strength,
			value.afterburner_fuel_capacity, value.afterburner_burn_rate,
			value.afterburner_recover_rate,
			value.afterburner_min_start_fuel,
			value.afterburner_cooldown_seconds, value.scan_range_normal,
			value.scan_range_capital, value.scanning_range_multiplier,
			value.glide_cap, value.autoaim_fov_rad,
			value.countermeasure_capacity,
			value.countermeasure_cargo_size, 0.0F, 0.0F}};
		for(const auto scalar:class_scalars)
			if(!std::isfinite(scalar))return false;
		for(const auto scalar:value.model_inertia)
			if(!std::isfinite(scalar))return false;
		for(const auto scalar:value.effective_inertia)
			if(!std::isfinite(scalar))return false;
		if(!finite_vec(value.center_of_mass)||
			!finite_vec(value.max_velocity)||
			!finite_vec(value.afterburner_max_velocity)||
			!finite_vec(value.booster_max_velocity)||
			!finite_vec(value.max_rotational_velocity)||
			value.max_hull_strength<0.0F||
			value.max_shield_strength<0.0F||
			value.forward_accel_time<0.0F||
			value.afterburner_forward_accel_time<0.0F||
			value.booster_forward_accel_time<0.0F||
			value.forward_decel_time<0.0F||
			value.slide_accel_time<0.0F||
			value.slide_decel_time<0.0F||
			value.afterburner_cooldown_seconds<0.0F||
			value.scan_time_ms<0||
			value.countermeasure_capacity<0.0F||
			value.countermeasure_cargo_size<0.0F||
			(value.countermeasure_uses_capacity&&
			 value.countermeasure_capacity<=0.0F))
			return false;
		if(value.subsystem_offset>MaximumPhase2StaticSubsystems||
			value.subsystem_count>
				MaximumPhase2StaticSubsystems-value.subsystem_offset||
			value.bank_offset>catalog.bank_storage.size()||
			value.bank_count>catalog.bank_storage.size()-value.bank_offset)
			return false;
		for(std::uint32_t subsystem=0;subsystem<value.subsystem_count;++subsystem) {
			const auto flat=value.subsystem_offset+subsystem;
			if(subsystem_used[flat])return false;
			subsystem_used[flat]=true;
			const auto& definition=catalog.subsystem_storage[flat];
			if(!finite_vec(definition.local_position)||
				!std::isfinite(definition.radius)||
				!std::isfinite(definition.max_hits)||
				definition.radius<0.0F||
				definition.max_hits<0.0F)
				return false;
		}
		for(std::uint32_t bank=0;bank<value.bank_count;++bank) {
			const auto& definition=
				catalog.bank_storage[value.bank_offset+bank];
			if(definition.fire_point_count>
					MaximumPhase2StaticFirePoints||
				!std::isfinite(definition.capacity)||
				definition.capacity<0.0F)return false;
			for(std::uint32_t point=0;point<definition.fire_point_count;++point)
				if(!finite_vec(definition.fire_points[point]))return false;
		}
	}
	for(std::uint32_t index=0U;index<catalog.weapon_count;++index) {
		const auto& value=catalog.weapon_definitions[index];
		const std::array<float, 19U> scalars{{
			value.max_speed,value.mass,value.gravity_constant,
			value.velocity_inherit_amount,value.lifetime_seconds,
			value.acceleration_time_seconds,value.minimum_range,
			value.optimal_range,value.maximum_range,value.fire_wait_seconds,
			value.energy_consumed,value.damage,value.shockwave_outer_radius,
			value.guidance_fov_source_cosine,value.lock_time_seconds,
			value.lock_fov_source_cosine,value.cargo_size,
			value.rearm_rate_seconds,value.burst_delay_seconds}};
		for(const auto scalar:scalars)if(!std::isfinite(scalar))return false;
		if(value.weapon_subtype_source>2U||
			value.guidance_type_source>3U||
			value.max_speed<0.0F||
			value.mass<0.0F||
			value.lifetime_seconds<0.0F||
			value.acceleration_time_seconds<0.0F||
			value.minimum_range<0.0F||
			value.optimal_range<value.minimum_range||
			value.maximum_range<value.optimal_range||
			value.fire_wait_seconds<0.0F||
			value.energy_consumed<0.0F||
			value.damage<0.0F||
			value.shockwave_outer_radius<0.0F||
			value.lock_time_seconds<0.0F||
			value.cargo_size<0.0F||
			value.rearm_rate_seconds<0.0F||
			value.reloaded_per_batch<1U||
			value.reloaded_per_batch>1'000'000U||
			value.burst_shots<0||
			value.burst_delay_seconds<0.0F||
			value.swarm_count_source<0||
			value.shots_source<0)return false;
		if(value.guidance_type_source!=0U&&
			(value.guidance_fov_source_cosine < -1.0F ||
			 value.guidance_fov_source_cosine > 1.0F))return false;
	}
	std::uint32_t aggregate=0U;
	for(const auto used:subsystem_used)if(used)++aggregate;
	return aggregate==catalog.aggregate_subsystem_count;
}

bool semantic_equal(const Phase2RawVec3& left,
	const Phase2RawVec3& right) noexcept
{
	return left.x == right.x && left.y == right.y && left.z == right.z;
}

void normalize_zero(float& value) noexcept
{
	if (value == 0.0F) {
		value = 0.0F;
	}
}

void normalize_zero(Phase2RawVec3& value) noexcept
{
	normalize_zero(value.x);
	normalize_zero(value.y);
	normalize_zero(value.z);
}

void normalize_semantic_floats(Phase2RawSubsystemDefinition& value) noexcept
{
	normalize_zero(value.local_position);
	normalize_zero(value.radius);
	normalize_zero(value.max_hits);
}

void normalize_semantic_floats(Phase2RawBankDefinition& value) noexcept
{
	normalize_zero(value.capacity);
	for (std::uint32_t index = 0U; index < value.fire_point_count; ++index) {
		normalize_zero(value.fire_points[index]);
	}
}

void normalize_semantic_floats(Phase2RawWeaponDefinition& value) noexcept
{
	for (auto* scalar : {&value.max_speed, &value.mass,
			 &value.gravity_constant, &value.velocity_inherit_amount,
			 &value.lifetime_seconds, &value.acceleration_time_seconds,
			 &value.minimum_range, &value.optimal_range,
			 &value.maximum_range, &value.fire_wait_seconds,
			 &value.energy_consumed, &value.damage,
			 &value.shockwave_outer_radius,
			 &value.guidance_fov_source_cosine, &value.lock_time_seconds,
			 &value.lock_fov_source_cosine, &value.cargo_size,
			 &value.rearm_rate_seconds, &value.burst_delay_seconds}) {
		normalize_zero(*scalar);
	}
}

void normalize_semantic_floats(Phase2RawClassDefinition& value) noexcept
{
	for (auto* scalar : {&value.model_mass, &value.density,
			 &value.effective_mass, &value.max_rear_velocity,
			 &value.forward_accel_time,
			 &value.afterburner_forward_accel_time,
			 &value.booster_forward_accel_time,
			 &value.forward_decel_time, &value.slide_accel_time,
			 &value.slide_decel_time, &value.max_hull_strength,
			 &value.max_shield_strength,
			 &value.afterburner_fuel_capacity,
			 &value.afterburner_burn_rate,
			 &value.afterburner_recover_rate,
			 &value.afterburner_min_start_fuel,
			 &value.afterburner_cooldown_seconds,
			 &value.scan_range_normal, &value.scan_range_capital,
			 &value.scanning_range_multiplier, &value.glide_cap,
			 &value.autoaim_fov_rad, &value.countermeasure_capacity,
			 &value.countermeasure_cargo_size}) {
		normalize_zero(*scalar);
	}
	for (auto& scalar : value.model_inertia) normalize_zero(scalar);
	for (auto& scalar : value.effective_inertia) normalize_zero(scalar);
	normalize_zero(value.center_of_mass);
	normalize_zero(value.max_velocity);
	normalize_zero(value.afterburner_max_velocity);
	normalize_zero(value.booster_max_velocity);
	normalize_zero(value.max_rotational_velocity);
}

bool semantic_equal(const Phase2RawSubsystemDefinition& left,
	const Phase2RawSubsystemDefinition& right) noexcept
{
	return left.internal_name.view() == right.internal_name.view() &&
		left.alt_name.view() == right.alt_name.view() &&
		left.hud_name.view() == right.hud_name.view() &&
		semantic_equal(left.local_position, right.local_position) &&
		std::tie(left.subsystem_capture_key, left.radius, left.max_hits,
			left.subsystem_type_source, left.raw_static_flags,
			left.armor_capture_key) ==
		std::tie(right.subsystem_capture_key, right.radius, right.max_hits,
			right.subsystem_type_source, right.raw_static_flags,
			right.armor_capture_key);
}

bool semantic_equal(const Phase2RawBankDefinition& left,
	const Phase2RawBankDefinition& right) noexcept
{
	if (std::tie(left.bank_capture_key,
			left.owner_subsystem_capture_key, left.family_source,
			left.source_family, left.bank_index, left.weapon_capture_key,
			left.consumes_ammunition, left.has_capacity, left.capacity,
			left.firing_pattern_source_code, left.fire_point_count) !=
		std::tie(right.bank_capture_key,
			right.owner_subsystem_capture_key, right.family_source,
			right.source_family, right.bank_index, right.weapon_capture_key,
			right.consumes_ammunition, right.has_capacity, right.capacity,
			right.firing_pattern_source_code, right.fire_point_count)) {
		return false;
	}
	for (std::uint32_t index = 0U; index < left.fire_point_count; ++index) {
		if (!semantic_equal(left.fire_points[index], right.fire_points[index])) {
			return false;
		}
	}
	return true;
}

bool semantic_equal(const Phase2RawWeaponDefinition& left,
	const Phase2RawWeaponDefinition& right) noexcept
{
	return left.internal_name.view() == right.internal_name.view() &&
		left.title.view() == right.title.view() &&
		std::tie(left.weapon_capture_key, left.weapon_subtype_source,
			left.raw_class_flags, left.max_speed, left.mass,
			left.gravity_constant, left.velocity_inherit_amount,
			left.lifetime_seconds, left.acceleration_time_seconds,
			left.minimum_range, left.optimal_range, left.maximum_range,
			left.fire_wait_seconds, left.energy_consumed, left.damage,
			left.damage_type_capture_key, left.shockwave_outer_radius,
			left.raw_effect_flags, left.guidance_type_source,
			left.guidance_fov_source_cosine, left.lock_time_seconds,
			left.lock_fov_source_cosine, left.cargo_size,
			left.rearm_rate_seconds, left.reloaded_per_batch,
			left.burst_shots, left.burst_delay_seconds, left.swarm_count_source,
			left.shots_source) ==
		std::tie(right.weapon_capture_key, right.weapon_subtype_source,
			right.raw_class_flags, right.max_speed, right.mass,
			right.gravity_constant, right.velocity_inherit_amount,
			right.lifetime_seconds, right.acceleration_time_seconds,
			right.minimum_range, right.optimal_range, right.maximum_range,
			right.fire_wait_seconds, right.energy_consumed, right.damage,
			right.damage_type_capture_key, right.shockwave_outer_radius,
			right.raw_effect_flags, right.guidance_type_source,
			right.guidance_fov_source_cosine, right.lock_time_seconds,
			right.lock_fov_source_cosine, right.cargo_size,
			right.rearm_rate_seconds, right.reloaded_per_batch,
			right.burst_shots, right.burst_delay_seconds,
			right.swarm_count_source, right.shots_source);
}

bool semantic_equal(const Phase2RawClassDefinition& left,
	const Phase2RawClassDefinition& right) noexcept
{
	return left.internal_name.view() == right.internal_name.view() &&
		semantic_equal(left.center_of_mass, right.center_of_mass) &&
		semantic_equal(left.max_velocity, right.max_velocity) &&
		semantic_equal(left.afterburner_max_velocity,
			right.afterburner_max_velocity) &&
		semantic_equal(left.booster_max_velocity,
			right.booster_max_velocity) &&
		semantic_equal(left.max_rotational_velocity,
			right.max_rotational_velocity) &&
		std::tie(left.class_capture_key, left.model_mass, left.density,
			left.model_inertia, left.effective_mass, left.effective_inertia,
			left.max_rear_velocity, left.forward_accel_time,
			left.afterburner_forward_accel_time,
			left.booster_forward_accel_time, left.forward_decel_time,
			left.slide_accel_time, left.slide_decel_time,
			left.max_hull_strength, left.max_shield_strength,
			left.has_afterburner, left.afterburner_fuel_capacity,
			left.afterburner_burn_rate, left.afterburner_recover_rate,
			left.afterburner_min_start_fuel,
			left.afterburner_cooldown_seconds, left.has_scan,
			left.scan_time_ms, left.scan_range_normal, left.scan_range_capital,
			left.scanning_range_multiplier, left.has_glide, left.glide_cap,
			left.has_autoaim, left.autoaim_fov_rad, left.species_capture_key,
			left.ship_type_capture_key, left.iff_capture_key,
			left.wing_capture_key, left.armor_capture_key,
			left.damage_type_capture_key, left.countermeasure_capacity,
			left.countermeasure_cargo_size,
			left.countermeasure_uses_capacity,
			left.countermeasure_weapon_capture_key,
			left.countermeasure_firewait_ms, left.subsystem_count,
			left.subsystem_offset, left.bank_count, left.bank_offset,
			left.primary_bank_count, left.secondary_bank_count,
			left.tertiary_bank_count, left.turret_bank_count) ==
		std::tie(right.class_capture_key, right.model_mass, right.density,
			right.model_inertia, right.effective_mass, right.effective_inertia,
			right.max_rear_velocity, right.forward_accel_time,
			right.afterburner_forward_accel_time,
			right.booster_forward_accel_time, right.forward_decel_time,
			right.slide_accel_time, right.slide_decel_time,
			right.max_hull_strength, right.max_shield_strength,
			right.has_afterburner, right.afterburner_fuel_capacity,
			right.afterburner_burn_rate, right.afterburner_recover_rate,
			right.afterburner_min_start_fuel,
			right.afterburner_cooldown_seconds, right.has_scan,
			right.scan_time_ms, right.scan_range_normal,
			right.scan_range_capital, right.scanning_range_multiplier,
			right.has_glide, right.glide_cap, right.has_autoaim,
			right.autoaim_fov_rad, right.species_capture_key,
			right.ship_type_capture_key, right.iff_capture_key,
			right.wing_capture_key, right.armor_capture_key,
			right.damage_type_capture_key, right.countermeasure_capacity,
			right.countermeasure_cargo_size,
			right.countermeasure_uses_capacity,
			right.countermeasure_weapon_capture_key,
			right.countermeasure_firewait_ms, right.subsystem_count,
			right.subsystem_offset, right.bank_count, right.bank_offset,
			right.primary_bank_count, right.secondary_bank_count,
			right.tertiary_bank_count, right.turret_bank_count);
}

bool merge_raw_static_catalog(Phase2RawStaticCatalog& destination,
	const Phase2RawStaticCatalog& source,
	Phase2RawStaticReferences& references,
	Phase2ObservationDto::RawStaticDiagnostic& diagnostic) noexcept
{
	if (source.class_count == 0U && source.weapon_count == 0U &&
		source.auxiliary_count == 0U &&
		references.class_capture_key == 0U &&
		references.weapon_count == 0U &&
		references.auxiliary_count == 0U) {
		return true;
	}
	const auto has_auxiliary_key = [&](std::uint32_t key) noexcept {
		if (key == 0U) return true;
		for (std::uint32_t index = 0U; index < source.auxiliary_count; ++index)
			if (source.auxiliary_entries[index].capture_key == key) return true;
		return false;
	};
	const auto has_weapon_key = [&](std::uint32_t key) noexcept {
		if (key == 0U) return true;
		for (std::uint32_t index = 0U; index < source.weapon_count; ++index)
			if (source.weapon_definitions[index].weapon_capture_key == key)
				return true;
		return false;
	};
	for (std::uint32_t left = 0U; left < source.auxiliary_count; ++left) {
		const auto& entry = source.auxiliary_entries[left];
		if (entry.capture_key == 0U ||
			(entry.registry != Phase2RawAuxiliaryRegistry::Pattern &&
			 entry.name.empty())) {
			diagnostic.reason =
				Phase2ObservationDto::RawStaticDiagnostic::Reason::
					MissingReferenceRemap;
			return false;
		}
	}
	for (std::uint32_t index = 0U; index < source.weapon_count; ++index) {
		const auto& weapon = source.weapon_definitions[index];
		if (weapon.weapon_capture_key == 0U ||
			!has_auxiliary_key(weapon.damage_type_capture_key)) {
			diagnostic.reason =
				Phase2ObservationDto::RawStaticDiagnostic::Reason::
					MissingReferenceRemap;
			return false;
		}
	}
	for (std::uint32_t class_index = 0U;
		 class_index < source.class_count;
		 ++class_index) {
		const auto& ship_class = source.class_definitions[class_index];
		if (ship_class.class_capture_key == 0U ||
			!has_auxiliary_key(ship_class.species_capture_key) ||
			!has_auxiliary_key(ship_class.ship_type_capture_key) ||
			!has_auxiliary_key(ship_class.iff_capture_key) ||
			!has_auxiliary_key(ship_class.wing_capture_key) ||
			!has_auxiliary_key(ship_class.armor_capture_key) ||
			!has_auxiliary_key(ship_class.damage_type_capture_key) ||
			!has_weapon_key(
				ship_class.countermeasure_weapon_capture_key)) {
			diagnostic.reason =
				Phase2ObservationDto::RawStaticDiagnostic::Reason::
					MissingReferenceRemap;
			return false;
		}
		for (std::uint32_t subsystem = 0U;
			 subsystem < ship_class.subsystem_count;
			 ++subsystem) {
			const auto& value = source.subsystem_storage[
				ship_class.subsystem_offset + subsystem];
			if (value.subsystem_capture_key == 0U ||
				!has_auxiliary_key(value.armor_capture_key)) {
				diagnostic.reason =
					Phase2ObservationDto::RawStaticDiagnostic::Reason::
						MissingReferenceRemap;
				return false;
			}
		}
		for (std::uint32_t bank = 0U; bank < ship_class.bank_count; ++bank) {
			const auto& value =
				source.bank_storage[ship_class.bank_offset + bank];
			const bool tertiary = value.family_source == 3U;
			const bool turret = value.family_source == 4U;
			if (value.bank_capture_key == 0U ||
				(!tertiary &&
				 (!has_weapon_key(value.weapon_capture_key) ||
				  value.weapon_capture_key == 0U ||
				  value.fire_point_count == 0U)) ||
				(tertiary &&
				 (value.weapon_capture_key != 0U ||
				  value.fire_point_count != 0U))) {
				diagnostic.reason =
					Phase2ObservationDto::RawStaticDiagnostic::Reason::
						MissingReferenceRemap;
				return false;
			}
			if (turret) {
				bool owner_found = false;
				for (std::uint32_t subsystem = 0U;
					 subsystem < ship_class.subsystem_count;
					 ++subsystem)
					if (source.subsystem_storage[
							ship_class.subsystem_offset + subsystem]
							.subsystem_capture_key ==
						value.owner_subsystem_capture_key)
						owner_found = true;
				if (!owner_found) {
					diagnostic.reason =
						Phase2ObservationDto::RawStaticDiagnostic::Reason::
							MissingSubsystemOwner;
					return false;
				}
			} else if (value.owner_subsystem_capture_key != 0U) {
				diagnostic.reason =
					Phase2ObservationDto::RawStaticDiagnostic::Reason::
						MissingSubsystemOwner;
				return false;
			}
		}
	}
	bool class_reference_found = false;
	for (std::uint32_t index = 0U; index < source.class_count; ++index)
		if (source.class_definitions[index].class_capture_key ==
			references.class_capture_key)
			class_reference_found = true;
	if (!class_reference_found) {
		diagnostic.reason =
			Phase2ObservationDto::RawStaticDiagnostic::Reason::
				MissingReferenceRemap;
		return false;
	}
	for (std::uint32_t index = 0U; index < references.weapon_count; ++index)
		if (!has_weapon_key(references.weapon_capture_keys[index]) ||
			references.weapon_capture_keys[index] == 0U) {
			diagnostic.reason =
				Phase2ObservationDto::RawStaticDiagnostic::Reason::
					MissingReferenceRemap;
			return false;
		}
	for (std::uint32_t index = 0U; index < references.auxiliary_count; ++index)
		if (!has_auxiliary_key(references.auxiliary_capture_keys[index]) ||
			references.auxiliary_capture_keys[index] == 0U) {
			diagnostic.reason =
				Phase2ObservationDto::RawStaticDiagnostic::Reason::
					MissingReferenceRemap;
			return false;
		}
	std::array<std::uint32_t, MaximumPhase2StaticAuxiliaryEntries> aux_map{};
	for (std::uint32_t source_index = 0U;
		 source_index < source.auxiliary_count;
		 ++source_index) {
		const auto& source_entry = source.auxiliary_entries[source_index];
		std::uint32_t target_key = 0U;
		for (std::uint32_t target_index = 0U;
			 target_index < destination.auxiliary_count;
			 ++target_index) {
			const auto& target = destination.auxiliary_entries[target_index];
			const bool same_pattern =
				target.registry == Phase2RawAuxiliaryRegistry::Pattern &&
				source_entry.registry ==
					Phase2RawAuxiliaryRegistry::Pattern &&
				target.firing_pattern_source_code ==
					source_entry.firing_pattern_source_code;
			const bool same_named_authority =
				target.registry == source_entry.registry &&
				target.registry != Phase2RawAuxiliaryRegistry::Pattern &&
				target.name.view() == source_entry.name.view() &&
				target.firing_pattern_source_code ==
					source_entry.firing_pattern_source_code;
			if (same_pattern || same_named_authority) {
				target_key = target.capture_key;
				break;
			}
			if (target.registry == source_entry.registry &&
				target.registry != Phase2RawAuxiliaryRegistry::Pattern &&
				target.name.view() == source_entry.name.view()) {
				diagnostic.reason =
					Phase2ObservationDto::RawStaticDiagnostic::Reason::
						AmbiguousAuxiliaryName;
			}
		}
		if (target_key == 0U) {
			if (destination.auxiliary_count >=
				MaximumPhase2StaticAuxiliaryEntries) {
				return false;
			}
			auto& target =
				destination.auxiliary_entries[destination.auxiliary_count++];
			target = source_entry;
			target.capture_key = destination.auxiliary_count;
			target_key = target.capture_key;
		}
		aux_map[source_index] = target_key;
	}
	const auto remap_aux = [&](std::uint32_t source_key) noexcept {
		if (source_key == 0U) return 0U;
		for (std::uint32_t index = 0U; index < source.auxiliary_count; ++index)
			if (source.auxiliary_entries[index].capture_key == source_key)
				return aux_map[index];
		return 0U;
	};

	std::array<std::uint32_t, MaximumPhase2StaticWeapons> weapon_map{};
	for (std::uint32_t source_index = 0U;
		 source_index < source.weapon_count;
		 ++source_index) {
		const auto& source_weapon = source.weapon_definitions[source_index];
		std::uint32_t target_key = 0U;
		for (std::uint32_t target_index = 0U;
			 target_index < destination.weapon_count;
			 ++target_index) {
			const auto& target =
				destination.weapon_definitions[target_index];
			if (target.internal_name.view() ==
				source_weapon.internal_name.view()) {
				auto normalized_source = source_weapon;
				auto normalized_target = target;
				normalized_source.weapon_capture_key = 0U;
				normalized_target.weapon_capture_key = 0U;
				normalized_source.damage_type_capture_key =
					remap_aux(source_weapon.damage_type_capture_key);
				if (semantic_equal(
						normalized_source, normalized_target)) {
					target_key = target.weapon_capture_key;
					break;
				}
				diagnostic.reason =
					Phase2ObservationDto::RawStaticDiagnostic::Reason::
						DistinctSameNameWeaponVariant;
			}
		}
		if (target_key == 0U) {
			if (destination.weapon_count >= MaximumPhase2StaticWeapons)
				return false;
			auto& target =
				destination.weapon_definitions[destination.weapon_count++];
			target = source_weapon;
			target.weapon_capture_key = destination.weapon_count;
			target.damage_type_capture_key =
				remap_aux(source_weapon.damage_type_capture_key);
			normalize_semantic_floats(target);
			target_key = target.weapon_capture_key;
		}
		weapon_map[source_index] = target_key;
	}
	const auto remap_weapon = [&](std::uint32_t source_key) noexcept {
		if (source_key == 0U) return 0U;
		for (std::uint32_t index = 0U; index < source.weapon_count; ++index)
			if (source.weapon_definitions[index].weapon_capture_key ==
				source_key)
				return weapon_map[index];
		return 0U;
	};

	std::array<std::uint32_t, MaximumPhase2StaticClasses> class_map{};
	for (std::uint32_t source_index = 0U;
		 source_index < source.class_count;
		 ++source_index) {
		const auto& source_class = source.class_definitions[source_index];
		std::uint32_t target_key = 0U;
		for (std::uint32_t target_index = 0U;
			 target_index < destination.class_count;
			 ++target_index) {
			const auto& target = destination.class_definitions[target_index];
			if (target.internal_name.view() !=
					source_class.internal_name.view() ||
				target.bank_count != source_class.bank_count ||
				target.subsystem_count != source_class.subsystem_count) {
				continue;
			}
			auto normalized_source_class = source_class;
			auto normalized_target_class = target;
			normalized_source_class.class_capture_key = 0U;
			normalized_target_class.class_capture_key = 0U;
			normalized_source_class.subsystem_offset = 0U;
			normalized_target_class.subsystem_offset = 0U;
			normalized_source_class.bank_offset = 0U;
			normalized_target_class.bank_offset = 0U;
			normalized_source_class.species_capture_key =
				remap_aux(source_class.species_capture_key);
			normalized_source_class.ship_type_capture_key =
				remap_aux(source_class.ship_type_capture_key);
			normalized_source_class.iff_capture_key =
				remap_aux(source_class.iff_capture_key);
			normalized_source_class.wing_capture_key =
				remap_aux(source_class.wing_capture_key);
			normalized_source_class.armor_capture_key =
				remap_aux(source_class.armor_capture_key);
			normalized_source_class.damage_type_capture_key =
				remap_aux(source_class.damage_type_capture_key);
			normalized_source_class.countermeasure_weapon_capture_key =
				remap_weapon(
					source_class.countermeasure_weapon_capture_key);
			bool same_variant =
				semantic_equal(normalized_source_class,
					normalized_target_class);
			for (std::uint32_t subsystem = 0U;
				 same_variant && subsystem < source_class.subsystem_count;
				 ++subsystem) {
				auto source_subsystem = source.subsystem_storage[
					source_class.subsystem_offset + subsystem];
				auto target_subsystem = destination.subsystem_storage[
					target.subsystem_offset + subsystem];
				source_subsystem.subsystem_capture_key = 0U;
				target_subsystem.subsystem_capture_key = 0U;
				source_subsystem.armor_capture_key =
					remap_aux(source_subsystem.armor_capture_key);
				same_variant =
					semantic_equal(source_subsystem, target_subsystem);
			}
			for (std::uint32_t bank = 0U;
				 same_variant && bank < source_class.bank_count;
				 ++bank) {
				auto source_bank = source.bank_storage[
					source_class.bank_offset + bank];
				auto target_bank = destination.bank_storage[
					target.bank_offset + bank];
				source_bank.bank_capture_key = 0U;
				target_bank.bank_capture_key = 0U;
				source_bank.weapon_capture_key =
					remap_weapon(source_bank.weapon_capture_key);
				const auto relative_owner =
					[&](const Phase2RawStaticCatalog& catalog,
						const Phase2RawClassDefinition& owner_class,
						std::uint32_t owner_key) noexcept {
						if (owner_key == 0U) return 0U;
						for (std::uint32_t subsystem = 0U;
							 subsystem < owner_class.subsystem_count;
							 ++subsystem)
							if (catalog.subsystem_storage[
									owner_class.subsystem_offset + subsystem]
									.subsystem_capture_key == owner_key)
								return subsystem + 1U;
						return std::numeric_limits<std::uint32_t>::max();
					};
				source_bank.owner_subsystem_capture_key =
					relative_owner(source, source_class,
						source_bank.owner_subsystem_capture_key);
				target_bank.owner_subsystem_capture_key =
					relative_owner(destination, target,
						target_bank.owner_subsystem_capture_key);
				same_variant =
					semantic_equal(source_bank, target_bank);
			}
			if (same_variant) {
				target_key = target.class_capture_key;
				break;
			}
			diagnostic.reason =
				Phase2ObservationDto::RawStaticDiagnostic::Reason::
					DistinctSameNameClassVariant;
		}
		if (target_key == 0U) {
			if (destination.class_count >= MaximumPhase2StaticClasses ||
				destination.aggregate_subsystem_count +
						source_class.subsystem_count >
					MaximumPhase2StaticSubsystems) {
				return false;
			}
			const auto target_index = destination.class_count++;
			auto& target = destination.class_definitions[target_index];
			target = source_class;
			target.class_capture_key = destination.class_count;
			target.species_capture_key =
				remap_aux(source_class.species_capture_key);
			target.ship_type_capture_key =
				remap_aux(source_class.ship_type_capture_key);
			target.iff_capture_key = remap_aux(source_class.iff_capture_key);
			target.wing_capture_key =
				remap_aux(source_class.wing_capture_key);
			target.armor_capture_key =
				remap_aux(source_class.armor_capture_key);
			target.damage_type_capture_key =
				remap_aux(source_class.damage_type_capture_key);
			target.countermeasure_weapon_capture_key =
				remap_weapon(
					source_class.countermeasure_weapon_capture_key);
			normalize_semantic_floats(target);
			target.subsystem_offset =
				destination.aggregate_subsystem_count;
			for (std::uint32_t subsystem = 0U;
				 subsystem < source_class.subsystem_count;
				 ++subsystem) {
				auto& target_subsystem = destination.subsystem_storage[
					target.subsystem_offset + subsystem];
				const auto& source_subsystem = source.subsystem_storage[
					source_class.subsystem_offset + subsystem];
				target_subsystem = source_subsystem;
				target_subsystem
					.subsystem_capture_key =
					destination.aggregate_subsystem_count + subsystem + 1U;
				target_subsystem.armor_capture_key =
					remap_aux(source_subsystem.armor_capture_key);
				normalize_semantic_floats(target_subsystem);
			}
			target.bank_offset =
				target_index * MaximumPhase2StaticBanksPerClass;
			for (std::uint32_t bank = 0U; bank < source_class.bank_count;
				 ++bank) {
				auto& target_bank = destination.bank_storage[
					target.bank_offset + bank];
				const auto& source_bank = source.bank_storage[
					source_class.bank_offset + bank];
				target_bank = source_bank;
				target_bank.bank_capture_key =
					target_index * MaximumPhase2StaticBanksPerClass +
					bank + 1U;
				target_bank.weapon_capture_key =
					remap_weapon(source_bank.weapon_capture_key);
				normalize_semantic_floats(target_bank);
				const auto owner = source_bank.owner_subsystem_capture_key;
				if (owner != 0U) {
					bool owner_found = false;
					for (std::uint32_t subsystem = 0U;
						 subsystem < source_class.subsystem_count;
						 ++subsystem) {
						const auto& source_subsystem =
							source.subsystem_storage[
								source_class.subsystem_offset + subsystem];
						if (source_subsystem.subsystem_capture_key == owner) {
							target_bank.owner_subsystem_capture_key =
								destination.subsystem_storage[
									target.subsystem_offset + subsystem]
									.subsystem_capture_key;
							owner_found = true;
							break;
						}
					}
					if (!owner_found) {
						diagnostic.reason =
							Phase2ObservationDto::RawStaticDiagnostic::Reason::
								MissingSubsystemOwner;
						return false;
					}
				}
			}
			destination.aggregate_subsystem_count +=
				source_class.subsystem_count;
			target_key = target.class_capture_key;
		}
		class_map[source_index] = target_key;
	}
	for (std::uint32_t index = 0U; index < source.class_count; ++index)
		if (source.class_definitions[index].class_capture_key ==
			references.class_capture_key)
			references.class_capture_key = class_map[index];
	for (std::uint32_t index = 0U; index < references.weapon_count; ++index)
		references.weapon_capture_keys[index] =
			remap_weapon(references.weapon_capture_keys[index]);
	for (std::uint32_t index = 0U; index < references.auxiliary_count; ++index)
		references.auxiliary_capture_keys[index] =
			remap_aux(references.auxiliary_capture_keys[index]);
	return references.class_capture_key != 0U;
}

Phase2CaptureResult capture_failure(Phase2ObservationDto& output,
	Phase2CaptureStatus status,
	Phase2CaptureReason reason) noexcept
{
	reset_observation(output);
	output.capture.status = status;
	output.capture.reason = reason;
	return output.capture;
}

bool canonicalize_bounded(float value, float minimum, float maximum, float& output) noexcept
{
	output = 0.0F;
	if (!std::isfinite(value) || value < minimum || value > maximum) {
		return false;
	}
	output = value == 0.0F ? 0.0F : value;
	return true;
}

template <std::size_t Size>
bool canonicalize_array(std::array<float, Size>& values, float minimum, float maximum) noexcept
{
	for (auto& value : values) {
		if (!canonicalize_bounded(value, minimum, maximum, value)) {
			return false;
		}
	}
	return true;
}

bool canonicalize_orientation(std::array<float, 4U>& orientation) noexcept
{
	if (!canonicalize_array(orientation, -1.0F, 1.0F)) {
		return false;
	}
	const auto squared_norm = static_cast<double>(orientation[0]) * orientation[0] +
		static_cast<double>(orientation[1]) * orientation[1] +
		static_cast<double>(orientation[2]) * orientation[2] +
		static_cast<double>(orientation[3]) * orientation[3];
	const auto norm = std::sqrt(squared_norm);
	if (!std::isfinite(norm) || norm < 0.9999 || norm > 1.0001) {
		return false;
	}
	if (orientation[0] < 0.0F) {
		return false;
	}
	if (orientation[0] == 0.0F) {
		for (std::size_t index = 1U; index < orientation.size(); ++index) {
			if (orientation[index] != 0.0F) {
				return orientation[index] > 0.0F;
			}
		}
		return false;
	}
	return true;
}

bool canonicalize_ship_blocks(ShipObservationDto& ship) noexcept
{
	if (static_cast<std::uint8_t>(ship.lifecycle.state) >=
			static_cast<std::uint8_t>(ShipLifecycleState::Count) ||
		ship.identity.class_source_key > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
		(ship.identity.presence & ~protocol::KnownShipIdentityPresenceFlags) != 0U ||
		(ship.lifecycle.presence & ~protocol::KnownEntityLifecyclePresenceFlags) != 0U ||
		(ship.lifecycle.lifecycle_flags & ~protocol::KnownEntityLifecycleFlags) != 0U ||
		(ship.energy.presence & ~protocol::KnownEnergyStatePresenceFlags) != 0U ||
		(ship.propulsion.presence & ~protocol::KnownPropulsionStatePresenceFlags) != 0U ||
		(ship.weapons.presence & ~protocol::KnownWeaponStatePresenceFlags) != 0U ||
		(ship.support.presence & ~protocol::KnownSupportStatePresenceFlags) != 0U ||
		(ship.docking.presence & ~protocol::KnownDockingStatePresenceFlags) != 0U ||
		(ship.propulsion.propulsion_flags & ~static_cast<std::uint32_t>(protocol::KnownPropulsionFlags)) !=
			0U) {
		return false;
	}

	if (!canonicalize_array(ship.flight.position_world, -1.0e12F, 1.0e12F) ||
		!canonicalize_orientation(ship.flight.orientation_local_to_world) ||
		!canonicalize_array(ship.flight.velocity_world, -1.0e9F, 1.0e9F) ||
		!canonicalize_array(ship.flight.rotational_velocity_local, -1.0e6F, 1.0e6F) ||
		!canonicalize_bounded(ship.flight.radius, 0.0F, 1.0e9F, ship.flight.radius)) {
		return false;
	}

	if (!canonicalize_bounded(ship.damage.hull_maximum, 0.0F, 1.0e12F, ship.damage.hull_maximum) ||
		!canonicalize_bounded(
			ship.damage.hull_current, 0.0F, ship.damage.hull_maximum, ship.damage.hull_current) ||
		!canonicalize_bounded(
			ship.damage.guardian_threshold, 0.0F, ship.damage.hull_maximum, ship.damage.guardian_threshold)) {
		return false;
	}

	if (ship.shields.segment_count > MaximumPhase2ShieldSegments ||
		!canonicalize_array(ship.shields.segment_current_hits, 0.0F, 1.0e12F) ||
		!canonicalize_array(ship.shields.segment_maximum_hits, 0.0F, 1.0e12F) ||
		!canonicalize_bounded(
			ship.shields.recharge_maximum, 0.0F, 1.0e12F, ship.shields.recharge_maximum) ||
		!canonicalize_bounded(
			ship.shields.regeneration_rate, 0.0F, 1.0e12F, ship.shields.regeneration_rate) ||
		!canonicalize_bounded(
			ship.shields.deferred_transfer, -1.0e12F, 1.0e12F, ship.shields.deferred_transfer)) {
		return false;
	}
	if (!ship.shields.has_shields) {
		if (ship.shields.segment_count != 0U || ship.shields.recharge_maximum != 0.0F ||
			ship.shields.regeneration_rate != 0.0F || ship.shields.deferred_transfer != 0.0F) {
			return false;
		}
	} else {
		if (ship.shields.segment_count == 0U) {
			return false;
		}
		double physical_maximum = 0.0;
		for (std::size_t index = 0U; index < ship.shields.segment_count; ++index) {
			if (ship.shields.segment_current_hits[index] > ship.shields.segment_maximum_hits[index]) {
				return false;
			}
			physical_maximum += ship.shields.segment_maximum_hits[index];
		}
		if (!std::isfinite(physical_maximum) ||
			static_cast<double>(ship.shields.recharge_maximum) > physical_maximum) {
			return false;
		}
	}

	if (!canonicalize_bounded(
			ship.energy.weapon_energy_maximum, 0.0F, 1.0e12F, ship.energy.weapon_energy_maximum) ||
		!canonicalize_bounded(ship.energy.weapon_energy_current,
			0.0F,
			ship.energy.weapon_energy_maximum,
			ship.energy.weapon_energy_current) ||
		ship.energy.shield_recharge_index > 12U || ship.energy.weapon_recharge_index > 12U ||
		ship.energy.engine_recharge_index > 12U) {
		return false;
	}
	if (!ship.energy.ets_available &&
		(ship.energy.shield_recharge_index != 0U || ship.energy.weapon_recharge_index != 0U ||
			ship.energy.engine_recharge_index != 0U)) {
		return false;
	}

	if (!canonicalize_bounded(
			ship.propulsion.afterburner_capacity, 0.0F, 1.0e12F, ship.propulsion.afterburner_capacity) ||
		!canonicalize_bounded(ship.propulsion.afterburner_fuel,
			0.0F,
			ship.propulsion.afterburner_capacity,
			ship.propulsion.afterburner_fuel) ||
		!canonicalize_bounded(
			ship.propulsion.burn_rate, 0.0F, 1.0e12F, ship.propulsion.burn_rate) ||
		!canonicalize_bounded(
			ship.propulsion.recovery_rate, 0.0F, 1.0e12F, ship.propulsion.recovery_rate) ||
		ship.propulsion.cooldown_remaining_us > 86'400'000'000ULL ||
		ship.propulsion.time_since_last_stop_us > 86'400'000'000ULL) {
		return false;
	}
	const auto has_propulsion_fuel =
		(ship.propulsion.presence & 0x0000000000000001ULL) != 0U;
	const auto has_propulsion_consumption =
		(ship.propulsion.presence & 0x0000000000000002ULL) != 0U;
	const auto has_propulsion_engagement =
		(ship.propulsion.presence & 0x0000000000000004ULL) != 0U;
	if (has_propulsion_fuel != has_propulsion_consumption ||
		(ship.propulsion.afterburner_capacity > 0.0F && !has_propulsion_fuel) ||
		(ship.propulsion.afterburner_capacity == 0.0F &&
			(has_propulsion_fuel || has_propulsion_engagement))) {
		return false;
	}
	if (!has_propulsion_engagement &&
		(ship.propulsion.cooldown_remaining_us != 0U ||
			ship.propulsion.time_since_last_stop_us != 0U)) {
		return false;
	}
	if (ship.propulsion.afterburner_capacity == 0.0F &&
		(ship.propulsion.afterburner_fuel != 0.0F || ship.propulsion.burn_rate != 0.0F ||
			ship.propulsion.recovery_rate != 0.0F)) {
		return false;
	}
	const auto afterburner_available =
		(ship.propulsion.propulsion_flags & protocol::PropulsionFlagAfterburnerAvailable) != 0U;
	const auto afterburner_locked =
		(ship.propulsion.propulsion_flags & protocol::PropulsionFlagAfterburnerLocked) != 0U;
	const auto afterburner_active =
		(ship.propulsion.propulsion_flags & protocol::PropulsionFlagAfterburnerActive) != 0U;
	const auto afterburner_requested =
		(ship.propulsion.propulsion_flags & protocol::PropulsionFlagAfterburnerRequested) != 0U;
	if (afterburner_available != has_propulsion_fuel ||
		(afterburner_locked && !afterburner_available) ||
		(afterburner_active && (!afterburner_available || afterburner_locked)) ||
		(afterburner_requested && (!afterburner_available || afterburner_locked))) {
		return false;
	}

	if (ship.weapons.primary_bank_count > MaximumPhase2WeaponBanksPerFamily ||
		ship.weapons.secondary_bank_count > MaximumPhase2WeaponBanksPerFamily ||
		ship.weapons.countermeasure_count >
			ship.weapons.countermeasure_maximum ||
		ship.subsystems.count > MaximumPhase2SubsystemsPerShip ||
		ship.docking.relation_count > MaximumPhase2DockRelationsPerShip ||
		static_cast<std::uint8_t>(ship.support.phase) >=
			static_cast<std::uint8_t>(ShipSupportPhase::Count) ||
		!canonicalize_bounded(ship.support.repair_progress,
			0.0F,
			1.0F,
			ship.support.repair_progress) ||
		!canonicalize_bounded(ship.support.rearm_progress,
			0.0F,
			1.0F,
			ship.support.rearm_progress) ||
		(ship.support.raw_support_flags & ~0x07ULL) != 0U) {
		return false;
	}
	const auto validate_bank = [](ShipWeaponBankObservation& bank,
								   ShipWeaponBankFamily family) noexcept {
		return bank.family == family &&
			bank.source_bank_key <= static_cast<std::uint32_t>(std::numeric_limits<int>::max()) &&
			bank.weapon_class_source_key.value <=
				static_cast<std::uint32_t>(std::numeric_limits<int>::max()) &&
			bank.ammunition_current <= bank.ammunition_initial &&
			bank.cooldown_remaining_us <= 86'400'000'000ULL;
	};
	for (std::size_t index = 0U; index < ship.weapons.primary_bank_count; ++index) {
		if (!validate_bank(ship.weapons.primary_banks[index], ShipWeaponBankFamily::Primary)) {
			return false;
		}
	}
	for (std::size_t index = 0U; index < ship.weapons.secondary_bank_count; ++index) {
		if (!validate_bank(ship.weapons.secondary_banks[index], ShipWeaponBankFamily::Secondary)) {
			return false;
		}
	}
	const auto has_support =
		(ship.support.presence & protocol::SupportStatePresenceFlagSupportEntity) != 0U;
	if (has_support != (ship.support.support_capture_key.value != 0U) ||
		(!has_support && (ship.support.phase != ShipSupportPhase::None ||
			ship.support.episode_sequence != 0U || ship.support.repair_progress != 0.0F ||
			ship.support.rearm_progress != 0.0F)) ||
		!canonicalize_bounded(ship.support.raw_hull_repair_work,
			0.0F,
			1.0e15F,
			ship.support.raw_hull_repair_work) ||
		!canonicalize_bounded(ship.support.raw_shield_repair_work,
			0.0F,
			1.0e15F,
			ship.support.raw_shield_repair_work) ||
		!canonicalize_bounded(ship.support.raw_subsystem_repair_work,
			0.0F,
			1.0e15F,
			ship.support.raw_subsystem_repair_work) ||
		!canonicalize_bounded(ship.support.raw_weapon_energy_rearm_work,
			0.0F,
			1.0e15F,
			ship.support.raw_weapon_energy_rearm_work) ||
		!canonicalize_bounded(ship.support.raw_max_hull_repair_fraction,
			0.0F,
			1.0F,
			ship.support.raw_max_hull_repair_fraction) ||
		!canonicalize_bounded(ship.support.raw_max_subsystem_repair_fraction,
			0.0F,
			1.0F,
			ship.support.raw_max_subsystem_repair_fraction) ||
		!canonicalize_bounded(ship.support.raw_hull_repair_rate,
			0.0F,
			1.0F,
			ship.support.raw_hull_repair_rate) ||
		!canonicalize_bounded(ship.support.raw_shield_repair_rate,
			0.0F,
			1.0F,
			ship.support.raw_shield_repair_rate) ||
		!canonicalize_bounded(ship.support.raw_subsystem_repair_rate,
			0.0F,
			1.0F,
			ship.support.raw_subsystem_repair_rate) ||
		ship.support.raw_countermeasure_rearm_pool < -1 ||
		ship.support.raw_countermeasure_rearm_work >
			ship.support.raw_countermeasure_capacity ||
		ship.support.raw_countermeasure_capacity !=
			ship.weapons.countermeasure_maximum ||
		(ship.support.raw_weapon_rearm_disallowed &&
			(ship.support.raw_ammunition_rearm_applicable ||
			 ship.support.raw_ammunition_rearm_work != 0U)) ||
		(ship.support.raw_hull_repair_applicable &&
			(!ship.support.raw_support_repairs_hull_authorized ||
			 ship.support.raw_hull_repair_rate <= 0.0F ||
			 ship.support.raw_hull_repair_work <= 0.0F)) ||
		(ship.support.raw_shield_repair_applicable &&
			(ship.support.raw_shield_repair_rate <= 0.0F ||
			 ship.support.raw_shield_repair_work <= 0.0F)) ||
		(ship.support.raw_subsystem_repair_applicable &&
			(ship.support.raw_subsystem_repair_rate <= 0.0F ||
			 ship.support.raw_subsystem_repair_work <= 0.0F)) ||
		(ship.support.raw_weapon_energy_rearm_applicable &&
			(ship.support.raw_mission_rearm_disallowed ||
			 ship.support.raw_weapon_energy_rearm_work <= 0.0F)) ||
		(ship.support.raw_ammunition_rearm_applicable &&
			(ship.support.raw_mission_rearm_disallowed ||
			 ship.support.raw_ammunition_rearm_work == 0U)) ||
		(ship.support.raw_countermeasure_rearm_applicable !=
			(ship.support.raw_countermeasure_rearm_work > 0U))) {
		return false;
	}
	for (std::size_t index = 0U; index < ship.docking.relation_count; ++index) {
		if (ship.docking.relations[index].remote_capture_key.value == 0U) {
			return false;
		}
	}
	for (std::size_t index = 0U; index < ship.subsystems.count; ++index) {
		auto& subsystem = ship.subsystems.values[index];
		if (static_cast<std::uint8_t>(subsystem.kind) >=
				static_cast<std::uint8_t>(ShipSubsystemKind::Count) ||
			(subsystem.presence & ~protocol::KnownSubsystemStatePresenceFlags) != 0U ||
			subsystem.source_key.value == 0U ||
			!canonicalize_bounded(
				subsystem.hits_maximum, 0.0F, 1.0e12F, subsystem.hits_maximum) ||
			!canonicalize_bounded(
				subsystem.hits_current, 0.0F, subsystem.hits_maximum, subsystem.hits_current) ||
			!canonicalize_bounded(subsystem.aggregate_maximum_hits,
				0.0F,
				1.0e12F,
				subsystem.aggregate_maximum_hits) ||
			!canonicalize_bounded(subsystem.aggregate_current_hits,
				0.0F,
				subsystem.aggregate_maximum_hits,
				subsystem.aggregate_current_hits) ||
			!canonicalize_array(subsystem.position_local, -1.0e9F, 1.0e9F) ||
			!canonicalize_orientation(subsystem.orientation_local) ||
			subsystem.disruption_remaining_us > 86'400'000'000ULL) {
			return false;
		}
		if (!subsystem.turret.has_value()) {
			continue;
		}
		auto& turret = *subsystem.turret;
		if (subsystem.kind != ShipSubsystemKind::Turret ||
			turret.turret_primary_bank_count > MaximumPhase2PhysicalPrimaryBanks ||
			turret.turret_secondary_bank_count > MaximumPhase2PhysicalSecondaryBanks ||
			turret.turret_next_fire_remaining_us > 86'400'000'000ULL ||
			!canonicalize_array(
				turret.turret_current_direction_local, -1.0F, 1.0F) ||
			!canonicalize_bounded(
				turret.turret_rof_scaler, 0.0F, 1.0e6F, turret.turret_rof_scaler)) {
			return false;
		}
		for (std::size_t bank = 0U; bank < turret.turret_primary_bank_count; ++bank) {
			const auto& facts = turret.turret_primary_banks[bank];
			if (turret.turret_primary_bank_weapon_source_keys[bank].value == 0U ||
				facts.turret_ammunition_current < 0 ||
				facts.turret_ammunition_capacity < 0 ||
				facts.turret_ammunition_current >
					facts.turret_ammunition_capacity ||
				facts.turret_cooldown_remaining_us > 86'400'000'000ULL) {
				return false;
			}
		}
		for (std::size_t bank = 0U; bank < turret.turret_secondary_bank_count; ++bank) {
			const auto& facts = turret.turret_secondary_banks[bank];
			if (turret.turret_secondary_bank_weapon_source_keys[bank].value == 0U ||
				facts.turret_ammunition_current < 0 ||
				facts.turret_ammunition_capacity < 0 ||
				facts.turret_ammunition_current >
					facts.turret_ammunition_capacity ||
				facts.turret_cooldown_remaining_us > 86'400'000'000ULL) {
				return false;
			}
		}
	}
	return true;
}

bool canonicalize_player_controls(PlayerControlObservation& controls) noexcept
{
	if (static_cast<std::uint8_t>(controls.mode) >=
			static_cast<std::uint8_t>(PlayerControlModeObservation::Count) ||
		(controls.presence & ~protocol::KnownControlStatePresenceFlags) != 0U ||
		(controls.action_flags & ~protocol::KnownControlFlags) != 0U ||
		!canonicalize_bounded(controls.pitch, -1.0F, 1.0F, controls.pitch) ||
		!canonicalize_bounded(controls.heading, -1.0F, 1.0F, controls.heading) ||
		!canonicalize_bounded(controls.bank, -1.0F, 1.0F, controls.bank) ||
		!canonicalize_bounded(controls.forward, -1.0F, 1.0F, controls.forward) ||
		!canonicalize_bounded(controls.sideways, -1.0F, 1.0F, controls.sideways) ||
		!canonicalize_bounded(controls.vertical, -1.0F, 1.0F, controls.vertical) ||
		!canonicalize_bounded(
			controls.forward_cruise_percent, -100.0F, 100.0F, controls.forward_cruise_percent) ||
		!canonicalize_bounded(
			controls.flight_cursor_pitch, -3.1415927F, 3.1415927F, controls.flight_cursor_pitch) ||
		!canonicalize_bounded(
			controls.flight_cursor_heading, -3.1415927F, 3.1415927F, controls.flight_cursor_heading) ||
		!canonicalize_bounded(
			controls.flight_cursor_sensitivity, 0.0F, 1.0F, controls.flight_cursor_sensitivity) ||
		!canonicalize_bounded(
			controls.effective_aim_extent, 0.000001F, 3.1415927F, controls.effective_aim_extent)) {
		return false;
	}
	if ((controls.presence & protocol::ControlStatePresenceFlagCruise) == 0U &&
		controls.forward_cruise_percent != 0.0F) {
		return false;
	}
	if ((controls.presence & protocol::ControlStatePresenceFlagRequestCounters) == 0U &&
		(controls.fire_primary_count != 0U || controls.fire_secondary_count != 0U ||
			controls.fire_countermeasure_count != 0U)) {
		return false;
	}
	return true;
}

bool validate_player_cargo_scan(const PlayerCargoScanObservation& cargo) noexcept
{
	if (static_cast<std::uint8_t>(cargo.phase) >=
			static_cast<std::uint8_t>(CargoScanPhaseObservation::Count) ||
		(cargo.presence & ~protocol::KnownCargoScanStatePresenceFlags) != 0U ||
		(cargo.validity_flags & ~static_cast<std::uint32_t>(protocol::KnownScanValidityFlags)) != 0U) {
		return false;
	}
	const auto text = cargo.cargo_text.view();
	for (std::size_t index = 0U; index < text.size();) {
		const auto first = static_cast<std::uint8_t>(text[index]);
		if (first <= 0x7fU) {
			++index;
			continue;
		}
		std::size_t continuation_count = 0U;
		std::uint32_t code_point = 0U;
		if (first >= 0xc2U && first <= 0xdfU) {
			continuation_count = 1U;
			code_point = first & 0x1fU;
		} else if (first >= 0xe0U && first <= 0xefU) {
			continuation_count = 2U;
			code_point = first & 0x0fU;
		} else if (first >= 0xf0U && first <= 0xf4U) {
			continuation_count = 3U;
			code_point = first & 0x07U;
		} else {
			return false;
		}
		if (index + continuation_count >= text.size()) {
			return false;
		}
		for (std::size_t offset = 1U; offset <= continuation_count; ++offset) {
			const auto next = static_cast<std::uint8_t>(text[index + offset]);
			if ((next & 0xc0U) != 0x80U) {
				return false;
			}
			code_point = (code_point << 6U) | (next & 0x3fU);
		}
		if ((continuation_count == 2U && code_point < 0x800U) ||
			(continuation_count == 3U && code_point < 0x10000U) ||
			(code_point >= 0xd800U && code_point <= 0xdfffU) || code_point > 0x10ffffU) {
			return false;
		}
		index += continuation_count + 1U;
	}
	const auto has_target =
		(cargo.presence & protocol::CargoScanStatePresenceFlagTarget) != 0U;
	const auto has_subsystem =
		(cargo.presence & protocol::CargoScanStatePresenceFlagSubsystem) != 0U;
	const auto has_timing =
		(cargo.presence & protocol::CargoScanStatePresenceFlagTiming) != 0U;
	const auto has_validity =
		(cargo.presence & protocol::CargoScanStatePresenceFlagValidity) != 0U;
	const auto has_text =
		(cargo.presence & protocol::CargoScanStatePresenceFlagCargoText) != 0U;
	if (has_target != (cargo.target_capture_key.value != 0U) ||
		(has_subsystem && (!has_target ||
			cargo.target_subsystem_source_key.value == 0U)) ||
		(!has_subsystem && cargo.target_subsystem_source_key.value != 0U) ||
		(!has_timing && (cargo.elapsed_us != 0U || cargo.required_us != 0U)) ||
		(has_timing && (cargo.required_us == 0U ||
			(cargo.phase != CargoScanPhaseObservation::Completed &&
				cargo.elapsed_us > cargo.required_us))) ||
		(!has_validity && cargo.validity_flags != 0U) ||
		(has_text != !cargo.cargo_text.empty())) {
		return false;
	}
	switch (cargo.phase) {
	case CargoScanPhaseObservation::Idle:
		return has_target && has_timing && has_validity && !has_text;
	case CargoScanPhaseObservation::Scanning:
		return has_target && has_timing && has_validity &&
			cargo.elapsed_us < cargo.required_us;
	case CargoScanPhaseObservation::Completed:
		return has_target;
	case CargoScanPhaseObservation::NotScannable:
		return cargo.presence == protocol::CargoScanStatePresenceFlagNone &&
			!has_target && !has_subsystem && !has_timing && !has_validity && !has_text;
	case CargoScanPhaseObservation::Count:
	default:
		return false;
	}
}

Phase2SourceReadStatus validate_discovery_keys(
	const Phase2DiscoveryNode& node) noexcept
{
	if (node.direct_docking_count > MaximumPhase2DockRelationsPerShip) {
		return Phase2SourceReadStatus::SourceLimitExceeded;
	}
	if (node.capture_key.value == 0U) {
		return Phase2SourceReadStatus::UnsupportedEngineState;
	}
	for (std::size_t index = 0U;
		 index < node.direct_docking_count;
		 ++index) {
		if (node.direct_docking_capture_keys[index].value == 0U) {
			return Phase2SourceReadStatus::UnsupportedEngineState;
		}
	}
	return Phase2SourceReadStatus::Valid;
}

} // namespace

SourceReadResult map_phase2_static_authorities(
	const Phase2StaticAuthorityInput& input,
	Phase2RawStaticCatalog& catalog) noexcept
{
	if (input.bank_count <= MaximumPhase2StaticBanksPerClass) {
		for (std::uint32_t bank = 0U; bank < input.bank_count; ++bank) {
			const auto& authority = input.banks[bank];
			if (authority.fire_point_count >
					MaximumPhase2StaticFirePoints ||
				authority.num_slots > MaximumPhase2StaticFirePoints) {
				return {Phase2SourceReadStatus::SourceLimitExceeded};
			}
			if (authority.family_source != 3U &&
				(authority.num_slots == 0U ||
				 authority.fire_point_count != authority.num_slots)) {
				return {Phase2SourceReadStatus::UnsupportedEngineState};
			}
		}
	}
	if (!input.guards_valid ||
		!std::isfinite(input.weapon_info.fire_wait_seconds) ||
		input.weapon_info.fire_wait_seconds < 0.0F ||
		input.subsystem_count > MaximumPhase2SubsystemsPerShip ||
		input.bank_count > MaximumPhase2StaticBanksPerClass ||
		!validate_raw_static_catalog_bounds(catalog)) {
		catalog.clear();
		return {Phase2SourceReadStatus::UnsupportedEngineState};
	}

	catalog.class_count = std::max(catalog.class_count, 1U);
	catalog.weapon_count = std::max(catalog.weapon_count, 1U);
	auto& ship_class = catalog.class_definitions[0];
	ship_class.class_capture_key = 1U;
	ship_class.effective_mass = input.ship_info.effective_mass;
	ship_class.max_rear_velocity = input.ship_info.max_rear_velocity;
	ship_class.center_of_mass = input.model.center_of_mass;
	ship_class.subsystem_count = input.subsystem_count;
	ship_class.subsystem_offset = 0U;
	ship_class.bank_count = input.bank_count;
	ship_class.bank_offset = 0U;
	ship_class.primary_bank_count = 0U;
	ship_class.secondary_bank_count = 0U;
	ship_class.tertiary_bank_count = 0U;
	ship_class.turret_bank_count = 0U;
	for (std::uint32_t index = 0U; index < input.subsystem_count; ++index) {
		catalog.subsystem_storage[index] = input.subsystems[index];
	}
	for (std::uint32_t index = 0U; index < input.bank_count; ++index) {
		catalog.bank_storage[index] =
			static_cast<const Phase2RawBankDefinition&>(input.banks[index]);
		switch (input.banks[index].family_source) {
		case 1U: ++ship_class.primary_bank_count; break;
		case 2U: ++ship_class.secondary_bank_count; break;
		case 3U: ++ship_class.tertiary_bank_count; break;
		case 4U: ++ship_class.turret_bank_count; break;
		default:
			catalog.clear();
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
	}

	auto& weapon = catalog.weapon_definitions[0];
	weapon.reloaded_per_batch =
		std::max(weapon.reloaded_per_batch, 1U);
	weapon.weapon_capture_key = 1U;
	weapon.mass = input.weapon_info.mass;
	weapon.damage = input.weapon_info.damage;
	weapon.fire_wait_seconds = input.weapon_info.fire_wait_seconds;
	catalog.aggregate_subsystem_count = input.subsystem_count;

	if (catalog.auxiliary_count == 0U &&
		input.registries.species.capture_key != 0U) {
		catalog.auxiliary_entries[catalog.auxiliary_count] =
			input.registries.species;
		catalog.auxiliary_entries[catalog.auxiliary_count].registry =
			Phase2RawAuxiliaryRegistry::Species;
		ship_class.species_capture_key =
			input.registries.species.capture_key;
		++catalog.auxiliary_count;
	}
	if (input.registries.weapon_damage_type.capture_key != 0U) {
		if (catalog.auxiliary_count == 0U ||
			catalog.weapon_definitions[0].damage_type_capture_key == 0U) {
			catalog.auxiliary_entries[catalog.auxiliary_count] =
				input.registries.weapon_damage_type;
			catalog.auxiliary_entries[catalog.auxiliary_count].registry =
				Phase2RawAuxiliaryRegistry::DamageType;
			++catalog.auxiliary_count;
		}
		weapon.damage_type_capture_key =
			input.registries.weapon_damage_type.capture_key;
	}
	if (catalog.class_count != 1U ||
		!validate_raw_static_catalog_bounds(catalog)) {
		catalog.clear();
		return {Phase2SourceReadStatus::UnsupportedEngineState};
	}
	return {Phase2SourceReadStatus::Valid};
}

bool OwnedPhase2String::assign(std::string_view value) noexcept
{
	if (value.size() > MaximumPhase2InternalNameBytes) {
		return false;
	}
	if (!value.empty()) {
		std::memcpy(m_bytes.data(), value.data(), value.size());
	}
	m_size = static_cast<std::uint16_t>(value.size());
	m_bytes[m_size] = '\0';
	return true;
}

std::size_t OwnedPhase2String::size() const noexcept
{
	return m_size;
}

bool OwnedPhase2String::empty() const noexcept
{
	return m_size == 0U;
}

const char* OwnedPhase2String::data() const noexcept
{
	return m_bytes.data();
}

std::string_view OwnedPhase2String::view() const noexcept
{
	return {m_bytes.data(), m_size};
}

Phase2RawStaticCatalog::Phase2RawStaticCatalog() noexcept {}

Phase2RawStaticCatalog::Phase2RawStaticCatalog(
	const Phase2RawStaticCatalog& other) noexcept : Phase2RawStaticCatalog()
{
	copy_from(other);
}

Phase2RawStaticCatalog& Phase2RawStaticCatalog::operator=(
	const Phase2RawStaticCatalog& other) noexcept
{
	if (this != &other) copy_from(other);
	return *this;
}

void Phase2RawStaticCatalog::clear() noexcept
{
	class_count = 0U;
	weapon_count = 0U;
	auxiliary_count = 0U;
	aggregate_subsystem_count = 0U;
}

bool Phase2RawStaticCatalog::copy_from(
	const Phase2RawStaticCatalog& other) noexcept
{
	if (other.class_count > MaximumPhase2StaticClasses ||
		other.weapon_count > MaximumPhase2StaticWeapons ||
		other.auxiliary_count > MaximumPhase2StaticAuxiliaryEntries ||
		other.aggregate_subsystem_count > MaximumPhase2StaticSubsystems) {
		class_count=other.class_count;weapon_count=other.weapon_count;
		auxiliary_count=other.auxiliary_count;
		aggregate_subsystem_count=other.aggregate_subsystem_count;
		return false;
	}
	for (std::uint32_t index = 0U; index < other.class_count; ++index) {
		const auto& source_class = other.class_definitions[index];
		const auto family_total = source_class.primary_bank_count +
			source_class.secondary_bank_count + source_class.tertiary_bank_count +
			source_class.turret_bank_count;
		if (source_class.subsystem_count > MaximumPhase2SubsystemsPerShip ||
			source_class.bank_count > MaximumPhase2StaticBanksPerClass ||
			source_class.primary_bank_count > MaximumPhase2WeaponBanksPerFamily ||
			source_class.secondary_bank_count > MaximumPhase2WeaponBanksPerFamily ||
			source_class.tertiary_bank_count > MaximumPhase2WeaponBanksPerFamily ||
			source_class.turret_bank_count > MaximumPhase2WeaponBanksPerFamily ||
			family_total != source_class.bank_count ||
			source_class.subsystem_offset > MaximumPhase2StaticSubsystems ||
			source_class.subsystem_count >
				MaximumPhase2StaticSubsystems -
					source_class.subsystem_offset ||
			source_class.bank_offset > bank_storage.size() ||
			source_class.bank_count >
				bank_storage.size() - source_class.bank_offset) {
			class_count=other.class_count;weapon_count=other.weapon_count;
			auxiliary_count=other.auxiliary_count;
			aggregate_subsystem_count=other.aggregate_subsystem_count;
			class_definitions[index]=source_class;
			return false;
		}
		for (std::uint32_t bank = 0U; bank < source_class.bank_count; ++bank) {
			if (other.bank_storage[source_class.bank_offset + bank]
					.fire_point_count >
				MaximumPhase2StaticFirePoints) {
				class_count=other.class_count;weapon_count=other.weapon_count;
				auxiliary_count=other.auxiliary_count;
				aggregate_subsystem_count=other.aggregate_subsystem_count;
				class_definitions[index]=source_class;
				return false;
			}
		}
	}
	if (!validate_raw_static_catalog_bounds(other)) {
		class_count=other.class_count;weapon_count=other.weapon_count;
		auxiliary_count=other.auxiliary_count;
		aggregate_subsystem_count=other.aggregate_subsystem_count;
		return false;
	}

	for (std::uint32_t index = 0U; index < other.class_count; ++index) {
		const auto& source_class = other.class_definitions[index];
		class_definitions[index] = source_class;
		for (std::uint32_t subsystem = 0U;
			 subsystem < source_class.subsystem_count; ++subsystem) {
			subsystem_storage[source_class.subsystem_offset + subsystem] =
				other.subsystem_storage[
					source_class.subsystem_offset + subsystem];
		}
		for (std::uint32_t bank = 0U; bank < source_class.bank_count; ++bank) {
			bank_storage[source_class.bank_offset + bank] =
				other.bank_storage[source_class.bank_offset + bank];
		}
	}
	for (std::uint32_t index = 0U; index < other.weapon_count; ++index) {
		weapon_definitions[index] = other.weapon_definitions[index];
	}
	for (std::uint32_t index = 0U; index < other.auxiliary_count; ++index) {
		auxiliary_entries[index] = other.auxiliary_entries[index];
	}
	class_count = other.class_count;
	weapon_count = other.weapon_count;
	auxiliary_count = other.auxiliary_count;
	aggregate_subsystem_count = other.aggregate_subsystem_count;
	return true;
}

static Phase2CaptureResult collect_phase2_observation_with_scratch(
	const Phase2EngineReadView& source,
	const Phase2ObservationSelection& selection,
	std::uint64_t producer_sample_time_us,
	Phase2ObservationDto& output,
	Phase2ShipSource& source_ship,
	Phase2ObservationProjection projection) noexcept
{
	reset_observation(output);
	if (!source.current_thread_is_main()) {
		return capture_failure(output, Phase2CaptureStatus::InvalidSource, Phase2CaptureReason::WrongThread);
	}
	if (!source.in_mission()) {
		return capture_failure(output, Phase2CaptureStatus::InvalidSource, Phase2CaptureReason::NotInMission);
	}

	const auto player_exists = source.player_exists();
	const auto player_object_exists = source.player_object_exists();
	const auto player_ship_exists = source.player_ship_exists();
	if (!player_exists && !player_object_exists && !player_ship_exists) {
		return capture_failure(output, Phase2CaptureStatus::NoPlayer, Phase2CaptureReason::None);
	}
	if (!player_exists || !player_object_exists || !player_ship_exists) {
		return capture_failure(
			output, Phase2CaptureStatus::InvalidSource, Phase2CaptureReason::PartialPlayerSource);
	}
	if (!source.player_source_is_consistent()) {
		return capture_failure(
			output, Phase2CaptureStatus::InvalidSource, Phase2CaptureReason::InconsistentPlayerSource);
	}

	EngineEntityKey player_root;
	const auto player_root_result = source.read_player_root_key(player_root);
	if (player_root_result.status != Phase2SourceReadStatus::Valid ||
		player_root.object_signature == 0U) {
		const auto status =
			player_root_result.status == Phase2SourceReadStatus::SourceLimitExceeded
			? Phase2CaptureStatus::SourceLimitExceeded
			: player_root_result.status == Phase2SourceReadStatus::UnsupportedEngineState
			? Phase2CaptureStatus::UnsupportedEngineState
			: Phase2CaptureStatus::InvalidSource;
		return capture_failure(output, status, Phase2CaptureReason::ShipReadFailure);
	}

	Phase2DiscoveryNode player_discovery{};
	const auto player_discovery_result =
		source.read_discovery_node(player_root, player_discovery);
	const auto player_discovery_keys =
		player_discovery_result.status == Phase2SourceReadStatus::Valid
		? validate_discovery_keys(player_discovery)
		: player_discovery_result.status;
	if (player_discovery_keys != Phase2SourceReadStatus::Valid) {
		return capture_failure(output,
			player_discovery_keys == Phase2SourceReadStatus::SourceLimitExceeded
				? Phase2CaptureStatus::SourceLimitExceeded
				: Phase2CaptureStatus::UnsupportedEngineState,
			Phase2CaptureReason::ShipReadFailure);
	}
	auto effective_selection = selection;
	if (effective_selection.count == 0U) {
		effective_selection.count = 1U;
		effective_selection.ship_keys[0] = player_discovery.capture_key;
	}
	if (effective_selection.count > MaximumPhase2ObservationShips) {
		return capture_failure(
			output, Phase2CaptureStatus::SourceLimitExceeded, Phase2CaptureReason::InvalidClosureCardinality);
	}
	if (effective_selection.ship_keys[0].value !=
		player_discovery.capture_key.value) {
		return capture_failure(
			output, Phase2CaptureStatus::InvalidSource, Phase2CaptureReason::ShipReadFailure);
	}
	auto sorted_keys = effective_selection.ship_keys;
	std::sort(sorted_keys.begin(), sorted_keys.begin() + effective_selection.count,
		[](const Phase2CaptureLocalKey& left,
			const Phase2CaptureLocalKey& right) noexcept {
			return left.value < right.value;
		});
	for (std::size_t index = 0U; index < effective_selection.count; ++index) {
		if (sorted_keys[index].value == 0U) {
			return capture_failure(output,
				Phase2CaptureStatus::UnsupportedEngineState,
				Phase2CaptureReason::ShipReadFailure);
		}
		if (index != 0U &&
			sorted_keys[index - 1U].value == sorted_keys[index].value) {
			return capture_failure(output,
				Phase2CaptureStatus::InvalidSource,
				Phase2CaptureReason::DuplicateObjectSignature);
		}
	}
	const auto remap_capture_local =
		[&](Phase2CaptureLocalKey source_key) noexcept {
			if (source_key.value == 0U) {
				return Phase2CaptureLocalKey{};
			}
			for (std::size_t index = 0U;
				 index < effective_selection.count;
				 ++index) {
				if (effective_selection.ship_keys[index].value ==
					source_key.value) {
					return Phase2CaptureLocalKey{
						static_cast<std::uint32_t>(index + 1U)};
				}
			}
			return Phase2CaptureLocalKey{};
		};
	std::array<EngineEntityKey, MaximumPhase2ObservationShips> engine_keys{};
	for (std::size_t index = 0U; index < effective_selection.count; ++index) {
		if (index == 0U) {
			engine_keys[index] = player_root;
			continue;
		}
		const auto resolved = source.resolve_capture_local_key(
			effective_selection.ship_keys[index], engine_keys[index]);
		if (resolved.status != Phase2SourceReadStatus::Valid) {
			return capture_failure(output,
				resolved.status == Phase2SourceReadStatus::SourceLimitExceeded
					? Phase2CaptureStatus::SourceLimitExceeded
					: Phase2CaptureStatus::UnsupportedEngineState,
				Phase2CaptureReason::ShipReadFailure);
		}
	}

	try {
		Phase2SourceReadResult first_discovery_failure{
			Phase2SourceReadStatus::Valid};
		output.ships.reserve(effective_selection.count);
		for (std::size_t index = 0U; index < effective_selection.count; ++index) {
			source_ship.~Phase2ShipSource();
			new (&source_ship) Phase2ShipSource;
			const auto source_result =
				source.read_ship(engine_keys[index], source_ship);
			if (source_result.status != Phase2SourceReadStatus::Valid) {
				const auto status =
					source_result.status == Phase2SourceReadStatus::SourceLimitExceeded
					? Phase2CaptureStatus::SourceLimitExceeded
					: source_result.status == Phase2SourceReadStatus::UnsupportedEngineState
					? Phase2CaptureStatus::UnsupportedEngineState
					: Phase2CaptureStatus::InvalidSource;
				return capture_failure(
					output, status, Phase2CaptureReason::ShipReadFailure);
			}
			if (source_ship.internal_name.size() > MaximumPhase2InternalNameBytes) {
				return capture_failure(
					output, Phase2CaptureStatus::SourceLimitExceeded, Phase2CaptureReason::StringLimitExceeded);
			}
			if(!validate_raw_static_catalog_bounds(source_ship.raw_static_catalog)||
				source_ship.raw_static_references.weapon_count>MaximumPhase2StaticWeapons||
				source_ship.raw_static_references.auxiliary_count>
					MaximumPhase2StaticAuxiliaryEntries) {
				return capture_failure(output,Phase2CaptureStatus::SourceLimitExceeded,
					Phase2CaptureReason::UnsupportedShipBlock);
			}
			if (!merge_raw_static_catalog(output.raw_static_catalog,
					source_ship.raw_static_catalog,
					source_ship.raw_static_references,
					output.raw_static_diagnostic)) {
				const auto merge_status =
					output.raw_static_diagnostic.reason ==
						Phase2ObservationDto::RawStaticDiagnostic::Reason::None
					? Phase2CaptureStatus::SourceLimitExceeded
					: Phase2CaptureStatus::UnsupportedEngineState;
				return capture_failure(output,merge_status,
					Phase2CaptureReason::UnsupportedShipBlock);
			}
			output.ships.emplace_back();
			auto& ship = output.ships.back();
			ship.capture_key = {
				static_cast<std::uint32_t>(index + 1U)};
			ship.identity.presence = source_ship.identity.presence;
			ship.identity.class_source_key = source_ship.identity.class_source_key;
			ship.lifecycle = source_ship.lifecycle;
			ship.flight = source_ship.flight;
			ship.damage = source_ship.damage;
			ship.shields = source_ship.shields;
			ship.energy = source_ship.energy;
			ship.propulsion = source_ship.propulsion;
			ship.weapons = source_ship.weapons;
			ship.support = source_ship.support;
			ship.docking = source_ship.docking;
			ship.subsystems = source_ship.subsystems;
			ship.raw_static_references=source_ship.raw_static_references;
			if (ship.support.support_capture_key.value != 0U) {
				ship.support.support_capture_key =
					remap_capture_local(
						ship.support.support_capture_key);
				if (ship.support.support_capture_key.value == 0U) {
					return capture_failure(output,
						Phase2CaptureStatus::UnsupportedEngineState,
						Phase2CaptureReason::UnsupportedShipBlock);
				}
			}
			for (std::size_t relation = 0U;
				 relation < ship.docking.relation_count;
				 ++relation) {
				auto& remote =
					ship.docking.relations[relation]
						.remote_capture_key;
				remote = remap_capture_local(remote);
				if (remote.value == 0U) {
					return capture_failure(output,
						Phase2CaptureStatus::UnsupportedEngineState,
						Phase2CaptureReason::UnsupportedShipBlock);
				}
			}
			if (ship.weapons.primary_bank_count > MaximumPhase2WeaponBanksPerFamily ||
				ship.weapons.secondary_bank_count > MaximumPhase2WeaponBanksPerFamily ||
				ship.subsystems.count > MaximumPhase2SubsystemsPerShip ||
				ship.docking.relation_count > MaximumPhase2DockRelationsPerShip) {
				return capture_failure(output,
					Phase2CaptureStatus::SourceLimitExceeded,
					Phase2CaptureReason::UnsupportedShipBlock);
			}
			if (!ship.identity.internal_name.assign(source_ship.internal_name)) {
				return capture_failure(
					output, Phase2CaptureStatus::SourceLimitExceeded, Phase2CaptureReason::StringLimitExceeded);
			}
			if (!ship.identity.class_name.assign(source_ship.class_name)) {
				return capture_failure(
					output, Phase2CaptureStatus::SourceLimitExceeded, Phase2CaptureReason::StringLimitExceeded);
			}
			ship.identity.sample_time_us = producer_sample_time_us;
			ship.lifecycle.sample_time_us = producer_sample_time_us;
			ship.flight.sample_time_us = producer_sample_time_us;
			ship.damage.sample_time_us = producer_sample_time_us;
			ship.shields.sample_time_us = producer_sample_time_us;
			ship.energy.sample_time_us = producer_sample_time_us;
			ship.propulsion.sample_time_us = producer_sample_time_us;
			ship.weapons.sample_time_us = producer_sample_time_us;
			ship.support.sample_time_us = producer_sample_time_us;
			ship.docking.sample_time_us = producer_sample_time_us;
			for (std::size_t subsystem_index = 0U;
				 subsystem_index < ship.subsystems.count &&
				 subsystem_index < MaximumPhase2SubsystemsPerShip;
				 ++subsystem_index) {
				ship.subsystems.values[subsystem_index].sample_time_us =
					producer_sample_time_us;
			}
			if (!canonicalize_ship_blocks(ship)) {
				return capture_failure(output,
					Phase2CaptureStatus::UnsupportedEngineState,
					Phase2CaptureReason::UnsupportedShipBlock);
			}
			if (projection == Phase2ObservationProjection::CoreGate) {
				continue;
			}
			if (projection == Phase2ObservationProjection::DiscoveryExtension ||
				projection == Phase2ObservationProjection::CompleteShip) {
				Phase2DiscoveryNode discovery_node;
				// Discovery is deliberately best-effort: CoreGate ship blocks
				// remain publishable when support/docking extension facts are
				// temporarily unavailable or unsupported.
				const auto discovery_result = source.read_discovery_node(
					engine_keys[index], discovery_node);
				if (discovery_result.status == Phase2SourceReadStatus::Valid) {
					const auto discovery_keys =
						validate_discovery_keys(discovery_node);
					if(discovery_keys != Phase2SourceReadStatus::Valid ||
						discovery_node.capture_key.value !=
							effective_selection.ship_keys[index].value)
						return capture_failure(output,
							discovery_keys ==
									Phase2SourceReadStatus::SourceLimitExceeded
								? Phase2CaptureStatus::SourceLimitExceeded
								: Phase2CaptureStatus::UnsupportedEngineState,
							Phase2CaptureReason::UnsupportedShipBlock);
					discovery_node.capture_key =
						remap_capture_local(
							discovery_node.capture_key);
					if (discovery_node.support_capture_key.value != 0U) {
						discovery_node.support_capture_key =
							remap_capture_local(
								discovery_node.support_capture_key);
						if (discovery_node.support_capture_key.value == 0U) {
							return capture_failure(output,
								Phase2CaptureStatus::UnsupportedEngineState,
								Phase2CaptureReason::UnsupportedShipBlock);
						}
					}
					if (discovery_node.group_leader_capture_key.value != 0U) {
						discovery_node.group_leader_capture_key =
							remap_capture_local(
								discovery_node.group_leader_capture_key);
						if (discovery_node.group_leader_capture_key.value == 0U) {
							return capture_failure(output,
								Phase2CaptureStatus::UnsupportedEngineState,
								Phase2CaptureReason::UnsupportedShipBlock);
						}
					}
					for (std::size_t relation = 0U;
						 relation < discovery_node.direct_docking_count;
						 ++relation) {
						discovery_node
							.direct_docking_capture_keys[relation] =
							remap_capture_local(
								discovery_node
									.direct_docking_capture_keys[relation]);
						if (discovery_node
								.direct_docking_capture_keys[relation]
								.value == 0U) {
							return capture_failure(output,
								Phase2CaptureStatus::UnsupportedEngineState,
								Phase2CaptureReason::UnsupportedShipBlock);
						}
					}
					if (discovery_node.capture_key.value == 0U) {
						return capture_failure(output,
							Phase2CaptureStatus::UnsupportedEngineState,
							Phase2CaptureReason::UnsupportedShipBlock);
					}
					output.discovery_nodes[output.discovery_count++] =
						std::move(discovery_node);
				} else if (first_discovery_failure.status ==
					Phase2SourceReadStatus::Valid) {
					first_discovery_failure = discovery_result;
				}
			}
		}
		if(projection==Phase2ObservationProjection::CompleteShip) {
			for(std::uint32_t node_index=0U;node_index<output.discovery_count;++node_index) {
				const auto& node=output.discovery_nodes[node_index];
				for(std::uint32_t relation=0U;relation<node.direct_docking_count;++relation) {
					const auto remote=node.direct_docking_capture_keys[relation].value;
					bool inverse=false;
					for(std::uint32_t other_index=0U;other_index<output.discovery_count;++other_index) {
						const auto& other=output.discovery_nodes[other_index];
						if(other.capture_key.value!=remote)continue;
						for(std::uint32_t other_relation=0U;
							other_relation<other.direct_docking_count;++other_relation)
							if(other.direct_docking_capture_keys[other_relation].value==
									node.capture_key.value&&
								other.direct_docking_reciprocal[other_relation]&&
								other.direct_local_dockpoints[other_relation]==
									node.direct_remote_dockpoints[relation]&&
								other.direct_remote_dockpoints[other_relation]==
									node.direct_local_dockpoints[relation])inverse=true;
					}
					if(!node.direct_docking_reciprocal[relation]||!inverse)
						return capture_failure(output,
							Phase2CaptureStatus::UnsupportedEngineState,
							Phase2CaptureReason::UnsupportedShipBlock);
				}
			}
		}
		output.discovery_status = first_discovery_failure.status;
		output.discovery_capture.status =
			first_discovery_failure.status == Phase2SourceReadStatus::Valid
			? Phase2CaptureStatus::Valid
			: first_discovery_failure.status ==
					Phase2SourceReadStatus::SourceLimitExceeded
			? Phase2CaptureStatus::SourceLimitExceeded
			: first_discovery_failure.status ==
					Phase2SourceReadStatus::UnsupportedEngineState
			? Phase2CaptureStatus::UnsupportedEngineState
			: Phase2CaptureStatus::InvalidSource;
		output.discovery_capture.reason =
			first_discovery_failure.status == Phase2SourceReadStatus::Valid
			? Phase2CaptureReason::None
			: Phase2CaptureReason::ShipReadFailure;
	} catch (...) {
		return capture_failure(
			output, Phase2CaptureStatus::SourceLimitExceeded, Phase2CaptureReason::AllocationFailure);
	}

	PlayerControlObservation controls;
	PlayerCargoScanObservation cargo;
	if (!source.read_player_controls(controls) || !source.read_player_cargo_scan(cargo)) {
		return capture_failure(output,
			Phase2CaptureStatus::UnsupportedEngineState,
			Phase2CaptureReason::UnsupportedShipBlock);
	}
	controls.sample_time_us = producer_sample_time_us;
	cargo.sample_time_us = producer_sample_time_us;
	if (cargo.target_capture_key.value != 0U) {
		cargo.target_capture_key =
			remap_capture_local(cargo.target_capture_key);
		if (cargo.target_capture_key.value == 0U) {
			return capture_failure(output,
				Phase2CaptureStatus::UnsupportedEngineState,
				Phase2CaptureReason::UnsupportedShipBlock);
		}
	}
	if (!canonicalize_player_controls(controls) || !validate_player_cargo_scan(cargo)) {
		return capture_failure(output,
			Phase2CaptureStatus::UnsupportedEngineState,
			Phase2CaptureReason::UnsupportedShipBlock);
	}

	output.capture.status = Phase2CaptureStatus::Valid;
	output.capture.reason = Phase2CaptureReason::None;
	output.player_key = {1U};
	output.producer_sample_time_us = producer_sample_time_us;
	output.player_controls = controls;
	output.player_cargo_scan = cargo;
	return output.capture;
}

Phase2CaptureResult collect_phase2_observation(const Phase2EngineReadView& source,
	const Phase2ObservationSelection& selection,
	std::uint64_t producer_sample_time_us,
	Phase2ObservationDto& output,
	Phase2ObservationProjection projection) noexcept
{
	try {
		auto direct_capture_scratch = std::make_unique<Phase2ShipSource>();
		const auto core_result = collect_phase2_observation_with_scratch(source,
			selection,
			producer_sample_time_us,
			output,
			*direct_capture_scratch,
			projection);
		if (projection == Phase2ObservationProjection::CompleteShip &&
			core_result.status == Phase2CaptureStatus::Valid &&
			output.discovery_capture.status != Phase2CaptureStatus::Valid) {
			// CompleteShip is fail-closed across both the core and discovery
			// authorities. Preserve the collected discovery evidence, but make
			// its failure the observable top-level result as well.
			output.capture = output.discovery_capture;
			return output.capture;
		}
		return core_result;
	} catch (...) {
		return capture_failure(
			output, Phase2CaptureStatus::SourceLimitExceeded, Phase2CaptureReason::AllocationFailure);
	}
}

Phase2CaptureResult collect_phase2_observation(const Phase2EngineReadView& source,
	std::uint64_t producer_sample_time_us,
	Phase2ObservationDto& output) noexcept
{
	Phase2ObservationSelection selection;
	return collect_phase2_observation(source,
		selection,
		producer_sample_time_us,
		output,
		Phase2ObservationProjection::DiscoveryExtension);
}

Phase2CaptureResult collect_phase2_observation(const Phase2EngineReadView& source,
	std::uint64_t producer_sample_time_us,
	Phase2ObservationDto& output,
	Phase2ObservationProjection projection) noexcept
{
	Phase2ObservationSelection selection;
	const auto source_failure = [&](Phase2SourceReadStatus status) noexcept {
		const auto capture_status =
			status == Phase2SourceReadStatus::SourceLimitExceeded
			? Phase2CaptureStatus::SourceLimitExceeded
			: status == Phase2SourceReadStatus::UnsupportedEngineState
			? Phase2CaptureStatus::UnsupportedEngineState
			: Phase2CaptureStatus::InvalidSource;
		return capture_failure(
			output, capture_status, Phase2CaptureReason::ShipReadFailure);
	};
	if (projection == Phase2ObservationProjection::CompleteShip) {
		EngineEntityKey root{};
		const auto root_result = source.read_player_root_key(root);
		if (root_result.status != Phase2SourceReadStatus::Valid) {
			return source_failure(root_result.status);
		}
		Phase2DiscoveryNode root_node{};
		const auto root_discovery =
			source.read_discovery_node(root, root_node);
		const auto root_keys =
			root_discovery.status == Phase2SourceReadStatus::Valid
			? validate_discovery_keys(root_node)
			: root_discovery.status;
		if (root_keys != Phase2SourceReadStatus::Valid) {
			return source_failure(
				root_keys);
		}
		selection.ship_keys[selection.count++] = root_node.capture_key;
		for (std::size_t cursor = 0U; cursor < selection.count; ++cursor) {
			EngineEntityKey engine_key{};
			if (cursor == 0U) {
				engine_key = root;
			} else {
				const auto resolved = source.resolve_capture_local_key(
					selection.ship_keys[cursor], engine_key);
				if (resolved.status != Phase2SourceReadStatus::Valid) {
					return source_failure(resolved.status);
				}
			}
			Phase2DiscoveryNode node{};
			const auto read_result =
				source.read_discovery_node(engine_key, node);
			if (read_result.status != Phase2SourceReadStatus::Valid) {
				return source_failure(read_result.status);
			}
			const auto node_keys = validate_discovery_keys(node);
			if (node_keys != Phase2SourceReadStatus::Valid) {
				return source_failure(node_keys);
			}
			if (node.capture_key.value != selection.ship_keys[cursor].value) {
				return source_failure(
					Phase2SourceReadStatus::UnsupportedEngineState);
			}
			std::array<Phase2CaptureLocalKey,
				MaximumPhase2DockRelationsPerShip + 2U> related{};
			std::size_t related_count = 0U;
			related[related_count++] = node.support_capture_key;
			related[related_count++] = node.group_leader_capture_key;
			if (node.direct_docking_count >
				MaximumPhase2DockRelationsPerShip) {
				return capture_failure(output,
					Phase2CaptureStatus::SourceLimitExceeded,
					Phase2CaptureReason::InvalidClosureCardinality);
			}
			for (std::uint32_t index = 0U;
				 index < node.direct_docking_count;
				 ++index) {
				related[related_count++] =
					node.direct_docking_capture_keys[index];
			}
			for (std::size_t index = 0U; index < related_count; ++index) {
				if (related[index].value == 0U) {
					continue;
				}
				bool already_selected = false;
				for (std::size_t existing = 0U;
					 existing < selection.count;
					 ++existing) {
					if (selection.ship_keys[existing].value ==
						related[index].value) {
						already_selected = true;
						break;
					}
				}
				if (already_selected) {
					continue;
				}
				if (selection.count >= MaximumPhase2ObservationShips) {
					return capture_failure(output,
						Phase2CaptureStatus::SourceLimitExceeded,
						Phase2CaptureReason::InvalidClosureCardinality);
				}
				EngineEntityKey resolved{};
				const auto resolve_result =
					source.resolve_capture_local_key(related[index], resolved);
				if (resolve_result.status != Phase2SourceReadStatus::Valid) {
					return source_failure(resolve_result.status);
				}
				selection.ship_keys[selection.count++] = related[index];
			}
		}
	}
	return collect_phase2_observation(
		source,selection,producer_sample_time_us,output,projection);
}

bool Phase2ObservationBuffer::provision(Phase2ProvisioningMode mode) noexcept
{
	if (m_state != Phase2ObservationBufferState::Unprovisioned) {
		return false;
	}

	switch (mode) {
	case Phase2ProvisioningMode::ConfigurationAbsent:
	case Phase2ProvisioningMode::ValidDisabled:
		m_state = Phase2ObservationBufferState::Disabled;
		return true;
	case Phase2ProvisioningMode::ValidEnabled:
		try {
			m_observation.ships.reserve(MaximumPhase2ObservationShips);
			m_source_scratch = std::make_unique<Phase2ShipSource>();
		} catch (...) {
			m_source_scratch.reset();
			m_state = Phase2ObservationBufferState::FailedClosed;
			return false;
		}
		if (m_observation.ships.capacity() < MaximumPhase2ObservationShips ||
			m_source_scratch == nullptr) {
			m_source_scratch.reset();
			m_state = Phase2ObservationBufferState::FailedClosed;
			return false;
		}
		m_state = Phase2ObservationBufferState::Provisioned;
		return true;
	case Phase2ProvisioningMode::Count:
	default:
		m_state = Phase2ObservationBufferState::FailedClosed;
		return false;
	}
}

bool Phase2ObservationBuffer::enter_ready() noexcept
{
	if (m_state != Phase2ObservationBufferState::Provisioned) {
		return false;
	}
	m_state = Phase2ObservationBufferState::Ready;
	return true;
}

Phase2CaptureResult Phase2ObservationBuffer::capture(const Phase2EngineReadView& source,
	std::uint64_t producer_sample_time_us,
	Phase2ObservationProjection projection) noexcept
{
	if (m_state != Phase2ObservationBufferState::Ready) {
		return capture_failure(
			m_observation, Phase2CaptureStatus::InvalidSource, Phase2CaptureReason::BufferNotReady);
	}

	if (m_source_scratch == nullptr) {
		m_state = Phase2ObservationBufferState::FailedClosed;
		return capture_failure(
			m_observation, Phase2CaptureStatus::InvalidSource, Phase2CaptureReason::BufferNotReady);
	}
	const Phase2ObservationSelection selection;
	const auto core_result = collect_phase2_observation_with_scratch(
		source,
		selection,
		producer_sample_time_us,
		m_observation,
		*m_source_scratch,
		projection);
	const auto result =
		projection == Phase2ObservationProjection::CompleteShip &&
			core_result.status == Phase2CaptureStatus::Valid &&
			m_observation.discovery_capture.status != Phase2CaptureStatus::Valid
		? m_observation.discovery_capture
		: core_result;
	if (result.status == Phase2CaptureStatus::InvalidSource ||
		result.status == Phase2CaptureStatus::UnsupportedEngineState ||
		result.status == Phase2CaptureStatus::SourceLimitExceeded) {
		if (result.reason != Phase2CaptureReason::NotInMission) {
			m_state = Phase2ObservationBufferState::FailedClosed;
		}
	}
	return result;
}

Phase2CaptureResult collect_phase2_observation(Phase2ObservationBuffer& buffer,
	const Phase2EngineReadView& source,
	std::uint64_t producer_sample_time_us,
	Phase2ObservationProjection projection) noexcept
{
	return buffer.capture(source, producer_sample_time_us, projection);
}

Phase2ObservationBufferState Phase2ObservationBuffer::state() const noexcept
{
	return m_state;
}

std::size_t Phase2ObservationBuffer::ship_capacity() const noexcept
{
	return m_observation.ships.capacity();
}

std::size_t Phase2ObservationBuffer::owned_bytes() const noexcept
{
	if (m_observation.ships.capacity() == 0U && m_source_scratch == nullptr) {
		return 0U;
	}
	return sizeof(Phase2ObservationBuffer) +
		m_observation.ships.capacity() * sizeof(ShipObservationDto) +
		(m_source_scratch != nullptr ? sizeof(Phase2ShipSource) : 0U);
}

const Phase2ObservationDto& Phase2ObservationBuffer::observation() const noexcept
{
	return m_observation;
}

Phase2ObservationDto& Phase2ObservationBuffer::observation() noexcept
{
	return m_observation;
}

void Phase2ObservationBuffer::reset_observation_and_clear_phase2() noexcept
{
	reset_observation(m_observation);
}

bool canonicalize_phase2_float(float value, float absolute_limit, float& output) noexcept
{
	output = 0.0F;
	if (!std::isfinite(value) || !std::isfinite(absolute_limit) || absolute_limit < 0.0F ||
		std::fabs(value) > absolute_limit) {
		return false;
	}
	output = value == 0.0F ? 0.0F : value;
	return true;
}

bool phase2_timestamp_remaining_us(std::int64_t now_ms,
	std::int64_t deadline_ms,
	std::uint64_t maximum_remaining_us,
	std::uint64_t& remaining_us) noexcept
{
	remaining_us = 0U;
	if (deadline_ms <= now_ms) {
		return true;
	}

	const auto delta_ms = static_cast<std::uint64_t>(deadline_ms) - static_cast<std::uint64_t>(now_ms);
	if (delta_ms > std::numeric_limits<std::uint64_t>::max() / 1000U) {
		return false;
	}
	const auto candidate = delta_ms * 1000U;
	if (candidate > maximum_remaining_us) {
		return false;
	}
	remaining_us = candidate;
	return true;
}

Phase2SeamTestDouble* phase2_seam_test_double() noexcept
{
	return Phase2TestDouble.load(std::memory_order_acquire);
}

void set_phase2_seam_test_double_internal(Phase2SeamTestDouble* test_double) noexcept
{
	Phase2TestDouble.store(test_double, std::memory_order_release);
}

void reset_phase2_seam_handoff() noexcept
{
	Phase2Handoff = {};
}

void reset_phase2_mission_observation_state() noexcept
{
	Phase2Handoff = {};
	Phase2Handoff.has_control_target = true;
	Phase2Handoff.control_target = ControlTargetAuthority::Ship;
}

Phase2SeamHandoffSnapshot phase2_seam_handoff_snapshot() noexcept
{
	return Phase2Handoff;
}

void capture_phase2_main_thread_authority() noexcept
{
	Phase2MainThread = std::this_thread::get_id();
	Phase2MainThreadCaptured = true;
}

bool phase2_current_thread_is_main() noexcept
{
	return Phase2MainThreadCaptured && std::this_thread::get_id() == Phase2MainThread;
}

} // namespace telemetry::detail

namespace telemetry {

void OnShipCleanup(std::uint32_t object_signature, ShipCleanupMode mode) noexcept
{
	if (static_cast<std::uint8_t>(mode) >= static_cast<std::uint8_t>(ShipCleanupMode::Count)) {
		return;
	}
	detail::Phase2Handoff.has_ship_cleanup = true;
	detail::Phase2Handoff.ship_cleanup = {object_signature, mode};
	if (auto* test_double = detail::phase2_seam_test_double()) {
		test_double->on_ship_cleanup(detail::ShipCleanupFact{object_signature, mode});
	}
}

void OnSupportTransition(std::uint32_t assisted_signature,
	std::uint32_t support_signature,
	std::uint32_t episode_sequence,
	SupportTransitionReason reason,
	std::uint64_t sample_time) noexcept
{
	if (static_cast<std::uint8_t>(reason) >= static_cast<std::uint8_t>(SupportTransitionReason::Count)) {
		return;
	}
	detail::Phase2Handoff.has_support_transition = true;
	detail::Phase2Handoff.support_transition =
		{assisted_signature, support_signature, episode_sequence, reason, sample_time};
	if (auto* test_double = detail::phase2_seam_test_double()) {
		test_double->on_support_transition(
			detail::SupportTransitionFact{assisted_signature, support_signature, episode_sequence, reason, sample_time});
	}
}

void OnControlTarget(ControlTargetAuthority authority) noexcept
{
	if (static_cast<std::uint8_t>(authority) >= static_cast<std::uint8_t>(ControlTargetAuthority::Count)) {
		return;
	}
	detail::Phase2Handoff.has_control_target = true;
	detail::Phase2Handoff.control_target = authority;
	if (auto* test_double = detail::phase2_seam_test_double()) {
		test_double->on_control_target(authority);
	}
}

void OnCargoAuthority(const CargoAuthorityFact& fact) noexcept
{
	detail::Phase2Handoff.has_cargo_authority = true;
	detail::Phase2Handoff.cargo_authority = fact;
	if (auto* test_double = detail::phase2_seam_test_double()) {
		test_double->on_cargo_authority(fact);
	}
}

namespace test_seam {

void set_phase2_seam_test_double(detail::Phase2SeamTestDouble* test_double) noexcept
{
	detail::set_phase2_seam_test_double_internal(test_double);
}

} // namespace test_seam
} // namespace telemetry
