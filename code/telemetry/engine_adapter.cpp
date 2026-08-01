#include "telemetry/engine_adapter.h"

#include "ai/ai.h"
#include "autopilot/autopilot.h"
#include "cmeasure/cmeasure.h"
#include "controlconfig/controlsconfig.h"
#include "globalincs/systemvars.h"
#include "hud/hudets.h"
#include "iff_defs/iff_defs.h"
#include "io/timer.h"
#include "mission/missionparse.h"
#include "model/model.h"
#include "mod_table/mod_table.h"
#include "object/object.h"
#include "object/objectdock.h"
#include "object/objectshield.h"
#include "physics/physics.h"
#include "playerman/player.h"
#include "ship/ship.h"
#include "telemetry/protocol/telemetry_protocol_constants.h"
#include "weapon/weapon.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>

extern int Game_skill_level;

namespace telemetry::detail {
namespace {

constexpr double MinimumBasisSquaredNorm = 1.0e-24;
constexpr double MinimumRelativeDeterminant = 1.0e-8;
constexpr double MinimumQuaternionSquaredNorm = 1.0e-24;
constexpr double QuantizedNormMinimum = 0.9999;
constexpr double QuantizedNormMaximum = 1.0001;

thread_local Phase2CaptureDiagnostics* ActivePhase2CaptureDiagnostics = nullptr;

void add_phase2_block_duration(Phase2CaptureDiagnostics& diagnostics,
	Phase2CaptureBlock block,
	std::uint64_t duration_ns) noexcept
{
	const auto index = static_cast<std::size_t>(block);
	diagnostics.attempted_mask |= static_cast<std::uint8_t>(1U << index);
	const auto previous = diagnostics.duration_ns[index];
	if (duration_ns > std::numeric_limits<std::uint64_t>::max() - previous) {
		diagnostics.duration_ns[index] = std::numeric_limits<std::uint64_t>::max();
		diagnostics.duration_overflow = true;
	} else {
		diagnostics.duration_ns[index] = previous + duration_ns;
	}
}

class Phase2ReadDiagnosticCursor {
  public:
	explicit Phase2ReadDiagnosticCursor(Phase2CaptureBlock block) noexcept
		: m_diagnostics(ActivePhase2CaptureDiagnostics)
	{
		begin(block);
	}
	~Phase2ReadDiagnosticCursor() { finish(); }
	void begin(Phase2CaptureBlock block) noexcept
	{
		finish();
		m_block = block;
		if (m_diagnostics != nullptr) {
			m_diagnostics->primary_failed_block = block;
			m_started = std::chrono::steady_clock::now();
		}
	}
  private:
	void finish() noexcept
	{
		if (m_diagnostics == nullptr ||
			m_block == Phase2CaptureBlock::Count)
			return;
		const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - m_started).count();
		add_phase2_block_duration(*m_diagnostics, m_block,
			elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0U);
		m_block = Phase2CaptureBlock::Count;
	}
	Phase2CaptureDiagnostics* m_diagnostics = nullptr;
	Phase2CaptureBlock m_block = Phase2CaptureBlock::Count;
	std::chrono::steady_clock::time_point m_started{};
};

static_assert(MaximumPhase2PhysicalPrimaryBanks == MAX_SHIP_PRIMARY_BANKS,
	"Phase 2 turret primary-bank storage must match the engine authority");
static_assert(MaximumPhase2PhysicalSecondaryBanks == MAX_SHIP_SECONDARY_BANKS,
	"Phase 2 turret secondary-bank storage must match the engine authority");

Phase2RawVec3 raw_vec3(const vec3d& value) noexcept
{
	return {value.xyz.x, value.xyz.y, value.xyz.z};
}

bool assign_bounded_engine_name(
	OwnedPhase2CatalogString& output, const char* value, std::size_t bound) noexcept
{
	if (value == nullptr) {
		return false;
	}
	std::size_t size = 0U;
	while (size < bound && value[size] != '\0') {
		++size;
	}
	return size < bound && output.assign(std::string_view{value, size});
}

bool fill_raw_weapon_definition(const weapon_info& source,
	std::uint32_t capture_key,
	Phase2RawWeaponDefinition& target) noexcept
{
	target = {};
	target.weapon_capture_key = capture_key;
	if (!assign_bounded_engine_name(target.internal_name, source.name, NAME_LENGTH) ||
		(source.has_display_name() && source.display_name.size() > 255U) ||
		(source.has_display_name() && !source.display_name.empty() &&
			!target.title.assign(std::string_view{
				source.display_name.data(), source.display_name.size()})) ||
		source.subtype < 0 || source.subtype > 255) {
		return false;
	}
	target.weapon_subtype_source = static_cast<std::uint8_t>(source.subtype);
	if (source.wi_flags[Weapon::Info_Flags::Bomb])
		target.raw_class_flags |= protocol::WeaponClassFlagBomb;
	if (source.wi_flags[Weapon::Info_Flags::Ballistic])
		target.raw_class_flags |= protocol::WeaponClassFlagBallistic;
	if (source.wi_flags[Weapon::Info_Flags::SecondaryNoAmmo])
		target.raw_class_flags |= protocol::WeaponClassFlagAmmoless;
	if (source.is_beam())
		target.raw_class_flags |= protocol::WeaponClassFlagBeam;
	if (source.wi_flags[Weapon::Info_Flags::Swarm])
		target.raw_class_flags |= protocol::WeaponClassFlagSwarm;
	if (source.wi_flags[Weapon::Info_Flags::Cmeasure])
		target.raw_class_flags |= protocol::WeaponClassFlagCountermeasure;
	if (source.is_homing())
		target.raw_class_flags |= protocol::WeaponClassFlagHoming;
	if (source.wi_flags[Weapon::Info_Flags::Remote])
		target.raw_class_flags |= protocol::WeaponClassFlagRemoteDetonatable;
	target.max_speed = source.max_speed;
	target.mass = source.mass;
	target.gravity_constant = source.gravity_const;
	target.velocity_inherit_amount = source.vel_inherit_amount;
	target.lifetime_seconds = source.lifetime;
	target.acceleration_time_seconds = source.acceleration_time;
	target.minimum_range = source.weapon_min_range;
	target.optimal_range = source.optimum_range;
	target.maximum_range = source.weapon_range;
	target.fire_wait_seconds = source.fire_wait;
	target.energy_consumed = source.energy_consumed;
	target.damage = source.damage;
	target.shockwave_outer_radius = source.shockwave.outer_rad;
	if (source.shockwave.outer_rad > 0.0F)
		target.raw_effect_flags |= protocol::WeaponEffectFlagShockwave;
	if (source.wi_flags[Weapon::Info_Flags::Emp])
		target.raw_effect_flags |= protocol::WeaponEffectFlagEmp;
	if (source.wi_flags[Weapon::Info_Flags::Tag])
		target.raw_effect_flags |= protocol::WeaponEffectFlagTag;
	if (source.wi_flags[Weapon::Info_Flags::Pierce_shields])
		target.raw_effect_flags |= protocol::WeaponEffectFlagShieldPiercing;
	if (source.wi_flags[Weapon::Info_Flags::Spawn])
		target.raw_effect_flags |= protocol::WeaponEffectFlagSpawnsChildren;
	if (source.wi_flags[Weapon::Info_Flags::Swarm]) {
		target.guidance_type_source = 1U;
	} else if (source.wi_flags[Weapon::Info_Flags::Homing_heat]) {
		target.guidance_type_source = 2U;
	} else if (source.wi_flags[Weapon::Info_Flags::Homing_aspect,
				   Weapon::Info_Flags::Homing_javelin]) {
		target.guidance_type_source = 3U;
	}
	target.guidance_fov_source_cosine = source.fov;
	target.lock_time_seconds = source.min_lock_time;
	target.lock_fov_source_cosine = source.lock_fov;
	target.cargo_size = source.cargo_size;
	target.rearm_rate_seconds = source.rearm_rate;
	target.reloaded_per_batch =
		source.reloaded_per_batch < 0
		? 0U
		: static_cast<std::uint32_t>(source.reloaded_per_batch);
	target.burst_shots = source.burst_shots;
	target.burst_delay_seconds = source.burst_delay;
	target.swarm_count_source = source.swarm_count;
	target.shots_source = source.shots;
	return true;
}

SourceReadResult extract_production_static_authorities(const object& ship_object,
	const ship& ship_instance,
	const ship_info& ship_class,
	Phase2ShipSource& output,
	bool core_gate) noexcept
{
	output.raw_static_references = {};
	auto& input = output.static_authority_input;
	reset_phase2_static_authority_input(input);
	std::array<int, MaximumPhase2StaticWeapons> referenced_weapon_indices{};
	std::uint32_t referenced_weapon_count = 0U;
	std::array<std::uint32_t, MaximumPhase2SubsystemsPerShip>
		instance_subsystem_armor_capture_keys{};
	const auto collected = [&]() noexcept -> SourceReadResult {
	if (ship_class.model_num < 0 || ship_class.n_subsystems < 0 ||
		ship_class.n_subsystems >
			static_cast<int>(MaximumPhase2SubsystemsPerShip) ||
		(!core_gate &&
		 (ship_class.num_primary_banks < 0 ||
		  ship_class.num_primary_banks > MAX_SHIP_PRIMARY_BANKS ||
		  ship_class.num_secondary_banks < 0 ||
		  ship_class.num_secondary_banks >
			  MAX_SHIP_SECONDARY_BANKS))) {
		return {Phase2SourceReadStatus::UnsupportedEngineState};
	}
	auto* model = model_get(ship_class.model_num);
	if (model == nullptr || model->id != ship_class.model_num ||
		(!core_gate &&
		 (model->n_guns != ship_class.num_primary_banks ||
		  model->n_missiles != ship_class.num_secondary_banks ||
		  (model->n_guns > 0 && model->gun_banks == nullptr) ||
		  (model->n_missiles > 0 &&
			  model->missile_banks == nullptr))) ||
		(ship_class.n_subsystems > 0 && ship_class.subsystems == nullptr)) {
		return {Phase2SourceReadStatus::UnsupportedEngineState};
	}

	const auto capture_weapon = [&](int engine_index,
									std::uint32_t& capture_key) noexcept {
		if (engine_index < 0 ||
			engine_index >= static_cast<int>(Weapon_info.size())) {
			return false;
		}
		for (std::uint32_t index = 0U; index < referenced_weapon_count; ++index) {
			if (referenced_weapon_indices[index] == engine_index) {
				capture_key = index + 1U;
				return true;
			}
		}
		if (referenced_weapon_count >= MaximumPhase2StaticWeapons) {
			return false;
		}
		referenced_weapon_indices[referenced_weapon_count] = engine_index;
		capture_key = ++referenced_weapon_count;
		return true;
	};
	input.guards_valid = true;
	input.ship_info.effective_mass = ship_object.phys_info.mass;
	input.ship_info.max_rear_velocity = ship_class.max_rear_vel;
	input.model.center_of_mass = raw_vec3(model->center_of_mass);
	input.subsystem_count =
		static_cast<std::uint32_t>(ship_class.n_subsystems);
	for (std::uint32_t index = 0U; index < input.subsystem_count; ++index) {
		const auto& source = ship_class.subsystems[index];
		auto& target = input.subsystems[index];
		target = {};
		target.subsystem_capture_key = index + 1U;
		if (!assign_bounded_engine_name(
				target.internal_name, source.name, MAX_NAME_LEN)) {
			return {Phase2SourceReadStatus::SourceLimitExceeded};
		}
		if (source.alt_sub_name[0] != '\0' &&
			!assign_bounded_engine_name(
				target.alt_name, source.alt_sub_name, NAME_LENGTH)) {
			return {Phase2SourceReadStatus::SourceLimitExceeded};
		}
		if (source.alt_dmg_sub_name[0] != '\0' &&
			!assign_bounded_engine_name(
				target.hud_name, source.alt_dmg_sub_name, NAME_LENGTH)) {
			return {Phase2SourceReadStatus::SourceLimitExceeded};
		}
		target.local_position = raw_vec3(source.pnt);
		target.radius = source.radius;
		target.max_hits = source.max_subsys_strength;
		target.subsystem_type_source =
			static_cast<std::uint8_t>(source.type);
		if (!source.flags[Model::Subsystem_Flags::Untargetable])
			target.raw_static_flags |=
				protocol::ClassSubsystemStaticFlagTargetable;
		if (source.scan_time > 0)
			target.raw_static_flags |=
				protocol::ClassSubsystemStaticFlagScannableCargo;
		if (source.flags[Model::Subsystem_Flags::Rotates,
				Model::Subsystem_Flags::Stepped_rotate,
				Model::Subsystem_Flags::Ai_rotate,
				Model::Subsystem_Flags::Triggered])
			target.raw_static_flags |=
				protocol::ClassSubsystemStaticFlagRotates;
		if (source.flags[Model::Subsystem_Flags::Translates,
				Model::Subsystem_Flags::Stepped_translate])
			target.raw_static_flags |=
				protocol::ClassSubsystemStaticFlagTranslates;
		if (source.type == SUBSYSTEM_TURRET)
			target.raw_static_flags |=
				protocol::ClassSubsystemStaticFlagTurret;
		if (source.flags[Model::Subsystem_Flags::Awacs])
			target.raw_static_flags |=
				protocol::ClassSubsystemStaticFlagAwacs;
	}

	input.bank_count = core_gate ? 0U : static_cast<std::uint32_t>(
		ship_class.num_primary_banks + ship_class.num_secondary_banks);
	for (std::uint32_t index = 0U; index < input.bank_count; ++index) {
		const auto primary =
			index < static_cast<std::uint32_t>(ship_class.num_primary_banks);
		const auto family_index = primary
			? index
			: index - static_cast<std::uint32_t>(ship_class.num_primary_banks);
		const auto& source = primary ? model->gun_banks[family_index]
									: model->missile_banks[family_index];
		if (source.num_slots <= 0 ||
			source.num_slots >
				static_cast<int>(MaximumPhase2StaticFirePoints) ||
			(source.num_slots > 0 && source.pnt == nullptr)) {
			return {source.num_slots >
						static_cast<int>(MaximumPhase2StaticFirePoints)
					? Phase2SourceReadStatus::SourceLimitExceeded
					: Phase2SourceReadStatus::UnsupportedEngineState};
		}
		auto& target = input.banks[index];
		target = {};
		target.num_slots =
			static_cast<std::uint32_t>(source.num_slots);
		target.bank_capture_key = index + 1U;
		target.family_source = primary ? 1U : 2U;
		target.bank_index = family_index;
		const auto weapon_index = primary
			? ship_instance.weapons.primary_bank_weapons[family_index]
			: ship_instance.weapons.secondary_bank_weapons[family_index];
		if (weapon_index < 0 ||
			weapon_index >= static_cast<int>(Weapon_info.size())) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
		const auto& bank_weapon = Weapon_info[weapon_index];
		target.consumes_ammunition = primary
			? bank_weapon.wi_flags[Weapon::Info_Flags::Ballistic]
			: !bank_weapon.wi_flags[Weapon::Info_Flags::SecondaryNoAmmo];
		target.capacity = target.consumes_ammunition
			? static_cast<float>(primary
				  ? ship_instance.weapons
						.primary_bank_capacity[family_index]
				  : ship_instance.weapons
						.secondary_bank_capacity[family_index])
			: 0.0F;
		target.has_capacity = target.consumes_ammunition;
		if (!capture_weapon(weapon_index, target.weapon_capture_key)) {
			return {Phase2SourceReadStatus::SourceLimitExceeded};
		}
		target.fire_point_count =
			static_cast<std::uint32_t>(source.num_slots);
		for (std::uint32_t point = 0U;
			 point < target.fire_point_count;
			 ++point) {
			target.fire_points[point] = raw_vec3(source.pnt[point]);
		}
		if (referenced_weapon_count == 1U) {
			input.weapon_info.mass = Weapon_info[weapon_index].mass;
			input.weapon_info.damage = Weapon_info[weapon_index].damage;
			input.weapon_info.fire_wait_seconds =
				Weapon_info[weapon_index].fire_wait;
		}
	}
	const auto tertiary_count = core_gate
		? 0 : ship_instance.weapons.num_tertiary_banks;
	if (tertiary_count < 0 ||
		tertiary_count >
			static_cast<int>(MaximumPhase2WeaponBanksPerFamily) ||
		input.bank_count + static_cast<std::uint32_t>(tertiary_count) >
			MaximumPhase2StaticBanksPerClass) {
		return {tertiary_count >
					static_cast<int>(MaximumPhase2WeaponBanksPerFamily)
				? Phase2SourceReadStatus::SourceLimitExceeded
				: Phase2SourceReadStatus::UnsupportedEngineState};
	}
	for (int tertiary = 0; tertiary < tertiary_count; ++tertiary) {
		auto& target = input.banks[input.bank_count++];
		target = {};
		target.num_slots = 1U;
		target.bank_capture_key = input.bank_count;
		target.family_source = 3U;
		target.bank_index = static_cast<std::uint16_t>(tertiary);
		target.has_capacity = ship_instance.weapons.tertiary_bank_capacity > 0;
		target.consumes_ammunition = target.has_capacity;
		target.capacity = static_cast<float>(
			ship_instance.weapons.tertiary_bank_capacity);
	}
	std::uint32_t turret_bank_count = 0U;
	for (std::uint32_t subsystem_index = 0U;
		 !core_gate &&
		 subsystem_index < input.subsystem_count;
		 ++subsystem_index) {
		const auto& subsystem_definition =
			ship_class.subsystems[subsystem_index];
		if (subsystem_definition.type != SUBSYSTEM_TURRET) {
			continue;
		}
		if (ship_instance.subsys_list_indexer == nullptr) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
		auto* subsystem_instance =
			ship_instance.subsys_list_indexer[subsystem_index];
		if (subsystem_instance == nullptr ||
			subsystem_instance->system_info != &subsystem_definition) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
		const auto& turret_weapons = subsystem_instance->weapons;
		if (turret_weapons.num_primary_banks < 0 ||
			turret_weapons.num_primary_banks > MAX_SHIP_PRIMARY_BANKS ||
			turret_weapons.num_secondary_banks < 0 ||
			turret_weapons.num_secondary_banks > MAX_SHIP_SECONDARY_BANKS ||
			subsystem_definition.turret_num_firing_points < 0 ||
			subsystem_definition.turret_num_firing_points >
				static_cast<int>(MaximumPhase2StaticFirePoints)) {
			return {subsystem_definition.turret_num_firing_points >
						static_cast<int>(MaximumPhase2StaticFirePoints)
					? Phase2SourceReadStatus::SourceLimitExceeded
					: Phase2SourceReadStatus::UnsupportedEngineState};
		}
		const auto append_turret_bank =
			[&](bool primary, int family_index) noexcept {
				if (input.bank_count >= MaximumPhase2StaticBanksPerClass ||
					turret_bank_count >=
						MaximumPhase2WeaponBanksPerFamily) {
					return false;
				}
				const auto weapon_index = primary
					? turret_weapons.primary_bank_weapons[family_index]
					: turret_weapons.secondary_bank_weapons[family_index];
				auto& target = input.banks[input.bank_count];
				target = {};
				target.bank_capture_key = input.bank_count + 1U;
				target.owner_subsystem_capture_key = subsystem_index + 1U;
				target.family_source = 4U;
				target.source_family = primary ? 1U : 2U;
				target.bank_index =
					static_cast<std::uint16_t>(family_index);
				if (!capture_weapon(
						weapon_index, target.weapon_capture_key)) {
					return false;
				}
				const auto& bank_weapon = Weapon_info[weapon_index];
				const auto turret_uses_ammunition =
					subsystem_definition.flags[
						Model::Subsystem_Flags::Turret_use_ammo];
				target.consumes_ammunition =
					turret_uses_ammunition &&
					(primary
						? bank_weapon.wi_flags[
							Weapon::Info_Flags::Ballistic]
						: !bank_weapon.wi_flags[
							Weapon::Info_Flags::SecondaryNoAmmo]);
				target.capacity = target.consumes_ammunition
					? static_cast<float>(primary
						  ? turret_weapons
								.primary_bank_capacity[family_index]
						  : turret_weapons
								.secondary_bank_capacity[family_index])
					: 0.0F;
				target.has_capacity = target.consumes_ammunition;
				target.fire_point_count = static_cast<std::uint32_t>(
					subsystem_definition.turret_num_firing_points);
				target.num_slots = target.fire_point_count;
				for (std::uint32_t point = 0U;
					 point < target.fire_point_count;
					 ++point) {
					target.fire_points[point] = raw_vec3(
						subsystem_definition.turret_firing_point[point]);
				}
				++input.bank_count;
				++turret_bank_count;
				return true;
			};
		for (int bank = 0; bank < turret_weapons.num_primary_banks;
			 ++bank) {
			if (!append_turret_bank(true, bank)) {
				return {Phase2SourceReadStatus::SourceLimitExceeded};
			}
		}
		for (int bank = 0; bank < turret_weapons.num_secondary_banks;
			 ++bank) {
			if (!append_turret_bank(false, bank)) {
				return {Phase2SourceReadStatus::SourceLimitExceeded};
			}
		}
	}
	if (!core_gate && ship_class.cmeasure_max > 0) {
		std::uint32_t countermeasure_key = 0U;
		if (!capture_weapon(ship_class.cmeasure_type, countermeasure_key)) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
		const auto& countermeasure = Weapon_info[ship_class.cmeasure_type];
		if (!countermeasure.wi_flags[Weapon::Info_Flags::Cmeasure] ||
			(Countermeasures_use_capacity &&
			 (!std::isfinite(countermeasure.cargo_size) ||
				 countermeasure.cargo_size <= 0.0F))) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
	}

	auto& raw_class = input.ship_info;
	raw_class.class_capture_key = 1U;
	raw_class.center_of_mass = input.model.center_of_mass;
	raw_class.subsystem_count = input.subsystem_count;
	raw_class.subsystem_offset = 0U;
	raw_class.bank_count = input.bank_count;
	raw_class.bank_offset = 0U;
	if (!assign_bounded_engine_name(
			raw_class.internal_name, ship_class.name, NAME_LENGTH)) {
		return {Phase2SourceReadStatus::SourceLimitExceeded};
	}
	raw_class.model_mass = model->mass;
	raw_class.density = ship_class.density;
	for (std::size_t index = 0U; index < raw_class.model_inertia.size(); ++index) {
		raw_class.model_inertia[index] = model->moment_of_inertia.a1d[index];
		raw_class.effective_inertia[index] =
			ship_object.phys_info.I_body_inv.a1d[index];
	}
	raw_class.max_velocity = raw_vec3(ship_class.max_vel);
	raw_class.afterburner_max_velocity =
		raw_vec3(ship_class.afterburner_max_vel);
	raw_class.booster_max_velocity =
		raw_vec3(ship_object.phys_info.booster_max_vel);
	raw_class.max_rotational_velocity = raw_vec3(ship_class.max_rotvel);
	raw_class.forward_accel_time = ship_class.forward_accel;
	raw_class.afterburner_forward_accel_time =
		ship_class.afterburner_forward_accel;
	raw_class.booster_forward_accel_time =
		ship_object.phys_info.booster_forward_accel_time_const;
	raw_class.forward_decel_time = ship_class.forward_decel;
	raw_class.slide_accel_time = ship_class.slide_accel;
	raw_class.slide_decel_time = ship_class.slide_decel;
	raw_class.max_hull_strength = ship_class.max_hull_strength;
	raw_class.max_shield_strength = ship_class.max_shield_strength;
	raw_class.has_afterburner =
		ship_class.flags[Ship::Info_Flags::Afterburner];
	raw_class.afterburner_fuel_capacity =
		ship_class.afterburner_fuel_capacity;
	raw_class.afterburner_burn_rate = ship_class.afterburner_burn_rate;
	raw_class.afterburner_recover_rate =
		ship_class.afterburner_recover_rate;
	raw_class.afterburner_min_start_fuel =
		ship_class.afterburner_min_start_fuel;
	raw_class.afterburner_cooldown_seconds =
		ship_class.afterburner_cooldown_time;
	raw_class.has_scan = ship_class.scan_time > 0;
	raw_class.scan_time_ms = ship_class.scan_time;
	raw_class.scan_range_normal = ship_class.scan_range_normal;
	raw_class.scan_range_capital = ship_class.scan_range_capital;
	raw_class.scanning_range_multiplier =
		ship_class.scanning_range_multiplier;
	raw_class.glide_cap = ship_object.phys_info.glide_cap;
	raw_class.has_glide =
		ship_class.can_glide && raw_class.glide_cap > 0.0F;
	raw_class.autoaim_fov_rad = ship_instance.autoaim_fov;
	raw_class.has_autoaim = raw_class.autoaim_fov_rad > 0.0F;
	raw_class.countermeasure_capacity = core_gate
		? 0.0F : static_cast<float>(ship_class.cmeasure_max);
	raw_class.countermeasure_uses_capacity =
		!core_gate && Countermeasures_use_capacity;
	if (!core_gate && ship_class.cmeasure_max > 0) {
		if (Weapon_info[ship_class.cmeasure_type].cmeasure_firewait < 0) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
		for (std::uint32_t index = 0U; index < referenced_weapon_count; ++index) {
			if (referenced_weapon_indices[index] == ship_class.cmeasure_type) {
				raw_class.countermeasure_weapon_capture_key = index + 1U;
				raw_class.countermeasure_cargo_size =
					Weapon_info[ship_class.cmeasure_type].cargo_size;
				raw_class.countermeasure_firewait_ms =
					static_cast<std::uint32_t>(
						Weapon_info[ship_class.cmeasure_type]
							.cmeasure_firewait);
				break;
			}
		}
	}
	raw_class.primary_bank_count = core_gate ? 0U
		: static_cast<std::uint32_t>(
			ship_class.num_primary_banks);
	raw_class.secondary_bank_count = core_gate ? 0U
		: static_cast<std::uint32_t>(
			ship_class.num_secondary_banks);
	raw_class.tertiary_bank_count =
		static_cast<std::uint32_t>(tertiary_count);
	raw_class.turret_bank_count = turret_bank_count;
	input.weapon_info.additional_count =
		referenced_weapon_count == 0U ? 0U : referenced_weapon_count - 1U;
	for (std::uint32_t index = 0U; index < referenced_weapon_count; ++index) {
		auto& raw_weapon = index == 0U
			? static_cast<Phase2RawWeaponDefinition&>(input.weapon_info)
			: input.weapon_info.additional_definitions[index - 1U];
		raw_weapon = {};
		const auto& source = Weapon_info[referenced_weapon_indices[index]];
		if (!fill_raw_weapon_definition(source, index + 1U, raw_weapon)) {
			return {Phase2SourceReadStatus::SourceLimitExceeded};
		}
	}
	input.registries.additional_count = 0U;
	const auto add_auxiliary =
		[&](Phase2RawAuxiliaryRegistry registry,
			std::string_view name,
			std::uint8_t pattern_code,
			std::uint32_t& capture_key) noexcept {
			if (registry != Phase2RawAuxiliaryRegistry::Pattern &&
				name.empty()) {
				return false;
			}
			for (std::uint32_t index = 0U;
				 index < input.registries.additional_count;
				 ++index) {
				const auto& existing =
					input.registries.additional_entries[index];
				if (existing.registry == registry &&
					existing.firing_pattern_source_code == pattern_code &&
					existing.name.view() == name) {
					capture_key = existing.capture_key;
					return true;
				}
			}
			if (input.registries.additional_count >=
				MaximumPhase2StaticAuxiliaryEntries) {
				return false;
			}
			auto& entry = input.registries.additional_entries[
				input.registries.additional_count++];
			entry = {};
			entry.registry = registry;
			entry.capture_key = input.registries.additional_count;
			entry.firing_pattern_source_code = pattern_code;
			if (!name.empty() && !entry.name.assign(name)) {
				--input.registries.additional_count;
				return false;
			}
			capture_key = entry.capture_key;
			return true;
		};
	if (ship_class.species < -1 ||
		ship_class.species >= static_cast<int>(Species_info.size()) ||
		(ship_class.species >= 0 &&
			!add_auxiliary(Phase2RawAuxiliaryRegistry::Species,
				Species_info[ship_class.species].species_name,
				0U,
				raw_class.species_capture_key))) {
		return {Phase2SourceReadStatus::UnsupportedEngineState};
	}
	if (ship_class.class_type < -1 ||
		ship_class.class_type >= static_cast<int>(Ship_types.size()) ||
		(ship_class.class_type >= 0 &&
			!add_auxiliary(Phase2RawAuxiliaryRegistry::ShipType,
				Ship_types[ship_class.class_type].name,
				0U,
				raw_class.ship_type_capture_key))) {
		return {Phase2SourceReadStatus::UnsupportedEngineState};
	}
	if (ship_instance.team < 0 ||
		ship_instance.team >= static_cast<int>(Iff_info.size()) ||
		!add_auxiliary(Phase2RawAuxiliaryRegistry::Iff,
			Iff_info[ship_instance.team].iff_name,
			0U,
			raw_class.iff_capture_key)) {
		return {Phase2SourceReadStatus::UnsupportedEngineState};
	}
	if (ship_instance.wingnum < -1 || ship_instance.wingnum >= MAX_WINGS ||
		(ship_instance.wingnum >= 0 &&
			!add_auxiliary(Phase2RawAuxiliaryRegistry::Wing,
				Wings[ship_instance.wingnum].name,
				0U,
				raw_class.wing_capture_key))) {
		return {Phase2SourceReadStatus::UnsupportedEngineState};
	}
	if (ship_instance.armor_type_idx < -1 ||
		ship_instance.armor_type_idx >= static_cast<int>(Armor_types.size()) ||
		(ship_instance.armor_type_idx >= 0 &&
			!add_auxiliary(Phase2RawAuxiliaryRegistry::Armor,
				Armor_types[ship_instance.armor_type_idx].GetNamePtr(),
				0U,
				raw_class.armor_capture_key))) {
		return {Phase2SourceReadStatus::UnsupportedEngineState};
	}
	for (std::uint32_t index = 0U; index < input.subsystem_count; ++index) {
		const auto armor_index = ship_class.subsystems[index].armor_type_idx;
		if (armor_index < -1 ||
			armor_index >= static_cast<int>(Armor_types.size()) ||
			(armor_index >= 0 &&
				!add_auxiliary(Phase2RawAuxiliaryRegistry::Armor,
					Armor_types[armor_index].GetNamePtr(),
					0U,
					input.subsystems[index].armor_capture_key))) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
		const auto instance_armor_source =
			output.subsystems.values[index].armor_source_key.value;
		if (instance_armor_source > Armor_types.size() ||
			(instance_armor_source != 0U &&
				!add_auxiliary(Phase2RawAuxiliaryRegistry::Armor,
					Armor_types[instance_armor_source - 1U].GetNamePtr(),
					0U,
					instance_subsystem_armor_capture_keys[index]))) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
	}
	for (std::uint32_t index = 0U; index < referenced_weapon_count; ++index) {
		const auto damage_index =
			Weapon_info[referenced_weapon_indices[index]].damage_type_idx;
		if (damage_index < -1 ||
			damage_index >= static_cast<int>(Damage_types.size()) ||
			(damage_index >= 0 &&
				!add_auxiliary(Phase2RawAuxiliaryRegistry::DamageType,
					Damage_types[damage_index].name,
					0U,
					(index == 0U
						? static_cast<Phase2RawWeaponDefinition&>(
							input.weapon_info)
						: input.weapon_info
							.additional_definitions[index - 1U])
						.damage_type_capture_key))) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
	}
	for (int bank = 0;
		 !core_gate && bank < ship_class.num_primary_banks; ++bank) {
		const auto weapon_index =
			ship_instance.weapons.primary_bank_weapons[bank];
		auto pattern = Weapon_info[weapon_index].firing_pattern;
		if (ship_class.flags[Ship::Info_Flags::Dyn_primary_linking]) {
			const auto dynamic_index =
				ship_instance.weapons.dynamic_firing_pattern[bank];
			if (dynamic_index < 0 ||
				static_cast<std::size_t>(dynamic_index) >=
					ship_class.dyn_firing_patterns_allowed[bank].size()) {
				return {Phase2SourceReadStatus::UnsupportedEngineState};
			}
			pattern =
				ship_class.dyn_firing_patterns_allowed[bank][dynamic_index];
		}
		const auto pattern_code = static_cast<std::uint8_t>(pattern);
		input.banks[bank].firing_pattern_source_code = pattern_code;
		if (pattern != FiringPattern::STANDARD) {
			std::uint32_t ignored_key = 0U;
			if (pattern_code > 5U ||
				!add_auxiliary(Phase2RawAuxiliaryRegistry::Pattern,
					{},
					pattern_code,
					ignored_key)) {
				return {Phase2SourceReadStatus::UnsupportedEngineState};
			}
		}
	}
	output.raw_static_references.class_capture_key =
		raw_class.class_capture_key;
	output.raw_static_references.weapon_count =
		referenced_weapon_count;
	for (std::uint32_t index = 0U; index < referenced_weapon_count; ++index) {
		output.raw_static_references.weapon_capture_keys[index] = index + 1U;
	}
	output.raw_static_references.auxiliary_count =
		input.registries.additional_count;
	for (std::uint32_t index = 0U;
		 index < output.raw_static_references.auxiliary_count;
		 ++index) {
		output.raw_static_references.auxiliary_capture_keys[index] =
			input.registries.additional_entries[index].capture_key;
	}
	return {Phase2SourceReadStatus::Valid};
	}();
	input.guards_valid =
		collected.status == Phase2SourceReadStatus::Valid;
	const auto mapped =
		map_phase2_static_authorities(input, output.raw_static_catalog);
	if (collected.status != Phase2SourceReadStatus::Valid) {
		output.raw_static_references = {};
		return collected;
	}
	if (mapped.status != Phase2SourceReadStatus::Valid) {
		output.raw_static_catalog.clear();
		output.raw_static_references = {};
		return mapped;
	}
	const auto remap_weapon_key =
		[&](Phase2CaptureLocalKey& key) noexcept {
			if (key.value == 0U) return true;
			const auto engine_index =
				static_cast<int>(key.value - 1U);
			for (std::uint32_t index = 0U;
				 index < referenced_weapon_count;
				 ++index) {
				if (referenced_weapon_indices[index] == engine_index) {
					key.value = index + 1U;
					return true;
				}
			}
			key = {};
			output.raw_static_catalog.clear();
			output.raw_static_references = {};
			return false;
		};
	for (std::size_t bank = 0U;
		 bank < output.weapons.primary_bank_count;
		 ++bank)
		if (!remap_weapon_key(
				output.weapons.primary_banks[bank]
					.weapon_class_source_key))
			return {Phase2SourceReadStatus::UnsupportedEngineState};
	for (std::size_t bank = 0U;
		 bank < output.weapons.secondary_bank_count;
		 ++bank)
		if (!remap_weapon_key(
				output.weapons.secondary_banks[bank]
					.weapon_class_source_key))
			return {Phase2SourceReadStatus::UnsupportedEngineState};
	if (!remap_weapon_key(
			output.weapons.countermeasure_class_source_key))
		return {Phase2SourceReadStatus::UnsupportedEngineState};
	for (std::size_t subsystem = 0U;
		 subsystem < output.subsystems.count;
		 ++subsystem) {
		output.subsystems.values[subsystem].source_key.value =
			static_cast<std::uint32_t>(subsystem + 1U);
		output.subsystems.values[subsystem].armor_source_key.value =
			instance_subsystem_armor_capture_keys[subsystem];
		if (!output.subsystems.values[subsystem].turret.has_value()) continue;
		auto& turret = *output.subsystems.values[subsystem].turret;
		for (std::size_t bank = 0U;
			 bank < turret.turret_primary_bank_count;
			 ++bank)
			if (!remap_weapon_key(
					turret.turret_primary_bank_weapon_source_keys[bank]))
				return {Phase2SourceReadStatus::UnsupportedEngineState};
		for (std::size_t bank = 0U;
			 bank < turret.turret_secondary_bank_count;
			 ++bank)
			if (!remap_weapon_key(
					turret.turret_secondary_bank_weapon_source_keys[bank]))
				return {Phase2SourceReadStatus::UnsupportedEngineState};
	}
	output.damage.armor_source_key.value =
		output.raw_static_catalog.class_definitions[0].armor_capture_key;
	return mapped;
}

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

bool finite_bounded(float value, float absolute_limit) noexcept
{
	return std::isfinite(value) && value >= -absolute_limit && value <= absolute_limit;
}

bool valid_vector(const CaptureVec3f& value, float absolute_limit) noexcept
{
	return finite_bounded(value.x, absolute_limit) && finite_bounded(value.y, absolute_limit) &&
		finite_bounded(value.z, absolute_limit);
}

void canonicalize_zero(float& value) noexcept
{
	if (value == 0.0f) {
		value = 0.0f;
	}
}

void canonicalize_zero(CaptureVec3f& value) noexcept
{
	canonicalize_zero(value.x);
	canonicalize_zero(value.y);
	canonicalize_zero(value.z);
}

CaptureResult no_player(CaptureReason reason) noexcept
{
	return {CaptureStatus::NoPlayer, reason};
}

CaptureResult invalid_source(CaptureReason reason) noexcept
{
	return {CaptureStatus::InvalidSource, reason};
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

bool has_raw_dock_leader_flag(const ship& source) noexcept
{
	return source.flags[Ship::Ship_Flags::Dock_leader];
}

SourceReadResult read_direct_docking_facts(
	object& subject, ShipDockingObservation& output) noexcept
{
	output = {};
	if (subject.type != OBJ_SHIP || subject.instance < 0 || subject.instance >= MAX_SHIPS) {
		return {Phase2SourceReadStatus::InvalidSource};
	}
	const auto& subject_ship = Ships[subject.instance];
	output.dock_leader = has_raw_dock_leader_flag(subject_ship);
	std::size_t direct_relation_steps = 0U;
	for (auto* relation = subject.dock_list; relation != nullptr; relation = relation->next) {
		if (++direct_relation_steps > MaximumPhase2DockRelationsPerShip) {
			return {Phase2SourceReadStatus::SourceLimitExceeded};
		}
		if (relation->docked_objp == nullptr || relation->docked_objp->type != OBJ_SHIP ||
			relation->docked_objp->signature <= 0 || relation->dockpoint_used < 0 ||
			relation->dockpoint_used > 4095 || relation->docked_objp->instance < 0 ||
			relation->docked_objp->instance >= MAX_SHIPS ||
			Ships[relation->docked_objp->instance].objnum < 0 ||
			Ships[relation->docked_objp->instance].objnum >= MAX_OBJECTS ||
			&Objects[Ships[relation->docked_objp->instance].objnum] !=
				relation->docked_objp) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
		int remote_dockpoint = -1;
		std::size_t inverse_relation_steps = 0U;
		for (auto* inverse_relation = relation->docked_objp->dock_list;
			 inverse_relation != nullptr;
			 inverse_relation = inverse_relation->next) {
			if (++inverse_relation_steps > MaximumPhase2DockRelationsPerShip) {
				return {Phase2SourceReadStatus::SourceLimitExceeded};
			}
			if (inverse_relation->docked_objp == &subject) {
				remote_dockpoint = inverse_relation->dockpoint_used;
				break;
			}
		}
		if (remote_dockpoint < 0 || remote_dockpoint > 4095) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
		auto& destination = output.relations[output.relation_count++];
		destination.remote_capture_key.value =
			static_cast<std::uint32_t>(relation->docked_objp->signature);
		destination.local_dockpoint =
			static_cast<std::uint16_t>(relation->dockpoint_used);
		destination.remote_dockpoint =
			static_cast<std::uint16_t>(remote_dockpoint);

		const auto copy_dock_bay_name = [](const object& dock_object,
										 int dockpoint,
										 OwnedPhase2String& name) noexcept {
			const auto& dock_ship = Ships[dock_object.instance];
			if (dock_ship.ship_info_index < 0 ||
				dock_ship.ship_info_index >= static_cast<int>(Ship_info.size())) {
				return false;
			}
			const auto* model = model_get(Ship_info[dock_ship.ship_info_index].model_num);
			return model != nullptr && dockpoint < model->n_docks &&
				name.assign(model->docking_bays[dockpoint].name);
		};
		if (!copy_dock_bay_name(
				subject, relation->dockpoint_used, destination.local_dock_bay_name) ||
			!copy_dock_bay_name(*relation->docked_objp,
				remote_dockpoint,
				destination.remote_dock_bay_name)) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
	}
	return {Phase2SourceReadStatus::Valid};
}

FsoEngineReadView::FsoEngineReadView() noexcept = default;

bool FsoEngineReadView::current_thread_is_main() const noexcept
{
	return phase2_current_thread_is_main();
}

bool FsoEngineReadView::in_mission() const noexcept
{
	return (Game_mode & GM_IN_MISSION) != 0;
}

bool FsoEngineReadView::player_exists() const noexcept
{
	return Player != nullptr;
}

bool FsoEngineReadView::player_object_exists() const noexcept
{
	return Player_obj != nullptr;
}

bool FsoEngineReadView::player_ship_exists() const noexcept
{
	return Player_ship != nullptr;
}

bool FsoEngineReadView::player_source_is_consistent() const noexcept
{
	return (Game_mode & GM_IN_MISSION) != 0 && Player != nullptr && Player_obj != nullptr &&
		Player_ship != nullptr && Player_obj->type == OBJ_SHIP && Player_obj->instance >= 0 &&
		Player_obj->instance < MAX_SHIPS && Player->objnum >= 0 && Player->objnum < MAX_OBJECTS &&
		&Objects[Player->objnum] == Player_obj && &Ships[Player_obj->instance] == Player_ship &&
		Player_ship->objnum >= 0 && Player_ship->objnum < MAX_OBJECTS &&
		&Objects[Player_ship->objnum] == Player_obj && Player_obj->signature > 0 &&
		Player_ship->ship_info_index >= 0 &&
		Player_ship->ship_info_index < static_cast<int>(Ship_info.size());
}

SourceReadResult FsoEngineReadView::read_player_root_key(EngineEntityKey& output) const noexcept
{
	// Pre-WP03 CoreGate selection has exactly one authorized engine key.
	output = {};
	if (!player_source_is_consistent()) {
		return {Phase2SourceReadStatus::InvalidSource};
	}
	output.object_index = Player->objnum;
	output.object_signature = static_cast<std::uint32_t>(Player_obj->signature);
	return {Phase2SourceReadStatus::Valid};
}

SourceReadResult FsoEngineReadView::read_discovery_node(
	EngineEntityKey key, Phase2DiscoveryNode& output) const noexcept
{
	output = {};
	if (!player_source_is_consistent() || key.object_index < 0 ||
		key.object_index >= MAX_OBJECTS) {
		return {Phase2SourceReadStatus::InvalidSource};
	}
	auto* Player_obj = &Objects[key.object_index];
	if (Player_obj->type != OBJ_SHIP || Player_obj->signature <= 0 ||
		key.object_signature != static_cast<std::uint32_t>(Player_obj->signature) ||
		Player_obj->instance < 0 || Player_obj->instance >= MAX_SHIPS) {
		return {Phase2SourceReadStatus::InvalidSource};
	}
	auto* Player_ship = &Ships[Player_obj->instance];
	if (Player_ship->objnum != key.object_index) {
		return {Phase2SourceReadStatus::InvalidSource};
	}
	output.capture_key.value=key.object_signature;
	std::uint32_t dock_leader_capture_key = 0U;
	if (has_raw_dock_leader_flag(*Player_ship)) {
		dock_leader_capture_key = key.object_signature;
	}
	{
		std::size_t leader_search_steps = 0U;
		for (auto* relation = Player_obj->dock_list;
			 relation != nullptr;
			 relation = relation->next) {
			if (++leader_search_steps > MaximumPhase2DockRelationsPerShip) {
				return {Phase2SourceReadStatus::SourceLimitExceeded};
			}
			if (relation->docked_objp != nullptr &&
				relation->docked_objp->type == OBJ_SHIP &&
				relation->docked_objp->instance >= 0 &&
				relation->docked_objp->instance < MAX_SHIPS &&
				has_raw_dock_leader_flag(
					Ships[relation->docked_objp->instance])) {
				const auto candidate =
					static_cast<std::uint32_t>(
						relation->docked_objp->signature);
				if (dock_leader_capture_key != 0U &&
					dock_leader_capture_key != candidate)
					return {
						Phase2SourceReadStatus::UnsupportedEngineState};
				dock_leader_capture_key = candidate;
			}
		}
	}
	if (dock_leader_capture_key != 0U) {
		output.group_leader_capture_key.value=dock_leader_capture_key;
	}
	if (Player_ship->ai_index < 0 || Player_ship->ai_index >= MAX_AI_INFO) {
		return {Phase2SourceReadStatus::UnsupportedEngineState};
	}
	const auto& ship_ai = Ai_info[Player_ship->ai_index];
	output.raw_support_flags =
		(ship_ai.ai_flags[AI::AI_Flags::Awaiting_repair] ? 0x01U : 0U) |
		(ship_ai.ai_flags[AI::AI_Flags::Being_repaired] ? 0x02U : 0U) |
		(ship_ai.ai_flags[AI::AI_Flags::Repairing] ? 0x04U : 0U);
	if (ship_ai.support_ship_objnum < -1 || ship_ai.support_ship_signature < -1 ||
		((ship_ai.support_ship_objnum == -1) != (ship_ai.support_ship_signature == -1))) {
		return {Phase2SourceReadStatus::UnsupportedEngineState};
	}
	if (ship_ai.support_ship_objnum >= 0 &&
		(ship_ai.support_ship_objnum >= MAX_OBJECTS ||
			Objects[ship_ai.support_ship_objnum].type != OBJ_SHIP ||
			Objects[ship_ai.support_ship_objnum].signature != ship_ai.support_ship_signature ||
			Objects[ship_ai.support_ship_objnum].instance < 0 ||
			Objects[ship_ai.support_ship_objnum].instance >= MAX_SHIPS ||
			Ships[Objects[ship_ai.support_ship_objnum].instance].objnum !=
				ship_ai.support_ship_objnum)) {
		return {Phase2SourceReadStatus::UnsupportedEngineState};
	}
	if(ship_ai.support_ship_signature>0)
		output.support_capture_key.value=
			static_cast<std::uint32_t>(ship_ai.support_ship_signature);
	ShipDockingObservation docking;
	const auto docking_result = read_direct_docking_facts(*Player_obj, docking);
	if (docking_result.status != Phase2SourceReadStatus::Valid) {
		return docking_result;
	}
	output.direct_docking_count = docking.relation_count;
	for (std::size_t index = 0U; index < docking.relation_count; ++index) {
		output.direct_docking_capture_keys[index].value=
			docking.relations[index].remote_capture_key.value;
		output.direct_docking_reciprocal[index]=true;
		output.direct_local_dockpoints[index] =
			docking.relations[index].local_dockpoint;
		output.direct_remote_dockpoints[index] =
			docking.relations[index].remote_dockpoint;
	}
	return {Phase2SourceReadStatus::Valid};
}

SourceReadResult FsoEngineReadView::resolve_capture_local_key(
	Phase2CaptureLocalKey key, EngineEntityKey& output) const noexcept
{
	output = {};
	if (key.value == 0U) {
		return {Phase2SourceReadStatus::UnsupportedEngineState};
	}
	if (!player_source_is_consistent()) {
		return {Phase2SourceReadStatus::InvalidSource};
	}
	for (int object_index = 0; object_index < MAX_OBJECTS; ++object_index) {
		const auto& candidate = Objects[object_index];
		if (candidate.type != OBJ_SHIP || candidate.signature <= 0 ||
			static_cast<std::uint32_t>(candidate.signature) != key.value ||
			candidate.instance < 0 || candidate.instance >= MAX_SHIPS ||
			Ships[candidate.instance].objnum != object_index) {
			continue;
		}
		output.object_index = object_index;
		output.object_signature = key.value;
		return {Phase2SourceReadStatus::Valid};
	}
	return {Phase2SourceReadStatus::InvalidSource};
}

SourceReadResult FsoEngineReadView::read_ship_for_projection(
	EngineEntityKey key, Phase2ShipSource& output,
	bool core_gate) const noexcept
{
	Phase2ReadDiagnosticCursor diagnostic_cursor{Phase2CaptureBlock::Identity};
	clear_phase2_ship_source_logical(output);
	if (!core_gate && phase2_wp07_seam_overflowed())
		return {Phase2SourceReadStatus::SourceLimitExceeded};
	if (!player_source_is_consistent() || key.object_index < 0 ||
		key.object_index >= MAX_OBJECTS) {
		return {Phase2SourceReadStatus::InvalidSource};
	}
	auto* Player_obj = &Objects[key.object_index];
	if (Player_obj->type != OBJ_SHIP || Player_obj->signature <= 0 ||
		key.object_signature != static_cast<std::uint32_t>(Player_obj->signature) ||
		Player_obj->instance < 0 || Player_obj->instance >= MAX_SHIPS) {
		return {Phase2SourceReadStatus::InvalidSource};
	}
	auto* Player_ship = &Ships[Player_obj->instance];
	if (Player_ship->objnum != key.object_index ||
		Player_ship->ship_info_index < 0 ||
		Player_ship->ship_info_index >= static_cast<int>(Ship_info.size())) {
		return {Phase2SourceReadStatus::InvalidSource};
	}

	const auto& ship_class = Ship_info[Player_ship->ship_info_index];
	std::size_t ship_name_size = 0U;
	while (ship_name_size < NAME_LENGTH && Player_ship->ship_name[ship_name_size] != '\0') {
		++ship_name_size;
	}
	std::size_t class_name_size = 0U;
	while (class_name_size < NAME_LENGTH && ship_class.name[class_name_size] != '\0') {
		++class_name_size;
	}
	if (ship_name_size == NAME_LENGTH || ship_name_size > MaximumPhase2InternalNameBytes ||
		class_name_size == NAME_LENGTH || class_name_size > MaximumPhase2InternalNameBytes) {
		return {Phase2SourceReadStatus::SourceLimitExceeded};
	}

	output.internal_name = std::string_view{Player_ship->ship_name, ship_name_size};
	output.class_name = std::string_view{ship_class.name, class_name_size};
	output.identity.presence = protocol::ShipIdentityPresenceFlagNone;
	output.identity.class_source_key.value = 1U;
	output.identity.role_flags =
		protocol::ShipRoleFlagPlayer | protocol::ShipRoleFlagMissionObject;
	if (Player_ship->ai_index >= 0) {
		output.identity.role_flags |= protocol::ShipRoleFlagAi;
	}
	if (ship_class.flags[Ship::Info_Flags::Support]) {
		output.identity.role_flags |= protocol::ShipRoleFlagSupport;
	}
	if (Player_ship->has_display_name() && !Player_ship->display_name.empty()) {
		if (!output.identity.display_name.assign(Player_ship->display_name)) {
			return {Phase2SourceReadStatus::SourceLimitExceeded};
		}
		output.identity.presence |= protocol::ShipIdentityPresenceFlagDisplayName;
	}
	if (Player_ship->callsign_index >= 0) {
		const auto* callsign = mission_parse_lookup_callsign_index(Player_ship->callsign_index);
		if (callsign == nullptr || callsign[0] == '\0' ||
			!output.identity.callsign.assign(callsign) ||
			output.identity.callsign.size() > 127U) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
		output.identity.presence |= protocol::ShipIdentityPresenceFlagCallsign;
	}
	if (Player_ship->wingnum >= 0) {
		if (Player_ship->wingnum >= MAX_WINGS) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
		const auto& wing = Wings[Player_ship->wingnum];
		std::uint16_t matches = 0U;
		for (int position = 0; position < wing.current_count; ++position) {
			if (wing.ship_index[position] == Player_obj->instance) {
				output.identity.wing_position = static_cast<std::uint16_t>(position);
				++matches;
			}
		}
		if (matches != 1U || !output.identity.wing_name.assign(wing.name)) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
		output.identity.presence |= protocol::ShipIdentityPresenceFlagWing;
	}
	output.lifecycle.presence = protocol::EntityLifecyclePresenceFlagNone;

	if (Player_ship->flags[Ship::Ship_Flags::Exploded]) {
		output.lifecycle.state = ShipLifecycleState::Destroyed;
	} else if (Player_ship->flags[Ship::Ship_Flags::Dying]) {
		output.lifecycle.state = ShipLifecycleState::Dying;
	} else if (Player_obj->flags[Object::Object_Flags::Should_be_dead]) {
		output.lifecycle.state = ShipLifecycleState::Removed;
	} else if (Player_ship->is_departing()) {
		output.lifecycle.state = ShipLifecycleState::Departing;
	} else if (Player_ship->is_arriving()) {
		output.lifecycle.state = ShipLifecycleState::Spawning;
	} else if (Player_ship->flags[Ship::Ship_Flags::Disabled]) {
		output.lifecycle.state = ShipLifecycleState::Disabled;
	} else {
		output.lifecycle.state = ShipLifecycleState::Present;
	}
	if (Player_ship->flags[Ship::Ship_Flags::Dying]) {
		output.lifecycle.lifecycle_flags |= protocol::EntityLifecycleFlagDying;
	}
	if (Player_ship->flags[Ship::Ship_Flags::Disabled]) {
		output.lifecycle.lifecycle_flags |= protocol::EntityLifecycleFlagDisabled;
	}
	if (Player_ship->flags[Ship::Ship_Flags::Exploded]) {
		output.lifecycle.lifecycle_flags |= protocol::EntityLifecycleFlagExploded;
	}
	if (Player_obj->flags[Object::Object_Flags::Should_be_dead]) {
		output.lifecycle.lifecycle_flags |= protocol::EntityLifecycleFlagShouldBeDead;
	}

	diagnostic_cursor.begin(Phase2CaptureBlock::Flight);
	output.flight.position_world =
		{Player_obj->pos.xyz.x, Player_obj->pos.xyz.y, Player_obj->pos.xyz.z};
	const CaptureOrientationBasis orientation_basis{
		{Player_obj->orient.vec.rvec.xyz.x,
			Player_obj->orient.vec.rvec.xyz.y,
			Player_obj->orient.vec.rvec.xyz.z},
		{Player_obj->orient.vec.uvec.xyz.x,
			Player_obj->orient.vec.uvec.xyz.y,
			Player_obj->orient.vec.uvec.xyz.z},
		{Player_obj->orient.vec.fvec.xyz.x,
			Player_obj->orient.vec.fvec.xyz.y,
			Player_obj->orient.vec.fvec.xyz.z}};
	CaptureQuaternionf orientation;
	if (convert_fso_orientation_to_local_to_world(orientation_basis, orientation) ==
		QuaternionConversionStatus::Converted) {
		output.flight.orientation_local_to_world =
			{orientation.w, orientation.x, orientation.y, orientation.z};
	} else {
		output.flight.orientation_local_to_world[0] = std::numeric_limits<float>::quiet_NaN();
	}
	output.flight.velocity_world =
		{Player_obj->phys_info.vel.xyz.x, Player_obj->phys_info.vel.xyz.y, Player_obj->phys_info.vel.xyz.z};
	output.flight.rotational_velocity_local = {Player_obj->phys_info.rotvel.xyz.x,
		Player_obj->phys_info.rotvel.xyz.y,
		Player_obj->phys_info.rotvel.xyz.z};
	output.flight.radius = Player_obj->radius;
	const EnginePhysicsFlagInput physics{static_cast<std::uint32_t>(Player_obj->phys_info.flags),
		Player_obj->flags[Object::Object_Flags::Immobile],
		Player_obj->flags[Object::Object_Flags::Dont_change_position],
		Player_obj->flags[Object::Object_Flags::Dont_change_orientation]};
	output.flight.physics_mode_flags = map_player_physics_mode_flags(physics);

	diagnostic_cursor.begin(Phase2CaptureBlock::DamageShield);
	output.damage.hull_maximum = Player_ship->ship_max_hull_strength;
	if (Player_obj->flags[Object::Object_Flags::Invulnerable]) {
		output.damage.protection_flags |= protocol::ProtectionFlagInvulnerable;
	}
	if (Player_obj->flags[Object::Object_Flags::Protected]) {
		output.damage.protection_flags |= protocol::ProtectionFlagProtected;
	}
	if (std::isfinite(output.damage.hull_maximum) && output.damage.hull_maximum >= 0.0F &&
		std::isfinite(Player_obj->hull_strength)) {
		output.damage.hull_current =
			std::max(0.0F, std::min(Player_obj->hull_strength, output.damage.hull_maximum));
	} else {
		output.damage.hull_current = Player_obj->hull_strength;
	}
	if (Player_ship->ship_guardian_threshold > 0) {
		output.damage.presence |= protocol::DamageStatePresenceFlagGuardian;
		output.damage.protection_flags |= protocol::ProtectionFlagGuardian;
		const auto guardian_hp = 0.01 * static_cast<double>(Player_ship->ship_guardian_threshold) *
			static_cast<double>(output.damage.hull_maximum);
		output.damage.guardian_threshold = static_cast<float>(guardian_hp);
	}
	if (Player_ship->armor_type_idx >= 0 &&
		Player_ship->armor_type_idx < static_cast<int>(Armor_types.size())) {
		output.damage.presence |= protocol::DamageStatePresenceFlagArmor;
		output.damage.armor_source_key.value =
			static_cast<std::uint32_t>(Player_ship->armor_type_idx + 1);
	} else if (Player_ship->armor_type_idx >= 0) {
		output.damage.hull_maximum = std::numeric_limits<float>::quiet_NaN();
	}

	const auto physical_shield_maximum = shield_get_max_strength(Player_obj, true);
	if (!std::isfinite(physical_shield_maximum)) {
		output.shields.recharge_maximum = std::numeric_limits<float>::quiet_NaN();
	} else {
		output.shields.has_shields =
			!Player_obj->flags[Object::Object_Flags::No_shields] && physical_shield_maximum > 0.0F;
	}
	if (std::isfinite(physical_shield_maximum) && output.shields.has_shields) {
		const auto segment_count = Player_obj->shield_quadrant.size();
		if (segment_count == 0U || segment_count > MaximumPhase2ShieldSegments) {
			output.shields.segment_count =
				static_cast<std::uint8_t>(MaximumPhase2ShieldSegments + 1U);
		} else {
			output.shields.segment_count = static_cast<std::uint8_t>(segment_count);
			const auto segment_maximum = shield_get_max_quad(Player_obj);
			for (std::size_t segment = 0U; segment < segment_count; ++segment) {
				const auto segment_current = Player_obj->shield_quadrant[segment];
				output.shields.segment_current_hits[segment] =
					std::isfinite(segment_current) && std::isfinite(segment_maximum)
					? std::max(0.0F, std::min(segment_current, segment_maximum))
					: segment_current;
				output.shields.segment_maximum_hits[segment] = segment_maximum;
			}
		}
		output.shields.presence |= protocol::ShieldStatePresenceFlagRechargeMax;
		output.shields.recharge_maximum = shield_get_max_strength(Player_obj, false);
		output.shields.presence |= protocol::ShieldStatePresenceFlagDeferredTransfer;
		output.shields.deferred_transfer = Player_ship->target_shields_delta;
	}

	diagnostic_cursor.begin(Phase2CaptureBlock::EnergyPropulsion);
	const auto ets_domains = ets_properties(Player_obj);
	const auto no_ets = Player_ship->flags[Ship::Ship_Flags::No_ets] || ets_domains == 0;
	const auto copy_ets_index = [&](int domain, int source_index, std::uint8_t& destination) noexcept {
		if (no_ets || (ets_domains & domain) == 0) {
			destination = 0U;
			return true;
		}
		if (source_index < 0 || source_index > 12) {
			return false;
		}
		destination = static_cast<std::uint8_t>(source_index);
		return true;
	};
	const auto ets_indices_valid =
		copy_ets_index(HAS_SHIELDS, Player_ship->shield_recharge_index, output.energy.shield_recharge_index) &&
		copy_ets_index(HAS_WEAPONS, Player_ship->weapon_recharge_index, output.energy.weapon_recharge_index) &&
		copy_ets_index(HAS_ENGINES, Player_ship->engine_recharge_index, output.energy.engine_recharge_index);
	if (ets_indices_valid) {
		const auto ets_domain_count = ((ets_domains & HAS_SHIELDS) != 0 ? 1U : 0U) +
			((ets_domains & HAS_WEAPONS) != 0 ? 1U : 0U) +
			((ets_domains & HAS_ENGINES) != 0 ? 1U : 0U);
		output.energy.ets_available = !no_ets && ets_domain_count >= 2U;
		output.energy.ets_mode = no_ets ? protocol::EtsMode::Absent
			: (ets_domain_count >= 2U ? protocol::EtsMode::Available : protocol::EtsMode::Locked);
	}
	if (ship_has_energy_weapons(Player_ship)) {
		output.energy.presence |= protocol::EnergyStatePresenceFlagWeaponEnergy;
		output.energy.weapon_energy_maximum = ship_class.max_weapon_reserve;
		if (std::isfinite(output.energy.weapon_energy_maximum) &&
			output.energy.weapon_energy_maximum >= 0.0F && std::isfinite(Player_ship->weapon_energy)) {
			output.energy.weapon_energy_current =
				std::max(0.0F, std::min(Player_ship->weapon_energy, output.energy.weapon_energy_maximum));
		} else {
			output.energy.weapon_energy_current = Player_ship->weapon_energy;
		}
	}
	if (!ets_indices_valid) {
		output.energy.weapon_energy_maximum = std::numeric_limits<float>::quiet_NaN();
	}
	output.energy.presence |= protocol::EnergyStatePresenceFlagRegeneration |
		protocol::EnergyStatePresenceFlagPowerOutput;
	output.energy.power_output = ship_class.power_output;
	const auto dying_or_unpowered =
		Player_ship->flags[Ship::Ship_Flags::Dying] || ship_class.power_output == 0.0F;
	const auto player_scale_valid = The_mission.ai_profile != nullptr &&
		Game_skill_level >= 0 && Game_skill_level < NUM_SKILL_LEVELS;
	if (!player_scale_valid) {
		output.energy.power_output = std::numeric_limits<float>::quiet_NaN();
	} else if (!dying_or_unpowered) {
		const auto power_factor = static_cast<double>(ets_power_factor(Player_obj));
		const auto weapon_scale =
			static_cast<double>(The_mission.ai_profile->weapon_energy_scale[Game_skill_level]);
		const auto shield_scale =
			static_cast<double>(The_mission.ai_profile->shield_energy_scale[Game_skill_level]);
		output.energy.weapon_regeneration_rate = static_cast<float>(power_factor *
			static_cast<double>(Energy_levels[output.energy.weapon_recharge_index]) *
			static_cast<double>(Player_ship->max_weapon_regen_per_second) *
			static_cast<double>(ship_class.max_weapon_reserve) * weapon_scale);
		output.energy.shield_regeneration_rate = static_cast<float>(power_factor *
			static_cast<double>(Energy_levels[output.energy.shield_recharge_index]) *
			static_cast<double>(Player_ship->max_shield_regen_per_second) *
			static_cast<double>(physical_shield_maximum) * shield_scale);
		if (Player_ship->ai_index >= 0 && Player_ship->ai_index < MAX_AI_INFO &&
			Missiontime - Ai_info[Player_ship->ai_index].last_hit_time <
				fl2f(ship_class.shield_regen_hit_delay)) {
			output.energy.shield_regeneration_rate = 0.0F;
		}
	}
	if (output.shields.has_shields) {
		output.shields.presence |= protocol::ShieldStatePresenceFlagRegenRate;
		output.shields.regeneration_rate = output.energy.shield_regeneration_rate;
	}
	if ((output.energy.presence & protocol::EnergyStatePresenceFlagWeaponEnergy) != 0U &&
		output.shields.has_shields) {
		output.energy.presence |= protocol::EnergyStatePresenceFlagDeferredTransfers;
		output.energy.deferred_weapon_transfer = Player_ship->target_weapon_energy_delta;
		output.energy.deferred_shield_transfer = Player_ship->target_shields_delta;
	}
	const auto& engine_aggregate = Player_ship->subsys_info[SUBSYSTEM_ENGINE];
	if (engine_aggregate.aggregate_max_hits > 0.0F) {
		output.energy.presence |= protocol::EnergyStatePresenceFlagEngineIntegrity;
		output.energy.engine_integrity_current = engine_aggregate.aggregate_current_hits;
		output.energy.engine_integrity_maximum = engine_aggregate.aggregate_max_hits;
	}

	const auto has_afterburner_class = ship_class.flags[Ship::Info_Flags::Afterburner];
	const auto afterburner_capacity = ship_class.afterburner_fuel_capacity;
	const auto has_afterburner_reservoir =
		has_afterburner_class && std::isfinite(afterburner_capacity) && afterburner_capacity > 0.0F;
	if (has_afterburner_class &&
		(!std::isfinite(afterburner_capacity) || afterburner_capacity < 0.0F)) {
		output.propulsion.afterburner_capacity = std::numeric_limits<float>::quiet_NaN();
	} else if (has_afterburner_reservoir) {
		output.propulsion.presence |= protocol::PropulsionStatePresenceFlagFuel |
			protocol::PropulsionStatePresenceFlagConsumption |
			protocol::PropulsionStatePresenceFlagDynamics;
		output.propulsion.propulsion_flags |= protocol::PropulsionFlagAfterburnerAvailable;
		output.propulsion.afterburner_capacity = afterburner_capacity;
		output.propulsion.afterburner_fuel =
			std::isfinite(Player_ship->afterburner_fuel)
			? std::max(0.0F, std::min(Player_ship->afterburner_fuel, afterburner_capacity))
			: Player_ship->afterburner_fuel;
		output.propulsion.burn_rate = ship_class.afterburner_burn_rate;
		output.propulsion.recovery_rate = ship_class.afterburner_recover_rate;
		output.propulsion.forward_acceleration_time_constant =
			Player_obj->phys_info.forward_accel_time_const;
		output.propulsion.afterburner_max_velocity = {
			ship_class.afterburner_max_vel.xyz.x,
			ship_class.afterburner_max_vel.xyz.y,
			ship_class.afterburner_max_vel.xyz.z};
		if (Player_ship->flags[Ship::Ship_Flags::Afterburner_locked]) {
			output.propulsion.propulsion_flags |= protocol::PropulsionFlagAfterburnerLocked;
		}
		if (Player_ship->flags[Ship::Ship_Flags::Attempting_to_afterburn]) {
			output.propulsion.propulsion_flags |= protocol::PropulsionFlagAfterburnerRequested;
		}
		if (!std::isfinite(ship_class.afterburner_min_start_fuel) ||
			ship_class.afterburner_min_start_fuel < 0.0F ||
			ship_class.afterburner_min_start_fuel > afterburner_capacity) {
			output.propulsion.afterburner_capacity = std::numeric_limits<float>::quiet_NaN();
		}
	}
	if (Player_ship->wash_intensity > 0.0F) {
		output.propulsion.presence |= protocol::PropulsionStatePresenceFlagEngineWash;
		output.propulsion.engine_wash_intensity = Player_ship->wash_intensity;
	}
	const auto raw_physics_flags = static_cast<std::uint32_t>(Player_obj->phys_info.flags);
	if ((raw_physics_flags & PF_AFTERBURNER_ON) != 0U) {
		output.propulsion.propulsion_flags |= protocol::PropulsionFlagAfterburnerActive;
		if (!has_afterburner_reservoir ||
			Player_ship->flags[Ship::Ship_Flags::Afterburner_locked]) {
			output.propulsion.afterburner_capacity = std::numeric_limits<float>::quiet_NaN();
		}
	}
	if ((raw_physics_flags & PF_BOOSTER_ON) != 0U) {
		output.propulsion.propulsion_flags |= protocol::PropulsionFlagBoosterActive;
	}
	if ((raw_physics_flags & PF_GLIDING) != 0U) {
		output.propulsion.propulsion_flags |= protocol::PropulsionFlagGlideActive;
	}
	if ((raw_physics_flags & PF_FORCE_GLIDE) != 0U) {
		output.propulsion.propulsion_flags |= protocol::PropulsionFlagGlideForced;
	}
	if (has_afterburner_reservoir && Player_ship->afterburner_last_end_time > 0) {
		const auto now_ms = static_cast<std::int64_t>(timer_get_milliseconds());
		const auto elapsed_ms =
			now_ms - static_cast<std::int64_t>(Player_ship->afterburner_last_end_time);
		const auto cooldown_ms = static_cast<double>(ship_class.afterburner_cooldown_time) * 1000.0;
		if (elapsed_ms < 0 || elapsed_ms > 86'400'000 || !std::isfinite(cooldown_ms) ||
			cooldown_ms < 0.0 || cooldown_ms > 86'400'000.0 ||
			!std::isfinite(Player_ship->afterburner_last_engage_fuel) ||
			Player_ship->afterburner_last_engage_fuel < 0.0F ||
			Player_ship->afterburner_last_engage_fuel > afterburner_capacity) {
			output.propulsion.afterburner_capacity = std::numeric_limits<float>::quiet_NaN();
		} else {
			output.propulsion.presence |= protocol::PropulsionStatePresenceFlagEngagement;
			output.propulsion.minimum_to_engage = ship_class.afterburner_min_start_fuel;
			output.propulsion.fuel_at_last_engagement =
				Player_ship->afterburner_last_engage_fuel;
			output.propulsion.time_since_last_stop_us =
				static_cast<std::uint64_t>(elapsed_ms) * 1000U;
			const auto remaining_ms = std::max(cooldown_ms - static_cast<double>(elapsed_ms), 0.0);
			output.propulsion.cooldown_remaining_us =
				static_cast<std::uint64_t>(remaining_ms * 1000.0);
		}
	}

	if (!core_gate) {
	diagnostic_cursor.begin(Phase2CaptureBlock::Weapons);
	const auto& source_weapons = Player_ship->weapons;
	if (source_weapons.num_primary_banks < 0 ||
		source_weapons.num_secondary_banks < 0 ||
		source_weapons.num_primary_banks > MAX_SHIP_PRIMARY_BANKS ||
		source_weapons.num_secondary_banks > MAX_SHIP_SECONDARY_BANKS) {
		return {Phase2SourceReadStatus::UnsupportedEngineState};
	}
	if (source_weapons.num_tertiary_banks < 0 ||
		source_weapons.num_tertiary_banks >
			static_cast<int>(MaximumPhase2WeaponBanksPerFamily) ||
		source_weapons.tertiary_bank_ammo < 0 ||
		source_weapons.tertiary_bank_start_ammo < 0 ||
		source_weapons.tertiary_bank_capacity < 0 ||
		source_weapons.tertiary_bank_ammo >
			source_weapons.tertiary_bank_start_ammo ||
		source_weapons.tertiary_bank_start_ammo >
			source_weapons.tertiary_bank_capacity ||
		Player_ship->num_swarm_missiles_to_fire < 0 ||
		Player_ship->num_swarm_missiles_to_fire >
			std::numeric_limits<std::uint16_t>::max()) {
		return {Phase2SourceReadStatus::UnsupportedEngineState};
	}
	if (source_weapons.num_primary_banks >
			static_cast<int>(MaximumPhase2WeaponBanksPerFamily) ||
		source_weapons.num_secondary_banks >
			static_cast<int>(MaximumPhase2WeaponBanksPerFamily)) {
		return {Phase2SourceReadStatus::SourceLimitExceeded};
	}
	output.weapons.primary_bank_count =
		static_cast<std::uint8_t>(source_weapons.num_primary_banks);
	output.weapons.secondary_bank_count =
		static_cast<std::uint8_t>(source_weapons.num_secondary_banks);
	if (source_weapons.current_primary_bank < -1 ||
		source_weapons.current_primary_bank >= source_weapons.num_primary_banks ||
		source_weapons.current_secondary_bank < -1 ||
		source_weapons.current_secondary_bank >= source_weapons.num_secondary_banks ||
		source_weapons.current_tertiary_bank < -1 ||
		source_weapons.current_tertiary_bank >= source_weapons.num_tertiary_banks) {
		return {Phase2SourceReadStatus::UnsupportedEngineState};
	}
	output.weapons.current_primary_bank = source_weapons.current_primary_bank;
	output.weapons.current_secondary_bank = source_weapons.current_secondary_bank;
	output.weapons.previous_primary_bank = source_weapons.previous_primary_bank;
	output.weapons.previous_secondary_bank = source_weapons.previous_secondary_bank;
	output.weapons.targeting_laser_bank = Player_ship->targeting_laser_bank;
	output.weapons.targeting_laser_active =
		Player_ship->targeting_laser_bank >= 0 &&
		Player_ship->targeting_laser_objnum >= 0 &&
		Player_ship->targeting_laser_objnum < MAX_OBJECTS &&
		Objects[Player_ship->targeting_laser_objnum].type == OBJ_BEAM;
	output.weapons.swarm_remaining = static_cast<std::uint16_t>(
		std::max(Player_ship->num_swarm_missiles_to_fire, 0));
	output.weapons.swarm_secondary_bank = Player_ship->swarm_missile_bank;
	output.weapons.remote_detonaters_active =
		static_cast<std::uint32_t>(
			std::max(source_weapons.remote_detonaters_active, 0));
	const auto remote_remaining_ms =
		std::max(timestamp_until(source_weapons.detonate_weapon_time), 0);
	if (remote_remaining_ms > 3'600'000) {
		return {Phase2SourceReadStatus::UnsupportedEngineState};
	}
	output.weapons.remote_detonation_remaining_us =
		static_cast<std::uint64_t>(remote_remaining_ms) * 1000U;
	output.weapons.per_burst_rotation = source_weapons.per_burst_rot;
	output.weapons.raw_weapon_flags =
		(Player_ship->flags[Ship::Ship_Flags::Primary_linked]
			 ? protocol::WeaponGlobalFlagPrimaryLinked : 0U) |
		(Player_ship->flags[Ship::Ship_Flags::Secondary_dual_fire]
			 ? protocol::WeaponGlobalFlagSecondaryDouble : 0U) |
		(source_weapons.flags[Ship::Weapon_Flags::Primary_trigger_down]
			 ? protocol::WeaponGlobalFlagPrimaryTriggerHeld : 0U) |
		(source_weapons.flags[Ship::Weapon_Flags::Secondary_trigger_down]
			 ? protocol::WeaponGlobalFlagSecondaryTriggerHeld : 0U) |
		(Player_ship->flags[Ship::Ship_Flags::Primaries_locked]
			 ? protocol::WeaponGlobalFlagPrimaryLocked : 0U) |
		(Player_ship->flags[Ship::Ship_Flags::Secondaries_locked]
			 ? protocol::WeaponGlobalFlagSecondaryLocked : 0U) |
		(output.weapons.targeting_laser_active
			 ? protocol::WeaponGlobalFlagTargetingLaser : 0U) |
		(output.weapons.remote_detonaters_active > 0U
			 ? protocol::WeaponGlobalFlagRemoteDetonatorsActive : 0U) |
		(source_weapons.flags[Ship::Weapon_Flags::Beam_Free]
			 ? protocol::WeaponGlobalFlagBeamFree : 0U) |
		(source_weapons.flags[Ship::Weapon_Flags::Turret_Lock]
			 ? protocol::WeaponGlobalFlagBeamLocked : 0U);
	output.weapons.tertiary_bank_count =
		static_cast<std::uint8_t>(source_weapons.num_tertiary_banks);
	output.weapons.current_tertiary_bank =
		source_weapons.current_tertiary_bank;
	output.weapons.tertiary_bank = source_weapons.current_tertiary_bank;
	output.weapons.tertiary_ammunition_current = source_weapons.tertiary_bank_ammo;
	output.weapons.tertiary_ammunition_initial =
		source_weapons.tertiary_bank_start_ammo;
	output.weapons.tertiary_ammunition_capacity = source_weapons.tertiary_bank_capacity;
	const auto tertiary_remaining_ms =
		std::max(timestamp_until(source_weapons.next_tertiary_fire_stamp), 0);
	if (tertiary_remaining_ms > 86'400'000) {
		return {Phase2SourceReadStatus::UnsupportedEngineState};
	}
	output.weapons.tertiary_cooldown_remaining_us =
		static_cast<std::uint64_t>(tertiary_remaining_ms) * 1000U;
	const auto tertiary_rearm_ms =
		std::max(timestamp_until(source_weapons.tertiary_bank_rearm_time), 0);
	if (tertiary_rearm_ms > 3'600'000) {
		return {Phase2SourceReadStatus::UnsupportedEngineState};
	}
	output.weapons.tertiary_rearm_remaining_us =
		static_cast<std::uint64_t>(tertiary_rearm_ms) * 1000U;
	const auto copy_bank = [](std::size_t bank_index,
							   ShipWeaponBankFamily family,
							   int weapon_class,
							   int ammunition_current,
							   int ammunition_initial,
							   int fire_stamp,
							   ShipWeaponBankObservation& destination) noexcept {
		destination = {};
		if (weapon_class < 0 || weapon_class >= static_cast<int>(Weapon_info.size()) ||
			ammunition_current < 0 ||
			ammunition_current > std::numeric_limits<std::uint16_t>::max() ||
			ammunition_initial < 0 ||
			ammunition_initial > std::numeric_limits<std::uint16_t>::max()) {
			return false;
		}
		const auto remaining_ms = std::max(timestamp_until(fire_stamp), 0);
		if (remaining_ms > 86'400'000) {
			return false;
		}
		destination.source_bank_key = static_cast<std::uint32_t>(bank_index);
		destination.weapon_class_source_key.value =
			static_cast<std::uint32_t>(weapon_class + 1);
		destination.family = family;
		destination.ammunition_current = static_cast<std::uint16_t>(ammunition_current);
		destination.ammunition_initial = static_cast<std::uint16_t>(ammunition_initial);
		destination.cooldown_remaining_us =
			static_cast<std::uint64_t>(remaining_ms) * 1000U;
		return true;
	};
	for (std::size_t bank_index = 0U;
		 bank_index < output.weapons.primary_bank_count;
		 ++bank_index) {
		if (!copy_bank(bank_index,
				ShipWeaponBankFamily::Primary,
				source_weapons.primary_bank_weapons[bank_index],
				source_weapons.primary_bank_ammo[bank_index],
				source_weapons.primary_bank_start_ammo[bank_index],
				source_weapons.next_primary_fire_stamp[bank_index],
				output.weapons.primary_banks[bank_index])) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
		auto& primary_bank = output.weapons.primary_banks[bank_index];
		primary_bank.primary_slot = source_weapons.primary_next_slot[bank_index];
		primary_bank.primary_fire_point =
			static_cast<std::uint16_t>(
				source_weapons.primary_firepoint_next_to_fire_index[bank_index]);
		primary_bank.simultaneous_slots =
			static_cast<std::uint16_t>(
				source_weapons.primary_bank_slot_count[bank_index]);
		primary_bank.burst_counter = source_weapons.burst_counter[bank_index];
		primary_bank.burst_seed = static_cast<std::uint32_t>(
			source_weapons.burst_seed[bank_index]);
		primary_bank.substitution_pattern_index =
			static_cast<std::uint16_t>(
				source_weapons.primary_bank_substitution_pattern_index[bank_index]);
		const auto primary_rearm_ms = std::max(timestamp_until(
			source_weapons.primary_bank_rearm_time[bank_index]), 0);
		if (primary_rearm_ms > 3'600'000) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
		primary_bank.rearm_remaining_us =
			static_cast<std::uint64_t>(primary_rearm_ms) * 1000U;
		const auto weapon_index =
			source_weapons.primary_bank_weapons[bank_index];
		const auto& weapon = Weapon_info[weapon_index];
		auto effective_pattern = weapon.firing_pattern;
		if (ship_class.flags[Ship::Info_Flags::Dyn_primary_linking]) {
			const auto dynamic_index =
				source_weapons.dynamic_firing_pattern[bank_index];
			if (dynamic_index < 0 ||
				static_cast<std::size_t>(dynamic_index) >=
					ship_class.dyn_firing_patterns_allowed[
						bank_index].size()) {
				return {Phase2SourceReadStatus::UnsupportedEngineState};
			}
			effective_pattern =
				ship_class.dyn_firing_patterns_allowed[
					bank_index][dynamic_index];
		}
		const auto pattern_code =
			static_cast<std::uint8_t>(effective_pattern);
		if (pattern_code > 5U) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
		primary_bank.firing_pattern_source_code = pattern_code;
		if (weapon.fof_reset_rate > 0.0F) {
			const auto duration =
				static_cast<double>(
					source_weapons.primary_bank_fof_cooldown[bank_index]) /
				static_cast<double>(weapon.fof_reset_rate) * 1'000'000.0;
			if (!std::isfinite(duration) || duration < 0.0 ||
				duration > 3'600'000'000.0) {
				return {Phase2SourceReadStatus::UnsupportedEngineState};
			}
			primary_bank.fof_cooldown_remaining_us =
				static_cast<std::uint64_t>(duration);
		}
		if (weapon.wi_flags[Weapon::Info_Flags::Beam] &&
			weapon.b_info.beam_type == BeamType::NORMAL_FIRE) {
			output.weapons.per_burst_rotation_active = true;
		}
		primary_bank.weapon_animation =
			static_cast<std::int32_t>(source_weapons.primary_animation_position[bank_index]);
	}
	for (std::size_t bank_index = 0U;
		 bank_index < output.weapons.secondary_bank_count;
		 ++bank_index) {
		if (!copy_bank(bank_index,
				ShipWeaponBankFamily::Secondary,
				source_weapons.secondary_bank_weapons[bank_index],
				source_weapons.secondary_bank_ammo[bank_index],
				source_weapons.secondary_bank_start_ammo[bank_index],
				source_weapons.next_secondary_fire_stamp[bank_index],
				output.weapons.secondary_banks[bank_index])) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
		auto& secondary_bank = output.weapons.secondary_banks[bank_index];
		secondary_bank.secondary_slot = source_weapons.secondary_next_slot[bank_index];
		secondary_bank.burst_counter =
			source_weapons.burst_counter[bank_index + MAX_SHIP_PRIMARY_BANKS];
		secondary_bank.burst_seed = static_cast<std::uint32_t>(
			source_weapons.burst_seed[
				bank_index + MAX_SHIP_PRIMARY_BANKS]);
		secondary_bank.substitution_pattern_index =
			static_cast<std::uint16_t>(
				source_weapons.secondary_bank_substitution_pattern_index[
					bank_index]);
		const auto secondary_rearm_ms = std::max(timestamp_until(
			source_weapons.secondary_bank_rearm_time[bank_index]), 0);
		if (secondary_rearm_ms > 3'600'000) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
		secondary_bank.rearm_remaining_us =
			static_cast<std::uint64_t>(secondary_rearm_ms) * 1000U;
		secondary_bank.weapon_animation =
			static_cast<std::int32_t>(source_weapons.secondary_animation_position[bank_index]);
	}
	if (Player_ship->cmeasure_count < 0 ||
		Player_ship->cmeasure_count > std::numeric_limits<std::uint16_t>::max() ||
		Player_ship->current_cmeasure < -1 ||
		Player_ship->current_cmeasure >= static_cast<int>(Weapon_info.size())) {
		return {Phase2SourceReadStatus::UnsupportedEngineState};
	}
	if (ship_class.cmeasure_max > 0) {
		if (ship_class.cmeasure_type < 0 ||
			ship_class.cmeasure_type >=
				static_cast<int>(Weapon_info.size()) ||
			(Player_ship->current_cmeasure >= 0 &&
			 Player_ship->current_cmeasure != ship_class.cmeasure_type)) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
		const auto& countermeasure =
			Weapon_info[ship_class.cmeasure_type];
		if (!countermeasure.wi_flags[Weapon::Info_Flags::Cmeasure] ||
			(Countermeasures_use_capacity &&
			 (!std::isfinite(countermeasure.cargo_size) ||
			  countermeasure.cargo_size <= 0.0F))) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
		output.weapons.presence |= protocol::WeaponStatePresenceFlagCountermeasure;
		output.weapons.countermeasure_count =
			static_cast<std::uint16_t>(Player_ship->cmeasure_count);
		output.weapons.countermeasure_class_source_key.value =
			static_cast<std::uint32_t>(ship_class.cmeasure_type + 1);
		output.weapons.countermeasures_enabled =
			Countermeasures_enabled != 0;
		const auto countermeasure_remaining_ms =
			std::max(timestamp_until(Player_ship->cmeasure_fire_stamp), 0);
		if (countermeasure_remaining_ms > 3'600'000) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
		output.weapons.countermeasure_cooldown_remaining_us =
			static_cast<std::uint64_t>(
				countermeasure_remaining_ms) * 1000U;
		const auto maximum = Countermeasures_use_capacity
			? static_cast<int>(std::floor(
				static_cast<double>(ship_class.cmeasure_max) /
				static_cast<double>(countermeasure.cargo_size)))
			: ship_class.cmeasure_max;
		if (maximum < 0 ||
			maximum > std::numeric_limits<std::uint16_t>::max()) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
		output.weapons.countermeasure_maximum =
			static_cast<std::uint16_t>(maximum);
		if (output.weapons.countermeasure_count >
			output.weapons.countermeasure_maximum) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
	} else if (Player_ship->cmeasure_count != 0) {
		return {Phase2SourceReadStatus::UnsupportedEngineState};
	}

	diagnostic_cursor.begin(Phase2CaptureBlock::SupportCargoDocking);
	if (Player_ship->ai_index >= 0 && Player_ship->ai_index < MAX_AI_INFO) {
		const auto& ship_ai = Ai_info[Player_ship->ai_index];
		output.support.raw_support_flags =
			(ship_ai.ai_flags[AI::AI_Flags::Awaiting_repair] ? 0x01U : 0U) |
			(ship_ai.ai_flags[AI::AI_Flags::Being_repaired] ? 0x02U : 0U) |
			(ship_ai.ai_flags[AI::AI_Flags::Repairing] ? 0x04U : 0U);
		const auto support_pair_is_well_formed =
			ship_ai.support_ship_objnum >= -1 &&
			ship_ai.support_ship_signature >= -1 &&
			((ship_ai.support_ship_objnum == -1) ==
				(ship_ai.support_ship_signature == -1));
		const auto support_object_is_valid =
			ship_ai.support_ship_objnum == -1 ||
			(ship_ai.support_ship_objnum >= 0 &&
			 ship_ai.support_ship_objnum < MAX_OBJECTS &&
			 Objects[ship_ai.support_ship_objnum].type == OBJ_SHIP &&
			 Objects[ship_ai.support_ship_objnum].signature ==
				 ship_ai.support_ship_signature &&
			 Objects[ship_ai.support_ship_objnum].instance >= 0 &&
			 Objects[ship_ai.support_ship_objnum].instance < MAX_SHIPS &&
			 Ships[Objects[ship_ai.support_ship_objnum].instance].objnum ==
				 ship_ai.support_ship_objnum);
		if (!support_pair_is_well_formed || !support_object_is_valid) {
			output.support = {};
		} else if (ship_ai.support_ship_objnum >= 0) {
			output.support.presence =
				protocol::SupportStatePresenceFlagSupportEntity;
			output.support.support_capture_key.value =
				static_cast<std::uint32_t>(ship_ai.support_ship_signature);
		}
		if (ship_ai.ai_flags[AI::AI_Flags::Awaiting_repair]) {
			output.support.phase =
				ship_ai.support_ship_objnum >= 0
				? ShipSupportPhase::OnWay
				: ShipSupportPhase::Queued;
		}
	}

	const auto docking_result =
		read_direct_docking_facts(*Player_obj, output.docking);
	if (docking_result.status != Phase2SourceReadStatus::Valid) {
		output.docking = {};
	} else if (Player_ship->ai_index >= 0 &&
		Player_ship->ai_index < MAX_AI_INFO) {
		const auto& ship_ai = Ai_info[Player_ship->ai_index];
		if (ship_ai.mode == AIM_DOCK &&
			ship_ai.submode >= AIS_UNDOCK_0 &&
			ship_ai.submode <= AIS_UNDOCK_4) {
			output.docking.phase = protocol::DockingPhase::Undocking;
		} else if (output.docking.relation_count != 0U) {
			output.docking.phase = protocol::DockingPhase::Docked;
		} else if (ship_ai.mode == AIM_DOCK &&
			ship_ai.submode >= AIS_DOCK_2 &&
			ship_ai.submode <= AIS_DOCK_4A) {
			output.docking.phase = protocol::DockingPhase::Docking;
		} else if (ship_ai.mode == AIM_DOCK &&
			ship_ai.submode >= AIS_DOCK_0 &&
			ship_ai.submode <= AIS_DOCK_1) {
			output.docking.phase = protocol::DockingPhase::Approach;
		}
	}
	}

	diagnostic_cursor.begin(Phase2CaptureBlock::Subsystems);
	if (ship_class.n_subsystems < 0 ||
		(ship_class.n_subsystems > 0 && ship_class.subsystems == nullptr) ||
		ship_class.n_subsystems > static_cast<int>(MaximumPhase2SubsystemsPerShip)) {
		return ship_class.n_subsystems > static_cast<int>(MaximumPhase2SubsystemsPerShip)
			? SourceReadResult{Phase2SourceReadStatus::SourceLimitExceeded}
			: SourceReadResult{Phase2SourceReadStatus::UnsupportedEngineState};
	}
	output.subsystems.count = static_cast<std::uint16_t>(ship_class.n_subsystems);
	if (output.subsystems.count > 0U && Player_ship->subsys_list_indexer == nullptr) {
		return {Phase2SourceReadStatus::UnsupportedEngineState};
	}
	for (std::size_t source_key = 0U; source_key < output.subsystems.count; ++source_key) {
		auto* subsystem = Player_ship->subsys_list_indexer[source_key];
		if (subsystem == nullptr ||
			subsystem->system_info != &ship_class.subsystems[source_key]) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
		auto& destination = output.subsystems.values[source_key];
		destination = {};
		destination.source_key.value =
			static_cast<std::uint32_t>(source_key + 1U);
		switch (subsystem->system_info->type) {
		case SUBSYSTEM_ENGINE:
			destination.kind = ShipSubsystemKind::Engine;
			break;
		case SUBSYSTEM_TURRET:
			destination.kind = ShipSubsystemKind::Turret;
			break;
		case SUBSYSTEM_WEAPONS:
			destination.kind = ShipSubsystemKind::Weapon;
			break;
		case SUBSYSTEM_SENSORS:
			destination.kind = ShipSubsystemKind::Sensors;
			break;
		case SUBSYSTEM_RADAR:
			destination.kind = ShipSubsystemKind::Radar;
			break;
		case SUBSYSTEM_COMMUNICATION:
			destination.kind = ShipSubsystemKind::Communication;
			break;
		case SUBSYSTEM_NAVIGATION:
			destination.kind = ShipSubsystemKind::Navigation;
			break;
		case SUBSYSTEM_NONE:
		case SUBSYSTEM_UNKNOWN:
			destination.kind = ShipSubsystemKind::Generic;
			break;
		case SUBSYSTEM_SOLAR:
			destination.kind = ShipSubsystemKind::Reactor;
			break;
		case SUBSYSTEM_GAS_COLLECT:
		case SUBSYSTEM_ACTIVATION:
			destination.kind = ShipSubsystemKind::Other;
			break;
		default:
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
		destination.hits_current = subsystem->current_hits;
		destination.hits_maximum = subsystem->max_hits;
		destination.position_local = {0.0F, 0.0F, 0.0F};
		destination.raw_flags = protocol::SubsystemFlagNone;
		const auto perturbed = !timestamp_elapsed(subsystem->disruption_timestamp);
		if (perturbed) {
			destination.raw_flags |= protocol::SubsystemFlagPerturbed;
			destination.presence |= protocol::SubsystemStatePresenceFlagPerturbation;
		}
		if (!subsystem->flags[Ship::Subsystem_Flags::Untargetable])
			destination.raw_flags |= protocol::SubsystemFlagTargetable;
		if (subsystem->submodel_instance_1 != nullptr &&
			!subsystem->submodel_instance_1->blown_off)
			destination.raw_flags |= protocol::SubsystemFlagVisible;
		if (subsystem->flags[Ship::Subsystem_Flags::Cargo_revealed])
			destination.raw_flags |= protocol::SubsystemFlagRevealed;
		if (subsystem->subsys_guardian_threshold > 0)
			destination.raw_flags |= protocol::SubsystemFlagGuardian;
		if (Player_ship->flags[Ship::Ship_Flags::Subsystem_movement_locked])
			destination.raw_flags |= protocol::SubsystemFlagMovementLocked;
		if (subsystem->weapons.flags[Ship::Weapon_Flags::Beam_Free])
			destination.raw_flags |= protocol::SubsystemFlagBeamFree;
		if (subsystem->weapons.flags[Ship::Weapon_Flags::Turret_Lock])
			destination.raw_flags |= protocol::SubsystemFlagBeamLocked;
		if (subsystem->armor_type_idx < -1 ||
			subsystem->armor_type_idx >= static_cast<int>(Armor_types.size())) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
		destination.armor_source_key.value =
			subsystem->armor_type_idx >= 0
			? static_cast<std::uint32_t>(subsystem->armor_type_idx + 1)
			: 0U;
		if (destination.armor_source_key.value != 0U)
			destination.presence |= protocol::SubsystemStatePresenceFlagArmor;
		const auto disruption_remaining_ms =
			std::max(timestamp_until(subsystem->disruption_timestamp), 0);
		if (disruption_remaining_ms > 86'400'000) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
		destination.disruption_remaining_us =
			static_cast<std::uint64_t>(disruption_remaining_ms) * 1000U;
		const auto aggregate_index = subsystem->system_info->type;
		if (aggregate_index < 0 || aggregate_index >= SUBSYSTEM_MAX) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
		destination.aggregate_current_hits =
			Player_ship->subsys_info[aggregate_index].aggregate_current_hits;
		destination.aggregate_maximum_hits =
			Player_ship->subsys_info[aggregate_index].aggregate_max_hits;
		if (!subsystem->flags[Ship::Subsystem_Flags::No_aggregate] &&
			Player_ship->subsys_info[aggregate_index].type_count > 0)
			destination.presence |= protocol::SubsystemStatePresenceFlagTypeAggregate;
		if (subsystem->submodel_instance_1 != nullptr) {
			const auto& transform = *subsystem->submodel_instance_1;
			destination.position_local = {
				transform.canonical_offset.xyz.x,
				transform.canonical_offset.xyz.y,
				transform.canonical_offset.xyz.z};
			const CaptureOrientationBasis subsystem_basis{
				{transform.canonical_orient.vec.rvec.xyz.x,
					transform.canonical_orient.vec.rvec.xyz.y,
					transform.canonical_orient.vec.rvec.xyz.z},
				{transform.canonical_orient.vec.uvec.xyz.x,
					transform.canonical_orient.vec.uvec.xyz.y,
					transform.canonical_orient.vec.uvec.xyz.z},
				{transform.canonical_orient.vec.fvec.xyz.x,
					transform.canonical_orient.vec.fvec.xyz.y,
					transform.canonical_orient.vec.fvec.xyz.z}};
			CaptureQuaternionf subsystem_orientation;
			if (convert_fso_orientation_to_local_to_world(
					subsystem_basis, subsystem_orientation) !=
				QuaternionConversionStatus::Converted) {
				return {Phase2SourceReadStatus::UnsupportedEngineState};
			}
			destination.orientation_local = {subsystem_orientation.w,
				subsystem_orientation.x,
				subsystem_orientation.y,
				subsystem_orientation.z};
			const auto has_offset =
				destination.position_local[0] != 0.0F ||
				destination.position_local[1] != 0.0F ||
				destination.position_local[2] != 0.0F;
			const auto has_orientation =
				destination.orientation_local[0] != 1.0F ||
				destination.orientation_local[1] != 0.0F ||
				destination.orientation_local[2] != 0.0F ||
				destination.orientation_local[3] != 0.0F;
			if (has_offset || has_orientation) {
				destination.presence |=
					protocol::SubsystemStatePresenceFlagAnimatedTransform;
			}
		}
		if (core_gate ||
			destination.kind != ShipSubsystemKind::Turret) {
			continue;
		}
		destination.presence |=
			protocol::SubsystemStatePresenceFlagTurret;
		destination.turret.emplace();
		auto& turret = *destination.turret;
		const auto& turret_weapons = subsystem->weapons;
		if (turret_weapons.num_primary_banks < 0 ||
			turret_weapons.num_secondary_banks < 0 ||
			turret_weapons.num_primary_banks > MAX_SHIP_PRIMARY_BANKS ||
			turret_weapons.num_secondary_banks > MAX_SHIP_SECONDARY_BANKS) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
		if (turret_weapons.num_primary_banks >
				static_cast<int>(MaximumPhase2WeaponBanksPerFamily) ||
			turret_weapons.num_secondary_banks >
				static_cast<int>(MaximumPhase2WeaponBanksPerFamily)) {
			return {Phase2SourceReadStatus::SourceLimitExceeded};
		}
		turret.turret_primary_bank_count =
			static_cast<std::uint8_t>(turret_weapons.num_primary_banks);
		turret.turret_secondary_bank_count =
			static_cast<std::uint8_t>(turret_weapons.num_secondary_banks);
		for (std::size_t bank = 0U; bank < turret.turret_primary_bank_count; ++bank) {
			if (turret_weapons.primary_bank_weapons[bank] < 0 ||
				turret_weapons.primary_bank_weapons[bank] >=
					static_cast<int>(Weapon_info.size()) ||
				turret_weapons.primary_bank_ammo[bank] < 0 ||
				turret_weapons.primary_bank_capacity[bank] < 0 ||
				turret_weapons.primary_bank_ammo[bank] >
					turret_weapons.primary_bank_capacity[bank]) {
				return {Phase2SourceReadStatus::UnsupportedEngineState};
			}
			turret.turret_primary_bank_weapon_source_keys[bank].value =
				static_cast<std::uint32_t>(
					turret_weapons.primary_bank_weapons[bank] + 1);
			auto& turret_bank = turret.turret_primary_banks[bank];
			turret_bank.turret_ammunition_current =
				turret_weapons.primary_bank_ammo[bank];
			turret_bank.turret_ammunition_capacity =
				turret_weapons.primary_bank_capacity[bank];
			const auto cooldown_ms =
				std::max(timestamp_until(turret_weapons.next_primary_fire_stamp[bank]), 0);
			if (cooldown_ms > 86'400'000) {
				return {Phase2SourceReadStatus::UnsupportedEngineState};
			}
			turret_bank.turret_cooldown_remaining_us =
				static_cast<std::uint64_t>(cooldown_ms) * 1000U;
		}
		for (std::size_t bank = 0U; bank < turret.turret_secondary_bank_count; ++bank) {
			if (turret_weapons.secondary_bank_weapons[bank] < 0 ||
				turret_weapons.secondary_bank_weapons[bank] >=
					static_cast<int>(Weapon_info.size()) ||
				turret_weapons.secondary_bank_ammo[bank] < 0 ||
				turret_weapons.secondary_bank_capacity[bank] < 0 ||
				turret_weapons.secondary_bank_ammo[bank] >
					turret_weapons.secondary_bank_capacity[bank]) {
				return {Phase2SourceReadStatus::UnsupportedEngineState};
			}
			turret.turret_secondary_bank_weapon_source_keys[bank].value =
				static_cast<std::uint32_t>(
					turret_weapons.secondary_bank_weapons[bank] + 1);
			auto& turret_bank = turret.turret_secondary_banks[bank];
			turret_bank.turret_ammunition_current =
				turret_weapons.secondary_bank_ammo[bank];
			turret_bank.turret_ammunition_capacity =
				turret_weapons.secondary_bank_capacity[bank];
			const auto cooldown_ms =
				std::max(timestamp_until(turret_weapons.next_secondary_fire_stamp[bank]), 0);
			if (cooldown_ms > 86'400'000) {
				return {Phase2SourceReadStatus::UnsupportedEngineState};
			}
			turret_bank.turret_cooldown_remaining_us =
				static_cast<std::uint64_t>(cooldown_ms) * 1000U;
		}
		turret.turret_next_fire_pos = subsystem->turret_next_fire_pos;
		const auto turret_remaining_ms =
			std::max(timestamp_until(subsystem->turret_next_fire_stamp), 0);
		if (turret_remaining_ms > 86'400'000) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
		turret.turret_next_fire_remaining_us =
			static_cast<std::uint64_t>(turret_remaining_ms) * 1000U;
		vec3d turret_current_direction;
		model_instance_local_to_global_dir(&turret_current_direction,
			&subsystem->system_info->turret_norm,
			Player_ship->model_instance_num,
			subsystem->system_info->subobj_num,
			&vmd_identity_matrix,
			true);
		if (vm_vec_normalize(&turret_current_direction) == 0.0F) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
		turret.turret_current_direction_local = {
			turret_current_direction.xyz.x,
			turret_current_direction.xyz.y,
			turret_current_direction.xyz.z};
		if (subsystem->system_info->turret_num_firing_points < 0 ||
			subsystem->system_info->turret_num_firing_points > 64) {
			return {
				subsystem->system_info->turret_num_firing_points > 64
				? Phase2SourceReadStatus::SourceLimitExceeded
				: Phase2SourceReadStatus::UnsupportedEngineState};
		}
		turret.turret_firing_point_count =
			static_cast<std::uint16_t>(
				subsystem->system_info->turret_num_firing_points);
		turret.turret_rof_scaler = subsystem->rof_scaler;
		turret.turret_animation =
			static_cast<std::int32_t>(subsystem->turret_animation_position);
		const auto animation_remaining_ms = std::max(
			timestamp_until(subsystem->turret_animation_done_time), 0);
		if (animation_remaining_ms > 86'400'000) {
			return {Phase2SourceReadStatus::UnsupportedEngineState};
		}
		turret.turret_animation_remaining_us =
			static_cast<std::uint64_t>(animation_remaining_ms) * 1000U;
		turret.turret_beam_free =
			turret_weapons.flags[Ship::Weapon_Flags::Beam_Free];
		turret.turret_locked =
			turret_weapons.flags[Ship::Weapon_Flags::Turret_Lock];
	}

	if (!core_gate) {
	diagnostic_cursor.begin(Phase2CaptureBlock::SupportCargoDocking);
	SupportWorkEvaluation support_work;
	if (evaluate_support_work(Player_obj, support_work) !=
		SupportWorkStatus::Valid) {
		return {Phase2SourceReadStatus::UnsupportedEngineState};
	}
	if (support_work.countermeasure_capacity >
		std::numeric_limits<std::uint16_t>::max()) {
		return {Phase2SourceReadStatus::UnsupportedEngineState};
	}
	output.weapons.countermeasure_maximum =
		static_cast<std::uint16_t>(support_work.countermeasure_capacity);
	output.support.raw_support_repairs_hull_authorized =
		support_work.support_repairs_hull_authorized;
	output.support.raw_mission_rearm_disallowed =
		support_work.mission_rearm_disallowed;
	output.support.raw_weapon_rearm_disallowed =
		support_work.weapon_rearm_disallowed;
	output.support.raw_max_hull_repair_fraction =
		support_work.max_hull_repair_fraction;
	output.support.raw_max_subsystem_repair_fraction =
		support_work.max_subsystem_repair_fraction;
	output.support.raw_hull_repair_rate = ship_class.sup_hull_repair_rate;
	output.support.raw_shield_repair_rate = ship_class.sup_shield_repair_rate;
	output.support.raw_subsystem_repair_rate = ship_class.sup_subsys_repair_rate;
	output.support.raw_hull_repair_work =
		support_work.hull_repair_work;
	output.support.raw_shield_repair_work =
		support_work.shield_repair_work;
	output.support.raw_subsystem_repair_work =
		support_work.subsystem_repair_work;
	output.support.raw_weapon_energy_rearm_work =
		support_work.weapon_energy_rearm_work;
	output.support.raw_ammunition_rearm_work =
		support_work.ammunition_rearm_work;
	output.support.raw_countermeasure_rearm_work =
		support_work.countermeasure_rearm_work;
	output.support.raw_countermeasure_capacity =
		support_work.countermeasure_capacity;
	output.support.raw_countermeasure_rearm_pool =
		support_work.countermeasure_rearm_pool;
	output.support.raw_hull_repair_applicable =
		support_work.hull_repair_applicable;
	output.support.raw_shield_repair_applicable =
		support_work.shield_repair_applicable;
	output.support.raw_subsystem_repair_applicable =
		support_work.subsystem_repair_applicable;
	output.support.raw_weapon_energy_rearm_applicable =
		support_work.weapon_energy_rearm_applicable;
	output.support.raw_ammunition_rearm_applicable =
		support_work.ammunition_rearm_applicable;
	output.support.raw_countermeasure_rearm_applicable =
		support_work.countermeasure_rearm_applicable;
	if (Player_ship->ai_index >= 0 &&
		Player_ship->ai_index < MAX_AI_INFO) {
		const auto& ship_ai = Ai_info[Player_ship->ai_index];
		if (ship_ai.ai_flags[AI::AI_Flags::Being_repaired]) {
			const auto repair_remaining =
				support_work.hull_repair_applicable ||
				support_work.shield_repair_applicable ||
				support_work.subsystem_repair_applicable;
			const auto rearm_remaining =
				support_work.weapon_energy_rearm_applicable ||
				support_work.ammunition_rearm_applicable ||
				support_work.countermeasure_rearm_applicable;
			output.support.phase = repair_remaining
				? ShipSupportPhase::Repairing
				: rearm_remaining
				? ShipSupportPhase::Rearming
				: ShipSupportPhase::Docking;
		}
	}
	const auto terminal = phase2_latest_support_terminal(
		static_cast<std::uint32_t>(Player_obj->signature));
	if (terminal.assisted_signature != 0U) {
		output.support.episode_sequence =
			terminal.episode_sequence;
		switch (terminal.reason) {
		case SupportTransitionReason::Broken:
			output.support.phase = ShipSupportPhase::Obstructed;
			break;
		case SupportTransitionReason::Abort:
		case SupportTransitionReason::Killed:
			output.support.phase = ShipSupportPhase::Aborted;
			break;
		case SupportTransitionReason::End:
		case SupportTransitionReason::Complete:
			output.support.phase = ShipSupportPhase::None;
			if (output.support.support_capture_key.value == 0U)
				output.support.presence =
					protocol::SupportStatePresenceFlagNone;
			break;
		case SupportTransitionReason::Queue:
		case SupportTransitionReason::OnWay:
		case SupportTransitionReason::Begin:
		case SupportTransitionReason::Count:
			break;
		}
	}
	}
	diagnostic_cursor.begin(Phase2CaptureBlock::Identity);
	const auto static_result = extract_production_static_authorities(
		*Player_obj, *Player_ship, ship_class, output, core_gate);
	if (static_result.status != Phase2SourceReadStatus::Valid) {
		output.raw_static_catalog.clear();
		output.raw_static_references = {};
		return static_result;
	}
	return {Phase2SourceReadStatus::Valid};
}

SourceReadResult FsoEngineReadView::read_ship(
	EngineEntityKey key, Phase2ShipSource& output) const noexcept
{
	return read_ship_for_projection(key, output, false);
}

SourceReadResult FsoEngineReadView::read_ship_flight(
	EngineEntityKey key, ShipFlightObservation& output) const noexcept
{
	output = {};
	if (!player_source_is_consistent() || key.object_index < 0 ||
		key.object_index >= MAX_OBJECTS)
		return {Phase2SourceReadStatus::InvalidSource};
	const auto& object = Objects[key.object_index];
	if (object.type != OBJ_SHIP || object.signature <= 0 ||
		key.object_signature != static_cast<std::uint32_t>(object.signature) ||
		object.instance < 0 || object.instance >= MAX_SHIPS ||
		Ships[object.instance].objnum != key.object_index)
		return {Phase2SourceReadStatus::InvalidSource};
	output.position_world =
		{object.pos.xyz.x, object.pos.xyz.y, object.pos.xyz.z};
	const CaptureOrientationBasis basis{
		{object.orient.vec.rvec.xyz.x, object.orient.vec.rvec.xyz.y,
			object.orient.vec.rvec.xyz.z},
		{object.orient.vec.uvec.xyz.x, object.orient.vec.uvec.xyz.y,
			object.orient.vec.uvec.xyz.z},
		{object.orient.vec.fvec.xyz.x, object.orient.vec.fvec.xyz.y,
			object.orient.vec.fvec.xyz.z}};
	CaptureQuaternionf orientation;
	if (convert_fso_orientation_to_local_to_world(basis, orientation) !=
		QuaternionConversionStatus::Converted)
		return {Phase2SourceReadStatus::UnsupportedEngineState};
	output.orientation_local_to_world =
		{orientation.w, orientation.x, orientation.y, orientation.z};
	output.velocity_world = {object.phys_info.vel.xyz.x,
		object.phys_info.vel.xyz.y, object.phys_info.vel.xyz.z};
	output.rotational_velocity_local = {object.phys_info.rotvel.xyz.x,
		object.phys_info.rotvel.xyz.y, object.phys_info.rotvel.xyz.z};
	output.radius = object.radius;
	const EnginePhysicsFlagInput physics{
		static_cast<std::uint32_t>(object.phys_info.flags),
		object.flags[Object::Object_Flags::Immobile],
		object.flags[Object::Object_Flags::Dont_change_position],
		object.flags[Object::Object_Flags::Dont_change_orientation]};
	output.physics_mode_flags = map_player_physics_mode_flags(physics);
	return {Phase2SourceReadStatus::Valid};
}

SourceReadResult FsoEngineReadView::read_core_gate_ship(
	EngineEntityKey key, Phase2ShipSource& output) const noexcept
{
	return read_ship_for_projection(key, output, true);
}

SourceReadResult FsoEngineReadView::read_ship_diagnosed(
	EngineEntityKey key,
	Phase2ShipSource& output,
	Phase2CaptureDiagnostics& diagnostics) const noexcept
{
	auto* previous = ActivePhase2CaptureDiagnostics;
	ActivePhase2CaptureDiagnostics = &diagnostics;
	const auto result = read_ship(key, output);
	if (result.status == Phase2SourceReadStatus::Valid)
		diagnostics.primary_failed_block = Phase2CaptureBlock::Count;
	ActivePhase2CaptureDiagnostics = previous;
	return result;
}

SourceReadResult FsoEngineReadView::read_core_gate_ship_diagnosed(
	EngineEntityKey key, Phase2ShipSource& output,
	Phase2CaptureDiagnostics& diagnostics) const noexcept
{
	auto* previous = ActivePhase2CaptureDiagnostics;
	ActivePhase2CaptureDiagnostics = &diagnostics;
	const auto result = read_core_gate_ship(key, output);
	if (result.status == Phase2SourceReadStatus::Valid)
		diagnostics.primary_failed_block =
			Phase2CaptureBlock::Count;
	ActivePhase2CaptureDiagnostics = previous;
	return result;
}

bool FsoEngineReadView::read_player_controls(PlayerControlObservation& output) const noexcept
{
	output = {};
	if (!player_source_is_consistent()) {
		return false;
	}
	const auto handoff = phase2_seam_handoff_snapshot();
	if (!handoff.has_control_target ||
		static_cast<std::uint8_t>(handoff.control_target) >=
			static_cast<std::uint8_t>(ControlTargetAuthority::Count)) {
		return false;
	}
	const auto& ship_class = Ship_info[Player_ship->ship_info_index];

	output.mode = handoff.control_target == ControlTargetAuthority::Ship
		? PlayerControlModeObservation::Ship
		: PlayerControlModeObservation::Camera;
	output.pitch = Player->ci.pitch;
	output.heading = Player->ci.heading;
	output.bank = Player->ci.bank;
	output.forward = Player->ci.forward;
	output.sideways = Player->ci.sideways;
	output.vertical = Player->ci.vertical;
	output.presence = protocol::ControlStatePresenceFlagCruise |
		protocol::ControlStatePresenceFlagRequestCounters;
	output.forward_cruise_percent = Player->ci.forward_cruise_percent;
	if (Player->ci.fire_primary_count < 0 || Player->ci.fire_primary_count > 65535 ||
		Player->ci.fire_secondary_count < 0 || Player->ci.fire_secondary_count > 65535 ||
		Player->ci.fire_countermeasure_count < 0 ||
		Player->ci.fire_countermeasure_count > 65535) {
		return false;
	}
	output.fire_primary_count = static_cast<std::uint16_t>(Player->ci.fire_primary_count);
	output.fire_secondary_count = static_cast<std::uint16_t>(Player->ci.fire_secondary_count);
	output.fire_countermeasure_count =
		static_cast<std::uint16_t>(Player->ci.fire_countermeasure_count);
	output.autopilot_engaged = AutoPilotEngaged;
	output.player_use_ai = Player_use_ai;
	output.engine_control_mode = Player->control_mode;
	output.flight_cursor_active =
		Player_flight_mode == FlightMode::FlightCursor || ship_class.aims_at_flight_cursor;
	output.flight_cursor_pitch = Player_flight_cursor.p;
	output.flight_cursor_heading = Player_flight_cursor.h;
	output.flight_cursor_sensitivity = Player_flight_cursor_sensitivity;
	output.effective_aim_extent = ship_class.flight_cursor_aim_extent > 0.0F
		? ship_class.flight_cursor_aim_extent
		: Flight_cursor_extent;
	output.flight_cursor_deadzone_extent = Flight_cursor_deadzone;
	if (output.flight_cursor_active) {
		output.presence |=
			protocol::ControlStatePresenceFlagFlightCursor;
	}
	output.afterburner_requested = Control_config[AFTERBURNER].continuous_ongoing;
	if ((Player->flags & PLAYER_FLAGS_MATCH_TARGET) != 0) {
		output.action_flags |= protocol::ControlFlagMatchSpeed;
	}
	if ((Player->flags & PLAYER_FLAGS_AUTO_TARGETING) != 0) {
		output.action_flags |= protocol::ControlFlagAutoTarget;
	}
	if ((Player->flags & PLAYER_FLAGS_AUTO_MATCH_SPEED) != 0) {
		output.action_flags |= protocol::ControlFlagAutoMatchSpeed;
	}
	if (Player_ship->flags[Ship::Ship_Flags::Primary_linked]) {
		output.action_flags |= protocol::ControlFlagPrimaryLinked;
	}
	if (Player_ship->flags[Ship::Ship_Flags::Secondary_dual_fire]) {
		output.action_flags |= protocol::ControlFlagSecondaryDouble;
	}
	return true;
}

bool FsoEngineReadView::read_player_cargo_scan(PlayerCargoScanObservation& output) const noexcept
{
	output = {};
	if (!player_source_is_consistent()) {
		return false;
	}
	const auto handoff = phase2_seam_handoff_snapshot();
	if (!handoff.has_cargo_authority) {
		output.phase = CargoScanPhaseObservation::NotScannable;
		output.presence = protocol::CargoScanStatePresenceFlagNone;
		output.validity_flags = protocol::ScanValidityFlagNone;
		return true;
	}
	const auto& cargo_authority = handoff.cargo_authority;
	if (!cargo_authority.source_valid ||
		cargo_authority.player_signature != static_cast<std::uint32_t>(Player_obj->signature) ||
		Player_ai == nullptr) {
		return false;
	}
	const auto target_signature = cargo_authority.target_signature;
	const object* target_object = nullptr;
	if ((cargo_authority.presence & protocol::CargoScanStatePresenceFlagTarget) != 0U) {
		if (Player_ai->target_objnum < 0 || Player_ai->target_objnum >= MAX_OBJECTS) {
			return false;
		}
		target_object = &Objects[Player_ai->target_objnum];
		if (target_object->type != OBJ_SHIP || target_object->signature <= 0 ||
			static_cast<std::uint32_t>(target_object->signature) != target_signature ||
			target_object->instance < 0 ||
			target_object->instance >= MAX_SHIPS) {
			return false;
		}
		const auto& target_ship = Ships[target_object->instance];
		if (target_ship.objnum < 0 || target_ship.objnum >= MAX_OBJECTS ||
			&Objects[target_ship.objnum] != target_object) {
			return false;
		}
	}
	if ((cargo_authority.presence & protocol::CargoScanStatePresenceFlagSubsystem) != 0U) {
		if (target_object == nullptr) {
			return false;
		}
		const auto& target_ship = Ships[target_object->instance];
		if (target_ship.ship_info_index < 0 ||
			target_ship.ship_info_index >= static_cast<int>(Ship_info.size())) {
			return false;
		}
		const auto& target_class = Ship_info[target_ship.ship_info_index];
		if (cargo_authority.target_subsystem_source_key >=
			static_cast<std::uint32_t>(target_class.n_subsystems)) {
			return false;
		}
		const auto* expected_system_info =
			&target_class.subsystems[cargo_authority.target_subsystem_source_key];
		if (Player_ai->targeted_subsys == nullptr ||
			Player_ai->targeted_subsys->system_info != expected_system_info) {
			return false;
		}
	}
	output.presence = cargo_authority.presence;
	output.phase = cargo_authority.phase;
	output.target_capture_key.value = target_signature;
	output.target_subsystem_source_key.value =
		(cargo_authority.presence &
			protocol::CargoScanStatePresenceFlagSubsystem) != 0U
		? cargo_authority.target_subsystem_source_key + 1U
		: 0U;
	output.elapsed_us = cargo_authority.elapsed_us;
	output.required_us = cargo_authority.required_us;
	output.validity_flags = cargo_authority.validity_flags;
	output.cargo_text = cargo_authority.cargo_text;
	return true;
}

bool FsoEngineReadView::player_object_is_ship() const noexcept
{
	return Player_obj != nullptr && Player_obj->type == OBJ_SHIP;
}

bool FsoEngineReadView::player_object_ship_instance_in_range() const noexcept
{
	return Player_obj != nullptr && Player_obj->type == OBJ_SHIP && Player_obj->instance >= 0 &&
		Player_obj->instance < MAX_SHIPS;
}

bool FsoEngineReadView::player_object_matches_player() const noexcept
{
	return Player != nullptr && Player_obj != nullptr && Player->objnum >= 0 && Player->objnum < MAX_OBJECTS &&
		&Objects[Player->objnum] == Player_obj;
}

bool FsoEngineReadView::player_ship_matches_object() const noexcept
{
	if (Player_obj == nullptr || Player_ship == nullptr || Player_obj->type != OBJ_SHIP ||
		Player_obj->instance < 0 || Player_obj->instance >= MAX_SHIPS) {
		return false;
	}
	return &Ships[Player_obj->instance] == Player_ship && Player_ship->objnum >= 0 &&
		Player_ship->objnum < MAX_OBJECTS && &Objects[Player_ship->objnum] == Player_obj;
}

void FsoEngineReadView::read_player_kinematics(EnginePlayerKinematicsRead& output) const noexcept
{
	output = {};
	const auto* player = Player;
	const auto* object = Player_obj;
	const auto* ship = Player_ship;
	if ((Game_mode & GM_IN_MISSION) == 0 || player == nullptr || object == nullptr || ship == nullptr ||
		object->type != OBJ_SHIP || object->instance < 0 || object->instance >= MAX_SHIPS ||
		player->objnum < 0 || player->objnum >= MAX_OBJECTS || &Objects[player->objnum] != object ||
		&Ships[object->instance] != ship || ship->objnum < 0 || ship->objnum >= MAX_OBJECTS ||
		&Objects[ship->objnum] != object) {
		return;
	}

	output.object_signature = static_cast<std::int32_t>(object->signature);
	output.position_world = {object->pos.xyz.x, object->pos.xyz.y, object->pos.xyz.z};
	output.orientation.right_world =
		{object->orient.vec.rvec.xyz.x, object->orient.vec.rvec.xyz.y, object->orient.vec.rvec.xyz.z};
	output.orientation.up_world =
		{object->orient.vec.uvec.xyz.x, object->orient.vec.uvec.xyz.y, object->orient.vec.uvec.xyz.z};
	output.orientation.forward_world =
		{object->orient.vec.fvec.xyz.x, object->orient.vec.fvec.xyz.y, object->orient.vec.fvec.xyz.z};
	output.velocity_world =
		{object->phys_info.vel.xyz.x, object->phys_info.vel.xyz.y, object->phys_info.vel.xyz.z};
	output.rotational_velocity_local =
		{object->phys_info.rotvel.xyz.x, object->phys_info.rotvel.xyz.y, object->phys_info.rotvel.xyz.z};
	output.radius = object->radius;
	output.physics.raw_physics_flags = static_cast<std::uint32_t>(object->phys_info.flags);
	output.physics.object_immobile = object->flags[Object::Object_Flags::Immobile];
	output.physics.object_position_locked = object->flags[Object::Object_Flags::Dont_change_position];
	output.physics.object_orientation_locked = object->flags[Object::Object_Flags::Dont_change_orientation];
}

FsoEngineReadView make_fso_engine_read_view() noexcept
{
	return {};
}

CaptureResult collect_player_kinematics(const EngineReadView& view,
	std::uint64_t now_us,
	PlayerObservationDto& output) noexcept
{
	output = {};
	if (!view.in_mission()) return no_player(CaptureReason::NotInMission);
	if (!view.player_exists()) return no_player(CaptureReason::MissingPlayer);
	if (!view.player_object_exists()) return no_player(CaptureReason::MissingPlayerObject);
	if (!view.player_ship_exists()) return no_player(CaptureReason::MissingPlayerShip);
	if (!view.player_object_is_ship()) return invalid_source(CaptureReason::WrongObjectType);
	if (!view.player_object_ship_instance_in_range()) {
		return invalid_source(CaptureReason::ShipInstanceOutOfRange);
	}
	if (!view.player_object_matches_player()) return invalid_source(CaptureReason::PlayerObjectMismatch);
	if (!view.player_ship_matches_object()) return invalid_source(CaptureReason::PlayerShipMismatch);

	EnginePlayerKinematicsRead raw;
	view.read_player_kinematics(raw);
	if (raw.object_signature <= 0) return invalid_source(CaptureReason::InvalidObservationKey);
	if (!valid_vector(raw.position_world, 1.0e12f)) return invalid_source(CaptureReason::InvalidPosition);
	CaptureQuaternionf orientation;
	if (convert_fso_orientation_to_local_to_world(raw.orientation, orientation) !=
		QuaternionConversionStatus::Converted) {
		return invalid_source(CaptureReason::InvalidOrientation);
	}
	if (!valid_vector(raw.velocity_world, 1.0e9f)) return invalid_source(CaptureReason::InvalidVelocity);
	if (!valid_vector(raw.rotational_velocity_local, 1.0e6f)) {
		return invalid_source(CaptureReason::InvalidRotationalVelocity);
	}
	if (!std::isfinite(raw.radius) || raw.radius < 0.0f || raw.radius > 1.0e9f) {
		return invalid_source(CaptureReason::InvalidRadius);
	}

	PlayerObservationDto candidate;
	candidate.key.object_signature = static_cast<std::uint32_t>(raw.object_signature);
	candidate.value.producer_sample_time_us = now_us;
	candidate.value.position_world = raw.position_world;
	candidate.value.orientation_local_to_world = orientation;
	candidate.value.velocity_world = raw.velocity_world;
	candidate.value.rotational_velocity_local = raw.rotational_velocity_local;
	candidate.value.radius = raw.radius;
	candidate.value.physics_mode_flags = map_player_physics_mode_flags(raw.physics);
	canonicalize_zero(candidate.value.position_world);
	canonicalize_zero(candidate.value.orientation_local_to_world.w);
	canonicalize_zero(candidate.value.orientation_local_to_world.x);
	canonicalize_zero(candidate.value.orientation_local_to_world.y);
	canonicalize_zero(candidate.value.orientation_local_to_world.z);
	canonicalize_zero(candidate.value.velocity_world);
	canonicalize_zero(candidate.value.rotational_velocity_local);
	canonicalize_zero(candidate.value.radius);
	output = candidate;
	return {CaptureStatus::Valid, CaptureReason::None};
}

} // namespace telemetry::detail
