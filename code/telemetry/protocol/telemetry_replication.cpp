#include "telemetry/protocol/telemetry_replication.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace telemetry::protocol {

namespace {

constexpr std::uint64_t TransactionAssemblyTimeoutUs =
	static_cast<std::uint64_t>(TransactionAssemblyTimeoutMs) * 1000ULL;

bool is_known_lifecycle(StateRecordLifecycle lifecycle) noexcept
{
	return lifecycle == StateRecordLifecycle::UpsertOnly || lifecycle == StateRecordLifecycle::ExplicitCreateDelete;
}

bool is_known_mutation(StateMutationKind kind) noexcept
{
	return kind == StateMutationKind::Upsert || kind == StateMutationKind::Create || kind == StateMutationKind::Delete;
}

bool checked_add(std::size_t left, std::size_t right, std::size_t& result) noexcept
{
	if (right > std::numeric_limits<std::size_t>::max() - left) {
		return false;
	}
	result = left + right;
	return true;
}

bool checked_multiply(std::size_t left, std::size_t right, std::size_t& result) noexcept
{
	if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
		return false;
	}
	result = left * right;
	return true;
}

std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) noexcept
{
	return right > std::numeric_limits<std::uint64_t>::max() - left ? std::numeric_limits<std::uint64_t>::max()
																	: left + right;
}

bool structurally_valid_atom(const StateAtom& atom, std::size_t& encoded_size) noexcept
{
	const auto supported_record_version =
		atom.record_version == 1U ||
		((atom.record_version == 2U || atom.record_version == 3U) &&
		 (atom.key.record_type == static_cast<std::uint16_t>(RecordType::RadarContacts) ||
		  atom.key.record_type == static_cast<std::uint16_t>(RecordType::TargetState)));
	if (atom.key.record_type == 0 || !supported_record_version ||
		!is_known_lifecycle(atom.lifecycle) ||
		atom.key.identity.size() > std::numeric_limits<std::uint16_t>::max() ||
		atom.value.size() > std::numeric_limits<std::uint16_t>::max() || atom.key.identity.size() > atom.value.size() ||
		!std::equal(atom.key.identity.begin(), atom.key.identity.end(), atom.value.begin()) ||
		(atom.has_cascade_owner && (atom.cascade_owner.record_type == 0 || atom.cascade_owner == atom.key ||
									   atom.cascade_owner.identity.size() > atom.key.identity.size() ||
									   !std::equal(atom.cascade_owner.identity.begin(),
										   atom.cascade_owner.identity.end(),
										   atom.key.identity.begin()))) ||
		(!atom.has_cascade_owner && (atom.cascade_owner.record_type != 0 || !atom.cascade_owner.identity.empty()))) {
		return false;
	}
	return checked_add(RecordEnvelopeHeaderSize, atom.value.size(), encoded_size);
}

bool structurally_valid_delete_atom(const StateAtom& atom) noexcept
{
	const auto supported_record_version =
		atom.record_version == 1U ||
		((atom.record_version == 2U || atom.record_version == 3U) &&
		 (atom.key.record_type == static_cast<std::uint16_t>(RecordType::RadarContacts) ||
		  atom.key.record_type == static_cast<std::uint16_t>(RecordType::TargetState)));
	return atom.key.record_type != 0 && supported_record_version &&
		   atom.lifecycle == StateRecordLifecycle::ExplicitCreateDelete &&
		   atom.key.identity.size() <= std::numeric_limits<std::uint16_t>::max() && atom.value.empty() &&
		   !atom.has_cascade_owner && atom.cascade_owner.record_type == 0 && atom.cascade_owner.identity.empty();
}

StateAtom normalized_atom_copy(const StateAtom& atom)
{
	StateAtom normalized;
	normalized.key.record_type = atom.key.record_type;
	normalized.key.identity.assign(atom.key.identity.begin(), atom.key.identity.end());
	normalized.record_version = atom.record_version;
	normalized.lifecycle = atom.lifecycle;
	normalized.has_cascade_owner = atom.has_cascade_owner;
	normalized.cascade_owner.record_type = atom.cascade_owner.record_type;
	normalized.cascade_owner.identity.assign(atom.cascade_owner.identity.begin(), atom.cascade_owner.identity.end());
	normalized.value.assign(atom.value.begin(), atom.value.end());
	return normalized;
}

CumulativeStateDelta normalized_delta_copy(const CumulativeStateDelta& delta)
{
	CumulativeStateDelta normalized;
	normalized.baseline_snapshot_id = delta.baseline_snapshot_id;
	normalized.delta_sequence = delta.delta_sequence;
	normalized.producer_sample_time_us = delta.producer_sample_time_us;
	normalized.mutations.reserve(delta.mutation_count());
	for (std::size_t index = 0U; index < delta.mutation_count(); ++index) {
		const auto& mutation = delta.mutations[index];
		normalized.mutations.push_back({mutation.kind, normalized_atom_copy(mutation.atom)});
	}
	return normalized;
}

StateMutation make_delete_mutation(const StateAtom& atom)
{
	StateMutation mutation;
	mutation.kind = StateMutationKind::Delete;
	mutation.atom.key = atom.key;
	mutation.atom.record_version = atom.record_version;
	mutation.atom.lifecycle = StateRecordLifecycle::ExplicitCreateDelete;
	return mutation;
}

const StateAtom* find_atom(const StateImage& image, const StateAtomKey& key) noexcept
{
	const auto& records = image.records();
	const auto iterator =
		std::lower_bound(records.begin(), records.end(), key, [](const StateAtom& atom, const StateAtomKey& searched) {
			return atom.key < searched;
		});
	return iterator != records.end() && iterator->key == key ? &*iterator : nullptr;
}

bool is_implicitly_deleted_by_cascade(const StateAtom& atom, const StateImage& current) noexcept
{
	return atom.has_cascade_owner && find_atom(current, atom.cascade_owner) == nullptr;
}

const StateMutation* find_mutation(const CumulativeStateDelta& delta, const StateAtomKey& key) noexcept
{
	const auto logical_end = delta.mutations.begin() + static_cast<std::ptrdiff_t>(delta.mutation_count());
	const auto iterator = std::lower_bound(delta.mutations.begin(),
		logical_end,
		key,
		[](const StateMutation& mutation, const StateAtomKey& searched) { return mutation.atom.key < searched; });
	return iterator != logical_end && iterator->atom.key == key ? &*iterator : nullptr;
}

bool cascade_owner_is_deleted(const StateAtom& atom, const CumulativeStateDelta& delta) noexcept
{
	if (!atom.has_cascade_owner) {
		return false;
	}
	const auto* owner_mutation = find_mutation(delta, atom.cascade_owner);
	return owner_mutation != nullptr && owner_mutation->kind == StateMutationKind::Delete;
}

struct ApplyPlan {
	std::size_t record_count = 0;
	std::size_t encoded_size = 0;
	std::size_t retained_size = 0;
};

StateDeltaApplyResult append_planned_record(const StateAtom& atom, ApplyPlan& plan, std::vector<StateAtom>* records)
{
	std::size_t encoded_size = 0;
	std::size_t retained_size = 0;
	std::size_t atom_payload_size = 0;
	if (!checked_add(RecordEnvelopeHeaderSize, atom.value.size(), encoded_size) ||
		!checked_add(plan.encoded_size, encoded_size, encoded_size) || encoded_size > MaxTransactionSize ||
		!checked_add(atom.key.identity.size(), atom.value.size(), atom_payload_size) ||
		!checked_add(atom_payload_size, atom.cascade_owner.identity.size(), atom_payload_size) ||
		!checked_add(plan.retained_size, atom_payload_size, retained_size) ||
		!checked_add(retained_size, sizeof(StateAtom), retained_size) ||
		retained_size > MaxReplicationStateImageRetainedBytes || plan.record_count == MaxReplicationStateAtomCount) {
		return StateDeltaApplyResult::InvalidTransition;
	}
	if (records != nullptr) {
		records->push_back(atom);
	}
	++plan.record_count;
	plan.encoded_size = encoded_size;
	plan.retained_size = retained_size;
	return StateDeltaApplyResult::Applied;
}

StateDeltaApplyResult merge_cumulative_state_delta(const StateImage& baseline,
	const CumulativeStateDelta& delta,
	ApplyPlan& plan,
	std::vector<StateAtom>* records)
{
	const auto& baseline_records = baseline.records();
	std::size_t baseline_index = 0;
	std::size_t mutation_index = 0;
	while (baseline_index < baseline_records.size() || mutation_index < delta.mutation_count()) {
		const StateAtom* old_atom =
			baseline_index < baseline_records.size() ? &baseline_records[baseline_index] : nullptr;
		const StateMutation* mutation =
			mutation_index < delta.mutation_count() ? &delta.mutations[mutation_index] : nullptr;

		if (mutation == nullptr || (old_atom != nullptr && old_atom->key < mutation->atom.key)) {
			if (!cascade_owner_is_deleted(*old_atom, delta)) {
				if (const auto result = append_planned_record(*old_atom, plan, records);
					result != StateDeltaApplyResult::Applied) {
					return result;
				}
			}
			++baseline_index;
			continue;
		}

		if (old_atom == nullptr || mutation->atom.key < old_atom->key) {
			if (mutation->kind == StateMutationKind::Delete ||
				(mutation->kind == StateMutationKind::Upsert &&
					mutation->atom.lifecycle == StateRecordLifecycle::ExplicitCreateDelete) ||
				cascade_owner_is_deleted(mutation->atom, delta)) {
				return StateDeltaApplyResult::InvalidTransition;
			}
			if (const auto result = append_planned_record(mutation->atom, plan, records);
				result != StateDeltaApplyResult::Applied) {
				return result;
			}
			++mutation_index;
			continue;
		}

		switch (mutation->kind) {
		case StateMutationKind::Upsert:
			if (old_atom->record_version != mutation->atom.record_version ||
				old_atom->lifecycle != mutation->atom.lifecycle ||
				old_atom->has_cascade_owner != mutation->atom.has_cascade_owner ||
				old_atom->cascade_owner != mutation->atom.cascade_owner ||
				cascade_owner_is_deleted(mutation->atom, delta)) {
				return StateDeltaApplyResult::InvalidTransition;
			}
			if (const auto result = append_planned_record(mutation->atom, plan, records);
				result != StateDeltaApplyResult::Applied) {
				return result;
			}
			break;
		case StateMutationKind::Create:
			return StateDeltaApplyResult::InvalidTransition;
		case StateMutationKind::Delete:
			if (old_atom->lifecycle != StateRecordLifecycle::ExplicitCreateDelete ||
				old_atom->record_version != mutation->atom.record_version) {
				return StateDeltaApplyResult::InvalidTransition;
			}
			break;
		}
		++baseline_index;
		++mutation_index;
	}
	return StateDeltaApplyResult::Applied;
}

struct DeltaPlan {
	std::size_t mutation_count = 0;
	std::size_t encoded_size = DeltaPayloadPrefixSize;
	std::size_t retained_size = 0;
};

ProducerBaselineResult plan_mutation(const StateAtom& atom, bool deletion, DeltaPlan& plan) noexcept
{
	const auto payload_size = deletion ? atom.key.identity.size() : atom.value.size();
	std::size_t record_size = 0;
	std::size_t total_size = 0;
	std::size_t retained_size = plan.retained_size;
	if (plan.mutation_count == std::numeric_limits<std::uint16_t>::max() ||
		!checked_add(RecordEnvelopeHeaderSize, payload_size, record_size) ||
		!checked_add(plan.encoded_size, record_size, total_size) || total_size > MaxStateMessageSize ||
		!checked_add(retained_size, sizeof(StateMutation), retained_size) ||
		!checked_add(retained_size, atom.key.identity.size(), retained_size) ||
		(!deletion && !checked_add(retained_size, atom.value.size(), retained_size)) ||
		(!deletion && atom.has_cascade_owner &&
			!checked_add(retained_size, atom.cascade_owner.identity.size(), retained_size)) ||
		retained_size > MaxReplicationDeltaRetainedBytes) {
		return ProducerBaselineResult::KeyframeRequired;
	}
	++plan.mutation_count;
	plan.encoded_size = total_size;
	plan.retained_size = retained_size;
	return ProducerBaselineResult::Applied;
}

ProducerBaselineResult analyze_delta(const StateImage& baseline, const StateImage& current, DeltaPlan& plan) noexcept
{
	const auto& before = baseline.records();
	const auto& after = current.records();
	std::size_t before_index = 0;
	std::size_t after_index = 0;
	while (before_index < before.size() || after_index < after.size()) {
		if (before_index == before.size()) {
			const auto& added = after[after_index++];
			if (const auto result = plan_mutation(added, false, plan); result != ProducerBaselineResult::Applied) {
				return result;
			}
			continue;
		}
		if (after_index == after.size()) {
			const auto& removed = before[before_index++];
			if (is_implicitly_deleted_by_cascade(removed, current)) {
				continue;
			}
			if (removed.lifecycle != StateRecordLifecycle::ExplicitCreateDelete) {
				return ProducerBaselineResult::KeyframeRequired;
			}
			if (const auto result = plan_mutation(removed, true, plan); result != ProducerBaselineResult::Applied) {
				return result;
			}
			continue;
		}

		const auto& old_atom = before[before_index];
		const auto& new_atom = after[after_index];
		if (old_atom.key < new_atom.key) {
			if (is_implicitly_deleted_by_cascade(old_atom, current)) {
				++before_index;
				continue;
			}
			if (old_atom.lifecycle != StateRecordLifecycle::ExplicitCreateDelete) {
				return ProducerBaselineResult::KeyframeRequired;
			}
			if (const auto result = plan_mutation(old_atom, true, plan); result != ProducerBaselineResult::Applied) {
				return result;
			}
			++before_index;
		} else if (new_atom.key < old_atom.key) {
			if (const auto result = plan_mutation(new_atom, false, plan); result != ProducerBaselineResult::Applied) {
				return result;
			}
			++after_index;
		} else {
			if (old_atom.record_version != new_atom.record_version || old_atom.lifecycle != new_atom.lifecycle ||
				old_atom.has_cascade_owner != new_atom.has_cascade_owner ||
				old_atom.cascade_owner != new_atom.cascade_owner) {
				return ProducerBaselineResult::KeyframeRequired;
			}
			if (old_atom.value != new_atom.value) {
				if (const auto result = plan_mutation(new_atom, false, plan);
					result != ProducerBaselineResult::Applied) {
					return result;
				}
			}
			++before_index;
			++after_index;
		}
	}
	return plan.mutation_count == 0 ? ProducerBaselineResult::NoChange : ProducerBaselineResult::Applied;
}

ProducerBaselineResult analyze_delta_indices(const StateImage& baseline,
	const StateImage& current,
	const std::uint16_t* dirty_indices,
	std::size_t dirty_index_count,
	DeltaPlan& plan) noexcept
{
	const auto& before = baseline.records();
	const auto& after = current.records();
	if (before.size() != after.size() ||
		(dirty_index_count != 0U && dirty_indices == nullptr)) {
		return ProducerBaselineResult::KeyframeRequired;
	}
	std::uint16_t previous = 0U;
	for (std::size_t dirty = 0U; dirty < dirty_index_count; ++dirty) {
		const auto index = static_cast<std::size_t>(dirty_indices[dirty]);
		if (index >= before.size() ||
			(dirty != 0U && dirty_indices[dirty] <= previous)) {
			return ProducerBaselineResult::InvalidArgument;
		}
		previous = dirty_indices[dirty];
		const auto& old_atom = before[index];
		const auto& new_atom = after[index];
		if (old_atom.key != new_atom.key ||
			old_atom.record_version != new_atom.record_version ||
			old_atom.lifecycle != new_atom.lifecycle ||
			old_atom.has_cascade_owner != new_atom.has_cascade_owner ||
			old_atom.cascade_owner != new_atom.cascade_owner) {
			return ProducerBaselineResult::KeyframeRequired;
		}
		if (old_atom.value != new_atom.value) {
			if (const auto result = plan_mutation(new_atom, false, plan);
				result != ProducerBaselineResult::Applied) {
				return result;
			}
		}
	}
	return plan.mutation_count == 0 ? ProducerBaselineResult::NoChange
									: ProducerBaselineResult::Applied;
}

std::size_t dirty_record_count(const StateImage& baseline, const StateImage& current) noexcept
{
	const auto& before = baseline.records();
	const auto& after = current.records();
	std::size_t before_index = 0;
	std::size_t after_index = 0;
	std::size_t count = 0;
	while (before_index < before.size() || after_index < after.size()) {
		if (before_index == before.size()) {
			count += after.size() - after_index;
			break;
		}
		if (after_index == after.size()) {
			count += before.size() - before_index;
			break;
		}
		const auto& old_atom = before[before_index];
		const auto& new_atom = after[after_index];
		if (old_atom.key < new_atom.key) {
			++count;
			++before_index;
		} else if (new_atom.key < old_atom.key) {
			++count;
			++after_index;
		} else {
			if (old_atom != new_atom) {
				++count;
			}
			++before_index;
			++after_index;
		}
	}
	return count;
}

std::size_t dirty_record_count_indices(const StateImage& baseline,
	const StateImage& current,
	const std::uint16_t* dirty_indices,
	std::size_t dirty_index_count) noexcept
{
	DeltaPlan plan;
	return analyze_delta_indices(
			   baseline, current, dirty_indices, dirty_index_count, plan) ==
			ProducerBaselineResult::Applied
		? plan.mutation_count
		: 0U;
}

ProducerBaselineResult build_delta(const StateImage& baseline,
	const StateImage& current,
	std::uint32_t baseline_snapshot_id,
	std::uint32_t delta_sequence,
	std::uint64_t producer_sample_time_us,
	CumulativeStateDelta& delta,
	const std::uint16_t* dirty_indices = nullptr,
	std::size_t dirty_index_count = 0U,
	bool use_dirty_indices = false,
	DeltaBuildChanges* changes = nullptr) noexcept
{
	DeltaPlan plan;
	const auto analyzed = use_dirty_indices
		? analyze_delta_indices(
			  baseline, current, dirty_indices, dirty_index_count, plan)
		: analyze_delta(baseline, current, plan);
	if (analyzed != ProducerBaselineResult::Applied) {
		return analyzed;
	}
	const auto previous_mutation_count =
		delta.active_mutation_count;
	if (changes != nullptr) {
		*changes = {};
		changes->layout_changed =
			!use_dirty_indices ||
			previous_mutation_count != plan.mutation_count;
	}
	try {
		// analyze_delta proves this reservation and all copied record payloads
		// fit the one-MiB Delta gate before any proportional output allocation.
		if (delta.mutations.capacity() < plan.mutation_count) {
			delta.mutations.reserve(plan.mutation_count);
		}
		if (delta.mutations.size() < plan.mutation_count) {
			delta.mutations.resize(plan.mutation_count);
		}
	} catch (const std::bad_alloc&) {
		return ProducerBaselineResult::AllocationFailed;
	}
	delta.baseline_snapshot_id = baseline_snapshot_id;
	delta.delta_sequence = delta_sequence;
	delta.producer_sample_time_us = producer_sample_time_us;
	delta.active_mutation_count = plan.mutation_count;
	std::size_t mutation_index = 0U;
	auto retain_preallocated_backing =
		[&delta](std::size_t target_index,
			const StateAtom& atom,
			bool copy_value) noexcept {
			auto& target = delta.mutations[target_index].atom;
			const auto fits = [&atom, copy_value](
				const StateAtom& candidate) noexcept {
				return candidate.key.record_type ==
						atom.key.record_type &&
					candidate.key.identity.capacity() >=
						atom.key.identity.size() &&
					(!copy_value ||
					 candidate.value.capacity() >=
						atom.value.size()) &&
					candidate.cascade_owner.identity.capacity() >=
						atom.cascade_owner.identity.size();
			};
			if (fits(target)) return;
			for (std::size_t candidate_index = target_index + 1U;
				 candidate_index < delta.mutations.size();
				 ++candidate_index) {
				auto& candidate =
					delta.mutations[candidate_index].atom;
				if (!fits(candidate)) continue;
				std::swap(target, candidate);
				return;
			}
		};
	auto append_mutation = [&delta, &mutation_index,
							   &retain_preallocated_backing,
							   previous_mutation_count,
							   changes](
		StateMutationKind kind, const StateAtom& atom) noexcept {
		if (mutation_index >= delta.mutation_count()) {
			return false;
		}
		const auto target_index = mutation_index++;
		const auto& existing =
			delta.mutations[target_index];
		const auto existing_active =
			target_index < previous_mutation_count;
		if (existing_active &&
			existing.kind == kind &&
			existing.atom == atom)
			return true;
		if (changes != nullptr) {
			const auto stable_layout =
				existing_active &&
				existing.kind == kind &&
				existing.atom.key == atom.key &&
				existing.atom.record_version ==
					atom.record_version &&
				existing.atom.lifecycle == atom.lifecycle &&
				existing.atom.has_cascade_owner ==
					atom.has_cascade_owner &&
				existing.atom.cascade_owner ==
					atom.cascade_owner &&
				existing.atom.value.size() ==
					atom.value.size();
			if (!stable_layout)
				changes->layout_changed = true;
			if (!changes->layout_changed) {
				if (changes->count ==
					changes->mutation_indices.size())
					changes->layout_changed = true;
				else
					changes->mutation_indices[
						changes->count++] =
						static_cast<std::uint16_t>(
							target_index);
			}
		}
		retain_preallocated_backing(target_index, atom, true);
		auto& target = delta.mutations[target_index];
		target.kind = kind;
		target.atom = atom;
		return true;
	};
	auto append_delete = [&delta, &mutation_index,
							 &retain_preallocated_backing](
		const StateAtom& atom) noexcept {
		if (mutation_index >= delta.mutation_count()) {
			return false;
		}
		const auto target_index = mutation_index++;
		retain_preallocated_backing(target_index, atom, false);
		auto& target = delta.mutations[target_index];
		target.kind = StateMutationKind::Delete;
		target.atom.key = atom.key;
		target.atom.record_version = atom.record_version;
		target.atom.lifecycle = StateRecordLifecycle::ExplicitCreateDelete;
		target.atom.value.clear();
		target.atom.has_cascade_owner = false;
		target.atom.cascade_owner = {};
		return true;
	};

	const auto& before = baseline.records();
	const auto& after = current.records();
	if (use_dirty_indices) {
		try {
			for (std::size_t dirty = 0U; dirty < dirty_index_count;
				 ++dirty) {
				const auto index =
					static_cast<std::size_t>(dirty_indices[dirty]);
				if (before[index].value == after[index].value)
					continue;
				if (!append_mutation(
						StateMutationKind::Upsert, after[index]))
					return ProducerBaselineResult::AllocationFailed;
			}
		} catch (const std::bad_alloc&) {
			return ProducerBaselineResult::AllocationFailed;
		}
		if (mutation_index != delta.mutation_count())
			return ProducerBaselineResult::AllocationFailed;
		// analyze_delta_indices already proves canonical index order, stable
		// keys/versions/lifecycle/cascade ownership, exact payload and retained
		// bounds, and that every emitted mutation is an Upsert copied from a
		// validated StateImage. The incremental image builders also validate
		// each rebuilt business record before publishing it. Re-running the
		// generic wire-delta validator here would therefore rescan and decode
		// every dirty atom without adding an invariant. Generic/non-indexed
		// callers retain the full validation below.
		if (changes != nullptr && changes->layout_changed)
			changes->count = 0U;
		return ProducerBaselineResult::Applied;
	}
	std::size_t before_index = 0;
	std::size_t after_index = 0;
	try {
		while (before_index < before.size() || after_index < after.size()) {
			if (before_index == before.size()) {
				const auto& atom = after[after_index++];
				if (!append_mutation(atom.lifecycle == StateRecordLifecycle::ExplicitCreateDelete ? StateMutationKind::Create
																												: StateMutationKind::Upsert, atom)) return ProducerBaselineResult::AllocationFailed;
				continue;
			}
			if (after_index == after.size()) {
				const auto& atom = before[before_index++];
				if (is_implicitly_deleted_by_cascade(atom, current)) {
					continue;
				}
				if (atom.lifecycle != StateRecordLifecycle::ExplicitCreateDelete) {
					return ProducerBaselineResult::KeyframeRequired;
				}
				if (!append_delete(atom)) return ProducerBaselineResult::AllocationFailed;
				continue;
			}

			const auto& old_atom = before[before_index];
			const auto& new_atom = after[after_index];
			if (old_atom.key < new_atom.key) {
				if (is_implicitly_deleted_by_cascade(old_atom, current)) {
					++before_index;
					continue;
				}
				if (old_atom.lifecycle != StateRecordLifecycle::ExplicitCreateDelete) {
					return ProducerBaselineResult::KeyframeRequired;
				}
				if (!append_delete(old_atom)) return ProducerBaselineResult::AllocationFailed;
				++before_index;
			} else if (new_atom.key < old_atom.key) {
				if (!append_mutation(new_atom.lifecycle == StateRecordLifecycle::ExplicitCreateDelete ? StateMutationKind::Create
																														 : StateMutationKind::Upsert, new_atom)) return ProducerBaselineResult::AllocationFailed;
				++after_index;
			} else {
				if (old_atom.record_version != new_atom.record_version || old_atom.lifecycle != new_atom.lifecycle ||
					old_atom.has_cascade_owner != new_atom.has_cascade_owner ||
					old_atom.cascade_owner != new_atom.cascade_owner) {
					return ProducerBaselineResult::KeyframeRequired;
				}
				if (old_atom.value != new_atom.value) {
					if (!append_mutation(StateMutationKind::Upsert, new_atom)) return ProducerBaselineResult::AllocationFailed;
				}
				++before_index;
				++after_index;
			}
		}
	} catch (const std::bad_alloc&) {
		return ProducerBaselineResult::AllocationFailed;
	}

	if (validate_cumulative_state_delta(delta) != StateDeltaValidationResult::Valid) {
		return delta.encoded_size() > MaxStateMessageSize ? ProducerBaselineResult::KeyframeRequired
																													 : ProducerBaselineResult::InvalidArgument;
	}
	if (changes != nullptr) {
		changes->layout_changed = true;
		changes->count = 0U;
	}
	return ProducerBaselineResult::Applied;
}

ClientDeltaResult map_delta_resync_result(ClientResyncResult result) noexcept
{
	switch (result) {
	case ClientResyncResult::NotRequested:
		return ClientDeltaResult::ResyncUnavailable;
	case ClientResyncResult::Requested:
		return ClientDeltaResult::ResyncRequested;
	case ClientResyncResult::RateLimited:
		return ClientDeltaResult::ResyncRateLimited;
	case ClientResyncResult::Unavailable:
		return ClientDeltaResult::ResyncUnavailable;
	}
	return ClientDeltaResult::ResyncUnavailable;
}

bool merge_dirty_indices(
	const std::uint16_t* existing,
	std::size_t existing_count,
	const std::uint16_t* incoming,
	std::size_t incoming_count,
	std::uint16_t* output,
	std::size_t capacity,
	std::size_t& output_count) noexcept
{
	if (existing == nullptr || output == nullptr ||
		existing_count > capacity ||
		incoming_count > capacity ||
		(incoming_count != 0U && incoming == nullptr))
		return false;
	std::size_t left = 0U;
	std::size_t right = 0U;
	output_count = 0U;
	while (left < existing_count || right < incoming_count) {
		const auto value =
			right == incoming_count ||
					(left < existing_count &&
					 existing[left] < incoming[right])
				? existing[left++]
				: incoming[right++];
		if (output_count != 0U &&
			output[output_count - 1U] == value)
			continue;
		if (output_count == capacity)
			return false;
		output[output_count++] = value;
	}
	return true;
}

} // namespace

bool operator==(const StateAtomKey& left, const StateAtomKey& right) noexcept
{
	return left.record_type == right.record_type && left.identity == right.identity;
}

bool operator<(const StateAtomKey& left, const StateAtomKey& right) noexcept
{
	return left.record_type < right.record_type ||
		   (left.record_type == right.record_type && left.identity < right.identity);
}

bool operator==(const StateAtom& left, const StateAtom& right) noexcept
{
	return left.key == right.key && left.record_version == right.record_version && left.lifecycle == right.lifecycle &&
		   left.has_cascade_owner == right.has_cascade_owner && left.cascade_owner == right.cascade_owner &&
		   left.value == right.value;
}

StateImageResult StateImage::create(std::vector<StateAtom> records, StateImage& image) noexcept
{
	StateImageInvalidRecordReason invalid_record_reason = StateImageInvalidRecordReason::None;
	return create(std::move(records), image, invalid_record_reason);
}

StateImageResult StateImage::create(std::vector<StateAtom> records,
	StateImage& image,
	StateImageInvalidRecordReason& invalid_record_reason) noexcept
{
	invalid_record_reason = StateImageInvalidRecordReason::None;
	std::size_t total_size = 0;
	std::size_t retained_size = 0;
	if (!checked_multiply(records.size(), sizeof(StateAtom), retained_size) ||
		retained_size > MaxReplicationStateImageRetainedBytes) {
		return StateImageResult::SizeLimitExceeded;
	}
	for (const auto& record : records) {
		std::size_t record_size = 0;
		if (!structurally_valid_atom(record, record_size)) {
			invalid_record_reason = StateImageInvalidRecordReason::MalformedAtom;
			return StateImageResult::InvalidRecord;
		}
		if (!checked_add(total_size, record_size, total_size) || total_size > MaxTransactionSize) {
			return StateImageResult::SizeLimitExceeded;
		}
		std::size_t record_retained_size = 0;
		if (!checked_add(record.value.size(), record.key.identity.size(), record_retained_size) ||
			!checked_add(record_retained_size, record.cascade_owner.identity.size(), record_retained_size) ||
			!checked_add(retained_size, record_retained_size, retained_size) ||
			retained_size > MaxReplicationStateImageRetainedBytes) {
			return StateImageResult::SizeLimitExceeded;
		}
	}

	std::sort(records.begin(), records.end(), [](const StateAtom& left, const StateAtom& right) {
		return left.key < right.key;
	});
	for (std::size_t index = 1; index < records.size(); ++index) {
		if (records[index - 1].key == records[index].key) {
			return StateImageResult::DuplicateKey;
		}
	}
	for (const auto& record : records) {
		if (!record.has_cascade_owner) {
			continue;
		}
		const auto owner = std::lower_bound(records.begin(),
			records.end(),
			record.cascade_owner,
			[](const StateAtom& atom, const StateAtomKey& key) { return atom.key < key; });
		if (owner == records.end() || owner->key != record.cascade_owner) {
			invalid_record_reason = StateImageInvalidRecordReason::MissingCascadeOwner;
			return StateImageResult::InvalidRecord;
		}
		if (owner->lifecycle != StateRecordLifecycle::ExplicitCreateDelete || owner->has_cascade_owner) {
			invalid_record_reason = StateImageInvalidRecordReason::InvalidCascadeOwner;
			return StateImageResult::InvalidRecord;
		}
	}

	std::vector<StateAtom> normalized_records;
	try {
		// Rebuild from logical sizes so hostile spare capacities supplied by a
		// caller are not retained in the immutable image. The complete logical
		// quota above is validated before this proportional normalization.
		normalized_records.reserve(records.size());
		for (const auto& record : records) {
			normalized_records.push_back(normalized_atom_copy(record));
		}
	} catch (const std::bad_alloc&) {
		return StateImageResult::AllocationFailed;
	}

	StateImage candidate;
	try {
		candidate.m_records = std::make_shared<const std::vector<StateAtom>>(std::move(normalized_records));
	} catch (const std::bad_alloc&) {
		return StateImageResult::AllocationFailed;
	}
	candidate.m_encoded_snapshot_records_size = total_size;
	candidate.m_retained_payload_bytes = retained_size;
	image = std::move(candidate);
	return StateImageResult::Created;
}

StateImageResult StateImage::adopt_preallocated(const std::shared_ptr<const std::vector<StateAtom>>& records,
	StateImage& image,
	StateImageInvalidRecordReason& invalid_record_reason) noexcept
{
	invalid_record_reason = StateImageInvalidRecordReason::None;
	if (!records) {
		return StateImageResult::InvalidRecord;
	}
	std::size_t total_size = 0U;
	std::size_t retained_size = 0U;
	if (!checked_multiply(records->size(), sizeof(StateAtom), retained_size) ||
		retained_size > MaxReplicationStateImageRetainedBytes) {
		return StateImageResult::SizeLimitExceeded;
	}
	for (std::size_t index = 0U; index < records->size(); ++index) {
		const auto& record = (*records)[index];
		std::size_t record_size = 0U;
		if (!structurally_valid_atom(record, record_size)) {
			invalid_record_reason = StateImageInvalidRecordReason::MalformedAtom;
			return StateImageResult::InvalidRecord;
		}
		if ((index != 0U && !((*records)[index - 1U].key < record.key)) ||
			!checked_add(total_size, record_size, total_size) || total_size > MaxTransactionSize) {
			return index != 0U && (*records)[index - 1U].key == record.key ? StateImageResult::DuplicateKey
																									 : StateImageResult::InvalidRecord;
		}
		std::size_t record_retained_size = 0U;
		if (!checked_add(record.value.size(), record.key.identity.size(), record_retained_size) ||
			!checked_add(record_retained_size, record.cascade_owner.identity.size(), record_retained_size) ||
			!checked_add(retained_size, record_retained_size, retained_size) ||
			retained_size > MaxReplicationStateImageRetainedBytes) {
			return StateImageResult::SizeLimitExceeded;
		}
	}
	for (const auto& record : *records) {
		if (!record.has_cascade_owner) continue;
		const auto owner = std::lower_bound(records->begin(), records->end(), record.cascade_owner,
			[](const StateAtom& atom, const StateAtomKey& key) { return atom.key < key; });
		if (owner == records->end() || owner->key != record.cascade_owner) {
			invalid_record_reason = StateImageInvalidRecordReason::MissingCascadeOwner;
			return StateImageResult::InvalidRecord;
		}
		if (owner->lifecycle != StateRecordLifecycle::ExplicitCreateDelete || owner->has_cascade_owner) {
			invalid_record_reason = StateImageInvalidRecordReason::InvalidCascadeOwner;
			return StateImageResult::InvalidRecord;
		}
	}
	StateImage candidate;
	candidate.m_records = records;
	candidate.m_encoded_snapshot_records_size = total_size;
	candidate.m_retained_payload_bytes = retained_size;
	candidate.m_preallocated_mutable_backing = true;
	image = std::move(candidate);
	return StateImageResult::Created;
}

const std::vector<StateAtom>& StateImage::records() const noexcept
{
	static const std::vector<StateAtom> EmptyRecords;
	return m_records == nullptr ? EmptyRecords : *m_records;
}

std::vector<StateAtom>*
StateImage::mutable_preallocated_records_if_unique() noexcept
{
	if (!m_preallocated_mutable_backing || !m_records ||
		m_records.use_count() != 2L)
		return nullptr;
	return const_cast<std::vector<StateAtom>*>(m_records.get());
}

StateImageResult StateImage::refresh_preallocated_metadata(
	StateImageInvalidRecordReason& invalid_record_reason) noexcept
{
	if (!m_preallocated_mutable_backing || !m_records)
		return StateImageResult::InvalidRecord;
	StateImage refreshed;
	const auto result =
		adopt_preallocated(m_records, refreshed, invalid_record_reason);
	if (result == StateImageResult::Created)
		*this = std::move(refreshed);
	return result;
}

StateImageResult StateImage::refresh_preallocated_metadata_incremental(
	const std::vector<StateAtom>& previous_records,
	const std::uint16_t* canonical_indices,
	const std::uint16_t* previous_record_indices,
	std::size_t canonical_index_count,
	StateImageInvalidRecordReason& invalid_record_reason) noexcept
{
	invalid_record_reason = StateImageInvalidRecordReason::None;
	if (!m_preallocated_mutable_backing || !m_records ||
		previous_records.size() != m_records->size() ||
		(canonical_index_count != 0U &&
			(canonical_indices == nullptr ||
			 previous_record_indices == nullptr)))
		return StateImageResult::InvalidRecord;
	auto encoded_size = m_encoded_snapshot_records_size;
	auto retained_size = m_retained_payload_bytes;
	std::size_t previous_index = 0U;
	for (std::size_t dirty = 0U; dirty < canonical_index_count; ++dirty) {
		const auto index =
			static_cast<std::size_t>(canonical_indices[dirty]);
		if (index >= m_records->size() ||
			(dirty != 0U && index <= previous_index))
			return StateImageResult::InvalidRecord;
		previous_index = index;
		const auto previous_record_index =
			static_cast<std::size_t>(
				previous_record_indices[dirty]);
		if (previous_record_index >= previous_records.size())
			return StateImageResult::InvalidRecord;
		const auto& old_record =
			previous_records[previous_record_index];
		const auto& new_record = (*m_records)[index];
		if (old_record.key != new_record.key ||
			old_record.record_version != new_record.record_version ||
			old_record.lifecycle != new_record.lifecycle ||
			old_record.has_cascade_owner !=
				new_record.has_cascade_owner ||
			old_record.cascade_owner != new_record.cascade_owner) {
			invalid_record_reason =
				StateImageInvalidRecordReason::MalformedAtom;
			return StateImageResult::InvalidRecord;
		}
		std::size_t old_encoded = 0U;
		std::size_t new_encoded = 0U;
		if (!structurally_valid_atom(old_record, old_encoded) ||
			!structurally_valid_atom(new_record, new_encoded)) {
			invalid_record_reason =
				StateImageInvalidRecordReason::MalformedAtom;
			return StateImageResult::InvalidRecord;
		}
		std::size_t old_retained = 0U;
		std::size_t new_retained = 0U;
		if (!checked_add(old_record.value.size(),
				old_record.key.identity.size(), old_retained) ||
			!checked_add(old_retained,
				old_record.cascade_owner.identity.size(),
				old_retained) ||
			!checked_add(new_record.value.size(),
				new_record.key.identity.size(), new_retained) ||
			!checked_add(new_retained,
				new_record.cascade_owner.identity.size(),
				new_retained) ||
			encoded_size < old_encoded ||
			retained_size < old_retained) {
			return StateImageResult::SizeLimitExceeded;
		}
		encoded_size -= old_encoded;
		retained_size -= old_retained;
		if (!checked_add(encoded_size, new_encoded, encoded_size) ||
			encoded_size > MaxTransactionSize ||
			!checked_add(retained_size, new_retained, retained_size) ||
			retained_size > MaxReplicationStateImageRetainedBytes)
			return StateImageResult::SizeLimitExceeded;
	}
	m_encoded_snapshot_records_size = encoded_size;
	m_retained_payload_bytes = retained_size;
	return StateImageResult::Created;
}

bool operator==(const StateImage& left, const StateImage& right) noexcept
{
	return left.records() == right.records();
}

bool operator==(const StateMutation& left, const StateMutation& right) noexcept
{
	return left.kind == right.kind && left.atom == right.atom;
}

std::size_t CumulativeStateDelta::encoded_size() const noexcept
{
	std::size_t total = DeltaPayloadPrefixSize;
	for (std::size_t index = 0U; index < mutation_count(); ++index) {
		const auto& mutation = mutations[index];
		const auto payload_size =
			mutation.kind == StateMutationKind::Delete ? mutation.atom.key.identity.size() : mutation.atom.value.size();
		std::size_t record_size = 0;
		if (!checked_add(RecordEnvelopeHeaderSize, payload_size, record_size) ||
			!checked_add(total, record_size, total)) {
			return std::numeric_limits<std::size_t>::max();
		}
	}
	return total;
}

bool operator==(const CumulativeStateDelta& left, const CumulativeStateDelta& right) noexcept
{
	if (left.baseline_snapshot_id != right.baseline_snapshot_id || left.delta_sequence != right.delta_sequence ||
		left.producer_sample_time_us != right.producer_sample_time_us || left.mutation_count() != right.mutation_count()) {
		return false;
	}
	for (std::size_t index = 0U; index < left.mutation_count(); ++index) {
		if (!(left.mutations[index] == right.mutations[index])) return false;
	}
	return true;
}

namespace {

bool same_resync_request(const ResyncRequestPayload& left, const ResyncRequestPayload& right) noexcept
{
	return left.request_id == right.request_id && left.reason == right.reason &&
		   left.request_flags == right.request_flags &&
		   left.last_applied_snapshot_id == right.last_applied_snapshot_id &&
		   left.last_applied_delta_sequence == right.last_applied_delta_sequence &&
		   left.client_send_time_us == right.client_send_time_us;
}

} // namespace

ProducerResyncResult ProducerResyncTracker::accept(const ResyncRequestPayload& request, std::uint64_t now_us) noexcept
{
	if (validate_resync_request_payload(request) != ValidationError::None) {
		return ProducerResyncResult::Invalid;
	}
	expire(now_us);

	DeduplicationEntry* free_entry = nullptr;
	for (auto& entry : m_deduplication_entries) {
		if (!entry.occupied) {
			if (free_entry == nullptr) {
				free_entry = &entry;
			}
			continue;
		}
		if (entry.request.request_id != request.request_id) {
			continue;
		}
		if (!same_resync_request(request, entry.request)) {
			return ProducerResyncResult::Invalid;
		}
		return m_has_candidate ? ProducerResyncResult::AcceptedDuplicate : ProducerResyncResult::Stale;
	}
	if (free_entry == nullptr) {
		return ProducerResyncResult::ResourceLimit;
	}

	free_entry->request = request;
	free_entry->deadline_us = saturating_add(now_us, ResyncRequestDeduplicationWindowUs);
	free_entry->occupied = true;
	++m_deduplication_entry_count;
	if (m_has_candidate) {
		return ProducerResyncResult::AcceptedCoalesced;
	}

	m_candidate_request = request;
	m_candidate_deadline_us = saturating_add(now_us, TransactionAssemblyTimeoutUs);
	m_has_candidate = true;
	return ProducerResyncResult::AcceptedNewCandidate;
}

bool ProducerResyncTracker::is_known_duplicate(const ResyncRequestPayload& request) const noexcept
{
	if (validate_resync_request_payload(request) != ValidationError::None) {
		return false;
	}
	for (const auto& entry : m_deduplication_entries) {
		if (entry.occupied && entry.request.request_id == request.request_id) {
			return same_resync_request(request, entry.request);
		}
	}
	return false;
}

ProducerResyncResult ProducerResyncTracker::expire(std::uint64_t now_us) noexcept
{
	expire_deduplication_entries(now_us);
	if (!m_has_candidate || now_us < m_candidate_deadline_us) {
		return ProducerResyncResult::NoChange;
	}
	m_has_candidate = false;
	m_candidate_deadline_us = 0;
	return ProducerResyncResult::Expired;
}

ProducerResyncResult ProducerResyncTracker::complete() noexcept
{
	if (!m_has_candidate) {
		return ProducerResyncResult::NoChange;
	}
	m_has_candidate = false;
	m_candidate_deadline_us = 0;
	return ProducerResyncResult::Completed;
}

void ProducerResyncTracker::clear() noexcept
{
	m_candidate_request = ResyncRequestPayload{};
	m_deduplication_entries = {};
	m_candidate_deadline_us = 0;
	m_deduplication_entry_count = 0;
	m_has_candidate = false;
}

void ProducerResyncTracker::expire_deduplication_entries(std::uint64_t now_us) noexcept
{
	for (auto& entry : m_deduplication_entries) {
		if (!entry.occupied || now_us < entry.deadline_us) {
			continue;
		}
		entry = DeduplicationEntry{};
		--m_deduplication_entry_count;
	}
}

StateDeltaValidationResult validate_cumulative_state_delta(const CumulativeStateDelta& delta) noexcept
{
	if (delta.baseline_snapshot_id == 0 || delta.delta_sequence == 0 || delta.mutation_count() == 0U ||
		delta.mutation_count() > std::numeric_limits<std::uint16_t>::max()) {
		return StateDeltaValidationResult::InvalidIdentity;
	}
	std::size_t retained_size = 0;
	if (!checked_multiply(delta.mutation_count(), sizeof(StateMutation), retained_size) ||
		retained_size > MaxReplicationDeltaRetainedBytes) {
		return StateDeltaValidationResult::SizeLimitExceeded;
	}

	const StateAtomKey* previous_key = nullptr;
	for (std::size_t index = 0U; index < delta.mutation_count(); ++index) {
		const auto& mutation = delta.mutations[index];
		std::size_t ignored = 0;
		if (!is_known_mutation(mutation.kind) ||
			(mutation.kind == StateMutationKind::Delete ? !structurally_valid_delete_atom(mutation.atom)
														: !structurally_valid_atom(mutation.atom, ignored))) {
			return StateDeltaValidationResult::InvalidMutation;
		}
		std::size_t mutation_bytes = 0;
		if (!checked_add(mutation.atom.key.identity.size(), mutation.atom.value.size(), mutation_bytes) ||
			!checked_add(mutation_bytes, mutation.atom.cascade_owner.identity.size(), mutation_bytes) ||
			!checked_add(retained_size, mutation_bytes, retained_size) ||
			retained_size > MaxReplicationDeltaRetainedBytes) {
			return StateDeltaValidationResult::SizeLimitExceeded;
		}
		if ((mutation.kind == StateMutationKind::Create || mutation.kind == StateMutationKind::Delete) &&
			mutation.atom.lifecycle != StateRecordLifecycle::ExplicitCreateDelete) {
			return StateDeltaValidationResult::InvalidMutation;
		}
		if (previous_key != nullptr) {
			if (mutation.atom.key == *previous_key) {
				return StateDeltaValidationResult::DuplicateKey;
			}
			// The model is canonical even though the wire parser accepts any
			// record order. P0.7 canonicalizes after parsing and before calling
			// this state layer.
			if (mutation.atom.key < *previous_key) {
				return StateDeltaValidationResult::InvalidMutation;
			}
		}
		previous_key = &mutation.atom.key;
	}

	return delta.encoded_size() <= MaxStateMessageSize ? StateDeltaValidationResult::Valid
													   : StateDeltaValidationResult::SizeLimitExceeded;
}

StateDeltaApplyResult apply_cumulative_state_delta(const StateImage& baseline,
	const CumulativeStateDelta& delta,
	const StateImageValidator* validator,
	StateImage& applied) noexcept
{
	if (validate_cumulative_state_delta(delta) != StateDeltaValidationResult::Valid) {
		return StateDeltaApplyResult::InvalidDelta;
	}

	ApplyPlan plan;
	if (const auto result = merge_cumulative_state_delta(baseline, delta, plan, nullptr);
		result != StateDeltaApplyResult::Applied) {
		return result;
	}

	std::vector<StateAtom> records;
	try {
		// The first pass validates every transition and the complete retained
		// output budget before this sole proportional reservation.
		records.reserve(plan.record_count);
		ApplyPlan materialized;
		if (const auto result = merge_cumulative_state_delta(baseline, delta, materialized, &records);
			result != StateDeltaApplyResult::Applied) {
			return result;
		}
	} catch (const std::bad_alloc&) {
		return StateDeltaApplyResult::AllocationFailed;
	}

	StateImage candidate;
	const auto create_result = StateImage::create(std::move(records), candidate);
	if (create_result == StateImageResult::AllocationFailed) {
		return StateDeltaApplyResult::AllocationFailed;
	}
	if (create_result != StateImageResult::Created) {
		return StateDeltaApplyResult::InvalidTransition;
	}
	if (validator != nullptr &&
		validator->validate_delta_transition(baseline, candidate) != ValidationError::None) {
		return StateDeltaApplyResult::ValidationFailed;
	}
	applied = std::move(candidate);
	return StateDeltaApplyResult::Applied;
}

bool ProducerBaselineTracker::provision_dirty_index_backing() noexcept
{
	if (m_dirty_indices) return true;
	m_dirty_indices.reset(new (std::nothrow) std::uint16_t[
		3U * MaxIncrementalDirtyStateAtomCount]{});
	return m_dirty_indices != nullptr;
}

ProducerBaselineResult ProducerBaselineTracker::initialize(const StateImage& current) noexcept
{
	if (!dirty_index_backing_ready() &&
		!provision_dirty_index_backing())
		return ProducerBaselineResult::AllocationFailed;
	clear();
	try {
		m_current = current;
	} catch (const std::bad_alloc&) {
		clear();
		return ProducerBaselineResult::AllocationFailed;
	}
	return ProducerBaselineResult::Applied;
}

ProducerBaselineResult ProducerBaselineTracker::replace_current(const StateImage& current) noexcept
{
	try {
		StateImage candidate = current;
		m_current = std::move(candidate);
	} catch (const std::bad_alloc&) {
		return ProducerBaselineResult::AllocationFailed;
	}
	m_active_incremental_record_set_compatible = false;
	m_candidate_incremental_record_set_compatible = false;
	m_active_dirty_index_count = 0U;
	m_candidate_dirty_index_count = 0U;
	return ProducerBaselineResult::Applied;
}

ProducerBaselineResult ProducerBaselineTracker::replace_current_incremental(
	const StateImage& current,
	const std::uint16_t* rebuilt_indices,
	std::size_t rebuilt_index_count) noexcept
{
	const auto& previous_records = m_current.records();
	const auto& current_records = current.records();
	if (!dirty_index_backing_ready() ||
		current_records.size() > MaxIncrementalDirtyStateAtomCount ||
		current_records.size() != previous_records.size() ||
		(rebuilt_index_count != 0U && rebuilt_indices == nullptr)) {
		return ProducerBaselineResult::InvalidArgument;
	}
	std::uint16_t previous_index = 0U;
	for (std::size_t dirty = 0U; dirty < rebuilt_index_count; ++dirty) {
		const auto index = static_cast<std::size_t>(rebuilt_indices[dirty]);
		if (index >= current_records.size() ||
			(dirty != 0U && rebuilt_indices[dirty] <= previous_index) ||
			previous_records[index].key != current_records[index].key ||
			previous_records[index].record_version !=
				current_records[index].record_version ||
			previous_records[index].lifecycle !=
				current_records[index].lifecycle ||
			previous_records[index].has_cascade_owner !=
				current_records[index].has_cascade_owner ||
			previous_records[index].cascade_owner !=
				current_records[index].cascade_owner) {
			return ProducerBaselineResult::InvalidArgument;
		}
		previous_index = rebuilt_indices[dirty];
	}
	if (has_active_baseline()) {
		auto* const active = m_dirty_indices.get();
		auto* const scratch =
			active + 2U * MaxIncrementalDirtyStateAtomCount;
		std::size_t merged_count = 0U;
		if (!merge_dirty_indices(active,
				m_active_dirty_index_count, rebuilt_indices,
				rebuilt_index_count, scratch,
				MaxIncrementalDirtyStateAtomCount,
				merged_count))
			return ProducerBaselineResult::InvalidArgument;
		std::copy_n(scratch, merged_count, active);
		m_active_dirty_index_count = merged_count;
		m_active_incremental_record_set_compatible = true;
	}
	if (has_candidate()) {
		auto* const candidate =
			m_dirty_indices.get() + MaxIncrementalDirtyStateAtomCount;
		auto* const scratch =
			m_dirty_indices.get() +
			2U * MaxIncrementalDirtyStateAtomCount;
		std::size_t merged_count = 0U;
		if (!merge_dirty_indices(candidate,
				m_candidate_dirty_index_count, rebuilt_indices,
				rebuilt_index_count, scratch,
				MaxIncrementalDirtyStateAtomCount,
				merged_count))
			return ProducerBaselineResult::InvalidArgument;
		std::copy_n(scratch, merged_count, candidate);
		m_candidate_dirty_index_count = merged_count;
		m_candidate_incremental_record_set_compatible = true;
	}
	m_current = current;
	return ProducerBaselineResult::Applied;
}

ProducerBaselineResult
ProducerBaselineTracker::take_current_for_incremental_patch(
	StateImage& current) noexcept
{
	if (!current.empty() || m_current.empty() ||
		(!m_active_incremental_record_set_compatible &&
		 !m_candidate_incremental_record_set_compatible) ||
		m_current.mutable_preallocated_records_if_unique() == nullptr) {
		return ProducerBaselineResult::InvalidArgument;
	}
	current = std::move(m_current);
	return ProducerBaselineResult::Applied;
}

ProducerBaselineResult
ProducerBaselineTracker::restore_current_after_incremental_patch(
	StateImage&& current) noexcept
{
	if (!m_current.empty() || current.empty())
		return ProducerBaselineResult::InvalidArgument;
	m_current = std::move(current);
	return ProducerBaselineResult::Applied;
}

ProducerBaselineResult
ProducerBaselineTracker::commit_current_incremental_patch(
	StateImage&& current,
	const std::uint16_t* rebuilt_indices,
	std::size_t rebuilt_index_count) noexcept
{
	const auto& current_records = current.records();
	if (!dirty_index_backing_ready() ||
		!m_current.empty() || current_records.empty() ||
		current_records.size() > MaxIncrementalDirtyStateAtomCount ||
		(rebuilt_index_count != 0U && rebuilt_indices == nullptr)) {
		return ProducerBaselineResult::InvalidArgument;
	}
	std::uint16_t previous_index = 0U;
	for (std::size_t dirty = 0U; dirty < rebuilt_index_count; ++dirty) {
		if (static_cast<std::size_t>(rebuilt_indices[dirty]) >=
				current_records.size() ||
			(dirty != 0U &&
			 rebuilt_indices[dirty] <= previous_index))
			return ProducerBaselineResult::InvalidArgument;
		previous_index = rebuilt_indices[dirty];
	}
	if (has_active_baseline()) {
		auto* const active = m_dirty_indices.get();
		auto* const scratch =
			active + 2U * MaxIncrementalDirtyStateAtomCount;
		std::size_t merged_count = 0U;
		if (!merge_dirty_indices(active,
				m_active_dirty_index_count, rebuilt_indices,
				rebuilt_index_count, scratch,
				MaxIncrementalDirtyStateAtomCount,
				merged_count))
			return ProducerBaselineResult::InvalidArgument;
		std::copy_n(scratch, merged_count, active);
		m_active_dirty_index_count = merged_count;
	}
	if (has_candidate()) {
		auto* const candidate =
			m_dirty_indices.get() + MaxIncrementalDirtyStateAtomCount;
		auto* const scratch =
			m_dirty_indices.get() +
			2U * MaxIncrementalDirtyStateAtomCount;
		std::size_t merged_count = 0U;
		if (!merge_dirty_indices(candidate,
				m_candidate_dirty_index_count, rebuilt_indices,
				rebuilt_index_count, scratch,
				MaxIncrementalDirtyStateAtomCount,
				merged_count))
			return ProducerBaselineResult::InvalidArgument;
		std::copy_n(scratch, merged_count, candidate);
		m_candidate_dirty_index_count = merged_count;
	}
	m_current = std::move(current);
	return ProducerBaselineResult::Applied;
}

ProducerBaselineResult ProducerBaselineTracker::capture_snapshot(std::uint32_t snapshot_id,
	std::uint32_t required_manifest_id,
	const StateImage& captured,
	const std::vector<SnapshotCandidatePart>& parts,
	std::uint64_t now_us) noexcept
{
	if (!dirty_index_backing_ready())
		return ProducerBaselineResult::AllocationFailed;
	if (snapshot_id == 0 || snapshot_id <= m_highest_snapshot_id || parts.empty() ||
		parts.size() > MaxTransactionParts) {
		return ProducerBaselineResult::InvalidArgument;
	}
	if (has_candidate()) {
		return ProducerBaselineResult::CandidateBusy;
	}
	for (std::size_t index = 0; index < parts.size(); ++index) {
		const auto& target = parts[index].target;
		if (target.message_id == 0 || target.message_type != MessageType::FullSnapshot || target.fragment_count == 0 ||
			target.fragment_count > MaxStateFragments || target.deadline_bearing) {
			return ProducerBaselineResult::InvalidArgument;
		}
		for (std::size_t previous = 0; previous < index; ++previous) {
			if (parts[previous].target.message_id == target.message_id) {
				return ProducerBaselineResult::InvalidArgument;
			}
		}
	}

	StateImage captured_copy;
	std::vector<SnapshotCandidatePart> copied_parts;
	std::vector<bool> applied;
	try {
		captured_copy = captured;
		copied_parts = parts;
		applied.assign(parts.size(), false);
	} catch (const std::bad_alloc&) {
		return ProducerBaselineResult::AllocationFailed;
	}

	m_candidate_baseline = std::move(captured_copy);
	m_candidate_parts = std::move(copied_parts);
	m_candidate_parts_applied = std::move(applied);
	m_candidate_snapshot_id = snapshot_id;
	m_candidate_required_manifest_id = required_manifest_id;
	m_candidate_deadline_us = saturating_add(now_us, TransactionAssemblyTimeoutUs);
	m_highest_snapshot_id = snapshot_id;
	m_candidate_dirty_index_count = 0U;
	m_candidate_incremental_record_set_compatible =
		captured.records().size() <= MaxIncrementalDirtyStateAtomCount;
	return ProducerBaselineResult::Applied;
}

ProducerBaselineResult ProducerBaselineTracker::acknowledge_snapshot_part(const AckPayload& ack,
	std::uint64_t now_us) noexcept
{
	if (!has_candidate()) {
		return ProducerBaselineResult::UnknownPart;
	}
	if (now_us >= m_candidate_deadline_us) {
		expire_candidate(now_us);
		return ProducerBaselineResult::Expired;
	}
	if (validate_ack_payload(ack) != ValidationError::None) {
		return ProducerBaselineResult::InvalidAck;
	}

	std::size_t part_index = m_candidate_parts.size();
	for (std::size_t index = 0; index < m_candidate_parts.size(); ++index) {
		if (m_candidate_parts[index].target.message_id == ack.target_message_id) {
			part_index = index;
			break;
		}
	}
	if (part_index == m_candidate_parts.size()) {
		return ProducerBaselineResult::UnknownPart;
	}
	if (validate_ack_target(ack, m_candidate_parts[part_index].target) != ValidationError::None) {
		return ProducerBaselineResult::InvalidAck;
	}
	if (ack.ack_flags != KnownAckFlags) {
		return ProducerBaselineResult::NoChange;
	}
	if (m_candidate_parts_applied[part_index]) {
		return ProducerBaselineResult::NoChange;
	}
	m_candidate_parts_applied[part_index] = true;
	if (std::find(m_candidate_parts_applied.begin(), m_candidate_parts_applied.end(), false) !=
		m_candidate_parts_applied.end()) {
		return ProducerBaselineResult::Applied;
	}

	m_active_baseline = std::move(m_candidate_baseline);
	m_active_snapshot_id = m_candidate_snapshot_id;
	m_active_required_manifest_id = m_candidate_required_manifest_id;
	m_next_delta_sequence = 1;
	m_emitted_delta_for_active_baseline = false;
	m_active_dirty_index_count = m_candidate_dirty_index_count;
	if (!dirty_index_backing_ready())
		return ProducerBaselineResult::AllocationFailed;
	std::copy_n(
		m_dirty_indices.get() + MaxIncrementalDirtyStateAtomCount,
		m_candidate_dirty_index_count, m_dirty_indices.get());
	m_active_incremental_record_set_compatible =
		m_candidate_incremental_record_set_compatible;
	m_candidate_dirty_index_count = 0U;
	m_candidate_incremental_record_set_compatible = false;
	m_candidate_parts.clear();
	m_candidate_parts_applied.clear();
	m_candidate_snapshot_id = 0;
	m_candidate_required_manifest_id = 0;
	m_candidate_deadline_us = 0;
	return ProducerBaselineResult::Applied;
}

ProducerBaselineResult ProducerBaselineTracker::expire_candidate(std::uint64_t now_us) noexcept
{
	if (!has_candidate() || now_us < m_candidate_deadline_us) {
		return ProducerBaselineResult::NoChange;
	}
	m_candidate_baseline = StateImage{};
	m_candidate_parts.clear();
	m_candidate_parts_applied.clear();
	m_candidate_snapshot_id = 0;
	m_candidate_required_manifest_id = 0;
	m_candidate_deadline_us = 0;
	m_candidate_dirty_index_count = 0U;
	m_candidate_incremental_record_set_compatible = false;
	return ProducerBaselineResult::Expired;
}

ProducerBaselineResult ProducerBaselineTracker::emit_cumulative_delta(
	std::uint64_t producer_sample_time_us,
	CumulativeStateDelta& delta,
	DeltaBuildChanges* changes) noexcept
{
	if (!has_active_baseline()) {
		return ProducerBaselineResult::NoActiveBaseline;
	}
	if (m_next_delta_sequence == 0) {
		return ProducerBaselineResult::SequenceExhausted;
	}
	// The caller supplies a per-session scratch delta with its mutation and
	// payload capacities provisioned at startup. Building through a fresh local
	// candidate defeats that ownership and reallocates/copies every tick before
	// moving into the same scratch. analyze_delta runs before build_delta writes
	// anything, and callers discard the scratch on non-Applied, so direct reuse
	// preserves the transaction result while keeping steady-state work bounded.
	const auto result = build_delta(m_active_baseline,
		m_current,
		m_active_snapshot_id,
		m_next_delta_sequence,
		producer_sample_time_us,
		delta,
		m_dirty_indices.get(),
		m_active_dirty_index_count,
		m_active_incremental_record_set_compatible,
		changes);
	if (result == ProducerBaselineResult::NoChange) {
		// v1 forbids an empty Delta. analyze_delta already performed the exact
		// ordered comparison, so do not repeat it with dirty_record_count just
		// to distinguish this case.
		return m_emitted_delta_for_active_baseline ? ProducerBaselineResult::KeyframeRequired
												   : ProducerBaselineResult::NoChange;
	}
	if (result != ProducerBaselineResult::Applied) {
		return result;
	}
	m_emitted_delta_for_active_baseline = true;
	m_next_delta_sequence =
		m_next_delta_sequence == std::numeric_limits<std::uint32_t>::max() ? 0 : m_next_delta_sequence + 1;
	return ProducerBaselineResult::Applied;
}

void ProducerBaselineTracker::clear() noexcept
{
	m_current = StateImage{};
	m_active_baseline = StateImage{};
	m_candidate_baseline = StateImage{};
	m_candidate_parts.clear();
	m_candidate_parts_applied.clear();
	m_active_snapshot_id = 0;
	m_active_required_manifest_id = 0;
	m_candidate_snapshot_id = 0;
	m_candidate_required_manifest_id = 0;
	m_highest_snapshot_id = 0;
	m_next_delta_sequence = 1;
	m_candidate_deadline_us = 0;
	m_emitted_delta_for_active_baseline = false;
	m_active_dirty_index_count = 0U;
	m_candidate_dirty_index_count = 0U;
	m_active_incremental_record_set_compatible = false;
	m_candidate_incremental_record_set_compatible = false;
}

std::size_t ProducerBaselineTracker::active_dirty_record_count() const noexcept
{
	if (!has_active_baseline()) return 0U;
	return m_active_incremental_record_set_compatible
		? dirty_record_count_indices(m_active_baseline, m_current,
			  m_dirty_indices.get(),
			  m_active_dirty_index_count)
		: dirty_record_count(m_active_baseline, m_current);
}

std::size_t ProducerBaselineTracker::candidate_dirty_record_count() const noexcept
{
	if (!has_candidate()) return 0U;
	return m_candidate_incremental_record_set_compatible
		? dirty_record_count_indices(m_candidate_baseline, m_current,
			  m_dirty_indices.get() +
				  MaxIncrementalDirtyStateAtomCount,
			  m_candidate_dirty_index_count)
		: dirty_record_count(m_candidate_baseline, m_current);
}

ManifestInstallResult ClientReplicationModel::install_manifest(std::uint32_t manifest_id) noexcept
{
	if (manifest_id == 0) {
		return ManifestInstallResult::InvalidId;
	}
	if (manifest_id == m_installed_manifest_id) {
		return ManifestInstallResult::AlreadyInstalled;
	}
	if (manifest_id < m_installed_manifest_id) {
		return ManifestInstallResult::Stale;
	}
	m_installed_manifest_id = manifest_id;
	return ManifestInstallResult::Installed;
}

SnapshotCandidateResult ClientReplicationModel::note_snapshot_candidate(std::uint32_t snapshot_id,
	std::uint32_t required_manifest_id,
	std::uint64_t first_part_time_us,
	std::uint64_t transaction_deadline_us) noexcept
{
	if (snapshot_id == 0 ||
		transaction_deadline_us != saturating_add(first_part_time_us, TransactionAssemblyTimeoutUs)) {
		return SnapshotCandidateResult::InvalidArgument;
	}
	if (snapshot_id == m_active_snapshot_id) {
		return SnapshotCandidateResult::AlreadyCommitted;
	}
	if (m_active_snapshot_id != 0 && snapshot_id < m_active_snapshot_id) {
		return SnapshotCandidateResult::Stale;
	}
	if (m_candidate_snapshot_id != 0) {
		return snapshot_id == m_candidate_snapshot_id && required_manifest_id == m_candidate_required_manifest_id &&
					   first_part_time_us == m_candidate_first_part_time_us &&
					   transaction_deadline_us == m_candidate_deadline_us
				   ? SnapshotCandidateResult::AlreadyKnown
				   : SnapshotCandidateResult::CandidateBusy;
	}
	if (m_highest_snapshot_id_seen != 0 && snapshot_id <= m_highest_snapshot_id_seen) {
		return SnapshotCandidateResult::Stale;
	}
	m_candidate_snapshot_id = snapshot_id;
	m_candidate_required_manifest_id = required_manifest_id;
	m_candidate_first_part_time_us = first_part_time_us;
	m_candidate_deadline_us = transaction_deadline_us;
	m_highest_snapshot_id_seen = snapshot_id;
	return SnapshotCandidateResult::Known;
}

SnapshotCommitResult ClientReplicationModel::commit_snapshot(std::uint32_t snapshot_id,
	std::uint32_t required_manifest_id,
	const StateImage& snapshot,
	std::uint64_t now_us,
	ClientResyncChannel& resync_channel,
	const StateImageValidator* validator) noexcept
{
	resync_channel.disposition = ClientResyncResult::NotRequested;
	if (snapshot_id != 0 && snapshot_id == m_active_snapshot_id) {
		return SnapshotCommitResult::AlreadyCommitted;
	}
	if (m_candidate_snapshot_id == 0 || snapshot_id != m_candidate_snapshot_id ||
		required_manifest_id != m_candidate_required_manifest_id) {
		return SnapshotCommitResult::UnknownCandidate;
	}
	if (now_us >= m_candidate_deadline_us) {
		const auto scope =
			m_candidate_required_manifest_id != 0 && m_candidate_required_manifest_id != m_installed_manifest_id
				? ClientResyncScope::ManifestAndFullSnapshot
				: ClientResyncScope::FullSnapshot;
		clear_candidate();
		request_resynchronization(ResyncReason::ReassemblyTimeout, now_us, scope, resync_channel);
		return SnapshotCommitResult::Expired;
	}
	if (required_manifest_id != 0 && required_manifest_id != m_installed_manifest_id) {
		// The valid snapshot remains a candidate so Manifest and Snapshot may
		// be pipelined. APPLIED is deferred until this exact dependency lands.
		return SnapshotCommitResult::MissingManifest;
	}
	if (validator != nullptr && validator->validate(snapshot) != ValidationError::None) {
		clear_candidate();
		request_resynchronization(ResyncReason::ValidationFailed,
			now_us,
			ClientResyncScope::FullSnapshot,
			resync_channel);
		return SnapshotCommitResult::ValidationFailed;
	}

	StateImage active = snapshot;
	StateImage published = snapshot;

	SnapshotCommitResult result = SnapshotCommitResult::Committed;
	std::uint32_t applied_sequence = 0;
	if (m_has_pending_delta && now_us < m_pending_delta_deadline_us &&
		m_pending_delta.baseline_snapshot_id == snapshot_id) {
		StateImage pending_published;
		const auto apply_result = apply_cumulative_state_delta(active, m_pending_delta, validator, pending_published);
		if (apply_result == StateDeltaApplyResult::Applied) {
			published = std::move(pending_published);
			applied_sequence = m_pending_delta.delta_sequence;
			result = SnapshotCommitResult::CommittedAndPendingDeltaApplied;
		} else if (apply_result == StateDeltaApplyResult::AllocationFailed) {
			// Preserve the old publication and the complete candidate. A local
			// transient allocation failure can then be retried before expiry.
			return SnapshotCommitResult::AllocationFailed;
		} else {
			result = SnapshotCommitResult::CommittedPendingDeltaRejected;
		}
	}

	m_active_baseline = std::move(active);
	m_published = std::move(published);
	m_active_snapshot_id = snapshot_id;
	m_last_delta_sequence = applied_sequence;
	clear_candidate();
	if (result == SnapshotCommitResult::CommittedPendingDeltaRejected) {
		request_resynchronization(ResyncReason::ValidationFailed,
			now_us,
			ClientResyncScope::FullSnapshot,
			resync_channel);
	}
	return result;
}

ClientDeltaResult ClientReplicationModel::receive_delta(const CumulativeStateDelta& delta,
	std::uint64_t now_us,
	std::uint64_t session_id,
	const EndpointKey& endpoint,
	ProtocolRateLimiter& rate_limiter,
	ResyncRequestPayload& resync,
	const StateImageValidator* validator) noexcept
{
	ClientResyncChannel resync_channel{session_id, endpoint, rate_limiter, resync};
	if (expire_snapshot_candidate(now_us, resync_channel)) {
		return map_delta_resync_result(resync_channel.disposition);
	}
	if (validate_cumulative_state_delta(delta) != StateDeltaValidationResult::Valid) {
		return map_delta_resync_result(request_resynchronization(ResyncReason::ValidationFailed,
			now_us,
			ClientResyncScope::FullSnapshot,
			resync_channel));
	}
	expire_pending_delta(now_us);

	if (delta.baseline_snapshot_id == m_active_snapshot_id && m_active_snapshot_id != 0) {
		if (delta.delta_sequence <= m_last_delta_sequence) {
			return ClientDeltaResult::IgnoredOldSequence;
		}
		const auto applied = apply_active_delta(delta, validator);
		if (applied == ClientDeltaResult::Applied) {
			return applied;
		}
		if (applied == ClientDeltaResult::AllocationFailed) {
			return applied;
		}
		return map_delta_resync_result(request_resynchronization(ResyncReason::ValidationFailed,
			now_us,
			ClientResyncScope::FullSnapshot,
			resync_channel));
	}
	if (m_active_snapshot_id != 0 && delta.baseline_snapshot_id < m_active_snapshot_id) {
		// A Delta is replaceable: old-baseline traffic is silently discarded
		// here and this layer deliberately does not resolve the conflicting
		// StaleBaseline-NACK wording in the normative text.
		return ClientDeltaResult::IgnoredOldBaseline;
	}
	if (m_candidate_snapshot_id != 0 && delta.baseline_snapshot_id == m_candidate_snapshot_id) {
		if (m_has_pending_delta && delta.delta_sequence <= m_pending_delta.delta_sequence) {
			return ClientDeltaResult::IgnoredOlderQueuedDelta;
		}
		try {
			CumulativeStateDelta copy = normalized_delta_copy(delta);
			if (!m_has_pending_delta) {
				m_pending_delta_deadline_us =
					std::min(saturating_add(now_us, PendingDeltaLifetimeUs), m_candidate_deadline_us);
			}
			const auto replaced = m_has_pending_delta;
			m_pending_delta = std::move(copy);
			m_has_pending_delta = true;
			return replaced ? ClientDeltaResult::ReplacedQueuedDelta : ClientDeltaResult::QueuedForCandidate;
		} catch (const std::bad_alloc&) {
			return ClientDeltaResult::AllocationFailed;
		}
	}
	const auto request_result = request_resynchronization(ResyncReason::UnknownBaseline,
		now_us,
		ClientResyncScope::FullSnapshot,
		resync_channel);
	switch (request_result) {
	case ClientResyncResult::NotRequested:
		return ClientDeltaResult::UnknownBaselineResyncUnavailable;
	case ClientResyncResult::Requested:
		return ClientDeltaResult::UnknownBaselineResyncRequested;
	case ClientResyncResult::RateLimited:
		return ClientDeltaResult::UnknownBaselineRateLimited;
	case ClientResyncResult::Unavailable:
		return ClientDeltaResult::UnknownBaselineResyncUnavailable;
	}
	return ClientDeltaResult::UnknownBaselineResyncUnavailable;
}

bool ClientReplicationModel::expire_snapshot_candidate(std::uint64_t now_us,
	ClientResyncChannel& resync_channel) noexcept
{
	resync_channel.disposition = ClientResyncResult::NotRequested;
	if (m_candidate_snapshot_id == 0 || now_us < m_candidate_deadline_us) {
		return false;
	}
	const auto scope =
		m_candidate_required_manifest_id != 0 && m_candidate_required_manifest_id != m_installed_manifest_id
			? ClientResyncScope::ManifestAndFullSnapshot
			: ClientResyncScope::FullSnapshot;
	clear_candidate();
	request_resynchronization(ResyncReason::ReassemblyTimeout, now_us, scope, resync_channel);
	return true;
}

ClientResyncResult ClientReplicationModel::notify_manifest_transaction_expired(std::uint64_t now_us,
	ClientResyncChannel& channel) noexcept
{
	return request_resynchronization(ResyncReason::ReassemblyTimeout,
		now_us,
		ClientResyncScope::ManifestAndFullSnapshot,
		channel);
}

bool ClientReplicationModel::expire_pending_delta(std::uint64_t now_us) noexcept
{
	if (!m_has_pending_delta || now_us < m_pending_delta_deadline_us) {
		return false;
	}
	m_pending_delta = CumulativeStateDelta{};
	m_pending_delta_deadline_us = 0;
	m_has_pending_delta = false;
	return true;
}

void ClientReplicationModel::clear() noexcept
{
	m_active_baseline = StateImage{};
	m_published = StateImage{};
	m_installed_manifest_id = 0;
	m_active_snapshot_id = 0;
	m_highest_snapshot_id_seen = 0;
	m_last_delta_sequence = 0;
	m_next_resync_request_id = 1;
	clear_candidate();
}

ClientDeltaResult ClientReplicationModel::apply_active_delta(const CumulativeStateDelta& delta,
	const StateImageValidator* validator) noexcept
{
	StateImage candidate;
	const auto result = apply_cumulative_state_delta(m_active_baseline, delta, validator, candidate);
	switch (result) {
	case StateDeltaApplyResult::Applied:
		m_published = std::move(candidate);
		m_last_delta_sequence = delta.delta_sequence;
		return ClientDeltaResult::Applied;
	case StateDeltaApplyResult::ValidationFailed:
		return ClientDeltaResult::ValidationFailed;
	case StateDeltaApplyResult::AllocationFailed:
		return ClientDeltaResult::AllocationFailed;
	case StateDeltaApplyResult::InvalidDelta:
	case StateDeltaApplyResult::InvalidTransition:
		return ClientDeltaResult::InvalidDelta;
	}
	return ClientDeltaResult::InvalidDelta;
}

ClientResyncResult ClientReplicationModel::request_resynchronization(ResyncReason reason,
	std::uint64_t now_us,
	ClientResyncScope scope,
	ClientResyncChannel& channel) noexcept
{
	channel.disposition = ClientResyncResult::Unavailable;
	if (m_next_resync_request_id == 0 ||
		(scope != ClientResyncScope::FullSnapshot && scope != ClientResyncScope::ManifestAndFullSnapshot)) {
		return channel.disposition;
	}

	ResyncRequestPayload request;
	request.request_id = m_next_resync_request_id;
	request.reason = reason;
	request.request_flags = ResyncRequestFlagRequireFullSnapshot;
	if (m_installed_manifest_id == 0 || scope == ClientResyncScope::ManifestAndFullSnapshot) {
		request.request_flags = static_cast<std::uint8_t>(request.request_flags | ResyncRequestFlagRequireManifest);
	}
	request.last_applied_snapshot_id = m_active_snapshot_id;
	request.last_applied_delta_sequence = m_last_delta_sequence;
	request.client_send_time_us = now_us;
	if (validate_resync_request_payload(request) != ValidationError::None) {
		return channel.disposition;
	}

	const auto limit = channel.rate_limiter.consume_session(RateLimitClass::ResyncRequest,
		channel.session_id,
		channel.endpoint,
		now_us);
	if (limit == ProtocolRateLimitResult::RateLimited) {
		channel.disposition = ClientResyncResult::RateLimited;
		return channel.disposition;
	}
	if (limit != ProtocolRateLimitResult::Allowed) {
		return channel.disposition;
	}
	channel.request = request;
	m_next_resync_request_id = next_resync_request_id(m_next_resync_request_id);
	channel.disposition = ClientResyncResult::Requested;
	return channel.disposition;
}

void ClientReplicationModel::clear_candidate() noexcept
{
	m_candidate_snapshot_id = 0;
	m_candidate_required_manifest_id = 0;
	m_candidate_first_part_time_us = 0;
	m_candidate_deadline_us = 0;
	m_pending_delta = CumulativeStateDelta{};
	m_pending_delta_deadline_us = 0;
	m_has_pending_delta = false;
}

} // namespace telemetry::protocol
