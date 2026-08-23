#include "telemetry/protocol/telemetry_business_records_internal.h"

#include "telemetry/protocol/packet_reader.h"
#include "telemetry/protocol/packet_writer.h"
#include "telemetry/protocol/telemetry_protocol_constants.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace telemetry::protocol::detail {

ValidationError validate_business_record_29(ByteView payload) noexcept
{
	PacketReader reader(payload);
	std::uint64_t entity_id = 0U;
	std::uint64_t presence = 0U;
	std::uint64_t sample_time = 0U;
	std::uint8_t primary_fire = 0U;
	std::uint8_t lock_state = 0U;
	std::uint8_t missile_direction_sector_mask = 0U;
	if (!reader.read_u64(entity_id) || !reader.read_u64(presence) ||
		!reader.read_u64(sample_time) || !reader.read_u8(primary_fire) ||
		!reader.read_u8(lock_state) ||
		!reader.read_u8(missile_direction_sector_mask)) {
		return ValidationError::TruncatedPayload;
	}
	if (entity_id == 0U) {
		return ValidationError::OutOfRange;
	}
	if ((presence & ReservedHudAlertStatePresenceFlags) != 0U) {
		return ValidationError::ReservedFlag;
	}
	if (primary_fire > 1U) {
		return ValidationError::OutOfRange;
	}
	if (lock_state > static_cast<std::uint8_t>(HudAlertMissileLockState::Acquired)) {
		return ValidationError::UnknownEnum;
	}
	(void)missile_direction_sector_mask; // Every bit names one of the eight HUD sectors.
	if ((presence & HudAlertStatePresenceFlagActiveWarning) != 0U) {
		std::uint8_t warning_kind = 0U;
		std::uint64_t warning_instance_id = 0U;
		std::uint64_t warning_remaining_us = 0U;
		std::uint16_t text_length = 0U;
		if (!reader.read_u8(warning_kind) ||
			!reader.read_u64(warning_instance_id) ||
			!reader.read_u64(warning_remaining_us) ||
			!reader.read_u16(text_length)) {
			return ValidationError::TruncatedPayload;
		}
		if (warning_kind < static_cast<std::uint8_t>(HudAlertWarningKind::Launch) ||
			warning_kind > static_cast<std::uint8_t>(HudAlertWarningKind::Other)) {
			return ValidationError::UnknownEnum;
		}
		if (warning_instance_id == 0U || warning_remaining_us == 0U ||
			text_length == 0U) {
			return ValidationError::OutOfRange;
		}
		if (text_length > 511U) {
			return ValidationError::StringTooLong;
		}
		ByteView text_bytes;
		if (!reader.read_bytes(text_length, text_bytes)) {
			return ValidationError::TruncatedPayload;
		}
		const auto text = std::string_view(
			static_cast<const char*>(static_cast<const void*>(text_bytes.data)),
			text_bytes.size);
		if (!is_valid_utf8(text)) {
			return ValidationError::InvalidUtf8;
		}
	}
	return reader.at_end() ? ValidationError::None : ValidationError::TrailingBytes;
}

} // namespace telemetry::protocol::detail
