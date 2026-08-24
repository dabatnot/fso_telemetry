#pragma once

#include "telemetry/communication_bundle.h"
#include "telemetry/communication_view.h"
#include "telemetry/protocol/telemetry_specialized_views.h"

#include <array>
#include <cstdint>

namespace telemetry::detail {

struct PendingCommunicationEvent {
	protocol::CommViewEventPayload payload{};
	std::uint32_t sender_object_signature = 0U;
};

class CommunicationViewProducer final {
  public:
	static constexpr std::size_t EventCapacity = CommunicationViewBridge::QueueCapacity;

	void configure(const CommunicationBundle* bundle) noexcept;
	void tick(std::uint64_t now_us, std::uint32_t mission_generation, bool paused, float time_compression) noexcept;
	void mission_changed(std::uint64_t now_us) noexcept;
	void session_stopped(std::uint64_t now_us) noexcept;
	bool pop_event(PendingCommunicationEvent& event) noexcept;
	bool peek_event(PendingCommunicationEvent& event) const noexcept;
	void consume_event() noexcept;
	protocol::CommViewStatePayload state(std::uint64_t sender_entity_id) const noexcept;
	bool state_due(std::uint64_t now_us) const noexcept;
	void state_published(std::uint64_t now_us) noexcept;
	bool available() const noexcept { return m_available; }
	bool active() const noexcept { return m_state.active; }
	std::uint32_t sender_signature() const noexcept { return m_sender_signature; }
	std::uint64_t unknown_asset_count() const noexcept { return m_unknown_asset_count; }

  private:
	bool push_event(protocol::CommEventKind kind, protocol::CommStopReason reason, std::uint64_t now_us) noexcept;
	void stop(protocol::CommStopReason reason, std::uint64_t now_us) noexcept;
	const CommunicationBundle* m_bundle = nullptr;
	protocol::CommViewStatePayload m_state{};
	std::array<PendingCommunicationEvent, EventCapacity> m_events{};
	std::size_t m_read = 0U, m_write = 0U;
	std::uint64_t m_next_playback_id = 1U;
	std::uint64_t m_next_event_id = 1U;
	std::uint64_t m_latest_sample_generation = 0U;
	std::uint64_t m_next_periodic_state_us = 0U;
	std::uint64_t m_unknown_asset_count = 0U;
	std::uint32_t m_sender_signature = 0U;
	std::uint32_t m_mission_generation = 0U;
	bool m_available = false;
	bool m_state_dirty = false;
};

} // namespace telemetry::detail
