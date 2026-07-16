#include "telemetry/startup_budget.h"

#include "telemetry/identity.h"
#include "telemetry/protocol/telemetry_protocol_constants.h"
#include "telemetry/protocol/telemetry_reliable_window.h"

#include <limits>

namespace telemetry::detail {
namespace {

constexpr std::size_t MinimumClientCount = 1U;
constexpr std::size_t MaximumClientCount = 4U;
constexpr std::size_t BaselineSlotsPerClient = 2U;
constexpr std::size_t DeltaSlotsPerClient = 2U;
constexpr std::uint16_t AllDeferredCategories =
	(static_cast<std::uint16_t>(1U) << static_cast<std::uint8_t>(DeferredStartupBudgetCategory::Count)) - 1U;

static_assert(SessionIdRegistryStorageSlotCount == MaximumSessionIdsPerProcess * 2U,
	"The WP03 budget must price the real SessionIdAllocator registry representation");
static_assert(protocol::MaxStateReassembliesPerClient == 4U,
	"Phase 1 requires exactly four state reassembly slots per client");

Wp03KnownBudgetSubtotal failed_result(StartupBudgetError error) noexcept
{
	Wp03KnownBudgetSubtotal result;
	result.error = error;
	return result;
}

bool size_value_to_metric(std::size_t value, std::uint64_t& output) noexcept
{
	output = 0U;
	if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
		if (value > static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max())) {
			return false;
		}
	}
	output = static_cast<std::uint64_t>(value);
	return true;
}

} // namespace

bool checked_add_size(std::size_t left, std::size_t right, std::size_t& output) noexcept
{
	output = 0U;
	if (right > std::numeric_limits<std::size_t>::max() - left) {
		return false;
	}
	output = left + right;
	return true;
}

bool checked_multiply_size(std::size_t left, std::size_t right, std::size_t& output) noexcept
{
	output = 0U;
	if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left) {
		return false;
	}
	output = left * right;
	return true;
}

bool checked_add_metric_u64(std::uint64_t left, std::uint64_t right, std::uint64_t& output) noexcept
{
	output = 0U;
	if (right > std::numeric_limits<std::uint64_t>::max() - left) {
		return false;
	}
	output = left + right;
	return true;
}

bool checked_multiply_metric_u64(std::uint64_t left, std::uint64_t right, std::uint64_t& output) noexcept
{
	output = 0U;
	if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
		return false;
	}
	output = left * right;
	return true;
}

Wp03KnownBudgetRequest make_wp03_known_budget_request(std::size_t max_clients) noexcept
{
	Wp03KnownBudgetRequest request;
	request.max_clients = max_clients;
	request.session_id_registry_bytes = SessionIdRegistryStorageBytes;
	request.reassembly_bytes_per_client = protocol::MaxStateReassemblyBytesPerClient;
	request.reliable_retention_projection_bytes_per_client = protocol::ReliableWindowMaximumRetainedBytes;
	return request;
}

Wp03KnownBudgetSubtotal calculate_wp03_known_budget_subtotal(const Wp03KnownBudgetRequest& request) noexcept
{
	if (request.max_clients < MinimumClientCount || request.max_clients > MaximumClientCount) {
		return failed_result(StartupBudgetError::InvalidClientCount);
	}

	std::size_t reassembly_bytes = 0U;
	std::size_t reliable_retention_bytes = 0U;
	std::size_t subtotal = 0U;
	std::size_t known_bytes = 0U;
	std::size_t reassembly_slot_count = 0U;
	std::size_t baseline_slot_count = 0U;
	std::size_t delta_slot_count = 0U;
	if (!checked_multiply_size(request.max_clients, request.reassembly_bytes_per_client, reassembly_bytes) ||
		!checked_multiply_size(
			request.max_clients, request.reliable_retention_projection_bytes_per_client, reliable_retention_bytes) ||
		!checked_add_size(request.session_id_registry_bytes, reassembly_bytes, subtotal) ||
		!checked_add_size(subtotal, reliable_retention_bytes, known_bytes) ||
		!checked_multiply_size(
			request.max_clients, protocol::MaxStateReassembliesPerClient, reassembly_slot_count) ||
		!checked_multiply_size(request.max_clients, BaselineSlotsPerClient, baseline_slot_count) ||
		!checked_multiply_size(request.max_clients, DeltaSlotsPerClient, delta_slot_count)) {
		return failed_result(StartupBudgetError::ArithmeticOverflow);
	}

	std::uint64_t metric_clients = 0U;
	std::uint64_t metric_registry_bytes = 0U;
	std::uint64_t metric_reassembly_per_client = 0U;
	std::uint64_t metric_reliable_per_client = 0U;
	std::uint64_t metric_reassembly_bytes = 0U;
	std::uint64_t metric_reliable_bytes = 0U;
	std::uint64_t metric_subtotal = 0U;
	std::uint64_t metric_known_bytes = 0U;
	if (!size_value_to_metric(request.max_clients, metric_clients) ||
		!size_value_to_metric(request.session_id_registry_bytes, metric_registry_bytes) ||
		!size_value_to_metric(request.reassembly_bytes_per_client, metric_reassembly_per_client) ||
		!size_value_to_metric(
			request.reliable_retention_projection_bytes_per_client, metric_reliable_per_client) ||
		!checked_multiply_metric_u64(metric_clients, metric_reassembly_per_client, metric_reassembly_bytes) ||
		!checked_multiply_metric_u64(metric_clients, metric_reliable_per_client, metric_reliable_bytes) ||
		!checked_add_metric_u64(metric_registry_bytes, metric_reassembly_bytes, metric_subtotal) ||
		!checked_add_metric_u64(metric_subtotal, metric_reliable_bytes, metric_known_bytes)) {
		return failed_result(StartupBudgetError::ArithmeticOverflow);
	}

	if (known_bytes > WP03ProvisionalKnownBudgetCapBytes) {
		return failed_result(StartupBudgetError::StaticCapExceeded);
	}

	Wp03KnownBudgetSubtotal result;
	result.error = StartupBudgetError::None;
	result.is_complete = false;
	result.known_bytes = known_bytes;
	result.metric_known_bytes = metric_known_bytes;
	result.session_id_registry_bytes = request.session_id_registry_bytes;
	result.reassembly_bytes = reassembly_bytes;
	result.reliable_retention_projection_bytes = reliable_retention_bytes;
	result.client_slot_count = request.max_clients;
	result.reassembly_slot_count = reassembly_slot_count;
	result.baseline_slot_count = baseline_slot_count;
	result.delta_slot_count = delta_slot_count;
	result.deferred_categories = AllDeferredCategories;
	return result;
}

Wp03KnownBudgetSubtotal apply_wp04_transport_budget(const Wp03KnownBudgetSubtotal& wp03_subtotal,
	std::size_t application_buffer_count,
	std::size_t application_buffer_bytes) noexcept
{
	if (wp03_subtotal.error != StartupBudgetError::None) {
		return failed_result(wp03_subtotal.error);
	}

	std::size_t transport_buffer_bytes = 0U;
	std::size_t known_bytes = 0U;
	std::uint64_t metric_buffer_count = 0U;
	std::uint64_t metric_buffer_bytes = 0U;
	std::uint64_t metric_transport_buffer_bytes = 0U;
	std::uint64_t metric_known_bytes = 0U;
	if (!checked_multiply_size(
			application_buffer_count, application_buffer_bytes, transport_buffer_bytes) ||
		!checked_add_size(wp03_subtotal.known_bytes, transport_buffer_bytes, known_bytes) ||
		!size_value_to_metric(application_buffer_count, metric_buffer_count) ||
		!size_value_to_metric(application_buffer_bytes, metric_buffer_bytes) ||
		!checked_multiply_metric_u64(
			metric_buffer_count, metric_buffer_bytes, metric_transport_buffer_bytes) ||
		!checked_add_metric_u64(
			wp03_subtotal.metric_known_bytes, metric_transport_buffer_bytes, metric_known_bytes)) {
		return failed_result(StartupBudgetError::ArithmeticOverflow);
	}
	if (known_bytes > WP03ProvisionalKnownBudgetCapBytes) {
		return failed_result(StartupBudgetError::StaticCapExceeded);
	}

	auto result = wp03_subtotal;
	result.known_bytes = known_bytes;
	result.metric_known_bytes = metric_known_bytes;
	result.transport_buffer_bytes = transport_buffer_bytes;
	const auto transport_bit = static_cast<std::uint16_t>(
		static_cast<std::uint16_t>(1U)
		<< static_cast<std::uint8_t>(DeferredStartupBudgetCategory::TransportBuffers));
	result.deferred_categories = static_cast<std::uint16_t>(result.deferred_categories & ~transport_bit);
	result.is_complete = wp03_subtotal.is_complete && result.deferred_categories == 0U;
	return result;
}

Wp03KnownBudgetSubtotal calculate_wp04_startup_budget(const Wp03KnownBudgetRequest& request) noexcept
{
	return apply_wp04_transport_budget(calculate_wp03_known_budget_subtotal(request),
		WP04TransportApplicationBufferCount,
		WP04TransportApplicationBufferBytes);
}

Wp03KnownBudgetSubtotal calculate_wp06_startup_budget(const Wp03KnownBudgetSubtotal& wp04_subtotal,
	std::size_t max_clients) noexcept
{
	if (wp04_subtotal.error != StartupBudgetError::None) {
		return failed_result(wp04_subtotal.error);
	}
	if (max_clients < MinimumClientCount || max_clients > MaximumClientCount) {
		return failed_result(StartupBudgetError::InvalidClientCount);
	}

	std::size_t client_slot_bytes = 0U;
	std::size_t reassembly_bytes = 0U;
	std::size_t reliable_bytes = 0U;
	std::size_t prior_projection = 0U;
	std::size_t retained_known = 0U;
	std::size_t subtotal = 0U;
	std::size_t subtotal2 = 0U;
	std::size_t subtotal3 = 0U;
	std::size_t subtotal4 = 0U;
	std::size_t subtotal5 = 0U;
	std::size_t subtotal6 = 0U;
	std::size_t known_bytes = 0U;
	if (!checked_add_size(wp04_subtotal.reassembly_bytes,
			wp04_subtotal.reliable_retention_projection_bytes,
			prior_projection) ||
		wp04_subtotal.known_bytes < prior_projection) {
		return failed_result(StartupBudgetError::ArithmeticOverflow);
	}
	retained_known = wp04_subtotal.known_bytes - prior_projection;
	if (!checked_multiply_size(max_clients, Wp06ClientSlotStorageBytes, client_slot_bytes) ||
		!checked_multiply_size(max_clients, protocol::MaxStateReassemblyBytesPerClient, reassembly_bytes) ||
		!checked_multiply_size(max_clients, Wp06ReliableRetentionBytesPerClient, reliable_bytes) ||
		!checked_add_size(retained_known, client_slot_bytes, subtotal) ||
		!checked_add_size(subtotal, reassembly_bytes, subtotal2) ||
		!checked_add_size(subtotal2, reliable_bytes, subtotal3) ||
		!checked_add_size(subtotal3, Wp06RateLimiterStorageBytes, subtotal4) ||
		!checked_add_size(subtotal4, Wp06HandshakeCacheStorageBytes, subtotal5) ||
		!checked_add_size(subtotal5, Wp06PreproofLedgerStorageBytes, subtotal6) ||
		!checked_add_size(subtotal6, Wp06OutputQueueStorageBytes, known_bytes)) {
		return failed_result(StartupBudgetError::ArithmeticOverflow);
	}
	if (known_bytes > WP03ProvisionalKnownBudgetCapBytes) {
		return failed_result(StartupBudgetError::StaticCapExceeded);
	}

	auto result = wp04_subtotal;
	result.known_bytes = known_bytes;
	result.metric_known_bytes = static_cast<std::uint64_t>(known_bytes);
	result.client_slot_bytes = client_slot_bytes;
	result.rate_limiter_bytes = Wp06RateLimiterStorageBytes;
	result.handshake_cache_bytes = Wp06HandshakeCacheStorageBytes;
	result.preproof_ledger_bytes = Wp06PreproofLedgerStorageBytes;
	result.output_queue_bytes = Wp06OutputQueueStorageBytes;
	result.reassembly_bytes = reassembly_bytes;
	result.reliable_retention_projection_bytes = reliable_bytes;
	result.client_slot_count = max_clients;
	result.reassembly_slot_count = max_clients * protocol::MaxStateReassembliesPerClient;
	for (const auto category : {DeferredStartupBudgetCategory::ClientSlotStorage,
			 DeferredStartupBudgetCategory::StateReassemblyStorage,
			 DeferredStartupBudgetCategory::ReliableWindowStorage}) {
		const auto bit = static_cast<std::uint16_t>(1U << static_cast<std::uint8_t>(category));
		result.deferred_categories = static_cast<std::uint16_t>(result.deferred_categories & ~bit);
	}
	result.is_complete = result.deferred_categories == 0U;
	return result;
}

bool wp06_budget_matches_owned_storage(const Wp03KnownBudgetSubtotal& budget,
	const Wp06OwnedCapacity& owned) noexcept
{
	std::size_t expected_client_slot_bytes = 0U;
	if (!checked_multiply_size(owned.client_slots, Wp06ClientSlotStorageBytes, expected_client_slot_bytes)) {
		return false;
	}
	return budget.error == StartupBudgetError::None &&
		budget.client_slot_count == owned.client_slots &&
		budget.client_slot_bytes == expected_client_slot_bytes &&
		budget.reassembly_slot_count == owned.state_reassembly_slots &&
		budget.reassembly_bytes == owned.state_reassembly_bytes &&
		budget.reliable_retention_projection_bytes == owned.reliable_retention_bytes &&
		budget.rate_limiter_bytes == owned.rate_limiter_bytes &&
		budget.handshake_cache_bytes == owned.handshake_cache_bytes &&
		budget.preproof_ledger_bytes == owned.preproof_ledger_bytes &&
		budget.output_queue_bytes == owned.output_queue_bytes &&
		owned.dynamic_allocations_after_ready == 0U;
}

bool startup_budget_category_is_deferred(const Wp03KnownBudgetSubtotal& subtotal,
	DeferredStartupBudgetCategory category) noexcept
{
	const auto index = static_cast<std::uint8_t>(category);
	if (index >= static_cast<std::uint8_t>(DeferredStartupBudgetCategory::Count)) {
		return false;
	}
	const auto bit = static_cast<std::uint16_t>(static_cast<std::uint16_t>(1U) << index);
	return (subtotal.deferred_categories & bit) != 0U;
}

} // namespace telemetry::detail
