#include "telemetry/phase2_manifest_test_support.h"

#include "telemetry/protocol/telemetry_records.h"
#include "telemetry/protocol/telemetry_sha256.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace telemetry::test::phase2test {
namespace {

std::unique_ptr<Phase2ManifestSource> base_source()
{
	auto source = std::make_unique<Phase2ManifestSource>();
	source->ship_classes[0].source_key = 41;
	source->ship_classes[0].name = "Ulysses";
	source->ship_classes[0].model_mass = 100.0F;
	source->ship_classes[0].density_provenance = 1.0F;
	source->ship_classes[0].effective_mass = 100.0F;
	source->ship_class_count = 1;
	source->referenced_ship_class_keys[0] = 41;
	source->referenced_ship_class_count = 1;
	return source;
}

void add_weapon(Phase2ManifestSource& source, std::uint32_t key, const std::string& name, bool referenced)
{
	auto& weapon = source.weapons[source.weapon_count++];
	weapon.source_key = key;
	weapon.name = name;
	if (referenced) {
		source.referenced_weapon_keys[source.referenced_weapon_count++] = key;
	}
}

void refresh_transaction_metadata(protocol::CompletedTransaction& transaction)
{
	std::vector<std::uint8_t> bytes;
	std::size_t total = 0;
	for (const auto& part : transaction.parts) total += part.records.size();
	bytes.reserve(total);
	for (const auto& part : transaction.parts) {
		bytes.insert(bytes.end(), part.records.begin(), part.records.end());
	}
	transaction.transaction_size = static_cast<std::uint32_t>(bytes.size());
	protocol::sha256(protocol::ByteView{bytes.data(), bytes.size()}, transaction.transaction_sha256);
}

} // namespace

std::unique_ptr<Phase2ManifestSource> make_source(SourceCase source_case)
{
	auto storage = base_source();
	auto& source = *storage;
	auto& value = source;
	switch (source_case) {
	case SourceCase::TwoClassesThreeWeaponsWithDecoys:
		value.ship_classes[1] = value.ship_classes[0];
		value.ship_classes[1].source_key = 42;
		value.ship_classes[1].name = "Apollo";
		value.ship_class_count = 2;
		value.referenced_ship_class_keys[1] = 42;
		value.referenced_ship_class_count = 2;
		add_weapon(value, 101, "Alpha", true);
		add_weapon(value, 102, "Beta", true);
		add_weapon(value, 103, "Gamma", true);
		add_weapon(value, 9001, "WP03-DECOY-WEAPON", false);
		value.ship_classes[2] = value.ship_classes[0];
		value.ship_classes[2].source_key = 9000;
		value.ship_classes[2].name = "WP03-DECOY-CLASS";
		value.ship_class_count = 3;
		break;
	case SourceCase::CanonicalIdsWithHullTurretAndTertiaryBanks:
		value.ship_classes[0].subsystem_count = 2;
		value.ship_classes[0].subsystems[0].source_key = 80;
		value.ship_classes[0].subsystems[0].system_info_key = 20;
		value.ship_classes[0].subsystems[1].source_key = 10;
		value.ship_classes[0].subsystems[1].system_info_key = 10;
		value.ship_classes[0].bank_count = 3;
		break;
	case SourceCase::AllAuxiliaryRegistries:
		source.auxiliary_entries[0] = {AuxiliaryRegistry::Species, 9, "Zulu"};
		source.auxiliary_entries[1] = {AuxiliaryRegistry::Species, 2, "Alpha"};
		source.auxiliary_entries[2] = {AuxiliaryRegistry::ShipType, 8, "ZuluType"};
		source.auxiliary_entries[3] = {AuxiliaryRegistry::ShipType, 1, "AlphaType"};
		source.auxiliary_entries[4] = {AuxiliaryRegistry::Iff, 7, "ZuluIFF"};
		source.auxiliary_entries[5] = {AuxiliaryRegistry::Iff, 6, "AlphaIFF"};
		source.auxiliary_entries[6] = {AuxiliaryRegistry::Wing, 12, "ZuluWing"};
		source.auxiliary_entries[7] = {AuxiliaryRegistry::Wing, 3, "AlphaWing"};
		source.auxiliary_entries[8] = {AuxiliaryRegistry::Armor, 5, "ZuluArmor"};
		source.auxiliary_entries[9] = {AuxiliaryRegistry::Armor, 4, "AlphaArmor"};
		source.auxiliary_entries[10] = {AuxiliaryRegistry::DamageType, 14, "ZuluDamage"};
		source.auxiliary_entries[11] = {AuxiliaryRegistry::DamageType, 10, "AlphaDamage"};
		source.auxiliary_entry_count = 12;
		source.ship_classes[0].species_index = 9;
		source.ship_classes[0].ship_type_index = 8;
		source.ship_classes[0].iff_index = 7;
		source.ship_classes[0].wing_index = 12;
		source.ship_classes[0].subsystem_count = 1;
		source.ship_classes[0].subsystems[0].source_key = 1;
		source.ship_classes[0].subsystems[0].system_info_key = 1;
		source.ship_classes[0].subsystems[0].armor_index = 5;
		add_weapon(source, 101, "Countermeasure", true);
		source.weapons[0].damage_type_index = 14;
		source.ship_classes[1] = source.ship_classes[0];
		source.ship_classes[1].source_key = 42;
		source.ship_classes[1].name = "Auxiliary Alpha";
		source.ship_classes[1].species_index = 2;
		source.ship_classes[1].ship_type_index = 1;
		source.ship_classes[1].iff_index = 6;
		source.ship_classes[1].wing_index = 3;
		source.ship_classes[1].subsystems[0].armor_index = 4;
		source.ship_class_count = 2;
		source.referenced_ship_class_keys[1] = 42;
		source.referenced_ship_class_count = 2;
		add_weapon(source, 102, "Auxiliary weapon", true);
		source.weapons[1].damage_type_index = 10;
		break;
	case SourceCase::MultipartFullRequired:
		for (std::uint32_t key = 1; key <= 4096; ++key) {
			auto name = std::string(247, static_cast<char>('A' + (key % 26))) + std::to_string(key);
			name.resize(255, 'N');
			add_weapon(source, 1000 + key, name, true);
			source.weapons[source.weapon_count - 1].title.assign(255, 'T');
		}
		break;
	case SourceCase::NearMaxTransaction:
		source = std::move(*make_source(SourceCase::MultipartFullRequired));
		source.ship_class_count = 64;
		source.referenced_ship_class_count = 64;
		for (std::uint32_t class_index = 0; class_index < 64; ++class_index) {
			if (class_index > 0) source.ship_classes[class_index] = source.ship_classes[0];
			auto& ship_class = source.ship_classes[class_index];
			ship_class.source_key = 10000 + class_index;
			ship_class.name.assign(255, static_cast<char>('A' + class_index % 26));
			ship_class.subsystem_count = 64;
			source.referenced_ship_class_keys[class_index] = ship_class.source_key;
			for (std::uint32_t subsystem = 0; subsystem < 64; ++subsystem) {
				ship_class.subsystems[subsystem].source_key = 100000 + class_index * 64 + subsystem;
				ship_class.subsystems[subsystem].system_info_key =
					ship_class.subsystems[subsystem].source_key;
				ship_class.subsystems[subsystem].name.assign(255, 'S');
				ship_class.subsystems[subsystem].alt_name.assign(255, 'A');
				ship_class.subsystems[subsystem].hud_name.assign(255, 'H');
				ship_class.subsystems[subsystem].max_hits = 100.0F;
				ship_class.subsystems[subsystem].current_hits = 100.0F;
			}
			ship_class.bank_count = 8;
			for (std::uint32_t bank_index = 0; bank_index < ship_class.bank_count; ++bank_index) {
				auto& bank = ship_class.banks[bank_index];
				bank.family = bank_index < 4 ? WeaponFamily::Primary : WeaponFamily::Secondary;
				bank.source_family = WeaponFamily::None;
				bank.bank_index = static_cast<std::uint16_t>(bank_index % 4);
				bank.weapon_source_key = 1001 + ((class_index * 8 + bank_index) % 4096);
				bank.capacity = bank.family == WeaponFamily::Secondary ? 100.0F : 0.0F;
				bank.fire_point_count = 64;
				for (std::uint32_t point = 0; point < bank.fire_point_count; ++point) {
					bank.fire_points[point] = {static_cast<float>(point), 0.0F, 0.0F};
				}
			}
			ship_class.countermeasure_capacity = 20.0F;
			ship_class.countermeasure_cargo_size = 2.0F;
			ship_class.countermeasure_uses_capacity = true;
			ship_class.countermeasure_weapon_source_key = 1001;
			ship_class.countermeasure_firewait_ms = 250;
		}
		for (std::uint32_t weapon_index = 0; weapon_index < source.weapon_count; ++weapon_index) {
			auto& weapon = source.weapons[weapon_index];
			weapon.has_acceleration = true;
			weapon.has_ranges = true;
			weapon.has_fire = true;
			weapon.has_damage = true;
			weapon.has_guidance = true;
			weapon.has_lock = true;
			weapon.has_cargo_rearm = true;
			weapon.has_burst = true;
			weapon.has_swarm = true;
			weapon.acceleration_time_us = 1;
			weapon.minimum_range = 1.0F;
			weapon.optimal_range = 2.0F;
			weapon.maximum_range = 3.0F;
			weapon.fire_wait_us = 1;
			weapon.energy_consumed = 1.0F;
			weapon.damage = 1.0F;
			weapon.guidance_fov_rad = 1.0F;
			weapon.lock_time_us = 1;
			weapon.lock_fov_rad = 1.0F;
			weapon.cargo_size = 1.0F;
			weapon.rearm_time_us = 1;
			weapon.reloaded_per_batch = 1;
			weapon.burst_count = 1;
			weapon.burst_interval_us = 1;
			weapon.swarm_count = 1;
			weapon.shots_per_trigger = 1;
		}
		break;
	}
	return storage;
}

std::unique_ptr<Phase2ManifestSource> make_boundary_source(BoundaryCase boundary_case)
{
	auto storage = base_source();
	auto& source = *storage;
	switch (boundary_case) {
	case BoundaryCase::String65535:
		source.metadata.maximum_string_bytes = 65535;
		break;
	case BoundaryCase::String65536:
		source.metadata.maximum_string_bytes = 65536;
		break;
	case BoundaryCase::RecordCount65535:
		source.metadata.projected_record_count = 65535;
		break;
	case BoundaryCase::RecordCount65536:
		source.metadata.projected_record_count = 65536;
		break;
	case BoundaryCase::RecordLength65535:
		source.metadata.maximum_record_length = 65535;
		break;
	case BoundaryCase::RecordLength65536:
		source.metadata.maximum_record_length = 65536;
		break;
	case BoundaryCase::ClassBanks192:
		source.ship_classes[0].bank_count = 192;
		break;
	case BoundaryCase::ClassBanks193:
		source.ship_classes[0].bank_count = 193;
		break;
	}
	return storage;
}

std::unique_ptr<Phase2ManifestSource> make_subsystem_source(std::initializer_list<std::uint32_t> counts)
{
	auto storage = base_source();
	auto& source = *storage;
	source.ship_class_count = static_cast<std::uint32_t>(counts.size());
	source.referenced_ship_class_count = source.ship_class_count;
	std::uint32_t class_index = 0;
	std::uint32_t subsystem_key = 1;
	for (const auto count : counts) {
		if (class_index > 0) source.ship_classes[class_index] = source.ship_classes[0];
		auto& ship_class = source.ship_classes[class_index];
		ship_class.source_key = 100 + class_index;
		ship_class.name = "Subsystem fixture " + std::to_string(class_index);
		ship_class.subsystem_count = count;
		source.referenced_ship_class_keys[class_index] = ship_class.source_key;
		const auto stored_count = std::min<std::uint32_t>(count, Phase2ManifestLimits::MaxSubsystemsPerShip);
		for (std::uint32_t subsystem = 0; subsystem < stored_count; ++subsystem) {
			ship_class.subsystems[subsystem].source_key = subsystem_key;
			ship_class.subsystems[subsystem].system_info_key = subsystem_key;
			++subsystem_key;
		}
		++class_index;
	}
	return storage;
}

void reverse_engine_order(Phase2ManifestSource& source)
{
	std::reverse(source.ship_classes.begin(), source.ship_classes.begin() + source.ship_class_count);
	std::reverse(source.weapons.begin(), source.weapons.begin() + source.weapon_count);
	std::reverse(source.referenced_ship_class_keys.begin(),
		source.referenced_ship_class_keys.begin() + source.referenced_ship_class_count);
	std::reverse(source.referenced_weapon_keys.begin(),
		source.referenced_weapon_keys.begin() + source.referenced_weapon_count);
}

void erase_weapon_definition(Phase2ManifestSource& source, std::uint32_t source_key)
{
	const auto end = source.weapons.begin() + source.weapon_count;
	const auto found =
		std::find_if(source.weapons.begin(), end, [source_key](const auto& value) { return value.source_key == source_key; });
	if (found == end) return;
	std::move(found + 1, end, found);
	--source.weapon_count;
}

void duplicate_weapon_definition(Phase2ManifestSource& source, std::uint32_t source_key)
{
	const auto end = source.weapons.begin() + source.weapon_count;
	const auto found =
		std::find_if(source.weapons.begin(), end, [source_key](const auto& value) { return value.source_key == source_key; });
	if (found != end) source.weapons[source.weapon_count++] = *found;
}

protocol::CompletedTransaction
mutate_completed_catalog(const Phase2ManifestCandidate& candidate, CatalogMutation mutation)
{
	protocol::CompletedTransaction result{};
	result.message_type = protocol::MessageType::Manifest;
	result.transaction_id = candidate.manifest_id;
	result.kind_or_flags = static_cast<std::uint16_t>(protocol::ManifestKind::FullRequired);
	for (std::uint16_t index = 0; index < candidate.part_count; ++index) {
		protocol::CompletedTransactionPart part{};
		part.message_id = index + 1;
		part.record_count = candidate.parts[index].record_count;
		part.records.assign(candidate.parts[index].records.begin(), candidate.parts[index].records.end());
		result.parts.push_back(std::move(part));
	}

	const auto wanted = mutation == CatalogMutation::MissingClass || mutation == CatalogMutation::DuplicateClass ||
			mutation == CatalogMutation::UnauthorizedExtraClass
		? protocol::RecordType::ClassManifest
		: protocol::RecordType::WeaponManifest;
	for (auto& part : result.parts) {
		protocol::RecordEnvelopeIterator iterator(part.records_view(), part.record_count,
			protocol::RecordFlagPolicy::RequireNone);
		std::size_t offset = 0;
		for (;;) {
			protocol::RecordEnvelopeView envelope{};
			bool has_value = false;
			if (iterator.next(envelope, has_value) != protocol::ValidationError::None || !has_value) break;
			const auto size = protocol::RecordEnvelopeHeaderSize + envelope.payload.size;
			if (envelope.raw_record_type == static_cast<std::uint16_t>(wanted)) {
				if (mutation == CatalogMutation::MissingClass || mutation == CatalogMutation::MissingWeapon) {
					part.records.erase(part.records.begin() + static_cast<std::ptrdiff_t>(offset),
						part.records.begin() + static_cast<std::ptrdiff_t>(offset + size));
					--part.record_count;
				} else {
					std::vector<std::uint8_t> copy(part.records.begin() + static_cast<std::ptrdiff_t>(offset),
						part.records.begin() + static_cast<std::ptrdiff_t>(offset + size));
					if (mutation == CatalogMutation::UnauthorizedExtraClass ||
						mutation == CatalogMutation::UnauthorizedExtraWeapon) {
						// Payload starts with manifest_generation:u32, then the
						// public class_id/weapon_class_id at offset +4.
						copy[protocol::RecordEnvelopeHeaderSize + 4] = 0xfe;
						copy[protocol::RecordEnvelopeHeaderSize + 5] = 0xff;
						copy[protocol::RecordEnvelopeHeaderSize + 6] = 0xff;
						copy[protocol::RecordEnvelopeHeaderSize + 7] = 0x7f;
					}
					part.records.insert(part.records.end(), copy.begin(), copy.end());
					++part.record_count;
				}
				refresh_transaction_metadata(result);
				return result;
			}
			offset += size;
		}
	}
	refresh_transaction_metadata(result);
	return result;
}

} // namespace telemetry::test::phase2test
