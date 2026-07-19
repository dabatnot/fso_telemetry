#pragma once

#include "telemetry/runtime.h"
#include "telemetry/config.h"
#include "telemetry/logging.h"

namespace telemetry::detail {

class NativeSessionRuntime;
enum class NativeSessionTickStatus : std::uint8_t;

class RuntimeAdapterPlayerTestAccess final {
  public:
	static RuntimeTickStatus service_tick(NativeSessionRuntime* runtime,
		const RuntimeTickContext& context) noexcept;
	static RuntimeTickStatus map_tick_status(NativeSessionTickStatus status) noexcept;
	static const char* startup_diagnostic_message(RuntimeTerminalReason reason) noexcept;
	static TelemetryLogReason config_log_reason(ConfigError error) noexcept;
	static void stop_collection(NativeSessionRuntime* runtime) noexcept;
	static void invalidate_mission_state_and_entities(NativeSessionRuntime* runtime) noexcept;
	static void close_sessions_and_stores(NativeSessionRuntime* runtime) noexcept;
	static void stop_transport(NativeSessionRuntime* runtime) noexcept;
};

RuntimeStartupServices& runtime_startup_services() noexcept;

} // namespace telemetry::detail
