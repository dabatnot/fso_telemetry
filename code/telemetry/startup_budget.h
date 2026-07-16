#pragma once

#include "telemetry/identity.h"
#include "telemetry/protocol/telemetry_protocol_constants.h"

#include <cstddef>
#include <cstdint>

namespace telemetry::detail {

// Phase 1 requires a compiled ceiling but does not assign it a normative
// value. WP03 uses this explicit provisional implementation ceiling until the
// complete runtime storage layout is available.
constexpr std::size_t WP03ProvisionalKnownBudgetCapBytes = 256U * 1024U * 1024U;
constexpr std::size_t WP04TransportApplicationBufferCount = 2U;
constexpr std::size_t WP04TransportApplicationBufferBytes = protocol::MaxDatagramSize;
constexpr std::size_t WP04TransportStorageBytes =
	WP04TransportApplicationBufferCount * WP04TransportApplicationBufferBytes;

enum class StartupBudgetError : std::uint8_t {
	None = 0,
	InvalidClientCount,
	ArithmeticOverflow,
	StaticCapExceeded,
};

// These categories cannot be priced honestly until their owning work
// packages provide concrete storage layouts. Keeping them explicit prevents a
// known subtotal from being mistaken for a complete startup budget.
enum class DeferredStartupBudgetCategory : std::uint8_t {
	ClientSlotStorage = 0,
	StateReassemblyStorage,
	TransportBuffers,
	ReliableWindowStorage,
	BaselineStorage,
	DeltaStorage,
	SerializationScratch,
	Metrics,
	Count,
};

struct Wp03KnownBudgetRequest {
	std::size_t max_clients = 0U;
	std::size_t session_id_registry_bytes = 0U;
	std::size_t reassembly_bytes_per_client = 0U;
	std::size_t reliable_retention_projection_bytes_per_client = 0U;
};

struct Wp03KnownBudgetSubtotal {
	StartupBudgetError error = StartupBudgetError::None;
	bool is_complete = false;
	std::size_t known_bytes = 0U;
	std::uint64_t metric_known_bytes = 0U;
	std::size_t session_id_registry_bytes = 0U;
	std::size_t reassembly_bytes = 0U;
	std::size_t reliable_retention_projection_bytes = 0U;
	std::size_t transport_buffer_bytes = 0U;
	std::size_t client_slot_count = 0U;
	std::size_t reassembly_slot_count = 0U;
	std::size_t baseline_slot_count = 0U;
	std::size_t delta_slot_count = 0U;
	std::uint16_t deferred_categories = 0U;
};

bool checked_add_size(std::size_t left, std::size_t right, std::size_t& output) noexcept;
bool checked_multiply_size(std::size_t left, std::size_t right, std::size_t& output) noexcept;
bool checked_add_metric_u64(std::uint64_t left, std::uint64_t right, std::uint64_t& output) noexcept;
bool checked_multiply_metric_u64(std::uint64_t left, std::uint64_t right, std::uint64_t& output) noexcept;

Wp03KnownBudgetRequest make_wp03_known_budget_request(std::size_t max_clients) noexcept;
Wp03KnownBudgetSubtotal calculate_wp03_known_budget_subtotal(const Wp03KnownBudgetRequest& request) noexcept;
Wp03KnownBudgetSubtotal apply_wp04_transport_budget(const Wp03KnownBudgetSubtotal& wp03_subtotal,
	std::size_t application_buffer_count,
	std::size_t application_buffer_bytes) noexcept;
Wp03KnownBudgetSubtotal calculate_wp04_startup_budget(const Wp03KnownBudgetRequest& request) noexcept;
bool startup_budget_category_is_deferred(const Wp03KnownBudgetSubtotal& subtotal,
	DeferredStartupBudgetCategory category) noexcept;

} // namespace telemetry::detail
