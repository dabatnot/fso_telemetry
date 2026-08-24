#include "comm_bundle_core.h"

#include "telemetry/protocol/telemetry_protocol_types.h"
#include "telemetry/protocol/telemetry_sha256.h"

#include <zlib.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace comm_bundle {
namespace {

using Bytes = std::vector<std::uint8_t>;

void append_u16_be(Bytes& out, std::uint16_t value)
{
	out.push_back(static_cast<std::uint8_t>(value >> 8U));
	out.push_back(static_cast<std::uint8_t>(value));
}

void append_u32_be(Bytes& out, std::uint32_t value)
{
	out.push_back(static_cast<std::uint8_t>(value >> 24U));
	out.push_back(static_cast<std::uint8_t>(value >> 16U));
	out.push_back(static_cast<std::uint8_t>(value >> 8U));
	out.push_back(static_cast<std::uint8_t>(value));
}

void append_u32_le(Bytes& out, std::uint32_t value)
{
	out.push_back(static_cast<std::uint8_t>(value));
	out.push_back(static_cast<std::uint8_t>(value >> 8U));
	out.push_back(static_cast<std::uint8_t>(value >> 16U));
	out.push_back(static_cast<std::uint8_t>(value >> 24U));
}

void append_chunk(Bytes& out, const char type[5], const Bytes& payload)
{
	if (payload.size() > std::numeric_limits<std::uint32_t>::max() ||
		payload.size() > std::numeric_limits<uInt>::max() - 4ULL) {
		throw std::runtime_error("PNG chunk is too large");
	}
	append_u32_be(out, static_cast<std::uint32_t>(payload.size()));
	const auto type_begin = out.size();
	out.insert(out.end(), type, type + 4);
	out.insert(out.end(), payload.begin(), payload.end());
	const auto crc = ::crc32(0L, reinterpret_cast<const Bytef*>(out.data() + type_begin),
		static_cast<uInt>(4 + payload.size()));
	append_u32_be(out, static_cast<std::uint32_t>(crc));
}

Bytes deflate_bytes(const Bytes& input, int window_bits)
{
	if (input.size() > std::numeric_limits<uInt>::max() || input.size() > std::numeric_limits<uLong>::max()) {
		throw std::runtime_error("compression input is too large for zlib");
	}
	const auto bound = compressBound(static_cast<uLong>(input.size()));
	if (bound > std::numeric_limits<uInt>::max()) {
		throw std::runtime_error("compression output bound is too large for zlib");
	}
	z_stream stream{};
	if (deflateInit2(&stream, 6, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
		throw std::runtime_error("zlib initialization failed");
	}
	Bytes output(std::max<std::size_t>(256, static_cast<std::size_t>(bound)));
	stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
	stream.avail_in = static_cast<uInt>(input.size());
	stream.next_out = reinterpret_cast<Bytef*>(output.data());
	stream.avail_out = static_cast<uInt>(output.size());
	const auto result = deflate(&stream, Z_FINISH);
	if (result != Z_STREAM_END) {
		deflateEnd(&stream);
		throw std::runtime_error("zlib compression failed");
	}
	output.resize(stream.total_out);
	deflateEnd(&stream);
	return output;
}

Bytes scanlines(const Frame& frame)
{
	if (frame.width == 0 || frame.height == 0 || frame.width > 4096 || frame.height > 4096) {
		throw std::runtime_error("frame dimensions are outside the bundle limits");
	}
	const auto pixels = static_cast<std::uint64_t>(frame.width) * frame.height;
	if (pixels > std::numeric_limits<std::size_t>::max() / 4 || frame.rgba.size() != pixels * 4) {
		throw std::runtime_error("frame RGBA buffer has an invalid size");
	}
	Bytes rows;
	rows.reserve(frame.rgba.size() + frame.height);
	for (std::uint32_t y = 0; y < frame.height; ++y) {
		rows.push_back(0); // deterministic PNG filter: None
		const auto offset = static_cast<std::size_t>(y) * frame.width * 4;
		rows.insert(rows.end(), frame.rgba.begin() + offset, frame.rgba.begin() + offset + frame.width * 4);
	}
	return rows;
}

Bytes png_header(std::uint32_t width, std::uint32_t height)
{
	Bytes out{137, 80, 78, 71, 13, 10, 26, 10};
	Bytes ihdr;
	append_u32_be(ihdr, width);
	append_u32_be(ihdr, height);
	ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
	append_chunk(out, "IHDR", ihdr);
	return out;
}

std::string json_escape(const std::string& value)
{
	std::ostringstream out;
	out << '"';
	static constexpr char hex[] = "0123456789abcdef";
	for (const auto raw : value) {
		const auto ch = static_cast<unsigned char>(raw);
		switch (ch) {
		case '"': out << "\\\""; break;
		case '\\': out << "\\\\"; break;
		case '\b': out << "\\b"; break;
		case '\f': out << "\\f"; break;
		case '\n': out << "\\n"; break;
		case '\r': out << "\\r"; break;
		case '\t': out << "\\t"; break;
		default:
			if (ch < 0x20) {
				out << "\\u00" << hex[ch >> 4U] << hex[ch & 0x0fU];
			} else {
				out << static_cast<char>(ch);
			}
		}
	}
	out << '"';
	return out.str();
}

bool valid_utf8(const std::string& value)
{
	for (std::size_t offset = 0; offset < value.size();) {
		const auto first = static_cast<std::uint8_t>(value[offset]);
		if (first <= 0x7f) {
			++offset;
			continue;
		}
		std::size_t continuation_count = 0;
		std::uint32_t codepoint = 0;
		std::uint32_t minimum = 0;
		if ((first & 0xe0U) == 0xc0U) {
			continuation_count = 1;
			codepoint = first & 0x1fU;
			minimum = 0x80;
		} else if ((first & 0xf0U) == 0xe0U) {
			continuation_count = 2;
			codepoint = first & 0x0fU;
			minimum = 0x800;
		} else if ((first & 0xf8U) == 0xf0U) {
			continuation_count = 3;
			codepoint = first & 0x07U;
			minimum = 0x10000;
		} else {
			return false;
		}
		if (offset + continuation_count >= value.size()) return false;
		for (std::size_t index = 1; index <= continuation_count; ++index) {
			const auto next = static_cast<std::uint8_t>(value[offset + index]);
			if ((next & 0xc0U) != 0x80U) return false;
			codepoint = (codepoint << 6U) | (next & 0x3fU);
		}
		if (codepoint < minimum || codepoint > 0x10ffffU ||
			(codepoint >= 0xd800U && codepoint <= 0xdfffU)) return false;
		offset += continuation_count + 1;
	}
	return true;
}

std::array<std::uint8_t, 32> digest_of(const Bytes& bytes)
{
	telemetry::protocol::Sha256Digest digest{};
	if (!telemetry::protocol::sha256({bytes.data(), bytes.size()}, digest)) {
		throw std::runtime_error("SHA-256 calculation failed");
	}
	return digest;
}

std::string digest_hex(const std::array<std::uint8_t, 32>& digest)
{
	static constexpr char hex[] = "0123456789abcdef";
	std::string result;
	result.reserve(64);
	for (const auto byte : digest) {
		result.push_back(hex[byte >> 4U]);
		result.push_back(hex[byte & 0x0fU]);
	}
	return result;
}

std::string id_hex(std::uint64_t id)
{
	static constexpr char hex[] = "0123456789abcdef";
	std::string result(16, '0');
	for (int i = 15; i >= 0; --i) {
		result[static_cast<std::size_t>(i)] = hex[id & 0x0fU];
		id >>= 4U;
	}
	return result;
}

bool ascii_case_less(const std::string& left, const std::string& right)
{
	auto fold = [](unsigned char ch) { return static_cast<unsigned char>(std::tolower(ch)); };
	return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
		[&](char a, char b) {
			const auto fa = fold(static_cast<unsigned char>(a));
			const auto fb = fold(static_cast<unsigned char>(b));
			return fa == fb ? static_cast<unsigned char>(a) < static_cast<unsigned char>(b) : fa < fb;
		});
}

std::string casefold_ascii(std::string value)
{
	for (auto& ch : value) {
		if (ch == '\\') ch = '/';
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	}
	return value;
}

bool reserved_segment(const std::string& segment)
{
	auto stem = casefold_ascii(segment.substr(0, segment.find('.')));
	if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") return true;
	if (stem.size() == 4 && (stem.rfind("com", 0) == 0 || stem.rfind("lpt", 0) == 0) &&
		stem[3] >= '1' && stem[3] <= '9') return true;
	return false;
}

void put_octal(char* destination, std::size_t length, std::uint64_t value)
{
	std::ostringstream text;
	text << std::oct << value;
	const auto encoded = text.str();
	if (encoded.size() + 1 > length) throw std::runtime_error("tar field overflow");
	std::memset(destination, '0', length);
	std::memcpy(destination + length - encoded.size() - 1, encoded.data(), encoded.size());
	destination[length - 1] = '\0';
}

Bytes make_tar(const std::map<std::string, Bytes>& files)
{
	Bytes tar;
	for (const auto& entry : files) {
		if (!portable_bundle_path(entry.first)) {
			throw std::runtime_error("tar path is not portable: " + entry.first);
		}
		std::array<char, 512> header{};
		if (entry.first.size() <= 100) {
			std::memcpy(header.data(), entry.first.data(), entry.first.size());
		} else {
			const auto split = entry.first.rfind('/');
			if (split == std::string::npos || split > 155 || entry.first.size() - split - 1 > 100) {
				throw std::runtime_error("tar path exceeds the ustar name and prefix fields: " + entry.first);
			}
			std::memcpy(header.data(), entry.first.data() + split + 1, entry.first.size() - split - 1);
			std::memcpy(header.data() + 345, entry.first.data(), split);
		}
		put_octal(header.data() + 100, 8, 0644);
		put_octal(header.data() + 108, 8, 0);
		put_octal(header.data() + 116, 8, 0);
		put_octal(header.data() + 124, 12, entry.second.size());
		put_octal(header.data() + 136, 12, 0);
		std::memset(header.data() + 148, ' ', 8);
		header[156] = '0';
		std::memcpy(header.data() + 257, "ustar", 5);
		header[262] = '\0';
		std::memcpy(header.data() + 263, "00", 2);
		std::uint64_t checksum = 0;
		for (const auto byte : header) checksum += static_cast<unsigned char>(byte);
		char checksum_field[8]{};
		std::snprintf(checksum_field, sizeof(checksum_field), "%06llo", static_cast<unsigned long long>(checksum));
		std::memcpy(header.data() + 148, checksum_field, 6);
		header[154] = '\0';
		header[155] = ' ';
		tar.insert(tar.end(), reinterpret_cast<const std::uint8_t*>(header.data()),
			reinterpret_cast<const std::uint8_t*>(header.data() + header.size()));
		tar.insert(tar.end(), entry.second.begin(), entry.second.end());
		while (tar.size() % 512 != 0) tar.push_back(0);
	}
	tar.resize(tar.size() + 1024, 0);
	return tar;
}

Bytes make_gzip(const Bytes& input)
{
	Bytes result{0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff};
	auto compressed = deflate_bytes(input, -MAX_WBITS);
	result.insert(result.end(), compressed.begin(), compressed.end());
	const auto crc = ::crc32(0L, reinterpret_cast<const Bytef*>(input.data()), static_cast<uInt>(input.size()));
	append_u32_le(result, static_cast<std::uint32_t>(crc));
	append_u32_le(result, static_cast<std::uint32_t>(input.size()));
	return result;
}

void write_atomic(const std::filesystem::path& target, const Bytes& bytes)
{
	const auto temporary = target.string() + ".tmp";
	std::error_code error;
	std::filesystem::remove(temporary, error);
	try {
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		if (!output) throw std::runtime_error("cannot create temporary archive");
		output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		if (!output) throw std::runtime_error("cannot write temporary archive");
		output.close();
		if (!output) throw std::runtime_error("cannot close temporary archive");
	} catch (...) {
		std::filesystem::remove(temporary, error);
		throw;
	}
	std::filesystem::rename(temporary, target, error);
	if (error) {
		std::filesystem::remove(temporary);
		throw std::runtime_error("cannot publish archive atomically: " + error.message());
	}
}

Bytes read_file(const std::filesystem::path& path)
{
	std::ifstream input(path, std::ios::binary);
	if (!input) return {};
	return Bytes(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void validate_input(const BundleInput& input)
{
	if (input.mod_signature.empty() || input.mod_signature.size() > 255 || input.source_revision.size() > 64 ||
		!valid_utf8(input.mod_signature) || !valid_utf8(input.source_revision)) {
		throw std::runtime_error("invalid sourceRevision or modSignature length");
	}
	if (input.assets.empty() || input.assets.size() > 4096) {
		throw std::runtime_error("bundle must contain between 1 and 4096 assets");
	}
	std::set<std::uint64_t> ids;
	std::set<std::string> paths;
	for (const auto& asset : input.assets) {
		if (asset.id == 0 || asset.id != asset_id_from_sha256(asset.sha256) || asset.sha256 != digest_of(asset.content)) {
			throw std::runtime_error("asset identity does not match delivered content");
		}
		if (!ids.insert(asset.id).second) throw std::runtime_error("duplicate or colliding asset id");
		if (!portable_bundle_path(asset.file) || !paths.insert(casefold_ascii(asset.file)).second) {
			throw std::runtime_error("non-portable or colliding asset path");
		}
		if (asset.logical_name.empty() || asset.logical_name.size() > 255 || !valid_utf8(asset.logical_name) ||
			asset.width == 0 || asset.height == 0 ||
			asset.width > 4096 || asset.height > 4096 || asset.frames == 0 || asset.frames > 65535 ||
			asset.duration_us == 0 || asset.duration_us > 86'400'000'000ULL) {
			throw std::runtime_error("asset metadata is outside protocol bounds");
		}
		if (asset.timing_mode == TimingMode::FormatIntrinsic && !asset.frame_durations_us.empty()) {
			throw std::runtime_error("intrinsic timing cannot carry explicit durations");
		}
		if (asset.timing_mode == TimingMode::Constant &&
			(asset.frame_durations_us.size() != 1 ||
			 static_cast<std::uint64_t>(asset.frames) * asset.frame_durations_us[0] != asset.duration_us)) {
			throw std::runtime_error("constant timing metadata is inconsistent");
		}
	}
	if ((input.frame_asset_id != 0 && ids.count(input.frame_asset_id) == 0) ||
		(input.placeholder_asset_id != 0 && ids.count(input.placeholder_asset_id) == 0)) {
		throw std::runtime_error("frame or placeholder id is absent from assets");
	}
	for (const auto& mapping : input.mappings) {
		if (mapping.generic_anim_name.empty() || mapping.generic_anim_name.size() >= 256 ||
			!valid_utf8(mapping.generic_anim_name) ||
			(mapping.generic_anim_type != "ANI" && mapping.generic_anim_type != "EFF" && mapping.generic_anim_type != "APNG") ||
			ids.count(mapping.asset_id) == 0) {
			throw std::runtime_error("invalid bundle-info mapping");
		}
	}
	std::set<std::pair<std::string, std::string>> mapping_keys;
	for (const auto& mapping : input.mappings) {
		if (!mapping_keys.emplace(casefold_ascii(mapping.generic_anim_name), mapping.generic_anim_type).second) {
			throw std::runtime_error("duplicate bundle-info mapping");
		}
	}
}

} // namespace

std::vector<std::uint8_t> encode_png(const Frame& frame)
{
	auto output = png_header(frame.width, frame.height);
	append_chunk(output, "IDAT", deflate_bytes(scanlines(frame), MAX_WBITS));
	append_chunk(output, "IEND", {});
	return output;
}

std::vector<std::uint8_t> encode_apng(const std::vector<Frame>& frames)
{
	if (frames.empty() || frames.size() > 65535) throw std::runtime_error("invalid APNG frame count");
	const auto width = frames.front().width;
	const auto height = frames.front().height;
	auto output = png_header(width, height);
	Bytes actl;
	append_u32_be(actl, static_cast<std::uint32_t>(frames.size()));
	append_u32_be(actl, 0);
	append_chunk(output, "acTL", actl);
	std::uint32_t sequence = 0;
	for (std::size_t index = 0; index < frames.size(); ++index) {
		const auto& frame = frames[index];
		if (frame.width != width || frame.height != height || frame.delay_num == 0 || frame.delay_den == 0) {
			throw std::runtime_error("APNG frames must share dimensions and have positive rational delays");
		}
		Bytes fctl;
		append_u32_be(fctl, sequence++);
		append_u32_be(fctl, width);
		append_u32_be(fctl, height);
		append_u32_be(fctl, 0);
		append_u32_be(fctl, 0);
		append_u16_be(fctl, frame.delay_num);
		append_u16_be(fctl, frame.delay_den);
		fctl.push_back(0); // APNG_DISPOSE_OP_NONE
		fctl.push_back(0); // APNG_BLEND_OP_SOURCE
		append_chunk(output, "fcTL", fctl);
		auto compressed = deflate_bytes(scanlines(frame), MAX_WBITS);
		if (index == 0) {
			append_chunk(output, "IDAT", compressed);
		} else {
			Bytes fdat;
			append_u32_be(fdat, sequence++);
			fdat.insert(fdat.end(), compressed.begin(), compressed.end());
			append_chunk(output, "fdAT", fdat);
		}
	}
	append_chunk(output, "IEND", {});
	return output;
}

std::uint64_t duration_us(const std::vector<Frame>& frames)
{
	if (frames.empty()) throw std::runtime_error("animation has no frames");
	auto checked_multiply = [](std::uint64_t left, std::uint64_t right) {
		if (right != 0 && left > std::numeric_limits<std::uint64_t>::max() / right) {
			throw std::runtime_error("animation timing fraction is not representable");
		}
		return left * right;
	};
	std::uint64_t numerator = 0;
	std::uint64_t denominator = 1;
	for (const auto& frame : frames) {
		if (frame.delay_num == 0 || frame.delay_den == 0) throw std::runtime_error("frame delay is zero");
		const auto common = std::gcd(denominator, static_cast<std::uint64_t>(frame.delay_den));
		const auto left_multiplier = static_cast<std::uint64_t>(frame.delay_den) / common;
		const auto right_multiplier = denominator / common;
		const auto left_term = checked_multiply(numerator, left_multiplier);
		const auto right_term = checked_multiply(checked_multiply(frame.delay_num, 1'000'000), right_multiplier);
		if (left_term > std::numeric_limits<std::uint64_t>::max() - right_term) {
			throw std::runtime_error("animation timing fraction is not representable");
		}
		numerator = left_term + right_term;
		denominator = checked_multiply(denominator, left_multiplier);
		const auto reduction = std::gcd(numerator, denominator);
		numerator /= reduction;
		denominator /= reduction;
	}
	const auto quotient = numerator / denominator;
	const auto remainder = numerator % denominator;
	const auto rounded = quotient + (remainder >= denominator / 2 + denominator % 2 ? 1U : 0U);
	if (rounded == 0) throw std::runtime_error("animation duration rounds to zero microseconds");
	return rounded;
}

Asset make_animation_asset(std::string logical_name, SourceFormat source_format, const std::vector<Frame>& frames)
{
	Asset asset;
	asset.logical_name = std::move(logical_name);
	asset.source_format = source_format;
	asset.delivered_format = DeliveredFormat::Apng;
	asset.timing_mode = TimingMode::FormatIntrinsic;
	asset.width = frames.empty() ? 0 : frames.front().width;
	asset.height = frames.empty() ? 0 : frames.front().height;
	asset.frames = static_cast<std::uint32_t>(frames.size());
	asset.duration_us = duration_us(frames);
	asset.alpha_mode = AlphaMode::None;
	for (const auto& frame : frames) {
		for (std::size_t i = 3; i < frame.rgba.size(); i += 4) {
			if (frame.rgba[i] != 255) asset.alpha_mode = AlphaMode::Straight;
		}
	}
	asset.content = encode_apng(frames);
	asset.sha256 = digest_of(asset.content);
	asset.id = asset_id_from_sha256(asset.sha256);
	asset.file = "communication/animations/" + digest_hex(asset.sha256) + ".png";
	return asset;
}

Asset make_static_asset(std::string logical_name, const Frame& frame, const std::string& category)
{
	if (category != "frames" && category != "placeholder") throw std::runtime_error("invalid static asset category");
	Asset asset;
	asset.logical_name = std::move(logical_name);
	asset.source_format = SourceFormat::StaticImage;
	asset.delivered_format = DeliveredFormat::Png;
	asset.timing_mode = TimingMode::Constant;
	asset.width = frame.width;
	asset.height = frame.height;
	asset.frames = 1;
	asset.duration_us = 1;
	asset.frame_durations_us = {1};
	asset.alpha_mode = AlphaMode::None;
	for (std::size_t i = 3; i < frame.rgba.size(); i += 4) {
		if (frame.rgba[i] != 255) asset.alpha_mode = AlphaMode::Straight;
	}
	asset.content = encode_png(frame);
	asset.sha256 = digest_of(asset.content);
	asset.id = asset_id_from_sha256(asset.sha256);
	asset.file = "communication/" + category + "/" + digest_hex(asset.sha256) + ".png";
	return asset;
}

std::string canonical_manifest(const BundleInput& input)
{
	auto assets = input.assets;
	std::sort(assets.begin(), assets.end(), [](const Asset& a, const Asset& b) {
		return a.id != b.id ? a.id < b.id : a.logical_name < b.logical_name;
	});
	std::ostringstream out;
	// Keys are emitted in UTF-8 byte order as required by the canonical schema.
	out << "{\"assets\":[";
	for (std::size_t index = 0; index < assets.size(); ++index) {
		if (index != 0) out << ',';
		const auto& asset = assets[index];
		out << "{\"alphaMode\":" << static_cast<unsigned>(asset.alpha_mode)
			<< ",\"deliveredFormat\":" << static_cast<unsigned>(asset.delivered_format)
			<< ",\"durationUs\":" << asset.duration_us
			<< ",\"file\":" << json_escape(asset.file)
			<< ",\"frameDurationsUs\":[";
		for (std::size_t duration = 0; duration < asset.frame_durations_us.size(); ++duration) {
			if (duration != 0) out << ',';
			out << asset.frame_durations_us[duration];
		}
		out << "],\"frames\":" << asset.frames
			<< ",\"height\":" << asset.height
			<< ",\"id\":" << asset.id
			<< ",\"logicalName\":" << json_escape(asset.logical_name)
			<< ",\"sha256\":" << json_escape(digest_hex(asset.sha256))
			<< ",\"sourceFormat\":" << static_cast<unsigned>(asset.source_format)
			<< ",\"timingMode\":" << static_cast<unsigned>(asset.timing_mode)
			<< ",\"width\":" << asset.width << '}';
	}
	out << "],\"bundleVersion\":1"
		<< ",\"converterId\":" << json_escape(ConverterId)
		<< ",\"converterVersion\":" << json_escape(ConverterVersion)
		<< ",\"frameAssetId\":" << input.frame_asset_id
		<< ",\"modSignature\":" << json_escape(input.mod_signature)
		<< ",\"placeholderAssetId\":" << input.placeholder_asset_id
		<< ",\"sourceRevision\":" << json_escape(input.source_revision) << '}';
	return out.str();
}

std::string bundle_info_json(const BundleInput& input, const std::string& bundle_hash)
{
	auto mappings = input.mappings;
	std::sort(mappings.begin(), mappings.end(), [](const Mapping& a, const Mapping& b) {
		if (casefold_ascii(a.generic_anim_name) != casefold_ascii(b.generic_anim_name))
			return ascii_case_less(a.generic_anim_name, b.generic_anim_name);
		return a.generic_anim_type < b.generic_anim_type;
	});
	std::ostringstream out;
	out << "{\"bundleHash\":" << json_escape(bundle_hash) << ",\"bundleVersion\":1,\"mappings\":[";
	for (std::size_t index = 0; index < mappings.size(); ++index) {
		if (index != 0) out << ',';
		const auto& mapping = mappings[index];
		out << "{\"assetId\":" << json_escape(id_hex(mapping.asset_id))
			<< ",\"genericAnimName\":" << json_escape(mapping.generic_anim_name)
			<< ",\"genericAnimType\":" << json_escape(mapping.generic_anim_type) << '}';
	}
	out << "]}";
	return out.str();
}

std::string sha256_hex(const std::vector<std::uint8_t>& bytes) { return digest_hex(digest_of(bytes)); }

std::uint64_t asset_id_from_sha256(const std::array<std::uint8_t, 32>& digest)
{
	std::uint64_t id = 0;
	for (std::size_t index = 0; index < 8; ++index) id = (id << 8U) | digest[index];
	return id;
}

bool portable_bundle_path(const std::string& path)
{
	if (path.empty() || path.size() > 1024 || path.front() == '/' || path.back() == '/') return false;
	std::size_t start = 0;
	while (start < path.size()) {
		const auto end = path.find('/', start);
		const auto length = (end == std::string::npos ? path.size() : end) - start;
		if (length == 0 || length > 255) return false;
		const auto segment = path.substr(start, length);
		if (segment.front() == '.' || segment.back() == '.' || segment.front() == ' ' || segment.back() == ' ' ||
			reserved_segment(segment)) return false;
		for (std::size_t index = 0; index < segment.size(); ++index) {
			const auto ch = static_cast<unsigned char>(segment[index]);
			const bool edge = index == 0 || index + 1 == segment.size();
			const bool ascii_alnum = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
				(ch >= '0' && ch <= '9');
			const bool allowed = ascii_alnum || ch == '_' || ch == '-' || (!edge && ch == '.');
			if (!allowed) return false;
		}
		if (end == std::string::npos) break;
		start = end + 1;
	}
	return true;
}

BundleResult write_bundle(BundleInput input, const std::filesystem::path& output_directory)
{
	// Canonicalize same-content aliases before validating the public manifest.
	std::sort(input.assets.begin(), input.assets.end(), [](const Asset& a, const Asset& b) {
		if (a.id != b.id) return a.id < b.id;
		return a.logical_name < b.logical_name;
	});
	input.assets.erase(std::unique(input.assets.begin(), input.assets.end(), [](const Asset& a, const Asset& b) {
		return a.sha256 == b.sha256 && a.content == b.content;
	}), input.assets.end());
	validate_input(input);
	const auto manifest = canonical_manifest(input);
	const Bytes manifest_bytes(manifest.begin(), manifest.end());
	const auto hash = sha256_hex(manifest_bytes);
	const auto info = bundle_info_json(input, hash);
	const auto root = "communication-bundle-" + hash;
	std::map<std::string, Bytes> files;
	files.emplace(root + "/manifest.json", manifest_bytes);
	files.emplace(root + "/bundle-info.json", Bytes(info.begin(), info.end()));
	for (const auto& asset : input.assets) files.emplace(root + "/" + asset.file, asset.content);
	const auto archive = make_gzip(make_tar(files));
	std::filesystem::create_directories(output_directory);
	const auto target = output_directory / (root + ".tar.gz");
	if (std::filesystem::exists(target)) {
		if (read_file(target) != archive) throw std::runtime_error("target archive exists with different bytes");
	} else {
		write_atomic(target, archive);
	}
	return {hash, target, input.assets.size()};
}

} // namespace comm_bundle
