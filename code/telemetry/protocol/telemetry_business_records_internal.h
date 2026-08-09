#pragma once

#include "telemetry/protocol/telemetry_records.h"

namespace telemetry::protocol::detail {

ValidationError validate_business_record_1_10(RecordType type,
	ByteView payload,
	std::uint8_t protocol_minor) noexcept;
ValidationError validate_business_record_11_18(
	RecordType type, std::uint8_t record_version, ByteView payload) noexcept;
ValidationError validate_business_record_19_24(RecordType type, ByteView payload) noexcept;
ValidationError validate_business_record_28(ByteView payload, bool reliable_delivery) noexcept;
ValidationError validate_business_record_29(ByteView payload) noexcept;

} // namespace telemetry::protocol::detail
