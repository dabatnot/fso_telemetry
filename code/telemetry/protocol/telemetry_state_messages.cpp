#include "telemetry/protocol/telemetry_state_messages.h"

#include "telemetry/protocol/packet_reader.h"
#include "telemetry/protocol/packet_writer.h"
#include "telemetry/protocol/telemetry_records.h"

#include <array>
#include <cstring>

namespace telemetry::protocol {

namespace {

ValidationError validate_transaction_part(std::uint32_t transaction_id,
	std::uint16_t part_index,
	std::uint16_t part_count,
	std::uint32_t transaction_size,
	std::uint16_t record_count,
	ByteView records,
	std::size_t prefix_size) noexcept
{
	if (transaction_id == 0 || part_count == 0 || part_count > MaxTransactionParts || part_index >= part_count ||
		transaction_size == 0 || transaction_size > MaxTransactionSize || transaction_size < part_count ||
		record_count == 0) {
		return ValidationError::OutOfRange;
	}
	if (records.size > MaxStatePartSize - prefix_size) {
		return ValidationError::MessageTooLarge;
	}
	if (records.size > transaction_size) {
		return ValidationError::OutOfRange;
	}
	return ValidationError::None;
}

template <std::size_t PrefixSize>
ValidationError publish_payload(const std::array<std::uint8_t, PrefixSize>& prefix,
	ByteView records,
	MutableByteView output,
	std::size_t& written) noexcept
{
	const auto total_size = PrefixSize + records.size;
	if (output.data == nullptr || output.size < total_size) {
		return ValidationError::InternalSerializationError;
	}

	// The source record region may start inside the destination, including in
	// the bytes which will become the prefix.
	if (!records.empty()) {
		std::memmove(output.data + PrefixSize, records.data, records.size);
	}
	std::memcpy(output.data, prefix.data(), prefix.size());
	written = total_size;
	return ValidationError::None;
}

template <std::size_t PrefixSize>
bool is_complete_prefix(const PacketWriter& writer) noexcept
{
	return writer.ok() && writer.size() == PrefixSize;
}

ValidationError validate_variable_input(ByteView input, std::size_t prefix_size) noexcept
{
	if (input.size != 0 && input.data == nullptr) {
		return ValidationError::TruncatedPayload;
	}
	if (input.size < prefix_size) {
		return ValidationError::TruncatedPayload;
	}
	if (input.size > MaxStatePartSize) {
		return ValidationError::MessageTooLarge;
	}
	return ValidationError::None;
}

bool has_exactly_one_snapshot_kind(std::uint16_t flags) noexcept
{
	return flags == SnapshotFlagInitial || flags == SnapshotFlagPeriodicKeyframe || flags == SnapshotFlagResync;
}

} // namespace

ValidationError validate_manifest_part_payload(const ManifestPartPayload& payload) noexcept
{
	if (const auto error = validate_transaction_part(payload.manifest_id,
			payload.part_index,
			payload.part_count,
			payload.transaction_size,
			payload.record_count,
			payload.records,
			ManifestPartPayloadPrefixSize);
		error != ValidationError::None) {
		return error;
	}
	if (payload.manifest_kind != ManifestKind::FullRequired) {
		return ValidationError::UnknownEnum;
	}
	return validate_record_region(payload.records, payload.record_count, RecordFlagPolicy::RequireNone);
}

ValidationError validate_full_snapshot_part_payload(const FullSnapshotPartPayload& payload) noexcept
{
	if (const auto error = validate_transaction_part(payload.snapshot_id,
			payload.part_index,
			payload.part_count,
			payload.transaction_size,
			payload.record_count,
			payload.records,
			FullSnapshotPartPayloadPrefixSize);
		error != ValidationError::None) {
		return error;
	}
	if ((payload.snapshot_flags & ReservedSnapshotFlags) != 0) {
		return ValidationError::ReservedFlag;
	}
	if (!has_exactly_one_snapshot_kind(payload.snapshot_flags)) {
		return ValidationError::InvalidStateTransition;
	}
	return validate_record_region(payload.records, payload.record_count, RecordFlagPolicy::RequireNone);
}

ValidationError validate_delta_payload(const DeltaPayload& payload) noexcept
{
	if (payload.baseline_snapshot_id == 0 || payload.delta_sequence == 0 || payload.record_count == 0) {
		return ValidationError::OutOfRange;
	}
	if (payload.records.size > MaxStateMessageSize - DeltaPayloadPrefixSize) {
		return ValidationError::MessageTooLarge;
	}
	return validate_record_region(payload.records, payload.record_count, RecordFlagPolicy::AllowV1Mutations);
}

ValidationError
encode_manifest_part_payload(const ManifestPartPayload& payload, MutableByteView output, std::size_t& written) noexcept
{
	written = 0;
	if (const auto error = validate_manifest_part_payload(payload); error != ValidationError::None) {
		return error;
	}

	std::array<std::uint8_t, ManifestPartPayloadPrefixSize> prefix{};
	PacketWriter writer(MutableByteView{prefix.data(), prefix.size()});
	const bool ok =
		writer.write_u32(payload.manifest_id) && writer.write_u16(payload.part_index) &&
		writer.write_u16(payload.part_count) && writer.write_u32(payload.transaction_size) &&
		writer.write_bytes(ByteView{payload.transaction_sha256.data(), payload.transaction_sha256.size()}) &&
		writer.write_u64(payload.producer_sample_time_us) &&
		writer.write_u16(static_cast<std::uint16_t>(payload.manifest_kind)) && writer.write_u16(payload.record_count);
	if (!ok || !is_complete_prefix<ManifestPartPayloadPrefixSize>(writer)) {
		return ValidationError::InternalSerializationError;
	}
	return publish_payload(prefix, payload.records, output, written);
}

ValidationError decode_manifest_part_payload(ByteView input, ManifestPartPayload& payload) noexcept
{
	payload = ManifestPartPayload{};
	if (const auto error = validate_variable_input(input, ManifestPartPayloadPrefixSize);
		error != ValidationError::None) {
		return error;
	}

	ManifestPartPayload candidate;
	ByteView digest;
	std::uint16_t raw_manifest_kind = 0;
	PacketReader reader(ByteView{input.data, ManifestPartPayloadPrefixSize});
	const bool ok = reader.read_u32(candidate.manifest_id) && reader.read_u16(candidate.part_index) &&
					reader.read_u16(candidate.part_count) && reader.read_u32(candidate.transaction_size) &&
					reader.read_bytes(Sha256DigestSize, digest) && reader.read_u64(candidate.producer_sample_time_us) &&
					reader.read_u16(raw_manifest_kind) && reader.read_u16(candidate.record_count);
	if (!ok || !reader.at_end()) {
		return ValidationError::TruncatedPayload;
	}
	std::memcpy(candidate.transaction_sha256.data(), digest.data, digest.size);
	candidate.manifest_kind = static_cast<ManifestKind>(raw_manifest_kind);
	candidate.records =
		ByteView{input.data + ManifestPartPayloadPrefixSize, input.size - ManifestPartPayloadPrefixSize};
	if (const auto error = validate_manifest_part_payload(candidate); error != ValidationError::None) {
		return error;
	}
	payload = candidate;
	return ValidationError::None;
}

ValidationError encode_full_snapshot_part_payload(const FullSnapshotPartPayload& payload,
	MutableByteView output,
	std::size_t& written) noexcept
{
	written = 0;
	if (const auto error = validate_full_snapshot_part_payload(payload); error != ValidationError::None) {
		return error;
	}

	std::array<std::uint8_t, FullSnapshotPartPayloadPrefixSize> prefix{};
	PacketWriter writer(MutableByteView{prefix.data(), prefix.size()});
	const bool ok =
		writer.write_u32(payload.snapshot_id) && writer.write_u16(payload.part_index) &&
		writer.write_u16(payload.part_count) && writer.write_u32(payload.transaction_size) &&
		writer.write_bytes(ByteView{payload.transaction_sha256.data(), payload.transaction_sha256.size()}) &&
		writer.write_u64(payload.producer_sample_time_us) && writer.write_u32(payload.required_manifest_id) &&
		writer.write_u16(payload.snapshot_flags) && writer.write_u16(payload.record_count);
	if (!ok || !is_complete_prefix<FullSnapshotPartPayloadPrefixSize>(writer)) {
		return ValidationError::InternalSerializationError;
	}
	return publish_payload(prefix, payload.records, output, written);
}

ValidationError decode_full_snapshot_part_payload(ByteView input, FullSnapshotPartPayload& payload) noexcept
{
	payload = FullSnapshotPartPayload{};
	if (const auto error = validate_variable_input(input, FullSnapshotPartPayloadPrefixSize);
		error != ValidationError::None) {
		return error;
	}

	FullSnapshotPartPayload candidate;
	ByteView digest;
	PacketReader reader(ByteView{input.data, FullSnapshotPartPayloadPrefixSize});
	const bool ok = reader.read_u32(candidate.snapshot_id) && reader.read_u16(candidate.part_index) &&
					reader.read_u16(candidate.part_count) && reader.read_u32(candidate.transaction_size) &&
					reader.read_bytes(Sha256DigestSize, digest) && reader.read_u64(candidate.producer_sample_time_us) &&
					reader.read_u32(candidate.required_manifest_id) && reader.read_u16(candidate.snapshot_flags) &&
					reader.read_u16(candidate.record_count);
	if (!ok || !reader.at_end()) {
		return ValidationError::TruncatedPayload;
	}
	std::memcpy(candidate.transaction_sha256.data(), digest.data, digest.size);
	candidate.records =
		ByteView{input.data + FullSnapshotPartPayloadPrefixSize, input.size - FullSnapshotPartPayloadPrefixSize};
	if (const auto error = validate_full_snapshot_part_payload(candidate); error != ValidationError::None) {
		return error;
	}
	payload = candidate;
	return ValidationError::None;
}

ValidationError encode_delta_payload(const DeltaPayload& payload, MutableByteView output, std::size_t& written) noexcept
{
	written = 0;
	if (const auto error = validate_delta_payload(payload); error != ValidationError::None) {
		return error;
	}

	std::array<std::uint8_t, DeltaPayloadPrefixSize> prefix{};
	PacketWriter writer(MutableByteView{prefix.data(), prefix.size()});
	const bool ok = writer.write_u32(payload.baseline_snapshot_id) && writer.write_u32(payload.delta_sequence) &&
					writer.write_u64(payload.producer_sample_time_us) && writer.write_u16(payload.record_count) &&
					writer.write_u16(0);
	if (!ok || !is_complete_prefix<DeltaPayloadPrefixSize>(writer)) {
		return ValidationError::InternalSerializationError;
	}
	return publish_payload(prefix, payload.records, output, written);
}

ValidationError decode_delta_payload(ByteView input, DeltaPayload& payload) noexcept
{
	payload = DeltaPayload{};
	if (const auto error = validate_variable_input(input, DeltaPayloadPrefixSize); error != ValidationError::None) {
		return error;
	}

	DeltaPayload candidate;
	std::uint16_t reserved = 0;
	PacketReader reader(ByteView{input.data, DeltaPayloadPrefixSize});
	const bool ok = reader.read_u32(candidate.baseline_snapshot_id) && reader.read_u32(candidate.delta_sequence) &&
					reader.read_u64(candidate.producer_sample_time_us) && reader.read_u16(candidate.record_count) &&
					reader.read_u16(reserved);
	if (!ok || !reader.at_end()) {
		return ValidationError::TruncatedPayload;
	}
	if (reserved != 0) {
		return ValidationError::ReservedFlag;
	}
	candidate.records = ByteView{input.data + DeltaPayloadPrefixSize, input.size - DeltaPayloadPrefixSize};
	if (const auto error = validate_delta_payload(candidate); error != ValidationError::None) {
		return error;
	}
	payload = candidate;
	return ValidationError::None;
}

} // namespace telemetry::protocol
