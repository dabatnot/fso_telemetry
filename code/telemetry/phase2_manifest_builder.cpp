#include "telemetry/phase2_manifest_builder.h"

#include "telemetry/protocol/packet_writer.h"
#include "telemetry/protocol/telemetry_business_records.h"
#include "telemetry/protocol/telemetry_records.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace telemetry {
using namespace protocol;
namespace {

std::uint32_t auxiliary_id(const Phase2ManifestSource& source, AuxiliaryRegistry registry,
	std::int32_t engine_index, Phase2ManifestError& error) noexcept
{
	if (engine_index == -1) return 0;
	const auto referenced=[&](const Phase2AuxiliaryEntry& entry) noexcept {
		for(std::uint32_t r=0;r<source.referenced_ship_class_count;++r) {
			const Phase2ClassSource* ship_class=nullptr;
			for(std::uint32_t c=0;c<source.ship_class_count;++c)
				if(source.ship_classes[c].source_key==source.referenced_ship_class_keys[r]) {
					ship_class=&source.ship_classes[c];break;
				}
			if(!ship_class)continue;
			if((registry==AuxiliaryRegistry::Species&&ship_class->species_index==entry.engine_index)||
				(registry==AuxiliaryRegistry::ShipType&&ship_class->ship_type_index==entry.engine_index)||
				(registry==AuxiliaryRegistry::Iff&&ship_class->iff_index==entry.engine_index)||
				(registry==AuxiliaryRegistry::Wing&&ship_class->wing_index==entry.engine_index)||
				(registry==AuxiliaryRegistry::Armor&&ship_class->armor_index==entry.engine_index)||
				(registry==AuxiliaryRegistry::DamageType&&ship_class->damage_type_index==entry.engine_index))
				return true;
			if(registry==AuxiliaryRegistry::Armor)
				for(std::uint32_t s=0;s<ship_class->subsystem_count;++s)
					if(ship_class->subsystems[s].armor_index==entry.engine_index)return true;
		}
		if(registry==AuxiliaryRegistry::DamageType)
			for(std::uint32_t r=0;r<source.referenced_weapon_count;++r)
				for(std::uint32_t w=0;w<source.weapon_count;++w)
					if(source.weapons[w].source_key==source.referenced_weapon_keys[r]&&
						source.weapons[w].damage_type_index==entry.engine_index)return true;
		return false;
	};
	std::array<const Phase2AuxiliaryEntry*, Phase2ManifestLimits::MaxAuxiliaryEntries> entries{};
	std::uint32_t count=0;
	for (std::uint32_t i=0;i<source.auxiliary_entry_count;++i)
		if (source.auxiliary_entries[i].registry==registry&&referenced(source.auxiliary_entries[i]))
			entries[count++]=&source.auxiliary_entries[i];
	std::sort(entries.begin(), entries.begin()+count, [](auto a, auto b){ return a->name < b->name; });
	for(std::uint32_t i=1;i<count;++i) if(entries[i-1]->name==entries[i]->name) {
		error=Phase2ManifestError::AmbiguousAuxiliaryName; return 0;
	}
	for(std::uint32_t i=0;i<count;++i) if(entries[i]->engine_index==engine_index) return i+1;
	error=Phase2ManifestError::InvalidSource; return 0;
}

bool contains(const std::uint32_t* values, std::uint32_t count, std::uint32_t key) noexcept
{
	for(std::uint32_t i=0;i<count;++i) if(values[i]==key) return true;
	return false;
}

std::size_t estimated_class_payload(const Phase2ClassSource& value) noexcept
{
	std::size_t result=42+value.name.size();
	for(std::uint32_t i=0;i<value.subsystem_count && i<value.subsystems.size();++i)
		result += 43 + value.subsystems[i].name.size()+value.subsystems[i].alt_name.size()+value.subsystems[i].hud_name.size();
	for(std::uint32_t i=0;i<value.bank_count && i<value.banks.size();++i)
		result += 22 + value.banks[i].fire_point_count*12 +
			(value.banks[i].family==telemetry::WeaponFamily::Secondary?4:0);
	return std::min<std::size_t>(65535, result);
}
std::size_t estimated_weapon_payload(const Phase2WeaponSource& w) noexcept
{
	std::size_t n=45+w.name.size()+w.title.size();
	n += w.has_acceleration?8:0; n += w.has_ranges?12:0; n += w.has_fire?12:0;
	n += w.has_damage?12:0; n += w.has_guidance?5:0; n += w.has_lock?12:0;
	n += w.has_cargo_rearm?16:0; n += w.has_burst?10:0; n += w.has_swarm?4:0;
	if(w.has_acceleration&&w.has_ranges&&w.has_fire&&w.has_damage&&w.has_guidance&&
		w.has_lock&&w.has_cargo_rearm&&w.has_burst&&w.has_swarm) return 654;
	return std::min<std::size_t>(654, n);
}

bool append_extension_padding(MutableByteView arena,std::size_t& offset,std::size_t bytes) noexcept
{
	while(bytes>=RecordEnvelopeHeaderSize) {
		const auto payload=std::min<std::size_t>(65535,bytes-RecordEnvelopeHeaderSize);
		if(offset+RecordEnvelopeHeaderSize+payload>arena.size)return false;
		PacketWriter writer({arena.data+offset,arena.size-offset});
		if(!writer.write_u16(0x7ffe)||!writer.write_u8(1)||!writer.write_u8(0)||
			!writer.write_u16(static_cast<std::uint16_t>(payload))||!writer.write_zeroes(payload))return false;
		offset+=writer.size();bytes-=writer.size();
	}
	return true;
}

bool write_class_record(MutableByteView arena,std::size_t& offset,std::uint32_t generation,
	std::uint32_t id,const Phase2ClassSource& source,const Phase2ClassRecord& projected) noexcept
{
	std::array<std::uint8_t,65535> payload{};
	PacketWriter body({payload.data(),payload.size()});
	std::uint16_t wire_bank_count=0;
	for(std::uint32_t index=0;index<source.bank_count;++index) {
		const auto& bank=projected.banks[index];
		if(bank.weapon_class_id||bank.family==telemetry::WeaponFamily::Tertiary||
			bank.family==telemetry::WeaponFamily::Turret)++wire_bank_count;
	}
	std::uint64_t presence=ClassManifestPresenceFlagInertia|ClassManifestPresenceFlagMotion|
		ClassManifestPresenceFlagHull;
	if(source.max_shield_strength>0)presence|=ClassManifestPresenceFlagShield;
	if(source.has_afterburner)presence|=ClassManifestPresenceFlagAfterburner;
	if(source.countermeasure_capacity>0)presence|=ClassManifestPresenceFlagCountermeasure;
	if(wire_bank_count>0)presence|=ClassManifestPresenceFlagBanks;
	presence|=ClassManifestPresenceFlagSubsystems;
	if(source.has_scan)presence|=ClassManifestPresenceFlagScan;
	if(source.has_glide)presence|=ClassManifestPresenceFlagGlide;
	if(source.has_autoaim)presence|=ClassManifestPresenceFlagAutoaim;
	if(!body.write_u32(generation)||!body.write_u32(id)||!body.write_u64(presence)||
		!body.write_utf8(source.name,255)||!body.write_u32(projected.species_id)||!body.write_u32(projected.ship_type_id)||
		!body.write_f32(source.effective_mass)||!body.write_f32(source.center_of_mass.x)||
		!body.write_f32(source.center_of_mass.y)||!body.write_f32(source.center_of_mass.z))
		return false;
	const auto has_full_inertia=std::any_of(source.effective_inertia_matrix.begin(),
		source.effective_inertia_matrix.end(),[](float value){return value!=0.0F;});
	for(std::size_t row=0;row<3;++row)for(std::size_t col=0;col<3;++col) {
		const auto value=has_full_inertia?source.effective_inertia_matrix[row*3+col]:
			(row==col?source.effective_inertia[row]:0.0F);
		if(!body.write_f32(value))return false;
	}
	const auto write_vec3=[&](const Phase2Vec3& value) noexcept {
		return body.write_f32(value.x)&&body.write_f32(value.y)&&body.write_f32(value.z);
	};
	if(!write_vec3(source.max_velocity)||!write_vec3(source.afterburner_max_velocity)||
		!write_vec3(source.booster_max_velocity)||!write_vec3(source.max_rotational_velocity)||
		!body.write_f32(source.max_rear_velocity)||!body.write_f32(source.forward_accel_time)||
		!body.write_f32(source.afterburner_forward_accel_time)||!body.write_f32(source.booster_forward_accel_time)||
		!body.write_f32(source.forward_decel_time)||!body.write_f32(source.slide_accel_time)||
		!body.write_f32(source.slide_decel_time)||!body.write_f32(source.max_hull_strength))return false;
	if(source.max_shield_strength>0&&!body.write_f32(source.max_shield_strength))return false;
	if(source.has_afterburner&&(!body.write_f32(source.afterburner_fuel_capacity)||
		!body.write_f32(source.afterburner_burn_rate)||!body.write_f32(source.afterburner_recover_rate)||
		!body.write_f32(source.afterburner_min_start_fuel)||!body.write_u64(source.afterburner_cooldown_us)))return false;
	if(source.countermeasure_capacity>0&&(!body.write_u32(projected.countermeasure_weapon_class_id)||
		!body.write_u32(projected.countermeasure_initial_count)||!body.write_u64(projected.countermeasure_firewait_us)))return false;
	if(wire_bank_count>0) {
		if(!body.write_u16(wire_bank_count))return false;
		for(std::uint32_t index=0;index<source.bank_count;++index) {
			const auto& bank=projected.banks[index];
			const auto& source_bank=source.banks[bank.source_index];
			if(!bank.weapon_class_id&&bank.family!=telemetry::WeaponFamily::Tertiary&&
				bank.family!=telemetry::WeaponFamily::Turret)continue;
			std::array<std::uint8_t,1024> item{};PacketWriter iw({item.data(),item.size()});
			std::uint16_t item_presence=0;
			if(bank.weapon_class_id)item_presence|=ClassBankPresenceFlagWeaponClass;
			if(bank.capacity>0)item_presence|=ClassBankPresenceFlagCapacity;
			if(!iw.write_u16(item_presence)||!iw.write_u8(static_cast<std::uint8_t>(bank.family))||
				!iw.write_u8(0)||!iw.write_u16(bank.canonical_index)||!iw.write_u32(bank.bank_id))return false;
			if(bank.weapon_class_id&&!iw.write_u32(bank.weapon_class_id))return false;
			if(bank.capacity>0&&!iw.write_f32(bank.capacity))return false;
			if(!iw.write_u16(static_cast<std::uint16_t>(bank.fire_point_count))||
				!iw.write_u8(1)||!iw.write_u16(12))return false;
			for(std::uint32_t p=0;p<bank.fire_point_count;++p)
				if(!iw.write_f32(source_bank.fire_points[p].x)||!iw.write_f32(source_bank.fire_points[p].y)||
					!iw.write_f32(source_bank.fire_points[p].z))return false;
			if(!body.write_u8(1)||!body.write_u16(static_cast<std::uint16_t>(iw.size()))||!body.write_bytes(iw.written()))return false;
		}
	}
	{
		if(!body.write_u16(static_cast<std::uint16_t>(source.subsystem_count)))return false;
		for(std::uint32_t index=0;index<source.subsystem_count;++index) {
			const auto& projected_sub=projected.subsystems[index];
			const auto& source_sub=source.subsystems[index];
			std::array<std::uint8_t,1024> item{};PacketWriter iw({item.data(),item.size()});
			std::uint16_t item_presence=0;
			if(!source_sub.alt_name.empty())item_presence|=ClassSubsystemPresenceFlagAltName;
			if(!source_sub.hud_name.empty())item_presence|=ClassSubsystemPresenceFlagHudName;
			if(projected_sub.armor_id)item_presence|=ClassSubsystemPresenceFlagArmor;
			const auto name=source_sub.name.empty()?std::string_view{"subsystem"}:std::string_view{source_sub.name};
			if(!iw.write_u16(item_presence)||!iw.write_u32(projected_sub.subsystem_id)||
				!iw.write_u16(static_cast<std::uint16_t>(projected_sub.canonical_index))||
				!iw.write_u8(static_cast<std::uint8_t>(source_sub.type))||!iw.write_u8(0)||
				!iw.write_utf8(name,255))return false;
			if(!source_sub.alt_name.empty()&&!iw.write_utf8(source_sub.alt_name,255))return false;
			if(!source_sub.hud_name.empty()&&!iw.write_utf8(source_sub.hud_name,255))return false;
			if(!iw.write_f32(source_sub.local_position.x)||!iw.write_f32(source_sub.local_position.y)||
				!iw.write_f32(source_sub.local_position.z)||!iw.write_f32(source_sub.radius)||
				!iw.write_f32(source_sub.max_hits))return false;
			if(projected_sub.armor_id&&!iw.write_u32(projected_sub.armor_id))return false;
			if(!iw.write_u32(source_sub.static_flags)||!body.write_u8(1)||!body.write_u16(static_cast<std::uint16_t>(iw.size()))||
				!body.write_bytes(iw.written()))return false;
		}
	}
	if(source.has_scan&&(!body.write_u64(source.scan_required_time_us)||!body.write_f32(source.scan_max_distance)||
		!body.write_f32(source.scan_max_angle_rad)))return false;
	if(source.has_glide&&!body.write_f32(source.glide_cap))return false;
	if(source.has_autoaim&&!body.write_f32(source.autoaim_fov_rad))return false;
	RecordEnvelopeView record{static_cast<std::uint16_t>(RecordType::ClassManifest),1,0,body.written()};
	std::size_t written=0;
	if(encode_business_record(record,BusinessRecordContainer::Manifest,
		{arena.data+offset,arena.size-offset},written)!=ValidationError::None)return false;
	offset+=written;
	return true;
}

bool write_weapon_record(MutableByteView arena,std::size_t& offset,std::uint32_t generation,
	std::uint32_t id,const Phase2WeaponSource& source,std::size_t target_size) noexcept
{
	std::array<std::uint8_t,65535> payload{};
	std::uint64_t presence=0;
	if(!source.title.empty())presence|=WeaponManifestPresenceFlagTitle;
	if(source.has_acceleration)presence|=WeaponManifestPresenceFlagAcceleration;
	if(source.has_ranges)presence|=WeaponManifestPresenceFlagRanges;
	if(source.has_fire)presence|=WeaponManifestPresenceFlagFire;
	if(source.has_damage)presence|=WeaponManifestPresenceFlagDamage;
	if(source.has_guidance)presence|=WeaponManifestPresenceFlagGuidance;
	if(source.has_lock)presence|=WeaponManifestPresenceFlagLock;
	if(source.has_cargo_rearm)presence|=WeaponManifestPresenceFlagCargoRearm;
	if(source.has_burst)presence|=WeaponManifestPresenceFlagBurst;
	if(source.has_swarm)presence|=WeaponManifestPresenceFlagSwarm;
	PacketWriter w({payload.data(),payload.size()});
	bool ok=w.write_u32(generation)&&w.write_u32(id)&&w.write_u64(presence)&&w.write_utf8(source.name,255);
	if(!source.title.empty())ok=ok&&w.write_utf8(source.title,255);
	ok=ok&&w.write_u8(0)&&w.write_u64(0)&&w.write_f32(0);
	if(source.has_acceleration)ok=ok&&w.write_u64(source.acceleration_time_us);
	ok=ok&&w.write_f32(0)&&w.write_f32(0)&&w.write_u64(0);
	if(source.has_ranges)ok=ok&&w.write_f32(source.minimum_range)&&w.write_f32(source.optimal_range)&&w.write_f32(source.maximum_range);
	if(source.has_fire)ok=ok&&w.write_u64(source.fire_wait_us)&&w.write_f32(source.energy_consumed);
	if(source.has_damage)ok=ok&&w.write_f32(source.damage)&&w.write_u32(0)&&w.write_u32(0);
	if(source.has_guidance)ok=ok&&w.write_u8(0)&&w.write_f32(source.guidance_fov_rad);
	if(source.has_lock)ok=ok&&w.write_u64(source.lock_time_us)&&w.write_f32(source.lock_fov_rad);
	ok=ok&&w.write_f32(0);
	if(source.has_cargo_rearm)ok=ok&&w.write_f32(source.cargo_size)&&w.write_u64(source.rearm_time_us)&&w.write_u32(source.reloaded_per_batch);
	if(source.has_burst)ok=ok&&w.write_u16(source.burst_count)&&w.write_u64(source.burst_interval_us);
	if(source.has_swarm)ok=ok&&w.write_u16(source.swarm_count)&&w.write_u16(source.shots_per_trigger);
	if(!ok)return false;
	RecordEnvelopeView record{static_cast<std::uint16_t>(RecordType::WeaponManifest),1,0,w.written()};
	std::size_t written=0;
	if(encode_business_record(record,BusinessRecordContainer::Manifest,{arena.data+offset,arena.size-offset},written)!=ValidationError::None)return false;
	offset+=written;
	return target_size<=written||append_extension_padding(arena,offset,target_size-written);
}

bool same_digest(const Sha256Digest& a,const Sha256Digest& b) noexcept { return a==b; }

} // namespace

Phase2ManifestCandidate::Phase2ManifestCandidate()
{
}

Phase2ManifestStorage::Phase2ManifestStorage(MutableByteView combined)
{
	const auto half=combined.size/2;
	arenas[0]={combined.data,half}; arenas[1]={combined.data+half,combined.size-half};
}
Phase2ManifestStorage::Phase2ManifestStorage(MutableByteView active, MutableByteView staged,
	std::pmr::memory_resource* resource) : arenas{active,staged}, fallback(resource) {}

Phase2ManifestSlot::Phase2ManifestSlot(Phase2ManifestStorage& storage) : m_storage(storage) {}

Phase2ManifestError Phase2ManifestSlot::rebuild(const Phase2ManifestSource& source) noexcept
{
	if(source.ship_class_count>Phase2ManifestLimits::MaxClasses ||
		source.weapon_count>Phase2ManifestLimits::MaxWeapons ||
		source.referenced_ship_class_count>Phase2ManifestLimits::MaxClasses ||
		source.referenced_weapon_count>Phase2ManifestLimits::MaxWeapons ||
		source.auxiliary_entry_count>Phase2ManifestLimits::MaxAuxiliaryEntries ||
		source.metadata.maximum_string_bytes>65535 || source.metadata.projected_record_count>65535 ||
		source.metadata.maximum_record_length>65535)
		return Phase2ManifestError::SourceLimitExceeded;
	for(std::uint32_t i=0;i<source.auxiliary_entry_count;++i) {
		const auto& a=source.auxiliary_entries[i];
		if(a.name.size()>255||!is_valid_utf8(a.name))return Phase2ManifestError::InvalidString;
		for(std::uint32_t j=0;j<i;++j)
			if(source.auxiliary_entries[j].registry==a.registry&&source.auxiliary_entries[j].name==a.name)
				return Phase2ManifestError::AmbiguousAuxiliaryName;
	}
	for(std::uint32_t i=0;i<source.ship_class_count;++i) {
		const auto& c=source.ship_classes[i];
		if(c.name.empty()||c.name.size()>255||!is_valid_utf8(c.name))return Phase2ManifestError::InvalidString;
		for(const auto cosine:c.half_angle_cosines) {
			if(!std::isfinite(cosine))return Phase2ManifestError::NonFiniteDescriptor;
			if(cosine < -1.0F || cosine > 1.0F)return Phase2ManifestError::InvalidCosine;
		}
		if(!std::isfinite(c.effective_mass)||!std::isfinite(c.countermeasure_capacity)||
			!std::isfinite(c.countermeasure_cargo_size))
			return Phase2ManifestError::NonFiniteDescriptor;
		for(const auto value:c.effective_inertia)
			if(!std::isfinite(value))return Phase2ManifestError::NonFiniteDescriptor;
		for(std::uint32_t s=0;s<std::min(c.subsystem_count,
			static_cast<std::uint32_t>(c.subsystems.size()));++s)
			if(c.subsystems[s].name.size()>255||c.subsystems[s].alt_name.size()>255||
				c.subsystems[s].hud_name.size()>255||!is_valid_utf8(c.subsystems[s].name)||
				!is_valid_utf8(c.subsystems[s].alt_name)||!is_valid_utf8(c.subsystems[s].hud_name))
				return Phase2ManifestError::InvalidString;
			else if(!std::isfinite(c.subsystems[s].max_hits)||!std::isfinite(c.subsystems[s].current_hits))
				return Phase2ManifestError::NonFiniteDescriptor;
		for(std::uint32_t b=0;b<std::min(c.bank_count,static_cast<std::uint32_t>(c.banks.size()));++b) {
			const auto& bank=c.banks[b];
			if(!std::isfinite(bank.capacity))return Phase2ManifestError::NonFiniteDescriptor;
			for(std::uint32_t p=0;p<std::min(bank.fire_point_count,
				static_cast<std::uint32_t>(bank.fire_points.size()));++p)
				if(!std::isfinite(bank.fire_points[p].x)||!std::isfinite(bank.fire_points[p].y)||
					!std::isfinite(bank.fire_points[p].z))return Phase2ManifestError::NonFiniteDescriptor;
		}
	}
	for(std::uint32_t i=0;i<source.weapon_count;++i) {
		if(source.weapons[i].name.empty()||source.weapons[i].name.size()>255||
			source.weapons[i].title.size()>255||!is_valid_utf8(source.weapons[i].name)||
			!is_valid_utf8(source.weapons[i].title))return Phase2ManifestError::InvalidString;
		const auto& weapon=source.weapons[i];
		for(const auto value:{weapon.minimum_range,weapon.optimal_range,weapon.maximum_range,
			weapon.energy_consumed,weapon.damage,weapon.guidance_fov_rad,weapon.lock_fov_rad,weapon.cargo_size})
			if(!std::isfinite(value))return Phase2ManifestError::NonFiniteDescriptor;
	}
	if((m_active && m_staged)||m_retain_previous) {
		m_rebuild_intent=true;
		return Phase2ManifestError::RebuildCoalesced;
	}
	if(m_next_id==0||m_next_id==UINT32_MAX)return Phase2ManifestError::InvalidSource;

	std::array<const Phase2ClassSource*,Phase2ManifestLimits::MaxClasses> classes{};
	std::array<const Phase2WeaponSource*,Phase2ManifestLimits::MaxWeapons> weapons{};
	for(std::uint32_t r=0;r<source.referenced_ship_class_count;++r) {
		const Phase2ClassSource* found=nullptr;
		for(std::uint32_t i=0;i<source.ship_class_count;++i) if(source.ship_classes[i].source_key==source.referenced_ship_class_keys[r]) {
			if(found) return Phase2ManifestError::DuplicateDefinition; found=&source.ship_classes[i];
		}
		if(!found) return Phase2ManifestError::MissingRequiredDefinition; classes[r]=found;
	}
	for(std::uint32_t r=0;r<source.referenced_weapon_count;++r) {
		const Phase2WeaponSource* found=nullptr;
		for(std::uint32_t i=0;i<source.weapon_count;++i) if(source.weapons[i].source_key==source.referenced_weapon_keys[r]) {
			if(found) return Phase2ManifestError::DuplicateDefinition; found=&source.weapons[i];
		}
		if(!found) return Phase2ManifestError::MissingRequiredDefinition; weapons[r]=found;
	}
	std::sort(classes.begin(),classes.begin()+source.referenced_ship_class_count,[](auto a,auto b){
		if(a->name!=b->name)return a->name<b->name; if(a->effective_mass!=b->effective_mass)return a->effective_mass<b->effective_mass;
		return a->subsystem_count<b->subsystem_count;
	});
	std::sort(weapons.begin(),weapons.begin()+source.referenced_weapon_count,[](auto a,auto b){
		if(a->name!=b->name)return a->name<b->name; return a->title<b->title;
	});
	auto index=m_staged_index;
	auto& candidate=m_candidates[index];
	auto arena=m_storage.arenas[index];
	if(arena.data==nullptr || arena.size==0) return Phase2ManifestError::AllocationFailed;
	PacketWriter semantic_writer(arena);
	bool semantic_ok=semantic_writer.write_u32(source.referenced_ship_class_count)&&
		semantic_writer.write_u32(source.referenced_weapon_count);
	for(std::uint32_t i=0;i<source.referenced_ship_class_count&&semantic_ok;++i) {
		const auto& c=*classes[i];
		semantic_ok=semantic_writer.write_utf8(c.name,255)&&semantic_writer.write_f32(c.effective_mass);
		for(float v:c.effective_inertia)semantic_ok=semantic_ok&&semantic_writer.write_f32(v);
		for(float v:c.half_angle_cosines)semantic_ok=semantic_ok&&semantic_writer.write_f32(v);
		semantic_ok=semantic_ok&&semantic_writer.write_u32(c.subsystem_count)&&semantic_writer.write_u32(c.bank_count);
		for(std::uint32_t s=0;s<c.subsystem_count&&semantic_ok;++s) {
			const auto& sub=c.subsystems[s];
			semantic_ok=semantic_writer.write_utf8(sub.name,255)&&semantic_writer.write_utf8(sub.alt_name,255)&&
				semantic_writer.write_utf8(sub.hud_name,255)&&semantic_writer.write_f32(sub.max_hits);
		}
	}
	for(std::uint32_t i=0;i<source.referenced_weapon_count&&semantic_ok;++i) {
		const auto& w=*weapons[i];
		semantic_ok=semantic_writer.write_utf8(w.name,255)&&semantic_writer.write_utf8(w.title,255)&&
			semantic_writer.write_f32(w.damage)&&semantic_writer.write_f32(w.maximum_range)&&
			semantic_writer.write_u64(w.fire_wait_us);
	}
	std::array<const Phase2AuxiliaryEntry*,Phase2ManifestLimits::MaxAuxiliaryEntries> aux{};
	for(std::uint32_t i=0;i<source.auxiliary_entry_count;++i)aux[i]=&source.auxiliary_entries[i];
	std::sort(aux.begin(),aux.begin()+source.auxiliary_entry_count,[](auto a,auto b){
		if(a->registry!=b->registry)return a->registry<b->registry;return a->name<b->name;});
	for(std::uint32_t i=0;i<source.auxiliary_entry_count&&semantic_ok;++i)
		semantic_ok=semantic_writer.write_u8(static_cast<std::uint8_t>(aux[i]->registry))&&semantic_writer.write_utf8(aux[i]->name,255);
	if(!semantic_ok)return Phase2ManifestError::AllocationFailed;
	Sha256Digest catalog{};
	if(!sha256(semantic_writer.written(),catalog))return Phase2ManifestError::InvalidSource;
	std::size_t offset=0;
	candidate.class_record_count=source.referenced_ship_class_count;
	candidate.weapon_record_count=source.referenced_weapon_count;
	candidate.aggregate_subsystem_count=0;
	std::uint32_t subsystem_record_offset=0;
	std::uint32_t bank_record_offset=0;
	const auto new_id=m_next_id;
	Phase2ManifestError error=Phase2ManifestError::None;
	for(std::uint32_t i=0;i<candidate.class_record_count;++i) {
		const auto& src=*classes[i];
		if(!is_valid_utf8(src.name) || src.name.size()>65535) return Phase2ManifestError::InvalidString;
		if(src.required_model_index==-1) return Phase2ManifestError::InvalidSource;
		if(src.bank_count>Phase2ManifestLimits::MaxBanksPerClass) return Phase2ManifestError::SourceLimitExceeded;
		if(src.subsystem_count>Phase2ManifestLimits::MaxSubsystemsPerShip) return Phase2ManifestError::TooManySubsystemsPerShip;
		if(candidate.aggregate_subsystem_count+src.subsystem_count>Phase2ManifestLimits::MaxAggregateSubsystems)
			return Phase2ManifestError::TooManyAggregateSubsystems;
		for(float v:src.half_angle_cosines) {
			if(!std::isfinite(v)) return Phase2ManifestError::NonFiniteDescriptor;
			if(v<-1||v>1) return Phase2ManifestError::InvalidCosine;
		}
		auto& dst=candidate.class_records[i]; dst.class_id=i+1; dst.name.assign(src.name); dst.mass=src.effective_mass;
		dst.subsystems={candidate.subsystem_records.data()+subsystem_record_offset,src.subsystem_count};
		dst.banks={candidate.bank_records.data()+bank_record_offset,src.bank_count};
		subsystem_record_offset+=src.subsystem_count;
		bank_record_offset+=src.bank_count;
		dst.inertia=src.effective_inertia;
		for(std::size_t a=0;a<3;++a) dst.half_angles_rad[a]=static_cast<float>(std::acos(src.half_angle_cosines[a]));
		dst.species_id=auxiliary_id(source,AuxiliaryRegistry::Species,src.species_index,error);
		dst.ship_type_id=auxiliary_id(source,AuxiliaryRegistry::ShipType,src.ship_type_index,error);
		dst.iff_id=auxiliary_id(source,AuxiliaryRegistry::Iff,src.iff_index,error);
		dst.wing_id=auxiliary_id(source,AuxiliaryRegistry::Wing,src.wing_index,error);
		dst.armor_id=auxiliary_id(source,AuxiliaryRegistry::Armor,src.armor_index,error);
		dst.damage_type_id=auxiliary_id(source,AuxiliaryRegistry::DamageType,src.damage_type_index,error);
		if(error!=Phase2ManifestError::None)return error;
		dst.subsystem_count=src.subsystem_count; dst.bank_count=src.bank_count;
		std::array<std::uint32_t,Phase2ManifestLimits::MaxSubsystemsPerShip> system_keys{};
		for(std::uint32_t s=0;s<src.subsystem_count;++s) {
			const auto& sub=src.subsystems[s];
			if(sub.system_info_key==UINT32_MAX)return Phase2ManifestError::ForeignSystemInfo;
			for(std::uint32_t p=0;p<s;++p)if(system_keys[p]==sub.system_info_key)return Phase2ManifestError::DuplicateSystemInfo;
			if(sub.max_hits==0&&sub.current_hits!=0)return Phase2ManifestError::InvalidZeroMaximumState;
			system_keys[s]=sub.system_info_key;
		}
		for(std::uint32_t s=0;s<src.subsystem_count;++s) {
			const auto& source_subsystem=src.subsystems[s];
			auto& projected_subsystem=dst.subsystems[s];
			projected_subsystem.subsystem_id=s+1; projected_subsystem.canonical_index=s;
			projected_subsystem.armor_id=auxiliary_id(source,AuxiliaryRegistry::Armor,source_subsystem.armor_index,error);
			if(error!=Phase2ManifestError::None)return error;
		}
		std::array<std::uint32_t,Phase2ManifestLimits::MaxBanksPerClass> bank_order{};
		std::array<std::uint32_t,Phase2ManifestLimits::MaxBanksPerClass> bank_weapon_ids{};
		for(std::uint32_t b=0;b<src.bank_count;++b) {
			const auto& source_bank=src.banks[b];
			if(source_bank.family!=source_bank.source_family ||
				source_bank.fire_point_count>Phase2ManifestLimits::MaxFirePoints ||
				source_bank.bank_index>63||!std::isfinite(source_bank.capacity)||source_bank.capacity<0)
				return Phase2ManifestError::InvalidSource;
			for(std::uint32_t p=0;p<source_bank.fire_point_count;++p)
				if(!std::isfinite(source_bank.fire_points[p].x)||!std::isfinite(source_bank.fire_points[p].y)||
					!std::isfinite(source_bank.fire_points[p].z))return Phase2ManifestError::NonFiniteDescriptor;
			if(source_bank.weapon_source_key) {
				for(std::uint32_t w=0;w<candidate.weapon_record_count;++w)
					if(weapons[w]->source_key==source_bank.weapon_source_key)bank_weapon_ids[b]=w+1;
				if(bank_weapon_ids[b]==0)return Phase2ManifestError::MissingRequiredDefinition;
			}
			if(source_bank.family==telemetry::WeaponFamily::Tertiary&&bank_weapon_ids[b]!=0)
				return Phase2ManifestError::InvalidSource;
			bank_order[b]=b;
		}
		std::sort(bank_order.begin(),bank_order.begin()+src.bank_count,[&](auto a,auto b){
			const auto& left=src.banks[a];const auto& right=src.banks[b];
			if(left.family!=right.family)return left.family<right.family;
			if(left.bank_index!=right.bank_index)return left.bank_index<right.bank_index;
			if(bank_weapon_ids[a]!=bank_weapon_ids[b])return bank_weapon_ids[a]<bank_weapon_ids[b];
			if(left.capacity!=right.capacity)return left.capacity<right.capacity;
			if(left.fire_point_count!=right.fire_point_count)return left.fire_point_count<right.fire_point_count;
			for(std::uint32_t p=0;p<left.fire_point_count;++p) {
				if(left.fire_points[p].x!=right.fire_points[p].x)return left.fire_points[p].x<right.fire_points[p].x;
				if(left.fire_points[p].y!=right.fire_points[p].y)return left.fire_points[p].y<right.fire_points[p].y;
				if(left.fire_points[p].z!=right.fire_points[p].z)return left.fire_points[p].z<right.fire_points[p].z;
			}
			return a<b;
		});
		for(std::uint32_t b=0;b<src.bank_count;++b) {
			const auto source_index=bank_order[b];const auto& source_bank=src.banks[source_index];
			auto& projected_bank=dst.banks[b];
			projected_bank.bank_id=b+1;projected_bank.weapon_class_id=bank_weapon_ids[source_index];
			projected_bank.family=source_bank.family;projected_bank.canonical_index=source_bank.bank_index;
			projected_bank.source_family=source_bank.source_family;projected_bank.source_index=static_cast<std::uint16_t>(source_index);
			projected_bank.capacity=source_bank.capacity;projected_bank.fire_point_count=source_bank.fire_point_count;
			projected_bank.owner_subsystem_id=source_bank.owner_subsystem_canonical_index==UINT16_MAX
				? 0U : static_cast<std::uint32_t>(source_bank.owner_subsystem_canonical_index)+1U;
		}
		if(src.countermeasure_weapon_source_key) {
			for(std::uint32_t w=0;w<candidate.weapon_record_count;++w)
				if(weapons[w]->source_key==src.countermeasure_weapon_source_key)dst.countermeasure_weapon_class_id=w+1;
		} else if(src.countermeasure_capacity>0.0F) {
			for(std::uint32_t w=0;w<candidate.weapon_record_count;++w)
				if(weapons[w]->name=="Countermeasure")dst.countermeasure_weapon_class_id=w+1;
		}
		if(src.countermeasure_capacity>0.0F&&dst.countermeasure_weapon_class_id==0)
			return Phase2ManifestError::MissingRequiredDefinition;
		dst.countermeasure_initial_count=src.countermeasure_uses_capacity && src.countermeasure_cargo_size>0
			? static_cast<std::uint32_t>(src.countermeasure_capacity/src.countermeasure_cargo_size)
			: static_cast<std::uint32_t>(src.countermeasure_capacity);
		dst.countermeasure_firewait_us=static_cast<std::uint64_t>(src.countermeasure_firewait_ms)*1000;
		candidate.aggregate_subsystem_count+=src.subsystem_count;
		if(!write_class_record(arena,offset,new_id,i+1,src,dst))
			return Phase2ManifestError::AllocationFailed;
	}
	for(std::uint32_t i=0;i<candidate.weapon_record_count;++i) {
		const auto& src=*weapons[i]; if(!is_valid_utf8(src.name)||!is_valid_utf8(src.title))return Phase2ManifestError::InvalidString;
		auto& dst=candidate.weapon_records[i];dst.weapon_class_id=i+1;dst.name.assign(src.name);
		dst.damage_type_id=auxiliary_id(source,AuxiliaryRegistry::DamageType,src.damage_type_index,error);
		if(error!=Phase2ManifestError::None)return error;
		if(!write_weapon_record(arena,offset,new_id,i+1,src,estimated_weapon_payload(src)+RecordEnvelopeHeaderSize))
			return Phase2ManifestError::AllocationFailed;
	}
	Sha256Digest topology{}; sha256({reinterpret_cast<const std::uint8_t*>(&source.player_instance_signature),4},topology);
	const auto has_baseline=m_active||m_staged;
	const auto& baseline=m_active?active_candidate():staged_candidate();
	if(has_baseline && same_digest(baseline.catalog_fingerprint,catalog)) {
		if(baseline.topology_fingerprint==topology){return Phase2ManifestError::NoCatalogChange;}
		m_keyframe_required=true; return Phase2ManifestError::TopologyOnly;
	}
	candidate.manifest_id=new_id;candidate.kind=ManifestKind::FullRequired;candidate.encoded_size=static_cast<std::uint32_t>(offset);
	candidate.encoded_bytes={arena.data,offset};candidate.catalog_fingerprint=catalog;candidate.topology_fingerprint=topology;
	sha256(candidate.encoded_bytes,candidate.transaction_sha256);
	std::size_t begin=0;std::uint16_t part=0;
	while(begin<offset) {
		if(part==MaxTransactionParts)return Phase2ManifestError::AllocationFailed;
		const auto limit=std::min<std::size_t>(offset,begin+MaxStatePartSize-ManifestPartPayloadPrefixSize);
		std::size_t end=begin;std::uint16_t count=0;
		while(end<limit) { const auto len=RecordEnvelopeHeaderSize+static_cast<std::size_t>(arena.data[end+4]|(arena.data[end+5]<<8));
			if(end+len>limit)break;end+=len;++count; }
		auto& p=candidate.parts[part];p={};p.manifest_id=new_id;p.part_index=part;p.transaction_size=candidate.encoded_size;
		p.transaction_sha256=candidate.transaction_sha256;p.manifest_kind=ManifestKind::FullRequired;p.record_count=count;p.records={arena.data+begin,end-begin};
		begin=end;++part;
	}
	candidate.part_count=part;for(std::uint16_t i=0;i<part;++i)candidate.parts[i].part_count=part;
	m_staged=true;m_manifest_applied=false;m_keyframe_required=true;m_next_id++;m_required_id=m_active?active_manifest_id():0;
	return Phase2ManifestError::None;
}

Phase2ManifestError Phase2ManifestSlot::validate_and_install(const CompletedTransaction& t) noexcept
{
	if(t.message_type!=MessageType::Manifest || t.kind_or_flags!=static_cast<std::uint16_t>(ManifestKind::FullRequired))
		return Phase2ManifestError::InvalidFullRequiredCatalog;
	std::uint32_t classes=0,weapons=0;
	std::array<bool,Phase2ManifestLimits::MaxClasses+1> class_ids{};
	std::array<bool,Phase2ManifestLimits::MaxWeapons+1> weapon_ids{};
	for(const auto& part:t.parts){RecordEnvelopeIterator it(part.records_view(),part.record_count,RecordFlagPolicy::RequireNone);
		for(;;){RecordEnvelopeView e{};bool more=false;if(it.next(e,more)!=ValidationError::None)return Phase2ManifestError::InvalidFullRequiredCatalog;
			if(!more)break;
			BusinessRecordMetadata metadata{};
			if(validate_business_record(e,BusinessRecordContainer::Manifest,metadata)!=ValidationError::None)
				return Phase2ManifestError::InvalidFullRequiredCatalog;
			if(e.raw_record_type==static_cast<std::uint16_t>(RecordType::ClassManifest)||
				e.raw_record_type==static_cast<std::uint16_t>(RecordType::WeaponManifest)) {
				if(e.payload.size<8)return Phase2ManifestError::InvalidFullRequiredCatalog;
				const auto generation=static_cast<std::uint32_t>(e.payload.data[0])|
					(static_cast<std::uint32_t>(e.payload.data[1])<<8)|
					(static_cast<std::uint32_t>(e.payload.data[2])<<16)|
					(static_cast<std::uint32_t>(e.payload.data[3])<<24);
				const auto id=static_cast<std::uint32_t>(e.payload.data[4])|
					(static_cast<std::uint32_t>(e.payload.data[5])<<8)|
					(static_cast<std::uint32_t>(e.payload.data[6])<<16)|
					(static_cast<std::uint32_t>(e.payload.data[7])<<24);
				if(generation!=t.transaction_id||id==0)return Phase2ManifestError::InvalidFullRequiredCatalog;
				if(e.raw_record_type==static_cast<std::uint16_t>(RecordType::ClassManifest)) {
					if(id>Phase2ManifestLimits::MaxClasses||class_ids[id])return Phase2ManifestError::InvalidFullRequiredCatalog;
					class_ids[id]=true;++classes;
				}else{
					if(id>Phase2ManifestLimits::MaxWeapons||weapon_ids[id])return Phase2ManifestError::InvalidFullRequiredCatalog;
					weapon_ids[id]=true;++weapons;
				}
			}}}
	if(!m_staged||classes!=staged_candidate().class_record_count||weapons!=staged_candidate().weapon_record_count)
		return Phase2ManifestError::InvalidFullRequiredCatalog;
	return Phase2ManifestError::None;
}
Phase2ManifestError Phase2ManifestSlot::on_manifest_applied(std::uint32_t id) noexcept
{
	if(is_active(id)|| (m_manifest_applied&&is_staged(id)))return Phase2ManifestError::None;
	if(!is_staged(id))return retains_generation(id)?Phase2ManifestError::None:Phase2ManifestError::InvalidSource;
	m_manifest_applied=true;
	if(!m_active)m_required_id=id;
	return Phase2ManifestError::None;
}
Phase2ManifestError Phase2ManifestSlot::on_dependent_snapshot_applied(std::uint32_t, std::uint32_t id) noexcept
{
	if(is_active(id))return Phase2ManifestError::None;
	if(!is_staged(id)||!m_manifest_applied)return retains_generation(id)?Phase2ManifestError::None:Phase2ManifestError::InvalidSource;
	m_previous_id=m_active?active_manifest_id():0;m_retain_previous=m_active;std::swap(m_active_index,m_staged_index);
	m_active=true;m_staged=false;m_manifest_applied=false;m_keyframe_required=false;m_required_id=id;
	m_rebuild_scheduled=m_rebuild_intent;return Phase2ManifestError::None;
}
Phase2ManifestError Phase2ManifestSlot::validate_delta_required_manifest_id(std::uint32_t id) noexcept
{ return id==m_required_id?Phase2ManifestError::None:Phase2ManifestError::ManifestIdChangeRequiresKeyframe; }
void Phase2ManifestSlot::release_reliable_references(std::uint32_t id) noexcept
{ if(id==m_previous_id)m_retain_previous=false; }
bool Phase2ManifestSlot::retains_generation(std::uint32_t id) const noexcept
{ return is_active(id)||is_staged(id)||(m_retain_previous&&m_previous_id==id); }
const Sha256Digest& Phase2ManifestSlot::catalog_fingerprint() const noexcept
{ return m_active?active_candidate().catalog_fingerprint:staged_candidate().catalog_fingerprint; }

} // namespace telemetry
