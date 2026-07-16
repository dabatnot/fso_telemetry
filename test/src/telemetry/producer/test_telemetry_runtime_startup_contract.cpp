#include "telemetry/runtime.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace detail = telemetry::detail;

enum class StartupCall : std::uint8_t {
	CaptureMainThread = 0,
	MainThreadCheck,
	LoadConfig,
	LoadProducerIdentity,
	DrawSessionCandidate,
	CalculateKnownBudget,
	AllocateRegistry,
	RegisterCandidate,
	StartTransport,
	ReleaseRegistry,
	EmitDiagnostic,
};

struct RuntimeObservation {
	StartupCall call = StartupCall::CaptureMainThread;
	detail::RuntimeState state = detail::RuntimeState::Cold;
	std::uint64_t published_session_id = 0U;
	std::size_t socket_count = 0U;
};

detail::Wp03KnownBudgetSubtotal successful_incomplete_budget() noexcept
{
	detail::Wp03KnownBudgetSubtotal result;
	result.error = detail::StartupBudgetError::None;
	result.is_complete = false;
	result.known_bytes = detail::SessionIdRegistryStorageBytes;
	result.metric_known_bytes = detail::SessionIdRegistryStorageBytes;
	result.session_id_registry_bytes = detail::SessionIdRegistryStorageBytes;
	result.client_slot_count = 1U;
	return result;
}

detail::Wp03KnownBudgetSubtotal successful_complete_budget() noexcept
{
	auto result = successful_incomplete_budget();
	result.is_complete = true;
	result.deferred_categories = 0U;
	return result;
}

class ScriptedRuntimeStartupServices final : public detail::RuntimeStartupServices {
  public:
	void observe_runtime(const detail::Runtime& runtime) noexcept
	{
		observed_runtime = &runtime;
	}

	void capture_main_thread() noexcept override
	{
		record_call(StartupCall::CaptureMainThread);
		++capture_main_thread_calls;
		main_thread_captured = true;
	}

	bool is_on_captured_main_thread() noexcept override
	{
		record_call(StartupCall::MainThreadCheck);
		++main_thread_checks;
		return main_thread_captured && main_thread_matches;
	}

	detail::RuntimeConfigResult load_config() noexcept override
	{
		record_call(StartupCall::LoadConfig);
		++config_loads;
		return config_result;
	}

	detail::IdentityResult load_producer_identity() noexcept override
	{
		record_call(StartupCall::LoadProducerIdentity);
		++producer_identity_loads;
		return identity_result;
	}

	detail::SessionIdCandidateResult draw_session_candidate() noexcept override
	{
		record_call(StartupCall::DrawSessionCandidate);
		++session_candidate_draws;
		return candidate_result;
	}

	detail::Wp03KnownBudgetSubtotal calculate_known_budget(std::size_t max_clients) noexcept override
	{
		record_call(StartupCall::CalculateKnownBudget);
		++budget_calculations;
		budget_max_clients = max_clients;
		return budget_result;
	}

	bool allocate_session_registry() noexcept override
	{
		record_call(StartupCall::AllocateRegistry);
		++registry_allocation_calls;
		registry_ready = allocation_makes_storage_ready;
		return allocation_succeeds;
	}

	detail::SessionIdRegistrationStatus register_session_candidate(std::uint64_t candidate) noexcept override
	{
		record_call(StartupCall::RegisterCandidate);
		++registration_calls;
		registered_candidate = candidate;
		return registration_status;
	}

	detail::RuntimeTransportStatus start_transport() noexcept override
	{
		record_call(StartupCall::StartTransport);
		++transport_calls;
		return transport_status;
	}

	void release_session_registry() noexcept override
	{
		record_call(StartupCall::ReleaseRegistry);
		++registry_release_calls;
		registry_ready = false;
	}

	void emit_startup_diagnostic(detail::RuntimeTerminalReason reason) noexcept override
	{
		record_call(StartupCall::EmitDiagnostic);
		diagnostic_reasons.push_back(reason);
	}

  private:
	void record_call(StartupCall call) noexcept
	{
		calls.push_back(call);
		if (observed_runtime != nullptr) {
			observations.push_back({call,
				observed_runtime->state(),
				observed_runtime->published_session_id(),
				observed_runtime->socket_count()});
		}
	}

  public:
	bool main_thread_matches = true;
	detail::RuntimeConfigResult config_result{detail::RuntimeConfigStatus::Enabled, 1U};
	detail::IdentityResult identity_result{7U, detail::IdentityError::None};
	detail::SessionIdCandidateResult candidate_result{detail::SessionIdCandidateStatus::Ready, 42U};
	detail::Wp03KnownBudgetSubtotal budget_result = successful_complete_budget();
	bool allocation_succeeds = true;
	bool allocation_makes_storage_ready = true;
	detail::SessionIdRegistrationStatus registration_status = detail::SessionIdRegistrationStatus::Registered;
	detail::RuntimeTransportStatus transport_status = detail::RuntimeTransportStatus::Unavailable;

	std::vector<StartupCall> calls;
	std::vector<RuntimeObservation> observations;
	std::vector<detail::RuntimeTerminalReason> diagnostic_reasons;
	bool main_thread_captured = false;
	std::size_t capture_main_thread_calls = 0U;
	std::size_t main_thread_checks = 0U;
	std::size_t config_loads = 0U;
	std::size_t producer_identity_loads = 0U;
	std::size_t session_candidate_draws = 0U;
	std::size_t budget_calculations = 0U;
	std::size_t budget_max_clients = 0U;
	std::size_t registry_allocation_calls = 0U;
	std::size_t registration_calls = 0U;
	std::size_t transport_calls = 0U;
	std::size_t registry_release_calls = 0U;
	std::uint64_t registered_candidate = 0U;
	bool registry_ready = false;

  private:
	const detail::Runtime* observed_runtime = nullptr;
};

std::vector<StartupCall> without_diagnostics(const std::vector<StartupCall>& calls)
{
	std::vector<StartupCall> result;
	result.reserve(calls.size());
	for (const auto call : calls) {
		if (call != StartupCall::EmitDiagnostic) {
			result.push_back(call);
		}
	}
	return result;
}

void expect_terminal_no_retry(detail::Runtime& runtime, ScriptedRuntimeStartupServices& services)
{
	const auto state = runtime.state();
	const auto reason = runtime.terminal_reason();
	const auto socket_count = runtime.socket_count();
	const auto published_session_id = runtime.published_session_id();
	const auto diagnostic_count = runtime.diagnostic_count();
	const auto calls = services.calls;
	const auto diagnostic_reasons = services.diagnostic_reasons;

	runtime.on_engine_update();
	runtime.on_engine_update();

	EXPECT_EQ(state, runtime.state());
	EXPECT_EQ(reason, runtime.terminal_reason());
	EXPECT_EQ(socket_count, runtime.socket_count());
	EXPECT_EQ(published_session_id, runtime.published_session_id());
	EXPECT_EQ(diagnostic_count, runtime.diagnostic_count());
	EXPECT_EQ(calls, services.calls);
	EXPECT_EQ(diagnostic_reasons, services.diagnostic_reasons);
}

void expect_unpublished_and_socket_free(const detail::Runtime& runtime)
{
	EXPECT_EQ(0U, runtime.published_session_id());
	EXPECT_EQ(0U, runtime.socket_count());
}

void expect_observed_state(const ScriptedRuntimeStartupServices& services,
	StartupCall call,
	detail::RuntimeState expected_state)
{
	const auto observation = std::find_if(services.observations.begin(),
		services.observations.end(),
		[call](const RuntimeObservation& item) { return item.call == call; });
	ASSERT_NE(services.observations.end(), observation);
	EXPECT_EQ(expected_state, observation->state);
}

void expect_standard_startup_entry_states(const ScriptedRuntimeStartupServices& services)
{
	expect_observed_state(services, StartupCall::MainThreadCheck, detail::RuntimeState::Cold);

	for (const auto& observation : services.observations) {
		if (observation.call >= StartupCall::LoadConfig && observation.call <= StartupCall::StartTransport) {
			SCOPED_TRACE(static_cast<unsigned int>(observation.call));
			EXPECT_EQ(detail::RuntimeState::Starting, observation.state);
		}
	}

	expect_observed_state(services, StartupCall::LoadConfig, detail::RuntimeState::Starting);
}

void expect_no_publication_at_any_service_boundary(const ScriptedRuntimeStartupServices& services)
{
	ASSERT_EQ(services.calls.size(), services.observations.size());
	for (const auto& observation : services.observations) {
		SCOPED_TRACE(static_cast<unsigned int>(observation.call));
		EXPECT_EQ(0U, observation.published_session_id);
		EXPECT_EQ(0U, observation.socket_count);
	}
}

void expect_single_fault_diagnostic(const detail::Runtime& runtime,
	const ScriptedRuntimeStartupServices& services,
	detail::RuntimeTerminalReason reason)
{
	EXPECT_EQ(detail::RuntimeState::Faulted, runtime.state());
	EXPECT_EQ(reason, runtime.terminal_reason());
	EXPECT_EQ(1U, runtime.diagnostic_count());
	EXPECT_EQ(std::vector<detail::RuntimeTerminalReason>{reason}, services.diagnostic_reasons);
	expect_unpublished_and_socket_free(runtime);
	expect_no_publication_at_any_service_boundary(services);
}

static_assert(std::is_polymorphic_v<detail::RuntimeStartupServices>,
	"The runtime startup seam must support a production adapter and a test fake.");
static_assert(noexcept(std::declval<detail::Runtime&>().capture_main_thread()));
static_assert(noexcept(std::declval<detail::Runtime&>().on_engine_update()));
static_assert(noexcept(std::declval<const detail::Runtime&>().state()));
static_assert(noexcept(std::declval<const detail::Runtime&>().terminal_reason()));
static_assert(noexcept(std::declval<const detail::Runtime&>().socket_count()));
static_assert(noexcept(std::declval<const detail::Runtime&>().published_session_id()));
static_assert(noexcept(std::declval<const detail::Runtime&>().diagnostic_count()));

TEST(TelemetryRuntimeStartupContract, MainThreadCaptureIsIdempotentAndStartupWaitsForFirstEngineUpdate)
{
	ScriptedRuntimeStartupServices services;
	detail::Runtime runtime(services);
	services.observe_runtime(runtime);

	EXPECT_EQ(detail::RuntimeState::Cold, runtime.state());
	EXPECT_EQ(detail::RuntimeTerminalReason::None, runtime.terminal_reason());
	expect_unpublished_and_socket_free(runtime);

	runtime.capture_main_thread();
	runtime.capture_main_thread();

	EXPECT_EQ(1U, services.capture_main_thread_calls);
	EXPECT_EQ(std::vector<StartupCall>{StartupCall::CaptureMainThread}, services.calls);
	EXPECT_EQ(detail::RuntimeState::Cold, runtime.state());
	EXPECT_EQ(0U, services.config_loads);
	EXPECT_EQ(0U, services.session_candidate_draws);
	EXPECT_EQ(0U, services.transport_calls);
	EXPECT_EQ(0U, runtime.diagnostic_count());
	expect_unpublished_and_socket_free(runtime);

	services.config_result.status = detail::RuntimeConfigStatus::Absent;
	runtime.on_engine_update();

	EXPECT_EQ(detail::RuntimeState::Disabled, runtime.state());
	EXPECT_EQ(detail::RuntimeTerminalReason::ConfigAbsent, runtime.terminal_reason());
	EXPECT_EQ(1U, services.main_thread_checks);
	EXPECT_EQ(1U, services.config_loads);
	EXPECT_EQ((std::vector<StartupCall>{StartupCall::CaptureMainThread,
			  StartupCall::MainThreadCheck,
			  StartupCall::LoadConfig}),
		without_diagnostics(services.calls));
	expect_standard_startup_entry_states(services);
	expect_no_publication_at_any_service_boundary(services);
	EXPECT_LE(runtime.diagnostic_count(), 1U);
	expect_terminal_no_retry(runtime, services);
}

TEST(TelemetryRuntimeStartupContract, EngineUpdateWithoutMainThreadCaptureFaultsBeforeThreadCheck)
{
	ScriptedRuntimeStartupServices services;
	detail::Runtime runtime(services);
	services.observe_runtime(runtime);

	runtime.on_engine_update();

	EXPECT_FALSE(services.main_thread_captured);
	EXPECT_EQ(std::vector<StartupCall>{StartupCall::EmitDiagnostic}, services.calls);
	EXPECT_EQ(0U, services.main_thread_checks);
	EXPECT_EQ(0U, services.config_loads);
	EXPECT_EQ(0U, services.producer_identity_loads);
	EXPECT_EQ(0U, services.transport_calls);
	expect_single_fault_diagnostic(runtime, services, detail::RuntimeTerminalReason::MainThreadNotCaptured);
	expect_terminal_no_retry(runtime, services);
}

TEST(TelemetryRuntimeStartupContract, WrongThreadFaultsBeforeConfigurationAndNeverRetries)
{
	ScriptedRuntimeStartupServices services;
	services.main_thread_matches = false;
	detail::Runtime runtime(services);
	services.observe_runtime(runtime);
	runtime.capture_main_thread();

	runtime.on_engine_update();

	EXPECT_EQ((std::vector<StartupCall>{StartupCall::CaptureMainThread,
			  StartupCall::MainThreadCheck,
			  StartupCall::EmitDiagnostic}),
		services.calls);
	EXPECT_EQ(0U, services.config_loads);
	EXPECT_EQ(0U, services.producer_identity_loads);
	EXPECT_EQ(0U, services.transport_calls);
	expect_observed_state(services, StartupCall::MainThreadCheck, detail::RuntimeState::Cold);
	expect_single_fault_diagnostic(runtime, services, detail::RuntimeTerminalReason::MainThreadViolation);
	expect_terminal_no_retry(runtime, services);
}

TEST(TelemetryRuntimeStartupContract, NonEnabledConfigurationStopsBeforeIdentityAndSocketWork)
{
	struct ConfigCase {
		detail::RuntimeConfigStatus status;
		detail::RuntimeTerminalReason reason;
		std::size_t minimum_diagnostics;
	};

	const std::array<ConfigCase, 3> cases{{
		{detail::RuntimeConfigStatus::Absent, detail::RuntimeTerminalReason::ConfigAbsent, 0U},
		{detail::RuntimeConfigStatus::Invalid, detail::RuntimeTerminalReason::ConfigInvalid, 1U},
		{detail::RuntimeConfigStatus::Disabled, detail::RuntimeTerminalReason::ConfigDisabled, 0U},
	}};

	for (const auto& item : cases) {
		SCOPED_TRACE(static_cast<unsigned int>(item.status));
		ScriptedRuntimeStartupServices services;
		services.config_result.status = item.status;
		detail::Runtime runtime(services);
		services.observe_runtime(runtime);
		runtime.capture_main_thread();

		runtime.on_engine_update();

		EXPECT_EQ(detail::RuntimeState::Disabled, runtime.state());
		EXPECT_EQ(item.reason, runtime.terminal_reason());
		EXPECT_EQ((std::vector<StartupCall>{StartupCall::CaptureMainThread,
				  StartupCall::MainThreadCheck,
				  StartupCall::LoadConfig}),
			without_diagnostics(services.calls));
		EXPECT_EQ(0U, services.producer_identity_loads);
		EXPECT_EQ(0U, services.session_candidate_draws);
		EXPECT_EQ(0U, services.budget_calculations);
		EXPECT_EQ(0U, services.registry_allocation_calls);
		EXPECT_EQ(0U, services.transport_calls);
		expect_standard_startup_entry_states(services);
		EXPECT_GE(runtime.diagnostic_count(), item.minimum_diagnostics);
		EXPECT_LE(runtime.diagnostic_count(), 1U);
		EXPECT_EQ(runtime.diagnostic_count(), services.diagnostic_reasons.size());
		if (!services.diagnostic_reasons.empty()) {
			EXPECT_EQ(std::vector<detail::RuntimeTerminalReason>{item.reason}, services.diagnostic_reasons);
		}
		expect_unpublished_and_socket_free(runtime);
		expect_no_publication_at_any_service_boundary(services);
		expect_terminal_no_retry(runtime, services);
	}
}

TEST(TelemetryRuntimeStartupContract, ProducerIdentityFailureStopsBeforeSessionCandidate)
{
	const std::array<detail::IdentityResult, 2> failures{{
		{0U, detail::IdentityError::None},
		{7U, detail::IdentityError::ProfileReadFailure},
	}};

	for (const auto& failure : failures) {
		SCOPED_TRACE(static_cast<unsigned int>(failure.error));
		ScriptedRuntimeStartupServices services;
		services.identity_result = failure;
		detail::Runtime runtime(services);
		services.observe_runtime(runtime);
		runtime.capture_main_thread();

		runtime.on_engine_update();

		EXPECT_EQ((std::vector<StartupCall>{StartupCall::CaptureMainThread,
				  StartupCall::MainThreadCheck,
				  StartupCall::LoadConfig,
				  StartupCall::LoadProducerIdentity,
				  StartupCall::EmitDiagnostic}),
			services.calls);
		EXPECT_EQ(0U, services.session_candidate_draws);
		EXPECT_EQ(0U, services.budget_calculations);
		EXPECT_EQ(0U, services.registry_allocation_calls);
		EXPECT_EQ(0U, services.registry_release_calls);
		EXPECT_EQ(0U, services.transport_calls);
		expect_standard_startup_entry_states(services);
		expect_single_fault_diagnostic(runtime, services, detail::RuntimeTerminalReason::ProducerIdentityFailure);
		expect_terminal_no_retry(runtime, services);
	}
}

TEST(TelemetryRuntimeStartupContract, SessionCandidateFailureConsumesOneDrawAndStopsBeforeBudget)
{
	const std::array<detail::SessionIdCandidateResult, 2> failures{{
		{detail::SessionIdCandidateStatus::EntropyFailure, 0U},
		{detail::SessionIdCandidateStatus::Ready, 0U},
	}};

	for (const auto& failure : failures) {
		SCOPED_TRACE(static_cast<unsigned int>(failure.status));
		ScriptedRuntimeStartupServices services;
		services.candidate_result = failure;
		detail::Runtime runtime(services);
		services.observe_runtime(runtime);
		runtime.capture_main_thread();

		runtime.on_engine_update();

		EXPECT_EQ((std::vector<StartupCall>{StartupCall::CaptureMainThread,
				  StartupCall::MainThreadCheck,
				  StartupCall::LoadConfig,
				  StartupCall::LoadProducerIdentity,
				  StartupCall::DrawSessionCandidate,
				  StartupCall::EmitDiagnostic}),
			services.calls);
		EXPECT_EQ(1U, services.session_candidate_draws);
		EXPECT_EQ(0U, services.budget_calculations);
		EXPECT_EQ(0U, services.registry_allocation_calls);
		EXPECT_EQ(0U, services.registry_release_calls);
		EXPECT_EQ(0U, services.transport_calls);
		expect_standard_startup_entry_states(services);
		expect_single_fault_diagnostic(runtime, services, detail::RuntimeTerminalReason::SessionCandidateFailure);
		expect_terminal_no_retry(runtime, services);
	}
}

TEST(TelemetryRuntimeStartupContract, EveryBudgetErrorStopsBeforeAllocationAndTransport)
{
	const std::array<detail::StartupBudgetError, 3> failures{{
		detail::StartupBudgetError::InvalidClientCount,
		detail::StartupBudgetError::ArithmeticOverflow,
		detail::StartupBudgetError::StaticCapExceeded,
	}};

	for (const auto failure : failures) {
		SCOPED_TRACE(static_cast<unsigned int>(failure));
		ScriptedRuntimeStartupServices services;
		services.config_result.max_clients = 4U;
		services.budget_result.error = failure;
		detail::Runtime runtime(services);
		services.observe_runtime(runtime);
		runtime.capture_main_thread();

		runtime.on_engine_update();

		EXPECT_EQ((std::vector<StartupCall>{StartupCall::CaptureMainThread,
				  StartupCall::MainThreadCheck,
				  StartupCall::LoadConfig,
				  StartupCall::LoadProducerIdentity,
				  StartupCall::DrawSessionCandidate,
				  StartupCall::CalculateKnownBudget,
				  StartupCall::EmitDiagnostic}),
			services.calls);
		EXPECT_EQ(1U, services.session_candidate_draws);
		EXPECT_EQ(1U, services.budget_calculations);
		EXPECT_EQ(4U, services.budget_max_clients);
		EXPECT_EQ(0U, services.registry_allocation_calls);
		EXPECT_EQ(0U, services.registry_release_calls);
		EXPECT_EQ(0U, services.transport_calls);
		expect_standard_startup_entry_states(services);
		expect_single_fault_diagnostic(runtime, services, detail::RuntimeTerminalReason::BudgetFailure);
		expect_terminal_no_retry(runtime, services);
	}
}

TEST(TelemetryRuntimeStartupContract, AllocationFailureRollsBackPartialRegistryBeforeFaulting)
{
	ScriptedRuntimeStartupServices services;
	services.allocation_succeeds = false;
	services.allocation_makes_storage_ready = true;
	detail::Runtime runtime(services);
	services.observe_runtime(runtime);
	runtime.capture_main_thread();

	runtime.on_engine_update();

	EXPECT_EQ((std::vector<StartupCall>{StartupCall::CaptureMainThread,
			  StartupCall::MainThreadCheck,
			  StartupCall::LoadConfig,
			  StartupCall::LoadProducerIdentity,
			  StartupCall::DrawSessionCandidate,
			  StartupCall::CalculateKnownBudget,
			  StartupCall::AllocateRegistry,
			  StartupCall::ReleaseRegistry,
			  StartupCall::EmitDiagnostic}),
		services.calls);
	EXPECT_EQ(1U, services.session_candidate_draws);
	EXPECT_EQ(1U, services.registry_allocation_calls);
	EXPECT_EQ(1U, services.registry_release_calls);
	EXPECT_FALSE(services.registry_ready);
	EXPECT_EQ(0U, services.registration_calls);
	EXPECT_EQ(0U, services.transport_calls);
	expect_standard_startup_entry_states(services);
	expect_single_fault_diagnostic(runtime, services, detail::RuntimeTerminalReason::AllocationFailure);
	expect_terminal_no_retry(runtime, services);
}

TEST(TelemetryRuntimeStartupContract, EveryRegistrationFailureRollsBackAndNeverStartsTransport)
{
	const std::array<detail::SessionIdRegistrationStatus, 4> failures{{
		detail::SessionIdRegistrationStatus::StorageUnavailable,
		detail::SessionIdRegistrationStatus::InvalidCandidate,
		detail::SessionIdRegistrationStatus::Duplicate,
		detail::SessionIdRegistrationStatus::Capacity,
	}};

	for (const auto failure : failures) {
		SCOPED_TRACE(static_cast<unsigned int>(failure));
		ScriptedRuntimeStartupServices services;
		services.registration_status = failure;
		detail::Runtime runtime(services);
		services.observe_runtime(runtime);
		runtime.capture_main_thread();

		runtime.on_engine_update();

		EXPECT_EQ((std::vector<StartupCall>{StartupCall::CaptureMainThread,
				  StartupCall::MainThreadCheck,
				  StartupCall::LoadConfig,
				  StartupCall::LoadProducerIdentity,
				  StartupCall::DrawSessionCandidate,
				  StartupCall::CalculateKnownBudget,
				  StartupCall::AllocateRegistry,
				  StartupCall::RegisterCandidate,
				  StartupCall::ReleaseRegistry,
				  StartupCall::EmitDiagnostic}),
			services.calls);
		EXPECT_EQ(1U, services.session_candidate_draws);
		EXPECT_EQ(1U, services.registration_calls);
		EXPECT_EQ(42U, services.registered_candidate);
		EXPECT_EQ(1U, services.registry_release_calls);
		EXPECT_FALSE(services.registry_ready);
		EXPECT_EQ(0U, services.transport_calls);
		expect_standard_startup_entry_states(services);
		expect_single_fault_diagnostic(runtime, services, detail::RuntimeTerminalReason::SessionRegistrationFailure);
		expect_terminal_no_retry(runtime, services);
	}
}

TEST(TelemetryRuntimeStartupContract, IncompleteOrDeferredBudgetFailsBeforeAllocationRegistrationAndTransport)
{
	struct IncompleteCase {
		bool is_complete;
		std::uint16_t deferred_categories;
	};

	const auto transport_deferred = static_cast<std::uint16_t>(
		1U << static_cast<unsigned int>(detail::DeferredStartupBudgetCategory::TransportBuffers));
	const std::array<IncompleteCase, 2> cases{{
		{false, 0U},
		{true, transport_deferred},
	}};

	for (const auto& item : cases) {
		SCOPED_TRACE(item.is_complete ? "deferred category remains" : "budget is explicitly incomplete");
		ScriptedRuntimeStartupServices services;
		services.config_result.max_clients = 4U;
		services.budget_result = successful_complete_budget();
		services.budget_result.is_complete = item.is_complete;
		services.budget_result.deferred_categories = item.deferred_categories;
		ASSERT_EQ(detail::StartupBudgetError::None, services.budget_result.error);
		detail::Runtime runtime(services);
		services.observe_runtime(runtime);
		runtime.capture_main_thread();

		runtime.on_engine_update();

		EXPECT_EQ((std::vector<StartupCall>{StartupCall::CaptureMainThread,
				  StartupCall::MainThreadCheck,
				  StartupCall::LoadConfig,
				  StartupCall::LoadProducerIdentity,
				  StartupCall::DrawSessionCandidate,
				  StartupCall::CalculateKnownBudget,
				  StartupCall::EmitDiagnostic}),
			services.calls);
		EXPECT_EQ(1U, services.session_candidate_draws);
		EXPECT_EQ(1U, services.budget_calculations);
		EXPECT_EQ(4U, services.budget_max_clients);
		EXPECT_EQ(0U, services.registry_allocation_calls);
		EXPECT_EQ(0U, services.registration_calls);
		EXPECT_EQ(0U, services.transport_calls)
			<< "No socket bind may be attempted until the global startup budget is complete.";
		EXPECT_EQ(0U, services.registry_release_calls);
		EXPECT_FALSE(services.registry_ready);
		expect_standard_startup_entry_states(services);
		expect_single_fault_diagnostic(runtime, services, detail::RuntimeTerminalReason::BudgetFailure);
		expect_terminal_no_retry(runtime, services);
	}
}

} // namespace
