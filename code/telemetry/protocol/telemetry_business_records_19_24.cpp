#include "telemetry/protocol/telemetry_business_records_internal.h"

#include "telemetry/protocol/packet_reader.h"
#include "telemetry/protocol/packet_writer.h"
#include "telemetry/protocol/telemetry_protocol_constants.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

namespace telemetry::protocol::detail {

namespace {

constexpr float Pi = 3.14159265358979323846F;
constexpr float PositionLimit = 1.0e12F;
constexpr float VelocityLimit = 1.0e9F;
constexpr float QuantityLimit = 1.0e12F;
constexpr std::uint64_t DayUs = 86'400'000'000ULL;

class Validator final {
  public:
	explicit Validator(ByteView input) noexcept : m_reader(input)
	{
		if (input.size != 0 && input.data == nullptr) {
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

	std::size_t remaining() const noexcept
	{
		return m_reader.remaining();
	}

	ValidationError finish() noexcept
	{
		if (m_error != ValidationError::None) {
			return m_error;
		}
		if (!m_reader.at_end()) {
			fail(ValidationError::TrailingBytes);
		}
		return m_error;
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

	bool zeroes(std::size_t count) noexcept
	{
		ByteView bytes;
		if (!ok() || (!m_reader.read_bytes(count, bytes) && !fail(ValidationError::TruncatedPayload))) {
			return false;
		}
		for (std::size_t index = 0; index < bytes.size; ++index) {
			if (bytes.data[index] != 0) {
				return fail(ValidationError::ReservedFlag);
			}
		}
		return true;
	}

	bool bytes(std::size_t count, ByteView& value) noexcept
	{
		return ok() && (m_reader.read_bytes(count, value) || fail(ValidationError::TruncatedPayload));
	}

	bool subreader(std::size_t count, Validator& value) noexcept
	{
		PacketReader child;
		if (!ok() || !m_reader.subreader(count, child)) {
			return fail(ValidationError::TruncatedPayload);
		}
		value = Validator(child.unread());
		return true;
	}

	bool f32(float& value, float minimum, float maximum) noexcept
	{
		std::uint32_t bits = 0;
		if (!u32(bits)) {
			return false;
		}
		float candidate = 0.0F;
		std::memcpy(&candidate, &bits, sizeof(candidate));
		if (!std::isfinite(candidate)) {
			return fail(ValidationError::NonFiniteFloat);
		}
		candidate = candidate == 0.0F ? 0.0F : candidate;
		if (candidate < minimum || candidate > maximum) {
			return fail(ValidationError::OutOfRange);
		}
		value = candidate;
		return true;
	}

	bool string(std::size_t minimum, std::size_t maximum, std::string_view& value) noexcept
	{
		std::uint16_t length = 0;
		if (!u16(length)) {
			return false;
		}
		if (length > maximum) {
			return fail(ValidationError::StringTooLong);
		}
		if (length < minimum) {
			return fail(ValidationError::OutOfRange);
		}
		ByteView bytes_view;
		if (!bytes(length, bytes_view)) {
			return false;
		}
		const auto candidate = std::string_view(
			static_cast<const char*>(static_cast<const void*>(bytes_view.data)), bytes_view.size);
		if (!is_valid_utf8(candidate)) {
			return fail(ValidationError::InvalidUtf8);
		}
		value = candidate;
		return true;
	}

  private:
	PacketReader m_reader;
	ValidationError m_error = ValidationError::None;
};

bool id64(Validator& reader, std::uint64_t& value, bool zero_allowed = false) noexcept
{
	return reader.u64(value) && (zero_allowed || value != 0 || reader.fail(ValidationError::OutOfRange));
}

bool id32(Validator& reader, std::uint32_t& value, bool zero_allowed = false) noexcept
{
	return reader.u32(value) && (zero_allowed || value != 0 || reader.fail(ValidationError::OutOfRange));
}

bool presence64(Validator& reader, std::uint64_t known, std::uint64_t& value) noexcept
{
	return reader.u64(value) && ((value & ~known) == 0 || reader.fail(ValidationError::ReservedFlag));
}

bool enum8(Validator& reader, std::uint8_t minimum, std::uint8_t maximum, std::uint8_t& value) noexcept
{
	return reader.u8(value) &&
		   (value >= minimum && value <= maximum || reader.fail(ValidationError::UnknownEnum));
}

bool flags8(Validator& reader, std::uint8_t known, std::uint8_t& value) noexcept
{
	return reader.u8(value) && ((value & static_cast<std::uint8_t>(~known)) == 0 ||
							   reader.fail(ValidationError::ReservedFlag));
}

bool flags32(Validator& reader, std::uint32_t known, std::uint32_t& value) noexcept
{
	return reader.u32(value) && ((value & ~known) == 0 || reader.fail(ValidationError::ReservedFlag));
}

bool vec3(Validator& reader, float minimum, float maximum) noexcept
{
	float value = 0.0F;
	return reader.f32(value, minimum, maximum) && reader.f32(value, minimum, maximum) &&
		   reader.f32(value, minimum, maximum);
}

bool quaternion(Validator& reader) noexcept
{
	std::array<float, 4> values{};
	for (auto& value : values) {
		if (!reader.f32(value, -1.0001F, 1.0001F)) {
			return false;
		}
	}
	const auto norm_squared = values[0] * values[0] + values[1] * values[1] + values[2] * values[2] +
						  values[3] * values[3];
	if (norm_squared < 0.9999F * 0.9999F || norm_squared > 1.0001F * 1.0001F) {
		return reader.fail(ValidationError::OutOfRange);
	}
	if (values[0] < 0.0F) {
		return reader.fail(ValidationError::OutOfRange);
	}
	if (values[0] == 0.0F) {
		for (std::size_t index = 1; index < values.size(); ++index) {
			if (values[index] != 0.0F) {
				return values[index] > 0.0F || reader.fail(ValidationError::OutOfRange);
			}
		}
	}
	return true;
}

template <typename Key, std::size_t Size>
bool insert_unique(Validator& reader, std::array<Key, Size>& values, std::size_t count, const Key& value) noexcept
{
	for (std::size_t index = 0; index < count; ++index) {
		if (values[index] == value) {
			return reader.fail(ValidationError::DuplicateItemKey);
		}
	}
	values[count] = value;
	return true;
}

bool vlist_item(Validator& reader, Validator& item) noexcept
{
	std::uint8_t version = 0;
	std::uint16_t size = 0;
	if (!reader.u8(version) || !reader.u16(size)) {
		return false;
	}
	if (version != 1) {
		return reader.fail(ValidationError::UnsupportedRecordVersion);
	}
	return reader.subreader(size, item);
}

ValidationError validate_threat_state(ByteView payload) noexcept
{
	Validator reader(payload);
	std::uint64_t entity = 0;
	std::uint64_t presence = 0;
	std::uint64_t sample = 0;
	std::uint8_t threat = 0;
	if (!id64(reader, entity) || !presence64(reader, KnownThreatStatePresenceFlags, presence) ||
		!reader.u64(sample) || !enum8(reader, 0, 3, threat)) {
		return reader.error();
	}
	std::uint64_t optional_entity = 0;
	if ((presence & ThreatStatePresenceFlagNearestAttacker) != 0 && !id64(reader, optional_entity)) {
		return reader.error();
	}
	if ((presence & ThreatStatePresenceFlagDangerousWeapon) != 0 && !id64(reader, optional_entity)) {
		return reader.error();
	}
	if ((presence & ThreatStatePresenceFlagNearestHoming) != 0 && !id64(reader, optional_entity)) {
		return reader.error();
	}

	std::uint16_t count = 0;
	if (!reader.u16(count)) {
		return reader.error();
	}
	if (count > 256) {
		return ValidationError::OutOfRange;
	}
	std::array<std::uint64_t, 256> missile_ids{};
	for (std::size_t index = 0; index < count; ++index) {
		Validator item(ByteView{});
		if (!vlist_item(reader, item)) {
			return reader.error();
		}
		std::uint16_t item_presence = 0;
		std::uint8_t guidance = 0;
		std::uint8_t visibility = 0;
		std::uint64_t missile = 0;
		std::uint32_t weapon_class = 0;
		std::uint64_t target = 0;
		if (!item.u16(item_presence) || (item_presence & ReservedIncomingMissilePresenceFlags) != 0 ||
			!enum8(item, 0, 5, guidance) || !enum8(item, 0, 2, visibility) || !id64(item, missile) ||
			!id32(item, weapon_class) || !id64(item, target)) {
			if (item.ok() && (item_presence & ReservedIncomingMissilePresenceFlags) != 0) {
				item.fail(ValidationError::ReservedFlag);
			}
			return item.error();
		}
		if (target != entity) {
			return ValidationError::InvalidStateTransition;
		}
		if ((item_presence & IncomingMissilePresenceFlagHomingSubsystem) != 0) {
			std::uint32_t subsystem = 0;
			if (!id32(item, subsystem)) {
				return item.error();
			}
		}
		if (!vec3(item, -PositionLimit, PositionLimit) || !quaternion(item) ||
			!vec3(item, -VelocityLimit, VelocityLimit)) {
			return item.error();
		}
		if (const auto error = item.finish(); error != ValidationError::None) {
			return error;
		}
		if (!insert_unique(reader, missile_ids, index, missile)) {
			return reader.error();
		}
	}
	return reader.finish();
}

ValidationError validate_cargo_scan_state(ByteView payload) noexcept
{
	Validator reader(payload);
	std::uint64_t entity = 0;
	std::uint64_t presence = 0;
	std::uint64_t sample = 0;
	std::uint8_t phase = 0;
	std::uint8_t disclosure = 0;
	if (!id64(reader, entity) || !presence64(reader, KnownCargoScanStatePresenceFlags, presence) ||
		!reader.u64(sample) || !enum8(reader, 0, 3, phase) || !enum8(reader, 0, 1, disclosure)) {
		return reader.error();
	}
	if ((presence & CargoScanStatePresenceFlagSubsystem) != 0 &&
		(presence & CargoScanStatePresenceFlagTarget) == 0) {
		return ValidationError::InvalidAbsence;
	}
	std::uint64_t target = 0;
	if ((presence & CargoScanStatePresenceFlagTarget) != 0 && !id64(reader, target)) {
		return reader.error();
	}
	if ((presence & CargoScanStatePresenceFlagSubsystem) != 0) {
		std::uint32_t subsystem = 0;
		if (!id32(reader, subsystem)) {
			return reader.error();
		}
	}
	if ((presence & CargoScanStatePresenceFlagTiming) != 0) {
		std::uint64_t elapsed = 0;
		std::uint64_t required = 0;
		if (!reader.u64(elapsed) || !reader.u64(required)) {
			return reader.error();
		}
		if (elapsed > DayUs || required == 0 || required > DayUs) {
			return ValidationError::OutOfRange;
		}
	}
	if ((presence & CargoScanStatePresenceFlagValidity) != 0) {
		std::uint8_t validity = 0;
		if (!flags8(reader, KnownScanValidityFlags, validity)) {
			return reader.error();
		}
	}
	const bool has_text = (presence & CargoScanStatePresenceFlagCargoText) != 0;
	if (has_text != (disclosure == static_cast<std::uint8_t>(DisclosureState::Revealed))) {
		return ValidationError::InvalidAbsence;
	}
	if (has_text) {
		std::string_view text;
		if (!reader.string(1, 511, text)) {
			return reader.error();
		}
	}
	return reader.finish();
}

struct DockingKey {
	std::uint64_t remote = 0;
	std::uint16_t local_point = 0;
	std::uint16_t remote_point = 0;

	friend bool operator==(const DockingKey& left, const DockingKey& right) noexcept
	{
		return left.remote == right.remote && left.local_point == right.local_point &&
			   left.remote_point == right.remote_point;
	}
};

ValidationError validate_docking_state(ByteView payload) noexcept
{
	Validator reader(payload);
	std::uint64_t entity = 0;
	std::uint64_t presence = 0;
	std::uint64_t sample = 0;
	std::uint8_t phase = 0;
	std::uint64_t leader = 0;
	if (!id64(reader, entity) || !presence64(reader, KnownDockingStatePresenceFlags, presence) ||
		!reader.u64(sample) || !enum8(reader, 0, 4, phase) || !id64(reader, leader, true)) {
		return reader.error();
	}
	std::uint16_t count = 0;
	if (!reader.u16(count)) {
		return reader.error();
	}
	if (count > 64) {
		return ValidationError::OutOfRange;
	}
	std::array<DockingKey, 64> keys{};
	for (std::size_t index = 0; index < count; ++index) {
		Validator item(ByteView{});
		if (!vlist_item(reader, item)) {
			return reader.error();
		}
		DockingKey key;
		std::string_view local_name;
		std::string_view remote_name;
		if (!id64(item, key.remote) || !item.u16(key.local_point) || !item.u16(key.remote_point) ||
			key.local_point > 4095 || key.remote_point > 4095 || !item.string(0, 127, local_name) ||
			!item.string(0, 127, remote_name)) {
			if (item.ok() && (key.local_point > 4095 || key.remote_point > 4095)) {
				item.fail(ValidationError::OutOfRange);
			}
			return item.error();
		}
		if (const auto error = item.finish(); error != ValidationError::None) {
			return error;
		}
		if (!insert_unique(reader, keys, index, key)) {
			return reader.error();
		}
	}
	return reader.finish();
}

ValidationError validate_support_state(ByteView payload) noexcept
{
	Validator reader(payload);
	std::uint64_t entity = 0;
	std::uint64_t presence = 0;
	std::uint64_t sample = 0;
	std::uint8_t phase = 0;
	std::uint8_t flags = 0;
	if (!id64(reader, entity) || !presence64(reader, KnownSupportStatePresenceFlags, presence) ||
		!reader.u64(sample) || !enum8(reader, 0, 7, phase) || !flags8(reader, KnownSupportFlags, flags) ||
		!reader.zeroes(3)) {
		return reader.error();
	}
	if ((presence & SupportStatePresenceFlagSupportEntity) != 0) {
		std::uint64_t support = 0;
		if (!id64(reader, support)) {
			return reader.error();
		}
	}
	return reader.finish();
}

bool validate_navpoint(Validator& item, std::uint32_t& navpoint_id) noexcept
{
	std::uint16_t presence = 0;
	std::uint8_t type = 0;
	std::uint8_t flags = 0;
	std::string_view name;
	if (!item.u16(presence)) {
		return false;
	}
	if ((presence & ReservedNavPointPresenceFlags) != 0) {
		return item.fail(ValidationError::ReservedFlag);
	}
	if (!enum8(item, 0, 2, type) || !flags8(item, KnownNavPointFlags, flags) || !id32(item, navpoint_id) ||
		!item.string(1, 255, name) || !vec3(item, -PositionLimit, PositionLimit)) {
		return false;
	}
	const auto entity_link = (presence & NavPointPresenceFlagEntityLink) != 0;
	const auto waypoint_link = (presence & NavPointPresenceFlagWaypointLink) != 0;
	if (entity_link == waypoint_link && (entity_link || type != static_cast<std::uint8_t>(NavPointType::Position))) {
		return item.fail(ValidationError::InvalidStateTransition);
	}
	if ((type == static_cast<std::uint8_t>(NavPointType::Entity)) != entity_link ||
		(type == static_cast<std::uint8_t>(NavPointType::Waypoint)) != waypoint_link) {
		return item.fail(ValidationError::InvalidStateTransition);
	}
	if (entity_link) {
		std::uint64_t linked = 0;
		if (!id64(item, linked)) {
			return false;
		}
	}
	if (waypoint_link) {
		std::uint32_t list_id = 0;
		std::uint16_t waypoint_index = 0;
		if (!id32(item, list_id) || !item.u16(waypoint_index)) {
			return false;
		}
	}
	return item.finish() == ValidationError::None;
}

ValidationError validate_navigation_state(ByteView payload) noexcept
{
	Validator reader(payload);
	std::uint64_t entity = 0;
	std::uint64_t presence = 0;
	std::uint64_t sample = 0;
	std::uint8_t autopilot = 0;
	if (!id64(reader, entity) || !presence64(reader, KnownNavigationStatePresenceFlags, presence) ||
		!reader.u64(sample) || !enum8(reader, 0, 3, autopilot)) {
		return reader.error();
	}
	std::uint16_t count = 0;
	if (!reader.u16(count)) {
		return reader.error();
	}
	if (count > 1024) {
		return ValidationError::OutOfRange;
	}
	std::array<std::uint32_t, 1024> navpoint_ids{};
	for (std::size_t index = 0; index < count; ++index) {
		Validator item(ByteView{});
		if (!vlist_item(reader, item)) {
			return reader.error();
		}
		std::uint32_t navpoint_id = 0;
		if (!validate_navpoint(item, navpoint_id)) {
			return item.error();
		}
		if (!insert_unique(reader, navpoint_ids, index, navpoint_id)) {
			return reader.error();
		}
	}
	if ((presence & NavigationStatePresenceFlagCurrentNavpoint) != 0) {
		std::uint32_t current = 0;
		if (!id32(reader, current)) {
			return reader.error();
		}
		bool found = false;
		for (std::size_t index = 0; index < count; ++index) {
			found = found || navpoint_ids[index] == current;
		}
		if (!found) {
			return ValidationError::InvalidStateTransition;
		}
	}
	const bool has_refusal = (presence & NavigationStatePresenceFlagAutopilotRefusal) != 0;
	if (has_refusal != (autopilot == static_cast<std::uint8_t>(AutopilotState::Refused))) {
		return ValidationError::InvalidAbsence;
	}
	if (has_refusal) {
		std::uint8_t refusal = 0;
		if (!enum8(reader, 1, 6, refusal)) {
			return reader.error();
		}
	}
	if ((presence & NavigationStatePresenceFlagWaypointRoute) != 0) {
		std::uint16_t route_count = 0;
		std::uint8_t item_version = 0;
		std::uint16_t item_size = 0;
		if (!reader.u16(route_count) || !reader.u8(item_version) || !reader.u16(item_size)) {
			return reader.error();
		}
		if (route_count == 0 || route_count > 2048 || item_version != 1 || item_size != 20) {
			return item_version != 1 ? ValidationError::UnsupportedRecordVersion : ValidationError::OutOfRange;
		}
		for (std::size_t index = 0; index < route_count; ++index) {
			std::uint32_t list_id = 0;
			std::uint16_t waypoint_index = 0;
			std::uint16_t reserved = 0;
			if (!id32(reader, list_id) || !reader.u16(waypoint_index) || !reader.u16(reserved) ||
				!vec3(reader, -PositionLimit, PositionLimit)) {
				return reader.error();
			}
			if (reserved != 0) {
				return ValidationError::ReservedFlag;
			}
		}
		std::uint16_t current_index = 0;
		float speed = 0.0F;
		if (!reader.u16(current_index) || !reader.f32(speed, 0.0F, VelocityLimit)) {
			return reader.error();
		}
		if (current_index >= route_count) {
			return ValidationError::OutOfRange;
		}
	}
	return reader.finish();
}

template <std::size_t Maximum>
bool validate_scalar_list(Validator& reader) noexcept
{
	std::uint16_t count = 0;
	std::uint8_t version = 0;
	std::uint16_t size = 0;
	if (!reader.u16(count) || !reader.u8(version) || !reader.u16(size)) {
		return false;
	}
	if (count > Maximum || version != 1 || size != 8) {
		return reader.fail(version != 1 ? ValidationError::UnsupportedRecordVersion : ValidationError::OutOfRange);
	}
	std::array<std::uint16_t, Maximum> ids{};
	for (std::size_t index = 0; index < count; ++index) {
		std::uint16_t id = 0;
		std::uint16_t reserved = 0;
		float value = 0.0F;
		if (!reader.u16(id) || !reader.u16(reserved) || !reader.f32(value, 0.0F, 1.0F)) {
			return false;
		}
		if (id == 0) {
			return reader.fail(ValidationError::OutOfRange);
		}
		if (reserved != 0) {
			return reader.fail(ValidationError::ReservedFlag);
		}
		if (!insert_unique(reader, ids, index, id)) {
			return false;
		}
	}
	return true;
}

ValidationError validate_effect_state(ByteView payload) noexcept
{
	Validator reader(payload);
	std::uint64_t entity = 0;
	std::uint64_t presence = 0;
	std::uint64_t sample = 0;
	std::uint32_t effect_flags = 0;
	if (!id64(reader, entity) || !presence64(reader, KnownEffectStatePresenceFlags, presence) ||
		!reader.u64(sample) || !flags32(reader, KnownEffectFlags, effect_flags)) {
		return reader.error();
	}
	if ((presence & EffectStatePresenceFlagEmpVisual) != 0) {
		float intensity = 0.0F;
		std::uint64_t remaining = 0;
		if (!reader.f32(intensity, 0.0F, 1.0F) || !reader.u64(remaining)) {
			return reader.error();
		}
		if (remaining > DayUs) {
			return ValidationError::OutOfRange;
		}
	}
	if ((presence & EffectStatePresenceFlagTags) != 0) {
		std::uint16_t count = 0;
		std::uint8_t version = 0;
		std::uint16_t size = 0;
		if (!reader.u16(count) || !reader.u8(version) || !reader.u16(size)) {
			return reader.error();
		}
		if (count > 64 || version != 1 || size != 16) {
			return version != 1 ? ValidationError::UnsupportedRecordVersion : ValidationError::OutOfRange;
		}
		std::array<std::uint32_t, 64> ids{};
		for (std::size_t index = 0; index < count; ++index) {
			std::uint32_t tag = 0;
			std::uint64_t remaining = 0;
			float intensity = 0.0F;
			if (!id32(reader, tag) || !reader.u64(remaining) || !reader.f32(intensity, 0.0F, QuantityLimit)) {
				return reader.error();
			}
			if (remaining > DayUs) {
				return ValidationError::OutOfRange;
			}
			if (!insert_unique(reader, ids, index, tag)) {
				return reader.error();
			}
		}
	}
	if ((presence & EffectStatePresenceFlagRcs) != 0 && !vec3(reader, -1.0F, 1.0F)) {
		return reader.error();
	}
	if ((presence & EffectStatePresenceFlagBayDoors) != 0 && !validate_scalar_list<64>(reader)) {
		return reader.error();
	}
	if ((presence & EffectStatePresenceFlagGlowBanks) != 0 && !validate_scalar_list<256>(reader)) {
		return reader.error();
	}
	if ((presence & EffectStatePresenceFlagThrusters) != 0) {
		float intensity = 0.0F;
		for (std::size_t index = 0; index < 6; ++index) {
			if (!reader.f32(intensity, 0.0F, 1.0F)) {
				return reader.error();
			}
		}
	}
	if ((presence & EffectStatePresenceFlagTeamColors) != 0 && !reader.zeroes(0)) {
		return reader.error();
	}
	if ((presence & EffectStatePresenceFlagTeamColors) != 0) {
		ByteView colors;
		if (!reader.bytes(8, colors)) {
			return reader.error();
		}
	}
	if ((presence & EffectStatePresenceFlagAutoaim) != 0) {
		float fov = 0.0F;
		if (!reader.f32(fov, 0.0F, Pi)) {
			return reader.error();
		}
	}
	if ((presence & EffectStatePresenceFlagTargetingLaserVisual) != 0) {
		float intensity = 0.0F;
		if ((effect_flags & EffectFlagTargetingLaser) == 0) {
			return ValidationError::InvalidStateTransition;
		}
		if (!reader.f32(intensity, 0.0F, 1.0F)) {
			return reader.error();
		}
	}
	return reader.finish();
}

} // namespace

ValidationError validate_business_record_19_24(RecordType type, ByteView payload) noexcept
{
	switch (type) {
	case RecordType::ThreatState:
		return validate_threat_state(payload);
	case RecordType::CargoScanState:
		return validate_cargo_scan_state(payload);
	case RecordType::DockingState:
		return validate_docking_state(payload);
	case RecordType::SupportState:
		return validate_support_state(payload);
	case RecordType::NavigationState:
		return validate_navigation_state(payload);
	case RecordType::EffectState:
		return validate_effect_state(payload);
	default:
		return ValidationError::UnknownRequiredRecord;
	}
}

} // namespace telemetry::protocol::detail
