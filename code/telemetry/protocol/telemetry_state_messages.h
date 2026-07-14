#pragma once

#include "telemetry/protocol/telemetry_protocol_constants.h"
#include "telemetry/protocol/telemetry_protocol_types.h"
#include "telemetry/protocol/telemetry_sha256.h"

#include <cstddef>
#include <cstdint>

namespace telemetry::protocol {

constexpr std::size_t ManifestPartPayloadPrefixSize = 56;
constexpr std::size_t FullSnapshotPartPayloadPrefixSize = 60;
constexpr std::size_t DeltaPayloadPrefixSize = 20;

// The records view contains the exact record region following the fixed
// prefix. These codecs validate record-envelope framing and common flags;
// record-specific layouts, keys and cross-record semantics belong to P0.7.
struct ManifestPartPayload {
	std::uint32_t manifest_id = 0;
	std::uint16_t part_index = 0;
	std::uint16_t part_count = 0;
	std::uint32_t transaction_size = 0;
	Sha256Digest transaction_sha256{};
	std::uint64_t producer_sample_time_us = 0;
	ManifestKind manifest_kind = ManifestKind::FullRequired;
	std::uint16_t record_count = 0;
	ByteView records;
};

struct FullSnapshotPartPayload {
	std::uint32_t snapshot_id = 0;
	std::uint16_t part_index = 0;
	std::uint16_t part_count = 0;
	std::uint32_t transaction_size = 0;
	Sha256Digest transaction_sha256{};
	std::uint64_t producer_sample_time_us = 0;
	std::uint32_t required_manifest_id = 0;
	std::uint16_t snapshot_flags = SnapshotFlagNone;
	std::uint16_t record_count = 0;
	ByteView records;
};

struct DeltaPayload {
	std::uint32_t baseline_snapshot_id = 0;
	std::uint32_t delta_sequence = 0;
	std::uint64_t producer_sample_time_us = 0;
	std::uint16_t record_count = 0;
	ByteView records;
};

ValidationError validate_manifest_part_payload(const ManifestPartPayload& payload) noexcept;
ValidationError validate_full_snapshot_part_payload(const FullSnapshotPartPayload& payload) noexcept;
ValidationError validate_delta_payload(const DeltaPayload& payload) noexcept;

ValidationError
encode_manifest_part_payload(const ManifestPartPayload& payload, MutableByteView output, std::size_t& written) noexcept;
ValidationError decode_manifest_part_payload(ByteView input, ManifestPartPayload& payload) noexcept;

ValidationError encode_full_snapshot_part_payload(const FullSnapshotPartPayload& payload,
	MutableByteView output,
	std::size_t& written) noexcept;
ValidationError decode_full_snapshot_part_payload(ByteView input, FullSnapshotPartPayload& payload) noexcept;

ValidationError
encode_delta_payload(const DeltaPayload& payload, MutableByteView output, std::size_t& written) noexcept;
ValidationError decode_delta_payload(ByteView input, DeltaPayload& payload) noexcept;

} // namespace telemetry::protocol
