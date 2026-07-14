#include "telemetry/protocol/telemetry_specialized_views.h"

#include "telemetry/protocol/packet_writer.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

using namespace telemetry::protocol;

template <typename Container>
ByteView byte_view(const Container& bytes)
{
	return ByteView{static_cast<const std::uint8_t*>(static_cast<const void*>(bytes.data())), bytes.size()};
}

template <typename Container>
MutableByteView mutable_byte_view(Container& bytes)
{
	return MutableByteView{static_cast<std::uint8_t*>(static_cast<void*>(bytes.data())), bytes.size()};
}

Sha256Digest nonzero_digest()
{
	Sha256Digest digest{};
	for (std::size_t index = 0; index < digest.size(); ++index) {
		digest[index] = static_cast<std::uint8_t>(index + 1U);
	}
	return digest;
}

TargetVideoSubscribePayload valid_subscribe()
{
	TargetVideoSubscribePayload payload;
	payload.request_id = 7;
	payload.max_width = 1024;
	payload.max_height = 1024;
	payload.preferred_width = 960;
	payload.preferred_height = 960;
	payload.max_fps = 20;
	payload.preferred_fps = 15;
	payload.max_bitrate_kbps = 8000;
	payload.preferred_bitrate_kbps = 4000;
	payload.supported_h264_profiles = H264ProfileBitConstrainedBaseline | H264ProfileBitHigh;
	payload.supported_h264_levels = H264LevelBitLevel3_1 | H264LevelBitLevel4_1;
	payload.overlay_capabilities = RequiredOverlayCapabilityBits;
	payload.acceptable_render_profiles = RenderProfileBitMfdHigh | RenderProfileBitHudExact;
	payload.preferred_render_profile = RenderProfile::MfdHigh;
	return payload;
}

TargetVideoConfigPayload valid_config()
{
	TargetVideoConfigPayload payload;
	payload.request_id = 7;
	payload.result = VideoConfigResult::Accepted;
	payload.codec = Codec::H264AnnexB;
	payload.codec_profile = H264Profile::High;
	payload.codec_level = H264Level::Level4_1;
	payload.stream_id = 9;
	payload.config_generation = 1;
	payload.pixel_format = PixelFormat::Yuv420p8;
	payload.render_profile = RenderProfile::MfdHigh;
	payload.overlay_mode = OverlayMode::Client;
	payload.recovery_mode = RecoveryMode::IdrSelectiveRetransmit;
	payload.width = 960;
	payload.height = 960;
	payload.fps_num = 15;
	payload.fps_den = 1;
	payload.bitrate_kbps = 4000;
	payload.gop_duration_ms = 1000;
	payload.idr_recovery_window_ms = 500;
	return payload;
}

constexpr std::array<std::uint8_t, 18> ValidIdr{{
	0x00, 0x00, 0x00, 0x01, 0x67, 0x11,
	0x00, 0x00, 0x00, 0x01, 0x68, 0x22,
	0x00, 0x00, 0x00, 0x01, 0x65, 0x33,
}};

constexpr std::array<std::uint8_t, 6> ValidInterframe{{0x00, 0x00, 0x00, 0x01, 0x61, 0x44}};

std::vector<std::uint8_t> idr_with_sps(std::initializer_list<std::uint8_t> sps)
{
	std::vector<std::uint8_t> access_unit{{0x00, 0x00, 0x00, 0x01, 0x67}};
	access_unit.insert(access_unit.end(), sps.begin(), sps.end());
	const std::array<std::uint8_t, 12> suffix{{
		0x00, 0x00, 0x00, 0x01, 0x68, 0x22,
		0x00, 0x00, 0x00, 0x01, 0x65, 0x33,
	}};
	access_unit.insert(access_unit.end(), suffix.begin(), suffix.end());
	return access_unit;
}

std::vector<std::uint8_t> configured_idr()
{
	// High, level 4.1, 960x960 progressive 4:2:0 8-bit, limited-range
	// BT.709 VUI. The final byte contains rbsp_stop_one_bit and padding.
	return idr_with_sps({
		0x64, 0x00, 0x29, 0xac, 0xe8, 0x0f, 0x01, 0xe6, 0x9a, 0x80, 0x80, 0x80, 0x81,
	});
}

Sha256Digest asset_hash(std::uint8_t first)
{
	Sha256Digest hash{};
	for (std::size_t index = 0; index < hash.size(); ++index) {
		hash[index] = static_cast<std::uint8_t>(first + index);
	}
	return hash;
}

std::vector<std::uint8_t> make_asset_entry(std::uint8_t hash_first,
	const std::string& logical_name,
	const std::string& path,
	AssetTimingMode timing_mode,
	const std::vector<std::uint32_t>& durations,
	std::uint32_t frame_count,
	std::uint64_t duration_us)
{
	const auto entry_size = CommAssetEntryPrefixSize + logical_name.size() + path.size() + durations.size() * 4U;
	EXPECT_LE(entry_size, MaximumCommAssetRecordPayloadSize);
	std::vector<std::uint8_t> bytes(entry_size);
	const auto hash = asset_hash(hash_first);
	PacketWriter writer(mutable_byte_view(bytes));
	EXPECT_TRUE(writer.write_u16(static_cast<std::uint16_t>(entry_size)));
	EXPECT_TRUE(writer.write_u64(comm_asset_id_from_content_hash(hash)));
	EXPECT_TRUE(writer.write_bytes(byte_view(hash)));
	EXPECT_TRUE(writer.write_u8(static_cast<std::uint8_t>(SourceFormat::StaticImage)));
	EXPECT_TRUE(writer.write_u8(static_cast<std::uint8_t>(DeliveredFormat::Png)));
	EXPECT_TRUE(writer.write_u8(static_cast<std::uint8_t>(AlphaMode::Straight)));
	EXPECT_TRUE(writer.write_u8(static_cast<std::uint8_t>(timing_mode)));
	EXPECT_TRUE(writer.write_u16(0));
	EXPECT_TRUE(writer.write_u16(320));
	EXPECT_TRUE(writer.write_u16(200));
	EXPECT_TRUE(writer.write_u32(frame_count));
	EXPECT_TRUE(writer.write_u64(duration_us));
	EXPECT_TRUE(writer.write_u16(static_cast<std::uint16_t>(logical_name.size())));
	EXPECT_TRUE(writer.write_u16(static_cast<std::uint16_t>(path.size())));
	EXPECT_TRUE(writer.write_u32(static_cast<std::uint32_t>(durations.size())));
	EXPECT_TRUE(writer.write_bytes(byte_view(logical_name)));
	EXPECT_TRUE(writer.write_bytes(byte_view(path)));
	for (const auto duration : durations) {
		EXPECT_TRUE(writer.write_u32(duration));
	}
	EXPECT_EQ(bytes.size(), writer.size());
	return bytes;
}

std::vector<std::uint8_t> make_manifest_chunk(const std::vector<std::vector<std::uint8_t>>& entries,
	std::uint16_t flags = ManifestFlagNone,
	std::uint64_t frame_asset_id = 0,
	std::uint64_t placeholder_asset_id = 0)
{
	constexpr char ConverterId[] = "identity";
	constexpr char ConverterVersion[] = "test-1";
	std::size_t entries_size = 0;
	for (const auto& entry : entries) {
		entries_size += entry.size();
	}
	std::vector<std::uint8_t> bytes(
		CommAssetManifestPrefixSize + sizeof(ConverterId) - 1U + sizeof(ConverterVersion) - 1U + entries_size);
	const auto bundle_hash = asset_hash(0x80);
	PacketWriter writer(mutable_byte_view(bytes));
	EXPECT_TRUE(writer.write_u16(CommBundleVersionV1));
	EXPECT_TRUE(writer.write_u16(flags));
	EXPECT_TRUE(writer.write_bytes(byte_view(bundle_hash)));
	EXPECT_TRUE(writer.write_u8(static_cast<std::uint8_t>(sizeof(ConverterId) - 1U)));
	EXPECT_TRUE(writer.write_u8(static_cast<std::uint8_t>(sizeof(ConverterVersion) - 1U)));
	EXPECT_TRUE(writer.write_u64(frame_asset_id));
	EXPECT_TRUE(writer.write_u64(placeholder_asset_id));
	EXPECT_TRUE(writer.write_u32(static_cast<std::uint32_t>(entries.size())));
	EXPECT_TRUE(writer.write_u32(0));
	EXPECT_TRUE(writer.write_u16(static_cast<std::uint16_t>(entries.size())));
	EXPECT_TRUE(writer.write_bytes(
		ByteView{reinterpret_cast<const std::uint8_t*>(ConverterId), sizeof(ConverterId) - 1U}));
	EXPECT_TRUE(writer.write_bytes(
		ByteView{reinterpret_cast<const std::uint8_t*>(ConverterVersion), sizeof(ConverterVersion) - 1U}));
	for (const auto& entry : entries) {
		EXPECT_TRUE(writer.write_bytes(byte_view(entry)));
	}
	EXPECT_EQ(bytes.size(), writer.size());
	return bytes;
}

TEST(TelemetryProtocolSpecializedViews, CommAssetIdentityPathGrammarAndEntryTimingAreStrict)
{
	const auto hash = asset_hash(1);
	EXPECT_EQ(0x0102030405060708ULL, comm_asset_id_from_content_hash(hash));

	const std::string valid_path = "portraits/Alpha_1-head.png";
	EXPECT_EQ(ValidationError::None, validate_portable_bundle_path(byte_view(valid_path)));
	for (const std::string invalid_path : {
			"CON.png", "aux.data/file.png", "../file.png", "folder//file.png", "folder\\file.png", ".hidden"}) {
		EXPECT_NE(ValidationError::None, validate_portable_bundle_path(byte_view(invalid_path))) << invalid_path;
	}

	auto entry_bytes = make_asset_entry(1, "Alpha", valid_path, AssetTimingMode::PerFrame, {10, 20}, 2, 30);
	CommAssetEntryView entry;
	std::size_t consumed = 0;
	ASSERT_EQ(ValidationError::None, decode_comm_asset_entry(byte_view(entry_bytes), entry, consumed));
	EXPECT_EQ(entry_bytes.size(), consumed);
	EXPECT_EQ(2U, entry.frame_duration_count);
	EXPECT_EQ(30U, entry.duration_us);
	EXPECT_EQ(comm_asset_id_from_content_hash(hash), entry.asset_id);

	auto bad_hash = entry_bytes;
	bad_hash[10] ^= 0xffU;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_comm_asset_entry_payload(byte_view(bad_hash)));
	auto bad_timing = entry_bytes;
	bad_timing.back() = 21;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_comm_asset_entry_payload(byte_view(bad_timing)));
	auto bad_length = entry_bytes;
	bad_length[0] = static_cast<std::uint8_t>(bad_length[0] - 1U);
	EXPECT_EQ(ValidationError::BadRecordLength, validate_comm_asset_entry_payload(byte_view(bad_length)));
}

TEST(TelemetryProtocolSpecializedViews, CommAssetManifestValidatesChunkAndEntryBoundariesWithoutAllocation)
{
	auto first = make_asset_entry(1, "Alpha", "heads/a.png", AssetTimingMode::Constant, {100}, 2, 200);
	auto second = make_asset_entry(2, "Beta", "heads/b.png", AssetTimingMode::FormatIntrinsic, {}, 1, 100);
	const auto first_id = comm_asset_id_from_content_hash(asset_hash(1));
	auto manifest = make_manifest_chunk({first, second}, ManifestFlagFrameAsset, first_id, 0);
	CommAssetManifestPayloadView decoded;
	ASSERT_EQ(ValidationError::None, decode_comm_asset_manifest_payload(byte_view(manifest), decoded));
	EXPECT_EQ(2U, decoded.entry_count);
	EXPECT_EQ(2U, decoded.total_asset_count);
	EXPECT_EQ(first_id, decoded.frame_asset_id);
	EXPECT_EQ(first.size() + second.size(), decoded.encoded_entries.size);
	EXPECT_EQ(ValidationError::None, validate_comm_asset_manifest_record_payload(byte_view(manifest)));

	auto trailing = manifest;
	trailing.push_back(0);
	EXPECT_EQ(ValidationError::TrailingBytes, validate_comm_asset_manifest_record_payload(byte_view(trailing)));

	auto missing_global = make_manifest_chunk({first, second}, ManifestFlagFrameAsset, 0xabcdefU, 0);
	EXPECT_EQ(ValidationError::UnknownEntity,
		validate_comm_asset_manifest_record_payload(byte_view(missing_global)));

	auto mismatched_flag = manifest;
	mismatched_flag[2] = 0;
	mismatched_flag[3] = 0;
	EXPECT_EQ(ValidationError::InvalidAbsence,
		validate_comm_asset_manifest_record_payload(byte_view(mismatched_flag)));

	auto unsupported_bundle = manifest;
	unsupported_bundle[0] = 2;
	EXPECT_EQ(ValidationError::OutOfRange,
		validate_comm_asset_manifest_record_payload(byte_view(unsupported_bundle)));
	auto excessive_total = manifest;
	const auto excessive_count = MaximumCommAssetCount + 1U;
	for (std::size_t index = 0; index < 4U; ++index) {
		excessive_total[54U + index] = static_cast<std::uint8_t>(excessive_count >> (index * 8U));
	}
	EXPECT_EQ(ValidationError::ResourceLimit,
		validate_comm_asset_manifest_record_payload(byte_view(excessive_total)));
	auto excessive_chunk = manifest;
	excessive_chunk[62] = static_cast<std::uint8_t>(excessive_count);
	excessive_chunk[63] = static_cast<std::uint8_t>(excessive_count >> 8U);
	EXPECT_EQ(ValidationError::ResourceLimit,
		validate_comm_asset_manifest_record_payload(byte_view(excessive_chunk)));
}

TEST(TelemetryProtocolSpecializedViews, CommAssetManifestRejectsNonCanonicalOrderAndCasefoldedPaths)
{
	auto first = make_asset_entry(1, "Alpha", "heads/Same.png", AssetTimingMode::FormatIntrinsic, {}, 1, 100);
	auto second = make_asset_entry(2, "Beta", "HEADS/same.PNG", AssetTimingMode::FormatIntrinsic, {}, 1, 100);
	auto duplicate_path = make_manifest_chunk({first, second});
	EXPECT_EQ(ValidationError::DuplicateItemKey,
		validate_comm_asset_manifest_record_payload(byte_view(duplicate_path)));
	auto malformed_third = make_asset_entry(3, "Gamma", "heads/c.png", AssetTimingMode::FormatIntrinsic, {}, 1, 100);
	malformed_third[10] ^= 0xffU;
	auto duplicate_before_malformed = make_manifest_chunk({first, second, malformed_third});
	EXPECT_EQ(ValidationError::DuplicateItemKey,
		validate_comm_asset_manifest_record_payload(byte_view(duplicate_before_malformed)));

	auto reverse_order = make_manifest_chunk({second, first});
	EXPECT_EQ(ValidationError::OutOfRange,
		validate_comm_asset_manifest_record_payload(byte_view(reverse_order)));

	auto duplicate_id = make_asset_entry(1, "Beta", "heads/other.png", AssetTimingMode::FormatIntrinsic, {}, 1, 100);
	auto duplicate_id_manifest = make_manifest_chunk({first, duplicate_id});
	EXPECT_EQ(ValidationError::DuplicateItemKey,
		validate_comm_asset_manifest_record_payload(byte_view(duplicate_id_manifest)));
}

TEST(TelemetryProtocolSpecializedViews, CommAssetManifestIndexesManyPathsAndPreservesDuplicateTaxonomy)
{
	constexpr std::size_t EntryCount = 200;
	std::vector<std::vector<std::uint8_t>> entries;
	entries.reserve(EntryCount);
	for (std::size_t index = 1; index <= EntryCount; ++index) {
		entries.push_back(make_asset_entry(static_cast<std::uint8_t>(index),
			"Head " + std::to_string(index),
			"heads/item-" + std::to_string(index) + ".png",
			AssetTimingMode::FormatIntrinsic,
			{},
			1,
			100));
	}
	const auto manifest = make_manifest_chunk(entries);
	EXPECT_EQ(ValidationError::None, validate_comm_asset_manifest_record_payload(byte_view(manifest)));

	entries.back() = make_asset_entry(static_cast<std::uint8_t>(EntryCount),
		"Duplicate path",
		"HEADS/ITEM-1.PNG",
		AssetTimingMode::FormatIntrinsic,
		{},
		1,
		100);
	const auto duplicate_path = make_manifest_chunk(entries);
	EXPECT_EQ(ValidationError::DuplicateItemKey,
		validate_comm_asset_manifest_record_payload(byte_view(duplicate_path)));
}

TEST(TelemetryProtocolSpecializedViews, BundleNegotiationUsesCanonicalFixedLayouts)
{
	CommBundleOffer offer;
	offer.bundle_version = CommBundleVersionV1;
	offer.supported_delivered_formats = DeliveredFormatBitAni | DeliveredFormatBitPng;
	offer.bundle_hash = nonzero_digest();
	std::array<std::uint8_t, CommBundleOfferSize> bytes{};
	std::size_t written = 0;
	ASSERT_EQ(ValidationError::None, encode_comm_bundle_offer(offer, mutable_byte_view(bytes), written));
	EXPECT_EQ(bytes.size(), written);
	EXPECT_EQ(0x01U, bytes[0]);
	EXPECT_EQ(0x21U, bytes[4]);

	CommBundleOffer decoded;
	ASSERT_EQ(ValidationError::None, decode_comm_bundle_offer(byte_view(bytes), decoded));
	EXPECT_EQ(offer.bundle_version, decoded.bundle_version);
	EXPECT_EQ(offer.supported_delivered_formats, decoded.supported_delivered_formats);
	EXPECT_EQ(offer.bundle_hash, decoded.bundle_hash);

	bytes[2] = 1;
	EXPECT_EQ(ValidationError::ReservedFlag, decode_comm_bundle_offer(byte_view(bytes), decoded));
	bytes[2] = 0;
	bytes[4] = 0x61;
	EXPECT_EQ(ValidationError::None, decode_comm_bundle_offer(byte_view(bytes), decoded));
	EXPECT_EQ(0x61U, decoded.supported_delivered_formats);
	offer.supported_delivered_formats = 0x61U;
	EXPECT_EQ(ValidationError::ReservedFlag, encode_comm_bundle_offer(offer, mutable_byte_view(bytes), written));
}

TEST(TelemetryProtocolSpecializedViews, CommStateAndEventsEnforceCanonicalActiveAndStopForms)
{
	CommViewStatePayload state;
	state.active = true;
	state.playback_mode = CommPlaybackMode::Loop;
	state.color_mode = CommColorMode::FullColor;
	state.playback_id = 1;
	state.engine_message_id = 2;
	state.sender_entity_id = 3;
	state.head_asset_id = 4;
	state.producer_sample_time_us = 5;
	state.animation_time_us = 6;
	state.duration_us = 7;
	state.playback_rate = -64.0F;
	std::array<std::uint8_t, CommViewStatePayloadSize> state_bytes{};
	std::size_t written = 0;
	ASSERT_EQ(ValidationError::None,
		encode_comm_view_state_payload(state, mutable_byte_view(state_bytes), written));
	CommViewStatePayload decoded_state;
	EXPECT_EQ(ValidationError::None, decode_comm_view_state_payload(byte_view(state_bytes), decoded_state));
	EXPECT_EQ(state.playback_id, decoded_state.playback_id);
	EXPECT_EQ(state.playback_rate, decoded_state.playback_rate);

	state.active = false;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_comm_view_state_payload(state));

	CommViewEventPayload stop;
	stop.event_id = 8;
	stop.event_kind = CommEventKind::Stop;
	stop.stop_reason = CommStopReason::Completed;
	stop.playback_id = 1;
	stop.producer_sample_time_us = 9;
	EXPECT_EQ(ValidationError::None, validate_comm_view_event_payload(stop));
	stop.head_asset_id = 4;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_comm_view_event_payload(stop));
}

TEST(TelemetryProtocolSpecializedViews, SubscribeAndConfigRoundTripAndHonorNegotiatedBounds)
{
	const auto subscribe = valid_subscribe();
	std::array<std::uint8_t, TargetVideoSubscribePayloadSize> subscribe_bytes{};
	std::size_t written = 0;
	ASSERT_EQ(ValidationError::None,
		encode_target_video_subscribe_payload(subscribe, mutable_byte_view(subscribe_bytes), written));
	TargetVideoSubscribePayload decoded_subscribe;
	EXPECT_EQ(ValidationError::None,
		decode_target_video_subscribe_payload(byte_view(subscribe_bytes), decoded_subscribe));
	EXPECT_EQ(subscribe.preferred_width, decoded_subscribe.preferred_width);
	subscribe_bytes[27] |= 0x80U;
	ASSERT_EQ(ValidationError::None,
		decode_target_video_subscribe_payload(byte_view(subscribe_bytes), decoded_subscribe));
	EXPECT_NE(0U, decoded_subscribe.supported_h264_profiles & 0x80000000U);
	EXPECT_EQ(ValidationError::ReservedFlag,
		encode_target_video_subscribe_payload(decoded_subscribe, mutable_byte_view(subscribe_bytes), written));

	const auto config = valid_config();
	EXPECT_EQ(ValidationError::None, validate_target_video_config_for_subscribe(config, subscribe));
	std::array<std::uint8_t, TargetVideoConfigPayloadSize> config_bytes{};
	ASSERT_EQ(ValidationError::None,
		encode_target_video_config_payload(config, mutable_byte_view(config_bytes), written));
	TargetVideoConfigPayload decoded_config;
	EXPECT_EQ(ValidationError::None, decode_target_video_config_payload(byte_view(config_bytes), decoded_config));
	EXPECT_EQ(config.stream_id, decoded_config.stream_id);

	auto too_large = config;
	too_large.width = 1024;
	auto narrower = subscribe;
	narrower.max_width = 960;
	EXPECT_EQ(ValidationError::CapabilityNotNegotiated,
		validate_target_video_config_for_subscribe(too_large, narrower));

	auto profile_offer = subscribe;
	profile_offer.supported_h264_profiles = H264ProfileBitConstrainedBaseline;
	EXPECT_EQ(ValidationError::CapabilityNotNegotiated,
		validate_target_video_config_for_subscribe(config, profile_offer));
	auto level_offer = subscribe;
	level_offer.supported_h264_levels = H264LevelBitLevel3_1;
	EXPECT_EQ(ValidationError::CapabilityNotNegotiated,
		validate_target_video_config_for_subscribe(config, level_offer));
	auto render_offer = subscribe;
	render_offer.acceptable_render_profiles = RenderProfileBitHudExact;
	render_offer.preferred_render_profile = RenderProfile::HudExact;
	EXPECT_EQ(ValidationError::CapabilityNotNegotiated,
		validate_target_video_config_for_subscribe(config, render_offer));
	auto fps_offer = subscribe;
	fps_offer.max_fps = 10;
	fps_offer.preferred_fps = 10;
	EXPECT_EQ(ValidationError::CapabilityNotNegotiated,
		validate_target_video_config_for_subscribe(config, fps_offer));
	auto bitrate_offer = subscribe;
	bitrate_offer.max_bitrate_kbps = 3000;
	bitrate_offer.preferred_bitrate_kbps = 3000;
	EXPECT_EQ(ValidationError::CapabilityNotNegotiated,
		validate_target_video_config_for_subscribe(config, bitrate_offer));

	TargetVideoConfigPayload rejected;
	rejected.request_id = subscribe.request_id;
	rejected.result = VideoConfigResult::ResourceLimit;
	rejected.codec_profile = static_cast<H264Profile>(0);
	rejected.codec_level = static_cast<H264Level>(0);
	EXPECT_EQ(ValidationError::None, validate_target_video_config_payload(rejected));
	rejected.stream_id = 1;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_target_video_config_payload(rejected));
}

TEST(TelemetryProtocolSpecializedViews, AvcLimitsCoverMaxFsMaxMbpsAndMaxBrBoundaries)
{
	EXPECT_EQ(ValidationError::None,
		validate_avc_level_configuration(H264Level::Level3_1, 960, 960, 30, 1, 14'000));
	EXPECT_EQ(ValidationError::OutOfRange,
		validate_avc_level_configuration(H264Level::Level3_1, 960, 960, 31, 1, 14'000));
	EXPECT_EQ(ValidationError::OutOfRange,
		validate_avc_level_configuration(H264Level::Level3_1, 960, 960, 30, 1, 14'001));
	EXPECT_EQ(ValidationError::OutOfRange,
		validate_avc_level_configuration(H264Level::Level3_1, 976, 960, 1, 1, 1000));
}

TEST(TelemetryProtocolSpecializedViews, AnnexBAndFrameValidationRejectNonCanonicalOrInconsistentAccessUnits)
{
	AnnexBValidationSummary summary;
	EXPECT_EQ(ValidationError::None, validate_h264_annex_b_access_unit(byte_view(ValidIdr), true, summary));
	EXPECT_TRUE(summary.has_sps);
	EXPECT_TRUE(summary.has_pps);
	EXPECT_TRUE(summary.has_idr_slice);
	EXPECT_EQ(ValidationError::None,
		validate_h264_annex_b_access_unit(byte_view(ValidInterframe), false, summary));
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		validate_h264_annex_b_access_unit(byte_view(ValidIdr), false, summary));

	constexpr std::array<std::uint8_t, 5> ThreeByteStart{{0x00, 0x00, 0x01, 0x65, 0x01}};
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		validate_h264_annex_b_access_unit(byte_view(ThreeByteStart), true, summary));

	TargetVideoFramePayload frame;
	frame.stream_id = 1;
	frame.config_generation = 2;
	frame.video_frame_id = 3;
	frame.target_entity_id = 4;
	frame.presentation_time_us = 5;
	frame.encoded_frame_size = static_cast<std::uint32_t>(ValidIdr.size());
	frame.video_flags = VideoFrameFlagIdr | VideoFrameFlagTargetChanged;
	frame.annex_b_access_unit = byte_view(ValidIdr);
	std::vector<std::uint8_t> encoded(TargetVideoFramePrefixSize + ValidIdr.size());
	std::size_t written = 0;
	ASSERT_EQ(ValidationError::None, encode_target_video_frame_payload(frame, mutable_byte_view(encoded), written));
	TargetVideoFramePayload decoded;
	ASSERT_EQ(ValidationError::None, decode_target_video_frame_payload(byte_view(encoded), decoded));
	EXPECT_EQ(frame.video_frame_id, decoded.video_frame_id);
	EXPECT_EQ(frame.encoded_frame_size, decoded.annex_b_access_unit.size);
	frame.video_flags = VideoFrameFlagTargetChanged;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_target_video_frame_payload(frame));
}

TEST(TelemetryProtocolSpecializedViews, SpsRbspMatchesConfigProgressive420CroppingAndBt709Vui)
{
	const auto config = valid_config();
	AnnexBValidationSummary summary;
	const auto valid = configured_idr();
	ASSERT_EQ(ValidationError::None,
		validate_h264_annex_b_access_unit(byte_view(valid), true, config, summary));
	EXPECT_EQ(1U, summary.validated_sps_count);
	EXPECT_EQ(config.width, summary.displayed_width);
	EXPECT_EQ(config.height, summary.displayed_height);
	EXPECT_TRUE(summary.has_bt709_limited_range_vui);

	// 61 coded macroblocks (976 pixels) cropped by 8 chroma samples on
	// the right still produces the configured 960-pixel display width.
	const auto valid_crop = idr_with_sps({
		0x64, 0x00, 0x29, 0xac, 0xe8, 0x0f, 0x41, 0xe7, 0x89, 0xe6, 0xa0, 0x20, 0x20, 0x20, 0x40,
	});
	EXPECT_EQ(ValidationError::None,
		validate_h264_annex_b_access_unit(byte_view(valid_crop), true, config, summary));

	auto profile_mismatch = valid;
	profile_mismatch[5] = static_cast<std::uint8_t>(H264Profile::Main);
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		validate_h264_annex_b_access_unit(byte_view(profile_mismatch), true, config, summary));
	auto level_mismatch = valid;
	level_mismatch[7] = static_cast<std::uint8_t>(H264Level::Level4_0);
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		validate_h264_annex_b_access_unit(byte_view(level_mismatch), true, config, summary));
	auto baseline_config = config;
	baseline_config.codec_profile = H264Profile::ConstrainedBaseline;
	const auto constrained_baseline = idr_with_sps({
		0x42, 0x40, 0x29, 0xf4, 0x07, 0x80, 0xf3, 0x4d, 0x40, 0x40, 0x40, 0x40, 0x80,
	});
	EXPECT_EQ(ValidationError::None,
		validate_h264_annex_b_access_unit(byte_view(constrained_baseline), true, baseline_config, summary));
	const auto unconstrained_baseline = idr_with_sps({
		0x42, 0x00, 0x29, 0xf4, 0x07, 0x80, 0xf3, 0x4d, 0x40, 0x40, 0x40, 0x40, 0x80,
	});
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		validate_h264_annex_b_access_unit(byte_view(unconstrained_baseline), true, baseline_config, summary));

	const auto chroma_422 = idr_with_sps({
		0x64, 0x00, 0x29, 0xbc, 0xe8, 0x0f, 0x01, 0xe6, 0x9a, 0x80, 0x80, 0x80, 0x81,
	});
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		validate_h264_annex_b_access_unit(byte_view(chroma_422), true, config, summary));
	const auto interlaced = idr_with_sps({
		0x64, 0x00, 0x29, 0xac, 0xe8, 0x0f, 0x01, 0xe1, 0x4d, 0x40, 0x40, 0x40, 0x40, 0x80,
	});
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		validate_h264_annex_b_access_unit(byte_view(interlaced), true, config, summary));
	const auto wrong_dimensions = idr_with_sps({
		0x64, 0x00, 0x29, 0xac, 0xe8, 0x0e, 0xc1, 0xe6, 0x9a, 0x80, 0x80, 0x80, 0x81,
	});
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		validate_h264_annex_b_access_unit(byte_view(wrong_dimensions), true, config, summary));
	const auto wrong_crop = idr_with_sps({
		0x64, 0x00, 0x29, 0xac, 0xe8, 0x0f, 0x41, 0xe7, 0x88, 0xe6, 0xa0, 0x20, 0x20, 0x20, 0x40,
	});
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		validate_h264_annex_b_access_unit(byte_view(wrong_crop), true, config, summary));
	const auto full_range = idr_with_sps({
		0x64, 0x00, 0x29, 0xac, 0xe8, 0x0f, 0x01, 0xe6, 0x9b, 0x80, 0x80, 0x80, 0x81,
	});
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		validate_h264_annex_b_access_unit(byte_view(full_range), true, config, summary));
	const auto non_bt709 = idr_with_sps({
		0x64, 0x00, 0x29, 0xac, 0xe8, 0x0f, 0x01, 0xe6, 0x9a, 0x81, 0x00, 0x80, 0x81,
	});
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		validate_h264_annex_b_access_unit(byte_view(non_bt709), true, config, summary));

	TargetVideoFramePayload frame;
	frame.stream_id = config.stream_id;
	frame.config_generation = config.config_generation;
	frame.video_frame_id = 1;
	frame.target_entity_id = 42;
	frame.encoded_frame_size = static_cast<std::uint32_t>(valid.size());
	frame.video_flags = VideoFrameFlagIdr;
	frame.annex_b_access_unit = byte_view(valid);
	EXPECT_EQ(ValidationError::None, validate_target_video_frame_payload(frame, config));
}

TEST(TelemetryProtocolSpecializedViews, SpsRbspRejectsMalformedEmulationPrevention)
{
	const auto config = valid_config();
	AnnexBValidationSummary summary;
	// timing_info contains 00 00 03 00 in EBSP and decodes to 00 00 00.
	const auto escaped = idr_with_sps({
		0x64, 0x00, 0x29, 0xac, 0xe8, 0x0f, 0x01, 0xe6, 0x9a, 0x80, 0x80, 0x80, 0xa0, 0x00, 0x00, 0x03,
		0x00, 0x20, 0x00, 0x0e, 0xa6, 0x10, 0x80,
	});
	EXPECT_EQ(ValidationError::None,
		validate_h264_annex_b_access_unit(byte_view(escaped), true, config, summary));

	auto malformed = escaped;
	malformed[21] = 0x04; // 00 00 03 must be followed by a byte in 00..03.
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		validate_h264_annex_b_access_unit(byte_view(malformed), true, config, summary));
}

TEST(TelemetryProtocolSpecializedViews, FrameCodecAcceptsExactLogicalMaximumAndRejectsOneByteMoreBeforeParsing)
{
	const auto maximum_access_unit_size = MaxVideoMessageSize - TargetVideoFramePrefixSize;
	std::vector<std::uint8_t> access_unit(maximum_access_unit_size, 0x55U);
	access_unit[0] = 0;
	access_unit[1] = 0;
	access_unit[2] = 0;
	access_unit[3] = 1;
	access_unit[4] = 0x61;

	TargetVideoFramePayload frame;
	frame.stream_id = 1;
	frame.config_generation = 2;
	frame.video_frame_id = 3;
	frame.target_entity_id = 4;
	frame.encoded_frame_size = static_cast<std::uint32_t>(access_unit.size());
	frame.annex_b_access_unit = byte_view(access_unit);
	std::vector<std::uint8_t> encoded(MaxVideoMessageSize);
	std::size_t written = 0;
	ASSERT_EQ(ValidationError::None, encode_target_video_frame_payload(frame, mutable_byte_view(encoded), written));
	EXPECT_EQ(MaxVideoMessageSize, written);
	TargetVideoFramePayload decoded;
	EXPECT_EQ(ValidationError::None, decode_target_video_frame_payload(byte_view(encoded), decoded));

	access_unit.push_back(0x55U);
	frame.encoded_frame_size = static_cast<std::uint32_t>(access_unit.size());
	frame.annex_b_access_unit = byte_view(access_unit);
	EXPECT_EQ(ValidationError::MessageTooLarge, validate_target_video_frame_payload(frame));
	encoded.push_back(0);
	EXPECT_EQ(ValidationError::MessageTooLarge,
		decode_target_video_frame_payload(byte_view(encoded), decoded));
}

TEST(TelemetryProtocolSpecializedViews, ControlAndStatsPayloadsRejectReservedAndInvalidClockFields)
{
	TargetVideoKeyframeRequestPayload keyframe;
	keyframe.request_id = 1;
	keyframe.stream_id = 2;
	keyframe.config_generation = 3;
	keyframe.needed_before_producer_time_us = 1'400'000;
	keyframe.reason = VideoKeyframeReason::PacketLoss;
	bool idr_required = false;
	EXPECT_EQ(ValidationError::None,
		validate_target_video_keyframe_request_clock(keyframe, true, 1'000'000, idr_required));
	EXPECT_TRUE(idr_required);
	std::array<std::uint8_t, TargetVideoKeyframeRequestPayloadSize> keyframe_bytes{};
	std::size_t written = 0;
	ASSERT_EQ(ValidationError::None,
		encode_target_video_keyframe_request_payload(keyframe, mutable_byte_view(keyframe_bytes), written));
	TargetVideoKeyframeRequestPayload decoded_keyframe;
	ASSERT_EQ(ValidationError::None,
		decode_target_video_keyframe_request_payload(byte_view(keyframe_bytes), decoded_keyframe));
	EXPECT_EQ(keyframe.needed_before_producer_time_us, decoded_keyframe.needed_before_producer_time_us);
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		validate_target_video_keyframe_request_clock(keyframe, false, 1'000'000, idr_required));
	EXPECT_FALSE(idr_required);
	keyframe.needed_before_producer_time_us = 0;
	EXPECT_EQ(ValidationError::None,
		validate_target_video_keyframe_request_clock(keyframe, false, 1'000'000, idr_required));
	EXPECT_TRUE(idr_required);
	keyframe.needed_before_producer_time_us = 999'999;
	EXPECT_EQ(ValidationError::None,
		validate_target_video_keyframe_request_clock(keyframe, true, 1'000'000, idr_required));
	EXPECT_FALSE(idr_required);
	keyframe.needed_before_producer_time_us = 1'000'000;
	EXPECT_EQ(ValidationError::None,
		validate_target_video_keyframe_request_clock(keyframe, true, 1'000'000, idr_required));
	EXPECT_FALSE(idr_required);
	keyframe.needed_before_producer_time_us = 1'500'001;
	EXPECT_EQ(ValidationError::OutOfRange,
		validate_target_video_keyframe_request_clock(keyframe, true, 1'000'000, idr_required));
	EXPECT_FALSE(idr_required);

	TargetVideoStopPayload stop;
	stop.request_id = 1;
	stop.stream_id = 2;
	stop.config_generation = 3;
	EXPECT_EQ(ValidationError::None, validate_target_video_stop_payload(stop));
	stop.producer_time_us = 4;
	EXPECT_EQ(ValidationError::InvalidStateTransition, validate_target_video_stop_payload(stop));
	stop.stop_flags = VideoStopFlagConfirmation;
	EXPECT_EQ(ValidationError::None, validate_target_video_stop_payload(stop));
	std::array<std::uint8_t, TargetVideoStopPayloadSize> stop_bytes{};
	ASSERT_EQ(ValidationError::None, encode_target_video_stop_payload(stop, mutable_byte_view(stop_bytes), written));
	TargetVideoStopPayload decoded_stop;
	ASSERT_EQ(ValidationError::None, decode_target_video_stop_payload(byte_view(stop_bytes), decoded_stop));
	EXPECT_EQ(stop.producer_time_us, decoded_stop.producer_time_us);

	TargetVideoStatsPayload stats;
	stats.stream_id = 1;
	stats.config_generation = 2;
	stats.report_id = 3;
	stats.estimated_loss_ppm = MaximumVideoEstimatedLossPpm;
	stats.report_interval_ms = MinimumVideoStatsIntervalMs;
	EXPECT_EQ(ValidationError::None, validate_target_video_stats_payload(stats));
	std::array<std::uint8_t, TargetVideoStatsPayloadSize> stats_bytes{};
	ASSERT_EQ(ValidationError::None, encode_target_video_stats_payload(stats, mutable_byte_view(stats_bytes), written));
	TargetVideoStatsPayload decoded_stats;
	ASSERT_EQ(ValidationError::None, decode_target_video_stats_payload(byte_view(stats_bytes), decoded_stats));
	EXPECT_EQ(stats.report_id, decoded_stats.report_id);
	stats.stats_flags = 0x8000;
	EXPECT_EQ(ValidationError::ReservedFlag, validate_target_video_stats_payload(stats));
}

TEST(TelemetryProtocolSpecializedViews, EncodersDoNotPartiallyPublishWhenOutputIsTooSmall)
{
	CommBundleOffer offer;
	offer.bundle_version = CommBundleVersionV1;
	offer.supported_delivered_formats = DeliveredFormatBitPng;
	offer.bundle_hash = nonzero_digest();
	std::array<std::uint8_t, CommBundleOfferSize - 1U> output{};
	output.fill(0xa5U);
	const auto before = output;
	std::size_t written = 999;
	EXPECT_EQ(ValidationError::InternalSerializationError,
		encode_comm_bundle_offer(offer, mutable_byte_view(output), written));
	EXPECT_EQ(0U, written);
	EXPECT_EQ(before, output);
}

TEST(TelemetryProtocolSpecializedViews, VideoTokenBucketRefillIsFrequencyIndependentAndReconfigureAddsNoBurst)
{
	VideoTokenBucket once;
	VideoTokenBucket stepped;
	ASSERT_EQ(ValidationError::None, VideoTokenBucket::configure(4000, 100, once));
	ASSERT_EQ(ValidationError::None, VideoTokenBucket::configure(4000, 100, stepped));
	ASSERT_EQ(VideoTokenBucketResult::Allowed, once.try_consume(100, MaxDatagramSize));
	ASSERT_EQ(VideoTokenBucketResult::Allowed, stepped.try_consume(100, MaxDatagramSize));
	EXPECT_EQ(VideoTokenBucketResult::Allowed, once.advance(123'557));
	for (std::uint64_t now = 101; now <= 123'557; ++now) {
		ASSERT_EQ(VideoTokenBucketResult::Allowed, stepped.advance(now));
	}
	EXPECT_EQ(once.tokens_bytes(), stepped.tokens_bytes());
	EXPECT_EQ(once.refill_remainder(), stepped.refill_remainder());

	const auto tokens_before = once.tokens_bytes();
	EXPECT_EQ(VideoTokenBucketResult::Allowed, once.reconfigure(8000, 123'557));
	EXPECT_EQ(tokens_before, once.tokens_bytes());
	EXPECT_EQ(VideoTokenBucketResult::ClockRegressed, once.advance(123'556));
	EXPECT_TRUE(once.clock_regressed());
}

} // namespace
