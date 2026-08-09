#include "telemetry/runtime_adapter.h"

#include <cstddef>
#include <cstdint>
#include <thread>

namespace telemetry::detail {
namespace {

class ReadyRuntimeServices final : public RuntimeStartupServices {
  public:
	void reset() noexcept
	{
		m_captured_thread = std::thread::id{};
		m_main_thread_captured = false;
		m_registry_ready = false;
		m_transport_active = false;
		m_service_calls = 0U;
		m_lifecycle_calls = 0U;
		m_diagnostic_calls = 0U;
	}

	void capture_main_thread() noexcept override
	{
		++m_service_calls;
		m_captured_thread = std::this_thread::get_id();
		m_main_thread_captured = true;
	}

	bool is_on_captured_main_thread() noexcept override
	{
		++m_service_calls;
		return m_main_thread_captured && std::this_thread::get_id() == m_captured_thread;
	}

	RuntimeConfigResult load_config() noexcept override
	{
		++m_service_calls;
		return {RuntimeConfigStatus::Enabled, 1U};
	}

	IdentityResult load_producer_identity() noexcept override
	{
		++m_service_calls;
		return {7U, IdentityError::None};
	}

	SessionIdCandidateResult draw_session_candidate() noexcept override
	{
		++m_service_calls;
		return {SessionIdCandidateStatus::Ready, 42U};
	}

	Wp03KnownBudgetSubtotal calculate_known_budget(std::size_t) noexcept override
	{
		++m_service_calls;
		Wp03KnownBudgetSubtotal result;
		result.error = StartupBudgetError::None;
		result.is_complete = true;
		result.deferred_categories = 0U;
		result.known_bytes = SessionIdRegistryStorageBytes;
		result.metric_known_bytes = SessionIdRegistryStorageBytes;
		result.session_id_registry_bytes = SessionIdRegistryStorageBytes;
		result.client_slot_count = 1U;
		return result;
	}

	bool allocate_session_registry() noexcept override
	{
		++m_service_calls;
		m_registry_ready = true;
		return true;
	}

	SessionIdRegistrationStatus register_session_candidate(std::uint64_t) noexcept override
	{
		++m_service_calls;
		return SessionIdRegistrationStatus::Registered;
	}

	RuntimeTransportStatus start_transport() noexcept override
	{
		++m_service_calls;
		m_transport_active = true;
		return RuntimeTransportStatus::Started;
	}

	void stop_collection() noexcept override
	{
		record_lifecycle();
	}
	void invalidate_mission_state_and_entities() noexcept override
	{
		record_lifecycle();
	}
	void cancel_replication() noexcept override
	{
		record_lifecycle();
	}
	void close_sessions_and_stores() noexcept override
	{
		record_lifecycle();
	}
	void reset_mission_scope() noexcept override
	{
		record_lifecycle();
	}
	void stop_transport() noexcept override
	{
		record_lifecycle();
		m_transport_active = false;
	}
	void emit_runtime_summary() noexcept override
	{
		record_lifecycle();
	}
	void release_runtime_allocations() noexcept override
	{
		record_lifecycle();
	}
	void release_session_registry() noexcept override
	{
		record_lifecycle();
		m_registry_ready = false;
	}

	void emit_startup_diagnostic(RuntimeTerminalReason) noexcept override
	{
		++m_service_calls;
		++m_diagnostic_calls;
	}

	std::uint64_t service_calls() const noexcept
	{
		return m_service_calls;
	}
	std::uint64_t lifecycle_calls() const noexcept
	{
		return m_lifecycle_calls;
	}
	std::uint64_t diagnostic_calls() const noexcept
	{
		return m_diagnostic_calls;
	}

  private:
	void record_lifecycle() noexcept
	{
		++m_service_calls;
		++m_lifecycle_calls;
	}

	std::thread::id m_captured_thread{};
	bool m_main_thread_captured = false;
	bool m_registry_ready = false;
	bool m_transport_active = false;
	std::uint64_t m_service_calls = 0U;
	std::uint64_t m_lifecycle_calls = 0U;
	std::uint64_t m_diagnostic_calls = 0U;
};

ReadyRuntimeServices& ready_services() noexcept
{
	static ReadyRuntimeServices services;
	return services;
}

std::uint64_t factory_requests = 0U;

} // namespace

RuntimeStartupServices& runtime_startup_services() noexcept
{
	++factory_requests;
	return ready_services();
}

} // namespace telemetry::detail

namespace telemetry::test_seam {

void reset_ready_runtime_adapter() noexcept
{
	detail::ready_services().reset();
	detail::factory_requests = 0U;
}

std::uint64_t ready_runtime_factory_requests() noexcept
{
	return detail::factory_requests;
}

std::uint64_t ready_runtime_service_calls() noexcept
{
	return detail::ready_services().service_calls();
}

std::uint64_t ready_runtime_lifecycle_calls() noexcept
{
	return detail::ready_services().lifecycle_calls();
}

std::uint64_t ready_runtime_diagnostic_calls() noexcept
{
	return detail::ready_services().diagnostic_calls();
}

} // namespace telemetry::test_seam
