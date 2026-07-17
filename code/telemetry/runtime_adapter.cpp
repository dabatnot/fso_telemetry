#include "telemetry/runtime_adapter.h"

#include "cmdline/cmdline.h"
#include "globalincs/pstypes.h"
#include "io/timer.h"
#include "telemetry/config.h"
#include "telemetry/native_session_runtime.h"

#include <memory>
#include <new>
#include <thread>

namespace telemetry::detail {
namespace {

class NativeRuntimeStartupServices final : public RuntimeStartupServices {
  public:
	NativeRuntimeStartupServices() noexcept : m_session_ids(m_random, m_session_registry) {}

	void capture_main_thread() noexcept override
	{
		m_captured_thread = std::this_thread::get_id();
		m_main_thread_captured = true;
	}

	bool is_on_captured_main_thread() noexcept override
	{
		return m_main_thread_captured && std::this_thread::get_id() == m_captured_thread;
	}

	RuntimeConfigResult load_config() noexcept override
	{
		const auto loaded = load_telemetry_config();
		m_effective_config = loaded.effective;
		switch (loaded.status) {
		case ConfigStatus::Absent:
			return RuntimeConfigResult{RuntimeConfigStatus::Absent, 0U};
		case ConfigStatus::ValidDisabled:
			return RuntimeConfigResult{RuntimeConfigStatus::Disabled, m_effective_config.max_clients};
		case ConfigStatus::ValidEnabled:
			return RuntimeConfigResult{RuntimeConfigStatus::Enabled, m_effective_config.max_clients};
		case ConfigStatus::Invalid:
			return RuntimeConfigResult{RuntimeConfigStatus::Invalid, 0U};
		}
		return RuntimeConfigResult{RuntimeConfigStatus::Invalid, 0U};
	}

	IdentityResult load_producer_identity() noexcept override
	{
		NativeProducerProfileStore store(profile_storage_root_for_mode(Cmdline_portable_mode));
		const auto result = load_or_create_producer_identity(store, m_random);
		m_producer_id = result.error == IdentityError::None ? result.producer_id : 0U;
		return result;
	}

	SessionIdCandidateResult draw_session_candidate() noexcept override
	{
		return draw_session_id_candidate(m_random);
	}

	Wp03KnownBudgetSubtotal calculate_known_budget(std::size_t max_clients) noexcept override
	{
		return calculate_wp06_startup_budget(
			calculate_wp04_startup_budget(make_wp03_known_budget_request(max_clients)), max_clients);
	}

	bool allocate_session_registry() noexcept override
	{
		return m_session_registry.allocate_storage();
	}

	SessionIdRegistrationStatus register_session_candidate(std::uint64_t candidate) noexcept override
	{
		return m_session_registry.register_candidate(candidate);
	}

	RuntimeTransportStatus start_transport() noexcept override
	{
		// Runtime calls this seam only after the complete-budget gate. The real
		// Phase 1 budget remains incomplete (0x00f0), so production cannot reach
		// this construction or bind a socket yet.
		auto native = std::unique_ptr<NativeSessionRuntime>(
			new (std::nothrow) NativeSessionRuntime(m_backend, m_output_completion));
		if (native == nullptr) {
			return RuntimeTransportStatus::Unavailable;
		}
		const NativeSessionStartRequest request{&m_effective_config,
			m_producer_id,
			&m_session_ids,
			&m_random};
		if (native->start(request) != NativeSessionStartStatus::Started) {
			return RuntimeTransportStatus::Unavailable;
		}
		m_native = std::move(native);
		return RuntimeTransportStatus::Started;
	}

	std::uint64_t monotonic_now_us() noexcept override { return timer_get_microseconds(); }
	RuntimeTickStatus service_tick(const RuntimeTickContext& context) noexcept override
	{
		if (m_native == nullptr) {
			return RuntimeTickStatus::Unavailable;
		}
		const auto status = m_native->service_tick(
			{context.now_us, context.mission_generation, context.mission_active});
		switch (status) {
		case NativeSessionTickStatus::Complete:
			return RuntimeTickStatus::Complete;
		case NativeSessionTickStatus::Unavailable:
			return RuntimeTickStatus::Unavailable;
		case NativeSessionTickStatus::PermanentTransportFailure:
			return RuntimeTickStatus::PermanentTransportFailure;
		}
		return RuntimeTickStatus::PermanentTransportFailure;
	}

	void stop_collection() noexcept override {}
	void invalidate_mission_state_and_entities() noexcept override
	{
		if (m_native != nullptr) {
			m_native->purge_all(SessionCloseReason::MissionDiscontinuity);
		}
	}
	void cancel_replication() noexcept override {}
	void close_sessions_and_stores() noexcept override
	{
		if (m_native != nullptr) {
			m_native->purge_all(SessionCloseReason::Shutdown);
		}
	}
	void reset_mission_scope() noexcept override {}
	void stop_transport() noexcept override
	{
		if (m_native != nullptr) {
			m_native->shutdown();
		}
	}
	void emit_runtime_summary() noexcept override {}
	void release_runtime_allocations() noexcept override { m_native.reset(); }

	void release_session_registry() noexcept override
	{
		m_session_registry.release_storage();
	}

	void emit_startup_diagnostic(RuntimeTerminalReason reason) noexcept override
	{
		switch (reason) {
		case RuntimeTerminalReason::ConfigAbsent:
			mprintf(("Telemetry startup disabled: configuration absent.\n"));
			break;
		case RuntimeTerminalReason::ConfigInvalid:
			mprintf(("Telemetry startup disabled: invalid configuration.\n"));
			break;
		case RuntimeTerminalReason::ConfigDisabled:
			mprintf(("Telemetry startup disabled by configuration.\n"));
			break;
		case RuntimeTerminalReason::MainThreadNotCaptured:
			mprintf(("Telemetry startup failed: main thread was not captured.\n"));
			break;
		case RuntimeTerminalReason::MainThreadViolation:
			mprintf(("Telemetry startup failed: main-thread invariant.\n"));
			break;
		case RuntimeTerminalReason::ProducerIdentityFailure:
			mprintf(("Telemetry startup failed: producer identity.\n"));
			break;
		case RuntimeTerminalReason::SessionCandidateFailure:
			mprintf(("Telemetry startup failed: session entropy.\n"));
			break;
		case RuntimeTerminalReason::BudgetFailure:
			mprintf(("Telemetry startup failed: memory budget.\n"));
			break;
		case RuntimeTerminalReason::AllocationFailure:
			mprintf(("Telemetry startup failed: startup allocation.\n"));
			break;
		case RuntimeTerminalReason::SessionRegistrationFailure:
			mprintf(("Telemetry startup failed: session registration.\n"));
			break;
		case RuntimeTerminalReason::TransportUnavailable:
			mprintf(("Telemetry startup failed: transport unavailable.\n"));
			break;
		case RuntimeTerminalReason::InvalidLifecycleTransition:
			mprintf(("Telemetry runtime failed: invalid lifecycle transition.\n"));
			break;
		case RuntimeTerminalReason::MissionGenerationOverflow:
			mprintf(("Telemetry runtime failed: mission generation overflow.\n"));
			break;
		case RuntimeTerminalReason::None:
			mprintf(("Telemetry startup stopped.\n"));
			break;
		}
	}

  private:
	TelemetryConfig m_effective_config;
	NativeUdpSocketBackend m_backend;
	NativeOutputCompletionForwarder m_output_completion;
	OsRandomSource m_random;
	SessionIdRegistry m_session_registry;
	SessionIdAllocator m_session_ids;
	std::unique_ptr<NativeSessionRuntime> m_native;
	std::thread::id m_captured_thread{};
	std::uint64_t m_producer_id = 0U;
	bool m_main_thread_captured = false;
};

} // namespace

RuntimeStartupServices& runtime_startup_services() noexcept
{
	static NativeRuntimeStartupServices services;
	return services;
}

} // namespace telemetry::detail
