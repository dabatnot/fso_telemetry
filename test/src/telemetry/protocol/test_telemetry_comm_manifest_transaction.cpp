#include "telemetry/protocol/telemetry_comm_manifest_transaction.h"

#include "telemetry/protocol/packet_writer.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace telemetry::protocol;

template <typename Container>
ByteView byte_view(const Container& bytes)
{
	return ByteView{reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
}

template <typename Container>
MutableByteView mutable_byte_view(Container& bytes)
{
	return MutableByteView{reinterpret_cast<std::uint8_t*>(bytes.data()), bytes.size()};
}

Sha256Digest asset_hash(std::uint8_t first)
{
	Sha256Digest result{};
	for (std::size_t index = 0; index < result.size(); ++index) {
		result[index] = static_cast<std::uint8_t>(first + index);
	}
	return result;
}

std::vector<std::uint8_t> make_entry(std::uint8_t hash_first, const std::string& path)
{
	const std::string logical_name = "head";
	std::vector<std::uint8_t> bytes(CommAssetEntryPrefixSize + logical_name.size() + path.size());
	const auto hash = asset_hash(hash_first);
	PacketWriter writer(mutable_byte_view(bytes));
	EXPECT_TRUE(writer.write_u16(static_cast<std::uint16_t>(bytes.size())));
	EXPECT_TRUE(writer.write_u64(comm_asset_id_from_content_hash(hash)));
	EXPECT_TRUE(writer.write_bytes(byte_view(hash)));
	EXPECT_TRUE(writer.write_u8(static_cast<std::uint8_t>(SourceFormat::StaticImage)));
	EXPECT_TRUE(writer.write_u8(static_cast<std::uint8_t>(DeliveredFormat::Png)));
	EXPECT_TRUE(writer.write_u8(static_cast<std::uint8_t>(AlphaMode::Straight)));
	EXPECT_TRUE(writer.write_u8(static_cast<std::uint8_t>(AssetTimingMode::FormatIntrinsic)));
	EXPECT_TRUE(writer.write_u16(0));
	EXPECT_TRUE(writer.write_u16(320));
	EXPECT_TRUE(writer.write_u16(200));
	EXPECT_TRUE(writer.write_u32(1));
	EXPECT_TRUE(writer.write_u64(1'000'000));
	EXPECT_TRUE(writer.write_u16(static_cast<std::uint16_t>(logical_name.size())));
	EXPECT_TRUE(writer.write_u16(static_cast<std::uint16_t>(path.size())));
	EXPECT_TRUE(writer.write_u32(0));
	EXPECT_TRUE(writer.write_bytes(byte_view(logical_name)));
	EXPECT_TRUE(writer.write_bytes(byte_view(path)));
	EXPECT_EQ(bytes.size(), writer.size());
	return bytes;
}

std::vector<std::uint8_t> make_chunk(std::uint32_t first_index,
	std::uint32_t total_count,
	const std::vector<std::uint8_t>& entry,
	const Sha256Digest& bundle_hash,
	std::uint16_t flags = ManifestFlagNone,
	std::uint64_t frame_asset_id = 0)
{
	const std::string converter = "identity";
	const std::string version = "1";
	std::vector<std::uint8_t> bytes(CommAssetManifestPrefixSize + converter.size() + version.size() + entry.size());
	PacketWriter writer(mutable_byte_view(bytes));
	EXPECT_TRUE(writer.write_u16(CommBundleVersionV1));
	EXPECT_TRUE(writer.write_u16(flags));
	EXPECT_TRUE(writer.write_bytes(byte_view(bundle_hash)));
	EXPECT_TRUE(writer.write_u8(static_cast<std::uint8_t>(converter.size())));
	EXPECT_TRUE(writer.write_u8(static_cast<std::uint8_t>(version.size())));
	EXPECT_TRUE(writer.write_u64(frame_asset_id));
	EXPECT_TRUE(writer.write_u64(0));
	EXPECT_TRUE(writer.write_u32(total_count));
	EXPECT_TRUE(writer.write_u32(first_index));
	EXPECT_TRUE(writer.write_u16(1));
	EXPECT_TRUE(writer.write_bytes(byte_view(converter)));
	EXPECT_TRUE(writer.write_bytes(byte_view(version)));
	EXPECT_TRUE(writer.write_bytes(byte_view(entry)));
	EXPECT_EQ(bytes.size(), writer.size());
	return bytes;
}

TEST(TelemetryProtocolCommManifestTransaction, PublishesOnlyAfterContiguousCompleteCatalog)
{
	const auto bundle = asset_hash(0x80);
	const auto first = make_chunk(0, 2, make_entry(1, "heads/a.png"), bundle);
	const auto second = make_chunk(1, 2, make_entry(2, "heads/b.png"), bundle);
	CommManifestTransaction transaction;
	CommAssetCatalogDescriptor catalog;
	catalog.asset_count = 0xfeedU;

	EXPECT_EQ(ValidationError::None, transaction.ingest_record(byte_view(first)));
	EXPECT_EQ(ValidationError::MissingManifest, transaction.finalize(catalog));
	EXPECT_EQ(0xfeedU, catalog.asset_count);
	EXPECT_EQ(ValidationError::None, transaction.ingest_record(byte_view(second)));
	EXPECT_EQ(ValidationError::None, transaction.finalize(catalog));
	EXPECT_EQ(CommManifestTransactionState::Complete, transaction.state());
	EXPECT_EQ(2U, catalog.asset_count);
	EXPECT_EQ(bundle, catalog.bundle_hash);
	EXPECT_EQ(ValidationError::InvalidStateTransition, transaction.ingest_record(byte_view(second)));
}

TEST(TelemetryProtocolCommManifestTransaction, RejectsGapsHeaderDriftAndCrossChunkDuplicates)
{
	const auto bundle = asset_hash(0x80);
	const auto first = make_chunk(0, 2, make_entry(1, "heads/Alpha.png"), bundle);

	CommManifestTransaction gap;
	const auto starts_at_one = make_chunk(1, 2, make_entry(2, "heads/b.png"), bundle);
	EXPECT_EQ(ValidationError::InvalidStateTransition, gap.ingest_record(byte_view(starts_at_one)));
	EXPECT_EQ(CommManifestTransactionState::Failed, gap.state());

	CommManifestTransaction drift;
	ASSERT_EQ(ValidationError::None, drift.ingest_record(byte_view(first)));
	const auto other_bundle = make_chunk(1, 2, make_entry(2, "heads/b.png"), asset_hash(0x90));
	EXPECT_EQ(ValidationError::InvalidStateTransition, drift.ingest_record(byte_view(other_bundle)));

	CommManifestTransaction duplicate_path;
	ASSERT_EQ(ValidationError::None, duplicate_path.ingest_record(byte_view(first)));
	const auto casefold_duplicate = make_chunk(1, 2, make_entry(2, "HEADS/alpha.PNG"), bundle);
	EXPECT_EQ(ValidationError::DuplicateItemKey,
		duplicate_path.ingest_record(byte_view(casefold_duplicate)));
}

TEST(TelemetryProtocolCommManifestTransaction, GlobalAssetReferencesResolveAcrossAllChunks)
{
	const auto bundle = asset_hash(0x80);
	const auto missing_id = comm_asset_id_from_content_hash(asset_hash(3));
	const auto first = make_chunk(
		0, 2, make_entry(1, "heads/a.png"), bundle, ManifestFlagFrameAsset, missing_id);
	const auto second = make_chunk(
		1, 2, make_entry(2, "heads/b.png"), bundle, ManifestFlagFrameAsset, missing_id);
	CommManifestTransaction transaction;
	ASSERT_EQ(ValidationError::None, transaction.ingest_record(byte_view(first)));
	ASSERT_EQ(ValidationError::None, transaction.ingest_record(byte_view(second)));
	CommAssetCatalogDescriptor catalog;
	catalog.asset_count = 99;
	EXPECT_EQ(ValidationError::UnknownEntity, transaction.finalize(catalog));
	EXPECT_EQ(99U, catalog.asset_count);
	EXPECT_EQ(CommManifestTransactionState::Failed, transaction.state());
}

TEST(TelemetryProtocolCommManifestTransaction, IndexesManyChunksAndPublishesAtomically)
{
	constexpr std::uint32_t AssetCount = 200;
	const auto bundle = asset_hash(0x80);
	const auto frame_asset_id = comm_asset_id_from_content_hash(asset_hash(1));
	CommManifestTransaction transaction;
	for (std::uint32_t index = 0; index < AssetCount; ++index) {
		const auto entry = make_entry(static_cast<std::uint8_t>(index + 1U),
			"heads/item-" + std::to_string(index + 1U) + ".png");
		const auto chunk = make_chunk(index,
			AssetCount,
			entry,
			bundle,
			ManifestFlagFrameAsset,
			frame_asset_id);
		ASSERT_EQ(ValidationError::None, transaction.ingest_record(byte_view(chunk))) << index;
	}

	CommAssetCatalogDescriptor catalog;
	catalog.asset_count = 0xfeedU;
	ASSERT_EQ(ValidationError::None, transaction.finalize(catalog));
	EXPECT_EQ(AssetCount, catalog.asset_count);
	EXPECT_EQ(frame_asset_id, catalog.frame_asset_id);

	CommManifestTransaction duplicate_path;
	constexpr std::uint32_t DuplicateCatalogSize = 128;
	for (std::uint32_t index = 0; index < DuplicateCatalogSize; ++index) {
		const auto path = index + 1U == DuplicateCatalogSize
			? std::string{"HEADS/ITEM-1.PNG"}
			: "heads/item-" + std::to_string(index + 1U) + ".png";
		const auto chunk = make_chunk(index,
			DuplicateCatalogSize,
			make_entry(static_cast<std::uint8_t>(index + 1U), path),
			bundle);
		const auto expected = index + 1U == DuplicateCatalogSize ? ValidationError::DuplicateItemKey
														   : ValidationError::None;
		EXPECT_EQ(expected, duplicate_path.ingest_record(byte_view(chunk))) << index;
	}
	EXPECT_EQ(CommManifestTransactionState::Failed, duplicate_path.state());
}

} // namespace
