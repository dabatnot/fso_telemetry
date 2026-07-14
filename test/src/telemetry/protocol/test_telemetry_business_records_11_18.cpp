#include "telemetry/protocol/telemetry_business_records.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <vector>

namespace {

using namespace telemetry::protocol;

void u8(std::vector<std::uint8_t>& bytes, std::uint8_t value)
{
	bytes.push_back(value);
}

void u16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
	bytes.push_back(static_cast<std::uint8_t>(value));
	bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void u32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
	for (unsigned int shift = 0; shift < 32; shift += 8) {
		bytes.push_back(static_cast<std::uint8_t>(value >> shift));
	}
}

void u64(std::vector<std::uint8_t>& bytes, std::uint64_t value)
{
	for (unsigned int shift = 0; shift < 64; shift += 8) {
		bytes.push_back(static_cast<std::uint8_t>(value >> shift));
	}
}

void f32(std::vector<std::uint8_t>& bytes, float value)
{
	std::uint32_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	u32(bytes, bits);
}

void string(std::vector<std::uint8_t>& bytes, std::string_view value)
{
	u16(bytes, static_cast<std::uint16_t>(value.size()));
	bytes.insert(bytes.end(), value.begin(), value.end());
}

void vec3(std::vector<std::uint8_t>& bytes, float x, float y, float z)
{
	f32(bytes, x);
	f32(bytes, y);
	f32(bytes, z);
}

void vitem(std::vector<std::uint8_t>& bytes,
	const std::vector<std::uint8_t>& item,
	std::uint8_t version = 1,
	std::size_t declared_extra = 0)
{
	u8(bytes, version);
	u16(bytes, static_cast<std::uint16_t>(item.size() + declared_extra));
	bytes.insert(bytes.end(), item.begin(), item.end());
	bytes.insert(bytes.end(), declared_extra, 0);
}

ByteView view(const std::vector<std::uint8_t>& bytes)
{
	return ByteView{bytes.empty() ? nullptr : bytes.data(), bytes.size()};
}

RecordEnvelopeView record(RecordType type, const std::vector<std::uint8_t>& payload)
{
	return RecordEnvelopeView{static_cast<std::uint16_t>(type), 1, RecordFlagNone, view(payload)};
}

ValidationError validate(RecordType type, const std::vector<std::uint8_t>& payload)
{
	BusinessRecordMetadata metadata;
	return validate_business_record(record(type, payload), BusinessRecordContainer::FullSnapshot, metadata);
}

std::vector<std::uint8_t> subsystem(std::uint64_t presence = 0,
	std::uint8_t type = static_cast<std::uint8_t>(SubsystemType::Engine),
	std::uint32_t flags = 0,
	float current_hits = 0.0F,
	float max_hits = 0.0F)
{
	std::vector<std::uint8_t> bytes;
	u64(bytes, 1);
	u32(bytes, 2);
	u64(bytes, presence);
	u64(bytes, 3);
	u16(bytes, 0);
	u8(bytes, type);
	f32(bytes, current_hits);
	f32(bytes, max_hits);
	u32(bytes, flags);
	return bytes;
}

std::vector<std::uint8_t> energy(std::uint64_t presence = 0,
	std::uint8_t mode = static_cast<std::uint8_t>(EtsMode::Absent),
	std::uint8_t shields = 0,
	std::uint8_t weapons = 0,
	std::uint8_t engines = 0)
{
	std::vector<std::uint8_t> bytes;
	u64(bytes, 1);
	u64(bytes, presence);
	u64(bytes, 3);
	u8(bytes, mode);
	u8(bytes, shields);
	u8(bytes, weapons);
	u8(bytes, engines);
	u8(bytes, 0);
	return bytes;
}

std::vector<std::uint8_t> propulsion(std::uint64_t presence = 0, std::uint16_t flags = 0)
{
	std::vector<std::uint8_t> bytes;
	u64(bytes, 1);
	u64(bytes, presence);
	u64(bytes, 3);
	u16(bytes, flags);
	u16(bytes, 0);
	return bytes;
}

std::vector<std::uint8_t> weapon_header(std::uint64_t presence,
	std::uint16_t primary_count,
	std::uint16_t secondary_count,
	std::uint16_t tertiary_count,
	std::uint32_t current_primary,
	std::uint32_t current_secondary,
	std::uint32_t current_tertiary,
	std::uint32_t flags = 0)
{
	std::vector<std::uint8_t> bytes;
	u64(bytes, 1);
	u64(bytes, presence);
	u64(bytes, 3);
	u16(bytes, primary_count);
	u16(bytes, secondary_count);
	u16(bytes, tertiary_count);
	u16(bytes, 0);
	u32(bytes, current_primary);
	u32(bytes, current_secondary);
	u32(bytes, current_tertiary);
	u32(bytes, flags);
	return bytes;
}

std::vector<std::uint8_t> primary_bank(std::uint32_t bank_id, std::uint16_t presence = 0)
{
	std::vector<std::uint8_t> item;
	u16(item, presence);
	u16(item, 0);
	u32(item, bank_id);
	u32(item, 10);
	u64(item, 0);
	u16(item, 0);
	u16(item, 0);
	u16(item, 1);
	u16(item, 0);
	return item;
}

std::vector<std::uint8_t> secondary_bank(std::uint32_t bank_id, std::uint16_t presence = 0)
{
	std::vector<std::uint8_t> item;
	u16(item, presence);
	u16(item, 0);
	u32(item, bank_id);
	u32(item, 11);
	u64(item, 0);
	u16(item, 0);
	u16(item, 0);
	return item;
}

std::vector<std::uint8_t> minimal_weapon()
{
	auto bytes = weapon_header(0, 0, 0, 0, 0, 0, 0);
	u16(bytes, 0);
	u16(bytes, 0);
	return bytes;
}

std::vector<std::uint8_t> lock_state(std::uint16_t count = 0)
{
	std::vector<std::uint8_t> bytes;
	u64(bytes, 1);
	u64(bytes, 0);
	u64(bytes, 3);
	u16(bytes, count);
	return bytes;
}

std::vector<std::uint8_t> lock_item(std::uint64_t target = 2, std::uint16_t presence = 0)
{
	std::vector<std::uint8_t> item;
	u16(item, presence);
	u8(item, 0);
	u8(item, 1);
	u64(item, target);
	if ((presence & LockItemPresenceFlagSubsystem) != 0) {
		u32(item, 7);
	}
	vec3(item, 1.0F, 2.0F, 3.0F);
	if ((presence & LockItemPresenceFlagLockAttempt) != 0) {
		u64(item, 0);
	}
	return item;
}

std::vector<std::uint8_t> target(std::uint64_t presence = 0, std::uint64_t current = 0)
{
	std::vector<std::uint8_t> bytes;
	u64(bytes, 1);
	u64(bytes, presence);
	u64(bytes, 3);
	u64(bytes, current);
	return bytes;
}

std::vector<std::uint8_t> radar(std::uint64_t presence = 0,
	std::uint64_t sample = 3,
	std::uint8_t mode = static_cast<std::uint8_t>(RadarMode::Short),
	float selected_range = 0.0F,
	float sensor_current = 0.0F,
	float sensor_maximum = 1.0F)
{
	std::vector<std::uint8_t> bytes;
	u64(bytes, 1);
	u64(bytes, presence);
	u64(bytes, sample);
	u8(bytes, mode);
	f32(bytes, selected_range);
	u8(bytes, static_cast<std::uint8_t>(SensorState::Offline));
	f32(bytes, sensor_current);
	f32(bytes, sensor_maximum);
	return bytes;
}

std::vector<std::uint8_t> radar_contact(std::uint64_t presence = 0,
	std::uint64_t sample = 3,
	std::uint8_t object_type = static_cast<std::uint8_t>(ObjectType::Unknown),
	std::uint32_t flags = 0)
{
	std::vector<std::uint8_t> bytes;
	u64(bytes, 1);
	u64(bytes, 2);
	u64(bytes, presence);
	u64(bytes, sample);
	u8(bytes, object_type);
	u8(bytes, static_cast<std::uint8_t>(RadarCategory::Unknown));
	u8(bytes, static_cast<std::uint8_t>(RadarVisibility::NotVisible));
	vec3(bytes, 0.0F, 0.0F, 0.0F);
	vec3(bytes, 0.0F, 0.0F, 0.0F);
	f32(bytes, 0.0F);
	u32(bytes, flags);
	return bytes;
}

std::vector<std::uint8_t> minimal(RecordType type)
{
	switch (type) {
	case RecordType::SubsystemState:
		return subsystem();
	case RecordType::EnergyState:
		return energy();
	case RecordType::PropulsionState:
		return propulsion();
	case RecordType::WeaponState:
		return minimal_weapon();
	case RecordType::LockState:
		return lock_state();
	case RecordType::TargetState:
		return target();
	case RecordType::RadarState:
		return radar();
	case RecordType::RadarContacts:
		return radar_contact();
	default:
		return {};
	}
}

TEST(TelemetryProtocolBusinessRecords11To18, MinimalCanonicalPayloadForEveryTypeIsAcceptedExactly)
{
	for (std::uint16_t raw = static_cast<std::uint16_t>(RecordType::SubsystemState);
		raw <= static_cast<std::uint16_t>(RecordType::RadarContacts);
		++raw) {
		const auto type = static_cast<RecordType>(raw);
		auto payload = minimal(type);
		SCOPED_TRACE(raw);
		ASSERT_FALSE(payload.empty());
		EXPECT_EQ(ValidationError::None, validate(type, payload));

		payload.push_back(0);
		EXPECT_EQ(ValidationError::TrailingBytes, validate(type, payload));
		payload.pop_back();
		payload.pop_back();
		EXPECT_EQ(ValidationError::TruncatedPayload, validate(type, payload));
	}
}

TEST(TelemetryProtocolBusinessRecords11To18, ReservedPresenceBitsAreRejectedForEveryType)
{
	for (std::uint16_t raw = static_cast<std::uint16_t>(RecordType::SubsystemState);
		raw <= static_cast<std::uint16_t>(RecordType::RadarContacts);
		++raw) {
		const auto type = static_cast<RecordType>(raw);
		auto payload = minimal(type);
		const std::size_t presence_offset = type == RecordType::SubsystemState ? 12U :
			type == RecordType::RadarContacts ? 16U : 8U;
		payload[presence_offset + 7U] |= 0x80U;
		SCOPED_TRACE(raw);
		EXPECT_EQ(ValidationError::ReservedFlag, validate(type, payload));
	}
}

TEST(TelemetryProtocolBusinessRecords11To18, StateAtomKeysAndCascadeOwnersUseCanonicalEntityPrefixes)
{
	for (std::uint16_t raw = static_cast<std::uint16_t>(RecordType::SubsystemState);
		raw <= static_cast<std::uint16_t>(RecordType::RadarContacts);
		++raw) {
		const auto type = static_cast<RecordType>(raw);
		StateAtom atom;
		ASSERT_EQ(ValidationError::None,
			decode_business_state_atom(record(type, minimal(type)), BusinessRecordContainer::FullSnapshot, atom));
		const auto expected_key_size = type == RecordType::SubsystemState ? 12U :
			type == RecordType::RadarContacts ? 16U : 8U;
		EXPECT_EQ(expected_key_size, atom.key.identity.size());
		ASSERT_TRUE(atom.has_cascade_owner);
		EXPECT_EQ(8U, atom.cascade_owner.identity.size());
		EXPECT_TRUE(std::equal(atom.key.identity.begin(), atom.key.identity.begin() + 8,
			atom.cascade_owner.identity.begin()));
	}
}

TEST(TelemetryProtocolBusinessRecords11To18, SubsystemEnforcesDamagePerturbationCargoAndTurretRelations)
{
	EXPECT_EQ(ValidationError::OutOfRange,
		validate(RecordType::SubsystemState, subsystem(0, static_cast<std::uint8_t>(SubsystemType::Engine), 0, 2, 1)));

	auto missing_perturbation = subsystem(0,
		static_cast<std::uint8_t>(SubsystemType::Engine),
		SubsystemFlagPerturbed);
	EXPECT_EQ(ValidationError::InvalidAbsence, validate(RecordType::SubsystemState, missing_perturbation));

	auto hidden_cargo = subsystem(SubsystemStatePresenceFlagLocalCargo);
	u8(hidden_cargo, static_cast<std::uint8_t>(DisclosureState::Hidden));
	string(hidden_cargo, "secret");
	EXPECT_EQ(ValidationError::VisibilityViolation, validate(RecordType::SubsystemState, hidden_cargo));

	auto non_turret = subsystem(SubsystemStatePresenceFlagTurret);
	EXPECT_EQ(ValidationError::InvalidAbsence, validate(RecordType::SubsystemState, non_turret));

	auto turret = subsystem(SubsystemStatePresenceFlagTurret, static_cast<std::uint8_t>(SubsystemType::Turret));
	u32(turret, 0);
	u64(turret, 0);
	vec3(turret, 0.0F, 0.0F, 1.0F);
	EXPECT_EQ(ValidationError::None, validate(RecordType::SubsystemState, turret));
}

TEST(TelemetryProtocolBusinessRecords11To18, SubsystemAnimationVlistIsExactAndKeysAreUnique)
{
	std::vector<std::uint8_t> animation;
	u16(animation, SubsystemAnimationPresenceFlagAngle);
	u16(animation, 9);
	u8(animation, static_cast<std::uint8_t>(AnimationState::Moving));
	u8(animation, 0);
	f32(animation, 0.5F);
	f32(animation, 0.25F);
	u64(animation, 0);

	auto payload = subsystem(SubsystemStatePresenceFlagAnimations);
	u16(payload, 2);
	vitem(payload, animation);
	vitem(payload, animation);
	EXPECT_EQ(ValidationError::DuplicateItemKey, validate(RecordType::SubsystemState, payload));

	payload = subsystem(SubsystemStatePresenceFlagAnimations);
	u16(payload, 1);
	vitem(payload, animation, 1, 1);
	EXPECT_EQ(ValidationError::TrailingBytes, validate(RecordType::SubsystemState, payload));

	animation[0] = 0;
	animation[1] = 0;
	payload = subsystem(SubsystemStatePresenceFlagAnimations);
	u16(payload, 1);
	vitem(payload, animation);
	EXPECT_EQ(ValidationError::InvalidAbsence, validate(RecordType::SubsystemState, payload));
}

TEST(TelemetryProtocolBusinessRecords11To18, EnergyEnforcesEtsAndCurrentMaximumRelations)
{
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		validate(RecordType::EnergyState, energy(0, static_cast<std::uint8_t>(EtsMode::Absent), 1)));

	auto payload = energy(EnergyStatePresenceFlagWeaponEnergy);
	f32(payload, 2.0F);
	f32(payload, 1.0F);
	EXPECT_EQ(ValidationError::OutOfRange, validate(RecordType::EnergyState, payload));

	payload = energy(EnergyStatePresenceFlagEngineIntegrity);
	f32(payload, 0.0F);
	f32(payload, 0.0F);
	EXPECT_EQ(ValidationError::OutOfRange, validate(RecordType::EnergyState, payload));
}

TEST(TelemetryProtocolBusinessRecords11To18, PropulsionEnforcesPresenceAndAfterburnerCoherence)
{
	EXPECT_EQ(ValidationError::InvalidAbsence,
		validate(RecordType::PropulsionState, propulsion(PropulsionStatePresenceFlagConsumption)));

	auto payload = propulsion(PropulsionStatePresenceFlagFuel,
		PropulsionFlagAfterburnerAvailable | PropulsionFlagAfterburnerLocked | PropulsionFlagAfterburnerActive);
	f32(payload, 1.0F);
	f32(payload, 2.0F);
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate(RecordType::PropulsionState, payload));

	payload = propulsion(PropulsionStatePresenceFlagFuel | PropulsionStatePresenceFlagEngagement,
		PropulsionFlagAfterburnerAvailable);
	f32(payload, 1.0F);
	f32(payload, 2.0F);
	f32(payload, 1.0F);
	u64(payload, 3'600'000'001ULL);
	u64(payload, 0);
	f32(payload, 1.0F);
	EXPECT_EQ(ValidationError::OutOfRange, validate(RecordType::PropulsionState, payload));
}

TEST(TelemetryProtocolBusinessRecords11To18, WeaponKeepsBankFamiliesDistinctAndCountsExact)
{
	auto payload = weapon_header(0, 1, 1, 0, 1, 2, 0);
	u16(payload, 1);
	vitem(payload, primary_bank(1));
	u16(payload, 1);
	vitem(payload, secondary_bank(2));
	EXPECT_EQ(ValidationError::None, validate(RecordType::WeaponState, payload));

	auto count_mismatch = weapon_header(0, 2, 1, 0, 1, 2, 0);
	u16(count_mismatch, 1);
	vitem(count_mismatch, primary_bank(1));
	u16(count_mismatch, 1);
	vitem(count_mismatch, secondary_bank(2));
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate(RecordType::WeaponState, count_mismatch));

	auto duplicate = weapon_header(0, 1, 1, 0, 1, 1, 0);
	u16(duplicate, 1);
	vitem(duplicate, primary_bank(1));
	u16(duplicate, 1);
	vitem(duplicate, secondary_bank(1));
	EXPECT_EQ(ValidationError::DuplicateItemKey, validate(RecordType::WeaponState, duplicate));
}

TEST(TelemetryProtocolBusinessRecords11To18, WeaponTertiaryLaserAndCountermeasureConditionsAreEnforced)
{
	auto missing_tertiary = weapon_header(0, 0, 0, 1, 0, 0, 3);
	u16(missing_tertiary, 0);
	u16(missing_tertiary, 0);
	EXPECT_EQ(ValidationError::InvalidAbsence, validate(RecordType::WeaponState, missing_tertiary));

	auto tertiary = weapon_header(WeaponStatePresenceFlagTertiary, 0, 0, 1, 0, 0, 3);
	u16(tertiary, 0);
	u16(tertiary, 0);
	u32(tertiary, 3);
	u32(tertiary, 0);
	u32(tertiary, 0);
	f32(tertiary, 0.0F);
	u64(tertiary, 0);
	u64(tertiary, 0);
	EXPECT_EQ(ValidationError::None, validate(RecordType::WeaponState, tertiary));

	auto laser = weapon_header(0, 0, 0, 0, 0, 0, 0, WeaponGlobalFlagTargetingLaser);
	u16(laser, 0);
	u16(laser, 0);
	EXPECT_EQ(ValidationError::InvalidAbsence, validate(RecordType::WeaponState, laser));

	auto countermeasure = weapon_header(WeaponStatePresenceFlagCountermeasure, 0, 0, 0, 0, 0, 0);
	u16(countermeasure, 0);
	u16(countermeasure, 0);
	u16(countermeasure, 0);
	u16(countermeasure, CountermeasureStateFlagAvailable | CountermeasureStateFlagLocked);
	u32(countermeasure, 0);
	u32(countermeasure, 0);
	u64(countermeasure, 0);
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate(RecordType::WeaponState, countermeasure));
}

TEST(TelemetryProtocolBusinessRecords11To18, LockSupportsNoAttemptAndRejectsDuplicatePointsAndInvalidBools)
{
	auto payload = lock_state(1);
	vitem(payload, lock_item());
	EXPECT_EQ(ValidationError::None, validate(RecordType::LockState, payload));

	payload = lock_state(2);
	vitem(payload, lock_item());
	vitem(payload, lock_item());
	EXPECT_EQ(ValidationError::DuplicateItemKey, validate(RecordType::LockState, payload));

	auto invalid_item = lock_item();
	invalid_item[2] = 2;
	payload = lock_state(1);
	vitem(payload, invalid_item);
	EXPECT_EQ(ValidationError::OutOfRange, validate(RecordType::LockState, payload));
}

TEST(TelemetryProtocolBusinessRecords11To18, TargetPresenceIsBoundToCurrentTargetAndValidUtf8)
{
	auto no_target = target(TargetStatePresenceFlagExactHudDistance, 0);
	f32(no_target, 0.0F);
	EXPECT_EQ(ValidationError::InvalidAbsence, validate(RecordType::TargetState, no_target));

	auto invalid_utf8 = target(TargetStatePresenceFlagRevealedIdentity, 2);
	u8(invalid_utf8, static_cast<std::uint8_t>(ObjectType::Ship));
	u16(invalid_utf8, 1);
	u8(invalid_utf8, 0xff);
	u32(invalid_utf8, 0);
	u32(invalid_utf8, 0);
	u32(invalid_utf8, 0);
	EXPECT_EQ(ValidationError::InvalidUtf8, validate(RecordType::TargetState, invalid_utf8));

	auto unknown_trend = target(TargetStatePresenceFlagDistanceTrend, 2);
	u8(unknown_trend, 4);
	EXPECT_EQ(ValidationError::UnknownEnum, validate(RecordType::TargetState, unknown_trend));
}

TEST(TelemetryProtocolBusinessRecords11To18, RadarRejectsNonFiniteRangesAndInvalidVisibilityIntervals)
{
	auto infinite = radar(0, 3, static_cast<std::uint8_t>(RadarMode::Infinite), 1.0e12F);
	EXPECT_EQ(ValidationError::None, validate(RecordType::RadarState, infinite));
	EXPECT_EQ(ValidationError::OutOfRange,
		validate(RecordType::RadarState, radar(0, 3, static_cast<std::uint8_t>(RadarMode::Infinite), 100.0F)));
	EXPECT_EQ(ValidationError::NonFiniteFloat,
		validate(RecordType::RadarState,
			radar(0, 3, static_cast<std::uint8_t>(RadarMode::Short), 0.0F,
				std::numeric_limits<float>::infinity(), 1.0F)));

	auto times = radar(RadarStatePresenceFlagVisibilityTimes, 10);
	u64(times, 8);
	u64(times, 11);
	EXPECT_EQ(ValidationError::OutOfRange, validate(RecordType::RadarState, times));
}

TEST(TelemetryProtocolBusinessRecords11To18, RadarContactsValidateBombTypeAndDetectionOrdering)
{
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		validate(RecordType::RadarContacts,
			radar_contact(0, 3, static_cast<std::uint8_t>(ObjectType::Ship), ContactFlagBomb)));

	auto times = radar_contact(RadarContactsPresenceFlagDetectionTimes, 10);
	u64(times, 9);
	u64(times, 8);
	EXPECT_EQ(ValidationError::OutOfRange, validate(RecordType::RadarContacts, times));

	auto valid_bomb = radar_contact(0, 3, static_cast<std::uint8_t>(ObjectType::Weapon), ContactFlagBomb);
	EXPECT_EQ(ValidationError::None, validate(RecordType::RadarContacts, valid_bomb));
}

} // namespace
