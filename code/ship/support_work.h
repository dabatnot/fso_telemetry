#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

class object;

namespace telemetry::detail {

constexpr std::size_t MaximumSupportWorkComponents = 512U;

enum class SupportWorkStatus : std::uint8_t {
	Valid = 0,
	UnsupportedEngineState,
};

struct SupportRearmComponentInput {
	std::int32_t current = 0;
	std::int32_t maximum = 0;
	std::int32_t rearm_pool = -1;
	bool weapon_info_disallow_rearm = false;
	bool ammoless = false;
};

struct SupportWorkInput {
	bool support_rearm_allowed = true;
	bool mission_disallow_rearm = false;
	bool support_repairs_hull = false;
	float max_hull_repair_val = 100.0F;
	float max_subsys_repair_val = 100.0F;
	float sup_hull_repair_rate = 0.0F;
	float sup_shield_repair_rate = 0.0F;
	float sup_subsys_repair_rate = 0.0F;
	float hull_current = 0.0F;
	float hull_maximum = 0.0F;
	float shield_current = 0.0F;
	float shield_maximum = 0.0F;
	float subsystem_repair_work = 0.0F;
	float weapon_energy_current = 0.0F;
	float weapon_energy_maximum = 0.0F;
	std::size_t rearm_component_count = 0U;
	std::array<SupportRearmComponentInput, MaximumSupportWorkComponents>
		rearm_components{};
	std::int32_t countermeasure_current = 0;
	std::int32_t countermeasure_maximum = 0;
	std::int32_t countermeasure_rearm_pool = -1;
	bool countermeasure_weapon_info_disallow_rearm = false;
};

struct SupportWorkEvaluation {
	bool support_repairs_hull_authorized = false;
	bool mission_rearm_disallowed = false;
	bool weapon_rearm_disallowed = false;
	bool hull_repair_applicable = false;
	bool shield_repair_applicable = false;
	bool subsystem_repair_applicable = false;
	bool weapon_energy_rearm_applicable = false;
	bool ammunition_rearm_applicable = false;
	bool countermeasure_rearm_applicable = false;
	float max_hull_repair_fraction = 0.0F;
	float max_subsystem_repair_fraction = 0.0F;
	float hull_repair_work = 0.0F;
	float shield_repair_work = 0.0F;
	float subsystem_repair_work = 0.0F;
	float weapon_energy_rearm_work = 0.0F;
	std::uint64_t ammunition_rearm_work = 0U;
	std::uint64_t countermeasure_rearm_work = 0U;
	std::uint64_t countermeasure_capacity = 0U;
	std::int32_t countermeasure_rearm_pool = -1;
};

SupportWorkStatus evaluate_support_work(
	const SupportWorkInput& input, SupportWorkEvaluation& output) noexcept;
SupportWorkStatus evaluate_support_work(
	object* repaired_object, SupportWorkEvaluation& output) noexcept;

} // namespace telemetry::detail
