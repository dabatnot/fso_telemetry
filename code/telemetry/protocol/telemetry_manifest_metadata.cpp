#include "telemetry/protocol/telemetry_manifest_metadata.h"

#include "telemetry/protocol/packet_reader.h"
#include "telemetry/protocol/telemetry_business_records.h"

#include <string_view>

namespace telemetry::protocol {
namespace {

bool skip_versioned_items(PacketReader& reader) noexcept
{
	std::uint16_t count = 0;
	if (!reader.read_u16(count)) return false;
	for (std::uint16_t index = 0; index < count; ++index) {
		std::uint8_t version = 0;
		std::uint16_t size = 0;
		if (!reader.read_u8(version) || version != 1U ||
			!reader.read_u16(size) || !reader.skip(size)) {
			return false;
		}
	}
	return true;
}

ValidationError validate_manifest_record(const RecordEnvelopeView& record,
	RecordType expected,
	std::uint8_t protocol_minor) noexcept
{
	if (record.raw_record_type != static_cast<std::uint16_t>(expected))
		return ValidationError::InvalidStateTransition;
	BusinessRecordMetadata metadata;
	return validate_business_record(
		record, BusinessRecordContainer::Manifest, protocol_minor, metadata);
}

} // namespace

ValidationError decode_class_manifest_radar_metadata(
	const RecordEnvelopeView& record,
	std::uint8_t protocol_minor,
	ClassManifestRadarMetadata& output) noexcept
{
	if (const auto error = validate_manifest_record(
			record, RecordType::ClassManifest, protocol_minor);
		error != ValidationError::None) {
		return error;
	}
	ClassManifestRadarMetadata candidate;
	PacketReader reader(record.payload);
	std::uint64_t presence = 0;
	std::string_view name;
	std::uint32_t ignored_u32 = 0;
	float ignored_f32 = 0.0F;
	if (!reader.read_u32(candidate.manifest_generation) ||
		!reader.read_u32(candidate.class_id) || !reader.read_u64(presence) ||
		!reader.read_utf8(255U, name) || !reader.read_u32(ignored_u32) ||
		!reader.read_u32(candidate.ship_type_id) ||
		!reader.read_f32(ignored_f32) || !reader.skip(3U * sizeof(float))) {
		return ValidationError::BadRecordLength;
	}
	if ((presence & ClassManifestPresenceFlagInertia) != 0U &&
		!reader.skip(9U * sizeof(float))) return ValidationError::BadRecordLength;
	if ((presence & ClassManifestPresenceFlagDamping) != 0U &&
		!reader.skip(5U * sizeof(float))) return ValidationError::BadRecordLength;
	if ((presence & ClassManifestPresenceFlagMotion) != 0U &&
		!reader.skip(19U * sizeof(float))) return ValidationError::BadRecordLength;
	if ((presence & ClassManifestPresenceFlagHull) != 0U &&
		!reader.skip(sizeof(float))) return ValidationError::BadRecordLength;
	if ((presence & ClassManifestPresenceFlagShield) != 0U &&
		!reader.skip(sizeof(float))) return ValidationError::BadRecordLength;
	if ((presence & ClassManifestPresenceFlagEnergy) != 0U &&
		!reader.skip(5U * sizeof(float))) return ValidationError::BadRecordLength;
	if ((presence & ClassManifestPresenceFlagAfterburner) != 0U &&
		!reader.skip(4U * sizeof(float) + sizeof(std::uint64_t)))
		return ValidationError::BadRecordLength;
	if ((presence & ClassManifestPresenceFlagCountermeasure) != 0U &&
		!reader.skip(2U * sizeof(std::uint32_t) + sizeof(std::uint64_t)))
		return ValidationError::BadRecordLength;
	if ((presence & ClassManifestPresenceFlagBanks) != 0U &&
		!skip_versioned_items(reader)) return ValidationError::BadRecordLength;
	if ((presence & ClassManifestPresenceFlagSubsystems) != 0U &&
		!skip_versioned_items(reader)) return ValidationError::BadRecordLength;
	if ((presence & ClassManifestPresenceFlagScan) != 0U &&
		!reader.skip(sizeof(std::uint64_t) + 2U * sizeof(float)))
		return ValidationError::BadRecordLength;
	if ((presence & ClassManifestPresenceFlagGlide) != 0U &&
		!reader.skip(sizeof(float))) return ValidationError::BadRecordLength;
	if ((presence & ClassManifestPresenceFlagAutoaim) != 0U &&
		!reader.skip(sizeof(float))) return ValidationError::BadRecordLength;
	if ((presence & ClassManifestPresenceFlagRadarIcon) != 0U) {
		if (!reader.read_u32(candidate.radar_icon_id))
			return ValidationError::BadRecordLength;
		candidate.has_radar_icon = true;
	}
	if (!reader.at_end()) return ValidationError::BadRecordLength;
	output = candidate;
	return ValidationError::None;
}

ValidationError decode_weapon_manifest_radar_metadata(
	const RecordEnvelopeView& record,
	std::uint8_t protocol_minor,
	WeaponManifestRadarMetadata& output) noexcept
{
	if (const auto error = validate_manifest_record(
			record, RecordType::WeaponManifest, protocol_minor);
		error != ValidationError::None) {
		return error;
	}
	WeaponManifestRadarMetadata candidate;
	PacketReader reader(record.payload);
	std::uint64_t presence = 0;
	std::string_view text;
	std::uint8_t subtype = 0;
	if (!reader.read_u32(candidate.manifest_generation) ||
		!reader.read_u32(candidate.weapon_class_id) ||
		!reader.read_u64(presence) || !reader.read_utf8(255U, text) ||
		((presence & WeaponManifestPresenceFlagTitle) != 0U &&
		 !reader.read_utf8(255U, text)) ||
		!reader.read_u8(subtype) || !reader.read_u64(candidate.weapon_flags)) {
		return ValidationError::BadRecordLength;
	}
	candidate.subtype = static_cast<WeaponSubtype>(subtype);
	output = candidate;
	return ValidationError::None;
}

} // namespace telemetry::protocol
