#include "telemetry/phase2_state_image.h"
#include "telemetry/phase2_allocation_tracker.h"
#include "telemetry/protocol/packet_reader.h"
#include "telemetry/protocol/telemetry_business_records.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>

namespace {

using namespace telemetry;
using namespace telemetry::detail;
using namespace telemetry::protocol;

struct WeaponHeader {
	std::uint64_t entity = 0U;
	std::uint64_t presence = 0U;
	std::uint64_t sample = 0U;
	std::uint16_t primary_count = 0U;
	std::uint16_t secondary_count = 0U;
	std::uint16_t tertiary_count = 0U;
	std::uint32_t current_primary = 0U;
	std::uint32_t current_secondary = 0U;
	std::uint32_t current_tertiary = 0U;
	std::uint32_t flags = 0U;
};

struct PrimaryBank {
	std::uint16_t presence = 0U;
	std::uint16_t index = 0U;
	std::uint32_t id = 0U;
	std::uint32_t weapon = 0U;
	std::uint64_t cooldown = 0U;
	std::uint16_t slot = 0U;
	std::uint16_t fire_point = 0U;
	std::uint16_t simultaneous = 0U;
	std::uint16_t pattern = 0U;
	std::uint32_t ammunition = 0U;
	std::uint32_t initial = 0U;
	float capacity = 0.0F;
	std::uint64_t rearm = 0U;
	std::uint16_t burst = 0U;
	std::uint32_t seed = 0U;
	std::uint16_t substitution = 0U;
	std::uint64_t fof = 0U;
};

struct SecondaryBank {
	std::uint16_t presence = 0U;
	std::uint16_t index = 0U;
	std::uint32_t id = 0U;
	std::uint32_t weapon = 0U;
	std::uint64_t cooldown = 0U;
	std::uint16_t slot = 0U;
	std::uint32_t ammunition = 0U;
	std::uint32_t initial = 0U;
	float capacity = 0.0F;
	std::uint64_t rearm = 0U;
	std::uint16_t burst = 0U;
	std::uint32_t seed = 0U;
	std::uint16_t substitution = 0U;
};

PacketReader read_vlist_item(PacketReader& list)
{
	std::uint8_t version = 0U;
	std::uint16_t size = 0U;
	EXPECT_TRUE(list.read_u8(version));
	EXPECT_EQ(1U, version);
	EXPECT_TRUE(list.read_u16(size));
	PacketReader item;
	EXPECT_TRUE(list.subreader(size, item));
	return item;
}

PrimaryBank read_primary(PacketReader& list)
{
	auto item = read_vlist_item(list);
	PrimaryBank value{};
	std::uint16_t reserved = 1U;
	EXPECT_TRUE(item.read_u16(value.presence));
	EXPECT_TRUE(item.read_u16(value.index));
	EXPECT_TRUE(item.read_u32(value.id));
	EXPECT_TRUE(item.read_u32(value.weapon));
	EXPECT_TRUE(item.read_u64(value.cooldown));
	EXPECT_TRUE(item.read_u16(value.slot));
	EXPECT_TRUE(item.read_u16(value.fire_point));
	EXPECT_TRUE(item.read_u16(value.simultaneous));
	EXPECT_TRUE(item.read_u16(value.pattern));
	if ((value.presence & PrimaryBankPresenceFlagBallisticAmmo) != 0U) {
		EXPECT_TRUE(item.read_u32(value.ammunition));
		EXPECT_TRUE(item.read_u32(value.initial));
		EXPECT_TRUE(item.read_f32(value.capacity));
	}
	if ((value.presence & PrimaryBankPresenceFlagRearm) != 0U)
		EXPECT_TRUE(item.read_u64(value.rearm));
	if ((value.presence & PrimaryBankPresenceFlagBurst) != 0U) {
		EXPECT_TRUE(item.read_u16(value.burst));
		EXPECT_TRUE(item.read_u16(reserved));
		EXPECT_EQ(0U, reserved);
		EXPECT_TRUE(item.read_u32(value.seed));
	}
	if ((value.presence & PrimaryBankPresenceFlagSubstitution) != 0U) {
		EXPECT_TRUE(item.read_u16(value.substitution));
		EXPECT_TRUE(item.read_u16(reserved));
		EXPECT_EQ(0U, reserved);
	}
	EXPECT_EQ(0U, value.presence & PrimaryBankPresenceFlagAnimation);
	if ((value.presence & PrimaryBankPresenceFlagFofCooldown) != 0U)
		EXPECT_TRUE(item.read_u64(value.fof));
	EXPECT_TRUE(item.at_end());
	return value;
}

SecondaryBank read_secondary(PacketReader& list)
{
	auto item = read_vlist_item(list);
	SecondaryBank value{};
	std::uint16_t reserved = 1U;
	EXPECT_TRUE(item.read_u16(value.presence));
	EXPECT_TRUE(item.read_u16(value.index));
	EXPECT_TRUE(item.read_u32(value.id));
	EXPECT_TRUE(item.read_u32(value.weapon));
	EXPECT_TRUE(item.read_u64(value.cooldown));
	EXPECT_TRUE(item.read_u16(value.slot));
	EXPECT_TRUE(item.read_u16(reserved));
	EXPECT_EQ(0U, reserved);
	if ((value.presence & SecondaryBankPresenceFlagAmmo) != 0U) {
		EXPECT_TRUE(item.read_u32(value.ammunition));
		EXPECT_TRUE(item.read_u32(value.initial));
		EXPECT_TRUE(item.read_f32(value.capacity));
	}
	if ((value.presence & SecondaryBankPresenceFlagRearm) != 0U)
		EXPECT_TRUE(item.read_u64(value.rearm));
	if ((value.presence & SecondaryBankPresenceFlagBurst) != 0U) {
		EXPECT_TRUE(item.read_u16(value.burst));
		EXPECT_TRUE(item.read_u16(reserved));
		EXPECT_EQ(0U, reserved);
		EXPECT_TRUE(item.read_u32(value.seed));
	}
	if ((value.presence & SecondaryBankPresenceFlagSubstitution) != 0U) {
		EXPECT_TRUE(item.read_u16(value.substitution));
		EXPECT_TRUE(item.read_u16(reserved));
		EXPECT_EQ(0U, reserved);
	}
	EXPECT_EQ(0U, value.presence & SecondaryBankPresenceFlagAnimation);
	EXPECT_TRUE(item.at_end());
	return value;
}

const StateAtom* weapon_atom(const StateImage& image, std::uint64_t entity)
{
	const auto type = static_cast<std::uint16_t>(RecordType::WeaponState);
	for (const auto& value : image.records()) {
		if (value.key.record_type != type) continue;
		PacketReader reader({value.value.data(), value.value.size()});
		std::uint64_t found = 0U;
		if (reader.read_u64(found) && found == entity) return &value;
	}
	return nullptr;
}

WeaponHeader read_header(PacketReader& reader)
{
	WeaponHeader header{};
	std::uint16_t reserved = 1U;
	EXPECT_TRUE(reader.read_u64(header.entity));
	EXPECT_TRUE(reader.read_u64(header.presence));
	EXPECT_TRUE(reader.read_u64(header.sample));
	EXPECT_TRUE(reader.read_u16(header.primary_count));
	EXPECT_TRUE(reader.read_u16(header.secondary_count));
	EXPECT_TRUE(reader.read_u16(header.tertiary_count));
	EXPECT_TRUE(reader.read_u16(reserved));
	EXPECT_EQ(0U, reserved);
	EXPECT_TRUE(reader.read_u32(header.current_primary));
	EXPECT_TRUE(reader.read_u32(header.current_secondary));
	EXPECT_TRUE(reader.read_u32(header.current_tertiary));
	EXPECT_TRUE(reader.read_u32(header.flags));
	return header;
}

ValidationError validate_atom(const StateAtom& atom)
{
	BusinessRecordMetadata metadata{};
	const RecordEnvelopeView envelope{
		atom.key.record_type, atom.record_version, RecordFlagNone,
		{atom.value.data(), atom.value.size()}};
	return validate_business_record(
		envelope, BusinessRecordContainer::FullSnapshot, VersionMinor, metadata);
}

struct Fixture {
	std::unique_ptr<Phase2ObservationDto> observation =
		std::make_unique<Phase2ObservationDto>();
	std::unique_ptr<Phase2ManifestCandidate> manifest =
		std::make_unique<Phase2ManifestCandidate>();
	std::array<Phase2Wp05SubjectBinding, MaximumPhase2ObservationShips> subjects{};
	Phase2Wp06WeaponProjectionInput input{};

	Fixture()
	{
		observation->capture = {Phase2CaptureStatus::Valid, Phase2CaptureReason::None};
		observation->producer_sample_time_us = 123'000U;
		observation->player_key.value = 11U;
		observation->ships.resize(1U);
		configure_ship(0U, 11U, 101U, 9001U);

		manifest->manifest_id = 7U;
		manifest->class_record_count = 1U;
		manifest->class_records[0].source_key = 101U;
		manifest->class_records[0].class_id = 501U;
		manifest->class_records[0].banks =
			{manifest->bank_records.data(), Phase2ManifestLimits::MaxBanksPerClass};

		input.observation = observation.get();
		input.installed_manifest = manifest.get();
		input.subjects = subjects.data();
		input.subject_count = 1U;
	}

	void configure_ship(std::size_t index,
		std::uint32_t capture_key,
		std::uint32_t class_key,
		std::uint64_t entity)
	{
		auto& ship = observation->ships[index];
		ship.capture_key.value = capture_key;
		ship.identity.class_source_key.value = class_key;
		ship.weapons.sample_time_us = observation->producer_sample_time_us;
		subjects[index] = {{capture_key}, entity};
	}

	void set_bank(std::size_t slot,
		WeaponFamily family,
		std::uint16_t index,
		std::uint32_t bank_id,
		std::uint32_t weapon_source,
		std::uint32_t weapon_id,
		std::uint64_t class_flags)
	{
		auto& bank = manifest->bank_records[slot];
		bank.family = family;
		bank.source_family = WeaponFamily::None;
		bank.canonical_index = index;
		bank.source_index = index;
		bank.bank_id = bank_id;
		bank.weapon_class_id = weapon_id;
		auto& weapon = manifest->weapon_records[slot];
		weapon.source_key = weapon_source;
		weapon.weapon_class_id = weapon_id;
		weapon.class_flags = class_flags;
		manifest->weapon_record_count =
			std::max(manifest->weapon_record_count, static_cast<std::uint32_t>(slot + 1U));
	}
};

TEST(Phase2WeaponState, P2TST037UnarmedK1AndK2StillEmitOneEmptyRecordPerPublicSubject)
{
	Fixture fixture;
	fixture.observation->ships.resize(2U);
	fixture.configure_ship(1U, 12U, 101U, 9002U);
	fixture.input.subject_count = 2U;
	StateImage image;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_wp06_weapon_projection(fixture.input, image));
	for (const auto entity : {9001U, 9002U}) {
		const auto* atom = weapon_atom(image, entity);
		ASSERT_NE(nullptr, atom);
		PacketReader reader({atom->value.data(), atom->value.size()});
		const auto header = read_header(reader);
		EXPECT_EQ(0U, header.presence);
		EXPECT_EQ(0U, header.primary_count);
		EXPECT_EQ(0U, header.secondary_count);
		EXPECT_EQ(0U, header.tertiary_count);
		EXPECT_EQ(0U, header.current_primary);
		EXPECT_EQ(0U, header.current_secondary);
		std::uint16_t list_count = 1U;
		ASSERT_TRUE(reader.read_u16(list_count));
		EXPECT_EQ(0U, list_count);
		ASSERT_TRUE(reader.read_u16(list_count));
		EXPECT_EQ(0U, list_count);
		EXPECT_TRUE(reader.at_end());
		EXPECT_EQ(ValidationError::None, validate_atom(*atom));
	}
}

TEST(Phase2WeaponState, P2TST036MapsEnergyBallisticAmmolessBurstSubstitutionAndFofExactly)
{
	Fixture fixture;
	auto& weapons = fixture.observation->ships[0].weapons;
	weapons.primary_bank_count = 2U;
	weapons.secondary_bank_count = 2U;
	fixture.manifest->class_records[0].bank_count = 4U;
	fixture.set_bank(0U, WeaponFamily::Primary, 0U, 1001U, 201U, 3001U, 0U);
	fixture.set_bank(1U, WeaponFamily::Primary, 1U, 1002U, 202U, 3002U,
		WeaponClassFlagBallistic);
	fixture.set_bank(2U, WeaponFamily::Secondary, 0U, 1003U, 203U, 3003U,
		WeaponClassFlagAmmoless);
	fixture.set_bank(3U, WeaponFamily::Secondary, 1U, 1004U, 204U, 3004U, 0U);
	fixture.manifest->bank_records[1].capacity = 12.5F;
	fixture.manifest->bank_records[3].capacity = 20.0F;
	fixture.manifest->bank_records[1].pattern_id = 4242U;
	fixture.manifest->auxiliary_records[0] =
		{AuxiliaryRegistry::Pattern, 3U, 4242U};
	fixture.manifest->auxiliary_record_count = 1U;
	for (std::size_t i = 0; i < 2U; ++i) {
		auto& bank = weapons.primary_banks[i];
		bank.source_bank_key = static_cast<std::uint32_t>(1001U + i);
		bank.weapon_class_source_key.value = static_cast<std::uint32_t>(201U + i);
		bank.primary_slot = static_cast<std::int32_t>(i);
		bank.primary_fire_point = static_cast<std::uint16_t>(i + 1U);
		bank.simultaneous_slots = 1U;
	}
	auto& ballistic = weapons.primary_banks[1];
	ballistic.ammunition_current = 7U;
	ballistic.ammunition_initial = 9U;
	ballistic.rearm_remaining_us = 10U;
	ballistic.burst_counter = 2;
	ballistic.burst_seed = 77U;
	ballistic.substitution_pattern_index = 3U;
	ballistic.fof_cooldown_remaining_us = 11U;
	ballistic.weapon_animation = 99;
	ballistic.cooldown_remaining_us = 9U;
	ballistic.primary_slot = 4;
	ballistic.primary_fire_point = 5U;
	ballistic.simultaneous_slots = 2U;
	ballistic.firing_pattern_source_code = 2U;
	auto& secondary = weapons.secondary_banks[0];
	secondary.source_bank_key = 1003U;
	secondary.weapon_class_source_key.value = 203U;
	secondary.secondary_slot = 2;
	secondary.ammunition_current = 55U; // poison: AMMOLESS must suppress it.
	auto& secondary_ammo = weapons.secondary_banks[1];
	secondary_ammo.weapon_class_source_key.value = 204U;
	secondary_ammo.secondary_slot = 3;
	secondary_ammo.cooldown_remaining_us = 12U;
	secondary_ammo.ammunition_current = 13U;
	secondary_ammo.ammunition_initial = 17U;
	secondary_ammo.rearm_remaining_us = 14U;
	secondary_ammo.burst_counter = 4;
	secondary_ammo.burst_seed = 88U;
	secondary_ammo.substitution_pattern_index = 6U;

	StateImage image;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_wp06_weapon_projection(fixture.input, image));
	const auto* atom = weapon_atom(image, 9001U);
	ASSERT_NE(nullptr, atom);
	EXPECT_EQ(ValidationError::None, validate_atom(*atom));
	PacketReader reader({atom->value.data(), atom->value.size()});
	const auto header = read_header(reader);
	EXPECT_EQ(2U, header.primary_count);
	EXPECT_EQ(2U, header.secondary_count);
	std::uint16_t count = 0U;
	ASSERT_TRUE(reader.read_u16(count));
	ASSERT_EQ(2U, count);
	const auto energy = read_primary(reader);
	const auto ammo = read_primary(reader);
	EXPECT_EQ(PrimaryBankPresenceFlagNone, energy.presence);
	EXPECT_EQ(1001U, energy.id);
	EXPECT_EQ(3001U, energy.weapon);
	EXPECT_EQ(PrimaryBankPresenceFlagBallisticAmmo |
			PrimaryBankPresenceFlagRearm |
			PrimaryBankPresenceFlagBurst |
			PrimaryBankPresenceFlagSubstitution |
			PrimaryBankPresenceFlagFofCooldown,
		ammo.presence);
	EXPECT_EQ(1002U, ammo.id);
	EXPECT_EQ(3002U, ammo.weapon);
	EXPECT_EQ(9U, ammo.cooldown);
	EXPECT_EQ(4U, ammo.slot);
	EXPECT_EQ(5U, ammo.fire_point);
	EXPECT_EQ(2U, ammo.simultaneous);
	EXPECT_EQ(4242U, ammo.pattern);
	EXPECT_EQ(7U, ammo.ammunition);
	EXPECT_EQ(9U, ammo.initial);
	EXPECT_FLOAT_EQ(12.5F, ammo.capacity);
	EXPECT_EQ(10U, ammo.rearm);
	EXPECT_EQ(2U, ammo.burst);
	EXPECT_EQ(77U, ammo.seed);
	EXPECT_EQ(3U, ammo.substitution);
	EXPECT_EQ(11U, ammo.fof);
	ASSERT_TRUE(reader.read_u16(count));
	ASSERT_EQ(2U, count);
	const auto ammoless = read_secondary(reader);
	EXPECT_EQ(SecondaryBankPresenceFlagNone, ammoless.presence);
	EXPECT_EQ(1003U, ammoless.id);
	EXPECT_EQ(3003U, ammoless.weapon);
	EXPECT_EQ(2U, ammoless.slot);
	const auto secondary_with_ammo = read_secondary(reader);
	EXPECT_EQ(SecondaryBankPresenceFlagAmmo |
			SecondaryBankPresenceFlagRearm |
			SecondaryBankPresenceFlagBurst |
			SecondaryBankPresenceFlagSubstitution,
		secondary_with_ammo.presence);
	EXPECT_EQ(1004U, secondary_with_ammo.id);
	EXPECT_EQ(3004U, secondary_with_ammo.weapon);
	EXPECT_EQ(12U, secondary_with_ammo.cooldown);
	EXPECT_EQ(3U, secondary_with_ammo.slot);
	EXPECT_EQ(13U, secondary_with_ammo.ammunition);
	EXPECT_EQ(17U, secondary_with_ammo.initial);
	EXPECT_FLOAT_EQ(20.0F, secondary_with_ammo.capacity);
	EXPECT_EQ(14U, secondary_with_ammo.rearm);
	EXPECT_EQ(4U, secondary_with_ammo.burst);
	EXPECT_EQ(88U, secondary_with_ammo.seed);
	EXPECT_EQ(6U, secondary_with_ammo.substitution);
	EXPECT_TRUE(reader.at_end());
}

TEST(Phase2WeaponState, P2TST037MapsSelectorsPreviousLaserSwarmRemoteAndPerBurstGroups)
{
	Fixture fixture;
	auto& weapons = fixture.observation->ships[0].weapons;
	weapons.primary_bank_count = 1U;
	weapons.secondary_bank_count = 1U;
	weapons.current_primary_bank = 0;
	weapons.current_secondary_bank = 0;
	weapons.previous_primary_bank = 0;
	weapons.previous_secondary_bank = 0;
	weapons.targeting_laser_bank = 0;
	weapons.targeting_laser_active = true;
	weapons.swarm_remaining = 4U;
	weapons.swarm_secondary_bank = 0;
	weapons.remote_detonaters_active = 2U;
	weapons.remote_detonation_remaining_us = 5'000U;
	weapons.per_burst_rotation_active = true;
	weapons.per_burst_rotation = 0.25F;
	weapons.raw_weapon_flags = KnownWeaponGlobalFlags;
	fixture.manifest->class_records[0].bank_count = 2U;
	fixture.set_bank(0U, WeaponFamily::Primary, 0U, 1101U, 211U, 3101U, 0U);
	fixture.set_bank(1U, WeaponFamily::Secondary, 0U, 1102U, 212U, 3102U, 0U);
	weapons.primary_banks[0].weapon_class_source_key.value = 211U;
	weapons.primary_banks[0].simultaneous_slots = 1U;
	weapons.secondary_banks[0].weapon_class_source_key.value = 212U;
	StateImage image;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_wp06_weapon_projection(fixture.input, image));
	const auto* atom = weapon_atom(image, 9001U);
	ASSERT_NE(nullptr, atom);
	PacketReader reader({atom->value.data(), atom->value.size()});
	const auto header = read_header(reader);
	EXPECT_EQ(KnownWeaponStatePresenceFlags & ~WeaponStatePresenceFlagCountermeasure &
			~WeaponStatePresenceFlagTertiary,
		header.presence);
	EXPECT_EQ(KnownWeaponGlobalFlags, header.flags);
	EXPECT_EQ(1101U, header.current_primary);
	EXPECT_EQ(1102U, header.current_secondary);
	std::uint32_t id = 0U;
	std::uint16_t swarm = 0U;
	std::uint64_t remote = 0U;
	float rotation = 0.0F;
	ASSERT_TRUE(reader.read_u32(id));
	EXPECT_EQ(1101U, id);
	ASSERT_TRUE(reader.read_u32(id));
	EXPECT_EQ(1102U, id);
	ASSERT_TRUE(reader.read_u32(id));
	EXPECT_EQ(1101U, id);
	ASSERT_TRUE(reader.read_u16(swarm));
	EXPECT_EQ(4U, swarm);
	ASSERT_TRUE(reader.read_u32(id));
	EXPECT_EQ(1102U, id);
	ASSERT_TRUE(reader.read_u64(remote));
	EXPECT_EQ(5'000U, remote);
	ASSERT_TRUE(reader.read_f32(rotation));
	EXPECT_FLOAT_EQ(0.25F, rotation);
	std::uint16_t count = 0U;
	ASSERT_TRUE(reader.read_u16(count));
	ASSERT_EQ(1U, count);
	EXPECT_EQ(1101U, read_primary(reader).id);
	ASSERT_TRUE(reader.read_u16(count));
	ASSERT_EQ(1U, count);
	EXPECT_EQ(1102U, read_secondary(reader).id);
	EXPECT_TRUE(reader.at_end());
	EXPECT_EQ(ValidationError::None, validate_atom(*atom));
}

TEST(Phase2WeaponState, P2TST037AcceptsZeroAndSixtyFourBanksAndRejectsSixtyFiveAtomically)
{
	Fixture fixture;
	auto& weapons = fixture.observation->ships[0].weapons;
	weapons.primary_bank_count = 64U;
	weapons.secondary_bank_count = 64U;
	fixture.manifest->class_records[0].bank_count = 128U;
	for (std::size_t i = 0; i < 64U; ++i) {
		fixture.set_bank(i, WeaponFamily::Primary, static_cast<std::uint16_t>(i),
			static_cast<std::uint32_t>(1000U + i), 200U, 300U, 0U);
		fixture.set_bank(64U + i, WeaponFamily::Secondary, static_cast<std::uint16_t>(i),
			static_cast<std::uint32_t>(2000U + i), 201U, 301U, 0U);
		weapons.primary_banks[i].weapon_class_source_key.value = 200U;
		weapons.primary_banks[i].simultaneous_slots = 1U;
		weapons.secondary_banks[i].weapon_class_source_key.value = 201U;
	}
	StateImage image;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_wp06_weapon_projection(fixture.input, image));
	const auto* atom = weapon_atom(image, 9001U);
	ASSERT_NE(nullptr, atom);
	PacketReader reader({atom->value.data(), atom->value.size()});
	const auto header = read_header(reader);
	EXPECT_EQ(64U, header.primary_count);
	EXPECT_EQ(64U, header.secondary_count);
	std::uint16_t count = 0U;
	ASSERT_TRUE(reader.read_u16(count));
	ASSERT_EQ(64U, count);
	for (std::uint32_t index = 0U; index < count; ++index) {
		const auto bank = read_primary(reader);
		EXPECT_EQ(index, bank.index);
		EXPECT_EQ(1000U + index, bank.id);
	}
	ASSERT_TRUE(reader.read_u16(count));
	ASSERT_EQ(64U, count);
	for (std::uint32_t index = 0U; index < count; ++index) {
		const auto bank = read_secondary(reader);
		EXPECT_EQ(index, bank.index);
		EXPECT_EQ(2000U + index, bank.id);
	}
	EXPECT_TRUE(reader.at_end());
	const auto sentinel = image;
	weapons.primary_bank_count = 65U;
	EXPECT_EQ(Phase2StateImageBuildStatus::InvalidInput,
		build_phase2_wp06_weapon_projection(fixture.input, image));
	EXPECT_EQ(sentinel, image);
}

TEST(Phase2WeaponState, P2TST038TertiaryZeroOneSixtyFourAndInvalidSelectorsAreClosed)
{
	for (const auto count : {0U, 1U, 64U}) {
		Fixture fixture;
		auto& weapons = fixture.observation->ships[0].weapons;
		weapons.tertiary_bank_count = static_cast<std::uint8_t>(count);
		weapons.current_tertiary_bank = count == 0U ? -1 : static_cast<std::int32_t>(count - 1U);
		weapons.tertiary_ammunition_current = 4;
		weapons.tertiary_ammunition_initial = 8;
		weapons.tertiary_ammunition_capacity = 12;
		weapons.tertiary_cooldown_remaining_us = 13U;
		weapons.tertiary_rearm_remaining_us = 14U;
		for (std::size_t i = 0; i < count; ++i) {
			auto& bank = fixture.manifest->bank_records[i];
			bank.family = WeaponFamily::Tertiary;
			bank.canonical_index = static_cast<std::uint16_t>(i);
			bank.bank_id = static_cast<std::uint32_t>(4000U + i);
		}
		fixture.manifest->class_records[0].bank_count = count;
		StateImage image;
		ASSERT_EQ(Phase2StateImageBuildStatus::Created,
			build_phase2_wp06_weapon_projection(fixture.input, image));
		const auto* atom = weapon_atom(image, 9001U);
		ASSERT_NE(nullptr, atom);
		EXPECT_EQ(ValidationError::None, validate_atom(*atom));
		PacketReader reader({atom->value.data(), atom->value.size()});
		const auto header = read_header(reader);
		EXPECT_EQ(count, header.tertiary_count);
		EXPECT_EQ(count == 0U ? 0U : 4000U + count - 1U,
			header.current_tertiary);
		std::uint16_t list_count = 1U;
		ASSERT_TRUE(reader.read_u16(list_count));
		EXPECT_EQ(0U, list_count);
		ASSERT_TRUE(reader.read_u16(list_count));
		EXPECT_EQ(0U, list_count);
		if (count != 0U) {
			std::uint32_t value = 0U;
			float capacity = 0.0F;
			std::uint64_t duration = 0U;
			ASSERT_TRUE(reader.read_u32(value));
			EXPECT_EQ(4000U + count - 1U, value);
			ASSERT_TRUE(reader.read_u32(value));
			EXPECT_EQ(4U, value);
			ASSERT_TRUE(reader.read_u32(value));
			EXPECT_EQ(8U, value);
			ASSERT_TRUE(reader.read_f32(capacity));
			EXPECT_FLOAT_EQ(12.0F, capacity);
			ASSERT_TRUE(reader.read_u64(duration));
			EXPECT_EQ(13U, duration);
			ASSERT_TRUE(reader.read_u64(duration));
			EXPECT_EQ(14U, duration);
		}
		EXPECT_TRUE(reader.at_end());
	}
	Fixture invalid;
	auto& invalid_weapons = invalid.observation->ships[0].weapons;
	invalid_weapons.tertiary_bank_count = 1U;
	invalid_weapons.current_tertiary_bank = -1;
	invalid.manifest->class_records[0].bank_count = 1U;
	invalid.manifest->bank_records[0].family = WeaponFamily::Tertiary;
	invalid.manifest->bank_records[0].bank_id = 4000U;
	StateImage image;
	const auto empty = image;
	EXPECT_NE(Phase2StateImageBuildStatus::Created,
		build_phase2_wp06_weapon_projection(invalid.input, image));
	EXPECT_EQ(empty, image);
	invalid_weapons.tertiary_bank_count = 65U;
	EXPECT_EQ(Phase2StateImageBuildStatus::InvalidInput,
		build_phase2_wp06_weapon_projection(invalid.input, image));
}

TEST(Phase2WeaponState, P2TST038CountermeasureCapacityLockAndCooldownAreExact)
{
	for (const bool capacity_mode : {false, true}) {
		Fixture fixture;
		auto& ship_class = fixture.manifest->class_records[0];
		ship_class.countermeasure_weapon_class_id = 7001U;
		ship_class.countermeasure_initial_count = capacity_mode ? 5U : 20U;
		auto& weapons = fixture.observation->ships[0].weapons;
		weapons.presence = WeaponStatePresenceFlagCountermeasure;
		weapons.countermeasure_count = 3U;
		weapons.countermeasure_maximum = capacity_mode ? 5U : 20U;
		weapons.countermeasure_class_source_key.value = 601U;
		weapons.countermeasures_enabled = !capacity_mode;
		weapons.countermeasure_cooldown_remaining_us = capacity_mode ? 9U : 0U;
		fixture.manifest->weapon_records[0].source_key = 601U;
		fixture.manifest->weapon_records[0].weapon_class_id = 7001U;
		fixture.manifest->weapon_records[0].class_flags = WeaponClassFlagCountermeasure;
		fixture.manifest->weapon_record_count = 1U;
		StateImage image;
		ASSERT_EQ(Phase2StateImageBuildStatus::Created,
			build_phase2_wp06_weapon_projection(fixture.input, image));
		const auto* atom = weapon_atom(image, 9001U);
		ASSERT_NE(nullptr, atom);
		EXPECT_EQ(ValidationError::None, validate_atom(*atom));
		PacketReader reader({atom->value.data(), atom->value.size()});
		const auto header = read_header(reader);
		EXPECT_EQ(WeaponStatePresenceFlagCountermeasure, header.presence);
		std::uint16_t list_count = 1U;
		ASSERT_TRUE(reader.read_u16(list_count));
		EXPECT_EQ(0U, list_count);
		ASSERT_TRUE(reader.read_u16(list_count));
		EXPECT_EQ(0U, list_count);
		std::uint16_t presence = 0U;
		std::uint16_t flags = 0U;
		std::uint32_t value = 0U;
		std::uint64_t cooldown = 0U;
		ASSERT_TRUE(reader.read_u16(presence));
		EXPECT_EQ(CountermeasureStatePresenceFlagClass, presence);
		ASSERT_TRUE(reader.read_u16(flags));
		EXPECT_EQ(capacity_mode ? CountermeasureStateFlagLocked :
			CountermeasureStateFlagAvailable, flags);
		ASSERT_TRUE(reader.read_u32(value));
		EXPECT_EQ(7001U, value);
		ASSERT_TRUE(reader.read_u32(value));
		EXPECT_EQ(3U, value);
		ASSERT_TRUE(reader.read_u32(value));
		EXPECT_EQ(capacity_mode ? 5U : 20U, value);
		ASSERT_TRUE(reader.read_u64(cooldown));
		EXPECT_EQ(capacity_mode ? 9U : 0U, cooldown);
		EXPECT_TRUE(reader.at_end());
	}
	Fixture negative;
	auto& ship_class = negative.manifest->class_records[0];
	ship_class.countermeasure_installed = true;
	ship_class.countermeasure_weapon_class_id = 7001U;
	ship_class.countermeasure_initial_count = 5U;
	auto& weapons = negative.observation->ships[0].weapons;
	weapons.presence = WeaponStatePresenceFlagCountermeasure;
	weapons.countermeasure_count = static_cast<std::uint16_t>(-1);
	weapons.countermeasure_maximum = 5U;
	weapons.countermeasure_class_source_key.value = 601U;
	negative.manifest->weapon_records[0].source_key = 601U;
	negative.manifest->weapon_records[0].weapon_class_id = 7001U;
	negative.manifest->weapon_records[0].class_flags =
		WeaponClassFlagCountermeasure;
	negative.manifest->weapon_record_count = 1U;
	StateImage image;
	const auto empty = image;
	EXPECT_NE(Phase2StateImageBuildStatus::Created,
		build_phase2_wp06_weapon_projection(negative.input, image));
	EXPECT_EQ(empty, image);
}

TEST(Phase2WeaponState, P2TST036To038RoundtripMissingMappingAndReadyPoolAreAtomicAndNoGrowth)
{
	Fixture fixture;
	StateImage image;
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_wp06_weapon_projection(fixture.input, image));
	const auto sentinel = image;
	fixture.observation->ships[0].identity.class_source_key.value = 999U;
	EXPECT_EQ(Phase2StateImageBuildStatus::SourceMappingMissing,
		build_phase2_wp06_weapon_projection(fixture.input, image));
	EXPECT_EQ(sentinel, image);

	fixture.observation->ships[0].identity.class_source_key.value = 101U;
	fixture.observation->ships[0].weapons.primary_bank_count = 1U;
	fixture.manifest->class_records[0].bank_count = 1U;
	fixture.set_bank(0U, WeaponFamily::Primary, 0U, 1001U, 201U, 3001U, 0U);
	fixture.manifest->bank_records[0].pattern_id = 4242U;
	fixture.observation->ships[0].weapons.primary_banks[0]
		.weapon_class_source_key.value = 201U;
	fixture.observation->ships[0].weapons.primary_banks[0]
		.simultaneous_slots = 1U;
	fixture.observation->ships[0].weapons.primary_banks[0]
		.firing_pattern_source_code = 2U;
	fixture.manifest->auxiliary_records[0] =
		{AuxiliaryRegistry::Pattern, 3U, 4242U};
	fixture.manifest->auxiliary_records[1] =
		{AuxiliaryRegistry::Pattern, 3U, 4243U};
	fixture.manifest->auxiliary_record_count = 2U;
	EXPECT_NE(Phase2StateImageBuildStatus::Created,
		build_phase2_wp06_weapon_projection(fixture.input, image));
	EXPECT_EQ(sentinel, image);
	fixture.manifest->auxiliary_record_count = 1U;
	Phase2Wp06WeaponProjectionPool pool;
	ASSERT_TRUE(pool.provision(MaximumPhase2ObservationShips,
		MaximumPhase2WeaponBanksPerFamily,
		MaximumPhase2WeaponBanksPerFamily));
	const auto bytes = pool.owned_backing_bytes();
	ASSERT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_wp06_weapon_projection_preallocated(fixture.input, pool, image));
	telemetry::test::phase2test::GlobalAllocationScope allocations;
	EXPECT_EQ(Phase2StateImageBuildStatus::Created,
		build_phase2_wp06_weapon_projection_preallocated(fixture.input, pool, image));
	EXPECT_EQ(0U, allocations.finish());
	EXPECT_EQ(bytes, pool.owned_backing_bytes());
	const auto* atom = weapon_atom(image, 9001U);
	ASSERT_NE(nullptr, atom);
	EXPECT_EQ(ValidationError::None, validate_atom(*atom));
	PacketReader reader({atom->value.data(), atom->value.size()});
	const auto header = read_header(reader);
	EXPECT_EQ(header.presence,
		header.presence & KnownWeaponStatePresenceFlags);
	std::uint16_t list_count = 0U;
	ASSERT_TRUE(reader.read_u16(list_count));
	ASSERT_EQ(1U, list_count);
	EXPECT_EQ(4242U, read_primary(reader).pattern);
	ASSERT_TRUE(reader.read_u16(list_count));
	EXPECT_EQ(0U, list_count);
	EXPECT_TRUE(reader.at_end());
	EXPECT_EQ(1U, image.records().size());
	EXPECT_EQ(static_cast<std::uint16_t>(RecordType::WeaponState),
		image.records()[0].key.record_type);
}

} // namespace
