#include "telemetry/phase3_identity_registry.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace telemetry::detail {

Phase3IdentityProvisionStatus Phase3IdentityRegistry::provision() noexcept
{
	if (ready()) {
		return Phase3IdentityProvisionStatus::AlreadyReady;
	}

	auto entries = std::unique_ptr<Entry[]>(new (std::nothrow) Entry[Capacity]);
	auto transaction_slots =
		std::unique_ptr<std::size_t[]>(new (std::nothrow) std::size_t[Capacity]);
	if (!entries || !transaction_slots) {
		return Phase3IdentityProvisionStatus::AllocationFailure;
	}

	m_entries = std::move(entries);
	m_transaction_slots = std::move(transaction_slots);
	m_identity_count = 0U;
	m_last_allocated_entity_id = 0U;
	m_transaction_slot_count = 0U;
	m_transaction_identity_count = 0U;
	m_transaction_last_allocated_entity_id = 0U;
	m_transaction_active = false;
	return Phase3IdentityProvisionStatus::Ready;
}

void Phase3IdentityRegistry::reset_session() noexcept
{
	if (m_entries) {
		std::fill_n(m_entries.get(), Capacity, Entry{});
	}
	m_identity_count = 0U;
	m_last_allocated_entity_id = 0U;
}

Phase3IdentityResolveResult Phase3IdentityRegistry::resolve(
	const Phase3SensorIdentityKey& key) noexcept
{
	if (!valid_key(key)) {
		return {Phase3IdentityResolveStatus::InvalidKey, 0U};
	}
	if (!ready()) {
		return {Phase3IdentityResolveStatus::NotReady, 0U};
	}
	if (const auto* existing = find_entry(key)) {
		return {Phase3IdentityResolveStatus::Existing, existing->entity_id};
	}
	if (m_identity_count == Capacity) {
		return {Phase3IdentityResolveStatus::CapacityExhausted, 0U};
	}
	if (m_last_allocated_entity_id == std::numeric_limits<std::uint64_t>::max()) {
		return {Phase3IdentityResolveStatus::CounterExhausted, 0U};
	}

	auto* slot = find_empty_slot(key);
	if (slot == nullptr) {
		return {Phase3IdentityResolveStatus::CapacityExhausted, 0U};
	}

	++m_last_allocated_entity_id;
	slot->key = key;
	slot->entity_id = m_last_allocated_entity_id;
	slot->occupied = true;
	if (m_transaction_active) {
		m_transaction_slots[m_transaction_slot_count++] =
			static_cast<std::size_t>(slot - m_entries.get());
	}
	++m_identity_count;
	return {Phase3IdentityResolveStatus::Allocated, slot->entity_id};
}

Phase3IdentityReconcileStatus Phase3IdentityRegistry::reconcile_phase2_binding(
	const Phase3SensorIdentityKey& key, std::uint64_t entity_id) noexcept
{
	if (!valid_key(key)) {
		return Phase3IdentityReconcileStatus::InvalidKey;
	}
	if (entity_id == 0U) {
		return Phase3IdentityReconcileStatus::InvalidEntityId;
	}
	if (!ready()) {
		return Phase3IdentityReconcileStatus::NotReady;
	}
	if (const auto* existing = find_entry(key)) {
		return existing->entity_id == entity_id ? Phase3IdentityReconcileStatus::Existing
												: Phase3IdentityReconcileStatus::KeyConflict;
	}
	if (find_entity_id(entity_id) != nullptr) {
		return Phase3IdentityReconcileStatus::EntityIdConflict;
	}
	if (m_identity_count == Capacity) {
		return Phase3IdentityReconcileStatus::CapacityExhausted;
	}

	auto* slot = find_empty_slot(key);
	if (slot == nullptr) {
		return Phase3IdentityReconcileStatus::CapacityExhausted;
	}
	slot->key = key;
	slot->entity_id = entity_id;
	slot->occupied = true;
	if (m_transaction_active) {
		m_transaction_slots[m_transaction_slot_count++] =
			static_cast<std::size_t>(slot - m_entries.get());
	}
	++m_identity_count;
	m_last_allocated_entity_id = std::max(m_last_allocated_entity_id, entity_id);
	return Phase3IdentityReconcileStatus::Bound;
}

bool Phase3IdentityRegistry::begin_transaction() noexcept
{
	if (!ready() || m_transaction_active) return false;
	m_transaction_slot_count = 0U;
	m_transaction_identity_count = m_identity_count;
	m_transaction_last_allocated_entity_id = m_last_allocated_entity_id;
	m_transaction_active = true;
	return true;
}

void Phase3IdentityRegistry::commit_transaction() noexcept
{
	if (!m_transaction_active) return;
	m_transaction_slot_count = 0U;
	m_transaction_active = false;
}

void Phase3IdentityRegistry::rollback_transaction() noexcept
{
	if (!m_transaction_active) return;
	for (std::size_t index = 0U; index < m_transaction_slot_count; ++index) {
		m_entries[m_transaction_slots[index]] = {};
	}
	m_identity_count = m_transaction_identity_count;
	m_last_allocated_entity_id =
		m_transaction_last_allocated_entity_id;
	m_transaction_slot_count = 0U;
	m_transaction_active = false;
}

bool Phase3IdentityRegistry::ready() const noexcept
{
	return m_entries != nullptr && m_transaction_slots != nullptr;
}

bool Phase3IdentityRegistry::transaction_active() const noexcept
{
	return m_transaction_active;
}

std::size_t Phase3IdentityRegistry::identity_count() const noexcept
{
	return m_identity_count;
}

std::uint64_t Phase3IdentityRegistry::last_allocated_entity_id() const noexcept
{
	return m_last_allocated_entity_id;
}

std::size_t Phase3IdentityRegistry::provisioned_bytes() const noexcept
{
	return ready()
		? (sizeof(Entry) + sizeof(std::size_t)) * Capacity
		: 0U;
}

bool Phase3IdentityRegistry::valid_key(const Phase3SensorIdentityKey& key) noexcept
{
	return key.object_signature != 0U && key.object_type != 0U;
}

bool Phase3IdentityRegistry::keys_equal(
	const Phase3SensorIdentityKey& left, const Phase3SensorIdentityKey& right) noexcept
{
	return left.object_signature == right.object_signature && left.object_type == right.object_type;
}

std::size_t Phase3IdentityRegistry::hash_key(const Phase3SensorIdentityKey& key) noexcept
{
	const auto mixed = static_cast<std::uint64_t>(key.object_signature) * 0x9E3779B185EBCA87ULL ^
		static_cast<std::uint64_t>(key.object_type) * 0xC2B2AE3D27D4EB4FULL;
	return static_cast<std::size_t>(mixed ^ (mixed >> 32U)) & (Capacity - 1U);
}

Phase3IdentityRegistry::Entry* Phase3IdentityRegistry::find_entry(
	const Phase3SensorIdentityKey& key) noexcept
{
	return const_cast<Entry*>(static_cast<const Phase3IdentityRegistry&>(*this).find_entry(key));
}

const Phase3IdentityRegistry::Entry* Phase3IdentityRegistry::find_entry(
	const Phase3SensorIdentityKey& key) const noexcept
{
	auto index = hash_key(key);
	for (std::size_t probe = 0U; probe < Capacity; ++probe) {
		const auto& entry = m_entries[index];
		if (!entry.occupied) {
			return nullptr;
		}
		if (keys_equal(entry.key, key)) {
			return &entry;
		}
		index = (index + 1U) & (Capacity - 1U);
	}
	return nullptr;
}

Phase3IdentityRegistry::Entry* Phase3IdentityRegistry::find_empty_slot(
	const Phase3SensorIdentityKey& key) noexcept
{
	auto index = hash_key(key);
	for (std::size_t probe = 0U; probe < Capacity; ++probe) {
		auto& entry = m_entries[index];
		if (!entry.occupied) {
			return &entry;
		}
		index = (index + 1U) & (Capacity - 1U);
	}
	return nullptr;
}

const Phase3IdentityRegistry::Entry* Phase3IdentityRegistry::find_entity_id(
	std::uint64_t entity_id) const noexcept
{
	for (std::size_t index = 0U; index < Capacity; ++index) {
		const auto& entry = m_entries[index];
		if (entry.occupied && entry.entity_id == entity_id) {
			return &entry;
		}
	}
	return nullptr;
}

} // namespace telemetry::detail
