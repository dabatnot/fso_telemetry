#include "telemetry/phase4_catalog_assembly.h"

#include "telemetry/phase4_catalog_dependencies.h"
#include "telemetry/phase4_manifest_plan.h"

#include <memory>
#include <new>
#include <tuple>

namespace telemetry::detail {
namespace {

Phase4CatalogAssemblyStatus map_read_status(
	Phase4CatalogDefinitionReadStatus status) noexcept
{
	switch (status) {
	case Phase4CatalogDefinitionReadStatus::Read:
		return Phase4CatalogAssemblyStatus::Created;
	case Phase4CatalogDefinitionReadStatus::NotReady:
		return Phase4CatalogAssemblyStatus::NotReady;
	case Phase4CatalogDefinitionReadStatus::MissingDefinition:
		return Phase4CatalogAssemblyStatus::MissingDefinition;
	case Phase4CatalogDefinitionReadStatus::InvalidDefinition:
		return Phase4CatalogAssemblyStatus::InvalidDefinition;
	case Phase4CatalogDefinitionReadStatus::SourceLimitExceeded:
		return Phase4CatalogAssemblyStatus::SourceLimitExceeded;
	default:
		return Phase4CatalogAssemblyStatus::InvalidDefinition;
	}
}

bool same_weapon(const Phase2WeaponSource& left,
	const Phase2WeaponSource& right) noexcept
{
	return std::tie(left.source_key, left.name, left.title,
		left.damage_type_index, left.subtype, left.class_flags,
		left.max_speed, left.mass, left.gravity_constant,
		left.velocity_inherit_amount, left.turn_factor, left.lifetime_us,
		left.effect_flags, left.guidance_type, left.has_acceleration,
		left.has_ranges, left.has_fire, left.has_damage, left.has_guidance,
		left.has_lock, left.has_cargo_rearm, left.has_burst, left.has_swarm,
		left.acceleration_time_us, left.fire_wait_us, left.lock_time_us,
		left.rearm_time_us, left.burst_interval_us, left.minimum_range,
		left.optimal_range, left.maximum_range, left.energy_consumed,
		left.damage, left.guidance_fov_rad, left.lock_fov_rad,
		left.cargo_size, left.reloaded_per_batch, left.burst_count,
		left.swarm_count, left.shots_per_trigger) ==
		std::tie(right.source_key, right.name, right.title,
		right.damage_type_index, right.subtype, right.class_flags,
		right.max_speed, right.mass, right.gravity_constant,
		right.velocity_inherit_amount, right.turn_factor, right.lifetime_us,
		right.effect_flags, right.guidance_type, right.has_acceleration,
		right.has_ranges, right.has_fire, right.has_damage, right.has_guidance,
		right.has_lock, right.has_cargo_rearm, right.has_burst, right.has_swarm,
		right.acceleration_time_us, right.fire_wait_us, right.lock_time_us,
		right.rearm_time_us, right.burst_interval_us, right.minimum_range,
		right.optimal_range, right.maximum_range, right.energy_consumed,
		right.damage, right.guidance_fov_rad, right.lock_fov_rad,
		right.cargo_size, right.reloaded_per_batch, right.burst_count,
		right.swarm_count, right.shots_per_trigger);
}

bool same_vec3(const Phase2Vec3& left, const Phase2Vec3& right) noexcept
{
	return std::tie(left.x, left.y, left.z) ==
		std::tie(right.x, right.y, right.z);
}

bool same_subsystem(const Phase2SubsystemSource& left,
	const Phase2SubsystemSource& right) noexcept
{
	return std::tie(left.source_key, left.system_info_key, left.name,
		left.alt_name, left.hud_name, left.max_hits, left.current_hits,
		left.armor_index, left.type, left.radius, left.static_flags) ==
		std::tie(right.source_key, right.system_info_key, right.name,
		right.alt_name, right.hud_name, right.max_hits, right.current_hits,
		right.armor_index, right.type, right.radius, right.static_flags) &&
		same_vec3(left.local_position, right.local_position);
}

bool same_bank(const Phase2BankSource& left,
	const Phase2BankSource& right) noexcept
{
	if (std::tie(left.family, left.source_family, left.bank_index,
		left.owner_subsystem_canonical_index, left.weapon_source_key,
		left.firing_pattern_source_code, left.consumes_ammunition,
		left.capacity, left.fire_point_count) !=
		std::tie(right.family, right.source_family, right.bank_index,
		right.owner_subsystem_canonical_index, right.weapon_source_key,
		right.firing_pattern_source_code, right.consumes_ammunition,
		right.capacity, right.fire_point_count)) return false;
	if (left.fire_point_count > Phase2ManifestLimits::MaxFirePoints) return false;
	for (std::uint32_t index = 0U; index < left.fire_point_count; ++index)
		if (!same_vec3(left.fire_points[index], right.fire_points[index]))
			return false;
	return true;
}

bool same_ship_class(const Phase2ClassSource& left,
	const Phase2ClassSource& right) noexcept
{
	if (std::tie(left.name, left.model_mass,
		left.density_provenance, left.effective_mass, left.model_inertia,
		left.effective_inertia, left.full_angle_degrees_provenance,
		left.half_angle_cosines, left.effective_inertia_matrix,
		left.max_rear_velocity, left.forward_accel_time,
		left.afterburner_forward_accel_time, left.booster_forward_accel_time,
		left.forward_decel_time, left.slide_accel_time, left.slide_decel_time,
		left.max_hull_strength, left.max_shield_strength,
		left.has_afterburner, left.afterburner_fuel_capacity,
		left.afterburner_burn_rate, left.afterburner_recover_rate,
		left.afterburner_min_start_fuel, left.afterburner_cooldown_us,
		left.has_scan, left.has_glide, left.has_autoaim,
		left.scan_required_time_us, left.scan_max_distance,
		left.scan_max_angle_rad, left.glide_cap, left.autoaim_fov_rad,
		left.species_index, left.ship_type_index, left.iff_index,
		left.wing_index, left.armor_index, left.damage_type_index,
		left.required_model_index, left.subsystem_count, left.bank_count,
		left.countermeasure_capacity, left.countermeasure_cargo_size,
		left.countermeasure_uses_capacity,
		left.countermeasure_weapon_source_key,
		left.countermeasure_firewait_ms) !=
		std::tie(right.name, right.model_mass,
		right.density_provenance, right.effective_mass, right.model_inertia,
		right.effective_inertia, right.full_angle_degrees_provenance,
		right.half_angle_cosines, right.effective_inertia_matrix,
		right.max_rear_velocity, right.forward_accel_time,
		right.afterburner_forward_accel_time, right.booster_forward_accel_time,
		right.forward_decel_time, right.slide_accel_time, right.slide_decel_time,
		right.max_hull_strength, right.max_shield_strength,
		right.has_afterburner, right.afterburner_fuel_capacity,
		right.afterburner_burn_rate, right.afterburner_recover_rate,
		right.afterburner_min_start_fuel, right.afterburner_cooldown_us,
		right.has_scan, right.has_glide, right.has_autoaim,
		right.scan_required_time_us, right.scan_max_distance,
		right.scan_max_angle_rad, right.glide_cap, right.autoaim_fov_rad,
		right.species_index, right.ship_type_index, right.iff_index,
		right.wing_index, right.armor_index, right.damage_type_index,
		right.required_model_index, right.subsystem_count, right.bank_count,
		right.countermeasure_capacity, right.countermeasure_cargo_size,
		right.countermeasure_uses_capacity,
		right.countermeasure_weapon_source_key,
		right.countermeasure_firewait_ms)) return false;
	if (!same_vec3(left.center_of_mass, right.center_of_mass) ||
		!same_vec3(left.max_velocity, right.max_velocity) ||
		!same_vec3(left.afterburner_max_velocity,
			right.afterburner_max_velocity) ||
		!same_vec3(left.booster_max_velocity, right.booster_max_velocity) ||
		!same_vec3(left.max_rotational_velocity,
			right.max_rotational_velocity) ||
		left.subsystem_count > Phase2ManifestLimits::MaxSubsystemsPerShip ||
		left.bank_count > Phase2ManifestLimits::MaxBanksPerClass)
		return false;
	for (std::uint32_t index = 0U; index < left.subsystem_count; ++index)
		if (!same_subsystem(left.subsystems[index], right.subsystems[index]))
			return false;
	for (std::uint32_t index = 0U; index < left.bank_count; ++index)
		if (!same_bank(left.banks[index], right.banks[index])) return false;
	return true;
}

Phase4CatalogAssemblyStatus merge_auxiliary(
	const Phase2ManifestSource& fragment,
	Phase2ManifestSource& available) noexcept
{
	if (fragment.auxiliary_entry_count > Phase2ManifestLimits::MaxAuxiliaryEntries)
		return Phase4CatalogAssemblyStatus::SourceLimitExceeded;
	for (std::uint32_t source_index = 0U;
		 source_index < fragment.auxiliary_entry_count; ++source_index) {
		const auto& source = fragment.auxiliary_entries[source_index];
		if (source.registry > AuxiliaryRegistry::Pattern ||
			source.engine_index < 0 || source.name.size() > 255U)
			return Phase4CatalogAssemblyStatus::InvalidDefinition;
		bool found = false;
		for (std::uint32_t index = 0U;
			 index < available.auxiliary_entry_count; ++index) {
			const auto& existing = available.auxiliary_entries[index];
			if (existing.registry == source.registry &&
				existing.engine_index == source.engine_index) {
				if (existing.name != source.name)
					return Phase4CatalogAssemblyStatus::AmbiguousDefinition;
				found = true;
				break;
			}
			if (existing.registry == source.registry &&
				existing.name == source.name &&
				existing.engine_index != source.engine_index)
				return Phase4CatalogAssemblyStatus::AmbiguousDefinition;
		}
		if (found) continue;
		if (available.auxiliary_entry_count ==
			Phase2ManifestLimits::MaxAuxiliaryEntries)
			return Phase4CatalogAssemblyStatus::SourceLimitExceeded;
		available.auxiliary_entries[available.auxiliary_entry_count++] = source;
	}
	return Phase4CatalogAssemblyStatus::Created;
}

Phase4CatalogAssemblyStatus merge_weapons(
	const Phase2ManifestSource& fragment,
	Phase2ManifestSource& available) noexcept
{
	if (fragment.weapon_count > Phase2ManifestLimits::MaxWeapons)
		return Phase4CatalogAssemblyStatus::SourceLimitExceeded;
	for (std::uint32_t source_index = 0U;
		 source_index < fragment.weapon_count; ++source_index) {
		const auto& source = fragment.weapons[source_index];
		if (source.source_key == 0U)
			return Phase4CatalogAssemblyStatus::InvalidDefinition;
		const Phase2WeaponSource* existing = nullptr;
		for (std::uint32_t index = 0U; index < available.weapon_count; ++index)
			if (available.weapons[index].source_key == source.source_key) {
				existing = &available.weapons[index];
				break;
			}
		if (existing != nullptr) {
			if (!same_weapon(*existing, source))
				return Phase4CatalogAssemblyStatus::AmbiguousDefinition;
			continue;
		}
		if (available.weapon_count == Phase2ManifestLimits::MaxWeapons)
			return Phase4CatalogAssemblyStatus::SourceLimitExceeded;
		available.weapons[available.weapon_count++] = source;
	}
	return merge_auxiliary(fragment, available);
}

void clear_manifest_source_logical(Phase2ManifestSource& source) noexcept
{
	source.ship_class_count = 0U;
	source.weapon_count = 0U;
	source.referenced_ship_class_count = 0U;
	source.referenced_weapon_count = 0U;
	source.auxiliary_entry_count = 0U;
	source.metadata = {};
	source.player_instance_signature = 0U;
	source.engine_index = 0U;
	source.manifest_generation = 0U;
	source.topology_fingerprint = {};
	source.referenced_ship_class_keys.fill(0U);
	source.referenced_weapon_keys.fill(0U);
}

bool reserve_manifest_source_strings(Phase2ManifestSource& source,
	bool fragment) noexcept
{
	try {
		const auto class_count = fragment ? 1U : Phase2ManifestLimits::MaxClasses;
		for (std::uint32_t index = 0U; index < class_count; ++index) {
			auto& ship_class = source.ship_classes[index];
			ship_class.name.reserve(255U);
			for (auto& subsystem : ship_class.subsystems) {
				subsystem.name.reserve(255U);
				subsystem.alt_name.reserve(255U);
				subsystem.hud_name.reserve(255U);
			}
		}
		for (auto& weapon : source.weapons) {
			weapon.name.reserve(255U);
			weapon.title.reserve(255U);
		}
		for (auto& auxiliary : source.auxiliary_entries)
			auxiliary.name.reserve(255U);
		return true;
	} catch (const std::bad_alloc&) {
		return false;
	}
}

std::size_t manifest_source_backing_bytes(
	const Phase2ManifestSource& source) noexcept
{
	std::size_t total = sizeof(source);
	for (const auto& ship_class : source.ship_classes) {
		total += ship_class.name.capacity();
		for (const auto& subsystem : ship_class.subsystems)
			total += subsystem.name.capacity() + subsystem.alt_name.capacity() +
				subsystem.hud_name.capacity();
	}
	for (const auto& weapon : source.weapons)
		total += weapon.name.capacity() + weapon.title.capacity();
	for (const auto& auxiliary : source.auxiliary_entries)
		total += auxiliary.name.capacity();
	return total;
}

Phase4CatalogAssemblyStatus assemble_into(
	const Phase4CatalogDefinitionReadView& reader,
	std::vector<Phase4EngineInventoryEntry>& inventory,
	Phase2ManifestSource& available,
	Phase2ManifestSource& fragment) noexcept
{
	clear_manifest_source_logical(available);
	try {
		// Read SHIPs in stable engine-signature order. The resulting source keys
		// name effective static configurations, not a raw ship_info_index.
		std::uint64_t previous_ship_identity = 0U;
		for (;;) {
			Phase4EngineInventoryEntry* entry = nullptr;
			for (auto& candidate : inventory) {
				if (candidate.identity.object_type != protocol::ObjectType::Ship ||
					candidate.identity.stable_identity == 0U ||
					candidate.identity.stable_identity <= previous_ship_identity) continue;
				if (entry == nullptr || candidate.identity.stable_identity <
					entry->identity.stable_identity)
					entry = &candidate;
			}
			if (entry == nullptr) break;
			previous_ship_identity = entry->identity.stable_identity;
			if (entry->source_class_key == 0U)
				return Phase4CatalogAssemblyStatus::InvalidInventory;
			clear_manifest_source_logical(fragment);
			const auto read = reader.read_ship_definition(*entry, fragment);
			if (read != Phase4CatalogDefinitionReadStatus::Read)
				return map_read_status(read);
			if (fragment.ship_class_count != 1U ||
				fragment.ship_classes[0].source_key == 0U)
				return Phase4CatalogAssemblyStatus::InvalidDefinition;
			std::uint32_t effective_key = 0U;
			for (std::uint32_t index = 0U; index < available.ship_class_count; ++index)
				if (same_ship_class(available.ship_classes[index],
					fragment.ship_classes[0])) {
					effective_key = available.ship_classes[index].source_key;
					break;
				}
			if (effective_key == 0U) {
				if (available.ship_class_count == Phase2ManifestLimits::MaxClasses)
					return Phase4CatalogAssemblyStatus::SourceLimitExceeded;
				effective_key = available.ship_class_count + 1U;
				fragment.ship_classes[0].source_key = effective_key;
				const auto merged = merge_weapons(fragment, available);
				if (merged != Phase4CatalogAssemblyStatus::Created) return merged;
				available.ship_classes[available.ship_class_count++] =
					fragment.ship_classes[0];
			}
			entry->source_class_key = effective_key;
		}
		Phase4CatalogDependencies dependencies;
		const auto dependency_status =
			discover_phase4_catalog_dependencies(inventory, dependencies);
		if (dependency_status == Phase4CatalogDependencyStatus::InvalidInventory)
			return Phase4CatalogAssemblyStatus::InvalidInventory;
		if (dependency_status != Phase4CatalogDependencyStatus::Collected)
			return Phase4CatalogAssemblyStatus::SourceLimitExceeded;

		for (std::uint32_t dependency = 0U;
			 dependency < dependencies.weapon_count; ++dependency) {
			const auto source_key = dependencies.weapon_source_keys[dependency];
			bool already_available = false;
			for (std::uint32_t index = 0U; index < available.weapon_count; ++index)
				if (available.weapons[index].source_key == source_key) {
					already_available = true;
					break;
				}
			if (already_available) continue;
			clear_manifest_source_logical(fragment);
			const auto read = reader.read_weapon_definition(source_key, fragment);
			if (read != Phase4CatalogDefinitionReadStatus::Read)
				return map_read_status(read);
			if (fragment.ship_class_count != 0U ||
				fragment.weapon_count != 1U ||
				fragment.weapons[0].source_key != source_key)
				return Phase4CatalogAssemblyStatus::InvalidDefinition;
			const auto merged = merge_weapons(fragment, available);
			if (merged != Phase4CatalogAssemblyStatus::Created) return merged;
		}
		const auto prepared =
			prepare_phase4_manifest_source_in_place(available, dependencies);
		switch (prepared) {
		case Phase4ManifestSourceStatus::Created:
			return Phase4CatalogAssemblyStatus::Created;
		case Phase4ManifestSourceStatus::MissingDefinition:
			return Phase4CatalogAssemblyStatus::MissingDefinition;
		case Phase4ManifestSourceStatus::DuplicateDefinition:
			return Phase4CatalogAssemblyStatus::AmbiguousDefinition;
		default:
			return Phase4CatalogAssemblyStatus::InvalidDefinition;
		}
	} catch (const std::bad_alloc&) {
		return Phase4CatalogAssemblyStatus::AllocationFailure;
	}
}

} // namespace

bool Phase4CatalogAssemblyWorkspace::provision() noexcept
{
	reset();
	m_available.reset(new (std::nothrow) Phase2ManifestSource());
	m_fragment.reset(new (std::nothrow) Phase2ManifestSource());
	if (!m_available || !m_fragment ||
		!reserve_manifest_source_strings(*m_available, false) ||
		!reserve_manifest_source_strings(*m_fragment, true)) {
		reset();
		return false;
	}
	reset_cycle();
	return true;
}

void Phase4CatalogAssemblyWorkspace::reset() noexcept
{
	m_available.reset();
	m_fragment.reset();
}

void Phase4CatalogAssemblyWorkspace::reset_cycle() noexcept
{
	if (m_available) clear_manifest_source_logical(*m_available);
	if (m_fragment) clear_manifest_source_logical(*m_fragment);
}

bool Phase4CatalogAssemblyWorkspace::ready() const noexcept
{
	return m_available != nullptr && m_fragment != nullptr;
}

std::size_t Phase4CatalogAssemblyWorkspace::owned_backing_bytes() const noexcept
{
	return (m_available ? manifest_source_backing_bytes(*m_available) : 0U) +
		(m_fragment ? manifest_source_backing_bytes(*m_fragment) : 0U);
}

Phase4CatalogAssemblyStatus assemble_phase4_catalog_definitions_preallocated(
	const Phase4CatalogDefinitionReadView& reader,
	std::vector<Phase4EngineInventoryEntry>& inventory,
	Phase4CatalogAssemblyWorkspace& workspace,
	const Phase2ManifestSource*& output) noexcept
{
	output = nullptr;
	if (!workspace.ready())
		return Phase4CatalogAssemblyStatus::NotReady;
	workspace.reset_cycle();
	const auto status = assemble_into(reader, inventory,
		*workspace.m_available, *workspace.m_fragment);
	if (status == Phase4CatalogAssemblyStatus::Created)
		output = workspace.m_available.get();
	return status;
}

} // namespace telemetry::detail
