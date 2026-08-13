#include "telemetry/phase4_docking_collector.h"

#include "globalincs/linklist.h"
#include "model/model.h"
#include "object/object.h"
#include "object/objectdock.h"
#include "ship/ship.h"

#include <utility>

namespace telemetry::detail {
namespace {

const Phase4EngineInventoryEntry* find_ship(
	const std::vector<Phase4EngineInventoryEntry>& inventory,
	std::uint64_t signature) noexcept
{
	for (const auto& entry : inventory)
		if (entry.identity.object_type == protocol::ObjectType::Ship &&
			entry.identity.stable_identity == signature)
			return &entry;
	return nullptr;
}

bool valid_ship(const object* value) noexcept
{
	return value != nullptr && value->type == OBJ_SHIP && value->signature > 0 &&
		value->instance >= 0 && value->instance < MAX_SHIPS &&
		Ships[value->instance].objnum == OBJ_INDEX(value) &&
		Ships[value->instance].ship_info_index >= 0;
}

bool dock_name(const object& value, int dockpoint, const char*& output) noexcept
{
	if (!valid_ship(&value) || dockpoint < 0 || dockpoint > 4095) return false;
	const auto model = Ship_info[Ships[value.instance].ship_info_index].model_num;
	output = model_get_dock_name(model, dockpoint);
	return output != nullptr;
}

} // namespace

Phase4DockingCollectorStatus collect_phase4_docking_relations(
	const std::vector<Phase4EngineInventoryEntry>& inventory,
	std::vector<Phase4DockingRelation>& output) noexcept
{
	if (output.capacity() < 64U) return Phase4DockingCollectorStatus::CapacityExceeded;
	// The runtime storage reserves this workspace at startup. Clearing it before
	// traversal makes a failed collection unpublishable and avoids allocations
	// on the main-thread capture path.
	output.clear();
	for (auto* local = GET_FIRST(&obj_used_list); local != END_OF_LIST(&obj_used_list);
		local = GET_NEXT(local)) {
		if (!valid_ship(local) || local->dock_list == nullptr) continue;
		const auto* local_entry = find_ship(inventory,
			static_cast<std::uint64_t>(local->signature));
		if (local_entry == nullptr) return Phase4DockingCollectorStatus::UnknownEndpoint;
		for (auto* link = local->dock_list; link != nullptr; link = link->next) {
			if (!valid_ship(link->docked_objp)) return Phase4DockingCollectorStatus::InvalidSource;
			const auto* remote_entry = find_ship(inventory,
				static_cast<std::uint64_t>(link->docked_objp->signature));
			if (remote_entry == nullptr) return Phase4DockingCollectorStatus::UnknownEndpoint;
			const auto remote_point = dock_find_dockpoint_used_by_object(
				link->docked_objp, local);
			const char* local_name = nullptr;
			const char* remote_name = nullptr;
			if (remote_point < 0 || !dock_name(*local, link->dockpoint_used, local_name) ||
				!dock_name(*link->docked_objp, remote_point, remote_name))
				return Phase4DockingCollectorStatus::InvalidSource;
			if (output.size() == output.capacity())
				return Phase4DockingCollectorStatus::CapacityExceeded;
			output.push_back({local_entry->entity_id, remote_entry->entity_id,
				static_cast<std::uint16_t>(link->dockpoint_used),
				static_cast<std::uint16_t>(remote_point), local_name, remote_name});
		}
	}
	return Phase4DockingCollectorStatus::Collected;
}

} // namespace telemetry::detail
