#pragma once

// Test-only diagnostics seam. It is intentionally separate from runtime.h so
// shipping callers cannot obtain the adapter-owned logger or mutate it.
#include "telemetry/logging.h"
#include "telemetry/runtime.h"

#include <cstdint>

namespace telemetry::detail {

class RuntimeAdapterDiagnosticsTestAccess final {
  public:
	static void reset_for_test() noexcept;
	static void set_delivery_suppressed_for_test(bool suppressed) noexcept;
	static void emit_activation_for_test(std::uint64_t now_us) noexcept;
	static void emit_fault_for_test(RuntimeTerminalReason reason) noexcept;
	static TelemetryLogFault runtime_fault_log(RuntimeTerminalReason reason) noexcept;
	static TelemetryLogSnapshot log_snapshot() noexcept;
};

} // namespace telemetry::detail
