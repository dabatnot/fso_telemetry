#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace telemetry::detail {

enum class Phase1AllocationGrowthSource : std::uint8_t {
	SnapshotSlotDeltaScratch = 0,
	SnapshotEgressReliable,
	SnapshotEgressScratch,
	DeltaEgressScratch,
	TestProbe,
	Count,
};

// Runtime-owned accounting for Phase 1 replication container growth. It is
// inactive in normal production operation and can be scoped by benchmarks or
// test seams around actual runtime work, without a production dependency on
// a test binary's global allocator override.
class Phase1AllocationObserver final {
  public:
	void begin() noexcept
	{
		m_observed = 0U;
		m_by_source = {};
		m_active = true;
	}
	void end() noexcept { m_active = false; }
	bool active() const noexcept { return m_active; }
	std::uint64_t observed() const noexcept { return m_observed; }
	std::uint64_t observed(
		Phase1AllocationGrowthSource source) const noexcept
	{
		const auto index = static_cast<std::size_t>(source);
		return index < m_by_source.size() ? m_by_source[index] : 0U;
	}
	void note_growth(std::uint64_t before, std::uint64_t after,
		Phase1AllocationGrowthSource source) noexcept
	{
		if (m_active && after > before && m_observed != UINT64_MAX) {
			++m_observed;
			const auto index = static_cast<std::size_t>(source);
			if (index < m_by_source.size() &&
				m_by_source[index] != UINT64_MAX)
				++m_by_source[index];
		}
	}

  private:
	std::uint64_t m_observed = 0U;
	std::array<std::uint64_t,
		static_cast<std::size_t>(
			Phase1AllocationGrowthSource::Count)> m_by_source{};
	bool m_active = false;
};

} // namespace telemetry::detail
