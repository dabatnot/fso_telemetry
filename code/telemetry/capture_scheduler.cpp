#include "telemetry/capture_scheduler.h"

#include <limits>

namespace telemetry::detail {
namespace {

constexpr std::uint64_t MicrosecondsPerSecond = 1'000'000U;
constexpr std::uint32_t MinimumFlightHz = 1U;
constexpr std::uint32_t MaximumFlightHz = 60U;

} // namespace

bool Capture30Hz::configure(std::uint32_t flight_hz) noexcept
{
	reset();
	if (flight_hz < MinimumFlightHz || flight_hz > MaximumFlightHz) {
		return false;
	}

	m_period_us = MicrosecondsPerSecond / flight_hz;
	if (MicrosecondsPerSecond % flight_hz != 0U) {
		++m_period_us;
	}
	return true;
}

CaptureCadenceResult Capture30Hz::poll(std::uint64_t now_us, bool active) noexcept
{
	if (m_period_us == 0U) {
		return {};
	}
	if (m_have_last_time && now_us < m_last_time_us) {
		m_armed = false;
		m_next_due_us = 0U;
		return {CaptureCadenceStatus::ClockRegression, 0U};
	}
	m_last_time_us = now_us;
	m_have_last_time = true;

	if (!active) {
		m_armed = false;
		m_next_due_us = 0U;
		return {CaptureCadenceStatus::Inactive, 0U};
	}
	if (!m_armed || now_us >= m_next_due_us) {
		return schedule_from(now_us);
	}
	return {CaptureCadenceStatus::NotDue, m_next_due_us};
}

void Capture30Hz::stop() noexcept
{
	m_next_due_us = 0U;
	m_last_time_us = 0U;
	m_armed = false;
	m_have_last_time = false;
}

void Capture30Hz::reset() noexcept
{
	stop();
	m_period_us = 0U;
}

CaptureCadenceResult Capture30Hz::schedule_from(std::uint64_t now_us) noexcept
{
	if (m_period_us > std::numeric_limits<std::uint64_t>::max() - now_us) {
		m_armed = false;
		m_next_due_us = 0U;
		return {CaptureCadenceStatus::DeadlineOverflow, 0U};
	}
	m_next_due_us = now_us + m_period_us;
	m_armed = true;
	return {CaptureCadenceStatus::Due, m_next_due_us};
}

} // namespace telemetry::detail
