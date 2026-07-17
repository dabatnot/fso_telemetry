#pragma once

#include <cstdint>

namespace telemetry::detail {

enum class CaptureCadenceStatus : std::uint8_t {
	Inactive = 0,
	NotDue,
	Due,
	InvalidRate,
	ClockRegression,
	DeadlineOverflow,
	Count,
};

struct CaptureCadenceResult {
	CaptureCadenceStatus status = CaptureCadenceStatus::InvalidRate;
	std::uint64_t next_due_us = 0U;
};

class Capture30Hz final {
  public:
	bool configure(std::uint32_t flight_hz) noexcept;
	CaptureCadenceResult poll(std::uint64_t now_us, bool active) noexcept;
	void stop() noexcept;
	void reset() noexcept;

	std::uint64_t period_us() const noexcept
	{
		return m_period_us;
	}
	bool armed() const noexcept
	{
		return m_armed;
	}
	std::uint64_t next_due_us() const noexcept
	{
		return m_next_due_us;
	}

  private:
	CaptureCadenceResult schedule_from(std::uint64_t now_us) noexcept;

	std::uint64_t m_period_us = 0U;
	std::uint64_t m_next_due_us = 0U;
	std::uint64_t m_last_time_us = 0U;
	bool m_armed = false;
	bool m_have_last_time = false;
};

} // namespace telemetry::detail
