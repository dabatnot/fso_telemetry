#pragma once

#include "telemetry/protocol/telemetry_protocol_constants.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace telemetry::detail {

constexpr std::size_t Phase4MaximumEntityIdentities = 65'536U;

// This is an opaque, collector-owned stable value. It is never serialized and
// therefore cannot expose an engine pointer, handle or array index.
struct Phase4EntityIdentityKey {
	std::uint64_t stable_identity = 0U;
	protocol::ObjectType object_type = protocol::ObjectType::Unknown;
};

enum class Phase4EntityRegistryStatus : std::uint8_t {
	Existing = 0,
	Allocated,
	Retired,
	InvalidKey,
	NotReady,
	CapacityExhausted,
	CounterExhausted,
	NotFound,
	AlreadyRetired,
	Reconciled,
	Count,
};

struct Phase4EntityResolveResult {
	Phase4EntityRegistryStatus status = Phase4EntityRegistryStatus::InvalidKey;
	std::uint64_t entity_id = 0U;
};

class Phase4EntityRegistry final {
  public:
	static constexpr std::size_t Capacity = Phase4MaximumEntityIdentities;

	Phase4EntityRegistry() noexcept = default;
	~Phase4EntityRegistry() = default;
	Phase4EntityRegistry(const Phase4EntityRegistry&) = delete;
	Phase4EntityRegistry& operator=(const Phase4EntityRegistry&) = delete;

	Phase4EntityRegistryStatus provision() noexcept;
	void reset_session() noexcept;
	Phase4EntityResolveResult resolve(const Phase4EntityIdentityKey& key) noexcept;
	Phase4EntityRegistryStatus retire(const Phase4EntityIdentityKey& key) noexcept;
	// Commits an exact collector inventory. Every active identity absent from
	// observed becomes a tombstone only after the complete input is valid.
	Phase4EntityRegistryStatus reconcile_observed(
		const Phase4EntityIdentityKey* observed, std::size_t observed_count) noexcept;
	Phase4EntityRegistryStatus begin_reconciliation() noexcept;
	Phase4EntityRegistryStatus mark_observed(
		const Phase4EntityIdentityKey& key) noexcept;
	Phase4EntityRegistryStatus commit_reconciliation() noexcept;

	bool ready() const noexcept;
	std::size_t identity_count() const noexcept;
	std::size_t active_count() const noexcept;
	std::uint64_t last_allocated_entity_id() const noexcept;
	std::size_t provisioned_bytes() const noexcept;

  private:
	struct Entry {
		Phase4EntityIdentityKey key{};
		std::uint64_t entity_id = 0U;
		bool occupied = false;
		bool retired = false;
		bool observed = false;
	};

	static bool valid_key(const Phase4EntityIdentityKey& key) noexcept;
	static bool keys_equal(const Phase4EntityIdentityKey& left,
		const Phase4EntityIdentityKey& right) noexcept;
	static std::size_t hash_key(const Phase4EntityIdentityKey& key) noexcept;
	Entry* find_entry(const Phase4EntityIdentityKey& key) noexcept;
	const Entry* find_entry(const Phase4EntityIdentityKey& key) const noexcept;
	Entry* find_empty_slot(const Phase4EntityIdentityKey& key) noexcept;

	std::unique_ptr<Entry[]> m_entries;
	std::size_t m_identity_count = 0U;
	std::size_t m_active_count = 0U;
	std::uint64_t m_last_allocated_entity_id = 0U;
	bool m_reconciliation_active = false;
};

} // namespace telemetry::detail
