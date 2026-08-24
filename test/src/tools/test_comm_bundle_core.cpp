#include "comm_bundle_core.h"
#include "mission/messageheadvariants.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

comm_bundle::Frame frame(std::uint8_t red,
	std::uint8_t green,
	std::uint8_t blue,
	std::uint8_t alpha,
	std::uint16_t delay_num = 1,
	std::uint16_t delay_den = 10)
{
	comm_bundle::Frame result;
	result.width = 2;
	result.height = 1;
	result.delay_num = delay_num;
	result.delay_den = delay_den;
	result.rgba = {red, green, blue, alpha, blue, red, green, 255};
	return result;
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path)
{
	std::ifstream input(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

TEST(CommBundleCore, ComputesVariableTimingWithExactRationalArithmetic)
{
	const std::vector<comm_bundle::Frame> frames{frame(1, 2, 3, 255, 1, 3), frame(4, 5, 6, 255, 1, 7)};
	EXPECT_EQ(comm_bundle::duration_us(frames), 476190U);
}

TEST(CommBundleCore, PngAndApngEncodingAreDeterministic)
{
	const auto source = frame(10, 20, 30, 40);
	const auto png_a = comm_bundle::encode_png(source);
	const auto png_b = comm_bundle::encode_png(source);
	ASSERT_EQ(png_a, png_b);
	ASSERT_GE(png_a.size(), 8U);
	EXPECT_EQ(std::vector<std::uint8_t>(png_a.begin(), png_a.begin() + 8),
		(std::vector<std::uint8_t>{137, 80, 78, 71, 13, 10, 26, 10}));

	const std::vector<comm_bundle::Frame> frames{source, frame(50, 60, 70, 255, 2, 15)};
	const auto apng_a = comm_bundle::encode_apng(frames);
	const auto apng_b = comm_bundle::encode_apng(frames);
	EXPECT_EQ(apng_a, apng_b);
	const std::vector<std::uint8_t> animation_control{'a', 'c', 'T', 'L'};
	const std::vector<std::uint8_t> frame_data{'f', 'd', 'A', 'T'};
	EXPECT_NE(std::search(apng_a.begin(), apng_a.end(), animation_control.begin(), animation_control.end()), apng_a.end());
	EXPECT_NE(std::search(apng_a.begin(), apng_a.end(), frame_data.begin(), frame_data.end()), apng_a.end());
}

TEST(CommBundleCore, TalkingHeadVariantSelectionMatchesLegacyAndNewSuffixRules)
{
	auto legacy = select_message_head_variant("head-tp", false, true, false,
		MessageHeadPersonaClass::WingmanSupport, false, 1);
	EXPECT_EQ(legacy.filename, "head-tpb");
	EXPECT_FALSE(legacy.death_scream);

	auto death = select_message_head_variant("head-tp", false, true, false,
		MessageHeadPersonaClass::WingmanSupport, true, 0);
	EXPECT_EQ(death.filename, "head-tpc");
	EXPECT_TRUE(death.death_scream);

	auto modern = select_message_head_variant("head-test", false, true, true,
		MessageHeadPersonaClass::Large, true, 0);
	EXPECT_EQ(modern.filename, "head-test-death");
	const auto variants = enumerate_message_head_variants("head-test", false, true);
	EXPECT_NE(std::find(variants.begin(), variants.end(), "head-test-reg"), variants.end());
	EXPECT_NE(std::find(variants.begin(), variants.end(), "head-test-death"), variants.end());
}

TEST(CommBundleCore, EmitsCanonicalManifestAndStableAssetIdentity)
{
	auto asset = comm_bundle::make_animation_asset("head-test.ani", comm_bundle::SourceFormat::Ani,
		{frame(1, 2, 3, 255), frame(4, 5, 6, 128)});
	EXPECT_EQ(asset.id, comm_bundle::asset_id_from_sha256(asset.sha256));
	EXPECT_EQ(asset.alpha_mode, comm_bundle::AlphaMode::Straight);
	EXPECT_TRUE(comm_bundle::portable_bundle_path(asset.file));

	comm_bundle::BundleInput input;
	input.mod_signature = "test-mod";
	input.source_revision = "revision";
	input.assets = {asset};
	input.mappings = {{"head-test.ani", "ANI", asset.id}};
	const auto manifest = comm_bundle::canonical_manifest(input);
	EXPECT_EQ(manifest.front(), '{');
	EXPECT_EQ(manifest.back(), '}');
	EXPECT_NE(manifest.find("\"assets\":["), std::string::npos);
	EXPECT_NE(manifest.find("\"converterId\":\"fs2open-comm-bundle\""), std::string::npos);
	EXPECT_EQ(manifest.find(" \""), std::string::npos);
}

TEST(CommBundleCore, RejectsUnsafePortablePaths)
{
	EXPECT_TRUE(comm_bundle::portable_bundle_path("communication/animations/asset-1.png"));
	EXPECT_FALSE(comm_bundle::portable_bundle_path("../asset.png"));
	EXPECT_FALSE(comm_bundle::portable_bundle_path("communication\\asset.png"));
	EXPECT_FALSE(comm_bundle::portable_bundle_path("communication/CON.png"));
	EXPECT_FALSE(comm_bundle::portable_bundle_path("communication/.hidden"));
}

TEST(CommBundleCore, RejectsInvalidUtf8AndDuplicateMappings)
{
	auto asset = comm_bundle::make_animation_asset("head-test.ani", comm_bundle::SourceFormat::Ani,
		{frame(1, 2, 3, 255)});
	comm_bundle::BundleInput input;
	input.mod_signature = "test-mod";
	input.assets = {asset};
	input.mappings = {{"head-test.ani", "ANI", asset.id}, {"HEAD-TEST.ANI", "ANI", asset.id}};
	const auto base = std::filesystem::temp_directory_path() / "fso-comm-bundle-invalid-test";
	EXPECT_THROW(comm_bundle::write_bundle(input, base), std::runtime_error);
	input.mappings.resize(1);
	input.mod_signature = std::string("invalid-") + static_cast<char>(0xc0) + static_cast<char>(0x80);
	EXPECT_THROW(comm_bundle::write_bundle(input, base), std::runtime_error);
}

TEST(CommBundleCore, PublishesAnIdempotentByteStableArchive)
{
	const auto base = std::filesystem::temp_directory_path() / "fso-comm-bundle-core-test";
	std::error_code ignored;
	std::filesystem::remove_all(base, ignored);
	std::filesystem::create_directories(base / "first");
	std::filesystem::create_directories(base / "second");

	auto asset = comm_bundle::make_animation_asset("head-test.ani", comm_bundle::SourceFormat::Ani,
		{frame(1, 2, 3, 255), frame(4, 5, 6, 128)});
	comm_bundle::BundleInput input;
	input.mod_signature = "test-mod";
	input.assets = {asset};
	input.mappings = {{"head-test.ani", "ANI", asset.id}};

	const auto first = comm_bundle::write_bundle(input, base / "first");
	const auto repeated = comm_bundle::write_bundle(input, base / "first");
	const auto second = comm_bundle::write_bundle(input, base / "second");
	EXPECT_EQ(first.bundle_hash, repeated.bundle_hash);
	EXPECT_EQ(first.bundle_hash, second.bundle_hash);
	EXPECT_EQ(read_bytes(first.archive_path), read_bytes(second.archive_path));
	std::filesystem::remove_all(base, ignored);
}

} // namespace
