#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace comm_bundle {

constexpr std::uint16_t BundleVersion = 1;
constexpr const char* ConverterId = "fs2open-comm-bundle";
constexpr const char* ConverterVersion = "1.0.0";

enum class SourceFormat : std::uint8_t {
	Ani = 1,
	Eff = 2,
	Apng = 3,
	StaticImage = 4,
};

enum class DeliveredFormat : std::uint8_t {
	Apng = 3,
	Png = 6,
};

enum class AlphaMode : std::uint8_t {
	None = 0,
	Straight = 1,
};

enum class TimingMode : std::uint8_t {
	FormatIntrinsic = 0,
	Constant = 1,
};

struct Frame {
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint16_t delay_num = 1;
	std::uint16_t delay_den = 1;
	std::vector<std::uint8_t> rgba;
};

struct Asset {
	std::uint64_t id = 0;
	std::array<std::uint8_t, 32> sha256{};
	std::string logical_name;
	SourceFormat source_format = SourceFormat::Ani;
	DeliveredFormat delivered_format = DeliveredFormat::Apng;
	std::string file;
	AlphaMode alpha_mode = AlphaMode::None;
	TimingMode timing_mode = TimingMode::FormatIntrinsic;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint32_t frames = 0;
	std::uint64_t duration_us = 0;
	std::vector<std::uint32_t> frame_durations_us;
	std::vector<std::uint8_t> content;
};

struct Mapping {
	std::string generic_anim_name;
	std::string generic_anim_type;
	std::uint64_t asset_id = 0;
};

struct BundleInput {
	std::string source_revision;
	std::string mod_signature;
	std::vector<Asset> assets;
	std::vector<Mapping> mappings;
	std::uint64_t frame_asset_id = 0;
	std::uint64_t placeholder_asset_id = 0;
};

struct BundleResult {
	std::string bundle_hash;
	std::filesystem::path archive_path;
	std::size_t asset_count = 0;
};

std::vector<std::uint8_t> encode_png(const Frame& frame);
std::vector<std::uint8_t> encode_apng(const std::vector<Frame>& frames);
std::uint64_t duration_us(const std::vector<Frame>& frames);
Asset make_animation_asset(std::string logical_name, SourceFormat source_format, const std::vector<Frame>& frames);
Asset make_static_asset(std::string logical_name, const Frame& frame, const std::string& category);

std::string canonical_manifest(const BundleInput& input);
std::string bundle_info_json(const BundleInput& input, const std::string& bundle_hash);
std::string sha256_hex(const std::vector<std::uint8_t>& bytes);
std::uint64_t asset_id_from_sha256(const std::array<std::uint8_t, 32>& digest);
bool portable_bundle_path(const std::string& path);

BundleResult write_bundle(BundleInput input, const std::filesystem::path& output_directory);

} // namespace comm_bundle
