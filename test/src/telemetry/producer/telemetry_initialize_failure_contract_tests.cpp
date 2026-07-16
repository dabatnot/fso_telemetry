#include "events/events.h"
#include "telemetry/runtime.h"
#include "telemetry/telemetry.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

#if defined(main)
#undef main
#endif

namespace telemetry::test_seam {

void reset_observation_counts() noexcept;
std::uint64_t registration_attempts(std::size_t event_index) noexcept;
std::uint64_t callback_invocations(std::size_t event_index) noexcept;
void reset_runtime_adapter_observations() noexcept;
std::uint64_t runtime_adapter_factory_requests() noexcept;
std::uint64_t runtime_adapter_main_thread_captures() noexcept;
std::uint64_t runtime_adapter_main_thread_checks() noexcept;
std::uint64_t runtime_adapter_config_loads() noexcept;
std::uint64_t runtime_adapter_post_config_calls() noexcept;
std::uint64_t runtime_adapter_diagnostic_calls() noexcept;
detail::RuntimeState runtime_state() noexcept;
detail::RuntimeTerminalReason runtime_terminal_reason() noexcept;

} // namespace telemetry::test_seam

namespace {

constexpr std::size_t EventCount = 5;
constexpr int TerminateExitCode = 86;
constexpr int ContractFailureExitCode = 1;

std::atomic<bool> injection_armed{false};
std::atomic<bool> injection_fired{false};
std::atomic<std::uint64_t> allocation_attempts{0};
std::atomic<std::uint64_t> fail_after{0};

void observe_allocation()
{
	if (!injection_armed.load(std::memory_order_relaxed)) {
		return;
	}

	const auto attempt = allocation_attempts.fetch_add(1, std::memory_order_relaxed);
	if (!injection_fired.load(std::memory_order_relaxed) &&
		attempt == fail_after.load(std::memory_order_relaxed)) {
		injection_fired.store(true, std::memory_order_relaxed);
		throw std::bad_alloc{};
	}
}

void* allocate_unaligned(std::size_t size)
{
	observe_allocation();
	if (auto* allocation = std::malloc(size == 0 ? 1 : size)) {
		return allocation;
	}
	throw std::bad_alloc{};
}

void* allocate_aligned(std::size_t size, std::size_t alignment)
{
	observe_allocation();
	void* allocation = nullptr;
#if defined(_MSC_VER)
	allocation = _aligned_malloc(size == 0 ? 1 : size, alignment);
#else
	if (posix_memalign(&allocation, alignment, size == 0 ? 1 : size) != 0) {
		allocation = nullptr;
	}
#endif
	if (allocation == nullptr) {
		throw std::bad_alloc{};
	}
	return allocation;
}

void free_aligned(void* allocation) noexcept
{
#if defined(_MSC_VER)
	_aligned_free(allocation);
#else
	std::free(allocation);
#endif
}

void arm_failure(std::uint64_t failure_index) noexcept
{
	fail_after.store(failure_index, std::memory_order_relaxed);
	allocation_attempts.store(0, std::memory_order_relaxed);
	injection_fired.store(false, std::memory_order_relaxed);
	injection_armed.store(true, std::memory_order_relaxed);
}

void disarm_failure() noexcept
{
	injection_armed.store(false, std::memory_order_relaxed);
}

using Counts = std::array<std::uint64_t, EventCount>;

Counts registration_counts() noexcept
{
	return {telemetry::test_seam::registration_attempts(0),
		telemetry::test_seam::registration_attempts(1),
		telemetry::test_seam::registration_attempts(2),
		telemetry::test_seam::registration_attempts(3),
		telemetry::test_seam::registration_attempts(4)};
}

Counts invocation_counts() noexcept
{
	return {telemetry::test_seam::callback_invocations(0),
		telemetry::test_seam::callback_invocations(1),
		telemetry::test_seam::callback_invocations(2),
		telemetry::test_seam::callback_invocations(3),
		telemetry::test_seam::callback_invocations(4)};
}

bool equals(const Counts& left, const Counts& right) noexcept
{
	for (std::size_t i = 0; i < EventCount; ++i) {
		if (left[i] != right[i]) {
			return false;
		}
	}
	return true;
}

Counts expected_registration_attempts(std::size_t failure_index) noexcept
{
	Counts expected{};
	for (std::size_t i = 0; i <= failure_index; ++i) {
		expected[i] = 1;
	}
	return expected;
}

Counts expected_callback_invocations(std::size_t failure_index) noexcept
{
	Counts expected{};
	for (std::size_t i = 0; i < failure_index; ++i) {
		expected[i] = 1;
	}
	return expected;
}

void print_counts(const char* label, const Counts& counts) noexcept
{
	std::printf("%s={%llu,%llu,%llu,%llu,%llu}\n",
		label,
		static_cast<unsigned long long>(counts[0]),
		static_cast<unsigned long long>(counts[1]),
		static_cast<unsigned long long>(counts[2]),
		static_cast<unsigned long long>(counts[3]),
		static_cast<unsigned long long>(counts[4]));
}

bool runtime_remained_cold_without_startup() noexcept
{
	return telemetry::test_seam::runtime_state() == telemetry::detail::RuntimeState::Cold &&
		telemetry::test_seam::runtime_terminal_reason() == telemetry::detail::RuntimeTerminalReason::None &&
		telemetry::test_seam::runtime_adapter_main_thread_checks() == 0U &&
		telemetry::test_seam::runtime_adapter_config_loads() == 0U &&
		telemetry::test_seam::runtime_adapter_post_config_calls() == 0U &&
		telemetry::test_seam::runtime_adapter_diagnostic_calls() == 0U;
}

void print_runtime_observations() noexcept
{
	std::printf("runtime_state=%u\n", static_cast<unsigned int>(telemetry::test_seam::runtime_state()));
	std::printf("runtime_terminal_reason=%u\n",
		static_cast<unsigned int>(telemetry::test_seam::runtime_terminal_reason()));
	std::printf("runtime_adapter_factory_requests=%llu\n",
		static_cast<unsigned long long>(telemetry::test_seam::runtime_adapter_factory_requests()));
	std::printf("runtime_adapter_main_thread_captures=%llu\n",
		static_cast<unsigned long long>(telemetry::test_seam::runtime_adapter_main_thread_captures()));
	std::printf("runtime_adapter_main_thread_checks=%llu\n",
		static_cast<unsigned long long>(telemetry::test_seam::runtime_adapter_main_thread_checks()));
	std::printf("runtime_adapter_config_loads=%llu\n",
		static_cast<unsigned long long>(telemetry::test_seam::runtime_adapter_config_loads()));
	std::printf("runtime_adapter_post_config_calls=%llu\n",
		static_cast<unsigned long long>(telemetry::test_seam::runtime_adapter_post_config_calls()));
	std::printf("runtime_adapter_diagnostic_calls=%llu\n",
		static_cast<unsigned long long>(telemetry::test_seam::runtime_adapter_diagnostic_calls()));
}

bool parse_failure_index(const char* value, std::size_t& failure_index) noexcept
{
	if (std::strlen(value) != 1 || value[0] < '0' || value[0] > '4') {
		return false;
	}
	failure_index = static_cast<std::size_t>(value[0] - '0');
	return true;
}

} // namespace

void* operator new(std::size_t size)
{
	return allocate_unaligned(size);
}

void* operator new[](std::size_t size)
{
	return allocate_unaligned(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
	try {
		return allocate_unaligned(size);
	} catch (...) {
		return nullptr;
	}
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
	try {
		return allocate_unaligned(size);
	} catch (...) {
		return nullptr;
	}
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
	return allocate_aligned(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
	return allocate_aligned(size, static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
	try {
		return allocate_aligned(size, static_cast<std::size_t>(alignment));
	} catch (...) {
		return nullptr;
	}
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
	try {
		return allocate_aligned(size, static_cast<std::size_t>(alignment));
	} catch (...) {
		return nullptr;
	}
}

void operator delete(void* allocation) noexcept
{
	std::free(allocation);
}

void operator delete[](void* allocation) noexcept
{
	std::free(allocation);
}

void operator delete(void* allocation, std::size_t) noexcept
{
	std::free(allocation);
}

void operator delete[](void* allocation, std::size_t) noexcept
{
	std::free(allocation);
}

void operator delete(void* allocation, const std::nothrow_t&) noexcept
{
	std::free(allocation);
}

void operator delete[](void* allocation, const std::nothrow_t&) noexcept
{
	std::free(allocation);
}

void operator delete(void* allocation, std::align_val_t) noexcept
{
	free_aligned(allocation);
}

void operator delete[](void* allocation, std::align_val_t) noexcept
{
	free_aligned(allocation);
}

void operator delete(void* allocation, std::size_t, std::align_val_t) noexcept
{
	free_aligned(allocation);
}

void operator delete[](void* allocation, std::size_t, std::align_val_t) noexcept
{
	free_aligned(allocation);
}

void operator delete(void* allocation, std::align_val_t, const std::nothrow_t&) noexcept
{
	free_aligned(allocation);
}

void operator delete[](void* allocation, std::align_val_t, const std::nothrow_t&) noexcept
{
	free_aligned(allocation);
}

int main(int argc, char** argv)
{
	if (argc != 2) {
		std::fprintf(stderr, "usage: telemetry_initialize_failure_contract_tests <probe|fail-after:0..4>\n");
		return 2;
	}

	const bool calibration_probe = std::strcmp(argv[1], "probe") == 0;
	std::size_t failure_index = 0;
	if (!calibration_probe && !parse_failure_index(argv[1], failure_index)) {
		std::fprintf(stderr, "invalid failure index: %s\n", argv[1]);
		return 2;
	}

	std::set_terminate([]() noexcept {
		disarm_failure();
		std::printf("terminate_observed=1\n");
		std::fflush(stdout);
		std::_Exit(TerminateExitCode);
	});

	telemetry::test_seam::reset_observation_counts();
	telemetry::test_seam::reset_runtime_adapter_observations();
	if (calibration_probe) {
		arm_failure(~std::uint64_t{0});
		telemetry::initialize();
		const auto attempts = allocation_attempts.load(std::memory_order_relaxed);
		disarm_failure();
		const auto registrations = registration_counts();
		const Counts one_registration_each{1, 1, 1, 1, 1};
		const bool passed = !injection_fired.load(std::memory_order_relaxed) && attempts == EventCount &&
			equals(registrations, one_registration_each) && runtime_remained_cold_without_startup();
		std::printf("calibration_probe=1\n");
		std::printf("allocation_attempts=%llu\n", static_cast<unsigned long long>(attempts));
		print_counts("registration_attempts", registrations);
		print_runtime_observations();
		std::printf("calibration_check=%s\n", passed ? "PASS" : "FAIL");
		return passed ? 0 : ContractFailureExitCode;
	}

	arm_failure(failure_index);
	telemetry::initialize();

	const auto attempts_after_first = allocation_attempts.load(std::memory_order_relaxed);
	const auto registrations_after_first = registration_counts();
	telemetry::initialize();
	const auto attempts_after_second = allocation_attempts.load(std::memory_order_relaxed);
	const auto registrations_after_second = registration_counts();
	disarm_failure();

	events::EngineUpdate();
	events::EngineShutdown();
	events::GameMissionLoad("allocation-failure.fs2");
	events::GameEnterState(1, 2);
	events::GameLeaveState(2, 1);
	const auto invocations_after_emission = invocation_counts();

	const auto expected_registrations = expected_registration_attempts(failure_index);
	const auto expected_invocations = expected_callback_invocations(failure_index);
	const bool passed = injection_fired.load(std::memory_order_relaxed) &&
		attempts_after_first == failure_index + 1 && attempts_after_second == attempts_after_first &&
		equals(registrations_after_first, expected_registrations) &&
		equals(registrations_after_second, registrations_after_first) &&
		equals(invocations_after_emission, expected_invocations) && runtime_remained_cold_without_startup();

	std::printf("fail_after=%llu\n", static_cast<unsigned long long>(failure_index));
	std::printf("injection_fired=%u\n", injection_fired.load(std::memory_order_relaxed) ? 1U : 0U);
	std::printf("allocation_attempts_after_first=%llu\n", static_cast<unsigned long long>(attempts_after_first));
	std::printf("allocation_attempts_after_second=%llu\n", static_cast<unsigned long long>(attempts_after_second));
	print_counts("registrations_after_first", registrations_after_first);
	print_counts("registrations_after_second", registrations_after_second);
	print_counts("invocations_after_emission", invocations_after_emission);
	print_runtime_observations();
	std::printf("contract_check=%s\n", passed ? "PASS" : "FAIL");
	return passed ? 0 : ContractFailureExitCode;
}
