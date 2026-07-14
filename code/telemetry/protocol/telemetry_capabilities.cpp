#include "telemetry/protocol/telemetry_capabilities.h"

namespace telemetry::protocol {

namespace {

constexpr bool is_complete_pair(std::uint64_t capabilities, std::uint64_t pair) noexcept
{
	const auto selected = capabilities & pair;
	return selected == 0 || selected == pair;
}

ValidationError validate_active_shape(std::uint64_t active) noexcept
{
	if (!is_complete_pair(active, CommViewCapabilityPair) || !is_complete_pair(active, TargetVideoCapabilityPair)) {
		return ValidationError::CapabilityNotNegotiated;
	}
	return ValidationError::None;
}

ValidationError
validate_initial_effective_masks(std::uint64_t client, std::uint64_t producer, std::uint64_t active) noexcept
{
	if ((client & ~ClientOwnedCapabilities) != 0 || (producer & ~ProducerOwnedCapabilities) != 0) {
		return ValidationError::CapabilityNotNegotiated;
	}
	if (const auto error = validate_active_shape(active); error != ValidationError::None) {
		return error;
	}

	if ((active & CommViewCapabilityPair) != 0 &&
		((client & CapabilityCommViewLocalAssets) == 0 || (producer & CapabilityCommViewAuthoritativeSource) == 0)) {
		return ValidationError::CapabilityNotNegotiated;
	}
	if ((active & TargetVideoCapabilityPair) != 0 &&
		((client & CapabilityTargetVideoH264) == 0 || (producer & CapabilityTargetVideoRemoteRender) == 0)) {
		return ValidationError::CapabilityNotNegotiated;
	}
	if ((active & CapabilityUpdate) != 0 && ((client & CapabilityUpdate) == 0 || (producer & CapabilityUpdate) == 0)) {
		return ValidationError::CapabilityNotNegotiated;
	}
	return ValidationError::None;
}

bool updates_equal(const CapabilityUpdatePayload& lhs, const CapabilityUpdatePayload& rhs) noexcept
{
	return lhs.capability_generation == rhs.capability_generation &&
		   lhs.advertised_capabilities == rhs.advertised_capabilities &&
		   lhs.active_capabilities == rhs.active_capabilities && lhs.effective_time_us == rhs.effective_time_us &&
		   lhs.reason == rhs.reason;
}

CapabilityUpdateOutcome outcome(CapabilityUpdateDisposition disposition,
	ValidationError error = ValidationError::None,
	std::uint64_t removed = 0) noexcept
{
	return CapabilityUpdateOutcome{disposition, error, removed};
}

} // namespace

ValidationError validate_emittable_capability_offer(CapabilityPeerRole owner, std::uint64_t capabilities) noexcept
{
	const auto owner_mask = owned_capabilities(owner);
	if ((capabilities & ~KnownCapabilities) != 0) {
		return ValidationError::ReservedFlag;
	}
	if (owner_mask == 0 || (capabilities & ~owner_mask) != 0) {
		return ValidationError::CapabilityNotNegotiated;
	}
	return ValidationError::None;
}

ValidationError validate_emittable_active_capabilities(std::uint64_t active_capabilities) noexcept
{
	if ((active_capabilities & ~KnownCapabilities) != 0) {
		return ValidationError::ReservedFlag;
	}
	return validate_active_shape(active_capabilities);
}

void CapabilitySessionState::reset() noexcept
{
	*this = CapabilitySessionState{};
}

ValidationError initialize_capability_session(std::uint64_t client_advertised,
	std::uint64_t producer_advertised,
	std::uint64_t selected_active,
	CapabilitySessionState& state) noexcept
{
	const auto client = effective_capabilities(client_advertised);
	const auto producer = effective_capabilities(producer_advertised);
	const auto active = effective_capabilities(selected_active);
	if (const auto error = validate_initial_effective_masks(client, producer, active); error != ValidationError::None) {
		return error;
	}

	CapabilitySessionState candidate;
	candidate.m_client.advertised = client;
	candidate.m_client.declared_active = active;
	candidate.m_producer.advertised = producer;
	candidate.m_producer.declared_active = active;
	candidate.m_active = active;
	candidate.m_initialized = true;
	candidate.m_update_negotiated_initially = (active & CapabilityUpdate) != 0;
	candidate.m_client.update_channel_open = candidate.m_update_negotiated_initially;
	candidate.m_producer.update_channel_open = candidate.m_update_negotiated_initially;
	state = candidate;
	return ValidationError::None;
}

ValidationError negotiate_initial_capabilities(std::uint64_t client_advertised,
	std::uint64_t producer_advertised,
	std::uint64_t compatible_visual_pairs,
	CapabilitySessionState& state) noexcept
{
	if ((compatible_visual_pairs & ~VisualCapabilities) != 0 ||
		!is_complete_pair(compatible_visual_pairs, CommViewCapabilityPair) ||
		!is_complete_pair(compatible_visual_pairs, TargetVideoCapabilityPair)) {
		return ValidationError::CapabilityNotNegotiated;
	}

	const auto client = effective_capabilities(client_advertised);
	const auto producer = effective_capabilities(producer_advertised);
	std::uint64_t active = 0;
	if ((compatible_visual_pairs & CommViewCapabilityPair) == CommViewCapabilityPair &&
		(client & CapabilityCommViewLocalAssets) != 0 && (producer & CapabilityCommViewAuthoritativeSource) != 0) {
		active |= CommViewCapabilityPair;
	}
	if ((compatible_visual_pairs & TargetVideoCapabilityPair) == TargetVideoCapabilityPair &&
		(client & CapabilityTargetVideoH264) != 0 && (producer & CapabilityTargetVideoRemoteRender) != 0) {
		active |= TargetVideoCapabilityPair;
	}
	if ((client & CapabilityUpdate) != 0 && (producer & CapabilityUpdate) != 0) {
		active |= CapabilityUpdate;
	}
	return initialize_capability_session(client_advertised, producer_advertised, active, state);
}

CapabilityUpdateOutcome apply_capability_update(CapabilitySessionState& state,
	CapabilityPeerRole sender,
	const CapabilityUpdatePayload& update) noexcept
{
	if (!state.m_initialized || (sender != CapabilityPeerRole::Client && sender != CapabilityPeerRole::Producer)) {
		return outcome(CapabilityUpdateDisposition::NotNegotiated, ValidationError::CapabilityNotNegotiated);
	}
	if (!state.m_update_negotiated_initially) {
		return outcome(CapabilityUpdateDisposition::NotNegotiated, ValidationError::CapabilityNotNegotiated);
	}
	if (const auto error = validate_capability_update_payload(update); error != ValidationError::None) {
		return outcome(CapabilityUpdateDisposition::Rejected, error);
	}

	auto& peer = sender == CapabilityPeerRole::Client ? state.m_client : state.m_producer;
	if (peer.has_last_update) {
		if (update.capability_generation < peer.generation) {
			return outcome(CapabilityUpdateDisposition::Obsolete);
		}
		if (update.capability_generation == peer.generation) {
			return updates_equal(update, peer.last_update)
					   ? outcome(CapabilityUpdateDisposition::DuplicateIdentical)
					   : outcome(CapabilityUpdateDisposition::SameGenerationDifferent,
							 ValidationError::InvalidStateTransition);
		}
	}
	if (!peer.update_channel_open) {
		return outcome(CapabilityUpdateDisposition::RequiresNewSession, ValidationError::InvalidStateTransition);
	}

	const auto advertised = effective_capabilities(update.advertised_capabilities);
	const auto declared_active = effective_capabilities(update.active_capabilities);
	const auto owner_mask = owned_capabilities(sender);
	if ((advertised & ~owner_mask) != 0) {
		return outcome(CapabilityUpdateDisposition::Rejected, ValidationError::CapabilityNotNegotiated);
	}
	if ((advertised & ~peer.advertised) != 0 || (declared_active & ~peer.declared_active) != 0) {
		return outcome(CapabilityUpdateDisposition::RequiresNewSession, ValidationError::InvalidStateTransition);
	}
	if (const auto error = validate_active_shape(declared_active); error != ValidationError::None) {
		return outcome(CapabilityUpdateDisposition::Rejected, error);
	}

	const auto owned_comm_role =
		sender == CapabilityPeerRole::Client ? CapabilityCommViewLocalAssets : CapabilityCommViewAuthoritativeSource;
	const auto owned_video_role =
		sender == CapabilityPeerRole::Client ? CapabilityTargetVideoH264 : CapabilityTargetVideoRemoteRender;
	if (((advertised & owned_comm_role) == 0 && (declared_active & CommViewCapabilityPair) != 0) ||
		((advertised & owned_video_role) == 0 && (declared_active & TargetVideoCapabilityPair) != 0) ||
		((advertised & CapabilityUpdate) == 0 && (declared_active & CapabilityUpdate) != 0)) {
		return outcome(CapabilityUpdateDisposition::Rejected, ValidationError::CapabilityNotNegotiated);
	}

	const auto old_active = state.m_active;
	peer.advertised = advertised;
	peer.declared_active = declared_active;
	peer.generation = update.capability_generation;
	peer.last_update = update;
	peer.has_last_update = true;
	if ((advertised & CapabilityUpdate) == 0 || (declared_active & CapabilityUpdate) == 0) {
		peer.update_channel_open = false;
	}
	state.m_active = state.m_client.declared_active & state.m_producer.declared_active;
	const auto removed = old_active & ~state.m_active;
	return outcome(CapabilityUpdateDisposition::Applied, ValidationError::None, removed);
}

} // namespace telemetry::protocol
