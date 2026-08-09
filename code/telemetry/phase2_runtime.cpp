#include "telemetry/phase2_runtime.h"

#include <algorithm>
#include <limits>

namespace telemetry::detail {
namespace {

bool digest_is_zero(const protocol::Sha256Digest& value) noexcept
{
	for (const auto byte : value)
		if (byte != 0U) return false;
	return true;
}

int cause_priority(Phase2RuntimeSnapshotCause cause) noexcept
{
	switch (cause) {
	case Phase2RuntimeSnapshotCause::Resync:
		return 8;
	case Phase2RuntimeSnapshotCause::Initial:
		return 7;
	case Phase2RuntimeSnapshotCause::Catalog:
		return 6;
	case Phase2RuntimeSnapshotCause::Lifecycle:
	case Phase2RuntimeSnapshotCause::SupportTerminal:
	case Phase2RuntimeSnapshotCause::Topology:
		return 5;
	case Phase2RuntimeSnapshotCause::DeltaCapacity:
		return 4;
	case Phase2RuntimeSnapshotCause::Periodic:
		return 3;
	case Phase2RuntimeSnapshotCause::None:
	case Phase2RuntimeSnapshotCause::Count:
	default:
		return 0;
	}
}

} // namespace

bool Phase2RuntimeSlot::configure(
	Phase2Profile profile, std::size_t session_slot) noexcept
{
	if ((profile != Phase2Profile::CoreGate &&
		 profile != Phase2Profile::CompleteShip &&
		 profile != Phase2Profile::CockpitSensors) ||
		session_slot >= Phase2Wp07EpisodeLatches::SessionCapacity)
		return false;
	reset();
	m_profile = profile;
	m_session_slot = session_slot;
	m_configured =
		m_support_latches.activate_session(session_slot);
	return m_configured;
}

void Phase2RuntimeSlot::reset() noexcept
{
	m_entries = {};
	m_entry_count = 0U;
	m_next_entity_id = 1U;
	m_next_lifecycle_generation = 1U;
	m_next_snapshot_id = 1U;
	m_active_snapshot_id = 0U;
	m_candidate_snapshot_id = 0U;
	m_candidate_manifest_id = 0U;
	m_pending_cause = Phase2RuntimeSnapshotCause::None;
	m_manifest = {};
	m_active_catalog = {};
	m_staged_catalog = {};
	m_topology = {};
	m_support_latches.reset();
	m_candidate_support_latches.reset();
	m_pending_support = {};
	m_candidate_support = {};
	m_pending_support_count = 0U;
	m_candidate_support_count = 0U;
	m_cleanup_batch = {};
	m_candidate_cleanup_batch = {};
	m_pending_lifecycle = {};
	m_candidate_lifecycle = {};
	m_pending_lifecycle_count = 0U;
	m_candidate_lifecycle_count = 0U;
	m_next_event_id = 1U;
	m_started_snapshot_sequence = 0U;
	m_last_started_snapshot_cause =
		Phase2RuntimeSnapshotCause::None;
	m_current_block_samples = {};
	m_candidate_block_samples = {};
	m_active_block_samples = {};
	m_profile = Phase2Profile::None;
	m_session_slot = 0U;
	m_configured = false;
}

Phase2RuntimeResult Phase2RuntimeSlot::reconcile_closure(
	const Phase2CaptureLocalKey* signatures,
	std::size_t count,
	Phase2Wp05SubjectBinding* bindings,
	std::size_t binding_capacity) noexcept

{
	return reconcile_closure_with_public_ids(signatures, signatures, nullptr,
		count, bindings, binding_capacity);
}

Phase2RuntimeResult Phase2RuntimeSlot::reconcile_closure_with_public_ids(
	const Phase2CaptureLocalKey* identity_signatures,
	const Phase2CaptureLocalKey* binding_keys,
	const std::uint64_t* public_entity_ids,
	std::size_t count,
	Phase2Wp05SubjectBinding* bindings,
	std::size_t binding_capacity) noexcept
{
	if (!m_configured || count > ClosureCapacity ||
		(m_profile == Phase2Profile::CoreGate && count > 1U) ||
		(count != 0U &&
		 (identity_signatures == nullptr || binding_keys == nullptr ||
		  bindings == nullptr)) ||
		binding_capacity < count)
		return Phase2RuntimeResult::InvalidInput;
	for (std::size_t index = 0U; index < count; ++index) {
		if (identity_signatures[index].value == 0U ||
			binding_keys[index].value == 0U ||
			(public_entity_ids != nullptr &&
			 public_entity_ids[index] == 0U))
			return Phase2RuntimeResult::InvalidInput;
		for (std::size_t previous = 0U; previous < index; ++previous)
			if (identity_signatures[previous].value ==
					identity_signatures[index].value ||
				binding_keys[previous].value == binding_keys[index].value ||
				(public_entity_ids != nullptr &&
				 public_entity_ids[previous] == public_entity_ids[index]))
				return Phase2RuntimeResult::InvalidInput;
	}

	std::array<EntityEntry, ClosureCapacity> candidate{};
	std::array<Phase2Wp05SubjectBinding, ClosureCapacity>
		candidate_bindings{};
	std::size_t candidate_count = 0U;
	auto next_id = m_next_entity_id;
	auto next_generation = m_next_lifecycle_generation;
	bool changed = count != m_entry_count;
	for (std::size_t index = 0U; index < count; ++index) {
		const EntityEntry* previous_entry = nullptr;
		for (std::size_t existing = 0U;
			 existing < m_entry_count; ++existing)
			if (m_entries[existing].active &&
				m_entries[existing].signature ==
					identity_signatures[index].value) {
				previous_entry = &m_entries[existing];
				break;
			}
		auto& entry = candidate[candidate_count++];
		if (previous_entry == nullptr) {
			const auto requested_id = public_entity_ids != nullptr
				? public_entity_ids[index] : next_id;
			if (requested_id == 0U ||
				requested_id == std::numeric_limits<std::uint64_t>::max() ||
				next_generation == 0U ||
				next_generation ==
					std::numeric_limits<std::uint32_t>::max())
				return Phase2RuntimeResult::CounterExhausted;
			entry.signature = identity_signatures[index].value;
			entry.entity_id = requested_id;
			next_id = std::max(next_id, requested_id + 1U);
			entry.generation = next_generation++;
			changed = true;
		} else {
			if (public_entity_ids != nullptr &&
				previous_entry->entity_id != public_entity_ids[index])
				return Phase2RuntimeResult::InvalidInput;
			entry = *previous_entry;
		}
		entry.active = true;
		candidate_bindings[index].capture_key = binding_keys[index];
		candidate_bindings[index].entity_id = entry.entity_id;
	}
	m_entries = candidate;
	m_entry_count = candidate_count;
	m_next_entity_id = next_id;
	m_next_lifecycle_generation = next_generation;
	for (std::size_t index = 0U; index < count; ++index)
		bindings[index] = candidate_bindings[index];
	if (changed)
		(void)request_snapshot(
			m_active_snapshot_id == 0U
			? Phase2RuntimeSnapshotCause::Initial
			: Phase2RuntimeSnapshotCause::Topology);
	return changed ? Phase2RuntimeResult::SnapshotRequired :
		Phase2RuntimeResult::NoChange;
}

bool Phase2RuntimeSlot::has_catalog(
	const protocol::Sha256Digest& fingerprint) const noexcept
{
	return (!digest_is_zero(m_active_catalog) &&
			m_active_catalog == fingerprint) ||
		(!digest_is_zero(m_staged_catalog) &&
			m_staged_catalog == fingerprint);
}

Phase2RuntimeResult Phase2RuntimeSlot::stage_manifest(
	std::uint32_t manifest_id,
	const protocol::Sha256Digest& catalog_fingerprint) noexcept
{
	if (!m_configured || manifest_id == 0U ||
		digest_is_zero(catalog_fingerprint))
		return Phase2RuntimeResult::InvalidInput;
	if (has_catalog(catalog_fingerprint))
		return Phase2RuntimeResult::NoChange;
	if (m_manifest.staged_id != 0U) {
		m_manifest.rebuild_intent = true;
		return Phase2RuntimeResult::CandidateBusy;
	}
	if (manifest_id <= m_manifest.active_id)
		return Phase2RuntimeResult::Stale;
	m_manifest.staged_id = manifest_id;
	m_manifest.staged_applied = false;
	m_manifest.rebuild_intent = false;
	m_staged_catalog = catalog_fingerprint;
	return Phase2RuntimeResult::ManifestRequired;
}

Phase2RuntimeResult Phase2RuntimeSlot::preview_stage_manifest(
	std::uint32_t manifest_id,
	const protocol::Sha256Digest& catalog_fingerprint) const noexcept
{
	if (!m_configured || manifest_id == 0U ||
		digest_is_zero(catalog_fingerprint))
		return Phase2RuntimeResult::InvalidInput;
	if (has_catalog(catalog_fingerprint))
		return Phase2RuntimeResult::NoChange;
	if (m_manifest.staged_id != 0U)
		return Phase2RuntimeResult::CandidateBusy;
	if (manifest_id <= m_manifest.active_id)
		return Phase2RuntimeResult::Stale;
	return Phase2RuntimeResult::ManifestRequired;
}

Phase2RuntimeResult Phase2RuntimeSlot::observe_topology(
	const protocol::Sha256Digest& topology_fingerprint) noexcept
{
	if (!m_configured || digest_is_zero(topology_fingerprint))
		return Phase2RuntimeResult::InvalidInput;
	if (m_topology == topology_fingerprint)
		return Phase2RuntimeResult::NoChange;
	m_topology = topology_fingerprint;
	return request_snapshot(
		m_active_snapshot_id == 0U
		? Phase2RuntimeSnapshotCause::Initial
		: Phase2RuntimeSnapshotCause::Topology);
}

bool Phase2RuntimeSlot::append_lifecycle(
	Phase2RuntimeLifecycleEventKind kind, const EntityEntry& entry,
	std::uint64_t sample_time_us) noexcept
{
	if (kind == Phase2RuntimeLifecycleEventKind::Count ||
		entry.signature == 0U || entry.entity_id == 0U ||
		m_pending_lifecycle_count >= m_pending_lifecycle.size() ||
		m_next_event_id == 0U ||
		m_next_event_id == std::numeric_limits<std::uint64_t>::max())
		return false;
	auto& fact = m_pending_lifecycle[m_pending_lifecycle_count++];
	fact.event_id = m_next_event_id++;
	fact.dependency_snapshot_id =
		m_candidate_snapshot_id != 0U ? m_candidate_snapshot_id :
		m_active_snapshot_id;
	fact.entity_id = entry.entity_id;
	fact.sample_time_us = sample_time_us;
	fact.source_signature = entry.signature;
	fact.generation = entry.generation;
	fact.kind = kind;
	return true;
}

Phase2RuntimeResult Phase2RuntimeSlot::observe_lifecycle(
	const Phase2ObservationDto& observation,
	const Phase2Wp05SubjectBinding* bindings,
	std::size_t binding_count) noexcept
{
	if (!m_configured || binding_count != observation.ships.size() ||
		(binding_count != 0U && bindings == nullptr))
		return Phase2RuntimeResult::InvalidInput;
	auto candidate_entries = m_entries;
	auto candidate_pending = m_pending_lifecycle;
	const auto pending_before = m_pending_lifecycle_count;
	const auto event_before = m_next_event_id;
	bool changed = false;
	for (std::size_t index = 0U; index < binding_count; ++index) {
		const auto& ship = observation.ships[index];
		const auto& binding = bindings[index];
		if (binding.capture_key.value != ship.capture_key.value ||
			binding.entity_id == 0U) {
			m_pending_lifecycle = candidate_pending;
			m_pending_lifecycle_count = pending_before;
			m_next_event_id = event_before;
			return Phase2RuntimeResult::InvalidInput;
		}
		EntityEntry* entry = nullptr;
		for (auto& value : m_entries)
			if (value.active &&
				value.entity_id == binding.entity_id) {
				entry = &value;
				break;
			}
		if (entry == nullptr) {
			m_pending_lifecycle = candidate_pending;
			m_pending_lifecycle_count = pending_before;
			m_next_event_id = event_before;
			return Phase2RuntimeResult::InvalidInput;
		}
		const auto previous_state = entry->lifecycle;
		const auto previous_flags = entry->lifecycle_flags;
		if (entry->generation == 0U) {
			m_pending_lifecycle = candidate_pending;
			m_pending_lifecycle_count = pending_before;
			m_next_event_id = event_before;
			return Phase2RuntimeResult::InvalidInput;
		}
		if (previous_state == ShipLifecycleState::Count) {
			if (!append_lifecycle(Phase2RuntimeLifecycleEventKind::Appeared,
					*entry, ship.lifecycle.sample_time_us))
				goto capacity_failure;
			changed = true;
		}
		const auto disabled =
			(ship.lifecycle.lifecycle_flags &
			 protocol::EntityLifecycleFlagDisabled) != 0U;
		const auto was_disabled =
			(previous_flags & protocol::EntityLifecycleFlagDisabled) != 0U;
		if (disabled && !was_disabled) {
			if (!append_lifecycle(Phase2RuntimeLifecycleEventKind::Disabled,
					*entry, ship.lifecycle.sample_time_us))
				goto capacity_failure;
			changed = true;
		}
		if (ship.lifecycle.state == ShipLifecycleState::Dying &&
			previous_state != ShipLifecycleState::Dying) {
			if (!append_lifecycle(
					Phase2RuntimeLifecycleEventKind::DyingStarted,
					*entry, ship.lifecycle.sample_time_us))
				goto capacity_failure;
			changed = true;
		}
		if (ship.lifecycle.state == ShipLifecycleState::Destroyed &&
			previous_state != ShipLifecycleState::Destroyed) {
			if (!append_lifecycle(Phase2RuntimeLifecycleEventKind::Destroyed,
					*entry, ship.lifecycle.sample_time_us))
				goto capacity_failure;
			changed = true;
		}
		if (ship.lifecycle.state == ShipLifecycleState::Removed &&
			previous_state != ShipLifecycleState::Removed) {
			if (!append_lifecycle(
					Phase2RuntimeLifecycleEventKind::Disappeared,
					*entry, ship.lifecycle.sample_time_us))
				goto capacity_failure;
			changed = true;
		}
		entry->lifecycle = ship.lifecycle.state;
		entry->lifecycle_flags = ship.lifecycle.lifecycle_flags;
	}
	if (changed)
		(void)request_snapshot(Phase2RuntimeSnapshotCause::Lifecycle);
	return changed ? Phase2RuntimeResult::SnapshotRequired :
		Phase2RuntimeResult::NoChange;

capacity_failure:
	m_entries = candidate_entries;
	m_pending_lifecycle = candidate_pending;
	m_pending_lifecycle_count = pending_before;
	m_next_event_id = event_before;
	return Phase2RuntimeResult::CapacityExceeded;
}

bool Phase2RuntimeSlot::set_current_block_samples(
	const std::array<std::uint64_t, BlockCount>& samples) noexcept
{
	if (!m_configured) return false;
	for (const auto sample : samples)
		if (sample == 0U) return false;
	m_current_block_samples = samples;
	return true;
}

Phase2RuntimeResult Phase2RuntimeSlot::on_manifest_applied(
	std::uint32_t manifest_id) noexcept
{
	if (!m_configured || manifest_id == 0U)
		return Phase2RuntimeResult::InvalidInput;
	if (manifest_id == m_manifest.active_id)
		return Phase2RuntimeResult::NoChange;
	if (manifest_id != m_manifest.staged_id)
		return Phase2RuntimeResult::Stale;
	m_manifest.staged_applied = true;
	(void)request_snapshot(
		m_active_snapshot_id == 0U
		? Phase2RuntimeSnapshotCause::Initial
		: Phase2RuntimeSnapshotCause::Catalog);
	return Phase2RuntimeResult::SnapshotRequired;
}

Phase2RuntimeResult Phase2RuntimeSlot::request_snapshot(
	Phase2RuntimeSnapshotCause cause) noexcept
{
	if (!m_configured || cause == Phase2RuntimeSnapshotCause::None ||
		cause == Phase2RuntimeSnapshotCause::Count)
		return Phase2RuntimeResult::InvalidInput;
	if (cause_priority(cause) > cause_priority(m_pending_cause))
		m_pending_cause = cause;
	return Phase2RuntimeResult::SnapshotRequired;
}

Phase2RuntimeResult Phase2RuntimeSlot::apply_global_events(
	const Phase2Wp07GlobalEventBatch& batch) noexcept
{
	const auto preview = preview_global_events(batch);
	if (preview == Phase2RuntimeResult::InvalidInput ||
		preview == Phase2RuntimeResult::CapacityExceeded)
		return preview;
	for (std::size_t index = 0U; index < batch.cleanup.count; ++index) {
		const auto& intent = batch.cleanup.intents[index];
		std::size_t destination = m_cleanup_batch.count;
		for (std::size_t existing = 0U;
			 existing < m_cleanup_batch.count; ++existing)
			if (m_cleanup_batch.intents[existing].object_signature ==
				intent.object_signature) {
				destination = existing;
				break;
			}
		if (destination == m_cleanup_batch.count)
			++m_cleanup_batch.count;
		m_cleanup_batch.intents[destination] = intent;
		const EntityEntry* entry = nullptr;
		for (const auto& candidate : m_entries)
			if (candidate.active &&
				candidate.signature == intent.object_signature) {
				entry = &candidate;
				break;
			}
		if (entry == nullptr ||
			!append_lifecycle(
				intent.mode == ShipCleanupMode::Destroyed
					? Phase2RuntimeLifecycleEventKind::Destroyed
					: Phase2RuntimeLifecycleEventKind::Disappeared,
				*entry, intent.sample_time))
			return Phase2RuntimeResult::CapacityExceeded;
	}
	for (std::size_t index = 0U; index < batch.support_count; ++index)
		if (!m_support_latches.latch(m_session_slot, batch.support[index]))
			return Phase2RuntimeResult::CapacityExceeded;
	for (std::size_t index = 0U; index < batch.support_count; ++index)
		m_pending_support[m_pending_support_count++] =
			batch.support[index];
	if (batch.cleanup.count != 0U)
		(void)request_snapshot(Phase2RuntimeSnapshotCause::Lifecycle);
	if (batch.support_count != 0U)
		(void)request_snapshot(Phase2RuntimeSnapshotCause::SupportTerminal);
	return batch.cleanup.count == 0U && batch.support_count == 0U
		? Phase2RuntimeResult::NoChange
		: Phase2RuntimeResult::SnapshotRequired;
}

Phase2RuntimeResult Phase2RuntimeSlot::preview_global_events(
	const Phase2Wp07GlobalEventBatch& batch) const noexcept
{
	if (!m_configured || batch.cleanup.count > batch.cleanup.intents.size() ||
		batch.support_count > batch.support.size())
		return Phase2RuntimeResult::InvalidInput;
	std::size_t cleanup_new = 0U;
	for (std::size_t index = 0U; index < batch.cleanup.count; ++index) {
		const auto signature = batch.cleanup.intents[index].object_signature;
		if (signature == 0U) return Phase2RuntimeResult::InvalidInput;
		bool known_entity = false;
		for (const auto& entry : m_entries)
			known_entity = known_entity ||
				(entry.active && entry.signature == signature);
		if (!known_entity) return Phase2RuntimeResult::InvalidInput;
		bool present = false;
		for (std::size_t existing = 0U;
			 existing < m_cleanup_batch.count; ++existing)
			present = present ||
				m_cleanup_batch.intents[existing].object_signature ==
					signature;
		for (std::size_t previous = 0U; previous < index; ++previous)
			present = present ||
				batch.cleanup.intents[previous].object_signature ==
					signature;
		if (!present) ++cleanup_new;
	}
	if (cleanup_new > m_cleanup_batch.intents.size() -
			m_cleanup_batch.count)
		return Phase2RuntimeResult::CapacityExceeded;
	if (batch.cleanup.count >
		m_pending_lifecycle.size() - m_pending_lifecycle_count)
		return Phase2RuntimeResult::CapacityExceeded;
	if (batch.cleanup.count != 0U &&
		(m_next_event_id == 0U ||
		 m_next_event_id >
			std::numeric_limits<std::uint64_t>::max() -
				batch.cleanup.count))
		return Phase2RuntimeResult::CounterExhausted;
	std::size_t support_new = 0U;
	for (std::size_t index = 0U; index < batch.support_count; ++index) {
		const auto& fact = batch.support[index];
		if (fact.assisted_signature == 0U ||
			fact.episode_sequence == 0U)
			return Phase2RuntimeResult::InvalidInput;
		bool present = m_support_latches.pending(
			m_session_slot, fact.assisted_signature);
		for (std::size_t previous = 0U; previous < index; ++previous)
			present = present ||
				batch.support[previous].assisted_signature ==
					fact.assisted_signature;
		if (!present) ++support_new;
	}
	if (support_new >
		Phase2Wp07EpisodeLatches::EntriesPerSession -
			m_support_latches.size(m_session_slot))
		return Phase2RuntimeResult::CapacityExceeded;
	if (batch.support_count >
		m_pending_support.size() - m_pending_support_count)
		return Phase2RuntimeResult::CapacityExceeded;
	return batch.cleanup.count == 0U && batch.support_count == 0U
		? Phase2RuntimeResult::NoChange
		: Phase2RuntimeResult::Applied;
}

std::uint16_t Phase2RuntimeSlot::snapshot_flags(
	Phase2RuntimeSnapshotCause cause) noexcept
{
	if (cause == Phase2RuntimeSnapshotCause::Initial)
		return protocol::SnapshotFlagInitial;
	if (cause == Phase2RuntimeSnapshotCause::Resync)
		return protocol::SnapshotFlagResync;
	return cause == Phase2RuntimeSnapshotCause::None ||
		cause == Phase2RuntimeSnapshotCause::Count
		? protocol::SnapshotFlagNone
		: protocol::SnapshotFlagPeriodicKeyframe;
}

Phase2RuntimeResult Phase2RuntimeSlot::next_snapshot_plan(
	Phase2RuntimeSnapshotPlan& plan) noexcept
{
	plan = {};
	if (!m_configured)
		return Phase2RuntimeResult::InvalidInput;
	if (m_candidate_snapshot_id != 0U)
		return Phase2RuntimeResult::CandidateBusy;
	if (m_pending_cause == Phase2RuntimeSnapshotCause::None)
		return Phase2RuntimeResult::NoChange;
	const auto manifest_id = m_manifest.staged_applied
		? m_manifest.staged_id : m_manifest.active_id;
	if (manifest_id == 0U)
		return Phase2RuntimeResult::ManifestRequired;
	if (m_next_snapshot_id == 0U)
		return Phase2RuntimeResult::CounterExhausted;
	plan.snapshot_id = m_next_snapshot_id;
	plan.required_manifest_id = manifest_id;
	plan.flags = snapshot_flags(m_pending_cause);
	plan.cause = m_pending_cause;
	return Phase2RuntimeResult::Applied;
}

Phase2RuntimeResult Phase2RuntimeSlot::on_snapshot_started(
	const Phase2RuntimeSnapshotPlan& plan) noexcept
{
	if (!m_configured || m_candidate_snapshot_id != 0U ||
		plan.snapshot_id != m_next_snapshot_id ||
		plan.required_manifest_id == 0U ||
		plan.flags != snapshot_flags(plan.cause))
		return Phase2RuntimeResult::InvalidInput;
	m_candidate_snapshot_id = plan.snapshot_id;
	m_candidate_manifest_id = plan.required_manifest_id;
	m_candidate_support_latches = m_support_latches;
	m_candidate_support_count = m_pending_support_count;
	for (std::size_t index = 0U;
		 index < m_candidate_support_count; ++index)
		m_candidate_support[index] = m_pending_support[index];
	m_candidate_cleanup_batch = m_cleanup_batch;
	m_candidate_lifecycle_count = m_pending_lifecycle_count;
	for (std::size_t index = 0U;
		 index < m_candidate_lifecycle_count; ++index)
		m_candidate_lifecycle[index] = m_pending_lifecycle[index];
	m_candidate_block_samples = m_current_block_samples;
	++m_next_snapshot_id;
	++m_started_snapshot_sequence;
	m_last_started_snapshot_cause = plan.cause;
	m_pending_cause = Phase2RuntimeSnapshotCause::None;
	return Phase2RuntimeResult::Applied;
}

Phase2RuntimeResult Phase2RuntimeSlot::on_snapshot_applied(
	std::uint32_t snapshot_id, std::uint32_t manifest_id) noexcept
{
	if (!m_configured || snapshot_id == 0U || manifest_id == 0U)
		return Phase2RuntimeResult::InvalidInput;
	if (snapshot_id == m_active_snapshot_id &&
		manifest_id == m_manifest.active_id)
		return Phase2RuntimeResult::NoChange;
	if (snapshot_id != m_candidate_snapshot_id ||
		manifest_id != m_candidate_manifest_id)
		return Phase2RuntimeResult::Stale;
	m_active_snapshot_id = snapshot_id;
	m_candidate_snapshot_id = 0U;
	m_candidate_manifest_id = 0U;
	for (std::size_t index = 0U;
		 index < m_candidate_cleanup_batch.count; ++index) {
		const auto signature =
			m_candidate_cleanup_batch.intents[index].object_signature;
		for (std::size_t current = 0U; current < m_cleanup_batch.count;) {
			if (m_cleanup_batch.intents[current].object_signature != signature) {
				++current;
				continue;
			}
			for (std::size_t move = current + 1U;
				 move < m_cleanup_batch.count; ++move)
				m_cleanup_batch.intents[move - 1U] =
					m_cleanup_batch.intents[move];
			m_cleanup_batch.intents[--m_cleanup_batch.count] = {};
		}
	}
	for (std::size_t entry = 0U;
		 entry < Phase2Wp07EpisodeLatches::EntriesPerSession; ++entry) {
		const auto fact = m_candidate_support_latches.value(
			m_session_slot, static_cast<std::uint32_t>(entry + 1U));
		if (fact.assisted_signature != 0U)
			(void)m_support_latches.on_applied(m_session_slot,
				fact.assisted_signature, fact.episode_sequence);
	}
	m_candidate_support_latches.reset();
	for (std::size_t candidate = 0U;
		 candidate < m_candidate_support_count; ++candidate) {
		const auto& fact = m_candidate_support[candidate];
		for (std::size_t current = 0U;
			 current < m_pending_support_count;) {
			const auto& pending = m_pending_support[current];
			if (pending.assisted_signature !=
					fact.assisted_signature ||
				pending.episode_sequence != fact.episode_sequence ||
				pending.reason != fact.reason ||
				pending.sample_time != fact.sample_time) {
				++current;
				continue;
			}
			for (std::size_t move = current + 1U;
				 move < m_pending_support_count; ++move)
				m_pending_support[move - 1U] =
					m_pending_support[move];
			m_pending_support[--m_pending_support_count] = {};
			break;
		}
	}
	m_candidate_support = {};
	m_candidate_support_count = 0U;
	m_candidate_cleanup_batch = {};
	if (m_candidate_lifecycle_count != 0U) {
		for (std::size_t candidate = 0U;
			 candidate < m_candidate_lifecycle_count; ++candidate) {
			const auto event_id =
				m_candidate_lifecycle[candidate].event_id;
			for (std::size_t current = 0U;
				 current < m_pending_lifecycle_count;) {
				if (m_pending_lifecycle[current].event_id != event_id) {
					++current;
					continue;
				}
				for (std::size_t move = current + 1U;
					 move < m_pending_lifecycle_count; ++move)
					m_pending_lifecycle[move - 1U] =
						m_pending_lifecycle[move];
				m_pending_lifecycle[--m_pending_lifecycle_count] = {};
			}
		}
	}
	m_candidate_lifecycle = {};
	m_candidate_lifecycle_count = 0U;
	m_active_block_samples = m_candidate_block_samples;
	m_candidate_block_samples = {};
	if (m_manifest.staged_applied &&
		manifest_id == m_manifest.staged_id) {
		m_manifest.active_id = m_manifest.staged_id;
		m_active_catalog = m_staged_catalog;
		m_manifest.staged_id = 0U;
		m_manifest.staged_applied = false;
		m_staged_catalog = {};
	}
	return Phase2RuntimeResult::Applied;
}

Phase2RuntimeResult Phase2RuntimeSlot::on_snapshot_abandoned(
	std::uint32_t snapshot_id) noexcept
{
	if (!m_configured || snapshot_id == 0U)
		return Phase2RuntimeResult::InvalidInput;
	if (snapshot_id != m_candidate_snapshot_id)
		return Phase2RuntimeResult::Stale;
	m_candidate_snapshot_id = 0U;
	m_candidate_manifest_id = 0U;
	m_candidate_support_latches.reset();
	m_candidate_support = {};
	m_candidate_support_count = 0U;
	m_candidate_cleanup_batch = {};
	m_candidate_lifecycle = {};
	m_candidate_lifecycle_count = 0U;
	m_candidate_block_samples = {};
	return request_snapshot(Phase2RuntimeSnapshotCause::Resync);
}

Phase2RuntimeResult Phase2RuntimeSlot::classify_delta_size(
	std::size_t encoded_size) noexcept
{
	if (!m_configured || m_active_snapshot_id == 0U)
		return Phase2RuntimeResult::InvalidInput;
	if (encoded_size > protocol::MaxStateMessageSize) {
		(void)request_snapshot(
			Phase2RuntimeSnapshotCause::DeltaCapacity);
		return Phase2RuntimeResult::SnapshotRequired;
	}
	return Phase2RuntimeResult::Applied;
}

bool Phase2RuntimeSlot::can_emit_delta() const noexcept
{
	return m_configured && m_active_snapshot_id != 0U &&
		m_candidate_snapshot_id == 0U &&
		m_pending_cause == Phase2RuntimeSnapshotCause::None &&
		m_manifest.active_id != 0U &&
		m_manifest.staged_id == 0U;
}

bool Phase2RuntimeSlot::consume_rebuild_intent() noexcept
{
	if (!m_configured || !m_manifest.rebuild_intent)
		return false;
	m_manifest.rebuild_intent = false;
	return true;
}

} // namespace telemetry::detail
