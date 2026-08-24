#include "comm_bundle_core.h"

#include "anim/animplay.h"
#include "anim/packunpack.h"
#include "bmpman/bmpman.h"
#include "cfile/cfile.h"
#include "cmdline/cmdline.h"
#include "ddsutils/ddsutils.h"
#include "jpgutils/jpgutils.h"
#include "mission/messageheadvariants.h"
#include "pcxutils/pcxutils.h"
#include "pngutils/pngutils.h"
#include "tgautils/tgautils.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Options {
	std::filesystem::path fs2_root;
	std::string mods;
	std::vector<std::string> missions;
	bool all_missions = false;
	std::filesystem::path output;
	std::string mod_signature;
	std::string source_revision;
	std::string frame;
	std::string placeholder;
};

struct HeadReference {
	std::string name;
	bool builtin = false;
};

std::string trim(std::string value)
{
	auto whitespace = [](unsigned char ch) { return std::isspace(ch) != 0; };
	value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), whitespace));
	value.erase(std::find_if_not(value.rbegin(), value.rend(), whitespace).base(), value.end());
	if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
		value = value.substr(1, value.size() - 2);
	}
	return value;
}

std::string lower_ascii(std::string value)
{
	for (auto& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	return value;
}

bool starts_with_ci(const std::string& value, const std::string& prefix)
{
	return value.size() >= prefix.size() && lower_ascii(value.substr(0, prefix.size())) == lower_ascii(prefix);
}

std::string strip_known_extension(std::string value)
{
	const auto extension = lower_ascii(std::filesystem::path(value).extension().string());
	if (extension == ".ani" || extension == ".eff" || extension == ".png" || extension == ".jpg" ||
		extension == ".pcx" || extension == ".tga" || extension == ".dds" || extension == ".ktx") {
		value.resize(value.size() - extension.size());
	}
	return value;
}

void print_usage()
{
	std::cout
		<< "Usage: comm_bundle_packager --fs2-root <path> [--mod <a,b>] "
		   "(--mission <name> ... | --all-missions) --output <directory>\n"
		   "       [--mod-signature <text>] [--source-revision <text>]\n"
		   "       [--frame <CFile asset>] [--placeholder <CFile asset>]\n";
}

Options parse_options(int argc, char** argv)
{
	Options options;
	for (int index = 1; index < argc; ++index) {
		const std::string argument = argv[index];
		auto value = [&]() -> std::string {
			if (++index >= argc) throw std::runtime_error("missing value after " + argument);
			return argv[index];
		};
		if (argument == "--fs2-root") options.fs2_root = value();
		else if (argument == "--mod") options.mods = value();
		else if (argument == "--mission") options.missions.push_back(value());
		else if (argument == "--all-missions") options.all_missions = true;
		else if (argument == "--output") options.output = value();
		else if (argument == "--mod-signature") options.mod_signature = value();
		else if (argument == "--source-revision") options.source_revision = value();
		else if (argument == "--frame") options.frame = value();
		else if (argument == "--placeholder") options.placeholder = value();
		else if (argument == "--help" || argument == "-h") {
			print_usage();
			std::exit(0);
		} else {
			throw std::runtime_error("unknown argument: " + argument);
		}
	}
	if (options.fs2_root.empty() || options.output.empty()) {
		throw std::runtime_error("--fs2-root and --output are required");
	}
	if (options.all_missions == !options.missions.empty()) {
		throw std::runtime_error("choose either one or more --mission options or --all-missions");
	}
	if (options.mod_signature.empty()) options.mod_signature = options.mods.empty() ? "base" : options.mods;
	if (options.mod_signature.size() > 255) {
		throw std::runtime_error("derived mod signature is too long; pass --mod-signature explicitly");
	}
	return options;
}

std::vector<char> make_mod_list(const std::string& mods)
{
	if (mods.empty()) return {};
	std::vector<char> result(mods.begin(), mods.end());
	result.push_back('\0');
	result.push_back('\0');
	for (std::size_t index = 0; index + 1 < result.size(); ++index) {
		if (result[index] == ',') result[index] = '\0';
	}
	return result;
}

std::string read_cfile(const std::string& filename, int type)
{
	auto* file = cfopen(filename.c_str(), "rb", type);
	if (file == nullptr) throw std::runtime_error("CFile cannot open " + filename);
	const auto length = cfilelength(file);
	if (length < 0) {
		cfclose(file);
		throw std::runtime_error("CFile returned an invalid size for " + filename);
	}
	std::string content(static_cast<std::size_t>(length), '\0');
	if (length != 0 && cfread(content.data(), 1, static_cast<std::size_t>(length), file) != static_cast<std::size_t>(length)) {
		cfclose(file);
		throw std::runtime_error("CFile cannot read " + filename);
	}
	cfclose(file);
	return content;
}

std::vector<HeadReference> scan_avi_names(const std::string& content, bool table)
{
	std::vector<HeadReference> result;
	bool in_messages = table;
	std::size_t position = 0;
	while (position <= content.size()) {
		const auto end = content.find('\n', position);
		auto line = trim(content.substr(position, end == std::string::npos ? std::string::npos : end - position));
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (!table && starts_with_ci(line, "#Messages")) in_messages = true;
		else if (!table && in_messages && starts_with_ci(line, "#Reinforcements")) in_messages = false;
		else if (in_messages && starts_with_ci(line, "+AVI Name:")) {
			auto name = trim(line.substr(line.find(':') + 1));
			const auto comment = name.find(';');
			if (comment != std::string::npos) name = trim(name.substr(0, comment));
			if (!name.empty() && lower_ascii(name) != "none") result.push_back({name, table});
		}
		if (end == std::string::npos) break;
		position = end + 1;
	}
	return result;
}

std::vector<std::string> mission_names(const Options& options)
{
	if (!options.all_missions) {
		auto names = options.missions;
		for (auto& name : names) {
			if (lower_ascii(std::filesystem::path(name).extension().string()) != ".fs2") name += ".fs2";
		}
		return names;
	}
	SCP_vector<SCP_string> visible;
	cf_get_file_list(visible, CF_TYPE_MISSIONS, "*.fs2", CF_SORT_NAME);
	std::vector<std::string> names;
	for (const auto& name : visible) {
		auto value = std::string(name.c_str());
		if (lower_ascii(std::filesystem::path(value).extension().string()) != ".fs2") value += ".fs2";
		names.push_back(std::move(value));
	}
	if (names.empty()) throw std::runtime_error("--all-missions found no visible .fs2 mission");
	return names;
}

std::vector<HeadReference> collect_references(const Options& options)
{
	std::vector<HeadReference> references;
	for (const auto& mission : mission_names(options)) {
		auto found = scan_avi_names(read_cfile(mission, CF_TYPE_MISSIONS), false);
		references.insert(references.end(), found.begin(), found.end());
	}
	if (cf_exists_full("messages.tbl", CF_TYPE_TABLES)) {
		auto found = scan_avi_names(read_cfile("messages.tbl", CF_TYPE_TABLES), true);
		references.insert(references.end(), found.begin(), found.end());
	}
	SCP_vector<SCP_string> modular_tables;
	cf_get_file_list(modular_tables, CF_TYPE_TABLES, "*-msg.tbm", CF_SORT_NAME);
	for (const auto& table : modular_tables) {
		auto name = std::string(table.c_str());
		if (lower_ascii(std::filesystem::path(name).extension().string()) != ".tbm") name += ".tbm";
		auto found = scan_avi_names(read_cfile(name, CF_TYPE_TABLES), true);
		references.insert(references.end(), found.begin(), found.end());
	}
	std::sort(references.begin(), references.end(), [](const HeadReference& a, const HeadReference& b) {
		return lower_ascii(a.name) < lower_ascii(b.name);
	});
	references.erase(std::unique(references.begin(), references.end(), [](const HeadReference& a, const HeadReference& b) {
		return lower_ascii(a.name) == lower_ascii(b.name) && a.builtin == b.builtin;
	}), references.end());
	return references;
}

std::pair<std::string, BM_TYPE> resolve_animation(const std::string& candidate)
{
	const auto stem = strip_known_extension(candidate);
	const auto location = cf_find_file_location_ext(stem.c_str(), BM_ANI_NUM_TYPES, bm_ani_ext_list, CF_TYPE_ANY);
	if (!location.found) return {{}, BM_TYPE_NONE};
	return {stem + bm_ani_ext_list[location.extension_index], bm_ani_type_list[location.extension_index]};
}

std::vector<std::pair<std::string, BM_TYPE>> resolve_variants(const HeadReference& reference)
{
	const auto stem = strip_known_extension(reference.name);
	if (const auto direct = resolve_animation(stem); direct.second != BM_TYPE_NONE) return {direct};
	const auto head_prefixed = starts_with_ci(stem, "head-");
	if (!reference.builtin && !head_prefixed) return {};
	std::vector<std::pair<std::string, BM_TYPE>> resolved;
	for (const auto& candidate : enumerate_message_head_variants(stem, false, true)) {
		auto value = resolve_animation(candidate.c_str());
		if (value.second != BM_TYPE_NONE) resolved.push_back(std::move(value));
	}
	return resolved;
}

comm_bundle::Frame bgra_to_frame(std::uint32_t width,
	std::uint32_t height,
	const std::uint8_t* source,
	std::size_t bytes_per_pixel)
{
	if (bytes_per_pixel != 3 && bytes_per_pixel != 4) throw std::runtime_error("unsupported decoded pixel size");
	comm_bundle::Frame frame;
	frame.width = width;
	frame.height = height;
	frame.rgba.resize(static_cast<std::size_t>(width) * height * 4);
	for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(width) * height; ++pixel) {
		frame.rgba[pixel * 4] = source[pixel * bytes_per_pixel + 2];
		frame.rgba[pixel * 4 + 1] = source[pixel * bytes_per_pixel + 1];
		frame.rgba[pixel * 4 + 2] = source[pixel * bytes_per_pixel];
		frame.rgba[pixel * 4 + 3] = bytes_per_pixel == 4 ? source[pixel * 4 + 3] : 255;
	}
	return frame;
}

void validate_image_dimensions(int width, int height, const std::string& filename)
{
	if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
		throw std::runtime_error("image dimensions are outside bundle limits: " + filename);
	}
}

comm_bundle::Frame read_image(const std::string& filename, BM_TYPE type)
{
	int width = 0;
	int height = 0;
	int bpp = 0;
	int result = 1;
	std::vector<std::uint8_t> decoded;
	if (type == BM_TYPE_PNG) {
		if (png_read_header(filename.c_str(), nullptr, &width, &height, &bpp) != PNG_ERROR_NONE) throw std::runtime_error("invalid PNG header: " + filename);
		validate_image_dimensions(width, height, filename);
		const auto bytes = static_cast<std::size_t>(bpp) / 8;
		decoded.resize(static_cast<std::size_t>(width) * height * bytes);
		result = png_read_bitmap(filename.c_str(), decoded.data(), &bpp, static_cast<int>(bytes), CF_TYPE_ANY);
		if (result != PNG_ERROR_NONE) throw std::runtime_error("cannot decode PNG: " + filename);
		return bgra_to_frame(width, height, decoded.data(), static_cast<std::size_t>(bpp) / 8);
	}
	if (type == BM_TYPE_JPG) {
		if (jpeg_read_header(filename.c_str(), nullptr, &width, &height, &bpp) != JPEG_ERROR_NONE) throw std::runtime_error("invalid JPEG header: " + filename);
		validate_image_dimensions(width, height, filename);
		decoded.resize(static_cast<std::size_t>(width) * height * 3);
		result = jpeg_read_bitmap(filename.c_str(), decoded.data(), nullptr, 3, CF_TYPE_ANY);
		if (result != JPEG_ERROR_NONE) throw std::runtime_error("cannot decode JPEG: " + filename);
		return bgra_to_frame(width, height, decoded.data(), 3);
	}
	if (type == BM_TYPE_TGA) {
		if (targa_read_header(filename.c_str(), nullptr, &width, &height, &bpp) != TARGA_ERROR_NONE) throw std::runtime_error("invalid TGA header: " + filename);
		validate_image_dimensions(width, height, filename);
		const auto bytes = bpp == 32 ? 4U : 3U;
		decoded.resize(static_cast<std::size_t>(width) * height * bytes);
		result = targa_read_bitmap(filename.c_str(), decoded.data(), nullptr, static_cast<int>(bytes), CF_TYPE_ANY);
		if (result != TARGA_ERROR_NONE) throw std::runtime_error("cannot decode TGA: " + filename);
		return bgra_to_frame(width, height, decoded.data(), bytes);
	}
	if (type == BM_TYPE_PCX) {
		if (pcx_read_header(filename.c_str(), nullptr, &width, &height, &bpp) != PCX_ERROR_NONE) throw std::runtime_error("invalid PCX header: " + filename);
		validate_image_dimensions(width, height, filename);
		decoded.resize(static_cast<std::size_t>(width) * height * 4);
		result = pcx_read_bitmap(filename.c_str(), decoded.data(), nullptr, 4, 0, false, CF_TYPE_ANY);
		if (result != PCX_ERROR_NONE) throw std::runtime_error("cannot decode PCX: " + filename);
		return bgra_to_frame(width, height, decoded.data(), 4);
	}
	if (type == BM_TYPE_DDS) {
		int compression = 0;
		int levels = 0;
		size_t encoded_size = 0;
		if (dds_read_header(filename.c_str(), nullptr, &width, &height, &bpp, &compression, &levels, &encoded_size) != DDS_ERROR_NONE) throw std::runtime_error("invalid DDS header: " + filename);
		validate_image_dimensions(width, height, filename);
		const auto first_level = static_cast<std::size_t>(width) * height * 4;
		if (encoded_size > std::numeric_limits<std::size_t>::max() / 8 ||
			first_level > std::numeric_limits<std::size_t>::max() / 2) {
			throw std::runtime_error("DDS working buffer is too large: " + filename);
		}
		decoded.resize(std::max(encoded_size * 8, first_level * 2));
		ubyte decoded_bpp = 0;
		result = dds_read_bitmap(filename.c_str(), decoded.data(), &decoded_bpp, CF_TYPE_ANY);
		if (result != DDS_ERROR_NONE) throw std::runtime_error("cannot decode DDS: " + filename);
		const auto bytes = decoded_bpp == 24 ? 3U : 4U;
		return bgra_to_frame(width, height, decoded.data(), bytes);
	}
	throw std::runtime_error("unsupported EFF/static frame type: " + filename);
}

std::vector<comm_bundle::Frame> decode_ani(const std::string& filename)
{
	auto* animation = anim_load(filename.c_str(), CF_TYPE_ANY);
	if (animation == nullptr) throw std::runtime_error("cannot load ANI: " + filename);
	auto* instance = init_anim_instance(animation, 32);
	if (instance == nullptr) {
		anim_free(animation);
		throw std::runtime_error("cannot allocate ANI decoder: " + filename);
	}
	std::vector<comm_bundle::Frame> frames;
	try {
		if (animation->fps <= 0 || animation->fps > 65535 || animation->total_frames <= 0 || animation->total_frames > 65535) {
			throw std::runtime_error("ANI timing or frame count is outside bundle limits: " + filename);
		}
		frames.reserve(static_cast<std::size_t>(animation->total_frames));
		for (int index = 0; index < animation->total_frames; ++index) {
			auto* pixels = anim_get_next_raw_buffer(instance, 0, 0, 32);
			if (pixels == nullptr) throw std::runtime_error("ANI frame decoding failed: " + filename);
			auto frame = bgra_to_frame(animation->width, animation->height, pixels, 4);
			frame.delay_num = 1;
			frame.delay_den = static_cast<std::uint16_t>(animation->fps);
			frames.push_back(std::move(frame));
		}
	} catch (...) {
		free_anim_instance(instance);
		anim_free(animation);
		throw;
	}
	free_anim_instance(instance);
	anim_free(animation);
	return frames;
}

std::vector<comm_bundle::Frame> decode_eff(const std::string& filename)
{
	int count = 0;
	int fps = 0;
	int keyframe = 0;
	BM_TYPE type = BM_TYPE_NONE;
	if (!bm_load_and_parse_eff(filename.c_str(), CF_TYPE_ANY, &count, &fps, &keyframe, &type) ||
		count <= 0 || count > 65535 || fps <= 0 || fps > 65535) {
		throw std::runtime_error("invalid EFF descriptor: " + filename);
	}
	auto stem = strip_known_extension(filename);
	std::vector<comm_bundle::Frame> frames;
	frames.reserve(static_cast<std::size_t>(count));
	for (int index = 0; index < count; ++index) {
		char suffix[16];
		std::snprintf(suffix, sizeof(suffix), "_%04d", index);
		auto frame = read_image(stem + suffix, type);
		frame.delay_num = 1;
		frame.delay_den = static_cast<std::uint16_t>(fps);
		frames.push_back(std::move(frame));
	}
	return frames;
}

std::vector<comm_bundle::Frame> decode_apng(const std::string& filename)
{
	apng::apng_ani animation(filename.c_str(), true);
	if (animation.nframes == 0 || animation.nframes > 65535) throw std::runtime_error("invalid APNG frame count: " + filename);
	std::vector<comm_bundle::Frame> frames;
	frames.reserve(animation.nframes);
	animation.goto_start();
	for (std::uint32_t index = 0; index < animation.nframes; ++index) {
		animation.next_frame();
		auto frame = bgra_to_frame(animation.w, animation.h, animation.frame.data.data(), 4);
		frame.delay_num = animation.frame.delay_num;
		frame.delay_den = animation.frame.delay_den;
		frames.push_back(std::move(frame));
	}
	return frames;
}

std::vector<comm_bundle::Frame> decode_animation(const std::string& filename, BM_TYPE type)
{
	if (type == BM_TYPE_ANI) return decode_ani(filename);
	if (type == BM_TYPE_EFF) return decode_eff(filename);
	if (type == BM_TYPE_PNG) return decode_apng(filename);
	throw std::runtime_error("unsupported animation type: " + filename);
}

comm_bundle::SourceFormat source_format(BM_TYPE type)
{
	if (type == BM_TYPE_ANI) return comm_bundle::SourceFormat::Ani;
	if (type == BM_TYPE_EFF) return comm_bundle::SourceFormat::Eff;
	if (type == BM_TYPE_PNG) return comm_bundle::SourceFormat::Apng;
	throw std::runtime_error("invalid animation source type");
}

std::string source_type_name(BM_TYPE type)
{
	if (type == BM_TYPE_ANI) return "ANI";
	if (type == BM_TYPE_EFF) return "EFF";
	if (type == BM_TYPE_PNG) return "APNG";
	throw std::runtime_error("invalid animation source type");
}

std::pair<std::string, BM_TYPE> resolve_static(const std::string& requested)
{
	const auto stem = strip_known_extension(requested);
	const auto location = cf_find_file_location_ext(stem.c_str(), BM_NUM_TYPES, bm_ext_list, CF_TYPE_ANY);
	if (!location.found) throw std::runtime_error("static CFile asset is missing: " + requested);
	const auto type = bm_type_list[location.extension_index];
	if (type == BM_TYPE_KTX) throw std::runtime_error("KTX static assets are not supported by bundle v1: " + requested);
	return {stem + bm_ext_list[location.extension_index], type};
}

comm_bundle::BundleInput build_input(const Options& options)
{
	comm_bundle::BundleInput input;
	input.source_revision = options.source_revision;
	input.mod_signature = options.mod_signature;
	std::set<std::pair<std::string, int>> resolved_sources;
	for (const auto& reference : collect_references(options)) {
		const auto variants = resolve_variants(reference);
		if (variants.empty()) {
			std::cerr << "warning: no visible Talking Head for " << reference.name << '\n';
			continue;
		}
		for (const auto& resolved : variants) {
			const auto key = std::make_pair(lower_ascii(resolved.first), static_cast<int>(resolved.second));
			if (!resolved_sources.insert(key).second) continue;
			auto asset = comm_bundle::make_animation_asset(resolved.first,
				source_format(resolved.second), decode_animation(resolved.first, resolved.second));
			input.mappings.push_back({resolved.first, source_type_name(resolved.second), asset.id});
			input.assets.push_back(std::move(asset));
		}
	}
	if (!options.frame.empty()) {
		const auto resolved = resolve_static(options.frame);
		auto asset = comm_bundle::make_static_asset(resolved.first, read_image(resolved.first, resolved.second), "frames");
		input.frame_asset_id = asset.id;
		input.assets.push_back(std::move(asset));
	}
	if (!options.placeholder.empty()) {
		const auto resolved = resolve_static(options.placeholder);
		auto asset = comm_bundle::make_static_asset(resolved.first, read_image(resolved.first, resolved.second), "placeholder");
		input.placeholder_asset_id = asset.id;
		input.assets.push_back(std::move(asset));
	}
	if (input.assets.empty()) throw std::runtime_error("mission scope resolved no bundle asset");
	return input;
}

} // namespace

int main(int argc, char** argv)
{
	try {
		auto options = parse_options(argc, argv);
		options.fs2_root = std::filesystem::absolute(options.fs2_root).lexically_normal();
		options.output = std::filesystem::absolute(options.output).lexically_normal();
		if (!std::filesystem::is_directory(options.fs2_root)) throw std::runtime_error("--fs2-root is not a directory");
		auto mod_list = make_mod_list(options.mods);
		Cmdline_mod = mod_list.empty() ? nullptr : mod_list.data();
		const auto synthetic_executable = options.fs2_root / "comm_bundle_packager.exe";
		if (cfile_init(synthetic_executable.string().c_str()) != 0) throw std::runtime_error("CFile initialization failed");
		try {
			const auto result = comm_bundle::write_bundle(build_input(options), options.output);
			cfile_close();
			Cmdline_mod = nullptr;
			std::cout << "bundle_hash=" << result.bundle_hash << '\n'
				<< "archive=" << result.archive_path.string() << '\n'
				<< "assets=" << result.asset_count << '\n';
		} catch (...) {
			cfile_close();
			Cmdline_mod = nullptr;
			throw;
		}
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "comm_bundle_packager: " << error.what() << '\n';
		print_usage();
		return 1;
	}
}
