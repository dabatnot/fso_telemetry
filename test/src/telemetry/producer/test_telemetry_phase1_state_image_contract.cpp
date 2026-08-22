#include "telemetry/engine_adapter.h"
#include "telemetry/protocol/packet_reader.h"
#include "telemetry/protocol/telemetry_business_state_validation.h"
#include "telemetry/protocol/telemetry_protocol_constants.h"
#include "telemetry/protocol/telemetry_replication.h"

#if __has_include("telemetry/phase1_state_image.h")
#include "telemetry/phase1_state_image.h"
#define FSO_HAS_TELEMETRY_PHASE1_STATE_IMAGE 1
#else
#define FSO_HAS_TELEMETRY_PHASE1_STATE_IMAGE 0

namespace telemetry::detail {

struct MissionObservationDto {
	std::uint64_t producer_sample_time_us = 0U;
	std::uint32_t mission_generation = 0U;
	protocol::MissionPhase phase = protocol::MissionPhase::None;
	bool paused = false;
	float time_compression = 0.0F;
};

struct Phase1StateImageInput {
	std::uint64_t producer_id = 0U;
	std::uint32_t negotiated_capability_generation = 0U;
	MissionObservationDto mission{};
	CaptureResult player_capture{};
	PlayerKinematicsSample player{};
};

enum class Phase1StateImageBuildStatus : std::uint8_t {
	Created = 0,
	InvalidInput,
	AllocationFailed,
	Count,
};

inline Phase1StateImageBuildStatus build_phase1_state_image(
	const Phase1StateImageInput&, protocol::StateImage&) noexcept
{
	return Phase1StateImageBuildStatus::InvalidInput;
}

} // namespace telemetry::detail
#endif

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace {

namespace detail = telemetry::detail;
namespace protocol = telemetry::protocol;

using protocol::RecordType;

static_assert(std::is_standard_layout_v<detail::MissionObservationDto> &&
	std::is_trivially_copyable_v<detail::MissionObservationDto>);
static_assert(std::is_same_v<decltype(detail::MissionObservationDto::producer_sample_time_us), std::uint64_t> &&
	std::is_same_v<decltype(detail::MissionObservationDto::mission_generation), std::uint32_t> &&
	std::is_same_v<decltype(detail::MissionObservationDto::phase), protocol::MissionPhase> &&
	std::is_same_v<decltype(detail::MissionObservationDto::paused), bool> &&
	std::is_same_v<decltype(detail::MissionObservationDto::time_compression), float>);
static_assert(std::is_same_v<decltype(&detail::build_phase1_state_image),
	detail::Phase1StateImageBuildStatus (*)(
		const detail::Phase1StateImageInput&, protocol::StateImage&) noexcept>);

constexpr std::uint64_t SampleTime = 555'000U;
constexpr std::uint64_t PlayerEntityId = 42U;

protocol::ByteView bytes(const std::vector<std::uint8_t>& value) noexcept
{
	return {value.data(), value.size()};
}

const protocol::StateAtom* find_record(const protocol::StateImage& image, RecordType type)
{
	for (const auto& record : image.records()) {
		if (record.key.record_type == static_cast<std::uint16_t>(type)) return &record;
	}
	return nullptr;
}

detail::Phase1StateImageInput valid_input()
{
	detail::Phase1StateImageInput input{};
	input.producer_id = 0x1020304050607080ULL;
	input.negotiated_capability_generation = 3U;
	input.mission.producer_sample_time_us = SampleTime;
	input.mission.mission_generation = 9U;
	input.mission.phase = protocol::MissionPhase::Active;
	input.mission.paused = true;
	input.mission.time_compression = 1.5F;
	input.player_capture = {detail::CaptureStatus::Valid, detail::CaptureReason::None};
	input.player.entity_id = PlayerEntityId;
	input.player.value.producer_sample_time_us = SampleTime;
	input.player.value.position_world = {1.0F, 2.0F, 3.0F};
	input.player.value.orientation_local_to_world = {0.5F, 0.5F, 0.5F, 0.5F};
	input.player.value.velocity_world = {4.0F, 5.0F, 6.0F};
	input.player.value.rotational_velocity_local = {0.1F, 0.2F, 0.3F};
	input.player.value.radius = 7.0F;
	input.player.value.physics_mode_flags = protocol::PhysicsModeFlagAfterburner |
		protocol::PhysicsModeFlagImmobile;
	return input;
}

void expect_fstl11_valid(const protocol::StateImage& image)
{
	protocol::BusinessStateValidationContext context{};
	context.protocol_minor = protocol::VersionMinor;
	context.required_manifest_id = 0U;
	protocol::BusinessStateImageValidator validator(context);
	EXPECT_EQ(protocol::ValidationError::None, validator.validate(image));

	context.protocol_minor = 0U;
	protocol::BusinessStateImageValidator fstl10_validator(context);
	EXPECT_NE(protocol::ValidationError::None, fstl10_validator.validate(image))
		<< "PLAYER_KINEMATICS is an FSTL 1.1-only image.";
}

void expect_session_record(const protocol::StateAtom& atom, bool has_player)
{
	protocol::PacketReader reader(bytes(atom.value));
	std::uint64_t presence = 0U;
	std::uint64_t producer_id = 0U;
	std::uint64_t sample_time = 0U;
	std::uint8_t authority = 0U, visibility = 0U, phase = 0U, reserved = 0U;
	std::uint32_t generation = 0U;
	std::uint64_t capabilities = 0U, state_coverage = 0U, derived = 0U, exact = 0U;
	ASSERT_TRUE(reader.read_u64(presence));
	ASSERT_TRUE(reader.read_u64(producer_id));
	ASSERT_TRUE(reader.read_u64(sample_time));
	ASSERT_TRUE(reader.read_u8(authority));
	ASSERT_TRUE(reader.read_u8(visibility));
	ASSERT_TRUE(reader.read_u8(phase));
	ASSERT_TRUE(reader.read_u8(reserved));
	ASSERT_TRUE(reader.read_u32(generation));
	ASSERT_TRUE(reader.read_u64(capabilities));
	ASSERT_TRUE(reader.read_u64(state_coverage));
	ASSERT_TRUE(reader.read_u64(derived));
	ASSERT_TRUE(reader.read_u64(exact));
	EXPECT_EQ(has_player ? static_cast<std::uint64_t>(protocol::SessionStatePresenceFlagObservedPlayer) : 0U, presence);
	EXPECT_EQ(0x1020304050607080ULL, producer_id);
	EXPECT_EQ(SampleTime, sample_time);
	EXPECT_EQ(static_cast<std::uint8_t>(protocol::AuthorityMode::Solo), authority);
	EXPECT_EQ(static_cast<std::uint8_t>(protocol::VisibilityMode::Cockpit), visibility);
	EXPECT_EQ(static_cast<std::uint8_t>(protocol::SessionPhase::Synchronizing), phase);
	EXPECT_EQ(0U, reserved);
	EXPECT_EQ(3U, generation);
	EXPECT_EQ(0U, capabilities);
	EXPECT_EQ(protocol::StateDomainCoverageBitPlayerKinematics, state_coverage);
	EXPECT_EQ(0U, derived);
	EXPECT_EQ(0U, exact);
	if (has_player) {
		std::uint64_t player = 0U;
		ASSERT_TRUE(reader.read_u64(player));
		EXPECT_EQ(PlayerEntityId, player);
	}
	EXPECT_TRUE(reader.at_end());
}

void expect_mission_record(const protocol::StateAtom& atom)
{
	protocol::PacketReader reader(bytes(atom.value));
	std::uint64_t presence = 0U, sample_time = 0U;
	std::uint32_t generation = 0U;
	std::uint8_t phase = 0U;
	bool paused = false;
	std::uint16_t reserved = 1U;
	float compression = 0.0F;
	ASSERT_TRUE(reader.read_u64(presence));
	ASSERT_TRUE(reader.read_u32(generation));
	ASSERT_TRUE(reader.read_u8(phase));
	ASSERT_TRUE(reader.read_bool8(paused));
	ASSERT_TRUE(reader.read_u16(reserved));
	ASSERT_TRUE(reader.read_f32(compression));
	ASSERT_TRUE(reader.read_u64(sample_time));
	EXPECT_EQ(0U, presence) << "Phase 1 never publishes mission_name.";
	EXPECT_EQ(9U, generation);
	EXPECT_EQ(static_cast<std::uint8_t>(protocol::MissionPhase::Active), phase);
	EXPECT_TRUE(paused);
	EXPECT_EQ(0U, reserved);
	EXPECT_FLOAT_EQ(1.5F, compression);
	EXPECT_EQ(SampleTime, sample_time);
	EXPECT_TRUE(reader.at_end());
}

void expect_lifecycle_record(const protocol::StateAtom& atom)
{
	protocol::PacketReader reader(bytes(atom.value));
	std::uint64_t entity = 0U, presence = 1U, sample_time = 0U;
	std::uint8_t object_type = 0U, phase = 0U;
	std::uint32_t flags = 1U;
	ASSERT_TRUE(reader.read_u64(entity));
	ASSERT_TRUE(reader.read_u64(presence));
	ASSERT_TRUE(reader.read_u64(sample_time));
	ASSERT_TRUE(reader.read_u8(object_type));
	ASSERT_TRUE(reader.read_u8(phase));
	ASSERT_TRUE(reader.read_u32(flags));
	EXPECT_EQ(PlayerEntityId, entity);
	EXPECT_EQ(0U, presence);
	EXPECT_EQ(SampleTime, sample_time);
	EXPECT_EQ(static_cast<std::uint8_t>(protocol::ObjectType::Ship), object_type);
	EXPECT_EQ(static_cast<std::uint8_t>(protocol::LifecyclePhase::Active), phase);
	EXPECT_EQ(0U, flags);
	EXPECT_TRUE(reader.at_end());
}

void expect_flight_record(const protocol::StateAtom& atom)
{
	protocol::PacketReader reader(bytes(atom.value));
	std::uint64_t entity = 0U, presence = 1U, sample_time = 0U;
	ASSERT_TRUE(reader.read_u64(entity));
	ASSERT_TRUE(reader.read_u64(presence));
	ASSERT_TRUE(reader.read_u64(sample_time));
	EXPECT_EQ(PlayerEntityId, entity);
	EXPECT_EQ(0U, presence);
	EXPECT_EQ(SampleTime, sample_time);
	const std::array<float, 13> expected{{1.0F, 2.0F, 3.0F, 0.5F, 0.5F, 0.5F, 0.5F,
		4.0F, 5.0F, 6.0F, 0.1F, 0.2F, 0.3F}};
	for (const auto expected_value : expected) {
		float actual = 0.0F;
		ASSERT_TRUE(reader.read_f32(actual));
		EXPECT_FLOAT_EQ(expected_value, actual);
	}
	float radius = 0.0F;
	std::uint32_t physics = 0U;
	ASSERT_TRUE(reader.read_f32(radius));
	ASSERT_TRUE(reader.read_u32(physics));
	EXPECT_FLOAT_EQ(7.0F, radius);
	EXPECT_EQ(protocol::PhysicsModeFlagAfterburner | protocol::PhysicsModeFlagImmobile, physics);
	EXPECT_TRUE(reader.at_end());
}

void expect_invalid_input_transactional(const detail::Phase1StateImageInput& invalid)
{
	protocol::StateImage absent;
	EXPECT_EQ(detail::Phase1StateImageBuildStatus::InvalidInput,
		detail::build_phase1_state_image(invalid, absent));
	EXPECT_TRUE(absent.empty()) << "Invalid input must not publish a partial image.";

	protocol::StateImage existing;
	ASSERT_EQ(detail::Phase1StateImageBuildStatus::Created,
		detail::build_phase1_state_image(valid_input(), existing));
	const auto before = existing;
	EXPECT_EQ(detail::Phase1StateImageBuildStatus::InvalidInput,
		detail::build_phase1_state_image(invalid, existing));
	EXPECT_EQ(before, existing) << "Invalid input must leave an existing image unchanged.";
}

TEST(TelemetryPhase1StateImageContract, ValidPlayerBuildsExactCanonicalFstl11Image)
{
	SCOPED_TRACE(FSO_HAS_TELEMETRY_PHASE1_STATE_IMAGE
		? "production phase1_state_image contract present"
		: "P8.1 RED: production phase1_state_image contract is absent");
	protocol::StateImage image;
	ASSERT_EQ(detail::Phase1StateImageBuildStatus::Created,
		detail::build_phase1_state_image(valid_input(), image));
	ASSERT_EQ(4U, image.records().size());
	const std::array<std::uint16_t, 4> expected_order{{
		static_cast<std::uint16_t>(RecordType::SessionState),
		static_cast<std::uint16_t>(RecordType::MissionState),
		static_cast<std::uint16_t>(RecordType::EntityLifecycle),
		static_cast<std::uint16_t>(RecordType::FlightState),
	}};
	for (std::size_t index = 0U; index < expected_order.size(); ++index) {
		EXPECT_EQ(expected_order[index], image.records()[index].key.record_type);
	}

	const auto* session = find_record(image, RecordType::SessionState);
	const auto* mission = find_record(image, RecordType::MissionState);
	const auto* lifecycle = find_record(image, RecordType::EntityLifecycle);
	const auto* flight = find_record(image, RecordType::FlightState);
	ASSERT_NE(nullptr, session);
	ASSERT_NE(nullptr, mission);
	ASSERT_NE(nullptr, lifecycle);
	ASSERT_NE(nullptr, flight);
	expect_session_record(*session, true);
	expect_mission_record(*mission);
	expect_lifecycle_record(*lifecycle);
	expect_flight_record(*flight);
	EXPECT_EQ(protocol::StateRecordLifecycle::ExplicitCreateDelete, lifecycle->lifecycle);
	EXPECT_TRUE(flight->has_cascade_owner);
	EXPECT_EQ(lifecycle->key, flight->cascade_owner);
	expect_fstl11_valid(image);
}

TEST(TelemetryPhase1StateImageContract, AbsentAndInvalidPlayerNeverPublishPlayerRecords)
{
	for (const auto capture : {
			detail::CaptureResult{detail::CaptureStatus::NoPlayer, detail::CaptureReason::MissingPlayer},
			detail::CaptureResult{detail::CaptureStatus::InvalidSource, detail::CaptureReason::InvalidRadius},
		}) {
		auto input = valid_input();
		input.player_capture = capture;
		protocol::StateImage image;
		ASSERT_EQ(detail::Phase1StateImageBuildStatus::Created,
			detail::build_phase1_state_image(input, image));
		ASSERT_EQ(2U, image.records().size());
		EXPECT_EQ(static_cast<std::uint16_t>(RecordType::SessionState), image.records()[0].key.record_type);
		EXPECT_EQ(static_cast<std::uint16_t>(RecordType::MissionState), image.records()[1].key.record_type);
		EXPECT_EQ(nullptr, find_record(image, RecordType::EntityLifecycle));
		EXPECT_EQ(nullptr, find_record(image, RecordType::FlightState));
		expect_session_record(image.records()[0], false);
		expect_mission_record(image.records()[1]);
		expect_fstl11_valid(image);
	}
}

TEST(TelemetryPhase1StateImageContract, ImageContainsNoManifestIdentityCoreShipOrLaterPhaseRecord)
{
	protocol::StateImage image;
	ASSERT_EQ(detail::Phase1StateImageBuildStatus::Created,
		detail::build_phase1_state_image(valid_input(), image));
	for (const auto& record : image.records()) {
		EXPECT_TRUE(record.key.record_type == static_cast<std::uint16_t>(RecordType::SessionState) ||
			record.key.record_type == static_cast<std::uint16_t>(RecordType::MissionState) ||
			record.key.record_type == static_cast<std::uint16_t>(RecordType::EntityLifecycle) ||
			record.key.record_type == static_cast<std::uint16_t>(RecordType::FlightState));
	}
	EXPECT_EQ(nullptr, find_record(image, RecordType::ClassManifest));
	EXPECT_EQ(nullptr, find_record(image, RecordType::ShipIdentity));
	EXPECT_EQ(nullptr, find_record(image, RecordType::DamageState));
	EXPECT_EQ(nullptr, find_record(image, RecordType::ShieldState));
	EXPECT_EQ(nullptr, find_record(image, RecordType::SubsystemState));
	EXPECT_EQ(nullptr, find_record(image, RecordType::EnergyState));
	EXPECT_EQ(nullptr, find_record(image, RecordType::PropulsionState));
}

TEST(TelemetryPhase1StateImageContract, TimeCompressionOutsideClosedRangeIsTransactionalInvalidInput)
{
	for (const auto compression : {-0.01F, 64.01F}) {
		auto input = valid_input();
		input.mission.time_compression = compression;
		expect_invalid_input_transactional(input);
	}
}

TEST(TelemetryPhase1StateImageContract, UnknownMissionPhaseIsTransactionalInvalidInput)
{
	auto input = valid_input();
	input.mission.phase = static_cast<protocol::MissionPhase>(0xffU);
	expect_invalid_input_transactional(input);
}

TEST(TelemetryPhase1StateImageContract, PlayerAndMissionFromDifferentEngineUpdatesAreTransactionalInvalidInput)
{
	auto input = valid_input();
	input.player.value.producer_sample_time_us = SampleTime + 1U;
	expect_invalid_input_transactional(input);
}

} // namespace
