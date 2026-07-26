#include "telemetry/phase2_wp03_allocation_tracker.h"

#include <atomic>
#include <cstdlib>
#include <new>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace {

std::atomic<bool> tracking{false};
std::atomic<std::size_t> allocation_count{0};

void observe()
{
	if (!tracking.load(std::memory_order_relaxed)) {
		return;
	}
	allocation_count.fetch_add(1, std::memory_order_relaxed);
}

void* allocate(std::size_t size)
{
	observe();
	if (auto* value = std::malloc(size == 0 ? 1 : size)) {
		return value;
	}
	throw std::bad_alloc();
}

void* allocate_aligned(std::size_t size, std::size_t alignment)
{
	observe();
#if defined(_MSC_VER)
	if (auto* value = _aligned_malloc(size == 0 ? 1 : size, alignment)) {
		return value;
	}
#else
	void* value = nullptr;
	if (posix_memalign(&value, alignment, size == 0 ? 1 : size) == 0) {
		return value;
	}
#endif
	throw std::bad_alloc();
}

void free_aligned(void* value) noexcept
{
#if defined(_MSC_VER)
	_aligned_free(value);
#else
	std::free(value);
#endif
}

} // namespace

namespace telemetry::test::wp03 {

void begin_global_allocation_tracking() noexcept
{
	allocation_count.store(0, std::memory_order_relaxed);
	tracking.store(true, std::memory_order_release);
}

std::size_t end_global_allocation_tracking() noexcept
{
	tracking.store(false, std::memory_order_release);
	return allocation_count.load(std::memory_order_relaxed);
}

GlobalAllocationScope::GlobalAllocationScope() noexcept
{
	begin_global_allocation_tracking();
}

GlobalAllocationScope::~GlobalAllocationScope()
{
	if (m_active) end_global_allocation_tracking();
}

std::size_t GlobalAllocationScope::finish() noexcept
{
	if (!m_active) return 0;
	m_active = false;
	return end_global_allocation_tracking();
}

} // namespace telemetry::test::wp03

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
	try {
		return allocate(size);
	} catch (...) {
		return nullptr;
	}
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
	try {
		return allocate(size);
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

void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }
void operator delete(void* value, const std::nothrow_t&) noexcept { std::free(value); }
void operator delete[](void* value, const std::nothrow_t&) noexcept { std::free(value); }
void operator delete(void* value, std::align_val_t) noexcept { free_aligned(value); }
void operator delete[](void* value, std::align_val_t) noexcept { free_aligned(value); }
void operator delete(void* value, std::size_t, std::align_val_t) noexcept { free_aligned(value); }
void operator delete[](void* value, std::size_t, std::align_val_t) noexcept { free_aligned(value); }
