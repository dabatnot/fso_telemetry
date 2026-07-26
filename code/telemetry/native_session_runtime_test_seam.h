#pragma once

// Test-only friend seam. This header is deliberately kept out of the runtime
// public surface; test forwarding headers may include it without creating a
// production dependency on test sources.
#include "telemetry/native_session_runtime.h"
#include "telemetry/logging.h"
#include "telemetry/session_controller_test_seam.h"

#include <cstddef>
#include <cstdint>

namespace telemetry::detail {

class NativeSessionRuntimeTestAccess final {
  public:
	static constexpr bool private_contract_available() noexcept { return true; }
	static const SessionControllerSlot* slot(const NativeSessionRuntime& runtime, std::size_t index) noexcept;
	static SessionController* controller(NativeSessionRuntime& runtime) noexcept;
	static void set_session_controller_provision_failure(NativeSessionRuntime& runtime, bool fail) noexcept;
	static void set_startup_owned_budget_adjustment(
		NativeSessionRuntime& runtime, std::size_t additional_bytes) noexcept;
	static std::size_t startup_owned_bytes(
		const NativeSessionRuntime& runtime) noexcept;
	static std::uint64_t startup_allocation_count(
		const NativeSessionRuntime& runtime) noexcept;
	static Phase2CapturePlan phase2_keyframe_test_seam(
		NativeSessionRuntime& runtime) noexcept;
	static void begin_steady_state_allocation_tracking(NativeSessionRuntime& runtime) noexcept;
	static std::uint64_t steady_state_allocation_count(const NativeSessionRuntime& runtime) noexcept;
	static void force_steady_state_allocation_for_tests(NativeSessionRuntime& runtime) noexcept;
	// The P9.3 runner enables this only after native startup/warm-up and reads a
	// bounded copy after each real service_tick. No timing hook is active in a
	// normal game tick.
	static void begin_performance_observation(NativeSessionRuntime& runtime) noexcept;
	static NativeRuntimePerformanceSample last_performance_sample(const NativeSessionRuntime& runtime) noexcept;
	static bool seed_last_allocated_entity_id(NativeSessionRuntime& runtime,
		std::size_t index,
		std::uint64_t last_id) noexcept;
	static NativeSessionTickStatus inject_collected_player_capture(NativeSessionRuntime& runtime,
		const CaptureResult& result,
		const PlayerObservationDto& observation) noexcept;
	static NativeSessionTickStatus service_r2_tick(NativeSessionRuntime& runtime,
		const NativeSessionTickContext& context) noexcept;
	// P9.2 integration-only observation. This returns a copy of the logger's
	// fixed record array; it never exposes the mutable logger or a production API.
	static TelemetryLogSnapshot log_snapshot(const NativeSessionRuntime& runtime) noexcept;
};

} // namespace telemetry::detail
