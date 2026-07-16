#include "events/events.h"
#include "telemetry/telemetry.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <new>
#include <vector>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

#if defined(main)
#undef main
#endif

namespace {

std::atomic<bool> track_allocations{false};
std::atomic<std::uint64_t> allocation_count{0};

void observe_allocation() noexcept
{
	if (track_allocations.load(std::memory_order_relaxed)) {
		allocation_count.fetch_add(1, std::memory_order_relaxed);
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

constexpr std::size_t WarmupCount = 10'000;
constexpr std::size_t SampleCount = 100'000;
constexpr double MeanLimitMs = 0.01;
constexpr double P99LimitMs = 0.05;

enum class Mode {
	Baseline,
	Disabled,
};

bool parse_mode(const char* value, Mode& mode) noexcept
{
	if (std::strcmp(value, "baseline") == 0) {
		mode = Mode::Baseline;
		return true;
	}
	if (std::strcmp(value, "disabled") == 0) {
		mode = Mode::Disabled;
		return true;
	}
	return false;
}

const char* mode_name(Mode mode) noexcept
{
	return mode == Mode::Baseline ? "baseline" : "disabled";
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
	if (argc != 3) {
		std::cerr << "usage: telemetry_disabled_benchmark <baseline|disabled> <raw-samples.csv>\n";
		return 2;
	}

	Mode mode{};
	if (!parse_mode(argv[1], mode)) {
		std::cerr << "invalid benchmark mode: " << argv[1] << '\n';
		return 2;
	}

	std::vector<std::uint64_t> samples(SampleCount);
	if (mode == Mode::Disabled) {
		telemetry::initialize();
	}

	for (std::size_t i = 0; i < WarmupCount; ++i) {
		events::EngineUpdate();
	}

	allocation_count.store(0, std::memory_order_relaxed);
	track_allocations.store(true, std::memory_order_relaxed);
	for (auto& sample : samples) {
		const auto before = std::chrono::steady_clock::now();
		events::EngineUpdate();
		const auto after = std::chrono::steady_clock::now();
		sample = static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count());
	}
	track_allocations.store(false, std::memory_order_relaxed);
	const auto measured_allocations = allocation_count.load(std::memory_order_relaxed);

	std::uint64_t total_ns = 0;
	for (const auto sample : samples) {
		total_ns += sample;
	}
	auto sorted_samples = samples;
	std::sort(sorted_samples.begin(), sorted_samples.end());
	const auto p99_rank = ((99 * sorted_samples.size()) + 99) / 100;
	const auto p99_ns = sorted_samples[p99_rank - 1];
	const auto median_ns = sorted_samples[sorted_samples.size() / 2];
	const auto mean_ms = static_cast<double>(total_ns) / static_cast<double>(samples.size()) / 1'000'000.0;
	const auto p99_ms = static_cast<double>(p99_ns) / 1'000'000.0;

	std::ofstream raw_samples(argv[2], std::ios::out | std::ios::trunc);
	if (!raw_samples) {
		std::cerr << "failed to open raw sample output: " << argv[2] << '\n';
		return 2;
	}
	raw_samples << "sample_index,duration_ns\n";
	for (std::size_t i = 0; i < samples.size(); ++i) {
		raw_samples << i << ',' << samples[i] << '\n';
	}
	if (!raw_samples) {
		std::cerr << "failed to write all raw samples: " << argv[2] << '\n';
		return 2;
	}

	const bool allocation_check_passed = measured_allocations == 0;
	const bool timing_check_passed = mode == Mode::Baseline || (mean_ms <= MeanLimitMs && p99_ms <= P99LimitMs);
	std::cout << std::fixed << std::setprecision(9)
			  << "mode=" << mode_name(mode) << '\n'
			  << "warmup_callbacks=" << WarmupCount << '\n'
			  << "measured_callbacks=" << SampleCount << '\n'
			  << "mean_ms=" << mean_ms << '\n'
			  << "median_ns=" << median_ns << '\n'
			  << "p99_ms=" << p99_ms << '\n'
			  << "tracked_cpp_allocations=" << measured_allocations << '\n'
			  << "allocation_check=" << (allocation_check_passed ? "PASS" : "FAIL") << '\n'
			  << "timing_check=" << (timing_check_passed ? "PASS" : "FAIL") << '\n'
			  << "raw_samples=" << argv[2] << '\n';

	return allocation_check_passed && timing_check_passed ? 0 : 1;
}
