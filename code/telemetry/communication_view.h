#pragma once

#include "globalincs/pstypes.h"
#include "telemetry/protocol/telemetry_protocol_constants.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace telemetry {

struct CommunicationViewSample {
	std::array<char, MAX_FILENAME_LEN> generic_anim_name{};
	protocol::SourceFormat generic_anim_type = protocol::SourceFormat::Invalid;
	std::uint32_t engine_message_id = 0U;
	std::uint32_t sender_object_signature = 0U;
	std::uint64_t animation_time_us = 0U;
	std::uint64_t duration_us = 0U;
	protocol::CommPlaybackMode playback_mode = protocol::CommPlaybackMode::Once;
	protocol::CommColorMode color_mode = protocol::CommColorMode::HudTint;
	float playback_rate = 0.0F;
};

enum class CommunicationNotificationKind : std::uint8_t { Start = 1, Stop = 2 };

struct CommunicationViewNotification {
	CommunicationNotificationKind kind = CommunicationNotificationKind::Start;
	CommunicationViewSample sample{};
	protocol::CommStopReason stop_reason = protocol::CommStopReason::None;
};

class CommunicationViewBridge final {
  public:
	static constexpr std::size_t QueueCapacity = 16U;

	void set_enabled(bool enabled) noexcept;
	bool enabled() const noexcept { return m_enabled.load(std::memory_order_acquire); }
	bool start(const CommunicationViewSample& sample) noexcept;
	void sample(const CommunicationViewSample& sample) noexcept;
	bool stop(protocol::CommStopReason reason) noexcept;
	bool pop(CommunicationViewNotification& notification) noexcept;
	bool latest(CommunicationViewSample& sample, std::uint64_t& generation) const noexcept;
	void purge() noexcept;
	bool overflowed() const noexcept { return m_overflowed.load(std::memory_order_acquire); }

  private:
	bool push(const CommunicationViewNotification& notification) noexcept;
	std::array<CommunicationViewNotification, QueueCapacity> m_queue{};
	std::atomic<std::size_t> m_write{0U}, m_read{0U};
	mutable std::atomic<std::uint64_t> m_sample_sequence{0U};
	CommunicationViewSample m_latest{};
	std::atomic<bool> m_enabled{false}, m_overflowed{false}, m_active{false};
};

CommunicationViewBridge& communication_view_bridge() noexcept;
void communication_view_started(const CommunicationViewSample& sample) noexcept;
void communication_view_sampled(const CommunicationViewSample& sample) noexcept;
void communication_view_stopped(protocol::CommStopReason reason) noexcept;

} // namespace telemetry
