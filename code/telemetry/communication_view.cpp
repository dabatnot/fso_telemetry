#include "telemetry/communication_view.h"

namespace telemetry {

void CommunicationViewBridge::set_enabled(bool enabled) noexcept
{
	if (!enabled) purge();
	m_enabled.store(enabled, std::memory_order_release);
}

bool CommunicationViewBridge::push(const CommunicationViewNotification& notification) noexcept
{
	const auto write = m_write.load(std::memory_order_relaxed);
	const auto next = (write + 1U) % QueueCapacity;
	if (next == m_read.load(std::memory_order_acquire)) {
		m_overflowed.store(true, std::memory_order_release);
		m_enabled.store(false, std::memory_order_release);
		return false;
	}
	m_queue[write] = notification;
	m_write.store(next, std::memory_order_release);
	return true;
}

bool CommunicationViewBridge::start(const CommunicationViewSample& sample) noexcept
{
	if (!enabled()) return false;
	if (m_active.load(std::memory_order_acquire) && !stop(protocol::CommStopReason::Replaced)) return false;
	CommunicationViewNotification notification;
	notification.kind = CommunicationNotificationKind::Start;
	notification.sample = sample;
	if (!push(notification)) return false;
	m_active.store(true, std::memory_order_release);
	this->sample(sample);
	return true;
}

void CommunicationViewBridge::sample(const CommunicationViewSample& sample) noexcept
{
	if (!enabled() || !m_active.load(std::memory_order_acquire)) return;
	const auto sequence = m_sample_sequence.load(std::memory_order_relaxed);
	m_sample_sequence.store(sequence + 1U, std::memory_order_release);
	m_latest = sample;
	m_sample_sequence.store(sequence + 2U, std::memory_order_release);
}

bool CommunicationViewBridge::stop(protocol::CommStopReason reason) noexcept
{
	if (!enabled() || !m_active.exchange(false, std::memory_order_acq_rel)) return false;
	CommunicationViewNotification notification;
	notification.kind = CommunicationNotificationKind::Stop;
	notification.stop_reason = reason;
	return push(notification);
}

bool CommunicationViewBridge::pop(CommunicationViewNotification& notification) noexcept
{
	const auto read = m_read.load(std::memory_order_relaxed);
	if (read == m_write.load(std::memory_order_acquire)) return false;
	notification = m_queue[read];
	m_read.store((read + 1U) % QueueCapacity, std::memory_order_release);
	return true;
}

bool CommunicationViewBridge::latest(CommunicationViewSample& sample, std::uint64_t& generation) const noexcept
{
	for (unsigned attempt = 0U; attempt < 4U; ++attempt) {
		const auto before = m_sample_sequence.load(std::memory_order_acquire);
		if ((before & 1U) != 0U) continue;
		sample = m_latest;
		const auto after = m_sample_sequence.load(std::memory_order_acquire);
		if (before == after) { generation = after / 2U; return after != 0U; }
	}
	return false;
}

void CommunicationViewBridge::purge() noexcept
{
	m_read.store(0U, std::memory_order_release);
	m_write.store(0U, std::memory_order_release);
	m_sample_sequence.store(0U, std::memory_order_release);
	m_latest = {};
	m_active.store(false, std::memory_order_release);
	m_overflowed.store(false, std::memory_order_release);
}

CommunicationViewBridge& communication_view_bridge() noexcept
{
	static CommunicationViewBridge bridge;
	return bridge;
}

void communication_view_started(const CommunicationViewSample& sample) noexcept { (void)communication_view_bridge().start(sample); }
void communication_view_sampled(const CommunicationViewSample& sample) noexcept { communication_view_bridge().sample(sample); }
void communication_view_stopped(protocol::CommStopReason reason) noexcept { (void)communication_view_bridge().stop(reason); }

} // namespace telemetry
