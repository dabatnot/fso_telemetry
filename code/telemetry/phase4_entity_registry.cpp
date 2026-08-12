#include "telemetry/phase4_entity_registry.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace telemetry::detail {

Phase4EntityRegistryStatus Phase4EntityRegistry::provision() noexcept
{
	if (ready()) return Phase4EntityRegistryStatus::Existing;
	auto entries = std::unique_ptr<Entry[]>(new (std::nothrow) Entry[Capacity]);
	if (!entries) return Phase4EntityRegistryStatus::NotReady;
	m_entries = std::move(entries);
	m_identity_count = 0U;
	m_active_count = 0U;
	m_last_allocated_entity_id = 0U;
	return Phase4EntityRegistryStatus::Allocated;
}

void Phase4EntityRegistry::reset_session() noexcept
{
	if (m_entries) std::fill_n(m_entries.get(), Capacity, Entry{});
	m_identity_count = 0U;
	m_active_count = 0U;
	m_last_allocated_entity_id = 0U;
}

Phase4EntityResolveResult Phase4EntityRegistry::resolve(const Phase4EntityIdentityKey& key) noexcept
{
	if (!valid_key(key)) return {Phase4EntityRegistryStatus::InvalidKey, 0U};
	if (!ready()) return {Phase4EntityRegistryStatus::NotReady, 0U};
	if (const auto* existing = find_entry(key)) {
		return {existing->retired ? Phase4EntityRegistryStatus::AlreadyRetired :
			Phase4EntityRegistryStatus::Existing, existing->entity_id};
	}
	if (m_identity_count == Capacity) return {Phase4EntityRegistryStatus::CapacityExhausted, 0U};
	if (m_last_allocated_entity_id == std::numeric_limits<std::uint64_t>::max()) {
		return {Phase4EntityRegistryStatus::CounterExhausted, 0U};
	}
	auto* slot = find_empty_slot(key);
	if (slot == nullptr) return {Phase4EntityRegistryStatus::CapacityExhausted, 0U};
	slot->key = key;
	slot->entity_id = ++m_last_allocated_entity_id;
	slot->occupied = true;
	++m_identity_count;
	++m_active_count;
	return {Phase4EntityRegistryStatus::Allocated, slot->entity_id};
}

Phase4EntityRegistryStatus Phase4EntityRegistry::retire(const Phase4EntityIdentityKey& key) noexcept
{
	if (!valid_key(key)) return Phase4EntityRegistryStatus::InvalidKey;
	if (!ready()) return Phase4EntityRegistryStatus::NotReady;
	auto* entry = find_entry(key);
	if (entry == nullptr) return Phase4EntityRegistryStatus::NotFound;
	if (entry->retired) return Phase4EntityRegistryStatus::AlreadyRetired;
	entry->retired = true;
	--m_active_count;
	return Phase4EntityRegistryStatus::Retired;
}

bool Phase4EntityRegistry::ready() const noexcept { return m_entries != nullptr; }
std::size_t Phase4EntityRegistry::identity_count() const noexcept { return m_identity_count; }
std::size_t Phase4EntityRegistry::active_count() const noexcept { return m_active_count; }
std::uint64_t Phase4EntityRegistry::last_allocated_entity_id() const noexcept { return m_last_allocated_entity_id; }
std::size_t Phase4EntityRegistry::provisioned_bytes() const noexcept { return ready() ? sizeof(Entry) * Capacity : 0U; }

bool Phase4EntityRegistry::valid_key(const Phase4EntityIdentityKey& key) noexcept
{
	return key.stable_identity != 0U && key.object_type >= protocol::ObjectType::Ship &&
		key.object_type <= protocol::ObjectType::Other;
}

bool Phase4EntityRegistry::keys_equal(const Phase4EntityIdentityKey& left,
	const Phase4EntityIdentityKey& right) noexcept
{
	return left.stable_identity == right.stable_identity && left.object_type == right.object_type;
}

std::size_t Phase4EntityRegistry::hash_key(const Phase4EntityIdentityKey& key) noexcept
{
	const auto mixed = key.stable_identity * 0x9E3779B185EBCA87ULL ^
		static_cast<std::uint64_t>(key.object_type) * 0xC2B2AE3D27D4EB4FULL;
	return static_cast<std::size_t>(mixed ^ (mixed >> 32U)) & (Capacity - 1U);
}

Phase4EntityRegistry::Entry* Phase4EntityRegistry::find_entry(const Phase4EntityIdentityKey& key) noexcept
{
	return const_cast<Entry*>(static_cast<const Phase4EntityRegistry&>(*this).find_entry(key));
}

const Phase4EntityRegistry::Entry* Phase4EntityRegistry::find_entry(const Phase4EntityIdentityKey& key) const noexcept
{
	auto index = hash_key(key);
	for (std::size_t probe = 0U; probe < Capacity; ++probe) {
		const auto& entry = m_entries[index];
		if (!entry.occupied) return nullptr;
		if (keys_equal(entry.key, key)) return &entry;
		index = (index + 1U) & (Capacity - 1U);
	}
	return nullptr;
}

Phase4EntityRegistry::Entry* Phase4EntityRegistry::find_empty_slot(const Phase4EntityIdentityKey& key) noexcept
{
	auto index = hash_key(key);
	for (std::size_t probe = 0U; probe < Capacity; ++probe) {
		auto& entry = m_entries[index];
		if (!entry.occupied) return &entry;
		index = (index + 1U) & (Capacity - 1U);
	}
	return nullptr;
}

} // namespace telemetry::detail
