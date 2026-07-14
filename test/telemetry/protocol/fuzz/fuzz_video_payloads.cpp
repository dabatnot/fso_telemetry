#include "fuzz_common.h"

#include "telemetry/protocol/telemetry_specialized_views.h"

#include <array>

using namespace telemetry::protocol;

namespace {

TargetVideoConfigPayload reference_video_config()
{
	TargetVideoConfigPayload config;
	config.request_id = 1;
	config.result = VideoConfigResult::Accepted;
	config.codec = Codec::H264AnnexB;
	config.codec_profile = H264Profile::High;
	config.codec_level = H264Level::Level4_1;
	config.stream_id = 1;
	config.config_generation = 1;
	config.pixel_format = PixelFormat::Yuv420p8;
	config.render_profile = RenderProfile::MfdHigh;
	config.overlay_mode = OverlayMode::Client;
	config.recovery_mode = RecoveryMode::IdrSelectiveRetransmit;
	config.width = 960;
	config.height = 960;
	config.fps_num = 15;
	config.fps_den = 1;
	config.bitrate_kbps = 4000;
	config.gop_duration_ms = 1000;
	config.idr_recovery_window_ms = 500;
	return config;
}

template <typename Payload, typename Decoder, typename Encoder>
void decode_and_roundtrip(ByteView input, Decoder decoder, Encoder encoder)
{
	Payload payload;
	if (decoder(input, payload) != ValidationError::None) {
		return;
	}
	std::array<std::uint8_t, 128> output{};
	std::size_t written = 0;
	static_cast<void>(encoder(payload, MutableByteView{output.data(), output.size()}, written));
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
	const auto config = reference_video_config();
	if (data == nullptr || size == 0) {
		AnnexBValidationSummary summary;
		static_cast<void>(validate_h264_annex_b_access_unit(ByteView{}, false, summary));
		static_cast<void>(validate_h264_annex_b_access_unit(ByteView{}, false, config, summary));
		return 0;
	}
	const auto payload = fuzz::tail(data, size, 1U);
	switch (static_cast<MessageType>(data[0])) {
	case MessageType::TargetVideoSubscribe:
		decode_and_roundtrip<TargetVideoSubscribePayload>(payload,
			decode_target_video_subscribe_payload,
			encode_target_video_subscribe_payload);
		break;
	case MessageType::TargetVideoConfig:
		decode_and_roundtrip<TargetVideoConfigPayload>(payload,
			decode_target_video_config_payload,
			encode_target_video_config_payload);
		break;
	case MessageType::TargetVideoFrame: {
		TargetVideoFramePayload frame;
		if (decode_target_video_frame_payload(payload, frame) == ValidationError::None) {
			AnnexBValidationSummary summary;
			static_cast<void>(validate_h264_annex_b_access_unit(frame.annex_b_access_unit,
				(frame.video_flags & VideoFrameFlagIdr) != 0,
				summary));
			static_cast<void>(validate_h264_annex_b_access_unit(frame.annex_b_access_unit,
				(frame.video_flags & VideoFrameFlagIdr) != 0,
				config,
				summary));
		}
		break;
	}
	case MessageType::TargetVideoKeyframeRequest:
		decode_and_roundtrip<TargetVideoKeyframeRequestPayload>(payload,
			decode_target_video_keyframe_request_payload,
			encode_target_video_keyframe_request_payload);
		break;
	case MessageType::TargetVideoStop:
		decode_and_roundtrip<TargetVideoStopPayload>(payload,
			decode_target_video_stop_payload,
			encode_target_video_stop_payload);
		break;
	case MessageType::TargetVideoStats:
		decode_and_roundtrip<TargetVideoStatsPayload>(payload,
			decode_target_video_stats_payload,
			encode_target_video_stats_payload);
		break;
	default: {
		AnnexBValidationSummary summary;
		static_cast<void>(validate_h264_annex_b_access_unit(payload, (data[0] & 1U) != 0, summary));
		static_cast<void>(
			validate_h264_annex_b_access_unit(payload, (data[0] & 1U) != 0, config, summary));
		break;
	}
	}
	return 0;
}
