#pragma once

#include <cstddef>

namespace telemetry::test::wp03 {

void begin_global_allocation_tracking() noexcept;
std::size_t end_global_allocation_tracking() noexcept;

class GlobalAllocationScope final {
  public:
	GlobalAllocationScope() noexcept;
	~GlobalAllocationScope();
	GlobalAllocationScope(const GlobalAllocationScope&) = delete;
	GlobalAllocationScope& operator=(const GlobalAllocationScope&) = delete;
	std::size_t finish() noexcept;

  private:
	bool m_active = true;
};

} // namespace telemetry::test::wp03
