#include "telemetry/session_controller.h"

#include "telemetry/protocol/telemetry_control_messages.h"
#include "telemetry/protocol/telemetry_crc32.h"
#include "telemetry/protocol/telemetry_datagram.h"
#include "telemetry/protocol/telemetry_reliability_messages.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace telemetry::detail {
const std::size_t Wp06ClientSlotStorageBytes = sizeof(SessionControllerSlot);
const std::size_t Wp06RateLimiterStorageBytes = sizeof(protocol::ProtocolRateLimiter);
const std::size_t Wp06HandshakeCacheStorageBytes = SessionController::handshake_cache_storage_bytes();
const std::size_t Wp06PreproofLedgerStorageBytes = sizeof(PreproofAmplificationLedger);
const std::size_t Wp06OutputQueueStorageBytes = sizeof(SessionControllerOutput);
namespace {

constexpr std::size_t InvalidIndex = std::numeric_limits<std::size_t>::max();
constexpr std::uint64_t HandshakeCacheLifetimeUs = protocol::HandshakeCacheLifetimeMs * 1000U;

bool add_would_overflow(std::uint64_t left, std::uint64_t right) noexcept
{
	return right > std::numeric_limits<std::uint64_t>::max() - left;
}

SessionIngressResult dropped(SessionIngressDropReason reason) noexcept
{
	return {SessionIngressDisposition::Dropped, reason};
}

bool encode_control_datagram(protocol::TelemetryDatagramHeader header,
	protocol::ByteView payload,
	std::array<std::uint8_t, protocol::MaxDatagramSize>& output,
	std::size_t& written) noexcept
{
	header.message_size = static_cast<std::uint32_t>(payload.size);
	header.message_crc32 = protocol::crc32_iso_hdlc(payload);
	return protocol::encode_datagram(header,
		protocol::ProtocolMinorRange{header.version_minor, header.version_minor},
		payload,
		{output.data(), output.size()},
		written) == protocol::ValidationError::None;
}

} // namespace

bool PreproofAmplificationLedger::configure(std::size_t capacity) noexcept
{
	if (capacity == 0U || capacity > m_accounts.size()) {
		return false;
	}
	m_accounts = {};
	m_used = {};
	m_capacity = capacity;
	m_size = 0U;
	return true;
}

std::size_t PreproofAmplificationLedger::find(const protocol::EndpointKey& endpoint) const noexcept
{
	for (std::size_t i = 0U; i < m_capacity; ++i) {
		if (m_used[i] && m_accounts[i].endpoint == endpoint) {
			return i;
		}
	}
	return InvalidIndex;
}

PreproofLedgerResult PreproofAmplificationLedger::note_validated_receive(
	const protocol::EndpointKey& endpoint, std::uint64_t bytes) noexcept
{
	if (!endpoint.is_valid()) {
		return PreproofLedgerResult::InvalidEndpoint;
	}
	auto index = find(endpoint);
	if (index == InvalidIndex) {
		for (std::size_t i = 0U; i < m_capacity; ++i) {
			if (!m_used[i]) {
				index = i;
				break;
			}
		}
		if (index == InvalidIndex) {
			return PreproofLedgerResult::CapacityReached;
		}
		m_used[index] = true;
		m_accounts[index] = PreproofAccount{};
		m_accounts[index].endpoint = endpoint;
		++m_size;
	}
	if (m_accounts[index].proven) {
		return PreproofLedgerResult::ProofAlreadyApplied;
	}
	if (add_would_overflow(m_accounts[index].validated_bytes_received, bytes)) {
		return PreproofLedgerResult::ArithmeticOverflow;
	}
	m_accounts[index].validated_bytes_received += bytes;
	return PreproofLedgerResult::Recorded;
}

PreproofLedgerResult PreproofAmplificationLedger::try_account_send(
	const protocol::EndpointKey& endpoint, std::uint64_t bytes) noexcept
{
	const auto index = find(endpoint);
	if (index == InvalidIndex) {
		return PreproofLedgerResult::InvalidEndpoint;
	}
	auto& account = m_accounts[index];
	if (account.proven) {
		return PreproofLedgerResult::ProofAlreadyApplied;
	}
	const auto maximum = std::numeric_limits<std::uint64_t>::max();
	const auto limit = account.validated_bytes_received > maximum / 3U
		? maximum
		: account.validated_bytes_received * 3U;
	if (add_would_overflow(account.bytes_sent, bytes) || account.bytes_sent + bytes > limit) {
		return PreproofLedgerResult::AntiAmplificationLimit;
	}
	account.bytes_sent += bytes;
	return PreproofLedgerResult::Allowed;
}

void PreproofAmplificationLedger::mark_welcome_proven(const protocol::EndpointKey& endpoint) noexcept
{
	const auto index = find(endpoint);
	if (index != InvalidIndex) {
		m_accounts[index].proven = true;
	}
}

void PreproofAmplificationLedger::release_contribution(const protocol::EndpointKey& endpoint,
	std::uint64_t received_bytes,
	std::uint64_t sent_bytes) noexcept
{
	const auto index = find(endpoint);
	if (index == InvalidIndex) {
		return;
	}
	auto& account = m_accounts[index];
	if (received_bytes > account.validated_bytes_received || sent_bytes > account.bytes_sent) {
		return;
	}
	account.validated_bytes_received -= received_bytes;
	account.bytes_sent -= sent_bytes;
	if (account.validated_bytes_received == 0U && account.bytes_sent == 0U) {
		m_used[index] = false;
		account = PreproofAccount{};
		--m_size;
	}
}

void PreproofAmplificationLedger::erase(const protocol::EndpointKey& endpoint) noexcept
{
	const auto index = find(endpoint);
	if (index != InvalidIndex) {
		m_used[index] = false;
		m_accounts[index] = PreproofAccount{};
		--m_size;
	}
}

PreproofAccount PreproofAmplificationLedger::account(const protocol::EndpointKey& endpoint) const noexcept
{
	const auto index = find(endpoint);
	return index == InvalidIndex ? PreproofAccount{} : m_accounts[index];
}

SessionControllerConfigureResult SessionController::configure(const SessionControllerConfig& config,
	SessionIdAllocator& ids,
	RandomSource& packet_sequences,
	std::uint64_t initial_time_us,
	SessionControllerObserver* observer,
	SessionController& output) noexcept
{
	protocol::TelemetryResourceBudgetTotals totals;
	if (config.max_clients < 1U || config.max_clients > 4U || config.producer_id == 0U ||
		config.security.resources.max_clients != config.max_clients ||
		protocol::validate_security_configuration(config.security, totals) !=
			protocol::SecurityConfigurationError::None) {
		return SessionControllerConfigureResult::InvalidConfiguration;
	}

	auto limiter = std::unique_ptr<protocol::ProtocolRateLimiter>(
		new (std::nothrow) protocol::ProtocolRateLimiter());
	if (!limiter) {
		return SessionControllerConfigureResult::AllocationFailure;
	}
	protocol::ProtocolRateLimiterConfig limiter_config;
	limiter_config.limits.max_sessions = config.max_clients;
	if (protocol::ProtocolRateLimiter::configure(limiter_config, initial_time_us, *limiter) !=
		protocol::ValidationError::None) {
		return SessionControllerConfigureResult::InvalidConfiguration;
	}

	std::size_t slot_bytes = 0U;
	std::size_t reassembly_bytes = 0U;
	std::size_t reliable_bytes = 0U;
	if (!checked_multiply_size(config.max_clients, Wp06ClientSlotStorageBytes, slot_bytes) ||
		!checked_multiply_size(config.max_clients, protocol::MaxStateReassemblyBytesPerClient, reassembly_bytes) ||
		!checked_multiply_size(config.max_clients, Wp06ReliableRetentionBytesPerClient, reliable_bytes)) {
		return SessionControllerConfigureResult::InvalidConfiguration;
	}
	auto slots = std::unique_ptr<SessionControllerSlot[]>(new (std::nothrow) SessionControllerSlot[config.max_clients]);
	auto reliable_windows = std::unique_ptr<protocol::PreallocatedReliableControlWindow[]>(
		new (std::nothrow) protocol::PreallocatedReliableControlWindow[config.max_clients]);
	auto reassembly = std::unique_ptr<std::uint8_t[]>(new (std::nothrow) std::uint8_t[reassembly_bytes]{});
	if (!slots || !reliable_windows || !reassembly) {
		return SessionControllerConfigureResult::AllocationFailure;
	}

	SessionController candidate;
	candidate.m_config = config;
	candidate.m_ids = &ids;
	candidate.m_packet_sequences = &packet_sequences;
	candidate.m_observer = observer;
	candidate.m_rate_limiter = std::move(limiter);
	candidate.m_slots = std::move(slots);
	candidate.m_reliable_windows = std::move(reliable_windows);
	candidate.m_reassembly_backing = std::move(reassembly);
	if (!candidate.m_preproof.configure(protocol::HandshakeCacheCapacity)) {
		return SessionControllerConfigureResult::InvalidConfiguration;
	}
	for (std::size_t i = 0U; i < config.max_clients; ++i) {
		if (!candidate.initialize_slot(i)) {
			return SessionControllerConfigureResult::AllocationFailure;
		}
	}
	candidate.m_ready = true;
	output = std::move(candidate);
	return SessionControllerConfigureResult::Ready;
}

void SessionController::stage(SessionIngressStage value) noexcept
{
	if (m_observer != nullptr) {
		m_observer->stage_reached(value);
	}
}

std::size_t SessionController::find_cache(const protocol::EndpointKey& endpoint, std::uint64_t nonce) const noexcept
{
	for (std::size_t i = 0U; i < m_cache.size(); ++i) {
		if (m_cache[i].used && m_cache[i].endpoint == endpoint && m_cache[i].nonce == nonce) {
			return i;
		}
	}
	return InvalidIndex;
}

std::size_t SessionController::free_cache() const noexcept
{
	for (std::size_t i = 0U; i < m_cache.size(); ++i) {
		if (!m_cache[i].used) {
			return i;
		}
	}
	return InvalidIndex;
}

std::size_t SessionController::free_slot() const noexcept
{
	for (std::size_t i = 0U; i < m_config.max_clients; ++i) {
		if (m_slots[i].progress == ProducerSessionProgress::Empty) {
			return i;
		}
	}
	return InvalidIndex;
}

std::size_t SessionController::find_awaiting_slot() const noexcept
{
	for (std::size_t i = 0U; i < m_config.max_clients; ++i) {
		if (m_slots[i].progress == ProducerSessionProgress::AwaitWelcomeApplied) {
			return i;
		}
	}
	return InvalidIndex;
}

std::size_t SessionController::find_slot(const protocol::EndpointKey& endpoint,
	std::uint64_t session_id) const noexcept
{
	for (std::size_t i = 0U; i < m_config.max_clients; ++i) {
		if (m_slots[i].progress != ProducerSessionProgress::Empty &&
			m_slots[i].endpoint == endpoint && m_slots[i].session_id == session_id) {
			return i;
		}
	}
	return InvalidIndex;
}

bool SessionController::initialize_slot(std::size_t index) noexcept
{
	if (index >= m_config.max_clients) {
		return false;
	}
	m_slots[index] = SessionControllerSlot{};
	m_reliable_windows[index].configure();
	return true;
}

bool SessionController::next_packet_sequence(std::uint32_t& sequence) noexcept
{
	std::uint64_t value = 0U;
	if (m_packet_sequences == nullptr || !m_packet_sequences->next_u64(value)) {
		return false;
	}
	sequence = static_cast<std::uint32_t>(value);
	return true;
}

bool SessionController::queue_bytes(const protocol::EndpointKey& endpoint,
	const std::uint8_t* bytes,
	std::size_t size) noexcept
{
	if (m_has_output || size > m_output.bytes.size()) {
		return false;
	}
	m_output = SessionControllerOutput{};
	m_output.endpoint = endpoint;
	std::memcpy(m_output.bytes.data(), bytes, size);
	m_output.size = size;
	m_has_output = true;
	return true;
}

SessionIngressResult SessionController::ingest(const protocol::EndpointKey& endpoint,
	protocol::ByteView datagram,
	std::uint64_t now_us,
	std::uint32_t mission_generation,
	bool mission_active) noexcept
{
	if (!m_ready || m_faulted) {
		return {SessionIngressDisposition::Faulted, SessionIngressDropReason::SessionIdUnavailable};
	}
	stage(SessionIngressStage::SourcePolicy);
	if (!protocol::source_is_allowed(m_config.security, endpoint)) {
		return dropped(SessionIngressDropReason::SourceNotAllowed);
	}
	if (m_has_output) {
		return dropped(SessionIngressDropReason::OutputBusy);
	}
	stage(SessionIngressStage::DatagramEnvelope);
	protocol::DatagramView decoded;
	if (protocol::decode_and_validate_datagram(datagram,
			protocol::ProtocolMinorRange{protocol::VersionMinorV1_0, protocol::VersionMinorV1_1},
			decoded) != protocol::ValidationError::None) {
		return dropped(SessionIngressDropReason::DatagramEnvelopeInvalid);
	}
	stage(SessionIngressStage::EndpointAndSession);
	if (decoded.header.message_type == protocol::MessageType::Hello) {
		if (decoded.header.session_id != 0U) {
			return dropped(SessionIngressDropReason::EndpointSessionMismatch);
		}
		return ingest_hello(endpoint, decoded, datagram.size, now_us);
	}
	if (decoded.header.message_type == protocol::MessageType::Ack) {
		return ingest_ack(endpoint, decoded, now_us, mission_generation, mission_active);
	}
	if (decoded.header.message_type == protocol::MessageType::Nack) {
		return ingest_nack(endpoint, decoded, now_us);
	}
	return dropped(SessionIngressDropReason::EndpointSessionMismatch);
}

SessionIngressResult SessionController::ingest_hello(const protocol::EndpointKey& endpoint,
	const protocol::DatagramView& decoded,
	std::size_t received_size,
	std::uint64_t now_us) noexcept
{
	stage(SessionIngressStage::RateLimit);
	if (m_rate_limiter->consume_pre_session(protocol::RateLimitClass::Hello, endpoint, now_us) !=
		protocol::ProtocolRateLimitResult::Allowed) {
		return dropped(SessionIngressDropReason::HelloRateLimited);
	}
	stage(SessionIngressStage::AntiAmplification);
	stage(SessionIngressStage::Payload);
	protocol::HelloPayload hello;
	if (protocol::decode_hello_payload(decoded.payload, hello) != protocol::ValidationError::None) {
		return dropped(SessionIngressDropReason::PayloadInvalid);
	}

	expire_housekeeping(now_us);
	const auto cached_index = find_cache(endpoint, hello.client_nonce);
	if (cached_index != InvalidIndex) {
		auto& cached = m_cache[cached_index];
		bool proof_already_applied = false;
		for (std::size_t i = 0U; i < m_config.max_clients; ++i) {
			proof_already_applied = proof_already_applied ||
				(cached.session_id != 0U && m_slots[i].session_id == cached.session_id &&
				 m_slots[i].progress == ProducerSessionProgress::ReadyForState);
		}
		if (!proof_already_applied) {
			if (m_preproof.note_validated_receive(endpoint, received_size) != PreproofLedgerResult::Recorded ||
				m_preproof.try_account_send(endpoint, cached.size) != PreproofLedgerResult::Allowed) {
				return dropped(SessionIngressDropReason::AntiAmplificationLimit);
			}
			cached.accounted_received += received_size;
			cached.accounted_sent += cached.size;
			cached.preproof_active = true;
		}
		if (!queue_bytes(endpoint, cached.bytes.data(), cached.size)) {
			return dropped(SessionIngressDropReason::OutputBusy);
		}
		return {SessionIngressDisposition::CachedResponseQueued, SessionIngressDropReason::None};
	}
	if (m_cache_size == m_cache.size()) {
		return dropped(SessionIngressDropReason::HandshakeCacheFull);
	}

	const bool supported = hello.min_major <= protocol::VersionMajor && hello.max_major >= protocol::VersionMajor &&
		hello.min_minor <= protocol::VersionMinorV1_1 && hello.max_minor >= protocol::VersionMinorV1_1;
	std::uint64_t session_id = 0U;
	std::uint32_t initial_packet_sequence = 0U;
	std::size_t slot_index = InvalidIndex;
	if (supported) {
		if (m_rate_limiter->consume_pre_session(protocol::RateLimitClass::SessionCreation, endpoint, now_us) !=
			protocol::ProtocolRateLimitResult::Allowed) {
			return dropped(SessionIngressDropReason::SessionCreationRateLimited);
		}
		slot_index = free_slot();
		if (slot_index == InvalidIndex) {
			return dropped(SessionIngressDropReason::NoClientSlot);
		}
		const auto id = m_ids->allocate();
		if (id.status != SessionIdStatus::Allocated) {
			clear_all();
			m_faulted = true;
			return {SessionIngressDisposition::Faulted, SessionIngressDropReason::SessionIdUnavailable};
		}
		session_id = id.session_id;
	}
	if (!next_packet_sequence(initial_packet_sequence)) {
		clear_all();
		m_faulted = true;
		return {SessionIngressDisposition::Faulted, SessionIngressDropReason::SessionIdUnavailable};
	}

	protocol::WelcomePayload welcome;
	welcome.client_nonce = hello.client_nonce;
	welcome.client_send_t0_us = hello.client_send_t0_us;
	welcome.producer_receive_t1_us = now_us;
	welcome.producer_send_t2_us = now_us;
	welcome.status = supported ? protocol::WelcomeStatus::Accepted : protocol::WelcomeStatus::UnsupportedVersion;
	welcome.selected_major = supported ? protocol::VersionMajor : 0U;
	welcome.selected_minor = supported ? protocol::VersionMinorV1_1 : 0U;
	welcome.selected_visibility_mode = protocol::VisibilityMode::Cockpit;
	welcome.heartbeat_interval_ms = supported ? hello.requested_heartbeat_ms : 0U;
	welcome.reliable_reassembly_timeout_ms = supported ? protocol::ReliableReassemblyTimeoutV1Ms : 0U;
	welcome.producer_id = m_config.producer_id;

	std::array<std::uint8_t, protocol::MaxCachedWelcomePayloadSize> payload{};
	std::size_t payload_size = 0U;
	if (protocol::encode_welcome_payload(welcome, {payload.data(), payload.size()}, payload_size) !=
		protocol::ValidationError::None) {
		return dropped(SessionIngressDropReason::PayloadInvalid);
	}
	protocol::TelemetryDatagramHeader header;
	header.version_minor = supported ? protocol::VersionMinorV1_1 : protocol::VersionMinorV1_0;
	header.message_type = protocol::MessageType::Welcome;
	header.flags = supported ? protocol::MessageFlagAckRequired : protocol::MessageFlagNone;
	header.session_id = session_id;
	header.packet_sequence = initial_packet_sequence;
	header.sent_time_us = now_us;
	header.message_id = 1U;
	std::array<std::uint8_t, protocol::MaxDatagramSize> encoded{};
	std::size_t encoded_size = 0U;
	if (!encode_control_datagram(header, {payload.data(), payload_size}, encoded, encoded_size)) {
		return dropped(SessionIngressDropReason::PayloadInvalid);
	}
	if (m_preproof.note_validated_receive(endpoint, received_size) != PreproofLedgerResult::Recorded ||
		m_preproof.try_account_send(endpoint, encoded_size) != PreproofLedgerResult::Allowed) {
		return dropped(SessionIngressDropReason::AntiAmplificationLimit);
	}

	stage(SessionIngressStage::SessionMutation);
	const auto cache_index = free_cache();
	auto& cache = m_cache[cache_index];
	cache.used = true;
	cache.endpoint = endpoint;
	cache.nonce = hello.client_nonce;
	cache.session_id = session_id;
	cache.stored_at_us = now_us;
	cache.accounted_received = received_size;
	cache.accounted_sent = encoded_size;
	cache.preproof_active = true;
	cache.size = encoded_size;
	std::copy_n(encoded.data(), encoded_size, cache.bytes.data());
	++m_cache_size;
	if (supported) {
		if (!initialize_slot(slot_index)) {
			clear_all();
			m_faulted = true;
			return {SessionIngressDisposition::Faulted, SessionIngressDropReason::SessionIdUnavailable};
		}
		auto& slot = m_slots[slot_index];
		slot.progress = ProducerSessionProgress::AwaitWelcomeApplied;
		slot.endpoint = endpoint;
		slot.session_id = session_id;
		slot.session_start_us = now_us;
		slot.welcome_deadline_us = now_us > std::numeric_limits<std::uint64_t>::max() - protocol::ReliableOrdinaryRetentionUs
			? std::numeric_limits<std::uint64_t>::max()
			: now_us + protocol::ReliableOrdinaryRetentionUs;
		slot.next_message_id = 2U;
		slot.next_packet_sequence = initial_packet_sequence + 1U;
		protocol::DatagramView welcome_view;
		(void)protocol::decode_and_validate_datagram({encoded.data(), encoded_size},
			protocol::ProtocolMinorRange{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1}, welcome_view);
		slot.welcome_message_id = welcome_view.header.message_id;
		slot.welcome_fragment_count = welcome_view.header.fragment_count;
		slot.welcome_message_crc32 = welcome_view.header.message_crc32;
		protocol::ReliableMessageToRetain retained;
		retained.session_id = session_id;
		retained.endpoint = endpoint;
		retained.message_type = protocol::MessageType::Welcome;
		retained.base_flags = protocol::MessageFlagAckRequired;
		retained.message_id = welcome_view.header.message_id;
		retained.fragment_count = welcome_view.header.fragment_count;
		retained.message_crc32 = welcome_view.header.message_crc32;
		retained.logical_payload = {payload.data(), payload_size};
		retained.required_ack = protocol::RequiredAckLevel::Applied;
		retained.message_class = protocol::ReliableMessageClass::HandshakeCritical;
		if (m_reliable_windows[slot_index].retain(retained, now_us) != protocol::ReliableRetainResult::Retained) {
			clear_all();
			m_faulted = true;
			return {SessionIngressDisposition::Faulted, SessionIngressDropReason::SessionIdUnavailable};
		}
		slot.reliable_items_in_use = m_reliable_windows[slot_index].entry_count();
		const auto account = m_preproof.account(endpoint);
		slot.preproof_validated_bytes_received = account.validated_bytes_received;
		slot.preproof_bytes_sent = account.bytes_sent;
	}
	if (!queue_bytes(endpoint, encoded.data(), encoded_size)) {
		return dropped(SessionIngressDropReason::OutputBusy);
	}
	return {SessionIngressDisposition::ResponseQueued, SessionIngressDropReason::None};
}

SessionIngressResult SessionController::ingest_ack(const protocol::EndpointKey& endpoint,
	const protocol::DatagramView& decoded,
	std::uint64_t now_us,
	std::uint32_t mission_generation,
	bool mission_active) noexcept
{
	stage(SessionIngressStage::RateLimit);
	const auto awaiting = find_slot(endpoint, decoded.header.session_id);
	if (awaiting == InvalidIndex) {
		return dropped(SessionIngressDropReason::WelcomeProofMismatch);
	}
	auto& slot = m_slots[awaiting];
	if (slot.progress != ProducerSessionProgress::AwaitWelcomeApplied) {
		return dropped(SessionIngressDropReason::WelcomeProofMismatch);
	}
	if (m_rate_limiter->consume_ack_nack(slot.session_id,
			endpoint,
			{static_cast<std::uint8_t>(protocol::MessageType::Welcome), slot.welcome_message_id,
				slot.welcome_message_crc32},
			now_us) != protocol::ProtocolRateLimitResult::Allowed) {
		return dropped(SessionIngressDropReason::WelcomeProofMismatch);
	}
	stage(SessionIngressStage::AntiAmplification);
	stage(SessionIngressStage::Payload);
	protocol::AckPayload ack;
	if (protocol::decode_ack_payload(decoded.payload, ack) != protocol::ValidationError::None ||
		ack.ack_flags != protocol::KnownAckFlags ||
		ack.target_message_id != slot.welcome_message_id ||
		ack.target_message_type != protocol::MessageType::Welcome ||
		ack.target_fragment_count != slot.welcome_fragment_count ||
		ack.target_message_crc32 != slot.welcome_message_crc32) {
		return dropped(SessionIngressDropReason::WelcomeProofMismatch);
	}
	if (now_us >= slot.welcome_deadline_us) {
		return dropped(SessionIngressDropReason::WelcomeProofExpired);
	}
	if (m_reliable_windows[awaiting].acknowledge(slot.session_id, endpoint, ack, now_us) !=
		protocol::ReliableResponseResult::Released) {
		return dropped(SessionIngressDropReason::WelcomeProofMismatch);
	}

	protocol::SessionBeginPayload begin;
	begin.session_flags = protocol::SessionBeginFlagReadOnly |
		(mission_active ? protocol::SessionBeginFlagMissionActive : 0U);
	begin.producer_session_start_us = slot.session_start_us;
	begin.mission_instance_id = mission_active ? mission_generation : 0U;
	std::array<std::uint8_t, protocol::SessionBeginPayloadSize> payload{};
	std::size_t payload_size = 0U;
	if (protocol::encode_session_begin_payload(begin, {payload.data(), payload.size()}, payload_size) !=
		protocol::ValidationError::None) {
		return dropped(SessionIngressDropReason::PayloadInvalid);
	}
	protocol::TelemetryDatagramHeader header;
	header.version_minor = protocol::VersionMinorV1_1;
	header.message_type = protocol::MessageType::SessionBegin;
	header.flags = protocol::MessageFlagAckRequired;
	header.session_id = slot.session_id;
	header.packet_sequence = slot.next_packet_sequence++;
	header.sent_time_us = now_us;
	header.message_id = slot.next_message_id++;
	std::array<std::uint8_t, protocol::MaxDatagramSize> encoded{};
	std::size_t encoded_size = 0U;
	if (!encode_control_datagram(header, {payload.data(), payload_size}, encoded, encoded_size) ||
		encoded_size > m_output.bytes.size()) {
		return dropped(SessionIngressDropReason::OutputBusy);
	}
	protocol::DatagramView begin_view;
	if (protocol::decode_and_validate_datagram({encoded.data(), encoded_size},
			protocol::ProtocolMinorRange{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1},
			begin_view) != protocol::ValidationError::None) {
		return dropped(SessionIngressDropReason::PayloadInvalid);
	}
	protocol::ReliableMessageToRetain retained;
	retained.session_id = slot.session_id;
	retained.endpoint = endpoint;
	retained.message_type = protocol::MessageType::SessionBegin;
	retained.base_flags = protocol::MessageFlagAckRequired;
	retained.message_id = begin_view.header.message_id;
	retained.fragment_count = begin_view.header.fragment_count;
	retained.message_crc32 = begin_view.header.message_crc32;
	retained.logical_payload = {payload.data(), payload_size};
	retained.required_ack = protocol::RequiredAckLevel::Applied;
	retained.message_class = protocol::ReliableMessageClass::SessionCritical;
	if (m_reliable_windows[awaiting].retain(retained, now_us) != protocol::ReliableRetainResult::Retained ||
		!queue_bytes(endpoint, encoded.data(), encoded_size)) {
		return dropped(SessionIngressDropReason::OutputBusy);
	}
	stage(SessionIngressStage::SessionMutation);
	slot.progress = ProducerSessionProgress::ReadyForState;
	slot.reliable_items_in_use = m_reliable_windows[awaiting].entry_count();
	for (auto& cache : m_cache) {
		if (cache.used && cache.session_id == slot.session_id) {
			release_cache_preproof(cache);
		}
	}
	return {SessionIngressDisposition::WelcomeProofApplied, SessionIngressDropReason::None};
}

SessionIngressResult SessionController::ingest_nack(const protocol::EndpointKey& endpoint,
	const protocol::DatagramView& decoded,
	std::uint64_t now_us) noexcept
{
	stage(SessionIngressStage::RateLimit);
	const auto index = find_slot(endpoint, decoded.header.session_id);
	if (index == InvalidIndex) {
		return dropped(SessionIngressDropReason::EndpointSessionMismatch);
	}
	auto& slot = m_slots[index];
	stage(SessionIngressStage::AntiAmplification);
	stage(SessionIngressStage::Payload);
	protocol::NackPayload nack;
	if (protocol::decode_nack_payload(decoded.payload, nack) != protocol::ValidationError::None) {
		return dropped(SessionIngressDropReason::PayloadInvalid);
	}
	if (m_rate_limiter->consume_ack_nack(slot.session_id,
			endpoint,
			{static_cast<std::uint8_t>(nack.target_message_type), nack.target_message_id,
				nack.target_message_crc32},
			now_us) != protocol::ProtocolRateLimitResult::Allowed) {
		return dropped(SessionIngressDropReason::PayloadInvalid);
	}
	const auto preproof = slot.progress == ProducerSessionProgress::AwaitWelcomeApplied;
	const auto received_size = static_cast<std::size_t>(decoded.header.header_size) + decoded.payload.size;
	std::size_t cache_index = InvalidIndex;
	if (preproof) {
		for (std::size_t candidate = 0U; candidate < m_cache.size(); ++candidate) {
			if (m_cache[candidate].used && m_cache[candidate].session_id == slot.session_id) {
				cache_index = candidate;
				break;
			}
		}
	}
	if (preproof && m_preproof.note_validated_receive(endpoint, received_size) !=
			PreproofLedgerResult::Recorded) {
		return dropped(SessionIngressDropReason::AntiAmplificationLimit);
	}
	const auto saved_window = m_reliable_windows[index];
	protocol::ReliableNackDecision decision;
	if (m_reliable_windows[index].reject(slot.session_id, endpoint, nack, now_us, decision) !=
		protocol::ReliableResponseResult::ValidatedRetained) {
		m_reliable_windows[index] = saved_window;
		if (preproof) {
			m_preproof.release_contribution(endpoint, received_size, 0U);
		}
		return dropped(SessionIngressDropReason::PayloadInvalid);
	}
	protocol::ReliableWindowAction action;
	if (m_reliable_windows[index].pull_next_action(now_us, action) != protocol::ReliablePullResult::Action ||
		action.kind != protocol::ReliableWindowActionKind::Retransmit) {
		m_reliable_windows[index] = saved_window;
		if (preproof) {
			m_preproof.release_contribution(endpoint, received_size, 0U);
		}
		return dropped(SessionIngressDropReason::PayloadInvalid);
	}
	const auto& retransmission = action.retransmission;
	protocol::TelemetryDatagramHeader header;
	header.version_minor = protocol::VersionMinorV1_1;
	header.message_type = retransmission.key.message_type;
	header.flags = static_cast<std::uint8_t>(retransmission.base_flags | protocol::MessageFlagRetransmission);
	header.session_id = retransmission.key.session_id;
	header.packet_sequence = slot.next_packet_sequence;
	header.frame_id = retransmission.frame_id;
	header.mission_time_us = retransmission.mission_time_us;
	header.sent_time_us = now_us;
	header.message_id = retransmission.key.message_id;
	header.fragment_count = retransmission.key.fragment_count;
	std::array<std::uint8_t, protocol::MaxDatagramSize> encoded{};
	std::size_t encoded_size = 0U;
	if (!encode_control_datagram(header, retransmission.logical_payload, encoded, encoded_size)) {
		m_reliable_windows[index] = saved_window;
		if (preproof) {
			m_preproof.release_contribution(endpoint, received_size, 0U);
		}
		return dropped(SessionIngressDropReason::PayloadInvalid);
	}
	if (preproof &&
		((cache_index != InvalidIndex &&
			(add_would_overflow(m_cache[cache_index].accounted_received, received_size) ||
				add_would_overflow(m_cache[cache_index].accounted_sent, encoded_size))) ||
			m_preproof.try_account_send(endpoint, encoded_size) != PreproofLedgerResult::Allowed)) {
		m_reliable_windows[index] = saved_window;
		m_preproof.release_contribution(endpoint, received_size, 0U);
		return dropped(SessionIngressDropReason::AntiAmplificationLimit);
	}
	if (!queue_bytes(endpoint, encoded.data(), encoded_size)) {
		m_reliable_windows[index] = saved_window;
		if (preproof) {
			m_preproof.release_contribution(endpoint, received_size, encoded_size);
		}
		return dropped(SessionIngressDropReason::OutputBusy);
	}
	if (preproof && cache_index != InvalidIndex) {
		m_cache[cache_index].accounted_received += received_size;
		m_cache[cache_index].accounted_sent += encoded_size;
		m_cache[cache_index].preproof_active = true;
	}
	++slot.next_packet_sequence;
	stage(SessionIngressStage::SessionMutation);
	return {SessionIngressDisposition::ResponseQueued, SessionIngressDropReason::None};
}

bool SessionController::pop_output(SessionControllerOutput& output) noexcept
{
	if (!m_has_output) {
		return false;
	}
	output = m_output;
	m_output = SessionControllerOutput{};
	m_has_output = false;
	return true;
}

std::size_t SessionController::active_slots() const noexcept
{
	std::size_t count = 0U;
	for (std::size_t i = 0U; i < m_config.max_clients; ++i) {
		count += m_slots[i].progress != ProducerSessionProgress::Empty ? 1U : 0U;
	}
	return count;
}

void SessionController::remove_cache_for_session(std::uint64_t session_id) noexcept
{
	for (auto& entry : m_cache) {
		if (entry.used && entry.session_id == session_id) {
			release_cache_preproof(entry);
			entry = CacheEntry{};
			--m_cache_size;
		}
	}
}

void SessionController::release_cache_preproof(CacheEntry& entry) noexcept
{
	if (!entry.preproof_active) {
		return;
	}
	m_preproof.release_contribution(entry.endpoint, entry.accounted_received, entry.accounted_sent);
	entry.accounted_received = 0U;
	entry.accounted_sent = 0U;
	entry.preproof_active = false;
}

bool SessionController::close_slot(std::size_t index, SessionCloseReason) noexcept
{
	if (index >= m_config.max_clients || m_slots[index].progress == ProducerSessionProgress::Empty) {
		return false;
	}
	const auto old_session_id = m_slots[index].session_id;
	const auto old_endpoint = m_slots[index].endpoint;
	// Session IDs remain process-used in SessionIdRegistry; only slot-owned
	// resources are released here.
	remove_cache_for_session(old_session_id);
	(void)m_rate_limiter->purge_session(old_session_id, old_endpoint);
	return initialize_slot(index);
}

void SessionController::expire_housekeeping(std::uint64_t now_us) noexcept
{
	for (auto& entry : m_cache) {
		if (entry.used && (now_us < entry.stored_at_us || now_us - entry.stored_at_us >= HandshakeCacheLifetimeUs)) {
			release_cache_preproof(entry);
			entry = CacheEntry{};
			--m_cache_size;
		}
	}
	(void)m_rate_limiter->purge_expired(now_us);
}

void SessionController::clear_all() noexcept
{
	for (std::size_t i = 0U; i < m_config.max_clients; ++i) {
		(void)initialize_slot(i);
	}
	m_cache = {};
	m_cache_size = 0U;
	(void)m_preproof.configure(protocol::HandshakeCacheCapacity);
	m_output = {};
	m_has_output = false;
}

SessionControllerOwnedCapacity SessionController::owned_capacity() const noexcept
{
	return {m_config.max_clients,
		m_config.max_clients * protocol::MaxStateReassembliesPerClient,
		m_config.max_clients * protocol::MaxStateReassemblyBytesPerClient,
		m_config.max_clients * Wp06ReliableRetentionBytesPerClient,
		0U,
		m_config.max_clients * sizeof(SessionControllerSlot),
		sizeof(protocol::ProtocolRateLimiter),
		sizeof(m_cache),
		sizeof(m_preproof),
		sizeof(m_output)};
}

std::size_t SessionController::handshake_cache_storage_bytes() noexcept
{
	return sizeof(m_cache);
}

SessionControllerOwnedUsage SessionController::owned_usage() const noexcept
{
	SessionControllerOwnedUsage result;
	result.active_slots = active_slots();
	result.cache_entries = m_cache_size;
	result.preproof_accounts = m_preproof.size();
	result.output_queued = m_has_output;
	for (std::size_t i = 0U; i < m_config.max_clients; ++i) {
		result.reassembly_bytes += m_slots[i].reassembly_bytes_reserved;
		result.reliable_items += m_reliable_windows[i].entry_count();
	}
	return result;
}

} // namespace telemetry::detail
