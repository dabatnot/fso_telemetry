#pragma once

#include "telemetry/protocol/telemetry_protocol_constants.h"
#include "telemetry/protocol/telemetry_protocol_types.h"

#include <cstddef>
#include <cstdint>

namespace telemetry::protocol {

constexpr std::size_t RecordEnvelopeHeaderSize = 6;

// Common v1 envelope validation deliberately stops before container/type
// compatibility and record-payload semantics. Manifest and FullSnapshot use
// RequireNone; Delta and EventBatch may use the generic mutation flags and
// leave the per-RecordType decision to their semantic validators.
enum class RecordFlagPolicy : std::uint8_t {
	AllowV1Mutations,
	RequireNone,
};

// raw_record_type and record_version remain numeric so unknown extensions can
// be skipped structurally without pretending that they are known v1 records.
struct RecordEnvelopeView {
	std::uint16_t raw_record_type = 0;
	std::uint8_t record_version = 0;
	std::uint8_t record_flags = RecordFlagNone;
	ByteView payload;
};

ValidationError validate_record_flags_v1(std::uint8_t flags, RecordFlagPolicy policy) noexcept;

// Allocation-free iterator over exactly record_count concatenated envelopes.
// next() is sticky after failure. On success, has_value is true for a record
// and false only at the exact declared end. Unknown non-zero types and record
// versions are returned normally for bounded skipping by the semantic layer.
class RecordEnvelopeIterator final {
  public:
	RecordEnvelopeIterator(ByteView records,
		std::uint16_t record_count,
		RecordFlagPolicy flag_policy = RecordFlagPolicy::AllowV1Mutations) noexcept;

	// record and has_value are reset before any error is reported.
	ValidationError next(RecordEnvelopeView& record, bool& has_value) noexcept;

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
	ByteView m_records;
	std::size_t m_offset = 0;
	std::uint16_t m_record_count = 0;
	std::uint16_t m_index = 0;
	RecordFlagPolicy m_flag_policy = RecordFlagPolicy::AllowV1Mutations;
	ValidationError m_error = ValidationError::None;
};

// Performs a complete structural pass, including exact count and trailing-byte
// validation. Callers which will publish state validate first, then iterate a
// second time while building their unpublished candidate.
ValidationError validate_record_region(ByteView records,
	std::uint16_t record_count,
	RecordFlagPolicy flag_policy = RecordFlagPolicy::AllowV1Mutations) noexcept;

} // namespace telemetry::protocol
