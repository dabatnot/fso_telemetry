#pragma once

#include "telemetry/engine_adapter.h"

#include <cstdint>

namespace telemetry::detail {

enum class EntityIdResolveStatus : std::uint8_t {
	Existing = 0,
	Allocated,
	InvalidKey,
	CounterExhausted,
	Count,
};

enum class EntityIdInvalidationStatus : std::uint8_t {
	Invalidated = 0,
	AlreadyInvalid,
	Count,
};

enum class PlayerSampleMaterializeStatus : std::uint8_t {
	MaterializedExisting = 0,
	MaterializedNew,
	NoPlayer,
	InvalidSource,
	InvalidCapture,
	EntityIdCounterExhausted,
	Count,
};

struct EntityIdResolveResult {
	EntityIdResolveStatus status = EntityIdResolveStatus::InvalidKey;
	std::uint64_t entity_id = 0U;
};

class EntityIdRegistry final {
  public:
	EntityIdRegistry() noexcept = default;
	explicit EntityIdRegistry(std::uint64_t last_allocated_entity_id) noexcept;

	EntityIdResolveResult resolve(const PlayerObservationKey& key) noexcept;
	EntityIdInvalidationStatus invalidate() noexcept;
	void reset_session() noexcept;

	bool has_active_mapping() const noexcept;
	std::uint32_t active_object_signature() const noexcept;
	std::uint64_t active_entity_id() const noexcept;
	std::uint64_t last_allocated_entity_id() const noexcept;

  private:
	bool m_has_active_mapping = false;
	std::uint32_t m_active_object_signature = 0U;
	std::uint64_t m_active_entity_id = 0U;
	std::uint64_t m_last_allocated_entity_id = 0U;
};

PlayerSampleMaterializeStatus materialize_player_sample(EntityIdRegistry& registry,
	const CaptureResult& capture,
	const PlayerObservationDto& observation,
	PlayerKinematicsSample& output) noexcept;

} // namespace telemetry::detail
