#pragma once

#include "telemetry/protocol/telemetry_records.h"
#include "telemetry/protocol/telemetry_replication.h"

#include <cstddef>
#include <cstdint>

namespace telemetry::protocol {

enum class BusinessRecordContainer : std::uint8_t {
	Manifest = 1,
	FullSnapshot = 2,
	Delta = 3,
	EventBatchReplaceable = 4,
	EventBatchReliable = 5,
};

struct BusinessRecordMetadata {
	RecordType type = RecordType::Invalid;
	std::size_t key_size = 0;
	bool state_atom = false;
	StateRecordLifecycle lifecycle = StateRecordLifecycle::UpsertOnly;
	bool cascades_with_entity = false;
};

// Returns the frozen v1 schema metadata for a known RecordType. Unknown types
// return false and reset metadata. This function performs no payload access.
bool business_record_metadata(std::uint16_t raw_record_type, BusinessRecordMetadata& metadata) noexcept;

// Validates container membership, the per-type record flags and the complete
// v1 payload. Unknown RecordTypes are skipped successfully; known types with
// another version are reported as UnsupportedRecordVersion. metadata is reset
// on every error or skipped extension.
ValidationError validate_business_record(const RecordEnvelopeView& record,
	BusinessRecordContainer container,
	BusinessRecordMetadata& metadata) noexcept;
ValidationError validate_business_record(const RecordEnvelopeView& record,
	BusinessRecordContainer container,
	std::uint8_t protocol_minor,
	BusinessRecordMetadata& metadata) noexcept;

// Emits one fully validated, known v1 business-record envelope. Unknown
// extension records are intentionally decode-only and are never relayed by a
// v1 encoder. The output and written count remain unchanged/zero on failure.
ValidationError encode_business_record(const RecordEnvelopeView& record,
	BusinessRecordContainer container,
	MutableByteView output,
	std::size_t& written) noexcept;
ValidationError encode_business_record(const RecordEnvelopeView& record,
	BusinessRecordContainer container,
	std::uint8_t protocol_minor,
	MutableByteView output,
	std::size_t& written) noexcept;

// Validates a state record and materializes its immutable P0.6 atom. Only
// records legal in FULL_SNAPSHOT or DELTA are accepted. A DELETE produces an
// atom carrying only its key and lifecycle metadata. output is unchanged on
// failure.
ValidationError decode_business_state_atom(const RecordEnvelopeView& record,
	BusinessRecordContainer container,
	StateAtom& output) noexcept;
ValidationError decode_business_state_atom(const RecordEnvelopeView& record,
	BusinessRecordContainer container,
	std::uint8_t protocol_minor,
	StateAtom& output) noexcept;

// Converts an exact FULL_SNAPSHOT record region into the canonical immutable
// image used by P0.6. Unknown extension records are skipped; every known v1
// record is fully validated before publication.
ValidationError decode_business_snapshot_region(ByteView records,
	std::uint16_t record_count,
	StateImage& output) noexcept;

// Same conversion with the transaction-level validator run before the
// candidate becomes observable. This is the publication path for complete
// Phase 0 snapshots; output remains unchanged on semantic failure.
ValidationError decode_business_snapshot_region_validated(ByteView records,
	std::uint16_t record_count,
	const StateImageValidator& validator,
	StateImage& output) noexcept;

// Converts a validated DELTA wire payload to the sorted cumulative mutation
// model. The output remains unchanged on any structural, semantic, duplicate
// key or allocation failure.
ValidationError decode_business_delta(const DeltaPayload& payload, CumulativeStateDelta& output) noexcept;
ValidationError decode_business_delta(const DeltaPayload& payload,
	std::uint8_t protocol_minor,
	CumulativeStateDelta& output) noexcept;

} // namespace telemetry::protocol
