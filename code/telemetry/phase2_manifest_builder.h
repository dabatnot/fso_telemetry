#pragma once

#include "telemetry/protocol/telemetry_protocol_constants.h"
#include "telemetry/protocol/telemetry_protocol_types.h"
#include "telemetry/protocol/telemetry_sha256.h"
#include "telemetry/protocol/telemetry_state_messages.h"
#include "telemetry/protocol/telemetry_transaction.h"

#include <array>
#include <cstring>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

namespace telemetry {

std::uint32_t radar_icon_id_for_ship_type(std::string_view name) noexcept;

struct Phase2ManifestLimits {
	static constexpr std::uint32_t MaxClasses = 64;
	static constexpr std::uint32_t MaxWeapons = 4096;
	static constexpr std::uint32_t MaxSubsystemsPerShip = 1024;
	static constexpr std::uint32_t MaxAggregateSubsystems = 4096;
	static constexpr std::uint32_t MaxBanksPerClass = 192;
	static constexpr std::uint32_t MaxFirePoints = 64;
	static constexpr std::uint32_t MaxAuxiliaryEntries = 256;
};

enum class AuxiliaryRegistry : std::uint8_t {
	Species, ShipType, Iff, Wing, Armor, DamageType, Pattern
};
using protocol::WeaponFamily;

struct Phase2Vec3 { float x = 0, y = 0, z = 0; };
struct Phase2SubsystemSource {
	std::uint32_t source_key = 0, system_info_key = 0;
	std::string name, alt_name, hud_name;
	float max_hits = 0, current_hits = 0;
	std::int32_t armor_index = -1;
	protocol::SubsystemType type=protocol::SubsystemType::Unknown;
	Phase2Vec3 local_position{};
	float radius=0;
	std::uint32_t static_flags=0;
};
struct Phase2BankSource {
	WeaponFamily family = WeaponFamily::Primary, source_family = WeaponFamily::None;
	std::uint16_t bank_index = 0;
	std::uint16_t owner_subsystem_canonical_index=UINT16_MAX;
	std::uint32_t weapon_source_key = 0;
	std::uint8_t firing_pattern_source_code = 0U;
	bool consumes_ammunition=false;
	float capacity = 0;
	std::array<Phase2Vec3, Phase2ManifestLimits::MaxFirePoints> fire_points{};
	std::uint32_t fire_point_count = 0;
};
struct Phase2ClassSource {
	std::uint32_t source_key = 0;
	std::string name;
	float model_mass = 0, density_provenance = 1, effective_mass = 0;
	std::array<float, 3> model_inertia{}, effective_inertia{}, full_angle_degrees_provenance{}, half_angle_cosines{1,1,1};
	std::array<float,9> effective_inertia_matrix{};
	Phase2Vec3 center_of_mass{}, max_velocity{}, afterburner_max_velocity{}, booster_max_velocity{}, max_rotational_velocity{};
	float max_rear_velocity=0, forward_accel_time=0, afterburner_forward_accel_time=0, booster_forward_accel_time=0;
	float forward_decel_time=0, slide_accel_time=0, slide_decel_time=0;
	float max_hull_strength=0, max_shield_strength=0;
	bool has_afterburner=false;
	float afterburner_fuel_capacity=0, afterburner_burn_rate=0, afterburner_recover_rate=0, afterburner_min_start_fuel=0;
	std::uint64_t afterburner_cooldown_us=0;
	bool has_scan=false, has_glide=false, has_autoaim=false;
	std::uint64_t scan_required_time_us=0;
	float scan_max_distance=0, scan_max_angle_rad=0, glide_cap=0, autoaim_fov_rad=0;
	std::int32_t species_index = -1, ship_type_index = -1, iff_index = -1, wing_index = -1;
	std::int32_t armor_index = -1, damage_type_index = -1, required_model_index = 0;
	std::array<Phase2SubsystemSource, Phase2ManifestLimits::MaxSubsystemsPerShip> subsystems{};
	std::uint32_t subsystem_count = 0;
	std::array<Phase2BankSource, Phase2ManifestLimits::MaxBanksPerClass> banks{};
	std::uint32_t bank_count = 0;
	float countermeasure_capacity = 0, countermeasure_cargo_size = 0;
	bool countermeasure_uses_capacity = false;
	std::uint32_t countermeasure_weapon_source_key = 0, countermeasure_firewait_ms = 0;
};
struct Phase2WeaponSource {
	std::uint32_t source_key = 0;
	std::string name, title;
	std::int32_t damage_type_index = -1;
	protocol::WeaponSubtype subtype=protocol::WeaponSubtype::Unknown;
	std::uint64_t class_flags=0;
	float max_speed=0, mass=0, gravity_constant=0, velocity_inherit_amount=0, turn_factor=0;
	std::uint64_t lifetime_us=0;
	std::uint32_t effect_flags=0;
	std::uint8_t guidance_type=0;
	bool has_acceleration=false, has_ranges=false, has_fire=false, has_damage=false, has_guidance=false;
	bool has_lock=false, has_cargo_rearm=false, has_burst=false, has_swarm=false;
	std::uint64_t acceleration_time_us=0, fire_wait_us=0, lock_time_us=0, rearm_time_us=0, burst_interval_us=0;
	float minimum_range=0, optimal_range=0, maximum_range=0, energy_consumed=0, damage=0;
	float guidance_fov_rad=0, lock_fov_rad=0, cargo_size=0;
	std::uint32_t reloaded_per_batch=1;
	std::uint16_t burst_count=1, swarm_count=1, shots_per_trigger=1;
};
struct Phase2AuxiliaryEntry { AuxiliaryRegistry registry=AuxiliaryRegistry::Species; std::int32_t engine_index=-1; std::string name; };
struct Phase2ManifestMetadata {
	std::uint32_t maximum_string_bytes=0, projected_record_count=0, maximum_record_length=0;
};
struct Phase2ManifestSource {
	std::array<Phase2ClassSource, Phase2ManifestLimits::MaxClasses> ship_classes{};
	std::uint32_t ship_class_count=0;
	std::array<Phase2WeaponSource, Phase2ManifestLimits::MaxWeapons> weapons{};
	std::uint32_t weapon_count=0;
	std::array<std::uint32_t, Phase2ManifestLimits::MaxClasses> referenced_ship_class_keys{};
	std::uint32_t referenced_ship_class_count=0;
	std::array<std::uint32_t, Phase2ManifestLimits::MaxWeapons> referenced_weapon_keys{};
	std::uint32_t referenced_weapon_count=0;
	std::array<Phase2AuxiliaryEntry, Phase2ManifestLimits::MaxAuxiliaryEntries> auxiliary_entries{};
	std::uint32_t auxiliary_entry_count=0;
	Phase2ManifestMetadata metadata{};
	std::uint32_t player_instance_signature=0, engine_index=0, manifest_generation=0;
	protocol::Sha256Digest topology_fingerprint{};
};

struct Phase2OwnedName {
	std::array<char,256> bytes{};
	std::uint16_t size=0;
	void assign(std::string_view value) noexcept {
		size=static_cast<std::uint16_t>(value.size());
		if(size) std::memcpy(bytes.data(),value.data(),size);
		bytes[size]='\0';
	}
	operator std::string_view() const noexcept { return {bytes.data(),size}; }
	friend bool operator==(const Phase2OwnedName& a,std::string_view b) noexcept {
		return static_cast<std::string_view>(a)==b;
	}
	friend bool operator==(std::string_view a,const Phase2OwnedName& b) noexcept { return b==a; }
};
struct Phase2SubsystemRecord {
	std::uint32_t source_key=0, subsystem_id=0, canonical_index=0, armor_id=0;
};
struct Phase2BankRecord {
	std::uint32_t bank_id=0, weapon_class_id=0;
	std::uint32_t owner_subsystem_id=0;
	telemetry::WeaponFamily family=telemetry::WeaponFamily::Primary;
	telemetry::WeaponFamily source_family=telemetry::WeaponFamily::None;
	std::uint16_t canonical_index=0, source_index=0;
	float capacity=0;
	std::uint32_t fire_point_count=0;
	std::uint32_t pattern_id=0;
	bool consumes_ammunition=false;
};
template <typename T> struct Phase2RecordView {
	T* data=nullptr;
	std::uint32_t capacity=0;
	T& operator[](std::size_t index) noexcept { return data[index]; }
	const T& operator[](std::size_t index) const noexcept { return data[index]; }
	T* begin() noexcept { return data; }
	const T* begin() const noexcept { return data; }
	T* end() noexcept { return data+capacity; }
	const T* end() const noexcept { return data+capacity; }
};
struct Phase2ClassRecord {
	std::uint32_t source_key=0, class_id=0, species_id=0, ship_type_id=0, radar_icon_id=0, iff_id=0, wing_id=0, armor_id=0, damage_type_id=0;
	Phase2OwnedName name;
	float mass=0;
	std::array<float,3> inertia{}, half_angles_rad{};
	std::uint32_t countermeasure_weapon_class_id=0, countermeasure_initial_count=0;
	std::uint64_t countermeasure_firewait_us=0;
	bool countermeasure_installed=false;
	Phase2RecordView<Phase2SubsystemRecord> subsystems;
	std::uint32_t subsystem_count=0;
	Phase2RecordView<Phase2BankRecord> banks;
	std::uint32_t bank_count=0;
	bool has_wing() const noexcept { return wing_id != 0; }
	bool has_armor() const noexcept { return armor_id != 0; }
};
struct Phase2WeaponRecord {
	std::uint32_t source_key=0, weapon_class_id=0, damage_type_id=0;
	std::uint64_t class_flags=0;
	Phase2OwnedName name;
};
struct Phase2AuxiliaryRecord {
	AuxiliaryRegistry registry=AuxiliaryRegistry::Species;
	std::uint32_t source_key=0, public_id=0;
};
struct Phase2ManifestCandidate {
	protocol::ManifestKind kind=protocol::ManifestKind::FullRequired;
	std::uint32_t manifest_id=0, encoded_size=0, aggregate_subsystem_count=0;
	std::uint32_t class_record_count=0, weapon_record_count=0;
	std::uint32_t lifecycle_create_record_count=0, lifecycle_delete_record_count=0;
	std::array<Phase2ClassRecord,Phase2ManifestLimits::MaxClasses> class_records{};
	std::array<Phase2WeaponRecord,Phase2ManifestLimits::MaxWeapons> weapon_records{};
	std::array<Phase2SubsystemRecord,Phase2ManifestLimits::MaxAggregateSubsystems> subsystem_records{};
	std::array<Phase2BankRecord,Phase2ManifestLimits::MaxClasses*Phase2ManifestLimits::MaxBanksPerClass> bank_records{};
	std::array<Phase2AuxiliaryRecord,Phase2ManifestLimits::MaxAuxiliaryEntries> auxiliary_records{};
	std::uint32_t auxiliary_record_count=0;
	std::array<protocol::ManifestPartPayload, protocol::MaxTransactionParts> parts{};
	std::uint16_t part_count=0;
	protocol::ByteView encoded_bytes;
	protocol::Sha256Digest transaction_sha256{}, catalog_fingerprint{}, topology_fingerprint{};
	Phase2ManifestCandidate();
};

enum class Phase2ManifestError : std::uint8_t {
	None, NoCatalogChange, TopologyOnly, RebuildCoalesced, MissingRequiredDefinition,
	DuplicateDefinition, AmbiguousAuxiliaryName, InvalidString, InvalidSource,
	SourceLimitExceeded, TooManySubsystemsPerShip, TooManyAggregateSubsystems,
	DuplicateSystemInfo, ForeignSystemInfo, InvalidZeroMaximumState, InvalidCosine,
	NonFiniteDescriptor, AllocationFailed, InvalidFullRequiredCatalog,
	ManifestIdChangeRequiresKeyframe
};

class Phase2ManifestStorage {
  public:
	explicit Phase2ManifestStorage(protocol::MutableByteView combined);
	Phase2ManifestStorage(protocol::MutableByteView active, protocol::MutableByteView staged,
		std::pmr::memory_resource* fallback=nullptr);
	protocol::MutableByteView arenas[2]{};
	std::pmr::memory_resource* fallback=nullptr;
};

class Phase2ManifestSlot {
  public:
	explicit Phase2ManifestSlot(Phase2ManifestStorage& storage);
	Phase2ManifestError rebuild(const Phase2ManifestSource& source) noexcept;
	Phase2ManifestError validate_and_install(const protocol::CompletedTransaction& transaction) noexcept;
	Phase2ManifestError on_manifest_applied(std::uint32_t id) noexcept;
	Phase2ManifestError on_dependent_snapshot_applied(std::uint32_t snapshot, std::uint32_t id) noexcept;
	Phase2ManifestError validate_delta_required_manifest_id(std::uint32_t id) noexcept;
	void release_reliable_references(std::uint32_t id) noexcept;
	const Phase2ManifestCandidate& staged_candidate() const noexcept { return m_candidates[m_staged_index]; }
	const Phase2ManifestCandidate& active_candidate() const noexcept { return m_candidates[m_active_index]; }
	const Phase2ManifestCandidate* candidate_for_id(
		std::uint32_t id) const noexcept;
	std::uint32_t staged_manifest_id() const noexcept { return m_staged ? staged_candidate().manifest_id : 0; }
	std::uint32_t active_manifest_id() const noexcept { return m_active ? active_candidate().manifest_id : 0; }
	std::uint32_t previous_manifest_id() const noexcept {
		return m_retain_previous ? m_previous_id : 0U;
	}
	std::uint32_t required_manifest_id() const noexcept { return m_required_id; }
	bool is_active(std::uint32_t id) const noexcept { return m_active && active_manifest_id()==id; }
	bool is_staged(std::uint32_t id) const noexcept { return m_staged && staged_manifest_id()==id; }
	bool catalog_visible(std::uint32_t id) const noexcept { return is_active(id); }
	bool can_publish_snapshot_requiring(std::uint32_t id) const noexcept { return is_active(id); }
	bool can_publish_keyframe_requiring(std::uint32_t id) const noexcept { return keyframe_allowed_for(id); }
	bool keyframe_allowed_for(std::uint32_t id) const noexcept { return is_active(id)||(is_staged(id)&&m_manifest_applied); }
	bool keyframe_required_for(std::uint32_t id) const noexcept { return m_keyframe_required && (is_active(id)||is_staged(id)); }
	bool retains_generation(std::uint32_t id) const noexcept;
	std::uint32_t resident_generation_count() const noexcept { return (m_active?1u:0u)+(m_staged?1u:0u); }
	bool has_rebuild_intent() const noexcept { return m_rebuild_intent; }
	bool rebuild_intent_scheduled() const noexcept { return m_rebuild_scheduled; }
	const protocol::Sha256Digest& catalog_fingerprint() const noexcept;
	bool source_catalog_matches_active(
		const Phase2ManifestSource& source) const noexcept;
  private:
	Phase2ManifestStorage& m_storage;
	std::array<Phase2ManifestCandidate,2> m_candidates;
	std::uint8_t m_active_index=0, m_staged_index=1;
	bool m_active=false,m_staged=false,m_manifest_applied=false,m_keyframe_required=false;
	bool m_rebuild_intent=false,m_rebuild_scheduled=false,m_retain_previous=false;
	std::uint32_t m_required_id=0,m_previous_id=0,m_next_id=1;
};

} // namespace telemetry
