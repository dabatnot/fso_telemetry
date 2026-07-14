#include "telemetry/protocol/telemetry_specialized_lifecycle.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

using namespace telemetry::protocol;

constexpr std::array<std::uint8_t, 30> ValidIdr{{
	0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x29, 0xac, 0xe8, 0x0f, 0x01, 0xe6, 0x9a, 0x80, 0x80,
	0x80, 0x81,
	0x00, 0x00, 0x00, 0x01, 0x68, 0x22,
	0x00, 0x00, 0x00, 0x01, 0x65, 0x33,
}};
constexpr std::array<std::uint8_t, 6> ValidInterframe{{0x00, 0x00, 0x00, 0x01, 0x61, 0x44}};

template <typename Container>
ByteView byte_view(const Container& bytes)
{
	return ByteView{bytes.data(), bytes.size()};
}

TargetVideoConfigPayload valid_config(std::uint32_t stream_id = 9,
	std::uint32_t config_generation = 1,
	std::uint32_t request_id = 7)
{
	TargetVideoConfigPayload payload;
	payload.request_id = request_id;
	payload.result = VideoConfigResult::Accepted;
	payload.codec = Codec::H264AnnexB;
	payload.codec_profile = H264Profile::High;
	payload.codec_level = H264Level::Level4_1;
	payload.stream_id = stream_id;
	payload.config_generation = config_generation;
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

TargetVideoFramePayload valid_frame(std::uint32_t frame_id,
	std::uint32_t generation,
	std::uint64_t target,
	bool idr)
{
	TargetVideoFramePayload frame;
	frame.stream_id = 9;
	frame.config_generation = generation;
	frame.video_frame_id = frame_id;
	frame.target_entity_id = target;
	frame.presentation_time_us = 1000U + frame_id;
	frame.video_flags = idr ? VideoFrameFlagIdr : VideoFrameFlagNone;
	frame.annex_b_access_unit = idr ? byte_view(ValidIdr) : byte_view(ValidInterframe);
	frame.encoded_frame_size = static_cast<std::uint32_t>(frame.annex_b_access_unit.size);
	return frame;
}

CommAssetRuntimeStatus valid_comm_asset(std::uint64_t asset_id = 4U, std::uint64_t duration_us = 1000U)
{
	CommAssetRuntimeStatus asset;
	asset.asset_id = asset_id;
	asset.duration_us = duration_us;
	asset.available = true;
	asset.content_valid = true;
	return asset;
}

CommViewEventPayload comm_start(std::uint64_t event_id,
	std::uint64_t playback_id,
	std::uint64_t sample_time_us = 1000U)
{
	CommViewEventPayload event;
	event.event_id = event_id;
	event.event_kind = CommEventKind::Start;
	event.playback_id = playback_id;
	event.engine_message_id = 2U;
	event.sender_entity_id = 3U;
	event.head_asset_id = 4U;
	event.producer_sample_time_us = sample_time_us;
	event.animation_time_us = 100U;
	event.duration_us = 1000U;
	event.playback_rate = 1.0F;
	return event;
}

CommViewEventPayload comm_stop(std::uint64_t event_id,
	std::uint64_t playback_id,
	CommStopReason reason = CommStopReason::Completed,
	std::uint64_t sample_time_us = 2000U)
{
	CommViewEventPayload event;
	event.event_id = event_id;
	event.event_kind = CommEventKind::Stop;
	event.stop_reason = reason;
	event.playback_id = playback_id;
	event.producer_sample_time_us = sample_time_us;
	return event;
}

CommViewStatePayload comm_state(std::uint64_t playback_id, std::uint64_t sample_time_us = 1000U)
{
	CommViewStatePayload state;
	state.active = true;
	state.playback_id = playback_id;
	state.engine_message_id = 2U;
	state.sender_entity_id = 3U;
	state.head_asset_id = 4U;
	state.producer_sample_time_us = sample_time_us;
	state.animation_time_us = 100U;
	state.duration_us = 1000U;
	state.playback_rate = 1.0F;
	return state;
}

TEST(TelemetryProtocolSpecializedLifecycle, CommNegotiationStatesAndStartWaitForTheValidatedBundle)
{
	CommViewClientLifecycle lifecycle;
	lifecycle.reset_session(false, true, CommNegotiationResult::Accepted);
	EXPECT_EQ(CommViewClientLifecycleState::Unsupported, lifecycle.state());
	lifecycle.reset_session(true, false, CommNegotiationResult::SourceUnavailable);
	EXPECT_EQ(CommViewClientLifecycleState::SourceUnavailable, lifecycle.state());
	lifecycle.reset_session(true, true, CommNegotiationResult::BundleHashMismatch);
	EXPECT_EQ(CommViewClientLifecycleState::BundleMismatch, lifecycle.state());
	EXPECT_EQ(CommViewClientLifecycleResult::BundleMismatch,
		lifecycle.apply_event(comm_start(1U, 1U), valid_comm_asset()));
	EXPECT_FALSE(lifecycle.has_deferred_playback());

	lifecycle.reset_session(true, true, CommNegotiationResult::Accepted);
	EXPECT_EQ(CommViewClientLifecycleState::BundleMismatch, lifecycle.state());
	const auto start = comm_start(1U, 1U);
	EXPECT_EQ(CommViewClientLifecycleResult::Deferred, lifecycle.apply_event(start, valid_comm_asset()));
	EXPECT_TRUE(lifecycle.has_deferred_playback());
	EXPECT_FALSE(lifecycle.has_playback());
	EXPECT_EQ(CommViewClientLifecycleResult::Duplicate, lifecycle.apply_event(start, valid_comm_asset()));
	auto conflicting = start;
	conflicting.animation_time_us += 1U;
	EXPECT_EQ(CommViewClientLifecycleResult::ConflictingDuplicate,
		lifecycle.apply_event(conflicting, valid_comm_asset()));
	EXPECT_EQ(CommViewClientLifecycleResult::Applied, lifecycle.install_bundle(1U, valid_comm_asset()));
	EXPECT_EQ(CommViewClientLifecycleState::Active, lifecycle.state());
	EXPECT_TRUE(lifecycle.has_playback());
	EXPECT_FALSE(lifecycle.has_deferred_playback());
}

TEST(TelemetryProtocolSpecializedLifecycle, CommProjectionSupportsPauseAccelerationAndEuclideanReverseLoop)
{
	CommViewClientLifecycle lifecycle;
	lifecycle.reset_session(true, true, CommNegotiationResult::Accepted);
	ASSERT_EQ(CommViewClientLifecycleResult::Applied, lifecycle.install_bundle(1U));

	auto state = comm_state(1U, 1000U);
	state.playback_rate = 0.0F;
	state.animation_time_us = 321U;
	ASSERT_EQ(CommViewClientLifecycleResult::Applied,
		lifecycle.apply_state(state, valid_comm_asset()));
	auto projection = lifecycle.project(9'000U);
	EXPECT_EQ(CommViewProjectionResult::Presentable, projection.result);
	EXPECT_DOUBLE_EQ(321.0, projection.animation_time_us);

	state.producer_sample_time_us = 2000U;
	state.animation_time_us = 100U;
	state.playback_mode = CommPlaybackMode::Loop;
	state.playback_rate = -1.0F;
	ASSERT_EQ(CommViewClientLifecycleResult::Applied,
		lifecycle.apply_state(state, valid_comm_asset()));
	projection = lifecycle.project(2200U);
	EXPECT_DOUBLE_EQ(900.0, projection.animation_time_us);

	state.producer_sample_time_us = 3000U;
	state.animation_time_us = 100U;
	state.playback_mode = CommPlaybackMode::Once;
	state.playback_rate = 64.0F;
	ASSERT_EQ(CommViewClientLifecycleResult::Applied,
		lifecycle.apply_state(state, valid_comm_asset()));
	projection = lifecycle.project(4000U);
	EXPECT_DOUBLE_EQ(1000.0, projection.animation_time_us);
}

TEST(TelemetryProtocolSpecializedLifecycle, CommLocalAssetFailureUsesPlaceholderAndCanRecover)
{
	CommViewClientLifecycle lifecycle;
	lifecycle.reset_session(true, true, CommNegotiationResult::Accepted);
	ASSERT_EQ(CommViewClientLifecycleResult::Applied, lifecycle.install_bundle(1U));
	auto missing = valid_comm_asset();
	missing.available = false;
	EXPECT_EQ(CommViewClientLifecycleResult::Placeholder,
		lifecycle.apply_state(comm_state(1U), missing));
	EXPECT_EQ(CommViewClientLifecycleState::Placeholder, lifecycle.state());
	EXPECT_EQ(CommViewProjectionResult::Placeholder, lifecycle.project(2000U).result);
	EXPECT_EQ(CommViewClientLifecycleResult::Applied,
		lifecycle.update_asset_status(valid_comm_asset()));
	EXPECT_EQ(CommViewClientLifecycleState::Active, lifecycle.state());

	auto wrong_duration = valid_comm_asset();
	wrong_duration.duration_us += 1U;
	EXPECT_EQ(CommViewClientLifecycleResult::Placeholder,
		lifecycle.update_asset_status(wrong_duration));
	EXPECT_EQ(CommViewClientLifecycleState::Placeholder, lifecycle.state());
}

TEST(TelemetryProtocolSpecializedLifecycle, CommReplacementAndDelayedStopAreIdempotent)
{
	CommViewClientLifecycle lifecycle;
	lifecycle.reset_session(true, true, CommNegotiationResult::Accepted);
	ASSERT_EQ(CommViewClientLifecycleResult::Applied, lifecycle.install_bundle(1U));
	ASSERT_EQ(CommViewClientLifecycleResult::Applied,
		lifecycle.apply_event(comm_start(1U, 1U), valid_comm_asset()));
	ASSERT_EQ(CommViewClientLifecycleResult::Applied,
		lifecycle.apply_event(comm_stop(2U, 1U, CommStopReason::Replaced), valid_comm_asset()));
	EXPECT_EQ(CommViewClientLifecycleState::Ready, lifecycle.state());
	ASSERT_EQ(CommViewClientLifecycleResult::Applied,
		lifecycle.apply_event(comm_start(3U, 2U, 3000U), valid_comm_asset()));
	EXPECT_EQ(2U, lifecycle.playback().playback_id);

	const auto delayed = comm_stop(4U, 1U, CommStopReason::Completed, 4000U);
	EXPECT_EQ(CommViewClientLifecycleResult::StalePlayback,
		lifecycle.apply_event(delayed, valid_comm_asset()));
	EXPECT_EQ(CommViewClientLifecycleState::Active, lifecycle.state());
	EXPECT_EQ(2U, lifecycle.playback().playback_id);
	EXPECT_EQ(CommViewClientLifecycleResult::Duplicate,
		lifecycle.apply_event(delayed, valid_comm_asset()));
	EXPECT_EQ(CommViewClientLifecycleResult::InvalidTransition,
		lifecycle.apply_event(comm_start(5U, 3U, 5000U), valid_comm_asset()));
	EXPECT_EQ(CommViewClientLifecycleResult::Applied,
		lifecycle.apply_event(comm_stop(6U, 2U), valid_comm_asset()));
	EXPECT_EQ(CommViewClientLifecycleState::Ready, lifecycle.state());
}

TEST(TelemetryProtocolSpecializedLifecycle, CommGenerationAndMissionSessionPurgesRejectOldPlayback)
{
	CommViewClientLifecycle lifecycle;
	lifecycle.reset_session(true, true, CommNegotiationResult::Accepted);
	ASSERT_EQ(CommViewClientLifecycleResult::Applied, lifecycle.install_bundle(1U));
	ASSERT_EQ(CommViewClientLifecycleResult::Applied,
		lifecycle.apply_state(comm_state(5U), valid_comm_asset()));
	lifecycle.purge_mission();
	EXPECT_EQ(CommViewClientLifecycleState::Ready, lifecycle.state());
	EXPECT_EQ(5U, lifecycle.highest_playback_id());
	EXPECT_EQ(CommViewClientLifecycleResult::StalePlayback,
		lifecycle.apply_state(comm_state(5U, 2000U), valid_comm_asset()));
	EXPECT_EQ(CommViewClientLifecycleResult::Applied,
		lifecycle.apply_state(comm_state(6U, 2000U), valid_comm_asset()));

	EXPECT_EQ(CommViewClientLifecycleResult::Applied, lifecycle.install_bundle(2U));
	EXPECT_EQ(CommViewClientLifecycleState::Ready, lifecycle.state());
	EXPECT_FALSE(lifecycle.has_playback());
	EXPECT_EQ(CommViewClientLifecycleResult::StaleGeneration, lifecycle.install_bundle(1U));
	lifecycle.purge_session();
	EXPECT_EQ(CommViewClientLifecycleState::Unsupported, lifecycle.state());
	EXPECT_EQ(0U, lifecycle.highest_playback_id());
	EXPECT_EQ(0U, lifecycle.highest_event_id());
}

TEST(TelemetryProtocolSpecializedLifecycle, CommCapabilityGenerationOnlyWithdrawsAndPurges)
{
	CommViewClientLifecycle lifecycle;
	lifecycle.reset_session(true, true, CommNegotiationResult::Accepted);
	ASSERT_EQ(CommViewClientLifecycleResult::Applied, lifecycle.install_bundle(1U));
	ASSERT_EQ(CommViewClientLifecycleResult::Applied,
		lifecycle.apply_state(comm_state(1U), valid_comm_asset()));
	EXPECT_EQ(CommViewClientLifecycleResult::Applied,
		lifecycle.apply_capability_update(1U, true, true));
	EXPECT_EQ(CommViewClientLifecycleResult::Duplicate,
		lifecycle.apply_capability_update(1U, true, true));
	EXPECT_EQ(CommViewClientLifecycleResult::ConflictingDuplicate,
		lifecycle.apply_capability_update(1U, true, false));
	EXPECT_EQ(CommViewClientLifecycleResult::SourceUnavailable,
		lifecycle.apply_capability_update(2U, true, false));
	EXPECT_EQ(CommViewClientLifecycleState::SourceUnavailable, lifecycle.state());
	EXPECT_FALSE(lifecycle.has_playback());
	EXPECT_EQ(CommViewClientLifecycleResult::InvalidTransition,
		lifecycle.apply_capability_update(3U, true, true));
}

TEST(TelemetryProtocolSpecializedLifecycle, ProducerRequiresWelcomeConfigAckAndFirstIdr)
{
	TargetVideoProducerLifecycle lifecycle;
	lifecycle.reset(true);
	auto config = valid_config();
	EXPECT_EQ(VideoProducerLifecycleResult::WelcomeNotAcknowledged, lifecycle.install_config(config, 42));
	EXPECT_EQ(VideoProducerLifecycleResult::Applied, lifecycle.acknowledge_welcome());
	EXPECT_EQ(VideoProducerLifecycleResult::Applied, lifecycle.install_config(config, 42));

	auto interframe = valid_frame(1, 1, 42, false);
	EXPECT_EQ(VideoProducerLifecycleResult::ConfigNotAcknowledged, lifecycle.authorize_frame(interframe));
	EXPECT_EQ(VideoProducerLifecycleResult::InvalidTransition, lifecycle.acknowledge_config_applied(9, 2));
	EXPECT_EQ(VideoProducerLifecycleResult::Applied, lifecycle.acknowledge_config_applied(9, 1));
	EXPECT_EQ(VideoProducerLifecycleResult::IdrRequired, lifecycle.authorize_frame(interframe));

	auto idr = valid_frame(1, 1, 42, true);
	EXPECT_EQ(VideoProducerLifecycleResult::ReadyToSend, lifecycle.authorize_frame(idr));
	EXPECT_EQ(VideoProducerLifecycleState::Live, lifecycle.state());
	EXPECT_EQ(VideoProducerLifecycleResult::CounterNotMonotone, lifecycle.authorize_frame(idr));

	auto reconfigured = valid_config(9, 2, 8);
	EXPECT_EQ(VideoProducerLifecycleResult::Applied, lifecycle.install_config(reconfigured, 43));
	EXPECT_EQ(VideoProducerLifecycleResult::Applied, lifecycle.acknowledge_config_applied(9, 2));
	auto old_target = valid_frame(2, 2, 42, true);
	EXPECT_EQ(VideoProducerLifecycleResult::TargetMismatch, lifecycle.authorize_frame(old_target));
	auto new_target = valid_frame(2, 2, 43, true);
	EXPECT_EQ(VideoProducerLifecycleResult::ReadyToSend, lifecycle.authorize_frame(new_target));
	EXPECT_EQ(VideoProducerLifecycleResult::Applied, lifecycle.set_capability_pair_active(false));
	EXPECT_EQ(VideoProducerLifecycleState::Unsupported, lifecycle.state());
	EXPECT_EQ(VideoProducerLifecycleResult::InvalidTransition, lifecycle.set_capability_pair_active(true));
	EXPECT_EQ(VideoProducerLifecycleState::Unsupported, lifecycle.state());
	lifecycle.reset(true);
	EXPECT_EQ(VideoProducerLifecycleState::Stopped, lifecycle.state());
}

TEST(TelemetryProtocolSpecializedLifecycle, ClientDropsOldGenerationAndTargetAndEnforcesStaleDeadlines)
{
	TargetVideoClientLifecycle lifecycle;
	lifecycle.reset(true);
	EXPECT_EQ(VideoClientLifecycleResult::Applied, lifecycle.begin_subscribe(7));
	EXPECT_EQ(VideoClientLifecycleResult::Applied, lifecycle.apply_config(valid_config(), 42));

	auto idr = valid_frame(1, 1, 42, true);
	EXPECT_EQ(VideoClientLifecycleResult::ConfigNotAcknowledged, lifecycle.accept_frame(idr, 42, 100));
	EXPECT_EQ(VideoClientLifecycleResult::Applied, lifecycle.acknowledge_config_applied(9, 1));
	EXPECT_EQ(VideoClientLifecycleResult::TargetMismatch, lifecycle.accept_frame(idr, 41, 100));
	auto interframe = valid_frame(1, 1, 42, false);
	EXPECT_EQ(VideoClientLifecycleResult::IdrRequired, lifecycle.accept_frame(interframe, 42, 100));
	EXPECT_EQ(VideoClientLifecycleResult::Presentable, lifecycle.accept_frame(idr, 42, 100));
	EXPECT_EQ(VideoClientLifecycleState::Live, lifecycle.state());

	EXPECT_EQ(VideoClientLifecycleResult::NoChange, lifecycle.observe_time(100 + VideoStaleAfterUs - 1));
	EXPECT_EQ(VideoClientLifecycleResult::Applied, lifecycle.observe_time(100 + VideoStaleAfterUs));
	EXPECT_EQ(VideoClientLifecycleState::Stale, lifecycle.state());
	EXPECT_FALSE(lifecycle.placeholder_required());
	EXPECT_EQ(VideoClientLifecycleResult::Applied, lifecycle.observe_time(100 + VideoPlaceholderAfterUs));
	EXPECT_TRUE(lifecycle.placeholder_required());

	EXPECT_EQ(VideoClientLifecycleResult::Applied, lifecycle.apply_config(valid_config(9, 2, 8), 43));
	EXPECT_EQ(VideoClientLifecycleResult::Applied, lifecycle.acknowledge_config_applied(9, 2));
	EXPECT_EQ(VideoClientLifecycleResult::StaleGeneration, lifecycle.accept_frame(idr, 43, 200));
	auto changed_target = valid_frame(2, 2, 43, true);
	EXPECT_EQ(VideoClientLifecycleResult::Presentable, lifecycle.accept_frame(changed_target, 43, 200));
	EXPECT_EQ(VideoClientLifecycleResult::Applied, lifecycle.set_capability_pair_active(false));
	EXPECT_EQ(VideoClientLifecycleState::Unsupported, lifecycle.state());
	EXPECT_EQ(0U, lifecycle.stream_id());
	EXPECT_EQ(VideoClientLifecycleResult::InvalidTransition, lifecycle.set_capability_pair_active(true));
	EXPECT_EQ(VideoClientLifecycleState::Unsupported, lifecycle.state());
	lifecycle.reset(true);
	EXPECT_EQ(VideoClientLifecycleState::Stopped, lifecycle.state());
}

TEST(TelemetryProtocolSpecializedLifecycle, StreamAndGenerationChecksPrecedeConfigSpecificSpsValidation)
{
	auto mismatched_sps = ValidIdr;
	mismatched_sps[5] = static_cast<std::uint8_t>(H264Profile::Main);

	TargetVideoClientLifecycle client;
	client.reset(true);
	ASSERT_EQ(VideoClientLifecycleResult::Applied, client.begin_subscribe(7));
	ASSERT_EQ(VideoClientLifecycleResult::Applied, client.apply_config(valid_config(), 42));
	ASSERT_EQ(VideoClientLifecycleResult::Applied, client.acknowledge_config_applied(9, 1));
	auto stale = valid_frame(1, 2, 42, true);
	stale.annex_b_access_unit = byte_view(mismatched_sps);
	stale.encoded_frame_size = static_cast<std::uint32_t>(mismatched_sps.size());
	EXPECT_EQ(VideoClientLifecycleResult::StaleGeneration, client.accept_frame(stale, 42, 100));
	stale.stream_id = 99;
	EXPECT_EQ(VideoClientLifecycleResult::UnknownStream, client.accept_frame(stale, 42, 100));

	TargetVideoProducerLifecycle producer;
	producer.reset(true);
	ASSERT_EQ(VideoProducerLifecycleResult::Applied, producer.acknowledge_welcome());
	ASSERT_EQ(VideoProducerLifecycleResult::Applied, producer.install_config(valid_config(), 42));
	ASSERT_EQ(VideoProducerLifecycleResult::Applied, producer.acknowledge_config_applied(9, 1));
	EXPECT_EQ(VideoProducerLifecycleResult::CounterNotMonotone, producer.authorize_frame(stale));
}

TEST(TelemetryProtocolSpecializedLifecycle, StreamIdentifiersAreNeverReusedAfterStop)
{
	TargetVideoProducerLifecycle lifecycle;
	lifecycle.reset(true);
	ASSERT_EQ(VideoProducerLifecycleResult::Applied, lifecycle.acknowledge_welcome());
	ASSERT_EQ(VideoProducerLifecycleResult::Applied, lifecycle.install_config(valid_config(), 42));
	ASSERT_EQ(VideoProducerLifecycleResult::Applied, lifecycle.stop(9, 1));
	EXPECT_EQ(VideoProducerLifecycleResult::CounterNotMonotone, lifecycle.install_config(valid_config(), 42));
	auto next = valid_config(10, 1, 8);
	EXPECT_EQ(VideoProducerLifecycleResult::Applied, lifecycle.install_config(next, 42));
}

TEST(TelemetryProtocolSpecializedLifecycle, SubscribeDecisionsAreExactByteDeduplicatedAndNeverReallocated)
{
	VideoSubscribeDecisionCache cache;
	cache.reset(100);
	std::array<std::uint8_t, TargetVideoSubscribePayloadSize> subscribe{};
	std::array<std::uint8_t, TargetVideoConfigPayloadSize> config{};
	for (std::size_t index = 0; index < subscribe.size(); ++index) {
		subscribe[index] = static_cast<std::uint8_t>(index + 1U);
	}
	for (std::size_t index = 0; index < config.size(); ++index) {
		config[index] = static_cast<std::uint8_t>(0xa0U + index);
	}
	VideoDecisionOutput output;
	EXPECT_EQ(VideoDecisionCacheResult::Stored,
		cache.remember(7, byte_view(subscribe), byte_view(config), 100, output));
	EXPECT_FALSE(output.duplicate);
	EXPECT_EQ(config, output.encoded_config);
	EXPECT_EQ(1U, cache.size());

	auto recomputed_config = config;
	recomputed_config[0] ^= 0xffU;
	EXPECT_EQ(VideoDecisionCacheResult::Duplicate,
		cache.remember(7, byte_view(subscribe), byte_view(recomputed_config), 200, output));
	EXPECT_TRUE(output.duplicate);
	EXPECT_EQ(config, output.encoded_config);
	EXPECT_EQ(1U, cache.size());

	auto conflicting = subscribe;
	conflicting.back() ^= 1U;
	EXPECT_EQ(VideoDecisionCacheResult::ConflictingDuplicate,
		cache.remember(7, byte_view(conflicting), byte_view(config), 300, output));
	EXPECT_EQ(1U, cache.size());
	EXPECT_EQ(1U, cache.purge_expired(100 + VideoDecisionRetentionUs));
	EXPECT_EQ(VideoDecisionCacheResult::StaleRequest,
		cache.remember(7, byte_view(subscribe), byte_view(config), 101 + VideoDecisionRetentionUs, output));
	EXPECT_EQ(VideoDecisionCacheResult::Stored,
		cache.remember(8, byte_view(subscribe), byte_view(config), 102 + VideoDecisionRetentionUs, output));
	EXPECT_EQ(VideoDecisionCacheResult::ClockRegressed,
		cache.remember(9, byte_view(subscribe), byte_view(config), 101 + VideoDecisionRetentionUs, output));
}

TEST(TelemetryProtocolSpecializedLifecycle, SpecializedRateLimitsUseTheNormativeScopesAndBursts)
{
	SpecializedOperationRateLimiter subscribe(SpecializedRateLimitClass::VideoSubscribe, 100);
	EXPECT_EQ(TokenBucketResult::Allowed, subscribe.consume(100));
	EXPECT_EQ(TokenBucketResult::Allowed, subscribe.consume(100));
	EXPECT_EQ(TokenBucketResult::RateLimited, subscribe.consume(100));
	EXPECT_EQ(TokenBucketResult::Allowed, subscribe.consume(500'100));

	SpecializedOperationRateLimiter keyframe(SpecializedRateLimitClass::VideoKeyframeRequest, 100);
	EXPECT_EQ(TokenBucketResult::Allowed, keyframe.consume(100));
	EXPECT_EQ(TokenBucketResult::RateLimited, keyframe.consume(100));
	EXPECT_EQ(TokenBucketResult::Allowed, keyframe.consume(500'100));
}

TEST(TelemetryProtocolSpecializedLifecycle, QosClassificationNeverDropsControlOrStateForVideo)
{
	EXPECT_EQ(TelemetrySendPriority::CriticalControl,
		classify_telemetry_send({MessageType::Ack, false, false, false, false}));
	EXPECT_EQ(TelemetrySendPriority::ReliableState,
		classify_telemetry_send({MessageType::FullSnapshot, false, false, false, false}));
	EXPECT_EQ(TelemetrySendPriority::ReplaceableState,
		classify_telemetry_send({MessageType::Delta, false, false, false, false}));
	EXPECT_EQ(TelemetrySendPriority::IdrRetransmission,
		classify_telemetry_send({MessageType::TargetVideoFrame, false, false, true, true}));
	EXPECT_EQ(TelemetrySendPriority::NewIdr,
		classify_telemetry_send({MessageType::TargetVideoFrame, false, false, true, false}));
	EXPECT_EQ(TelemetrySendPriority::VideoInterframe,
		classify_telemetry_send({MessageType::TargetVideoFrame, false, false, false, false}));
	EXPECT_EQ(TelemetrySendPriority::VideoStats,
		classify_telemetry_send({MessageType::TargetVideoStats, false, false, false, false}));

	EXPECT_FALSE(drop_on_would_block(TelemetrySendPriority::CriticalControl));
	EXPECT_FALSE(drop_on_would_block(TelemetrySendPriority::ReliableState));
	EXPECT_FALSE(drop_on_would_block(TelemetrySendPriority::ReplaceableState));
	EXPECT_FALSE(drop_on_would_block(TelemetrySendPriority::IdrRetransmission));
	EXPECT_TRUE(drop_on_would_block(TelemetrySendPriority::NewIdr));
	EXPECT_TRUE(drop_on_would_block(TelemetrySendPriority::VideoInterframe));
	EXPECT_TRUE(drop_on_would_block(TelemetrySendPriority::VideoStats));
}

TEST(TelemetryProtocolSpecializedLifecycle, RichLifecycleResultsMapToStableValidationTaxonomy)
{
	EXPECT_EQ(ValidationError::None,
		comm_view_lifecycle_validation_error(CommViewClientLifecycleResult::Deferred));
	EXPECT_EQ(ValidationError::CapabilityNotNegotiated,
		comm_view_lifecycle_validation_error(CommViewClientLifecycleResult::Unsupported));
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		comm_view_lifecycle_validation_error(CommViewClientLifecycleResult::BundleMismatch));
	EXPECT_EQ(ValidationError::StaleGeneration,
		comm_view_lifecycle_validation_error(CommViewClientLifecycleResult::StaleGeneration));

	EXPECT_EQ(ValidationError::None,
		video_lifecycle_validation_error(VideoClientLifecycleResult::Presentable));
	EXPECT_EQ(ValidationError::CapabilityNotNegotiated,
		video_lifecycle_validation_error(VideoClientLifecycleResult::Unsupported));
	EXPECT_EQ(ValidationError::StaleGeneration,
		video_lifecycle_validation_error(VideoClientLifecycleResult::StaleGeneration));
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		video_lifecycle_validation_error(VideoClientLifecycleResult::TargetMismatch));
	EXPECT_EQ(ValidationError::InvalidStateTransition,
		video_lifecycle_validation_error(VideoClientLifecycleResult::UnknownStream));
}

} // namespace
