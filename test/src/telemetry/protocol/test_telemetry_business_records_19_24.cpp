#include "telemetry/protocol/telemetry_business_records.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
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

void string(std::vector<std::uint8_t>& bytes, const char* value)
{
	const auto length = std::strlen(value);
	u16(bytes, static_cast<std::uint16_t>(length));
	bytes.insert(bytes.end(), value, value + length);
}

ByteView view(const std::vector<std::uint8_t>& bytes)
{
	return ByteView{bytes.empty() ? nullptr : bytes.data(), bytes.size()};
}

RecordEnvelopeView record(RecordType type, const std::vector<std::uint8_t>& payload, std::uint8_t flags = 0)
{
	return RecordEnvelopeView{static_cast<std::uint16_t>(type), 1, flags, view(payload)};
}

std::vector<std::uint8_t> base(RecordType type, std::uint64_t presence = 0)
{
	std::vector<std::uint8_t> bytes;
	u64(bytes, 0x0102030405060708ULL);
	u64(bytes, presence);
	u64(bytes, 1234);
	switch (type) {
	case RecordType::ThreatState:
		u8(bytes, static_cast<std::uint8_t>(ThreatLevel::None));
		u16(bytes, 0);
		break;
	case RecordType::CargoScanState:
		u8(bytes, static_cast<std::uint8_t>(ScanPhase::Idle));
		u8(bytes, static_cast<std::uint8_t>(DisclosureState::Hidden));
		break;
	case RecordType::DockingState:
		u8(bytes, static_cast<std::uint8_t>(DockingPhase::None));
		u64(bytes, 0);
		u16(bytes, 0);
		break;
	case RecordType::SupportState:
		u8(bytes, static_cast<std::uint8_t>(SupportPhase::None));
		u8(bytes, 0);
		u8(bytes, 0);
		u8(bytes, 0);
		u8(bytes, 0);
		break;
	case RecordType::NavigationState:
		u8(bytes, static_cast<std::uint8_t>(AutopilotState::Disengaged));
		u16(bytes, 0);
		break;
	case RecordType::EffectState:
		u32(bytes, 0);
		break;
	default:
		break;
	}
	return bytes;
}

ValidationError validate(RecordType type, const std::vector<std::uint8_t>& payload)
{
	BusinessRecordMetadata metadata;
	return validate_business_record(record(type, payload), BusinessRecordContainer::FullSnapshot, metadata);
}

TEST(TelemetryProtocolBusinessRecords19To24, MinimalCanonicalPayloadForEveryTypeIsAcceptedExactly)
{
	for (std::uint16_t raw = static_cast<std::uint16_t>(RecordType::ThreatState);
		raw <= static_cast<std::uint16_t>(RecordType::EffectState);
		++raw) {
		const auto type = static_cast<RecordType>(raw);
		auto payload = base(type);
		SCOPED_TRACE(raw);
		EXPECT_EQ(ValidationError::None, validate(type, payload));
		payload.push_back(0);
		EXPECT_EQ(ValidationError::TrailingBytes, validate(type, payload));
	}
}

TEST(TelemetryProtocolBusinessRecords19To24, MetadataAndStateAtomUseEntityRootAsTheOnlyCascadeOwner)
{
	for (std::uint16_t raw = static_cast<std::uint16_t>(RecordType::ThreatState);
		raw <= static_cast<std::uint16_t>(RecordType::EffectState);
		++raw) {
		const auto type = static_cast<RecordType>(raw);
		const auto payload = base(type);
		StateAtom atom;
		ASSERT_EQ(ValidationError::None,
			decode_business_state_atom(record(type, payload), BusinessRecordContainer::FullSnapshot, atom));
		EXPECT_EQ(raw, atom.key.record_type);
		EXPECT_EQ(8U, atom.key.identity.size());
		EXPECT_TRUE(atom.has_cascade_owner);
		EXPECT_EQ(static_cast<std::uint16_t>(RecordType::EntityLifecycle), atom.cascade_owner.record_type);
		EXPECT_EQ(atom.key.identity, atom.cascade_owner.identity);
		EXPECT_EQ(payload, atom.value);
	}
}

TEST(TelemetryProtocolBusinessRecords19To24, ThreatRejectsUnknownEnumAndReservedPresence)
{
	auto payload = base(RecordType::ThreatState);
	payload[24] = 4;
	EXPECT_EQ(ValidationError::UnknownEnum, validate(RecordType::ThreatState, payload));

	payload = base(RecordType::ThreatState);
	payload[8] = 0x08;
	EXPECT_EQ(ValidationError::ReservedFlag, validate(RecordType::ThreatState, payload));
}

TEST(TelemetryProtocolBusinessRecords19To24, CargoDisclosureControlsTextWithoutLeakingHiddenCargo)
{
	auto hidden_completed = base(RecordType::CargoScanState);
	hidden_completed[24] = static_cast<std::uint8_t>(ScanPhase::Completed);
	EXPECT_EQ(ValidationError::None, validate(RecordType::CargoScanState, hidden_completed));

	auto inconsistent = base(RecordType::CargoScanState);
	inconsistent[25] = static_cast<std::uint8_t>(DisclosureState::Revealed);
	EXPECT_EQ(ValidationError::InvalidAbsence, validate(RecordType::CargoScanState, inconsistent));

	std::vector<std::uint8_t> revealed;
	u64(revealed, 7);
	u64(revealed, CargoScanStatePresenceFlagCargoText);
	u64(revealed, 50);
	u8(revealed, static_cast<std::uint8_t>(ScanPhase::Completed));
	u8(revealed, static_cast<std::uint8_t>(DisclosureState::Revealed));
	string(revealed, "Medical supplies");
	EXPECT_EQ(ValidationError::None, validate(RecordType::CargoScanState, revealed));
}

TEST(TelemetryProtocolBusinessRecords19To24, NavigationRequiresRefusalAndCurrentNavpointMembership)
{
	auto refused = base(RecordType::NavigationState);
	refused[24] = static_cast<std::uint8_t>(AutopilotState::Refused);
	EXPECT_EQ(ValidationError::InvalidAbsence, validate(RecordType::NavigationState, refused));

	std::vector<std::uint8_t> payload;
	u64(payload, 7);
	u64(payload, NavigationStatePresenceFlagCurrentNavpoint);
	u64(payload, 1);
	u8(payload, static_cast<std::uint8_t>(AutopilotState::Available));
	u16(payload, 0);
	u32(payload, 99);
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate(RecordType::NavigationState, payload));
}

TEST(TelemetryProtocolBusinessRecords19To24, EffectRejectsNonFiniteAndLaserWithoutMatchingFlag)
{
	std::vector<std::uint8_t> laser;
	u64(laser, 7);
	u64(laser, EffectStatePresenceFlagTargetingLaserVisual);
	u64(laser, 1);
	u32(laser, 0);
	f32(laser, 0.5F);
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate(RecordType::EffectState, laser));

	laser[24] = static_cast<std::uint8_t>(EffectFlagTargetingLaser);
	laser[28] = 0;
	laser[29] = 0;
	laser[30] = 0x80;
	laser[31] = 0x7f;
	EXPECT_EQ(ValidationError::NonFiniteFloat, validate(RecordType::EffectState, laser));
}

} // namespace
