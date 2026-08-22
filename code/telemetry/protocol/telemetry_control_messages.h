#pragma once

#include "telemetry/protocol/telemetry_protocol_constants.h"
#include "telemetry/protocol/telemetry_protocol_types.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace telemetry::protocol {

constexpr std::size_t CapabilityExtensionHeaderSize = 6;
constexpr std::size_t MaxControlExtensionsSize = 768;
constexpr std::uint16_t MaxControlExtensionCount = 128;

constexpr std::size_t DiscoveryPayloadPrefixSize = 28;
constexpr std::size_t MaximumDiscoveryProducerNameSize = 64;
constexpr std::size_t MaximumDiscoveryPayloadSize =
	DiscoveryPayloadPrefixSize + MaximumDiscoveryProducerNameSize;
constexpr std::size_t HelloPayloadPrefixSize = 38;
constexpr std::size_t WelcomePayloadPrefixSize = 68;
constexpr std::size_t SessionBeginPayloadSize = 28;
constexpr std::size_t HeartbeatPayloadSize = 32;
constexpr std::size_t CapabilityUpdatePayloadSize = 36;

constexpr std::uint16_t MinHeartbeatIntervalMs = 200;
constexpr std::uint16_t MaxHeartbeatIntervalMs = 5000;
constexpr std::uint16_t ReliableReassemblyTimeoutV1Ms = 2000;

ValidationError validate_protocol_minor_range(ProtocolMinorRange range) noexcept;

// Logical view of one common capability-extension envelope. Unknown non-zero
// types and versions are structurally valid here; their optional semantics are
// handled only by a negotiated specialized-capability codec.
struct CapabilityExtensionView {
	std::uint16_t type = 0;
	std::uint8_t version = 0;
	std::uint8_t flags = 0;
	ByteView payload;
};

// Iterates a declared extension region without allocation. It enforces the
// common v1.0 envelope rules, including count, exact length, reserved flags and
// type uniqueness. Unknown types are returned as ordinary items so a caller
// can skip them without interpreting their payload.
class CapabilityExtensionIterator {
  public:
	CapabilityExtensionIterator(ByteView extensions, std::uint16_t extension_count) noexcept;

	// On success, has_value is true for an item and false at the exact end.
	// extension and has_value are reset before any error is reported.
	ValidationError next(CapabilityExtensionView& extension, bool& has_value) noexcept;

	ValidationError error() const noexcept
	{
		return m_error;
	}
	std::uint16_t consumed_count() const noexcept
	{
		return m_index;
	}
	std::size_t consumed_bytes() const noexcept
	{
		return m_offset;
	}

  private:
	ByteView m_extensions;
	std::array<std::uint16_t, MaxControlExtensionCount> m_seen_types{};
	std::size_t m_offset = 0;
	std::uint16_t m_extension_count = 0;
	std::uint16_t m_index = 0;
	ValidationError m_error = ValidationError::None;
};

ValidationError validate_capability_extensions(ByteView extensions, std::uint16_t extension_count) noexcept;
// Encoding accepts only extension types defined by the current wire contract.
ValidationError encode_capability_extension(const CapabilityExtensionView& extension,
	MutableByteView output,
	std::size_t& written) noexcept;

// Capability bitmaps are explicitly extensible. Decoders retain the received
// raw bitmap for diagnostics/deduplication; consumers use this helper before
// interpreting it. Encoders reject unknown bits so they are never relayed.
constexpr std::uint64_t known_capabilities(std::uint64_t capabilities) noexcept
{
	return capabilities & KnownCapabilities;
}

struct DiscoveryPayload {
	std::uint64_t producer_id = 0;
	std::uint16_t listen_port = 0;
	std::uint8_t min_major = VersionMajor;
	std::uint8_t max_major = VersionMajor;
	std::uint8_t min_minor = VersionMinor;
	std::uint8_t max_minor = VersionMinor;
	std::uint64_t producer_capabilities = 0;
	std::uint32_t advert_sequence = 0;
	ByteView producer_name;
};

struct HelloPayload {
	std::uint64_t client_nonce = 0;
	std::uint64_t client_send_t0_us = 0;
	std::uint8_t min_major = VersionMajor;
	std::uint8_t max_major = VersionMajor;
	std::uint8_t min_minor = VersionMinor;
	std::uint8_t max_minor = VersionMinor;
	VisibilityMode requested_visibility_mode = VisibilityMode::Cockpit;
	std::uint64_t advertised_capabilities = 0;
	std::uint16_t requested_heartbeat_ms = 0;
	std::uint16_t extension_count = 0;
	ByteView extensions;
};

struct WelcomePayload {
	std::uint64_t client_nonce = 0;
	std::uint64_t client_send_t0_us = 0;
	std::uint64_t producer_receive_t1_us = 0;
	std::uint64_t producer_send_t2_us = 0;
	WelcomeStatus status = WelcomeStatus::Accepted;
	std::uint8_t selected_major = 0;
	std::uint8_t selected_minor = 0;
	VisibilityMode selected_visibility_mode = VisibilityMode::Cockpit;
	std::uint64_t producer_capabilities = 0;
	std::uint64_t active_capabilities = 0;
	std::uint16_t heartbeat_interval_ms = 0;
	std::uint16_t reliable_reassembly_timeout_ms = 0;
	std::uint16_t extension_count = 0;
	std::uint64_t producer_id = 0;
	ByteView extensions;
};

struct SessionBeginPayload {
	std::uint32_t session_flags = SessionBeginFlagNone;
	std::uint64_t producer_session_start_us = 0;
	std::uint64_t mission_instance_id = 0;
	std::uint32_t initial_snapshot_id = 0;
	std::uint32_t required_manifest_id = 0;
};

struct HeartbeatPayload {
	std::uint32_t probe_id = 0;
	HeartbeatKind kind = HeartbeatKind::Request;
	std::uint64_t origin_t0_us = 0;
	std::uint64_t receive_t1_us = 0;
	std::uint64_t transmit_t2_us = 0;
};

struct CapabilityUpdatePayload {
	std::uint32_t capability_generation = 0;
	std::uint64_t advertised_capabilities = 0;
	std::uint64_t active_capabilities = 0;
	std::uint64_t effective_time_us = 0;
	CapabilityUpdateReason reason = CapabilityUpdateReason::Invalid;
};

ValidationError validate_discovery_payload(const DiscoveryPayload& payload) noexcept;
ValidationError validate_hello_payload(const HelloPayload& payload) noexcept;
ValidationError validate_welcome_payload(const WelcomePayload& payload) noexcept;
ValidationError validate_session_begin_payload(const SessionBeginPayload& payload) noexcept;
ValidationError validate_heartbeat_payload(const HeartbeatPayload& payload) noexcept;
ValidationError validate_capability_update_payload(const CapabilityUpdatePayload& payload) noexcept;

ValidationError
encode_discovery_payload(const DiscoveryPayload& payload, MutableByteView output, std::size_t& written) noexcept;
ValidationError decode_discovery_payload(ByteView input, DiscoveryPayload& payload) noexcept;

ValidationError
encode_hello_payload(const HelloPayload& payload, MutableByteView output, std::size_t& written) noexcept;
ValidationError decode_hello_payload(ByteView input, HelloPayload& payload) noexcept;

ValidationError
encode_welcome_payload(const WelcomePayload& payload, MutableByteView output, std::size_t& written) noexcept;
ValidationError decode_welcome_payload(ByteView input, WelcomePayload& payload) noexcept;

ValidationError
encode_session_begin_payload(const SessionBeginPayload& payload, MutableByteView output, std::size_t& written) noexcept;
ValidationError decode_session_begin_payload(ByteView input, SessionBeginPayload& payload) noexcept;

ValidationError
encode_heartbeat_payload(const HeartbeatPayload& payload, MutableByteView output, std::size_t& written) noexcept;
ValidationError decode_heartbeat_payload(ByteView input, HeartbeatPayload& payload) noexcept;

ValidationError encode_capability_update_payload(const CapabilityUpdatePayload& payload,
	MutableByteView output,
	std::size_t& written) noexcept;
ValidationError decode_capability_update_payload(ByteView input, CapabilityUpdatePayload& payload) noexcept;

} // namespace telemetry::protocol
