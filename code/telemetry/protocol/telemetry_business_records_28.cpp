#include "telemetry/protocol/telemetry_business_records_internal.h"

#include "telemetry/protocol/packet_reader.h"
#include "telemetry/protocol/packet_writer.h"
#include "telemetry/protocol/telemetry_protocol_constants.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace telemetry::protocol::detail {

namespace {

constexpr float PositionLimit = 1.0e12F;
constexpr float QuantityLimit = 1.0e12F;

class EventReader final {
  public:
	explicit EventReader(ByteView bytes) noexcept : m_reader(bytes)
	{
		if (bytes.size != 0 && bytes.data == nullptr) {
			m_error = ValidationError::TruncatedPayload;
		}
	}

	ValidationError error() const noexcept
	{
		return m_error;
	}

	bool ok() const noexcept
	{
		return m_error == ValidationError::None;
	}

	bool fail(ValidationError error) noexcept
	{
		if (m_error == ValidationError::None) {
			m_error = error;
		}
		return false;
	}

	bool u8(std::uint8_t& value) noexcept
	{
		return ok() && (m_reader.read_u8(value) || fail(ValidationError::TruncatedPayload));
	}
	bool u16(std::uint16_t& value) noexcept
	{
		return ok() && (m_reader.read_u16(value) || fail(ValidationError::TruncatedPayload));
	}
	bool u32(std::uint32_t& value) noexcept
	{
		return ok() && (m_reader.read_u32(value) || fail(ValidationError::TruncatedPayload));
	}
	bool u64(std::uint64_t& value) noexcept
	{
		return ok() && (m_reader.read_u64(value) || fail(ValidationError::TruncatedPayload));
	}

	bool f32(float& value, float minimum, float maximum) noexcept
	{
		std::uint32_t bits = 0;
		if (!u32(bits)) {
			return false;
		}
		std::memcpy(&value, &bits, sizeof(value));
		if (!std::isfinite(value)) {
			return fail(ValidationError::NonFiniteFloat);
		}
		value = value == 0.0F ? 0.0F : value;
		return (value >= minimum && value <= maximum) || fail(ValidationError::OutOfRange);
	}

	bool zero16() noexcept
	{
		std::uint16_t value = 0;
		return u16(value) && (value == 0 || fail(ValidationError::ReservedFlag));
	}

	bool zeroes(std::size_t count) noexcept
	{
		ByteView bytes;
		if (!ok() || !m_reader.read_bytes(count, bytes)) {
			return fail(ValidationError::TruncatedPayload);
		}
		for (std::size_t index = 0; index < count; ++index) {
			if (bytes.data[index] != 0) {
				return fail(ValidationError::ReservedFlag);
			}
		}
		return true;
	}

	bool string(std::size_t maximum) noexcept
	{
		std::uint16_t length = 0;
		if (!u16(length)) {
			return false;
		}
		if (length == 0) {
			return fail(ValidationError::OutOfRange);
		}
		if (length > maximum) {
			return fail(ValidationError::StringTooLong);
		}
		ByteView bytes;
		if (!m_reader.read_bytes(length, bytes)) {
			return fail(ValidationError::TruncatedPayload);
		}
		const auto text = std::string_view(static_cast<const char*>(static_cast<const void*>(bytes.data)), bytes.size);
		return is_valid_utf8(text) || fail(ValidationError::InvalidUtf8);
	}

	bool child(std::size_t size, EventReader& output) noexcept
	{
		PacketReader child_reader;
		if (!ok() || !m_reader.subreader(size, child_reader)) {
			return fail(ValidationError::TruncatedPayload);
		}
		output = EventReader(child_reader.unread());
		return true;
	}

	ValidationError finish() noexcept
	{
		if (ok() && !m_reader.at_end()) {
			fail(ValidationError::TrailingBytes);
		}
		return m_error;
	}

  private:
	PacketReader m_reader;
	ValidationError m_error = ValidationError::None;
};

bool nonzero_u32(EventReader& reader, std::uint32_t& value) noexcept
{
	return reader.u32(value) && (value != 0 || reader.fail(ValidationError::OutOfRange));
}

bool nonzero_u64(EventReader& reader, std::uint64_t& value) noexcept
{
	return reader.u64(value) && (value != 0 || reader.fail(ValidationError::OutOfRange));
}

bool vec3(EventReader& reader, float minimum, float maximum) noexcept
{
	float value = 0.0F;
	return reader.f32(value, minimum, maximum) && reader.f32(value, minimum, maximum) &&
		   reader.f32(value, minimum, maximum);
}

bool normalized_vec3(EventReader& reader) noexcept
{
	std::array<float, 3> value{};
	for (auto& component : value) {
		if (!reader.f32(component, -1.001F, 1.001F)) {
			return false;
		}
	}
	const auto length_squared =
		value[0] * value[0] + value[1] * value[1] + value[2] * value[2];
	return (length_squared >= 0.999F * 0.999F && length_squared <= 1.001F * 1.001F) ||
		   reader.fail(ValidationError::OutOfRange);
}

struct PresenceRule {
	std::uint32_t required = 0;
	std::uint32_t allowed = 0;
	bool zero_subject = false;
};

PresenceRule presence_rule(EventKind kind) noexcept
{
	using P = EventItemPresenceFlag;
	const auto actor = static_cast<std::uint32_t>(P::EventItemPresenceFlagActor);
	const auto target = static_cast<std::uint32_t>(P::EventItemPresenceFlagTarget);
	const auto subsystem = static_cast<std::uint32_t>(P::EventItemPresenceFlagSubsystem);
	const auto weapon = static_cast<std::uint32_t>(P::EventItemPresenceFlagWeaponClass);
	const auto bank = static_cast<std::uint32_t>(P::EventItemPresenceFlagBank);
	const auto fire_point = static_cast<std::uint32_t>(P::EventItemPresenceFlagFirePoint);
	const auto projectiles = static_cast<std::uint32_t>(P::EventItemPresenceFlagProjectiles);
	const auto position = static_cast<std::uint32_t>(P::EventItemPresenceFlagPosition);
	const auto normal = static_cast<std::uint32_t>(P::EventItemPresenceFlagNormal);
	const auto amount = static_cast<std::uint32_t>(P::EventItemPresenceFlagAmount);
	const auto shield = static_cast<std::uint32_t>(P::EventItemPresenceFlagShieldSegment);
	const auto burst = static_cast<std::uint32_t>(P::EventItemPresenceFlagBurst);
	const auto cargo = static_cast<std::uint32_t>(P::EventItemPresenceFlagCargoText);
	const auto reason = static_cast<std::uint32_t>(P::EventItemPresenceFlagReasonCode);
	const auto previous = static_cast<std::uint32_t>(P::EventItemPresenceFlagPreviousTarget);
	const auto transit = static_cast<std::uint32_t>(P::EventItemPresenceFlagTransitMode);

	switch (kind) {
	case EventKind::EntityAppeared:
	case EventKind::EntityDepartureStarted:
	case EventKind::EntityDisappeared:
		return {0, reason, false};
	case EventKind::WeaponFired:
		return {weapon | bank | fire_point, weapon | bank | fire_point | target | projectiles | burst, false};
	case EventKind::BeamStarted:
	case EventKind::BeamEnded:
		return {weapon | bank, weapon | bank | target | subsystem | fire_point | position, false};
	case EventKind::RemoteDetonation:
		return {0, target | projectiles | position, false};
	case EventKind::CountermeasureLaunched:
		return {weapon, weapon | position | projectiles, false};
	case EventKind::ShieldImpact:
		return {position | normal | amount | shield, position | normal | amount | shield | actor | weapon, false};
	case EventKind::HullImpact:
		return {position | normal | amount, position | normal | amount | actor | weapon, false};
	case EventKind::ShieldSegmentDepleted:
	case EventKind::ShieldSegmentRestored:
		return {shield, shield | actor | weapon | position, false};
	case EventKind::SubsystemDamaged:
	case EventKind::SubsystemPerturbed:
	case EventKind::SubsystemRestored:
	case EventKind::SubsystemDestroyed:
		return {subsystem, subsystem | actor | weapon | position | amount, false};
	case EventKind::ShipDisabled:
	case EventKind::ShipDyingStarted:
	case EventKind::EntityDestroyed:
		return {0, actor | weapon | position | reason, false};
	case EventKind::TargetChanged:
		return {target | previous, target | previous, false};
	case EventKind::ScanCompleted:
		return {target, target | subsystem, false};
	case EventKind::CargoRevealed:
		return {target | cargo, target | cargo | subsystem, false};
	case EventKind::DockingStarted:
	case EventKind::Docked:
	case EventKind::UndockingStarted:
	case EventKind::Undocked:
		return {target, target | reason, false};
	case EventKind::WarpStarted:
	case EventKind::WarpEnded:
		return {transit, transit | reason, false};
	case EventKind::MissionStarted:
	case EventKind::MissionEnded:
	case EventKind::SessionEnded:
		return {0, reason, true};
	case EventKind::AfterburnerStarted:
	case EventKind::AfterburnerStopped:
		return {0, 0, false};
	default:
		return {};
	}
}

bool inherently_reliable(EventKind kind) noexcept
{
	return kind >= EventKind::WeaponFired && kind <= EventKind::HullImpact;
}

ValidationError validate_event_item(EventReader& item,
	bool reliable_delivery,
	std::uint64_t previous_event_id,
	std::uint64_t& event_id) noexcept
{
	std::uint32_t presence = 0;
	std::uint64_t sample = 0;
	std::uint16_t raw_kind = 0;
	std::uint16_t flags = 0;
	std::uint64_t subject = 0;
	if (!item.u32(presence)) {
		return item.error();
	}
	if ((presence & ReservedEventItemPresenceFlags) != 0) {
		return ValidationError::ReservedFlag;
	}
	if (!nonzero_u64(item, event_id) || !item.u64(sample) || !item.u16(raw_kind) || !item.u16(flags) ||
		!item.u64(subject)) {
		return item.error();
	}
	if (previous_event_id != 0 && event_id <= previous_event_id) {
		return ValidationError::InvalidStateTransition;
	}
	if (raw_kind < static_cast<std::uint16_t>(EventKind::EntityAppeared) ||
		raw_kind > static_cast<std::uint16_t>(EventKind::AfterburnerStopped)) {
		return ValidationError::UnknownEnum;
	}
	if ((flags & ReservedEventFlags) != 0) {
		return ValidationError::ReservedFlag;
	}
	const auto kind = static_cast<EventKind>(raw_kind);
	const auto rule = presence_rule(kind);
	if ((presence & rule.required) != rule.required) {
		return ValidationError::InvalidAbsence;
	}
	if ((presence & ~rule.allowed) != 0) {
		return ValidationError::InvalidStateTransition;
	}
	if (rule.zero_subject != (subject == 0)) {
		return ValidationError::OutOfRange;
	}
	if (reliable_delivery) {
		if ((flags & EventFlagReliable) == 0) {
			return ValidationError::InvalidStateTransition;
		}
	} else if ((flags & EventFlagReliable) != 0 || (flags & EventFlagReconstructible) == 0) {
		return ValidationError::InvalidStateTransition;
	}
	if (inherently_reliable(kind) && (flags & EventFlagReliable) == 0) {
		return ValidationError::InvalidStateTransition;
	}

	std::uint64_t entity = 0;
	std::uint32_t id = 0;
	if ((presence & EventItemPresenceFlagActor) != 0 && !nonzero_u64(item, entity)) {
		return item.error();
	}
	if ((presence & EventItemPresenceFlagTarget) != 0) {
		if (!item.u64(entity)) {
			return item.error();
		}
		if (entity == 0 && kind != EventKind::TargetChanged) {
			return ValidationError::OutOfRange;
		}
	}
	if ((presence & EventItemPresenceFlagSubsystem) != 0 && !nonzero_u32(item, id)) {
		return item.error();
	}
	if ((presence & EventItemPresenceFlagWeaponClass) != 0 && !nonzero_u32(item, id)) {
		return item.error();
	}
	if ((presence & EventItemPresenceFlagBank) != 0) {
		std::uint8_t family = 0;
		std::uint8_t reserved = 0;
		std::uint16_t index = 0;
		if (!item.u8(family) || !item.u8(reserved) || !item.u16(index) || !nonzero_u32(item, id)) {
			return item.error();
		}
		if (family > static_cast<std::uint8_t>(WeaponFamily::Turret)) {
			return ValidationError::UnknownEnum;
		}
		if (reserved != 0) {
			return ValidationError::ReservedFlag;
		}
		if (index > 63) {
			return ValidationError::OutOfRange;
		}
	}
	if ((presence & EventItemPresenceFlagFirePoint) != 0) {
		std::uint16_t index = 0;
		if (!item.u16(index) || !item.zero16()) {
			return item.error();
		}
		if (index > 255) {
			return ValidationError::OutOfRange;
		}
	}
	if ((presence & EventItemPresenceFlagProjectiles) != 0) {
		std::uint16_t count = 0;
		std::uint8_t version = 0;
		std::uint16_t size = 0;
		if (!item.u16(count) || !item.u8(version) || !item.u16(size)) {
			return item.error();
		}
		if (count == 0 || count > 256 || version != 1 || size != 8) {
			return version != 1 ? ValidationError::UnsupportedRecordVersion : ValidationError::OutOfRange;
		}
		std::array<std::uint64_t, 256> projectiles{};
		for (std::size_t index = 0; index < count; ++index) {
			std::uint64_t projectile = 0;
			if (!nonzero_u64(item, projectile)) {
				return item.error();
			}
			for (std::size_t prior = 0; prior < index; ++prior) {
				if (projectiles[prior] == projectile) {
					return ValidationError::DuplicateItemKey;
				}
			}
			projectiles[index] = projectile;
		}
	}
	if ((presence & EventItemPresenceFlagPosition) != 0 && !vec3(item, -PositionLimit, PositionLimit)) {
		return item.error();
	}
	if ((presence & EventItemPresenceFlagNormal) != 0 && !normalized_vec3(item)) {
		return item.error();
	}
	if ((presence & EventItemPresenceFlagAmount) != 0) {
		float amount = 0.0F;
		if (!item.f32(amount, 0.0F, QuantityLimit)) {
			return item.error();
		}
	}
	if ((presence & EventItemPresenceFlagShieldSegment) != 0) {
		std::uint16_t segment = 0;
		if (!item.u16(segment) || !item.zero16()) {
			return item.error();
		}
		if (segment > 63) {
			return ValidationError::OutOfRange;
		}
	}
	if ((presence & EventItemPresenceFlagBurst) != 0) {
		std::uint16_t counter = 0;
		std::uint32_t seed = 0;
		if (!item.u16(counter) || !item.zero16() || !item.u32(seed)) {
			return item.error();
		}
	}
	if ((presence & EventItemPresenceFlagCargoText) != 0 && !item.string(511)) {
		return item.error();
	}
	if ((presence & EventItemPresenceFlagReasonCode) != 0) {
		std::uint16_t reason = 0;
		if (!item.u16(reason)) {
			return item.error();
		}
		if (reason > static_cast<std::uint16_t>(EventReasonCode::Unknown)) {
			return ValidationError::UnknownEnum;
		}
	}
	if ((presence & EventItemPresenceFlagPreviousTarget) != 0 && !item.u64(entity)) {
		return item.error();
	}
	if ((presence & EventItemPresenceFlagTransitMode) != 0) {
		std::uint8_t transit = 0;
		if (!item.u8(transit) || !item.zeroes(3)) {
			return item.error();
		}
		if (transit < static_cast<std::uint8_t>(TransitMode::Warp) ||
			transit > static_cast<std::uint8_t>(TransitMode::Scripted)) {
			return ValidationError::UnknownEnum;
		}
	}
	return item.finish();
}

} // namespace

ValidationError validate_business_record_28(ByteView payload, bool reliable_delivery) noexcept
{
	EventReader reader(payload);
	std::uint16_t count = 0;
	if (!reader.u16(count)) {
		return reader.error();
	}
	if (count == 0 || count > 256) {
		return ValidationError::OutOfRange;
	}
	std::uint64_t previous_event_id = 0;
	for (std::size_t index = 0; index < count; ++index) {
		std::uint8_t version = 0;
		std::uint16_t size = 0;
		if (!reader.u8(version) || !reader.u16(size)) {
			return reader.error();
		}
		if (version != 1) {
			return ValidationError::UnsupportedRecordVersion;
		}
		EventReader item(ByteView{});
		if (!reader.child(size, item)) {
			return reader.error();
		}
		std::uint64_t event_id = 0;
		if (const auto error = validate_event_item(item, reliable_delivery, previous_event_id, event_id);
			error != ValidationError::None) {
			return error;
		}
		previous_event_id = event_id;
	}
	return reader.finish();
}

} // namespace telemetry::protocol::detail
