// WP11 test-only reliability peer.  It drives the actual native UDP backend;
// it does not duplicate or alter production transport/session code.
#include "telemetry/config.h"
#include "telemetry/engine_adapter.h"
#include "telemetry/identity.h"
#include "telemetry/native_session_runtime.h"
#include "telemetry/native_session_runtime_test_seam.h"
#include "telemetry/protocol/telemetry_control_messages.h"
#include "telemetry/protocol/telemetry_crc32.h"
#include "telemetry/protocol/telemetry_datagram.h"
#include "telemetry/protocol/telemetry_reliability_messages.h"
#include "telemetry/protocol/telemetry_state_messages.h"
#include "telemetry/transport.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

namespace {
namespace detail = telemetry::detail;
namespace protocol = telemetry::protocol;

template <typename Container> protocol::ByteView bytes(const Container& input) noexcept
{
	return {reinterpret_cast<const std::uint8_t*>(input.data()), input.size()};
}
template <typename Container> protocol::MutableByteView writable_bytes(Container& input) noexcept
{
	return {reinterpret_cast<std::uint8_t*>(input.data()), input.size()};
}

struct FixedRandom final : detail::RandomSource {
	std::uint64_t value = 1U;
	bool next_u64(std::uint64_t& output) noexcept override { output = value++; return true; }
};
struct Completion final : detail::NativeOutputCompletionPort {
	void complete(detail::SessionController& controller, detail::IoStatus status) noexcept override { forwarder.complete(controller, status); }
	detail::NativeOutputCompletionForwarder forwarder;
};
struct Engine final : detail::EngineReadView {
	float position_x = 1.F;
	bool in_mission() const noexcept override { return true; } bool player_exists() const noexcept override { return true; }
	bool player_object_exists() const noexcept override { return true; } bool player_ship_exists() const noexcept override { return true; }
	bool player_object_is_ship() const noexcept override { return true; } bool player_object_ship_instance_in_range() const noexcept override { return true; }
	bool player_object_matches_player() const noexcept override { return true; } bool player_ship_matches_object() const noexcept override { return true; }
	void read_player_kinematics(detail::EnginePlayerKinematicsRead& out) const noexcept override {
		out.object_signature = 42U; out.position_world = {position_x,2.F,3.F}; out.velocity_world = {4.F,5.F,6.F};
		out.rotational_velocity_local = {.1F,.2F,.3F}; out.radius = 7.F;
	}
};
struct Backend final : detail::UdpSocketBackend {
	struct Packet { std::vector<std::uint8_t> data; protocol::EndpointKey endpoint; };
	std::vector<Packet> rx, tx; std::size_t rx_index = 0U;
	std::uint64_t send_attempts = 0U, loss = 0U, duplicate_delivered = 0U, reorder_delivered = 0U, jitter = 0U, fragment_probe = 0U;
	std::uint32_t loss_percent = 1U; bool burst = false, impairments_active = false;
	std::uint64_t now_us = 0U, loss_accumulator = 0U, burst_remaining = 0U;
	std::uint64_t delta_loss = 0U, snapshot_fragment_loss = 0U, would_block_injected = 0U, outage_loss = 0U, jitter_delayed = 0U;
	// The bounded [30 s, 32 s) downstream cut is the recovery oracle's
	// reference impairment.  It is deliberately distinct from the continuous
	// configured loss, whose end is not a meaningful recovery boundary.
	std::uint64_t bounded_impairment_start_us = 0U, bounded_impairment_end_us = 0U;
	// Armed only after the peer has requested resynchronization.  Returning
	// Complete while withholding this datagram models a real downstream UDP
	// loss: the production reliable window must retransmit it before it can be
	// ACKed.  Handshake control is never synthetically lost.
	std::uint32_t recovery_losses_remaining = 0U;
	bool outage_active() const noexcept { const auto second=(now_us/1000000U)%60U; return second>=30U && second<32U; }
	void observe_bounded_impairment_window() noexcept {
		if (!impairments_active || bounded_impairment_start_us != 0U || !outage_active()) return;
		const auto minute_start_us = (now_us / 60000000U) * 60000000U;
		bounded_impairment_start_us = minute_start_us + 30000000U;
		bounded_impairment_end_us = minute_start_us + 32000000U;
	}
	detail::SocketOpenResult open_socket(const detail::SocketOpenRequest& request) noexcept override {
		return {detail::SocketOpenStatus::Complete,1U,protocol::EndpointKey::from_ipv4({127U,0U,0U,1U},request.port)};
	}
	detail::SocketReceiveResult try_receive(detail::SocketHandle, protocol::MutableByteView out) noexcept override {
		++jitter;
		if (impairments_active && rx_index < rx.size() && (jitter % 17U)==0U) { ++jitter_delayed; return {detail::IoStatus::WouldBlock,{},0U,false}; }
		if (rx_index >= rx.size()) { ++fragment_probe; return {detail::IoStatus::WouldBlock,{},0U,false}; }
		const auto& packet = rx[rx_index++];
		if (packet.data.size() > out.size) return {detail::IoStatus::Error,{},0U,true};
		std::copy(packet.data.begin(),packet.data.end(),out.data);
		return {detail::IoStatus::Complete,packet.endpoint,packet.data.size(),false};
	}
	detail::SocketSendResult try_send(detail::SocketHandle, const protocol::EndpointKey& endpoint, protocol::ByteView payload) noexcept override {
		++send_attempts;
		protocol::DatagramView datagram{};
		const auto decoded = protocol::decode_and_validate_datagram(payload,{protocol::VersionMinorV1_1,protocol::VersionMinorV1_1},datagram)==protocol::ValidationError::None;
		if (recovery_losses_remaining != 0U && decoded && datagram.header.message_type==protocol::MessageType::FullSnapshot) {
			--recovery_losses_remaining; ++loss; ++snapshot_fragment_loss;
			return {detail::IoStatus::Complete,payload.size};
		}
		if (impairments_active && decoded && (datagram.header.message_type==protocol::MessageType::Delta || datagram.header.message_type==protocol::MessageType::FullSnapshot)) {
			// A two-second deterministic cut each simulated minute is real UDP
			// downstream loss, never a synthetic report-only counter.
			if (outage_active()) {
				++loss; ++outage_loss; return {detail::IoStatus::Complete,payload.size};
			}
			if (datagram.header.message_type==protocol::MessageType::Delta && (send_attempts % 47U)==0U) {
				++would_block_injected; return {detail::IoStatus::WouldBlock,0U};
			}
			bool drop = false;
			if (burst) {
				if (burst_remaining != 0U) { --burst_remaining; drop = true; }
				else { loss_accumulator += loss_percent; if (loss_accumulator >= 300U) { loss_accumulator -= 300U; burst_remaining=2U; drop=true; } }
			} else { loss_accumulator += loss_percent; if (loss_accumulator >= 100U) { loss_accumulator -= 100U; drop=true; } }
			if (drop) { ++loss; if(datagram.header.message_type==protocol::MessageType::Delta)++delta_loss; else ++snapshot_fragment_loss; return {detail::IoStatus::Complete,payload.size}; }
		}
		tx.push_back({{payload.data,payload.data + payload.size},endpoint});
		return {detail::IoStatus::Complete,payload.size};
	}
	// Fully encoded peer frames are actually queued through the production
	// receive path. They are not merely observed counters.
	void enqueue_duplicate(Packet packet) {
		rx.push_back(packet);
		rx.push_back(std::move(packet));
		++duplicate_delivered;
	}
	void enqueue_reordered(Packet current, Packet delayed) {
		rx.push_back(std::move(current));
		rx.push_back(std::move(delayed));
		++reorder_delivered;
	}
	void close_socket(detail::SocketHandle) noexcept override {}
	void reset_peer_io() noexcept {
		rx.clear(); tx.clear(); rx_index=0U; impairments_active=false; recovery_losses_remaining=0U;
	}
};

bool valid_profile(const char* profile, Backend& backend) noexcept {
	if (std::strncmp(profile,"independent-loss-",17) == 0) backend.burst=false;
	else if (std::strncmp(profile,"burst-loss-",11) == 0) backend.burst=true;
	else return false;
	const auto value = std::strtoul(profile + (backend.burst ? 11 : 17),nullptr,10);
	backend.loss_percent=static_cast<std::uint32_t>(value);
	return value == 1U || value == 5U || value == 20U;
}
std::uint32_t recovery_loss_budget(const Backend& backend) noexcept
{
	// This is a short deterministic smoke profile, not the ten-minute rate
	// proof.  Two retained retransmission windows fit in its bounded simulated
	// timeline; the JSON still records the requested percentage/mode exactly.
	return backend.loss_percent == 1U ? 1U : 2U;
}
bool tick(detail::NativeSessionRuntime& runtime, const Engine& engine, std::uint64_t now, std::uint32_t mission_generation=1U) noexcept {
	return runtime.service_tick({now,mission_generation,true},engine) == detail::NativeSessionTickStatus::Complete;
}
bool r2_tick(detail::NativeSessionRuntime& runtime, std::uint64_t now, std::uint32_t mission_generation=1U) noexcept {
	return detail::NativeSessionRuntimeTestAccess::service_r2_tick(runtime,{now,mission_generation,true}) == detail::NativeSessionTickStatus::Complete;
}
bool make_hello(protocol::EndpointKey endpoint, Backend::Packet& result) noexcept {
	protocol::HelloPayload value{};
	value.client_nonce=0x1020304050607080ULL; value.client_send_t0_us=100U;
	value.min_major=value.max_major=protocol::VersionMajor; value.min_minor=value.max_minor=protocol::VersionMinorV1_1;
	value.requested_visibility_mode=protocol::VisibilityMode::Cockpit; value.requested_heartbeat_ms=1000U;
	std::array<std::uint8_t,protocol::HelloPayloadPrefixSize> payload{}; std::size_t payload_size=0U;
	if(protocol::encode_hello_payload(value,writable_bytes(payload),payload_size)!=protocol::ValidationError::None)return false;
	protocol::TelemetryDatagramHeader header{}; header.version_minor=protocol::VersionMinorV1_1; header.message_type=protocol::MessageType::Hello;
	header.packet_sequence=1U; header.sent_time_us=100U; header.message_id=1U; header.message_size=static_cast<std::uint32_t>(payload_size);
	header.message_crc32=protocol::crc32_iso_hdlc({payload.data(),payload_size}); result.endpoint=endpoint; result.data.resize(protocol::HeaderSizeV1+payload_size);
	std::size_t written=0U; return protocol::encode_datagram(header,{protocol::VersionMinorV1_1,protocol::VersionMinorV1_1},{payload.data(),payload_size},writable_bytes(result.data),written)==protocol::ValidationError::None;
}
bool make_ack(const Backend::Packet& target, std::uint32_t sequence, Backend::Packet& result) noexcept {
	protocol::DatagramView decoded{};
	if(protocol::decode_and_validate_datagram(bytes(target.data),{protocol::VersionMinorV1_1,protocol::VersionMinorV1_1},decoded)!=protocol::ValidationError::None)return false;
	protocol::AckPayload ack{}; ack.target_message_id=decoded.header.message_id; ack.target_message_type=decoded.header.message_type;
	ack.ack_flags=protocol::KnownAckFlags; ack.target_fragment_count=decoded.header.fragment_count; ack.target_message_crc32=decoded.header.message_crc32;
	std::array<std::uint8_t,protocol::AckPayloadSize> payload{}; std::size_t payload_size=0U;
	if(protocol::encode_ack_payload(ack,writable_bytes(payload),payload_size)!=protocol::ValidationError::None)return false;
	protocol::TelemetryDatagramHeader header{}; header.version_minor=decoded.header.version_minor; header.message_type=protocol::MessageType::Ack;
	header.session_id=decoded.header.session_id; header.packet_sequence=sequence; header.sent_time_us=200U+sequence; header.message_id=sequence;
	header.message_size=static_cast<std::uint32_t>(payload_size); header.message_crc32=protocol::crc32_iso_hdlc({payload.data(),payload_size});
	result.endpoint=target.endpoint; result.data.resize(protocol::HeaderSizeV1+payload_size); std::size_t written=0U;
	return protocol::encode_datagram(header,{header.version_minor,header.version_minor},{payload.data(),payload_size},writable_bytes(result.data),written)==protocol::ValidationError::None;
}
bool make_resync(const Backend::Packet& target, std::uint32_t sequence, Backend::Packet& result) noexcept {
	protocol::DatagramView decoded{};
	if(protocol::decode_and_validate_datagram(bytes(target.data),{protocol::VersionMinorV1_1,protocol::VersionMinorV1_1},decoded)!=protocol::ValidationError::None)return false;
	protocol::ResyncRequestPayload request{}; request.request_id=sequence; request.reason=protocol::ResyncReason::UnknownBaseline;
	request.request_flags=protocol::ResyncRequestFlagRequireFullSnapshot; request.client_send_time_us=decoded.header.sent_time_us;
	std::array<std::uint8_t,protocol::ResyncRequestPayloadSize> payload{}; std::size_t payload_size=0U;
	if(protocol::encode_resync_request_payload(request,writable_bytes(payload),payload_size)!=protocol::ValidationError::None)return false;
	protocol::TelemetryDatagramHeader header{}; header.version_minor=decoded.header.version_minor; header.message_type=protocol::MessageType::ResyncRequest;
	header.flags=protocol::MessageFlagAckRequired; header.session_id=decoded.header.session_id; header.packet_sequence=sequence; header.sent_time_us=decoded.header.sent_time_us+1U;
	header.message_id=sequence; header.message_size=static_cast<std::uint32_t>(payload_size); header.message_crc32=protocol::crc32_iso_hdlc({payload.data(),payload_size});
	result.endpoint=target.endpoint; result.data.resize(protocol::HeaderSizeV1+payload_size); std::size_t written=0U;
	return protocol::encode_datagram(header,{header.version_minor,header.version_minor},{payload.data(),payload_size},writable_bytes(result.data),written)==protocol::ValidationError::None;
}
bool retransmit_reliable(const Backend::Packet& original, std::uint32_t sequence, std::uint64_t now_us, Backend::Packet& result) noexcept {
	protocol::DatagramView decoded{};
	if(protocol::decode_and_validate_datagram(bytes(original.data),{protocol::VersionMinorV1_1,protocol::VersionMinorV1_1},decoded)!=protocol::ValidationError::None)return false;
	protocol::TelemetryDatagramHeader header=decoded.header; header.flags=static_cast<std::uint8_t>(header.flags|protocol::MessageFlagRetransmission); header.packet_sequence=sequence; header.sent_time_us=now_us;
	result.endpoint=original.endpoint; result.data.resize(protocol::HeaderSizeV1+decoded.payload.size); std::size_t written=0U;
	return protocol::encode_datagram(header,{header.version_minor,header.version_minor},decoded.payload,writable_bytes(result.data),written)==protocol::ValidationError::None;
}
bool make_heartbeat_response(const Backend::Packet& target, std::uint32_t sequence, std::uint64_t now_us, Backend::Packet& result) noexcept {
	protocol::DatagramView decoded{};
	if(protocol::decode_and_validate_datagram(bytes(target.data),{protocol::VersionMinorV1_1,protocol::VersionMinorV1_1},decoded)!=protocol::ValidationError::None)return false;
	protocol::HeartbeatPayload request{};
	if(decoded.header.message_type!=protocol::MessageType::Heartbeat || protocol::decode_heartbeat_payload(decoded.payload,request)!=protocol::ValidationError::None || request.kind!=protocol::HeartbeatKind::Request)return false;
	protocol::HeartbeatPayload response{}; response.probe_id=request.probe_id; response.kind=protocol::HeartbeatKind::Response; response.origin_t0_us=request.origin_t0_us; response.receive_t1_us=now_us; response.transmit_t2_us=now_us;
	std::array<std::uint8_t,protocol::HeartbeatPayloadSize> payload{}; std::size_t payload_size=0U;
	if(protocol::encode_heartbeat_payload(response,writable_bytes(payload),payload_size)!=protocol::ValidationError::None)return false;
	protocol::TelemetryDatagramHeader header{}; header.version_minor=decoded.header.version_minor; header.message_type=protocol::MessageType::Heartbeat; header.session_id=decoded.header.session_id; header.packet_sequence=sequence; header.sent_time_us=now_us; header.message_id=sequence; header.message_size=static_cast<std::uint32_t>(payload_size); header.message_crc32=protocol::crc32_iso_hdlc({payload.data(),payload_size});
	result.endpoint=target.endpoint; result.data.resize(protocol::HeaderSizeV1+payload_size); std::size_t written=0U;
	return protocol::encode_datagram(header,{header.version_minor,header.version_minor},{payload.data(),payload_size},writable_bytes(result.data),written)==protocol::ValidationError::None;
}

struct Peer {
	std::size_t tx_seen=0U; std::uint32_t sequence=1000U, resync_message_id=0U, initial_snapshot_id=0U, resync_retries=0U; std::uint64_t session_id=0U; std::uint64_t next_resync_us=0U, next_resync_retry_us=0U, resync_started_us=0U, resync_ack_us=0U, recovery_snapshot_us=0U, max_convergence_us=0U, worst_resync_start_us=0U, worst_resync_ack_us=0U, worst_snapshot_us=0U, worst_live_us=0U, first_live_post_impairment_us=0U, resync_cycles=0U, successful_recoveries=0U, ack_loss=0U, slow_client_withheld_reliable_acks=0U; std::uint8_t session_end_reason=0U; bool welcome=false, begin=false, snapshot=false, delta=false, delta_observed=false, resync_sent=false, resync_ack=false, recovered_snapshot=false, recovery_loss_armed=false, duplicate_delivered=false, reorder_delivered=false, resync_snapshot_flag_seen=false, invalid_ack=false, live_baseline_published=false, recovered_baseline_published=false, slow_client_recovered=false, session_end_seen=false, withhold_recovery_ack=true; Backend::Packet resync_packet{};
	Backend::Packet delayed_snapshot_ack{};
	struct Snapshot { std::uint64_t session=0U; std::uint32_t id=0U, crc=0U; std::uint16_t count=0U; std::vector<bool> parts; bool acked=false; } pending{};
	bool consume(Backend& backend, std::uint64_t now_us) noexcept {
		while(tx_seen<backend.tx.size()) {
			const auto& packet=backend.tx[tx_seen++]; protocol::DatagramView d{};
			const auto decoded=protocol::decode_and_validate_datagram(bytes(packet.data),{protocol::VersionMinorV1_1,protocol::VersionMinorV1_1},d);
			if(decoded!=protocol::ValidationError::None) { std::cerr<<"peer packet validation="<<static_cast<unsigned>(decoded)<<" tx="<<(tx_seen-1U)<<" bytes="<<packet.data.size()<<"\n"; return false; }
			if(d.header.message_type==protocol::MessageType::Welcome) { if(session_id!=0U && session_id!=d.header.session_id)return false; session_id=d.header.session_id; Backend::Packet ack{}; if(!make_ack(packet,sequence++,ack))return false; backend.rx.push_back(std::move(ack)); welcome=true; }
			else if(d.header.message_type==protocol::MessageType::SessionBegin) { Backend::Packet ack{}; if(!make_ack(packet,sequence++,ack))return false; backend.rx.push_back(std::move(ack)); begin=true; }
			else if(d.header.message_type==protocol::MessageType::FullSnapshot) {
				protocol::FullSnapshotPartPayload snapshot_part{};
				if(protocol::decode_full_snapshot_part_payload(d.payload,snapshot_part)!=protocol::ValidationError::None) return false;
				// A periodic candidate already queued before the accepted resync may
				// legitimately drain first.  It is not the replacement proof; require
				// instead that a subsequent fresh RESYNC candidate is observed.
				if(resync_sent && snapshot_part.snapshot_flags==protocol::SnapshotFlagResync) { if(snapshot_part.snapshot_id==initial_snapshot_id) return false; resync_snapshot_flag_seen=true; }
				if(!pending.count) { pending={d.header.session_id,d.header.message_id,d.header.message_crc32,d.header.fragment_count,std::vector<bool>(d.header.fragment_count,false),false}; }
				if(pending.session!=d.header.session_id || pending.id!=d.header.message_id || pending.crc!=d.header.message_crc32 || pending.count!=d.header.fragment_count) {
					// Acknowledged transactions may be followed by a periodic or
					// recovery snapshot. Only an unfinished transaction may not be
					// replaced; that would hide a partial-publication defect.
					if(!pending.acked) return false;
					pending={d.header.session_id,d.header.message_id,d.header.message_crc32,d.header.fragment_count,std::vector<bool>(d.header.fragment_count,false),false};
				}
				if(d.header.fragment_index>=pending.parts.size())return false;
				pending.parts[d.header.fragment_index]=true;
				if(!pending.acked && std::find(pending.parts.begin(),pending.parts.end(),false)==pending.parts.end()) {
					Backend::Packet ack{}; if(!make_ack(packet,sequence++,ack))return false;
					if(!snapshot) { delayed_snapshot_ack=ack; backend.enqueue_duplicate(std::move(ack)); duplicate_delivered=true; snapshot=true; }
					else if(resync_sent) {
						if(withhold_recovery_ack) { withhold_recovery_ack=false; ++ack_loss; ++slow_client_withheld_reliable_acks; continue; }
						if(!reorder_delivered) { if(delayed_snapshot_ack.data.empty()) return false; backend.enqueue_reordered(std::move(ack),std::move(delayed_snapshot_ack)); reorder_delivered=true; }
						else backend.rx.push_back(std::move(ack));
						recovered_snapshot=true; slow_client_recovered=true; recovery_snapshot_us=now_us;
					} else backend.rx.push_back(std::move(ack));
					pending.acked=true;
				}
			}
			else if(d.header.message_type==protocol::MessageType::Delta && snapshot) { delta_observed=true; if(!resync_sent && now_us>=next_resync_us) { Backend::Packet request{}; const auto request_sequence=sequence++; if(!make_resync(packet,request_sequence,request))return false; resync_message_id=request_sequence; resync_packet=request; backend.rx.push_back(std::move(request)); delta=true; resync_sent=true; resync_started_us=now_us; next_resync_retry_us=now_us+250000U; resync_retries=0U; resync_ack_us=0U; recovery_snapshot_us=0U; ++resync_cycles; pending={}; } }
			else if(d.header.message_type==protocol::MessageType::Heartbeat) { Backend::Packet response{}; if(!make_heartbeat_response(packet,sequence++,now_us,response))return false; backend.rx.push_back(std::move(response)); }
			else if(d.header.message_type==protocol::MessageType::SessionEnd) { protocol::SessionEndPayload end{}; if(protocol::decode_session_end_payload(d.payload,end)!=protocol::ValidationError::None)return false; session_end_seen=true; session_end_reason=static_cast<std::uint8_t>(end.reason); }
			else if(d.header.message_type==protocol::MessageType::Ack && resync_sent) { protocol::AckPayload ack{}; if(protocol::decode_ack_payload(d.payload,ack)!=protocol::ValidationError::None)return false; if(ack.target_message_type==protocol::MessageType::ResyncRequest && ack.target_message_id==resync_message_id && ack.target_fragment_count==1U) { resync_ack=true; resync_ack_us=now_us; } else invalid_ack=true; }
		}
		return true;
	}
	bool retry_resync(Backend& backend, std::uint64_t now_us) noexcept {
		if(!resync_sent || resync_ack || now_us<next_resync_retry_us || resync_packet.data.empty()) return true;
		if(resync_retries>=8U) return true;
		Backend::Packet retry{}; if(!retransmit_reliable(resync_packet,sequence++,now_us,retry)) return false;
		backend.rx.push_back(std::move(retry)); ++resync_retries; next_resync_retry_us=now_us+(250000U<<std::min<std::uint32_t>(resync_retries,4U)); return true;
	}
};

// The soak mode samples owned runtime resources once per simulated minute.
// These are actual controller/transport values, not estimates derived from
// the peer transcript.
struct MinuteObservation {
	std::uint64_t minute=0U, client_count=0U, baselines=0U, sockets=0U, cache_entries=0U,
		preproof_accounts=0U, reassembly_bytes=0U, reliable=0U, queue_depth=0U, p99_tick_ns=0U;
};

struct LifecycleEvidence {
	bool mission_requested=false, mission_converged=false, process_requested=false, process_converged=false,
		normal_shutdown_released=false, process_shutdown_released=false;
	std::uint32_t mission_generation_before=1U, mission_generation_after=1U;
	std::uint64_t initial_session_id=0U, mission_session_id=0U, process_session_id=0U;
};

bool write_report(const char* path,const char* profile,const char* seed,const char* duration,const Backend& b,const Peer& peer,const std::vector<MinuteObservation>& observations,const LifecycleEvidence& lifecycle,bool complete) {
	std::ofstream out(path); if(!out)return false;
	const auto post_end_recovery_us = peer.first_live_post_impairment_us >= b.bounded_impairment_end_us && b.bounded_impairment_end_us != 0U ? peer.first_live_post_impairment_us-b.bounded_impairment_end_us : 0U;
	const auto recovered_within_ten_seconds_after_end = post_end_recovery_us != 0U && post_end_recovery_us <= 10000000U;
	MinuteObservation peaks{};
	for (const auto& observation : observations) {
		peaks.client_count=std::max(peaks.client_count,observation.client_count); peaks.baselines=std::max(peaks.baselines,observation.baselines);
		peaks.sockets=std::max(peaks.sockets,observation.sockets); peaks.cache_entries=std::max(peaks.cache_entries,observation.cache_entries);
		peaks.preproof_accounts=std::max(peaks.preproof_accounts,observation.preproof_accounts); peaks.reassembly_bytes=std::max(peaks.reassembly_bytes,observation.reassembly_bytes);
		peaks.reliable=std::max(peaks.reliable,observation.reliable); peaks.queue_depth=std::max(peaks.queue_depth,observation.queue_depth); peaks.p99_tick_ns=std::max(peaks.p99_tick_ns,observation.p99_tick_ns);
	}
	out << "{\n  \"schema\": \"fs2open.telemetry.phase1.reliability-profile.v1\",\n  \"status\": \"" << (complete?"passed":"failed") << "\",\n"
		<< "  \"profile\": \""<<profile<<"\",\n  \"seed\": "<<seed<<",\n  \"durationSeconds\": "<<duration<<",\n"
		<< "  \"configured\": {\"lossPercent\": "<<b.loss_percent<<", \"burst\": "<<(b.burst?"true":"false")<<"},\n"
		<< "  \"peer\": {\"welcome\": "<<(peer.welcome?"true":"false")<<", \"sessionBegin\": "<<(peer.begin?"true":"false")<<", \"snapshot\": "<<(peer.snapshot?"true":"false")<<", \"delta\": "<<(peer.delta?"true":"false")<<", \"resyncCycles\": "<<peer.resync_cycles<<", \"successfulRecoveries\": "<<peer.successful_recoveries<<", \"lastCyclePending\": "<<(peer.resync_sent?"true":"false")<<", \"lastCycleResyncAcked\": "<<(peer.resync_ack?"true":"false")<<", \"lastCycleSnapshotReceived\": "<<(peer.recovered_snapshot?"true":"false")<<", \"duplicateDelivered\": "<<(peer.duplicate_delivered?"true":"false")<<", \"reorderDelivered\": "<<(peer.reorder_delivered?"true":"false")<<", \"resyncSnapshotFlagSeen\": "<<(peer.resync_snapshot_flag_seen?"true":"false")<<", \"liveBaselinePublished\": "<<(peer.live_baseline_published?"true":"false")<<", \"recoveredBaselinePublished\": "<<(peer.recovered_baseline_published?"true":"false")<<", \"sessionEndSeen\": "<<(peer.session_end_seen?"true":"false")<<", \"sessionEndReason\": "<<static_cast<unsigned>(peer.session_end_reason)<<", \"invalidAck\": "<<(peer.invalid_ack?"true":"false")<<"},\n"
		<< "  \"injected\": {\"downstreamLoss\": "<<b.loss<<", \"deltaLoss\": "<<b.delta_loss<<", \"snapshotFragmentLoss\": "<<b.snapshot_fragment_loss<<", \"outageLoss\": "<<b.outage_loss<<", \"ackLoss\": "<<peer.ack_loss<<", \"duplicateFrames\": "<<b.duplicate_delivered<<", \"reorderedFrames\": "<<b.reorder_delivered<<", \"jitterDelayedFrames\": "<<b.jitter_delayed<<", \"wouldBlock\": "<<b.would_block_injected<<"},\n"
		<< "  \"slowClient\": {\"withheldReliableAcks\": "<<peer.slow_client_withheld_reliable_acks<<", \"recovered\": "<<(peer.slow_client_recovered?"true":"false")<<", \"boundedOutcome\": "<<((peer.slow_client_recovered && peer.recovered_baseline_published)?"true":"false")<<"},\n"
		<< "  \"convergence\": {\"cycles\": "<<peer.resync_cycles<<", \"successfulRecoveries\": "<<peer.successful_recoveries<<", \"continuousLossDiagnostic\": {\"maxLiveRecoveryUs\": "<<peer.max_convergence_us<<", \"worstTimelineUs\": {\"resyncSend\": "<<peer.worst_resync_start_us<<", \"ack\": "<<peer.worst_resync_ack_us<<", \"snapshot\": "<<peer.worst_snapshot_us<<", \"live\": "<<peer.worst_live_us<<"}}, \"boundedImpairment\": {\"startUs\": "<<b.bounded_impairment_start_us<<", \"endUs\": "<<b.bounded_impairment_end_us<<", \"firstLivePostEndUs\": "<<peer.first_live_post_impairment_us<<", \"recoveryAfterEndUs\": "<<post_end_recovery_us<<", \"withinTenSecondsPostEnd\": "<<(recovered_within_ten_seconds_after_end?"true":"false")<<"}},\n  \"lifecycle\": {\"missionRequested\": "<<(lifecycle.mission_requested?"true":"false")<<", \"missionGenerationBefore\": "<<lifecycle.mission_generation_before<<", \"missionGenerationAfter\": "<<lifecycle.mission_generation_after<<", \"missionSessionId\": "<<lifecycle.mission_session_id<<", \"missionConverged\": "<<(lifecycle.mission_converged?"true":"false")<<", \"processRequested\": "<<(lifecycle.process_requested?"true":"false")<<", \"initialSessionId\": "<<lifecycle.initial_session_id<<", \"processSessionId\": "<<lifecycle.process_session_id<<", \"processConverged\": "<<(lifecycle.process_converged?"true":"false")<<", \"normalShutdownReleased\": "<<(lifecycle.normal_shutdown_released?"true":"false")<<", \"processShutdownReleased\": "<<(lifecycle.process_shutdown_released?"true":"false")<<"},\n  \"minuteObservations\": [";
	for(std::size_t i=0U;i<observations.size();++i) { const auto& m=observations[i]; if(i!=0U)out<<','; out<<"{\"minute\":"<<m.minute<<",\"clients\":"<<m.client_count<<",\"baselines\":"<<m.baselines<<",\"sockets\":"<<m.sockets<<",\"cacheEntries\":"<<m.cache_entries<<",\"preproofAccounts\":"<<m.preproof_accounts<<",\"memoryBytes\":"<<m.reassembly_bytes<<",\"reliable\":"<<m.reliable<<",\"queueDepth\":"<<m.queue_depth<<",\"p99TickNs\":"<<m.p99_tick_ns<<"}"; }
	out << "],\n  \"resourcePeaks\": {\"clients\":"<<peaks.client_count<<",\"baselines\":"<<peaks.baselines<<",\"sockets\":"<<peaks.sockets<<",\"cacheEntries\":"<<peaks.cache_entries<<",\"preproofAccounts\":"<<peaks.preproof_accounts<<",\"memoryBytes\":"<<peaks.reassembly_bytes<<",\"reliable\":"<<peaks.reliable<<",\"queueDepth\":"<<peaks.queue_depth<<",\"p99TickNs\":"<<peaks.p99_tick_ns<<"}\n}\n";
	return static_cast<bool>(out);
}

// Reuses the real native runtime, UDP backend, wire peer, and snapshot ACK
// path after a lifecycle discontinuity. It is intentionally test-only and
// proves a newly negotiated live baseline rather than merely reconstructing
// a local fixture state.
bool converge_after_discontinuity(detail::NativeSessionRuntime& runtime, Backend& backend, const Engine& engine,
	Peer& peer, const protocol::EndpointKey& endpoint, std::uint64_t& now, std::uint32_t mission_generation) noexcept
{
	backend.reset_peer_io();
	Backend::Packet hello{};
	if (!make_hello(endpoint,hello)) return false;
	backend.rx.push_back(std::move(hello));
	for (std::uint64_t index=0U; index<256U; ++index) {
		backend.now_us=now;
		if (index<32U ? !r2_tick(runtime,now,mission_generation) : !tick(runtime,engine,now,mission_generation)) return false;
		if (!peer.consume(backend,now) || !peer.retry_resync(backend,now)) return false;
		const auto* slot=detail::NativeSessionRuntimeTestAccess::slot(runtime,0U);
		if (slot!=nullptr && slot->snapshot.has_active_baseline()) {
			peer.live_baseline_published=true;
			return peer.welcome && peer.begin && peer.snapshot && peer.session_id!=0U;
		}
		now+=33334U;
	}
	return false;
}
}

int main(int argc,char** argv) {
	const char *profile=nullptr,*seed=nullptr,*duration=nullptr,*report=nullptr;
	bool restart_mission=false,restart_process=false;
	for(int index=1;index<argc;++index) {
		if(!std::strcmp(argv[index],"--restart-mission")) { restart_mission=true; continue; }
		if(!std::strcmp(argv[index],"--restart-process")) { restart_process=true; continue; }
		if(index+1>=argc)return 2;
		if(!std::strcmp(argv[index],"--profile"))profile=argv[++index]; else if(!std::strcmp(argv[index],"--seed"))seed=argv[++index]; else if(!std::strcmp(argv[index],"--duration-seconds"))duration=argv[++index]; else if(!std::strcmp(argv[index],"--report"))report=argv[++index]; else return 2;
	}
	if(!profile||!seed||!duration||!report)return 2;
	char* duration_end=nullptr; const auto duration_seconds=std::strtoull(duration,&duration_end,10);
	if(duration_end==duration || *duration_end!='\0' || duration_seconds<60U || duration_seconds>3600U) return 2;
	Backend backend; if(!valid_profile(profile,backend))return 2; Completion completion; FixedRandom ids_random,packet_random; detail::SessionIdRegistry registry; if(!registry.allocate_storage())return 3; detail::SessionIdAllocator ids(ids_random,registry);
	telemetry::TelemetryConfig config; config.enabled=true; config.bind_addresses.clear(); config.bind_addresses.add(telemetry::NumericIpAddress::from_ipv4({127U,0U,0U,1U}));
	detail::NativeSessionRuntime runtime(backend,completion); if(runtime.start({&config,0x1020304050607080ULL,&ids,&packet_random})!=detail::NativeSessionStartStatus::Started)return 3;
	const auto endpoint=protocol::EndpointKey::from_ipv4({127U,0U,0U,2U},7808U); Backend::Packet hello{}; if(!make_hello(endpoint,hello))return 3; backend.rx.push_back(std::move(hello));
	Engine engine; Peer peer; std::uint64_t now=1000000U, next_observation_us=60000000U; std::vector<MinuteObservation> observations; std::vector<std::uint64_t> minute_ticks;
	const auto total_ticks=duration_seconds*30U;
	for(std::uint64_t tick_index=0U;tick_index<total_ticks;++tick_index) {
		backend.now_us=now;
		backend.observe_bounded_impairment_window();
		if(peer.snapshot) engine.position_x += .01F;
		const auto started=std::chrono::steady_clock::now();
		if(tick_index<32U ? !r2_tick(runtime,now) : !tick(runtime,engine,now)) { std::cerr<<"runtime tick failed at "<<tick_index<<"\n"; runtime.shutdown(); return 4; }
		minute_ticks.push_back(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count()));
		if(!peer.consume(backend,now)) { std::cerr<<"peer decode failed at "<<tick_index<<"\n"; runtime.shutdown(); return 4; }
		if(!peer.retry_resync(backend,now)) { std::cerr<<"resync retry encode failed at "<<tick_index<<"\n"; runtime.shutdown(); return 4; }
		const auto* slot=detail::NativeSessionRuntimeTestAccess::slot(runtime,0U);
		if(slot != nullptr && slot->snapshot.has_active_baseline()) {
			if(!peer.live_baseline_published) { peer.live_baseline_published=true; peer.initial_snapshot_id=slot->snapshot.active_snapshot_id(); peer.next_resync_us=now; }
			if(peer.recovered_snapshot && peer.resync_sent && peer.initial_snapshot_id != 0U && slot->snapshot.active_snapshot_id()!=peer.initial_snapshot_id) {
				peer.recovered_baseline_published=true; ++peer.successful_recoveries; const auto convergence=now-peer.resync_started_us; if(convergence>peer.max_convergence_us) { peer.max_convergence_us=convergence; peer.worst_resync_start_us=peer.resync_started_us; peer.worst_resync_ack_us=peer.resync_ack_us; peer.worst_snapshot_us=peer.recovery_snapshot_us; peer.worst_live_us=now; }
				if(peer.first_live_post_impairment_us==0U && backend.bounded_impairment_end_us!=0U && now>=backend.bounded_impairment_end_us) peer.first_live_post_impairment_us=now;
				peer.initial_snapshot_id=slot->snapshot.active_snapshot_id(); peer.resync_sent=false; peer.resync_ack=false; peer.recovered_snapshot=false; peer.next_resync_us=now+5000000U;
			}
		}
		if(peer.delta_observed) backend.impairments_active=true;
		if(peer.resync_sent && !peer.recovery_loss_armed) { backend.recovery_losses_remaining=1U; peer.recovery_loss_armed=true; }
		if(now>=next_observation_us) {
			std::sort(minute_ticks.begin(),minute_ticks.end()); const auto p99=minute_ticks.empty()?0U:minute_ticks[(minute_ticks.size()-1U)*99U/100U];
			const auto usage=runtime.owned_usage(); MinuteObservation observation{}; observation.minute=observations.size()+1U; observation.client_count=runtime.active_sessions(); observation.baselines=(slot!=nullptr&&slot->snapshot.has_active_baseline())?1U:0U; observation.sockets=runtime.socket_count(); observation.cache_entries=usage.cache_entries; observation.preproof_accounts=usage.preproof_accounts; observation.reassembly_bytes=usage.reassembly_bytes; observation.reliable=usage.reliable_items; observation.queue_depth=usage.output_queued?1U:0U; observation.p99_tick_ns=p99; observations.push_back(observation); minute_ticks.clear(); next_observation_us+=60000000U;
		}
		now += 33334U;
	}
	const auto* final_slot=detail::NativeSessionRuntimeTestAccess::slot(runtime,0U);
	const bool final_live=runtime.active_sessions()==1U && final_slot!=nullptr && final_slot->snapshot.has_active_baseline();
	const auto post_end_recovery_us = peer.first_live_post_impairment_us >= backend.bounded_impairment_end_us && backend.bounded_impairment_end_us != 0U ? peer.first_live_post_impairment_us-backend.bounded_impairment_end_us : 0U;
	const bool recovered_within_ten_seconds_after_end = post_end_recovery_us != 0U && post_end_recovery_us<=10000000U;
	LifecycleEvidence lifecycle{}; lifecycle.initial_session_id=peer.session_id; lifecycle.mission_requested=restart_mission; lifecycle.process_requested=restart_process;
	if (restart_mission) {
		lifecycle.mission_generation_before=1U; lifecycle.mission_generation_after=2U;
		runtime.purge_all(detail::SessionCloseReason::MissionDiscontinuity);
		Peer mission_peer{};
		lifecycle.mission_converged=converge_after_discontinuity(runtime,backend,engine,mission_peer,endpoint,now,2U) && mission_peer.session_id!=peer.session_id;
		lifecycle.mission_session_id=mission_peer.session_id;
	}
	runtime.shutdown();
	lifecycle.normal_shutdown_released=runtime.socket_count()==0U && runtime.owned_usage()==detail::SessionControllerOwnedUsage{};
	if (restart_process) {
		Backend restarted_backend{}; Completion restarted_completion{}; FixedRandom restarted_ids_random{},restarted_packet_random{};
		restarted_ids_random.value=0x1000U; restarted_packet_random.value=0x2000U;
		detail::SessionIdRegistry restarted_registry; detail::SessionIdAllocator restarted_ids(restarted_ids_random,restarted_registry);
		if (restarted_registry.allocate_storage()) {
			detail::NativeSessionRuntime restarted_runtime(restarted_backend,restarted_completion);
			if (restarted_runtime.start({&config,0x1020304050607080ULL,&restarted_ids,&restarted_packet_random})==detail::NativeSessionStartStatus::Started) {
				Peer process_peer{};
				lifecycle.process_converged=converge_after_discontinuity(restarted_runtime,restarted_backend,engine,process_peer,endpoint,now,1U) && process_peer.session_id!=peer.session_id;
				lifecycle.process_session_id=process_peer.session_id;
				restarted_runtime.shutdown();
				lifecycle.process_shutdown_released=restarted_runtime.socket_count()==0U && restarted_runtime.owned_usage()==detail::SessionControllerOwnedUsage{};
			}
		}
	}
	const bool complete=peer.welcome&&peer.begin&&peer.snapshot&&peer.delta&&peer.resync_cycles!=0U&&peer.successful_recoveries!=0U&&peer.duplicate_delivered&&peer.reorder_delivered&&peer.resync_snapshot_flag_seen&&peer.live_baseline_published&&peer.recovered_baseline_published&&peer.slow_client_withheld_reliable_acks!=0U&&peer.slow_client_recovered&&!peer.invalid_ack&&backend.loss!=0U&&backend.delta_loss!=0U&&backend.snapshot_fragment_loss!=0U&&backend.outage_loss!=0U&&peer.ack_loss!=0U&&backend.jitter_delayed!=0U&&backend.would_block_injected!=0U&&backend.bounded_impairment_start_us!=0U&&backend.bounded_impairment_end_us>backend.bounded_impairment_start_us&&recovered_within_ten_seconds_after_end&&observations.size()==duration_seconds/60U&&final_live&&lifecycle.normal_shutdown_released&&(!restart_mission||lifecycle.mission_converged)&&(!restart_process||(lifecycle.process_converged&&lifecycle.process_shutdown_released));
	return write_report(report,profile,seed,duration,backend,peer,observations,lifecycle,complete) ? (complete?0:4) : 5;
}
