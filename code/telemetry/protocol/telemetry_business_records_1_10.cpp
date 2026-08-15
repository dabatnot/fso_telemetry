#include "telemetry/protocol/telemetry_business_records_internal.h"

#include "telemetry/protocol/packet_reader.h"
#include "telemetry/protocol/packet_writer.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace telemetry::protocol::detail {

namespace {

constexpr float Pi = 3.14159265358979323846f;
constexpr float MaxQuantity = 1.0e12f;
constexpr float MaxPosition = 1.0e12f;
constexpr float MaxLinearVelocity = 1.0e9f;
constexpr float MaxRotationalVelocity = 1.0e6f;
constexpr float MaxDurationSeconds = 3600.0f;
constexpr std::uint64_t OneHourUs = 3'600'000'000ULL;
constexpr std::uint64_t OneDayUs = 86'400'000'000ULL;

class ValidationCursor final {
  public:
	explicit ValidationCursor(ByteView input) noexcept : m_reader(input)
	{
		if (input.size != 0 && input.data == nullptr) {
			m_error = ValidationError::TruncatedPayload;
		}
	}

	bool read_u8(std::uint8_t& value) noexcept
	{
		return read_scalar(1U, [&]() { return m_reader.read_u8(value); });
	}

	bool read_u16(std::uint16_t& value) noexcept
	{
		return read_scalar(2U, [&]() { return m_reader.read_u16(value); });
	}

	bool read_u32(std::uint32_t& value) noexcept
	{
		return read_scalar(4U, [&]() { return m_reader.read_u32(value); });
	}

	bool read_u64(std::uint64_t& value) noexcept
	{
		return read_scalar(8U, [&]() { return m_reader.read_u64(value); });
	}

	bool read_bool(bool& value) noexcept
	{
		std::uint8_t raw = 0;
		if (!read_u8(raw)) {
			return false;
		}
		if (raw > 1U) {
			return reject(ValidationError::OutOfRange);
		}
		value = raw != 0;
		return true;
	}

	bool read_f32(float& value) noexcept
	{
		if (m_error != ValidationError::None) {
			return false;
		}
		if (m_reader.remaining() < 4U) {
			return reject(ValidationError::TruncatedPayload);
		}
		if (!m_reader.read_f32(value)) {
			return reject(ValidationError::NonFiniteFloat);
		}
		return true;
	}

	bool read_bounded_f32(float minimum, float maximum, float& value) noexcept
	{
		return read_f32(value) && require(value >= minimum && value <= maximum, ValidationError::OutOfRange);
	}

	bool read_positive_f32(float maximum, float& value) noexcept
	{
		return read_f32(value) && require(value > 0.0f && value <= maximum, ValidationError::OutOfRange);
	}

	bool read_utf8(std::size_t maximum, std::size_t minimum = 0U) noexcept
	{
		std::uint16_t length = 0;
		if (!read_u16(length)) {
			return false;
		}
		if (length > maximum) {
			return reject(ValidationError::StringTooLong);
		}
		if (length < minimum) {
			return reject(ValidationError::OutOfRange);
		}
		ByteView bytes;
		if (!take(length, bytes)) {
			return false;
		}
		if (bytes.size == 0U) {
			return true;
		}
		const std::string_view value(static_cast<const char*>(static_cast<const void*>(bytes.data)), bytes.size);
		if (!is_valid_utf8(value)) {
			return reject(ValidationError::InvalidUtf8);
		}
		return true;
	}

	bool take(std::size_t size, ByteView& value) noexcept
	{
		if (m_error != ValidationError::None) {
			return false;
		}
		if (size > m_reader.remaining() || !m_reader.read_bytes(size, value)) {
			return reject(ValidationError::TruncatedPayload);
		}
		return true;
	}

	bool require(bool condition, ValidationError error) noexcept
	{
		return condition || reject(error);
	}

	bool reject(ValidationError error) noexcept
	{
		if (m_error == ValidationError::None) {
			m_error = error;
		}
		return false;
	}

	std::size_t remaining() const noexcept
	{
		return m_reader.remaining();
	}

	ValidationError finish(bool nested = false) const noexcept
	{
		if (m_error != ValidationError::None) {
			return m_error;
		}
		if (!m_reader.at_end()) {
			return nested ? ValidationError::BadRecordLength : ValidationError::TrailingBytes;
		}
		return ValidationError::None;
	}

  private:
	template <typename Reader>
	bool read_scalar(std::size_t size, Reader&& reader) noexcept
	{
		if (m_error != ValidationError::None) {
			return false;
		}
		if (m_reader.remaining() < size || !reader()) {
			return reject(ValidationError::TruncatedPayload);
		}
		return true;
	}

	PacketReader m_reader;
	ValidationError m_error = ValidationError::None;
};

template <typename Integer>
bool read_closed_flags(ValidationCursor& cursor, Integer known_mask, Integer& value) noexcept;

template <>
bool read_closed_flags<std::uint8_t>(ValidationCursor& cursor, std::uint8_t known_mask, std::uint8_t& value) noexcept
{
	return cursor.read_u8(value) && cursor.require((value & static_cast<std::uint8_t>(~known_mask)) == 0U,
		ValidationError::ReservedFlag);
}

template <>
bool read_closed_flags<std::uint16_t>(ValidationCursor& cursor,
	std::uint16_t known_mask,
	std::uint16_t& value) noexcept
{
	return cursor.read_u16(value) && cursor.require((value & static_cast<std::uint16_t>(~known_mask)) == 0U,
		ValidationError::ReservedFlag);
}

template <>
bool read_closed_flags<std::uint32_t>(ValidationCursor& cursor,
	std::uint32_t known_mask,
	std::uint32_t& value) noexcept
{
	return cursor.read_u32(value) && cursor.require((value & ~known_mask) == 0U, ValidationError::ReservedFlag);
}

template <>
bool read_closed_flags<std::uint64_t>(ValidationCursor& cursor,
	std::uint64_t known_mask,
	std::uint64_t& value) noexcept
{
	return cursor.read_u64(value) && cursor.require((value & ~known_mask) == 0U, ValidationError::ReservedFlag);
}

bool read_reserved_zero(ValidationCursor& cursor, std::size_t size) noexcept
{
	for (std::size_t index = 0; index < size; ++index) {
		std::uint8_t value = 0;
		if (!cursor.read_u8(value)) {
			return false;
		}
		if (value != 0U) {
			return cursor.reject(ValidationError::ReservedFlag);
		}
	}
	return true;
}

bool read_enum_u8(ValidationCursor& cursor, std::uint8_t maximum, std::uint8_t& value) noexcept
{
	return cursor.read_u8(value) && cursor.require(value <= maximum, ValidationError::UnknownEnum);
}

bool read_nonzero_u32(ValidationCursor& cursor, std::uint32_t& value) noexcept
{
	return cursor.read_u32(value) && cursor.require(value != 0U, ValidationError::OutOfRange);
}

bool read_nonzero_u64(ValidationCursor& cursor, std::uint64_t& value) noexcept
{
	return cursor.read_u64(value) && cursor.require(value != 0U, ValidationError::OutOfRange);
}

bool read_duration(ValidationCursor& cursor,
	std::uint64_t maximum,
	std::uint64_t& value,
	bool require_positive = false) noexcept
{
	return cursor.read_u64(value) && cursor.require(value <= maximum && (!require_positive || value != 0U),
		ValidationError::OutOfRange);
}

bool read_vec3(ValidationCursor& cursor, float minimum, float maximum) noexcept
{
	float value = 0.0f;
	return cursor.read_bounded_f32(minimum, maximum, value) && cursor.read_bounded_f32(minimum, maximum, value) &&
		   cursor.read_bounded_f32(minimum, maximum, value);
}

bool read_quaternion(ValidationCursor& cursor) noexcept
{
	std::array<float, 4> value{};
	for (auto& component : value) {
		if (!cursor.read_f32(component)) {
			return false;
		}
	}
	const auto norm_squared = static_cast<double>(value[0]) * value[0] + static_cast<double>(value[1]) * value[1] +
						  static_cast<double>(value[2]) * value[2] + static_cast<double>(value[3]) * value[3];
	const auto norm = std::sqrt(norm_squared);
	if (!cursor.require(norm >= 0.9999 && norm <= 1.0001, ValidationError::OutOfRange) ||
		!cursor.require(value[0] >= 0.0f, ValidationError::OutOfRange)) {
		return false;
	}
	if (value[0] == 0.0f) {
		for (std::size_t index = 1; index < value.size(); ++index) {
			if (value[index] != 0.0f) {
				return cursor.require(value[index] > 0.0f, ValidationError::OutOfRange);
			}
		}
	}
	return true;
}

bool read_mat3(ValidationCursor& cursor) noexcept
{
	float value = 0.0f;
	for (std::size_t index = 0; index < 9U; ++index) {
		if (!cursor.read_bounded_f32(-MaxQuantity, MaxQuantity, value)) {
			return false;
		}
	}
	return true;
}

bool read_fixed_list_header(ValidationCursor& cursor,
	std::uint16_t maximum_count,
	std::uint16_t expected_item_size,
	std::uint16_t& count) noexcept
{
	std::uint8_t item_version = 0;
	std::uint16_t item_size = 0;
	if (!cursor.read_u16(count) || !cursor.read_u8(item_version) || !cursor.read_u16(item_size)) {
		return false;
	}
	if (!cursor.require(count <= maximum_count, ValidationError::OutOfRange) ||
		!cursor.require(item_version == 1U, ValidationError::UnsupportedRecordVersion) ||
		!cursor.require(item_size == expected_item_size, ValidationError::BadRecordLength)) {
		return false;
	}
	const auto encoded_size = static_cast<std::size_t>(count) * expected_item_size;
	return cursor.require(encoded_size <= cursor.remaining(), ValidationError::TruncatedPayload);
}

bool read_class_motion(ValidationCursor& cursor) noexcept
{
	float value = 0.0f;
	return read_vec3(cursor, 0.0f, MaxLinearVelocity) && read_vec3(cursor, 0.0f, MaxLinearVelocity) &&
		   read_vec3(cursor, 0.0f, MaxLinearVelocity) && read_vec3(cursor, 0.0f, MaxRotationalVelocity) &&
		   cursor.read_bounded_f32(0.0f, MaxLinearVelocity, value) &&
		   cursor.read_bounded_f32(0.0f, MaxDurationSeconds, value) &&
		   cursor.read_bounded_f32(0.0f, MaxDurationSeconds, value) &&
		   cursor.read_bounded_f32(0.0f, MaxDurationSeconds, value) &&
		   cursor.read_bounded_f32(0.0f, MaxDurationSeconds, value) &&
		   cursor.read_bounded_f32(0.0f, MaxDurationSeconds, value) &&
		   cursor.read_bounded_f32(0.0f, MaxDurationSeconds, value);
}

bool read_class_energy(ValidationCursor& cursor) noexcept
{
	float value = 0.0f;
	for (std::size_t index = 0; index < 5U; ++index) {
		if (!cursor.read_bounded_f32(0.0f, MaxQuantity, value)) {
			return false;
		}
	}
	return true;
}

bool read_class_afterburner(ValidationCursor& cursor) noexcept
{
	float value = 0.0f;
	std::uint64_t duration = 0;
	return cursor.read_bounded_f32(0.0f, MaxQuantity, value) &&
		   cursor.read_bounded_f32(0.0f, MaxQuantity, value) &&
		   cursor.read_bounded_f32(0.0f, MaxQuantity, value) &&
		   cursor.read_bounded_f32(0.0f, MaxQuantity, value) && read_duration(cursor, OneDayUs, duration);
}

bool read_class_countermeasure(ValidationCursor& cursor) noexcept
{
	std::uint32_t weapon_class_id = 0;
	std::uint32_t initial_count = 0;
	std::uint64_t cooldown = 0;
	return cursor.read_u32(weapon_class_id) && cursor.read_u32(initial_count) &&
		   cursor.require(initial_count <= 1'000'000U, ValidationError::OutOfRange) &&
		   read_duration(cursor, OneDayUs, cooldown);
}

ValidationError validate_class_bank(ByteView input, std::uint32_t& bank_id, std::uint8_t& family) noexcept
{
	ValidationCursor cursor(input);
	std::uint16_t presence = 0;
	std::uint8_t reserved = 0;
	std::uint16_t bank_index = 0;
	std::uint32_t weapon_class_id = 0;
	float value = 0.0f;
	if (!read_closed_flags(cursor, KnownClassBankPresenceFlags, presence) || !read_enum_u8(cursor, 3U, family) ||
		!cursor.read_u8(reserved) || !cursor.require(reserved == 0U, ValidationError::ReservedFlag) ||
		!cursor.read_u16(bank_index) || !cursor.require(bank_index <= 63U, ValidationError::OutOfRange) ||
		!read_nonzero_u32(cursor, bank_id)) {
		return cursor.finish(true);
	}
	const bool has_weapon_class = (presence & ClassBankPresenceFlagWeaponClass) != 0U;
	const bool has_capacity = (presence & ClassBankPresenceFlagCapacity) != 0U;
	if (has_weapon_class && !read_nonzero_u32(cursor, weapon_class_id)) {
		return cursor.finish(true);
	}
	if (has_capacity && !cursor.read_bounded_f32(0.0f, MaxQuantity, value)) {
		return cursor.finish(true);
	}
	if ((family == static_cast<std::uint8_t>(WeaponFamily::Primary) ||
			family == static_cast<std::uint8_t>(WeaponFamily::Secondary)) &&
		!cursor.require(has_weapon_class, ValidationError::InvalidAbsence)) {
		return cursor.finish(true);
	}
	if (family == static_cast<std::uint8_t>(WeaponFamily::Tertiary) &&
		!cursor.require(!has_weapon_class, ValidationError::InvalidAbsence)) {
		return cursor.finish(true);
	}
	std::uint16_t fire_point_count = 0;
	if (!read_fixed_list_header(cursor, 256U, 12U, fire_point_count)) {
		return cursor.finish(true);
	}
	for (std::uint16_t index = 0; index < fire_point_count; ++index) {
		if (!read_vec3(cursor, -MaxPosition, MaxPosition)) {
			return cursor.finish(true);
		}
	}
	return cursor.finish(true);
}

ValidationError validate_class_subsystem(ByteView input, std::uint32_t& subsystem_id) noexcept
{
	ValidationCursor cursor(input);
	std::uint16_t presence = 0;
	std::uint16_t canonical_index = 0;
	std::uint8_t type = 0;
	std::uint8_t reserved = 0;
	std::uint32_t armor_id = 0;
	std::uint32_t static_flags = 0;
	float value = 0.0f;
	if (!read_closed_flags(cursor, KnownClassSubsystemPresenceFlags, presence) ||
		!read_nonzero_u32(cursor, subsystem_id) || !cursor.read_u16(canonical_index) ||
		!cursor.require(canonical_index <= 1023U, ValidationError::OutOfRange) ||
		!read_enum_u8(cursor, 13U, type) || !cursor.read_u8(reserved) ||
		!cursor.require(reserved == 0U, ValidationError::ReservedFlag) || !cursor.read_utf8(255U, 1U)) {
		return cursor.finish(true);
	}
	if ((presence & ClassSubsystemPresenceFlagAltName) != 0U && !cursor.read_utf8(255U)) {
		return cursor.finish(true);
	}
	if ((presence & ClassSubsystemPresenceFlagHudName) != 0U && !cursor.read_utf8(255U)) {
		return cursor.finish(true);
	}
	if (!read_vec3(cursor, -MaxPosition, MaxPosition)) {
		return cursor.finish(true);
	}
	if ((presence & ClassSubsystemPresenceFlagOrientation) != 0U && !read_quaternion(cursor)) {
		return cursor.finish(true);
	}
	if (!cursor.read_bounded_f32(0.0f, MaxLinearVelocity, value) ||
		!cursor.read_bounded_f32(0.0f, MaxQuantity, value)) {
		return cursor.finish(true);
	}
	if ((presence & ClassSubsystemPresenceFlagArmor) != 0U && !read_nonzero_u32(cursor, armor_id)) {
		return cursor.finish(true);
	}
	if (!read_closed_flags(cursor, KnownClassSubsystemStaticFlags, static_flags)) {
		return cursor.finish(true);
	}
	return cursor.finish(true);
}

bool read_class_scan(ValidationCursor& cursor) noexcept
{
	std::uint64_t required_time = 0;
	float value = 0.0f;
	return read_duration(cursor, OneDayUs, required_time) &&
		   cursor.read_bounded_f32(0.0f, MaxQuantity, value) && cursor.read_bounded_f32(0.0f, Pi, value);
}

ValidationError validate_session_state(ByteView payload, std::uint8_t protocol_minor) noexcept
{
	if (!is_supported_version_minor(protocol_minor)) {
		return ValidationError::UnsupportedMinor;
	}
	ValidationCursor cursor(payload);
	std::uint64_t presence = 0;
	std::uint64_t producer_id = 0;
	std::uint64_t sample_time = 0;
	std::uint8_t authority = 0;
	std::uint8_t visibility = 0;
	std::uint8_t phase = 0;
	std::uint8_t reserved = 0;
	std::uint32_t generation = 0;
	std::uint64_t capabilities = 0;
	std::uint64_t state_coverage = 0;
	std::uint64_t derived_coverage = 0;
	std::uint64_t exact_coverage = 0;
	std::uint64_t observed_player = 0;
	if (!read_closed_flags(cursor, KnownSessionStatePresenceFlags, presence) ||
		!read_nonzero_u64(cursor, producer_id) || !cursor.read_u64(sample_time) ||
		!read_enum_u8(cursor, 2U, authority) || !read_enum_u8(cursor, 0U, visibility) ||
		!read_enum_u8(cursor, 3U, phase) || !cursor.read_u8(reserved) ||
		!cursor.require(reserved == 0U, ValidationError::ReservedFlag) || !cursor.read_u32(generation) ||
		!cursor.require(generation != 0U, ValidationError::OutOfRange) ||
		!read_closed_flags(cursor, KnownCapabilities, capabilities) ||
		!read_closed_flags(cursor, known_state_domain_coverage_bits(protocol_minor), state_coverage) ||
		!read_closed_flags(cursor, KnownEventFamilyBits, derived_coverage) ||
		!read_closed_flags(cursor, KnownEventFamilyBits, exact_coverage)) {
		return cursor.finish();
	}
	const bool has_observed_player = (presence & SessionStatePresenceFlagObservedPlayer) != 0U;
	if (has_observed_player && !read_nonzero_u64(cursor, observed_player)) {
		return cursor.finish();
	}
	const auto communication_pair = static_cast<std::uint64_t>(CapabilityCommViewLocalAssets) |
									 static_cast<std::uint64_t>(CapabilityCommViewAuthoritativeSource);
	const auto video_pair = static_cast<std::uint64_t>(CapabilityTargetVideoH264) |
							 static_cast<std::uint64_t>(CapabilityTargetVideoRemoteRender);
	const auto required_domain = protocol_minor == VersionMinorV1_0 ? StateDomainCoverageBitCoreShip
															 : StateDomainCoverageBitPlayerKinematics;
	if (!cursor.require((state_coverage & required_domain) != 0U, ValidationError::InvalidAbsence) ||
		!cursor.require((capabilities & communication_pair) == 0U || (capabilities & communication_pair) == communication_pair,
			ValidationError::InvalidStateTransition) ||
		!cursor.require((capabilities & video_pair) == 0U || (capabilities & video_pair) == video_pair,
			ValidationError::InvalidStateTransition) ||
		!cursor.require(authority != static_cast<std::uint8_t>(AuthorityMode::MultiplayerClient) ||
			visibility == static_cast<std::uint8_t>(VisibilityMode::Cockpit),
			ValidationError::InvalidStateTransition) ||
		!cursor.require(authority != static_cast<std::uint8_t>(AuthorityMode::MultiplayerClient) || has_observed_player,
			ValidationError::InvalidAbsence) ||
		!cursor.require((exact_coverage & EventFamilyBitCommunication) == 0U ||
			(capabilities & communication_pair) == communication_pair,
			ValidationError::CapabilityNotNegotiated)) {
		return cursor.finish();
	}
	return cursor.finish();
}

ValidationError validate_mission_state(ByteView payload) noexcept
{
	ValidationCursor cursor(payload);
	std::uint64_t presence = 0;
	std::uint32_t generation = 0;
	std::uint8_t phase = 0;
	bool paused = false;
	float time_compression = 0.0f;
	std::uint64_t sample_time = 0;
	if (!read_closed_flags(cursor, KnownMissionStatePresenceFlags, presence) || !cursor.read_u32(generation) ||
		!cursor.require(generation != 0U, ValidationError::OutOfRange) || !read_enum_u8(cursor, 4U, phase) ||
		!cursor.read_bool(paused) || !read_reserved_zero(cursor, 2U) ||
		!cursor.read_bounded_f32(0.0f, 64.0f, time_compression) || !cursor.read_u64(sample_time)) {
		return cursor.finish();
	}
	const bool has_name = (presence & MissionStatePresenceFlagMissionName) != 0U;
	if (has_name && !cursor.read_utf8(255U)) {
		return cursor.finish();
	}
	if (!cursor.require(phase != static_cast<std::uint8_t>(MissionPhase::None) || !has_name,
		ValidationError::InvalidAbsence)) {
		return cursor.finish();
	}
	return cursor.finish();
}

ValidationError validate_class_manifest(ByteView payload) noexcept
{
	ValidationCursor cursor(payload);
	std::uint32_t manifest_generation = 0;
	std::uint32_t class_id = 0;
	std::uint64_t presence = 0;
	std::uint32_t species_id = 0;
	std::uint32_t ship_type_id = 0;
	float value = 0.0f;
	if (!read_nonzero_u32(cursor, manifest_generation) || !read_nonzero_u32(cursor, class_id) ||
		!read_closed_flags(cursor, KnownClassManifestPresenceFlags, presence) || !cursor.read_utf8(255U, 1U) ||
		!cursor.read_u32(species_id) || !cursor.read_u32(ship_type_id) ||
		!cursor.read_positive_f32(MaxQuantity, value) || !read_vec3(cursor, -MaxPosition, MaxPosition)) {
		return cursor.finish();
	}
	if ((presence & ClassManifestPresenceFlagInertia) != 0U && !read_mat3(cursor)) {
		return cursor.finish();
	}
	if ((presence & ClassManifestPresenceFlagDamping) != 0U &&
		(!cursor.read_bounded_f32(0.0f, MaxDurationSeconds, value) ||
			!cursor.read_bounded_f32(0.0f, MaxDurationSeconds, value) ||
			!read_vec3(cursor, 0.0f, MaxDurationSeconds))) {
		return cursor.finish();
	}
	if ((presence & ClassManifestPresenceFlagMotion) != 0U && !read_class_motion(cursor)) {
		return cursor.finish();
	}
	if ((presence & ClassManifestPresenceFlagHull) != 0U &&
		!cursor.read_bounded_f32(0.0f, MaxQuantity, value)) {
		return cursor.finish();
	}
	if ((presence & ClassManifestPresenceFlagShield) != 0U &&
		!cursor.read_bounded_f32(0.0f, MaxQuantity, value)) {
		return cursor.finish();
	}
	if ((presence & ClassManifestPresenceFlagEnergy) != 0U && !read_class_energy(cursor)) {
		return cursor.finish();
	}
	if ((presence & ClassManifestPresenceFlagAfterburner) != 0U && !read_class_afterburner(cursor)) {
		return cursor.finish();
	}
	if ((presence & ClassManifestPresenceFlagCountermeasure) != 0U && !read_class_countermeasure(cursor)) {
		return cursor.finish();
	}
	if ((presence & ClassManifestPresenceFlagBanks) != 0U) {
		std::uint16_t count = 0;
		if (!cursor.read_u16(count) || !cursor.require(count <= 192U, ValidationError::OutOfRange)) {
			return cursor.finish();
		}
		std::array<std::uint32_t, 192> bank_ids{};
		std::array<std::uint16_t, 4> family_counts{};
		for (std::uint16_t index = 0; index < count; ++index) {
			std::uint8_t version = 0;
			std::uint16_t item_size = 0;
			ByteView item;
			if (!cursor.read_u8(version) || !cursor.read_u16(item_size) ||
				!cursor.require(version == 1U, ValidationError::UnsupportedRecordVersion) || !cursor.take(item_size, item)) {
				return cursor.finish();
			}
			std::uint32_t bank_id = 0;
			std::uint8_t family = 0;
			if (const auto error = validate_class_bank(item, bank_id, family); error != ValidationError::None) {
				return error;
			}
			for (std::uint16_t previous = 0; previous < index; ++previous) {
				if (bank_ids[previous] == bank_id) {
					return ValidationError::DuplicateItemKey;
				}
			}
			bank_ids[index] = bank_id;
			if (++family_counts[family] > 64U) {
				return ValidationError::OutOfRange;
			}
		}
	}
	if ((presence & ClassManifestPresenceFlagSubsystems) != 0U) {
		std::uint16_t count = 0;
		if (!cursor.read_u16(count) || !cursor.require(count <= 1024U, ValidationError::OutOfRange)) {
			return cursor.finish();
		}
		std::array<std::uint32_t, 1024> subsystem_ids{};
		for (std::uint16_t index = 0; index < count; ++index) {
			std::uint8_t version = 0;
			std::uint16_t item_size = 0;
			ByteView item;
			if (!cursor.read_u8(version) || !cursor.read_u16(item_size) ||
				!cursor.require(version == 1U, ValidationError::UnsupportedRecordVersion) || !cursor.take(item_size, item)) {
				return cursor.finish();
			}
			std::uint32_t subsystem_id = 0;
			if (const auto error = validate_class_subsystem(item, subsystem_id); error != ValidationError::None) {
				return error;
			}
			for (std::uint16_t previous = 0; previous < index; ++previous) {
				if (subsystem_ids[previous] == subsystem_id) {
					return ValidationError::DuplicateItemKey;
				}
			}
			subsystem_ids[index] = subsystem_id;
		}
	}
	if ((presence & ClassManifestPresenceFlagScan) != 0U && !read_class_scan(cursor)) {
		return cursor.finish();
	}
	if ((presence & ClassManifestPresenceFlagGlide) != 0U &&
		!cursor.read_bounded_f32(0.0f, MaxLinearVelocity, value)) {
		return cursor.finish();
	}
	if ((presence & ClassManifestPresenceFlagAutoaim) != 0U &&
		!cursor.read_bounded_f32(0.0f, Pi, value)) {
		return cursor.finish();
	}
	if ((presence & ClassManifestPresenceFlagRadarIcon) != 0U) {
		std::uint32_t radar_icon_id = 0;
		if (!cursor.read_u32(radar_icon_id)) {
			return cursor.finish();
		}
	}
	return cursor.finish();
}

ValidationError validate_weapon_manifest(ByteView payload) noexcept
{
	ValidationCursor cursor(payload);
	std::uint32_t manifest_generation = 0;
	std::uint32_t weapon_class_id = 0;
	std::uint64_t presence = 0;
	std::uint8_t subtype = 0;
	std::uint64_t weapon_flags = 0;
	float value = 0.0f;
	std::uint64_t duration = 0;
	if (!read_nonzero_u32(cursor, manifest_generation) || !read_nonzero_u32(cursor, weapon_class_id) ||
		!read_closed_flags(cursor, KnownWeaponManifestPresenceFlags, presence) || !cursor.read_utf8(255U, 1U)) {
		return cursor.finish();
	}
	if ((presence & WeaponManifestPresenceFlagTitle) != 0U && !cursor.read_utf8(255U)) {
		return cursor.finish();
	}
	if (!read_enum_u8(cursor, 5U, subtype) || !read_closed_flags(cursor, KnownWeaponClassFlags, weapon_flags) ||
		!cursor.read_bounded_f32(0.0f, MaxLinearVelocity, value)) {
		return cursor.finish();
	}
	if ((presence & WeaponManifestPresenceFlagAcceleration) != 0U &&
		!read_duration(cursor, OneHourUs, duration)) {
		return cursor.finish();
	}
	if (!cursor.read_bounded_f32(0.0f, MaxQuantity, value) ||
		!cursor.read_bounded_f32(-1024.0f, 1024.0f, value) || !read_duration(cursor, OneDayUs, duration)) {
		return cursor.finish();
	}
	if ((presence & WeaponManifestPresenceFlagRanges) != 0U) {
		float minimum = 0.0f;
		float optimal = 0.0f;
		float maximum = 0.0f;
		if (!cursor.read_bounded_f32(0.0f, MaxQuantity, minimum) ||
			!cursor.read_bounded_f32(0.0f, MaxQuantity, optimal) ||
			!cursor.read_bounded_f32(0.0f, MaxQuantity, maximum) ||
			!cursor.require(minimum <= optimal && optimal <= maximum, ValidationError::OutOfRange)) {
			return cursor.finish();
		}
	}
	if ((presence & WeaponManifestPresenceFlagFire) != 0U &&
		(!read_duration(cursor, OneHourUs, duration) || !cursor.read_bounded_f32(0.0f, MaxQuantity, value))) {
		return cursor.finish();
	}
	if ((presence & WeaponManifestPresenceFlagDamage) != 0U) {
		std::uint32_t damage_type = 0;
		std::uint32_t effect_flags = 0;
		if (!cursor.read_bounded_f32(0.0f, MaxQuantity, value) || !cursor.read_u32(damage_type) ||
			!read_closed_flags(cursor, KnownWeaponEffectFlags, effect_flags)) {
			return cursor.finish();
		}
	}
	if ((presence & WeaponManifestPresenceFlagGuidance) != 0U) {
		std::uint8_t guidance = 0;
		if (!read_enum_u8(cursor, 5U, guidance) || !cursor.read_bounded_f32(0.0f, Pi, value)) {
			return cursor.finish();
		}
	}
	if ((presence & WeaponManifestPresenceFlagLock) != 0U &&
		(!read_duration(cursor, OneHourUs, duration) || !cursor.read_bounded_f32(0.0f, Pi, value))) {
		return cursor.finish();
	}
	if (!cursor.read_bounded_f32(-16.0f, 16.0f, value)) {
		return cursor.finish();
	}
	if ((presence & WeaponManifestPresenceFlagCargoRearm) != 0U) {
		std::uint32_t reloaded_per_batch = 0;
		if (!cursor.read_positive_f32(MaxLinearVelocity, value) || !read_duration(cursor, OneHourUs, duration) ||
			!cursor.read_u32(reloaded_per_batch) ||
			!cursor.require(reloaded_per_batch >= 1U && reloaded_per_batch <= 1'000'000U,
				ValidationError::OutOfRange) ||
			!cursor.require((weapon_flags & WeaponClassFlagAmmoless) == 0U, ValidationError::InvalidAbsence)) {
			return cursor.finish();
		}
	}
	if ((presence & WeaponManifestPresenceFlagBurst) != 0U) {
		std::uint16_t count = 0;
		if (!cursor.read_u16(count) || !cursor.require(count >= 1U && count <= 4096U, ValidationError::OutOfRange) ||
			!read_duration(cursor, OneHourUs, duration)) {
			return cursor.finish();
		}
	}
	if ((presence & WeaponManifestPresenceFlagSwarm) != 0U) {
		std::uint16_t swarm_count = 0;
		std::uint16_t shots_per_trigger = 0;
		if (!cursor.read_u16(swarm_count) || !cursor.read_u16(shots_per_trigger) ||
			!cursor.require(swarm_count >= 1U && swarm_count <= 4096U && shots_per_trigger >= 1U &&
				shots_per_trigger <= 4096U,
				ValidationError::OutOfRange)) {
			return cursor.finish();
		}
	}
	if ((presence & WeaponManifestPresenceFlagCountermeasure) != 0U &&
		!cursor.read_bounded_f32(0.0f, MaxQuantity, value)) {
		return cursor.finish();
	}
	return cursor.finish();
}

ValidationError validate_entity_lifecycle(ByteView payload) noexcept
{
	ValidationCursor cursor(payload);
	std::uint64_t entity_id = 0;
	std::uint64_t presence = 0;
	std::uint64_t sample_time = 0;
	std::uint8_t object_type = 0;
	std::uint8_t lifecycle_phase = 0;
	std::uint32_t lifecycle_flags = 0;
	if (!read_nonzero_u64(cursor, entity_id) ||
		!read_closed_flags(cursor, KnownEntityLifecyclePresenceFlags, presence) || !cursor.read_u64(sample_time) ||
		!read_enum_u8(cursor, 8U, object_type) || !read_enum_u8(cursor, 5U, lifecycle_phase) ||
		!read_closed_flags(cursor, KnownEntityLifecycleFlags, lifecycle_flags)) {
		return cursor.finish();
	}
	if ((presence & EntityLifecyclePresenceFlagSignature) != 0U) {
		std::uint32_t value = 0;
		if (!cursor.read_u32(value)) {
			return cursor.finish();
		}
	}
	if ((presence & EntityLifecyclePresenceFlagNetSignature) != 0U) {
		std::uint32_t value = 0;
		if (!cursor.read_u32(value)) {
			return cursor.finish();
		}
	}
	if ((presence & EntityLifecyclePresenceFlagClassReference) != 0U) {
		std::uint32_t class_id = 0;
		if (!read_nonzero_u32(cursor, class_id) ||
			!cursor.require(object_type == static_cast<std::uint8_t>(ObjectType::Ship) ||
				object_type == static_cast<std::uint8_t>(ObjectType::Weapon),
				ValidationError::InvalidAbsence)) {
			return cursor.finish();
		}
	}
	if ((presence & EntityLifecyclePresenceFlagParent) != 0U) {
		std::uint64_t parent = 0;
		if (!read_nonzero_u64(cursor, parent)) {
			return cursor.finish();
		}
	}
	const bool has_arrival = (presence & EntityLifecyclePresenceFlagArrivalMode) != 0U;
	if (has_arrival) {
		std::uint8_t mode = 0;
		if (!read_enum_u8(cursor, 3U, mode)) {
			return cursor.finish();
		}
	}
	const bool has_departure = (presence & EntityLifecyclePresenceFlagDepartureMode) != 0U;
	if (has_departure) {
		std::uint8_t mode = 0;
		if (!read_enum_u8(cursor, 3U, mode)) {
			return cursor.finish();
		}
	}
	const auto non_ship_groups = EntityLifecyclePresenceFlagNonShipNames |
							 EntityLifecyclePresenceFlagNonShipTeamIff |
							 EntityLifecyclePresenceFlagNonShipRadius;
	if ((presence & EntityLifecyclePresenceFlagNonShipNames) != 0U &&
		(!cursor.read_utf8(255U, 1U) || !cursor.read_utf8(255U) || !cursor.read_utf8(127U))) {
		return cursor.finish();
	}
	if ((presence & EntityLifecyclePresenceFlagNonShipTeamIff) != 0U) {
		std::uint32_t team = 0;
		std::uint32_t iff = 0;
		if (!cursor.read_u32(team) || !cursor.read_u32(iff) ||
			!cursor.require(team <= 65'535U && iff <= 65'535U, ValidationError::OutOfRange)) {
			return cursor.finish();
		}
	}
	if ((presence & EntityLifecyclePresenceFlagNonShipRadius) != 0U) {
		float radius = 0.0f;
		if (!cursor.read_bounded_f32(0.0f, MaxLinearVelocity, radius)) {
			return cursor.finish();
		}
	}
	if (!cursor.require(object_type != static_cast<std::uint8_t>(ObjectType::Ship) ||
			(presence & non_ship_groups) == 0U,
			ValidationError::InvalidAbsence) ||
		!cursor.require(lifecycle_phase != static_cast<std::uint8_t>(LifecyclePhase::Spawning) || has_arrival,
			ValidationError::InvalidAbsence) ||
		!cursor.require((lifecycle_flags & EntityLifecycleFlagBomb) == 0U ||
			object_type == static_cast<std::uint8_t>(ObjectType::Weapon),
			ValidationError::InvalidStateTransition) ||
		!cursor.require((lifecycle_phase == static_cast<std::uint8_t>(LifecyclePhase::Departing)) == has_departure,
			ValidationError::InvalidAbsence)) {
		return cursor.finish();
	}
	return cursor.finish();
}

ValidationError validate_ship_identity(ByteView payload) noexcept
{
	ValidationCursor cursor(payload);
	std::uint64_t entity_id = 0;
	std::uint64_t presence = 0;
	std::uint64_t sample_time = 0;
	std::uint32_t class_id = 0;
	std::uint32_t species_id = 0;
	std::uint32_t team_id = 0;
	std::uint32_t iff_id = 0;
	std::uint16_t role_flags = 0;
	float value = 0.0f;
	if (!read_nonzero_u64(cursor, entity_id) || !read_closed_flags(cursor, KnownShipIdentityPresenceFlags, presence) ||
		!cursor.read_u64(sample_time) || !read_nonzero_u32(cursor, class_id) || !cursor.read_utf8(255U, 1U)) {
		return cursor.finish();
	}
	if ((presence & ShipIdentityPresenceFlagDisplayName) != 0U && !cursor.read_utf8(255U)) {
		return cursor.finish();
	}
	if ((presence & ShipIdentityPresenceFlagCallsign) != 0U && !cursor.read_utf8(127U)) {
		return cursor.finish();
	}
	if (!cursor.read_u32(species_id) || !cursor.read_u32(team_id) || !cursor.read_u32(iff_id) ||
		!cursor.require(team_id <= 65'535U && iff_id <= 65'535U, ValidationError::OutOfRange) ||
		!read_closed_flags(cursor, KnownShipRoleFlags, role_flags) ||
		!cursor.read_bounded_f32(0.0f, MaxLinearVelocity, value)) {
		return cursor.finish();
	}
	if ((presence & ShipIdentityPresenceFlagWing) != 0U) {
		std::uint32_t wing_id = 0;
		std::uint16_t wing_position = 0;
		if (!read_nonzero_u32(cursor, wing_id) || !cursor.read_u16(wing_position) ||
			!cursor.require(wing_position <= 4095U, ValidationError::OutOfRange) || !cursor.read_utf8(127U, 1U)) {
			return cursor.finish();
		}
	}
	if ((presence & ShipIdentityPresenceFlagLogicalSize) != 0U &&
		!cursor.read_bounded_f32(0.0f, MaxLinearVelocity, value)) {
		return cursor.finish();
	}
	if ((presence & ShipIdentityPresenceFlagSensorVisibility) != 0U) {
		std::uint16_t flags = 0;
		if (!read_closed_flags(cursor, KnownSensorVisibilityFlags, flags)) {
			return cursor.finish();
		}
	}
	return cursor.finish();
}

ValidationError validate_flight_state(ByteView payload) noexcept
{
	ValidationCursor cursor(payload);
	std::uint64_t entity_id = 0;
	std::uint64_t presence = 0;
	std::uint64_t sample_time = 0;
	std::uint32_t physics_flags = 0;
	float value = 0.0f;
	if (!read_nonzero_u64(cursor, entity_id) || !read_closed_flags(cursor, KnownFlightStatePresenceFlags, presence) ||
		!cursor.read_u64(sample_time) || !read_vec3(cursor, -MaxPosition, MaxPosition) || !read_quaternion(cursor) ||
		!read_vec3(cursor, -MaxLinearVelocity, MaxLinearVelocity) ||
		!read_vec3(cursor, -MaxRotationalVelocity, MaxRotationalVelocity) ||
		!cursor.read_bounded_f32(0.0f, MaxLinearVelocity, value) ||
		!read_closed_flags(cursor, KnownPhysicsModeFlags, physics_flags)) {
		return cursor.finish();
	}
	if ((presence & FlightStatePresenceFlagDesiredVel) != 0U &&
		!read_vec3(cursor, -MaxLinearVelocity, MaxLinearVelocity)) {
		return cursor.finish();
	}
	if ((presence & FlightStatePresenceFlagDesiredRotvel) != 0U &&
		!read_vec3(cursor, -MaxRotationalVelocity, MaxRotationalVelocity)) {
		return cursor.finish();
	}
	if ((presence & FlightStatePresenceFlagPrevRampVel) != 0U &&
		!read_vec3(cursor, -MaxLinearVelocity, MaxLinearVelocity)) {
		return cursor.finish();
	}
	if ((presence & FlightStatePresenceFlagVelocityCaps) != 0U &&
		(!read_vec3(cursor, 0.0f, MaxLinearVelocity) || !read_vec3(cursor, 0.0f, MaxLinearVelocity) ||
			!read_vec3(cursor, 0.0f, MaxLinearVelocity))) {
		return cursor.finish();
	}
	if ((presence & FlightStatePresenceFlagRotationCaps) != 0U &&
		!read_vec3(cursor, 0.0f, MaxRotationalVelocity)) {
		return cursor.finish();
	}
	if ((presence & FlightStatePresenceFlagRearCap) != 0U &&
		!cursor.read_bounded_f32(0.0f, MaxLinearVelocity, value)) {
		return cursor.finish();
	}
	if ((presence & FlightStatePresenceFlagGlideCaps) != 0U &&
		(!cursor.read_bounded_f32(0.0f, MaxLinearVelocity, value) ||
			!cursor.read_bounded_f32(0.0f, MaxLinearVelocity, value))) {
		return cursor.finish();
	}
	if ((presence & FlightStatePresenceFlagGravity) != 0U &&
		!cursor.read_bounded_f32(-1024.0f, 1024.0f, value)) {
		return cursor.finish();
	}
	if ((presence & FlightStatePresenceFlagTimeConstants) != 0U) {
		for (std::size_t index = 0; index < 6U; ++index) {
			if (!cursor.read_bounded_f32(0.0f, MaxDurationSeconds, value)) {
				return cursor.finish();
			}
		}
	}
	if ((presence & FlightStatePresenceFlagRotdamp) != 0U &&
		!cursor.read_bounded_f32(0.0f, MaxDurationSeconds, value)) {
		return cursor.finish();
	}
	if ((presence & FlightStatePresenceFlagSideSlip) != 0U &&
		!cursor.read_bounded_f32(0.0f, MaxDurationSeconds, value)) {
		return cursor.finish();
	}
	if ((presence & FlightStatePresenceFlagCosmeticThrust) != 0U &&
		(!read_vec3(cursor, -1.0f, 1.0f) || !read_vec3(cursor, -1.0f, 1.0f))) {
		return cursor.finish();
	}
	return cursor.finish();
}

ValidationError validate_control_state(ByteView payload) noexcept
{
	ValidationCursor cursor(payload);
	std::uint64_t entity_id = 0;
	std::uint64_t presence = 0;
	std::uint64_t sample_time = 0;
	float value = 0.0f;
	std::uint8_t mode = 0;
	std::uint32_t flags = 0;
	if (!read_nonzero_u64(cursor, entity_id) || !read_closed_flags(cursor, KnownControlStatePresenceFlags, presence) ||
		!cursor.read_u64(sample_time)) {
		return cursor.finish();
	}
	for (std::size_t index = 0; index < 6U; ++index) {
		if (!cursor.read_bounded_f32(-1.0f, 1.0f, value)) {
			return cursor.finish();
		}
	}
	if (!read_enum_u8(cursor, 4U, mode) || !read_closed_flags(cursor, KnownControlFlags, flags)) {
		return cursor.finish();
	}
	if ((presence & ControlStatePresenceFlagCruise) != 0U &&
		!cursor.read_bounded_f32(-100.0f, 100.0f, value)) {
		return cursor.finish();
	}
	if ((presence & ControlStatePresenceFlagRequestCounters) != 0U) {
		std::uint16_t count = 0;
		if (!cursor.read_u16(count) || !cursor.read_u16(count) || !cursor.read_u16(count)) {
			return cursor.finish();
		}
	}
	if ((presence & ControlStatePresenceFlagFlightCursor) != 0U &&
		(!cursor.read_bounded_f32(-Pi, Pi, value) || !cursor.read_bounded_f32(-Pi, Pi, value) ||
			!cursor.read_bounded_f32(0.0f, 1.0f, value) || !cursor.read_bounded_f32(0.0f, 1.0f, value))) {
		return cursor.finish();
	}
	return cursor.finish();
}

struct ContributorKey {
	std::uint64_t source_entity_id = 0;
	std::uint32_t weapon_class_id = 0;
};

ValidationError validate_damage_state(ByteView payload) noexcept
{
	ValidationCursor cursor(payload);
	std::uint64_t entity_id = 0;
	std::uint64_t presence = 0;
	std::uint64_t sample_time = 0;
	float hull = 0.0f;
	float maximum_hull = 0.0f;
	std::uint16_t protection_flags = 0;
	if (!read_nonzero_u64(cursor, entity_id) || !read_closed_flags(cursor, KnownDamageStatePresenceFlags, presence) ||
		!cursor.read_u64(sample_time) || !cursor.read_bounded_f32(0.0f, MaxQuantity, hull) ||
		!cursor.read_positive_f32(MaxQuantity, maximum_hull) ||
		!cursor.require(hull <= maximum_hull, ValidationError::OutOfRange) ||
		!read_closed_flags(cursor, KnownProtectionFlags, protection_flags)) {
		return cursor.finish();
	}
	if ((presence & DamageStatePresenceFlagSimHull) != 0U) {
		float sim_hull = 0.0f;
		if (!cursor.read_bounded_f32(0.0f, maximum_hull, sim_hull)) {
			return cursor.finish();
		}
	}
	if ((presence & DamageStatePresenceFlagArmor) != 0U) {
		std::uint32_t armor_id = 0;
		if (!read_nonzero_u32(cursor, armor_id)) {
			return cursor.finish();
		}
	}
	if ((presence & DamageStatePresenceFlagGuardian) != 0U) {
		float threshold = 0.0f;
		if (!cursor.read_bounded_f32(0.0f, maximum_hull, threshold)) {
			return cursor.finish();
		}
	}
	if ((presence & DamageStatePresenceFlagCumulativeDamage) != 0U) {
		float cumulative = 0.0f;
		if (!cursor.read_bounded_f32(0.0f, MaxQuantity, cumulative)) {
			return cursor.finish();
		}
	}
	if ((presence & DamageStatePresenceFlagLastDamage) != 0U) {
		std::uint64_t source = 0;
		std::uint32_t weapon = 0;
		if (!cursor.read_u64(source) || !cursor.read_u32(weapon)) {
			return cursor.finish();
		}
	}
	if ((presence & DamageStatePresenceFlagContributors) != 0U) {
		std::uint16_t count = 0;
		if (!read_fixed_list_header(cursor, 64U, 20U, count)) {
			return cursor.finish();
		}
		std::array<ContributorKey, 64> keys{};
		for (std::uint16_t index = 0; index < count; ++index) {
			std::uint32_t reserved = 0;
			float accumulated = 0.0f;
			if (!cursor.read_u64(keys[index].source_entity_id) || !cursor.read_u32(keys[index].weapon_class_id) ||
				!cursor.read_u32(reserved) || !cursor.require(reserved == 0U, ValidationError::ReservedFlag) ||
				!cursor.read_bounded_f32(0.0f, MaxQuantity, accumulated)) {
				return cursor.finish();
			}
			for (std::uint16_t previous = 0; previous < index; ++previous) {
				if (keys[previous].source_entity_id == keys[index].source_entity_id &&
					keys[previous].weapon_class_id == keys[index].weapon_class_id) {
					return ValidationError::DuplicateItemKey;
				}
			}
		}
	}
	return cursor.finish();
}

ValidationError validate_shield_state(ByteView payload) noexcept
{
	ValidationCursor cursor(payload);
	std::uint64_t entity_id = 0;
	std::uint64_t presence = 0;
	std::uint64_t sample_time = 0;
	bool has_shields = false;
	std::uint16_t segment_count = 0;
	std::array<float, 64> current_hits{};
	if (!read_nonzero_u64(cursor, entity_id) || !read_closed_flags(cursor, KnownShieldStatePresenceFlags, presence) ||
		!cursor.read_u64(sample_time) || !cursor.read_bool(has_shields) || !cursor.read_u16(segment_count) ||
		!cursor.require(segment_count <= 64U, ValidationError::OutOfRange) || !read_reserved_zero(cursor, 2U)) {
		return cursor.finish();
	}
	for (std::uint16_t index = 0; index < segment_count; ++index) {
		if (!cursor.read_bounded_f32(0.0f, MaxQuantity, current_hits[index])) {
			return cursor.finish();
		}
	}
	for (std::uint16_t index = 0; index < segment_count; ++index) {
		float maximum = 0.0f;
		if (!cursor.read_positive_f32(MaxQuantity, maximum) ||
			!cursor.require(current_hits[index] <= maximum, ValidationError::OutOfRange)) {
			return cursor.finish();
		}
	}
	float value = 0.0f;
	if ((presence & ShieldStatePresenceFlagRechargeMax) != 0U &&
		!cursor.read_bounded_f32(0.0f, MaxQuantity, value)) {
		return cursor.finish();
	}
	if ((presence & ShieldStatePresenceFlagRegenRate) != 0U &&
		!cursor.read_bounded_f32(0.0f, MaxQuantity, value)) {
		return cursor.finish();
	}
	if ((presence & ShieldStatePresenceFlagDeferredTransfer) != 0U &&
		!cursor.read_bounded_f32(-MaxQuantity, MaxQuantity, value)) {
		return cursor.finish();
	}
	if (!cursor.require(has_shields || (segment_count == 0U && presence == 0U), ValidationError::InvalidAbsence)) {
		return cursor.finish();
	}
	return cursor.finish();
}

} // namespace

ValidationError validate_business_record_1_10(RecordType type,
	ByteView payload,
	std::uint8_t protocol_minor) noexcept
{
	switch (type) {
	case RecordType::SessionState:
		return validate_session_state(payload, protocol_minor);
	case RecordType::MissionState:
		return validate_mission_state(payload);
	case RecordType::ClassManifest:
		return validate_class_manifest(payload);
	case RecordType::WeaponManifest:
		return validate_weapon_manifest(payload);
	case RecordType::EntityLifecycle:
		return validate_entity_lifecycle(payload);
	case RecordType::ShipIdentity:
		return validate_ship_identity(payload);
	case RecordType::FlightState:
		return validate_flight_state(payload);
	case RecordType::ControlState:
		return validate_control_state(payload);
	case RecordType::DamageState:
		return validate_damage_state(payload);
	case RecordType::ShieldState:
		return validate_shield_state(payload);
	default:
		return ValidationError::UnknownRequiredRecord;
	}
}

} // namespace telemetry::protocol::detail
