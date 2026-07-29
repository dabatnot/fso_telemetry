#pragma once

#include "telemetry/protocol/telemetry_protocol_constants.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace telemetry::detail {

constexpr std::size_t MaximumPhase2ObservationShips = 64U;
constexpr std::size_t MaximumPhase2InternalNameBytes = 511U;
constexpr std::size_t MaximumPhase2ShieldSegments = 64U;
constexpr std::size_t MaximumPhase2SubsystemsPerShip = 1024U;
constexpr std::size_t MaximumPhase2WeaponBanksPerFamily = 64U;
constexpr std::size_t MaximumPhase2DockRelationsPerShip = 64U;
constexpr std::size_t MaximumPhase2PhysicalPrimaryBanks = 3U;
constexpr std::size_t MaximumPhase2PhysicalSecondaryBanks = 4U;
constexpr std::size_t MaximumPhase2StaticClasses = 64U;
constexpr std::size_t MaximumPhase2StaticWeapons = 4096U;
constexpr std::size_t MaximumPhase2StaticSubsystems = 4096U;
constexpr std::size_t MaximumPhase2StaticBanksPerClass = 192U;
constexpr std::size_t MaximumPhase2StaticFirePoints = 64U;
constexpr std::size_t MaximumPhase2StaticAuxiliaryEntries = 256U;
constexpr std::size_t MaximumPhase2OwnedBytes = 64U * 1024U * 1024U;

enum class Phase2CaptureStatus : std::uint8_t {
	Valid = 0,
	NoPlayer,
	InvalidSource,
	UnsupportedEngineState,
	SourceLimitExceeded,
	Count,
};

enum class Phase2CaptureReason : std::uint8_t {
	None = 0,
	WrongThread,
	NotInMission,
	PartialPlayerSource,
	InconsistentPlayerSource,
	InvalidClosureCardinality,
	ShipReadFailure,
	DuplicateObjectSignature,
	StringLimitExceeded,
	AllocationFailure,
	BufferNotReady,
	UnsupportedShipBlock,
	Count,
};

struct Phase2CaptureResult {
	Phase2CaptureStatus status = Phase2CaptureStatus::InvalidSource;
	Phase2CaptureReason reason = Phase2CaptureReason::None;
};

struct EngineEntityKey {
	std::int32_t object_index = -1;
	std::uint32_t object_signature = 0U;
};

struct Phase2CaptureLocalKey {
	std::uint32_t value = 0U;
};

enum class Phase2SourceReadStatus : std::uint8_t {
	Valid = 0,
	InvalidSource,
	UnsupportedEngineState,
	SourceLimitExceeded,
	Count,
};

struct Phase2SourceReadResult {
	Phase2SourceReadStatus status = Phase2SourceReadStatus::InvalidSource;
};

using SourceReadResult = Phase2SourceReadResult;

struct Phase2DiscoveryNode {
	Phase2CaptureLocalKey capture_key;
	Phase2CaptureLocalKey support_capture_key;
	Phase2CaptureLocalKey group_leader_capture_key;
	std::array<Phase2CaptureLocalKey, MaximumPhase2DockRelationsPerShip>
		direct_docking_capture_keys{};
	std::array<bool, MaximumPhase2DockRelationsPerShip>
		direct_docking_reciprocal{};
	std::uint64_t raw_support_flags = 0U;
	std::uint8_t direct_docking_count = 0U;
	std::array<std::uint16_t, MaximumPhase2DockRelationsPerShip>
		direct_local_dockpoints{};
	std::array<std::uint16_t, MaximumPhase2DockRelationsPerShip>
		direct_remote_dockpoints{};
};

struct Phase2ObservationSelection {
	std::size_t count = 0U;
	std::array<Phase2CaptureLocalKey, MaximumPhase2ObservationShips> ship_keys{};
};

class OwnedPhase2String {
  public:
	bool assign(std::string_view value) noexcept
	{
		if (value.size() > MaximumPhase2InternalNameBytes) return false;
		if (!value.empty()) std::memcpy(m_bytes.data(), value.data(), value.size());
		m_size = static_cast<std::uint16_t>(value.size());
		m_bytes[m_size] = '\0';
		return true;
	}
	std::size_t size() const noexcept { return m_size; }
	bool empty() const noexcept { return m_size == 0U; }
	const char* data() const noexcept { return m_bytes.data(); }
	std::string_view view() const noexcept { return {m_bytes.data(), m_size}; }

	friend bool operator==(const OwnedPhase2String& left, std::string_view right) noexcept
	{
		return left.view() == right;
	}
	friend bool operator==(std::string_view left, const OwnedPhase2String& right) noexcept
	{
		return left == right.view();
	}
	friend bool operator!=(const OwnedPhase2String& left, std::string_view right) noexcept
	{
		return !(left == right);
	}
	friend bool operator!=(std::string_view left, const OwnedPhase2String& right) noexcept
	{
		return !(left == right);
	}

  private:
	std::array<char, MaximumPhase2InternalNameBytes + 1U> m_bytes{};
	std::uint16_t m_size = 0U;
};

class OwnedPhase2CatalogString {
  public:
	bool assign(std::string_view value) noexcept {
		if(value.size()>255U)return false;
		if(!value.empty())std::memcpy(m_bytes.data(),value.data(),value.size());
		m_size=static_cast<std::uint16_t>(value.size());
		m_bytes[m_size]='\0';
		return true;
	}
	std::string_view view() const noexcept { return {m_bytes.data(),m_size}; }
	bool empty() const noexcept { return m_size==0U; }
  private:
	std::array<char,256U> m_bytes{};
	std::uint16_t m_size=0U;
};

struct Phase2RawVec3 {
	float x = 0.0F;
	float y = 0.0F;
	float z = 0.0F;
};

enum class Phase2RawAuxiliaryRegistry : std::uint8_t {
	Species = 0,
	ShipType,
	Iff,
	Wing,
	Armor,
	DamageType,
	Pattern,
	Count,
};

struct Phase2RawSubsystemDefinition {
	std::uint32_t subsystem_capture_key = 0U;
	OwnedPhase2CatalogString internal_name;
	OwnedPhase2CatalogString alt_name;
	OwnedPhase2CatalogString hud_name;
	Phase2RawVec3 local_position;
	float radius = 0.0F;
	float max_hits = 0.0F;
	std::uint8_t subsystem_type_source = 0U;
	std::uint32_t raw_static_flags = 0U;
	std::uint32_t armor_capture_key = 0U;
};

struct Phase2RawBankDefinition {
	std::uint32_t bank_capture_key = 0U;
	std::uint32_t owner_subsystem_capture_key = 0U;
	std::uint8_t family_source = 0U;
	std::uint8_t source_family = 0U;
	std::uint16_t bank_index = 0U;
	std::uint32_t weapon_capture_key = 0U;
	bool consumes_ammunition = false;
	bool has_capacity = false;
	float capacity = 0.0F;
	std::uint8_t firing_pattern_source_code = 0U;
	std::uint32_t fire_point_count = 0U;
	std::array<Phase2RawVec3, MaximumPhase2StaticFirePoints> fire_points{};
};

struct Phase2RawClassDefinition {
	std::uint32_t class_capture_key = 0U;
	OwnedPhase2CatalogString internal_name;
	float model_mass = 0.0F;
	float density = 0.0F;
	std::array<float, 9U> model_inertia{};
	float effective_mass = 0.0F;
	std::array<float, 9U> effective_inertia{};
	Phase2RawVec3 center_of_mass;
	Phase2RawVec3 max_velocity;
	Phase2RawVec3 afterburner_max_velocity;
	Phase2RawVec3 booster_max_velocity;
	Phase2RawVec3 max_rotational_velocity;
	float max_rear_velocity = 0.0F;
	float forward_accel_time = 0.0F;
	float afterburner_forward_accel_time = 0.0F;
	float booster_forward_accel_time = 0.0F;
	float forward_decel_time = 0.0F;
	float slide_accel_time = 0.0F;
	float slide_decel_time = 0.0F;
	float max_hull_strength = 0.0F;
	float max_shield_strength = 0.0F;
	bool has_afterburner = false;
	float afterburner_fuel_capacity = 0.0F;
	float afterburner_burn_rate = 0.0F;
	float afterburner_recover_rate = 0.0F;
	float afterburner_min_start_fuel = 0.0F;
	float afterburner_cooldown_seconds = 0.0F;
	bool has_scan = false;
	std::int32_t scan_time_ms = 0;
	float scan_range_normal = 0.0F;
	float scan_range_capital = 0.0F;
	float scanning_range_multiplier = 0.0F;
	bool has_glide = false;
	float glide_cap = 0.0F;
	bool has_autoaim = false;
	float autoaim_fov_rad = 0.0F;
	std::uint32_t species_capture_key = 0U;
	std::uint32_t ship_type_capture_key = 0U;
	std::uint32_t iff_capture_key = 0U;
	std::uint32_t wing_capture_key = 0U;
	std::uint32_t armor_capture_key = 0U;
	std::uint32_t damage_type_capture_key = 0U;
	float countermeasure_capacity = 0.0F;
	float countermeasure_cargo_size = 0.0F;
	bool countermeasure_uses_capacity = false;
	std::uint32_t countermeasure_weapon_capture_key = 0U;
	std::uint32_t countermeasure_firewait_ms = 0U;
	std::uint32_t subsystem_count = 0U;
	std::uint32_t subsystem_offset = 0U;
	std::uint32_t bank_count = 0U;
	std::uint32_t bank_offset = 0U;
	std::uint32_t primary_bank_count = 0U;
	std::uint32_t secondary_bank_count = 0U;
	std::uint32_t tertiary_bank_count = 0U;
	std::uint32_t turret_bank_count = 0U;
};

struct Phase2RawWeaponDefinition {
	std::uint32_t weapon_capture_key = 0U;
	OwnedPhase2CatalogString internal_name;
	OwnedPhase2CatalogString title;
	std::uint8_t weapon_subtype_source = 0U;
	std::uint64_t raw_class_flags = 0U;
	float max_speed = 0.0F;
	float mass = 0.0F;
	float gravity_constant = 0.0F;
	float velocity_inherit_amount = 0.0F;
	float lifetime_seconds = 0.0F;
	float acceleration_time_seconds = 0.0F;
	float minimum_range = 0.0F;
	float optimal_range = 0.0F;
	float maximum_range = 0.0F;
	float fire_wait_seconds = 0.0F;
	float energy_consumed = 0.0F;
	float damage = 0.0F;
	std::uint32_t damage_type_capture_key = 0U;
	float shockwave_outer_radius = 0.0F;
	std::uint32_t raw_effect_flags = 0U;
	std::uint8_t guidance_type_source = 0U;
	float guidance_fov_source_cosine = 0.0F;
	float lock_time_seconds = 0.0F;
	float lock_fov_source_cosine = 0.0F;
	float cargo_size = 0.0F;
	float rearm_rate_seconds = 0.0F;
	std::uint32_t reloaded_per_batch = 0U;
	std::int32_t burst_shots = 0;
	float burst_delay_seconds = 0.0F;
	std::int32_t swarm_count_source = 0;
	std::int32_t shots_source = 0;
};

struct Phase2RawAuxiliaryEntry {
	Phase2RawAuxiliaryRegistry registry = Phase2RawAuxiliaryRegistry::Species;
	std::uint32_t capture_key = 0U;
	OwnedPhase2CatalogString name;
	std::uint8_t firing_pattern_source_code = 0U;
};

struct Phase2RawStaticCatalog {
	std::uint32_t class_count = 0U;
	std::uint32_t weapon_count = 0U;
	std::uint32_t auxiliary_count = 0U;
	std::uint32_t aggregate_subsystem_count = 0U;
	std::array<Phase2RawClassDefinition, MaximumPhase2StaticClasses> class_definitions{};
	std::array<Phase2RawWeaponDefinition, MaximumPhase2StaticWeapons> weapon_definitions{};
	std::array<Phase2RawAuxiliaryEntry, MaximumPhase2StaticAuxiliaryEntries> auxiliary_entries{};
	std::array<Phase2RawSubsystemDefinition, MaximumPhase2StaticSubsystems> subsystem_storage{};
	std::array<Phase2RawBankDefinition,
		MaximumPhase2StaticClasses * MaximumPhase2StaticBanksPerClass> bank_storage{};

	Phase2RawStaticCatalog() noexcept;
	Phase2RawStaticCatalog(const Phase2RawStaticCatalog& other) noexcept;
	Phase2RawStaticCatalog& operator=(const Phase2RawStaticCatalog& other) noexcept;
	void clear() noexcept;
	bool copy_from(const Phase2RawStaticCatalog& other) noexcept;
};

struct Phase2StaticAuthorityInput {
	bool guards_valid = false;
	struct ShipAuthority : Phase2RawClassDefinition {
	} ship_info;
	struct WeaponAuthorities : Phase2RawWeaponDefinition {
		std::uint32_t additional_count = 0U;
		std::array<Phase2RawWeaponDefinition,
			MaximumPhase2StaticWeapons - 1U> additional_definitions{};
	} weapon_info;
	struct {
		Phase2RawVec3 center_of_mass;
	} model;
	std::uint32_t subsystem_count = 0U;
	std::array<Phase2RawSubsystemDefinition,
		MaximumPhase2SubsystemsPerShip> subsystems{};
	std::uint32_t bank_count = 0U;
	struct BankAuthority : Phase2RawBankDefinition {
		std::uint32_t num_slots = 0U;
	};
	std::array<BankAuthority,
		MaximumPhase2StaticBanksPerClass> banks{};
	struct RegistryFacts {
		Phase2RawAuxiliaryEntry species;
		Phase2RawAuxiliaryEntry weapon_damage_type;
		std::uint32_t additional_count = 0U;
		std::array<Phase2RawAuxiliaryEntry,
			MaximumPhase2StaticAuxiliaryEntries> additional_entries{};
	} registries;
};

void reset_phase2_static_authority_input(
	Phase2StaticAuthorityInput& input) noexcept;

SourceReadResult map_phase2_static_authorities(
	const Phase2StaticAuthorityInput& input,
	Phase2RawStaticCatalog& catalog) noexcept;

struct Phase2RawStaticReferences {
	std::uint32_t class_capture_key = 0U;
	std::uint32_t weapon_count = 0U;
	std::array<std::uint32_t, MaximumPhase2StaticWeapons> weapon_capture_keys{};
	std::uint32_t auxiliary_count = 0U;
	std::array<std::uint32_t, MaximumPhase2StaticAuxiliaryEntries> auxiliary_capture_keys{};
};

struct ShipIdentityObservation {
	OwnedPhase2String internal_name;
	OwnedPhase2String class_name;
	OwnedPhase2String display_name;
	OwnedPhase2String callsign;
	OwnedPhase2String wing_name;
	std::uint64_t sample_time_us = 0U;
	std::uint64_t presence = 0U;
	Phase2CaptureLocalKey class_source_key;
	std::uint16_t role_flags = 0U;
	std::uint16_t wing_position = 0U;
};

enum class ShipLifecycleState : std::uint8_t {
	Present = 0,
	Disabled,
	Spawning,
	Departing,
	Dying,
	Destroyed,
	Removed,
	Departed,
	Vanished,
	Count,
};

struct ShipLifecycleObservation {
	std::uint64_t sample_time_us = 0U;
	std::uint64_t presence = 0U;
	ShipLifecycleState state = ShipLifecycleState::Present;
	std::uint32_t lifecycle_flags = 0U;
};

struct ShipFlightObservation {
	std::uint64_t sample_time_us = 0U;
	std::uint64_t presence = 0U;
	std::array<float, 3U> position_world{};
	std::array<float, 4U> orientation_local_to_world{{1.0F, 0.0F, 0.0F, 0.0F}};
	std::array<float, 3U> velocity_world{};
	std::array<float, 3U> rotational_velocity_local{};
	float radius = 0.0F;
	std::uint32_t physics_mode_flags = 0U;
};

struct ShipDamageObservation {
	std::uint64_t sample_time_us = 0U;
	std::uint64_t presence = 0U;
	std::uint16_t protection_flags = 0U;
	float hull_current = 0.0F;
	float hull_maximum = 0.0F;
	float guardian_threshold = 0.0F;
	Phase2CaptureLocalKey armor_source_key;
};

struct ShipShieldObservation {
	std::uint64_t sample_time_us = 0U;
	std::uint64_t presence = 0U;
	bool has_shields = false;
	std::uint8_t segment_count = 0U;
	std::array<float, MaximumPhase2ShieldSegments> segment_current_hits{};
	std::array<float, MaximumPhase2ShieldSegments> segment_maximum_hits{};
	float recharge_maximum = 0.0F;
	float regeneration_rate = 0.0F;
	float deferred_transfer = 0.0F;
};

struct ShipEnergyObservation {
	std::uint64_t sample_time_us = 0U;
	std::uint64_t presence = 0U;
	float weapon_energy_current = 0.0F;
	float weapon_energy_maximum = 0.0F;
	std::uint8_t shield_recharge_index = 0U;
	std::uint8_t weapon_recharge_index = 0U;
	std::uint8_t engine_recharge_index = 0U;
	bool ets_available = false;
	protocol::EtsMode ets_mode = protocol::EtsMode::Absent;
	float shield_regeneration_rate = 0.0F;
	float weapon_regeneration_rate = 0.0F;
	float deferred_weapon_transfer = 0.0F;
	float deferred_shield_transfer = 0.0F;
	float power_output = 0.0F;
	float engine_integrity_current = 0.0F;
	float engine_integrity_maximum = 0.0F;
};

struct ShipPropulsionObservation {
	std::uint64_t sample_time_us = 0U;
	std::uint64_t presence = 0U;
	std::uint32_t propulsion_flags = 0U;
	float afterburner_fuel = 0.0F;
	float afterburner_capacity = 0.0F;
	float burn_rate = 0.0F;
	float recovery_rate = 0.0F;
	std::uint64_t cooldown_remaining_us = 0U;
	std::uint64_t time_since_last_stop_us = 0U;
	float minimum_to_engage = 0.0F;
	float fuel_at_last_engagement = 0.0F;
	float forward_acceleration_time_constant = 0.0F;
	std::array<float, 3U> afterburner_max_velocity{};
	float engine_wash_intensity = 0.0F;
};

enum class ShipWeaponBankFamily : std::uint8_t {
	Primary = 0,
	Secondary,
	TurretPrimary,
	TurretSecondary,
	Count,
};

struct ShipWeaponBankObservation {
	std::uint32_t source_bank_key = 0U;
	Phase2CaptureLocalKey weapon_class_source_key;
	ShipWeaponBankFamily family = ShipWeaponBankFamily::Primary;
	std::uint16_t ammunition_current = 0U;
	std::uint16_t ammunition_initial = 0U;
	std::uint64_t cooldown_remaining_us = 0U;
	std::int32_t primary_slot = -1;
	std::int32_t secondary_slot = -1;
	std::uint16_t primary_fire_point = 0U;
	std::uint16_t simultaneous_slots = 1U;
	std::uint16_t pattern_id = 0U;
	std::uint8_t firing_pattern_source_code = 0U;
	std::uint64_t rearm_remaining_us = 0U;
	std::int32_t burst_counter = 0;
	std::uint32_t burst_seed = 0U;
	std::uint16_t substitution_pattern_index = 0U;
	std::uint64_t fof_cooldown_remaining_us = 0U;
	std::int32_t weapon_animation = 0;
};

struct ShipWeaponsObservation {
	std::uint64_t sample_time_us = 0U;
	std::uint64_t presence = 0U;
	std::uint8_t primary_bank_count = 0U;
	std::uint8_t secondary_bank_count = 0U;
	std::int32_t current_primary_bank = -1;
	std::int32_t current_secondary_bank = -1;
	std::int32_t previous_primary_bank = -1;
	std::int32_t previous_secondary_bank = -1;
	std::int32_t targeting_laser_bank = -1;
	bool targeting_laser_active = false;
	std::uint16_t swarm_remaining = 0U;
	std::int32_t swarm_secondary_bank = -1;
	std::uint32_t remote_detonaters_active = 0U;
	std::uint64_t remote_detonation_remaining_us = 0U;
	bool per_burst_rotation_active = false;
	float per_burst_rotation = 0.0F;
	std::uint64_t raw_weapon_flags = 0U;
	std::uint8_t tertiary_bank_count = 0U;
	std::int32_t current_tertiary_bank = -1;
	std::int32_t tertiary_bank = -1;
	std::int32_t tertiary_ammunition_current = 0;
	std::int32_t tertiary_ammunition_initial = 0;
	std::int32_t tertiary_ammunition_capacity = 0;
	std::uint64_t tertiary_cooldown_remaining_us = 0U;
	std::uint64_t tertiary_rearm_remaining_us = 0U;
	std::array<ShipWeaponBankObservation, MaximumPhase2WeaponBanksPerFamily> primary_banks{};
	std::array<ShipWeaponBankObservation, MaximumPhase2WeaponBanksPerFamily> secondary_banks{};
	std::uint16_t countermeasure_count = 0U;
	std::uint16_t countermeasure_maximum = 0U;
	Phase2CaptureLocalKey countermeasure_class_source_key;
	bool countermeasures_enabled = true;
	std::uint64_t countermeasure_cooldown_remaining_us = 0U;
};

enum class ShipSupportPhase : std::uint8_t {
	None = 0,
	Queued,
	OnWay,
	Docking,
	Repairing,
	Rearming,
	Aborted,
	Obstructed,
	Count,
};

struct ShipSupportObservation {
	std::uint64_t sample_time_us = 0U;
	std::uint64_t presence = 0U;
	ShipSupportPhase phase = ShipSupportPhase::None;
	Phase2CaptureLocalKey support_capture_key;
	std::uint32_t episode_sequence = 0U;
	float repair_progress = 0.0F;
	float rearm_progress = 0.0F;
	std::uint64_t raw_support_flags = 0U;
	bool raw_support_repairs_hull_authorized = false;
	bool raw_hull_repair_applicable = false;
	bool raw_shield_repair_applicable = false;
	bool raw_subsystem_repair_applicable = false;
	bool raw_weapon_energy_rearm_applicable = false;
	bool raw_ammunition_rearm_applicable = false;
	bool raw_countermeasure_rearm_applicable = false;
	bool raw_mission_rearm_disallowed = false;
	bool raw_weapon_rearm_disallowed = false;
	float raw_max_hull_repair_fraction = 0.0F;
	float raw_max_subsystem_repair_fraction = 0.0F;
	float raw_hull_repair_work = 0.0F;
	float raw_shield_repair_work = 0.0F;
	float raw_subsystem_repair_work = 0.0F;
	float raw_weapon_energy_rearm_work = 0.0F;
	std::uint64_t raw_ammunition_rearm_work = 0U;
	std::uint64_t raw_countermeasure_rearm_work = 0U;
	std::uint64_t raw_countermeasure_capacity = 0U;
	std::int32_t raw_countermeasure_rearm_pool = -1;
	float raw_hull_repair_rate = 0.0F;
	float raw_shield_repair_rate = 0.0F;
	float raw_subsystem_repair_rate = 0.0F;
};

struct ShipDockRelationObservation {
	Phase2CaptureLocalKey remote_capture_key;
	std::uint16_t local_dockpoint = 0U;
	std::uint16_t remote_dockpoint = 0U;
	OwnedPhase2String local_dock_bay_name;
	OwnedPhase2String remote_dock_bay_name;
};

struct ShipDockingObservation {
	std::uint64_t sample_time_us = 0U;
	std::uint64_t presence = 0U;
	protocol::DockingPhase phase = protocol::DockingPhase::None;
	std::uint8_t relation_count = 0U;
	bool dock_leader = false;
	std::array<ShipDockRelationObservation, MaximumPhase2DockRelationsPerShip> relations{};
};

enum class ShipSubsystemKind : std::uint8_t {
	Generic = 0,
	Other,
	Engine,
	Weapon,
	Turret,
	Radar,
	Sensors,
	Communication,
	Navigation,
	Count,
};

struct ShipTurretBankRawObservation {
	std::int32_t turret_ammunition_current = 0;
	std::int32_t turret_ammunition_capacity = 0;
	std::uint64_t turret_cooldown_remaining_us = 0U;
};

struct ShipTurretObservation {
	std::uint8_t turret_primary_bank_count = 0U;
	std::uint8_t turret_secondary_bank_count = 0U;
	std::array<Phase2CaptureLocalKey, MaximumPhase2PhysicalPrimaryBanks>
		turret_primary_bank_weapon_source_keys{};
	std::array<Phase2CaptureLocalKey, MaximumPhase2PhysicalSecondaryBanks>
		turret_secondary_bank_weapon_source_keys{};
	std::int32_t turret_next_fire_pos = 0;
	std::uint64_t turret_next_fire_remaining_us = 0U;
	std::array<ShipTurretBankRawObservation, MaximumPhase2PhysicalPrimaryBanks>
		turret_primary_banks{};
	std::array<ShipTurretBankRawObservation, MaximumPhase2PhysicalSecondaryBanks>
		turret_secondary_banks{};
	std::array<float, 3U> turret_current_direction_local{};
	std::uint16_t turret_firing_point_count = 0U;
	float turret_rof_scaler = 1.0F;
	std::int32_t turret_animation = 0;
	std::uint64_t turret_animation_remaining_us = 0U;
	bool turret_beam_free = false;
	bool turret_locked = false;
};

struct ShipSubsystemObservation {
	std::uint64_t sample_time_us = 0U;
	std::uint64_t presence = 0U;
	Phase2CaptureLocalKey source_key;
	ShipSubsystemKind kind = ShipSubsystemKind::Generic;
	float hits_current = 0.0F;
	float hits_maximum = 0.0F;
	std::array<float, 3U> position_local{};
	std::array<float, 4U> orientation_local{{1.0F, 0.0F, 0.0F, 0.0F}};
	std::uint64_t raw_flags = 0U;
	Phase2CaptureLocalKey armor_source_key;
	std::uint64_t disruption_remaining_us = 0U;
	float aggregate_current_hits = 0.0F;
	float aggregate_maximum_hits = 0.0F;
	std::optional<ShipTurretObservation> turret;
};

struct ShipSubsystemStorage {
	std::uint16_t count = 0U;
	std::array<ShipSubsystemObservation, MaximumPhase2SubsystemsPerShip> values{};
};

struct ShipObservationDto {
	Phase2CaptureLocalKey capture_key;
	ShipIdentityObservation identity;
	ShipLifecycleObservation lifecycle;
	ShipFlightObservation flight;
	ShipDamageObservation damage;
	ShipShieldObservation shields;
	ShipEnergyObservation energy;
	ShipPropulsionObservation propulsion;
	ShipWeaponsObservation weapons;
	ShipSupportObservation support;
	ShipDockingObservation docking;
	ShipSubsystemStorage subsystems;
	Phase2RawStaticReferences raw_static_references;
};

enum class PlayerControlModeObservation : std::uint8_t {
	Ship = 0,
	Camera,
	Disabled,
	Count,
};

struct PlayerControlObservation {
	std::uint64_t sample_time_us = 0U;
	std::uint64_t presence = 0U;
	PlayerControlModeObservation mode = PlayerControlModeObservation::Disabled;
	float pitch = 0.0F;
	float heading = 0.0F;
	float bank = 0.0F;
	float forward = 0.0F;
	float sideways = 0.0F;
	float vertical = 0.0F;
	std::uint32_t action_flags = 0U;
	float forward_cruise_percent = 0.0F;
	std::uint16_t fire_primary_count = 0U;
	std::uint16_t fire_secondary_count = 0U;
	std::uint16_t fire_countermeasure_count = 0U;
	bool autopilot_engaged = false;
	bool player_use_ai = false;
	std::int32_t engine_control_mode = 0;
	bool flight_cursor_active = false;
	float flight_cursor_pitch = 0.0F;
	float flight_cursor_heading = 0.0F;
	float flight_cursor_sensitivity = 0.0F;
	float effective_aim_extent = 0.000001F;
	float flight_cursor_deadzone_extent = 1.0F;
	bool afterburner_requested = false;
};

enum class CargoScanPhaseObservation : std::uint8_t {
	Idle = 0,
	Scanning,
	Completed,
	NotScannable,
	Count,
};

struct PlayerCargoScanObservation {
	std::uint64_t sample_time_us = 0U;
	std::uint64_t presence = 0U;
	CargoScanPhaseObservation phase = CargoScanPhaseObservation::NotScannable;
	Phase2CaptureLocalKey target_capture_key;
	Phase2CaptureLocalKey target_subsystem_source_key;
	std::uint64_t elapsed_us = 0U;
	std::uint64_t required_us = 0U;
	std::uint32_t validity_flags = 0U;
	OwnedPhase2String cargo_text;
};

struct Phase2ObservationDto {
	Phase2CaptureResult capture;
	Phase2CaptureResult discovery_capture;
	Phase2SourceReadStatus discovery_status = Phase2SourceReadStatus::Valid;
	std::uint8_t discovery_count = 0U;
	std::array<Phase2DiscoveryNode, MaximumPhase2ObservationShips> discovery_nodes{};
	Phase2CaptureLocalKey player_key;
	std::uint64_t producer_sample_time_us = 0U;
	std::vector<ShipObservationDto> ships;
	Phase2RawStaticCatalog raw_static_catalog;
	PlayerControlObservation player_controls;
	PlayerCargoScanObservation player_cargo_scan;
	struct RawStaticDiagnostic {
		enum class Reason : std::uint8_t {
			None = 0,
			DistinctSameNameClassVariant,
			DistinctSameNameWeaponVariant,
			AmbiguousAuxiliaryName,
			MissingReferenceRemap,
			MissingSubsystemOwner,
			Count,
		};
		Reason reason = Reason::None;
	} raw_static_diagnostic;
};

struct Phase2ShipSource {
	std::string_view internal_name;
	std::string_view class_name;
	ShipIdentityObservation identity;
	ShipLifecycleObservation lifecycle;
	ShipFlightObservation flight;
	ShipDamageObservation damage;
	ShipShieldObservation shields;
	ShipEnergyObservation energy;
	ShipPropulsionObservation propulsion;
	ShipWeaponsObservation weapons;
	ShipSupportObservation support;
	ShipDockingObservation docking;
	ShipSubsystemStorage subsystems;
	Phase2RawStaticCatalog raw_static_catalog;
	Phase2RawStaticReferences raw_static_references;
	Phase2StaticAuthorityInput static_authority_input;
};

enum class Phase2CaptureBlock : std::uint8_t {
	Identity = 0,
	Flight,
	Control,
	DamageShield,
	EnergyPropulsion,
	Weapons,
	Subsystems,
	SupportCargoDocking,
	Count,
};

struct Phase2CaptureDiagnostics {
	std::array<std::uint64_t,
		static_cast<std::size_t>(Phase2CaptureBlock::Count)> duration_ns{};
	std::uint8_t attempted_mask = 0U;
	std::array<std::uint32_t, MaximumPhase2ObservationShips>
		source_signatures{};
	std::size_t source_count = 0U;
	bool duration_overflow = false;
	Phase2CaptureBlock primary_failed_block = Phase2CaptureBlock::Count;
};

class Phase2EngineReadView {
  public:
	virtual ~Phase2EngineReadView() = default;

	virtual bool current_thread_is_main() const noexcept = 0;
	virtual bool in_mission() const noexcept = 0;
	virtual bool player_exists() const noexcept = 0;
	virtual bool player_object_exists() const noexcept = 0;
	virtual bool player_ship_exists() const noexcept = 0;
	virtual bool player_source_is_consistent() const noexcept = 0;
	virtual SourceReadResult read_player_root_key(EngineEntityKey& output) const noexcept = 0;
	virtual SourceReadResult read_discovery_node(
		EngineEntityKey key, Phase2DiscoveryNode& output) const noexcept = 0;
	virtual SourceReadResult resolve_capture_local_key(
		Phase2CaptureLocalKey key, EngineEntityKey& output) const noexcept
	{
		output = {};
		if (key.value == 0U) {
			return {Phase2SourceReadStatus::InvalidSource};
		}
		output.object_signature = key.value;
		return {Phase2SourceReadStatus::Valid};
	}
	virtual SourceReadResult read_ship(
		EngineEntityKey key, Phase2ShipSource& output) const noexcept = 0;
	// CoreGate owns only the validated player root and the CORE_SHIP blocks.
	// The default fails closed without delegating to the broader read_ship()
	// seam, so an implementation cannot accidentally make extension
	// discovery/support/docking/cargo/weapon authorities a CoreGate
	// materializability prerequisite.
	virtual SourceReadResult read_core_gate_ship(
		EngineEntityKey, Phase2ShipSource&) const noexcept
	{
		return {Phase2SourceReadStatus::UnsupportedEngineState};
	}
	virtual SourceReadResult read_ship_diagnosed(EngineEntityKey key,
		Phase2ShipSource& output,
		Phase2CaptureDiagnostics& diagnostics) const noexcept
	{
		return read_ship(key, output);
	}
	virtual SourceReadResult read_core_gate_ship_diagnosed(
		EngineEntityKey key, Phase2ShipSource& output,
		Phase2CaptureDiagnostics&) const noexcept
	{
		return read_core_gate_ship(key, output);
	}
	virtual bool read_player_controls(PlayerControlObservation& output) const noexcept = 0;
	virtual bool read_player_cargo_scan(PlayerCargoScanObservation& output) const noexcept = 0;
};

enum class Phase2ProvisioningMode : std::uint8_t {
	ConfigurationAbsent = 0,
	ValidDisabled,
	ValidEnabled,
	Count,
};

enum class Phase2ObservationBufferState : std::uint8_t {
	Unprovisioned = 0,
	Disabled,
	Provisioned,
	Ready,
	FailedClosed,
	Count,
};

enum class Phase2ObservationProjection : std::uint8_t {
	CoreGate = 0,
	DiscoveryExtension,
	CompleteShip,
	Count,
};

class Phase2ObservationBuffer {
  public:
	bool provision(Phase2ProvisioningMode mode) noexcept;
	bool enter_ready() noexcept;
	Phase2CaptureResult capture(const Phase2EngineReadView& source,
		std::uint64_t producer_sample_time_us,
		Phase2ObservationProjection projection =
			Phase2ObservationProjection::DiscoveryExtension) noexcept;

	Phase2ObservationBufferState state() const noexcept;
	std::size_t ship_capacity() const noexcept;
	std::size_t owned_bytes() const noexcept;
	const Phase2ObservationDto& observation() const noexcept;
	Phase2ObservationDto& observation() noexcept;
	const Phase2CaptureDiagnostics& capture_diagnostics() const noexcept {
		return m_capture_diagnostics;
	}
	const Phase2CaptureDiagnostics& accepted_capture_map() const noexcept {
		return m_accepted_capture_map;
	}
	void reset_observation_and_clear_phase2() noexcept;

  private:
	Phase2ObservationBufferState m_state = Phase2ObservationBufferState::Unprovisioned;
	Phase2ObservationDto m_observation;
	std::unique_ptr<Phase2ShipSource> m_source_scratch;
	Phase2CaptureDiagnostics m_capture_diagnostics{};
	Phase2CaptureDiagnostics m_accepted_capture_map{};
};

void reset_phase2_observation_buffer_in_place(
	Phase2ObservationBuffer& buffer) noexcept;

static_assert(sizeof(Phase2ObservationBuffer) +
		MaximumPhase2ObservationShips * sizeof(ShipObservationDto) +
		sizeof(Phase2ShipSource) <= MaximumPhase2OwnedBytes,
	"Phase 2 owned maximum must remain inside the shared 64 MiB startup cap");

Phase2CaptureResult collect_phase2_observation(const Phase2EngineReadView& source,
	const Phase2ObservationSelection& selection,
	std::uint64_t producer_sample_time_us,
	Phase2ObservationDto& output,
	Phase2ObservationProjection projection =
		Phase2ObservationProjection::DiscoveryExtension) noexcept;
Phase2CaptureResult collect_phase2_observation(const Phase2EngineReadView& source,
	std::uint64_t producer_sample_time_us,
	Phase2ObservationDto& output) noexcept;
Phase2CaptureResult collect_phase2_observation(const Phase2EngineReadView& source,
	std::uint64_t producer_sample_time_us,
	Phase2ObservationDto& output,
	Phase2ObservationProjection projection) noexcept;
Phase2CaptureResult collect_phase2_observation(Phase2ObservationBuffer& buffer,
	const Phase2EngineReadView& source,
	std::uint64_t producer_sample_time_us,
	Phase2ObservationProjection projection =
		Phase2ObservationProjection::DiscoveryExtension) noexcept;

bool canonicalize_phase2_float(float value, float absolute_limit, float& output) noexcept;
bool phase2_timestamp_remaining_us(std::int64_t now_ms,
	std::int64_t deadline_ms,
	std::uint64_t maximum_remaining_us,
	std::uint64_t& remaining_us) noexcept;

enum class ShipCleanupMode : std::uint8_t {
	Destroyed = 0,
	Departed,
	Vanished,
	Count,
};

enum class SupportTransitionReason : std::uint8_t {
	Queue = 0,
	OnWay,
	Begin,
	Broken,
	End,
	Abort,
	Killed,
	Complete,
	Count,
};

enum class ControlTargetAuthority : std::uint8_t {
	Ship = 0,
	Camera,
	Count,
};

struct ShipCleanupFact {
	std::uint32_t object_signature = 0U;
	ShipCleanupMode mode = ShipCleanupMode::Destroyed;
};

struct SupportTransitionFact {
	std::uint32_t assisted_signature = 0U;
	std::uint32_t support_signature = 0U;
	std::uint32_t episode_sequence = 0U;
	SupportTransitionReason reason = SupportTransitionReason::Queue;
	std::uint64_t sample_time = 0U;
};

struct Phase2Wp07CleanupIntent {
	ShipCleanupMode mode = ShipCleanupMode::Destroyed;
	std::uint32_t object_signature = 0U;
	std::uint64_t sample_time = 0U;
	std::uint64_t event_order = 0U;
	std::uint64_t purge_order = 0U;
	std::uint64_t replacement_entity_id = 0U;
	bool event_ready = false;
	bool fence_ready = false;
	bool new_entity_id_request = false;
};

enum class Phase2Wp07DrainStatus : std::uint8_t {
	Drained = 0,
	NoFacts,
	InvalidInput,
	RingOverflow,
	TableOverflow,
	Count,
};

class Phase2Wp07CleanupRing {
  public:
	static constexpr std::size_t Capacity = 64U;
	bool record(const ShipCleanupFact& fact, std::uint64_t sample_time) noexcept;
	Phase2Wp07CleanupIntent latest() const noexcept;
	Phase2Wp07CleanupIntent at(std::size_t index) const noexcept;
	std::size_t size() const noexcept { return m_size; }
	bool overflowed() const noexcept { return m_overflowed; }
	bool commit_drain(std::size_t count) noexcept;
	void reset() noexcept;

  private:
	std::array<Phase2Wp07CleanupIntent, Capacity> m_values{};
	std::size_t m_size = 0U;
	std::uint64_t m_order = 0U;
	std::uint64_t m_replacement = 0U;
	bool m_overflowed = false;
};

class Phase2Wp07SupportTerminalRing {
  public:
	static constexpr std::size_t Capacity = 64U;
	bool record_transition(const SupportTransitionFact& fact) noexcept;
	bool record_terminal(const SupportTransitionFact& fact) noexcept;
	SupportTransitionFact latest(std::uint32_t assisted_signature) const noexcept;
	SupportTransitionFact at(std::size_t index) const noexcept;
	std::size_t size() const noexcept { return m_size; }
	bool overflowed() const noexcept { return m_overflowed; }
	bool commit_drain(std::size_t count) noexcept;
	void reset() noexcept;

  private:
	std::array<SupportTransitionFact, Capacity> m_values{};
	std::array<SupportTransitionFact, Capacity> m_drained_latest{};
	std::size_t m_drained_count = 0U;
	std::size_t m_size = 0U;
	bool m_overflowed = false;
};

class Phase2Wp07EpisodeLatches {
  public:
	static constexpr std::size_t SessionCapacity = 64U;
	static constexpr std::size_t EntriesPerSession = 64U;
	static constexpr std::size_t Capacity = EntriesPerSession;
	bool activate_session(std::size_t session_slot) noexcept;
	bool deactivate_session(std::size_t session_slot) noexcept;
	bool session_active(std::size_t session_slot) const noexcept;
	bool latch(std::size_t session_slot, const SupportTransitionFact& fact) noexcept;
	bool on_applied(std::size_t slot, std::uint32_t episode_sequence) noexcept;
	bool on_applied(std::size_t session_slot,
		std::uint32_t assisted_signature,
		std::uint32_t episode_sequence) noexcept;
	bool pending(std::size_t slot) const noexcept;
	std::size_t size(std::size_t slot) const noexcept;
	bool pending(std::size_t session_slot,
		std::uint32_t assisted_signature) const noexcept;
	SupportTransitionFact value(std::size_t slot) const noexcept;
	SupportTransitionFact value(std::size_t session_slot,
		std::uint32_t assisted_signature) const noexcept;
	void reset() noexcept;

  private:
	struct Entry {
		bool active = false;
		SupportTransitionFact fact{};
	};
	struct Session {
		bool active = false;
		std::array<Entry, EntriesPerSession> entries{};
	};
	std::array<Session, SessionCapacity> m_sessions{};
};

struct Phase2Wp07CleanupBatch {
	std::array<Phase2Wp07CleanupIntent,
		Phase2Wp07CleanupRing::Capacity> intents{};
	std::size_t count = 0U;
};

struct Phase2Wp07GlobalEventBatch {
	Phase2Wp07CleanupBatch cleanup{};
	std::array<SupportTransitionFact,
		Phase2Wp07SupportTerminalRing::Capacity> support{};
	std::size_t support_count = 0U;
	std::size_t cleanup_ring_count = 0U;
	std::size_t support_ring_count = 0U;
};

Phase2Wp07DrainStatus prepare_phase2_global_events(
	const Phase2CaptureDiagnostics& accepted_map,
	Phase2Wp07GlobalEventBatch& batch) noexcept;
bool commit_phase2_global_events(
	const Phase2Wp07GlobalEventBatch& batch) noexcept;
std::size_t phase2_cleanup_ring_depth() noexcept;
std::size_t phase2_support_ring_depth() noexcept;
bool phase2_cleanup_ring_overflowed() noexcept;
bool phase2_support_ring_overflowed() noexcept;

Phase2Wp07DrainStatus drain_support_terminals_once(
	Phase2Wp07SupportTerminalRing& ring,
	Phase2Wp07EpisodeLatches& latches) noexcept;
Phase2Wp07DrainStatus drain_cleanup_once(
	Phase2Wp07CleanupRing& ring,
	const std::uint32_t* closure_signatures,
	std::size_t closure_count,
	Phase2Wp07CleanupBatch& batch) noexcept;

struct CargoAuthorityFact {
	std::uint32_t player_signature = 0U;
	std::uint32_t target_signature = 0U;
	std::uint64_t elapsed_us = 0U;
	std::uint32_t target_subsystem_source_key = 0U;
	std::uint64_t presence = 0U;
	CargoScanPhaseObservation phase = CargoScanPhaseObservation::Idle;
	std::uint64_t required_us = 0U;
	std::uint32_t validity_flags = 0U;
	OwnedPhase2String cargo_text;
	bool source_valid = true;
};

struct Phase2SeamHandoffSnapshot {
	bool has_ship_cleanup = false;
	ShipCleanupFact ship_cleanup;
	bool has_support_transition = false;
	SupportTransitionFact support_transition;
	bool has_control_target = false;
	ControlTargetAuthority control_target = ControlTargetAuthority::Ship;
	bool has_cargo_authority = false;
	CargoAuthorityFact cargo_authority;
};

void reset_phase2_seam_handoff() noexcept;
void reset_phase2_mission_observation_state() noexcept;
Phase2SeamHandoffSnapshot phase2_seam_handoff_snapshot() noexcept;
SupportTransitionFact phase2_latest_support_terminal(
	std::uint32_t assisted_signature) noexcept;
bool phase2_wp07_seam_overflowed() noexcept;
void capture_phase2_main_thread_authority() noexcept;
bool phase2_current_thread_is_main() noexcept;

class Phase2SeamTestDouble {
  public:
	virtual ~Phase2SeamTestDouble() = default;

	virtual void on_ship_cleanup(const ShipCleanupFact& fact) noexcept = 0;
	virtual void on_support_transition(const SupportTransitionFact& fact) noexcept = 0;
	virtual void on_control_target(ControlTargetAuthority authority) noexcept = 0;
	virtual void on_cargo_authority(const CargoAuthorityFact& fact) noexcept = 0;
};

} // namespace telemetry::detail

namespace telemetry {

using detail::CargoAuthorityFact;
using detail::ControlTargetAuthority;
using detail::ShipCleanupMode;
using detail::SupportTransitionReason;

void OnShipCleanup(std::uint32_t object_signature, ShipCleanupMode mode) noexcept;
void OnSupportTransition(std::uint32_t assisted_signature,
	std::uint32_t support_signature,
	std::uint32_t episode_sequence,
	SupportTransitionReason reason,
	std::uint64_t sample_time) noexcept;
void OnControlTarget(ControlTargetAuthority authority) noexcept;
void OnCargoAuthority(const CargoAuthorityFact& fact) noexcept;

namespace test_seam {

void set_phase2_seam_test_double(detail::Phase2SeamTestDouble* test_double) noexcept;

} // namespace test_seam
} // namespace telemetry
