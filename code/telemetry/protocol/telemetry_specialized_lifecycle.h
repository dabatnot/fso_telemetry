#pragma once

#include "telemetry/protocol/telemetry_rate_limiter.h"
#include "telemetry/protocol/telemetry_specialized_views.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace telemetry::protocol {

enum class CommViewClientLifecycleState : std::uint8_t {
	Unsupported = 0,
	SourceUnavailable = 1,
	BundleMismatch = 2,
	Ready = 3,
	Active = 4,
	Placeholder = 5,
};

enum class CommViewClientLifecycleResult : std::uint8_t {
	Applied = 0,
	Deferred = 1,
	Duplicate = 2,
	NoChange = 3,
	Unsupported = 4,
	SourceUnavailable = 5,
	BundleMismatch = 6,
	Placeholder = 7,
	InvalidPayload = 8,
	InvalidTransition = 9,
	ConflictingDuplicate = 10,
	StaleEvent = 11,
	StalePlayback = 12,
	StaleGeneration = 13,
};

constexpr ValidationError comm_view_lifecycle_validation_error(CommViewClientLifecycleResult result) noexcept
{
	switch (result) {
	case CommViewClientLifecycleResult::Applied:
	case CommViewClientLifecycleResult::Deferred:
	case CommViewClientLifecycleResult::Duplicate:
	case CommViewClientLifecycleResult::NoChange:
	case CommViewClientLifecycleResult::Placeholder:
		return ValidationError::None;
	case CommViewClientLifecycleResult::Unsupported:
	case CommViewClientLifecycleResult::SourceUnavailable:
		return ValidationError::CapabilityNotNegotiated;
	case CommViewClientLifecycleResult::StaleGeneration:
		return ValidationError::StaleGeneration;
	case CommViewClientLifecycleResult::BundleMismatch:
	case CommViewClientLifecycleResult::InvalidPayload:
	case CommViewClientLifecycleResult::InvalidTransition:
	case CommViewClientLifecycleResult::ConflictingDuplicate:
	case CommViewClientLifecycleResult::StaleEvent:
	case CommViewClientLifecycleResult::StalePlayback:
	default:
		return ValidationError::InvalidStateTransition;
	}
}

struct CommAssetRuntimeStatus {
	std::uint64_t asset_id = 0;
	std::uint64_t duration_us = 0;
	bool available = false;
	bool content_valid = false;
};

enum class CommViewProjectionResult : std::uint8_t {
	Presentable = 0,
	Inactive = 1,
	Placeholder = 2,
	InvalidState = 3,
};

struct CommViewProjection {
	CommViewProjectionResult result = CommViewProjectionResult::Inactive;
	std::uint64_t playback_id = 0;
	std::uint64_t head_asset_id = 0;
	double animation_time_us = 0.0;
};

// Engine-neutral client automaton for the local-assets communication view.
// It owns no files or renderer resources: callers provide the result of their
// manifest-backed asset lookup. One full active state is retained, plus at
// most one active state deferred while the accepted manifest is unavailable.
class CommViewClientLifecycle final {
  public:
	void reset_session(bool client_capability,
		bool producer_capability,
		CommNegotiationResult negotiation_result) noexcept;
	void purge_mission() noexcept;
	void purge_session() noexcept;

	CommViewClientLifecycleResult apply_capability_update(std::uint32_t capability_generation,
		bool client_capability,
		bool producer_capability) noexcept;
	CommViewClientLifecycleResult install_bundle(std::uint32_t manifest_generation,
		CommAssetRuntimeStatus deferred_asset = {}) noexcept;
	CommViewClientLifecycleResult install_bundle(std::uint32_t manifest_generation,
		const Sha256Digest& required_bundle_hash,
		const Sha256Digest& installed_bundle_hash,
		CommAssetRuntimeStatus deferred_asset = {}) noexcept;
	CommViewClientLifecycleResult invalidate_bundle() noexcept;

	CommViewClientLifecycleResult apply_event(const CommViewEventPayload& event,
		CommAssetRuntimeStatus asset) noexcept;
	CommViewClientLifecycleResult apply_state(const CommViewStatePayload& state,
		CommAssetRuntimeStatus asset) noexcept;
	CommViewClientLifecycleResult update_asset_status(CommAssetRuntimeStatus asset) noexcept;
	CommViewProjection project(std::uint64_t estimated_producer_now_us) const noexcept;

	CommViewClientLifecycleState state() const noexcept
	{
		return m_state;
	}
	const CommViewStatePayload& playback() const noexcept
	{
		return m_playback;
	}
	bool has_playback() const noexcept
	{
		return m_has_playback;
	}
	bool has_deferred_playback() const noexcept
	{
		return m_has_deferred_playback;
	}
	std::uint64_t highest_event_id() const noexcept
	{
		return m_highest_event_id;
	}
	std::uint64_t highest_playback_id() const noexcept
	{
		return m_highest_playback_id;
	}
	std::uint32_t capability_generation() const noexcept
	{
		return m_capability_generation;
	}
	std::uint32_t manifest_generation() const noexcept
	{
		return m_manifest_generation;
	}
	CommStopReason last_stop_reason() const noexcept
	{
		return m_last_stop_reason;
	}

  private:
	void clear_playback(bool clear_deferred) noexcept;
	void select_base_state() noexcept;
	CommViewClientLifecycleResult unavailable_result() const noexcept;
	CommViewClientLifecycleResult publish_playback(const CommViewStatePayload& playback,
		CommAssetRuntimeStatus asset) noexcept;

	CommViewClientLifecycleState m_state = CommViewClientLifecycleState::Unsupported;
	CommNegotiationResult m_negotiation_result = CommNegotiationResult::NotRequested;
	CommViewStatePayload m_playback{};
	CommViewStatePayload m_deferred_playback{};
	CommViewEventPayload m_last_event{};
	std::uint64_t m_highest_event_id = 0;
	std::uint64_t m_highest_playback_id = 0;
	std::uint32_t m_capability_generation = 0;
	std::uint32_t m_manifest_generation = 0;
	CommStopReason m_last_stop_reason = CommStopReason::None;
	bool m_client_capability = false;
	bool m_producer_capability = false;
	bool m_negotiation_accepted = false;
	bool m_bundle_ready = false;
	bool m_has_playback = false;
	bool m_has_deferred_playback = false;
	bool m_have_last_event = false;
};

constexpr std::uint64_t VideoStaleAfterUs = 500'000ULL;
constexpr std::uint64_t VideoPlaceholderAfterUs = 1'000'000ULL;

enum class VideoClientLifecycleState : std::uint8_t {
	Unsupported = 0,
	Stopped = 1,
	Subscribing = 2,
	Configured = 3,
	Live = 4,
	Stale = 5,
};

enum class VideoClientLifecycleResult : std::uint8_t {
	Applied = 0,
	Presentable = 1,
	NoChange = 2,
	Unsupported = 3,
	InvalidTransition = 4,
	InvalidPayload = 5,
	RequestMismatch = 6,
	UnknownStream = 7,
	StaleGeneration = 8,
	TargetMismatch = 9,
	OldFrame = 10,
	IdrRequired = 11,
	ConfigNotAcknowledged = 12,
	ClockRegressed = 13,
};

constexpr ValidationError video_lifecycle_validation_error(VideoClientLifecycleResult result) noexcept
{
	switch (result) {
	case VideoClientLifecycleResult::Applied:
	case VideoClientLifecycleResult::Presentable:
	case VideoClientLifecycleResult::NoChange:
		return ValidationError::None;
	case VideoClientLifecycleResult::Unsupported:
		return ValidationError::CapabilityNotNegotiated;
	case VideoClientLifecycleResult::StaleGeneration:
		return ValidationError::StaleGeneration;
	case VideoClientLifecycleResult::UnknownStream:
	case VideoClientLifecycleResult::InvalidTransition:
	case VideoClientLifecycleResult::InvalidPayload:
	case VideoClientLifecycleResult::RequestMismatch:
	case VideoClientLifecycleResult::TargetMismatch:
	case VideoClientLifecycleResult::OldFrame:
	case VideoClientLifecycleResult::IdrRequired:
	case VideoClientLifecycleResult::ConfigNotAcknowledged:
	case VideoClientLifecycleResult::ClockRegressed:
	default:
		return ValidationError::InvalidStateTransition;
	}
}

// Entity membership belongs to the caller-owned replicated state image, while
// stream/generation/target ordering belongs to TargetVideoClientLifecycle.
// Keeping this contextual gate explicit preserves the normative UNKNOWN_ENTITY
// rejection without coupling the protocol library to an engine entity table.
constexpr ValidationError validate_target_video_frame_entity_context(const TargetVideoFramePayload& frame,
	bool target_entity_known) noexcept
{
	if (frame.target_entity_id == 0) {
		return ValidationError::OutOfRange;
	}
	return target_entity_known ? ValidationError::None : ValidationError::UnknownEntity;
}

// Engine-neutral client state for the specialized target-video view. Decoder,
// texture and reassembly ownership stay outside this class; every transition
// nevertheless purges the wire identity that would make an old frame
// presentable.
class TargetVideoClientLifecycle final {
  public:
	void reset(bool capability_pair_active) noexcept;
	VideoClientLifecycleResult set_capability_pair_active(bool active) noexcept;
	VideoClientLifecycleResult begin_subscribe(std::uint32_t request_id) noexcept;
	VideoClientLifecycleResult apply_config(const TargetVideoConfigPayload& config,
		std::uint64_t target_entity_id) noexcept;
	VideoClientLifecycleResult acknowledge_config_applied(std::uint32_t stream_id,
		std::uint32_t config_generation) noexcept;
	VideoClientLifecycleResult accept_frame(const TargetVideoFramePayload& frame,
		std::uint64_t current_target_entity_id,
		std::uint64_t now_us) noexcept;
	VideoClientLifecycleResult observe_time(std::uint64_t now_us) noexcept;
	VideoClientLifecycleResult stop(std::uint32_t stream_id, std::uint32_t config_generation) noexcept;

	VideoClientLifecycleState state() const noexcept
	{
		return m_state;
	}
	std::uint32_t stream_id() const noexcept
	{
		return m_stream_id;
	}
	std::uint32_t config_generation() const noexcept
	{
		return m_config_generation;
	}
	std::uint64_t target_entity_id() const noexcept
	{
		return m_target_entity_id;
	}
	std::uint32_t highest_presented_frame_id() const noexcept
	{
		return m_highest_presented_frame_id;
	}
	bool config_acknowledged() const noexcept
	{
		return m_config_acknowledged;
	}
	bool placeholder_required() const noexcept
	{
		return m_placeholder_required;
	}

  private:
	void purge_stream(VideoClientLifecycleState next_state) noexcept;

	VideoClientLifecycleState m_state = VideoClientLifecycleState::Unsupported;
	VideoClientLifecycleState m_state_before_subscribe = VideoClientLifecycleState::Stopped;
	std::uint32_t m_pending_request_id = 0;
	std::uint32_t m_stream_id = 0;
	std::uint32_t m_highest_stream_id = 0;
	std::uint32_t m_config_generation = 0;
	TargetVideoConfigPayload m_config{};
	std::uint64_t m_target_entity_id = 0;
	std::uint32_t m_highest_presented_frame_id = 0;
	std::uint64_t m_last_progress_time_us = 0;
	bool m_config_acknowledged = false;
	bool m_first_frame_of_generation = false;
	bool m_have_progress_time = false;
	bool m_placeholder_required = false;
};

enum class VideoProducerLifecycleState : std::uint8_t {
	Unsupported = 0,
	Stopped = 1,
	Configured = 2,
	Live = 3,
};

enum class VideoProducerLifecycleResult : std::uint8_t {
	Applied = 0,
	ReadyToSend = 1,
	NoChange = 2,
	Unsupported = 3,
	WelcomeNotAcknowledged = 4,
	InvalidPayload = 5,
	InvalidTransition = 6,
	CounterNotMonotone = 7,
	ConfigNotAcknowledged = 8,
	IdrRequired = 9,
	TargetMismatch = 10,
};

// Producer-side gate proving the ordering WELCOME ACK -> CONFIG -> CONFIG ACK
// -> first IDR. Resource creation is authorized only after install_config()
// succeeds; callers remain responsible for the actual renderer/encoder.
class TargetVideoProducerLifecycle final {
  public:
	void reset(bool capability_pair_active) noexcept;
	VideoProducerLifecycleResult set_capability_pair_active(bool active) noexcept;
	VideoProducerLifecycleResult acknowledge_welcome() noexcept;
	VideoProducerLifecycleResult install_config(const TargetVideoConfigPayload& config,
		std::uint64_t target_entity_id) noexcept;
	VideoProducerLifecycleResult acknowledge_config_applied(std::uint32_t stream_id,
		std::uint32_t config_generation) noexcept;
	VideoProducerLifecycleResult authorize_frame(const TargetVideoFramePayload& frame) noexcept;
	VideoProducerLifecycleResult stop(std::uint32_t stream_id, std::uint32_t config_generation) noexcept;

	VideoProducerLifecycleState state() const noexcept
	{
		return m_state;
	}
	bool welcome_acknowledged() const noexcept
	{
		return m_welcome_acknowledged;
	}
	std::uint32_t stream_id() const noexcept
	{
		return m_stream_id;
	}
	std::uint32_t config_generation() const noexcept
	{
		return m_config_generation;
	}

  private:
	void purge_stream(VideoProducerLifecycleState next_state) noexcept;

	VideoProducerLifecycleState m_state = VideoProducerLifecycleState::Unsupported;
	bool m_welcome_acknowledged = false;
	bool m_config_acknowledged = false;
	bool m_first_frame_of_generation = false;
	std::uint32_t m_stream_id = 0;
	std::uint32_t m_highest_stream_id = 0;
	std::uint32_t m_config_generation = 0;
	TargetVideoConfigPayload m_config{};
	std::uint32_t m_highest_frame_id = 0;
	std::uint64_t m_target_entity_id = 0;
};

constexpr std::size_t VideoDecisionCacheCapacity = 4096;
constexpr std::uint64_t VideoDecisionRetentionUs = 7'000'000ULL;

enum class VideoDecisionCacheResult : std::uint8_t {
	Stored = 0,
	Duplicate = 1,
	ConflictingDuplicate = 2,
	StaleRequest = 3,
	InvalidArgument = 4,
	ResourceLimit = 5,
	ClockRegressed = 6,
};

struct VideoDecisionOutput {
	std::array<std::uint8_t, TargetVideoConfigPayloadSize> encoded_config{};
	bool duplicate = false;
};

// Exact-byte decision cache for TARGET_VIDEO_SUBSCRIBE. Ownership is one
// instance per session/client. Entries expire after the ordinary reliable
// window plus the two-second receive grace, while the high-water request ID
// remains for the whole session so an expired request can never allocate a
// second stream.
class VideoSubscribeDecisionCache final {
  public:
	VideoDecisionCacheResult remember(std::uint32_t request_id,
		ByteView encoded_subscribe,
		ByteView encoded_config,
		std::uint64_t now_us,
		VideoDecisionOutput& output) noexcept;
	std::size_t purge_expired(std::uint64_t now_us) noexcept;
	void reset(std::uint64_t now_us) noexcept;

	std::size_t size() const noexcept
	{
		return m_size;
	}
	std::uint32_t highest_request_id() const noexcept
	{
		return m_highest_request_id;
	}

  private:
	struct Entry {
		bool active = false;
		std::uint32_t request_id = 0;
		std::uint64_t stored_at_us = 0;
		std::array<std::uint8_t, TargetVideoSubscribePayloadSize> encoded_subscribe{};
		std::array<std::uint8_t, TargetVideoConfigPayloadSize> encoded_config{};
	};

	std::array<Entry, VideoDecisionCacheCapacity> m_entries{};
	std::size_t m_size = 0;
	std::uint32_t m_highest_request_id = 0;
	std::uint64_t m_last_time_us = 0;
	bool m_clock_regressed = false;
};

enum class SpecializedRateLimitClass : std::uint8_t {
	VideoSubscribe = 0,
	VideoStop = 1,
	VideoKeyframeRequest = 2,
	VideoStats = 3,
	CommViewEvent = 4,
	CommViewState = 5,
};

constexpr RateLimitProfile specialized_rate_limit_profile(SpecializedRateLimitClass limit_class) noexcept
{
	switch (limit_class) {
	case SpecializedRateLimitClass::VideoSubscribe:
		return {2, 2};
	case SpecializedRateLimitClass::VideoStop:
		return {4, 4};
	case SpecializedRateLimitClass::VideoKeyframeRequest:
		return {2, 1};
	case SpecializedRateLimitClass::VideoStats:
		return {2, 2};
	case SpecializedRateLimitClass::CommViewEvent:
		return {20, 20};
	case SpecializedRateLimitClass::CommViewState:
		return {10, 2};
	}
	return {};
}

// Instantiate one limiter per normative owner: session for subscribe/stop,
// stream for keyframe/stats, producer for COMM_VIEW_EVENT and periodic state.
class SpecializedOperationRateLimiter final {
  public:
	SpecializedOperationRateLimiter(SpecializedRateLimitClass limit_class,
		std::uint64_t initial_time_us) noexcept;
	TokenBucketResult consume(std::uint64_t now_us) noexcept;
	void reset(std::uint64_t now_us) noexcept;

  private:
	TokenBucket m_bucket;
};

enum class TelemetrySendPriority : std::uint8_t {
	CriticalControl = 1,
	ReliableState = 2,
	ReplaceableState = 3,
	IdrRetransmission = 4,
	NewIdr = 5,
	VideoInterframe = 6,
	VideoStats = 7,
};

struct TelemetrySendClassification {
	MessageType message_type = MessageType::Invalid;
	bool reliable_event_batch = false;
	bool comm_view_state = false;
	bool video_frame_is_idr = false;
	bool retransmission = false;
};

TelemetrySendPriority classify_telemetry_send(const TelemetrySendClassification& item) noexcept;
bool drop_on_would_block(TelemetrySendPriority priority) noexcept;

} // namespace telemetry::protocol
