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
	if (atom.key.record_type == 0 || atom.record_version != 1 || !is_known_lifecycle(atom.lifecycle) ||
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
	return atom.key.record_type != 0 && atom.record_version == 1 &&
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
	normalized.mutations.reserve(delta.mutations.size());
	for (const auto& mutation : delta.mutations) {
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
	const auto iterator = std::lower_bound(delta.mutations.begin(),
		delta.mutations.end(),
		key,
		[](const StateMutation& mutation, const StateAtomKey& searched) { return mutation.atom.key < searched; });
	return iterator != delta.mutations.end() && iterator->atom.key == key ? &*iterator : nullptr;
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
	while (baseline_index < baseline_records.size() || mutation_index < delta.mutations.size()) {
		const StateAtom* old_atom =
			baseline_index < baseline_records.size() ? &baseline_records[baseline_index] : nullptr;
		const StateMutation* mutation =
			mutation_index < delta.mutations.size() ? &delta.mutations[mutation_index] : nullptr;

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

ProducerBaselineResult build_delta(const StateImage& baseline,
	const StateImage& current,
	std::uint32_t baseline_snapshot_id,
	std::uint32_t delta_sequence,
	std::uint64_t producer_sample_time_us,
	CumulativeStateDelta& delta) noexcept
{
	CumulativeStateDelta candidate;
	candidate.baseline_snapshot_id = baseline_snapshot_id;
	candidate.delta_sequence = delta_sequence;
	candidate.producer_sample_time_us = producer_sample_time_us;

	DeltaPlan plan;
	if (const auto result = analyze_delta(baseline, current, plan); result != ProducerBaselineResult::Applied) {
		return result;
	}
	try {
		// analyze_delta proves this reservation and all copied record payloads
		// fit the one-MiB Delta gate before any proportional output allocation.
		candidate.mutations.reserve(plan.mutation_count);
	} catch (const std::bad_alloc&) {
		return ProducerBaselineResult::AllocationFailed;
	}

	const auto& before = baseline.records();
	const auto& after = current.records();
	std::size_t before_index = 0;
	std::size_t after_index = 0;
	try {
		while (before_index < before.size() || after_index < after.size()) {
			if (before_index == before.size()) {
				const auto& atom = after[after_index++];
				candidate.mutations.push_back(
					{atom.lifecycle == StateRecordLifecycle::ExplicitCreateDelete ? StateMutationKind::Create
																				  : StateMutationKind::Upsert,
						atom});
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
				candidate.mutations.push_back(make_delete_mutation(atom));
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
				candidate.mutations.push_back(make_delete_mutation(old_atom));
				++before_index;
			} else if (new_atom.key < old_atom.key) {
				candidate.mutations.push_back(
					{new_atom.lifecycle == StateRecordLifecycle::ExplicitCreateDelete ? StateMutationKind::Create
																					  : StateMutationKind::Upsert,
						new_atom});
				++after_index;
			} else {
				if (old_atom.record_version != new_atom.record_version || old_atom.lifecycle != new_atom.lifecycle ||
					old_atom.has_cascade_owner != new_atom.has_cascade_owner ||
					old_atom.cascade_owner != new_atom.cascade_owner) {
					return ProducerBaselineResult::KeyframeRequired;
				}
				if (old_atom.value != new_atom.value) {
					candidate.mutations.push_back({StateMutationKind::Upsert, new_atom});
				}
				++before_index;
				++after_index;
			}
		}
	} catch (const std::bad_alloc&) {
		return ProducerBaselineResult::AllocationFailed;
	}

	if (validate_cumulative_state_delta(candidate) != StateDeltaValidationResult::Valid) {
		return candidate.encoded_size() > MaxStateMessageSize ? ProducerBaselineResult::KeyframeRequired
															  : ProducerBaselineResult::InvalidArgument;
	}
	delta = std::move(candidate);
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
	std::size_t total_size = 0;
	std::size_t retained_size = 0;
	if (!checked_multiply(records.size(), sizeof(StateAtom), retained_size) ||
		retained_size > MaxReplicationStateImageRetainedBytes) {
		return StateImageResult::SizeLimitExceeded;
	}
	for (const auto& record : records) {
		std::size_t record_size = 0;
		if (!structurally_valid_atom(record, record_size)) {
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
		if (owner == records.end() || owner->key != record.cascade_owner ||
			owner->lifecycle != StateRecordLifecycle::ExplicitCreateDelete || owner->has_cascade_owner) {
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

const std::vector<StateAtom>& StateImage::records() const noexcept
{
	static const std::vector<StateAtom> EmptyRecords;
	return m_records == nullptr ? EmptyRecords : *m_records;
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
	for (const auto& mutation : mutations) {
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
	return left.baseline_snapshot_id == right.baseline_snapshot_id && left.delta_sequence == right.delta_sequence &&
		   left.producer_sample_time_us == right.producer_sample_time_us && left.mutations == right.mutations;
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
	if (delta.baseline_snapshot_id == 0 || delta.delta_sequence == 0 || delta.mutations.empty() ||
		delta.mutations.size() > std::numeric_limits<std::uint16_t>::max()) {
		return StateDeltaValidationResult::InvalidIdentity;
	}
	std::size_t retained_size = 0;
	if (!checked_multiply(delta.mutations.size(), sizeof(StateMutation), retained_size) ||
		retained_size > MaxReplicationDeltaRetainedBytes) {
		return StateDeltaValidationResult::SizeLimitExceeded;
	}

	const StateAtomKey* previous_key = nullptr;
	for (const auto& mutation : delta.mutations) {
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
	if (validator != nullptr && validator->validate(candidate) != ValidationError::None) {
		return StateDeltaApplyResult::ValidationFailed;
	}
	applied = std::move(candidate);
	return StateDeltaApplyResult::Applied;
}

ProducerBaselineResult ProducerBaselineTracker::initialize(const StateImage& current) noexcept
{
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
	return ProducerBaselineResult::Applied;
}

ProducerBaselineResult ProducerBaselineTracker::capture_snapshot(std::uint32_t snapshot_id,
	std::uint32_t required_manifest_id,
	const StateImage& captured,
	const std::vector<SnapshotCandidatePart>& parts,
	std::uint64_t now_us) noexcept
{
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
	return ProducerBaselineResult::Expired;
}

ProducerBaselineResult ProducerBaselineTracker::emit_cumulative_delta(std::uint64_t producer_sample_time_us,
	CumulativeStateDelta& delta) noexcept
{
	if (!has_active_baseline()) {
		return ProducerBaselineResult::NoActiveBaseline;
	}
	if (m_next_delta_sequence == 0) {
		return ProducerBaselineResult::SequenceExhausted;
	}
	if (dirty_record_count(m_active_baseline, m_current) == 0) {
		// v1 forbids an empty Delta. Once a non-empty cumulative delta may
		// have reached the peer, returning completely to the baseline needs a
		// keyframe; otherwise the peer could retain the previous difference
		// forever.
		return m_emitted_delta_for_active_baseline ? ProducerBaselineResult::KeyframeRequired
												   : ProducerBaselineResult::NoChange;
	}
	CumulativeStateDelta candidate;
	const auto result = build_delta(m_active_baseline,
		m_current,
		m_active_snapshot_id,
		m_next_delta_sequence,
		producer_sample_time_us,
		candidate);
	if (result != ProducerBaselineResult::Applied) {
		return result;
	}
	delta = std::move(candidate);
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
}

std::size_t ProducerBaselineTracker::active_dirty_record_count() const noexcept
{
	return has_active_baseline() ? dirty_record_count(m_active_baseline, m_current) : 0;
}

std::size_t ProducerBaselineTracker::candidate_dirty_record_count() const noexcept
{
	return has_candidate() ? dirty_record_count(m_candidate_baseline, m_current) : 0;
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
