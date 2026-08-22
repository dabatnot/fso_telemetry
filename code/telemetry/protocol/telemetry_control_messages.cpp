#include "telemetry/protocol/telemetry_control_messages.h"

#include "telemetry/protocol/packet_reader.h"
#include "telemetry/protocol/packet_writer.h"

#include <array>
#include <cstring>
#include <string_view>

namespace telemetry::protocol {

namespace {

constexpr std::uint64_t CommCapabilityPair = static_cast<std::uint64_t>(CapabilityCommViewLocalAssets) |
											 static_cast<std::uint64_t>(CapabilityCommViewAuthoritativeSource);
constexpr std::uint64_t VideoCapabilityPair = static_cast<std::uint64_t>(CapabilityTargetVideoH264) |
											  static_cast<std::uint64_t>(CapabilityTargetVideoRemoteRender);
constexpr std::uint64_t ClientOwnedCapabilityMask = static_cast<std::uint64_t>(CapabilityCommViewLocalAssets) |
													static_cast<std::uint64_t>(CapabilityTargetVideoH264) |
													static_cast<std::uint64_t>(CapabilityUpdate);
constexpr std::uint64_t ProducerOwnedCapabilityMask =
	static_cast<std::uint64_t>(CapabilityCommViewAuthoritativeSource) |
	static_cast<std::uint64_t>(CapabilityTargetVideoRemoteRender) | static_cast<std::uint64_t>(CapabilityUpdate);

bool is_known_visibility_mode(VisibilityMode mode) noexcept
{
	return mode == VisibilityMode::Cockpit;
}

bool active_capability_pairs_are_complete(std::uint64_t capabilities) noexcept
{
	const auto known = known_capabilities(capabilities);
	const auto comm = known & CommCapabilityPair;
	const auto video = known & VideoCapabilityPair;
	return (comm == 0 || comm == CommCapabilityPair) && (video == 0 || video == VideoCapabilityPair);
}

ValidationError validate_emitted_capabilities(std::uint64_t capabilities) noexcept
{
	return (capabilities & ~KnownCapabilities) == 0 ? ValidationError::None : ValidationError::ReservedFlag;
}

ValidationError validate_emitted_capability_extensions(ByteView extensions, std::uint16_t extension_count) noexcept
{
	CapabilityExtensionIterator iterator(extensions, extension_count);
	for (;;) {
		CapabilityExtensionView extension;
		bool has_value = false;
		if (const auto error = iterator.next(extension, has_value); error != ValidationError::None) {
			return error;
		}
		if (!has_value) {
			return ValidationError::None;
		}
		if (!is_emittable_capability_extension_type(static_cast<CapabilityExtensionType>(extension.type))) {
			return ValidationError::OutOfRange;
		}
	}
}

ValidationError validate_input_size(ByteView input, std::size_t expected_size) noexcept
{
	if (input.size != 0 && input.data == nullptr) {
		return ValidationError::TruncatedPayload;
	}
	if (input.size < expected_size) {
		return ValidationError::TruncatedPayload;
	}
	if (input.size > expected_size) {
		return ValidationError::TrailingBytes;
	}
	return ValidationError::None;
}

template <std::size_t PrefixSize>
ValidationError publish_encoded_payload(const std::array<std::uint8_t, PrefixSize>& prefix,
	ByteView suffix,
	MutableByteView output,
	std::size_t& written) noexcept
{
	const auto total_size = PrefixSize + suffix.size;
	if (output.data == nullptr || output.size < total_size) {
		return ValidationError::InternalSerializationError;
	}

	// Moving the suffix before the stack-built prefix also makes in-place and
	// partially overlapping encodes well-defined.
	if (suffix.size != 0) {
		std::memmove(output.data + PrefixSize, suffix.data, suffix.size);
	}
	std::memcpy(output.data, prefix.data(), prefix.size());
	written = total_size;
	return ValidationError::None;
}

template <std::size_t PrefixSize>
ValidationError finish_prefix(PacketWriter& writer) noexcept
{
	return writer.ok() && writer.size() == PrefixSize ? ValidationError::None
													  : ValidationError::InternalSerializationError;
}

} // namespace

ValidationError validate_protocol_minor_range(ProtocolMinorRange range) noexcept
{
	if (!is_supported_version_minor(range.minimum) || !is_supported_version_minor(range.maximum) ||
		range.minimum > range.maximum) {
		return ValidationError::UnsupportedMinor;
	}
	return ValidationError::None;
}

ValidationError validate_discovery_payload(const DiscoveryPayload& payload) noexcept
{
	if (payload.producer_id == 0 || payload.listen_port == 0) {
		return ValidationError::OutOfRange;
	}
	if (payload.min_major != VersionMajor || payload.max_major != VersionMajor) {
		return ValidationError::UnsupportedMajor;
	}
	if (const auto error = validate_protocol_minor_range({payload.min_minor, payload.max_minor});
		error != ValidationError::None) {
		return error;
	}
	if (payload.producer_name.size > MaximumDiscoveryProducerNameSize) {
		return ValidationError::StringTooLong;
	}
	if (payload.producer_name.size != 0 && payload.producer_name.data == nullptr) {
		return ValidationError::TruncatedPayload;
	}
	const auto producer_name = payload.producer_name.empty()
							   ? std::string_view{}
							   : std::string_view(static_cast<const char*>(static_cast<const void*>(payload.producer_name.data)),
									 payload.producer_name.size);
	return is_valid_utf8(producer_name) ? ValidationError::None : ValidationError::InvalidUtf8;
}

ValidationError
encode_discovery_payload(const DiscoveryPayload& payload, MutableByteView output, std::size_t& written) noexcept
{
	written = 0;
	if (const auto error = validate_discovery_payload(payload); error != ValidationError::None) {
		return error;
	}
	if (const auto error = validate_emitted_capabilities(payload.producer_capabilities);
		error != ValidationError::None) {
		return error;
	}
	if ((payload.producer_capabilities & ~ProducerOwnedCapabilityMask) != 0) {
		return ValidationError::CapabilityNotNegotiated;
	}

	std::array<std::uint8_t, DiscoveryPayloadPrefixSize> prefix{};
	PacketWriter writer(MutableByteView{prefix.data(), prefix.size()});
	const bool ok = writer.write_u64(payload.producer_id) && writer.write_u16(payload.listen_port) &&
					writer.write_u8(payload.min_major) && writer.write_u8(payload.max_major) &&
					writer.write_u8(payload.min_minor) && writer.write_u8(payload.max_minor) &&
					writer.write_u64(payload.producer_capabilities) && writer.write_u32(payload.advert_sequence) &&
					writer.write_u16(static_cast<std::uint16_t>(payload.producer_name.size));
	if (!ok || finish_prefix<DiscoveryPayloadPrefixSize>(writer) != ValidationError::None) {
		return ValidationError::InternalSerializationError;
	}
	return publish_encoded_payload(prefix, payload.producer_name, output, written);
}

ValidationError decode_discovery_payload(ByteView input, DiscoveryPayload& payload) noexcept
{
	payload = DiscoveryPayload{};
	if (input.size != 0 && input.data == nullptr) {
		return ValidationError::TruncatedPayload;
	}
	if (input.size < DiscoveryPayloadPrefixSize) {
		return ValidationError::TruncatedPayload;
	}
	if (input.size > MaximumDiscoveryPayloadSize) {
		return ValidationError::StringTooLong;
	}

	DiscoveryPayload candidate;
	std::uint16_t producer_name_length = 0;
	PacketReader reader(ByteView{input.data, DiscoveryPayloadPrefixSize});
	const bool ok = reader.read_u64(candidate.producer_id) && reader.read_u16(candidate.listen_port) &&
					reader.read_u8(candidate.min_major) && reader.read_u8(candidate.max_major) &&
					reader.read_u8(candidate.min_minor) && reader.read_u8(candidate.max_minor) &&
					reader.read_u64(candidate.producer_capabilities) && reader.read_u32(candidate.advert_sequence) &&
					reader.read_u16(producer_name_length);
	if (!ok || !reader.at_end()) {
		return ValidationError::TruncatedPayload;
	}
	if (producer_name_length > MaximumDiscoveryProducerNameSize) {
		return ValidationError::StringTooLong;
	}
	const auto expected_size = DiscoveryPayloadPrefixSize + static_cast<std::size_t>(producer_name_length);
	if (input.size < expected_size) {
		return ValidationError::TruncatedPayload;
	}
	if (input.size > expected_size) {
		return ValidationError::TrailingBytes;
	}
	candidate.producer_name = producer_name_length == 0
								  ? ByteView{}
								  : ByteView{input.data + DiscoveryPayloadPrefixSize, producer_name_length};
	if (const auto error = validate_discovery_payload(candidate); error != ValidationError::None) {
		return error;
	}
	payload = candidate;
	return ValidationError::None;
}

CapabilityExtensionIterator::CapabilityExtensionIterator(ByteView extensions, std::uint16_t extension_count) noexcept
	: m_extensions(extensions), m_extension_count(extension_count)
{
	if (extensions.size != 0 && extensions.data == nullptr) {
		m_error = ValidationError::TruncatedPayload;
	} else if (extensions.size > MaxControlExtensionsSize || extension_count > MaxControlExtensionCount) {
		m_error = ValidationError::OutOfRange;
	}
}

ValidationError CapabilityExtensionIterator::next(CapabilityExtensionView& extension, bool& has_value) noexcept
{
	extension = CapabilityExtensionView{};
	has_value = false;
	if (m_error != ValidationError::None) {
		return m_error;
	}

	if (m_index == m_extension_count) {
		if (m_offset != m_extensions.size) {
			m_error = ValidationError::TrailingBytes;
		}
		return m_error;
	}

	const auto remaining = m_extensions.subview(m_offset, m_extensions.size - m_offset);
	PacketReader reader(remaining);
	CapabilityExtensionView candidate;
	std::uint16_t payload_length = 0;
	if (!reader.read_u16(candidate.type) || !reader.read_u8(candidate.version) || !reader.read_u8(candidate.flags) ||
		!reader.read_u16(payload_length) || !reader.read_bytes(payload_length, candidate.payload)) {
		m_error = ValidationError::TruncatedPayload;
		return m_error;
	}
	if (candidate.type == 0) {
		m_error = ValidationError::OutOfRange;
		return m_error;
	}
	if (candidate.flags != 0) {
		m_error = ValidationError::ReservedFlag;
		return m_error;
	}
	for (std::uint16_t i = 0; i < m_index; ++i) {
		if (m_seen_types[i] == candidate.type) {
			m_error = ValidationError::DuplicateItemKey;
			return m_error;
		}
	}

	m_seen_types[m_index] = candidate.type;
	++m_index;
	m_offset += reader.consumed();
	extension = candidate;
	has_value = true;
	return ValidationError::None;
}

ValidationError validate_capability_extensions(ByteView extensions, std::uint16_t extension_count) noexcept
{
	CapabilityExtensionIterator iterator(extensions, extension_count);
	for (;;) {
		CapabilityExtensionView extension;
		bool has_value = false;
		if (const auto error = iterator.next(extension, has_value); error != ValidationError::None) {
			return error;
		}
		if (!has_value) {
			return ValidationError::None;
		}
	}
}

ValidationError encode_capability_extension(const CapabilityExtensionView& extension,
	MutableByteView output,
	std::size_t& written) noexcept
{
	written = 0;
	if (extension.type == 0) {
		return ValidationError::OutOfRange;
	}
	if (!is_emittable_capability_extension_type(static_cast<CapabilityExtensionType>(extension.type))) {
		return ValidationError::OutOfRange;
	}
	if (extension.flags != 0) {
		return ValidationError::ReservedFlag;
	}
	if ((extension.payload.size != 0 && extension.payload.data == nullptr) ||
		extension.payload.size > MaxControlExtensionsSize - CapabilityExtensionHeaderSize) {
		return extension.payload.size > MaxControlExtensionsSize - CapabilityExtensionHeaderSize
				   ? ValidationError::OutOfRange
				   : ValidationError::TruncatedPayload;
	}

	std::array<std::uint8_t, CapabilityExtensionHeaderSize> prefix{};
	PacketWriter writer(MutableByteView{prefix.data(), prefix.size()});
	const bool ok = writer.write_u16(extension.type) && writer.write_u8(extension.version) &&
					writer.write_u8(extension.flags) &&
					writer.write_u16(static_cast<std::uint16_t>(extension.payload.size));
	if (!ok || finish_prefix<CapabilityExtensionHeaderSize>(writer) != ValidationError::None) {
		return ValidationError::InternalSerializationError;
	}
	return publish_encoded_payload(prefix, extension.payload, output, written);
}

ValidationError validate_hello_payload(const HelloPayload& payload) noexcept
{
	if (payload.client_nonce == 0) {
		return ValidationError::OutOfRange;
	}
	if (payload.min_major != VersionMajor || payload.max_major != VersionMajor) {
		return ValidationError::UnsupportedMajor;
	}
	if (const auto error = validate_protocol_minor_range({payload.min_minor, payload.max_minor});
		error != ValidationError::None) {
		return error;
	}
	if (!is_known_visibility_mode(payload.requested_visibility_mode)) {
		return ValidationError::UnknownEnum;
	}
	if (payload.requested_heartbeat_ms < MinHeartbeatIntervalMs ||
		payload.requested_heartbeat_ms > MaxHeartbeatIntervalMs) {
		return ValidationError::OutOfRange;
	}
	return validate_capability_extensions(payload.extensions, payload.extension_count);
}

ValidationError validate_welcome_payload(const WelcomePayload& payload) noexcept
{
	if (payload.client_nonce == 0 || payload.producer_id == 0) {
		return ValidationError::OutOfRange;
	}
	if (payload.producer_send_t2_us < payload.producer_receive_t1_us) {
		return ValidationError::OutOfRange;
	}

	switch (payload.status) {
	case WelcomeStatus::Accepted:
		if (payload.selected_major != VersionMajor) {
			return ValidationError::UnsupportedMajor;
		}
		if (!is_supported_version_minor(payload.selected_minor)) {
			return ValidationError::UnsupportedMinor;
		}
		if (!is_known_visibility_mode(payload.selected_visibility_mode)) {
			return ValidationError::UnknownEnum;
		}
		if (payload.heartbeat_interval_ms < MinHeartbeatIntervalMs ||
			payload.heartbeat_interval_ms > MaxHeartbeatIntervalMs ||
			payload.reliable_reassembly_timeout_ms != ReliableReassemblyTimeoutV1Ms) {
			return ValidationError::OutOfRange;
		}
		if (!active_capability_pairs_are_complete(payload.active_capabilities)) {
			return ValidationError::InvalidStateTransition;
		}
		return validate_capability_extensions(payload.extensions, payload.extension_count);

	case WelcomeStatus::Unauthorized:
	case WelcomeStatus::Busy:
	case WelcomeStatus::InvalidCapabilities:
		if (payload.selected_major != 0 || payload.selected_minor != 0 ||
			payload.selected_visibility_mode != VisibilityMode::Cockpit || payload.producer_capabilities != 0 ||
			payload.active_capabilities != 0 || payload.heartbeat_interval_ms != 0 ||
			payload.reliable_reassembly_timeout_ms != 0 || payload.extension_count != 0 ||
			!payload.extensions.empty()) {
			return ValidationError::InvalidStateTransition;
		}
		return ValidationError::None;

	default:
		return ValidationError::UnknownEnum;
	}
}

ValidationError validate_session_begin_payload(const SessionBeginPayload& payload) noexcept
{
	if ((payload.session_flags & ReservedSessionBeginFlags) != 0) {
		return ValidationError::ReservedFlag;
	}
	if ((payload.session_flags & SessionBeginFlagReadOnly) == 0) {
		return ValidationError::InvalidAbsence;
	}
	const bool mission_active = (payload.session_flags & SessionBeginFlagMissionActive) != 0;
	const bool manifest_required = (payload.session_flags & SessionBeginFlagManifestRequired) != 0;
	if (mission_active != (payload.mission_instance_id != 0) ||
		manifest_required != (payload.required_manifest_id != 0)) {
		return ValidationError::InvalidStateTransition;
	}
	return ValidationError::None;
}

ValidationError validate_heartbeat_payload(const HeartbeatPayload& payload) noexcept
{
	if (payload.probe_id == 0) {
		return ValidationError::OutOfRange;
	}
	switch (payload.kind) {
	case HeartbeatKind::Request:
		return payload.receive_t1_us == 0 && payload.transmit_t2_us == 0 ? ValidationError::None
																		 : ValidationError::InvalidStateTransition;
	case HeartbeatKind::Response:
		return payload.transmit_t2_us >= payload.receive_t1_us ? ValidationError::None : ValidationError::OutOfRange;
	default:
		return ValidationError::UnknownEnum;
	}
}

ValidationError validate_capability_update_payload(const CapabilityUpdatePayload& payload) noexcept
{
	if (payload.capability_generation == 0) {
		return ValidationError::OutOfRange;
	}
	switch (payload.reason) {
	case CapabilityUpdateReason::RuntimeAvailability:
	case CapabilityUpdateReason::PeerRequest:
	case CapabilityUpdateReason::ConfigurationChange:
	case CapabilityUpdateReason::ErrorRecovery:
		break;
	default:
		return ValidationError::UnknownEnum;
	}
	return active_capability_pairs_are_complete(payload.active_capabilities) ? ValidationError::None
																			 : ValidationError::InvalidStateTransition;
}

ValidationError encode_hello_payload(const HelloPayload& payload, MutableByteView output, std::size_t& written) noexcept
{
	written = 0;
	if (const auto error = validate_hello_payload(payload); error != ValidationError::None) {
		return error;
	}
	if (const auto error = validate_emitted_capabilities(payload.advertised_capabilities);
		error != ValidationError::None) {
		return error;
	}
	if ((payload.advertised_capabilities & ~ClientOwnedCapabilityMask) != 0) {
		return ValidationError::CapabilityNotNegotiated;
	}
	if (const auto error = validate_emitted_capability_extensions(payload.extensions, payload.extension_count);
		error != ValidationError::None) {
		return error;
	}

	std::array<std::uint8_t, HelloPayloadPrefixSize> prefix{};
	PacketWriter writer(MutableByteView{prefix.data(), prefix.size()});
	const bool ok = writer.write_u64(payload.client_nonce) && writer.write_u64(payload.client_send_t0_us) &&
					writer.write_u8(payload.min_major) && writer.write_u8(payload.max_major) &&
					writer.write_u8(payload.min_minor) && writer.write_u8(payload.max_minor) &&
					writer.write_u8(static_cast<std::uint8_t>(payload.requested_visibility_mode)) &&
					writer.write_zeroes(3) && writer.write_u64(payload.advertised_capabilities) &&
					writer.write_u16(payload.requested_heartbeat_ms) &&
					writer.write_u16(static_cast<std::uint16_t>(payload.extensions.size)) &&
					writer.write_u16(payload.extension_count);
	if (!ok || finish_prefix<HelloPayloadPrefixSize>(writer) != ValidationError::None) {
		return ValidationError::InternalSerializationError;
	}
	return publish_encoded_payload(prefix, payload.extensions, output, written);
}

ValidationError decode_hello_payload(ByteView input, HelloPayload& payload) noexcept
{
	payload = HelloPayload{};
	if (input.size != 0 && input.data == nullptr) {
		return ValidationError::TruncatedPayload;
	}
	if (input.size < HelloPayloadPrefixSize) {
		return ValidationError::TruncatedPayload;
	}

	HelloPayload candidate;
	std::uint8_t raw_visibility = 0;
	ByteView reserved;
	std::uint16_t extensions_length = 0;
	PacketReader reader(ByteView{input.data, HelloPayloadPrefixSize});
	const bool ok = reader.read_u64(candidate.client_nonce) && reader.read_u64(candidate.client_send_t0_us) &&
					reader.read_u8(candidate.min_major) && reader.read_u8(candidate.max_major) &&
					reader.read_u8(candidate.min_minor) && reader.read_u8(candidate.max_minor) &&
					reader.read_u8(raw_visibility) && reader.read_bytes(3, reserved) &&
					reader.read_u64(candidate.advertised_capabilities) &&
					reader.read_u16(candidate.requested_heartbeat_ms) && reader.read_u16(extensions_length) &&
					reader.read_u16(candidate.extension_count);
	if (!ok || !reader.at_end()) {
		return ValidationError::TruncatedPayload;
	}
	if (extensions_length > MaxControlExtensionsSize || candidate.extension_count > MaxControlExtensionCount) {
		return ValidationError::OutOfRange;
	}
	const auto expected_size = HelloPayloadPrefixSize + static_cast<std::size_t>(extensions_length);
	if (input.size < expected_size) {
		return ValidationError::TruncatedPayload;
	}
	if (input.size > expected_size) {
		return ValidationError::TrailingBytes;
	}
	if (reserved.data[0] != 0 || reserved.data[1] != 0 || reserved.data[2] != 0) {
		return ValidationError::ReservedFlag;
	}

	candidate.requested_visibility_mode = static_cast<VisibilityMode>(raw_visibility);
	candidate.extensions =
		extensions_length == 0 ? ByteView{} : ByteView{input.data + HelloPayloadPrefixSize, extensions_length};
	if (const auto error = validate_hello_payload(candidate); error != ValidationError::None) {
		return error;
	}
	payload = candidate;
	return ValidationError::None;
}

ValidationError
encode_welcome_payload(const WelcomePayload& payload, MutableByteView output, std::size_t& written) noexcept
{
	written = 0;
	if (const auto error = validate_welcome_payload(payload); error != ValidationError::None) {
		return error;
	}
	if (const auto error = validate_emitted_capabilities(payload.producer_capabilities);
		error != ValidationError::None) {
		return error;
	}
	if ((payload.producer_capabilities & ~ProducerOwnedCapabilityMask) != 0) {
		return ValidationError::CapabilityNotNegotiated;
	}
	if (const auto error = validate_emitted_capabilities(payload.active_capabilities); error != ValidationError::None) {
		return error;
	}
	if (const auto error = validate_emitted_capability_extensions(payload.extensions, payload.extension_count);
		error != ValidationError::None) {
		return error;
	}

	std::array<std::uint8_t, WelcomePayloadPrefixSize> prefix{};
	PacketWriter writer(MutableByteView{prefix.data(), prefix.size()});
	const bool ok = writer.write_u64(payload.client_nonce) && writer.write_u64(payload.client_send_t0_us) &&
					writer.write_u64(payload.producer_receive_t1_us) && writer.write_u64(payload.producer_send_t2_us) &&
					writer.write_u8(static_cast<std::uint8_t>(payload.status)) &&
					writer.write_u8(payload.selected_major) && writer.write_u8(payload.selected_minor) &&
					writer.write_u8(static_cast<std::uint8_t>(payload.selected_visibility_mode)) &&
					writer.write_u64(payload.producer_capabilities) && writer.write_u64(payload.active_capabilities) &&
					writer.write_u16(payload.heartbeat_interval_ms) &&
					writer.write_u16(payload.reliable_reassembly_timeout_ms) &&
					writer.write_u16(static_cast<std::uint16_t>(payload.extensions.size)) &&
					writer.write_u16(payload.extension_count) && writer.write_u64(payload.producer_id);
	if (!ok || finish_prefix<WelcomePayloadPrefixSize>(writer) != ValidationError::None) {
		return ValidationError::InternalSerializationError;
	}
	return publish_encoded_payload(prefix, payload.extensions, output, written);
}

ValidationError decode_welcome_payload(ByteView input, WelcomePayload& payload) noexcept
{
	payload = WelcomePayload{};
	if (input.size != 0 && input.data == nullptr) {
		return ValidationError::TruncatedPayload;
	}
	if (input.size < WelcomePayloadPrefixSize) {
		return ValidationError::TruncatedPayload;
	}

	WelcomePayload candidate;
	std::uint8_t raw_status = 0;
	std::uint8_t raw_visibility = 0;
	std::uint16_t extensions_length = 0;
	PacketReader reader(ByteView{input.data, WelcomePayloadPrefixSize});
	const bool ok = reader.read_u64(candidate.client_nonce) && reader.read_u64(candidate.client_send_t0_us) &&
					reader.read_u64(candidate.producer_receive_t1_us) &&
					reader.read_u64(candidate.producer_send_t2_us) && reader.read_u8(raw_status) &&
					reader.read_u8(candidate.selected_major) && reader.read_u8(candidate.selected_minor) &&
					reader.read_u8(raw_visibility) && reader.read_u64(candidate.producer_capabilities) &&
					reader.read_u64(candidate.active_capabilities) &&
					reader.read_u16(candidate.heartbeat_interval_ms) &&
					reader.read_u16(candidate.reliable_reassembly_timeout_ms) && reader.read_u16(extensions_length) &&
					reader.read_u16(candidate.extension_count) && reader.read_u64(candidate.producer_id);
	if (!ok || !reader.at_end()) {
		return ValidationError::TruncatedPayload;
	}
	if (extensions_length > MaxControlExtensionsSize || candidate.extension_count > MaxControlExtensionCount) {
		return ValidationError::OutOfRange;
	}
	const auto expected_size = WelcomePayloadPrefixSize + static_cast<std::size_t>(extensions_length);
	if (input.size < expected_size) {
		return ValidationError::TruncatedPayload;
	}
	if (input.size > expected_size) {
		return ValidationError::TrailingBytes;
	}

	candidate.status = static_cast<WelcomeStatus>(raw_status);
	candidate.selected_visibility_mode = static_cast<VisibilityMode>(raw_visibility);
	candidate.extensions =
		extensions_length == 0 ? ByteView{} : ByteView{input.data + WelcomePayloadPrefixSize, extensions_length};
	if (const auto error = validate_welcome_payload(candidate); error != ValidationError::None) {
		return error;
	}
	payload = candidate;
	return ValidationError::None;
}

ValidationError
encode_session_begin_payload(const SessionBeginPayload& payload, MutableByteView output, std::size_t& written) noexcept
{
	written = 0;
	if (const auto error = validate_session_begin_payload(payload); error != ValidationError::None) {
		return error;
	}

	std::array<std::uint8_t, SessionBeginPayloadSize> prefix{};
	PacketWriter writer(MutableByteView{prefix.data(), prefix.size()});
	const bool ok = writer.write_u32(payload.session_flags) && writer.write_u64(payload.producer_session_start_us) &&
					writer.write_u64(payload.mission_instance_id) && writer.write_u32(payload.initial_snapshot_id) &&
					writer.write_u32(payload.required_manifest_id);
	if (!ok || finish_prefix<SessionBeginPayloadSize>(writer) != ValidationError::None) {
		return ValidationError::InternalSerializationError;
	}
	return publish_encoded_payload(prefix, ByteView{}, output, written);
}

ValidationError decode_session_begin_payload(ByteView input, SessionBeginPayload& payload) noexcept
{
	payload = SessionBeginPayload{};
	if (const auto error = validate_input_size(input, SessionBeginPayloadSize); error != ValidationError::None) {
		return error;
	}

	SessionBeginPayload candidate;
	PacketReader reader(input);
	const bool ok = reader.read_u32(candidate.session_flags) && reader.read_u64(candidate.producer_session_start_us) &&
					reader.read_u64(candidate.mission_instance_id) && reader.read_u32(candidate.initial_snapshot_id) &&
					reader.read_u32(candidate.required_manifest_id);
	if (!ok || !reader.at_end()) {
		return ValidationError::TruncatedPayload;
	}
	if (const auto error = validate_session_begin_payload(candidate); error != ValidationError::None) {
		return error;
	}
	payload = candidate;
	return ValidationError::None;
}

ValidationError
encode_heartbeat_payload(const HeartbeatPayload& payload, MutableByteView output, std::size_t& written) noexcept
{
	written = 0;
	if (const auto error = validate_heartbeat_payload(payload); error != ValidationError::None) {
		return error;
	}

	std::array<std::uint8_t, HeartbeatPayloadSize> prefix{};
	PacketWriter writer(MutableByteView{prefix.data(), prefix.size()});
	const bool ok = writer.write_u32(payload.probe_id) && writer.write_u8(static_cast<std::uint8_t>(payload.kind)) &&
					writer.write_zeroes(3) && writer.write_u64(payload.origin_t0_us) &&
					writer.write_u64(payload.receive_t1_us) && writer.write_u64(payload.transmit_t2_us);
	if (!ok || finish_prefix<HeartbeatPayloadSize>(writer) != ValidationError::None) {
		return ValidationError::InternalSerializationError;
	}
	return publish_encoded_payload(prefix, ByteView{}, output, written);
}

ValidationError decode_heartbeat_payload(ByteView input, HeartbeatPayload& payload) noexcept
{
	payload = HeartbeatPayload{};
	if (const auto error = validate_input_size(input, HeartbeatPayloadSize); error != ValidationError::None) {
		return error;
	}

	HeartbeatPayload candidate;
	std::uint8_t raw_kind = 0;
	ByteView reserved;
	PacketReader reader(input);
	const bool ok = reader.read_u32(candidate.probe_id) && reader.read_u8(raw_kind) && reader.read_bytes(3, reserved) &&
					reader.read_u64(candidate.origin_t0_us) && reader.read_u64(candidate.receive_t1_us) &&
					reader.read_u64(candidate.transmit_t2_us);
	if (!ok || !reader.at_end()) {
		return ValidationError::TruncatedPayload;
	}
	if (reserved.data[0] != 0 || reserved.data[1] != 0 || reserved.data[2] != 0) {
		return ValidationError::ReservedFlag;
	}
	candidate.kind = static_cast<HeartbeatKind>(raw_kind);
	if (const auto error = validate_heartbeat_payload(candidate); error != ValidationError::None) {
		return error;
	}
	payload = candidate;
	return ValidationError::None;
}

ValidationError encode_capability_update_payload(const CapabilityUpdatePayload& payload,
	MutableByteView output,
	std::size_t& written) noexcept
{
	written = 0;
	if (const auto error = validate_capability_update_payload(payload); error != ValidationError::None) {
		return error;
	}
	if (const auto error = validate_emitted_capabilities(payload.advertised_capabilities);
		error != ValidationError::None) {
		return error;
	}
	if (const auto error = validate_emitted_capabilities(payload.active_capabilities); error != ValidationError::None) {
		return error;
	}

	std::array<std::uint8_t, CapabilityUpdatePayloadSize> prefix{};
	PacketWriter writer(MutableByteView{prefix.data(), prefix.size()});
	const bool ok = writer.write_u32(payload.capability_generation) &&
					writer.write_u64(payload.advertised_capabilities) &&
					writer.write_u64(payload.active_capabilities) && writer.write_u64(payload.effective_time_us) &&
					writer.write_u8(static_cast<std::uint8_t>(payload.reason)) && writer.write_zeroes(3) &&
					writer.write_u16(0) && writer.write_u16(0);
	if (!ok || finish_prefix<CapabilityUpdatePayloadSize>(writer) != ValidationError::None) {
		return ValidationError::InternalSerializationError;
	}
	return publish_encoded_payload(prefix, ByteView{}, output, written);
}

ValidationError decode_capability_update_payload(ByteView input, CapabilityUpdatePayload& payload) noexcept
{
	payload = CapabilityUpdatePayload{};
	if (const auto error = validate_input_size(input, CapabilityUpdatePayloadSize); error != ValidationError::None) {
		return error;
	}

	CapabilityUpdatePayload candidate;
	std::uint8_t raw_reason = 0;
	ByteView reserved;
	std::uint16_t extensions_length = 0;
	std::uint16_t extension_count = 0;
	PacketReader reader(input);
	const bool ok = reader.read_u32(candidate.capability_generation) &&
					reader.read_u64(candidate.advertised_capabilities) &&
					reader.read_u64(candidate.active_capabilities) && reader.read_u64(candidate.effective_time_us) &&
					reader.read_u8(raw_reason) && reader.read_bytes(3, reserved) &&
					reader.read_u16(extensions_length) && reader.read_u16(extension_count);
	if (!ok || !reader.at_end()) {
		return ValidationError::TruncatedPayload;
	}
	if (reserved.data[0] != 0 || reserved.data[1] != 0 || reserved.data[2] != 0) {
		return ValidationError::ReservedFlag;
	}
	if (extensions_length != 0 || extension_count != 0) {
		return ValidationError::OutOfRange;
	}
	candidate.reason = static_cast<CapabilityUpdateReason>(raw_reason);
	if (const auto error = validate_capability_update_payload(candidate); error != ValidationError::None) {
		return error;
	}
	payload = candidate;
	return ValidationError::None;
}

} // namespace telemetry::protocol
