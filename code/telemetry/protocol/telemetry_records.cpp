#include "telemetry/protocol/telemetry_records.h"

#include "telemetry/protocol/packet_reader.h"

namespace telemetry::protocol {

namespace {

bool is_known_flag_policy(RecordFlagPolicy policy) noexcept
{
	return policy == RecordFlagPolicy::AllowV1Mutations || policy == RecordFlagPolicy::RequireNone;
}

} // namespace

ValidationError validate_record_flags_v1(std::uint8_t flags, RecordFlagPolicy policy) noexcept
{
	if (!is_known_flag_policy(policy)) {
		return ValidationError::InvalidStateTransition;
	}
	if ((flags & ReservedRecordFlags) != 0 || (flags & RecordFlagPartial) != 0) {
		// PARTIAL is a known registry bit reserved for a compatible evolution,
		// but none of the 28 v1 records permits it.
		return ValidationError::ReservedFlag;
	}
	if ((flags & RecordFlagCreate) != 0 && (flags & RecordFlagDelete) != 0) {
		return ValidationError::InvalidStateTransition;
	}
	if (policy == RecordFlagPolicy::RequireNone && flags != RecordFlagNone) {
		return ValidationError::InvalidStateTransition;
	}
	return ValidationError::None;
}

RecordEnvelopeIterator::RecordEnvelopeIterator(ByteView records,
	std::uint16_t record_count,
	RecordFlagPolicy flag_policy) noexcept
	: m_records(records), m_record_count(record_count), m_flag_policy(flag_policy)
{
	if (records.size != 0 && records.data == nullptr) {
		m_error = ValidationError::TruncatedPayload;
	} else if (!is_known_flag_policy(flag_policy)) {
		m_error = ValidationError::InvalidStateTransition;
	}
}

ValidationError RecordEnvelopeIterator::next(RecordEnvelopeView& record, bool& has_value) noexcept
{
	record = RecordEnvelopeView{};
	has_value = false;
	if (m_error != ValidationError::None) {
		return m_error;
	}

	if (m_index == m_record_count) {
		if (m_offset != m_records.size) {
			m_error = ValidationError::TrailingBytes;
		}
		return m_error;
	}

	// m_offset is advanced only by a successfully bounded PacketReader, but
	// retain the guard so no future change can turn the subtraction below into
	// an overflow.
	if (m_offset > m_records.size) {
		m_error = ValidationError::BadRecordLength;
		return m_error;
	}
	const auto remaining_size = m_records.size - m_offset;
	if (remaining_size < RecordEnvelopeHeaderSize) {
		m_error = ValidationError::TruncatedPayload;
		return m_error;
	}

	const ByteView remaining{m_records.data + m_offset, remaining_size};
	PacketReader reader(remaining);
	RecordEnvelopeView candidate;
	std::uint16_t record_length = 0;
	if (!reader.read_u16(candidate.raw_record_type) || !reader.read_u8(candidate.record_version) ||
		!reader.read_u8(candidate.record_flags) || !reader.read_u16(record_length)) {
		m_error = ValidationError::TruncatedPayload;
		return m_error;
	}
	if (candidate.raw_record_type == static_cast<std::uint16_t>(RecordType::Invalid)) {
		m_error = ValidationError::OutOfRange;
		return m_error;
	}
	if (const auto error = validate_record_flags_v1(candidate.record_flags, m_flag_policy);
		error != ValidationError::None) {
		m_error = error;
		return m_error;
	}
	if (static_cast<std::size_t>(record_length) > reader.remaining()) {
		m_error = ValidationError::BadRecordLength;
		return m_error;
	}
	if (!reader.read_bytes(record_length, candidate.payload)) {
		m_error = ValidationError::BadRecordLength;
		return m_error;
	}

	m_offset += reader.consumed();
	++m_index;
	record = candidate;
	has_value = true;
	return ValidationError::None;
}

ValidationError
validate_record_region(ByteView records, std::uint16_t record_count, RecordFlagPolicy flag_policy) noexcept
{
	RecordEnvelopeIterator iterator(records, record_count, flag_policy);
	for (;;) {
		RecordEnvelopeView record;
		bool has_value = false;
		if (const auto error = iterator.next(record, has_value); error != ValidationError::None) {
			return error;
		}
		if (!has_value) {
			return ValidationError::None;
		}
	}
}

} // namespace telemetry::protocol
