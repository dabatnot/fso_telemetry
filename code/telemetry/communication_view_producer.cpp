#include "telemetry/communication_view_producer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace telemetry::detail {

void CommunicationViewProducer::configure(const CommunicationBundle* bundle) noexcept
{
	m_bundle = bundle;
	m_available = bundle != nullptr && !bundle->assets.empty();
	m_state = {};
	m_read = m_write = 0U;
	m_next_playback_id = m_next_event_id = 1U;
	m_latest_sample_generation = m_next_periodic_state_us = 0U;
	m_sender_signature = m_mission_generation = 0U;
	m_state_dirty = false;
}

bool CommunicationViewProducer::push_event(protocol::CommEventKind kind,
	protocol::CommStopReason reason, std::uint64_t now_us) noexcept
{
	const auto next = (m_write + 1U) % EventCapacity;
	if (next == m_read || m_next_event_id == std::numeric_limits<std::uint64_t>::max()) {
		m_available = false;
		communication_view_bridge().set_enabled(false);
		return false;
	}
	auto& event = m_events[m_write];
	event = {};
	event.payload.event_id = m_next_event_id++;
	event.payload.event_kind = kind;
	event.payload.playback_id = m_state.playback_id;
	event.payload.producer_sample_time_us = now_us;
	event.sender_object_signature = m_sender_signature;
	if (kind == protocol::CommEventKind::Start) {
		event.payload.playback_mode = m_state.playback_mode;
		event.payload.color_mode = m_state.color_mode;
		event.payload.engine_message_id = m_state.engine_message_id;
		event.payload.head_asset_id = m_state.head_asset_id;
		event.payload.animation_time_us = m_state.animation_time_us;
		event.payload.duration_us = m_state.duration_us;
		event.payload.playback_rate = m_state.playback_rate;
	} else {
		event.payload.stop_reason = reason;
	}
	m_write = next;
	return true;
}

void CommunicationViewProducer::stop(protocol::CommStopReason reason, std::uint64_t now_us) noexcept
{
	if (!m_state.active) return;
	(void)push_event(protocol::CommEventKind::Stop, reason, now_us);
	m_state = {};
	m_state.stop_reason = reason;
	m_sender_signature = 0U;
	m_state_dirty = true;
}

void CommunicationViewProducer::tick(std::uint64_t now_us, std::uint32_t mission_generation,
	bool paused, float time_compression) noexcept
{
	if (!m_available || m_bundle == nullptr) return;
	if (m_mission_generation != 0U && mission_generation != m_mission_generation) stop(protocol::CommStopReason::MissionChanged, now_us);
	m_mission_generation = mission_generation;
	CommunicationViewNotification notification;
	while (communication_view_bridge().pop(notification)) {
		if (notification.kind == CommunicationNotificationKind::Stop) {
			stop(notification.stop_reason, now_us);
			continue;
		}
		if (m_state.active) stop(protocol::CommStopReason::Replaced, now_us);
		const auto asset_id = m_bundle->resolve(notification.sample.generic_anim_name.data(), notification.sample.generic_anim_type);
		const auto* asset = m_bundle->find_asset(asset_id);
		if (asset == nullptr || notification.sample.duration_us == 0U) {
			++m_unknown_asset_count;
			continue;
		}
		if (m_next_playback_id == std::numeric_limits<std::uint64_t>::max()) {
			m_available = false; communication_view_bridge().set_enabled(false); return;
		}
		m_state = {};
		m_state.active = true;
		m_state.playback_id = m_next_playback_id++;
		m_state.engine_message_id = notification.sample.engine_message_id;
		m_state.head_asset_id = asset_id;
		m_state.producer_sample_time_us = now_us;
		m_state.animation_time_us = std::min(notification.sample.animation_time_us, asset->duration_us);
		m_state.duration_us = asset->duration_us;
		m_state.playback_mode = notification.sample.playback_mode;
		m_state.color_mode = notification.sample.color_mode;
		m_state.playback_rate = paused ? 0.0F : notification.sample.playback_rate;
		m_sender_signature = notification.sample.sender_object_signature;
		(void)push_event(protocol::CommEventKind::Start, protocol::CommStopReason::None, now_us);
		m_state_dirty = true;
		m_next_periodic_state_us = now_us + 100'000U;
	}
	CommunicationViewSample sample;
	std::uint64_t generation = 0U;
	if (m_state.active && communication_view_bridge().latest(sample, generation) && generation != m_latest_sample_generation) {
		m_latest_sample_generation = generation;
		const auto previous_rate = m_state.playback_rate;
		const auto rate = paused ? 0.0F : sample.playback_rate;
		const bool metadata_changed = previous_rate != rate || m_state.playback_mode != sample.playback_mode ||
			m_state.color_mode != sample.color_mode;
		const auto elapsed = now_us >= m_state.producer_sample_time_us ? now_us - m_state.producer_sample_time_us : 0U;
		const auto projected = static_cast<double>(m_state.animation_time_us) + static_cast<double>(previous_rate) * elapsed;
		const auto difference = std::fabs(projected - static_cast<double>(sample.animation_time_us));
		const bool discontinuity = difference > 50'000.0;
		if (metadata_changed || discontinuity || now_us >= m_next_periodic_state_us) {
			m_state.producer_sample_time_us = now_us;
			m_state.animation_time_us = std::min(sample.animation_time_us, m_state.duration_us);
			m_state.playback_rate = rate;
			m_state.playback_mode = sample.playback_mode;
			m_state.color_mode = sample.color_mode;
			m_state_dirty = true;
			m_next_periodic_state_us = now_us + 100'000U;
		}
	}
	if (communication_view_bridge().overflowed()) {
		stop(protocol::CommStopReason::Interrupted, now_us);
		m_available = false;
	}
	(void)time_compression;
}

void CommunicationViewProducer::mission_changed(std::uint64_t now_us) noexcept
{
	stop(protocol::CommStopReason::MissionChanged, now_us);
	communication_view_bridge().purge();
	m_latest_sample_generation = 0U;
}

void CommunicationViewProducer::session_stopped(std::uint64_t now_us) noexcept
{
	stop(protocol::CommStopReason::SessionStopped, now_us);
}

bool CommunicationViewProducer::pop_event(PendingCommunicationEvent& event) noexcept
{
	if (m_read == m_write) return false;
	event = m_events[m_read];
	m_read = (m_read + 1U) % EventCapacity;
	return true;
}

bool CommunicationViewProducer::peek_event(PendingCommunicationEvent& event) const noexcept
{
	if (m_read == m_write) return false;
	event = m_events[m_read];
	return true;
}

void CommunicationViewProducer::consume_event() noexcept
{
	if (m_read != m_write) m_read = (m_read + 1U) % EventCapacity;
}

protocol::CommViewStatePayload CommunicationViewProducer::state(std::uint64_t sender_entity_id) const noexcept
{
	auto result = m_state;
	if (result.active) result.sender_entity_id = sender_entity_id;
	return result;
}

bool CommunicationViewProducer::state_due(std::uint64_t now_us) const noexcept
{
	return m_state_dirty || (m_state.active && now_us >= m_next_periodic_state_us);
}

void CommunicationViewProducer::state_published(std::uint64_t now_us) noexcept
{
	m_state_dirty = false;
	m_next_periodic_state_us = now_us + 100'000U;
}

} // namespace telemetry::detail
