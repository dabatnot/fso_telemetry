#include "ai/ai.h"
#include "ai/ai_profiles.h"
#include "camera/photomode.h"
#include "gamesnd/gamesnd.h"
#include "hud/hudtargetbox.h"
#include "mission/missionparse.h"
#include "mod_table/mod_table.h"
#include "network/multi.h"
#include "object/object.h"
#include "playerman/player.h"
#include "ship/ship.h"
#include "ship/support_work.h"
#include "telemetry/phase2_gameplay_ab_test_seam.h"
#include "util/FSTestFixture.h"
#include "freespace.h"

#include <gtest/gtest.h>

#include <cstring>
#include <limits>
#include <tuple>

extern void read_player_controls(object* objp, float frametime);
extern ship_obj* get_ship_obj_ptr_from_index(int index);
extern bool Photo_mode_active;
extern SCP_vector<game_snd> Snds;

namespace {

using namespace telemetry::detail;

// This linkage stays in the unittest object and materializes only test-owned
// Ships/Objects slots in the real engine Ship_obj_list. Production keeps list
// mutation private to ship_create/ship_delete.
constexpr int test_ship_obj_used = 1 << 0;
constexpr int test_max_ship_objs = MAX_SHIPS;

bool register_fixture_ship_in_ship_obj_list(int shipnum, int objnum) noexcept
{
	if (shipnum < 0 || shipnum >= MAX_SHIPS ||
		objnum < 0 || objnum >= MAX_OBJECTS ||
		Objects[objnum].type != OBJ_SHIP ||
		Objects[objnum].instance != shipnum ||
		Ships[shipnum].objnum != objnum ||
		Ships[shipnum].ship_list_index != -1 ||
		Ship_obj_list.next == nullptr || Ship_obj_list.prev == nullptr) {
		return false;
	}

	int free_index = -1;
	for (int index = 0; index < test_max_ship_objs; ++index) {
		auto* slot = get_ship_obj_ptr_from_index(index);
		if ((slot->flags & test_ship_obj_used) != 0) {
			if (slot->objnum == objnum) {
				return false;
			}
		} else if (free_index == -1) {
			free_index = index;
		}
	}
	if (free_index == -1) {
		return false;
	}

	auto* slot = get_ship_obj_ptr_from_index(free_index);
	slot->flags = 0;
	slot->objnum = objnum;
	list_append(&Ship_obj_list, slot);
	slot->flags |= test_ship_obj_used;
	Ships[shipnum].ship_list_index = free_index;
	return true;
}

bool unregister_fixture_ship_from_ship_obj_list(
	int shipnum, int objnum) noexcept
{
	if (shipnum < 0 || shipnum >= MAX_SHIPS ||
		objnum < 0 || objnum >= MAX_OBJECTS ||
		Ships[shipnum].objnum != objnum) {
		return false;
	}
	const auto list_index = Ships[shipnum].ship_list_index;
	if (list_index == -1) {
		return true;
	}
	if (list_index < 0 || list_index >= test_max_ship_objs) {
		return false;
	}
	auto* slot = get_ship_obj_ptr_from_index(list_index);
	if ((slot->flags & test_ship_obj_used) == 0 || slot->objnum != objnum) {
		return false;
	}

	list_remove(Ship_obj_list, slot);
	slot->flags = 0;
	slot->next = nullptr;
	slot->prev = reinterpret_cast<ship_obj*>(-1);
	Ships[shipnum].ship_list_index = -1;
	return true;
}

class ControlHookCounter final : public Phase2SeamTestDouble {
  public:
	void on_ship_cleanup(const ShipCleanupFact&) noexcept override { ++cleanup_calls; }
	void on_support_transition(const SupportTransitionFact& fact) noexcept override
	{
		++support_calls;
		last_support_reason = fact.reason;
		if (fact.reason == SupportTransitionReason::Broken) {
			++support_broken_calls;
		} else if (fact.reason == SupportTransitionReason::End) {
			++support_end_calls;
		}
	}
	void on_control_target(ControlTargetAuthority authority) noexcept override
	{
		++control_calls;
		last_authority = authority;
	}
	void on_cargo_authority(const CargoAuthorityFact& fact) noexcept override
	{
		++cargo_calls;
		if (fact.phase == CargoScanPhaseObservation::Scanning) {
			++cargo_scanning_calls;
		} else if (fact.phase == CargoScanPhaseObservation::Completed) {
			++cargo_completed_calls;
		}
	}

	std::uint64_t cleanup_calls = 0U;
	std::uint64_t support_calls = 0U;
	std::uint64_t support_broken_calls = 0U;
	std::uint64_t support_end_calls = 0U;
	std::uint64_t control_calls = 0U;
	std::uint64_t cargo_calls = 0U;
	std::uint64_t cargo_scanning_calls = 0U;
	std::uint64_t cargo_completed_calls = 0U;
	ControlTargetAuthority last_authority = ControlTargetAuthority::Ship;
	SupportTransitionReason last_support_reason = SupportTransitionReason::Queue;
};

class TelemetryPhase2GameplayAbContract : public test::FSTestFixture {
  public:
	TelemetryPhase2GameplayAbContract()
		: FSTestFixture(test::INIT_CFILE | test::INIT_GRAPHICS | test::INIT_SHIPS)
	{
	}
};

TEST_F(TelemetryPhase2GameplayAbContract,
	RealPlayerControlsGameplayAbHarnessUsesEngineGlobalOffOnOff)
{
	struct Context {
		ControlHookCounter* recorder = nullptr;
		int support_return = 0;
		bool list_registered = false;
		bool list_unregistered = false;
		int cleanup_ship_index = MAX_SHIPS - 1;
		int cleanup_object_index = MAX_OBJECTS - 1;
		int target_ship_index = MAX_SHIPS - 2;
		int target_object_index = MAX_OBJECTS - 2;
		int ship_info_index = -1;
		int registry_index = -1;
		std::size_t exited_ship_count = 0U;
		player test_player;
		std::uint64_t support_flags = 0U;
		std::uint64_t cargo_advance_count = 0U;
		int cargo_progress_ms = 0;
		int cargo_final_ms = -1;
		bool cargo_revealed = false;
		bool target_display_cargo = false;
	};
	ControlHookCounter recorder;
	Context context{&recorder};
	const auto invoke = [](void* opaque) noexcept {
		auto& state = *static_cast<Context*>(opaque);
		read_player_controls(nullptr, 0.0F);
		auto& gameplay_ship = Ships[state.cleanup_ship_index];
		auto& gameplay_object = Objects[state.cleanup_object_index];
		auto& target_ship = Ships[state.target_ship_index];
		auto& target_object = Objects[state.target_object_index];
		gameplay_ship.clear();
		list_init(&gameplay_ship.subsys_list);
		gameplay_ship.weapons.clear();
		gameplay_ship.objnum = state.cleanup_object_index;
		gameplay_ship.ai_index = state.cleanup_ship_index;
		gameplay_ship.ship_info_index = state.ship_info_index;
		gameplay_ship.wingnum = -1;
		strcpy_s(gameplay_ship.ship_name, "Telemetry AB isolated ship");
		gameplay_object.clear();
		gameplay_object.type = OBJ_SHIP;
		gameplay_object.instance = state.cleanup_ship_index;
		gameplay_object.signature = 42001;
		gameplay_object.orient = vmd_identity_matrix;
		gameplay_object.flags.set(Object::Object_Flags::No_shields);
		Ai_info[state.cleanup_ship_index] = ai_info{};
		Ai_info[state.cleanup_ship_index].shipnum = state.cleanup_ship_index;
		target_ship.clear();
		list_init(&target_ship.subsys_list);
		target_ship.weapons.clear();
		target_ship.objnum = state.target_object_index;
		target_ship.ai_index = state.target_ship_index;
		target_ship.ship_info_index = state.ship_info_index;
		target_ship.wingnum = -1;
		target_ship.flags.set(Ship::Ship_Flags::Scannable);
		strcpy_s(target_ship.ship_name, "Telemetry AB cargo target");
		target_object.clear();
		target_object.type = OBJ_SHIP;
		target_object.instance = state.target_ship_index;
		target_object.signature = 42002;
		target_object.radius = 10.0F;
		target_object.pos.xyz.z = 50.0F;
		target_object.orient = vmd_identity_matrix;
		Ai_info[state.target_ship_index] = ai_info{};
		Ai_info[state.target_ship_index].shipnum = state.target_ship_index;
		auto& registry = Ship_registry[state.registry_index];
		registry.status = ShipStatus::PRESENT;
		registry.objnum = state.cleanup_object_index;
		registry.shipnum = state.cleanup_ship_index;
		state.list_registered =
			register_fixture_ship_in_ship_obj_list(
				state.cleanup_ship_index, state.cleanup_object_index) &&
			register_fixture_ship_in_ship_obj_list(
				state.target_ship_index, state.target_object_index);
		if (!state.list_registered) {
			return;
		}
		auto& repaired_ai = Ai_info[state.cleanup_ship_index];
		repaired_ai.ai_flags.set(AI::AI_Flags::Being_repaired);
		ai_do_objects_repairing_stuff(
			&gameplay_object, nullptr, REPAIR_INFO_BROKEN);
		state.support_return = ship_do_rearm_frame(&gameplay_object, 0.0F);
		state.support_flags =
			(repaired_ai.ai_flags[AI::AI_Flags::Being_repaired] ? 1U : 0U) |
			(repaired_ai.ai_flags[AI::AI_Flags::Awaiting_repair] ? 2U : 0U);

		state.test_player.cargo_inspect_time = 0;
		Player = &state.test_player;
		Player_obj = &gameplay_object;
		Player_ship = &gameplay_ship;
		Player_ai = &repaired_ai;
		Player_ai->target_objnum = state.target_object_index;
		Player_ai->current_target_distance = 50.0F;
		Target_display_cargo = false;
		state.cargo_advance_count = 0U;
		hud_cargo_scan_update(&target_object, 0.25F);
		state.cargo_progress_ms = Player->cargo_inspect_time;
		if (state.cargo_progress_ms == 250) {
			++state.cargo_advance_count;
		}
		hud_cargo_scan_update(&target_object, 0.30F);
		if (target_ship.flags[Ship::Ship_Flags::Cargo_revealed] &&
			Player->cargo_inspect_time == 0) {
			++state.cargo_advance_count;
		}
		state.cargo_final_ms = Player->cargo_inspect_time;
		state.cargo_revealed =
			target_ship.flags[Ship::Ship_Flags::Cargo_revealed];
		state.target_display_cargo = Target_display_cargo;

		Player = nullptr;
		Player_obj = nullptr;
		Player_ship = nullptr;
		Player_ai = nullptr;
		gameplay_object.flags.set(Object::Object_Flags::Should_be_dead);
		::ship_cleanup(state.cleanup_ship_index, SHIP_DESTROYED_REDALERT);
		state.list_unregistered =
			unregister_fixture_ship_from_ship_obj_list(
				state.cleanup_ship_index, state.cleanup_object_index) &&
			unregister_fixture_ship_from_ship_obj_list(
				state.target_ship_index, state.target_object_index);
	};
	const auto capture = [](void* opaque) noexcept {
		const auto& state = *static_cast<Context*>(opaque);
		GameplayAbSnapshot snapshot;
		snapshot.controls = game_is_photo_mode_active();
		snapshot.gameplay_return = state.support_return;
		snapshot.gameplay_mutations = state.support_flags;
		snapshot.cargo_advance_count = state.cargo_advance_count;
		snapshot.cargo_reset_count =
			static_cast<std::uint64_t>(state.cargo_final_ms);
		snapshot.cargo_reveal_count = state.cargo_revealed;
		snapshot.hud_side_effect_count = state.target_display_cargo;
		snapshot.allocation_count = state.recorder->control_calls;
		snapshot.socket_count = state.recorder->cargo_calls;
		return snapshot;
	};

	const auto prior_photo_mode = Photo_mode_active;
	const auto prior_player = Player;
	const auto prior_player_obj = Player_obj;
	const auto prior_player_ship = Player_ship;
	const auto prior_player_ai = Player_ai;
	const auto prior_target = Ai_info[0].target_objnum;
	const auto prior_game_skill_level = Game_skill_level;
	const auto prior_new_scanning_behavior = Use_new_scanning_behavior;
	const auto prior_target_display_cargo = Target_display_cargo;
	const auto prior_game_sound_count = Snds.size();
	const auto prior_num_cargo = Num_cargo;
	const auto prior_cargo_name_zero = Cargo_names[0];
	char prior_cargo_name_buffer_zero[NAME_LENGTH]{};
	std::memcpy(prior_cargo_name_buffer_zero,
		Cargo_names_buf[0],
		sizeof(prior_cargo_name_buffer_zero));
	const auto created_ship_info = Ship_info.empty();
	if (created_ship_info) {
		Ship_info.emplace_back();
	}
	context.ship_info_index = 0;
	auto& gameplay_class = Ship_info[context.ship_info_index];
	const auto prior_cmeasure_type = gameplay_class.cmeasure_type;
	const auto prior_sup_hull_repair_rate = gameplay_class.sup_hull_repair_rate;
	const auto prior_sup_shield_repair_rate = gameplay_class.sup_shield_repair_rate;
	const auto prior_sup_subsys_repair_rate = gameplay_class.sup_subsys_repair_rate;
	const auto prior_scan_time = gameplay_class.scan_time;
	const auto prior_scan_range_normal = gameplay_class.scan_range_normal;
	const auto prior_scanning_time_multiplier =
		gameplay_class.scanning_time_multiplier;
	const auto prior_scanning_range_multiplier =
		gameplay_class.scanning_range_multiplier;
	gameplay_class.cmeasure_type = -1;
	gameplay_class.sup_hull_repair_rate = 0.0F;
	gameplay_class.sup_shield_repair_rate = 0.0F;
	gameplay_class.sup_subsys_repair_rate = 0.0F;
	gameplay_class.scan_time = 500;
	gameplay_class.scan_range_normal = 100.0F;
	gameplay_class.scanning_time_multiplier = 1.0F;
	gameplay_class.scanning_range_multiplier = 1.0F;
	ship_level_init();
	const auto prior_ai_profile = The_mission.ai_profile;
	const auto prior_countermeasure_capacity = Countermeasures_use_capacity;
	const auto prior_max_hull = The_mission.support_ships.max_hull_repair_val;
	const auto prior_max_subsys = The_mission.support_ships.max_subsys_repair_val;
	const auto prior_disallow = The_mission.support_ships.disallow_rearm;
	const auto prior_support_hull =
		The_mission.flags[Mission::Mission_Flags::Support_repairs_hull];
	if (The_mission.ai_profile == nullptr) {
		The_mission.ai_profile = &Ai_profiles[0];
	}
	Countermeasures_use_capacity = false;
	The_mission.support_ships.max_hull_repair_val = 0.0F;
	The_mission.support_ships.max_subsys_repair_val = 0.0F;
	The_mission.support_ships.disallow_rearm = false;
	The_mission.flags.remove(Mission::Mission_Flags::Support_repairs_hull);
	Ship_registry.emplace_back("Telemetry AB isolated ship");
	context.registry_index = static_cast<int>(Ship_registry.size() - 1U);
	Ship_registry_map["Telemetry AB isolated ship"] = context.registry_index;
	context.exited_ship_count = Ships_exited.size();
	Photo_mode_active = true;
	Game_skill_level = 0;
	Use_new_scanning_behavior = false;
	Cargo_names[0] = Cargo_names_buf[0];
	strcpy_s(Cargo_names[0], NAME_LENGTH, "Nothing");
	Num_cargo = 1;
	if (Snds.size() <= static_cast<std::size_t>(GameSounds::CARGO_REVEAL)) {
		Snds.resize(static_cast<std::size_t>(GameSounds::CARGO_REVEAL) + 1U);
	}
	const auto run = run_phase2_gameplay_ab(invoke, capture, &context, &recorder);
	Photo_mode_active = prior_photo_mode;
	Player = prior_player;
	Player_obj = prior_player_obj;
	Player_ship = prior_player_ship;
	Player_ai = prior_player_ai;
	Ai_info[0].target_objnum = prior_target;
	Game_skill_level = prior_game_skill_level;
	Use_new_scanning_behavior = prior_new_scanning_behavior;
	Target_display_cargo = prior_target_display_cargo;
	Num_cargo = prior_num_cargo;
	std::memcpy(Cargo_names_buf[0],
		prior_cargo_name_buffer_zero,
		sizeof(prior_cargo_name_buffer_zero));
	Cargo_names[0] = prior_cargo_name_zero;
	Snds.resize(prior_game_sound_count);
	The_mission.ai_profile = prior_ai_profile;
	Countermeasures_use_capacity = prior_countermeasure_capacity;
	The_mission.support_ships.max_hull_repair_val = prior_max_hull;
	The_mission.support_ships.max_subsys_repair_val = prior_max_subsys;
	The_mission.support_ships.disallow_rearm = prior_disallow;
	The_mission.flags.set(
		Mission::Mission_Flags::Support_repairs_hull, prior_support_hull);
	Ship_registry_map.erase("Telemetry AB isolated ship");
	Ship_registry.pop_back();
	Ships_exited.resize(context.exited_ship_count);
	Objects[context.cleanup_object_index].clear();
	Ships[context.cleanup_ship_index].clear();
	Ai_info[context.cleanup_ship_index] = ai_info{};
	Objects[context.target_object_index].clear();
	Ships[context.target_ship_index].clear();
	Ai_info[context.target_ship_index] = ai_info{};
	gameplay_class.cmeasure_type = prior_cmeasure_type;
	gameplay_class.sup_hull_repair_rate = prior_sup_hull_repair_rate;
	gameplay_class.sup_shield_repair_rate = prior_sup_shield_repair_rate;
	gameplay_class.sup_subsys_repair_rate = prior_sup_subsys_repair_rate;
	gameplay_class.scan_time = prior_scan_time;
	gameplay_class.scan_range_normal = prior_scan_range_normal;
	gameplay_class.scanning_time_multiplier = prior_scanning_time_multiplier;
	gameplay_class.scanning_range_multiplier = prior_scanning_range_multiplier;
	if (created_ship_info) {
		Ship_info.pop_back();
	}

	EXPECT_EQ(run.snapshots[0].controls, run.snapshots[1].controls);
	EXPECT_EQ(run.snapshots[0].controls, run.snapshots[2].controls);
	EXPECT_EQ(run.snapshots[0].gameplay_return, run.snapshots[1].gameplay_return);
	EXPECT_EQ(run.snapshots[0].gameplay_return, run.snapshots[2].gameplay_return);
	EXPECT_EQ(run.snapshots[0].gameplay_mutations, run.snapshots[1].gameplay_mutations);
	EXPECT_EQ(run.snapshots[0].gameplay_mutations, run.snapshots[2].gameplay_mutations);
	EXPECT_EQ(run.snapshots[0].cargo_advance_count, run.snapshots[1].cargo_advance_count);
	EXPECT_EQ(run.snapshots[0].cargo_advance_count, run.snapshots[2].cargo_advance_count);
	EXPECT_EQ(run.snapshots[0].cargo_reveal_count, run.snapshots[1].cargo_reveal_count);
	EXPECT_EQ(run.snapshots[0].cargo_reveal_count, run.snapshots[2].cargo_reveal_count);
	EXPECT_EQ(run.snapshots[0].cargo_reset_count, run.snapshots[1].cargo_reset_count);
	EXPECT_EQ(run.snapshots[0].cargo_reset_count, run.snapshots[2].cargo_reset_count);
	EXPECT_EQ(run.snapshots[0].hud_side_effect_count,
		run.snapshots[1].hud_side_effect_count);
	EXPECT_EQ(run.snapshots[0].hud_side_effect_count,
		run.snapshots[2].hud_side_effect_count);
	// controls, Broken, two cargo updates, cleanup and its End emit once each.
	EXPECT_EQ(run.snapshots[0].hook_calls + 6U, run.snapshots[1].hook_calls);
	EXPECT_EQ(run.snapshots[0].hook_calls, run.snapshots[2].hook_calls);
	EXPECT_EQ(2U, run.snapshots[0].gameplay_mutations);
	EXPECT_EQ(2U, run.snapshots[0].cargo_advance_count);
	EXPECT_EQ(0U, run.snapshots[0].cargo_reset_count);
	EXPECT_EQ(1U, run.snapshots[0].cargo_reveal_count);
	EXPECT_EQ(1U, run.snapshots[0].hud_side_effect_count);
	EXPECT_EQ(1U, recorder.control_calls);
	EXPECT_EQ(2U, recorder.support_calls);
	EXPECT_EQ(1U, recorder.support_broken_calls);
	EXPECT_EQ(1U, recorder.support_end_calls);
	EXPECT_EQ(2U, recorder.cargo_calls);
	EXPECT_EQ(1U, recorder.cargo_scanning_calls);
	EXPECT_EQ(1U, recorder.cargo_completed_calls);
	EXPECT_EQ(1U, recorder.cleanup_calls);
	EXPECT_EQ(ControlTargetAuthority::Camera, recorder.last_authority);
	EXPECT_EQ(SupportTransitionReason::End, recorder.last_support_reason);
	EXPECT_TRUE(context.list_registered);
	EXPECT_TRUE(context.list_unregistered);
}

enum class SupportCase {
	MissionDisallowRearm,
	WeaponInfoDisallowRearm,
	AmmunitionMissionRearmPoolClassTeam,
	CountermeasureMissionRearmPoolClassTeam,
	CountermeasureCurrentBelowMaximum,
	MaxHullRepairValMinusOne,
	MaxHullRepairVal101,
	MaxSubsysRepairValMinusOne,
	MaxSubsysRepairVal101,
	RateAndApplicability,
	RateAboveOne,
	RateNaN,
	SharedOracleEqualsAdapterCapture,
};

auto evaluation_fields(const SupportWorkEvaluation& value)
{
	return std::make_tuple(value.support_repairs_hull_authorized,
		value.mission_rearm_disallowed,
		value.weapon_rearm_disallowed,
		value.hull_repair_applicable,
		value.shield_repair_applicable,
		value.subsystem_repair_applicable,
		value.weapon_energy_rearm_applicable,
		value.ammunition_rearm_applicable,
		value.countermeasure_rearm_applicable,
		value.max_hull_repair_fraction,
		value.max_subsystem_repair_fraction,
		value.hull_repair_work,
		value.shield_repair_work,
		value.subsystem_repair_work,
		value.weapon_energy_rearm_work,
		value.ammunition_rearm_work,
		value.countermeasure_rearm_work,
		value.countermeasure_capacity,
		value.countermeasure_rearm_pool);
}

struct SupportCaseParam {
	const char* name;
	SupportCase value;
};

SupportWorkInput baseline()
{
	SupportWorkInput input;
	input.support_repairs_hull = true;
	input.max_hull_repair_val = 50.0F;
	input.max_subsys_repair_val = 60.0F;
	input.sup_hull_repair_rate = 0.1F;
	input.sup_shield_repair_rate = 0.2F;
	input.sup_subsys_repair_rate = 0.3F;
	input.hull_current = 20.0F;
	input.hull_maximum = 100.0F;
	input.shield_current = 10.0F;
	input.shield_maximum = 40.0F;
	input.subsystem_repair_work = 12.0F;
	input.weapon_energy_current = 10.0F;
	input.weapon_energy_maximum = 50.0F;
	input.rearm_component_count = 1U;
	input.rearm_components[0] = {2, 10, -1, false, false};
	input.countermeasure_current = 1;
	input.countermeasure_maximum = 4;
	input.countermeasure_rearm_pool = -1;
	return input;
}

class SupportWorkContract : public ::testing::TestWithParam<SupportCaseParam> {};

TEST_P(SupportWorkContract, ExactBranchesAndSharedOracle)
{
	auto input = baseline();
	auto expected_status = SupportWorkStatus::Valid;
	switch (GetParam().value) {
	case SupportCase::MissionDisallowRearm: // mission_disallow_rearm
		input.mission_disallow_rearm = true;
		break;
	case SupportCase::WeaponInfoDisallowRearm: // weapon_info_disallow_rearm
		input.rearm_components[0].weapon_info_disallow_rearm = true;
		break;
	case SupportCase::AmmunitionMissionRearmPoolClassTeam: // mission_rearm_pool_class_team
		input.rearm_components[0].rearm_pool = 0;
		break;
	case SupportCase::CountermeasureMissionRearmPoolClassTeam:
		input.countermeasure_rearm_pool = 0;
		break;
	case SupportCase::CountermeasureCurrentBelowMaximum: // countermeasure_current_below_maximum
		input.countermeasure_current = 3;
		input.countermeasure_maximum = 5;
		break;
	case SupportCase::MaxHullRepairValMinusOne: // max_hull_repair_val_minus_one
		input.max_hull_repair_val = -1.0F;
		expected_status = SupportWorkStatus::UnsupportedEngineState;
		break;
	case SupportCase::MaxHullRepairVal101: // max_hull_repair_val_101
		input.max_hull_repair_val = 101.0F;
		expected_status = SupportWorkStatus::UnsupportedEngineState;
		break;
	case SupportCase::MaxSubsysRepairValMinusOne: // max_subsys_repair_val_minus_one
		input.max_subsys_repair_val = -1.0F;
		expected_status = SupportWorkStatus::UnsupportedEngineState;
		break;
	case SupportCase::MaxSubsysRepairVal101: // max_subsys_repair_val_101
		input.max_subsys_repair_val = 101.0F;
		expected_status = SupportWorkStatus::UnsupportedEngineState;
		break;
	case SupportCase::RateAndApplicability: // rate_and_applicability
		input.sup_hull_repair_rate = 0.0F;
		input.sup_shield_repair_rate = 0.0F;
		input.sup_subsys_repair_rate = 0.0F;
		break;
	case SupportCase::RateAboveOne:
		input.sup_hull_repair_rate = 1.01F;
		expected_status = SupportWorkStatus::UnsupportedEngineState;
		break;
	case SupportCase::RateNaN:
		input.sup_hull_repair_rate = std::numeric_limits<float>::quiet_NaN();
		expected_status = SupportWorkStatus::UnsupportedEngineState;
		break;
	case SupportCase::SharedOracleEqualsAdapterCapture: // shared_oracle_equals_adapter_capture
		break;
	}

	SupportWorkEvaluation first;
	EXPECT_EQ(expected_status, evaluate_support_work(input, first));
	if (expected_status != SupportWorkStatus::Valid) {
		const SupportWorkEvaluation zero{};
		EXPECT_EQ(evaluation_fields(zero), evaluation_fields(first));
		return;
	}

	EXPECT_FLOAT_EQ(0.5F, first.max_hull_repair_fraction);
	EXPECT_FLOAT_EQ(0.6F, first.max_subsystem_repair_fraction);
	EXPECT_FLOAT_EQ(30.0F, first.hull_repair_work);
	EXPECT_FLOAT_EQ(30.0F, first.shield_repair_work);
	EXPECT_FLOAT_EQ(12.0F, first.subsystem_repair_work);
	EXPECT_FLOAT_EQ(40.0F, first.weapon_energy_rearm_work);
	EXPECT_EQ(GetParam().value == SupportCase::WeaponInfoDisallowRearm ? 0U : 8U,
		first.ammunition_rearm_work);

	if (GetParam().value == SupportCase::MissionDisallowRearm) {
		EXPECT_TRUE(first.mission_rearm_disallowed);
		EXPECT_FALSE(first.weapon_energy_rearm_applicable);
		EXPECT_FALSE(first.ammunition_rearm_applicable);
		EXPECT_TRUE(first.countermeasure_rearm_applicable);
	} else if (GetParam().value == SupportCase::WeaponInfoDisallowRearm) {
		EXPECT_TRUE(first.weapon_rearm_disallowed);
		EXPECT_FALSE(first.ammunition_rearm_applicable);
		EXPECT_TRUE(first.countermeasure_rearm_applicable);
	} else if (GetParam().value == SupportCase::AmmunitionMissionRearmPoolClassTeam) {
		EXPECT_FALSE(first.ammunition_rearm_applicable);
		EXPECT_TRUE(first.countermeasure_rearm_applicable);
	} else if (GetParam().value == SupportCase::CountermeasureMissionRearmPoolClassTeam) {
		EXPECT_TRUE(first.ammunition_rearm_applicable);
		EXPECT_TRUE(first.countermeasure_rearm_applicable);
	} else if (GetParam().value == SupportCase::CountermeasureCurrentBelowMaximum) {
		EXPECT_EQ(5U, first.countermeasure_capacity);
		EXPECT_EQ(2U, first.countermeasure_rearm_work);
		EXPECT_TRUE(first.countermeasure_rearm_applicable);
	} else if (GetParam().value == SupportCase::RateAndApplicability) {
		EXPECT_FALSE(first.hull_repair_applicable);
		EXPECT_FALSE(first.shield_repair_applicable);
		EXPECT_FALSE(first.subsystem_repair_applicable);
	} else {
		EXPECT_TRUE(first.hull_repair_applicable);
		EXPECT_TRUE(first.shield_repair_applicable);
		EXPECT_TRUE(first.subsystem_repair_applicable);
		EXPECT_TRUE(first.weapon_energy_rearm_applicable);
		EXPECT_TRUE(first.ammunition_rearm_applicable);
		EXPECT_TRUE(first.countermeasure_rearm_applicable);
	}

	SupportWorkEvaluation second;
	ASSERT_EQ(SupportWorkStatus::Valid, evaluate_support_work(input, second));
	EXPECT_EQ(evaluation_fields(first), evaluation_fields(second));
}

INSTANTIATE_TEST_SUITE_P(FinalB,
	SupportWorkContract,
	::testing::Values(
		SupportCaseParam{"MissionDisallowRearm", SupportCase::MissionDisallowRearm},
		SupportCaseParam{"WeaponInfoDisallowRearm", SupportCase::WeaponInfoDisallowRearm},
		SupportCaseParam{"AmmunitionMissionRearmPoolClassTeam", SupportCase::AmmunitionMissionRearmPoolClassTeam},
		SupportCaseParam{"CountermeasureMissionRearmPoolClassTeam", SupportCase::CountermeasureMissionRearmPoolClassTeam},
		SupportCaseParam{"CountermeasureCurrentBelowMaximum", SupportCase::CountermeasureCurrentBelowMaximum},
		SupportCaseParam{"MaxHullRepairValMinusOne", SupportCase::MaxHullRepairValMinusOne},
		SupportCaseParam{"MaxHullRepairVal101", SupportCase::MaxHullRepairVal101},
		SupportCaseParam{"MaxSubsysRepairValMinusOne", SupportCase::MaxSubsysRepairValMinusOne},
		SupportCaseParam{"MaxSubsysRepairVal101", SupportCase::MaxSubsysRepairVal101},
		SupportCaseParam{"RateAndApplicability", SupportCase::RateAndApplicability},
		SupportCaseParam{"RateAboveOne", SupportCase::RateAboveOne},
		SupportCaseParam{"RateNaN", SupportCase::RateNaN},
		SupportCaseParam{"SharedOracleEqualsAdapterCapture", SupportCase::SharedOracleEqualsAdapterCapture}));

} // namespace
