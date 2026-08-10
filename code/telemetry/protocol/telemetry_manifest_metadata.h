#pragma once

#include "telemetry/protocol/telemetry_protocol_constants.h"
#include "telemetry/protocol/telemetry_records.h"

#include <cstdint>

namespace telemetry::protocol {

struct ClassManifestRadarMetadata {
	std::uint32_t manifest_generation = 0;
	std::uint32_t class_id = 0;
	std::uint32_t ship_type_id = 0;
	std::uint32_t radar_icon_id = 0;
	bool has_radar_icon = false;
};

struct WeaponManifestRadarMetadata {
	std::uint32_t manifest_generation = 0;
	std::uint32_t weapon_class_id = 0;
	WeaponSubtype subtype = WeaponSubtype::Unknown;
	std::uint64_t weapon_flags = 0;
};

// These focused decoders validate the complete manifest record before
// publishing the small subset needed by radar clients. Output is unchanged on
// error and unknown future radar_icon_id values are preserved for fallback.
ValidationError decode_class_manifest_radar_metadata(
	const RecordEnvelopeView& record,
	std::uint8_t protocol_minor,
	ClassManifestRadarMetadata& output) noexcept;
ValidationError decode_weapon_manifest_radar_metadata(
	const RecordEnvelopeView& record,
	std::uint8_t protocol_minor,
	WeaponManifestRadarMetadata& output) noexcept;

} // namespace telemetry::protocol
