#include "telemetry/cockpit_sensors_state_image.h"

#include "gtest/gtest.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

using telemetry::protocol::RecordType;
using telemetry::protocol::StateAtom;

std::uint64_t read_u64(
	const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
	std::uint64_t value = 0U;
	for (std::size_t index = 0U; index < 8U; ++index)
		value |= static_cast<std::uint64_t>(bytes[offset + index])
			<< (index * 8U);
	return value;
}
const StateAtom* find(
	const telemetry::protocol::StateImage& image, RecordType type)
{
	for (const auto& atom : image.records())
		if (atom.key.record_type == static_cast<std::uint16_t>(type))
			return &atom;
	return nullptr;
}

void set_sample_times(telemetry::Phase3Projection& projection)
{
	projection.target.producer_sample_time_us = 64U;
	projection.radar.producer_sample_time_us = 64U;
	projection.threat.producer_sample_time_us = 64U;
	projection.hud_alert.producer_sample_time_us = 64U;
	projection.cargo.producer_sample_time_us = 64U;
	projection.navigation.producer_sample_time_us = 64U;
}

struct DirectCockpitFixture {
	static constexpr std::uint64_t Player = 9001U;
	std::unique_ptr<telemetry::detail::Phase2ObservationDto> observation =
		std::make_unique<telemetry::detail::Phase2ObservationDto>();
	std::unique_ptr<telemetry::Phase2ManifestCandidate> manifest =
		std::make_unique<telemetry::Phase2ManifestCandidate>();
	std::array<telemetry::Phase2Wp05SubjectBinding, 1U> subjects{};
	telemetry::detail::Phase2Wp07CleanupRing cleanup;
	telemetry::detail::Phase2Wp07SupportTerminalRing support;
	telemetry::detail::Phase2Wp07EpisodeLatches latches;
	telemetry::CockpitSensorsStateImageInput input{};
	std::unique_ptr<telemetry::Phase3Projection> projection =
		std::make_unique<telemetry::Phase3Projection>();

	DirectCockpitFixture()
	{
		observation->capture = {
			telemetry::detail::Phase2CaptureStatus::Valid,
			telemetry::detail::Phase2CaptureReason::None};
		observation->producer_sample_time_us = 1'000'000U;
		observation->player_key.value = 11U;
		observation->ships.resize(1U);
		auto& ship = observation->ships[0];
		ship.capture_key.value = 11U;
		ship.identity.class_source_key.value = 101U;
		ship.lifecycle.sample_time_us =
			observation->producer_sample_time_us;
		ship.docking.sample_time_us =
			observation->producer_sample_time_us;
		ship.support.sample_time_us =
			observation->producer_sample_time_us;
		manifest->manifest_id = 7U;
		manifest->class_record_count = 1U;
		manifest->class_records[0].source_key = 101U;
		manifest->class_records[0].class_id = 501U;
		subjects[0] = {{11U}, Player};
		input.producer_id = 7U;
		input.session_phase = telemetry::protocol::SessionPhase::Live;
		input.mission.mission_generation = 1U;
		input.mission.phase = telemetry::protocol::MissionPhase::Active;
		input.observation = observation.get();
		input.installed_manifest = manifest.get();
		input.subjects = subjects.data();
		input.subject_count = subjects.size();
		input.player_entity_id = Player;
		input.cleanup_ring = &cleanup;
		input.support_terminal_ring = &support;
		input.episode_latches = &latches;
		projection->player_entity_id = Player;
		set_sample_times(*projection);
	}
};

TEST(TelemetryPhase3StateImage, DirectCockpitBuilderKeepsNoPlayerImageCanonical)
{
	auto observation =
		std::make_unique<telemetry::detail::Phase2ObservationDto>();
	observation->capture.status =
		telemetry::detail::Phase2CaptureStatus::NoPlayer;
	observation->capture.reason =
		telemetry::detail::Phase2CaptureReason::None;
	auto manifest = std::make_unique<telemetry::Phase2ManifestCandidate>();
	manifest->manifest_id = 1U;
	telemetry::CockpitSensorsStateImageInput input{};
	input.producer_id = 7U;
	input.session_phase = telemetry::protocol::SessionPhase::Live;
	input.mission.mission_generation = 1U;
	input.mission.phase = telemetry::protocol::MissionPhase::Active;
	input.observation = observation.get();
	input.installed_manifest = manifest.get();

	auto projection = std::make_unique<telemetry::Phase3Projection>();
	auto pool = std::make_unique<telemetry::CockpitSensorsStateImagePool>();
	ASSERT_TRUE(pool->provision(1U, 1U, 1U, 1U));
	telemetry::protocol::StateImage image;
	telemetry::CockpitSensorsStateImageRebuildSet rebuilt{};
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_cockpit_sensors_state_image_preallocated(
			input, *pool, *projection, image, nullptr, &rebuilt));
	ASSERT_EQ(2U, image.records().size());
	EXPECT_EQ(static_cast<std::uint16_t>(RecordType::SessionState),
		image.records()[0].key.record_type);
	EXPECT_EQ(static_cast<std::uint16_t>(RecordType::MissionState),
		image.records()[1].key.record_type);
	EXPECT_EQ(telemetry::Phase3CockpitSensorsCoverage,
		read_u64(image.records()[0].value, 40U));
	EXPECT_TRUE(rebuilt.exhaustive);
	EXPECT_EQ(2U, rebuilt.count);
}

TEST(TelemetryPhase3StateImage,
	DirectCockpitPatchKeepsBackingAndCanRollback)
{
	auto observation =
		std::make_unique<telemetry::detail::Phase2ObservationDto>();
	observation->capture.status =
		telemetry::detail::Phase2CaptureStatus::NoPlayer;
	auto manifest = std::make_unique<telemetry::Phase2ManifestCandidate>();
	manifest->manifest_id = 1U;
	telemetry::CockpitSensorsStateImageInput input{};
	input.producer_id = 7U;
	input.session_phase = telemetry::protocol::SessionPhase::Live;
	input.mission.mission_generation = 1U;
	input.mission.phase = telemetry::protocol::MissionPhase::Active;
	input.mission.producer_sample_time_us = 64U;
	input.observation = observation.get();
	input.installed_manifest = manifest.get();
	auto projection = std::make_unique<telemetry::Phase3Projection>();
	auto pool = std::make_unique<telemetry::CockpitSensorsStateImagePool>();
	ASSERT_TRUE(pool->provision(1U, 1U, 1U, 1U));

	telemetry::protocol::StateImage current;
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_cockpit_sensors_state_image_preallocated(
			input, *pool, *projection, current));
	const auto previous_records = current.records();
	const auto* const backing = current.records().data();
	input.mission.producer_sample_time_us = 128U;
	telemetry::protocol::StateImage candidate;
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_cockpit_sensors_state_image_preallocated(
			input, *pool, *projection, candidate));
	const auto candidate_records = candidate.records();

	telemetry::CockpitSensorsStateImageRebuildSet rebuilt{};
	ASSERT_TRUE(telemetry::patch_cockpit_sensors_state_image_preallocated(
		current, candidate, rebuilt));
	EXPECT_TRUE(rebuilt.patch_applied);
	EXPECT_FALSE(rebuilt.exhaustive);
	EXPECT_GT(rebuilt.count, 0U);
	EXPECT_EQ(backing, current.records().data());
	EXPECT_EQ(candidate_records, current.records());

	ASSERT_TRUE(telemetry::rollback_cockpit_sensors_state_image_preallocated(
		current, candidate, rebuilt));
	EXPECT_EQ(backing, current.records().data());
	EXPECT_EQ(previous_records, current.records());
}

TEST(TelemetryPhase3StateImage,
	DirectCockpitBuilderPublishesActiveCanonicalSensorImage)
{
	auto fixture = std::make_unique<DirectCockpitFixture>();
	fixture->projection->cargo.producer_sample_time_us = 777U;
	auto pool = std::make_unique<telemetry::CockpitSensorsStateImagePool>();
	ASSERT_TRUE(pool->provision(1U, 0U, 1U, 1U));
	telemetry::protocol::StateImage image;
	telemetry::CockpitSensorsStateImageRebuildSet rebuilt{};
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_cockpit_sensors_state_image_preallocated(
			fixture->input, *pool, *fixture->projection,
			image, nullptr, &rebuilt));
	ASSERT_EQ(20U, image.records().size());
	for (std::size_t index = 1U; index < image.records().size(); ++index)
		EXPECT_TRUE(image.records()[index - 1U].key <
			image.records()[index].key);
	const auto* session = find(image, RecordType::SessionState);
	const auto* cargo = find(image, RecordType::CargoScanState);
	ASSERT_NE(nullptr, session);
	ASSERT_NE(nullptr, cargo);
	EXPECT_EQ(telemetry::Phase3CockpitSensorsCoverage,
		read_u64(session->value, 40U));
	EXPECT_EQ(777U, read_u64(cargo->value, 16U));
	for (const auto type : {RecordType::LockState,
			 RecordType::TargetState, RecordType::RadarState,
			 RecordType::ThreatState, RecordType::HudAlertState,
			 RecordType::NavigationState})
		EXPECT_NE(nullptr, find(image, type));
	EXPECT_TRUE(rebuilt.exhaustive);
	EXPECT_EQ(image.records().size(), rebuilt.count);
}

TEST(TelemetryPhase3StateImage,
	DirectCockpitPoolReusesBackingAcrossPlayerAppearance)
{
	auto pool = std::make_unique<telemetry::CockpitSensorsStateImagePool>();
	ASSERT_TRUE(pool->provision(1U, 0U, 1U, 1U));
	auto no_player =
		std::make_unique<telemetry::detail::Phase2ObservationDto>();
	no_player->capture.status =
		telemetry::detail::Phase2CaptureStatus::NoPlayer;
	auto manifest = std::make_unique<telemetry::Phase2ManifestCandidate>();
	manifest->manifest_id = 1U;
	telemetry::CockpitSensorsStateImageInput absent{};
	absent.observation = no_player.get();
	absent.installed_manifest = manifest.get();
	absent.mission.mission_generation = 1U;
	auto absent_projection =
		std::make_unique<telemetry::Phase3Projection>();
	telemetry::protocol::StateImage image;
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_cockpit_sensors_state_image_preallocated(
			absent, *pool, *absent_projection, image));
	const auto* backing = image.records().data();
	image = {};
	auto active = std::make_unique<DirectCockpitFixture>();
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_cockpit_sensors_state_image_preallocated(
			active->input, *pool, *active->projection, image));
	EXPECT_EQ(backing, image.records().data());
	EXPECT_EQ(telemetry::Phase3CockpitSensorsCoverage,
		read_u64(find(image, RecordType::SessionState)->value, 40U));
}

TEST(TelemetryPhase3StateImage,
	DirectCockpitActivePatchAndRollbackPreserveBacking)
{
	auto fixture = std::make_unique<DirectCockpitFixture>();
	auto pool = std::make_unique<telemetry::CockpitSensorsStateImagePool>();
	ASSERT_TRUE(pool->provision(1U, 0U, 1U, 1U));
	telemetry::protocol::StateImage current;
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_cockpit_sensors_state_image_preallocated(
			fixture->input, *pool, *fixture->projection, current));
	const auto original = current.records();
	const auto* backing = current.records().data();
	fixture->projection->radar.producer_sample_time_us = 2'000U;
	fixture->projection->radar.selected_range = 2'000.0F;
	telemetry::protocol::StateImage candidate;
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_cockpit_sensors_state_image_preallocated(
			fixture->input, *pool, *fixture->projection, candidate));
	telemetry::CockpitSensorsStateImageRebuildSet rebuilt{};
	ASSERT_TRUE(telemetry::patch_cockpit_sensors_state_image_preallocated(
		current, candidate, rebuilt));
	EXPECT_EQ(backing, current.records().data());
	EXPECT_GT(rebuilt.count, 0U);
	ASSERT_TRUE(telemetry::rollback_cockpit_sensors_state_image_preallocated(
		current, candidate, rebuilt));
	EXPECT_EQ(original, current.records());
}

TEST(TelemetryPhase3StateImage,
	DirectCockpitTopologyChangeFallsBackWithoutMutatingCurrent)
{
	auto fixture = std::make_unique<DirectCockpitFixture>();
	auto pool = std::make_unique<telemetry::CockpitSensorsStateImagePool>();
	ASSERT_TRUE(pool->provision(1U, 0U, 1U, 1U));
	telemetry::protocol::StateImage current;
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_cockpit_sensors_state_image_preallocated(
			fixture->input, *pool, *fixture->projection, current));
	const auto original = current.records();
	fixture->projection->contact_count = 1U;
	auto& contact = fixture->projection->contacts[0];
	contact.producer_sample_time_us = 64U;
	contact.entity_id = 100U;
	contact.object_type = telemetry::protocol::ObjectType::Ship;
	contact.visibility = telemetry::protocol::RadarVisibility::Visible;
	telemetry::protocol::StateImage candidate;
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_cockpit_sensors_state_image_preallocated(
			fixture->input, *pool, *fixture->projection, candidate));
	telemetry::CockpitSensorsStateImageRebuildSet rebuilt{};
	EXPECT_FALSE(telemetry::patch_cockpit_sensors_state_image_preallocated(
		current, candidate, rebuilt));
	EXPECT_EQ(original, current.records());
}

TEST(TelemetryPhase3StateImage,
	DirectCockpitMaximumContactsAndPlusOneFailClosed)
{
	auto fixture = std::make_unique<DirectCockpitFixture>();
	fixture->projection->contact_count = telemetry::MaximumPhase3Contacts;
	for (std::size_t index = 0U;
		 index < fixture->projection->contact_count; ++index) {
		auto& contact = fixture->projection->contacts[index];
		contact.producer_sample_time_us = 64U;
		contact.entity_id = 100U + index;
		contact.object_type = telemetry::protocol::ObjectType::Ship;
		contact.visibility = telemetry::protocol::RadarVisibility::Visible;
	}
	auto pool = std::make_unique<telemetry::CockpitSensorsStateImagePool>();
	ASSERT_TRUE(pool->provision(1U, 0U, 1U, 1U));
	telemetry::protocol::StateImage image;
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_cockpit_sensors_state_image_preallocated(
			fixture->input, *pool, *fixture->projection, image));
	const auto published = image;
	fixture->projection->contact_count = telemetry::MaximumPhase3Contacts + 1U;
	EXPECT_EQ(telemetry::Phase3StateImageBuildStatus::InvalidInput,
		telemetry::build_cockpit_sensors_state_image_preallocated(
			fixture->input, *pool, *fixture->projection, image));
	EXPECT_EQ(published, image);
}

TEST(TelemetryPhase3StateImage,
	DirectCockpitPoolExhaustionIsBoundedAndReleasedSlotIsReusable)
{
	auto fixture = std::make_unique<DirectCockpitFixture>();
	auto pool = std::make_unique<telemetry::CockpitSensorsStateImagePool>();
	ASSERT_TRUE(pool->provision(1U, 0U, 1U, 1U));
	std::array<telemetry::protocol::StateImage,
		telemetry::CockpitSensorsStateImagePool::SlotCount> retained{};
	for (auto& image : retained)
		ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
			telemetry::build_cockpit_sensors_state_image_preallocated(
				fixture->input, *pool, *fixture->projection, image));
	telemetry::protocol::StateImage rejected;
	EXPECT_EQ(telemetry::Phase3StateImageBuildStatus::AllocationFailed,
		telemetry::build_cockpit_sensors_state_image_preallocated(
			fixture->input, *pool, *fixture->projection, rejected));
	retained[1] = {};
	EXPECT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_cockpit_sensors_state_image_preallocated(
			fixture->input, *pool, *fixture->projection, rejected));
}

TEST(TelemetryPhase3Dto, OwnsCapturedData)
{
	static_assert(std::is_trivially_copyable_v<telemetry::Phase3Projection>);
	static_assert(std::is_standard_layout_v<telemetry::Phase3Projection>);
	static_assert(std::is_same_v<
		decltype(telemetry::Phase3Projection::contacts),
		std::array<telemetry::Phase3RadarContact,
			telemetry::MaximumPhase3Contacts>>);
	static_assert(std::is_same_v<
		decltype(telemetry::Phase3Projection::locks),
		std::array<telemetry::Phase3LockItem,
			telemetry::MaximumPhase3Locks>>);

	auto source = std::make_unique<telemetry::Phase3Projection>();
	source->lock_count = 1U;
	source->locks[0].target_entity_id = 100U;
	source->contact_count = 1U;
	source->contacts[0].entity_id = 100U;
	ASSERT_TRUE(source->contacts[0].revealed_name.assign("Orion", 5U));
	source->navigation.navpoint_count = 1U;
	source->navigation.navpoints[0].navpoint_id = 7U;
	ASSERT_TRUE(source->navigation.navpoints[0].name.assign("Alpha", 5U));
	source->threat.incoming_missile_count = 1U;
	source->threat.incoming_missiles[0].entity_id = 200U;
	ASSERT_TRUE(source->cargo.cargo_text.assign("Medical supplies", 16U));

	auto captured =
		std::make_unique<telemetry::Phase3Projection>(*source);
	source->locks[0].target_entity_id = 0U;
	source->contacts[0].entity_id = 0U;
	ASSERT_TRUE(source->contacts[0].revealed_name.assign("Changed", 7U));
	source->navigation.navpoints[0].navpoint_id = 0U;
	ASSERT_TRUE(source->navigation.navpoints[0].name.assign("Changed", 7U));
	source->threat.incoming_missiles[0].entity_id = 0U;
	ASSERT_TRUE(source->cargo.cargo_text.assign("Changed", 7U));

	EXPECT_EQ(100U, captured->locks[0].target_entity_id);
	EXPECT_EQ(100U, captured->contacts[0].entity_id);
	EXPECT_EQ("Orion", std::string_view(
		captured->contacts[0].revealed_name.bytes.data(),
		captured->contacts[0].revealed_name.size));
	EXPECT_EQ(7U, captured->navigation.navpoints[0].navpoint_id);
	EXPECT_EQ("Alpha", std::string_view(
		captured->navigation.navpoints[0].name.bytes.data(),
		captured->navigation.navpoints[0].name.size));
	EXPECT_EQ(200U,
		captured->threat.incoming_missiles[0].entity_id);
	EXPECT_EQ("Medical supplies", std::string_view(
		captured->cargo.cargo_text.bytes.data(),
		captured->cargo.cargo_text.size));
}

} // namespace
