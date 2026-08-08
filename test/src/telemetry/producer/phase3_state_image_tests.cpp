#include "telemetry/phase3_state_image.h"

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

std::vector<std::uint8_t> little_u64(std::uint64_t value)
{
	std::vector<std::uint8_t> bytes(8U);
	for (std::size_t index = 0U; index < bytes.size(); ++index)
		bytes[index] =
			static_cast<std::uint8_t>(value >> (index * 8U));
	return bytes;
}

std::uint64_t read_u64(
	const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
	std::uint64_t value = 0U;
	for (std::size_t index = 0U; index < 8U; ++index)
		value |= static_cast<std::uint64_t>(bytes[offset + index])
			<< (index * 8U);
	return value;
}

std::uint32_t read_u32(
	const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
	std::uint32_t value = 0U;
	for (std::size_t index = 0U; index < 4U; ++index)
		value |= static_cast<std::uint32_t>(bytes[offset + index])
			<< (index * 8U);
	return value;
}

float read_f32(
	const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
	const auto bits = read_u32(bytes, offset);
	float value = 0.0F;
	std::memcpy(&value, &bits, sizeof(value));
	return value;
}

std::uint16_t read_u16(
	const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
	return static_cast<std::uint16_t>(bytes[offset]) |
		(static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}

StateAtom global_atom(RecordType type, std::size_t size)
{
	StateAtom atom;
	atom.key.record_type = static_cast<std::uint16_t>(type);
	atom.value.resize(size);
	return atom;
}

StateAtom entity_atom(RecordType type, std::uint64_t entity, std::size_t size)
{
	StateAtom atom;
	atom.key.record_type = static_cast<std::uint16_t>(type);
	atom.key.identity = little_u64(entity);
	atom.value.resize(size);
	for (std::size_t index = 0U; index < 8U; ++index)
		atom.value[index] = atom.key.identity[index];
	if (type != RecordType::EntityLifecycle) {
		atom.has_cascade_owner = true;
		atom.cascade_owner.record_type =
			static_cast<std::uint16_t>(RecordType::EntityLifecycle);
		atom.cascade_owner.identity = atom.key.identity;
	} else {
		atom.lifecycle =
			telemetry::protocol::StateRecordLifecycle::ExplicitCreateDelete;
	}
	return atom;
}

telemetry::protocol::StateImage make_base(std::uint64_t player)
{
	std::vector<StateAtom> atoms;
	auto session = global_atom(RecordType::SessionState,
		player == 0U ? 64U : 72U);
	session.value[0] = player == 0U ? 0U : 1U;
	session.value[8] = 7U;
	session.value[16] = 64U;
	session.value[24] =
		static_cast<std::uint8_t>(telemetry::protocol::AuthorityMode::Solo);
	session.value[25] =
		static_cast<std::uint8_t>(telemetry::protocol::VisibilityMode::Cockpit);
	session.value[26] =
		static_cast<std::uint8_t>(telemetry::protocol::SessionPhase::Live);
	session.value[28] = 1U;
	const auto complete_ship = std::uint64_t{0x0583U};
	for (std::size_t index = 0U; index < 8U; ++index)
		session.value[40U + index] =
			static_cast<std::uint8_t>(complete_ship >> (index * 8U));
	if (player != 0U) {
		const auto identity = little_u64(player);
		for (std::size_t index = 0U; index < identity.size(); ++index)
			session.value[64U + index] = identity[index];
	}
	atoms.push_back(std::move(session));
	auto mission = global_atom(RecordType::MissionState, 28U);
	mission.value[8] = 1U;
	atoms.push_back(std::move(mission));
	if (player != 0U) {
		atoms.push_back(entity_atom(RecordType::EntityLifecycle, player, 40U));
		atoms.push_back(entity_atom(RecordType::CargoScanState, player, 26U));
	}
	telemetry::protocol::StateImage image;
	EXPECT_EQ(telemetry::protocol::StateImageResult::Created,
		telemetry::protocol::StateImage::create(std::move(atoms), image));
	return image;
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
	projection.cargo.producer_sample_time_us = 64U;
	projection.navigation.producer_sample_time_us = 64U;
}

TEST(TelemetryPhase3StateImage, FrozenCoverageAndEmptySensorMatrix)
{
	constexpr std::uint64_t Player = 42U;
	auto base = make_base(Player);
	auto projection = std::make_unique<telemetry::Phase3Projection>();
	projection->player_entity_id = Player;
	set_sample_times(*projection);

	telemetry::protocol::StateImage image;
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_phase3_cockpit_sensor_state_image(
			base, *projection, image));
	EXPECT_EQ(base.records().size() + 5U, image.records().size());
	const auto* session = find(image, RecordType::SessionState);
	ASSERT_NE(nullptr, session);
	EXPECT_EQ(0x07cbU, read_u64(session->value, 40U));
	EXPECT_EQ(0x001dU, read_u64(session->value, 48U));
	EXPECT_EQ(0U, read_u64(session->value, 56U));
	EXPECT_NE(nullptr, find(image, RecordType::LockState));
	EXPECT_NE(nullptr, find(image, RecordType::TargetState));
	EXPECT_NE(nullptr, find(image, RecordType::RadarState));
	EXPECT_NE(nullptr, find(image, RecordType::ThreatState));
	EXPECT_NE(nullptr, find(image, RecordType::CargoScanState));
	EXPECT_NE(nullptr, find(image, RecordType::NavigationState));
}

TEST(TelemetryPhase3StateImage, ContactsUseObserverAndContactIdentity)
{
	constexpr std::uint64_t Player = 42U;
	auto base = make_base(Player);
	auto projection = std::make_unique<telemetry::Phase3Projection>();
	projection->player_entity_id = Player;
	set_sample_times(*projection);
	projection->contact_count = 2U;
	projection->contacts[0].entity_id = 100U;
	projection->contacts[1].entity_id = 101U;
	for (std::size_t index = 0U; index < projection->contact_count; ++index) {
		projection->contacts[index].producer_sample_time_us = 64U;
		projection->contacts[index].object_type =
			telemetry::protocol::ObjectType::Ship;
	}

	telemetry::protocol::StateImage image;
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_phase3_cockpit_sensor_state_image(
			base, *projection, image));
	std::size_t contact_count = 0U;
	for (const auto& atom : image.records()) {
		if (atom.key.record_type !=
			static_cast<std::uint16_t>(RecordType::RadarContacts))
			continue;
		++contact_count;
		ASSERT_EQ(16U, atom.key.identity.size());
		EXPECT_EQ(Player, read_u64(atom.key.identity, 0U));
	}
	EXPECT_EQ(2U, contact_count);
}

TEST(TelemetryPhase3StateImage, LockStateCarriesTheExactAuthorizedLockItem)
{
	constexpr std::uint64_t Player = 42U;
	constexpr std::uint64_t Target = 100U;
	auto base = make_base(Player);
	auto projection = std::make_unique<telemetry::Phase3Projection>();
	projection->player_entity_id = Player;
	set_sample_times(*projection);
	projection->lock_count = 1U;
	auto& lock = projection->locks[0];
	lock.presence = telemetry::protocol::LockItemPresenceFlagSubsystem |
		telemetry::protocol::LockItemPresenceFlagLockAttempt;
	lock.locked = true;
	lock.target_in_lock_cone = true;
	lock.target_entity_id = Target;
	lock.subsystem_id = 7U;
	lock.world_position = {{1.0F, 2.0F, 3.0F}};
	lock.time_to_lock_remaining_us = 9U;

	telemetry::protocol::StateImage image;
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_phase3_cockpit_sensor_state_image(
			base, *projection, image));
	const auto* encoded = find(image, RecordType::LockState);
	ASSERT_NE(nullptr, encoded);
	ASSERT_GE(encoded->value.size(), 65U);
	EXPECT_EQ(Player, read_u64(encoded->value, 0U));
	EXPECT_EQ(1U, encoded->value[24U]);
	EXPECT_EQ(1U, encoded->value[26U]);
	EXPECT_EQ(1U, encoded->value[31U]);
	EXPECT_EQ(1U, encoded->value[32U]);
	EXPECT_EQ(Target, read_u64(encoded->value, 33U));
	EXPECT_EQ(7U, encoded->value[41U]);
	EXPECT_EQ(9U, read_u64(encoded->value, 57U));

	projection->lock_count = 0U;
	telemetry::protocol::StateImage no_attempt;
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_phase3_cockpit_sensor_state_image(
			base, *projection, no_attempt));
	const auto* empty = find(no_attempt, RecordType::LockState);
	ASSERT_NE(nullptr, empty);
	ASSERT_EQ(26U, empty->value.size());
	EXPECT_EQ(0U, empty->value[24U]);
}

TEST(TelemetryPhase3StateImage,
	RadarTrackDoesNotMaterializeObservedEntityLifecycle)
{
	constexpr std::uint64_t Player = 42U;
	constexpr std::uint64_t Contact = 100U;
	auto base = make_base(Player);
	auto projection = std::make_unique<telemetry::Phase3Projection>();
	projection->player_entity_id = Player;
	set_sample_times(*projection);
	projection->contact_count = 1U;
	projection->contacts[0].producer_sample_time_us = 64U;
	projection->contacts[0].entity_id = Contact;
	projection->contacts[0].object_type =
		telemetry::protocol::ObjectType::Ship;

	telemetry::protocol::StateImage image;
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_phase3_cockpit_sensor_state_image(
			base, *projection, image));
	const auto* contact = find(image, RecordType::RadarContacts);
	ASSERT_NE(nullptr, contact);
	EXPECT_EQ(telemetry::protocol::StateRecordLifecycle::ExplicitCreateDelete,
		contact->lifecycle);
	ASSERT_TRUE(contact->has_cascade_owner);
	EXPECT_EQ(static_cast<std::uint16_t>(RecordType::EntityLifecycle),
		contact->cascade_owner.record_type);
	ASSERT_EQ(8U, contact->cascade_owner.identity.size());
	EXPECT_EQ(Player, read_u64(contact->cascade_owner.identity, 0U));
	for (const auto& atom : image.records()) {
		if (atom.key.record_type !=
			static_cast<std::uint16_t>(RecordType::EntityLifecycle))
			continue;
		ASSERT_EQ(8U, atom.key.identity.size());
		EXPECT_NE(Contact, read_u64(atom.key.identity, 0U));
	}
}

TEST(TelemetryPhase3StateImage,
	DistortedRadarTrackRejectsRevealedIdentityBeforePublication)
{
	constexpr std::uint64_t Player = 42U;
	auto base = make_base(Player);
	auto projection = std::make_unique<telemetry::Phase3Projection>();
	projection->player_entity_id = Player;
	set_sample_times(*projection);
	projection->contact_count = 1U;
	auto& contact = projection->contacts[0];
	contact.producer_sample_time_us = 64U;
	contact.entity_id = 100U;
	contact.object_type = telemetry::protocol::ObjectType::Ship;
	contact.visibility = telemetry::protocol::RadarVisibility::Distorted;
	contact.presence =
		telemetry::protocol::RadarContactsPresenceFlagRevealedName |
		telemetry::protocol::RadarContactsPresenceFlagHudTypeLabel;
	ASSERT_TRUE(contact.revealed_name.assign("Hidden", 6U));
	ASSERT_TRUE(contact.hud_type_label.assign("GTF Hidden", 10U));

	telemetry::protocol::StateImage image;
	EXPECT_EQ(telemetry::Phase3StateImageBuildStatus::EncodingFailed,
		telemetry::build_phase3_cockpit_sensor_state_image(
			base, *projection, image));
}

TEST(TelemetryPhase3StateImage,
	MaximumRadarContactsAreCompleteAndPlusOneIsRejectedBeforePublication)
{
	constexpr std::uint64_t Player = 42U;
	auto base = make_base(Player);
	auto projection = std::make_unique<telemetry::Phase3Projection>();
	projection->player_entity_id = Player;
	set_sample_times(*projection);
	projection->contact_count = telemetry::MaximumPhase3Contacts;
	for (std::size_t index = 0U; index < projection->contact_count; ++index) {
		auto& contact = projection->contacts[index];
		contact.producer_sample_time_us = 64U;
		contact.entity_id = 100U + index;
		contact.object_type = telemetry::protocol::ObjectType::Ship;
		contact.visibility = telemetry::protocol::RadarVisibility::Visible;
	}

	telemetry::protocol::StateImage image;
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_phase3_cockpit_sensor_state_image(
			base, *projection, image));
	std::size_t contacts = 0U;
	for (const auto& atom : image.records())
		contacts += atom.key.record_type ==
			static_cast<std::uint16_t>(RecordType::RadarContacts)
			? 1U : 0U;
	EXPECT_EQ(telemetry::MaximumPhase3Contacts, contacts);

	const auto published_count = image.records().size();
	projection->contact_count = telemetry::MaximumPhase3Contacts + 1U;
	EXPECT_EQ(telemetry::Phase3StateImageBuildStatus::InvalidInput,
		telemetry::build_phase3_cockpit_sensor_state_image(
			base, *projection, image));
	EXPECT_EQ(published_count, image.records().size())
		<< "An over-limit source must not replace a complete published image with a partial one.";
}

TEST(TelemetryPhase3StateImage,
	EveryRemainingCardinalityPlusOneIsRejectedBeforePublication)
{
	constexpr std::uint64_t Player = 42U;
	auto base = make_base(Player);
	auto projection = std::make_unique<telemetry::Phase3Projection>();
	projection->player_entity_id = Player;
	set_sample_times(*projection);
	telemetry::protocol::StateImage image;
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_phase3_cockpit_sensor_state_image(
			base, *projection, image));
	const auto published_count = image.records().size();
	const auto expect_rejected = [&] {
		EXPECT_EQ(telemetry::Phase3StateImageBuildStatus::InvalidInput,
			telemetry::build_phase3_cockpit_sensor_state_image(
				base, *projection, image));
		EXPECT_EQ(published_count, image.records().size());
	};

	projection->lock_count = telemetry::MaximumPhase3Locks + 1U;
	expect_rejected();
	projection->lock_count = 0U;
	projection->threat.incoming_missile_count =
		telemetry::MaximumPhase3IncomingMissiles + 1U;
	expect_rejected();
	projection->threat.incoming_missile_count = 0U;
	projection->navigation.navpoint_count =
		telemetry::MaximumPhase3Navpoints + 1U;
	expect_rejected();
	projection->navigation.navpoint_count = 0U;
	projection->navigation.route_waypoint_count =
		telemetry::MaximumPhase3RouteWaypoints + 1U;
	expect_rejected();
}

TEST(TelemetryPhase3StateImage,
	ThreatPublishesExactlyMaximumIncomingMissilesWithoutTruncation)
{
	constexpr std::uint64_t Player = 42U;
	auto base = make_base(Player);
	auto projection = std::make_unique<telemetry::Phase3Projection>();
	projection->player_entity_id = Player;
	set_sample_times(*projection);
	projection->threat.incoming_missile_count =
		telemetry::MaximumPhase3IncomingMissiles;
	for (std::size_t index = 0U;
		 index < projection->threat.incoming_missile_count; ++index) {
		auto& missile = projection->threat.incoming_missiles[index];
		missile.entity_id = 1'000U + index;
		missile.weapon_class_id = 1U;
		missile.guidance_type = static_cast<std::uint8_t>(
			telemetry::protocol::GuidanceType::Aspect);
		missile.radar_visibility =
			telemetry::protocol::RadarVisibility::Visible;
	}

	telemetry::protocol::StateImage image;
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_phase3_cockpit_sensor_state_image(
			base, *projection, image));
	const auto* threat = find(image, RecordType::ThreatState);
	ASSERT_NE(nullptr, threat);
	ASSERT_GE(threat->value.size(), 27U);
	EXPECT_EQ(telemetry::MaximumPhase3IncomingMissiles,
		static_cast<std::size_t>(read_u16(threat->value, 25U)));

	const auto published_count = image.records().size();
	projection->threat.incoming_missile_count =
		telemetry::MaximumPhase3IncomingMissiles + 1U;
	EXPECT_EQ(telemetry::Phase3StateImageBuildStatus::InvalidInput,
		telemetry::build_phase3_cockpit_sensor_state_image(
			base, *projection, image));
	EXPECT_EQ(published_count, image.records().size());
}

TEST(TelemetryPhase3StateImage,
	RebuildClearsWithdrawnTargetAndRemovesWithdrawnRadarContact)
{
	constexpr std::uint64_t Player = 42U;
	constexpr std::uint64_t Target = 100U;
	auto base = make_base(Player);
	auto projection = std::make_unique<telemetry::Phase3Projection>();
	projection->player_entity_id = Player;
	set_sample_times(*projection);
	projection->target.current_target_entity_id = Target;
	projection->contact_count = 1U;
	projection->contacts[0].producer_sample_time_us = 64U;
	projection->contacts[0].entity_id = Target;
	projection->contacts[0].object_type =
		telemetry::protocol::ObjectType::Ship;

	telemetry::protocol::StateImage observed;
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_phase3_cockpit_sensor_state_image(
			base, *projection, observed));
	ASSERT_NE(nullptr, find(observed, RecordType::RadarContacts));
	const auto* observed_target = find(observed, RecordType::TargetState);
	ASSERT_NE(nullptr, observed_target);
	ASSERT_GE(observed_target->value.size(), 32U);
	EXPECT_EQ(Target, read_u64(observed_target->value, 24U));

	projection->target = {};
	set_sample_times(*projection);
	projection->contact_count = 0U;
	telemetry::protocol::StateImage withdrawn;
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_phase3_cockpit_sensor_state_image(
			base, *projection, withdrawn));
	EXPECT_EQ(nullptr, find(withdrawn, RecordType::RadarContacts));
	const auto* withdrawn_target = find(withdrawn, RecordType::TargetState);
	ASSERT_NE(nullptr, withdrawn_target);
	ASSERT_GE(withdrawn_target->value.size(), 32U);
	EXPECT_EQ(0U, read_u64(withdrawn_target->value, 8U));
	EXPECT_EQ(0U, read_u64(withdrawn_target->value, 24U));
}

TEST(TelemetryPhase3StateImage,
	TargetCarriesCurrentPreviousAndOnlyItsAuthorizedConditionalGroups)
{
	constexpr std::uint64_t Player = 42U;
	auto base = make_base(Player);
	auto projection = std::make_unique<telemetry::Phase3Projection>();
	projection->player_entity_id = Player;
	set_sample_times(*projection);
	auto& target = projection->target;
	target.current_target_entity_id = 100U;
	target.previous_target_entity_id = 99U;
	target.time_on_target_us = 1234U;
	target.last_stealth_position = {{1.0F, 2.0F, 3.0F}};
	target.last_stealth_velocity = {{4.0F, 5.0F, 6.0F}};
	target.presence =
		telemetry::protocol::TargetStatePresenceFlagPreviousTarget |
		telemetry::protocol::TargetStatePresenceFlagTimeOnTarget |
		telemetry::protocol::TargetStatePresenceFlagLastStealthObservation |
		telemetry::protocol::TargetStatePresenceFlagDistanceTrend |
		telemetry::protocol::TargetStatePresenceFlagSpeedTrend |
		telemetry::protocol::TargetStatePresenceFlagInCone;
	target.distance_trend = 2U;
	target.speed_trend = 1U;
	target.in_cone = true;

	telemetry::protocol::StateImage image;
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_phase3_cockpit_sensor_state_image(
			base, *projection, image));
	const auto* encoded = find(image, RecordType::TargetState);
	ASSERT_NE(nullptr, encoded);
	EXPECT_EQ(target.presence, read_u64(encoded->value, 8U));
	EXPECT_EQ(target.producer_sample_time_us, read_u64(encoded->value, 16U));
	EXPECT_EQ(100U, read_u64(encoded->value, 24U));
	EXPECT_EQ(99U, read_u64(encoded->value, 32U));
	EXPECT_EQ(1234U, read_u64(encoded->value, 40U));
	EXPECT_EQ(0x3F800000U, read_u32(encoded->value, 48U));
	EXPECT_EQ(0x40000000U, read_u32(encoded->value, 52U));
	EXPECT_EQ(0x40400000U, read_u32(encoded->value, 56U));
	EXPECT_EQ(0x40800000U, read_u32(encoded->value, 60U));
	EXPECT_EQ(0x40A00000U, read_u32(encoded->value, 64U));
	EXPECT_EQ(0x40C00000U, read_u32(encoded->value, 68U));
	EXPECT_EQ(2U, encoded->value[72U]);
	EXPECT_EQ(1U, encoded->value[73U]);
	EXPECT_EQ(1U, encoded->value[74U]);
}

TEST(TelemetryPhase3StateImage, RejectsLeadWithoutAnAuthoritativeWeaponBank)
{
	constexpr std::uint64_t Player = 42U;
	auto base = make_base(Player);
	auto projection = std::make_unique<telemetry::Phase3Projection>();
	projection->player_entity_id = Player;
	set_sample_times(*projection);
	projection->target.current_target_entity_id = 100U;
	projection->target.presence =
		telemetry::protocol::TargetStatePresenceFlagLead;
	projection->target.lead_world = {{10.0F, 20.0F, 30.0F}};
	projection->target.lead_bank_id = 0U;

	telemetry::protocol::StateImage image;
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::EncodingFailed,
		telemetry::build_phase3_cockpit_sensor_state_image(
			base, *projection, image));
}

TEST(TelemetryPhase3StateImage, RejectsTargetSubsystemWithoutRevealedIdentity)
{
	constexpr std::uint64_t Player = 42U;
	auto base = make_base(Player);
	auto projection = std::make_unique<telemetry::Phase3Projection>();
	projection->player_entity_id = Player;
	set_sample_times(*projection);
	projection->target.current_target_entity_id = 100U;
	projection->target.presence =
		telemetry::protocol::TargetStatePresenceFlagTargetSubsystem;
	projection->target.target_subsystem_id = 7U;

	telemetry::protocol::StateImage image;
	EXPECT_EQ(telemetry::Phase3StateImageBuildStatus::EncodingFailed,
		telemetry::build_phase3_cockpit_sensor_state_image(
			base, *projection, image));
}

TEST(TelemetryPhase3StateImage, CrossReferencesKeepOnePublicIdForOneObject)
{
	constexpr std::uint64_t Player = 42U;
	constexpr std::uint64_t PublicObject = 100U;
	auto base = make_base(Player);
	auto projection = std::make_unique<telemetry::Phase3Projection>();
	projection->player_entity_id = Player;
	set_sample_times(*projection);
	projection->target.current_target_entity_id = PublicObject;
	projection->lock_count = 1U;
	projection->locks[0].target_entity_id = PublicObject;
	projection->contacts[0].producer_sample_time_us = 64U;
	projection->contacts[0].entity_id = PublicObject;
	projection->contacts[0].object_type =
		telemetry::protocol::ObjectType::Ship;
	projection->contact_count = 1U;
	projection->threat.presence =
		telemetry::protocol::ThreatStatePresenceFlagNearestAttacker;
	projection->threat.nearest_attacker_entity_id = PublicObject;
	projection->cargo.presence =
		telemetry::protocol::CargoScanStatePresenceFlagTarget;
	projection->cargo.target_entity_id = PublicObject;

	telemetry::protocol::StateImage image;
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_phase3_cockpit_sensor_state_image(
			base, *projection, image));
	const auto* target = find(image, RecordType::TargetState);
	const auto* locks = find(image, RecordType::LockState);
	const auto* contact = find(image, RecordType::RadarContacts);
	const auto* threat = find(image, RecordType::ThreatState);
	const auto* cargo = find(image, RecordType::CargoScanState);
	ASSERT_NE(nullptr, target);
	ASSERT_NE(nullptr, locks);
	ASSERT_NE(nullptr, contact);
	ASSERT_NE(nullptr, threat);
	ASSERT_NE(nullptr, cargo);
	EXPECT_EQ(PublicObject, read_u64(target->value, 24U));
	EXPECT_EQ(PublicObject, read_u64(locks->value, 33U));
	EXPECT_EQ(PublicObject, read_u64(contact->value, 8U));
	EXPECT_EQ(PublicObject, read_u64(threat->value, 25U));
	EXPECT_EQ(PublicObject, read_u64(cargo->value, 26U));
}

TEST(TelemetryPhase3StateImage,
	PreservesIndependentTargetAndRadarSampleTimes)
{
	constexpr std::uint64_t Player = 42U;
	constexpr std::uint64_t First = 100U;
	constexpr std::uint64_t Second = 200U;
	auto base = make_base(Player);
	auto projection = std::make_unique<telemetry::Phase3Projection>();
	projection->player_entity_id = Player;
	set_sample_times(*projection);
	projection->target.producer_sample_time_us = 200U;
	projection->target.current_target_entity_id = Second;
	projection->contact_count = 2U;
	for (std::size_t index = 0U; index < projection->contact_count; ++index) {
		auto& contact = projection->contacts[index];
		contact.producer_sample_time_us = 100U;
		contact.entity_id = index == 0U ? First : Second;
		contact.object_type = telemetry::protocol::ObjectType::Ship;
		contact.category = static_cast<std::uint8_t>(
			telemetry::protocol::RadarCategory::Ship);
		contact.visibility = telemetry::protocol::RadarVisibility::Visible;
		contact.radar_projection_distance = 1.0F;
	}
	projection->contacts[0].flags =
		telemetry::protocol::ContactFlagCurrentTarget;

	telemetry::protocol::StateImage image;
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_phase3_cockpit_sensor_state_image(
			base, *projection, image));
	const auto* target = find(image, RecordType::TargetState);
	const auto* first_contact = find(image, RecordType::RadarContacts);
	ASSERT_NE(nullptr, target);
	ASSERT_NE(nullptr, first_contact);
	EXPECT_EQ(200U, read_u64(target->value, 16U));
	EXPECT_EQ(100U, read_u64(first_contact->value, 24U));

	// systemsHz may also be newer than flightHz; the inverse lag is legal and
	// converges when the next target sample is captured.
	projection->target.producer_sample_time_us = 100U;
	projection->target.current_target_entity_id = First;
	projection->contacts[0].producer_sample_time_us = 200U;
	projection->contacts[1].producer_sample_time_us = 200U;
	projection->contacts[0].flags = 0U;
	projection->contacts[1].flags =
		telemetry::protocol::ContactFlagCurrentTarget;
	EXPECT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_phase3_cockpit_sensor_state_image(
			base, *projection, image));
}

TEST(TelemetryPhase3StateImage,
	NavigationPublishesItsExactCurrentDestinationRouteAndRefusal)
{
	constexpr std::uint64_t Player = 42U;
	auto base = make_base(Player);
	auto projection = std::make_unique<telemetry::Phase3Projection>();
	projection->player_entity_id = Player;
	set_sample_times(*projection);
	auto& navigation = projection->navigation;
	navigation.presence =
		telemetry::protocol::NavigationStatePresenceFlagCurrentNavpoint |
		telemetry::protocol::NavigationStatePresenceFlagAutopilotRefusal |
		telemetry::protocol::NavigationStatePresenceFlagWaypointRoute;
	navigation.autopilot_state = telemetry::protocol::AutopilotState::Refused;
	navigation.autopilot_refusal =
		telemetry::protocol::AutopilotRefusal::NoValidNav;
	navigation.navpoint_count = 1U;
	auto& navpoint = navigation.navpoints[0];
	navpoint.presence = telemetry::protocol::NavPointPresenceFlagWaypointLink;
	navpoint.type = static_cast<std::uint8_t>(
		telemetry::protocol::NavPointType::Waypoint);
	navpoint.navpoint_id = 77U;
	ASSERT_TRUE(navpoint.name.assign("Alpha", 5U));
	navpoint.waypoint_list_id = 8U;
	navpoint.waypoint_index = 2U;
	navigation.current_navpoint_id = navpoint.navpoint_id;
	navigation.route_waypoint_count = 1U;
	navigation.route_waypoints[0].waypoint_list_id = 8U;
	navigation.route_waypoints[0].waypoint_index = 2U;
	navigation.current_route_index = 0U;
	navigation.route_speed_limit = 50.0F;

	telemetry::protocol::StateImage image;
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_phase3_cockpit_sensor_state_image(
			base, *projection, image));
	const auto* atom = find(image, RecordType::NavigationState);
	ASSERT_NE(nullptr, atom);
	ASSERT_GE(atom->value.size(), 67U);
	EXPECT_EQ(navigation.presence, read_u64(atom->value, 8U));
	EXPECT_EQ(static_cast<std::uint8_t>(navigation.autopilot_state),
		atom->value[24U]);
	EXPECT_EQ(77U, read_u32(atom->value, 34U));
	EXPECT_EQ(77U, read_u32(atom->value, 63U));
}

TEST(TelemetryPhase3StateImage,
	NonFiniteNavigationPositionFailsClosedWithoutReplacingPublishedImage)
{
	constexpr std::uint64_t Player = 42U;
	auto base = make_base(Player);
	auto projection = std::make_unique<telemetry::Phase3Projection>();
	projection->player_entity_id = Player;
	set_sample_times(*projection);
	telemetry::protocol::StateImage image;
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_phase3_cockpit_sensor_state_image(
			base, *projection, image));
	const auto published_count = image.records().size();
	projection->navigation.navpoint_count = 1U;
	projection->navigation.navpoints[0].navpoint_id = 1U;
	ASSERT_TRUE(projection->navigation.navpoints[0].name.assign("Bad", 3U));
	projection->navigation.navpoints[0].position_world[1] =
		std::numeric_limits<float>::quiet_NaN();
	EXPECT_EQ(telemetry::Phase3StateImageBuildStatus::EncodingFailed,
		telemetry::build_phase3_cockpit_sensor_state_image(
			base, *projection, image));
	EXPECT_EQ(published_count, image.records().size());
}

TEST(TelemetryPhase3StateImage,
	RejectsInvalidRadarEnumAndNullContactIdentityBeforePublication)
{
	constexpr std::uint64_t Player = 42U;
	auto base = make_base(Player);
	auto projection = std::make_unique<telemetry::Phase3Projection>();
	projection->player_entity_id = Player;
	set_sample_times(*projection);
	telemetry::protocol::StateImage image;
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_phase3_cockpit_sensor_state_image(
			base, *projection, image));
	const auto published_count = image.records().size();

	projection->radar.mode =
		static_cast<telemetry::protocol::RadarMode>(0xffU);
	EXPECT_EQ(telemetry::Phase3StateImageBuildStatus::EncodingFailed,
		telemetry::build_phase3_cockpit_sensor_state_image(
			base, *projection, image));
	EXPECT_EQ(published_count, image.records().size());

	projection->radar.mode = telemetry::protocol::RadarMode::Short;
	projection->contact_count = 1U;
	projection->contacts[0].producer_sample_time_us = 64U;
	projection->contacts[0].object_type =
		telemetry::protocol::ObjectType::Ship;
	EXPECT_EQ(telemetry::Phase3StateImageBuildStatus::EncodingFailed,
		telemetry::build_phase3_cockpit_sensor_state_image(
			base, *projection, image));
	EXPECT_EQ(published_count, image.records().size());
}

TEST(TelemetryPhase3StateImage, CanonicalizesNegativeZeroBeforePublication)
{
	constexpr std::uint64_t Player = 42U;
	auto base = make_base(Player);
	auto projection = std::make_unique<telemetry::Phase3Projection>();
	projection->player_entity_id = Player;
	set_sample_times(*projection);
	projection->contact_count = 1U;
	auto& contact = projection->contacts[0];
	contact.producer_sample_time_us = 64U;
	contact.entity_id = 100U;
	contact.object_type = telemetry::protocol::ObjectType::Ship;
	contact.radar_local_position = {{10.0F, -20.0F, 30.0F}};
	contact.radar_projection_distance = 40.0F;
	contact.radius = -0.0F;

	telemetry::protocol::StateImage image;
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_phase3_cockpit_sensor_state_image(
			base, *projection, image));
	const auto* encoded = find(image, RecordType::RadarContacts);
	ASSERT_NE(nullptr, encoded);
	EXPECT_EQ(4U, encoded->record_version);
	ASSERT_GE(encoded->value.size(), 79U);
	EXPECT_FLOAT_EQ(10.0F, read_f32(encoded->value, 59U));
	EXPECT_FLOAT_EQ(-20.0F, read_f32(encoded->value, 63U));
	EXPECT_FLOAT_EQ(30.0F, read_f32(encoded->value, 67U));
	EXPECT_FLOAT_EQ(40.0F, read_f32(encoded->value, 71U));
	EXPECT_EQ(0U, read_u32(encoded->value, 75U));
}

TEST(TelemetryPhase3StateImage,
	PublishesVisibleShipHudIdentityAndVisualInExplicitRadarVersionFour)
{
	constexpr std::uint64_t Player = 42U;
	auto base = make_base(Player);
	auto projection = std::make_unique<telemetry::Phase3Projection>();
	projection->player_entity_id = Player;
	set_sample_times(*projection);
	projection->contact_count = 1U;
	auto& contact = projection->contacts[0];
	contact.producer_sample_time_us = 64U;
	contact.entity_id = 100U;
	contact.object_type = telemetry::protocol::ObjectType::Ship;
	contact.category = static_cast<std::uint8_t>(
		telemetry::protocol::RadarCategory::Ship);
	contact.visibility = telemetry::protocol::RadarVisibility::Visible;
	contact.radar_projection_distance = 1.0F;
	contact.presence =
		telemetry::protocol::RadarContactsPresenceFlagRevealedName |
		telemetry::protocol::RadarContactsPresenceFlagHudTypeLabel |
		telemetry::protocol::RadarContactsPresenceFlagRadarVisual;
	ASSERT_TRUE(contact.revealed_name.assign("Alpha 2", 7U));
	ASSERT_TRUE(contact.hud_type_label.assign("GTF Myrmidon", 12U));
	contact.radar_blip_color = {{0x11U, 0x22U, 0x33U, 0x44U}};
	contact.radar_blip_type = static_cast<std::uint8_t>(
		telemetry::protocol::RadarBlipType::NormalShip);

	telemetry::protocol::StateImage image;
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_phase3_cockpit_sensor_state_image(
			base, *projection, image));
	const auto* encoded = find(image, RecordType::RadarContacts);
	ASSERT_NE(nullptr, encoded);
	EXPECT_EQ(4U, encoded->record_version);
	ASSERT_GE(encoded->value.size(), 111U);
	EXPECT_EQ(7U, read_u16(encoded->value, 83U));
	EXPECT_EQ("Alpha 2", std::string(encoded->value.begin() + 85U,
		encoded->value.begin() + 92U));
	EXPECT_EQ(12U, read_u16(encoded->value, 92U));
	EXPECT_EQ("GTF Myrmidon", std::string(encoded->value.begin() + 94U,
		encoded->value.begin() + 106U));
	EXPECT_EQ(0x11U, encoded->value[106U]);
	EXPECT_EQ(0x22U, encoded->value[107U]);
	EXPECT_EQ(0x33U, encoded->value[108U]);
	EXPECT_EQ(0x44U, encoded->value[109U]);
	EXPECT_EQ(static_cast<std::uint8_t>(telemetry::protocol::RadarBlipType::NormalShip),
		encoded->value[110U]);
}

TEST(TelemetryPhase3StateImage,
	PublishesExactHudTargetReadoutAndColorInExplicitVersionFour)
{
	constexpr std::uint64_t Player = 42U;
	auto base = make_base(Player);
	auto projection = std::make_unique<telemetry::Phase3Projection>();
	projection->player_entity_id = Player;
	set_sample_times(*projection);
	telemetry::protocol::StateImage image;
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_phase3_cockpit_sensor_state_image(
			base, *projection, image));
	const auto published_count = image.records().size();
	projection->target.presence =
		telemetry::protocol::TargetStatePresenceFlagExactHudDistance |
		telemetry::protocol::TargetStatePresenceFlagExactHudSpeed |
		telemetry::protocol::TargetStatePresenceFlagHudTargetColor;
	projection->target.current_target_entity_id = 100U;
	projection->target.exact_hud_distance = 500.0F;
	projection->target.exact_hud_speed = 92.0F;
	projection->target.hud_target_color = {{0xaaU, 0xbbU, 0xccU, 0xddU}};
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_phase3_cockpit_sensor_state_image(
			base, *projection, image));
	const auto* target = find(image, RecordType::TargetState);
	ASSERT_NE(nullptr, target);
	EXPECT_EQ(4U, target->record_version);
	EXPECT_EQ(published_count, image.records().size());
	EXPECT_FLOAT_EQ(500.0F, read_f32(target->value, 32U));
	EXPECT_FLOAT_EQ(92.0F, read_f32(target->value, 36U));
	EXPECT_EQ(0xaaU, target->value[40U]);
	EXPECT_EQ(0xbbU, target->value[41U]);
	EXPECT_EQ(0xccU, target->value[42U]);
	EXPECT_EQ(0xddU, target->value[43U]);
}

TEST(TelemetryPhase3StateImage, PlayerAbsentRetainsOnlyGlobalSingletons)
{
	auto base = make_base(0U);
	auto projection = std::make_unique<telemetry::Phase3Projection>();
	telemetry::protocol::StateImage image;
	ASSERT_EQ(telemetry::Phase3StateImageBuildStatus::Created,
		telemetry::build_phase3_cockpit_sensor_state_image(
			base, *projection, image));
	EXPECT_EQ(2U, image.records().size());
	const auto* session = find(image, RecordType::SessionState);
	ASSERT_NE(nullptr, session);
	EXPECT_EQ(0x07cbU, read_u64(session->value, 40U));
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
