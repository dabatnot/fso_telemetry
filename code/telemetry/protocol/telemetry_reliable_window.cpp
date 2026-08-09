#include "telemetry/protocol/telemetry_reliable_window.h"

#include "telemetry/protocol/telemetry_fragmenter.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace telemetry::protocol {

namespace {

constexpr std::size_t NoEntry = std::numeric_limits<std::size_t>::max();

bool checked_add_size(std::size_t left, std::size_t right, std::size_t& result) noexcept
{
	if (right > std::numeric_limits<std::size_t>::max() - left) return false;
	result = left + right;
	return true;
}

bool checked_multiply_size(std::size_t left, std::size_t right, std::size_t& result) noexcept
{
	if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left) return false;
	result = left * right;
	return true;
}

void increment_saturated(std::uint64_t& value) noexcept
{
	if (value != std::numeric_limits<std::uint64_t>::max()) {
		++value;
	}
}

std::uint64_t mix64(std::uint64_t value) noexcept
{
	value += 0x9e3779b97f4a7c15ULL;
	value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
	value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
	return value ^ (value >> 31U);
}

bool checked_add(std::uint64_t left, std::uint64_t right, std::uint64_t& result) noexcept
{
	if (right > std::numeric_limits<std::uint64_t>::max() - left) {
		return false;
	}
	result = left + right;
	return true;
}

std::uint64_t
bounded_retry_time(std::uint64_t now_us, std::uint64_t delay_us, std::uint64_t absolute_deadline_us) noexcept
{
	if (now_us >= absolute_deadline_us || delay_us >= absolute_deadline_us - now_us) {
		return absolute_deadline_us;
	}
	return now_us + delay_us;
}

std::uint64_t retention_duration_us(ReliableMessageClass message_class) noexcept
{
	switch (message_class) {
	case ReliableMessageClass::Transaction:
		return ReliableTransactionRetentionUs;
	case ReliableMessageClass::VideoIdr:
		return ReliableVideoIdrRetentionUs;
	case ReliableMessageClass::ControlDrop:
	case ReliableMessageClass::ControlRequestKeyframe:
	case ReliableMessageClass::ControlRequestResync:
	case ReliableMessageClass::SessionCritical:
	case ReliableMessageClass::SessionClosing:
	case ReliableMessageClass::ReliableEvent:
	case ReliableMessageClass::HandshakeCritical:
	case ReliableMessageClass::HelloNegotiation:
		return ReliableOrdinaryRetentionUs;
	default:
		return 0;
	}
}

std::uint8_t scheduler_priority(ReliableMessageClass message_class) noexcept
{
	switch (message_class) {
	case ReliableMessageClass::Transaction:
	case ReliableMessageClass::ReliableEvent:
		return 1;
	case ReliableMessageClass::VideoIdr:
		return 3;
	case ReliableMessageClass::ControlDrop:
	case ReliableMessageClass::ControlRequestKeyframe:
	case ReliableMessageClass::ControlRequestResync:
	case ReliableMessageClass::SessionCritical:
	case ReliableMessageClass::SessionClosing:
	case ReliableMessageClass::HandshakeCritical:
	case ReliableMessageClass::HelloNegotiation:
	default:
		return 0;
	}
}

ReliableTerminalPolicy terminal_policy(ReliableMessageClass message_class) noexcept
{
	switch (message_class) {
	case ReliableMessageClass::ControlRequestKeyframe:
		return ReliableTerminalPolicy::RequestKeyframe;
	case ReliableMessageClass::ControlRequestResync:
	case ReliableMessageClass::Transaction:
	case ReliableMessageClass::ReliableEvent:
		return ReliableTerminalPolicy::RequestResync;
	case ReliableMessageClass::SessionCritical:
		return ReliableTerminalPolicy::MarkSessionStale;
	case ReliableMessageClass::SessionClosing:
	case ReliableMessageClass::HandshakeCritical:
	case ReliableMessageClass::HelloNegotiation:
		return ReliableTerminalPolicy::CloseSession;
	case ReliableMessageClass::ControlDrop:
	case ReliableMessageClass::VideoIdr:
	default:
		return ReliableTerminalPolicy::Drop;
	}
}

bool class_matches_type(ReliableMessageClass message_class, MessageType message_type) noexcept
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
	case ReliableMessageClass::HandshakeCritical:
		return message_type == MessageType::Welcome;
	case ReliableMessageClass::HelloNegotiation:
		return message_type == MessageType::Hello;
	case ReliableMessageClass::SessionClosing:
		return message_type == MessageType::SessionEnd;
	case ReliableMessageClass::Transaction:
		return message_type == MessageType::Manifest || message_type == MessageType::FullSnapshot;
	case ReliableMessageClass::ReliableEvent:
		return message_type == MessageType::EventBatch;
	case ReliableMessageClass::VideoIdr:
		return message_type == MessageType::TargetVideoFrame;
	default:
		return false;
	}
}

RequiredAckLevel required_ack_for(ReliableMessageClass message_class) noexcept
{
	switch (message_class) {
	case ReliableMessageClass::ControlRequestKeyframe:
	case ReliableMessageClass::ControlRequestResync:
		return RequiredAckLevel::Validated;
	case ReliableMessageClass::VideoIdr:
	case ReliableMessageClass::HelloNegotiation:
		return RequiredAckLevel::None;
	case ReliableMessageClass::ControlDrop:
	case ReliableMessageClass::SessionCritical:
	case ReliableMessageClass::SessionClosing:
	case ReliableMessageClass::Transaction:
	case ReliableMessageClass::ReliableEvent:
	case ReliableMessageClass::HandshakeCritical:
	default:
		return RequiredAckLevel::Applied;
	}
}

bool flags_match(const ReliableMessageToRetain& message) noexcept
{
	if ((message.base_flags & ReservedMessageFlags) != 0 || (message.base_flags & MessageFlagRetransmission) != 0) {
		return false;
	}

	const auto fragmented = message.fragment_count > 1;
	if (((message.base_flags & MessageFlagFragmented) != 0) != fragmented) {
		return false;
	}

	const auto ack_required = message.required_ack != RequiredAckLevel::None;
	if (((message.base_flags & MessageFlagAckRequired) != 0) != ack_required) {
		return false;
	}

	const auto keyframe = (message.base_flags & MessageFlagKeyframe) != 0;
	const auto video_idr = (message.base_flags & MessageFlagVideoIdr) != 0;
	if (keyframe && video_idr) {
		return false;
	}
	if (keyframe != (message.message_type == MessageType::FullSnapshot)) {
		return false;
	}
	if (video_idr != (message.message_class == ReliableMessageClass::VideoIdr)) {
		return false;
	}
	return !video_idr || message.message_type == MessageType::TargetVideoFrame;
}

bool context_fields_match(const ReliableMessageToRetain& message) noexcept
{
	const auto carries_capture =
		message.message_type == MessageType::FullSnapshot || message.message_type == MessageType::EventBatch;
	if (carries_capture) {
		return message.frame_id != 0;
	}
	return message.frame_id == 0 && message.mission_time_us == 0;
}

bool validate_message_before_copy(const ReliableMessageToRetain& message) noexcept
{
	const auto is_pre_session_hello = message.message_class == ReliableMessageClass::HelloNegotiation;
	const auto is_transaction = message.message_class == ReliableMessageClass::Transaction;
	if ((is_pre_session_hello ? message.session_id != 0 : message.session_id == 0) || !message.endpoint.is_valid() ||
		message.message_id == 0 || (message.logical_payload.size != 0 && message.logical_payload.data == nullptr) ||
		!class_matches_type(message.message_class, message.message_type) ||
		message.required_ack != required_ack_for(message.message_class) || !flags_match(message) ||
		!context_fields_match(message) ||
		(is_transaction ? message.transaction_id == 0
						: message.transaction_id != 0 || message.transaction_sha256 != Sha256Digest{})) {
		return false;
	}
	if (message.fragment_count > 1 && !message_type_allows_fragmentation(message.message_type)) {
		return false;
	}

	TelemetryFragmenter fragmenter;
	if (!TelemetryFragmenter::create(message.logical_payload, message_size_class(message.message_type), fragmenter)) {
		return false;
	}
	return fragmenter.fragment_count() == message.fragment_count && fragmenter.message_crc32() == message.message_crc32;
}

bool byte_views_equal(ByteView left, const std::vector<std::uint8_t>& right) noexcept
{
	if (left.size != right.size()) {
		return false;
	}
	return left.size == 0 || std::equal(left.begin(), left.end(), right.begin());
}

} // namespace

ValidationError ReliableFragmentSelection::is_selected(std::uint16_t fragment_index, bool& selected) const noexcept
{
	if (fragment_count == 0 || fragment_count > MaxVideoFragments) {
		return ValidationError::BadFragmentCount;
	}
	if (fragment_index >= fragment_count) {
		return ValidationError::BadFragmentIndex;
	}
	if (all_fragments) {
		if (bitmap_bytes != 0) {
			return ValidationError::BadFragmentSlice;
		}
		selected = true;
		return ValidationError::None;
	}

	const auto view = ByteView{bitmap.data(), bitmap_bytes};
	if (validate_missing_fragment_bitmap(view, fragment_count) != ValidationError::None) {
		return ValidationError::BadFragmentSlice;
	}
	selected = (bitmap[fragment_index / 8U] & static_cast<std::uint8_t>(1U << (fragment_index & 7U))) != 0;
	return ValidationError::None;
}

std::uint64_t reliable_base_rto_us(bool has_valid_minimum_rtt, std::uint64_t minimum_rtt_us) noexcept
{
	if (!has_valid_minimum_rtt) {
		return ReliableDefaultRtoUs;
	}
	if (minimum_rtt_us <= ReliableMinimumRtoUs / 2U) {
		return ReliableMinimumRtoUs;
	}
	if (minimum_rtt_us >= ReliableMaximumRtoUs / 2U) {
		return ReliableMaximumRtoUs;
	}
	return minimum_rtt_us * 2U;
}

std::uint64_t reliable_retry_delay_us(std::uint64_t base_rto_us,
	std::uint64_t jitter_seed,
	std::uint64_t session_id,
	std::uint32_t message_id,
	std::uint32_t retry_number) noexcept
{
	auto delay_us = std::max(ReliableMinimumRtoUs, std::min(base_rto_us, ReliableMaximumRtoUs));
	for (std::uint32_t retry = 0; retry < retry_number && delay_us < ReliableMaximumRtoUs; ++retry) {
		delay_us = std::min(delay_us * 2U, ReliableMaximumRtoUs);
	}

	auto mixed = mix64(jitter_seed ^ mix64(session_id));
	mixed ^= mix64((static_cast<std::uint64_t>(message_id) << 32U) | retry_number);
	mixed = mix64(mixed);
	const auto basis_points = 9000U + static_cast<std::uint32_t>(mixed % 2001U);
	return (delay_us * basis_points) / 10'000U;
}

void PreallocatedReliableControlWindow::configure() noexcept
{
	clear();
}

std::size_t PreallocatedReliableControlWindow::find(std::uint64_t session_id,
	const EndpointKey& endpoint,
	std::uint32_t message_id) const noexcept
{
	for (std::size_t i = 0U; i < m_entries.size(); ++i) {
		if (m_entries[i].used && m_entries[i].key.session_id == session_id &&
			m_entries[i].key.endpoint == endpoint && m_entries[i].key.message_id == message_id) {
			return i;
		}
	}
	return NoEntry;
}

std::size_t PreallocatedReliableControlWindow::free_slot() const noexcept
{
	for (std::size_t i = 0U; i < m_entries.size(); ++i) {
		if (!m_entries[i].used) {
			return i;
		}
	}
	return NoEntry;
}

void PreallocatedReliableControlWindow::erase(std::size_t index) noexcept
{
	if (index >= m_entries.size() || !m_entries[index].used) {
		return;
	}
	m_retained_bytes -= m_entries[index].payload_size;
	m_entries[index] = Entry{};
	--m_size;
}

ReliableRetainResult PreallocatedReliableControlWindow::retain(const ReliableMessageToRetain& message,
	std::uint64_t first_send_time_us) noexcept
{
	const auto supported_control =
		(message.message_type == MessageType::Welcome &&
		 message.message_class == ReliableMessageClass::HandshakeCritical) ||
		(message.message_type == MessageType::SessionBegin &&
		 message.message_class == ReliableMessageClass::SessionCritical) ||
		(message.message_type == MessageType::SessionEnd &&
		 message.message_class == ReliableMessageClass::SessionClosing);
	if (!supported_control || !validate_message_before_copy(message) ||
		message.logical_payload.size > PayloadBytesPerEntry || message.fragment_count != 1U) {
		return ReliableRetainResult::InvalidMessage;
	}
	const auto existing = find(message.session_id, message.endpoint, message.message_id);
	if (existing != NoEntry) {
		const auto& entry = m_entries[existing];
		const auto identical = entry.key.message_type == message.message_type &&
			entry.key.message_crc32 == message.message_crc32 && entry.payload_size == message.logical_payload.size &&
			std::equal(message.logical_payload.begin(), message.logical_payload.end(), entry.payload.begin());
		return identical ? ReliableRetainResult::Duplicate : ReliableRetainResult::IdentityConflict;
	}
	const auto slot = free_slot();
	if (slot == NoEntry || message.logical_payload.size >
			(PayloadBytesPerEntry * MaximumEntries) - m_retained_bytes) {
		return ReliableRetainResult::QuotaExceeded;
	}
	std::uint64_t deadline = 0U;
	if (!checked_add(first_send_time_us, ReliableOrdinaryRetentionUs, deadline)) {
		return ReliableRetainResult::ClockOverflow;
	}
	auto& entry = m_entries[slot];
	entry = Entry{};
	entry.used = true;
	entry.key = {message.session_id,
		message.endpoint,
		message.message_type,
		message.message_id,
		message.fragment_count,
		message.message_crc32,
		message.transaction_id,
		message.transaction_sha256};
	entry.base_flags = message.base_flags;
	entry.frame_id = message.frame_id;
	entry.mission_time_us = message.mission_time_us;
	entry.required_ack = message.required_ack;
	entry.message_class = message.message_class;
	entry.payload_size = message.logical_payload.size;
	std::copy(message.logical_payload.begin(), message.logical_payload.end(), entry.payload.begin());
	entry.absolute_deadline_us = deadline;
	entry.next_retry_at_us = bounded_retry_time(first_send_time_us,
		reliable_retry_delay_us(ReliableDefaultRtoUs,
			0x4653544c5f52544fULL,
			message.session_id,
			message.message_id,
			0U),
		deadline);
	entry.fragments.all_fragments = true;
	entry.fragments.fragment_count = message.fragment_count;
	++m_size;
	m_retained_bytes += entry.payload_size;
	return ReliableRetainResult::Retained;
}

ReliableResponseResult PreallocatedReliableControlWindow::acknowledge(std::uint64_t session_id,
	const EndpointKey& endpoint,
	const AckPayload& ack,
	std::uint64_t now_us) noexcept
{
	if (validate_ack_payload(ack) != ValidationError::None) {
		return ReliableResponseResult::IgnoredIncoherent;
	}
	const auto index = find(session_id, endpoint, ack.target_message_id);
	if (index == NoEntry || now_us >= m_entries[index].absolute_deadline_us) {
		return ReliableResponseResult::IgnoredUnknownOrLate;
	}
	auto& entry = m_entries[index];
	const ReliabilityTargetTuple target{entry.key.message_id,
		entry.key.message_type,
		entry.key.fragment_count,
		entry.key.message_crc32,
		false};
	if (validate_ack_target(ack, target) != ValidationError::None ||
		entry.required_ack == RequiredAckLevel::None) {
		return ReliableResponseResult::IgnoredIncoherent;
	}
	const auto applied = (ack.ack_flags & static_cast<std::uint8_t>(AckFlag::Applied)) != 0U;
	if (entry.required_ack == RequiredAckLevel::Validated || applied) {
		erase(index);
		return ReliableResponseResult::Released;
	}
	if (entry.validated) {
		return ReliableResponseResult::Duplicate;
	}
	entry.validated = true;
	entry.immediate_retry = false;
	entry.fragments = ReliableFragmentSelection{};
	entry.fragments.all_fragments = true;
	entry.fragments.fragment_count = entry.key.fragment_count;
	return ReliableResponseResult::ValidatedRetained;
}

ReliableResponseResult PreallocatedReliableControlWindow::reject(std::uint64_t session_id,
	const EndpointKey& endpoint,
	const NackPayload& nack,
	std::uint64_t now_us,
	ReliableNackDecision& decision) noexcept
{
	if (validate_nack_payload(nack) != ValidationError::None) {
		return ReliableResponseResult::IgnoredIncoherent;
	}
	const auto index = find(session_id, endpoint, nack.target_message_id);
	if (index == NoEntry || now_us >= m_entries[index].absolute_deadline_us) {
		return ReliableResponseResult::IgnoredUnknownOrLate;
	}
	auto& entry = m_entries[index];
	const ReliabilityTargetTuple target{entry.key.message_id,
		entry.key.message_type,
		entry.key.fragment_count,
		entry.key.message_crc32,
		false};
	if (validate_nack_target(nack, target) != ValidationError::None ||
		entry.required_ack == RequiredAckLevel::None) {
		return ReliableResponseResult::IgnoredIncoherent;
	}
	ReliableNackDecision accepted;
	accepted.target = entry.key;
	if (entry.validated) {
		const auto terminal_after_validation = nack.reason == NackReason::StaleBaseline ||
			nack.reason == NackReason::SemanticValidationFailed || nack.reason == NackReason::DeadlineExpired;
		if (!terminal_after_validation) {
			return ReliableResponseResult::IgnoredIncoherent;
		}
		accepted.kind = ReliableNackDecisionKind::TerminalPolicy;
		accepted.terminal_policy = terminal_policy(entry.message_class);
		erase(index);
		decision = accepted;
		return ReliableResponseResult::Released;
	}
	if (nack.reason == NackReason::MissingFragments) {
		entry.immediate_retry = true;
		entry.fragments = ReliableFragmentSelection{};
		entry.fragments.fragment_count = nack.target_fragment_count;
		entry.fragments.bitmap_bytes = static_cast<std::uint16_t>(nack.missing_bitmap.size);
		std::copy(nack.missing_bitmap.begin(), nack.missing_bitmap.end(), entry.fragments.bitmap.begin());
		accepted.kind = ReliableNackDecisionKind::SelectiveRetransmissionScheduled;
		decision = accepted;
		return ReliableResponseResult::ValidatedRetained;
	}
	if (nack.reason == NackReason::BadMessageCrc || nack.reason == NackReason::BadFragmentLayout) {
		entry.immediate_retry = true;
		entry.fragments = ReliableFragmentSelection{};
		entry.fragments.all_fragments = true;
		entry.fragments.fragment_count = entry.key.fragment_count;
		accepted.kind = ReliableNackDecisionKind::FullRetransmissionScheduled;
		decision = accepted;
		return ReliableResponseResult::ValidatedRetained;
	}
	if (nack.reason == NackReason::ResourceLimit) {
		entry.immediate_retry = false;
		entry.fragments = ReliableFragmentSelection{};
		entry.fragments.all_fragments = true;
		entry.fragments.fragment_count = entry.key.fragment_count;
		accepted.kind = ReliableNackDecisionKind::WaitForScheduledRetry;
		decision = accepted;
		return ReliableResponseResult::ValidatedRetained;
	}
	accepted.kind = ReliableNackDecisionKind::TerminalPolicy;
	accepted.terminal_policy = terminal_policy(entry.message_class);
	erase(index);
	decision = accepted;
	return ReliableResponseResult::Released;
}

ReliablePullResult PreallocatedReliableControlWindow::pull_next_action(std::uint64_t now_us,
	ReliableWindowAction& action) noexcept
{
	for (std::size_t index = 0U; index < m_entries.size(); ++index) {
		auto& entry = m_entries[index];
		if (!entry.used || now_us < entry.absolute_deadline_us) {
			continue;
		}
		ReliableWindowAction ready;
		ready.kind = ReliableWindowActionKind::TerminalPolicy;
		ready.terminal_policy = terminal_policy(entry.message_class);
		ready.target = entry.key;
		erase(index);
		action = ready;
		return ReliablePullResult::Action;
	}
	for (auto& entry : m_entries) {
		if (!entry.used || entry.validated ||
			(!entry.immediate_retry && now_us < entry.next_retry_at_us)) {
			continue;
		}
		const auto nack_triggered = entry.immediate_retry;
		const auto rto_already_due = now_us >= entry.next_retry_at_us;
		ReliableWindowAction ready;
		ready.kind = ReliableWindowActionKind::Retransmit;
		ready.target = entry.key;
		ready.retransmission.key = entry.key;
		ready.retransmission.base_flags = entry.base_flags;
		ready.retransmission.frame_id = entry.frame_id;
		ready.retransmission.mission_time_us = entry.mission_time_us;
		ready.retransmission.logical_payload = {entry.payload.data(), entry.payload_size};
		ready.retransmission.fragments = entry.fragments;
		ready.retransmission.retry_number = entry.retry_number;
		ready.retransmission.absolute_deadline_us = entry.absolute_deadline_us;
		entry.immediate_retry = false;
		entry.fragments = ReliableFragmentSelection{};
		entry.fragments.all_fragments = true;
		entry.fragments.fragment_count = entry.key.fragment_count;
		if (!nack_triggered || rto_already_due) {
			if (entry.retry_number != std::numeric_limits<std::uint32_t>::max()) {
				++entry.retry_number;
			}
			entry.next_retry_at_us = bounded_retry_time(now_us,
				reliable_retry_delay_us(ReliableDefaultRtoUs,
					0x4653544c5f52544fULL,
					entry.key.session_id,
					entry.key.message_id,
					entry.retry_number),
				entry.absolute_deadline_us);
		}
		action = ready;
		return ReliablePullResult::Action;
	}
	return ReliablePullResult::None;
}

bool PreallocatedReliableControlWindow::discard(std::uint64_t session_id,
	const EndpointKey& endpoint,
	std::uint32_t message_id) noexcept
{
	const auto index = find(session_id, endpoint, message_id);
	if (index == NoEntry) {
		return false;
	}
	erase(index);
	return true;
}

void PreallocatedReliableControlWindow::clear() noexcept
{
	m_entries = {};
	m_size = 0U;
	m_retained_bytes = 0U;
}

ReliableSendWindow::ReliableSendWindow(ReliableSendWindow&& other) noexcept
{
	*this = std::move(other);
}

ReliableSendWindow& ReliableSendWindow::operator=(ReliableSendWindow&& other) noexcept
{
	if (this == &other) {
		return *this;
	}
	m_limits = other.m_limits;
	m_entries = std::move(other.m_entries);
	m_free_entries = std::move(other.m_free_entries);
	m_retained_bytes = other.m_retained_bytes;
	m_video_entry_count = other.m_video_entry_count;
	m_video_retained_bytes = other.m_video_retained_bytes;
	m_has_valid_minimum_rtt = other.m_has_valid_minimum_rtt;
	m_minimum_rtt_us = other.m_minimum_rtt_us;
	m_counters = other.m_counters;
	m_allocation_events = other.m_allocation_events;

	std::vector<Entry>{}.swap(other.m_entries);
	std::vector<Entry>{}.swap(other.m_free_entries);
	other.m_limits = ReliableWindowLimits{};
	other.m_retained_bytes = 0;
	other.m_video_entry_count = 0;
	other.m_video_retained_bytes = 0;
	other.m_has_valid_minimum_rtt = false;
	other.m_minimum_rtt_us = 0;
	other.m_counters = ReliableWindowCounters{};
	other.m_allocation_events = 0U;
	return *this;
}

ValidationError ReliableSendWindow::configure(const ReliableWindowLimits& limits, ReliableSendWindow& window) noexcept
{
	if (limits.max_entries == 0 || limits.max_entries > ReliableWindowMaximumEntries ||
		limits.max_retained_bytes == 0 || limits.max_retained_bytes > ReliableWindowMaximumRetainedBytes ||
		limits.preallocated_payload_bytes_per_entry > limits.max_retained_bytes) {
		return ValidationError::OutOfRange;
	}

	std::vector<Entry>{}.swap(window.m_entries);
	std::vector<Entry>{}.swap(window.m_free_entries);
	try {
		window.m_entries.reserve(limits.max_entries);
		if (limits.preallocated_payload_bytes_per_entry != 0U) {
			window.m_free_entries.reserve(limits.max_entries);
			for (std::size_t index = 0U; index < limits.max_entries; ++index) {
				Entry entry;
				entry.payload.reserve(limits.preallocated_payload_bytes_per_entry);
				window.m_free_entries.emplace_back(std::move(entry));
			}
		}
	} catch (const std::bad_alloc&) {
		return ValidationError::ResourceLimit;
	}
	window.m_retained_bytes = 0;
	window.m_video_entry_count = 0;
	window.m_video_retained_bytes = 0;
	window.m_limits = limits;
	window.m_has_valid_minimum_rtt = false;
	window.m_minimum_rtt_us = 0;
	window.m_counters = ReliableWindowCounters{};
	window.m_allocation_events = 0U;
	return ValidationError::None;
}

std::size_t ReliableSendWindow::preallocated_heap_bytes(std::size_t entry_count,
	std::size_t payload_bytes_per_entry) noexcept
{
	std::size_t metadata_bytes = 0U;
	std::size_t payload_bytes = 0U;
	std::size_t total = 0U;
	if (!checked_multiply_size(entry_count, sizeof(Entry), metadata_bytes) ||
		!checked_multiply_size(metadata_bytes, 2U, metadata_bytes) ||
		!checked_multiply_size(entry_count, payload_bytes_per_entry, payload_bytes) ||
		!checked_add_size(metadata_bytes, payload_bytes, total)) {
		return 0U;
	}
	return total;
}

std::size_t ReliableSendWindow::owned_preallocated_heap_bytes() const noexcept
{
	std::size_t metadata_bytes = 0U;
	std::size_t payload_bytes = 0U;
	std::size_t total = 0U;
	if (!checked_multiply_size(m_entries.capacity(), sizeof(Entry), metadata_bytes) ||
		!checked_multiply_size(m_free_entries.capacity(), sizeof(Entry), total) ||
		!checked_add_size(metadata_bytes, total, metadata_bytes)) {
		return 0U;
	}
	for (const auto& entry : m_entries) {
		if (!checked_add_size(payload_bytes, entry.payload.capacity(), payload_bytes)) return 0U;
	}
	for (const auto& entry : m_free_entries) {
		if (!checked_add_size(payload_bytes, entry.payload.capacity(), payload_bytes)) return 0U;
	}
	return checked_add_size(metadata_bytes, payload_bytes, total) ? total : 0U;
}

void ReliableSendWindow::set_minimum_rtt_us(bool valid, std::uint64_t minimum_rtt_us) noexcept
{
	m_has_valid_minimum_rtt = valid;
	m_minimum_rtt_us = valid ? minimum_rtt_us : 0;
}

std::uint64_t ReliableSendWindow::base_rto_us() const noexcept
{
	return reliable_base_rto_us(m_has_valid_minimum_rtt, m_minimum_rtt_us);
}

ReliableRetainResult ReliableSendWindow::retain(const ReliableMessageToRetain& message,
	std::uint64_t first_send_time_us)
{
	if (!validate_message_before_copy(message)) {
		increment_saturated(m_counters.invalid_rejected);
		return ReliableRetainResult::InvalidMessage;
	}

	const auto existing_index = find(message.session_id, message.endpoint, message.message_id);
	if (existing_index != NoEntry) {
		const auto& retained = m_entries[existing_index];
		const auto exact_duplicate =
			message.session_id == retained.key.session_id && message.endpoint == retained.key.endpoint &&
			message.message_type == retained.key.message_type && message.message_id == retained.key.message_id &&
			message.fragment_count == retained.key.fragment_count &&
			message.message_crc32 == retained.key.message_crc32 && message.base_flags == retained.base_flags &&
			message.frame_id == retained.frame_id && message.mission_time_us == retained.mission_time_us &&
			message.transaction_id == retained.key.transaction_id &&
			message.transaction_sha256 == retained.key.transaction_sha256 &&
			message.required_ack == retained.required_ack && message.message_class == retained.message_class &&
			byte_views_equal(message.logical_payload, retained.payload);
		return exact_duplicate ? ReliableRetainResult::Duplicate : ReliableRetainResult::IdentityConflict;
	}

	const auto duration_us = retention_duration_us(message.message_class);
	std::uint64_t absolute_deadline_us = 0;
	if (duration_us == 0 || !checked_add(first_send_time_us, duration_us, absolute_deadline_us)) {
		increment_saturated(m_counters.invalid_rejected);
		return ReliableRetainResult::ClockOverflow;
	}
	if (message.message_class == ReliableMessageClass::Transaction) {
		for (const auto& retained : m_entries) {
			if (retained.message_class != ReliableMessageClass::Transaction ||
				retained.key.session_id != message.session_id || retained.key.endpoint != message.endpoint ||
				retained.key.message_type != message.message_type) {
				continue;
			}
			if (retained.key.transaction_id == message.transaction_id &&
				retained.key.transaction_sha256 != message.transaction_sha256) {
				return ReliableRetainResult::IdentityConflict;
			}
			if (retained.key.transaction_id != message.transaction_id ||
				retained.key.transaction_sha256 != message.transaction_sha256 ||
				first_send_time_us >= retained.absolute_deadline_us) {
				return ReliableRetainResult::TransactionBusy;
			}
			// Every part shares the immutable window anchored by the first part.
			absolute_deadline_us = retained.absolute_deadline_us;
			break;
		}
	}

	const auto is_video_idr = message.message_class == ReliableMessageClass::VideoIdr;
	std::size_t replaced_video_index = NoEntry;
	if (is_video_idr) {
		for (std::size_t index = 0; index < m_entries.size(); ++index) {
			const auto& existing = m_entries[index];
			if (existing.message_class != ReliableMessageClass::VideoIdr) {
				continue;
			}
			if (existing.key.session_id == message.session_id && message.message_id <= existing.key.message_id) {
				return ReliableRetainResult::IdentityConflict;
			}
			replaced_video_index = index;
			break;
		}
		if (replaced_video_index == NoEntry &&
			(m_video_entry_count >= ReliableWindowMaximumVideoEntries ||
				message.logical_payload.size > ReliableWindowMaximumVideoRetainedBytes)) {
			increment_saturated(m_counters.quota_rejected);
			return ReliableRetainResult::QuotaExceeded;
		}
	} else if (reliable_entry_count() >= m_limits.max_entries ||
			   message.logical_payload.size > m_limits.max_retained_bytes - reliable_retained_bytes()) {
		increment_saturated(m_counters.quota_rejected);
		return ReliableRetainResult::QuotaExceeded;
	}

	Entry candidate;
	if (!m_free_entries.empty()) {
		auto payload = std::move(m_free_entries.back().payload);
		m_free_entries.pop_back();
		candidate.payload = std::move(payload);
	}
	candidate.key.session_id = message.session_id;
	candidate.key.endpoint = message.endpoint;
	candidate.key.message_type = message.message_type;
	candidate.key.message_id = message.message_id;
	candidate.key.fragment_count = message.fragment_count;
	candidate.key.message_crc32 = message.message_crc32;
	candidate.key.transaction_id = message.transaction_id;
	candidate.key.transaction_sha256 = message.transaction_sha256;
	candidate.base_flags = message.base_flags;
	candidate.frame_id = message.frame_id;
	candidate.mission_time_us = message.mission_time_us;
	candidate.required_ack = message.required_ack;
	candidate.message_class = message.message_class;
	candidate.absolute_deadline_us = absolute_deadline_us;
	if (message.message_class == ReliableMessageClass::VideoIdr) {
		// IDRs are NACK-only. They never acquire an autonomous RTO retry.
		candidate.next_retry_at_us = absolute_deadline_us;
	} else {
		const auto delay_us =
			reliable_retry_delay_us(base_rto_us(), m_limits.jitter_seed, message.session_id, message.message_id, 0);
		candidate.next_retry_at_us = bounded_retry_time(first_send_time_us, delay_us, absolute_deadline_us);
	}

	try {
		const auto payload_capacity_before = candidate.payload.capacity();
		if (!message.logical_payload.empty()) {
			candidate.payload.assign(message.logical_payload.begin(), message.logical_payload.end());
		}
		if (candidate.payload.capacity() > payload_capacity_before &&
			m_allocation_events != std::numeric_limits<std::uint64_t>::max()) {
			++m_allocation_events;
		}
		m_entries.emplace_back(std::move(candidate));
	} catch (const std::bad_alloc&) {
		return ReliableRetainResult::AllocationFailed;
	}

	m_retained_bytes += message.logical_payload.size;
	if (is_video_idr) {
		++m_video_entry_count;
		m_video_retained_bytes += message.logical_payload.size;
		if (replaced_video_index != NoEntry) {
			// Admission is atomic: the new payload was copied successfully before
			// the previous IDR became unrecoverable.
			erase(replaced_video_index);
		}
	}
	increment_saturated(m_counters.retained);
	return ReliableRetainResult::Retained;
}

ReliableResponseResult ReliableSendWindow::acknowledge(std::uint64_t session_id,
	const EndpointKey& endpoint,
	const AckPayload& ack,
	std::uint64_t now_us) noexcept
{
	if (validate_ack_payload(ack) != ValidationError::None) {
		increment_saturated(m_counters.ack_ignored_incoherent);
		return ReliableResponseResult::IgnoredIncoherent;
	}
	const auto index = find(session_id, endpoint, ack.target_message_id);
	if (index == NoEntry || now_us >= m_entries[index].absolute_deadline_us) {
		increment_saturated(m_counters.ack_ignored_unknown_or_late);
		return ReliableResponseResult::IgnoredUnknownOrLate;
	}

	auto& entry = m_entries[index];
	const ReliabilityTargetTuple target{entry.key.message_id,
		entry.key.message_type,
		entry.key.fragment_count,
		entry.key.message_crc32,
		entry.message_class == ReliableMessageClass::VideoIdr};
	if (validate_ack_target(ack, target) != ValidationError::None || entry.required_ack == RequiredAckLevel::None) {
		increment_saturated(m_counters.ack_ignored_incoherent);
		return ReliableResponseResult::IgnoredIncoherent;
	}

	const auto applied = (ack.ack_flags & static_cast<std::uint8_t>(AckFlag::Applied)) != 0;
	increment_saturated(m_counters.ack_validated);
	if (applied) {
		increment_saturated(m_counters.ack_applied);
	}

	if (entry.required_ack == RequiredAckLevel::Validated || applied) {
		erase(index);
		return ReliableResponseResult::Released;
	}
	if (entry.validated) {
		return ReliableResponseResult::Duplicate;
	}

	// VALIDATED halts retransmission but cannot release an APPLIED target.
	entry.validated = true;
	entry.immediate_retry = false;
	entry.retry_needed_before_us = 0;
	entry.missing_bitmap_bytes = 0;
	return ReliableResponseResult::ValidatedRetained;
}

ReliableResponseResult ReliableSendWindow::reject(std::uint64_t session_id,
	const EndpointKey& endpoint,
	const NackPayload& nack,
	std::uint64_t now_us,
	ReliableNackDecision& decision) noexcept
{
	if (validate_nack_payload(nack) != ValidationError::None) {
		increment_saturated(m_counters.nack_ignored_incoherent);
		return ReliableResponseResult::IgnoredIncoherent;
	}
	const auto index = find(session_id, endpoint, nack.target_message_id);
	if (index == NoEntry || now_us >= m_entries[index].absolute_deadline_us) {
		increment_saturated(m_counters.nack_ignored_unknown_or_late);
		return ReliableResponseResult::IgnoredUnknownOrLate;
	}

	auto& entry = m_entries[index];
	const auto deadline_bearing = entry.message_class == ReliableMessageClass::VideoIdr;
	const ReliabilityTargetTuple target{entry.key.message_id,
		entry.key.message_type,
		entry.key.fragment_count,
		entry.key.message_crc32,
		deadline_bearing};
	if (validate_nack_target(nack, target) != ValidationError::None) {
		increment_saturated(m_counters.nack_ignored_incoherent);
		return ReliableResponseResult::IgnoredIncoherent;
	}
	if (entry.required_ack == RequiredAckLevel::None && !deadline_bearing) {
		// HELLO is completed by WELCOME, not by ACK/NACK. Video IDR is the only
		// NACK-only retained class in v1.0.
		increment_saturated(m_counters.nack_ignored_incoherent);
		return ReliableResponseResult::IgnoredIncoherent;
	}
	if (deadline_bearing && nack.reason != NackReason::MissingFragments) {
		// IDR recovery is selective-only. Reject the response before touching the
		// latched producer deadline so incoherent input cannot poison a later,
		// otherwise valid selective NACK.
		increment_saturated(m_counters.nack_ignored_incoherent);
		return ReliableResponseResult::IgnoredIncoherent;
	}
	if (deadline_bearing && nack.needed_before_producer_time_us != 0) {
		if (entry.video_recovery_deadline_us != 0 &&
			nack.needed_before_producer_time_us > entry.video_recovery_deadline_us) {
			increment_saturated(m_counters.nack_ignored_incoherent);
			return ReliableResponseResult::IgnoredIncoherent;
		}
		entry.video_recovery_deadline_us = nack.needed_before_producer_time_us;
	}
	if (entry.video_recovery_deadline_us != 0 && now_us >= entry.video_recovery_deadline_us) {
		increment_saturated(m_counters.nack_ignored_unknown_or_late);
		return ReliableResponseResult::IgnoredUnknownOrLate;
	}
	ReliableNackDecision accepted;
	accepted.target = entry.key;
	if (entry.validated) {
		// VALIDATED stops fragment retransmission, but it is not a successful
		// transaction commit. Cross-part or semantic validation may still fail
		// afterwards; that terminal NACK must release the retained part and start
		// its class recovery policy. Fragment/layout/resource responses after a
		// successful logical-message validation are contradictory and ignored.
		const auto terminal_after_validation = nack.reason == NackReason::StaleBaseline ||
											   nack.reason == NackReason::SemanticValidationFailed ||
											   nack.reason == NackReason::DeadlineExpired;
		if (!terminal_after_validation) {
			increment_saturated(m_counters.nack_ignored_incoherent);
			return ReliableResponseResult::IgnoredIncoherent;
		}
		accepted.kind = ReliableNackDecisionKind::TerminalPolicy;
		accepted.terminal_policy = terminal_policy(entry.message_class);
		const auto failed_key = entry.key;
		if (entry.message_class == ReliableMessageClass::Transaction) {
			erase_transaction_group(failed_key.session_id,
				failed_key.endpoint,
				failed_key.message_type,
				failed_key.transaction_id,
				failed_key.transaction_sha256);
		} else {
			erase(index);
		}
		decision = accepted;
		increment_saturated(m_counters.nack_accepted);
		return ReliableResponseResult::Released;
	}
	if (nack.reason == NackReason::MissingFragments) {
		entry.immediate_retry = true;
		entry.retry_all_fragments = false;
		entry.retry_needed_before_us = entry.video_recovery_deadline_us;
		entry.missing_bitmap_bytes = static_cast<std::uint16_t>(nack.missing_bitmap.size);
		std::copy(nack.missing_bitmap.begin(), nack.missing_bitmap.end(), entry.missing_bitmap.begin());
		accepted.kind = ReliableNackDecisionKind::SelectiveRetransmissionScheduled;
		decision = accepted;
		increment_saturated(m_counters.nack_accepted);
		return ReliableResponseResult::ValidatedRetained;
	}

	if (nack.reason == NackReason::BadMessageCrc || nack.reason == NackReason::BadFragmentLayout) {
		entry.immediate_retry = true;
		entry.retry_all_fragments = true;
		entry.retry_needed_before_us = 0;
		entry.missing_bitmap_bytes = 0;
		accepted.kind = ReliableNackDecisionKind::FullRetransmissionScheduled;
		decision = accepted;
		increment_saturated(m_counters.nack_accepted);
		return ReliableResponseResult::ValidatedRetained;
	}

	if (nack.reason == NackReason::ResourceLimit) {
		// Preserve the existing timer/backoff; immediate retry would amplify the
		// receiver's overload and the NACK never extends the absolute deadline.
		entry.immediate_retry = false;
		entry.retry_all_fragments = true;
		entry.retry_needed_before_us = 0;
		entry.missing_bitmap_bytes = 0;
		accepted.kind = ReliableNackDecisionKind::WaitForScheduledRetry;
		decision = accepted;
		increment_saturated(m_counters.nack_accepted);
		return ReliableResponseResult::ValidatedRetained;
	}

	accepted.kind = ReliableNackDecisionKind::TerminalPolicy;
	accepted.terminal_policy = terminal_policy(entry.message_class);
	// StaleBaseline, UnsupportedMessage, SemanticValidationFailed and
	// DeadlineExpired do not have a universal retry rule in v1.0. The class's
	// predeclared terminal policy makes the caller's recovery action explicit.
	const auto failed_key = entry.key;
	if (entry.message_class == ReliableMessageClass::Transaction) {
		erase_transaction_group(failed_key.session_id,
			failed_key.endpoint,
			failed_key.message_type,
			failed_key.transaction_id,
			failed_key.transaction_sha256);
	} else {
		erase(index);
	}
	decision = accepted;
	increment_saturated(m_counters.nack_accepted);
	return ReliableResponseResult::Released;
}

ReliablePullResult ReliableSendWindow::pull_next_action(std::uint64_t now_us, ReliableWindowAction& action) noexcept
{
	std::size_t expired_index = NoEntry;
	for (std::size_t index = 0; index < m_entries.size(); ++index) {
		if (now_us < m_entries[index].absolute_deadline_us) {
			continue;
		}
		if (expired_index == NoEntry ||
			scheduler_priority(m_entries[index].message_class) <
				scheduler_priority(m_entries[expired_index].message_class) ||
			(scheduler_priority(m_entries[index].message_class) ==
					scheduler_priority(m_entries[expired_index].message_class) &&
				m_entries[index].absolute_deadline_us < m_entries[expired_index].absolute_deadline_us)) {
			expired_index = index;
		}
	}

	std::size_t ready_index = NoEntry;
	for (std::size_t index = 0; index < m_entries.size(); ++index) {
		auto& entry = m_entries[index];
		if (now_us >= entry.absolute_deadline_us) {
			continue;
		}
		if (entry.immediate_retry && entry.retry_needed_before_us != 0 && now_us >= entry.retry_needed_before_us) {
			// Priority/rate-limit delay made this selective IDR recovery
			// obsolete. Keep the bounded IDR copy only until its immutable
			// retention deadline so a later, still-useful NACK may replace it.
			entry.immediate_retry = false;
			entry.retry_all_fragments = true;
			entry.retry_needed_before_us = 0;
			entry.missing_bitmap_bytes = 0;
		}
		if (entry.validated || (!entry.immediate_retry && now_us < entry.next_retry_at_us) ||
			(entry.message_class == ReliableMessageClass::VideoIdr && !entry.immediate_retry)) {
			continue;
		}

		if (ready_index == NoEntry) {
			ready_index = index;
			continue;
		}
		const auto& current = m_entries[ready_index];
		const auto entry_priority = scheduler_priority(entry.message_class);
		const auto current_priority = scheduler_priority(current.message_class);
		if (entry_priority < current_priority ||
			(entry_priority == current_priority && entry.immediate_retry && !current.immediate_retry) ||
			(entry_priority == current_priority && entry.immediate_retry == current.immediate_retry &&
				entry.next_retry_at_us < current.next_retry_at_us)) {
			ready_index = index;
		}
	}

	if (expired_index != NoEntry &&
		(ready_index == NoEntry || scheduler_priority(m_entries[expired_index].message_class) <=
									   scheduler_priority(m_entries[ready_index].message_class))) {
		ReliableWindowAction ready;
		ready.kind = ReliableWindowActionKind::TerminalPolicy;
		ready.terminal_policy = terminal_policy(m_entries[expired_index].message_class);
		ready.target = m_entries[expired_index].key;
		std::size_t expired_count = 1;
		if (m_entries[expired_index].message_class == ReliableMessageClass::Transaction) {
			expired_count = erase_transaction_group(ready.target.session_id,
				ready.target.endpoint,
				ready.target.message_type,
				ready.target.transaction_id,
				ready.target.transaction_sha256);
		} else {
			erase(expired_index);
		}
		action = ready;
		for (std::size_t count = 0; count < expired_count; ++count) {
			increment_saturated(m_counters.expirations);
		}
		return ReliablePullResult::Action;
	}

	if (ready_index == NoEntry) {
		return ReliablePullResult::None;
	}

	auto& entry = m_entries[ready_index];
	const auto nack_triggered = entry.immediate_retry;
	const auto rto_already_due =
		entry.message_class != ReliableMessageClass::VideoIdr && now_us >= entry.next_retry_at_us;
	ReliableWindowAction ready;
	ready.kind = ReliableWindowActionKind::Retransmit;
	ready.target = entry.key;
	ready.retransmission.key = entry.key;
	ready.retransmission.base_flags = entry.base_flags;
	ready.retransmission.frame_id = entry.frame_id;
	ready.retransmission.mission_time_us = entry.mission_time_us;
	ready.retransmission.logical_payload =
		ByteView{entry.payload.empty() ? nullptr : entry.payload.data(), entry.payload.size()};
	ready.retransmission.fragments.all_fragments = entry.retry_all_fragments;
	ready.retransmission.fragments.fragment_count = entry.key.fragment_count;
	if (!entry.retry_all_fragments) {
		ready.retransmission.fragments.bitmap_bytes = entry.missing_bitmap_bytes;
		std::copy_n(entry.missing_bitmap.begin(),
			entry.missing_bitmap_bytes,
			ready.retransmission.fragments.bitmap.begin());
	}
	ready.retransmission.retry_number = entry.retry_number;
	ready.retransmission.absolute_deadline_us = entry.absolute_deadline_us;

	entry.immediate_retry = false;
	entry.retry_all_fragments = true;
	entry.retry_needed_before_us = 0;
	entry.missing_bitmap_bytes = 0;
	if (!nack_triggered || rto_already_due) {
		// Only the RTO schedule advances the exponential retry number. A NACK
		// arriving before that RTO may trigger an extra immediate send, but it
		// leaves both the pending RTO and its backoff state exactly unchanged.
		if (entry.retry_number != std::numeric_limits<std::uint32_t>::max()) {
			++entry.retry_number;
		}
		const auto delay_us = reliable_retry_delay_us(base_rto_us(),
			m_limits.jitter_seed,
			entry.key.session_id,
			entry.key.message_id,
			entry.retry_number);
		entry.next_retry_at_us = bounded_retry_time(now_us, delay_us, entry.absolute_deadline_us);
	}
	action = ready;
	increment_saturated(m_counters.retransmissions);
	return ReliablePullResult::Action;
}

bool ReliableSendWindow::discard(std::uint64_t session_id,
	const EndpointKey& endpoint,
	std::uint32_t message_id) noexcept
{
	const auto index = find(session_id, endpoint, message_id);
	if (index == NoEntry) {
		return false;
	}
	erase(index);
	return true;
}

std::size_t ReliableSendWindow::discard_session(std::uint64_t session_id) noexcept
{
	std::size_t discarded = 0;
	for (std::size_t index = m_entries.size(); index > 0; --index) {
		if (m_entries[index - 1].key.session_id == session_id) {
			erase(index - 1);
			++discarded;
		}
	}
	return discarded;
}

void ReliableSendWindow::clear() noexcept
{
	if (m_limits.preallocated_payload_bytes_per_entry != 0U) {
		for (auto& entry : m_entries) {
			entry.payload.clear();
			m_free_entries.emplace_back(std::move(entry));
		}
		m_entries.clear();
		m_retained_bytes = 0U;
		m_video_entry_count = 0U;
		m_video_retained_bytes = 0U;
		return;
	}
	std::vector<Entry>{}.swap(m_entries);
	m_retained_bytes = 0;
	m_video_entry_count = 0;
	m_video_retained_bytes = 0;
}

std::size_t
ReliableSendWindow::find(std::uint64_t session_id, const EndpointKey& endpoint, std::uint32_t message_id) const noexcept
{
	for (std::size_t index = 0; index < m_entries.size(); ++index) {
		const auto& key = m_entries[index].key;
		if (key.session_id == session_id && key.endpoint == endpoint && key.message_id == message_id) {
			return index;
		}
	}
	return NoEntry;
}

void ReliableSendWindow::erase(std::size_t index) noexcept
{
	if (m_entries[index].message_class == ReliableMessageClass::VideoIdr) {
		--m_video_entry_count;
		m_video_retained_bytes -= m_entries[index].payload.size();
	}
	m_retained_bytes -= m_entries[index].payload.size();
	if (m_limits.preallocated_payload_bytes_per_entry != 0U) {
		Entry recycled = std::move(m_entries[index]);
		m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(index));
		recycled.payload.clear();
		m_free_entries.emplace_back(std::move(recycled));
		return;
	}
	m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(index));
}

std::size_t ReliableSendWindow::erase_transaction_group(std::uint64_t session_id,
	const EndpointKey& endpoint,
	MessageType message_type,
	std::uint32_t transaction_id,
	const Sha256Digest& transaction_sha256) noexcept
{
	std::size_t erased = 0;
	for (std::size_t index = m_entries.size(); index > 0; --index) {
		const auto& entry = m_entries[index - 1];
		if (entry.message_class == ReliableMessageClass::Transaction && entry.key.session_id == session_id &&
			entry.key.endpoint == endpoint && entry.key.message_type == message_type &&
			entry.key.transaction_id == transaction_id && entry.key.transaction_sha256 == transaction_sha256) {
			erase(index - 1);
			++erased;
		}
	}
	return erased;
}

} // namespace telemetry::protocol
