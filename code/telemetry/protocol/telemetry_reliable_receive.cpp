#include "telemetry/protocol/telemetry_reliable_receive.h"

#include <algorithm>
#include <limits>

namespace telemetry::protocol {

namespace {

constexpr std::size_t InvalidIndex = std::numeric_limits<std::size_t>::max();

constexpr bool has_message_flag(std::uint8_t flags, MessageFlag flag) noexcept
{
	return (flags & static_cast<std::uint8_t>(flag)) != 0;
}

bool header_declares_nack_eligible_class(const TelemetryDatagramHeader& header) noexcept
{
	const auto ack_required = has_message_flag(header.flags, MessageFlagAckRequired);
	const auto keyframe = has_message_flag(header.flags, MessageFlagKeyframe);
	const auto video_idr = has_message_flag(header.flags, MessageFlagVideoIdr);
	if (keyframe && video_idr) {
		return false;
	}
	if (keyframe != (header.message_type == MessageType::FullSnapshot) ||
		(video_idr && header.message_type != MessageType::TargetVideoFrame)) {
		return false;
	}
	if (header.message_type == MessageType::TargetVideoFrame) {
		return video_idr && !ack_required;
	}
	if (!ack_required) {
		return false;
	}
	switch (header.message_type) {
	case MessageType::Welcome:
	case MessageType::SessionBegin:
	case MessageType::Manifest:
	case MessageType::FullSnapshot:
	case MessageType::EventBatch:
	case MessageType::ResyncRequest:
	case MessageType::SessionEnd:
	case MessageType::TargetVideoSubscribe:
	case MessageType::TargetVideoConfig:
	case MessageType::TargetVideoKeyframeRequest:
	case MessageType::TargetVideoStop:
	case MessageType::CapabilityUpdate:
		return true;
	case MessageType::Invalid:
	case MessageType::Discovery:
	case MessageType::Hello:
	case MessageType::Delta:
	case MessageType::Heartbeat:
	case MessageType::Ack:
	case MessageType::Nack:
	case MessageType::TargetVideoFrame:
	case MessageType::TargetVideoStats:
		return false;
	}
	return false;
}

bool header_declares_video_idr(const TelemetryDatagramHeader& header) noexcept
{
	return header.message_type == MessageType::TargetVideoFrame && header_declares_nack_eligible_class(header);
}

constexpr std::uint64_t saturating_add(std::uint64_t lhs, std::uint64_t rhs) noexcept
{
	return rhs > std::numeric_limits<std::uint64_t>::max() - lhs ? std::numeric_limits<std::uint64_t>::max()
																 : lhs + rhs;
}

bool valid_receive_key(const ReliableReceiveKey& key) noexcept
{
	return key.session_id != 0 && key.endpoint.is_valid() && key.message_type != MessageType::Invalid &&
		   key.message_id != 0 && key.fragment_count != 0 &&
		   (!is_known_message_type(key.message_type) ||
			   (key.fragment_count <= max_fragment_count(key.message_type) &&
				   (key.fragment_count == 1 || message_type_allows_fragmentation(key.message_type))));
}

bool same_receive_base(const ReliableReceiveKey& lhs, const ReliableReceiveKey& rhs) noexcept
{
	return lhs.session_id == rhs.session_id && lhs.endpoint == rhs.endpoint && lhs.message_type == rhs.message_type &&
		   lhs.message_id == rhs.message_id;
}

bool same_receive_key(const ReliableReceiveKey& lhs, const ReliableReceiveKey& rhs) noexcept
{
	return same_receive_base(lhs, rhs) && lhs.message_crc32 == rhs.message_crc32 &&
		   lhs.fragment_count == rhs.fragment_count;
}

std::uint64_t send_window_retention_us(ReliableMessageClass message_class) noexcept
{
	switch (message_class) {
	case ReliableMessageClass::Transaction:
		return ReliableTransactionRetentionUs;
	case ReliableMessageClass::VideoIdr:
		// IDR is NACK-only and is deduplicated by the video/reassembly identity,
		// never by the reliable ACK-result cache.
		return 0;
	case ReliableMessageClass::ControlDrop:
	case ReliableMessageClass::ControlRequestKeyframe:
	case ReliableMessageClass::ControlRequestResync:
	case ReliableMessageClass::SessionCritical:
	case ReliableMessageClass::SessionClosing:
	case ReliableMessageClass::ReliableEvent:
	case ReliableMessageClass::HandshakeCritical:
		return ReliableOrdinaryRetentionUs;
	case ReliableMessageClass::HelloNegotiation:
		// HELLO uses ProducerHandshakeCache's (endpoint, client_nonce) identity;
		// it is never admitted to the session-bound reliable receive cache.
		return 0;
	}
	return 0;
}

bool fragment_payload_matches_view(const DatagramView& fragment) noexcept
{
	if (fragment.payload.size != fragment.header.payload_size) {
		return false;
	}
	return fragment.payload.size == 0 || fragment.payload.data != nullptr;
}

} // namespace

std::uint64_t reliable_receive_retention_us(ReliableMessageClass message_class) noexcept
{
	const auto sender_retention = send_window_retention_us(message_class);
	return sender_retention == 0 ? 0 : saturating_add(sender_retention, ReliableReceiveDeduplicationGraceUs);
}

bool reliable_receive_class_matches_type(ReliableMessageClass message_class, MessageType message_type) noexcept
{
	switch (message_class) {
	case ReliableMessageClass::ControlDrop:
		return message_type == MessageType::TargetVideoSubscribe || message_type == MessageType::TargetVideoConfig ||
			   message_type == MessageType::TargetVideoStop;
	case ReliableMessageClass::ControlRequestKeyframe:
		return message_type == MessageType::TargetVideoKeyframeRequest;
	case ReliableMessageClass::ControlRequestResync:
		return message_type == MessageType::ResyncRequest;
	case ReliableMessageClass::SessionCritical:
		return message_type == MessageType::SessionBegin || message_type == MessageType::CapabilityUpdate;
	case ReliableMessageClass::SessionClosing:
		return message_type == MessageType::SessionEnd;
	case ReliableMessageClass::Transaction:
		return message_type == MessageType::Manifest || message_type == MessageType::FullSnapshot;
	case ReliableMessageClass::ReliableEvent:
		return message_type == MessageType::EventBatch;
	case ReliableMessageClass::VideoIdr:
		return false;
	case ReliableMessageClass::HandshakeCritical:
		return message_type == MessageType::Welcome;
	case ReliableMessageClass::HelloNegotiation:
		return false;
	}
	return false;
}

ReliableReceiveCache::ReliableReceiveCache(std::size_t maximum_entries) noexcept
	: m_maximum_entries(std::min(maximum_entries, ReliableReceiveMaximumEntries))
{
}

ReliableReceiveReserveResult ReliableReceiveCache::reserve(const ReliableReceiveKey& key,
	ReliableMessageClass message_class,
	std::uint64_t now_us,
	ReliableReceiveOutcome& outcome) noexcept
{
	if (!valid_receive_key(key) || !reliable_receive_class_matches_type(message_class, key.message_type)) {
		return ReliableReceiveReserveResult::InvalidKey;
	}
	const auto retention_us = reliable_receive_retention_us(message_class);
	if (retention_us == 0) {
		return ReliableReceiveReserveResult::InvalidKey;
	}

	const auto effective_now = advance_time(now_us);
	expire_at(effective_now);

	const auto exact = find_exact(key);
	if (exact != InvalidIndex) {
		outcome = m_entries[exact].outcome;
		return ReliableReceiveReserveResult::Duplicate;
	}
	if (find_base_identity(key) != InvalidIndex) {
		return ReliableReceiveReserveResult::IdentityConflict;
	}
	if (m_entry_count >= m_maximum_entries) {
		// expire_at() is the only eviction path. A live entry is never replaced
		// merely to admit peer-controlled input.
		return ReliableReceiveReserveResult::ResourceLimit;
	}

	const auto free_index = find_free();
	if (free_index == InvalidIndex) {
		return ReliableReceiveReserveResult::ResourceLimit;
	}
	auto& entry = m_entries[free_index];
	entry.occupied = true;
	entry.key = key;
	entry.outcome = ReliableReceiveOutcome{};
	entry.expires_at_us = saturating_add(effective_now, retention_us);
	++m_entry_count;
	return ReliableReceiveReserveResult::Reserved;
}

ReliableReceiveLookupResult ReliableReceiveCache::lookup(const ReliableReceiveKey& key,
	std::uint64_t now_us,
	ReliableReceiveOutcome& outcome) noexcept
{
	if (!valid_receive_key(key)) {
		return ReliableReceiveLookupResult::InvalidKey;
	}
	const auto effective_now = advance_time(now_us);
	expire_at(effective_now);
	const auto exact = find_exact(key);
	if (exact != InvalidIndex) {
		outcome = m_entries[exact].outcome;
		return ReliableReceiveLookupResult::Found;
	}
	return find_base_identity(key) == InvalidIndex ? ReliableReceiveLookupResult::NotFound
												   : ReliableReceiveLookupResult::IdentityConflict;
}

ReliableReceiveUpdateResult ReliableReceiveCache::record_validated(const ReliableReceiveKey& key) noexcept
{
	return record(key, ReliableReceiveOutcome{ReliableReceiveOutcomeKind::Validated, ValidationError::None});
}

ReliableReceiveUpdateResult ReliableReceiveCache::record_applied(const ReliableReceiveKey& key) noexcept
{
	return record(key, ReliableReceiveOutcome{ReliableReceiveOutcomeKind::Applied, ValidationError::None});
}

ReliableReceiveUpdateResult ReliableReceiveCache::record_error(const ReliableReceiveKey& key,
	ValidationError error) noexcept
{
	if (error == ValidationError::None) {
		return ReliableReceiveUpdateResult::InvalidOutcome;
	}
	return record(key, ReliableReceiveOutcome{ReliableReceiveOutcomeKind::Error, error});
}

ValidationError
ReliableReceiveCache::cached_ack(const ReliableReceiveKey& key, std::uint64_t now_us, AckPayload& ack) noexcept
{
	ReliableReceiveOutcome outcome;
	const auto result = lookup(key, now_us, outcome);
	if (result == ReliableReceiveLookupResult::IdentityConflict) {
		return ValidationError::InvalidStateTransition;
	}
	if (result != ReliableReceiveLookupResult::Found) {
		return result == ReliableReceiveLookupResult::InvalidKey ? ValidationError::OutOfRange
																 : ValidationError::InvalidStateTransition;
	}
	if (outcome.kind != ReliableReceiveOutcomeKind::Validated && outcome.kind != ReliableReceiveOutcomeKind::Applied) {
		return ValidationError::InvalidStateTransition;
	}

	AckPayload candidate;
	candidate.target_message_id = key.message_id;
	candidate.target_message_type = key.message_type;
	candidate.ack_flags = outcome.kind == ReliableReceiveOutcomeKind::Applied
							  ? KnownAckFlags
							  : static_cast<std::uint8_t>(AckFlag::Validated);
	candidate.target_fragment_count = key.fragment_count;
	candidate.target_message_crc32 = key.message_crc32;
	if (const auto error = validate_ack_payload(candidate); error != ValidationError::None) {
		return error;
	}
	ack = candidate;
	return ValidationError::None;
}

std::size_t ReliableReceiveCache::expire(std::uint64_t now_us) noexcept
{
	return expire_at(advance_time(now_us));
}

std::size_t ReliableReceiveCache::purge_session_preserving_end_tombstone(std::uint64_t session_id) noexcept
{
	std::size_t removed = 0;
	for (std::size_t index = 0; index < m_maximum_entries; ++index) {
		const auto& entry = m_entries[index];
		const auto keep_tombstone = entry.occupied && entry.key.message_type == MessageType::SessionEnd &&
									entry.outcome.kind == ReliableReceiveOutcomeKind::Applied;
		if (entry.occupied && entry.key.session_id == session_id && !keep_tombstone) {
			erase(index);
			++removed;
		}
	}
	return removed;
}

std::size_t ReliableReceiveCache::purge_session_preserving_end_tombstone(std::uint64_t session_id,
	const EndpointKey& endpoint) noexcept
{
	std::size_t removed = 0;
	for (std::size_t index = 0; index < m_maximum_entries; ++index) {
		const auto& entry = m_entries[index];
		const auto keep_tombstone = entry.occupied && entry.key.message_type == MessageType::SessionEnd &&
									entry.outcome.kind == ReliableReceiveOutcomeKind::Applied;
		if (entry.occupied && entry.key.session_id == session_id && entry.key.endpoint == endpoint && !keep_tombstone) {
			erase(index);
			++removed;
		}
	}
	return removed;
}

std::size_t ReliableReceiveCache::purge_session(std::uint64_t session_id) noexcept
{
	std::size_t removed = 0;
	for (std::size_t index = 0; index < m_maximum_entries; ++index) {
		if (m_entries[index].occupied && m_entries[index].key.session_id == session_id) {
			erase(index);
			++removed;
		}
	}
	return removed;
}

std::size_t ReliableReceiveCache::purge_session(std::uint64_t session_id, const EndpointKey& endpoint) noexcept
{
	std::size_t removed = 0;
	for (std::size_t index = 0; index < m_maximum_entries; ++index) {
		if (m_entries[index].occupied && m_entries[index].key.session_id == session_id &&
			m_entries[index].key.endpoint == endpoint) {
			erase(index);
			++removed;
		}
	}
	return removed;
}

void ReliableReceiveCache::clear() noexcept
{
	for (std::size_t index = 0; index < m_maximum_entries; ++index) {
		m_entries[index] = Entry{};
	}
	m_entry_count = 0;
}

std::uint64_t ReliableReceiveCache::advance_time(std::uint64_t now_us) noexcept
{
	if (now_us > m_monotonic_time_us) {
		m_monotonic_time_us = now_us;
	}
	return m_monotonic_time_us;
}

std::size_t ReliableReceiveCache::expire_at(std::uint64_t now_us) noexcept
{
	std::size_t removed = 0;
	for (std::size_t index = 0; index < m_maximum_entries; ++index) {
		if (m_entries[index].occupied && now_us >= m_entries[index].expires_at_us) {
			erase(index);
			++removed;
		}
	}
	return removed;
}

std::size_t ReliableReceiveCache::find_exact(const ReliableReceiveKey& key) const noexcept
{
	for (std::size_t index = 0; index < m_maximum_entries; ++index) {
		if (m_entries[index].occupied && same_receive_key(m_entries[index].key, key)) {
			return index;
		}
	}
	return InvalidIndex;
}

std::size_t ReliableReceiveCache::find_base_identity(const ReliableReceiveKey& key) const noexcept
{
	for (std::size_t index = 0; index < m_maximum_entries; ++index) {
		if (m_entries[index].occupied && same_receive_base(m_entries[index].key, key)) {
			return index;
		}
	}
	return InvalidIndex;
}

std::size_t ReliableReceiveCache::find_free() const noexcept
{
	for (std::size_t index = 0; index < m_maximum_entries; ++index) {
		if (!m_entries[index].occupied) {
			return index;
		}
	}
	return InvalidIndex;
}

void ReliableReceiveCache::erase(std::size_t index) noexcept
{
	if (index >= m_maximum_entries || !m_entries[index].occupied) {
		return;
	}
	m_entries[index] = Entry{};
	--m_entry_count;
}

ReliableReceiveUpdateResult ReliableReceiveCache::record(const ReliableReceiveKey& key,
	ReliableReceiveOutcome outcome) noexcept
{
	if (!valid_receive_key(key) ||
		(outcome.kind != ReliableReceiveOutcomeKind::Validated && outcome.kind != ReliableReceiveOutcomeKind::Applied &&
			outcome.kind != ReliableReceiveOutcomeKind::Error) ||
		((outcome.kind == ReliableReceiveOutcomeKind::Error) != (outcome.error != ValidationError::None))) {
		return ReliableReceiveUpdateResult::InvalidOutcome;
	}

	const auto exact = find_exact(key);
	if (exact == InvalidIndex) {
		return find_base_identity(key) == InvalidIndex ? ReliableReceiveUpdateResult::NotFound
													   : ReliableReceiveUpdateResult::IdentityConflict;
	}

	auto& current = m_entries[exact].outcome;
	// APPLIED is terminal and never regresses. Error is also terminal: an exact
	// duplicate reuses it instead of reopening parsing. VALIDATED may advance to
	// APPLIED or to a later application/semantic error.
	if (current.kind == ReliableReceiveOutcomeKind::Applied || current.kind == ReliableReceiveOutcomeKind::Error ||
		current.kind == outcome.kind) {
		return ReliableReceiveUpdateResult::Unchanged;
	}
	if (current.kind == ReliableReceiveOutcomeKind::Validated &&
		outcome.kind == ReliableReceiveOutcomeKind::Validated) {
		return ReliableReceiveUpdateResult::Unchanged;
	}
	current = outcome;
	return ReliableReceiveUpdateResult::Updated;
}

bool is_selective_nack_candidate(const TelemetryDatagramHeader& header) noexcept
{
	if (validate_fragment_layout(header) != ValidationError::None || header.fragment_count <= 1) {
		return false;
	}
	return header_declares_nack_eligible_class(header);
}

SelectiveNackObserveResult SelectiveNackTracker::observe_validated_fragment(const DatagramView& fragment,
	const EndpointKey& source_endpoint,
	std::uint64_t now_us,
	std::uint64_t base_rto_us,
	std::uint64_t needed_before_producer_time_us,
	std::uint64_t estimated_producer_time_us,
	SelectiveNackRetirements* retirements) noexcept
{
	const auto& header = fragment.header;
	if (!source_endpoint.is_valid() || header.session_id == 0 || base_rto_us < ReliableMinimumRtoUs ||
		base_rto_us > ReliableMaximumRtoUs || !fragment_payload_matches_view(fragment) ||
		validate_fragment_layout(header) != ValidationError::None) {
		return SelectiveNackObserveResult::InvalidFragment;
	}
	const auto selectively_tracked = is_selective_nack_candidate(header);
	const auto video_idr = header_declares_video_idr(header);
	if (!selectively_tracked && !video_idr) {
		return SelectiveNackObserveResult::IgnoredIneligible;
	}
	if (needed_before_producer_time_us != 0 && !video_idr) {
		return SelectiveNackObserveResult::InvalidFragment;
	}

	advance_expiration(now_us, estimated_producer_time_us, retirements);
	const auto effective_now = m_monotonic_time_us;
	const auto message_class = message_size_class(header.message_type);
	auto index = find_base(header, source_endpoint);
	if (index != InvalidIndex) {
		auto& existing = m_entries[index];
		if (existing.message_crc32 != header.message_crc32 || existing.fragment_count != header.fragment_count ||
			existing.message_size != header.message_size || existing.message_class != message_class ||
			existing.needed_before_producer_time_us != needed_before_producer_time_us) {
			erase(index, retirements);
			return SelectiveNackObserveResult::IdentityConflict;
		}
	} else {
		if (message_class == MessageSizeClass::Video) {
			const auto same_latest_stream = m_has_latest_video_idr && m_latest_video_session_id == header.session_id &&
											m_latest_video_endpoint == source_endpoint;
			if (same_latest_stream && header.message_id <= m_latest_video_message_id) {
				// Completion, expiry, or supersession leaves a session-scoped high
				// watermark. Late fragments must never reopen recovery for an old IDR.
				return SelectiveNackObserveResult::IgnoredIneligible;
			}
			for (std::size_t existing_index = 0; existing_index < m_entries.size(); ++existing_index) {
				const auto& existing = m_entries[existing_index];
				if (!existing.occupied || existing.message_class != MessageSizeClass::Video ||
					existing.session_id != header.session_id || existing.endpoint != source_endpoint) {
					continue;
				}
				if (header.message_id < existing.message_id) {
					return SelectiveNackObserveResult::IgnoredIneligible;
				}
				// v1.0 has one active stream per client. A newer IDR makes recovery
				// of every previous IDR for this peer obsolete immediately.
				erase(existing_index, retirements);
				break;
			}
		}
		const auto producer_deadline_expired = video_idr && needed_before_producer_time_us != 0 &&
											   estimated_producer_time_us != 0 &&
											   estimated_producer_time_us >= needed_before_producer_time_us;
		if (producer_deadline_expired) {
			cancel_pending_keyframe(header.session_id, source_endpoint);
			m_has_latest_video_idr = true;
			m_latest_video_session_id = header.session_id;
			m_latest_video_endpoint = source_endpoint;
			m_latest_video_message_id = header.message_id;
			ReliableMessageKey target;
			target.session_id = header.session_id;
			target.endpoint = source_endpoint;
			target.message_type = header.message_type;
			target.message_id = header.message_id;
			target.fragment_count = header.fragment_count;
			target.message_crc32 = header.message_crc32;
			queue_recovery(SelectiveNackActionKind::RequestKeyframe, target);
			return SelectiveNackObserveResult::IgnoredIneligible;
		}
		if (video_idr && header.fragment_count == 1U) {
			cancel_pending_keyframe(header.session_id, source_endpoint);
			m_has_latest_video_idr = true;
			m_latest_video_session_id = header.session_id;
			m_latest_video_endpoint = source_endpoint;
			m_latest_video_message_id = header.message_id;
			return SelectiveNackObserveResult::Completed;
		}
		const auto at_quota =
			(message_class == MessageSizeClass::State && m_state_entries >= MaxStateReassembliesPerClient) ||
			(message_class == MessageSizeClass::Video && m_video_entries >= SelectiveNackMaximumVideoEntries);
		if (message_class == MessageSizeClass::Invalid || at_quota) {
			return SelectiveNackObserveResult::ResourceLimit;
		}
		index = find_free();
		if (index == InvalidIndex) {
			return SelectiveNackObserveResult::ResourceLimit;
		}
		if (message_class == MessageSizeClass::Video) {
			// A fresh IDR satisfies any keyframe recovery queued for the prior IDR.
			cancel_pending_keyframe(header.session_id, source_endpoint);
			m_has_latest_video_idr = true;
			m_latest_video_session_id = header.session_id;
			m_latest_video_endpoint = source_endpoint;
			m_latest_video_message_id = header.message_id;
		}

		auto& created = m_entries[index];
		created = Entry{};
		created.occupied = true;
		created.session_id = header.session_id;
		created.endpoint = source_endpoint;
		created.message_type = header.message_type;
		created.message_class = message_class;
		created.message_id = header.message_id;
		created.message_crc32 = header.message_crc32;
		created.message_size = header.message_size;
		created.fragment_count = header.fragment_count;
		created.needed_before_producer_time_us = needed_before_producer_time_us;
		created.first_receive_us = effective_now;
		created.absolute_deadline_us = saturating_add(effective_now,
			message_class == MessageSizeClass::Video ? ReliableVideoIdrRetentionUs : SelectiveNackReliableRetentionUs);
		if (message_class == MessageSizeClass::Video) {
			++m_video_entries;
		} else {
			++m_state_entries;
		}
	}

	auto& entry = m_entries[index];
	const auto byte_index = static_cast<std::size_t>(header.fragment_index / 8U);
	const auto bit_mask = static_cast<std::uint8_t>(1U << (header.fragment_index & 7U));
	if ((entry.received[byte_index] & bit_mask) != 0) {
		return SelectiveNackObserveResult::Duplicate;
	}

	entry.received[byte_index] = static_cast<std::uint8_t>(entry.received[byte_index] | bit_mask);
	++entry.received_count;
	if (!entry.has_received || header.fragment_index > entry.highest_received) {
		entry.highest_received = header.fragment_index;
	}
	entry.has_received = true;

	if (entry.received_count == entry.fragment_count) {
		if (entry.message_class == MessageSizeClass::Video) {
			cancel_pending_keyframe(entry.session_id, entry.endpoint);
		}
		erase(index);
		return SelectiveNackObserveResult::Completed;
	}

	if (!has_revealed_hole(entry)) {
		entry.nack_armed = false;
		return SelectiveNackObserveResult::Accepted;
	}
	if (!entry.nack_armed) {
		const auto delay_us = std::min(SelectiveNackMaximumDelayUs, base_rto_us / 4U);
		auto due_us = saturating_add(effective_now, delay_us);
		if (entry.has_sent_nack) {
			due_us = std::max(due_us, saturating_add(entry.last_nack_us, SelectiveNackMinimumIntervalUs));
		}
		entry.nack_due_us = due_us;
		entry.nack_armed = true;
	}
	return SelectiveNackObserveResult::Accepted;
}

void SelectiveNackTracker::advance_expiration(std::uint64_t now_us,
	std::uint64_t estimated_producer_time_us,
	SelectiveNackRetirements* retirements) noexcept
{
	const auto effective_now = advance_time(now_us);
	expire_to_pending(effective_now, estimated_producer_time_us, retirements);
}

SelectiveNackPollResult SelectiveNackTracker::poll(std::uint64_t now_us,
	std::uint64_t estimated_producer_time_us,
	SelectiveNackAction& action,
	SelectiveNackRetirements* retirements) noexcept
{
	advance_expiration(now_us, estimated_producer_time_us, retirements);
	const auto effective_now = m_monotonic_time_us;
	SelectiveNackAction ready_action;
	if (pop_recovery(ready_action)) {
		action = ready_action;
		return SelectiveNackPollResult::Ready;
	}
	std::size_t selected = InvalidIndex;
	std::uint64_t selected_due = std::numeric_limits<std::uint64_t>::max();
	for (std::size_t index = 0; index < m_entries.size(); ++index) {
		const auto& entry = m_entries[index];
		if (entry.occupied && entry.nack_armed && entry.nack_due_us <= effective_now &&
			entry.nack_due_us < selected_due) {
			selected = index;
			selected_due = entry.nack_due_us;
		}
	}
	if (selected == InvalidIndex) {
		return SelectiveNackPollResult::None;
	}

	auto& entry = m_entries[selected];
	if (!has_revealed_hole(entry)) {
		entry.nack_armed = false;
		return SelectiveNackPollResult::None;
	}
	build_missing_bitmap(entry);
	NackPayload candidate;
	candidate.target_message_id = entry.message_id;
	candidate.target_message_type = entry.message_type;
	candidate.reason = NackReason::MissingFragments;
	candidate.target_fragment_count = entry.fragment_count;
	candidate.target_message_crc32 = entry.message_crc32;
	candidate.needed_before_producer_time_us = entry.needed_before_producer_time_us;
	candidate.missing_bitmap = ByteView{entry.missing.data(), missing_fragment_bitmap_size(entry.fragment_count)};
	if (validate_nack_payload(candidate) != ValidationError::None) {
		entry.nack_armed = false;
		return SelectiveNackPollResult::None;
	}

	entry.has_sent_nack = true;
	entry.last_nack_us = effective_now;
	entry.nack_due_us = saturating_add(effective_now, SelectiveNackMinimumIntervalUs);
	ready_action.kind = SelectiveNackActionKind::SendNack;
	ready_action.target.session_id = entry.session_id;
	ready_action.target.endpoint = entry.endpoint;
	ready_action.target.message_type = entry.message_type;
	ready_action.target.message_id = entry.message_id;
	ready_action.target.fragment_count = entry.fragment_count;
	ready_action.target.message_crc32 = entry.message_crc32;
	ready_action.nack = candidate;
	action = ready_action;
	return SelectiveNackPollResult::Ready;
}

bool SelectiveNackTracker::discard_active(std::uint64_t session_id,
	const EndpointKey& endpoint,
	MessageType message_type,
	std::uint32_t message_id) noexcept
{
	for (std::size_t index = 0; index < m_entries.size(); ++index) {
		const auto& entry = m_entries[index];
		if (entry.occupied && entry.session_id == session_id && entry.endpoint == endpoint &&
			entry.message_type == message_type && entry.message_id == message_id) {
			erase(index);
			return true;
		}
	}
	return false;
}

void SelectiveNackTracker::queue_keyframe_recovery(const ReliableMessageKey& target) noexcept
{
	if (target.session_id == 0 || !target.endpoint.is_valid() || target.message_type != MessageType::TargetVideoFrame ||
		target.message_id == 0) {
		return;
	}
	queue_recovery(SelectiveNackActionKind::RequestKeyframe, target);
}

bool SelectiveNackTracker::discard(std::uint64_t session_id,
	const EndpointKey& endpoint,
	MessageType message_type,
	std::uint32_t message_id) noexcept
{
	if (discard_active(session_id, endpoint, message_type, message_id)) {
		return true;
	}
	for (auto& pending : m_pending_recoveries) {
		if (pending.occupied && pending.target.session_id == session_id && pending.target.endpoint == endpoint &&
			pending.target.message_type == message_type && pending.target.message_id == message_id) {
			pending = PendingRecovery{};
			--m_pending_recovery_count;
			return true;
		}
	}
	return false;
}

std::size_t SelectiveNackTracker::purge_session(std::uint64_t session_id) noexcept
{
	std::size_t removed = 0;
	for (std::size_t index = 0; index < m_entries.size(); ++index) {
		if (m_entries[index].occupied && m_entries[index].session_id == session_id) {
			erase(index);
			++removed;
		}
	}
	for (auto& pending : m_pending_recoveries) {
		if (pending.occupied && pending.target.session_id == session_id) {
			pending = PendingRecovery{};
			--m_pending_recovery_count;
			++removed;
		}
	}
	if (m_has_latest_video_idr && m_latest_video_session_id == session_id) {
		m_has_latest_video_idr = false;
		m_latest_video_session_id = 0;
		m_latest_video_endpoint = EndpointKey{};
		m_latest_video_message_id = 0;
	}
	return removed;
}

std::size_t SelectiveNackTracker::purge_session(std::uint64_t session_id, const EndpointKey& endpoint) noexcept
{
	std::size_t removed = 0;
	for (std::size_t index = 0; index < m_entries.size(); ++index) {
		if (m_entries[index].occupied && m_entries[index].session_id == session_id &&
			m_entries[index].endpoint == endpoint) {
			erase(index);
			++removed;
		}
	}
	for (auto& pending : m_pending_recoveries) {
		if (pending.occupied && pending.target.session_id == session_id && pending.target.endpoint == endpoint) {
			pending = PendingRecovery{};
			--m_pending_recovery_count;
			++removed;
		}
	}
	if (m_has_latest_video_idr && m_latest_video_session_id == session_id && m_latest_video_endpoint == endpoint) {
		m_has_latest_video_idr = false;
		m_latest_video_session_id = 0;
		m_latest_video_endpoint = EndpointKey{};
		m_latest_video_message_id = 0;
	}
	return removed;
}

void SelectiveNackTracker::clear() noexcept
{
	for (auto& entry : m_entries) {
		entry = Entry{};
	}
	m_state_entries = 0;
	m_video_entries = 0;
	for (auto& pending : m_pending_recoveries) {
		pending = PendingRecovery{};
	}
	m_pending_recovery_count = 0;
	m_has_latest_video_idr = false;
	m_latest_video_session_id = 0;
	m_latest_video_endpoint = EndpointKey{};
	m_latest_video_message_id = 0;
}

std::size_t SelectiveNackTracker::active_entries(MessageSizeClass message_class) const noexcept
{
	switch (message_class) {
	case MessageSizeClass::State:
		return m_state_entries;
	case MessageSizeClass::Video:
		return m_video_entries;
	case MessageSizeClass::Invalid:
		return 0;
	}
	return 0;
}

std::uint64_t SelectiveNackTracker::advance_time(std::uint64_t now_us) noexcept
{
	if (now_us > m_monotonic_time_us) {
		m_monotonic_time_us = now_us;
	}
	return m_monotonic_time_us;
}

std::size_t SelectiveNackTracker::find_base(const TelemetryDatagramHeader& header,
	const EndpointKey& endpoint) const noexcept
{
	for (std::size_t index = 0; index < m_entries.size(); ++index) {
		const auto& entry = m_entries[index];
		if (entry.occupied && entry.session_id == header.session_id && entry.endpoint == endpoint &&
			entry.message_type == header.message_type && entry.message_id == header.message_id) {
			return index;
		}
	}
	return InvalidIndex;
}

std::size_t SelectiveNackTracker::find_free() const noexcept
{
	for (std::size_t index = 0; index < m_entries.size(); ++index) {
		if (!m_entries[index].occupied) {
			return index;
		}
	}
	return InvalidIndex;
}

void SelectiveNackTracker::erase(std::size_t index, SelectiveNackRetirements* retirements) noexcept
{
	if (index >= m_entries.size() || !m_entries[index].occupied) {
		return;
	}
	if (retirements != nullptr) {
		ReliableMessageKey target;
		target.session_id = m_entries[index].session_id;
		target.endpoint = m_entries[index].endpoint;
		target.message_type = m_entries[index].message_type;
		target.message_id = m_entries[index].message_id;
		target.fragment_count = m_entries[index].fragment_count;
		target.message_crc32 = m_entries[index].message_crc32;
		retirements->append(target);
	}
	if (m_entries[index].message_class == MessageSizeClass::Video) {
		--m_video_entries;
	} else if (m_entries[index].message_class == MessageSizeClass::State) {
		--m_state_entries;
	}
	m_entries[index] = Entry{};
}

bool SelectiveNackTracker::has_revealed_hole(const Entry& entry) const noexcept
{
	if (!entry.has_received || entry.highest_received == 0) {
		return false;
	}
	for (std::uint16_t index = 0; index < entry.highest_received; ++index) {
		const auto byte_index = static_cast<std::size_t>(index / 8U);
		const auto bit_mask = static_cast<std::uint8_t>(1U << (index & 7U));
		if ((entry.received[byte_index] & bit_mask) == 0) {
			return true;
		}
	}
	return false;
}

void SelectiveNackTracker::build_missing_bitmap(Entry& entry) noexcept
{
	entry.missing.fill(0);
	for (std::uint16_t index = 0; index < entry.fragment_count; ++index) {
		const auto byte_index = static_cast<std::size_t>(index / 8U);
		const auto bit_mask = static_cast<std::uint8_t>(1U << (index & 7U));
		if ((entry.received[byte_index] & bit_mask) == 0) {
			entry.missing[byte_index] = static_cast<std::uint8_t>(entry.missing[byte_index] | bit_mask);
		}
	}
}

std::size_t SelectiveNackTracker::find_expired(std::uint64_t now_us,
	std::uint64_t estimated_producer_time_us) const noexcept
{
	std::size_t selected = InvalidIndex;
	for (std::size_t index = 0; index < m_entries.size(); ++index) {
		const auto& entry = m_entries[index];
		if (!entry.occupied) {
			continue;
		}
		const auto local_expired = now_us >= entry.absolute_deadline_us;
		const auto producer_deadline_expired =
			entry.message_class == MessageSizeClass::Video && entry.needed_before_producer_time_us != 0 &&
			estimated_producer_time_us != 0 && estimated_producer_time_us >= entry.needed_before_producer_time_us;
		if ((local_expired || producer_deadline_expired) &&
			(selected == InvalidIndex || entry.absolute_deadline_us < m_entries[selected].absolute_deadline_us)) {
			selected = index;
		}
	}
	return selected;
}

void SelectiveNackTracker::expire_to_pending(std::uint64_t now_us,
	std::uint64_t estimated_producer_time_us,
	SelectiveNackRetirements* retirements) noexcept
{
	for (;;) {
		const auto index = find_expired(now_us, estimated_producer_time_us);
		if (index == InvalidIndex) {
			return;
		}
		const auto& entry = m_entries[index];
		ReliableMessageKey target;
		target.session_id = entry.session_id;
		target.endpoint = entry.endpoint;
		target.message_type = entry.message_type;
		target.message_id = entry.message_id;
		target.fragment_count = entry.fragment_count;
		target.message_crc32 = entry.message_crc32;
		queue_recovery(entry.message_class == MessageSizeClass::Video ? SelectiveNackActionKind::RequestKeyframe
																	  : SelectiveNackActionKind::RequestResync,
			target);
		erase(index, retirements);
	}
}

void SelectiveNackTracker::queue_recovery(SelectiveNackActionKind kind, const ReliableMessageKey& target) noexcept
{
	for (const auto& pending : m_pending_recoveries) {
		if (pending.occupied && pending.kind == kind && pending.target.session_id == target.session_id &&
			pending.target.endpoint == target.endpoint) {
			return;
		}
	}
	for (auto& pending : m_pending_recoveries) {
		if (!pending.occupied) {
			pending.occupied = true;
			pending.kind = kind;
			pending.target = target;
			++m_pending_recovery_count;
			return;
		}
	}
	if (kind == SelectiveNackActionKind::RequestResync) {
		for (auto& pending : m_pending_recoveries) {
			if (pending.kind == SelectiveNackActionKind::RequestKeyframe) {
				pending.kind = kind;
				pending.target = target;
				return;
			}
		}
	}
}

void SelectiveNackTracker::cancel_pending_keyframe(std::uint64_t session_id, const EndpointKey& endpoint) noexcept
{
	for (auto& pending : m_pending_recoveries) {
		if (pending.occupied && pending.kind == SelectiveNackActionKind::RequestKeyframe &&
			pending.target.session_id == session_id && pending.target.endpoint == endpoint) {
			pending = PendingRecovery{};
			--m_pending_recovery_count;
		}
	}
}

bool SelectiveNackTracker::pop_recovery(SelectiveNackAction& action) noexcept
{
	for (auto& pending : m_pending_recoveries) {
		if (pending.occupied) {
			SelectiveNackAction ready;
			ready.kind = pending.kind;
			ready.target = pending.target;
			pending = PendingRecovery{};
			--m_pending_recovery_count;
			action = ready;
			return true;
		}
	}
	return false;
}

ReliableReceivePipelineResult ReliableReceivePipeline::ingest_validated_fragment(const DatagramView& fragment,
	const EndpointKey& source_endpoint,
	std::uint64_t now_us,
	std::uint64_t base_rto_us,
	ReassembledMessage& completed,
	std::uint64_t needed_before_producer_time_us,
	std::uint64_t estimated_producer_time_us)
{
	ReliableReceivePipelineResult result;
	if (!source_endpoint.is_valid()) {
		result.tracking = SelectiveNackObserveResult::InvalidFragment;
		return result;
	}

	// Expiration is processed before admission so retired buffers cannot make a
	// new, otherwise valid fragment fail quota. Every owner eviction is applied,
	// independently of the (possibly coalesced) recovery action sent on the wire.
	SelectiveNackRetirements expired;
	m_tracker.advance_expiration(now_us, estimated_producer_time_us, &expired);
	apply_retirements(expired);
	expire_lifetimes(m_tracker.monotonic_time_us());

	const auto incoming = key_for(fragment.header, source_endpoint);
	const auto selectively_tracked = is_selective_nack_candidate(fragment.header);
	const auto video_idr = header_declares_video_idr(fragment.header);
	const auto silent_class = silent_message_class(fragment.header);
	SelectiveNackRetirements observed_retirements;
	result.tracking = m_tracker.observe_validated_fragment(fragment,
		source_endpoint,
		now_us,
		base_rto_us,
		needed_before_producer_time_us,
		estimated_producer_time_us,
		&observed_retirements);
	apply_retirements(observed_retirements);

	switch (result.tracking) {
	case SelectiveNackObserveResult::InvalidFragment:
	case SelectiveNackObserveResult::ResourceLimit:
		return result;
	case SelectiveNackObserveResult::IdentityConflict:
		// The exact retired identity normally removed the allocation above. This
		// base-key cleanup is defensive and leaves no contradictory partial state.
		discard_reassembly(incoming);
		recover_failed_idr(incoming, video_idr);
		return result;
	case SelectiveNackObserveResult::IgnoredIneligible:
		// Non-reliable state and video interframes still use the same reassembler
		// and quotas. A selectively eligible IDR returning Ignored is instead a
		// stale/high-watermark rejection and must not reserve bytes again.
		if (selectively_tracked || video_idr) {
			if (video_idr) {
				record_silent_terminal(SilentMessageClass::VideoInterframe, incoming);
			}
			return result;
		}
		break;
	case SelectiveNackObserveResult::Accepted:
	case SelectiveNackObserveResult::Duplicate:
	case SelectiveNackObserveResult::Completed:
		break;
	}
	if (silent_high_watermark_blocks(silent_class, incoming)) {
		return result;
	}

	// One pipeline belongs to one authenticated endpoint. Do not let a caller
	// presenting another endpoint erase an existing (session,message_id) owner;
	// the contextual stage is expected to reject this earlier as well.
	const auto storage_index_before = find_lifetime_storage_base(incoming.session_id, incoming.message_id);
	if (storage_index_before != InvalidIndex && m_lifetimes[storage_index_before].key.endpoint != incoming.endpoint) {
		m_tracker.discard_active(incoming.session_id, incoming.endpoint, incoming.message_type, incoming.message_id);
		result.reassembly = ReassemblyResult::InconsistentFragment;
		return result;
	}

	result.reassembly_attempted = true;
	ReassembledMessage candidate;
	result.reassembly = m_reassembler.ingest(fragment, candidate);
	const auto reassembly_failed = result.reassembly == ReassemblyResult::InvalidLayout ||
								   result.reassembly == ReassemblyResult::QuotaExceeded ||
								   result.reassembly == ReassemblyResult::InconsistentFragment ||
								   result.reassembly == ReassemblyResult::MessageCrcMismatch ||
								   result.reassembly == ReassemblyResult::AllocationFailed;
	const auto tracker_managed = selectively_tracked || video_idr;
	const auto coherent =
		!tracker_managed || reassembly_failed ||
		(result.tracking == SelectiveNackObserveResult::Accepted && result.reassembly == ReassemblyResult::Accepted) ||
		(result.tracking == SelectiveNackObserveResult::Duplicate &&
			result.reassembly == ReassemblyResult::Duplicate) ||
		(result.tracking == SelectiveNackObserveResult::Completed && result.reassembly == ReassemblyResult::Completed);
	if (!coherent) {
		// Never expose a Completed payload unless both owners completed the same
		// logical identity. The reassembler writes only into the local candidate.
		m_tracker.discard_active(incoming.session_id, incoming.endpoint, incoming.message_type, incoming.message_id);
		discard_storage_base(incoming.session_id, incoming.message_id);
		recover_failed_idr(incoming, video_idr);
		record_silent_terminal(silent_class, incoming);
		result.reassembly = ReassemblyResult::InconsistentFragment;
		return result;
	}

	const auto exact_lifetime = [&]() noexcept {
		const auto index = find_lifetime_base(incoming);
		return index != InvalidIndex && m_lifetimes[index].key.fragment_count == incoming.fragment_count &&
			   m_lifetimes[index].key.message_crc32 == incoming.message_crc32 &&
			   m_lifetimes[index].message_size == fragment.header.message_size;
	};

	switch (result.reassembly) {
	case ReassemblyResult::Accepted: {
		auto lifetime_index = find_lifetime_base(incoming);
		if (lifetime_index == InvalidIndex) {
			lifetime_index = find_free_lifetime();
			if (lifetime_index == InvalidIndex) {
				m_tracker.discard_active(incoming.session_id,
					incoming.endpoint,
					incoming.message_type,
					incoming.message_id);
				m_reassembler.discard(incoming.session_id, incoming.message_id);
				recover_failed_idr(incoming, video_idr);
				record_silent_terminal(silent_class, incoming);
				result.reassembly = ReassemblyResult::QuotaExceeded;
				return result;
			}
			auto& lifetime = m_lifetimes[lifetime_index];
			lifetime.occupied = true;
			lifetime.key = incoming;
			lifetime.message_size = fragment.header.message_size;
			lifetime.expires_at_us =
				saturating_add(m_tracker.monotonic_time_us(), reassembly_retention_us(fragment.header));
			lifetime.selectively_tracked = selectively_tracked;
			lifetime.silent_class = silent_class;
		} else if (!exact_lifetime()) {
			m_tracker.discard_active(incoming.session_id,
				incoming.endpoint,
				incoming.message_type,
				incoming.message_id);
			discard_storage_base(incoming.session_id, incoming.message_id);
			recover_failed_idr(incoming, video_idr);
			record_silent_terminal(silent_class, incoming);
			result.reassembly = ReassemblyResult::InconsistentFragment;
		}
		break;
	}
	case ReassemblyResult::Duplicate:
		if (!exact_lifetime()) {
			m_tracker.discard_active(incoming.session_id,
				incoming.endpoint,
				incoming.message_type,
				incoming.message_id);
			discard_storage_base(incoming.session_id, incoming.message_id);
			recover_failed_idr(incoming, video_idr);
			record_silent_terminal(silent_class, incoming);
			result.reassembly = ReassemblyResult::InconsistentFragment;
		}
		break;
	case ReassemblyResult::Completed: {
		const auto lifetime_index = find_lifetime_base(incoming);
		if (fragment.header.fragment_count > 1U && (lifetime_index == InvalidIndex || !exact_lifetime())) {
			m_tracker.discard_active(incoming.session_id,
				incoming.endpoint,
				incoming.message_type,
				incoming.message_id);
			discard_storage_base(incoming.session_id, incoming.message_id);
			recover_failed_idr(incoming, video_idr);
			record_silent_terminal(silent_class, incoming);
			result.reassembly = ReassemblyResult::InconsistentFragment;
			return result;
		}
		if (lifetime_index != InvalidIndex) {
			erase_lifetime(lifetime_index);
		}
		record_silent_terminal(silent_class, incoming);
		if (video_idr) {
			record_silent_terminal(SilentMessageClass::VideoInterframe, incoming);
		}
		completed = std::move(candidate);
		break;
	}
	case ReassemblyResult::InvalidLayout:
	case ReassemblyResult::QuotaExceeded:
	case ReassemblyResult::InconsistentFragment:
	case ReassemblyResult::MessageCrcMismatch:
	case ReassemblyResult::AllocationFailed:
		m_tracker.discard_active(incoming.session_id, incoming.endpoint, incoming.message_type, incoming.message_id);
		discard_storage_base(incoming.session_id, incoming.message_id);
		recover_failed_idr(incoming, video_idr);
		record_silent_terminal(silent_class, incoming);
		break;
	}
	return result;
}

SelectiveNackPollResult ReliableReceivePipeline::poll(std::uint64_t now_us,
	std::uint64_t estimated_producer_time_us,
	SelectiveNackAction& action) noexcept
{
	SelectiveNackRetirements expired;
	m_tracker.advance_expiration(now_us, estimated_producer_time_us, &expired);
	apply_retirements(expired);
	expire_lifetimes(m_tracker.monotonic_time_us());
	SelectiveNackRetirements polled_retirements;
	const auto result = m_tracker.poll(now_us, estimated_producer_time_us, action, &polled_retirements);
	apply_retirements(polled_retirements);
	return result;
}

bool ReliableReceivePipeline::discard(std::uint64_t session_id,
	const EndpointKey& endpoint,
	MessageType message_type,
	std::uint32_t message_id) noexcept
{
	ReliableMessageKey key;
	key.session_id = session_id;
	key.endpoint = endpoint;
	key.message_type = message_type;
	key.message_id = message_id;
	const auto tracking_discarded = m_tracker.discard(session_id, endpoint, message_type, message_id);
	const auto lifetime_index = find_lifetime_base(key);
	if (lifetime_index == InvalidIndex) {
		return tracking_discarded;
	}
	const auto lifetime = m_lifetimes[lifetime_index];
	const auto reassembly_discarded = m_reassembler.discard(session_id, message_id);
	erase_lifetime(lifetime_index);
	record_silent_terminal(lifetime.silent_class, lifetime.key);
	if (lifetime.selectively_tracked && message_type == MessageType::TargetVideoFrame) {
		record_silent_terminal(SilentMessageClass::VideoInterframe, lifetime.key);
	}
	return tracking_discarded || reassembly_discarded;
}

void ReliableReceivePipeline::clear() noexcept
{
	m_tracker.clear();
	m_reassembler.clear();
	for (auto& lifetime : m_lifetimes) {
		lifetime = ReassemblyLifetime{};
	}
	for (auto& high_watermark : m_silent_high_watermarks) {
		high_watermark = SilentHighWatermark{};
	}
}

ReliableMessageKey ReliableReceivePipeline::key_for(const TelemetryDatagramHeader& header,
	const EndpointKey& endpoint) noexcept
{
	ReliableMessageKey key;
	key.session_id = header.session_id;
	key.endpoint = endpoint;
	key.message_type = header.message_type;
	key.message_id = header.message_id;
	key.fragment_count = header.fragment_count;
	key.message_crc32 = header.message_crc32;
	return key;
}

std::uint64_t ReliableReceivePipeline::reassembly_retention_us(const TelemetryDatagramHeader& header) noexcept
{
	if (header.message_type == MessageType::Delta) {
		return ReplaceableStateReassemblyRetentionUs;
	}
	if (header.message_type == MessageType::TargetVideoFrame) {
		return has_message_flag(header.flags, MessageFlagVideoIdr) ? ReliableVideoIdrRetentionUs
																   : VideoInterframeReassemblyRetentionUs;
	}
	// EventBatch delivery class is payload-defined and unavailable while the
	// message is incomplete. The negotiated reliable timeout is the bounded v1
	// fallback for that class as well as Manifest/FullSnapshot.
	return SelectiveNackReliableRetentionUs;
}

ReliableReceivePipeline::SilentMessageClass ReliableReceivePipeline::silent_message_class(
	const TelemetryDatagramHeader& header) noexcept
{
	if (header.message_type == MessageType::Delta && !has_message_flag(header.flags, MessageFlagAckRequired)) {
		return SilentMessageClass::Delta;
	}
	if (header.message_type == MessageType::EventBatch && !has_message_flag(header.flags, MessageFlagAckRequired)) {
		return SilentMessageClass::ReplaceableEvent;
	}
	if (header.message_type == MessageType::TargetVideoFrame && !has_message_flag(header.flags, MessageFlagVideoIdr)) {
		return SilentMessageClass::VideoInterframe;
	}
	return SilentMessageClass::Invalid;
}

std::size_t ReliableReceivePipeline::silent_high_watermark_index(SilentMessageClass message_class) noexcept
{
	if (message_class == SilentMessageClass::Invalid) {
		return InvalidIndex;
	}
	return static_cast<std::size_t>(message_class) - 1U;
}

bool ReliableReceivePipeline::silent_high_watermark_blocks(SilentMessageClass message_class,
	const ReliableMessageKey& key) const noexcept
{
	const auto index = silent_high_watermark_index(message_class);
	if (index >= m_silent_high_watermarks.size()) {
		return false;
	}
	const auto& high_watermark = m_silent_high_watermarks[index];
	return high_watermark.occupied && high_watermark.session_id == key.session_id &&
		   high_watermark.endpoint == key.endpoint && key.message_id <= high_watermark.message_id;
}

void ReliableReceivePipeline::record_silent_terminal(SilentMessageClass message_class,
	const ReliableMessageKey& key) noexcept
{
	const auto index = silent_high_watermark_index(message_class);
	if (index >= m_silent_high_watermarks.size() || key.session_id == 0 || !key.endpoint.is_valid() ||
		key.message_id == 0) {
		return;
	}
	auto& high_watermark = m_silent_high_watermarks[index];
	if (!high_watermark.occupied || high_watermark.session_id != key.session_id ||
		high_watermark.endpoint != key.endpoint) {
		high_watermark.occupied = true;
		high_watermark.session_id = key.session_id;
		high_watermark.endpoint = key.endpoint;
		high_watermark.message_id = key.message_id;
	} else if (key.message_id > high_watermark.message_id) {
		high_watermark.message_id = key.message_id;
	}

	// A terminal newer replaceable message makes every older partial assembly
	// of the same class obsolete immediately. Reliable ACK_REQUIRED lifetimes
	// have SilentMessageClass::Invalid and are never removed by this path.
	for (std::size_t lifetime_index = 0; lifetime_index < m_lifetimes.size(); ++lifetime_index) {
		const auto lifetime = m_lifetimes[lifetime_index];
		if (!lifetime.occupied || lifetime.silent_class != message_class ||
			lifetime.key.session_id != high_watermark.session_id || lifetime.key.endpoint != high_watermark.endpoint ||
			lifetime.key.message_id > high_watermark.message_id) {
			continue;
		}
		m_reassembler.discard(lifetime.key.session_id, lifetime.key.message_id);
		erase_lifetime(lifetime_index);
	}
}

std::size_t ReliableReceivePipeline::find_lifetime_base(const ReliableMessageKey& key) const noexcept
{
	for (std::size_t index = 0; index < m_lifetimes.size(); ++index) {
		const auto& lifetime = m_lifetimes[index];
		if (lifetime.occupied && lifetime.key.session_id == key.session_id && lifetime.key.endpoint == key.endpoint &&
			lifetime.key.message_type == key.message_type && lifetime.key.message_id == key.message_id) {
			return index;
		}
	}
	return InvalidIndex;
}

std::size_t ReliableReceivePipeline::find_lifetime_storage_base(std::uint64_t session_id,
	std::uint32_t message_id) const noexcept
{
	for (std::size_t index = 0; index < m_lifetimes.size(); ++index) {
		const auto& lifetime = m_lifetimes[index];
		if (lifetime.occupied && lifetime.key.session_id == session_id && lifetime.key.message_id == message_id) {
			return index;
		}
	}
	return InvalidIndex;
}

std::size_t ReliableReceivePipeline::find_free_lifetime() const noexcept
{
	for (std::size_t index = 0; index < m_lifetimes.size(); ++index) {
		if (!m_lifetimes[index].occupied) {
			return index;
		}
	}
	return InvalidIndex;
}

void ReliableReceivePipeline::erase_lifetime(std::size_t index) noexcept
{
	if (index < m_lifetimes.size()) {
		m_lifetimes[index] = ReassemblyLifetime{};
	}
}

void ReliableReceivePipeline::discard_reassembly(const ReliableMessageKey& key) noexcept
{
	const auto lifetime_index = find_lifetime_base(key);
	if (lifetime_index == InvalidIndex) {
		return;
	}
	m_reassembler.discard(key.session_id, key.message_id);
	erase_lifetime(lifetime_index);
}

void ReliableReceivePipeline::discard_storage_base(std::uint64_t session_id, std::uint32_t message_id) noexcept
{
	const auto lifetime_index = find_lifetime_storage_base(session_id, message_id);
	if (lifetime_index != InvalidIndex) {
		const auto key = m_lifetimes[lifetime_index].key;
		m_tracker.discard_active(key.session_id, key.endpoint, key.message_type, key.message_id);
		erase_lifetime(lifetime_index);
	}
	m_reassembler.discard(session_id, message_id);
}

void ReliableReceivePipeline::apply_retirements(const SelectiveNackRetirements& retirements) noexcept
{
	for (std::size_t index = 0; index < retirements.count; ++index) {
		if (retirements.targets[index].message_type == MessageType::TargetVideoFrame) {
			record_silent_terminal(SilentMessageClass::VideoInterframe, retirements.targets[index]);
		}
		discard_reassembly(retirements.targets[index]);
	}
}

void ReliableReceivePipeline::expire_lifetimes(std::uint64_t now_us) noexcept
{
	for (std::size_t index = 0; index < m_lifetimes.size(); ++index) {
		const auto lifetime = m_lifetimes[index];
		if (!lifetime.occupied || lifetime.selectively_tracked || now_us < lifetime.expires_at_us) {
			continue;
		}
		m_reassembler.discard(lifetime.key.session_id, lifetime.key.message_id);
		erase_lifetime(index);
		record_silent_terminal(lifetime.silent_class, lifetime.key);
	}
}

void ReliableReceivePipeline::recover_failed_idr(const ReliableMessageKey& key, bool video_idr) noexcept
{
	if (video_idr && key.message_type == MessageType::TargetVideoFrame) {
		record_silent_terminal(SilentMessageClass::VideoInterframe, key);
		m_tracker.queue_keyframe_recovery(key);
	}
}

ValidationError make_context_validated_error_nack(const TelemetryDatagramHeader& target,
	const EndpointKey& source_endpoint,
	const TelemetrySessionContext& session_context,
	NackReason reason,
	std::uint64_t needed_before_producer_time_us,
	NackPayload& nack) noexcept
{
	if (session_context.active_session_id == 0) {
		return ValidationError::SessionMismatch;
	}
	if (const auto error = validate_received_datagram_context(target, source_endpoint, session_context);
		error != ValidationError::None) {
		return error;
	}
	if ((target.flags & ReservedMessageFlags) != 0) {
		return ValidationError::ReservedHeaderFlag;
	}
	if (!header_declares_nack_eligible_class(target)) {
		return ValidationError::InvalidStateTransition;
	}
	// Video IDRs are selective-retransmission only: the receiver may report
	// MissingFragments, or request a fresh keyframe after local expiry, but must
	// never turn an IDR parse/layout/deadline error into a terminal NACK.
	if (target.message_type == MessageType::TargetVideoFrame) {
		return ValidationError::InvalidStateTransition;
	}
	switch (reason) {
	case NackReason::BadMessageCrc:
	case NackReason::StaleBaseline:
	case NackReason::BadFragmentLayout:
	case NackReason::ResourceLimit:
	case NackReason::SemanticValidationFailed:
	case NackReason::DeadlineExpired:
		break;
	case NackReason::Invalid:
	case NackReason::MissingFragments:
	case NackReason::UnsupportedMessage:
		return ValidationError::InvalidStateTransition;
	}

	NackPayload candidate;
	candidate.target_message_id = target.message_id;
	candidate.target_message_type = target.message_type;
	candidate.reason = reason;
	candidate.target_fragment_count = target.fragment_count;
	candidate.target_message_crc32 = target.message_crc32;
	candidate.needed_before_producer_time_us = needed_before_producer_time_us;
	if (const auto error = validate_nack_payload(candidate); error != ValidationError::None) {
		return error;
	}
	nack = candidate;
	return ValidationError::None;
}

ValidationError make_context_validated_unsupported_message_nack(const TelemetryDatagramHeader& target,
	const EndpointKey& source_endpoint,
	const TelemetrySessionContext& session_context,
	NackPayload& nack) noexcept
{
	if (session_context.active_session_id == 0) {
		return ValidationError::SessionMismatch;
	}
	const auto context_result = validate_received_datagram_context(target, source_endpoint, session_context);
	if (context_result != ValidationError::UnknownMessageType) {
		return context_result == ValidationError::None ? ValidationError::InvalidStateTransition : context_result;
	}
	if (target.message_type == MessageType::Invalid || is_known_message_type(target.message_type)) {
		return ValidationError::InvalidStateTransition;
	}
	if ((target.flags & ReservedMessageFlags) != 0) {
		return ValidationError::ReservedHeaderFlag;
	}
	if (!has_message_flag(target.flags, MessageFlagAckRequired)) {
		return ValidationError::InvalidStateTransition;
	}

	NackPayload candidate;
	candidate.target_message_id = target.message_id;
	candidate.target_message_type = target.message_type;
	candidate.reason = NackReason::UnsupportedMessage;
	candidate.target_fragment_count = target.fragment_count;
	candidate.target_message_crc32 = target.message_crc32;
	if (const auto error = validate_nack_payload(candidate); error != ValidationError::None) {
		return error;
	}
	nack = candidate;
	return ValidationError::None;
}

} // namespace telemetry::protocol
