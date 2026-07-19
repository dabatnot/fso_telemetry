#pragma once

#include <cstddef>
#include <cstdint>

namespace telemetry::detail {

// Runtime-owned accounting for Phase 1 replication container growth. It is
// inactive in normal production operation and can be scoped by benchmarks or
// test seams around actual runtime work, without a production dependency on
// a test binary's global allocator override.
class Phase1AllocationObserver final {
  public:
	void begin() noexcept { m_observed = 0U; m_active = true; }
	void end() noexcept { m_active = false; }
	std::uint64_t observed() const noexcept { return m_observed; }
	void note_growth(std::uint64_t before, std::uint64_t after) noexcept
	{
		if (m_active && after > before && m_observed != UINT64_MAX) {
			++m_observed;
		}
	}

  private:
	std::uint64_t m_observed = 0U;
	bool m_active = false;
};

} // namespace telemetry::detail
