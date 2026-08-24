#pragma once

#include "telemetry/protocol/telemetry_protocol_constants.h"
#include "telemetry/protocol/telemetry_protocol_types.h"
#include "telemetry/protocol/telemetry_sha256.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace telemetry::detail {

constexpr std::size_t MaximumCommunicationBundleBytes = 4U * 1024U * 1024U;
constexpr std::size_t MaximumCommunicationMappings = 4096U;

enum class CommunicationBundleError : std::uint8_t {
	None = 0, Absent, ReadFailure, TooLarge, InvalidJson, InvalidManifest,
	InvalidBundleInfo, HashMismatch, LimitExceeded, ContradictoryMapping,
};

struct CommunicationAsset {
	std::uint64_t asset_id = 0U;
	protocol::Sha256Digest content_hash{};
	protocol::SourceFormat source_format = protocol::SourceFormat::Invalid;
	protocol::DeliveredFormat delivered_format = protocol::DeliveredFormat::Invalid;
	protocol::AlphaMode alpha_mode = protocol::AlphaMode::None;
	protocol::AssetTimingMode timing_mode = protocol::AssetTimingMode::FormatIntrinsic;
	std::uint16_t width = 0U, height = 0U;
	std::uint32_t frame_count = 0U;
	std::uint64_t duration_us = 0U;
	std::string logical_name, file_path;
	std::vector<std::uint32_t> frame_durations_us;
};

struct CommunicationMapping {
	std::string generic_anim_name;
	protocol::SourceFormat generic_anim_type = protocol::SourceFormat::Invalid;
	std::uint64_t asset_id = 0U;
};

struct CommunicationBundle {
	protocol::Sha256Digest bundle_hash{};
	std::uint64_t frame_asset_id = 0U, placeholder_asset_id = 0U;
	std::uint32_t required_delivered_formats = 0U;
	std::string converter_id, converter_version;
	std::vector<CommunicationAsset> assets;
	std::vector<CommunicationMapping> mappings;

	std::uint64_t resolve(std::string_view final_name, protocol::SourceFormat type) const noexcept;
	const CommunicationAsset* find_asset(std::uint64_t asset_id) const noexcept;
};

struct CommunicationBundleLoadResult {
	CommunicationBundleError error = CommunicationBundleError::Absent;
	CommunicationBundle bundle;
	bool available() const noexcept { return error == CommunicationBundleError::None; }
};

CommunicationBundleLoadResult load_communication_bundle(std::string_view root_path) noexcept;
std::string normalize_generic_anim_name(std::string_view name);

} // namespace telemetry::detail
