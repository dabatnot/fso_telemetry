#include "telemetry/session_controller.h"

#include "telemetry/protocol/telemetry_control_messages.h"
#include "telemetry/protocol/telemetry_crc32.h"
#include "telemetry/protocol/telemetry_datagram.h"
#include "telemetry/protocol/telemetry_reliability_messages.h"
#include "telemetry/transport.h"

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
const std::size_t Wp06SnapshotEgressHeapBytesPerClient =
	Phase1SnapshotEgress::startup_heap_bytes({1U, 1U});
const std::size_t Wp06DeltaEgressHeapBytesPerClient = Phase1DeltaEgress::StartupHeapBytes;
const std::size_t Wp06DeltaScratchHeapBytesPerClient =
	4U * (sizeof(protocol::StateMutation) + 84U + sizeof(std::uint64_t) * 2U);
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
		config.mission_heartbeat_ms < protocol::MinHeartbeatIntervalMs ||
		config.mission_heartbeat_ms > protocol::MaxHeartbeatIntervalMs ||
		config.idle_heartbeat_ms < protocol::MinHeartbeatIntervalMs ||
		config.idle_heartbeat_ms > protocol::MaxHeartbeatIntervalMs ||
		config.keyframe_seconds < 1U || config.keyframe_seconds > 5U ||
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
	const auto budget = calculate_wp06_startup_budget(
		calculate_wp04_startup_budget(make_wp03_known_budget_request(config.max_clients)), config.max_clients);
	// Do not publish Ready unless every startup-priced P8 capacity corresponds
	// to a physically retained buffer. This catches a changed reserve policy,
	// an overflow, or a partial provisioning failure before bind.
	if (Wp06SnapshotEgressHeapBytesPerClient == 0U || Wp06DeltaEgressHeapBytesPerClient == 0U ||
		Wp06DeltaScratchHeapBytesPerClient == 0U || !wp06_budget_matches_owned_storage(budget, candidate.owned_capacity())) {
		return SessionControllerConfigureResult::AllocationFailure;
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
	auto snapshot_egress = std::move(m_slots[index].snapshot_egress);
	auto delta_egress = std::move(m_slots[index].delta_egress);
	auto delta_scratch = std::move(m_slots[index].delta_scratch);
	const auto needs_snapshot_egress_configuration = !snapshot_egress.configured();
	m_slots[index] = SessionControllerSlot{};
	m_slots[index].snapshot_egress = std::move(snapshot_egress);
	m_slots[index].delta_egress = std::move(delta_egress);
	m_slots[index].delta_scratch = std::move(delta_scratch);
	m_slots[index].snapshot.set_allocation_observer(&m_phase1_allocation_observer);
	m_slots[index].snapshot_egress.set_allocation_observer(&m_phase1_allocation_observer);
	m_slots[index].delta_egress.set_allocation_observer(&m_phase1_allocation_observer);
	// Delta egress owns its bounded byte buffers for the lifetime of the
	// controller slot. Reconnect/reset only clears logical session state; it
	// must not discard the preprovisioned capacities and allocate after Ready.
	m_slots[index].delta_egress.discard();
	if (!m_slots[index].delta_egress.provisioned() && !m_slots[index].delta_egress.provision()) {
		return false;
	}
	try {
		auto& scratch = m_slots[index].delta_scratch;
		if (scratch.mutations.capacity() < 4U) scratch.mutations.reserve(4U);
		scratch.mutations.resize(4U);
		for (auto& mutation : scratch.mutations) {
			mutation.atom.key.identity.reserve(sizeof(std::uint64_t));
			mutation.atom.value.reserve(84U);
			mutation.atom.cascade_owner.identity.reserve(sizeof(std::uint64_t));
		}
	} catch (const std::bad_alloc&) {
		return false;
	}
	m_reliable_windows[index].configure();
	if (needs_snapshot_egress_configuration) {
		return m_slots[index].snapshot_egress.configure({1U, 1U});
	}
	m_slots[index].snapshot_egress.rollback_candidate();
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
	std::size_t size,
	std::size_t owner_slot) noexcept
{
	if (m_has_output || size > m_output.bytes.size()) {
		return false;
	}
	m_output = SessionControllerOutput{};
	m_output.endpoint = endpoint;
	std::memcpy(m_output.bytes.data(), bytes, size);
	m_output.size = size;
	m_output_owner_slot = owner_slot;
	m_output_snapshot_egress_pending = false;
	m_output_delta_egress_pending = false;
	m_output_heartbeat_pending = false;
	m_output_heartbeat_owns_probe = false;
	m_output_heartbeat_probe = {};
	m_has_output = true;
	return true;
}

bool SessionController::queue_retransmission(std::size_t slot_index,
	const protocol::ReliableWindowAction& action,
	std::uint64_t now_us) noexcept
{
	if (slot_index >= m_config.max_clients || action.kind != protocol::ReliableWindowActionKind::Retransmit) {
		return false;
	}
	const auto& retransmission = action.retransmission;
	protocol::TelemetryDatagramHeader header;
	header.version_minor = protocol::VersionMinorV1_1;
	header.message_type = retransmission.key.message_type;
	header.flags = static_cast<std::uint8_t>(retransmission.base_flags | protocol::MessageFlagRetransmission);
	header.session_id = retransmission.key.session_id;
	header.packet_sequence = m_slots[slot_index].next_packet_sequence;
	header.frame_id = retransmission.frame_id;
	header.mission_time_us = retransmission.mission_time_us;
	header.sent_time_us = now_us;
	header.message_id = retransmission.key.message_id;
	header.fragment_count = retransmission.key.fragment_count;
	std::array<std::uint8_t, protocol::MaxDatagramSize> encoded{};
	std::size_t encoded_size = 0U;
	if (!encode_control_datagram(header, retransmission.logical_payload, encoded, encoded_size)) {
		return false;
	}
	// This is a selected reliable retransmission, not a speculative scheduler
	// probe: it may legitimately replace a tail-queued Delta.
	preempt_queued_delta();
	if (!queue_bytes(m_slots[slot_index].endpoint, encoded.data(), encoded_size, slot_index)) {
		return false;
	}
	m_output_reliability_pending = true;
	m_pending_preproof_send_accounted = false;
	m_pending_reliability_slot = slot_index;
	m_pending_reliability_time_us = now_us;
	return true;
}

void SessionController::preempt_queued_delta() noexcept
{
	if (!m_has_output || !m_output_delta_egress_pending) {
		return;
	}
	if (m_output_owner_slot < m_config.max_clients) {
		m_slots[m_output_owner_slot].delta_egress.release_output_for_preemption();
	}
	m_output = {};
	m_has_output = false;
	m_output_owner_slot = InvalidIndex;
	m_output_delta_egress_pending = false;
}

bool SessionController::queue_heartbeat(std::size_t slot_index,
	const protocol::HeartbeatPayload& heartbeat,
	std::uint64_t now_us,
	bool owns_probe,
	const protocol::ProbeToken& probe) noexcept
{
	if (slot_index >= m_config.max_clients) {
		return false;
	}
	std::array<std::uint8_t, protocol::HeartbeatPayloadSize> payload{};
	std::size_t payload_size = 0U;
	if (protocol::encode_heartbeat_payload(heartbeat, {payload.data(), payload.size()}, payload_size) !=
		protocol::ValidationError::None) {
		return false;
	}
	protocol::TelemetryDatagramHeader header;
	header.version_minor = protocol::VersionMinorV1_1;
	header.message_type = protocol::MessageType::Heartbeat;
	header.session_id = m_slots[slot_index].session_id;
	header.packet_sequence = m_slots[slot_index].next_packet_sequence;
	header.sent_time_us = now_us;
	header.message_id = m_slots[slot_index].next_message_id;
	std::array<std::uint8_t, protocol::MaxDatagramSize> encoded{};
	std::size_t encoded_size = 0U;
	if (!encode_control_datagram(header, {payload.data(), payload_size}, encoded, encoded_size)) {
		return false;
	}
	// A heartbeat is concrete higher-priority control work, but it must never
	// displace committed reliable/snapshot output. Check this before releasing
	// a tail Delta so malformed/oversized heartbeat work cannot alter egress.
	if (m_has_output && !m_output_delta_egress_pending) {
		return false;
	}
	const auto preempted_delta_owner = m_output_delta_egress_pending ? m_output_owner_slot : InvalidIndex;
	if (preempted_delta_owner != InvalidIndex) {
		preempt_queued_delta();
	}
	if (!queue_bytes(m_slots[slot_index].endpoint, encoded.data(), encoded_size, slot_index)) {
		// queue_bytes is expected to succeed after the fixed-size encoding and
		// output-priority checks above. Keep the defensive rollback transactional:
		// an unsent cumulative Delta remains eligible if a future implementation
		// adds another queue admission failure.
		if (preempted_delta_owner < m_config.max_clients &&
			m_slots[preempted_delta_owner].delta_egress.service(
				m_slots[preempted_delta_owner].next_packet_sequence, now_us)) {
			Phase1DeltaDatagram delta;
			if (m_slots[preempted_delta_owner].delta_egress.peek_output(delta) &&
				queue_bytes(delta.endpoint, delta.bytes.data(), delta.size, preempted_delta_owner)) {
				m_output_delta_egress_pending = true;
			}
		}
		return false;
	}
	m_output_heartbeat_pending = true;
	m_output_heartbeat_owns_probe = owns_probe;
	m_output_heartbeat_probe = owns_probe ? probe : protocol::ProbeToken{};
	return true;
}

void SessionController::note_network_activity(std::size_t slot_index, std::uint64_t now_us) noexcept
{
	if (slot_index >= m_config.max_clients) {
		return;
	}
	auto& last = m_slots[slot_index].heartbeat.last_valid_network_activity_us;
	if (now_us >= last) {
		last = now_us;
	}
}

void SessionController::apply_terminal_policy(std::size_t slot_index,
	protocol::ReliableTerminalPolicy policy) noexcept
{
	if (slot_index >= m_config.max_clients) {
		return;
	}
	if (policy == protocol::ReliableTerminalPolicy::CloseSession) {
		(void)close_slot(slot_index, SessionCloseReason::ProtocolError);
		return;
	}
	auto& slot = m_slots[slot_index];
	if (policy == protocol::ReliableTerminalPolicy::MarkSessionStale) {
		slot.progress = ProducerSessionProgress::Stale;
		slot.has_reliability_terminal_policy = true;
		slot.reliability_terminal_policy = policy;
	}
	slot.reliable_items_in_use = m_reliable_windows[slot_index].entry_count();
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
	protocol::DatagramView decoded;
	bool predecoded_for_output_arbitration = false;
	if (m_has_output) {
		if (protocol::decode_and_validate_datagram(datagram,
				protocol::ProtocolMinorRange{protocol::VersionMinorV1_0, protocol::VersionMinorV1_1},
				decoded) != protocol::ValidationError::None) {
			return dropped(SessionIngressDropReason::OutputBusy);
		}
		const auto is_control_response = decoded.header.message_type == protocol::MessageType::Ack ||
			decoded.header.message_type == protocol::MessageType::Nack ||
			decoded.header.message_type == protocol::MessageType::Heartbeat ||
			decoded.header.message_type == protocol::MessageType::ResyncRequest;
		// Only a tail-queued Delta can yield to ingress control work.  Do not
		// preempt here: endpoint/session, quota and payload semantic validation
		// still belong to the concrete message handler.  The handler that has
		// accepted and is about to queue its response performs the preemption.
		// This keeps even a valid-envelope hostile datagram from changing egress
		// state, while preserving already-queued higher-priority output.
		// Heartbeats must remain admissible behind committed priority output. A
		// correlated Response refreshes its existing probe without egress work;
		// a Request is deliberately allowed to refresh activity before its
		// response reports OutputBusy. The concrete handler repeats payload,
		// tuple, rate-limit and probe validation before every mutation, and
		// queue_heartbeat cannot replace non-Delta output.
		const auto heartbeat_is_admissible_behind_priority =
			decoded.header.message_type == protocol::MessageType::Heartbeat;
		if ((!m_output_delta_egress_pending && !heartbeat_is_admissible_behind_priority) || !is_control_response) {
			return dropped(SessionIngressDropReason::OutputBusy);
		}
		predecoded_for_output_arbitration = true;
	}
	stage(SessionIngressStage::DatagramEnvelope);
	if (!predecoded_for_output_arbitration && protocol::decode_and_validate_datagram(datagram,
			protocol::ProtocolMinorRange{protocol::VersionMinorV1_0, protocol::VersionMinorV1_1},
			decoded) != protocol::ValidationError::None) {
		return dropped(SessionIngressDropReason::DatagramEnvelopeInvalid);
	}
	stage(SessionIngressStage::EndpointAndSession);
	if (decoded.header.message_type == protocol::MessageType::Hello) {
		if (decoded.header.session_id != 0U) {
			return dropped(SessionIngressDropReason::EndpointSessionMismatch);
		}
		return ingest_hello(endpoint, decoded, datagram.size, now_us, mission_active);
	}
	if (decoded.header.message_type == protocol::MessageType::Ack) {
		return ingest_ack(endpoint, decoded, now_us, mission_generation, mission_active);
	}
	if (decoded.header.message_type == protocol::MessageType::Nack) {
		return ingest_nack(endpoint, decoded, now_us);
	}
	if (decoded.header.message_type == protocol::MessageType::Heartbeat) {
		return ingest_heartbeat(endpoint, decoded, now_us);
	}
	if (decoded.header.message_type == protocol::MessageType::ResyncRequest) {
		return ingest_resync_request(endpoint, decoded, now_us);
	}
	return dropped(SessionIngressDropReason::EndpointSessionMismatch);
}

SessionIngressResult SessionController::ingest_hello(const protocol::EndpointKey& endpoint,
	const protocol::DatagramView& decoded,
	std::size_t received_size,
	std::uint64_t now_us,
	bool mission_active) noexcept
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
		if (!queue_bytes(endpoint,
			cached.bytes.data(),
			cached.size,
			cached.session_id == 0U ? InvalidIndex : find_slot(endpoint, cached.session_id))) {
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
	welcome.heartbeat_interval_ms =
		supported ? (mission_active ? m_config.mission_heartbeat_ms : m_config.idle_heartbeat_ms) : 0U;
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
		slot.heartbeat.negotiated_interval_ms = welcome.heartbeat_interval_ms;
		(void)slot.heartbeat.probes.reset_session(session_id);
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
	if (!queue_bytes(endpoint, encoded.data(), encoded_size, supported ? slot_index : InvalidIndex)) {
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
		if (slot.progress != ProducerSessionProgress::ReadyForState) {
			return dropped(SessionIngressDropReason::PayloadInvalid);
		}
		stage(SessionIngressStage::AntiAmplification);
		stage(SessionIngressStage::Payload);
		protocol::AckPayload ack;
		if (protocol::decode_ack_payload(decoded.payload, ack) != protocol::ValidationError::None ||
			m_rate_limiter->consume_ack_nack(slot.session_id,
				endpoint,
				{static_cast<std::uint8_t>(ack.target_message_type), ack.target_message_id,
					ack.target_message_crc32},
				now_us) != protocol::ProtocolRateLimitResult::Allowed) {
			return dropped(SessionIngressDropReason::PayloadInvalid);
		}
		if (ack.target_message_type == protocol::MessageType::FullSnapshot && slot.snapshot_egress.has_candidate()) {
			const auto reliable = slot.snapshot_egress.acknowledge_candidate_part(ack, now_us);
			if (reliable != protocol::ReliableResponseResult::ValidatedRetained &&
				reliable != protocol::ReliableResponseResult::Released &&
				reliable != protocol::ReliableResponseResult::Duplicate) {
				return dropped(SessionIngressDropReason::PayloadInvalid);
			}
			const auto baseline = slot.snapshot.acknowledge_candidate_part(ack, now_us);
			if (baseline != protocol::ProducerBaselineResult::Applied && baseline != protocol::ProducerBaselineResult::NoChange) {
				slot.snapshot_egress.rollback_candidate();
				slot.snapshot.rollback_candidate();
				return dropped(SessionIngressDropReason::PayloadInvalid);
			}
			stage(SessionIngressStage::SessionMutation);
			note_network_activity(awaiting, now_us);
			return {SessionIngressDisposition::ResponseQueued, SessionIngressDropReason::None};
		}
		const auto response = m_reliable_windows[awaiting].acknowledge(slot.session_id, endpoint, ack, now_us);
		if (response != protocol::ReliableResponseResult::ValidatedRetained &&
			response != protocol::ReliableResponseResult::Duplicate &&
			response != protocol::ReliableResponseResult::Released) {
			return dropped(SessionIngressDropReason::PayloadInvalid);
		}
		stage(SessionIngressStage::SessionMutation);
		slot.reliable_items_in_use = m_reliable_windows[awaiting].entry_count();
		note_network_activity(awaiting, now_us);
		return {SessionIngressDisposition::ResponseQueued, SessionIngressDropReason::None};
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
	// The initial FullSnapshot is captured only after SESSION_BEGIN is ACKed.
	// Zero keeps the peer synchronizing until that reliable transaction arrives.
	begin.initial_snapshot_id = 0U;
	begin.required_manifest_id = 0U;
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
		!queue_bytes(endpoint, encoded.data(), encoded_size, awaiting)) {
		return dropped(SessionIngressDropReason::OutputBusy);
	}
	stage(SessionIngressStage::SessionMutation);
	slot.progress = ProducerSessionProgress::ReadyForState;
	slot.reliable_items_in_use = m_reliable_windows[awaiting].entry_count();
	std::uint32_t stale_timeout_ms = 0U;
	std::uint32_t disconnect_timeout_ms = 0U;
	if (protocol::compute_clock_stale_timeout_ms(slot.heartbeat.negotiated_interval_ms, stale_timeout_ms) !=
			protocol::ClockTimeoutResult::Computed ||
		protocol::compute_session_disconnect_timeout_ms(
			slot.heartbeat.negotiated_interval_ms, disconnect_timeout_ms) != protocol::ClockTimeoutResult::Computed) {
		return dropped(SessionIngressDropReason::PayloadInvalid);
	}
	const auto interval_us = static_cast<std::uint64_t>(slot.heartbeat.negotiated_interval_ms) * 1000U;
	slot.heartbeat.stale_timeout_us = static_cast<std::uint64_t>(stale_timeout_ms) * 1000U;
	slot.heartbeat.disconnect_timeout_us = static_cast<std::uint64_t>(disconnect_timeout_ms) * 1000U;
	slot.heartbeat.next_periodic_due_us = add_would_overflow(now_us, interval_us)
		? std::numeric_limits<std::uint64_t>::max()
		: now_us + interval_us;
	const auto keyframe_interval_us = static_cast<std::uint64_t>(m_config.keyframe_seconds) * 1'000'000U;
	slot.next_keyframe_due_us = add_would_overflow(now_us, keyframe_interval_us)
		? std::numeric_limits<std::uint64_t>::max()
		: now_us + keyframe_interval_us;
	slot.keyframe_due = false;
	slot.heartbeat.last_valid_network_activity_us = now_us;
	slot.heartbeat.last_valid_clock_response_us = now_us;
	slot.heartbeat.clock_stale = false;
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
	if (nack.target_message_type == protocol::MessageType::FullSnapshot && slot.snapshot_egress.has_candidate()) {
		protocol::ReliableNackDecision decision;
		const auto response = slot.snapshot_egress.reject_candidate_part(nack, now_us, decision);
		if (response != protocol::ReliableResponseResult::ValidatedRetained) {
			return dropped(SessionIngressDropReason::PayloadInvalid);
		}
		if (decision.kind == protocol::ReliableNackDecisionKind::TerminalPolicy) {
			slot.snapshot_egress.rollback_candidate();
			slot.snapshot.rollback_candidate();
			(void)close_slot(index, SessionCloseReason::ProtocolError);
			return {SessionIngressDisposition::ResponseQueued, SessionIngressDropReason::None};
		}
		if (decision.kind != protocol::ReliableNackDecisionKind::WaitForScheduledRetry) {
			protocol::ReliableWindowAction action;
			if (slot.snapshot_egress.pull_reliability(now_us, action) != protocol::ReliablePullResult::Action ||
				action.kind != protocol::ReliableWindowActionKind::Retransmit ||
				!slot.snapshot_egress.queue_retransmission(action, slot.next_packet_sequence, now_us)) {
				slot.snapshot_egress.rollback_candidate();
				slot.snapshot.rollback_candidate();
				(void)close_slot(index, SessionCloseReason::ProtocolError);
				return dropped(SessionIngressDropReason::PayloadInvalid);
			}
			Phase1SnapshotDatagram datagram;
			if (!slot.snapshot_egress.peek_output(datagram)) {
				slot.snapshot_egress.rollback_candidate();
				slot.snapshot.rollback_candidate();
				(void)close_slot(index, SessionCloseReason::TransportError);
				return dropped(SessionIngressDropReason::OutputBusy);
			}
			preempt_queued_delta();
			if (!queue_bytes(datagram.endpoint, datagram.bytes.data(), datagram.size, index)) {
				slot.snapshot_egress.rollback_candidate();
				slot.snapshot.rollback_candidate();
				(void)close_slot(index, SessionCloseReason::TransportError);
				return dropped(SessionIngressDropReason::OutputBusy);
			}
			m_output_snapshot_egress_pending = true;
			++slot.next_packet_sequence;
		}
		note_network_activity(index, now_us);
		stage(SessionIngressStage::SessionMutation);
		return {SessionIngressDisposition::ResponseQueued, SessionIngressDropReason::None};
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
	const auto response = m_reliable_windows[index].reject(slot.session_id, endpoint, nack, now_us, decision);
	if (response == protocol::ReliableResponseResult::Released &&
		decision.kind == protocol::ReliableNackDecisionKind::TerminalPolicy) {
		if (preproof) {
			m_preproof.release_contribution(endpoint, received_size, 0U);
		}
		slot.reliable_items_in_use = m_reliable_windows[index].entry_count();
		apply_terminal_policy(index, decision.terminal_policy);
		note_network_activity(index, now_us);
		stage(SessionIngressStage::SessionMutation);
		return {SessionIngressDisposition::ResponseQueued, SessionIngressDropReason::None};
	}
	if (response != protocol::ReliableResponseResult::ValidatedRetained) {
		m_reliable_windows[index] = saved_window;
		if (preproof) {
			m_preproof.release_contribution(endpoint, received_size, 0U);
		}
		return dropped(SessionIngressDropReason::PayloadInvalid);
	}
	if (decision.kind == protocol::ReliableNackDecisionKind::WaitForScheduledRetry) {
		if (preproof && cache_index != InvalidIndex) {
			if (add_would_overflow(m_cache[cache_index].accounted_received, received_size)) {
				m_reliable_windows[index] = saved_window;
				m_preproof.release_contribution(endpoint, received_size, 0U);
				return dropped(SessionIngressDropReason::AntiAmplificationLimit);
			}
			m_cache[cache_index].accounted_received += received_size;
			m_cache[cache_index].preproof_active = true;
		}
		if (preproof) {
			const auto account = m_preproof.account(endpoint);
			slot.preproof_validated_bytes_received = account.validated_bytes_received;
			slot.preproof_bytes_sent = account.bytes_sent;
		}
		slot.reliable_items_in_use = m_reliable_windows[index].entry_count();
		note_network_activity(index, now_us);
		stage(SessionIngressStage::SessionMutation);
		return {SessionIngressDisposition::ResponseQueued, SessionIngressDropReason::None};
	}
	auto preview = m_reliable_windows[index];
	protocol::ReliableWindowAction action;
	if ((decision.kind != protocol::ReliableNackDecisionKind::SelectiveRetransmissionScheduled &&
			decision.kind != protocol::ReliableNackDecisionKind::FullRetransmissionScheduled) ||
		preview.pull_next_action(now_us, action) != protocol::ReliablePullResult::Action ||
		action.kind != protocol::ReliableWindowActionKind::Retransmit || !queue_retransmission(index, action, now_us)) {
		m_reliable_windows[index] = saved_window;
		if (preproof) {
			m_preproof.release_contribution(endpoint, received_size, 0U);
		}
		return dropped(SessionIngressDropReason::PayloadInvalid);
	}
	if (preproof &&
		((cache_index != InvalidIndex &&
			(add_would_overflow(m_cache[cache_index].accounted_received, received_size) ||
				add_would_overflow(m_cache[cache_index].accounted_sent, m_output.size))) ||
			m_preproof.try_account_send(endpoint, m_output.size) != PreproofLedgerResult::Allowed)) {
		m_reliable_windows[index] = saved_window;
		m_preproof.release_contribution(endpoint, received_size, 0U);
		m_output = {};
		m_has_output = false;
		m_output_owner_slot = InvalidIndex;
		m_output_reliability_pending = false;
		m_pending_reliability_slot = InvalidIndex;
		m_output_heartbeat_pending = false;
		m_output_heartbeat_owns_probe = false;
		m_output_heartbeat_probe = {};
		return dropped(SessionIngressDropReason::AntiAmplificationLimit);
	}
	if (preproof && cache_index != InvalidIndex) {
		m_cache[cache_index].accounted_received += received_size;
		m_cache[cache_index].accounted_sent += m_output.size;
		m_cache[cache_index].preproof_active = true;
	}
	if (preproof) {
		const auto account = m_preproof.account(endpoint);
		slot.preproof_validated_bytes_received = account.validated_bytes_received;
		slot.preproof_bytes_sent = account.bytes_sent;
	}
	m_pending_preproof_send_accounted = preproof;
	note_network_activity(index, now_us);
	stage(SessionIngressStage::SessionMutation);
	return {SessionIngressDisposition::ResponseQueued, SessionIngressDropReason::None};
}

bool SessionController::queue_resync_validated_ack(std::size_t slot_index,
	const protocol::DatagramView& request,
	std::uint64_t now_us) noexcept
{
	if (slot_index >= m_config.max_clients) {
		return false;
	}
	auto& slot = m_slots[slot_index];
	protocol::AckPayload ack;
	ack.target_message_id = request.header.message_id;
	ack.target_message_type = protocol::MessageType::ResyncRequest;
	ack.ack_flags = static_cast<std::uint8_t>(protocol::AckFlag::Validated);
	ack.target_fragment_count = request.header.fragment_count;
	ack.target_message_crc32 = request.header.message_crc32;
	std::array<std::uint8_t, protocol::AckPayloadSize> payload{};
	std::size_t payload_size = 0U;
	if (protocol::encode_ack_payload(ack, {payload.data(), payload.size()}, payload_size) != protocol::ValidationError::None) {
		return false;
	}
	protocol::TelemetryDatagramHeader header;
	header.version_minor = protocol::VersionMinorV1_1;
	header.message_type = protocol::MessageType::Ack;
	header.session_id = slot.session_id;
	// Reserve both identities when the ACK enters the single output slot. A
	// Resync keyframe may be reserved immediately afterwards, before transport
	// completion, so it must never reuse this ACK's message or packet identity.
	header.packet_sequence = slot.next_packet_sequence;
	header.sent_time_us = now_us;
	header.message_id = slot.next_message_id;
	std::array<std::uint8_t, protocol::MaxDatagramSize> encoded{};
	std::size_t encoded_size = 0U;
	if (!encode_control_datagram(header, {payload.data(), payload_size}, encoded, encoded_size)) {
		return false;
	}
	// The validated ACK is immediate control work. It may supersede a queued
	// Delta, but must preserve any output that already has higher priority.
	preempt_queued_delta();
	if (!queue_bytes(slot.endpoint, encoded.data(), encoded_size, slot_index)) {
		return false;
	}
	++slot.next_packet_sequence;
	++slot.next_message_id;
	m_output_resync_ack_pending = true;
	return true;
}

SessionIngressResult SessionController::ingest_resync_request(const protocol::EndpointKey& endpoint,
	const protocol::DatagramView& decoded,
	std::uint64_t now_us) noexcept
{
	stage(SessionIngressStage::RateLimit);
	const auto index = find_slot(endpoint, decoded.header.session_id);
	if (index == InvalidIndex || m_slots[index].progress != ProducerSessionProgress::ReadyForState) {
		return dropped(SessionIngressDropReason::EndpointSessionMismatch);
	}
	auto& slot = m_slots[index];
	protocol::ResyncRequestPayload request;
	stage(SessionIngressStage::AntiAmplification);
	stage(SessionIngressStage::Payload);
	if (protocol::decode_resync_request_payload(decoded.payload, request) != protocol::ValidationError::None) {
		return dropped(SessionIngressDropReason::PayloadInvalid);
	}
	// Expire first, then let an exact reliable replay bypass the token bucket.
	// It remains the same bounded semantic identity and cannot reserve another
	// keyframe candidate. Every new/coalesced identity is still charged at the
	// Phase 0 ResyncRequest rate before it mutates the tracker.
	(void)slot.resync.expire(now_us);
	const auto duplicate = slot.resync.is_known_duplicate(request);
	if (!duplicate &&
		m_rate_limiter->consume_session(protocol::RateLimitClass::ResyncRequest, slot.session_id, endpoint, now_us) !=
			protocol::ProtocolRateLimitResult::Allowed) {
		return dropped(SessionIngressDropReason::PayloadInvalid);
	}
	const auto accepted = slot.resync.accept(request, now_us);
	if (!duplicate && !protocol::producer_resync_result_requires_validated_ack(accepted)) {
		return dropped(SessionIngressDropReason::PayloadInvalid);
	}
	if (!queue_resync_validated_ack(index, decoded, now_us)) {
		return dropped(SessionIngressDropReason::OutputBusy);
	}
	if (protocol::producer_resync_result_starts_candidate(accepted)) {
		// A resync is a concrete recovery request, not a request to wait behind a
		// periodic keyframe that has not been applied.  Keeping that candidate
		// until its transaction deadline adds the entire reliable window before
		// recovery can even start, which violates the bounded Live convergence
		// requirement under ordinary loss.  Both candidate owners are paired;
		// abandon only a replacement candidate so the immutable active baseline
		// remains valid while the fresh Resync keyframe is built.
		if (slot.snapshot.has_candidate() || slot.snapshot_egress.has_candidate()) {
			if (slot.snapshot.has_candidate() && slot.snapshot_egress.has_candidate() &&
				slot.snapshot.has_active_baseline()) {
				slot.snapshot_egress.rollback_candidate();
				(void)slot.snapshot.abandon_replacement_candidate();
			}
		}
		if (!slot.snapshot.has_candidate() && !slot.snapshot_egress.has_candidate() &&
			begin_replacement_snapshot(index, protocol::SnapshotFlagResync, now_us)) {
			(void)slot.resync.complete();
		}
	}
	note_network_activity(index, now_us);
	stage(SessionIngressStage::SessionMutation);
	return {SessionIngressDisposition::ResponseQueued, SessionIngressDropReason::None};
}

SessionIngressResult SessionController::ingest_heartbeat(const protocol::EndpointKey& endpoint,
	const protocol::DatagramView& decoded,
	std::uint64_t now_us) noexcept
{
	stage(SessionIngressStage::RateLimit);
	const auto index = find_slot(endpoint, decoded.header.session_id);
	if (index == InvalidIndex) {
		return dropped(SessionIngressDropReason::EndpointSessionMismatch);
	}
	auto& slot = m_slots[index];
	if (slot.progress != ProducerSessionProgress::ReadyForState && slot.progress != ProducerSessionProgress::Stale) {
		return dropped(SessionIngressDropReason::PayloadInvalid);
	}
	stage(SessionIngressStage::AntiAmplification);
	stage(SessionIngressStage::Payload);
	protocol::HeartbeatPayload heartbeat;
	if (protocol::decode_heartbeat_payload(decoded.payload, heartbeat) != protocol::ValidationError::None) {
		return dropped(SessionIngressDropReason::PayloadInvalid);
	}
	if (heartbeat.kind == protocol::HeartbeatKind::Request) {
		if (m_rate_limiter->consume_session(
				protocol::RateLimitClass::HeartbeatRequest, slot.session_id, endpoint, now_us) !=
			protocol::ProtocolRateLimitResult::Allowed) {
			return dropped(SessionIngressDropReason::PayloadInvalid);
		}
		note_network_activity(index, now_us);
		protocol::HeartbeatPayload response;
		response.probe_id = heartbeat.probe_id;
		response.kind = protocol::HeartbeatKind::Response;
		response.origin_t0_us = heartbeat.origin_t0_us;
		response.receive_t1_us = now_us;
		response.transmit_t2_us = now_us;
		if (!queue_heartbeat(index, response, now_us, false, {})) {
			return dropped(SessionIngressDropReason::OutputBusy);
		}
		stage(SessionIngressStage::SessionMutation);
		return {SessionIngressDisposition::ResponseQueued, SessionIngressDropReason::None};
	}
	if (slot.heartbeat.probes.preview_response(
			slot.session_id, heartbeat.probe_id, heartbeat.origin_t0_us) != protocol::ProbeResponseResult::Matched) {
		return dropped(SessionIngressDropReason::PayloadInvalid);
	}
	protocol::ClockSample sample;
	const auto sample_result = protocol::compute_clock_sample({heartbeat.origin_t0_us,
			heartbeat.receive_t1_us,
			heartbeat.transmit_t2_us,
			now_us},
			sample);
	if (sample_result != protocol::ClockSampleResult::Valid) {
		return dropped(SessionIngressDropReason::PayloadInvalid);
	}
	protocol::OrientedClockSample oriented;
	if (protocol::orient_clock_sample(sample, protocol::LocalClockRole::Initiator, oriented) !=
		protocol::ClockOffsetConversionResult::Converted) {
		return dropped(SessionIngressDropReason::PayloadInvalid);
	}
	// Commit only after every untrusted timestamp has passed clock and
	// orientation validation. A rejected response must leave the live probe,
	// activity and clock state exactly as it found them.
	if (slot.heartbeat.probes.correlate_response(
			slot.session_id, heartbeat.probe_id, heartbeat.origin_t0_us) != protocol::ProbeResponseResult::Matched) {
		return dropped(SessionIngressDropReason::PayloadInvalid);
	}
	note_network_activity(index, now_us);
	slot.heartbeat.clock_filter.add_sample(oriented);
	if (now_us >= slot.heartbeat.last_valid_clock_response_us) {
		slot.heartbeat.last_valid_clock_response_us = now_us;
	}
	slot.heartbeat.clock_stale = false;
	stage(SessionIngressStage::SessionMutation);
	return {SessionIngressDisposition::ResponseQueued, SessionIngressDropReason::None};
}

bool SessionController::pop_output(SessionControllerOutput& output) noexcept
{
	if (!peek_output(output)) {
		return false;
	}
	complete_output(IoStatus::Complete);
	return true;
}

bool SessionController::peek_output(SessionControllerOutput& output) const noexcept
{
	if (!m_has_output) {
		return false;
	}
	output = m_output;
	return true;
}

void SessionController::complete_output(IoStatus status) noexcept
{
	if (!m_has_output) {
		return;
	}
	if (m_output_delta_egress_pending) {
		const auto owner = m_output_owner_slot;
		if (owner < m_config.max_clients) {
			if (status == IoStatus::Complete) {
				m_slots[owner].delta_egress.complete_output();
				++m_slots[owner].next_packet_sequence;
			} else {
				// A non-reliable state message is deliberately abandoned on
				// WouldBlock/error; the next cumulative image replaces it.
				m_slots[owner].delta_egress.discard();
			}
		}
		m_output = {};
		m_has_output = false;
		m_output_owner_slot = InvalidIndex;
		m_output_delta_egress_pending = false;
		if ((status == IoStatus::Closed || status == IoStatus::Error) && owner < m_config.max_clients) {
			(void)close_slot(owner, SessionCloseReason::TransportError);
		}
		return;
	}
	if (m_output_resync_ack_pending) {
		const auto owner = m_output_owner_slot;
		if (status == IoStatus::WouldBlock) {
			return;
		}
		m_output = {};
		m_has_output = false;
		m_output_owner_slot = InvalidIndex;
		m_output_resync_ack_pending = false;
		if ((status == IoStatus::Closed || status == IoStatus::Error) && owner < m_config.max_clients) {
			(void)close_slot(owner, SessionCloseReason::TransportError);
		}
		return;
	}
	if (m_output_snapshot_egress_pending) {
		const auto owner = m_output_owner_slot;
		if (status == IoStatus::WouldBlock) {
			return;
		}
		if (owner < m_config.max_clients) {
			if (status == IoStatus::Complete) {
				m_slots[owner].snapshot_egress.complete_output();
			} else {
				m_slots[owner].snapshot_egress.rollback_candidate();
				m_slots[owner].snapshot.rollback_candidate();
			}
		}
		m_output = {};
		m_has_output = false;
		m_output_owner_slot = InvalidIndex;
		m_output_snapshot_egress_pending = false;
		if (status == IoStatus::Complete && owner < m_config.max_clients) {
			auto& slot = m_slots[owner];
			// `pull_reliability` consumes an RTO action once.  Continue its
			// bounded selection here, one transport completion at a time, rather
			// than waiting for a second reliable-window action (or window).
			if (slot.snapshot_egress.has_retransmission_pending()) {
				if (slot.snapshot_egress.queue_next_retransmission(slot.next_packet_sequence)) {
					Phase1SnapshotDatagram datagram;
					if (slot.snapshot_egress.peek_output(datagram) &&
						queue_bytes(datagram.endpoint, datagram.bytes.data(), datagram.size, owner)) {
						m_output_snapshot_egress_pending = true;
						++slot.next_packet_sequence;
						return;
					}
				}
				if (slot.snapshot_egress.has_retransmission_pending()) {
					slot.snapshot_egress.rollback_candidate();
					slot.snapshot.rollback_candidate();
					(void)close_slot(owner, SessionCloseReason::TransportError);
					return;
				}
			}
		}
		if ((status == IoStatus::Closed || status == IoStatus::Error) && owner < m_config.max_clients) {
			(void)close_slot(owner, SessionCloseReason::TransportError);
		}
		return;
	}
	if (status == IoStatus::WouldBlock) {
		if (!m_output_heartbeat_pending) {
			return;
		}
		if (m_output_heartbeat_owns_probe && m_output_owner_slot < m_config.max_clients) {
			(void)m_slots[m_output_owner_slot].heartbeat.probes.discard_probe(m_output_heartbeat_probe.session_id,
				m_output_heartbeat_probe.probe_id,
				m_output_heartbeat_probe.origin_t0_us);
		}
		m_output = {};
		m_has_output = false;
		m_output_owner_slot = InvalidIndex;
		m_output_heartbeat_pending = false;
		m_output_heartbeat_owns_probe = false;
		m_output_heartbeat_probe = {};
		return;
	}
	const auto owner = m_output_owner_slot;
	if (status == IoStatus::Complete && m_output_heartbeat_pending && owner < m_config.max_clients &&
		m_slots[owner].progress != ProducerSessionProgress::Empty) {
		++m_slots[owner].next_packet_sequence;
		++m_slots[owner].next_message_id;
	}
	if ((status == IoStatus::Closed || status == IoStatus::Error) && m_output_heartbeat_owns_probe &&
		owner < m_config.max_clients) {
		(void)m_slots[owner].heartbeat.probes.discard_probe(m_output_heartbeat_probe.session_id,
			m_output_heartbeat_probe.probe_id,
			m_output_heartbeat_probe.origin_t0_us);
	}
	if (status == IoStatus::Complete && m_output_reliability_pending &&
		m_pending_reliability_slot < m_config.max_clients &&
		m_slots[m_pending_reliability_slot].progress != ProducerSessionProgress::Empty) {
		const auto slot_index = m_pending_reliability_slot;
		if (!m_pending_preproof_send_accounted &&
			m_slots[slot_index].progress == ProducerSessionProgress::AwaitWelcomeApplied) {
			if (m_preproof.try_account_send(m_slots[slot_index].endpoint, m_output.size) ==
				PreproofLedgerResult::Allowed) {
				for (auto& cache : m_cache) {
					if (cache.used && cache.session_id == m_slots[slot_index].session_id &&
						!add_would_overflow(cache.accounted_sent, m_output.size)) {
						cache.accounted_sent += m_output.size;
						cache.preproof_active = true;
						break;
					}
				}
				const auto account = m_preproof.account(m_slots[slot_index].endpoint);
				m_slots[slot_index].preproof_validated_bytes_received = account.validated_bytes_received;
				m_slots[slot_index].preproof_bytes_sent = account.bytes_sent;
			}
		}
		protocol::ReliableWindowAction committed;
		if (m_reliable_windows[slot_index].pull_next_action(m_pending_reliability_time_us, committed) ==
				protocol::ReliablePullResult::Action &&
			committed.kind == protocol::ReliableWindowActionKind::Retransmit) {
			++m_slots[slot_index].next_packet_sequence;
		}
		m_slots[slot_index].reliable_items_in_use = m_reliable_windows[slot_index].entry_count();
	}
	m_output = SessionControllerOutput{};
	m_has_output = false;
	m_output_owner_slot = InvalidIndex;
	m_output_reliability_pending = false;
	m_pending_preproof_send_accounted = false;
	m_pending_reliability_slot = InvalidIndex;
	m_pending_reliability_time_us = 0U;
	m_output_heartbeat_pending = false;
	m_output_heartbeat_owns_probe = false;
	m_output_heartbeat_probe = {};
	if ((status == IoStatus::Closed || status == IoStatus::Error) && owner < m_config.max_clients) {
		(void)close_slot(owner, SessionCloseReason::TransportError);
	}
}

void SessionController::service_reliability(std::uint64_t now_us) noexcept
{
	if (!m_ready || m_faulted || m_config.max_clients == 0U) {
		return;
	}
	// A tail-queued Delta yields only to an actual due reliable action. Do not
	// release it merely to probe for work: doing so on every tick starves Delta
	// before the native scheduler can ever transmit it.
	if (!m_has_output || m_output_delta_egress_pending) {
		for (std::size_t index = 0U; index < m_config.max_clients; ++index) {
			auto& slot = m_slots[index];
			if (slot.progress != ProducerSessionProgress::ReadyForState || !slot.snapshot_egress.has_candidate()) {
				continue;
			}
			protocol::ReliableWindowAction action;
			if (slot.snapshot_egress.pull_reliability(now_us, action) != protocol::ReliablePullResult::Action) {
				continue;
			}
			if (action.kind == protocol::ReliableWindowActionKind::TerminalPolicy) {
				preempt_queued_delta();
				// A FULL_SNAPSHOT is a Transaction, whose Phase 0 terminal policy is
				// RequestResync.  Preserve an ACKed baseline when a replacement
				// transaction exhausts its reliable window, then reserve one fresh,
				// bounded resynchronization keyframe.  Only the initial snapshot has
				// no active baseline and remains terminal for this producer slot.
				if (action.terminal_policy == protocol::ReliableTerminalPolicy::RequestResync &&
					slot.snapshot.has_active_baseline()) {
					slot.snapshot_egress.rollback_candidate();
					if (slot.snapshot.abandon_replacement_candidate()) {
						(void)begin_replacement_snapshot(index, protocol::SnapshotFlagResync, now_us);
					}
					return;
				}
				(void)close_slot(index, SessionCloseReason::Timeout);
				return;
			}
			preempt_queued_delta();
			if (!slot.snapshot_egress.queue_retransmission(action, slot.next_packet_sequence, now_us)) {
				slot.snapshot_egress.rollback_candidate();
				slot.snapshot.rollback_candidate();
				return;
			}
			Phase1SnapshotDatagram datagram;
			if (!slot.snapshot_egress.peek_output(datagram) ||
				!queue_bytes(datagram.endpoint, datagram.bytes.data(), datagram.size, index)) {
				slot.snapshot_egress.rollback_candidate();
				slot.snapshot.rollback_candidate();
				(void)close_slot(index, SessionCloseReason::TransportError);
				return;
			}
			m_output_snapshot_egress_pending = true;
			++slot.next_packet_sequence;
			return;
		}
	}
	if (m_has_output && m_output_delta_egress_pending) {
		for (std::size_t offset = 0U; offset < m_config.max_clients; ++offset) {
			const auto index = (m_reliability_cursor + offset) % m_config.max_clients;
			const auto progress = m_slots[index].progress;
			if (progress == ProducerSessionProgress::Empty) {
				continue;
			}
			auto preview = m_reliable_windows[index];
			protocol::ReliableWindowAction action;
			if (preview.pull_next_action(now_us, action) != protocol::ReliablePullResult::Action ||
				(progress == ProducerSessionProgress::Stale &&
					action.kind != protocol::ReliableWindowActionKind::TerminalPolicy)) {
				continue;
			}
			preempt_queued_delta();
			break;
		}
	}
	if (m_has_output) {
		for (std::size_t offset = 0U; offset < m_config.max_clients; ++offset) {
			const auto index = (m_reliability_cursor + offset) % m_config.max_clients;
			const auto progress = m_slots[index].progress;
			if (progress == ProducerSessionProgress::Empty) {
				continue;
			}
			auto preview = m_reliable_windows[index];
			protocol::ReliableWindowAction action;
			if (preview.pull_next_action(now_us, action) != protocol::ReliablePullResult::Action ||
				action.kind != protocol::ReliableWindowActionKind::TerminalPolicy) {
				continue;
			}
			protocol::ReliableWindowAction committed;
			if (m_reliable_windows[index].pull_next_action(now_us, committed) !=
					protocol::ReliablePullResult::Action ||
				committed.kind != protocol::ReliableWindowActionKind::TerminalPolicy) {
				continue;
			}
			m_reliability_cursor = (index + 1U) % m_config.max_clients;
			if (m_output_owner_slot == index) {
				if (m_output_heartbeat_owns_probe) {
					(void)m_slots[index].heartbeat.probes.discard_probe(m_output_heartbeat_probe.session_id,
						m_output_heartbeat_probe.probe_id,
						m_output_heartbeat_probe.origin_t0_us);
				}
				m_output = {};
				m_has_output = false;
				m_output_owner_slot = InvalidIndex;
				m_output_reliability_pending = false;
				m_pending_preproof_send_accounted = false;
				m_pending_reliability_slot = InvalidIndex;
				m_pending_reliability_time_us = 0U;
				m_output_heartbeat_pending = false;
				m_output_heartbeat_owns_probe = false;
				m_output_heartbeat_probe = {};
			}
			apply_terminal_policy(index, committed.terminal_policy);
			return;
		}
		return;
	}
	for (std::size_t offset = 0U; offset < m_config.max_clients; ++offset) {
		const auto index = (m_reliability_cursor + offset) % m_config.max_clients;
		const auto progress = m_slots[index].progress;
		if (progress == ProducerSessionProgress::Empty) {
			continue;
		}
		auto preview = m_reliable_windows[index];
		protocol::ReliableWindowAction action;
		if (preview.pull_next_action(now_us, action) != protocol::ReliablePullResult::Action) {
			continue;
		}
		if (progress == ProducerSessionProgress::Stale &&
			action.kind != protocol::ReliableWindowActionKind::TerminalPolicy) {
			continue;
		}
		m_reliability_cursor = (index + 1U) % m_config.max_clients;
		if (action.kind == protocol::ReliableWindowActionKind::TerminalPolicy) {
			protocol::ReliableWindowAction committed;
			if (m_reliable_windows[index].pull_next_action(now_us, committed) ==
				protocol::ReliablePullResult::Action) {
				apply_terminal_policy(index, committed.terminal_policy);
			}
			return;
		}
		if (progress == ProducerSessionProgress::AwaitWelcomeApplied) {
			const auto account = m_preproof.account(m_slots[index].endpoint);
			const auto maximum = std::numeric_limits<std::uint64_t>::max();
			const auto limit = account.validated_bytes_received > maximum / 3U
				? maximum
				: account.validated_bytes_received * 3U;
			const auto retransmission_size = protocol::HeaderSizeV1 + action.retransmission.logical_payload.size;
			if (add_would_overflow(account.bytes_sent, retransmission_size) ||
				account.bytes_sent + retransmission_size > limit) {
				return;
			}
		}
		(void)queue_retransmission(index, action, now_us);
		return;
	}
}

bool SessionController::service_timeouts_impl(std::uint64_t now_us) noexcept
{
	if (!m_ready || m_faulted || m_config.max_clients == 0U) {
		return false;
	}
	for (std::size_t offset = 0U; offset < m_config.max_clients; ++offset) {
		const auto index = (m_heartbeat_cursor + offset) % m_config.max_clients;
		auto& slot = m_slots[index];
		if ((slot.progress != ProducerSessionProgress::ReadyForState &&
				slot.progress != ProducerSessionProgress::Stale) ||
			slot.heartbeat.negotiated_interval_ms == 0U) {
			continue;
		}
		const auto network_elapsed = now_us >= slot.heartbeat.last_valid_network_activity_us
			? now_us - slot.heartbeat.last_valid_network_activity_us
			: 0U;
		if (now_us >= slot.heartbeat.last_valid_network_activity_us &&
			network_elapsed >= slot.heartbeat.disconnect_timeout_us) {
			m_heartbeat_cursor = (index + 1U) % m_config.max_clients;
			(void)close_slot(index, SessionCloseReason::Timeout);
			return false;
		}
		const auto clock_elapsed = now_us >= slot.heartbeat.last_valid_clock_response_us
			? now_us - slot.heartbeat.last_valid_clock_response_us
			: 0U;
		if (now_us >= slot.heartbeat.last_valid_clock_response_us &&
			clock_elapsed >= slot.heartbeat.stale_timeout_us && !slot.heartbeat.clock_stale) {
			slot.progress = ProducerSessionProgress::Stale;
			slot.heartbeat.clock_filter.invalidate();
			slot.heartbeat.clock_stale = true;
			m_heartbeat_cursor = (index + 1U) % m_config.max_clients;
			if (m_has_output && m_output_owner_slot == index && m_output_heartbeat_pending) {
				if (m_output_heartbeat_owns_probe) {
					(void)slot.heartbeat.probes.discard_probe(m_output_heartbeat_probe.session_id,
						m_output_heartbeat_probe.probe_id,
						m_output_heartbeat_probe.origin_t0_us);
				}
				m_output = {};
				m_has_output = false;
				m_output_owner_slot = InvalidIndex;
				m_output_heartbeat_pending = false;
				m_output_heartbeat_owns_probe = false;
				m_output_heartbeat_probe = {};
				return false;
			}
			break;
		}
	}
	return true;
}

void SessionController::service_timeouts(std::uint64_t now_us) noexcept
{
	(void)service_timeouts_impl(now_us);
}

void SessionController::service_periodic(std::uint64_t now_us) noexcept
{
	if (!m_ready || m_faulted || m_config.max_clients == 0U) {
		return;
	}
	// Reserve due replacement transactions before considering output priority.
	// A heartbeat/control datagram may delay their first fragment, but it must
	// not defer the exact-due state capture or turn a missed period into a burst.
	for (std::size_t index = 0U; index < m_config.max_clients; ++index) {
		auto& slot = m_slots[index];
		if (slot.progress != ProducerSessionProgress::ReadyForState || !slot.snapshot.has_active_baseline()) {
			continue;
		}
		if (slot.resync.has_candidate() && !slot.snapshot.has_candidate() && !slot.snapshot_egress.has_candidate()) {
			if (begin_replacement_snapshot(index, protocol::SnapshotFlagResync, now_us)) {
				preempt_queued_delta();
				(void)slot.resync.complete();
				continue;
			}
		}
		if (now_us >= slot.next_keyframe_due_us) {
			const auto interval_us = static_cast<std::uint64_t>(m_config.keyframe_seconds) * 1'000'000U;
			slot.next_keyframe_due_us = add_would_overflow(now_us, interval_us)
				? std::numeric_limits<std::uint64_t>::max()
				: now_us + interval_us;
			slot.keyframe_due = true;
		}
		if (slot.keyframe_due && !slot.snapshot.has_candidate() && !slot.snapshot_egress.has_candidate() &&
			begin_replacement_snapshot(index, protocol::SnapshotFlagPeriodicKeyframe, now_us)) {
			preempt_queued_delta();
		}
	}
	if (m_has_output && !m_output_delta_egress_pending) {
		return;
	}
	for (std::size_t offset = 0U; offset < m_config.max_clients; ++offset) {
		const auto index = (m_heartbeat_cursor + offset) % m_config.max_clients;
		auto& slot = m_slots[index];
		if ((slot.progress != ProducerSessionProgress::ReadyForState &&
				slot.progress != ProducerSessionProgress::Stale) ||
			slot.heartbeat.negotiated_interval_ms == 0U || now_us < slot.heartbeat.next_periodic_due_us) {
			continue;
		}
		protocol::ProbeToken probe;
		if (slot.heartbeat.probes.begin_probe(now_us, probe) != protocol::ProbeStartResult::Started) {
			return;
		}
		protocol::HeartbeatPayload request;
		request.probe_id = probe.probe_id;
		request.kind = protocol::HeartbeatKind::Request;
		request.origin_t0_us = probe.origin_t0_us;
		if (!queue_heartbeat(index, request, now_us, true, probe)) {
			(void)slot.heartbeat.probes.discard_probe(probe.session_id, probe.probe_id, probe.origin_t0_us);
			return;
		}
		// Advance cadence only after the heartbeat is concretely queued. If
		// capacity/encoding/queue admission fails, the probe is discarded and
		// this due heartbeat (and any queued Delta) remains intact for retry.
		m_heartbeat_cursor = (index + 1U) % m_config.max_clients;
		const auto interval_us = static_cast<std::uint64_t>(slot.heartbeat.negotiated_interval_ms) * 1000U;
		slot.heartbeat.next_periodic_due_us = add_would_overflow(now_us, interval_us)
			? std::numeric_limits<std::uint64_t>::max()
			: now_us + interval_us;
		return;
	}
}

void SessionController::service_session_maintenance(std::uint64_t now_us) noexcept
{
	if (service_timeouts_impl(now_us)) {
		service_periodic(now_us);
	}
}

SessionPlayerMaterializationResult SessionController::apply_player_observation(const CaptureResult& capture,
	const PlayerObservationDto& observation) noexcept
{
	SessionPlayerMaterializationResult result;
	for (std::size_t index = 0U; index < m_config.max_clients; ++index) {
		auto& slot = m_slots[index];
		if (slot.progress != ProducerSessionProgress::ReadyForState &&
			slot.progress != ProducerSessionProgress::Stale) {
			continue;
		}

		++result.eligible_slots;
		auto candidate_registry = slot.player_entity_ids;
		PlayerKinematicsSample candidate_sample;
		const auto status = materialize_player_sample(candidate_registry, capture, observation, candidate_sample);
		if (status == PlayerSampleMaterializeStatus::EntityIdCounterExhausted) {
			++result.closed_exhausted_slots;
			(void)close_slot(index, SessionCloseReason::ProtocolError);
			continue;
		}

		slot.player_entity_ids = candidate_registry;
		slot.latest_player_sample = candidate_sample;
		slot.latest_player_sample_status = status;
		slot.has_latest_player_sample = status == PlayerSampleMaterializeStatus::MaterializedExisting ||
			status == PlayerSampleMaterializeStatus::MaterializedNew;
		switch (status) {
		case PlayerSampleMaterializeStatus::MaterializedExisting:
			++result.materialized_existing_slots;
			break;
		case PlayerSampleMaterializeStatus::MaterializedNew:
			++result.materialized_new_slots;
			break;
		case PlayerSampleMaterializeStatus::NoPlayer:
			++result.no_player_slots;
			break;
		case PlayerSampleMaterializeStatus::InvalidSource:
			++result.invalid_source_slots;
			break;
		case PlayerSampleMaterializeStatus::InvalidCapture:
		case PlayerSampleMaterializeStatus::Count:
		case PlayerSampleMaterializeStatus::EntityIdCounterExhausted:
			++result.invalid_capture_slots;
			break;
		}
	}
	return result;
}

bool SessionController::begin_initial_snapshot(std::size_t slot_index,
	const protocol::StateImage& image,
	std::uint64_t now_us) noexcept
{
	if (!m_ready || m_faulted || slot_index >= m_config.max_clients) {
		return false;
	}
	auto& slot = m_slots[slot_index];
	if (slot.progress != ProducerSessionProgress::ReadyForState || slot.snapshot.has_candidate() ||
		slot.snapshot.has_active_baseline() || slot.snapshot_egress.has_candidate() || slot.next_snapshot_id == 0U) {
		return false;
	}
	const auto snapshot_id = slot.next_snapshot_id;
	if (!slot.snapshot_egress.set_next_message_id(slot.next_message_id)) {
		return false;
	}
	if (slot.snapshot_egress.queue_initial_snapshot(
			slot.session_id, slot.endpoint, snapshot_id, now_us, image, now_us) != Phase1SnapshotEgressResult::Queued) {
		return false;
	}
	if (slot.snapshot.start_initial_candidate(snapshot_id, image, slot.snapshot_egress.candidate_parts(), now_us) !=
		protocol::ProducerBaselineResult::Applied) {
		slot.snapshot_egress.rollback_candidate();
		return false;
	}
	++slot.next_message_id;
	++slot.next_snapshot_id;
	return true;
}

bool SessionController::begin_replacement_snapshot(std::size_t slot_index,
	std::uint16_t snapshot_flags,
	std::uint64_t now_us) noexcept
{
	if (!m_ready || m_faulted || slot_index >= m_config.max_clients) {
		return false;
	}
	auto& slot = m_slots[slot_index];
	if (slot.progress != ProducerSessionProgress::ReadyForState || !slot.snapshot.has_active_baseline() ||
		slot.snapshot.has_candidate() || slot.snapshot_egress.has_candidate() || slot.next_snapshot_id == 0U ||
		slot.next_message_id == 0U) {
		return false;
	}
	const auto snapshot_id = slot.next_snapshot_id;
	const auto& current = slot.snapshot.current_state();
	if (!slot.snapshot_egress.set_next_message_id(slot.next_message_id) ||
		slot.snapshot_egress.queue_snapshot(slot.session_id,
			slot.endpoint,
			snapshot_id,
			now_us,
			current,
			snapshot_flags,
			now_us) != Phase1SnapshotEgressResult::Queued) {
		return false;
	}
	if (slot.snapshot.start_replacement_candidate(snapshot_id, current, slot.snapshot_egress.candidate_parts(), now_us) !=
		protocol::ProducerBaselineResult::Applied) {
		slot.snapshot_egress.rollback_candidate();
		return false;
	}
	// The retained replacement owns the newest state now; an unsent delta based
	// on the prior baseline is obsolete and must not overtake this keyframe.
	slot.delta_egress.discard();
	++slot.next_message_id;
	++slot.next_snapshot_id;
	slot.keyframe_due = false;
	return true;
}

bool SessionController::queue_cumulative_delta(std::size_t slot_index, std::uint64_t now_us) noexcept
{
	if (!m_ready || m_faulted || slot_index >= m_config.max_clients) {
		return false;
	}
	auto& slot = m_slots[slot_index];
	if (slot.snapshot.keyframe_intent() != Phase1KeyframeIntent::None) {
		if (begin_replacement_snapshot(slot_index, protocol::SnapshotFlagPeriodicKeyframe, now_us)) {
			(void)slot.snapshot.consume_keyframe_intent();
			return true;
		}
		return false;
	}
	if (slot.progress != ProducerSessionProgress::ReadyForState || !slot.snapshot.has_active_baseline() ||
		slot.next_message_id == 0U || !slot.delta_egress.can_replace()) {
		return false;
	}
	if (!slot.snapshot.current_record_set_compatible_with_active_baseline()) {
		if (slot.snapshot.keyframe_intent() != Phase1KeyframeIntent::None &&
			begin_replacement_snapshot(slot_index, protocol::SnapshotFlagPeriodicKeyframe, now_us)) {
			(void)slot.snapshot.consume_keyframe_intent();
			return true;
		}
		return false;
	}
	auto& delta = slot.delta_scratch;
	if (slot.snapshot.emit_cumulative_delta(now_us, delta) != protocol::ProducerBaselineResult::Applied ||
		!slot.delta_egress.replace(slot.session_id, slot.endpoint, slot.next_message_id, delta)) {
		return false;
	}
	++slot.next_message_id;
	(void)slot.snapshot.consume_session_state_dirty();
	return true;
}

protocol::ProducerBaselineResult SessionController::replace_current_state(std::size_t slot_index,
	const protocol::StateImage& image) noexcept
{
	if (!m_ready || m_faulted || slot_index >= m_config.max_clients ||
		m_slots[slot_index].progress != ProducerSessionProgress::ReadyForState) {
		return protocol::ProducerBaselineResult::InvalidArgument;
	}
	return m_slots[slot_index].snapshot.replace_current(image);
}

std::size_t SessionController::service_initial_snapshot_egress(std::size_t datagram_budget,
	std::uint64_t now_us) noexcept
{
	static_cast<void>(now_us);
	if (!m_ready || m_faulted || m_has_output || datagram_budget == 0U) {
		return 0U;
	}
	for (std::size_t index = 0U; index < m_config.max_clients; ++index) {
		auto& slot = m_slots[index];
		if (slot.progress != ProducerSessionProgress::ReadyForState || !slot.snapshot_egress.has_candidate()) {
			continue;
		}
		if (slot.snapshot_egress.service(1U, slot.next_packet_sequence, now_us) == 0U) {
			continue;
		}
		Phase1SnapshotDatagram datagram;
		if (!slot.snapshot_egress.peek_output(datagram) ||
			!queue_bytes(datagram.endpoint, datagram.bytes.data(), datagram.size, index)) {
			slot.snapshot_egress.rollback_candidate();
			slot.snapshot.rollback_candidate();
			return 0U;
		}
		m_output_snapshot_egress_pending = true;
		++slot.next_packet_sequence;
		return 1U;
	}
	return 0U;
}

std::size_t SessionController::service_delta_egress(std::size_t datagram_budget, std::uint64_t now_us) noexcept
{
	if (!m_ready || m_faulted || m_has_output || datagram_budget == 0U) {
		return 0U;
	}
	for (std::size_t index = 0U; index < m_config.max_clients; ++index) {
		auto& slot = m_slots[index];
		if (slot.progress != ProducerSessionProgress::ReadyForState || !slot.snapshot.has_active_baseline() ||
			!slot.delta_egress.has_delta() || !slot.delta_egress.service(slot.next_packet_sequence, now_us)) {
			continue;
		}
		Phase1DeltaDatagram datagram;
		if (!slot.delta_egress.peek_output(datagram) ||
			!queue_bytes(datagram.endpoint, datagram.bytes.data(), datagram.size, index)) {
			slot.delta_egress.discard();
			return 0U;
		}
		m_output_delta_egress_pending = true;
		return 1U;
	}
	return 0U;
}

Phase1SnapshotProgress SessionController::snapshot_progress(std::size_t slot_index) const noexcept
{
	return !m_ready || slot_index >= m_config.max_clients ? Phase1SnapshotProgress::Synchronizing
																 : m_slots[slot_index].snapshot.progress();
}

bool SessionController::session_state_dirty(std::size_t slot_index) const noexcept
{
	return m_ready && slot_index < m_config.max_clients && m_slots[slot_index].snapshot.session_state_dirty();
}

bool SessionController::consume_session_state_dirty(std::size_t slot_index) noexcept
{
	return m_ready && slot_index < m_config.max_clients && m_slots[slot_index].snapshot.consume_session_state_dirty();
}

void SessionController::clear_player_observations() noexcept
{
	for (std::size_t index = 0U; index < m_config.max_clients; ++index) {
		auto& slot = m_slots[index];
		if (slot.progress == ProducerSessionProgress::Empty) {
			continue;
		}
		(void)slot.player_entity_ids.invalidate();
		slot.latest_player_sample = {};
		slot.latest_player_sample_status = PlayerSampleMaterializeStatus::InvalidCapture;
		slot.has_latest_player_sample = false;
	}
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
	if (m_has_output && m_output_owner_slot == index) {
		m_output = {};
		m_has_output = false;
		m_output_owner_slot = InvalidIndex;
		m_output_reliability_pending = false;
		m_output_snapshot_egress_pending = false;
		m_output_delta_egress_pending = false;
		m_output_resync_ack_pending = false;
		m_pending_preproof_send_accounted = false;
		m_pending_reliability_slot = InvalidIndex;
		m_pending_reliability_time_us = 0U;
		m_output_heartbeat_pending = false;
		m_output_heartbeat_owns_probe = false;
		m_output_heartbeat_probe = {};
	}
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
	for (std::size_t index = 0U; index < m_config.max_clients; ++index) {
		if (m_slots[index].progress != ProducerSessionProgress::Empty) {
			(void)m_slots[index].resync.expire(now_us);
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
	m_output_owner_slot = InvalidIndex;
	m_output_reliability_pending = false;
	m_output_snapshot_egress_pending = false;
	m_output_delta_egress_pending = false;
	m_output_resync_ack_pending = false;
	m_pending_preproof_send_accounted = false;
	m_pending_reliability_slot = InvalidIndex;
	m_pending_reliability_time_us = 0U;
	m_reliability_cursor = 0U;
	m_heartbeat_cursor = 0U;
	m_output_heartbeat_pending = false;
	m_output_heartbeat_owns_probe = false;
	m_output_heartbeat_probe = {};
}

void SessionController::purge_all(SessionCloseReason reason) noexcept
{
	if (!m_ready || m_rate_limiter == nullptr) {
		return;
	}
	for (std::size_t index = 0U; index < m_config.max_clients; ++index) {
		(void)close_slot(index, reason);
	}
	clear_all();
}

SessionControllerOwnedCapacity SessionController::owned_capacity() const noexcept
{
	SessionControllerOwnedCapacity result{m_config.max_clients,
		m_config.max_clients * protocol::MaxStateReassembliesPerClient,
		m_config.max_clients * protocol::MaxStateReassemblyBytesPerClient,
		m_config.max_clients * Wp06ReliableRetentionBytesPerClient,
		m_config.max_clients * 2U,
		m_config.max_clients * 2U,
		0U,
		m_config.max_clients * sizeof(SessionControllerSlot),
		sizeof(protocol::ProtocolRateLimiter),
		sizeof(m_cache),
		sizeof(m_preproof),
		sizeof(m_output)};
	if (m_slots == nullptr) {
		return {};
	}
	for (std::size_t index = 0U; index < m_config.max_clients; ++index) {
		const auto& slot = m_slots[index];
		const auto snapshot_bytes = slot.snapshot_egress.owned_heap_bytes();
		const auto delta_egress_bytes = slot.delta_egress.owned_heap_bytes();
		const auto& scratch = slot.delta_scratch;
		std::size_t scratch_bytes = 0U;
		if (scratch.mutations.capacity() > std::numeric_limits<std::size_t>::max() / sizeof(protocol::StateMutation) ||
			!checked_add_size(scratch_bytes, scratch.mutations.capacity() * sizeof(protocol::StateMutation), scratch_bytes)) {
			return {};
		}
		for (const auto& mutation : scratch.mutations) {
			if (!checked_add_size(scratch_bytes, mutation.atom.key.identity.capacity(), scratch_bytes) ||
				!checked_add_size(scratch_bytes, mutation.atom.value.capacity(), scratch_bytes) ||
				!checked_add_size(scratch_bytes, mutation.atom.cascade_owner.identity.capacity(), scratch_bytes)) {
				return {};
			}
		}
		if (!checked_add_size(result.snapshot_egress_heap_bytes, snapshot_bytes, result.snapshot_egress_heap_bytes) ||
			!checked_add_size(result.delta_egress_heap_bytes, delta_egress_bytes, result.delta_egress_heap_bytes) ||
			!checked_add_size(result.delta_scratch_heap_bytes, scratch_bytes, result.delta_scratch_heap_bytes)) return {};
	}
	return result;
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
