#pragma once

#include "telemetry/protocol/telemetry_protocol_constants.h"
#include "telemetry/protocol/telemetry_protocol_types.h"
#include "telemetry/protocol/telemetry_sha256.h"

#include <cstddef>
#include <cstdint>

namespace telemetry::protocol {

constexpr std::size_t CommBundleOfferSize = 40;
constexpr std::size_t CommBundleSelectionSize = 40;
constexpr std::size_t CommAssetManifestPrefixSize = 64;
constexpr std::size_t CommAssetEntryPrefixSize = 72;
constexpr std::size_t MaximumCommAssetRecordPayloadSize = 65'535;
constexpr std::uint32_t MaximumCommAssetCount = 4096;
constexpr std::uint16_t MaximumCommLogicalNameLength = 255;
constexpr std::uint16_t MaximumCommFilePathLength = 1024;
constexpr std::size_t CommViewStatePayloadSize = 60;
constexpr std::size_t CommViewEventPayloadSize = 68;
constexpr std::size_t TargetVideoSubscribePayloadSize = 44;
constexpr std::size_t TargetVideoConfigPayloadSize = 36;
constexpr std::size_t TargetVideoFramePrefixSize = 36;
constexpr std::size_t TargetVideoKeyframeRequestPayloadSize = 32;
constexpr std::size_t TargetVideoStopPayloadSize = 24;
constexpr std::size_t TargetVideoStatsPayloadSize = 72;

constexpr std::uint16_t CommBundleVersionV1 = 1;
constexpr std::uint64_t MaximumCommDurationUs = 86'400'000'000ULL;
constexpr float MinimumCommPlaybackRate = -64.0F;
constexpr float MaximumCommPlaybackRate = 64.0F;

struct CommAssetEntryView {
	std::uint16_t entry_length = 0;
	std::uint64_t asset_id = 0;
	Sha256Digest content_hash{};
	SourceFormat source_format = SourceFormat::Invalid;
	DeliveredFormat delivered_format = DeliveredFormat::Invalid;
	AlphaMode alpha_mode = AlphaMode::None;
	AssetTimingMode timing_mode = AssetTimingMode::FormatIntrinsic;
	std::uint16_t width = 0;
	std::uint16_t height = 0;
	std::uint32_t frame_count = 0;
	std::uint64_t duration_us = 0;
	ByteView logical_name;
	ByteView file_path;
	std::uint32_t frame_duration_count = 0;
	ByteView frame_durations_le;
};

struct CommAssetManifestPayloadView {
	std::uint16_t bundle_version = 0;
	std::uint16_t manifest_flags = ManifestFlagNone;
	Sha256Digest bundle_hash{};
	std::uint64_t frame_asset_id = 0;
	std::uint64_t placeholder_asset_id = 0;
	std::uint32_t total_asset_count = 0;
	std::uint32_t first_asset_index = 0;
	std::uint16_t entry_count = 0;
	ByteView converter_id;
	ByteView converter_version;
	ByteView encoded_entries;
};

std::uint64_t comm_asset_id_from_content_hash(const Sha256Digest& content_hash) noexcept;
ValidationError validate_portable_bundle_path(ByteView path) noexcept;
ValidationError decode_comm_asset_entry(ByteView input,
	CommAssetEntryView& entry,
	std::size_t& consumed) noexcept;
ValidationError validate_comm_asset_entry_payload(ByteView input) noexcept;
ValidationError decode_comm_asset_manifest_payload(ByteView input, CommAssetManifestPayloadView& payload) noexcept;
ValidationError validate_comm_asset_manifest_record_payload(ByteView input) noexcept;

constexpr std::uint16_t MinimumVideoDimension = 64;
constexpr std::uint16_t MaximumVideoDimension = 1024;
constexpr std::uint16_t MinimumVideoFps = 1;
constexpr std::uint16_t MaximumVideoFps = 20;
constexpr std::uint32_t MinimumVideoBitrateKbps = 128;
constexpr std::uint32_t MaximumVideoBitrateKbps = 8000;
constexpr std::uint16_t MaximumVideoGopDurationMs = 1000;
constexpr std::uint16_t MaximumVideoIdrRecoveryWindowMs = 500;
constexpr std::uint32_t MaximumVideoEstimatedLossPpm = 1'000'000;
constexpr std::uint16_t MinimumVideoStatsIntervalMs = 500;
constexpr std::uint16_t MaximumVideoStatsIntervalMs = 5000;
constexpr std::uint64_t MaximumVideoKeyframeDeadlineLeadUs = 500'000;

struct CommBundleOffer {
	std::uint16_t bundle_version = 0;
	std::uint32_t supported_delivered_formats = 0;
	Sha256Digest bundle_hash{};
};

struct CommBundleSelection {
	CommNegotiationResult result = CommNegotiationResult::NotRequested;
	std::uint16_t bundle_version = 0;
	std::uint32_t required_delivered_formats = 0;
	Sha256Digest required_bundle_hash{};
};

ValidationError validate_comm_bundle_offer(const CommBundleOffer& payload) noexcept;
ValidationError validate_comm_bundle_selection(const CommBundleSelection& payload) noexcept;
ValidationError encode_comm_bundle_offer(const CommBundleOffer& payload,
	MutableByteView output,
	std::size_t& written) noexcept;
ValidationError decode_comm_bundle_offer(ByteView input, CommBundleOffer& payload) noexcept;
ValidationError encode_comm_bundle_selection(const CommBundleSelection& payload,
	MutableByteView output,
	std::size_t& written) noexcept;
ValidationError decode_comm_bundle_selection(ByteView input, CommBundleSelection& payload) noexcept;

struct CommViewStatePayload {
	bool active = false;
	CommPlaybackMode playback_mode = CommPlaybackMode::Once;
	CommColorMode color_mode = CommColorMode::HudTint;
	CommStopReason stop_reason = CommStopReason::None;
	std::uint64_t playback_id = 0;
	std::uint32_t engine_message_id = 0;
	std::uint64_t sender_entity_id = 0;
	std::uint64_t head_asset_id = 0;
	std::uint64_t producer_sample_time_us = 0;
	std::uint64_t animation_time_us = 0;
	std::uint64_t duration_us = 0;
	float playback_rate = 0.0F;
};

struct CommViewEventPayload {
	std::uint64_t event_id = 0;
	CommEventKind event_kind = CommEventKind::Start;
	CommPlaybackMode playback_mode = CommPlaybackMode::Once;
	CommColorMode color_mode = CommColorMode::HudTint;
	CommStopReason stop_reason = CommStopReason::None;
	std::uint64_t playback_id = 0;
	std::uint32_t engine_message_id = 0;
	std::uint64_t sender_entity_id = 0;
	std::uint64_t head_asset_id = 0;
	std::uint64_t producer_sample_time_us = 0;
	std::uint64_t animation_time_us = 0;
	std::uint64_t duration_us = 0;
	float playback_rate = 0.0F;
};

ValidationError validate_comm_view_state_payload(const CommViewStatePayload& payload) noexcept;
ValidationError validate_comm_view_event_payload(const CommViewEventPayload& payload) noexcept;
ValidationError encode_comm_view_state_payload(const CommViewStatePayload& payload,
	MutableByteView output,
	std::size_t& written) noexcept;
ValidationError decode_comm_view_state_payload(ByteView input, CommViewStatePayload& payload) noexcept;
ValidationError validate_comm_view_state_record_payload(ByteView input) noexcept;
ValidationError encode_comm_view_event_payload(const CommViewEventPayload& payload,
	MutableByteView output,
	std::size_t& written) noexcept;
ValidationError decode_comm_view_event_payload(ByteView input, CommViewEventPayload& payload) noexcept;
ValidationError validate_comm_view_event_record_payload(ByteView input) noexcept;

struct TargetVideoSubscribePayload {
	std::uint32_t request_id = 0;
	std::uint16_t max_width = 0;
	std::uint16_t max_height = 0;
	std::uint16_t preferred_width = 0;
	std::uint16_t preferred_height = 0;
	std::uint16_t max_fps = 0;
	std::uint16_t preferred_fps = 0;
	std::uint32_t max_bitrate_kbps = 0;
	std::uint32_t preferred_bitrate_kbps = 0;
	std::uint32_t supported_h264_profiles = 0;
	std::uint32_t supported_h264_levels = 0;
	std::uint32_t overlay_capabilities = 0;
	std::uint32_t acceptable_render_profiles = 0;
	RenderProfile preferred_render_profile = RenderProfile::Invalid;
};

struct TargetVideoConfigPayload {
	std::uint32_t request_id = 0;
	VideoConfigResult result = VideoConfigResult::Accepted;
	Codec codec = Codec::Invalid;
	H264Profile codec_profile = H264Profile::ConstrainedBaseline;
	H264Level codec_level = H264Level::Level3_1;
	std::uint32_t stream_id = 0;
	std::uint32_t config_generation = 0;
	PixelFormat pixel_format = PixelFormat::Invalid;
	RenderProfile render_profile = RenderProfile::Invalid;
	OverlayMode overlay_mode = OverlayMode::Invalid;
	RecoveryMode recovery_mode = RecoveryMode::Invalid;
	std::uint16_t width = 0;
	std::uint16_t height = 0;
	std::uint16_t fps_num = 0;
	std::uint16_t fps_den = 0;
	std::uint32_t bitrate_kbps = 0;
	std::uint16_t gop_duration_ms = 0;
	std::uint16_t idr_recovery_window_ms = 0;
};

struct TargetVideoFramePayload {
	std::uint32_t stream_id = 0;
	std::uint32_t config_generation = 0;
	std::uint32_t video_frame_id = 0;
	std::uint64_t target_entity_id = 0;
	std::uint64_t presentation_time_us = 0;
	std::uint32_t encoded_frame_size = 0;
	std::uint16_t video_flags = VideoFrameFlagNone;
	ByteView annex_b_access_unit;
};

struct TargetVideoKeyframeRequestPayload {
	std::uint32_t request_id = 0;
	std::uint32_t stream_id = 0;
	std::uint32_t config_generation = 0;
	std::uint32_t last_decodable_frame_id = 0;
	std::uint64_t needed_before_producer_time_us = 0;
	VideoKeyframeReason reason = VideoKeyframeReason::PacketLoss;
};

struct TargetVideoStopPayload {
	std::uint32_t request_id = 0;
	std::uint32_t stream_id = 0;
	std::uint32_t config_generation = 0;
	VideoStopReason reason = VideoStopReason::ClientUnsubscribe;
	std::uint8_t stop_flags = VideoStopFlagNone;
	std::uint64_t producer_time_us = 0;
};

struct TargetVideoStatsPayload {
	std::uint32_t stream_id = 0;
	std::uint32_t config_generation = 0;
	std::uint32_t report_id = 0;
	std::uint32_t highest_received_frame_id = 0;
	std::uint32_t highest_decoded_frame_id = 0;
	std::uint32_t highest_presented_frame_id = 0;
	std::uint32_t received_frames = 0;
	std::uint32_t decoded_frames = 0;
	std::uint32_t presented_frames = 0;
	std::uint32_t dropped_frames = 0;
	std::uint32_t missing_fragments = 0;
	std::uint32_t decoder_errors = 0;
	std::uint32_t estimated_loss_ppm = 0;
	std::uint32_t jitter_us = 0;
	std::uint32_t decode_latency_us = 0;
	std::uint32_t presentation_latency_us = 0;
	std::uint32_t buffered_duration_us = 0;
	std::uint16_t report_interval_ms = 0;
	std::uint16_t stats_flags = VideoStatsFlagNone;
};

struct AvcLevelLimits {
	std::uint32_t max_fs = 0;
	std::uint32_t max_mbps = 0;
	std::uint32_t max_br_kbps = 0;
};

ValidationError avc_level_limits(H264Level level, AvcLevelLimits& limits) noexcept;
ValidationError validate_avc_level_configuration(H264Level level,
	std::uint16_t width,
	std::uint16_t height,
	std::uint16_t fps_num,
	std::uint16_t fps_den,
	std::uint32_t bitrate_kbps) noexcept;

struct AnnexBValidationSummary {
	std::uint32_t nal_unit_count = 0;
	std::uint32_t validated_sps_count = 0;
	std::uint16_t displayed_width = 0;
	std::uint16_t displayed_height = 0;
	bool has_sps = false;
	bool has_pps = false;
	bool has_idr_slice = false;
	bool has_bt709_limited_range_vui = false;
};

ValidationError validate_h264_annex_b_access_unit(ByteView access_unit,
	bool declared_idr,
	AnnexBValidationSummary& summary) noexcept;
ValidationError validate_h264_annex_b_access_unit(ByteView access_unit,
	bool declared_idr,
	const TargetVideoConfigPayload& config,
	AnnexBValidationSummary& summary) noexcept;

ValidationError validate_target_video_subscribe_payload(const TargetVideoSubscribePayload& payload) noexcept;
ValidationError validate_target_video_config_payload(const TargetVideoConfigPayload& payload) noexcept;
ValidationError validate_target_video_config_for_subscribe(const TargetVideoConfigPayload& config,
	const TargetVideoSubscribePayload& subscribe) noexcept;
ValidationError validate_target_video_config_for_subscribe(const TargetVideoConfigPayload& config,
	const TargetVideoSubscribePayload& subscribe,
	bool h264_codec_capability_negotiated) noexcept;
ValidationError validate_target_video_frame_payload(const TargetVideoFramePayload& payload) noexcept;
ValidationError validate_target_video_frame_payload(const TargetVideoFramePayload& payload,
	const TargetVideoConfigPayload& config) noexcept;
ValidationError
validate_target_video_keyframe_request_payload(const TargetVideoKeyframeRequestPayload& payload) noexcept;
ValidationError validate_target_video_keyframe_request_clock(const TargetVideoKeyframeRequestPayload& payload,
	bool clock_filter_valid,
	std::uint64_t estimated_producer_now_us) noexcept;
ValidationError validate_target_video_keyframe_request_clock(const TargetVideoKeyframeRequestPayload& payload,
	bool clock_filter_valid,
	std::uint64_t estimated_producer_now_us,
	bool& idr_required) noexcept;
ValidationError validate_target_video_stop_payload(const TargetVideoStopPayload& payload) noexcept;
ValidationError validate_target_video_stats_payload(const TargetVideoStatsPayload& payload) noexcept;

ValidationError encode_target_video_subscribe_payload(const TargetVideoSubscribePayload& payload,
	MutableByteView output,
	std::size_t& written) noexcept;
ValidationError decode_target_video_subscribe_payload(ByteView input, TargetVideoSubscribePayload& payload) noexcept;
ValidationError encode_target_video_config_payload(const TargetVideoConfigPayload& payload,
	MutableByteView output,
	std::size_t& written) noexcept;
ValidationError decode_target_video_config_payload(ByteView input, TargetVideoConfigPayload& payload) noexcept;
ValidationError encode_target_video_frame_payload(const TargetVideoFramePayload& payload,
	MutableByteView output,
	std::size_t& written) noexcept;
ValidationError decode_target_video_frame_payload(ByteView input, TargetVideoFramePayload& payload) noexcept;
ValidationError encode_target_video_keyframe_request_payload(const TargetVideoKeyframeRequestPayload& payload,
	MutableByteView output,
	std::size_t& written) noexcept;
ValidationError decode_target_video_keyframe_request_payload(ByteView input,
	TargetVideoKeyframeRequestPayload& payload) noexcept;
ValidationError encode_target_video_stop_payload(const TargetVideoStopPayload& payload,
	MutableByteView output,
	std::size_t& written) noexcept;
ValidationError decode_target_video_stop_payload(ByteView input, TargetVideoStopPayload& payload) noexcept;
ValidationError encode_target_video_stats_payload(const TargetVideoStatsPayload& payload,
	MutableByteView output,
	std::size_t& written) noexcept;
ValidationError decode_target_video_stats_payload(ByteView input, TargetVideoStatsPayload& payload) noexcept;

constexpr std::uint64_t VideoBucketRefillDenominator = 8'000'000ULL;
constexpr std::uint64_t MinimumVideoBucketCapacityBytes = 262'144ULL;
constexpr std::uint64_t MaximumVideoBucketCapacityBytes = 2'097'152ULL;

std::uint64_t video_bucket_capacity_bytes(std::uint32_t bitrate_kbps) noexcept;

enum class VideoTokenBucketResult : std::uint8_t {
	Allowed,
	InsufficientTokens,
	InvalidBitrate,
	InvalidDatagramSize,
	ClockRegressed,
	Unconfigured,
};

class VideoTokenBucket final {
  public:
	static ValidationError configure(std::uint32_t bitrate_kbps,
		std::uint64_t initial_time_us,
		VideoTokenBucket& bucket) noexcept;

	VideoTokenBucketResult advance(std::uint64_t now_us) noexcept;
	VideoTokenBucketResult try_consume(std::uint64_t now_us, std::size_t datagram_bytes) noexcept;
	VideoTokenBucketResult reconfigure(std::uint32_t bitrate_kbps, std::uint64_t now_us) noexcept;

	std::uint32_t bitrate_kbps() const noexcept
	{
		return m_bitrate_kbps;
	}
	std::uint64_t capacity_bytes() const noexcept
	{
		return m_capacity_bytes;
	}
	std::uint64_t tokens_bytes() const noexcept
	{
		return m_tokens_bytes;
	}
	std::uint64_t refill_remainder() const noexcept
	{
		return m_refill_remainder;
	}
	std::uint64_t last_refill_time_us() const noexcept
	{
		return m_last_refill_time_us;
	}
	bool clock_regressed() const noexcept
	{
		return m_clock_regressed;
	}

  private:
	std::uint32_t m_bitrate_kbps = 0;
	std::uint64_t m_rate_bits_per_s = 0;
	std::uint64_t m_capacity_bytes = 0;
	std::uint64_t m_tokens_bytes = 0;
	std::uint64_t m_refill_remainder = 0;
	std::uint64_t m_last_refill_time_us = 0;
	bool m_configured = false;
	bool m_clock_regressed = false;
};

} // namespace telemetry::protocol
