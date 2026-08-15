#include "telemetry/protocol/telemetry_business_records.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace {

using namespace telemetry::protocol;
using Bytes = std::vector<std::uint8_t>;

void u8(Bytes& bytes, std::uint8_t value)
{
	bytes.push_back(value);
}

void u16(Bytes& bytes, std::uint16_t value)
{
	bytes.push_back(static_cast<std::uint8_t>(value));
	bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void u32(Bytes& bytes, std::uint32_t value)
{
	for (unsigned int shift = 0; shift < 32U; shift += 8U) {
		bytes.push_back(static_cast<std::uint8_t>(value >> shift));
	}
}

void u64(Bytes& bytes, std::uint64_t value)
{
	for (unsigned int shift = 0; shift < 64U; shift += 8U) {
		bytes.push_back(static_cast<std::uint8_t>(value >> shift));
	}
}

void f32(Bytes& bytes, float value)
{
	std::uint32_t bits = 0;
	static_assert(sizeof(bits) == sizeof(value), "FSTL requires binary32 floats");
	std::memcpy(&bits, &value, sizeof(bits));
	u32(bytes, bits);
}

void utf8(Bytes& bytes, const char* value)
{
	const auto length = std::strlen(value);
	ASSERT_LE(length, std::numeric_limits<std::uint16_t>::max());
	u16(bytes, static_cast<std::uint16_t>(length));
	bytes.insert(bytes.end(), value, value + length);
}

void overwrite_u16(Bytes& bytes, std::size_t offset, std::uint16_t value)
{
	ASSERT_LE(offset + 2U, bytes.size());
	bytes[offset] = static_cast<std::uint8_t>(value);
	bytes[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
}

void overwrite_u32(Bytes& bytes, std::size_t offset, std::uint32_t value)
{
	ASSERT_LE(offset + 4U, bytes.size());
	for (unsigned int shift = 0; shift < 32U; shift += 8U) {
		bytes[offset + shift / 8U] = static_cast<std::uint8_t>(value >> shift);
	}
}

void overwrite_u64(Bytes& bytes, std::size_t offset, std::uint64_t value)
{
	ASSERT_LE(offset + 8U, bytes.size());
	for (unsigned int shift = 0; shift < 64U; shift += 8U) {
		bytes[offset + shift / 8U] = static_cast<std::uint8_t>(value >> shift);
	}
}

void overwrite_f32(Bytes& bytes, std::size_t offset, float value)
{
	std::uint32_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	overwrite_u32(bytes, offset, bits);
}

ByteView view(const Bytes& bytes)
{
	return ByteView{bytes.empty() ? nullptr : bytes.data(), bytes.size()};
}

RecordEnvelopeView record(RecordType type, const Bytes& payload)
{
	return RecordEnvelopeView{static_cast<std::uint16_t>(type), 1U, RecordFlagNone, view(payload)};
}

ValidationError validate(RecordType type, const Bytes& payload)
{
	BusinessRecordMetadata metadata;
	const auto container = type == RecordType::ClassManifest || type == RecordType::WeaponManifest
							 ? BusinessRecordContainer::Manifest
							 : BusinessRecordContainer::FullSnapshot;
	return validate_business_record(record(type, payload), container, metadata);
}

Bytes session_state(std::uint64_t presence = 0U,
	std::uint8_t authority = static_cast<std::uint8_t>(AuthorityMode::Solo),
	std::uint8_t visibility = static_cast<std::uint8_t>(VisibilityMode::Cockpit),
	std::uint64_t capabilities = 0U,
	std::uint64_t state_coverage = StateDomainCoverageBitCoreShip,
	std::uint64_t derived_coverage = 0U,
	std::uint64_t exact_coverage = 0U)
{
	Bytes bytes;
	u64(bytes, presence);
	u64(bytes, 1U);
	u64(bytes, 1234U);
	u8(bytes, authority);
	u8(bytes, visibility);
	u8(bytes, static_cast<std::uint8_t>(SessionPhase::Live));
	u8(bytes, 0U);
	u32(bytes, 1U);
	u64(bytes, capabilities);
	u64(bytes, state_coverage);
	u64(bytes, derived_coverage);
	u64(bytes, exact_coverage);
	if ((presence & SessionStatePresenceFlagObservedPlayer) != 0U) {
		u64(bytes, 7U);
	}
	return bytes;
}

Bytes mission_state(std::uint64_t presence = 0U, std::uint8_t phase = static_cast<std::uint8_t>(MissionPhase::None))
{
	Bytes bytes;
	u64(bytes, presence);
	u32(bytes, 1U);
	u8(bytes, phase);
	u8(bytes, 0U);
	u16(bytes, 0U);
	f32(bytes, 1.0F);
	u64(bytes, 1234U);
	if ((presence & MissionStatePresenceFlagMissionName) != 0U) {
		utf8(bytes, "mission");
	}
	return bytes;
}

Bytes class_manifest(std::uint64_t presence = 0U, const Bytes* banks = nullptr, const Bytes* subsystems = nullptr)
{
	Bytes bytes;
	u32(bytes, 1U);
	u32(bytes, 1U);
	u64(bytes, presence);
	utf8(bytes, "ship");
	u32(bytes, 0U);
	u32(bytes, 0U);
	f32(bytes, 1.0F);
	for (unsigned int index = 0; index < 3U; ++index) {
		f32(bytes, 0.0F);
	}
	if ((presence & ClassManifestPresenceFlagInertia) != 0U) {
		for (unsigned int index = 0; index < 9U; ++index) {
			f32(bytes, 0.0F);
		}
	}
	if ((presence & ClassManifestPresenceFlagDamping) != 0U) {
		for (unsigned int index = 0; index < 5U; ++index) {
			f32(bytes, 0.0F);
		}
	}
	if ((presence & ClassManifestPresenceFlagMotion) != 0U) {
		for (unsigned int index = 0; index < 19U; ++index) {
			f32(bytes, 0.0F);
		}
	}
	if ((presence & ClassManifestPresenceFlagHull) != 0U) {
		f32(bytes, 0.0F);
	}
	if ((presence & ClassManifestPresenceFlagShield) != 0U) {
		f32(bytes, 0.0F);
	}
	if ((presence & ClassManifestPresenceFlagEnergy) != 0U) {
		for (unsigned int index = 0; index < 5U; ++index) {
			f32(bytes, 0.0F);
		}
	}
	if ((presence & ClassManifestPresenceFlagAfterburner) != 0U) {
		for (unsigned int index = 0; index < 4U; ++index) {
			f32(bytes, 0.0F);
		}
		u64(bytes, 0U);
	}
	if ((presence & ClassManifestPresenceFlagCountermeasure) != 0U) {
		u32(bytes, 0U);
		u32(bytes, 0U);
		u64(bytes, 0U);
	}
	if ((presence & ClassManifestPresenceFlagBanks) != 0U) {
		if (banks != nullptr) {
			bytes.insert(bytes.end(), banks->begin(), banks->end());
		} else {
			u16(bytes, 0U);
		}
	}
	if ((presence & ClassManifestPresenceFlagSubsystems) != 0U) {
		if (subsystems != nullptr) {
			bytes.insert(bytes.end(), subsystems->begin(), subsystems->end());
		} else {
			u16(bytes, 0U);
		}
	}
	if ((presence & ClassManifestPresenceFlagScan) != 0U) {
		u64(bytes, 0U);
		f32(bytes, 0.0F);
		f32(bytes, 0.0F);
	}
	if ((presence & ClassManifestPresenceFlagGlide) != 0U) {
		f32(bytes, 0.0F);
	}
	if ((presence & ClassManifestPresenceFlagAutoaim) != 0U) {
		f32(bytes, 0.0F);
	}
	if ((presence & ClassManifestPresenceFlagRadarIcon) != 0U) {
		u32(bytes, 0U);
	}
	return bytes;
}

Bytes class_bank_item(std::uint32_t bank_id,
	std::uint8_t family = static_cast<std::uint8_t>(WeaponFamily::Primary),
	std::uint16_t presence = ClassBankPresenceFlagWeaponClass)
{
	Bytes item;
	u16(item, presence);
	u8(item, family);
	u8(item, 0U);
	u16(item, 0U);
	u32(item, bank_id);
	if ((presence & ClassBankPresenceFlagWeaponClass) != 0U) {
		u32(item, 10U);
	}
	if ((presence & ClassBankPresenceFlagCapacity) != 0U) {
		f32(item, 20.0F);
	}
	u16(item, 0U);
	u8(item, 1U);
	u16(item, 12U);
	return item;
}

Bytes class_subsystem_item(std::uint32_t subsystem_id)
{
	Bytes item;
	u16(item, 0U);
	u32(item, subsystem_id);
	u16(item, 0U);
	u8(item, static_cast<std::uint8_t>(SubsystemType::Unknown));
	u8(item, 0U);
	utf8(item, "engine");
	f32(item, 0.0F);
	f32(item, 0.0F);
	f32(item, 0.0F);
	f32(item, 0.0F);
	f32(item, 0.0F);
	u32(item, 0U);
	return item;
}

void vlist_item(Bytes& list, const Bytes& item, std::uint8_t version = 1U, std::uint16_t extra_size = 0U)
{
	u8(list, version);
	u16(list, static_cast<std::uint16_t>(item.size() + extra_size));
	list.insert(list.end(), item.begin(), item.end());
	for (std::uint16_t index = 0; index < extra_size; ++index) {
		u8(list, 0U);
	}
}

Bytes weapon_manifest(std::uint64_t presence = 0U, std::uint64_t flags = 0U)
{
	Bytes bytes;
	u32(bytes, 1U);
	u32(bytes, 1U);
	u64(bytes, presence);
	utf8(bytes, "weapon");
	if ((presence & WeaponManifestPresenceFlagTitle) != 0U) {
		utf8(bytes, "title");
	}
	u8(bytes, static_cast<std::uint8_t>(WeaponSubtype::Unknown));
	u64(bytes, flags);
	f32(bytes, 0.0F);
	if ((presence & WeaponManifestPresenceFlagAcceleration) != 0U) {
		u64(bytes, 0U);
	}
	f32(bytes, 0.0F);
	f32(bytes, 0.0F);
	u64(bytes, 0U);
	if ((presence & WeaponManifestPresenceFlagRanges) != 0U) {
		f32(bytes, 1.0F);
		f32(bytes, 2.0F);
		f32(bytes, 3.0F);
	}
	if ((presence & WeaponManifestPresenceFlagFire) != 0U) {
		u64(bytes, 0U);
		f32(bytes, 0.0F);
	}
	if ((presence & WeaponManifestPresenceFlagDamage) != 0U) {
		f32(bytes, 0.0F);
		u32(bytes, 0U);
		u32(bytes, 0U);
	}
	if ((presence & WeaponManifestPresenceFlagGuidance) != 0U) {
		u8(bytes, static_cast<std::uint8_t>(GuidanceType::None));
		f32(bytes, 0.0F);
	}
	if ((presence & WeaponManifestPresenceFlagLock) != 0U) {
		u64(bytes, 0U);
		f32(bytes, 0.0F);
	}
	f32(bytes, 0.0F);
	if ((presence & WeaponManifestPresenceFlagCargoRearm) != 0U) {
		f32(bytes, 1.0F);
		u64(bytes, 0U);
		u32(bytes, 1U);
	}
	if ((presence & WeaponManifestPresenceFlagBurst) != 0U) {
		u16(bytes, 1U);
		u64(bytes, 0U);
	}
	if ((presence & WeaponManifestPresenceFlagSwarm) != 0U) {
		u16(bytes, 1U);
		u16(bytes, 1U);
	}
	if ((presence & WeaponManifestPresenceFlagCountermeasure) != 0U) {
		f32(bytes, 0.0F);
	}
	return bytes;
}

Bytes entity_lifecycle(std::uint64_t presence = 0U,
	std::uint8_t object_type = static_cast<std::uint8_t>(ObjectType::Other),
	std::uint8_t phase = static_cast<std::uint8_t>(LifecyclePhase::Active),
	std::uint32_t flags = 0U)
{
	Bytes bytes;
	u64(bytes, 1U);
	u64(bytes, presence);
	u64(bytes, 1234U);
	u8(bytes, object_type);
	u8(bytes, phase);
	u32(bytes, flags);
	if ((presence & EntityLifecyclePresenceFlagSignature) != 0U) {
		u32(bytes, 0U);
	}
	if ((presence & EntityLifecyclePresenceFlagNetSignature) != 0U) {
		u32(bytes, 0U);
	}
	if ((presence & EntityLifecyclePresenceFlagClassReference) != 0U) {
		u32(bytes, 1U);
	}
	if ((presence & EntityLifecyclePresenceFlagParent) != 0U) {
		u64(bytes, 2U);
	}
	if ((presence & EntityLifecyclePresenceFlagArrivalMode) != 0U) {
		u8(bytes, static_cast<std::uint8_t>(TransitMode::None));
	}
	if ((presence & EntityLifecyclePresenceFlagDepartureMode) != 0U) {
		u8(bytes, static_cast<std::uint8_t>(TransitMode::None));
	}
	if ((presence & EntityLifecyclePresenceFlagNonShipNames) != 0U) {
		utf8(bytes, "object");
		utf8(bytes, "");
		utf8(bytes, "");
	}
	if ((presence & EntityLifecyclePresenceFlagNonShipTeamIff) != 0U) {
		u32(bytes, 0U);
		u32(bytes, 0U);
	}
	if ((presence & EntityLifecyclePresenceFlagNonShipRadius) != 0U) {
		f32(bytes, 0.0F);
	}
	return bytes;
}

Bytes ship_identity(std::uint64_t presence = 0U)
{
	Bytes bytes;
	u64(bytes, 1U);
	u64(bytes, presence);
	u64(bytes, 1234U);
	u32(bytes, 1U);
	utf8(bytes, "ship");
	if ((presence & ShipIdentityPresenceFlagDisplayName) != 0U) {
		utf8(bytes, "display");
	}
	if ((presence & ShipIdentityPresenceFlagCallsign) != 0U) {
		utf8(bytes, "callsign");
	}
	u32(bytes, 0U);
	u32(bytes, 0U);
	u32(bytes, 0U);
	u16(bytes, 0U);
	f32(bytes, 0.0F);
	if ((presence & ShipIdentityPresenceFlagWing) != 0U) {
		u32(bytes, 1U);
		u16(bytes, 0U);
		utf8(bytes, "Alpha");
	}
	if ((presence & ShipIdentityPresenceFlagLogicalSize) != 0U) {
		f32(bytes, 0.0F);
	}
	if ((presence & ShipIdentityPresenceFlagSensorVisibility) != 0U) {
		u16(bytes, 0U);
	}
	return bytes;
}

void vec3(Bytes& bytes, float x = 0.0F, float y = 0.0F, float z = 0.0F)
{
	f32(bytes, x);
	f32(bytes, y);
	f32(bytes, z);
}

Bytes flight_state(std::uint64_t presence = 0U)
{
	Bytes bytes;
	u64(bytes, 1U);
	u64(bytes, presence);
	u64(bytes, 1234U);
	vec3(bytes);
	f32(bytes, 1.0F);
	f32(bytes, 0.0F);
	f32(bytes, 0.0F);
	f32(bytes, 0.0F);
	vec3(bytes);
	vec3(bytes);
	f32(bytes, 0.0F);
	u32(bytes, 0U);
	if ((presence & FlightStatePresenceFlagDesiredVel) != 0U) {
		vec3(bytes);
	}
	if ((presence & FlightStatePresenceFlagDesiredRotvel) != 0U) {
		vec3(bytes);
	}
	if ((presence & FlightStatePresenceFlagPrevRampVel) != 0U) {
		vec3(bytes);
	}
	if ((presence & FlightStatePresenceFlagVelocityCaps) != 0U) {
		vec3(bytes);
		vec3(bytes);
		vec3(bytes);
	}
	if ((presence & FlightStatePresenceFlagRotationCaps) != 0U) {
		vec3(bytes);
	}
	if ((presence & FlightStatePresenceFlagRearCap) != 0U) {
		f32(bytes, 0.0F);
	}
	if ((presence & FlightStatePresenceFlagGlideCaps) != 0U) {
		f32(bytes, 0.0F);
		f32(bytes, 0.0F);
	}
	if ((presence & FlightStatePresenceFlagGravity) != 0U) {
		f32(bytes, 0.0F);
	}
	if ((presence & FlightStatePresenceFlagTimeConstants) != 0U) {
		for (unsigned int index = 0; index < 6U; ++index) {
			f32(bytes, 0.0F);
		}
	}
	if ((presence & FlightStatePresenceFlagRotdamp) != 0U) {
		f32(bytes, 0.0F);
	}
	if ((presence & FlightStatePresenceFlagSideSlip) != 0U) {
		f32(bytes, 0.0F);
	}
	if ((presence & FlightStatePresenceFlagCosmeticThrust) != 0U) {
		vec3(bytes);
		vec3(bytes);
	}
	return bytes;
}

Bytes control_state(std::uint64_t presence = 0U)
{
	Bytes bytes;
	u64(bytes, 1U);
	u64(bytes, presence);
	u64(bytes, 1234U);
	for (unsigned int index = 0; index < 6U; ++index) {
		f32(bytes, 0.0F);
	}
	u8(bytes, static_cast<std::uint8_t>(ControlMode::Ship));
	u32(bytes, 0U);
	if ((presence & ControlStatePresenceFlagCruise) != 0U) {
		f32(bytes, 0.0F);
	}
	if ((presence & ControlStatePresenceFlagRequestCounters) != 0U) {
		u16(bytes, 0U);
		u16(bytes, 0U);
		u16(bytes, 0U);
	}
	if ((presence & ControlStatePresenceFlagFlightCursor) != 0U) {
		f32(bytes, 0.0F);
		f32(bytes, 0.0F);
		f32(bytes, 0.5F);
		f32(bytes, 0.1F);
	}
	return bytes;
}

Bytes damage_state(std::uint64_t presence = 0U)
{
	Bytes bytes;
	u64(bytes, 1U);
	u64(bytes, presence);
	u64(bytes, 1234U);
	f32(bytes, 50.0F);
	f32(bytes, 100.0F);
	u16(bytes, 0U);
	if ((presence & DamageStatePresenceFlagSimHull) != 0U) {
		f32(bytes, 50.0F);
	}
	if ((presence & DamageStatePresenceFlagArmor) != 0U) {
		u32(bytes, 1U);
	}
	if ((presence & DamageStatePresenceFlagGuardian) != 0U) {
		f32(bytes, 10.0F);
	}
	if ((presence & DamageStatePresenceFlagCumulativeDamage) != 0U) {
		f32(bytes, 100.0F);
	}
	if ((presence & DamageStatePresenceFlagLastDamage) != 0U) {
		u64(bytes, 0U);
		u32(bytes, 0U);
	}
	if ((presence & DamageStatePresenceFlagContributors) != 0U) {
		u16(bytes, 0U);
		u8(bytes, 1U);
		u16(bytes, 20U);
	}
	return bytes;
}

Bytes shield_state(bool has_shields = false, std::uint16_t segment_count = 0U, std::uint64_t presence = 0U)
{
	Bytes bytes;
	u64(bytes, 1U);
	u64(bytes, presence);
	u64(bytes, 1234U);
	u8(bytes, has_shields ? 1U : 0U);
	u16(bytes, segment_count);
	u16(bytes, 0U);
	for (std::uint16_t index = 0; index < segment_count; ++index) {
		f32(bytes, 5.0F);
	}
	for (std::uint16_t index = 0; index < segment_count; ++index) {
		f32(bytes, 10.0F);
	}
	if ((presence & ShieldStatePresenceFlagRechargeMax) != 0U) {
		f32(bytes, 0.0F);
	}
	if ((presence & ShieldStatePresenceFlagRegenRate) != 0U) {
		f32(bytes, 0.0F);
	}
	if ((presence & ShieldStatePresenceFlagDeferredTransfer) != 0U) {
		f32(bytes, 0.0F);
	}
	return bytes;
}

std::vector<std::pair<RecordType, Bytes>> minimal_payloads()
{
	return {{RecordType::SessionState, session_state()},
		{RecordType::MissionState, mission_state()},
		{RecordType::ClassManifest, class_manifest()},
		{RecordType::WeaponManifest, weapon_manifest()},
		{RecordType::EntityLifecycle, entity_lifecycle()},
		{RecordType::ShipIdentity, ship_identity()},
		{RecordType::FlightState, flight_state()},
		{RecordType::ControlState, control_state()},
		{RecordType::DamageState, damage_state()},
		{RecordType::ShieldState, shield_state()}};
}

TEST(TelemetryProtocolBusinessRecords1To10, MinimalCanonicalPayloadForEveryTypeIsAcceptedExactly)
{
	for (auto entry : minimal_payloads()) {
		SCOPED_TRACE(static_cast<std::uint16_t>(entry.first));
		EXPECT_EQ(ValidationError::None, validate(entry.first, entry.second));

		entry.second.push_back(0U);
		EXPECT_EQ(ValidationError::TrailingBytes, validate(entry.first, entry.second));
		entry.second.pop_back();
		entry.second.pop_back();
		EXPECT_EQ(ValidationError::TruncatedPayload, validate(entry.first, entry.second));
	}
}

TEST(TelemetryProtocolBusinessRecords1To10, ConditionalGroupsFollowTheSingleCanonicalDocumentOrder)
{
	const auto entity_presence = EntityLifecyclePresenceFlagSignature | EntityLifecyclePresenceFlagNetSignature |
						 EntityLifecyclePresenceFlagParent | EntityLifecyclePresenceFlagArrivalMode |
						 EntityLifecyclePresenceFlagDepartureMode | EntityLifecyclePresenceFlagNonShipNames |
						 EntityLifecyclePresenceFlagNonShipTeamIff | EntityLifecyclePresenceFlagNonShipRadius;
	const auto entity = entity_lifecycle(entity_presence,
		static_cast<std::uint8_t>(ObjectType::Other),
		static_cast<std::uint8_t>(LifecyclePhase::Departing));
	const std::array<std::pair<RecordType, Bytes>, 10U> payloads{{
		{RecordType::SessionState, session_state(SessionStatePresenceFlagObservedPlayer)},
		{RecordType::MissionState,
			mission_state(MissionStatePresenceFlagMissionName, static_cast<std::uint8_t>(MissionPhase::Active))},
		{RecordType::ClassManifest, class_manifest(KnownClassManifestPresenceFlags)},
		{RecordType::WeaponManifest, weapon_manifest(KnownWeaponManifestPresenceFlags)},
		{RecordType::EntityLifecycle, entity},
		{RecordType::ShipIdentity, ship_identity(KnownShipIdentityPresenceFlags)},
		{RecordType::FlightState, flight_state(KnownFlightStatePresenceFlags)},
		{RecordType::ControlState, control_state(KnownControlStatePresenceFlags)},
		{RecordType::DamageState, damage_state(KnownDamageStatePresenceFlags)},
		{RecordType::ShieldState, shield_state(true, 1U, KnownShieldStatePresenceFlags)},
	}};
	for (const auto& entry : payloads) {
		SCOPED_TRACE(static_cast<std::uint16_t>(entry.first));
		EXPECT_EQ(ValidationError::None, validate(entry.first, entry.second));
	}
}

TEST(TelemetryProtocolBusinessRecords1To10, EveryTopLevelPresenceBitmapIsClosed)
{
	for (auto entry : minimal_payloads()) {
		const auto presence_offset = entry.first == RecordType::SessionState || entry.first == RecordType::MissionState
								 ? 0U
								 : 8U;
		overwrite_u64(entry.second, presence_offset, std::uint64_t{1} << 63U);
		SCOPED_TRACE(static_cast<std::uint16_t>(entry.first));
		EXPECT_EQ(ValidationError::ReservedFlag, validate(entry.first, entry.second));
	}
}

TEST(TelemetryProtocolBusinessRecords1To10, SessionEnforcesCapabilityPairsCoverageAuthorityAndVisibility)
{
	auto payload = session_state(0U,
		static_cast<std::uint8_t>(AuthorityMode::Solo),
		static_cast<std::uint8_t>(VisibilityMode::Cockpit),
		CapabilityCommViewLocalAssets);
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate(RecordType::SessionState, payload));

	payload = session_state(0U,
		static_cast<std::uint8_t>(AuthorityMode::Solo),
		static_cast<std::uint8_t>(VisibilityMode::Cockpit),
		0U,
		0U);
	EXPECT_EQ(ValidationError::InvalidAbsence, validate(RecordType::SessionState, payload));

	payload = session_state(0U,
		static_cast<std::uint8_t>(AuthorityMode::MultiplayerClient),
		static_cast<std::uint8_t>(VisibilityMode::Cockpit));
	EXPECT_EQ(ValidationError::InvalidAbsence, validate(RecordType::SessionState, payload));

	payload = session_state(SessionStatePresenceFlagObservedPlayer,
		static_cast<std::uint8_t>(AuthorityMode::MultiplayerClient),
		1U);
	EXPECT_EQ(ValidationError::UnknownEnum, validate(RecordType::SessionState, payload));

	payload = session_state(0U,
		static_cast<std::uint8_t>(AuthorityMode::Solo),
		static_cast<std::uint8_t>(VisibilityMode::Cockpit),
		0U,
		StateDomainCoverageBitCoreShip | 0x0010ULL);
	EXPECT_EQ(ValidationError::ReservedFlag, validate(RecordType::SessionState, payload));

	payload = session_state(0U,
		static_cast<std::uint8_t>(AuthorityMode::Solo),
		static_cast<std::uint8_t>(VisibilityMode::Cockpit),
		0U,
		StateDomainCoverageBitCoreShip,
		0U,
		EventFamilyBitCommunication);
	EXPECT_EQ(ValidationError::CapabilityNotNegotiated, validate(RecordType::SessionState, payload));
}

TEST(TelemetryProtocolBusinessRecords1To10, PlayerKinematicsBitIsVersionGated)
{
	const auto payload = session_state(0U, static_cast<std::uint8_t>(AuthorityMode::Solo),
		static_cast<std::uint8_t>(VisibilityMode::Cockpit), 0U, StateDomainCoverageBitPlayerKinematics);
	BusinessRecordMetadata metadata;
	EXPECT_EQ(ValidationError::ReservedFlag, validate_business_record(record(RecordType::SessionState, payload),
		BusinessRecordContainer::FullSnapshot, VersionMinorV1_0, metadata));
	EXPECT_EQ(ValidationError::None, validate_business_record(record(RecordType::SessionState, payload),
		BusinessRecordContainer::FullSnapshot, VersionMinorV1_1, metadata));
}

TEST(TelemetryProtocolBusinessRecords1To10, MissionEnforcesEnumsBoolReservedBytesNameAndFloat)
{
	auto payload = mission_state();
	payload[13U] = 2U;
	EXPECT_EQ(ValidationError::OutOfRange, validate(RecordType::MissionState, payload));

	payload = mission_state();
	payload[14U] = 1U;
	EXPECT_EQ(ValidationError::ReservedFlag, validate(RecordType::MissionState, payload));

	payload = mission_state(MissionStatePresenceFlagMissionName, static_cast<std::uint8_t>(MissionPhase::None));
	EXPECT_EQ(ValidationError::InvalidAbsence, validate(RecordType::MissionState, payload));

	payload = mission_state(MissionStatePresenceFlagMissionName, static_cast<std::uint8_t>(MissionPhase::Active));
	payload[30U] = 0xffU;
	EXPECT_EQ(ValidationError::InvalidUtf8, validate(RecordType::MissionState, payload));

	payload = mission_state();
	overwrite_f32(payload, 16U, std::numeric_limits<float>::quiet_NaN());
	EXPECT_EQ(ValidationError::NonFiniteFloat, validate(RecordType::MissionState, payload));
}

TEST(TelemetryProtocolBusinessRecords1To10, ManifestListsEnforceVersionsLengthsKeysFamiliesAndFixedItems)
{
	Bytes banks;
	u16(banks, 1U);
	const auto primary = class_bank_item(7U);
	vlist_item(banks, primary);
	auto payload = class_manifest(ClassManifestPresenceFlagBanks, &banks);
	EXPECT_EQ(ValidationError::None, validate(RecordType::ClassManifest, payload));

	banks.clear();
	u16(banks, 2U);
	vlist_item(banks, primary);
	vlist_item(banks, primary);
	payload = class_manifest(ClassManifestPresenceFlagBanks, &banks);
	EXPECT_EQ(ValidationError::DuplicateItemKey, validate(RecordType::ClassManifest, payload));

	banks.clear();
	u16(banks, 1U);
	vlist_item(banks, primary, 2U);
	payload = class_manifest(ClassManifestPresenceFlagBanks, &banks);
	EXPECT_EQ(ValidationError::UnsupportedRecordVersion, validate(RecordType::ClassManifest, payload));

	banks.clear();
	u16(banks, 1U);
	vlist_item(banks, primary, 1U, 1U);
	payload = class_manifest(ClassManifestPresenceFlagBanks, &banks);
	EXPECT_EQ(ValidationError::BadRecordLength, validate(RecordType::ClassManifest, payload));

	banks.clear();
	u16(banks, 193U);
	payload = class_manifest(ClassManifestPresenceFlagBanks, &banks);
	EXPECT_EQ(ValidationError::OutOfRange, validate(RecordType::ClassManifest, payload));

	banks.clear();
	u16(banks, 1U);
	const auto turret = class_bank_item(8U, static_cast<std::uint8_t>(WeaponFamily::Tertiary));
	vlist_item(banks, turret);
	payload = class_manifest(ClassManifestPresenceFlagBanks, &banks);
	EXPECT_EQ(ValidationError::InvalidAbsence, validate(RecordType::ClassManifest, payload));

	banks.clear();
	u16(banks, 1U);
	auto bad_fire_points = primary;
	overwrite_u16(bad_fire_points, bad_fire_points.size() - 5U, 257U);
	vlist_item(banks, bad_fire_points);
	payload = class_manifest(ClassManifestPresenceFlagBanks, &banks);
	EXPECT_EQ(ValidationError::OutOfRange, validate(RecordType::ClassManifest, payload));

	Bytes subsystems;
	u16(subsystems, 1U);
	const auto subsystem = class_subsystem_item(9U);
	vlist_item(subsystems, subsystem);
	payload = class_manifest(ClassManifestPresenceFlagSubsystems, nullptr, &subsystems);
	EXPECT_EQ(ValidationError::None, validate(RecordType::ClassManifest, payload));

	subsystems.clear();
	u16(subsystems, 2U);
	vlist_item(subsystems, subsystem);
	vlist_item(subsystems, subsystem);
	payload = class_manifest(ClassManifestPresenceFlagSubsystems, nullptr, &subsystems);
	EXPECT_EQ(ValidationError::DuplicateItemKey, validate(RecordType::ClassManifest, payload));

	subsystems.clear();
	u16(subsystems, 1U);
	auto reserved_subsystem = subsystem;
	reserved_subsystem[9U] = 1U;
	vlist_item(subsystems, reserved_subsystem);
	payload = class_manifest(ClassManifestPresenceFlagSubsystems, nullptr, &subsystems);
	EXPECT_EQ(ValidationError::ReservedFlag, validate(RecordType::ClassManifest, payload));
}

TEST(TelemetryProtocolBusinessRecords1To10, ManifestScalarsAndStringsRejectInvalidInputs)
{
	auto payload = class_manifest();
	overwrite_f32(payload, 30U, 0.0F);
	EXPECT_EQ(ValidationError::OutOfRange, validate(RecordType::ClassManifest, payload));

	payload = class_manifest();
	payload[18U] = 0U;
	EXPECT_EQ(ValidationError::InvalidUtf8, validate(RecordType::ClassManifest, payload));

	payload = class_manifest(ClassManifestPresenceFlagInertia);
	overwrite_f32(payload, 46U, std::numeric_limits<float>::infinity());
	EXPECT_EQ(ValidationError::NonFiniteFloat, validate(RecordType::ClassManifest, payload));

	payload = class_manifest(ClassManifestPresenceFlagAutoaim);
	overwrite_f32(payload, payload.size() - 4U, 3.2F);
	EXPECT_EQ(ValidationError::OutOfRange, validate(RecordType::ClassManifest, payload));
}

TEST(TelemetryProtocolBusinessRecords1To10, WeaponManifestEnforcesFlagsEnumsRangesAndConditionalCoherence)
{
	auto payload = weapon_manifest();
	payload[24U] = 6U;
	EXPECT_EQ(ValidationError::UnknownEnum, validate(RecordType::WeaponManifest, payload));

	payload = weapon_manifest();
	overwrite_u64(payload, 25U, std::uint64_t{1} << 63U);
	EXPECT_EQ(ValidationError::ReservedFlag, validate(RecordType::WeaponManifest, payload));

	payload = weapon_manifest(WeaponManifestPresenceFlagRanges);
	overwrite_f32(payload, 53U, 4.0F);
	EXPECT_EQ(ValidationError::OutOfRange, validate(RecordType::WeaponManifest, payload));

	payload = weapon_manifest(WeaponManifestPresenceFlagCargoRearm, WeaponClassFlagAmmoless);
	EXPECT_EQ(ValidationError::InvalidAbsence, validate(RecordType::WeaponManifest, payload));

	payload = weapon_manifest(WeaponManifestPresenceFlagBurst);
	overwrite_u16(payload, 57U, 0U);
	EXPECT_EQ(ValidationError::OutOfRange, validate(RecordType::WeaponManifest, payload));
}

TEST(TelemetryProtocolBusinessRecords1To10, EntityLifecycleEnforcesApplicabilityEnumsFlagsAndDeparture)
{
	auto payload = entity_lifecycle(EntityLifecyclePresenceFlagClassReference,
		static_cast<std::uint8_t>(ObjectType::Other));
	EXPECT_EQ(ValidationError::InvalidAbsence, validate(RecordType::EntityLifecycle, payload));

	payload = entity_lifecycle(EntityLifecyclePresenceFlagNonShipRadius,
		static_cast<std::uint8_t>(ObjectType::Ship));
	EXPECT_EQ(ValidationError::InvalidAbsence, validate(RecordType::EntityLifecycle, payload));

	payload = entity_lifecycle(0U,
		static_cast<std::uint8_t>(ObjectType::Other),
		static_cast<std::uint8_t>(LifecyclePhase::Spawning));
	EXPECT_EQ(ValidationError::InvalidAbsence, validate(RecordType::EntityLifecycle, payload));

	payload = entity_lifecycle(0U,
		static_cast<std::uint8_t>(ObjectType::Other),
		static_cast<std::uint8_t>(LifecyclePhase::Active),
		EntityLifecycleFlagBomb);
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate(RecordType::EntityLifecycle, payload));

	payload = entity_lifecycle(0U,
		static_cast<std::uint8_t>(ObjectType::Other),
		static_cast<std::uint8_t>(LifecyclePhase::Departing));
	EXPECT_EQ(ValidationError::InvalidAbsence, validate(RecordType::EntityLifecycle, payload));

	payload = entity_lifecycle(EntityLifecyclePresenceFlagDepartureMode,
		static_cast<std::uint8_t>(ObjectType::Other),
		static_cast<std::uint8_t>(LifecyclePhase::Departing));
	EXPECT_EQ(ValidationError::None, validate(RecordType::EntityLifecycle, payload));
}

TEST(TelemetryProtocolBusinessRecords1To10, ShipIdentityEnforcesReferencesTextBoundsAndClosedFlags)
{
	auto payload = ship_identity();
	overwrite_u32(payload, 24U, 0U);
	EXPECT_EQ(ValidationError::OutOfRange, validate(RecordType::ShipIdentity, payload));

	payload = ship_identity();
	payload[30U] = 0xffU;
	EXPECT_EQ(ValidationError::InvalidUtf8, validate(RecordType::ShipIdentity, payload));

	payload = ship_identity();
	overwrite_u16(payload, 28U, 256U);
	EXPECT_EQ(ValidationError::StringTooLong, validate(RecordType::ShipIdentity, payload));

	payload = ship_identity();
	overwrite_u32(payload, 38U, 65'536U);
	EXPECT_EQ(ValidationError::OutOfRange, validate(RecordType::ShipIdentity, payload));

	payload = ship_identity(ShipIdentityPresenceFlagSensorVisibility);
	overwrite_u16(payload, payload.size() - 2U, 0x8000U);
	EXPECT_EQ(ValidationError::ReservedFlag, validate(RecordType::ShipIdentity, payload));

	payload = ship_identity(ShipIdentityPresenceFlagWing);
	overwrite_u16(payload, 56U, 4096U);
	EXPECT_EQ(ValidationError::OutOfRange, validate(RecordType::ShipIdentity, payload));
}

TEST(TelemetryProtocolBusinessRecords1To10, FlightStateEnforcesCanonicalQuaternionFiniteRangesAndFlags)
{
	auto payload = flight_state();
	overwrite_f32(payload, 36U, 0.5F);
	EXPECT_EQ(ValidationError::OutOfRange, validate(RecordType::FlightState, payload));

	payload = flight_state();
	overwrite_f32(payload, 36U, -1.0F);
	EXPECT_EQ(ValidationError::OutOfRange, validate(RecordType::FlightState, payload));

	payload = flight_state();
	overwrite_f32(payload, 24U, std::numeric_limits<float>::quiet_NaN());
	EXPECT_EQ(ValidationError::NonFiniteFloat, validate(RecordType::FlightState, payload));

	payload = flight_state();
	overwrite_u32(payload, 80U, 0x80000000U);
	EXPECT_EQ(ValidationError::ReservedFlag, validate(RecordType::FlightState, payload));

	payload = flight_state(FlightStatePresenceFlagCosmeticThrust);
	overwrite_f32(payload, 84U, 1.01F);
	EXPECT_EQ(ValidationError::OutOfRange, validate(RecordType::FlightState, payload));
}

TEST(TelemetryProtocolBusinessRecords1To10, ControlStateEnforcesNormalizedInputsEnumsFlagsAndCursorBounds)
{
	auto payload = control_state();
	overwrite_f32(payload, 24U, 1.01F);
	EXPECT_EQ(ValidationError::OutOfRange, validate(RecordType::ControlState, payload));

	payload = control_state();
	payload[48U] = 5U;
	EXPECT_EQ(ValidationError::UnknownEnum, validate(RecordType::ControlState, payload));

	payload = control_state();
	overwrite_u32(payload, 49U, 0x80000000U);
	EXPECT_EQ(ValidationError::ReservedFlag, validate(RecordType::ControlState, payload));

	payload = control_state(ControlStatePresenceFlagCruise);
	overwrite_f32(payload, 53U, 101.0F);
	EXPECT_EQ(ValidationError::OutOfRange, validate(RecordType::ControlState, payload));

	payload = control_state(ControlStatePresenceFlagFlightCursor);
	overwrite_f32(payload, 61U, 1.1F);
	EXPECT_EQ(ValidationError::OutOfRange, validate(RecordType::ControlState, payload));
}

TEST(TelemetryProtocolBusinessRecords1To10, DamageStateEnforcesCorrelatedBoundsAndContributorListRules)
{
	auto payload = damage_state();
	overwrite_f32(payload, 24U, 101.0F);
	EXPECT_EQ(ValidationError::OutOfRange, validate(RecordType::DamageState, payload));

	payload = damage_state(DamageStatePresenceFlagContributors);
	overwrite_u16(payload, 34U, 65U);
	EXPECT_EQ(ValidationError::OutOfRange, validate(RecordType::DamageState, payload));

	payload = damage_state(DamageStatePresenceFlagContributors);
	payload[36U] = 2U;
	EXPECT_EQ(ValidationError::UnsupportedRecordVersion, validate(RecordType::DamageState, payload));

	payload = damage_state(DamageStatePresenceFlagContributors);
	overwrite_u16(payload, 37U, 19U);
	EXPECT_EQ(ValidationError::BadRecordLength, validate(RecordType::DamageState, payload));

	payload = damage_state(DamageStatePresenceFlagContributors);
	overwrite_u16(payload, 34U, 2U);
	u64(payload, 7U);
	u32(payload, 8U);
	u32(payload, 0U);
	f32(payload, 1.0F);
	u64(payload, 7U);
	u32(payload, 8U);
	u32(payload, 0U);
	f32(payload, 2.0F);
	EXPECT_EQ(ValidationError::DuplicateItemKey, validate(RecordType::DamageState, payload));

	payload = damage_state(DamageStatePresenceFlagContributors);
	overwrite_u16(payload, 34U, 1U);
	u64(payload, 7U);
	u32(payload, 8U);
	u32(payload, 1U);
	f32(payload, 1.0F);
	EXPECT_EQ(ValidationError::ReservedFlag, validate(RecordType::DamageState, payload));
}

TEST(TelemetryProtocolBusinessRecords1To10, ShieldStateEnforcesBoolCardinalityCorrelatedArraysAndAbsence)
{
	auto payload = shield_state();
	payload[24U] = 2U;
	EXPECT_EQ(ValidationError::OutOfRange, validate(RecordType::ShieldState, payload));

	payload = shield_state(true, 65U);
	EXPECT_EQ(ValidationError::OutOfRange, validate(RecordType::ShieldState, payload));

	payload = shield_state(false, 1U);
	EXPECT_EQ(ValidationError::InvalidAbsence, validate(RecordType::ShieldState, payload));

	payload = shield_state(true, 1U);
	overwrite_f32(payload, 29U, 11.0F);
	EXPECT_EQ(ValidationError::OutOfRange, validate(RecordType::ShieldState, payload));

	payload = shield_state(false, 0U, ShieldStatePresenceFlagRechargeMax);
	EXPECT_EQ(ValidationError::InvalidAbsence, validate(RecordType::ShieldState, payload));
}

} // namespace
