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

constexpr float Pi = 3.14159265358979323846F;
constexpr float PositionLimit = 1.0e12F;
constexpr float VelocityLimit = 1.0e9F;
constexpr float QuantityLimit = 1.0e12F;
constexpr std::uint64_t HourUs = 3'600'000'000ULL;
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

	ValidationError finish() noexcept
	{
		if (!ok()) {
			return m_error;
		}
		return m_reader.at_end() ? ValidationError::None : ValidationError::TrailingBytes;
	}

	bool fail(ValidationError error) noexcept
	{
		if (ok()) {
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

	bool i16(std::int16_t& value) noexcept
	{
		return ok() && (m_reader.read_i16(value) || fail(ValidationError::TruncatedPayload));
	}

	bool u32(std::uint32_t& value) noexcept
	{
		return ok() && (m_reader.read_u32(value) || fail(ValidationError::TruncatedPayload));
	}

	bool u64(std::uint64_t& value) noexcept
	{
		return ok() && (m_reader.read_u64(value) || fail(ValidationError::TruncatedPayload));
	}

	bool boolean(bool& value) noexcept
	{
		std::uint8_t raw = 0;
		if (!u8(raw)) {
			return false;
		}
		if (raw > 1U) {
			return fail(ValidationError::OutOfRange);
		}
		value = raw != 0;
		return true;
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
		static_assert(sizeof(candidate) == sizeof(bits), "FSTL requires IEEE-754 binary32 floats");
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
		ByteView bytes;
		if (!m_reader.read_bytes(length, bytes)) {
			return fail(ValidationError::TruncatedPayload);
		}
		const auto candidate = bytes.size == 0 ? std::string_view{} :
			std::string_view(static_cast<const char*>(static_cast<const void*>(bytes.data)), bytes.size);
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

bool enum8(Validator& reader, std::uint8_t maximum, std::uint8_t& value) noexcept
{
	return reader.u8(value) && (value <= maximum || reader.fail(ValidationError::UnknownEnum));
}

bool flags16(Validator& reader, std::uint16_t known, std::uint16_t& value) noexcept
{
	return reader.u16(value) && ((value & static_cast<std::uint16_t>(~known)) == 0 ||
								 reader.fail(ValidationError::ReservedFlag));
}

bool flags32(Validator& reader, std::uint32_t known, std::uint32_t& value) noexcept
{
	return reader.u32(value) && ((value & ~known) == 0 || reader.fail(ValidationError::ReservedFlag));
}

bool duration(Validator& reader, std::uint64_t maximum = DayUs) noexcept
{
	std::uint64_t value = 0;
	return reader.u64(value) && (value <= maximum || reader.fail(ValidationError::OutOfRange));
}

bool vec3(Validator& reader, float minimum, float maximum, std::array<float, 3>* output = nullptr) noexcept
{
	std::array<float, 3> values{};
	for (auto& value : values) {
		if (!reader.f32(value, minimum, maximum)) {
			return false;
		}
	}
	if (output != nullptr) {
		*output = values;
	}
	return true;
}

bool normalized_vec3(Validator& reader) noexcept
{
	std::array<float, 3> values{};
	if (!vec3(reader, -1.001F, 1.001F, &values)) {
		return false;
	}
	const auto norm_squared =
		values[0] * values[0] + values[1] * values[1] + values[2] * values[2];
	return (norm_squared >= 0.999F * 0.999F && norm_squared <= 1.001F * 1.001F) ||
		   reader.fail(ValidationError::OutOfRange);
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

template <typename Key, std::size_t Size>
bool contains(const std::array<Key, Size>& values, std::size_t count, const Key& value) noexcept
{
	for (std::size_t index = 0; index < count; ++index) {
		if (values[index] == value) {
			return true;
		}
	}
	return false;
}

bool vlist_item(Validator& reader, Validator& item) noexcept
{
	std::uint8_t version = 0;
	std::uint16_t size = 0;
	if (!reader.u8(version) || !reader.u16(size)) {
		return false;
	}
	if (version != 1U) {
		return reader.fail(ValidationError::UnsupportedRecordVersion);
	}
	return reader.subreader(size, item);
}

bool finish_item(Validator& item) noexcept
{
	const auto error = item.finish();
	return error == ValidationError::None || item.fail(error);
}

bool validate_animation_item(Validator& item, std::uint16_t& animation_id) noexcept
{
	std::uint16_t presence = 0;
	std::uint8_t state = 0;
	std::uint8_t reserved = 0;
	if (!item.u16(presence)) {
		return false;
	}
	if ((presence & ReservedSubsystemAnimationPresenceFlags) != 0) {
		return item.fail(ValidationError::ReservedFlag);
	}
	const bool has_angle = (presence & SubsystemAnimationPresenceFlagAngle) != 0;
	const bool has_translation = (presence & SubsystemAnimationPresenceFlagTranslation) != 0;
	if (!has_angle && !has_translation) {
		return item.fail(ValidationError::InvalidAbsence);
	}
	if (!item.u16(animation_id) || !enum8(item, 4, state) || !item.u8(reserved)) {
		return false;
	}
	if (reserved != 0) {
		return item.fail(ValidationError::ReservedFlag);
	}
	float value = 0.0F;
	if (has_angle &&
		(!item.f32(value, -1.0e6F, 1.0e6F) || !item.f32(value, -1.0e6F, 1.0e6F))) {
		return false;
	}
	if (has_translation &&
		(!item.f32(value, -VelocityLimit, VelocityLimit) ||
			!item.f32(value, -VelocityLimit, VelocityLimit))) {
		return false;
	}
	if ((presence & SubsystemAnimationPresenceFlagTarget) != 0) {
		if (has_angle && !item.f32(value, -1.0e6F, 1.0e6F)) {
			return false;
		}
		if (has_translation && !item.f32(value, -VelocityLimit, VelocityLimit)) {
			return false;
		}
	}
	return duration(item) && finish_item(item);
}

bool validate_turret_bank_item(Validator& item, std::uint32_t& bank_id) noexcept
{
	std::uint16_t presence = 0;
	std::uint8_t family = 0;
	std::uint8_t reserved8 = 0;
	std::uint16_t bank_index = 0;
	std::uint32_t weapon_class = 0;
	if (!item.u16(presence)) {
		return false;
	}
	if ((presence & ReservedTurretBankPresenceFlags) != 0) {
		return item.fail(ValidationError::ReservedFlag);
	}
	if (!enum8(item, static_cast<std::uint8_t>(WeaponFamily::Secondary), family) || !item.u8(reserved8) ||
		!item.u16(bank_index) || !id32(item, bank_id) || !id32(item, weapon_class)) {
		return false;
	}
	if (reserved8 != 0) {
		return item.fail(ValidationError::ReservedFlag);
	}
	if (bank_index > 63U) {
		return item.fail(ValidationError::OutOfRange);
	}
	if ((presence & TurretBankPresenceFlagAmmo) != 0) {
		std::uint32_t ammo = 0;
		float capacity = 0.0F;
		if (!item.u32(ammo) || ammo > 1'000'000'000U || !item.f32(capacity, 0.0F, QuantityLimit)) {
			return item.ok() ? item.fail(ValidationError::OutOfRange) : false;
		}
	}
	return duration(item) && finish_item(item);
}

bool validate_turret(Validator& reader) noexcept
{
	std::uint32_t presence = 0;
	std::uint64_t target_entity = 0;
	if (!reader.u32(presence)) {
		return false;
	}
	if ((presence & ReservedTurretStatePresenceFlags) != 0) {
		return reader.fail(ValidationError::ReservedFlag);
	}
	if (!id64(reader, target_entity, true)) {
		return false;
	}
	if ((presence & TurretStatePresenceFlagTargetSubsystem) != 0) {
		std::uint32_t subsystem = 0;
		if (target_entity == 0) {
			return reader.fail(ValidationError::InvalidAbsence);
		}
		if (!id32(reader, subsystem)) {
			return false;
		}
	}
	if (!normalized_vec3(reader)) {
		return false;
	}
	if ((presence & TurretStatePresenceFlagAimPoint) != 0 &&
		(!vec3(reader, -PositionLimit, PositionLimit) || !vec3(reader, -VelocityLimit, VelocityLimit))) {
		return false;
	}
	if ((presence & TurretStatePresenceFlagNextFirePoint) != 0) {
		std::uint16_t point = 0;
		std::uint16_t reserved = 0;
		if (!reader.u16(point) || !reader.u16(reserved)) {
			return false;
		}
		if (point > 255U) {
			return reader.fail(ValidationError::OutOfRange);
		}
		if (reserved != 0) {
			return reader.fail(ValidationError::ReservedFlag);
		}
	}
	if ((presence & TurretStatePresenceFlagCooldown) != 0 && !duration(reader)) {
		return false;
	}
	if ((presence & TurretStatePresenceFlagTimeInRange) != 0 && !duration(reader)) {
		return false;
	}
	float value = 0.0F;
	if ((presence & TurretStatePresenceFlagOptimalRange) != 0 &&
		!reader.f32(value, 0.0F, QuantityLimit)) {
		return false;
	}
	if ((presence & TurretStatePresenceFlagTargetPriority) != 0) {
		std::int16_t priority = 0;
		std::uint16_t reserved = 0;
		if (!reader.i16(priority) || !reader.u16(reserved)) {
			return false;
		}
		if (reserved != 0) {
			return reader.fail(ValidationError::ReservedFlag);
		}
	}
	if ((presence & TurretStatePresenceFlagInaccuracy) != 0 && !reader.f32(value, 0.0F, Pi)) {
		return false;
	}
	if ((presence & TurretStatePresenceFlagRateMultiplier) != 0 &&
		!reader.f32(value, 0.0F, 1024.0F)) {
		return false;
	}
	if ((presence & TurretStatePresenceFlagAnimation) != 0) {
		std::uint8_t state = 0;
		if (!enum8(reader, 4, state) || !duration(reader)) {
			return false;
		}
	}
	if ((presence & TurretStatePresenceFlagBanks) != 0) {
		std::uint16_t count = 0;
		if (!reader.u16(count)) {
			return false;
		}
		if (count > 64U) {
			return reader.fail(ValidationError::OutOfRange);
		}
		std::array<std::uint32_t, 64> bank_ids{};
		for (std::size_t index = 0; index < count; ++index) {
			Validator item(ByteView{});
			if (!vlist_item(reader, item)) {
				return false;
			}
			std::uint32_t bank_id = 0;
			if (!validate_turret_bank_item(item, bank_id)) {
				return reader.fail(item.error());
			}
			if (!insert_unique(reader, bank_ids, index, bank_id)) {
				return false;
			}
		}
	}
	if ((presence & TurretStatePresenceFlagSwarm) != 0) {
		std::uint16_t remaining = 0;
		std::uint32_t bank_id = 0;
		if (!reader.u16(remaining) || !id32(reader, bank_id)) {
			return false;
		}
		if (remaining > 4096U) {
			return reader.fail(ValidationError::OutOfRange);
		}
	}
	if ((presence & TurretStatePresenceFlagAwacs) != 0 &&
		(!reader.f32(value, 0.0F, QuantityLimit) || !reader.f32(value, 0.0F, QuantityLimit))) {
		return false;
	}
	return true;
}

ValidationError validate_subsystem_state(ByteView payload) noexcept
{
	Validator reader(payload);
	std::uint64_t entity = 0;
	std::uint32_t subsystem = 0;
	std::uint64_t presence = 0;
	std::uint64_t sample = 0;
	std::uint16_t canonical_index = 0;
	std::uint8_t type = 0;
	float current_hits = 0.0F;
	float max_hits = 0.0F;
	std::uint32_t flags = 0;
	if (!id64(reader, entity) || !id32(reader, subsystem) ||
		!presence64(reader, KnownSubsystemStatePresenceFlags, presence) || !reader.u64(sample) ||
		!reader.u16(canonical_index) || !enum8(reader, 13, type) ||
		!reader.f32(current_hits, 0.0F, QuantityLimit) || !reader.f32(max_hits, 0.0F, QuantityLimit) ||
		!flags32(reader, KnownSubsystemFlags, flags)) {
		return reader.error();
	}
	if (canonical_index > 1023U || current_hits > max_hits) {
		return ValidationError::OutOfRange;
	}
	if ((presence & SubsystemStatePresenceFlagNameOverrides) != 0) {
		std::string_view internal_name;
		std::string_view alternate_name;
		std::string_view hud_name;
		if (!reader.string(1, 255, internal_name) || !reader.string(0, 255, alternate_name) ||
			!reader.string(0, 255, hud_name)) {
			return reader.error();
		}
	}
	if ((presence & SubsystemStatePresenceFlagArmor) != 0) {
		std::uint32_t armor = 0;
		if (!id32(reader, armor)) {
			return reader.error();
		}
	}
	const bool has_perturbation = (presence & SubsystemStatePresenceFlagPerturbation) != 0;
	const bool is_perturbed = (flags & SubsystemFlagPerturbed) != 0;
	if (has_perturbation != is_perturbed) {
		return ValidationError::InvalidAbsence;
	}
	if (has_perturbation && !duration(reader)) {
		return reader.error();
	}
	if ((presence & SubsystemStatePresenceFlagAnimatedTransform) != 0 &&
		(!vec3(reader, -PositionLimit, PositionLimit) || !quaternion(reader))) {
		return reader.error();
	}
	if ((presence & SubsystemStatePresenceFlagAnimations) != 0) {
		std::uint16_t count = 0;
		if (!reader.u16(count)) {
			return reader.error();
		}
		if (count > 64U) {
			return ValidationError::OutOfRange;
		}
		std::array<std::uint16_t, 64> animation_ids{};
		for (std::size_t index = 0; index < count; ++index) {
			Validator item(ByteView{});
			if (!vlist_item(reader, item)) {
				return reader.error();
			}
			std::uint16_t animation_id = 0;
			if (!validate_animation_item(item, animation_id)) {
				return item.error();
			}
			if (!insert_unique(reader, animation_ids, index, animation_id)) {
				return reader.error();
			}
		}
	}
	if ((presence & SubsystemStatePresenceFlagLocalCargo) != 0) {
		std::uint8_t disclosure = 0;
		std::string_view cargo_text;
		if (!enum8(reader, 1, disclosure) || !reader.string(0, 511, cargo_text)) {
			return reader.error();
		}
		if (disclosure == static_cast<std::uint8_t>(DisclosureState::Hidden) && !cargo_text.empty()) {
			return ValidationError::VisibilityViolation;
		}
	}
	if ((presence & SubsystemStatePresenceFlagTypeAggregate) != 0) {
		float aggregate_current = 0.0F;
		float aggregate_max = 0.0F;
		if (!reader.f32(aggregate_current, 0.0F, QuantityLimit) ||
			!reader.f32(aggregate_max, 0.0F, QuantityLimit)) {
			return reader.error();
		}
		if (aggregate_current > aggregate_max) {
			return ValidationError::OutOfRange;
		}
	}
	if ((presence & SubsystemStatePresenceFlagTurret) != 0) {
		if (type != static_cast<std::uint8_t>(SubsystemType::Turret)) {
			return ValidationError::InvalidAbsence;
		}
		if (!validate_turret(reader)) {
			return reader.error();
		}
	}
	return reader.finish();
}

ValidationError validate_energy_state(ByteView payload) noexcept
{
	Validator reader(payload);
	std::uint64_t entity = 0;
	std::uint64_t presence = 0;
	std::uint64_t sample = 0;
	std::uint8_t ets_mode = 0;
	std::uint8_t shields_index = 0;
	std::uint8_t weapons_index = 0;
	std::uint8_t engines_index = 0;
	std::uint8_t reserved = 0;
	if (!id64(reader, entity) || !presence64(reader, KnownEnergyStatePresenceFlags, presence) ||
		!reader.u64(sample) || !enum8(reader, 2, ets_mode) || !reader.u8(shields_index) ||
		!reader.u8(weapons_index) || !reader.u8(engines_index) || !reader.u8(reserved)) {
		return reader.error();
	}
	if (shields_index > 12U || weapons_index > 12U || engines_index > 12U) {
		return ValidationError::OutOfRange;
	}
	if (reserved != 0) {
		return ValidationError::ReservedFlag;
	}
	if (ets_mode == static_cast<std::uint8_t>(EtsMode::Absent) &&
		(shields_index != 0 || weapons_index != 0 || engines_index != 0)) {
		return ValidationError::InvalidStateTransition;
	}
	float value = 0.0F;
	if ((presence & EnergyStatePresenceFlagWeaponEnergy) != 0) {
		float current = 0.0F;
		float maximum = 0.0F;
		if (!reader.f32(current, 0.0F, QuantityLimit) || !reader.f32(maximum, 0.0F, QuantityLimit)) {
			return reader.error();
		}
		if (maximum <= 0.0F || current > maximum) {
			return ValidationError::OutOfRange;
		}
	}
	if ((presence & EnergyStatePresenceFlagRegeneration) != 0 &&
		(!reader.f32(value, 0.0F, QuantityLimit) || !reader.f32(value, 0.0F, QuantityLimit))) {
		return reader.error();
	}
	if ((presence & EnergyStatePresenceFlagDeferredTransfers) != 0 &&
		(!reader.f32(value, -QuantityLimit, QuantityLimit) ||
			!reader.f32(value, -QuantityLimit, QuantityLimit))) {
		return reader.error();
	}
	if ((presence & EnergyStatePresenceFlagEngineResult) != 0 &&
		(!reader.f32(value, 0.0F, QuantityLimit) || !reader.f32(value, 0.0F, VelocityLimit))) {
		return reader.error();
	}
	if ((presence & EnergyStatePresenceFlagPowerOutput) != 0 &&
		!reader.f32(value, 0.0F, QuantityLimit)) {
		return reader.error();
	}
	if ((presence & EnergyStatePresenceFlagEngineIntegrity) != 0) {
		float current = 0.0F;
		float maximum = 0.0F;
		if (!reader.f32(current, 0.0F, QuantityLimit) || !reader.f32(maximum, 0.0F, QuantityLimit)) {
			return reader.error();
		}
		if (maximum <= 0.0F || current > maximum) {
			return ValidationError::OutOfRange;
		}
	}
	return reader.finish();
}

ValidationError validate_propulsion_state(ByteView payload) noexcept
{
	Validator reader(payload);
	std::uint64_t entity = 0;
	std::uint64_t presence = 0;
	std::uint64_t sample = 0;
	std::uint16_t flags = 0;
	std::uint16_t reserved = 0;
	if (!id64(reader, entity) || !presence64(reader, KnownPropulsionStatePresenceFlags, presence) ||
		!reader.u64(sample) || !flags16(reader, KnownPropulsionFlags, flags) || !reader.u16(reserved)) {
		return reader.error();
	}
	if (reserved != 0) {
		return ValidationError::ReservedFlag;
	}
	const bool has_fuel = (presence & PropulsionStatePresenceFlagFuel) != 0;
	const bool has_consumption = (presence & PropulsionStatePresenceFlagConsumption) != 0;
	const bool has_engagement = (presence & PropulsionStatePresenceFlagEngagement) != 0;
	if ((has_consumption || has_engagement) && !has_fuel) {
		return ValidationError::InvalidAbsence;
	}
	constexpr std::uint16_t AfterburnerFlags = PropulsionFlagAfterburnerAvailable |
		PropulsionFlagAfterburnerLocked | PropulsionFlagAfterburnerActive | PropulsionFlagAfterburnerRequested;
	if (!has_fuel && (flags & AfterburnerFlags) != 0) {
		return ValidationError::InvalidAbsence;
	}
	if ((flags & PropulsionFlagAfterburnerActive) != 0 &&
		((flags & PropulsionFlagAfterburnerAvailable) == 0 ||
			(flags & PropulsionFlagAfterburnerLocked) != 0)) {
		return ValidationError::InvalidStateTransition;
	}
	float fuel_max = 0.0F;
	if (has_fuel) {
		float fuel_current = 0.0F;
		if (!reader.f32(fuel_current, 0.0F, QuantityLimit) ||
			!reader.f32(fuel_max, 0.0F, QuantityLimit)) {
			return reader.error();
		}
		if (fuel_max <= 0.0F || fuel_current > fuel_max) {
			return ValidationError::OutOfRange;
		}
	}
	float value = 0.0F;
	if (has_consumption &&
		(!reader.f32(value, 0.0F, QuantityLimit) || !reader.f32(value, 0.0F, QuantityLimit))) {
		return reader.error();
	}
	if (has_engagement) {
		float minimum = 0.0F;
		float engagement_fuel = 0.0F;
		if (!reader.f32(minimum, 0.0F, QuantityLimit) || !duration(reader, HourUs) || !duration(reader) ||
			!reader.f32(engagement_fuel, 0.0F, QuantityLimit)) {
			return reader.error();
		}
		if (minimum > fuel_max || engagement_fuel > fuel_max) {
			return ValidationError::OutOfRange;
		}
	}
	if ((presence & PropulsionStatePresenceFlagDynamics) != 0 &&
		(!reader.f32(value, 0.0F, 3600.0F) || !vec3(reader, 0.0F, VelocityLimit))) {
		return reader.error();
	}
	if ((presence & PropulsionStatePresenceFlagEngineWash) != 0 &&
		!reader.f32(value, 0.0F, QuantityLimit)) {
		return reader.error();
	}
	if ((presence & PropulsionStatePresenceFlagRcs) != 0 && !vec3(reader, -1.0F, 1.0F)) {
		return reader.error();
	}
	return reader.finish();
}

bool validate_primary_bank_item(Validator& item, std::uint32_t& bank_id) noexcept
{
	std::uint16_t presence = 0;
	std::uint16_t bank_index = 0;
	std::uint32_t weapon_class = 0;
	if (!item.u16(presence)) {
		return false;
	}
	if ((presence & ReservedPrimaryBankPresenceFlags) != 0) {
		return item.fail(ValidationError::ReservedFlag);
	}
	if (!item.u16(bank_index) || !id32(item, bank_id) || !id32(item, weapon_class)) {
		return false;
	}
	if (bank_index > 63U) {
		return item.fail(ValidationError::OutOfRange);
	}
	std::uint16_t next_slot = 0;
	std::uint16_t next_fire_point = 0;
	std::uint16_t simultaneous_slots = 0;
	std::uint16_t pattern = 0;
	if (!duration(item) || !item.u16(next_slot) || !item.u16(next_fire_point) ||
		!item.u16(simultaneous_slots) || !item.u16(pattern)) {
		return false;
	}
	if (next_slot > 255U || next_fire_point > 255U || simultaneous_slots == 0U || simultaneous_slots > 256U) {
		return item.fail(ValidationError::OutOfRange);
	}
	if ((presence & PrimaryBankPresenceFlagBallisticAmmo) != 0) {
		std::uint32_t current = 0;
		std::uint32_t initial = 0;
		float capacity = 0.0F;
		if (!item.u32(current) || !item.u32(initial)) {
			return false;
		}
		if (current > 1'000'000'000U || initial > 1'000'000'000U || current > initial) {
			return item.fail(ValidationError::OutOfRange);
		}
		if (!item.f32(capacity, 0.0F, QuantityLimit)) {
			return false;
		}
	}
	if ((presence & PrimaryBankPresenceFlagRearm) != 0 && !duration(item)) {
		return false;
	}
	if ((presence & PrimaryBankPresenceFlagBurst) != 0) {
		std::uint16_t burst_counter = 0;
		std::uint16_t reserved = 0;
		std::uint32_t burst_seed = 0;
		if (!item.u16(burst_counter) || !item.u16(reserved) || !item.u32(burst_seed)) {
			return false;
		}
		if (reserved != 0) {
			return item.fail(ValidationError::ReservedFlag);
		}
	}
	if ((presence & PrimaryBankPresenceFlagSubstitution) != 0) {
		std::uint16_t pattern_index = 0;
		std::uint16_t reserved = 0;
		if (!item.u16(pattern_index) || !item.u16(reserved)) {
			return false;
		}
		if (reserved != 0) {
			return item.fail(ValidationError::ReservedFlag);
		}
	}
	float animation_position = 0.0F;
	if ((presence & PrimaryBankPresenceFlagAnimation) != 0 &&
		(!item.f32(animation_position, 0.0F, 1.0F) || !duration(item))) {
		return false;
	}
	if ((presence & PrimaryBankPresenceFlagFofCooldown) != 0 && !duration(item)) {
		return false;
	}
	return finish_item(item);
}

bool validate_secondary_bank_item(Validator& item, std::uint32_t& bank_id) noexcept
{
	std::uint16_t presence = 0;
	std::uint16_t bank_index = 0;
	std::uint32_t weapon_class = 0;
	if (!item.u16(presence)) {
		return false;
	}
	if ((presence & ReservedSecondaryBankPresenceFlags) != 0) {
		return item.fail(ValidationError::ReservedFlag);
	}
	if (!item.u16(bank_index) || !id32(item, bank_id) || !id32(item, weapon_class)) {
		return false;
	}
	if (bank_index > 63U) {
		return item.fail(ValidationError::OutOfRange);
	}
	std::uint16_t next_slot = 0;
	std::uint16_t reserved = 0;
	if (!duration(item) || !item.u16(next_slot) || !item.u16(reserved)) {
		return false;
	}
	if (next_slot > 255U) {
		return item.fail(ValidationError::OutOfRange);
	}
	if (reserved != 0) {
		return item.fail(ValidationError::ReservedFlag);
	}
	if ((presence & SecondaryBankPresenceFlagAmmo) != 0) {
		std::uint32_t current = 0;
		std::uint32_t initial = 0;
		float capacity = 0.0F;
		if (!item.u32(current) || !item.u32(initial)) {
			return false;
		}
		if (current > 1'000'000'000U || initial > 1'000'000'000U || current > initial) {
			return item.fail(ValidationError::OutOfRange);
		}
		if (!item.f32(capacity, 0.0F, QuantityLimit)) {
			return false;
		}
	}
	if ((presence & SecondaryBankPresenceFlagRearm) != 0 && !duration(item)) {
		return false;
	}
	if ((presence & SecondaryBankPresenceFlagBurst) != 0) {
		std::uint16_t burst_counter = 0;
		std::uint16_t burst_reserved = 0;
		std::uint32_t burst_seed = 0;
		if (!item.u16(burst_counter) || !item.u16(burst_reserved) || !item.u32(burst_seed)) {
			return false;
		}
		if (burst_reserved != 0) {
			return item.fail(ValidationError::ReservedFlag);
		}
	}
	if ((presence & SecondaryBankPresenceFlagSubstitution) != 0) {
		std::uint16_t pattern_index = 0;
		std::uint16_t substitution_reserved = 0;
		if (!item.u16(pattern_index) || !item.u16(substitution_reserved)) {
			return false;
		}
		if (substitution_reserved != 0) {
			return item.fail(ValidationError::ReservedFlag);
		}
	}
	float animation_position = 0.0F;
	if ((presence & SecondaryBankPresenceFlagAnimation) != 0 &&
		(!item.f32(animation_position, 0.0F, 1.0F) || !duration(item))) {
		return false;
	}
	return finish_item(item);
}

bool validate_tertiary_bank(Validator& reader, std::uint32_t& bank_id) noexcept
{
	std::uint32_t current = 0;
	std::uint32_t initial = 0;
	float capacity = 0.0F;
	if (!id32(reader, bank_id) || !reader.u32(current) || !reader.u32(initial)) {
		return false;
	}
	if (current > 1'000'000'000U || initial > 1'000'000'000U || current > initial) {
		return reader.fail(ValidationError::OutOfRange);
	}
	return reader.f32(capacity, 0.0F, QuantityLimit) && duration(reader, HourUs) && duration(reader, HourUs);
}

bool validate_countermeasure(Validator& reader) noexcept
{
	std::uint16_t presence = 0;
	std::uint16_t flags = 0;
	if (!reader.u16(presence)) {
		return false;
	}
	if ((presence & ReservedCountermeasureStatePresenceFlags) != 0) {
		return reader.fail(ValidationError::ReservedFlag);
	}
	if (!flags16(reader, KnownCountermeasureStateFlags, flags)) {
		return false;
	}
	if ((flags & CountermeasureStateFlagAvailable) != 0 &&
		(flags & CountermeasureStateFlagLocked) != 0) {
		return reader.fail(ValidationError::InvalidStateTransition);
	}
	if ((presence & CountermeasureStatePresenceFlagClass) != 0) {
		std::uint32_t weapon_class = 0;
		if (!id32(reader, weapon_class)) {
			return false;
		}
	}
	std::uint32_t current = 0;
	std::uint32_t maximum = 0;
	if (!reader.u32(current) || !reader.u32(maximum)) {
		return false;
	}
	if (current > 1'000'000U || maximum > 1'000'000U || current > maximum) {
		return reader.fail(ValidationError::OutOfRange);
	}
	return duration(reader, HourUs);
}

ValidationError validate_weapon_state(ByteView payload) noexcept
{
	Validator reader(payload);
	std::uint64_t entity = 0;
	std::uint64_t presence = 0;
	std::uint64_t sample = 0;
	std::uint16_t primary_count = 0;
	std::uint16_t secondary_count = 0;
	std::uint16_t tertiary_count = 0;
	std::uint16_t reserved = 0;
	std::uint32_t current_primary = 0;
	std::uint32_t current_secondary = 0;
	std::uint32_t current_tertiary = 0;
	std::uint32_t weapon_flags = 0;
	if (!id64(reader, entity) || !presence64(reader, KnownWeaponStatePresenceFlags, presence) ||
		!reader.u64(sample) || !reader.u16(primary_count) || !reader.u16(secondary_count) ||
		!reader.u16(tertiary_count) || !reader.u16(reserved) || !id32(reader, current_primary, true) ||
		!id32(reader, current_secondary, true) || !id32(reader, current_tertiary, true) ||
		!flags32(reader, KnownWeaponGlobalFlags, weapon_flags)) {
		return reader.error();
	}
	if (primary_count > 64U || secondary_count > 64U || tertiary_count > 64U) {
		return ValidationError::OutOfRange;
	}
	if (reserved != 0) {
		return ValidationError::ReservedFlag;
	}
	std::uint32_t previous_primary = 0;
	std::uint32_t previous_secondary = 0;
	std::uint32_t targeting_laser = 0;
	std::uint32_t swarm_origin = 0;
	if ((presence & WeaponStatePresenceFlagPreviousPrimary) != 0 && !id32(reader, previous_primary)) {
		return reader.error();
	}
	if ((presence & WeaponStatePresenceFlagPreviousSecondary) != 0 && !id32(reader, previous_secondary)) {
		return reader.error();
	}
	if ((presence & WeaponStatePresenceFlagTargetingLaser) != 0 && !id32(reader, targeting_laser)) {
		return reader.error();
	}
	if ((weapon_flags & WeaponGlobalFlagTargetingLaser) != 0 &&
		(presence & WeaponStatePresenceFlagTargetingLaser) == 0) {
		return ValidationError::InvalidAbsence;
	}
	if ((presence & WeaponStatePresenceFlagSwarm) != 0) {
		std::uint16_t swarm_remaining = 0;
		if (!reader.u16(swarm_remaining) || !id32(reader, swarm_origin)) {
			return reader.error();
		}
		if (swarm_remaining > 4096U) {
			return ValidationError::OutOfRange;
		}
	}
	if ((presence & WeaponStatePresenceFlagRemoteDetonation) != 0 && !duration(reader, HourUs)) {
		return reader.error();
	}
	float value = 0.0F;
	if ((presence & WeaponStatePresenceFlagPerBurstRotation) != 0 &&
		!reader.f32(value, -VelocityLimit, VelocityLimit)) {
		return reader.error();
	}

	std::uint16_t encoded_primary_count = 0;
	if (!reader.u16(encoded_primary_count)) {
		return reader.error();
	}
	if (encoded_primary_count > 64U) {
		return ValidationError::OutOfRange;
	}
	if (encoded_primary_count != primary_count) {
		return ValidationError::InvalidStateTransition;
	}
	std::array<std::uint32_t, 64> primary_ids{};
	std::array<std::uint32_t, 129> all_ids{};
	std::size_t all_id_count = 0;
	for (std::size_t index = 0; index < encoded_primary_count; ++index) {
		Validator item(ByteView{});
		if (!vlist_item(reader, item)) {
			return reader.error();
		}
		std::uint32_t bank_id = 0;
		if (!validate_primary_bank_item(item, bank_id)) {
			return item.error();
		}
		if (!insert_unique(reader, primary_ids, index, bank_id) ||
			!insert_unique(reader, all_ids, all_id_count, bank_id)) {
			return reader.error();
		}
		++all_id_count;
	}

	std::uint16_t encoded_secondary_count = 0;
	if (!reader.u16(encoded_secondary_count)) {
		return reader.error();
	}
	if (encoded_secondary_count > 64U) {
		return ValidationError::OutOfRange;
	}
	if (encoded_secondary_count != secondary_count) {
		return ValidationError::InvalidStateTransition;
	}
	std::array<std::uint32_t, 64> secondary_ids{};
	for (std::size_t index = 0; index < encoded_secondary_count; ++index) {
		Validator item(ByteView{});
		if (!vlist_item(reader, item)) {
			return reader.error();
		}
		std::uint32_t bank_id = 0;
		if (!validate_secondary_bank_item(item, bank_id)) {
			return item.error();
		}
		if (!insert_unique(reader, secondary_ids, index, bank_id) ||
			!insert_unique(reader, all_ids, all_id_count, bank_id)) {
			return reader.error();
		}
		++all_id_count;
	}

	const bool has_tertiary = (presence & WeaponStatePresenceFlagTertiary) != 0;
	if (has_tertiary != (tertiary_count > 0U)) {
		return ValidationError::InvalidAbsence;
	}
	std::uint32_t tertiary_id = 0;
	if (has_tertiary) {
		if (!validate_tertiary_bank(reader, tertiary_id)) {
			return reader.error();
		}
		if (!insert_unique(reader, all_ids, all_id_count, tertiary_id)) {
			return reader.error();
		}
		++all_id_count;
		if (current_tertiary != tertiary_id) {
			return ValidationError::InvalidStateTransition;
		}
	} else if (current_tertiary != 0U) {
		return ValidationError::InvalidStateTransition;
	}
	if ((presence & WeaponStatePresenceFlagCountermeasure) != 0 && !validate_countermeasure(reader)) {
		return reader.error();
	}

	if ((current_primary != 0U && !contains(primary_ids, encoded_primary_count, current_primary)) ||
		(current_secondary != 0U && !contains(secondary_ids, encoded_secondary_count, current_secondary)) ||
		(previous_primary != 0U && !contains(primary_ids, encoded_primary_count, previous_primary)) ||
		(previous_secondary != 0U && !contains(secondary_ids, encoded_secondary_count, previous_secondary)) ||
		(targeting_laser != 0U && !contains(primary_ids, encoded_primary_count, targeting_laser)) ||
		(swarm_origin != 0U && !contains(secondary_ids, encoded_secondary_count, swarm_origin))) {
		return ValidationError::InvalidStateTransition;
	}
	return reader.finish();
}

struct LockKey {
	std::uint64_t target = 0;
	bool has_subsystem = false;
	std::uint32_t subsystem = 0;
	std::array<float, 3> world_position{};

	friend bool operator==(const LockKey& left, const LockKey& right) noexcept
	{
		if (left.target != right.target || left.has_subsystem != right.has_subsystem) {
			return false;
		}
		return left.has_subsystem ? left.subsystem == right.subsystem :
								 left.world_position == right.world_position;
	}
};

bool validate_lock_item(Validator& item, LockKey& key) noexcept
{
	std::uint16_t presence = 0;
	bool locked = false;
	bool in_cone = false;
	if (!item.u16(presence)) {
		return false;
	}
	if ((presence & ReservedLockItemPresenceFlags) != 0) {
		return item.fail(ValidationError::ReservedFlag);
	}
	if (!item.boolean(locked) || !item.boolean(in_cone) || !id64(item, key.target)) {
		return false;
	}
	key.has_subsystem = (presence & LockItemPresenceFlagSubsystem) != 0;
	if (key.has_subsystem && !id32(item, key.subsystem)) {
		return false;
	}
	if (!vec3(item, -PositionLimit, PositionLimit, &key.world_position)) {
		return false;
	}
	if ((presence & LockItemPresenceFlagLockAttempt) != 0 && !duration(item)) {
		return false;
	}
	return finish_item(item);
}

ValidationError validate_lock_state(ByteView payload) noexcept
{
	Validator reader(payload);
	std::uint64_t entity = 0;
	std::uint64_t presence = 0;
	std::uint64_t sample = 0;
	std::uint16_t count = 0;
	if (!id64(reader, entity) || !presence64(reader, KnownLockStatePresenceFlags, presence) ||
		!reader.u64(sample) || !reader.u16(count)) {
		return reader.error();
	}
	if (count > 64U) {
		return ValidationError::OutOfRange;
	}
	std::array<LockKey, 64> keys{};
	for (std::size_t index = 0; index < count; ++index) {
		Validator item(ByteView{});
		if (!vlist_item(reader, item)) {
			return reader.error();
		}
		LockKey key;
		if (!validate_lock_item(item, key)) {
			return item.error();
		}
		if (!insert_unique(reader, keys, index, key)) {
			return reader.error();
		}
	}
	return reader.finish();
}

ValidationError validate_target_state(ByteView payload) noexcept
{
	Validator reader(payload);
	std::uint64_t entity = 0;
	std::uint64_t presence = 0;
	std::uint64_t sample = 0;
	std::uint64_t current_target = 0;
	if (!id64(reader, entity) || !presence64(reader, KnownTargetStatePresenceFlags, presence) ||
		!reader.u64(sample) || !id64(reader, current_target, true)) {
		return reader.error();
	}
	if (current_target == 0U && (presence & ~TargetStatePresenceFlagPreviousTarget) != 0) {
		return ValidationError::InvalidAbsence;
	}
	if ((presence & TargetStatePresenceFlagPreviousTarget) != 0) {
		std::uint64_t previous = 0;
		if (!id64(reader, previous)) {
			return reader.error();
		}
	}
	if ((presence & TargetStatePresenceFlagRevealedIdentity) != 0) {
		std::uint8_t object_type = 0;
		std::string_view name;
		std::uint32_t class_id = 0;
		std::uint32_t team_id = 0;
		std::uint32_t iff_id = 0;
		if (!enum8(reader, 8, object_type) || !reader.string(0, 255, name) || !reader.u32(class_id) ||
			!reader.u32(team_id) || !reader.u32(iff_id)) {
			return reader.error();
		}
		if (team_id > 65'535U || iff_id > 65'535U) {
			return ValidationError::OutOfRange;
		}
	}
	if ((presence & TargetStatePresenceFlagTimeOnTarget) != 0 && !duration(reader)) {
		return reader.error();
	}
	if ((presence & TargetStatePresenceFlagTargetSubsystem) != 0) {
		std::uint32_t subsystem = 0;
		if (!id32(reader, subsystem)) {
			return reader.error();
		}
	}
	if ((presence & TargetStatePresenceFlagLockSubsystem) != 0) {
		std::uint32_t subsystem = 0;
		if (!id32(reader, subsystem)) {
			return reader.error();
		}
	}
	if ((presence & TargetStatePresenceFlagLastStealthObservation) != 0 &&
		(!vec3(reader, -PositionLimit, PositionLimit) || !vec3(reader, -VelocityLimit, VelocityLimit))) {
		return reader.error();
	}
	if ((presence & TargetStatePresenceFlagDistanceTrend) != 0) {
		std::uint8_t trend = 0;
		if (!enum8(reader, 3, trend)) {
			return reader.error();
		}
	}
	if ((presence & TargetStatePresenceFlagSpeedTrend) != 0) {
		std::uint8_t trend = 0;
		if (!enum8(reader, 3, trend)) {
			return reader.error();
		}
	}
	if ((presence & TargetStatePresenceFlagInCone) != 0) {
		bool in_cone = false;
		if (!reader.boolean(in_cone)) {
			return reader.error();
		}
	}
	if ((presence & TargetStatePresenceFlagLead) != 0) {
		std::uint32_t bank_id = 0;
		if (!vec3(reader, -PositionLimit, PositionLimit) || !id32(reader, bank_id)) {
			return reader.error();
		}
	}
	if ((presence & TargetStatePresenceFlagAttacker) != 0) {
		std::uint64_t attacker = 0;
		if (!id64(reader, attacker)) {
			return reader.error();
		}
	}
	if ((presence & TargetStatePresenceFlagDangerousWeapon) != 0) {
		std::uint64_t weapon = 0;
		if (!id64(reader, weapon)) {
			return reader.error();
		}
	}
	if ((presence & TargetStatePresenceFlagNearestLocked) != 0) {
		std::uint64_t nearest = 0;
		if (!id64(reader, nearest)) {
			return reader.error();
		}
	}
	float distance = 0.0F;
	if ((presence & TargetStatePresenceFlagExactHudDistance) != 0 &&
		!reader.f32(distance, 0.0F, QuantityLimit)) {
		return reader.error();
	}
	return reader.finish();
}

ValidationError validate_radar_state(ByteView payload) noexcept
{
	Validator reader(payload);
	std::uint64_t entity = 0;
	std::uint64_t presence = 0;
	std::uint64_t sample = 0;
	std::uint8_t radar_mode = 0;
	float selected_range = 0.0F;
	std::uint8_t sensor_state = 0;
	float sensor_current = 0.0F;
	float sensor_maximum = 0.0F;
	if (!id64(reader, entity) || !presence64(reader, KnownRadarStatePresenceFlags, presence) ||
		!reader.u64(sample) || !enum8(reader, 3, radar_mode) ||
		!reader.f32(selected_range, 0.0F, QuantityLimit) || !enum8(reader, 2, sensor_state) ||
		!reader.f32(sensor_current, 0.0F, QuantityLimit) ||
		!reader.f32(sensor_maximum, 0.0F, QuantityLimit)) {
		return reader.error();
	}
	if (sensor_maximum <= 0.0F || sensor_current > sensor_maximum) {
		return ValidationError::OutOfRange;
	}
	if (radar_mode == static_cast<std::uint8_t>(RadarMode::Infinite) && selected_range != QuantityLimit) {
		return ValidationError::OutOfRange;
	}
	float value = 0.0F;
	if ((presence & RadarStatePresenceFlagBrightRange) != 0 &&
		!reader.f32(value, 0.0F, QuantityLimit)) {
		return reader.error();
	}
	if ((presence & RadarStatePresenceFlagPrimitiveRange) != 0 &&
		!reader.f32(value, 0.0F, QuantityLimit)) {
		return reader.error();
	}
	if ((presence & RadarStatePresenceFlagAwacs) != 0 &&
		(!reader.f32(value, 0.0F, QuantityLimit) || !reader.f32(value, 0.0F, QuantityLimit))) {
		return reader.error();
	}
	if ((presence & RadarStatePresenceFlagEmp) != 0 &&
		(!reader.f32(value, 0.0F, QuantityLimit) || !duration(reader))) {
		return reader.error();
	}
	if ((presence & RadarStatePresenceFlagJamming) != 0 &&
		(!reader.f32(value, 0.0F, QuantityLimit) || !reader.f32(value, 0.0F, QuantityLimit))) {
		return reader.error();
	}
	if ((presence & RadarStatePresenceFlagVisibilityTimes) != 0) {
		std::uint64_t first = 0;
		std::uint64_t last = 0;
		if (!reader.u64(first) || !reader.u64(last)) {
			return reader.error();
		}
		if (first > last || last > sample) {
			return ValidationError::OutOfRange;
		}
	}
	return reader.finish();
}

ValidationError validate_radar_contacts(ByteView payload) noexcept
{
	Validator reader(payload);
	std::uint64_t entity = 0;
	std::uint64_t contact = 0;
	std::uint64_t presence = 0;
	std::uint64_t sample = 0;
	std::uint8_t object_type = 0;
	std::uint8_t category = 0;
	std::uint8_t visibility = 0;
	float radius = 0.0F;
	std::uint32_t flags = 0;
	if (!id64(reader, entity) || !id64(reader, contact) ||
		!presence64(reader, KnownRadarContactsPresenceFlags, presence) || !reader.u64(sample) ||
		!enum8(reader, 8, object_type) || !enum8(reader, 7, category) || !enum8(reader, 2, visibility) ||
		!vec3(reader, -PositionLimit, PositionLimit) || !vec3(reader, -VelocityLimit, VelocityLimit) ||
		!reader.f32(radius, 0.0F, VelocityLimit) || !flags32(reader, KnownContactFlags, flags)) {
		return reader.error();
	}
	if ((flags & ContactFlagBomb) != 0 && object_type != static_cast<std::uint8_t>(ObjectType::Weapon)) {
		return ValidationError::InvalidStateTransition;
	}
	float value = 0.0F;
	if ((presence & RadarContactsPresenceFlagIconSize) != 0 &&
		!reader.f32(value, 0.0F, VelocityLimit)) {
		return reader.error();
	}
	if ((presence & RadarContactsPresenceFlagRevealedName) != 0) {
		std::string_view name;
		if (!reader.string(0, 255, name)) {
			return reader.error();
		}
	}
	if ((presence & RadarContactsPresenceFlagRevealedClass) != 0) {
		std::uint32_t class_id = 0;
		if (!id32(reader, class_id)) {
			return reader.error();
		}
	}
	if ((presence & RadarContactsPresenceFlagRevealedTeamIff) != 0) {
		std::uint32_t team_id = 0;
		std::uint32_t iff_id = 0;
		if (!reader.u32(team_id) || !reader.u32(iff_id)) {
			return reader.error();
		}
		if (team_id > 65'535U || iff_id > 65'535U) {
			return ValidationError::OutOfRange;
		}
	}
	if ((presence & RadarContactsPresenceFlagDetectionTimes) != 0) {
		std::uint64_t first = 0;
		std::uint64_t last = 0;
		if (!reader.u64(first) || !reader.u64(last)) {
			return reader.error();
		}
		if (first > last || last > sample) {
			return ValidationError::OutOfRange;
		}
	}
	if ((presence & RadarContactsPresenceFlagConfidence) != 0 && !reader.f32(value, 0.0F, 1.0F)) {
		return reader.error();
	}
	return reader.finish();
}

} // namespace

ValidationError validate_business_record_11_18(RecordType type, ByteView payload) noexcept
{
	switch (type) {
	case RecordType::SubsystemState:
		return validate_subsystem_state(payload);
	case RecordType::EnergyState:
		return validate_energy_state(payload);
	case RecordType::PropulsionState:
		return validate_propulsion_state(payload);
	case RecordType::WeaponState:
		return validate_weapon_state(payload);
	case RecordType::LockState:
		return validate_lock_state(payload);
	case RecordType::TargetState:
		return validate_target_state(payload);
	case RecordType::RadarState:
		return validate_radar_state(payload);
	case RecordType::RadarContacts:
		return validate_radar_contacts(payload);
	default:
		return ValidationError::UnknownRequiredRecord;
	}
}

} // namespace telemetry::protocol::detail
