#include "telemetry/communication_bundle.h"

#include "libs/jansson.h"
#include "telemetry/protocol/telemetry_specialized_views.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>

namespace telemetry::detail {
namespace {

using JsonPtr = std::unique_ptr<json_t, decltype(&json_decref)>;

bool read_bounded(const std::filesystem::path& path, std::string& output)
{
	std::ifstream stream(path, std::ios::binary | std::ios::ate);
	if (!stream) return false;
	const auto end = stream.tellg();
	if (end < 0 || static_cast<std::uint64_t>(end) > MaximumCommunicationBundleBytes) return false;
	output.resize(static_cast<std::size_t>(end));
	stream.seekg(0);
	return output.empty() || static_cast<bool>(stream.read(output.data(), static_cast<std::streamsize>(output.size())));
}

int hex_nibble(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

bool parse_digest(std::string_view text, protocol::Sha256Digest& digest)
{
	if (text.size() != 64U) return false;
	for (std::size_t i = 0; i < digest.size(); ++i) {
		const auto high = hex_nibble(text[i * 2U]), low = hex_nibble(text[i * 2U + 1U]);
		if (high < 0 || low < 0) return false;
		digest[i] = static_cast<std::uint8_t>((high << 4) | low);
	}
	return true;
}

bool parse_asset_id(std::string_view text, std::uint64_t& id)
{
	if (text.size() != 16U) return false;
	id = 0U;
	for (const auto c : text) {
		const auto value = hex_nibble(c);
		if (value < 0) return false;
		id = (id << 4U) | static_cast<std::uint64_t>(value);
	}
	return id != 0U;
}

bool read_string(const json_t* object, const char* key, std::string& output, std::size_t maximum, bool empty = false)
{
	const auto* value = json_object_get(object, key);
	if (!json_is_string(value) || (!empty && json_string_length(value) == 0U) || json_string_length(value) > maximum) return false;
	const auto* text = json_string_value(value);
	if (std::memchr(text, '\0', json_string_length(value)) != nullptr) return false;
	output.assign(text, json_string_length(value));
	return true;
}

bool read_u64(const json_t* object, const char* key, std::uint64_t& output, bool zero = false)
{
	const auto* value = json_object_get(object, key);
	if (!json_is_integer(value) || json_integer_value(value) < 0 || (!zero && json_integer_value(value) == 0)) return false;
	output = static_cast<std::uint64_t>(json_integer_value(value));
	return true;
}

bool read_u32(const json_t* object, const char* key, std::uint32_t& output, bool zero = false)
{
	std::uint64_t value = 0U;
	if (!read_u64(object, key, value, zero) || value > std::numeric_limits<std::uint32_t>::max()) return false;
	output = static_cast<std::uint32_t>(value);
	return true;
}

protocol::SourceFormat mapping_type(std::string_view text)
{
	if (text == "ANI") return protocol::SourceFormat::Ani;
	if (text == "EFF") return protocol::SourceFormat::Eff;
	if (text == "APNG") return protocol::SourceFormat::Apng;
	return protocol::SourceFormat::Invalid;
}

bool parse_asset(const json_t* object, CommunicationAsset& asset)
{
	std::string hash;
	std::uint32_t source = 0U, delivered = 0U, alpha = 0U, timing = 0U, width = 0U, height = 0U;
	if (!json_is_object(object) || !read_u64(object, "id", asset.asset_id) ||
		!read_string(object, "sha256", hash, 64U) || !parse_digest(hash, asset.content_hash) ||
		asset.asset_id != protocol::comm_asset_id_from_content_hash(asset.content_hash) ||
		!read_u32(object, "sourceFormat", source, true) || !read_u32(object, "deliveredFormat", delivered, true) ||
		!read_u32(object, "alphaMode", alpha, true) || !read_u32(object, "timingMode", timing, true) ||
		!read_u32(object, "width", width) || width > 4096U || !read_u32(object, "height", height) || height > 4096U ||
		!read_u32(object, "frames", asset.frame_count) || asset.frame_count > 65535U ||
		!read_u64(object, "durationUs", asset.duration_us) || asset.duration_us > protocol::MaximumCommDurationUs ||
		!read_string(object, "logicalName", asset.logical_name, protocol::MaximumCommLogicalNameLength) ||
		!read_string(object, "file", asset.file_path, protocol::MaximumCommFilePathLength)) return false;
	asset.source_format = static_cast<protocol::SourceFormat>(source);
	asset.delivered_format = static_cast<protocol::DeliveredFormat>(delivered);
	asset.alpha_mode = static_cast<protocol::AlphaMode>(alpha);
	asset.timing_mode = static_cast<protocol::AssetTimingMode>(timing);
	asset.width = static_cast<std::uint16_t>(width); asset.height = static_cast<std::uint16_t>(height);
	if (source < 1U || source > 4U || (asset.delivered_format != protocol::DeliveredFormat::Apng &&
		asset.delivered_format != protocol::DeliveredFormat::Png) || alpha > 1U || timing > 2U ||
		protocol::validate_portable_bundle_path({reinterpret_cast<const std::uint8_t*>(asset.file_path.data()), asset.file_path.size()}) != protocol::ValidationError::None) return false;
	const auto* durations = json_object_get(object, "frameDurationsUs");
	if (!json_is_array(durations) || json_array_size(durations) > asset.frame_count) return false;
	std::uint64_t sum = 0U;
	for (std::size_t i = 0; i < json_array_size(durations); ++i) {
		const auto* item = json_array_get(durations, i);
		if (!json_is_integer(item) || json_integer_value(item) <= 0 ||
			static_cast<std::uint64_t>(json_integer_value(item)) > UINT32_MAX) return false;
		asset.frame_durations_us.push_back(static_cast<std::uint32_t>(json_integer_value(item)));
		sum += asset.frame_durations_us.back();
	}
	if ((asset.timing_mode == protocol::AssetTimingMode::FormatIntrinsic && !asset.frame_durations_us.empty()) ||
		(asset.timing_mode == protocol::AssetTimingMode::Constant && (asset.frame_durations_us.size() != 1U ||
			static_cast<std::uint64_t>(asset.frame_durations_us.front()) * asset.frame_count != asset.duration_us)) ||
		(asset.timing_mode == protocol::AssetTimingMode::PerFrame &&
			(asset.frame_durations_us.size() != asset.frame_count || sum != asset.duration_us))) return false;
	return true;
}

bool contains_asset(const CommunicationBundle& bundle, std::uint64_t id)
{
	return bundle.find_asset(id) != nullptr;
}

} // namespace

std::string normalize_generic_anim_name(std::string_view name)
{
	std::string result(name);
	std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return result;
}

std::uint64_t CommunicationBundle::resolve(std::string_view final_name, protocol::SourceFormat type) const noexcept
{
	const auto normalized = normalize_generic_anim_name(final_name);
	for (const auto& mapping : mappings)
		if (mapping.generic_anim_type == type && mapping.generic_anim_name == normalized) return mapping.asset_id;
	return 0U;
}

const CommunicationAsset* CommunicationBundle::find_asset(std::uint64_t asset_id) const noexcept
{
	const auto found = std::lower_bound(assets.begin(), assets.end(), asset_id,
		[](const auto& asset, auto id) { return asset.asset_id < id; });
	return found != assets.end() && found->asset_id == asset_id ? &*found : nullptr;
}

CommunicationBundleLoadResult load_communication_bundle(std::string_view root_path) noexcept
{
	CommunicationBundleLoadResult result;
	if (root_path.empty()) return result;
	try {
		std::string manifest_bytes, info_bytes;
		const std::filesystem::path root{std::string(root_path)};
		if (!read_bounded(root / "manifest.json", manifest_bytes) || !read_bounded(root / "bundle-info.json", info_bytes)) {
			result.error = CommunicationBundleError::ReadFailure; return result;
		}
		json_error_t json_error{};
		JsonPtr manifest(json_loadb(manifest_bytes.data(), manifest_bytes.size(), JSON_REJECT_DUPLICATES, &json_error), &json_decref);
		JsonPtr info(json_loadb(info_bytes.data(), info_bytes.size(), JSON_REJECT_DUPLICATES, &json_error), &json_decref);
		if (!manifest || !info || !json_is_object(manifest.get()) || !json_is_object(info.get())) {
			result.error = CommunicationBundleError::InvalidJson; return result;
		}
		std::uint32_t version = 0U;
		if (!read_u32(manifest.get(), "bundleVersion", version) || version != 1U ||
			!read_string(manifest.get(), "converterId", result.bundle.converter_id, 63U) ||
			!read_string(manifest.get(), "converterVersion", result.bundle.converter_version, 63U) ||
			!read_u64(manifest.get(), "frameAssetId", result.bundle.frame_asset_id, true) ||
			!read_u64(manifest.get(), "placeholderAssetId", result.bundle.placeholder_asset_id, true)) {
			result.error = CommunicationBundleError::InvalidManifest; return result;
		}
		const auto* assets = json_object_get(manifest.get(), "assets");
		if (!json_is_array(assets) || json_array_size(assets) == 0U || json_array_size(assets) > protocol::MaximumCommAssetCount) {
			result.error = CommunicationBundleError::LimitExceeded; return result;
		}
		result.bundle.assets.reserve(json_array_size(assets));
		for (std::size_t i = 0; i < json_array_size(assets); ++i) {
			CommunicationAsset asset;
			if (!parse_asset(json_array_get(assets, i), asset)) { result.error = CommunicationBundleError::InvalidManifest; return result; }
			result.bundle.required_delivered_formats |= asset.delivered_format == protocol::DeliveredFormat::Apng
				? protocol::DeliveredFormatBitApng : protocol::DeliveredFormatBitPng;
			result.bundle.assets.push_back(std::move(asset));
		}
		std::sort(result.bundle.assets.begin(), result.bundle.assets.end(), [](const auto& a, const auto& b) { return a.asset_id < b.asset_id; });
		for (std::size_t i = 1; i < result.bundle.assets.size(); ++i)
			if (result.bundle.assets[i - 1].asset_id == result.bundle.assets[i].asset_id) { result.error = CommunicationBundleError::InvalidManifest; return result; }
		if ((result.bundle.frame_asset_id != 0U && !contains_asset(result.bundle, result.bundle.frame_asset_id)) ||
			(result.bundle.placeholder_asset_id != 0U && !contains_asset(result.bundle, result.bundle.placeholder_asset_id))) {
			result.error = CommunicationBundleError::InvalidManifest; return result;
		}
		if (!protocol::sha256({reinterpret_cast<const std::uint8_t*>(manifest_bytes.data()), manifest_bytes.size()}, result.bundle.bundle_hash)) {
			result.error = CommunicationBundleError::InvalidManifest; return result;
		}
		std::string declared_hash;
		protocol::Sha256Digest digest{};
		if (!read_u32(info.get(), "bundleVersion", version) || version != 1U ||
			!read_string(info.get(), "bundleHash", declared_hash, 64U) || !parse_digest(declared_hash, digest) ||
			digest != result.bundle.bundle_hash) { result.error = CommunicationBundleError::HashMismatch; return result; }
		const auto* mappings = json_object_get(info.get(), "mappings");
		if (!json_is_array(mappings) || json_array_size(mappings) > MaximumCommunicationMappings) {
			result.error = CommunicationBundleError::LimitExceeded; return result;
		}
		result.bundle.mappings.reserve(json_array_size(mappings));
		for (std::size_t i = 0; i < json_array_size(mappings); ++i) {
			const auto* item = json_array_get(mappings, i);
			CommunicationMapping mapping;
			std::string type, id;
			if (!json_is_object(item) || !read_string(item, "genericAnimName", mapping.generic_anim_name, 255U) ||
				!read_string(item, "genericAnimType", type, 8U) || !read_string(item, "assetId", id, 16U) ||
				!parse_asset_id(id, mapping.asset_id)) { result.error = CommunicationBundleError::InvalidBundleInfo; return result; }
			mapping.generic_anim_name = normalize_generic_anim_name(mapping.generic_anim_name);
			mapping.generic_anim_type = mapping_type(type);
			if (mapping.generic_anim_type == protocol::SourceFormat::Invalid || !contains_asset(result.bundle, mapping.asset_id)) {
				result.error = CommunicationBundleError::InvalidBundleInfo; return result;
			}
			result.bundle.mappings.push_back(std::move(mapping));
		}
		std::sort(result.bundle.mappings.begin(), result.bundle.mappings.end(), [](const auto& a, const auto& b) {
			return a.generic_anim_name < b.generic_anim_name ||
				(a.generic_anim_name == b.generic_anim_name && a.generic_anim_type < b.generic_anim_type);
		});
		for (std::size_t i = 1; i < result.bundle.mappings.size(); ++i)
			if (result.bundle.mappings[i - 1].generic_anim_name == result.bundle.mappings[i].generic_anim_name &&
				result.bundle.mappings[i - 1].generic_anim_type == result.bundle.mappings[i].generic_anim_type) {
				result.error = CommunicationBundleError::ContradictoryMapping; return result;
			}
		result.error = CommunicationBundleError::None;
		return result;
	} catch (...) {
		result.error = CommunicationBundleError::ReadFailure;
		result.bundle = {};
		return result;
	}
}

} // namespace telemetry::detail
