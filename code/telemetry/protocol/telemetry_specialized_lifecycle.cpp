#include "telemetry/protocol/telemetry_specialized_lifecycle.h"

#include <algorithm>
#include <cmath>

namespace telemetry::protocol {

namespace {

constexpr bool frame_is_idr(const TargetVideoFramePayload& frame) noexcept
{
	return (frame.video_flags & VideoFrameFlagIdr) != 0;
}

constexpr bool client_has_stream(VideoClientLifecycleState state) noexcept
{
	return state == VideoClientLifecycleState::Configured || state == VideoClientLifecycleState::Live ||
		   state == VideoClientLifecycleState::Stale || state == VideoClientLifecycleState::Subscribing;
}

bool same_comm_state(const CommViewStatePayload& left, const CommViewStatePayload& right) noexcept
{
	return left.active == right.active && left.playback_mode == right.playback_mode &&
		   left.color_mode == right.color_mode && left.stop_reason == right.stop_reason &&
		   left.playback_id == right.playback_id && left.engine_message_id == right.engine_message_id &&
		   left.sender_entity_id == right.sender_entity_id && left.head_asset_id == right.head_asset_id &&
		   left.producer_sample_time_us == right.producer_sample_time_us &&
		   left.animation_time_us == right.animation_time_us && left.duration_us == right.duration_us &&
		   left.playback_rate == right.playback_rate;
}

bool same_comm_event(const CommViewEventPayload& left, const CommViewEventPayload& right) noexcept
{
	return left.event_id == right.event_id && left.event_kind == right.event_kind &&
		   left.playback_mode == right.playback_mode && left.color_mode == right.color_mode &&
		   left.stop_reason == right.stop_reason && left.playback_id == right.playback_id &&
		   left.engine_message_id == right.engine_message_id &&
		   left.sender_entity_id == right.sender_entity_id && left.head_asset_id == right.head_asset_id &&
		   left.producer_sample_time_us == right.producer_sample_time_us &&
		   left.animation_time_us == right.animation_time_us && left.duration_us == right.duration_us &&
		   left.playback_rate == right.playback_rate;
}

CommViewStatePayload state_from_start(const CommViewEventPayload& event) noexcept
{
	CommViewStatePayload state;
	state.active = true;
	state.playback_mode = event.playback_mode;
	state.color_mode = event.color_mode;
	state.stop_reason = CommStopReason::None;
	state.playback_id = event.playback_id;
	state.engine_message_id = event.engine_message_id;
	state.sender_entity_id = event.sender_entity_id;
	state.head_asset_id = event.head_asset_id;
	state.producer_sample_time_us = event.producer_sample_time_us;
	state.animation_time_us = event.animation_time_us;
	state.duration_us = event.duration_us;
	state.playback_rate = event.playback_rate;
	return state;
}

bool comm_asset_is_usable(const CommViewStatePayload& playback, CommAssetRuntimeStatus asset) noexcept
{
	return asset.available && asset.content_valid && asset.asset_id == playback.head_asset_id &&
		   asset.duration_us == playback.duration_us;
}

bool comm_asset_identity_is_inconsistent(std::uint64_t head_asset_id,
	std::uint64_t duration_us,
	CommAssetRuntimeStatus asset) noexcept
{
	return (asset.asset_id != 0 && asset.asset_id != head_asset_id) ||
		   (asset.asset_id == head_asset_id && asset.duration_us != 0 && asset.duration_us != duration_us);
}

} // namespace

void CommViewClientLifecycle::clear_playback(bool clear_deferred) noexcept
{
	m_playback = CommViewStatePayload{};
	m_has_playback = false;
	if (clear_deferred) {
		m_deferred_playback = CommViewStatePayload{};
		m_has_deferred_playback = false;
	}
}

void CommViewClientLifecycle::select_base_state() noexcept
{
	if (!m_client_capability) {
		m_state = CommViewClientLifecycleState::Unsupported;
	} else if (!m_producer_capability || m_negotiation_result == CommNegotiationResult::SourceUnavailable) {
		m_state = CommViewClientLifecycleState::SourceUnavailable;
	} else if (!m_negotiation_accepted || !m_bundle_ready) {
		m_state = CommViewClientLifecycleState::BundleMismatch;
	} else {
		m_state = CommViewClientLifecycleState::Ready;
	}
}

CommViewClientLifecycleResult CommViewClientLifecycle::unavailable_result() const noexcept
{
	switch (m_state) {
	case CommViewClientLifecycleState::Unsupported:
		return CommViewClientLifecycleResult::Unsupported;
	case CommViewClientLifecycleState::SourceUnavailable:
		return CommViewClientLifecycleResult::SourceUnavailable;
	default:
		return CommViewClientLifecycleResult::BundleMismatch;
	}
}

CommViewClientLifecycleResult CommViewClientLifecycle::publish_playback(const CommViewStatePayload& playback,
	CommAssetRuntimeStatus asset) noexcept
{
	m_playback = playback;
	m_has_playback = true;
	m_deferred_playback = CommViewStatePayload{};
	m_has_deferred_playback = false;
	m_last_stop_reason = CommStopReason::None;
	if (!comm_asset_is_usable(playback, asset)) {
		m_state = CommViewClientLifecycleState::Placeholder;
		return CommViewClientLifecycleResult::Placeholder;
	}
	m_state = CommViewClientLifecycleState::Active;
	return CommViewClientLifecycleResult::Applied;
}

void CommViewClientLifecycle::reset_session(bool client_capability,
	bool producer_capability,
	CommNegotiationResult negotiation_result) noexcept
{
	purge_session();
	m_client_capability = client_capability;
	m_producer_capability = producer_capability;
	m_negotiation_result = negotiation_result;
	m_negotiation_accepted = client_capability && producer_capability &&
						 negotiation_result == CommNegotiationResult::Accepted;
	select_base_state();
}

void CommViewClientLifecycle::purge_mission() noexcept
{
	clear_playback(true);
	m_last_stop_reason = CommStopReason::MissionChanged;
	select_base_state();
}

void CommViewClientLifecycle::purge_session() noexcept
{
	m_state = CommViewClientLifecycleState::Unsupported;
	m_negotiation_result = CommNegotiationResult::NotRequested;
	m_playback = CommViewStatePayload{};
	m_deferred_playback = CommViewStatePayload{};
	m_last_event = CommViewEventPayload{};
	m_highest_event_id = 0;
	m_highest_playback_id = 0;
	m_capability_generation = 0;
	m_manifest_generation = 0;
	m_last_stop_reason = CommStopReason::None;
	m_client_capability = false;
	m_producer_capability = false;
	m_negotiation_accepted = false;
	m_bundle_ready = false;
	m_has_playback = false;
	m_has_deferred_playback = false;
	m_have_last_event = false;
}

CommViewClientLifecycleResult CommViewClientLifecycle::apply_capability_update(
	std::uint32_t capability_generation,
	bool client_capability,
	bool producer_capability) noexcept
{
	if (capability_generation == 0) {
		return CommViewClientLifecycleResult::InvalidPayload;
	}
	if (capability_generation < m_capability_generation) {
		return CommViewClientLifecycleResult::StaleGeneration;
	}
	if (capability_generation == m_capability_generation && m_capability_generation != 0) {
		return client_capability == m_client_capability && producer_capability == m_producer_capability
				   ? CommViewClientLifecycleResult::Duplicate
				   : CommViewClientLifecycleResult::ConflictingDuplicate;
	}
	// FSTL 1.0 permits withdrawal only. A pair cannot be reactivated in the
	// same session, even with a larger generation.
	if ((client_capability && !m_client_capability) || (producer_capability && !m_producer_capability)) {
		return CommViewClientLifecycleResult::InvalidTransition;
	}

	m_capability_generation = capability_generation;
	const auto capability_lost = (m_client_capability && !client_capability) ||
								 (m_producer_capability && !producer_capability);
	m_client_capability = client_capability;
	m_producer_capability = producer_capability;
	if (capability_lost) {
		clear_playback(true);
		select_base_state();
		return unavailable_result();
	}
	return CommViewClientLifecycleResult::Applied;
}

CommViewClientLifecycleResult CommViewClientLifecycle::install_bundle(std::uint32_t manifest_generation,
	CommAssetRuntimeStatus deferred_asset) noexcept
{
	if (!m_client_capability || !m_producer_capability || !m_negotiation_accepted) {
		return unavailable_result();
	}
	if (manifest_generation == 0) {
		return CommViewClientLifecycleResult::InvalidPayload;
	}
	if (m_manifest_generation != 0 && manifest_generation < m_manifest_generation) {
		return CommViewClientLifecycleResult::StaleGeneration;
	}
	if (m_bundle_ready && manifest_generation == m_manifest_generation) {
		return CommViewClientLifecycleResult::Duplicate;
	}
	if (m_manifest_generation != 0 && manifest_generation > m_manifest_generation) {
		clear_playback(true);
		m_manifest_generation = manifest_generation;
		m_bundle_ready = true;
		select_base_state();
		return CommViewClientLifecycleResult::Applied;
	}

	m_manifest_generation = manifest_generation;
	m_bundle_ready = true;
	if (m_has_deferred_playback) {
		const auto deferred = m_deferred_playback;
		return publish_playback(deferred, deferred_asset);
	}
	select_base_state();
	return CommViewClientLifecycleResult::Applied;
}

CommViewClientLifecycleResult CommViewClientLifecycle::install_bundle(std::uint32_t manifest_generation,
	const Sha256Digest& required_bundle_hash,
	const Sha256Digest& installed_bundle_hash,
	CommAssetRuntimeStatus deferred_asset) noexcept
{
	if (required_bundle_hash != installed_bundle_hash) {
		return CommViewClientLifecycleResult::InvalidTransition;
	}
	return install_bundle(manifest_generation, deferred_asset);
}

CommViewClientLifecycleResult CommViewClientLifecycle::invalidate_bundle() noexcept
{
	if (!m_negotiation_accepted) {
		return unavailable_result();
	}
	if (!m_bundle_ready && m_manifest_generation == 0) {
		return CommViewClientLifecycleResult::NoChange;
	}
	clear_playback(true);
	m_bundle_ready = false;
	m_manifest_generation = 0;
	select_base_state();
	return CommViewClientLifecycleResult::BundleMismatch;
}

CommViewClientLifecycleResult CommViewClientLifecycle::apply_event(const CommViewEventPayload& event,
	CommAssetRuntimeStatus asset) noexcept
{
	if (validate_comm_view_event_payload(event) != ValidationError::None) {
		return CommViewClientLifecycleResult::InvalidPayload;
	}
	if (!m_client_capability || !m_producer_capability || !m_negotiation_accepted) {
		return unavailable_result();
	}
	if (event.event_id < m_highest_event_id) {
		return CommViewClientLifecycleResult::StaleEvent;
	}
	if (event.event_id == m_highest_event_id && m_have_last_event) {
		return same_comm_event(event, m_last_event) ? CommViewClientLifecycleResult::Duplicate
											 : CommViewClientLifecycleResult::ConflictingDuplicate;
	}

	if (event.event_kind == CommEventKind::Start) {
		if (event.playback_id <= m_highest_playback_id) {
			return CommViewClientLifecycleResult::StalePlayback;
		}
		if (m_has_playback || m_has_deferred_playback) {
			// Replacements are ordered STOP(REPLACED), then START with a new ID.
			return CommViewClientLifecycleResult::InvalidTransition;
		}
		if (comm_asset_identity_is_inconsistent(event.head_asset_id, event.duration_us, asset)) {
			return CommViewClientLifecycleResult::InvalidTransition;
		}
		const auto playback = state_from_start(event);
		m_highest_playback_id = event.playback_id;
		m_highest_event_id = event.event_id;
		m_last_event = event;
		m_have_last_event = true;
		if (!m_bundle_ready) {
			m_deferred_playback = playback;
			m_has_deferred_playback = true;
			select_base_state();
			return CommViewClientLifecycleResult::Deferred;
		}
		return publish_playback(playback, asset);
	}

	const auto matches_playback = m_has_playback && event.playback_id == m_playback.playback_id;
	const auto matches_deferred =
		m_has_deferred_playback && event.playback_id == m_deferred_playback.playback_id;
	if (matches_playback || matches_deferred) {
		clear_playback(true);
		m_last_stop_reason = event.stop_reason;
		m_highest_event_id = event.event_id;
		m_last_event = event;
		m_have_last_event = true;
		select_base_state();
		return CommViewClientLifecycleResult::Applied;
	}
	if (event.playback_id <= m_highest_playback_id) {
		// A delayed STOP is valid and deduplicated, but never stops the newer
		// visible playback.
		m_highest_event_id = event.event_id;
		m_last_event = event;
		m_have_last_event = true;
		return CommViewClientLifecycleResult::StalePlayback;
	}
	return CommViewClientLifecycleResult::InvalidTransition;
}

CommViewClientLifecycleResult CommViewClientLifecycle::apply_state(const CommViewStatePayload& state,
	CommAssetRuntimeStatus asset) noexcept
{
	if (validate_comm_view_state_payload(state) != ValidationError::None) {
		return CommViewClientLifecycleResult::InvalidPayload;
	}
	if (!m_client_capability || !m_producer_capability || !m_negotiation_accepted) {
		return unavailable_result();
	}
	if (!state.active) {
		const auto changed = m_has_playback || m_has_deferred_playback || m_last_stop_reason != state.stop_reason;
		clear_playback(true);
		m_last_stop_reason = state.stop_reason;
		select_base_state();
		return changed ? CommViewClientLifecycleResult::Applied : CommViewClientLifecycleResult::NoChange;
	}
	if (comm_asset_identity_is_inconsistent(state.head_asset_id, state.duration_us, asset)) {
		return CommViewClientLifecycleResult::InvalidTransition;
	}

	const CommViewStatePayload* previous = nullptr;
	if (m_has_playback && state.playback_id == m_playback.playback_id) {
		previous = &m_playback;
	} else if (m_has_deferred_playback && state.playback_id == m_deferred_playback.playback_id) {
		previous = &m_deferred_playback;
	}
	if (previous != nullptr) {
		if (state.producer_sample_time_us < previous->producer_sample_time_us) {
			return CommViewClientLifecycleResult::StalePlayback;
		}
		if (state.producer_sample_time_us == previous->producer_sample_time_us) {
			return same_comm_state(state, *previous) ? CommViewClientLifecycleResult::Duplicate
											  : CommViewClientLifecycleResult::InvalidTransition;
		}
	} else if (state.playback_id <= m_highest_playback_id) {
		// A stopped or mission-invalidated playback cannot be reactivated by a
		// delayed replaceable correction.
		return CommViewClientLifecycleResult::StalePlayback;
	} else {
		m_highest_playback_id = state.playback_id;
	}

	if (!m_bundle_ready) {
		clear_playback(false);
		m_deferred_playback = state;
		m_has_deferred_playback = true;
		select_base_state();
		return CommViewClientLifecycleResult::Deferred;
	}
	return publish_playback(state, asset);
}

CommViewClientLifecycleResult CommViewClientLifecycle::update_asset_status(CommAssetRuntimeStatus asset) noexcept
{
	if (!m_has_playback) {
		return m_has_deferred_playback ? CommViewClientLifecycleResult::Deferred
									  : CommViewClientLifecycleResult::NoChange;
	}
	const auto usable = comm_asset_is_usable(m_playback, asset);
	if (!usable) {
		if (m_state == CommViewClientLifecycleState::Placeholder) {
			return CommViewClientLifecycleResult::NoChange;
		}
		m_state = CommViewClientLifecycleState::Placeholder;
		return CommViewClientLifecycleResult::Placeholder;
	}
	if (m_state == CommViewClientLifecycleState::Active) {
		return CommViewClientLifecycleResult::NoChange;
	}
	m_state = CommViewClientLifecycleState::Active;
	return CommViewClientLifecycleResult::Applied;
}

CommViewProjection CommViewClientLifecycle::project(std::uint64_t estimated_producer_now_us) const noexcept
{
	CommViewProjection projection;
	if (!m_has_playback) {
		return projection;
	}
	projection.playback_id = m_playback.playback_id;
	projection.head_asset_id = m_playback.head_asset_id;
	if (m_state == CommViewClientLifecycleState::Placeholder) {
		projection.result = CommViewProjectionResult::Placeholder;
		return projection;
	}
	if (m_state != CommViewClientLifecycleState::Active || m_playback.duration_us == 0) {
		projection.result = CommViewProjectionResult::InvalidState;
		return projection;
	}

	const auto elapsed_us = estimated_producer_now_us > m_playback.producer_sample_time_us
							  ? estimated_producer_now_us - m_playback.producer_sample_time_us
							  : 0U;
	const auto duration = static_cast<double>(m_playback.duration_us);
	auto animation_time = static_cast<double>(m_playback.animation_time_us) +
					  static_cast<double>(m_playback.playback_rate) * static_cast<double>(elapsed_us);
	if (!std::isfinite(animation_time)) {
		projection.result = CommViewProjectionResult::InvalidState;
		return projection;
	}
	if (m_playback.playback_mode == CommPlaybackMode::Loop) {
		animation_time = std::fmod(animation_time, duration);
		if (animation_time < 0.0) {
			animation_time += duration;
		}
	} else {
		animation_time = std::max(0.0, std::min(animation_time, duration));
	}
	projection.animation_time_us = animation_time == 0.0 ? 0.0 : animation_time;
	projection.result = CommViewProjectionResult::Presentable;
	return projection;
}

void TargetVideoClientLifecycle::purge_stream(VideoClientLifecycleState next_state) noexcept
{
	m_state = next_state;
	m_state_before_subscribe = next_state;
	m_pending_request_id = 0;
	m_stream_id = 0;
	m_config_generation = 0;
	m_config = TargetVideoConfigPayload{};
	m_target_entity_id = 0;
	m_highest_presented_frame_id = 0;
	m_last_progress_time_us = 0;
	m_config_acknowledged = false;
	m_first_frame_of_generation = false;
	m_have_progress_time = false;
	m_placeholder_required = false;
}

void TargetVideoClientLifecycle::reset(bool capability_pair_active) noexcept
{
	m_highest_stream_id = 0;
	purge_stream(capability_pair_active ? VideoClientLifecycleState::Stopped
										: VideoClientLifecycleState::Unsupported);
}

VideoClientLifecycleResult TargetVideoClientLifecycle::set_capability_pair_active(bool active) noexcept
{
	if (!active) {
		if (m_state == VideoClientLifecycleState::Unsupported) {
			return VideoClientLifecycleResult::NoChange;
		}
		purge_stream(VideoClientLifecycleState::Unsupported);
		return VideoClientLifecycleResult::Applied;
	}
	if (m_state != VideoClientLifecycleState::Unsupported) {
		return VideoClientLifecycleResult::NoChange;
	}
	// Capability updates are monotone within a session. Negotiation for a new
	// session is represented only by reset(true); an update may never reactivate
	// a pair that was absent or withdrawn.
	return VideoClientLifecycleResult::InvalidTransition;
}

VideoClientLifecycleResult TargetVideoClientLifecycle::begin_subscribe(std::uint32_t request_id) noexcept
{
	if (m_state == VideoClientLifecycleState::Unsupported) {
		return VideoClientLifecycleResult::Unsupported;
	}
	if (request_id == 0 || m_state == VideoClientLifecycleState::Subscribing) {
		return VideoClientLifecycleResult::InvalidTransition;
	}
	m_state_before_subscribe = m_state;
	m_pending_request_id = request_id;
	m_state = VideoClientLifecycleState::Subscribing;
	return VideoClientLifecycleResult::Applied;
}

VideoClientLifecycleResult TargetVideoClientLifecycle::apply_config(const TargetVideoConfigPayload& config,
	std::uint64_t target_entity_id) noexcept
{
	if (validate_target_video_config_payload(config) != ValidationError::None) {
		return VideoClientLifecycleResult::InvalidPayload;
	}
	if (m_state == VideoClientLifecycleState::Unsupported || m_state == VideoClientLifecycleState::Stopped) {
		return VideoClientLifecycleResult::InvalidTransition;
	}
	if (m_pending_request_id != 0 && config.request_id != m_pending_request_id) {
		return VideoClientLifecycleResult::RequestMismatch;
	}
	if (config.result != VideoConfigResult::Accepted) {
		if (m_state != VideoClientLifecycleState::Subscribing) {
			return VideoClientLifecycleResult::InvalidTransition;
		}
		m_pending_request_id = 0;
		m_state = m_state_before_subscribe;
		m_state_before_subscribe = m_state;
		return VideoClientLifecycleResult::Applied;
	}
	if (target_entity_id == 0) {
		return VideoClientLifecycleResult::TargetMismatch;
	}

	const auto new_stream = m_stream_id == 0;
	if (new_stream) {
		if (config.stream_id <= m_highest_stream_id) {
			return VideoClientLifecycleResult::UnknownStream;
		}
	} else if (config.stream_id != m_stream_id) {
		return VideoClientLifecycleResult::UnknownStream;
	} else if (config.config_generation <= m_config_generation) {
		return VideoClientLifecycleResult::StaleGeneration;
	}

	if (new_stream) {
		m_highest_stream_id = config.stream_id;
		m_highest_presented_frame_id = 0;
	}
	m_pending_request_id = 0;
	m_stream_id = config.stream_id;
	m_config_generation = config.config_generation;
	m_config = config;
	m_target_entity_id = target_entity_id;
	m_config_acknowledged = false;
	m_first_frame_of_generation = true;
	m_have_progress_time = false;
	m_last_progress_time_us = 0;
	m_placeholder_required = false;
	m_state = VideoClientLifecycleState::Configured;
	m_state_before_subscribe = m_state;
	return VideoClientLifecycleResult::Applied;
}

VideoClientLifecycleResult TargetVideoClientLifecycle::acknowledge_config_applied(std::uint32_t stream_id,
	std::uint32_t config_generation) noexcept
{
	if (m_state != VideoClientLifecycleState::Configured || stream_id != m_stream_id) {
		return VideoClientLifecycleResult::UnknownStream;
	}
	if (config_generation != m_config_generation) {
		return VideoClientLifecycleResult::StaleGeneration;
	}
	if (m_config_acknowledged) {
		return VideoClientLifecycleResult::NoChange;
	}
	m_config_acknowledged = true;
	return VideoClientLifecycleResult::Applied;
}

VideoClientLifecycleResult TargetVideoClientLifecycle::accept_frame(const TargetVideoFramePayload& frame,
	std::uint64_t current_target_entity_id,
	std::uint64_t now_us) noexcept
{
	if (validate_target_video_frame_payload(frame) != ValidationError::None) {
		return VideoClientLifecycleResult::InvalidPayload;
	}
	if (!client_has_stream(m_state) || m_stream_id == 0 || frame.stream_id != m_stream_id) {
		return VideoClientLifecycleResult::UnknownStream;
	}
	if (frame.config_generation != m_config_generation) {
		return VideoClientLifecycleResult::StaleGeneration;
	}
	if (!m_config_acknowledged) {
		return VideoClientLifecycleResult::ConfigNotAcknowledged;
	}
	if (current_target_entity_id == 0 || current_target_entity_id != m_target_entity_id ||
		frame.target_entity_id != m_target_entity_id) {
		return VideoClientLifecycleResult::TargetMismatch;
	}
	if (frame.video_frame_id <= m_highest_presented_frame_id) {
		return VideoClientLifecycleResult::OldFrame;
	}
	if (validate_target_video_frame_payload(frame, m_config) != ValidationError::None) {
		return VideoClientLifecycleResult::InvalidPayload;
	}
	if (m_first_frame_of_generation && !frame_is_idr(frame)) {
		return VideoClientLifecycleResult::IdrRequired;
	}
	if (m_have_progress_time && now_us < m_last_progress_time_us) {
		return VideoClientLifecycleResult::ClockRegressed;
	}

	m_highest_presented_frame_id = frame.video_frame_id;
	m_first_frame_of_generation = false;
	m_last_progress_time_us = now_us;
	m_have_progress_time = true;
	m_placeholder_required = false;
	m_state = VideoClientLifecycleState::Live;
	return VideoClientLifecycleResult::Presentable;
}

VideoClientLifecycleResult TargetVideoClientLifecycle::observe_time(std::uint64_t now_us) noexcept
{
	if (!m_have_progress_time || (m_state != VideoClientLifecycleState::Live &&
								 m_state != VideoClientLifecycleState::Stale)) {
		return VideoClientLifecycleResult::NoChange;
	}
	if (now_us < m_last_progress_time_us) {
		return VideoClientLifecycleResult::ClockRegressed;
	}
	const auto elapsed = now_us - m_last_progress_time_us;
	bool changed = false;
	if (elapsed >= VideoStaleAfterUs && m_state == VideoClientLifecycleState::Live) {
		m_state = VideoClientLifecycleState::Stale;
		changed = true;
	}
	if (elapsed >= VideoPlaceholderAfterUs && !m_placeholder_required) {
		m_placeholder_required = true;
		changed = true;
	}
	return changed ? VideoClientLifecycleResult::Applied : VideoClientLifecycleResult::NoChange;
}

VideoClientLifecycleResult TargetVideoClientLifecycle::stop(std::uint32_t stream_id,
	std::uint32_t config_generation) noexcept
{
	if (m_state == VideoClientLifecycleState::Unsupported) {
		return VideoClientLifecycleResult::Unsupported;
	}
	if (m_stream_id == 0) {
		return VideoClientLifecycleResult::NoChange;
	}
	if (stream_id != m_stream_id) {
		return VideoClientLifecycleResult::UnknownStream;
	}
	if (config_generation != m_config_generation) {
		return VideoClientLifecycleResult::StaleGeneration;
	}
	purge_stream(VideoClientLifecycleState::Stopped);
	return VideoClientLifecycleResult::Applied;
}

void TargetVideoProducerLifecycle::purge_stream(VideoProducerLifecycleState next_state) noexcept
{
	m_state = next_state;
	m_config_acknowledged = false;
	m_first_frame_of_generation = false;
	m_stream_id = 0;
	m_config_generation = 0;
	m_config = TargetVideoConfigPayload{};
	m_highest_frame_id = 0;
	m_target_entity_id = 0;
}

void TargetVideoProducerLifecycle::reset(bool capability_pair_active) noexcept
{
	m_highest_stream_id = 0;
	m_welcome_acknowledged = false;
	purge_stream(capability_pair_active ? VideoProducerLifecycleState::Stopped
										: VideoProducerLifecycleState::Unsupported);
}

VideoProducerLifecycleResult TargetVideoProducerLifecycle::set_capability_pair_active(bool active) noexcept
{
	if (!active) {
		if (m_state == VideoProducerLifecycleState::Unsupported) {
			return VideoProducerLifecycleResult::NoChange;
		}
		m_welcome_acknowledged = false;
		purge_stream(VideoProducerLifecycleState::Unsupported);
		return VideoProducerLifecycleResult::Applied;
	}
	if (m_state != VideoProducerLifecycleState::Unsupported) {
		return VideoProducerLifecycleResult::NoChange;
	}
	// Capability updates are monotone within a session. Negotiation for a new
	// session is represented only by reset(true); an update may never reactivate
	// a pair that was absent or withdrawn.
	return VideoProducerLifecycleResult::InvalidTransition;
}

VideoProducerLifecycleResult TargetVideoProducerLifecycle::acknowledge_welcome() noexcept
{
	if (m_state == VideoProducerLifecycleState::Unsupported) {
		return VideoProducerLifecycleResult::Unsupported;
	}
	if (m_welcome_acknowledged) {
		return VideoProducerLifecycleResult::NoChange;
	}
	m_welcome_acknowledged = true;
	return VideoProducerLifecycleResult::Applied;
}

VideoProducerLifecycleResult TargetVideoProducerLifecycle::install_config(const TargetVideoConfigPayload& config,
	std::uint64_t target_entity_id) noexcept
{
	if (m_state == VideoProducerLifecycleState::Unsupported) {
		return VideoProducerLifecycleResult::Unsupported;
	}
	if (!m_welcome_acknowledged) {
		return VideoProducerLifecycleResult::WelcomeNotAcknowledged;
	}
	if (validate_target_video_config_payload(config) != ValidationError::None ||
		config.result != VideoConfigResult::Accepted || target_entity_id == 0) {
		return VideoProducerLifecycleResult::InvalidPayload;
	}

	const auto new_stream = m_stream_id == 0;
	if (new_stream) {
		if (config.stream_id <= m_highest_stream_id) {
			return VideoProducerLifecycleResult::CounterNotMonotone;
		}
	} else if (config.stream_id != m_stream_id || config.config_generation <= m_config_generation) {
		return VideoProducerLifecycleResult::CounterNotMonotone;
	}
	if (new_stream) {
		m_highest_stream_id = config.stream_id;
		m_highest_frame_id = 0;
	}
	m_stream_id = config.stream_id;
	m_config_generation = config.config_generation;
	m_config = config;
	m_target_entity_id = target_entity_id;
	m_config_acknowledged = false;
	m_first_frame_of_generation = true;
	m_state = VideoProducerLifecycleState::Configured;
	return VideoProducerLifecycleResult::Applied;
}

VideoProducerLifecycleResult TargetVideoProducerLifecycle::acknowledge_config_applied(std::uint32_t stream_id,
	std::uint32_t config_generation) noexcept
{
	if (m_state != VideoProducerLifecycleState::Configured || stream_id != m_stream_id ||
		config_generation != m_config_generation) {
		return VideoProducerLifecycleResult::InvalidTransition;
	}
	if (m_config_acknowledged) {
		return VideoProducerLifecycleResult::NoChange;
	}
	m_config_acknowledged = true;
	return VideoProducerLifecycleResult::Applied;
}

VideoProducerLifecycleResult
TargetVideoProducerLifecycle::authorize_frame(const TargetVideoFramePayload& frame) noexcept
{
	if (validate_target_video_frame_payload(frame) != ValidationError::None) {
		return VideoProducerLifecycleResult::InvalidPayload;
	}
	if (m_state != VideoProducerLifecycleState::Configured && m_state != VideoProducerLifecycleState::Live) {
		return VideoProducerLifecycleResult::InvalidTransition;
	}
	if (!m_config_acknowledged) {
		return VideoProducerLifecycleResult::ConfigNotAcknowledged;
	}
	if (frame.stream_id != m_stream_id || frame.config_generation != m_config_generation ||
		frame.video_frame_id <= m_highest_frame_id) {
		return VideoProducerLifecycleResult::CounterNotMonotone;
	}
	if (frame.target_entity_id != m_target_entity_id) {
		return VideoProducerLifecycleResult::TargetMismatch;
	}
	if (validate_target_video_frame_payload(frame, m_config) != ValidationError::None) {
		return VideoProducerLifecycleResult::InvalidPayload;
	}
	if (m_first_frame_of_generation && !frame_is_idr(frame)) {
		return VideoProducerLifecycleResult::IdrRequired;
	}
	m_highest_frame_id = frame.video_frame_id;
	m_first_frame_of_generation = false;
	m_state = VideoProducerLifecycleState::Live;
	return VideoProducerLifecycleResult::ReadyToSend;
}

VideoProducerLifecycleResult TargetVideoProducerLifecycle::stop(std::uint32_t stream_id,
	std::uint32_t config_generation) noexcept
{
	if (m_state == VideoProducerLifecycleState::Unsupported) {
		return VideoProducerLifecycleResult::Unsupported;
	}
	if (m_stream_id == 0) {
		return VideoProducerLifecycleResult::NoChange;
	}
	if (stream_id != m_stream_id || config_generation != m_config_generation) {
		return VideoProducerLifecycleResult::InvalidTransition;
	}
	purge_stream(VideoProducerLifecycleState::Stopped);
	return VideoProducerLifecycleResult::Applied;
}

VideoDecisionCacheResult VideoSubscribeDecisionCache::remember(std::uint32_t request_id,
	ByteView encoded_subscribe,
	ByteView encoded_config,
	std::uint64_t now_us,
	VideoDecisionOutput& output) noexcept
{
	if (request_id == 0 || encoded_subscribe.data == nullptr ||
		encoded_subscribe.size != TargetVideoSubscribePayloadSize || encoded_config.data == nullptr ||
		encoded_config.size != TargetVideoConfigPayloadSize) {
		return VideoDecisionCacheResult::InvalidArgument;
	}
	if (m_clock_regressed || now_us < m_last_time_us) {
		m_clock_regressed = true;
		return VideoDecisionCacheResult::ClockRegressed;
	}
	m_last_time_us = now_us;
	for (auto& entry : m_entries) {
		if (entry.active && now_us - entry.stored_at_us >= VideoDecisionRetentionUs) {
			entry = Entry{};
			--m_size;
		}
	}

	for (const auto& entry : m_entries) {
		if (!entry.active || entry.request_id != request_id) {
			continue;
		}
		if (!std::equal(entry.encoded_subscribe.begin(),
				entry.encoded_subscribe.end(),
				encoded_subscribe.data)) {
			return VideoDecisionCacheResult::ConflictingDuplicate;
		}
		VideoDecisionOutput candidate;
		candidate.encoded_config = entry.encoded_config;
		candidate.duplicate = true;
		output = candidate;
		return VideoDecisionCacheResult::Duplicate;
	}
	if (request_id <= m_highest_request_id) {
		return VideoDecisionCacheResult::StaleRequest;
	}

	auto slot = std::find_if(m_entries.begin(), m_entries.end(), [](const Entry& entry) { return !entry.active; });
	if (slot == m_entries.end()) {
		return VideoDecisionCacheResult::ResourceLimit;
	}
	Entry candidate;
	candidate.active = true;
	candidate.request_id = request_id;
	candidate.stored_at_us = now_us;
	std::copy(encoded_subscribe.data,
		encoded_subscribe.data + encoded_subscribe.size,
		candidate.encoded_subscribe.begin());
	std::copy(encoded_config.data, encoded_config.data + encoded_config.size, candidate.encoded_config.begin());
	*slot = candidate;
	++m_size;
	m_highest_request_id = request_id;

	VideoDecisionOutput decision;
	decision.encoded_config = candidate.encoded_config;
	decision.duplicate = false;
	output = decision;
	return VideoDecisionCacheResult::Stored;
}

std::size_t VideoSubscribeDecisionCache::purge_expired(std::uint64_t now_us) noexcept
{
	if (m_clock_regressed || now_us < m_last_time_us) {
		m_clock_regressed = true;
		return 0;
	}
	m_last_time_us = now_us;
	std::size_t purged = 0;
	for (auto& entry : m_entries) {
		if (entry.active && now_us - entry.stored_at_us >= VideoDecisionRetentionUs) {
			entry = Entry{};
			--m_size;
			++purged;
		}
	}
	return purged;
}

void VideoSubscribeDecisionCache::reset(std::uint64_t now_us) noexcept
{
	for (auto& entry : m_entries) {
		entry = Entry{};
	}
	m_size = 0;
	m_highest_request_id = 0;
	m_last_time_us = now_us;
	m_clock_regressed = false;
}

SpecializedOperationRateLimiter::SpecializedOperationRateLimiter(SpecializedRateLimitClass limit_class,
	std::uint64_t initial_time_us) noexcept
	: m_bucket(specialized_rate_limit_profile(limit_class), initial_time_us)
{
}

TokenBucketResult SpecializedOperationRateLimiter::consume(std::uint64_t now_us) noexcept
{
	return m_bucket.try_consume(now_us);
}

void SpecializedOperationRateLimiter::reset(std::uint64_t now_us) noexcept
{
	m_bucket.reset(now_us);
}

TelemetrySendPriority classify_telemetry_send(const TelemetrySendClassification& item) noexcept
{
	switch (item.message_type) {
	case MessageType::Manifest:
	case MessageType::FullSnapshot:
		return TelemetrySendPriority::ReliableState;
	case MessageType::EventBatch:
		if (item.comm_view_state || !item.reliable_event_batch) {
			return TelemetrySendPriority::ReplaceableState;
		}
		return TelemetrySendPriority::ReliableState;
	case MessageType::Delta:
		return TelemetrySendPriority::ReplaceableState;
	case MessageType::TargetVideoFrame:
		if (!item.video_frame_is_idr) {
			return TelemetrySendPriority::VideoInterframe;
		}
		return item.retransmission ? TelemetrySendPriority::IdrRetransmission
								   : TelemetrySendPriority::NewIdr;
	case MessageType::TargetVideoStats:
		return TelemetrySendPriority::VideoStats;
	default:
		return TelemetrySendPriority::CriticalControl;
	}
}

bool drop_on_would_block(TelemetrySendPriority priority) noexcept
{
	return priority == TelemetrySendPriority::NewIdr || priority == TelemetrySendPriority::VideoInterframe ||
		   priority == TelemetrySendPriority::VideoStats;
}

} // namespace telemetry::protocol
