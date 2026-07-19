#include "telemetry/phase1_state_image.h"

#include "telemetry/protocol/packet_writer.h"
#include "telemetry/protocol/telemetry_business_state_validation.h"

#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace telemetry::detail {
namespace {

using protocol::ByteView;
using protocol::MutableByteView;
using protocol::PacketWriter;
using protocol::RecordType;
using protocol::StateAtom;
using protocol::StateAtomKey;

constexpr std::size_t SessionStatePayloadBytes = 72U;
constexpr std::size_t MissionStatePayloadBytes = 28U;
constexpr std::size_t EntityLifecyclePayloadBytes = 30U;
constexpr std::size_t FlightStatePayloadBytes = 84U;

std::uint64_t elapsed_nanoseconds(std::chrono::steady_clock::time_point started,
	std::chrono::steady_clock::time_point ended) noexcept
{
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(ended - started).count());
}

void reset_atom(StateAtom& atom) noexcept
{
	// Keep every vector allocation made by Phase1StateImagePool::provision().
	// Clearing is deliberately used instead of assigning a fresh StateAtom.
	atom.key.record_type = 0U;
	atom.key.identity.clear();
	atom.record_version = 1U;
	atom.lifecycle = protocol::StateRecordLifecycle::UpsertOnly;
	atom.has_cascade_owner = false;
	atom.cascade_owner.record_type = 0U;
	atom.cascade_owner.identity.clear();
	atom.value.clear();
}

bool is_no_player_capture(const CaptureResult& capture) noexcept
{
	return capture.status == CaptureStatus::NoPlayer && capture.reason >= CaptureReason::NotInMission &&
		capture.reason <= CaptureReason::MissingPlayerShip;
}

bool is_invalid_source_capture(const CaptureResult& capture) noexcept
{
	return capture.status == CaptureStatus::InvalidSource && capture.reason >= CaptureReason::WrongObjectType &&
		capture.reason < CaptureReason::Count;
}

bool is_valid_player_capture(const CaptureResult& capture) noexcept
{
	return capture.status == CaptureStatus::Valid && capture.reason == CaptureReason::None;
}

bool write_entity_key(std::uint64_t entity_id, StateAtomKey& key)
{
	std::array<std::uint8_t, sizeof(entity_id)> bytes{};
	PacketWriter writer(MutableByteView{bytes.data(), bytes.size()});
	if (!writer.write_u64(entity_id) || writer.size() != bytes.size()) {
		return false;
	}
	key.identity.assign(bytes.begin(), bytes.end());
	return true;
}

bool assign_payload(const PacketWriter& writer, StateAtom& atom)
{
	if (!writer.ok()) {
		return false;
	}
	const auto payload = writer.written();
	atom.value.assign(payload.data, payload.data + payload.size);
	return true;
}

bool make_session_atom(const Phase1StateImageInput& input, bool has_player, StateAtom& atom)
{
	reset_atom(atom);
	std::array<std::uint8_t, SessionStatePayloadBytes> bytes{};
	PacketWriter writer(MutableByteView{bytes.data(), bytes.size()});
	const auto presence = has_player ? protocol::SessionStatePresenceFlagObservedPlayer : 0U;
	const bool written = writer.write_u64(presence) && writer.write_u64(input.producer_id) &&
		writer.write_u64(input.mission.producer_sample_time_us) &&
		writer.write_u8(static_cast<std::uint8_t>(protocol::AuthorityMode::Solo)) &&
		writer.write_u8(static_cast<std::uint8_t>(protocol::VisibilityMode::Cockpit)) &&
		writer.write_u8(static_cast<std::uint8_t>(input.session_phase)) && writer.write_u8(0U) &&
		writer.write_u32(input.negotiated_capability_generation) && writer.write_u64(protocol::CapabilityNone) &&
		writer.write_u64(protocol::StateDomainCoverageBitPlayerKinematics) && writer.write_u64(0U) &&
		writer.write_u64(0U) && (!has_player || writer.write_u64(input.player.entity_id));
	if (!written) {
		return false;
	}
	atom.key.record_type = static_cast<std::uint16_t>(RecordType::SessionState);
	return assign_payload(writer, atom);
}

bool make_mission_atom(const Phase1StateImageInput& input, StateAtom& atom)
{
	reset_atom(atom);
	std::array<std::uint8_t, MissionStatePayloadBytes> bytes{};
	PacketWriter writer(MutableByteView{bytes.data(), bytes.size()});
	const bool written = writer.write_u64(protocol::MissionStatePresenceFlagNone) &&
		writer.write_u32(input.mission.mission_generation) &&
		writer.write_u8(static_cast<std::uint8_t>(input.mission.phase)) && writer.write_bool8(input.mission.paused) &&
		writer.write_zeroes(2U) && writer.write_f32(input.mission.time_compression) &&
		writer.write_u64(input.mission.producer_sample_time_us);
	if (!written) {
		return false;
	}
	atom.key.record_type = static_cast<std::uint16_t>(RecordType::MissionState);
	return assign_payload(writer, atom);
}

bool make_lifecycle_atom(const Phase1StateImageInput& input, StateAtom& atom)
{
	reset_atom(atom);
	std::array<std::uint8_t, EntityLifecyclePayloadBytes> bytes{};
	PacketWriter writer(MutableByteView{bytes.data(), bytes.size()});
	const bool written = writer.write_u64(input.player.entity_id) &&
		writer.write_u64(protocol::EntityLifecyclePresenceFlagNone) &&
		writer.write_u64(input.player.value.producer_sample_time_us) &&
		writer.write_u8(static_cast<std::uint8_t>(protocol::ObjectType::Ship)) &&
		writer.write_u8(static_cast<std::uint8_t>(protocol::LifecyclePhase::Active)) &&
		writer.write_u32(protocol::EntityLifecycleFlagNone);
	if (!written || !write_entity_key(input.player.entity_id, atom.key)) {
		return false;
	}
	atom.key.record_type = static_cast<std::uint16_t>(RecordType::EntityLifecycle);
	atom.lifecycle = protocol::StateRecordLifecycle::ExplicitCreateDelete;
	return assign_payload(writer, atom);
}

bool make_flight_atom(const Phase1StateImageInput& input,
	const StateAtomKey& lifecycle_key,
	StateAtom& atom)
{
	reset_atom(atom);
	const auto& value = input.player.value;
	std::array<std::uint8_t, FlightStatePayloadBytes> bytes{};
	PacketWriter writer(MutableByteView{bytes.data(), bytes.size()});
	const bool written = writer.write_u64(input.player.entity_id) &&
		writer.write_u64(protocol::FlightStatePresenceFlagNone) && writer.write_u64(value.producer_sample_time_us) &&
		writer.write_f32(value.position_world.x) && writer.write_f32(value.position_world.y) &&
		writer.write_f32(value.position_world.z) && writer.write_f32(value.orientation_local_to_world.w) &&
		writer.write_f32(value.orientation_local_to_world.x) && writer.write_f32(value.orientation_local_to_world.y) &&
		writer.write_f32(value.orientation_local_to_world.z) && writer.write_f32(value.velocity_world.x) &&
		writer.write_f32(value.velocity_world.y) && writer.write_f32(value.velocity_world.z) &&
		writer.write_f32(value.rotational_velocity_local.x) && writer.write_f32(value.rotational_velocity_local.y) &&
		writer.write_f32(value.rotational_velocity_local.z) && writer.write_f32(value.radius) &&
		writer.write_u32(value.physics_mode_flags);
	if (!written || !write_entity_key(input.player.entity_id, atom.key)) {
		return false;
	}
	atom.key.record_type = static_cast<std::uint16_t>(RecordType::FlightState);
	atom.has_cascade_owner = true;
	atom.cascade_owner = lifecycle_key;
	return assign_payload(writer, atom);
}

Phase1StateImageBuildStatus map_state_image_result(protocol::StateImageResult result) noexcept
{
	switch (result) {
	case protocol::StateImageResult::Created:
		return Phase1StateImageBuildStatus::Created;
	case protocol::StateImageResult::AllocationFailed:
		return Phase1StateImageBuildStatus::AllocationFailed;
	case protocol::StateImageResult::InvalidRecord:
	case protocol::StateImageResult::DuplicateKey:
	case protocol::StateImageResult::SizeLimitExceeded:
	default:
		return Phase1StateImageBuildStatus::InvalidInput;
	}
}

bool valid_input(const Phase1StateImageInput& input, bool has_player) noexcept
{
	return input.producer_id != 0U && input.negotiated_capability_generation != 0U &&
		input.mission.mission_generation != 0U &&
		static_cast<std::uint8_t>(input.session_phase) <= static_cast<std::uint8_t>(protocol::SessionPhase::Ending) &&
		static_cast<std::uint8_t>(input.mission.phase) <= static_cast<std::uint8_t>(protocol::MissionPhase::Ended) &&
		std::isfinite(input.mission.time_compression) && input.mission.time_compression >= 0.0F &&
		input.mission.time_compression <= 64.0F &&
		(has_player || is_no_player_capture(input.player_capture) || is_invalid_source_capture(input.player_capture)) &&
		(!has_player || (input.player.entity_id != 0U &&
			input.player.value.producer_sample_time_us == input.mission.producer_sample_time_us));
}

bool fill_records(const Phase1StateImageInput& input, bool has_player, std::vector<StateAtom>& records) noexcept
{
	if (records.size() != (has_player ? 4U : 2U)) {
		return false;
	}
	if (!make_session_atom(input, has_player, records[0]) || !make_mission_atom(input, records[1])) {
		return false;
	}
	if (!has_player) {
		return true;
	}
	return make_lifecycle_atom(input, records[2]) && make_flight_atom(input, records[2].key, records[3]);
}

Phase1StateImageBuildStatus validate_and_publish(const std::shared_ptr<const std::vector<StateAtom>>& records,
	protocol::StateImage& image,
	Phase1StateImageBuildTiming* timing) noexcept
{
	const auto publish_started = timing != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	protocol::StateImage candidate;
	protocol::StateImageInvalidRecordReason invalid_record_reason = protocol::StateImageInvalidRecordReason::None;
	const auto adopt_started = timing != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	const auto created = protocol::StateImage::adopt_preallocated(records, candidate, invalid_record_reason);
	if (timing != nullptr) timing->adopt_preallocated_ns = elapsed_nanoseconds(adopt_started, std::chrono::steady_clock::now());
	if (const auto result = map_state_image_result(created); result != Phase1StateImageBuildStatus::Created) {
		return result;
	}

	protocol::BusinessStateValidationContext context{};
	context.protocol_minor = protocol::VersionMinorV1_1;
	context.required_manifest_id = 0U;
	protocol::BusinessStateImageValidator validator(context);
	const auto semantic_started = timing != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	if (validator.validate(candidate) != protocol::ValidationError::None) {
		return Phase1StateImageBuildStatus::InvalidInput;
	}
	if (timing != nullptr) timing->semantic_validate_ns = elapsed_nanoseconds(semantic_started, std::chrono::steady_clock::now());
	image = std::move(candidate);
	if (timing != nullptr) timing->publish_validate_ns = elapsed_nanoseconds(publish_started, std::chrono::steady_clock::now());
	return Phase1StateImageBuildStatus::Created;
}

} // namespace

bool Phase1StateImagePool::provision_slot(Slot& slot, bool has_player, std::uint64_t& allocation_count) noexcept
{
	const std::array<std::size_t, 4U> payload_sizes = {
		SessionStatePayloadBytes, MissionStatePayloadBytes, EntityLifecyclePayloadBytes, FlightStatePayloadBytes};
	const auto record_count = has_player ? 4U : 2U;
	try {
		auto records = std::make_shared<std::vector<StateAtom>>();
		++allocation_count;
		records->reserve(record_count);
		++allocation_count;
		for (std::size_t index = 0U; index < record_count; ++index) {
			records->emplace_back();
			auto& atom = records->back();
			atom.value.reserve(payload_sizes[index]);
			++allocation_count;
			if (index >= 2U) {
				atom.key.identity.reserve(sizeof(std::uint64_t));
				++allocation_count;
			}
			if (index == 3U) {
				atom.cascade_owner.identity.reserve(sizeof(std::uint64_t));
				++allocation_count;
			}
		}
		slot.records = std::move(records);
		return true;
	} catch (const std::bad_alloc&) {
		return false;
	}
}

bool Phase1StateImagePool::provision() noexcept
{
	reset();
	for (auto& slot : m_without_player) {
		if (!provision_slot(slot, false, m_successful_allocation_count)) {
			reset();
			return false;
		}
	}
	for (auto& slot : m_with_player) {
		if (!provision_slot(slot, true, m_successful_allocation_count)) {
			reset();
			return false;
		}
	}
	m_ready = true;
	return true;
}

void Phase1StateImagePool::reset() noexcept
{
	for (auto& slot : m_without_player) {
		slot.records.reset();
	}
	for (auto& slot : m_with_player) {
		slot.records.reset();
	}
	m_ready = false;
}

std::size_t Phase1StateImagePool::owned_backing_bytes() const noexcept
{
	std::size_t total = 0U;
	const auto add = [&total](std::size_t bytes) noexcept {
		if (bytes > std::numeric_limits<std::size_t>::max() - total) return false;
		total += bytes;
		return true;
	};
	const auto accumulate = [&add](const Slot& slot) noexcept {
		if (!slot.records) return true;
		if (slot.records->capacity() > std::numeric_limits<std::size_t>::max() / sizeof(StateAtom) ||
			!add(slot.records->capacity() * sizeof(StateAtom))) return false;
		for (const auto& atom : *slot.records) {
			if (!add(atom.key.identity.capacity()) || !add(atom.value.capacity()) ||
				!add(atom.cascade_owner.identity.capacity())) return false;
		}
		return true;
	};
	for (const auto& slot : m_without_player) {
		if (!accumulate(slot)) return 0U;
	}
	for (const auto& slot : m_with_player) {
		if (!accumulate(slot)) return 0U;
	}
	return total;
}

Phase1StateImagePool::Slot* Phase1StateImagePool::acquire(bool has_player) noexcept
{
	if (!m_ready) {
		return nullptr;
	}
	auto& slots = has_player ? m_with_player : m_without_player;
	for (auto& slot : slots) {
		if (slot.records && slot.records.use_count() == 1L) {
			return &slot;
		}
	}
	return nullptr;
}

Phase1StateImageBuildStatus build_phase1_state_image(const Phase1StateImageInput& input,
	protocol::StateImage& image) noexcept
{
	const auto has_player = is_valid_player_capture(input.player_capture);
	if (!valid_input(input, has_player)) {
		return Phase1StateImageBuildStatus::InvalidInput;
	}

	try {
		std::vector<StateAtom> records;
		records.reserve(has_player ? 4U : 2U);

		records.resize(has_player ? 4U : 2U);
		if (!fill_records(input, has_player, records)) return Phase1StateImageBuildStatus::InvalidInput;

		protocol::StateImage candidate;
		const auto created = protocol::StateImage::create(std::move(records), candidate);
		if (const auto result = map_state_image_result(created); result != Phase1StateImageBuildStatus::Created) {
			return result;
		}

		protocol::BusinessStateValidationContext context{};
		context.protocol_minor = protocol::VersionMinorV1_1;
		context.required_manifest_id = 0U;
		protocol::BusinessStateImageValidator validator(context);
		if (validator.validate(candidate) != protocol::ValidationError::None) {
			return Phase1StateImageBuildStatus::InvalidInput;
		}

		image = std::move(candidate);
		return Phase1StateImageBuildStatus::Created;
	} catch (const std::bad_alloc&) {
		return Phase1StateImageBuildStatus::AllocationFailed;
	}
}

Phase1StateImageBuildStatus build_phase1_state_image_preallocated(const Phase1StateImageInput& input,
	Phase1StateImagePool& pool,
	protocol::StateImage& image,
	Phase1StateImageBuildTiming* timing) noexcept
{
	const auto has_player = is_valid_player_capture(input.player_capture);
	if (!valid_input(input, has_player)) {
		return Phase1StateImageBuildStatus::InvalidInput;
	}
	auto* slot = pool.acquire(has_player);
	if (slot == nullptr) {
		return Phase1StateImageBuildStatus::AllocationFailed;
	}
	try {
		const auto fill_started = timing != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
		if (!fill_records(input, has_player, *slot->records)) {
			return Phase1StateImageBuildStatus::InvalidInput;
		}
		if (timing != nullptr) timing->fill_records_ns = elapsed_nanoseconds(fill_started, std::chrono::steady_clock::now());
		std::shared_ptr<const std::vector<StateAtom>> records = slot->records;
		return validate_and_publish(records, image, timing);
	} catch (const std::bad_alloc&) {
		// A provisioned slot has enough capacity for its fixed schema. This is
		// nevertheless fail-closed if a standard-library implementation reports
		// an allocation failure while assigning a payload.
		return Phase1StateImageBuildStatus::AllocationFailed;
	}
}

} // namespace telemetry::detail
