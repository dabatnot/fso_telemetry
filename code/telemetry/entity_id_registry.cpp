#include "telemetry/entity_id_registry.h"

#include <limits>

namespace telemetry::detail {

EntityIdRegistry::EntityIdRegistry(std::uint64_t last_allocated_entity_id) noexcept
	: m_last_allocated_entity_id(last_allocated_entity_id)
{
}

EntityIdResolveResult EntityIdRegistry::resolve(const PlayerObservationKey& key) noexcept
{
	if (key.object_signature == 0U) {
		invalidate();
		return {};
	}

	if (m_has_active_mapping && key.object_signature == m_active_object_signature) {
		return EntityIdResolveResult{EntityIdResolveStatus::Existing, m_active_entity_id};
	}

	invalidate();
	if (m_last_allocated_entity_id == std::numeric_limits<std::uint64_t>::max()) {
		return EntityIdResolveResult{EntityIdResolveStatus::CounterExhausted, 0U};
	}

	++m_last_allocated_entity_id;
	m_has_active_mapping = true;
	m_active_object_signature = key.object_signature;
	m_active_entity_id = m_last_allocated_entity_id;
	return EntityIdResolveResult{EntityIdResolveStatus::Allocated, m_active_entity_id};
}

EntityIdInvalidationStatus EntityIdRegistry::invalidate() noexcept
{
	const auto status = m_has_active_mapping ? EntityIdInvalidationStatus::Invalidated
											 : EntityIdInvalidationStatus::AlreadyInvalid;
	m_has_active_mapping = false;
	m_active_object_signature = 0U;
	m_active_entity_id = 0U;
	return status;
}

void EntityIdRegistry::reset_session() noexcept
{
	invalidate();
	m_last_allocated_entity_id = 0U;
}

bool EntityIdRegistry::has_active_mapping() const noexcept
{
	return m_has_active_mapping;
}

std::uint32_t EntityIdRegistry::active_object_signature() const noexcept
{
	return m_active_object_signature;
}

std::uint64_t EntityIdRegistry::active_entity_id() const noexcept
{
	return m_active_entity_id;
}

std::uint64_t EntityIdRegistry::last_allocated_entity_id() const noexcept
{
	return m_last_allocated_entity_id;
}

PlayerSampleMaterializeStatus materialize_player_sample(EntityIdRegistry& registry,
	const CaptureResult& capture,
	const PlayerObservationDto& observation,
	PlayerKinematicsSample& output) noexcept
{
	output = {};

	if (capture.status == CaptureStatus::NoPlayer && capture.reason >= CaptureReason::NotInMission &&
		capture.reason <= CaptureReason::MissingPlayerShip) {
		registry.invalidate();
		return PlayerSampleMaterializeStatus::NoPlayer;
	}
	if (capture.status == CaptureStatus::InvalidSource && capture.reason >= CaptureReason::WrongObjectType &&
		capture.reason < CaptureReason::Count) {
		registry.invalidate();
		return PlayerSampleMaterializeStatus::InvalidSource;
	}
	if (capture.status != CaptureStatus::Valid || capture.reason != CaptureReason::None ||
		observation.key.object_signature == 0U) {
		registry.invalidate();
		return PlayerSampleMaterializeStatus::InvalidCapture;
	}

	const auto resolved = registry.resolve(observation.key);
	if (resolved.status == EntityIdResolveStatus::CounterExhausted) {
		return PlayerSampleMaterializeStatus::EntityIdCounterExhausted;
	}
	if (resolved.status != EntityIdResolveStatus::Existing &&
		resolved.status != EntityIdResolveStatus::Allocated) {
		return PlayerSampleMaterializeStatus::InvalidCapture;
	}

	PlayerKinematicsSample candidate{};
	candidate.entity_id = resolved.entity_id;
	candidate.value = observation.value;
	output = candidate;
	return resolved.status == EntityIdResolveStatus::Existing
		? PlayerSampleMaterializeStatus::MaterializedExisting
		: PlayerSampleMaterializeStatus::MaterializedNew;
}

} // namespace telemetry::detail
