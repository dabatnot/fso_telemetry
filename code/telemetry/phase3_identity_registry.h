#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace telemetry::detail {

constexpr std::size_t Phase3MaximumSensorIdentities = 65'536U;

struct Phase3SensorIdentityKey {
	std::uint32_t object_signature = 0U;
	std::uint8_t object_type = 0U;
};

enum class Phase3IdentityProvisionStatus : std::uint8_t {
	Ready = 0,
	AlreadyReady,
	AllocationFailure,
	Count,
};

enum class Phase3IdentityResolveStatus : std::uint8_t {
	Existing = 0,
	Allocated,
	InvalidKey,
	NotReady,
	CapacityExhausted,
	CounterExhausted,
	Count,
};

enum class Phase3IdentityReconcileStatus : std::uint8_t {
	Existing = 0,
	Bound,
	InvalidKey,
	InvalidEntityId,
	NotReady,
	CapacityExhausted,
	KeyConflict,
	EntityIdConflict,
	Count,
};

struct Phase3IdentityResolveResult {
	Phase3IdentityResolveStatus status = Phase3IdentityResolveStatus::InvalidKey;
	std::uint64_t entity_id = 0U;
};

class Phase3IdentityRegistry final {
  public:
	static constexpr std::size_t Capacity = Phase3MaximumSensorIdentities;

	Phase3IdentityRegistry() noexcept = default;
	~Phase3IdentityRegistry() = default;

	Phase3IdentityRegistry(const Phase3IdentityRegistry&) = delete;
	Phase3IdentityRegistry& operator=(const Phase3IdentityRegistry&) = delete;
	Phase3IdentityRegistry(Phase3IdentityRegistry&&) = delete;
	Phase3IdentityRegistry& operator=(Phase3IdentityRegistry&&) = delete;

	Phase3IdentityProvisionStatus provision() noexcept;
	void reset_session() noexcept;

	Phase3IdentityResolveResult resolve(const Phase3SensorIdentityKey& key) noexcept;
	Phase3IdentityReconcileStatus reconcile_phase2_binding(
		const Phase3SensorIdentityKey& key, std::uint64_t entity_id) noexcept;
	bool begin_transaction() noexcept;
	void commit_transaction() noexcept;
	void rollback_transaction() noexcept;

	bool ready() const noexcept;
	bool transaction_active() const noexcept;
	std::size_t identity_count() const noexcept;
	std::uint64_t last_allocated_entity_id() const noexcept;
	std::size_t provisioned_bytes() const noexcept;

  private:
	struct Entry {
		Phase3SensorIdentityKey key{};
		std::uint64_t entity_id = 0U;
		bool occupied = false;
	};

	static bool valid_key(const Phase3SensorIdentityKey& key) noexcept;
	static bool keys_equal(
		const Phase3SensorIdentityKey& left, const Phase3SensorIdentityKey& right) noexcept;
	static std::size_t hash_key(const Phase3SensorIdentityKey& key) noexcept;

	Entry* find_entry(const Phase3SensorIdentityKey& key) noexcept;
	const Entry* find_entry(const Phase3SensorIdentityKey& key) const noexcept;
	Entry* find_empty_slot(const Phase3SensorIdentityKey& key) noexcept;
	const Entry* find_entity_id(std::uint64_t entity_id) const noexcept;

	std::unique_ptr<Entry[]> m_entries;
	std::unique_ptr<std::size_t[]> m_transaction_slots;
	std::size_t m_identity_count = 0U;
	std::uint64_t m_last_allocated_entity_id = 0U;
	std::size_t m_transaction_slot_count = 0U;
	std::size_t m_transaction_identity_count = 0U;
	std::uint64_t m_transaction_last_allocated_entity_id = 0U;
	bool m_transaction_active = false;
};

} // namespace telemetry::detail
