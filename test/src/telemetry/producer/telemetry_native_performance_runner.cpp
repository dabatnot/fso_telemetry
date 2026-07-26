#include "telemetry/config.h"
#include "telemetry/engine_adapter.h"
#include "telemetry/native_session_runtime.h"
#include "telemetry/native_session_runtime_test_seam.h"
#include "telemetry/identity.h"
#include "telemetry/phase1_state_image.h"
#include "telemetry/protocol/telemetry_control_messages.h"
#include "telemetry/protocol/telemetry_crc32.h"
#include "telemetry/protocol/telemetry_datagram.h"
#include "telemetry/transport.h"

#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

namespace {
namespace detail = telemetry::detail;
namespace protocol = telemetry::protocol;

struct FixedRandom final : detail::RandomSource {
	std::uint64_t value = 1U;
	bool next_u64(std::uint64_t& output) noexcept override { output = value++; return true; }
};

struct Backend final : detail::UdpSocketBackend {
	struct Packet {
		std::vector<std::uint8_t> bytes;
		protocol::EndpointKey endpoint;
	};
	struct Receive {
		detail::IoStatus status = detail::IoStatus::WouldBlock;
		Packet packet{};
	};
	std::vector<Receive> receives;
	std::vector<Packet> sent;
	std::size_t receive_index = 0U;
	detail::IoStatus send_status = detail::IoStatus::Complete;
	bool auto_ack_full_snapshots = true;
	std::uint32_t next_auto_ack_sequence = 20'000U;
	enum class ImpairmentStage : std::uint8_t { Disabled = 0, WouldBlockDelta, DropNextDelta, Recovered };
	ImpairmentStage impairment_stage = ImpairmentStage::Disabled;
	std::size_t would_block_remaining = 0U;
	std::uint32_t next_resync_sequence = 30'000U;
	std::size_t would_block_sends = 0U;
	std::size_t lost_delta_sends = 0U;
	std::size_t resync_requests_injected = 0U;
	std::size_t resync_validated_acks_seen = 0U;
	std::size_t full_snapshot_acks_injected = 0U;
	bool diagnostic = false;
	void queue_loopback_snapshot_ack(const Packet& packet) noexcept;
	bool queue_loopback_resync_request(const Packet& packet) noexcept;
	detail::SocketOpenResult open_socket(const detail::SocketOpenRequest& request) noexcept override
	{
		return {detail::SocketOpenStatus::Complete, 1U,
			protocol::EndpointKey::from_ipv4({127U, 0U, 0U, 1U}, request.port)};
	}
	detail::SocketReceiveResult try_receive(detail::SocketHandle, protocol::MutableByteView output) noexcept override
	{
		if (receive_index >= receives.size()) return {detail::IoStatus::WouldBlock, {}, 0U, false};
		const auto& item = receives[receive_index++];
		if (item.status == detail::IoStatus::Complete && item.packet.bytes.size() <= output.size) {
			std::copy(item.packet.bytes.begin(), item.packet.bytes.end(), output.data);
		}
		return {item.status, item.packet.endpoint, item.packet.bytes.size(), false};
	}
	detail::SocketSendResult try_send(detail::SocketHandle, const protocol::EndpointKey& endpoint, protocol::ByteView bytes) noexcept override
	{
		Packet packet{std::vector<std::uint8_t>(bytes.data, bytes.data + bytes.size), endpoint};
		protocol::DatagramView decoded{};
		const auto decoded_ok = protocol::decode_and_validate_datagram({packet.bytes.data(), packet.bytes.size()},
			{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1}, decoded) == protocol::ValidationError::None;
		const auto is_delta = decoded_ok && decoded.header.message_type == protocol::MessageType::Delta;
		if (impairment_stage == ImpairmentStage::WouldBlockDelta && is_delta && would_block_remaining != 0U) {
			++would_block_sends;
			--would_block_remaining;
			if (diagnostic) std::cerr << "impairment-transition would-block delta remaining=" << would_block_remaining << '\n';
			if (would_block_remaining == 0U) impairment_stage = ImpairmentStage::DropNextDelta;
			return {detail::IoStatus::WouldBlock, 0U};
		}
		if (send_status == detail::IoStatus::WouldBlock) {
			++would_block_sends;
			return {send_status, 0U};
		}
		if (send_status == detail::IoStatus::Complete) {
			if (decoded_ok && decoded.header.message_type == protocol::MessageType::Ack) {
				protocol::AckPayload ack{};
				if (protocol::decode_ack_payload(decoded.payload, ack) == protocol::ValidationError::None &&
					ack.target_message_type == protocol::MessageType::ResyncRequest) {
					++resync_validated_acks_seen;
					if (diagnostic) std::cerr << "impairment-resync validated-ack sent session=" << decoded.header.session_id << '\n';
				}
			}
			if (impairment_stage == ImpairmentStage::DropNextDelta && is_delta && queue_loopback_resync_request(packet)) {
				impairment_stage = ImpairmentStage::Recovered;
				++lost_delta_sends;
				if (diagnostic) std::cerr << "impairment-transition delta-lost-resync-queued\n";
			} else {
				sent.push_back(std::move(packet));
				if (auto_ack_full_snapshots) queue_loopback_snapshot_ack(sent.back());
			}
		}
		return {send_status, send_status == detail::IoStatus::Complete ? bytes.size : 0U};
	}
	void close_socket(detail::SocketHandle) noexcept override {}
};

struct Completion final : detail::NativeOutputCompletionPort {
	void complete(detail::SessionController& controller, detail::IoStatus status) noexcept override
	{
		forwarder.complete(controller, status);
	}
	detail::NativeOutputCompletionForwarder forwarder;
};

struct EngineView final : detail::EngineReadView {
	float position_x = 1.0F;
	bool in_mission() const noexcept override { return true; }
	bool player_exists() const noexcept override { return true; }
	bool player_object_exists() const noexcept override { return true; }
	bool player_ship_exists() const noexcept override { return true; }
	bool player_object_is_ship() const noexcept override { return true; }
	bool player_object_ship_instance_in_range() const noexcept override { return true; }
	bool player_object_matches_player() const noexcept override { return true; }
	bool player_ship_matches_object() const noexcept override { return true; }
	void read_player_kinematics(detail::EnginePlayerKinematicsRead& output) const noexcept override
	{
		output.object_signature = 42U;
		output.position_world = {position_x, 2.0F, 3.0F};
		output.orientation = {};
		output.velocity_world = {4.0F, 5.0F, 6.0F};
		output.rotational_velocity_local = {0.1F, 0.2F, 0.3F};
		output.radius = 7.0F;
	}
};

struct SnapshotAckIdentity {
	std::uint64_t session_id = 0U;
	std::uint32_t message_id = 0U;
	std::uint32_t message_crc32 = 0U;
};

enum class PerformanceWorkload : std::uint8_t { ActiveOneClient = 0, ActiveFourClients, WouldBlockLossResync };

struct SnapshotAckState {
	SnapshotAckIdentity identity{};
	std::uint16_t fragment_count = 0U;
	std::vector<bool> fragments_seen;
	bool ack_queued = false;
};

struct ClientResponseTracker {
	std::vector<SnapshotAckState> snapshot_ack_states;
	std::vector<Backend::Packet> pending_snapshot_acks;
	std::size_t sent_index = 0U;
	std::uint32_t next_packet_sequence = 10'000U;
	bool trace_bootstrap = false;
};

bool same_snapshot_ack_identity(const SnapshotAckIdentity& left, const SnapshotAckIdentity& right) noexcept
{
	return left.session_id == right.session_id && left.message_id == right.message_id &&
		left.message_crc32 == right.message_crc32;
}

std::size_t active_baseline_count(const detail::NativeSessionRuntime& runtime) noexcept
{
	std::size_t count = 0U;
	for (std::size_t index = 0U; index < 4U; ++index) {
		const auto* slot = detail::NativeSessionRuntimeTestAccess::slot(runtime, index);
		if (slot != nullptr && slot->snapshot.has_active_baseline()) ++count;
	}
	return count;
}

bool force_authoritative_delta(detail::NativeSessionRuntime& runtime, std::uint64_t& now_us, bool diagnostic) noexcept
{
	auto* controller = detail::NativeSessionRuntimeTestAccess::controller(runtime);
	if (controller == nullptr) return false;
	auto trace_drain_state = [&]() noexcept {
		detail::SessionControllerOutput output{};
		protocol::DatagramView datagram{};
		const auto has_output = controller->peek_output(output);
		const auto decoded = has_output && protocol::decode_and_validate_datagram({output.bytes.data(), output.size},
			{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1}, datagram) == protocol::ValidationError::None;
		const auto& observed = controller->slot(0U);
		if (!diagnostic) return;
		std::cerr << " impairment-drain delta=" << (observed.delta_egress.has_delta() ? 1U : 0U)
				  << " delta-output=" << (observed.delta_egress.has_output() ? 1U : 0U)
				  << " replaceable=" << (observed.delta_egress.can_replace() ? 1U : 0U)
				  << " controller-output=" << (has_output ? 1U : 0U)
				  << " output-type=" << (decoded ? static_cast<unsigned>(datagram.header.message_type) : 255U)
				  << " snapshot-candidate=" << (observed.snapshot.has_candidate() ? 1U : 0U) << '\n';
	};
	// Match the controller contract fixture's service_delta_egress/pop_output
	// sequence through the real R2 scheduler. A delta may already be started
	// without occupying the controller output slot, so output_queued alone is
	// not a sufficient drain condition.
	for (std::uint32_t attempt = 0U; attempt < 8U; ++attempt) {
		const auto& before = controller->slot(0U);
		if (before.delta_egress.can_replace() && !runtime.owned_usage().output_queued) break;
		if (diagnostic) std::cerr << "impairment-drain attempt=" << attempt;
		trace_drain_state();
		const auto status = detail::NativeSessionRuntimeTestAccess::service_r2_tick(runtime, {now_us, 1U, true});
		if (status != detail::NativeSessionTickStatus::Complete) {
			std::cerr << "impairment-authoritative-delta drain-status=" << static_cast<unsigned>(status) << '\n';
			return false;
		}
		++now_us;
	}
	if (diagnostic) std::cerr << "impairment-drain final";
	trace_drain_state();
	auto& slot = controller->slot(0U);
	if (diagnostic) {
		std::cerr << "impairment-authoritative-delta precondition progress=" << static_cast<unsigned>(slot.progress)
				  << " baseline=" << (slot.snapshot.has_active_baseline() ? 1U : 0U)
				  << " candidate=" << (slot.snapshot.has_candidate() ? 1U : 0U)
				  << " egress-candidate=" << (slot.snapshot_egress.has_candidate() ? 1U : 0U)
				  << " delta-replaceable=" << (slot.delta_egress.can_replace() ? 1U : 0U)
				  << " keyframe-intent=" << static_cast<unsigned>(slot.snapshot.keyframe_intent())
				  << " output=" << (runtime.owned_usage().output_queued ? 1U : 0U) << '\n';
	}
	if (slot.progress != detail::ProducerSessionProgress::ReadyForState || !slot.snapshot.has_active_baseline() ||
		!slot.has_latest_player_sample) {
		std::cerr << "impairment-authoritative-delta invalid-precondition progress="
				  << static_cast<unsigned>(slot.progress) << " baseline=" << (slot.snapshot.has_active_baseline() ? 1U : 0U)
				  << " player=" << (slot.has_latest_player_sample ? 1U : 0U) << '\n';
		return false;
	}
	detail::Phase1StateImageInput input{};
	input.producer_id = 0x1020304050607080ULL;
	input.negotiated_capability_generation = 1U;
	input.session_phase = protocol::SessionPhase::Live;
	input.mission.producer_sample_time_us = now_us;
	input.mission.mission_generation = 1U;
	input.mission.phase = protocol::MissionPhase::Active;
	input.mission.time_compression = 1.0F;
	input.player_capture = {detail::CaptureStatus::Valid, detail::CaptureReason::None};
	input.player = slot.latest_player_sample;
	const auto captured_sample_time_us = input.player.value.producer_sample_time_us;
	// The StateImage contract requires every valid player observation to carry
	// the same producer timestamp as its enclosing mission observation. This
	// is the same relation set explicitly by the delta-egress contract fixture.
	input.player.value.producer_sample_time_us = now_us;
	input.player.value.position_world.x += 7.0F;
	protocol::StateImage image{};
	const auto build = detail::build_phase1_state_image(input, image);
	if (build != detail::Phase1StateImageBuildStatus::Created) {
		std::cerr << "impairment-authoritative-delta build=" << static_cast<unsigned>(build)
				  << " captured-time=" << captured_sample_time_us << " mutation-time=" << now_us
				  << " entity=" << input.player.entity_id << '\n';
		return false;
	}
	const auto replaced = controller->replace_current_state(0U, image);
	if (replaced != protocol::ProducerBaselineResult::Applied) {
		std::cerr << "impairment-authoritative-delta replace=" << static_cast<unsigned>(replaced) << '\n';
		return false;
	}
	const auto queued = controller->queue_cumulative_delta(0U, now_us);
	if (diagnostic || !queued) {
		std::cerr << "impairment-authoritative-delta queued=" << (queued ? 1U : 0U)
				  << " entity=" << input.player.entity_id
				  << " keyframe-intent=" << static_cast<unsigned>(controller->slot(0U).snapshot.keyframe_intent())
				  << " delta-present=" << (controller->slot(0U).delta_egress.has_delta() ? 1U : 0U)
				  << " delta-replaceable=" << (controller->slot(0U).delta_egress.can_replace() ? 1U : 0U) << '\n';
	}
	return queued;
}

bool parse_u64(const char* text, std::uint64_t& output) noexcept
{
	char* end = nullptr;
	output = std::strtoull(text, &end, 10);
	return end != text && *end == '\0';
}

bool tick(detail::NativeSessionRuntime& runtime, const EngineView& view, std::uint64_t now_us) noexcept
{
	const auto status = runtime.service_tick({now_us, 1U, true}, view);
	if (status != detail::NativeSessionTickStatus::Complete) {
		std::cerr << "native tick failure status=" << static_cast<unsigned>(status) << " now-us=" << now_us << '\n';
	}
	return status == detail::NativeSessionTickStatus::Complete;
}

bool tick_handshake(detail::NativeSessionRuntime& runtime, std::uint64_t now_us) noexcept
{
	return detail::NativeSessionRuntimeTestAccess::service_r2_tick(runtime, {now_us, 1U, true}) ==
		detail::NativeSessionTickStatus::Complete;
}

bool make_hello(std::uint64_t nonce, protocol::EndpointKey endpoint, Backend::Packet& result) noexcept;
bool make_ack(const Backend::Packet& target,
	protocol::EndpointKey endpoint,
	std::uint32_t sequence,
	Backend::Packet& result) noexcept;
bool make_resync_request(std::uint64_t session_id,
	protocol::EndpointKey endpoint,
	std::uint32_t sequence,
	std::uint32_t request_id,
	std::uint64_t now_us,
	Backend::Packet& result) noexcept;
bool make_heartbeat_response(const Backend::Packet& target,
	protocol::EndpointKey endpoint,
	std::uint32_t sequence,
	std::uint64_t now_us,
	Backend::Packet& result) noexcept;

bool queue_new_client_responses(Backend& backend, ClientResponseTracker& tracker, std::uint64_t now_us) noexcept
{
	while (tracker.sent_index < backend.sent.size()) {
		const auto& packet = backend.sent[tracker.sent_index++];
		protocol::DatagramView datagram{};
		if (protocol::decode_and_validate_datagram({packet.bytes.data(), packet.bytes.size()},
			{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1}, datagram) != protocol::ValidationError::None) return false;
		if (tracker.trace_bootstrap) {
			std::cerr << "bootstrap-tx index=" << (tracker.sent_index - 1U)
					  << " type=" << static_cast<unsigned>(datagram.header.message_type)
					  << " session=" << datagram.header.session_id
					  << " message=" << datagram.header.message_id
					  << " fragment=" << datagram.header.fragment_index << '/' << datagram.header.fragment_count
					  << " crc=" << datagram.header.message_crc32 << '\n';
		}
		if (datagram.header.message_type == protocol::MessageType::Heartbeat) {
			Backend::Packet response{};
			if (!make_heartbeat_response(packet, packet.endpoint, tracker.next_packet_sequence++, now_us, response)) return false;
			backend.receives.push_back({detail::IoStatus::Complete, std::move(response)});
			continue;
		}
		if (datagram.header.message_type != protocol::MessageType::FullSnapshot) continue;
		const SnapshotAckIdentity identity{datagram.header.session_id, datagram.header.message_id,
			datagram.header.message_crc32};
		auto state = std::find_if(tracker.snapshot_ack_states.begin(), tracker.snapshot_ack_states.end(),
			[&](const SnapshotAckState& existing) { return same_snapshot_ack_identity(existing.identity, identity); });
		if (state == tracker.snapshot_ack_states.end()) {
			if (datagram.header.fragment_count == 0U || datagram.header.fragment_index >= datagram.header.fragment_count) return false;
			SnapshotAckState created;
			created.identity = identity;
			created.fragment_count = datagram.header.fragment_count;
			created.fragments_seen.assign(datagram.header.fragment_count, false);
			tracker.snapshot_ack_states.push_back(std::move(created));
			state = tracker.snapshot_ack_states.end() - 1;
		}
		if (state->fragment_count != datagram.header.fragment_count || datagram.header.fragment_index >= state->fragments_seen.size()) return false;
		state->fragments_seen[datagram.header.fragment_index] = true;
		if (state->ack_queued || std::find(state->fragments_seen.begin(), state->fragments_seen.end(), false) != state->fragments_seen.end()) continue;
		if (backend.auto_ack_full_snapshots) {
			state->ack_queued = true;
			continue;
		}
		Backend::Packet ack{};
		if (!make_ack(packet, packet.endpoint, tracker.next_packet_sequence++, ack)) return false;
		if (tracker.trace_bootstrap) {
			std::cerr << "bootstrap-ack-deferred session=" << identity.session_id
					  << " port=" << packet.endpoint.port() << " target-message=" << identity.message_id
					  << " crc=" << identity.message_crc32 << " packet-sequence=" << (tracker.next_packet_sequence - 1U) << '\n';
		}
		tracker.pending_snapshot_acks.push_back(std::move(ack));
		state->ack_queued = true;
	}
	return true;
}

int benchmark_stage_failure(const char* stage,
	std::uint64_t index,
	std::uint64_t now_us,
	const detail::NativeSessionRuntime& runtime,
	const ClientResponseTracker& responses) noexcept
{
	const auto usage = runtime.owned_usage();
	std::cerr << "benchmark failure stage=" << stage << " index=" << index << " now-us=" << now_us
			  << " sessions=" << runtime.active_sessions() << " baselines=" << active_baseline_count(runtime)
			  << " queued=" << (usage.output_queued ? 1U : 0U) << " reliable=" << usage.reliable_items
			  << " pending-acks=" << responses.pending_snapshot_acks.size() << '\n';
	return 4;
}

void trace_periodic_snapshot_state(const detail::NativeSessionRuntime& runtime,
	const ClientResponseTracker& responses,
	std::uint64_t index,
	std::uint64_t now_us) noexcept
{
	const auto usage = runtime.owned_usage();
	std::cerr << "periodic-trace index=" << index << " now-us=" << now_us
			  << " sessions=" << runtime.active_sessions() << " queued=" << (usage.output_queued ? 1U : 0U)
			  << " reliable=" << usage.reliable_items << " pending-acks=" << responses.pending_snapshot_acks.size() << '\n';
	for (std::size_t slot_index = 0U; slot_index < 4U; ++slot_index) {
		const auto* slot = detail::NativeSessionRuntimeTestAccess::slot(runtime, slot_index);
		if (slot == nullptr) continue;
		std::cerr << "periodic-trace slot=" << slot_index << " progress=" << static_cast<unsigned>(slot->progress)
				  << " session=" << slot->session_id << " baseline=" << (slot->snapshot.has_active_baseline() ? 1U : 0U)
				  << " candidate=" << (slot->snapshot.has_candidate() ? 1U : 0U)
				  << " egress-candidate=" << (slot->snapshot_egress.has_candidate() ? 1U : 0U)
				  << " reliable-items=" << slot->reliable_items_in_use
				  << " keyframe-due=" << (slot->keyframe_due ? 1U : 0U)
				  << " next-keyframe=" << slot->next_keyframe_due_us << '\n';
	}
}

void release_snapshot_acks_when_ingress_is_safe(Backend& backend,
	const detail::NativeSessionRuntime& runtime,
	ClientResponseTracker& tracker) noexcept
{
	// SessionController admits non-heartbeat ingress only when its single
	// output slot is empty. Deferring ACK APPLIED preserves that scheduler
	// invariant instead of sending a packet which must be rejected as OutputBusy.
	if (runtime.owned_usage().output_queued) return;
	if (tracker.trace_bootstrap && !tracker.pending_snapshot_acks.empty()) {
		std::cerr << "bootstrap-ack-release count=" << tracker.pending_snapshot_acks.size() << '\n';
	}
	for (auto& ack : tracker.pending_snapshot_acks) {
		backend.receives.push_back({detail::IoStatus::Complete, std::move(ack)});
	}
	tracker.pending_snapshot_acks.clear();
}

bool drain_for_pending_snapshot_acks(Backend& backend,
	detail::NativeSessionRuntime& runtime,
	ClientResponseTracker& tracker,
	std::uint64_t& now_us) noexcept
{
	// A regular service_tick immediately repopulates the idle-tail delta slot.
	// Use bounded real R2 ticks between measured ticks to transmit that output
	// without creating another delta, then deliver the pending ACK APPLIED.
	for (std::uint32_t attempt = 0U; attempt < 16U && !tracker.pending_snapshot_acks.empty(); ++attempt) {
		if (runtime.owned_usage().output_queued) {
			const auto status = detail::NativeSessionRuntimeTestAccess::service_r2_tick(runtime, {now_us, 1U, true});
			if (status != detail::NativeSessionTickStatus::Complete) {
				const auto usage = runtime.owned_usage();
				std::cerr << "snapshot-ack drain failure status=" << static_cast<unsigned>(status)
						  << " now-us=" << now_us << " attempt=" << attempt
						  << " queued=" << (usage.output_queued ? 1U : 0U)
						  << " reliable=" << usage.reliable_items
						  << " pending-acks=" << tracker.pending_snapshot_acks.size()
						  << " sessions=" << runtime.active_sessions() << '\n';
				return false;
			}
			now_us += 1U;
			if (!queue_new_client_responses(backend, tracker, now_us)) return false;
		}
		release_snapshot_acks_when_ingress_is_safe(backend, runtime, tracker);
	}
	return tracker.pending_snapshot_acks.empty();
}

bool establish_ready(Backend& backend,
	detail::NativeSessionRuntime& runtime,
	const EngineView& view,
	protocol::EndpointKey endpoint,
	std::uint64_t nonce,
	std::uint64_t& now_us,
	std::uint32_t sequence,
	ClientResponseTracker& responses) noexcept
{
	const auto session_count = runtime.active_sessions();
	Backend::Packet hello{};
	if (!make_hello(nonce, endpoint, hello)) { std::cerr << "handshake: hello encoding failed\n"; return false; }
	backend.receives.push_back({detail::IoStatus::Complete, std::move(hello)});
	const auto sent_before = backend.sent.size();
	for (std::uint32_t attempt = 0U; attempt < 8U && backend.sent.size() == sent_before; ++attempt) {
		if (!tick_handshake(runtime, now_us)) { std::cerr << "handshake: hello tick failed\n"; return false; }
		if (!queue_new_client_responses(backend, responses, now_us)) return false;
		release_snapshot_acks_when_ingress_is_safe(backend, runtime, responses);
		now_us += 1U;
	}
	if (backend.sent.size() == sent_before) { std::cerr << "handshake: no welcome send\n"; return false; }
	Backend::Packet welcome = backend.sent.back();
	Backend::Packet ack{};
	if (!make_ack(welcome, endpoint, sequence, ack)) { std::cerr << "handshake: ack encoding failed\n"; return false; }
	backend.receives.push_back({detail::IoStatus::Complete, std::move(ack)});
	for (std::uint32_t attempt = 0U; attempt < 8U && runtime.active_sessions() == session_count; ++attempt) {
		if (!tick_handshake(runtime, now_us)) { std::cerr << "handshake: ack tick failed\n"; return false; }
		if (!queue_new_client_responses(backend, responses, now_us)) return false;
		release_snapshot_acks_when_ingress_is_safe(backend, runtime, responses);
		now_us += 1U;
	}
	if (runtime.active_sessions() != session_count + 1U) {
		std::cerr << "handshake: active sessions " << runtime.active_sessions() << " expected " << (session_count + 1U) << '\n';
		return false;
	}
	// ReadyForState is allocated when WELCOME is applied, but the protocol does
	// not permit the initial FullSnapshot transaction until SESSION_BEGIN itself
	// is ACK APPLIED. Mirror the native integration fixture rather than treating
	// the active-slot count as a completed handshake.
	const auto begin_sent_before = backend.sent.size();
	for (std::uint32_t attempt = 0U; attempt < 8U && backend.sent.size() == begin_sent_before; ++attempt) {
		if (!tick_handshake(runtime, now_us)) { std::cerr << "handshake: session-begin tick failed\n"; return false; }
		if (!queue_new_client_responses(backend, responses, now_us)) return false;
		release_snapshot_acks_when_ingress_is_safe(backend, runtime, responses);
		now_us += 1U;
	}
	if (backend.sent.size() == begin_sent_before) { std::cerr << "handshake: no session-begin send\n"; return false; }
	const auto& begin = backend.sent.back();
	protocol::DatagramView begin_datagram{};
	if (protocol::decode_and_validate_datagram({begin.bytes.data(), begin.bytes.size()},
		{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1}, begin_datagram) != protocol::ValidationError::None ||
		begin_datagram.header.message_type != protocol::MessageType::SessionBegin) {
		std::cerr << "handshake: expected session-begin\n";
		return false;
	}
	Backend::Packet begin_ack{};
	if (!make_ack(begin, endpoint, sequence + 100U, begin_ack)) { std::cerr << "handshake: session-begin ack encoding failed\n"; return false; }
	backend.receives.push_back({detail::IoStatus::Complete, std::move(begin_ack)});
	const auto final_ack_index = backend.receives.size() - 1U;
	for (std::uint32_t attempt = 0U; attempt < 8U && backend.receive_index <= final_ack_index; ++attempt) {
		if (!tick_handshake(runtime, now_us)) { std::cerr << "handshake: session-begin ack tick failed\n"; return false; }
		if (!queue_new_client_responses(backend, responses, now_us)) return false;
		release_snapshot_acks_when_ingress_is_safe(backend, runtime, responses);
		now_us += 1U;
	}
	if (backend.receive_index <= final_ack_index) { std::cerr << "handshake: session-begin ack not consumed\n"; return false; }
	return true;
}

bool establish_live_baselines(Backend& backend,
	detail::NativeSessionRuntime& runtime,
	const EngineView& view,
	std::uint64_t& now_us,
	std::size_t expected_clients,
	ClientResponseTracker& responses,
	bool diagnostic) noexcept
{
	// ACK APPLIED is a client-side transaction result: it is valid only after
	// all fragments of the exact logical FullSnapshot message were received.
	// The native producer exposes one candidate message here, but that message
	// can span up to 1024 datagrams.
	std::vector<SnapshotAckState> snapshot_ack_states;
	std::size_t sent_index = 0U;
	auto active_baselines = [&]() noexcept {
		std::size_t count = 0U;
		for (std::size_t index = 0U; index < expected_clients; ++index) {
			const auto* slot = detail::NativeSessionRuntimeTestAccess::slot(runtime, index);
			if (slot != nullptr && slot->snapshot.has_active_baseline()) ++count;
		}
		return count;
	};
	std::uint32_t attempts = 0U;
	for (; attempts < 512U && active_baselines() < expected_clients; ++attempts) {
		if (!tick(runtime, view, now_us)) return false;
		if (!queue_new_client_responses(backend, responses, now_us)) return false;
		if (!drain_for_pending_snapshot_acks(backend, runtime, responses, now_us)) return false;
		sent_index = backend.sent.size();
		now_us += 1U;
		while (sent_index < backend.sent.size()) {
			const auto& packet = backend.sent[sent_index++];
			protocol::DatagramView datagram{};
			if (protocol::decode_and_validate_datagram({packet.bytes.data(), packet.bytes.size()},
				{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1}, datagram) != protocol::ValidationError::None ||
				datagram.header.message_type != protocol::MessageType::FullSnapshot) continue;
			const SnapshotAckIdentity identity{datagram.header.session_id, datagram.header.message_id,
				datagram.header.message_crc32};
			auto state = std::find_if(snapshot_ack_states.begin(), snapshot_ack_states.end(),
				[&](const SnapshotAckState& existing) { return same_snapshot_ack_identity(existing.identity, identity); });
			if (state == snapshot_ack_states.end()) {
				if (datagram.header.fragment_count == 0U || datagram.header.fragment_index >= datagram.header.fragment_count) return false;
				SnapshotAckState created;
				created.identity = identity;
				created.fragment_count = datagram.header.fragment_count;
				created.fragments_seen.assign(datagram.header.fragment_count, false);
				snapshot_ack_states.push_back(std::move(created));
				state = snapshot_ack_states.end() - 1;
				if (diagnostic) std::cerr << "snapshot-trace observed session=" << datagram.header.session_id
						  << " message=" << datagram.header.message_id << " crc=" << datagram.header.message_crc32
						  << " fragments=" << datagram.header.fragment_count << '\n';
			}
			if (state->fragment_count != datagram.header.fragment_count ||
				datagram.header.fragment_index >= state->fragments_seen.size()) return false;
			state->fragments_seen[datagram.header.fragment_index] = true;
			if (diagnostic) std::cerr << "snapshot-trace fragment session=" << datagram.header.session_id
					  << " message=" << datagram.header.message_id << " index=" << datagram.header.fragment_index
					  << '/' << datagram.header.fragment_count << '\n';
			if (state->ack_queued || std::find(state->fragments_seen.begin(), state->fragments_seen.end(), false) !=
				state->fragments_seen.end()) continue;
			Backend::Packet ack{};
			if (!make_ack(packet, packet.endpoint,
				static_cast<std::uint32_t>(100U + snapshot_ack_states.size()), ack)) return false;
			backend.receives.push_back({detail::IoStatus::Complete, std::move(ack)});
			state->ack_queued = true;
			if (diagnostic) std::cerr << "snapshot-trace ack-queued session=" << identity.session_id
					  << " message=" << identity.message_id << " crc=" << identity.message_crc32
					  << " packet-sequence=" << (100U + snapshot_ack_states.size()) << '\n';
		}
	}
	for (std::uint32_t attempt = 0U; attempt < 32U; ++attempt) {
		if (!tick(runtime, view, now_us)) return false;
		if (!queue_new_client_responses(backend, responses, now_us)) return false;
		if (!drain_for_pending_snapshot_acks(backend, runtime, responses, now_us)) return false;
		now_us += 1U;
	}
	const auto baseline_count = active_baselines();
	if (baseline_count != expected_clients) {
		std::cerr << "baseline-trace attempts=" << attempts << " expected=" << expected_clients
				  << " snapshot-transactions=" << responses.snapshot_ack_states.size() << " active-baselines=" << baseline_count
				  << " receives=" << backend.receive_index << '/' << backend.receives.size()
				  << " sent=" << backend.sent.size() << '\n';
		for (std::size_t index = 0U; index < expected_clients; ++index) {
			const auto* slot = detail::NativeSessionRuntimeTestAccess::slot(runtime, index);
			if (slot == nullptr) {
				std::cerr << "baseline-trace slot=" << index << " unavailable\n";
				continue;
			}
			std::cerr << "baseline-trace slot=" << index
					  << " progress=" << static_cast<unsigned>(slot->progress)
					  << " session=" << slot->session_id
					  << " endpoint-port=" << slot->endpoint.port()
					  << " candidate=" << (slot->snapshot.has_candidate() ? 1U : 0U)
					  << " baseline=" << (slot->snapshot.has_active_baseline() ? 1U : 0U)
					  << " candidate-id=" << slot->snapshot.candidate_snapshot_id()
					  << " baseline-id=" << slot->snapshot.active_snapshot_id()
				  << " snapshot-egress=" << (slot->snapshot_egress.has_candidate() ? 1U : 0U)
				  << " reliable=" << slot->reliable_items_in_use << '\n';
			for (const auto& part : slot->snapshot_egress.candidate_parts()) {
				std::cerr << "baseline-trace slot=" << index << " egress-target"
						  << " message=" << part.target.message_id
						  << " type=" << static_cast<unsigned>(part.target.message_type)
						  << " fragments=" << part.target.fragment_count
						  << " crc=" << part.target.message_crc32 << '\n';
			}
			for (const auto& state : responses.snapshot_ack_states) {
				if (state.identity.session_id != slot->session_id) continue;
				std::cerr << "baseline-trace slot=" << index << " ack-state message=" << state.identity.message_id
						  << " crc=" << state.identity.message_crc32 << " fragments=" << state.fragment_count
						  << " queued=" << (state.ack_queued ? 1U : 0U) << " seen=";
				for (const auto seen : state.fragments_seen) std::cerr << (seen ? '1' : '0');
				std::cerr << '\n';
			}
		}
	}
	return baseline_count == expected_clients;
}

protocol::EndpointKey peer(std::uint8_t last, std::uint16_t port) noexcept
{
	return protocol::EndpointKey::from_ipv4({127U, 0U, 0U, last}, port);
}

template <typename Container>
protocol::ByteView byte_view(const Container& bytes) noexcept
{
	return {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
}

template <typename Container>
protocol::MutableByteView mutable_byte_view(Container& bytes) noexcept
{
	return {reinterpret_cast<std::uint8_t*>(bytes.data()), bytes.size()};
}

bool make_hello(std::uint64_t nonce, protocol::EndpointKey endpoint, Backend::Packet& result) noexcept
{
	protocol::HelloPayload payload{};
	payload.client_nonce = nonce;
	payload.client_send_t0_us = 100U;
	payload.min_major = payload.max_major = protocol::VersionMajor;
	payload.min_minor = payload.max_minor = protocol::VersionMinorV1_1;
	payload.requested_visibility_mode = protocol::VisibilityMode::Cockpit;
	payload.requested_heartbeat_ms = 1000U;
	std::array<std::uint8_t, protocol::HelloPayloadPrefixSize> encoded{};
	std::size_t payload_size = 0U;
	if (protocol::encode_hello_payload(payload, mutable_byte_view(encoded), payload_size) != protocol::ValidationError::None) return false;
	protocol::TelemetryDatagramHeader header{};
	header.version_minor = protocol::VersionMinorV1_1;
	header.message_type = protocol::MessageType::Hello;
	header.packet_sequence = 1U;
	header.sent_time_us = 100U;
	header.message_id = 1U;
	header.message_size = static_cast<std::uint32_t>(payload_size);
	header.message_crc32 = protocol::crc32_iso_hdlc({encoded.data(), payload_size});
	result.endpoint = endpoint;
	result.bytes.resize(protocol::HeaderSizeV1 + payload_size);
	std::size_t written = 0U;
	return protocol::encode_datagram(header, {protocol::VersionMinorV1_1, protocol::VersionMinorV1_1},
		{encoded.data(), payload_size}, mutable_byte_view(result.bytes), written) == protocol::ValidationError::None;
}

bool make_ack(const Backend::Packet& target, protocol::EndpointKey endpoint, std::uint32_t sequence, Backend::Packet& result) noexcept
{
	protocol::DatagramView decoded{};
	if (protocol::decode_and_validate_datagram(byte_view(target.bytes), {protocol::VersionMinorV1_1, protocol::VersionMinorV1_1}, decoded) != protocol::ValidationError::None) return false;
	protocol::AckPayload ack{};
	ack.target_message_id = decoded.header.message_id;
	ack.target_message_type = decoded.header.message_type;
	ack.ack_flags = protocol::KnownAckFlags;
	ack.target_fragment_count = decoded.header.fragment_count;
	ack.target_message_crc32 = decoded.header.message_crc32;
	std::array<std::uint8_t, protocol::AckPayloadSize> payload{};
	std::size_t payload_size = 0U;
	if (protocol::encode_ack_payload(ack, mutable_byte_view(payload), payload_size) != protocol::ValidationError::None) return false;
	protocol::TelemetryDatagramHeader header{};
	header.version_minor = decoded.header.version_minor;
	header.message_type = protocol::MessageType::Ack;
	header.session_id = decoded.header.session_id;
	header.packet_sequence = sequence;
	header.sent_time_us = 200U + sequence;
	header.message_id = sequence;
	header.message_size = static_cast<std::uint32_t>(payload_size);
	header.message_crc32 = protocol::crc32_iso_hdlc({payload.data(), payload_size});
	result.endpoint = endpoint;
	result.bytes.resize(protocol::HeaderSizeV1 + payload_size);
	std::size_t written = 0U;
	return protocol::encode_datagram(header, {header.version_minor, header.version_minor}, {payload.data(), payload_size},
		mutable_byte_view(result.bytes), written) == protocol::ValidationError::None;
}

void Backend::queue_loopback_snapshot_ack(const Packet& packet) noexcept
{
	protocol::DatagramView datagram{};
	if (protocol::decode_and_validate_datagram(byte_view(packet.bytes), {protocol::VersionMinorV1_1, protocol::VersionMinorV1_1}, datagram) !=
		protocol::ValidationError::None || datagram.header.message_type != protocol::MessageType::FullSnapshot ||
		datagram.header.fragment_count == 0U || datagram.header.fragment_index + 1U != datagram.header.fragment_count) return;
	// Native snapshot egress emits fragments in-order. The final fragment can
	// therefore represent ACK APPLIED only after every fragment was handed to
	// this loopback client; enqueuing here lets the same scheduler turn consume
	// it after try_send cleared the single output slot.
	Packet ack{};
	if (make_ack(packet, packet.endpoint, next_auto_ack_sequence++, ack)) {
		++full_snapshot_acks_injected;
		receives.push_back({detail::IoStatus::Complete, std::move(ack)});
	}
}

bool make_resync_request(std::uint64_t session_id,
	protocol::EndpointKey endpoint,
	std::uint32_t sequence,
	std::uint32_t request_id,
	std::uint64_t now_us,
	Backend::Packet& result) noexcept
{
	protocol::ResyncRequestPayload request{};
	request.request_id = request_id;
	request.reason = protocol::ResyncReason::UnknownBaseline;
	request.request_flags = protocol::ResyncRequestFlagRequireFullSnapshot;
	request.client_send_time_us = now_us;
	std::array<std::uint8_t, protocol::ResyncRequestPayloadSize> payload{};
	std::size_t payload_size = 0U;
	if (protocol::encode_resync_request_payload(request, mutable_byte_view(payload), payload_size) != protocol::ValidationError::None) return false;
	protocol::TelemetryDatagramHeader header{};
	header.version_minor = protocol::VersionMinorV1_1;
	header.message_type = protocol::MessageType::ResyncRequest;
	header.flags = protocol::MessageFlagAckRequired;
	header.session_id = session_id;
	header.packet_sequence = sequence;
	header.sent_time_us = now_us;
	header.message_id = sequence;
	header.message_size = static_cast<std::uint32_t>(payload_size);
	header.message_crc32 = protocol::crc32_iso_hdlc({payload.data(), payload_size});
	result.endpoint = endpoint;
	result.bytes.resize(protocol::HeaderSizeV1 + payload_size);
	std::size_t written = 0U;
	return protocol::encode_datagram(header, {header.version_minor, header.version_minor}, {payload.data(), payload_size},
		mutable_byte_view(result.bytes), written) == protocol::ValidationError::None;
}

bool Backend::queue_loopback_resync_request(const Packet& packet) noexcept
{
	protocol::DatagramView datagram{};
	if (protocol::decode_and_validate_datagram(byte_view(packet.bytes), {protocol::VersionMinorV1_1, protocol::VersionMinorV1_1}, datagram) !=
		protocol::ValidationError::None || datagram.header.message_type != protocol::MessageType::Delta) return false;
	Packet request{};
	if (!make_resync_request(datagram.header.session_id, packet.endpoint, next_resync_sequence,
		next_resync_sequence, datagram.header.sent_time_us, request)) return false;
	++next_resync_sequence;
	++resync_requests_injected;
	if (diagnostic) {
		std::cerr << "impairment-resync injected session=" << datagram.header.session_id
				  << " port=" << packet.endpoint.port() << " request-sequence=" << (next_resync_sequence - 1U) << '\n';
	}
	receives.push_back({detail::IoStatus::Complete, std::move(request)});
	return true;
}

bool make_heartbeat_response(const Backend::Packet& target,
	protocol::EndpointKey endpoint,
	std::uint32_t sequence,
	std::uint64_t now_us,
	Backend::Packet& result) noexcept
{
	protocol::DatagramView decoded{};
	if (protocol::decode_and_validate_datagram(byte_view(target.bytes), {protocol::VersionMinorV1_1, protocol::VersionMinorV1_1}, decoded) !=
		protocol::ValidationError::None || decoded.header.message_type != protocol::MessageType::Heartbeat) return false;
	protocol::HeartbeatPayload request{};
	if (protocol::decode_heartbeat_payload(decoded.payload, request) != protocol::ValidationError::None ||
		request.kind != protocol::HeartbeatKind::Request) return false;
	protocol::HeartbeatPayload response{};
	response.probe_id = request.probe_id;
	response.kind = protocol::HeartbeatKind::Response;
	response.origin_t0_us = request.origin_t0_us;
	response.receive_t1_us = now_us;
	response.transmit_t2_us = now_us;
	std::array<std::uint8_t, protocol::HeartbeatPayloadSize> payload{};
	std::size_t payload_size = 0U;
	if (protocol::encode_heartbeat_payload(response, mutable_byte_view(payload), payload_size) != protocol::ValidationError::None) return false;
	protocol::TelemetryDatagramHeader header{};
	header.version_minor = decoded.header.version_minor;
	header.message_type = protocol::MessageType::Heartbeat;
	header.session_id = decoded.header.session_id;
	header.packet_sequence = sequence;
	header.sent_time_us = now_us;
	header.message_id = sequence;
	header.message_size = static_cast<std::uint32_t>(payload_size);
	header.message_crc32 = protocol::crc32_iso_hdlc({payload.data(), payload_size});
	result.endpoint = endpoint;
	result.bytes.resize(protocol::HeaderSizeV1 + payload_size);
	std::size_t written = 0U;
	return protocol::encode_datagram(header, {header.version_minor, header.version_minor}, {payload.data(), payload_size},
		mutable_byte_view(result.bytes), written) == protocol::ValidationError::None;
}

} // namespace

int main(int argc, char** argv)
{
	const char* workload = nullptr;
	const char* output_path = nullptr;
	std::uint64_t samples = 0U, warmup = 0U, flight_hz = 0U, seed = 0U;
	bool diagnostic = false;
	for (int index = 1; index + 1 < argc; index += 2) {
		if (std::strcmp(argv[index], "--workload") == 0) workload = argv[index + 1];
		else if (std::strcmp(argv[index], "--output") == 0) output_path = argv[index + 1];
		else if (std::strcmp(argv[index], "--samples") == 0) { if (!parse_u64(argv[index + 1], samples)) return 2; }
		else if (std::strcmp(argv[index], "--warmup") == 0) { if (!parse_u64(argv[index + 1], warmup)) return 2; }
		else if (std::strcmp(argv[index], "--flight-hz") == 0) { if (!parse_u64(argv[index + 1], flight_hz)) return 2; }
		else if (std::strcmp(argv[index], "--seed") == 0) { if (!parse_u64(argv[index + 1], seed)) return 2; }
		else if (std::strcmp(argv[index], "--diagnostic") == 0) {
			std::uint64_t value = 0U;
			if (!parse_u64(argv[index + 1], value) || value > 1U) return 2;
			diagnostic = value != 0U;
		}
		else return 2;
	}
	if (argc < 13 || (argc % 2) == 0 || workload == nullptr || output_path == nullptr || samples == 0U || flight_hz == 0U) return 2;
	PerformanceWorkload workload_mode{};
	if (std::strcmp(workload, "active-one-client") == 0) workload_mode = PerformanceWorkload::ActiveOneClient;
	else if (std::strcmp(workload, "active-four-clients") == 0) workload_mode = PerformanceWorkload::ActiveFourClients;
	else if (std::strcmp(workload, "would-block-loss-resync") == 0) workload_mode = PerformanceWorkload::WouldBlockLossResync;
	else return 2;
	const bool impairment_mode = workload_mode == PerformanceWorkload::WouldBlockLossResync;
	if (diagnostic) {
		std::cerr << "runner-workload parsed=" << workload
				  << " impairment=" << (impairment_mode ? 1U : 0U) << '\n';
	}

	Backend backend;
	backend.diagnostic = diagnostic;
	Completion completion;
	FixedRandom ids_random;
	ids_random.value = seed == 0U ? 1U : seed;
	FixedRandom packet_random;
	packet_random.value = seed == 0U ? 1U : seed ^ 0xa5a5a5a5U;
	detail::SessionIdRegistry registry;
	if (!registry.allocate_storage()) return 3;
	detail::SessionIdAllocator ids(ids_random, registry);
	auto runtime = std::make_unique<detail::NativeSessionRuntime>(backend, completion);
	telemetry::TelemetryConfig config;
	config.enabled = true;
	config.bind_addresses.clear();
	config.bind_addresses.add(telemetry::NumericIpAddress::from_ipv4({127U, 0U, 0U, 1U}));
	config.flight_hz = static_cast<std::uint8_t>(flight_hz);
	config.max_clients = workload_mode == PerformanceWorkload::ActiveFourClients ? 4U : 1U;
	detail::NativeSessionStartRequest request{&config, 0x1020304050607080ULL, &ids, &packet_random};
	if (runtime->start(request) != detail::NativeSessionStartStatus::Started) return 3;

	EngineView view;
	std::uint64_t now_us = 1'000'000U;
	const std::size_t expected_clients = config.max_clients;
	ClientResponseTracker responses;
	responses.trace_bootstrap = diagnostic;
	for (std::size_t client = 0U; client < expected_clients; ++client) {
		if (!establish_ready(backend, *runtime, view, peer(static_cast<std::uint8_t>(client + 1U),
			static_cast<std::uint16_t>(43000U + client)), seed + client + 1U, now_us,
			static_cast<std::uint32_t>(10U + client), responses)) return 41;
	}
	if (runtime->active_sessions() != expected_clients) return 42;
	if (!establish_live_baselines(backend, *runtime, view, now_us, expected_clients,
		responses, diagnostic)) return 43;
	const auto tick_step_us = (1'000'000U + flight_hz - 1U) / flight_hz;
	for (std::uint64_t index = 0U; index < warmup; ++index) {
		if (!tick(*runtime, view, now_us)) return benchmark_stage_failure("warmup-tick", index, now_us, *runtime, responses);
		if (!queue_new_client_responses(backend, responses, now_us)) return benchmark_stage_failure("warmup-response", index, now_us, *runtime, responses);
		if (!drain_for_pending_snapshot_acks(backend, *runtime, responses, now_us)) return benchmark_stage_failure("warmup-drain", index, now_us, *runtime, responses);
		now_us += tick_step_us;
	}
	// Active traffic legitimately keeps one output datagram and/or reliable
	// control item in flight. Certification requires bounded, stable ownership,
	// not an artificially empty scheduler. Snapshot ACKs, sessions and active
	// baselines must nevertheless be fully settled before timing begins.
	const auto resource_limit = expected_clients * protocol::ReliableWindowMaximumEntries + 1U;
	if (!responses.pending_snapshot_acks.empty() || runtime->active_sessions() != expected_clients ||
		active_baseline_count(*runtime) != expected_clients || runtime->owned_usage().reliable_items > resource_limit) return 44;
	if (impairment_mode) {
		if (!force_authoritative_delta(*runtime, now_us, diagnostic)) return 47;
		backend.impairment_stage = Backend::ImpairmentStage::WouldBlockDelta;
		backend.would_block_remaining = 4U;
		if (diagnostic) std::cerr << "impairment-transition armed would-block-delta\n";
	}
	std::size_t max_observed_queue_depth = 0U;
	std::size_t max_observed_reliable_items = runtime->owned_usage().reliable_items;
	responses.trace_bootstrap = false;
	detail::NativeSessionRuntimeTestAccess::begin_performance_observation(*runtime);
	std::ofstream output(output_path, std::ios::out | std::ios::trunc);
	if (!output) return 5;
	output << "sample_index,tick_duration_ns,collect_ns,diff_ns,state_image_build_ns,state_image_fill_ns,state_image_publish_validate_ns,state_image_adopt_ns,state_image_semantic_validate_ns,delta_build_ns,serialization_ns,network_ns,allocation_events,syscall_count,queue_depth,baselines_active,is_keyframe\n";
	for (std::uint64_t index = 0U; index < samples; ++index) {
		// Diagnostic-only microscope for the deterministic four-client loss at
		// the first recurring keyframe. It has no effect on the runner's I/O,
		// ACK scheduling or production controller state.
		const auto trace_periodic_window = diagnostic && workload_mode == PerformanceWorkload::ActiveFourClients &&
			index >= 595U && index <= 606U;
		if (trace_periodic_window) {
			responses.trace_bootstrap = true;
			trace_periodic_snapshot_state(*runtime, responses, index, now_us);
		}
		if (!drain_for_pending_snapshot_acks(backend, *runtime, responses, now_us)) return benchmark_stage_failure("sample-pre-drain", index, now_us, *runtime, responses);
		if (!tick(*runtime, view, now_us)) return benchmark_stage_failure("sample-tick", index, now_us, *runtime, responses);
		if (!queue_new_client_responses(backend, responses, now_us)) return benchmark_stage_failure("sample-response", index, now_us, *runtime, responses);
		release_snapshot_acks_when_ingress_is_safe(backend, *runtime, responses);
		if (trace_periodic_window) trace_periodic_snapshot_state(*runtime, responses, index, now_us);
		responses.trace_bootstrap = false;
		now_us += tick_step_us;
		const auto sample = detail::NativeSessionRuntimeTestAccess::last_performance_sample(*runtime);
		const auto direct_baselines = active_baseline_count(*runtime);
		const auto usage = runtime->owned_usage();
		max_observed_queue_depth = std::max(max_observed_queue_depth, sample.queue_depth);
		max_observed_reliable_items = std::max(max_observed_reliable_items, usage.reliable_items);
		if (!responses.pending_snapshot_acks.empty() || runtime->active_sessions() != expected_clients ||
			usage.reliable_items > resource_limit || sample.queue_depth > resource_limit ||
			direct_baselines != expected_clients || sample.baselines_active != direct_baselines) {
			std::cerr << "baseline-sample invariant index=" << index << " expected=" << expected_clients
					  << " direct=" << direct_baselines << " measured=" << sample.baselines_active
					  << " sessions=" << runtime->active_sessions() << " reliable=" << usage.reliable_items
					  << " queue-depth=" << sample.queue_depth << " pending-acks=" << responses.pending_snapshot_acks.size() << '\n';
			return 45;
		}
		output << index << ',' << sample.tick_duration_ns << ',' << sample.collect_duration_ns << ',' << sample.diff_duration_ns
			<< ',' << sample.state_image_build_duration_ns << ',' << sample.state_image_fill_duration_ns << ','
			<< sample.state_image_publish_validate_duration_ns << ',' << sample.state_image_adopt_duration_ns << ','
			<< sample.state_image_semantic_validate_duration_ns << ',' << sample.delta_build_duration_ns << ','
			<< sample.serialization_duration_ns << ',' << sample.network_duration_ns << ',' << sample.allocation_events
			<< ',' << sample.syscall_count << ',' << sample.queue_depth << ',' << sample.baselines_active << ','
			<< (sample.keyframe ? 1U : 0U) << '\n';
	}
	if (diagnostic) {
		std::cerr << "steady-state-resource max-queue-depth=" << max_observed_queue_depth
				  << " max-reliable-items=" << max_observed_reliable_items << " limit=" << resource_limit << '\n';
	}
	if (impairment_mode) {
		const auto final_baselines = active_baseline_count(*runtime);
		if (diagnostic) {
			std::cerr << "impairment-evidence would-block=" << backend.would_block_sends
					  << " lost-delta=" << backend.lost_delta_sends
					  << " resync-injected=" << backend.resync_requests_injected
					  << " resync-validated=" << backend.resync_validated_acks_seen
					  << " fullsnapshot-acks=" << backend.full_snapshot_acks_injected
					  << " sessions=" << runtime->active_sessions()
					  << " baselines=" << final_baselines << '\n';
		}
		if (backend.would_block_sends == 0U || backend.lost_delta_sends != 1U || backend.resync_requests_injected != 1U ||
			backend.resync_validated_acks_seen == 0U || backend.full_snapshot_acks_injected < 2U ||
			backend.impairment_stage != Backend::ImpairmentStage::Recovered ||
			runtime->active_sessions() != expected_clients || final_baselines != expected_clients) return 46;
	}
	return output ? 0 : 5;
}
