#include "telemetry/session_controller.h"

#include "telemetry/phase1_snapshot_slot.h"
#include "telemetry/phase1_state_image.h"

#include "telemetry/protocol/telemetry_control_messages.h"
#include "telemetry/protocol/telemetry_crc32.h"
#include "telemetry/protocol/telemetry_datagram.h"
#include "telemetry/protocol/telemetry_reliability_messages.h"
#include "telemetry/protocol/telemetry_reliable_window.h"
#include "telemetry/startup_budget.h"
#include "telemetry/transport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace detail = telemetry::detail;
namespace protocol = telemetry::protocol;

template <typename Controller, typename = void>
struct has_p8_3_delta_egress_seams : std::false_type {
};

template <typename Controller>
struct has_p8_3_delta_egress_seams<Controller,
	std::void_t<decltype(std::declval<Controller&>().queue_cumulative_delta(
		std::declval<std::size_t>(), std::declval<std::uint64_t>())),
		decltype(std::declval<Controller&>().service_delta_egress(
			std::declval<std::size_t>(), std::declval<std::uint64_t>()))>> : std::true_type {
};

template <typename Container>
protocol::ByteView view(const Container& bytes)
{
	return {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
}

template <typename Container>
protocol::MutableByteView mutable_view(Container& bytes)
{
	return {reinterpret_cast<std::uint8_t*>(bytes.data()), bytes.size()};
}

protocol::EndpointKey endpoint(std::uint8_t last_octet = 2U, std::uint16_t port = 42043U)
{
	return protocol::EndpointKey::from_ipv4({127U, 0U, 0U, last_octet}, port);
}

struct ScriptedRandomSource final : detail::RandomSource {
	struct Draw {
		bool succeeds;
		std::uint64_t value;
	};
	std::vector<Draw> values;
	std::size_t calls = 0U;
	ScriptedRandomSource() = default;
	ScriptedRandomSource(std::initializer_list<Draw> script) : values(script) {}

	bool next_u64(std::uint64_t& output) noexcept override
	{
		++calls;
		if (calls > values.size()) {
			return false;
		}
		const auto draw = values[calls - 1U];
		output = draw.value;
		return draw.succeeds;
	}
};

struct IdentityHarness {
	ScriptedRandomSource random;
	detail::SessionIdRegistry registry;
	detail::SessionIdAllocator allocator;

	IdentityHarness(std::initializer_list<ScriptedRandomSource::Draw> draws)
		: random(draws), allocator(random, registry)
	{
		EXPECT_TRUE(registry.allocate_storage());
	}
	explicit IdentityHarness(std::vector<ScriptedRandomSource::Draw> draws)
		: allocator(random, registry)
	{
		random.values = std::move(draws);
		EXPECT_TRUE(registry.allocate_storage());
	}
};

struct StageRecorder final : detail::SessionControllerObserver {
	std::vector<detail::SessionIngressStage> stages;

	void stage_reached(detail::SessionIngressStage stage) noexcept override
	{
		stages.push_back(stage);
	}
};

struct EncodedDatagram {
	std::vector<std::uint8_t> bytes;
	protocol::TelemetryDatagramHeader header;
};

EncodedDatagram encode_datagram(protocol::TelemetryDatagramHeader header, protocol::ByteView payload)
{
	EncodedDatagram result;
	result.bytes.resize(protocol::HeaderSizeV1 + payload.size);
	std::size_t written = 0U;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::encode_datagram(header,
			protocol::ProtocolMinorRange{header.version_minor, header.version_minor},
			payload,
			mutable_view(result.bytes),
			written));
	EXPECT_EQ(result.bytes.size(), written);
	protocol::DatagramView decoded;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::decode_and_validate_datagram(view(result.bytes),
			protocol::ProtocolMinorRange{header.version_minor, header.version_minor},
			decoded));
	result.header = decoded.header;
	return result;
}

EncodedDatagram hello(std::uint64_t nonce,
	std::uint8_t min_minor = protocol::VersionMinorV1_1,
	std::uint8_t max_minor = protocol::VersionMinorV1_1,
	std::uint32_t packet_sequence = 1U)
{
	protocol::HelloPayload payload;
	payload.client_nonce = nonce;
	payload.client_send_t0_us = 1'000'000U;
	payload.min_major = protocol::VersionMajor;
	payload.max_major = protocol::VersionMajor;
	payload.min_minor = min_minor;
	payload.max_minor = max_minor;
	payload.requested_visibility_mode = protocol::VisibilityMode::Cockpit;
	payload.advertised_capabilities = 0U;
	payload.requested_heartbeat_ms = 1000U;

	std::array<std::uint8_t, protocol::HelloPayloadPrefixSize> encoded_payload{};
	std::size_t written = 0U;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::encode_hello_payload(payload, mutable_view(encoded_payload), written));
	EXPECT_EQ(encoded_payload.size(), written);

	protocol::TelemetryDatagramHeader header;
	header.version_minor = max_minor;
	header.message_type = protocol::MessageType::Hello;
	header.session_id = 0U;
	header.packet_sequence = packet_sequence;
	header.sent_time_us = payload.client_send_t0_us;
	header.message_id = 1U;
	header.message_size = static_cast<std::uint32_t>(encoded_payload.size());
	header.message_crc32 = protocol::crc32_iso_hdlc(view(encoded_payload));
	return encode_datagram(header, view(encoded_payload));
}

void put_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
	for (std::size_t i = 0U; i < 4U; ++i) {
		bytes[offset + i] = static_cast<std::uint8_t>(value >> (8U * i));
	}
}

void reseal_single_fragment(std::vector<std::uint8_t>& bytes)
{
	ASSERT_GE(bytes.size(), protocol::HeaderSizeV1);
	const protocol::ByteView payload{bytes.data() + protocol::HeaderSizeV1,
		bytes.size() - protocol::HeaderSizeV1};
	put_u32(bytes, 60U, protocol::crc32_iso_hdlc(payload));
	put_u32(bytes, 64U, 0U);
	put_u32(bytes, 64U, protocol::crc32_iso_hdlc(view(bytes)));
}

struct DecodedOutput {
	protocol::DatagramView datagram;
	std::vector<std::uint8_t> storage;
};

EncodedDatagram applied_welcome_ack(const DecodedOutput& welcome);
protocol::AckPayload welcome_ack_payload(const DecodedOutput& welcome);
EncodedDatagram encode_welcome_ack(const DecodedOutput& welcome,
	const protocol::AckPayload& payload,
	std::uint64_t session_id = 0U);

DecodedOutput pop_output(detail::SessionController& controller)
{
	detail::SessionControllerOutput output;
	EXPECT_TRUE(controller.pop_output(output));
	DecodedOutput decoded;
	decoded.storage.assign(output.bytes.begin(), output.bytes.begin() + static_cast<std::ptrdiff_t>(output.size));
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::decode_and_validate_datagram(view(decoded.storage),
			protocol::ProtocolMinorRange{decoded.storage[5], decoded.storage[5]},
			decoded.datagram));
	return decoded;
}

protocol::WelcomePayload decode_welcome(const DecodedOutput& output)
{
	protocol::WelcomePayload payload;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::decode_welcome_payload(output.datagram.payload, payload));
	return payload;
}

detail::SessionControllerConfig config(std::size_t max_clients = 1U)
{
	detail::SessionControllerConfig result;
	result.max_clients = max_clients;
	result.producer_id = 0x1020304050607080ULL;
	result.mission_heartbeat_ms = 500U;
	result.idle_heartbeat_ms = 1000U;
	result.security.enabled = true;
	result.security.port = 42042U;
	result.security.bind_mode = protocol::NetworkBindMode::LoopbackOnly;
	result.security.resources.max_clients = max_clients;
	result.security.resources.global_state_reassembly_bytes =
		max_clients * protocol::MaxStateReassemblyBytesPerClient;
	return result;
}

detail::SessionController make_controller(detail::SessionIdAllocator& ids,
	StageRecorder* observer = nullptr,
	std::size_t max_clients = 1U,
	std::uint8_t keyframe_seconds = 2U)
{
	struct PacketSequences final : detail::RandomSource {
		std::uint64_t next = 0x10203040U;
		bool next_u64(std::uint64_t& output) noexcept override
		{
			output = next++;
			return true;
		}
	};
	static PacketSequences packet_sequences;
	detail::SessionController controller;
	auto controller_config = config(max_clients);
	controller_config.keyframe_seconds = keyframe_seconds;
	EXPECT_EQ(detail::SessionControllerConfigureResult::Ready,
		detail::SessionController::configure(
			controller_config, ids, packet_sequences, 0U, observer, controller));
	return controller;
}

template <typename T, typename = void>
struct has_physical_wp06_capacity_fields : std::false_type {};

template <typename T>
struct has_physical_wp06_capacity_fields<T,
	std::void_t<decltype(std::declval<T>().client_slot_bytes),
		decltype(std::declval<T>().rate_limiter_bytes),
		decltype(std::declval<T>().handshake_cache_bytes),
		decltype(std::declval<T>().preproof_ledger_bytes),
		decltype(std::declval<T>().output_queue_bytes)>> : std::true_type {};

template <typename T, typename = void>
struct has_phase1_replication_capacity_fields : std::false_type {};

template <typename T>
struct has_phase1_replication_capacity_fields<T,
	std::void_t<decltype(std::declval<T>().baseline_slots), decltype(std::declval<T>().delta_slots)>>
	: std::true_type {};

template <typename Budget, typename Capacity>
void expect_phase1_replication_capacity(const Budget& budget, const Capacity& owned)
{
	if constexpr (has_phase1_replication_capacity_fields<Capacity>::value) {
		EXPECT_EQ(budget.baseline_slot_count, owned.baseline_slots);
		EXPECT_EQ(budget.delta_slot_count, owned.delta_slots);
	}
}

template <typename T>
void expect_physical_wp06_capacity(const T& owned)
{
	if constexpr (has_physical_wp06_capacity_fields<T>::value) {
		EXPECT_EQ(owned.client_slots * sizeof(detail::SessionControllerSlot), owned.client_slot_bytes);
		EXPECT_EQ(sizeof(protocol::ProtocolRateLimiter), owned.rate_limiter_bytes);
		EXPECT_GT(owned.handshake_cache_bytes, 0U);
		EXPECT_GT(owned.preproof_ledger_bytes, 0U);
		EXPECT_GE(owned.output_queue_bytes, protocol::MaxDatagramSize);
	}
}

template <typename T, typename = void>
struct accepts_phase0_packet_sequence_source : std::false_type {};

template <typename T>
struct accepts_phase0_packet_sequence_source<T,
	std::void_t<decltype(T::configure(std::declval<const detail::SessionControllerConfig&>(),
		std::declval<detail::SessionIdAllocator&>(),
		std::declval<detail::RandomSource&>(),
		std::declval<std::uint64_t>(),
		std::declval<detail::SessionControllerObserver*>(),
		std::declval<T&>()))>> : std::true_type {};

template <typename T, typename = void>
struct accepts_legacy_packet_sequence_fallback : std::false_type {};

template <typename T>
struct accepts_legacy_packet_sequence_fallback<T,
	std::void_t<decltype(T::configure(std::declval<const detail::SessionControllerConfig&>(),
		std::declval<detail::SessionIdAllocator&>(),
		std::declval<std::uint64_t>(),
		std::declval<detail::SessionControllerObserver*>(),
		std::declval<T&>()))>> : std::true_type {};

template <typename T, typename = void>
struct has_integrated_phase1_snapshot_egress : std::false_type {};

template <typename T>
struct has_integrated_phase1_snapshot_egress<T,
	std::void_t<decltype(std::declval<T&>().begin_initial_snapshot(std::declval<std::size_t>(),
		std::declval<const protocol::StateImage&>(), std::declval<std::uint64_t>())),
		decltype(std::declval<T&>().service_initial_snapshot_egress(std::declval<std::size_t>(),
			std::declval<std::uint64_t>())),
		decltype(std::declval<const T&>().snapshot_progress(std::declval<std::size_t>())),
		decltype(std::declval<const T&>().session_state_dirty(std::declval<std::size_t>())),
		decltype(std::declval<T&>().consume_session_state_dirty(std::declval<std::size_t>()))>> : std::true_type {};

detail::Phase1StateImageInput phase1_integration_input()
{
	detail::Phase1StateImageInput input{};
	input.producer_id = 0x1020304050607080ULL;
	input.negotiated_capability_generation = 3U;
	input.mission.producer_sample_time_us = 555'000U;
	input.mission.mission_generation = 7U;
	input.mission.phase = protocol::MissionPhase::Active;
	input.mission.time_compression = 1.0F;
	input.player_capture = {detail::CaptureStatus::Valid, detail::CaptureReason::None};
	input.player.entity_id = 42U;
	input.player.value.producer_sample_time_us = input.mission.producer_sample_time_us;
	input.player.value.position_world = {1.0F, 2.0F, 3.0F};
	input.player.value.orientation_local_to_world = {1.0F, 0.0F, 0.0F, 0.0F};
	input.player.value.radius = 1.0F;
	return input;
}

protocol::StateImage phase1_integration_image()
{
	protocol::StateImage image;
	EXPECT_EQ(detail::Phase1StateImageBuildStatus::Created,
		detail::build_phase1_state_image(phase1_integration_input(), image));
	return image;
}

protocol::StateImage phase1_integration_image_at(float player_x, std::uint64_t sample_time_us)
{
	auto input = phase1_integration_input();
	input.mission.producer_sample_time_us = sample_time_us;
	input.player.value.producer_sample_time_us = sample_time_us;
	input.player.value.position_world.x = player_x;
	protocol::StateImage image;
	EXPECT_EQ(detail::Phase1StateImageBuildStatus::Created, detail::build_phase1_state_image(input, image));
	return image;
}

void activate_phase1_live_baseline(detail::SessionController& controller, std::uint64_t nonce,
	std::uint64_t start_us = 1'000U)
{
	const auto request = hello(nonce);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(endpoint(), view(request.bytes), start_us, 7U, true).disposition);
	const auto welcome = pop_output(controller);
	const auto welcome_ack = applied_welcome_ack(welcome);
	ASSERT_EQ(detail::SessionIngressDisposition::WelcomeProofApplied,
		controller.ingest(endpoint(), view(welcome_ack.bytes), start_us + 1'000U, 7U, true).disposition);
	(void)pop_output(controller); // SESSION_BEGIN
	ASSERT_TRUE(controller.begin_initial_snapshot(0U, phase1_integration_image(), start_us + 2'000U));
	ASSERT_EQ(1U, controller.service_initial_snapshot_egress(1U, start_us + 2'001U));
	const auto snapshot = pop_output(controller);
	ASSERT_EQ(protocol::MessageType::FullSnapshot, snapshot.datagram.header.message_type);
	auto snapshot_ack = welcome_ack_payload(snapshot);
	snapshot_ack.target_message_type = protocol::MessageType::FullSnapshot;
	snapshot_ack.ack_flags = protocol::KnownAckFlags;
	const auto encoded_snapshot_ack = encode_welcome_ack(snapshot, snapshot_ack);
	ASSERT_NE(detail::SessionIngressDisposition::Dropped,
		controller.ingest(endpoint(), view(encoded_snapshot_ack.bytes), start_us + 3'000U, 7U, true).disposition);
	ASSERT_EQ(detail::Phase1SnapshotProgress::Live, controller.snapshot_progress(0U));
}

template <typename T, typename = void>
struct has_complete_wp06_budget_fields : std::false_type {};

template <typename T>
struct has_complete_wp06_budget_fields<T,
	std::void_t<decltype(std::declval<T>().rate_limiter_bytes),
		decltype(std::declval<T>().handshake_cache_bytes),
		decltype(std::declval<T>().preproof_ledger_bytes),
		decltype(std::declval<T>().output_queue_bytes)>> : std::true_type {};

template <typename T>
std::size_t wp06_runtime_overhead_bytes(const T& budget)
{
	if constexpr (has_complete_wp06_budget_fields<T>::value) {
		return budget.rate_limiter_bytes + budget.handshake_cache_bytes + budget.preproof_ledger_bytes +
			budget.output_queue_bytes;
	}
	return 0U;
}

TEST(TelemetryWp06BudgetContract, PricesConcreteSlotsReassemblyReliableRetentionAndWp08Backing)
{
	for (const auto clients : {1U, 4U}) {
		const auto wp04 = detail::calculate_wp04_startup_budget(detail::make_wp03_known_budget_request(clients));
		ASSERT_EQ(detail::StartupBudgetError::None, wp04.error);
		const auto wp06 = detail::calculate_wp06_startup_budget(wp04, clients);
		ASSERT_EQ(detail::StartupBudgetError::None, wp06.error);
		EXPECT_EQ(clients * detail::Wp06ClientSlotStorageBytes, wp06.client_slot_bytes);
		EXPECT_EQ(clients * protocol::MaxStateReassemblyBytesPerClient, wp06.reassembly_bytes);
		EXPECT_EQ(clients * detail::Wp06ReliableRetentionBytesPerClient, wp06.reliable_retention_projection_bytes);
		const auto projected_wp03_storage = wp04.reassembly_bytes + wp04.reliable_retention_projection_bytes;
		ASSERT_GE(wp04.known_bytes, projected_wp03_storage);
		const auto expected_known = wp04.known_bytes - projected_wp03_storage + wp06.client_slot_bytes +
			wp06.reassembly_bytes + wp06.reliable_retention_projection_bytes + wp06_runtime_overhead_bytes(wp06) +
			wp06.state_image_pool_bytes + wp06.snapshot_egress_heap_bytes + wp06.delta_egress_heap_bytes +
			wp06.delta_scratch_heap_bytes;
		EXPECT_EQ(expected_known, wp06.known_bytes)
			<< "The budget replaces WP03 projections with owned storage and prices all P8 backing exactly once.";
		EXPECT_EQ(wp06.session_id_registry_bytes + wp06.transport_buffer_bytes + wp06.client_slot_bytes +
			wp06.reassembly_bytes + wp06.reliable_retention_projection_bytes + wp06_runtime_overhead_bytes(wp06) +
			wp06.state_image_pool_bytes + wp06.snapshot_egress_heap_bytes + wp06.delta_egress_heap_bytes +
			wp06.delta_scratch_heap_bytes,
			wp06.known_bytes);
		EXPECT_EQ(static_cast<std::uint64_t>(wp06.known_bytes), wp06.metric_known_bytes);
		EXPECT_FALSE(detail::startup_budget_category_is_deferred(
			wp06, detail::DeferredStartupBudgetCategory::ClientSlotStorage));
		EXPECT_FALSE(detail::startup_budget_category_is_deferred(
			wp06, detail::DeferredStartupBudgetCategory::StateReassemblyStorage));
		EXPECT_FALSE(detail::startup_budget_category_is_deferred(
			wp06, detail::DeferredStartupBudgetCategory::ReliableWindowStorage));
		EXPECT_FALSE(detail::startup_budget_category_is_deferred(
			wp06, detail::DeferredStartupBudgetCategory::BaselineStorage));
		EXPECT_FALSE(detail::startup_budget_category_is_deferred(
			wp06, detail::DeferredStartupBudgetCategory::DeltaStorage));
		EXPECT_FALSE(detail::startup_budget_category_is_deferred(
			wp06, detail::DeferredStartupBudgetCategory::SerializationScratch));
		EXPECT_TRUE(detail::startup_budget_category_is_deferred(
			wp06, detail::DeferredStartupBudgetCategory::Metrics));
		EXPECT_FALSE(wp06.is_complete) << "Metrics remain an honest post-WP08 startup blocker.";
	}
}

TEST(TelemetryP85BudgetContract, ExactP8BudgetDefersMetricsOnly)
{
	for (const auto clients : {1U, 4U}) {
		const auto wp04 = detail::calculate_wp04_startup_budget(detail::make_wp03_known_budget_request(clients));
		const auto budget = detail::calculate_wp06_startup_budget(wp04, clients);
		ASSERT_EQ(detail::StartupBudgetError::None, budget.error);
		EXPECT_FALSE(detail::startup_budget_category_is_deferred(budget, detail::DeferredStartupBudgetCategory::BaselineStorage));
		EXPECT_FALSE(detail::startup_budget_category_is_deferred(budget, detail::DeferredStartupBudgetCategory::DeltaStorage));
		EXPECT_FALSE(detail::startup_budget_category_is_deferred(budget, detail::DeferredStartupBudgetCategory::SerializationScratch));
		EXPECT_TRUE(detail::startup_budget_category_is_deferred(budget, detail::DeferredStartupBudgetCategory::Metrics));
		EXPECT_FALSE(budget.is_complete) << "P8.5 permits Metrics as the only remaining deferred category.";
	}
}

TEST(TelemetryP85BudgetContract, OwnedCapacityAccountsExactlyForP8ReplicationAtOneAndFourClients)
{
	for (const auto clients : {1U, 4U}) {
		IdentityHarness ids{{{true, 1U}}};
		auto controller = make_controller(ids.allocator, nullptr, clients);
		const auto owned = controller.owned_capacity();
		const auto wp04 = detail::calculate_wp04_startup_budget(detail::make_wp03_known_budget_request(clients));
		const auto budget = detail::calculate_wp06_startup_budget(wp04, clients);

		ASSERT_EQ(detail::StartupBudgetError::None, budget.error);
		EXPECT_EQ(clients * 2U, budget.baseline_slot_count)
			<< "Each client owns one active baseline and at most one candidate baseline.";
		EXPECT_EQ(clients * 2U, budget.delta_slot_count)
			<< "Each client owns the current/cumulative delta state and one replaceable egress delta.";
		EXPECT_TRUE(has_phase1_replication_capacity_fields<detail::SessionControllerOwnedCapacity>::value)
			<< "owned_capacity() must expose the P8 baseline and delta capacities priced by the startup budget.";
		expect_phase1_replication_capacity(budget, owned);
		EXPECT_EQ(budget.snapshot_egress_heap_bytes, owned.snapshot_egress_heap_bytes);
		EXPECT_EQ(budget.delta_egress_heap_bytes, owned.delta_egress_heap_bytes);
		EXPECT_EQ(budget.delta_scratch_heap_bytes, owned.delta_scratch_heap_bytes);
		EXPECT_TRUE(detail::wp06_budget_matches_owned_storage(budget, owned));
	}

	const auto extreme = detail::calculate_wp06_startup_budget(
		detail::calculate_wp04_startup_budget(
			detail::make_wp03_known_budget_request(std::numeric_limits<std::size_t>::max())),
		std::numeric_limits<std::size_t>::max());
	EXPECT_NE(detail::StartupBudgetError::None, extreme.error)
		<< "An extreme maxClients value must fail closed before any capacity allocation.";
	EXPECT_EQ(0U, extreme.known_bytes);
}

TEST(TelemetryP85ReconnectContract, ClosedLiveSlotRehandshakesAndReusesProvisionedDeltaEgressWithoutAllocation)
{
	IdentityHarness ids{{{true, 0x8501U}, {true, 0x8502U}}};
	auto controller = make_controller(ids.allocator);
	activate_phase1_live_baseline(controller, 0x8501U);
	ASSERT_TRUE(controller.close_slot(0U, detail::SessionCloseReason::ProtocolError));
	EXPECT_EQ(0U, controller.active_slots());

	activate_phase1_live_baseline(controller, 0x8502U, 10'000U);
	ASSERT_EQ(detail::Phase1SnapshotProgress::Live, controller.snapshot_progress(0U));
	controller.begin_phase1_allocation_observation();
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		controller.replace_current_state(0U, phase1_integration_image_at(12.0F, 8'500U)));
	ASSERT_TRUE(controller.queue_cumulative_delta(0U, 8'501U));
	ASSERT_EQ(1U, controller.service_delta_egress(1U, 8'502U));
	const auto output = pop_output(controller);
	EXPECT_EQ(protocol::MessageType::Delta, output.datagram.header.message_type);
	EXPECT_EQ(0U, controller.phase1_observed_allocation_count())
		<< "A rehandshake must reuse the slot-owned delta egress and its preallocated capacity.";
}

TEST(TelemetryWp06BudgetContract, ReplacementRejectsIncoherentPriorProjectionInsteadOfUnderflowingOrDoubleCounting)
{
	auto wp04 = detail::calculate_wp04_startup_budget(detail::make_wp03_known_budget_request(1U));
	ASSERT_EQ(detail::StartupBudgetError::None, wp04.error);
	wp04.known_bytes = wp04.reassembly_bytes + wp04.reliable_retention_projection_bytes - 1U;
	wp04.metric_known_bytes = static_cast<std::uint64_t>(wp04.known_bytes);
	const auto result = detail::calculate_wp06_startup_budget(wp04, 1U);
	EXPECT_EQ(detail::StartupBudgetError::ArithmeticOverflow, result.error);
	EXPECT_EQ(0U, result.known_bytes);
	EXPECT_EQ(0U, result.metric_known_bytes);
}

TEST(TelemetryWp06BudgetContract, ControllerOwnedBackingMatchesEveryWp06CapacityAndRejectsInvalidComposition)
{
	IdentityHarness ids{{{true, 1U}}};
	auto controller = make_controller(ids.allocator, nullptr, 4U);
	const auto owned = controller.owned_capacity();
	EXPECT_EQ(4U, owned.client_slots);
	EXPECT_EQ(4U * protocol::MaxStateReassembliesPerClient, owned.state_reassembly_slots);
	EXPECT_EQ(4U * protocol::MaxStateReassemblyBytesPerClient, owned.state_reassembly_bytes);
	EXPECT_EQ(4U * detail::Wp06ReliableRetentionBytesPerClient, owned.reliable_retention_bytes);
	EXPECT_EQ(0U, owned.dynamic_allocations_after_ready);
	const auto wp04 = detail::calculate_wp04_startup_budget(detail::make_wp03_known_budget_request(4U));
	EXPECT_TRUE(detail::wp06_budget_matches_owned_storage(
		detail::calculate_wp06_startup_budget(wp04, 4U), owned));
	EXPECT_EQ(detail::StartupBudgetError::InvalidClientCount,
		detail::calculate_wp06_startup_budget(wp04, 0U).error);
	auto failed = wp04;
		failed.error = detail::StartupBudgetError::ArithmeticOverflow;
	EXPECT_EQ(detail::StartupBudgetError::ArithmeticOverflow,
		detail::calculate_wp06_startup_budget(failed, 4U).error);
}

TEST(TelemetryWp06BudgetContract, OwnedCapacityInventoriesPhysicalObjectsInsteadOfSyntheticConstants)
{
	IdentityHarness ids{{{true, 1U}}};
	auto controller = make_controller(ids.allocator, nullptr, 1U);
	const auto owned = controller.owned_capacity();
	EXPECT_TRUE(has_physical_wp06_capacity_fields<detail::SessionControllerOwnedCapacity>::value)
		<< "The owned-capacity seam omits the actual slot, rate-limiter, cache, ledger and output objects.";
	expect_physical_wp06_capacity(owned);
}

TEST(TelemetryWp06BudgetContract, BudgetPricesEveryOwnedRuntimeObjectAndMatcherRejectsEachMismatch)
{
	EXPECT_TRUE(has_complete_wp06_budget_fields<detail::Wp03KnownBudgetSubtotal>::value)
		<< "The subtotal cannot explain rate-limiter, cache, preproof-ledger and output bytes.";
	IdentityHarness ids{{{true, 1U}}};
	auto controller = make_controller(ids.allocator, nullptr, 1U);
	const auto owned = controller.owned_capacity();
	const auto wp04 = detail::calculate_wp04_startup_budget(detail::make_wp03_known_budget_request(1U));
	const auto wp06 = detail::calculate_wp06_startup_budget(wp04, 1U);
	for (std::size_t index = 0U; index < 4U; ++index) {
		auto mismatch = owned;
		switch (index) {
		case 0U: ++mismatch.rate_limiter_bytes; break;
		case 1U: ++mismatch.handshake_cache_bytes; break;
		case 2U: ++mismatch.preproof_ledger_bytes; break;
		case 3U: ++mismatch.output_queue_bytes; break;
		}
		EXPECT_FALSE(detail::wp06_budget_matches_owned_storage(wp06, mismatch)) << index;
	}
	auto overflow = wp04;
	overflow.reassembly_bytes = 0U;
	overflow.reliable_retention_projection_bytes = 0U;
	overflow.known_bytes = std::numeric_limits<std::size_t>::max();
	overflow.metric_known_bytes = std::numeric_limits<std::uint64_t>::max();
	EXPECT_EQ(detail::StartupBudgetError::ArithmeticOverflow,
		detail::calculate_wp06_startup_budget(overflow, 1U).error);
}

TEST(TelemetryWp06IngressContract, SourcePolicyStopsBeforeReadingHostileBytesAndReportsOnePrimaryReason)
{
	IdentityHarness ids{{{true, 0x1111U}}};
	StageRecorder observer;
	auto controller = make_controller(ids.allocator, &observer);
	const protocol::ByteView hostile{nullptr, std::numeric_limits<std::size_t>::max()};
	const auto result = controller.ingest(
		protocol::EndpointKey::from_ipv4({10U, 0U, 0U, 1U}, 9999U), hostile, 1U, 0U, false);
	EXPECT_EQ(detail::SessionIngressDisposition::Dropped, result.disposition);
	EXPECT_EQ(detail::SessionIngressDropReason::SourceNotAllowed, result.drop_reason);
	EXPECT_EQ((std::vector<detail::SessionIngressStage>{detail::SessionIngressStage::SourcePolicy}), observer.stages);
	EXPECT_EQ(0U, ids.random.calls);
	EXPECT_EQ(0U, controller.active_slots());
	EXPECT_FALSE(controller.has_output());
}

TEST(TelemetryWp06HandshakeContract, AcceptsOnlyExact11AndBuildsCanonicalWelcomeWithoutHeavyState)
{
	IdentityHarness ids{{{true, 0x1111222233334444ULL}}};
	auto controller = make_controller(ids.allocator);
	const auto request = hello(77U);
	const auto result = controller.ingest(endpoint(), view(request.bytes), 1'010'000U, 0U, false);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued, result.disposition);
	const auto output = pop_output(controller);
	ASSERT_EQ(protocol::MessageType::Welcome, output.datagram.header.message_type);
	EXPECT_EQ(protocol::VersionMinorV1_1, output.datagram.header.version_minor);
	EXPECT_EQ(0x1111222233334444ULL, output.datagram.header.session_id);
	EXPECT_EQ(protocol::MessageFlagAckRequired, output.datagram.header.flags);
	const auto welcome = decode_welcome(output);
	EXPECT_EQ(protocol::WelcomeStatus::Accepted, welcome.status);
	EXPECT_EQ(protocol::VersionMajor, welcome.selected_major);
	EXPECT_EQ(protocol::VersionMinorV1_1, welcome.selected_minor);
	EXPECT_EQ(77U, welcome.client_nonce);
	EXPECT_EQ(1'000'000U, welcome.client_send_t0_us);
	EXPECT_EQ(1'010'000U, welcome.producer_receive_t1_us);
	EXPECT_GE(welcome.producer_send_t2_us, welcome.producer_receive_t1_us);
	EXPECT_EQ(0x1020304050607080ULL, welcome.producer_id);
	EXPECT_EQ(protocol::VisibilityMode::Cockpit, welcome.selected_visibility_mode);
	EXPECT_EQ(0U, welcome.producer_capabilities);
	EXPECT_EQ(0U, welcome.active_capabilities);
	EXPECT_EQ(1000U, welcome.heartbeat_interval_ms);
	EXPECT_EQ(protocol::ReliableReassemblyTimeoutV1Ms, welcome.reliable_reassembly_timeout_ms);
	EXPECT_TRUE(welcome.extensions.empty());
	EXPECT_EQ(1U, controller.active_slots());
	EXPECT_EQ(detail::ProducerSessionProgress::AwaitWelcomeApplied, controller.slot(0U).progress);
	EXPECT_EQ(0U, controller.slot(0U).reassembly_bytes_reserved);
	EXPECT_EQ(1U, controller.slot(0U).reliable_items_in_use)
		<< "WELCOME Accepted is retained reliably until its exact APPLIED proof.";
}

TEST(TelemetryWp06IngressContract, ValidHelloTraversesTheNormativeStagesBeforeSessionMutation)
{
	IdentityHarness ids{{{true, 0x1111U}}};
	StageRecorder observer;
	auto controller = make_controller(ids.allocator, &observer);
	const auto request = hello(770U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(endpoint(), view(request.bytes), 1'010'000U, 0U, false).disposition);
	EXPECT_EQ((std::vector<detail::SessionIngressStage>{
			  detail::SessionIngressStage::SourcePolicy,
			  detail::SessionIngressStage::DatagramEnvelope,
			  detail::SessionIngressStage::EndpointAndSession,
			  detail::SessionIngressStage::RateLimit,
			  detail::SessionIngressStage::AntiAmplification,
			  detail::SessionIngressStage::Payload,
			  detail::SessionIngressStage::SessionMutation}),
		observer.stages);
}

TEST(TelemetryWp06IngressContract, EachInvalidStageStopsWithOnePrimaryReasonAndZeroMutationOrReservation)
{
	IdentityHarness ids{{{true, 0x1111U}}};
	auto controller = make_controller(ids.allocator);
	const auto valid = hello(771U);
	std::vector<std::pair<std::vector<std::uint8_t>, detail::SessionIngressDropReason>> cases;
	cases.push_back({std::vector<std::uint8_t>(10U, 0U), detail::SessionIngressDropReason::DatagramEnvelopeInvalid});
	auto bad_crc = valid.bytes;
	bad_crc.back() ^= 1U;
	cases.push_back({bad_crc, detail::SessionIngressDropReason::DatagramEnvelopeInvalid});
	auto bad_payload = valid.bytes;
	bad_payload[protocol::HeaderSizeV1 + 21U] = 1U;
	reseal_single_fragment(bad_payload);
	cases.push_back({bad_payload, detail::SessionIngressDropReason::PayloadInvalid});

	for (const auto& item : cases) {
		const auto before = controller.owned_usage();
		const auto result = controller.ingest(endpoint(), view(item.first), 1U, 0U, false);
		EXPECT_EQ(detail::SessionIngressDisposition::Dropped, result.disposition);
		EXPECT_EQ(item.second, result.drop_reason);
		EXPECT_EQ(before, controller.owned_usage());
		EXPECT_EQ(0U, controller.active_slots());
		EXPECT_EQ(0U, ids.random.calls);
		EXPECT_FALSE(controller.has_output());
	}
}

TEST(TelemetryWp06HandshakeContract, Rejects10WithA10PresessionWelcomeAndNoDurableSlot)
{
	IdentityHarness ids{{{true, 0x9999U}}};
	auto controller = make_controller(ids.allocator);
	const auto request = hello(78U, protocol::VersionMinorV1_0, protocol::VersionMinorV1_0);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(endpoint(), view(request.bytes), 1'010'000U, 0U, false).disposition);
	const auto output = pop_output(controller);
	EXPECT_EQ(protocol::VersionMinorV1_0, output.datagram.header.version_minor);
	EXPECT_EQ(0U, output.datagram.header.session_id);
	EXPECT_EQ(protocol::MessageFlagNone, output.datagram.header.flags);
	const auto welcome = decode_welcome(output);
	EXPECT_EQ(protocol::WelcomeStatus::UnsupportedVersion, welcome.status);
	EXPECT_EQ(78U, welcome.client_nonce);
	EXPECT_EQ(1'000'000U, welcome.client_send_t0_us);
	EXPECT_EQ(1'010'000U, welcome.producer_receive_t1_us);
	EXPECT_GE(welcome.producer_send_t2_us, welcome.producer_receive_t1_us);
	EXPECT_EQ(0U, welcome.selected_major);
	EXPECT_EQ(0U, welcome.selected_minor);
	EXPECT_EQ(protocol::VisibilityMode::Cockpit, welcome.selected_visibility_mode);
	EXPECT_EQ(0U, welcome.producer_capabilities);
	EXPECT_EQ(0U, welcome.active_capabilities);
	EXPECT_EQ(0U, welcome.heartbeat_interval_ms);
	EXPECT_EQ(0U, welcome.reliable_reassembly_timeout_ms);
	EXPECT_EQ(0x1020304050607080ULL, welcome.producer_id);
	EXPECT_TRUE(welcome.extensions.empty());
	EXPECT_EQ(0U, controller.active_slots());
	EXPECT_EQ(0U, ids.random.calls);
}

TEST(TelemetryWp06HandshakeContract, OfferedRange10Through11SelectsTheExactPhase1Minor11)
{
	IdentityHarness ids{{{true, 0x1234U}}};
	auto controller = make_controller(ids.allocator);
	const auto request = hello(781U, protocol::VersionMinorV1_0, protocol::VersionMinorV1_1);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(endpoint(), view(request.bytes), 1'010'000U, 0U, false).disposition);
	const auto output = pop_output(controller);
	EXPECT_EQ(protocol::VersionMinorV1_1, output.datagram.header.version_minor);
	EXPECT_EQ(protocol::VersionMinorV1_1, decode_welcome(output).selected_minor);
	EXPECT_EQ(0x1234U, output.datagram.header.session_id);
}

TEST(TelemetryWp06HandshakeContract, DuplicateHelloWithinTenSecondsReplaysIdenticalWelcomeAndSession)
{
	IdentityHarness ids{{{true, 0x1111U}, {true, 0x2222U}}};
	auto controller = make_controller(ids.allocator);
	const auto request = hello(79U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(endpoint(), view(request.bytes), 1'000'000U, 0U, false).disposition);
	const auto first = pop_output(controller).storage;
	ASSERT_EQ(detail::SessionIngressDisposition::CachedResponseQueued,
		controller.ingest(endpoint(), view(request.bytes), 10'999'999U, 0U, false).disposition);
	EXPECT_EQ(first, pop_output(controller).storage);
	EXPECT_EQ(1U, ids.random.calls);
	EXPECT_EQ(1U, controller.active_slots());
}

TEST(TelemetryWp06HandshakeContract, RejectedWelcomeIsCachedButExpiresExactlyAtTenSeconds)
{
	IdentityHarness ids{{{true, 1U}}};
	auto controller = make_controller(ids.allocator);
	const auto request = hello(790U, protocol::VersionMinorV1_0, protocol::VersionMinorV1_0);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(endpoint(), view(request.bytes), 0U, 0U, false).disposition);
	const auto first = pop_output(controller).storage;
	ASSERT_EQ(detail::SessionIngressDisposition::CachedResponseQueued,
		controller.ingest(endpoint(), view(request.bytes), 9'999'999U, 0U, false).disposition);
	EXPECT_EQ(first, pop_output(controller).storage);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(endpoint(), view(request.bytes), 10'000'000U, 0U, false).disposition);
	EXPECT_NE(first, pop_output(controller).storage) << "expiry requires a freshly timestamped rejection";
	EXPECT_EQ(0U, ids.random.calls);
}

TEST(TelemetryWp06HandshakeContract, CacheKeyRequiresBothCanonicalEndpointAndNonceAndNeverEvictsLiveEntries)
{
	IdentityHarness ids{{{true, 1U}}};
	auto controller = make_controller(ids.allocator);
	const auto original = hello(791U, protocol::VersionMinorV1_0, protocol::VersionMinorV1_0);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(endpoint(2U), view(original.bytes), 0U, 0U, false).disposition);
	(void)pop_output(controller);
	const auto other_nonce = hello(792U, protocol::VersionMinorV1_0, protocol::VersionMinorV1_0);
	EXPECT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(endpoint(2U), view(other_nonce.bytes), 1U, 0U, false).disposition);
	(void)pop_output(controller);
	EXPECT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(endpoint(3U), view(original.bytes), 2U, 0U, false).disposition);
	(void)pop_output(controller);

	for (std::uint64_t nonce = 793U; nonce < 798U; ++nonce) {
		const auto request = hello(nonce, protocol::VersionMinorV1_0, protocol::VersionMinorV1_0);
		ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
			controller.ingest(endpoint(static_cast<std::uint8_t>(nonce - 789U)),
				view(request.bytes), nonce, 0U, false).disposition);
		(void)pop_output(controller);
	}
	EXPECT_EQ(protocol::HandshakeCacheCapacity, controller.handshake_cache_entries());
	const auto overflow = hello(999U, protocol::VersionMinorV1_0, protocol::VersionMinorV1_0);
	const auto result = controller.ingest(endpoint(20U), view(overflow.bytes), 999U, 0U, false);
	EXPECT_EQ(detail::SessionIngressDisposition::Dropped, result.disposition);
	EXPECT_EQ(detail::SessionIngressDropReason::HandshakeCacheFull, result.drop_reason);
	EXPECT_EQ(protocol::HandshakeCacheCapacity, controller.handshake_cache_entries());
}

TEST(TelemetryWp06HandshakeContract, CacheAndPreproofLedgerStorageRecycleOnlyAfterExpiryCloseOrProof)
{
	{
		IdentityHarness ids{{{true, 1U}}};
		auto controller = make_controller(ids.allocator);
		const auto rejected = hello(798U, protocol::VersionMinorV1_0, protocol::VersionMinorV1_0);
		ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
			controller.ingest(endpoint(), view(rejected.bytes), 0U, 0U, false).disposition);
		(void)pop_output(controller);
		EXPECT_EQ(1U, controller.handshake_cache_entries());
		EXPECT_EQ(1U, controller.preproof_account_count());
		controller.expire_housekeeping(protocol::HandshakeCacheLifetimeMs * 1000U);
		EXPECT_EQ(0U, controller.handshake_cache_entries());
		EXPECT_EQ(0U, controller.preproof_account_count());
	}
	{
		IdentityHarness ids{{{true, 2U}}};
		auto controller = make_controller(ids.allocator);
		const auto accepted = hello(799U);
		ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
			controller.ingest(endpoint(), view(accepted.bytes), 0U, 0U, false).disposition);
		(void)pop_output(controller);
		ASSERT_TRUE(controller.close_slot(0U, detail::SessionCloseReason::ProtocolError));
		EXPECT_EQ(0U, controller.handshake_cache_entries());
		EXPECT_EQ(0U, controller.preproof_account_count());
	}
	{
		IdentityHarness ids{{{true, 3U}}};
		auto controller = make_controller(ids.allocator);
		const auto accepted = hello(800U);
		ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
			controller.ingest(endpoint(), view(accepted.bytes), 0U, 0U, false).disposition);
		const auto welcome = pop_output(controller);
		const auto cached_welcome_bytes = welcome.storage;
		const auto cached_session_id = welcome.datagram.header.session_id;
		const auto ack = applied_welcome_ack(welcome);
		ASSERT_EQ(detail::SessionIngressDisposition::WelcomeProofApplied,
			controller.ingest(endpoint(), view(ack.bytes), 1U, 0U, false).disposition);
		(void)pop_output(controller);
		EXPECT_EQ(1U, controller.handshake_cache_entries())
			<< "proof releases anti-amplification state, not the normative 10-second replay cache";
		EXPECT_EQ(0U, controller.preproof_account_count());
		const auto ids_before_replay = ids.random.calls;
		const auto slots_before_replay = controller.active_slots();
		ASSERT_EQ(detail::SessionIngressDisposition::CachedResponseQueued,
			controller.ingest(endpoint(), view(accepted.bytes), 9'999'999U, 0U, false).disposition);
		const auto replay = pop_output(controller);
		EXPECT_EQ(cached_welcome_bytes, replay.storage);
		EXPECT_EQ(cached_session_id, replay.datagram.header.session_id);
		EXPECT_EQ(ids_before_replay, ids.random.calls);
		EXPECT_EQ(slots_before_replay, controller.active_slots());
		EXPECT_EQ(0U, controller.preproof_account_count());
		controller.expire_housekeeping(protocol::HandshakeCacheLifetimeMs * 1000U);
		EXPECT_EQ(0U, controller.handshake_cache_entries());
	}
}

TEST(TelemetryWp06HandshakeContract, SessionIdsRetryCollisionsWithoutZeroOrReuseAndFailClosedAfterSixteenDraws)
{
	std::vector<ScriptedRandomSource::Draw> draws(16U, {true, 0x1111U});
	draws.push_back({true, 0x2222U});
	IdentityHarness ids(std::move(draws));
	ASSERT_EQ(detail::SessionIdRegistrationStatus::Registered, ids.registry.register_candidate(0x1111U));
	auto controller = make_controller(ids.allocator, nullptr, 2U);
	const auto second = hello(81U);
	const auto failure = controller.ingest(endpoint(3U), view(second.bytes), 2'000'000U, 0U, false);
	EXPECT_EQ(detail::SessionIngressDisposition::Faulted, failure.disposition);
	EXPECT_EQ(detail::SessionIngressDropReason::SessionIdUnavailable, failure.drop_reason);
	EXPECT_EQ(16U, ids.random.calls) << "draw 17 must remain unconsumed after sixteen collisions";
	EXPECT_EQ(0U, controller.active_slots());
	EXPECT_FALSE(controller.has_output());
}

TEST(TelemetryWp06HandshakeContract, ZeroAndEntropyFailureStopImmediatelyAndUniqueDrawSixteenSucceeds)
{
	for (const auto first : {ScriptedRandomSource::Draw{true, 0U}, ScriptedRandomSource::Draw{false, 99U}}) {
		IdentityHarness ids{{first, {true, 7U}}};
		auto controller = make_controller(ids.allocator);
		const auto request = hello(800U);
		const auto result = controller.ingest(endpoint(), view(request.bytes), 1U, 0U, false);
		EXPECT_EQ(detail::SessionIngressDisposition::Faulted, result.disposition);
		EXPECT_EQ(detail::SessionIngressDropReason::SessionIdUnavailable, result.drop_reason);
		EXPECT_EQ(1U, ids.random.calls);
		EXPECT_EQ(0U, ids.registry.used_count());
		EXPECT_EQ(0U, controller.active_slots());
	}

	std::vector<ScriptedRandomSource::Draw> draws;
	for (std::size_t i = 0; i < 15U; ++i) {
		draws.push_back({true, 42U});
	}
	draws.push_back({true, 99U});
	IdentityHarness ids(std::move(draws));
	ASSERT_EQ(detail::SessionIdRegistrationStatus::Registered, ids.registry.register_candidate(42U));
	auto controller = make_controller(ids.allocator);
	const auto request = hello(801U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(endpoint(), view(request.bytes), 1U, 0U, false).disposition);
	EXPECT_EQ(99U, pop_output(controller).datagram.header.session_id);
	EXPECT_EQ(16U, ids.random.calls);
}

TEST(TelemetryWp06HandshakeContract, ClosingASlotNeverPermitsItsSessionIdToBeReused)
{
	IdentityHarness ids{{{true, 42U}, {true, 42U}, {true, 99U}}};
	auto controller = make_controller(ids.allocator);
	const auto first = hello(802U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(endpoint(), view(first.bytes), 1U, 0U, false).disposition);
	EXPECT_EQ(42U, pop_output(controller).datagram.header.session_id);
	ASSERT_TRUE(controller.close_slot(0U, detail::SessionCloseReason::ProtocolError));
	EXPECT_EQ(0U, controller.active_slots());
	const auto second = hello(803U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(endpoint(3U), view(second.bytes), 2'000'000U, 0U, false).disposition);
	EXPECT_EQ(99U, pop_output(controller).datagram.header.session_id);
	EXPECT_EQ(3U, ids.random.calls);
	EXPECT_EQ(2U, ids.registry.used_count());
}

TEST(TelemetryWp06HandshakeContract, ExhaustedProcessRegistryFailsBeforeEntropyOrSlotMutation)
{
	IdentityHarness ids{{{true, 999U}}};
	for (std::size_t value = 1U; value <= detail::MaximumSessionIdsPerProcess; ++value) {
		ASSERT_EQ(detail::SessionIdRegistrationStatus::Registered, ids.registry.register_candidate(value));
	}
	auto controller = make_controller(ids.allocator);
	const auto request = hello(804U);
	const auto result = controller.ingest(endpoint(), view(request.bytes), 1U, 0U, false);
	EXPECT_EQ(detail::SessionIngressDisposition::Faulted, result.disposition);
	EXPECT_EQ(detail::SessionIngressDropReason::SessionIdUnavailable, result.drop_reason);
	EXPECT_EQ(0U, ids.random.calls);
	EXPECT_EQ(0U, controller.active_slots());
}

TEST(TelemetryWp06SecurityContract, HelloAndSessionCreationTokenBucketsPrecedeSessionIdAndMutation)
{
	std::vector<ScriptedRandomSource::Draw> draws;
	for (std::uint64_t id = 1U; id <= 16U; ++id) {
		draws.push_back({true, id});
	}
	IdentityHarness ids(std::move(draws));
	auto controller = make_controller(ids.allocator, nullptr, 4U);
	for (std::uint64_t nonce = 1U; nonce <= protocol::HelloRateLimit.burst_tokens; ++nonce) {
		const auto request = hello(nonce, protocol::VersionMinorV1_0, protocol::VersionMinorV1_0,
			static_cast<std::uint32_t>(nonce));
		EXPECT_EQ(detail::SessionIngressDisposition::ResponseQueued,
			controller.ingest(endpoint(2U, static_cast<std::uint16_t>(43000U + nonce)),
				view(request.bytes), 0U, 0U, false).disposition);
		(void)pop_output(controller);
	}
	const auto ninth = hello(99U, protocol::VersionMinorV1_0, protocol::VersionMinorV1_0, 99U);
	const auto limited = controller.ingest(endpoint(2U, 44000U), view(ninth.bytes), 0U, 0U, false);
	EXPECT_EQ(detail::SessionIngressDisposition::Dropped, limited.disposition);
	EXPECT_EQ(detail::SessionIngressDropReason::HelloRateLimited, limited.drop_reason);
	EXPECT_EQ(0U, ids.random.calls);
	EXPECT_EQ(0U, controller.active_slots());
}

TEST(TelemetryWp06SecurityContract, SessionCreationBurstIsFourAndFifthRequestIsRejectedBeforeIdDrawOrSlotMutation)
{
	IdentityHarness ids{{{true, 1U}, {true, 2U}, {true, 3U}, {true, 4U}, {true, 5U}}};
	auto controller = make_controller(ids.allocator, nullptr, 4U);
	for (std::uint64_t nonce = 1U; nonce <= protocol::SessionCreationRateLimit.burst_tokens; ++nonce) {
		const auto request = hello(100U + nonce, protocol::VersionMinorV1_1, protocol::VersionMinorV1_1,
			static_cast<std::uint32_t>(nonce));
		ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
			controller.ingest(endpoint(2U, static_cast<std::uint16_t>(45000U + nonce)),
				view(request.bytes), 0U, 0U, false).disposition);
		(void)pop_output(controller);
	}
	ASSERT_EQ(4U, controller.active_slots());
	ASSERT_EQ(4U, ids.random.calls);
	const auto fifth = hello(199U, protocol::VersionMinorV1_1, protocol::VersionMinorV1_1, 99U);
	const auto limited = controller.ingest(endpoint(2U, 46000U), view(fifth.bytes), 0U, 0U, false);
	EXPECT_EQ(detail::SessionIngressDisposition::Dropped, limited.disposition);
	EXPECT_EQ(detail::SessionIngressDropReason::SessionCreationRateLimited, limited.drop_reason);
	EXPECT_EQ(4U, ids.random.calls);
	EXPECT_EQ(4U, controller.active_slots());
	EXPECT_FALSE(controller.has_output());
}

TEST(TelemetryWp06SecurityContract, HelloBucketUsesSourceIpNotPortHasExactRefillAndIsIndependentPerIp)
{
	IdentityHarness ids{{{true, 1U}}};
	auto controller = make_controller(ids.allocator);
	auto invalid_hello = [](std::uint64_t nonce) {
		auto request = hello(nonce);
		request.bytes[protocol::HeaderSizeV1 + 21U] = 1U;
		reseal_single_fragment(request.bytes);
		return request;
	};
	for (std::uint64_t nonce = 1U; nonce <= protocol::HelloRateLimit.burst_tokens; ++nonce) {
		const auto request = invalid_hello(nonce);
		EXPECT_EQ(detail::SessionIngressDropReason::PayloadInvalid,
			controller.ingest(endpoint(2U, static_cast<std::uint16_t>(47000U + nonce)),
				view(request.bytes), 0U, 0U, false).drop_reason);
	}
	const auto ninth = invalid_hello(99U);
	EXPECT_EQ(detail::SessionIngressDropReason::HelloRateLimited,
		controller.ingest(endpoint(2U, 48000U), view(ninth.bytes), 0U, 0U, false).drop_reason);
	EXPECT_EQ(detail::SessionIngressDropReason::PayloadInvalid,
		controller.ingest(endpoint(3U, 48000U), view(ninth.bytes), 0U, 0U, false).drop_reason);
	EXPECT_EQ(detail::SessionIngressDropReason::HelloRateLimited,
		controller.ingest(endpoint(2U, 48001U), view(ninth.bytes), 249'999U, 0U, false).drop_reason);
	EXPECT_EQ(detail::SessionIngressDropReason::PayloadInvalid,
		controller.ingest(endpoint(2U, 48002U), view(ninth.bytes), 250'000U, 0U, false).drop_reason);
	EXPECT_EQ(0U, ids.random.calls);
	EXPECT_EQ(0U, controller.active_slots());
}

TEST(TelemetryWp06SecurityContract, PreproofResponsesNeverExceedThreeTimesValidatedReceivedBytes)
{
	IdentityHarness ids{{{true, 0x1111U}}};
	auto controller = make_controller(ids.allocator);
	const auto request = hello(82U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(endpoint(), view(request.bytes), 1'000U, 0U, false).disposition);
	const auto output = pop_output(controller);
	EXPECT_LE(output.storage.size(), request.bytes.size() * 3U);
	EXPECT_EQ(request.bytes.size(), controller.slot(0U).preproof_validated_bytes_received);
	EXPECT_EQ(output.storage.size(), controller.slot(0U).preproof_bytes_sent);
	EXPECT_EQ(detail::ProducerSessionProgress::AwaitWelcomeApplied, controller.slot(0U).progress);
}

TEST(TelemetryWp06SecurityContract, AntiAmplificationLedgerIsCumulativePerEndpointAtMinusOneEqualAndPlusOne)
{
	detail::PreproofAmplificationLedger ledger;
	ASSERT_TRUE(ledger.configure(2U));
	const auto first = endpoint(2U);
	const auto second = endpoint(3U);
	ASSERT_EQ(detail::PreproofLedgerResult::Recorded, ledger.note_validated_receive(first, 100U));
	EXPECT_EQ(detail::PreproofLedgerResult::Allowed, ledger.try_account_send(first, 299U));
	EXPECT_EQ(detail::PreproofLedgerResult::Allowed, ledger.try_account_send(first, 1U));
	EXPECT_EQ(detail::PreproofLedgerResult::AntiAmplificationLimit, ledger.try_account_send(first, 1U));
	EXPECT_EQ(300U, ledger.account(first).bytes_sent);
	EXPECT_EQ(detail::PreproofLedgerResult::Recorded, ledger.note_validated_receive(second, 1U));
	EXPECT_EQ(detail::PreproofLedgerResult::Allowed, ledger.try_account_send(second, 3U));
	EXPECT_EQ(3U, ledger.account(second).bytes_sent);
	EXPECT_EQ(detail::PreproofLedgerResult::CapacityReached,
		ledger.note_validated_receive(endpoint(4U), 1U));
	EXPECT_EQ(detail::PreproofLedgerResult::ArithmeticOverflow,
		ledger.note_validated_receive(first, std::numeric_limits<std::uint64_t>::max()));
	EXPECT_EQ(100U, ledger.account(first).validated_bytes_received);
	ledger.mark_welcome_proven(first);
	EXPECT_EQ(detail::PreproofLedgerResult::ProofAlreadyApplied,
		ledger.try_account_send(first, std::numeric_limits<std::uint64_t>::max()));
}

TEST(TelemetryWp06SecurityContract, RejectionsAndCachedRepliesUseTheSameEndpointLedger)
{
	IdentityHarness ids{{{true, 1U}}};
	auto controller = make_controller(ids.allocator);
	const auto request = hello(820U, protocol::VersionMinorV1_0, protocol::VersionMinorV1_0);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(endpoint(), view(request.bytes), 0U, 0U, false).disposition);
	const auto first = pop_output(controller).storage;
	ASSERT_EQ(detail::SessionIngressDisposition::CachedResponseQueued,
		controller.ingest(endpoint(), view(request.bytes), 1U, 0U, false).disposition);
	(void)pop_output(controller);
	const auto account = controller.preproof_account(endpoint());
	EXPECT_EQ(request.bytes.size() * 2U, account.validated_bytes_received);
	EXPECT_EQ(first.size() * 2U, account.bytes_sent);
	EXPECT_LE(account.bytes_sent, account.validated_bytes_received * 3U);
}

protocol::AckPayload welcome_ack_payload(const DecodedOutput& welcome)
{
	protocol::AckPayload payload;
	payload.target_message_id = welcome.datagram.header.message_id;
	payload.target_message_type = protocol::MessageType::Welcome;
	payload.ack_flags = static_cast<std::uint8_t>(protocol::AckFlag::Validated) |
		static_cast<std::uint8_t>(protocol::AckFlag::Applied);
	payload.target_fragment_count = welcome.datagram.header.fragment_count;
	payload.target_message_crc32 = welcome.datagram.header.message_crc32;
	return payload;
}

EncodedDatagram encode_welcome_ack(const DecodedOutput& welcome,
	const protocol::AckPayload& payload,
	std::uint64_t session_id)
{
	std::array<std::uint8_t, protocol::AckPayloadSize> bytes{};
	auto put_u32 = [&bytes](std::size_t offset, std::uint32_t value) {
		for (std::size_t i = 0U; i < 4U; ++i) {
			bytes[offset + i] = static_cast<std::uint8_t>(value >> (8U * i));
		}
	};
	put_u32(0U, payload.target_message_id);
	bytes[4U] = static_cast<std::uint8_t>(payload.target_message_type);
	bytes[5U] = payload.ack_flags;
	bytes[6U] = static_cast<std::uint8_t>(payload.target_fragment_count);
	bytes[7U] = static_cast<std::uint8_t>(payload.target_fragment_count >> 8U);
	put_u32(8U, payload.target_message_crc32);
	protocol::TelemetryDatagramHeader header;
	header.version_minor = protocol::VersionMinorV1_1;
	header.message_type = protocol::MessageType::Ack;
	header.session_id = session_id == 0U ? welcome.datagram.header.session_id : session_id;
	header.packet_sequence = 2U;
	header.sent_time_us = 2'000U;
	header.message_id = 2U;
	header.message_size = static_cast<std::uint32_t>(bytes.size());
	header.message_crc32 = protocol::crc32_iso_hdlc(view(bytes));
	return encode_datagram(header, view(bytes));
}

EncodedDatagram applied_welcome_ack(const DecodedOutput& welcome)
{
	return encode_welcome_ack(welcome, welcome_ack_payload(welcome));
}

enum class IntegratedSnapshotAckMode : std::uint8_t { Applied = 0, ValidatedOnly, PartialTuple };

template <typename Controller>
void verify_integrated_phase1_snapshot_commit(IntegratedSnapshotAckMode mode)
{
	if constexpr (!has_integrated_phase1_snapshot_egress<Controller>::value) {
		ADD_FAILURE() << "P8.2 integration RED: SessionController must own the snapshot egress and slot lifecycle "
					  "(begin_initial_snapshot/service_initial_snapshot_egress/progress/dirty latch).";
		return;
	} else {
		IdentityHarness ids{{{true, 0x5151U}}};
		auto controller = make_controller(ids.allocator);
		const auto request = hello(0x900U + static_cast<std::uint64_t>(mode));
		ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
			controller.ingest(endpoint(), view(request.bytes), 1'000U, 7U, true).disposition);
		const auto welcome = pop_output(controller);
		const auto welcome_ack = applied_welcome_ack(welcome);
		ASSERT_EQ(detail::SessionIngressDisposition::WelcomeProofApplied,
			controller.ingest(endpoint(), view(welcome_ack.bytes), 2'000U, 7U, true).disposition);
		const auto begin = pop_output(controller);
		ASSERT_EQ(protocol::MessageType::SessionBegin, begin.datagram.header.message_type);
		EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState, controller.slot(0U).progress);

		const auto image = phase1_integration_image();
		ASSERT_TRUE(controller.begin_initial_snapshot(0U, image, 3'000U));
		EXPECT_EQ(detail::Phase1SnapshotProgress::Synchronizing, controller.snapshot_progress(0U));
		EXPECT_FALSE(controller.session_state_dirty(0U));
		ASSERT_EQ(1U, controller.service_initial_snapshot_egress(1U, 3'001U));
		const auto snapshot = pop_output(controller);
		ASSERT_EQ(protocol::MessageType::FullSnapshot, snapshot.datagram.header.message_type);
		EXPECT_EQ(begin.datagram.header.message_id + 1U, snapshot.datagram.header.message_id)
			<< "The initial snapshot must consume the controller's next message identity after SESSION_BEGIN.";
		EXPECT_EQ(begin.datagram.header.packet_sequence + 1U, snapshot.datagram.header.packet_sequence)
			<< "The initial snapshot must consume the controller's next packet sequence after SESSION_BEGIN.";
		EXPECT_NE(0U, static_cast<std::uint8_t>(snapshot.datagram.header.flags & protocol::MessageFlagAckRequired));
		EXPECT_NE(0U, static_cast<std::uint8_t>(snapshot.datagram.header.flags & protocol::MessageFlagKeyframe));

		auto snapshot_ack = welcome_ack_payload(snapshot);
		snapshot_ack.target_message_type = protocol::MessageType::FullSnapshot;
		if (mode == IntegratedSnapshotAckMode::ValidatedOnly) {
			snapshot_ack.ack_flags = static_cast<std::uint8_t>(protocol::AckFlag::Validated);
		} else {
			snapshot_ack.ack_flags = protocol::KnownAckFlags;
			if (mode == IntegratedSnapshotAckMode::PartialTuple) {
				// ACKs are message-level. A tuple not covering every retained
				// fragment is the only legal wire representation of a partial
				// acknowledgement attempt, and must never commit the baseline.
				++snapshot_ack.target_fragment_count;
			}
		}
		const auto encoded_snapshot_ack = encode_welcome_ack(snapshot, snapshot_ack);
		const auto ack_result = controller.ingest(endpoint(), view(encoded_snapshot_ack.bytes), 4'000U, 7U, true);

		if (mode != IntegratedSnapshotAckMode::Applied) {
			if (mode == IntegratedSnapshotAckMode::PartialTuple) {
				EXPECT_EQ(detail::SessionIngressDisposition::Dropped, ack_result.disposition);
			} else {
				EXPECT_NE(detail::SessionIngressDisposition::Dropped, ack_result.disposition);
			}
			EXPECT_EQ(detail::Phase1SnapshotProgress::Synchronizing, controller.snapshot_progress(0U));
			EXPECT_FALSE(controller.session_state_dirty(0U));
			EXPECT_FALSE(controller.consume_session_state_dirty(0U));
		} else {
			EXPECT_NE(detail::SessionIngressDisposition::Dropped, ack_result.disposition);
			EXPECT_EQ(detail::Phase1SnapshotProgress::Live, controller.snapshot_progress(0U));
			EXPECT_TRUE(controller.session_state_dirty(0U));
			EXPECT_TRUE(controller.consume_session_state_dirty(0U));
			EXPECT_FALSE(controller.consume_session_state_dirty(0U))
				<< "The P8.3 notification must remain a bounded latch after the controller-level commit.";
		}
	}
}

TEST(TelemetryPhase1SnapshotSessionIntegration, WelcomeReadyImageEgressAndFinalAppliedAckReachLive)
{
	verify_integrated_phase1_snapshot_commit<detail::SessionController>(IntegratedSnapshotAckMode::Applied);
}

TEST(TelemetryPhase1SnapshotSessionIntegration, ValidatedAckNeverCrossesTheControllerLiveGate)
{
	verify_integrated_phase1_snapshot_commit<detail::SessionController>(IntegratedSnapshotAckMode::ValidatedOnly);
}

TEST(TelemetryPhase1SnapshotSessionIntegration, PartialAckTupleNeverCrossesTheControllerLiveGate)
{
	verify_integrated_phase1_snapshot_commit<detail::SessionController>(IntegratedSnapshotAckMode::PartialTuple);
}

EncodedDatagram missing_fragment_nack(const DecodedOutput& target, std::uint32_t packet_sequence)
{
	std::array<std::uint8_t, 1U> bitmap{};
	std::size_t bitmap_size = 0U;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::initialize_missing_fragment_bitmap(
			target.datagram.header.fragment_count, mutable_view(bitmap), bitmap_size));
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::set_fragment_missing(
			{bitmap.data(), bitmap_size}, target.datagram.header.fragment_count, 0U));
	protocol::NackPayload nack;
	nack.target_message_id = target.datagram.header.message_id;
	nack.target_message_type = target.datagram.header.message_type;
	nack.reason = protocol::NackReason::MissingFragments;
	nack.target_fragment_count = target.datagram.header.fragment_count;
	nack.target_message_crc32 = target.datagram.header.message_crc32;
	nack.missing_bitmap = {bitmap.data(), bitmap_size};
	std::vector<std::uint8_t> payload(protocol::NackPayloadPrefixSize + bitmap_size);
	std::size_t written = 0U;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::encode_nack_payload(nack, mutable_view(payload), written));
	payload.resize(written);
	protocol::TelemetryDatagramHeader header;
	header.version_minor = protocol::VersionMinorV1_1;
	header.message_type = protocol::MessageType::Nack;
	header.session_id = target.datagram.header.session_id;
	header.packet_sequence = packet_sequence;
	header.sent_time_us = 3'000U;
	header.message_id = packet_sequence;
	header.message_size = static_cast<std::uint32_t>(payload.size());
	header.message_crc32 = protocol::crc32_iso_hdlc(view(payload));
	return encode_datagram(header, view(payload));
}

protocol::StateImage fragmented_snapshot_image()
{
	protocol::StateAtom atom;
	atom.key.record_type = 0x8001U;
	atom.value.resize(3'000U, 0x5aU);
	protocol::StateImage image;
	EXPECT_EQ(protocol::StateImageResult::Created,
		protocol::StateImage::create(std::vector<protocol::StateAtom>{std::move(atom)}, image));
	return image;
}

void begin_fragmented_snapshot(detail::SessionController& controller,
	DecodedOutput& begin,
	DecodedOutput& first_fragment)
{
	const auto request = hello(0xa01U);
	EXPECT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(endpoint(), view(request.bytes), 1'000U, 7U, true).disposition);
	const auto welcome = pop_output(controller);
	const auto welcome_ack = applied_welcome_ack(welcome);
	EXPECT_EQ(detail::SessionIngressDisposition::WelcomeProofApplied,
		controller.ingest(endpoint(), view(welcome_ack.bytes), 2'000U, 7U, true).disposition);
	begin = pop_output(controller);
	EXPECT_TRUE(controller.begin_initial_snapshot(0U, fragmented_snapshot_image(), 3'000U));
	EXPECT_EQ(1U, controller.service_initial_snapshot_egress(1U, 3'001U));
	first_fragment = pop_output(controller);
	EXPECT_EQ(protocol::MessageType::FullSnapshot, first_fragment.datagram.header.message_type);
	EXPECT_GT(first_fragment.datagram.header.fragment_count, 1U)
		<< "The controller-level retransmission contract needs a genuinely fragmented snapshot.";
}

TEST(TelemetryPhase1SnapshotSessionIntegration, MissingFragmentNackRetransmitsTheExactSnapshotWithFreshPacketSequence)
{
	IdentityHarness ids{{{true, 0x7171U}}};
	auto controller = make_controller(ids.allocator);
	DecodedOutput begin, first_fragment;
	begin_fragmented_snapshot(controller, begin, first_fragment);
	const auto nack = missing_fragment_nack(first_fragment, 700U);
	const auto disposition =
		controller.ingest(endpoint(), view(nack.bytes), 4'000U, 7U, true).disposition;
	EXPECT_NE(detail::SessionIngressDisposition::Dropped, disposition);
	ASSERT_TRUE(controller.has_output());
	const auto retransmission = pop_output(controller);
	EXPECT_EQ(protocol::MessageType::FullSnapshot, retransmission.datagram.header.message_type);
	EXPECT_EQ(first_fragment.datagram.header.message_id, retransmission.datagram.header.message_id);
	EXPECT_EQ(first_fragment.datagram.header.fragment_index, retransmission.datagram.header.fragment_index);
	EXPECT_NE(first_fragment.datagram.header.packet_sequence, retransmission.datagram.header.packet_sequence);
	EXPECT_NE(0U,
		static_cast<std::uint8_t>(retransmission.datagram.header.flags & protocol::MessageFlagRetransmission));
}

TEST(TelemetryPhase1SnapshotSessionIntegration, SnapshotNackPreemptsQueuedDeltaTail)
{
	IdentityHarness ids{{{true, 0x7181U}}};
	auto controller = make_controller(ids.allocator);
	activate_phase1_live_baseline(controller, 0x7181U);
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		controller.replace_current_state(0U, fragmented_snapshot_image()));
	protocol::ResyncRequestPayload request{1U, protocol::ResyncReason::UnknownBaseline,
		protocol::ResyncRequestFlagRequireFullSnapshot, 1U, 0U, 5'000U};
	std::array<std::uint8_t, protocol::ResyncRequestPayloadSize> payload{}; std::size_t written = 0U;
	ASSERT_EQ(protocol::ValidationError::None, protocol::encode_resync_request_payload(request, mutable_view(payload), written));
	protocol::TelemetryDatagramHeader header{}; header.version_minor = protocol::VersionMinorV1_1;
	header.message_type = protocol::MessageType::ResyncRequest; header.flags = protocol::MessageFlagAckRequired;
	header.session_id = controller.slot(0U).session_id; header.packet_sequence = 981U; header.sent_time_us = 5'000U;
	header.message_id = 981U; header.fragment_count = 1U; header.message_size = static_cast<std::uint32_t>(written);
	header.message_crc32 = protocol::crc32_iso_hdlc({payload.data(), written});
	const auto request_packet = encode_datagram(header, {payload.data(), written});
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(endpoint(), view(request_packet.bytes), 5'000U, 7U, true).disposition);
	(void)pop_output(controller); // validated RESYNC ACK
	ASSERT_EQ(1U, controller.service_initial_snapshot_egress(1U, 5'001U));
	const auto first_fragment = pop_output(controller);
	ASSERT_EQ(protocol::MessageType::FullSnapshot, first_fragment.datagram.header.message_type);
	ASSERT_GT(first_fragment.datagram.header.fragment_count, 1U);
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		controller.replace_current_state(0U, phase1_integration_image_at(9.0F, 5'002U)));
	ASSERT_TRUE(controller.queue_cumulative_delta(0U, 5'003U));
	ASSERT_EQ(1U, controller.service_delta_egress(1U, 5'004U)); // leave DELTA as the tail output
	auto expect_delta_tail = [&controller]() {
		detail::SessionControllerOutput pending;
		ASSERT_TRUE(controller.peek_output(pending));
		protocol::DatagramView pending_view;
		ASSERT_EQ(protocol::ValidationError::None,
			protocol::decode_and_validate_datagram({pending.bytes.data(), pending.size},
				protocol::ProtocolMinorRange{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1}, pending_view));
		EXPECT_EQ(protocol::MessageType::Delta, pending_view.header.message_type);
	};
	const auto foreign_nack = missing_fragment_nack(first_fragment, 982U);
	EXPECT_EQ(detail::SessionIngressDisposition::Dropped,
		controller.ingest(endpoint(3U), view(foreign_nack.bytes), 5'005U, 7U, true).disposition);
	expect_delta_tail();
	// A valid datagram envelope is insufficient: a NACK that does not name the
	// retained snapshot must not evict the queued Delta either.
	auto wrong_target_nack = missing_fragment_nack(first_fragment, 983U);
	put_u32(wrong_target_nack.bytes, protocol::HeaderSizeV1, first_fragment.datagram.header.message_id + 1U);
	reseal_single_fragment(wrong_target_nack.bytes);
	EXPECT_EQ(detail::SessionIngressDisposition::Dropped,
		controller.ingest(endpoint(), view(wrong_target_nack.bytes), 5'006U, 7U, true).disposition);
	expect_delta_tail();
	const auto nack = missing_fragment_nack(first_fragment, 984U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(endpoint(), view(nack.bytes), 5'007U, 7U, true).disposition);
	ASSERT_TRUE(controller.has_output());
	EXPECT_EQ(protocol::MessageType::FullSnapshot, pop_output(controller).datagram.header.message_type);
}

TEST(TelemetryPhase1SnapshotSessionIntegration, LostAppliedAckRetriesSnapshotAndTransactionExpiryPurgesTheSlot)
{
	IdentityHarness ids{{{true, 0x7272U}}};
	auto controller = make_controller(ids.allocator);
	DecodedOutput begin, first_fragment;
	begin_fragmented_snapshot(controller, begin, first_fragment);
	controller.service_reliability(4'000'000U);
	ASSERT_TRUE(controller.has_output()) << "A lost ACK must produce a retained snapshot retry before expiry.";
	const auto retry = pop_output(controller);
	EXPECT_EQ(protocol::MessageType::FullSnapshot, retry.datagram.header.message_type);
	EXPECT_NE(0U, static_cast<std::uint8_t>(retry.datagram.header.flags & protocol::MessageFlagRetransmission));
	for (std::uint16_t fragment_index = 1U; fragment_index < retry.datagram.header.fragment_count; ++fragment_index) {
		ASSERT_TRUE(controller.has_output())
			<< "The full RTO retransmission must complete before its transaction can reach the terminal timeout.";
		const auto next_retry = pop_output(controller);
		EXPECT_EQ(protocol::MessageType::FullSnapshot, next_retry.datagram.header.message_type);
		EXPECT_EQ(fragment_index, next_retry.datagram.header.fragment_index);
	}
	EXPECT_FALSE(controller.has_output());

	controller.service_reliability(3'000U + protocol::ReliableTransactionRetentionUs);
	EXPECT_EQ(detail::ProducerSessionProgress::Empty, controller.slot(0U).progress)
		<< "An initial snapshot transaction that expires after publication must purge the session back to Listening.";
	EXPECT_FALSE(controller.slot(0U).snapshot.has_candidate());
	EXPECT_FALSE(controller.slot(0U).snapshot.has_active_baseline());
	EXPECT_FALSE(controller.slot(0U).snapshot_egress.has_candidate());
	EXPECT_FALSE(controller.begin_initial_snapshot(0U, fragmented_snapshot_image(),
		3'000U + protocol::ReliableTransactionRetentionUs + 1U))
		<< "A purged slot cannot accept a new snapshot before a fresh handshake reaches ReadyForState.";
}

TEST(TelemetryPhase1SnapshotSessionIntegration, LostAppliedAckRtoRetransmitsEverySnapshotFragmentInTheSameCycle)
{
	IdentityHarness ids{{{true, 0x7373U}}};
	auto controller = make_controller(ids.allocator);
	DecodedOutput begin, first_fragment;
	begin_fragmented_snapshot(controller, begin, first_fragment);
	const auto fragment_count = first_fragment.datagram.header.fragment_count;
	ASSERT_GT(fragment_count, 1U);

	std::vector<DecodedOutput> original_fragments;
	original_fragments.push_back(std::move(first_fragment));
	for (std::uint16_t index = 1U; index < fragment_count; ++index) {
		ASSERT_EQ(1U, controller.service_initial_snapshot_egress(1U, 3'001U));
		auto fragment = pop_output(controller);
		ASSERT_EQ(protocol::MessageType::FullSnapshot, fragment.datagram.header.message_type);
		ASSERT_EQ(index, fragment.datagram.header.fragment_index);
		original_fragments.push_back(std::move(fragment));
	}

	const auto rto_us = 4'000'000U;
	std::vector<bool> retransmitted(fragment_count, false);
	controller.service_reliability(rto_us);
	for (std::uint16_t expected_count = 0U; expected_count < fragment_count; ++expected_count) {
		ASSERT_TRUE(controller.has_output())
			<< "Every fragment selected by the one full RTO action must be emitted before a later RTO.";
		const auto retransmission = pop_output(controller);
		ASSERT_EQ(protocol::MessageType::FullSnapshot, retransmission.datagram.header.message_type);
		ASSERT_EQ(original_fragments.front().datagram.header.message_id, retransmission.datagram.header.message_id);
		ASSERT_EQ(fragment_count, retransmission.datagram.header.fragment_count);
		ASSERT_LT(retransmission.datagram.header.fragment_index, fragment_count);
		EXPECT_FALSE(retransmitted[retransmission.datagram.header.fragment_index])
			<< "A full RTO must select each candidate fragment exactly once.";
		retransmitted[retransmission.datagram.header.fragment_index] = true;
		EXPECT_NE(original_fragments[retransmission.datagram.header.fragment_index].datagram.header.packet_sequence,
			retransmission.datagram.header.packet_sequence);
		EXPECT_NE(0U, static_cast<std::uint8_t>(
			retransmission.datagram.header.flags & protocol::MessageFlagRetransmission));
	}
	EXPECT_TRUE(std::all_of(retransmitted.begin(), retransmitted.end(), [](bool sent) { return sent; }));
}

TEST(TelemetryPhase1KeyframeContract, ControllerConsumesDiscontinuityIntoReliableReplacementCandidate)
{
	IdentityHarness ids{{{true, 0x7501U}}};
	auto controller = make_controller(ids.allocator);
	activate_phase1_live_baseline(controller, 0x7501U);
	auto no_player = phase1_integration_input();
	no_player.player_capture = {detail::CaptureStatus::InvalidSource, detail::CaptureReason::WrongObjectType};
	protocol::StateImage current;
	ASSERT_EQ(detail::Phase1StateImageBuildStatus::Created, detail::build_phase1_state_image(no_player, current));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied, controller.replace_current_state(0U, current));
	ASSERT_TRUE(controller.queue_cumulative_delta(0U, 5'000U));
	EXPECT_TRUE(controller.slot(0U).snapshot.has_candidate());
	EXPECT_TRUE(controller.slot(0U).snapshot_egress.has_candidate());
	EXPECT_EQ(1U, controller.slot(0U).snapshot.active_snapshot_id());
	ASSERT_EQ(1U, controller.service_initial_snapshot_egress(1U, 5'001U));
	const auto replacement = pop_output(controller);
	EXPECT_EQ(protocol::MessageType::FullSnapshot, replacement.datagram.header.message_type);
	EXPECT_NE(0U, static_cast<std::uint8_t>(replacement.datagram.header.flags & protocol::MessageFlagAckRequired));
	EXPECT_NE(0U, static_cast<std::uint8_t>(replacement.datagram.header.flags & protocol::MessageFlagKeyframe));
	protocol::FullSnapshotPartPayload replacement_payload;
	ASSERT_EQ(protocol::ValidationError::None,
		protocol::decode_full_snapshot_part_payload(replacement.datagram.payload, replacement_payload));
	EXPECT_EQ(2U, replacement_payload.record_count)
		<< "InvalidSource replacement omits player records; it is not a Delta or EVENT_BATCH workaround.";
}

TEST(TelemetryPhase1KeyframeContract, DefaultTwoSecondPeriodicDueStartsOneCandidateAndCoalescesWhilePending)
{
	IdentityHarness ids{{{true, 0x7502U}}};
	auto controller = make_controller(ids.allocator);
	activate_phase1_live_baseline(controller, 0x7502U);
	const auto due = controller.slot(0U).next_keyframe_due_us;
	ASSERT_EQ(2'002'000U, due) << "The default keyframeSeconds=2 is anchored at ReadyForState.";
	controller.service_periodic(due - 1U);
	EXPECT_FALSE(controller.slot(0U).snapshot.has_candidate());
	controller.service_periodic(due);
	ASSERT_TRUE(controller.slot(0U).snapshot.has_candidate());
	const auto candidate_id = controller.slot(0U).snapshot.candidate_snapshot_id();
	controller.service_periodic(due + 10'000'000U);
	EXPECT_TRUE(controller.slot(0U).snapshot.has_candidate());
	EXPECT_EQ(candidate_id, controller.slot(0U).snapshot.candidate_snapshot_id())
		<< "Missed periodic intervals coalesce while APPLIED is pending; no third snapshot is retained.";
}

TEST(TelemetryPhase1KeyframeContract, ResyncRequestIsValidatedAndStartsOneReplacement)
{
	IdentityHarness ids{{{true, 0x7503U}}}; auto controller = make_controller(ids.allocator);
	activate_phase1_live_baseline(controller, 0x7503U);
	protocol::ResyncRequestPayload request{1U, protocol::ResyncReason::UnknownBaseline,
		protocol::ResyncRequestFlagRequireFullSnapshot, 1U, 0U, 5'000U};
	std::array<std::uint8_t, protocol::ResyncRequestPayloadSize> payload{}; std::size_t written = 0U;
	ASSERT_EQ(protocol::ValidationError::None, protocol::encode_resync_request_payload(request, mutable_view(payload), written));
	protocol::TelemetryDatagramHeader header; header.version_minor = protocol::VersionMinorV1_1;
	header.message_type = protocol::MessageType::ResyncRequest; header.flags = protocol::MessageFlagAckRequired;
	header.session_id = controller.slot(0U).session_id;
	header.packet_sequence = 99U; header.sent_time_us = 5'000U; header.message_id = 99U;
	header.fragment_count = 1U; header.message_size = static_cast<std::uint32_t>(written);
	header.message_crc32 = protocol::crc32_iso_hdlc({payload.data(), written});
	const auto packet = encode_datagram(header, {payload.data(), written});
	EXPECT_NE(detail::SessionIngressDisposition::Dropped, controller.ingest(endpoint(), view(packet.bytes), 5'000U, 7U, true).disposition);
	ASSERT_TRUE(controller.has_output()); const auto ack_output = pop_output(controller);
	EXPECT_EQ(protocol::MessageType::Ack, ack_output.datagram.header.message_type);
	EXPECT_TRUE(controller.slot(0U).snapshot.has_candidate());
	ASSERT_EQ(1U, controller.service_initial_snapshot_egress(1U, 5'001U));
	const auto full = pop_output(controller);
	EXPECT_EQ(protocol::MessageType::FullSnapshot, full.datagram.header.message_type);
	EXPECT_EQ(ack_output.datagram.header.message_id + 1U, full.datagram.header.message_id);
	EXPECT_EQ(ack_output.datagram.header.packet_sequence + 1U, full.datagram.header.packet_sequence);
	const auto candidate_id = controller.slot(0U).snapshot.candidate_snapshot_id();
	EXPECT_NE(detail::SessionIngressDisposition::Dropped,
		controller.ingest(endpoint(), view(packet.bytes), 5'001U, 7U, true).disposition);
	EXPECT_EQ(candidate_id, controller.slot(0U).snapshot.candidate_snapshot_id())
		<< "A duplicate RESYNC identity is idempotent and cannot reserve a second replacement candidate.";
}

TEST(TelemetryPhase1KeyframeContract, ResyncRateLimitAllowsBurstTwoThenRejectsThirdIdentity)
{
	IdentityHarness ids{{{true, 0x7504U}}}; auto controller = make_controller(ids.allocator);
	activate_phase1_live_baseline(controller, 0x7504U);
	auto make_resync = [&](std::uint32_t message_id, std::uint32_t request_id) {
		protocol::ResyncRequestPayload request{request_id, protocol::ResyncReason::UnknownBaseline,
			protocol::ResyncRequestFlagRequireFullSnapshot, 1U, 0U, 5'000U};
		std::array<std::uint8_t, protocol::ResyncRequestPayloadSize> payload{}; std::size_t written = 0U;
		EXPECT_EQ(protocol::ValidationError::None, protocol::encode_resync_request_payload(request, mutable_view(payload), written));
		protocol::TelemetryDatagramHeader header; header.version_minor = protocol::VersionMinorV1_1;
		header.message_type = protocol::MessageType::ResyncRequest; header.flags = protocol::MessageFlagAckRequired;
		header.session_id = controller.slot(0U).session_id; header.packet_sequence = message_id;
		header.sent_time_us = 5'000U; header.message_id = message_id; header.fragment_count = 1U;
		header.message_size = static_cast<std::uint32_t>(written); header.message_crc32 = protocol::crc32_iso_hdlc({payload.data(), written});
		return encode_datagram(header, {payload.data(), written});
	};
	const auto first = make_resync(99U, 1U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued, controller.ingest(endpoint(), view(first.bytes), 5'000U, 7U, true).disposition);
	ASSERT_TRUE(controller.has_output()); (void)pop_output(controller);
	const auto candidate_id = controller.slot(0U).snapshot.candidate_snapshot_id();
	const auto second = make_resync(100U, 2U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued, controller.ingest(endpoint(), view(second.bytes), 5'000U, 7U, true).disposition);
	ASSERT_TRUE(controller.has_output()); (void)pop_output(controller);
	const auto candidate_after_second = controller.slot(0U).snapshot.candidate_snapshot_id();
	EXPECT_NE(candidate_id, candidate_after_second)
		<< "The second allowed RESYNC identity replaces the pending recovery keyframe with a fresh candidate.";
	const auto third = make_resync(101U, 3U);
	EXPECT_EQ(detail::SessionIngressDisposition::Dropped, controller.ingest(endpoint(), view(third.bytes), 5'000U, 7U, true).disposition);
	EXPECT_EQ(candidate_after_second, controller.slot(0U).snapshot.candidate_snapshot_id())
		<< "The rate-limited third identity cannot reserve another replacement candidate.";
}

TEST(TelemetryPhase1DeltaEgressContract, DeltaEgressIsV11UnreliableCumulativeAndSingleSlotReplaceable)
{
	EXPECT_TRUE(has_p8_3_delta_egress_seams<detail::SessionController>::value)
		<< "P8.3 RED: controller needs queue_cumulative_delta(slot, now) and service_delta_egress(budget, now). "
			   "The resulting DELTA must be v1.1, non-reliable, reference the active snapshot id and exact delta sequence; "
			   "WouldBlock replaces the sole pending delta with the newest cumulative payload, ACK(DELTA) is inert, "
			   "and only the Phase 1 compatible record-set may be encoded.";
}

TEST(TelemetryPhase1DeltaEgressContract, SaturatedHeartbeatProbesCannotPreemptOrMutateQueuedDelta)
{
	IdentityHarness ids{{{true, 0x7400U}}};
	auto controller = make_controller(ids.allocator, nullptr, 1U, 5U);
	activate_phase1_live_baseline(controller, 0x7400U);

	for (std::size_t index = 0U; index < protocol::MaxInFlightProbes; ++index) {
		controller.service_periodic(controller.slot(0U).heartbeat.next_periodic_due_us);
		const auto heartbeat = pop_output(controller);
		ASSERT_EQ(protocol::MessageType::Heartbeat, heartbeat.datagram.header.message_type);
	}
	ASSERT_EQ(protocol::MaxInFlightProbes, controller.slot(0U).heartbeat.probes.in_flight_count());
	const auto blocked_due = controller.slot(0U).heartbeat.next_periodic_due_us;
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		controller.replace_current_state(0U, phase1_integration_image_at(9.0F, 5'000U)));
	ASSERT_TRUE(controller.queue_cumulative_delta(0U, blocked_due - 1U));
	ASSERT_EQ(1U, controller.service_delta_egress(1U, blocked_due));
	detail::SessionControllerOutput before;
	ASSERT_TRUE(controller.peek_output(before));
	protocol::DatagramView before_datagram;
	ASSERT_EQ(protocol::ValidationError::None,
		protocol::decode_and_validate_datagram({before.bytes.data(), before.size},
			protocol::ProtocolMinorRange{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1}, before_datagram));
	ASSERT_EQ(protocol::MessageType::Delta, before_datagram.header.message_type);
	const auto probes_before = controller.slot(0U).heartbeat.probes.in_flight_count();
	const auto sequence_before = controller.slot(0U).next_packet_sequence;

	controller.service_periodic(blocked_due);

	detail::SessionControllerOutput after;
	ASSERT_TRUE(controller.peek_output(after));
	EXPECT_EQ(before.endpoint, after.endpoint);
	EXPECT_EQ(before.size, after.size);
	EXPECT_TRUE(std::equal(before.bytes.begin(),
		before.bytes.begin() + static_cast<std::ptrdiff_t>(before.size), after.bytes.begin()))
		<< "A saturated periodic heartbeat must not evict the queued DELTA.";
	EXPECT_EQ(probes_before, controller.slot(0U).heartbeat.probes.in_flight_count());
	EXPECT_EQ(blocked_due, controller.slot(0U).heartbeat.next_periodic_due_us)
		<< "A heartbeat with no available probe must not advance its periodic deadline.";
	EXPECT_EQ(sequence_before, controller.slot(0U).next_packet_sequence)
		<< "A heartbeat that could not reserve a probe must not consume a packet sequence.";
}

TEST(TelemetryPhase1DeltaEgressContract, LiveDeltaIsDecodableV11UnreliableAndReferencesTheActiveBaseline)
{
	IdentityHarness ids{{{true, 0x7401U}}};
	auto controller = make_controller(ids.allocator);
	activate_phase1_live_baseline(controller, 0x7401U);

	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		controller.replace_current_state(0U, phase1_integration_image_at(9.0F, 5'000U)));
	ASSERT_TRUE(controller.queue_cumulative_delta(0U, 5'001U));
	const auto expected_packet_sequence = controller.slot(0U).next_packet_sequence;
	ASSERT_EQ(1U, controller.service_delta_egress(1U, 5'002U));
	const auto output = pop_output(controller);

	EXPECT_EQ(protocol::VersionMinorV1_1, output.datagram.header.version_minor);
	EXPECT_EQ(protocol::MessageType::Delta, output.datagram.header.message_type);
	EXPECT_EQ(0U, static_cast<std::uint8_t>(output.datagram.header.flags & protocol::MessageFlagAckRequired));
	EXPECT_EQ(0U, static_cast<std::uint8_t>(output.datagram.header.flags & protocol::MessageFlagRetransmission));
	EXPECT_EQ(1U, output.datagram.header.frame_id);
	EXPECT_EQ(expected_packet_sequence, output.datagram.header.packet_sequence);
	protocol::DeltaPayload payload;
	ASSERT_EQ(protocol::ValidationError::None, protocol::decode_delta_payload(output.datagram.payload, payload));
	EXPECT_EQ(1U, payload.baseline_snapshot_id);
	EXPECT_EQ(1U, payload.delta_sequence);
	EXPECT_EQ(5'001U, payload.producer_sample_time_us);
	EXPECT_GT(payload.record_count, 0U);
}

TEST(TelemetryPhase1DeltaEgressContract, NewestUnstartedDeltaReplacesButOutstandingDeltaIsImmutable)
{
	IdentityHarness ids{{{true, 0x7402U}}};
	auto controller = make_controller(ids.allocator);
	activate_phase1_live_baseline(controller, 0x7402U);

	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		controller.replace_current_state(0U, phase1_integration_image_at(9.0F, 5'000U)));
	ASSERT_TRUE(controller.queue_cumulative_delta(0U, 5'001U));
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		controller.replace_current_state(0U, phase1_integration_image_at(10.0F, 5'002U)));
	ASSERT_TRUE(controller.queue_cumulative_delta(0U, 5'003U))
		<< "A newer cumulative delta replaces the sole delta before any fragment has entered the output slot.";

	ASSERT_EQ(1U, controller.service_delta_egress(1U, 5'004U));
	EXPECT_FALSE(controller.queue_cumulative_delta(0U, 5'005U))
		<< "A delta already offered to transport is immutable until its completion result is known.";
	const auto output = pop_output(controller);
	protocol::DeltaPayload payload;
	ASSERT_EQ(protocol::ValidationError::None, protocol::decode_delta_payload(output.datagram.payload, payload));
	EXPECT_EQ(2U, payload.delta_sequence)
		<< "The output must be the newest cumulative image, not the superseded unstarted delta.";
	EXPECT_EQ(5'003U, payload.producer_sample_time_us);
}

TEST(TelemetryPhase1DeltaEgressContract, WouldBlockDropsNonReliableDeltaAndDeltaAckCannotChangeState)
{
	IdentityHarness ids{{{true, 0x7403U}}};
	auto controller = make_controller(ids.allocator);
	activate_phase1_live_baseline(controller, 0x7403U);

	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		controller.replace_current_state(0U, phase1_integration_image_at(9.0F, 5'000U)));
	ASSERT_TRUE(controller.queue_cumulative_delta(0U, 5'001U));
	ASSERT_EQ(1U, controller.service_delta_egress(1U, 5'002U));
	detail::SessionControllerOutput pending;
	ASSERT_TRUE(controller.peek_output(pending));
	protocol::DatagramView pending_view;
	ASSERT_EQ(protocol::ValidationError::None,
		protocol::decode_and_validate_datagram({pending.bytes.data(), pending.size},
			protocol::ProtocolMinorRange{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1}, pending_view));
	ASSERT_EQ(protocol::MessageType::Delta, pending_view.header.message_type);

	// ACK(DELTA) is neither a reliable completion nor a baseline transition.
	DecodedOutput pending_output;
	pending_output.storage.assign(pending.bytes.begin(), pending.bytes.begin() + static_cast<std::ptrdiff_t>(pending.size));
	pending_output.datagram = pending_view;
	auto delta_ack = welcome_ack_payload(pending_output);
	delta_ack.target_message_type = protocol::MessageType::Delta;
	delta_ack.ack_flags = protocol::KnownAckFlags;
	const auto encoded_delta_ack = encode_welcome_ack(pending_output, delta_ack);
	EXPECT_EQ(detail::SessionIngressDisposition::Dropped,
		controller.ingest(endpoint(), view(encoded_delta_ack.bytes), 5'003U, 7U, true).disposition);
	EXPECT_TRUE(controller.has_output()) << "An ACK for DELTA must not complete or replace the pending non-reliable output.";

	controller.complete_output(detail::IoStatus::WouldBlock);
	EXPECT_FALSE(controller.has_output());
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		controller.replace_current_state(0U, phase1_integration_image_at(10.0F, 5'004U)));
	EXPECT_TRUE(controller.queue_cumulative_delta(0U, 5'005U))
		<< "WouldBlock abandons the non-reliable delta so the next cumulative image can replace it.";
}

TEST(TelemetryWp06HandshakeContract, ExactWelcomeAppliedProofQueuesSessionBeginWithMissionGenerationOrZero)
{
	for (const bool mission_active : {false, true}) {
		IdentityHarness ids{{{true, mission_active ? 0x2222U : 0x1111U}}};
		auto controller = make_controller(ids.allocator);
		const auto request = hello(mission_active ? 84U : 83U);
		ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
			controller.ingest(endpoint(), view(request.bytes), 1'000U, 7U, mission_active).disposition);
		const auto welcome = pop_output(controller);
		const auto ack = applied_welcome_ack(welcome);
		ASSERT_EQ(detail::SessionIngressDisposition::WelcomeProofApplied,
			controller.ingest(endpoint(), view(ack.bytes), 2'000U, 7U, mission_active).disposition);
		const auto begin = pop_output(controller);
		ASSERT_EQ(protocol::MessageType::SessionBegin, begin.datagram.header.message_type);
		EXPECT_EQ(protocol::VersionMinorV1_1, begin.datagram.header.version_minor);
		EXPECT_EQ(welcome.datagram.header.session_id, begin.datagram.header.session_id);
		EXPECT_EQ(protocol::MessageFlagAckRequired, begin.datagram.header.flags);
		EXPECT_EQ(0U, begin.datagram.header.frame_id);
		EXPECT_EQ(0, begin.datagram.header.mission_time_us);
		EXPECT_NE(0U, begin.datagram.header.message_id);
		protocol::SessionBeginPayload payload;
		ASSERT_EQ(protocol::ValidationError::None,
			protocol::decode_session_begin_payload(begin.datagram.payload, payload));
		EXPECT_EQ(mission_active ? 7U : 0U, payload.mission_instance_id);
		EXPECT_EQ(1'000U, payload.producer_session_start_us);
		EXPECT_EQ(0U, payload.required_manifest_id);
		EXPECT_EQ(0U, payload.initial_snapshot_id) << "WP08 owns the initial snapshot transaction.";
		EXPECT_NE(0U, payload.session_flags & protocol::SessionBeginFlagReadOnly);
		EXPECT_EQ(mission_active,
			(payload.session_flags & protocol::SessionBeginFlagMissionActive) != 0U);
		EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState, controller.slot(0U).progress);
		EXPECT_EQ(1U, controller.slot(0U).reliable_items_in_use)
			<< "SESSION_BEGIN remains retained until its exact APPLIED proof.";
		EXPECT_FALSE(controller.has_output())
			<< "WP06 must not emit MANIFEST, FULL_SNAPSHOT, DELTA or any WP08 state.";
	}
}

TEST(TelemetryWp06HandshakeContract, ForgedAppliedAckCannotOpenTheHeavyStateGate)
{
	IdentityHarness ids{{{true, 0x1111U}}};
	auto controller = make_controller(ids.allocator);
	const auto request = hello(85U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(endpoint(), view(request.bytes), 1'000U, 1U, true).disposition);
	const auto welcome = pop_output(controller);
	auto ack = applied_welcome_ack(welcome);
	ack.bytes.back() ^= 0x01U;
	const auto rejected = controller.ingest(endpoint(), view(ack.bytes), 2'000U, 1U, true);
	EXPECT_EQ(detail::SessionIngressDisposition::Dropped, rejected.disposition);
	EXPECT_EQ(detail::ProducerSessionProgress::AwaitWelcomeApplied, controller.slot(0U).progress);
	EXPECT_FALSE(controller.has_output());
}

TEST(TelemetryWp06HandshakeContract, EveryWelcomeProofDimensionIsBoundAndFlagsMustBeExactly03)
{
	IdentityHarness ids{{{true, 0x1111U}}};
	auto controller = make_controller(ids.allocator);
	const auto request = hello(86U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(endpoint(), view(request.bytes), 1'000U, 1U, true).disposition);
	const auto welcome = pop_output(controller);
	const auto canonical = welcome_ack_payload(welcome);

	std::vector<protocol::AckPayload> false_payloads;
	for (const auto flags : {std::uint8_t{0x01U}, std::uint8_t{0x02U}, std::uint8_t{0x07U}}) {
		auto value = canonical;
		value.ack_flags = flags;
		false_payloads.push_back(value);
	}
	auto wrong_id = canonical;
	++wrong_id.target_message_id;
	false_payloads.push_back(wrong_id);
	auto wrong_type = canonical;
	wrong_type.target_message_type = protocol::MessageType::SessionBegin;
	false_payloads.push_back(wrong_type);
	auto wrong_fragments = canonical;
	++wrong_fragments.target_fragment_count;
	false_payloads.push_back(wrong_fragments);
	auto wrong_crc = canonical;
	++wrong_crc.target_message_crc32;
	false_payloads.push_back(wrong_crc);

	for (const auto& payload : false_payloads) {
		const auto ack = encode_welcome_ack(welcome, payload);
		const auto result = controller.ingest(endpoint(), view(ack.bytes), 2'000U, 1U, true);
		EXPECT_EQ(detail::SessionIngressDisposition::Dropped, result.disposition);
		EXPECT_EQ(detail::SessionIngressDropReason::WelcomeProofMismatch, result.drop_reason);
		EXPECT_EQ(detail::ProducerSessionProgress::AwaitWelcomeApplied, controller.slot(0U).progress);
		EXPECT_FALSE(controller.has_output());
	}
	const auto wrong_session = encode_welcome_ack(welcome, canonical, welcome.datagram.header.session_id + 1U);
	EXPECT_EQ(detail::SessionIngressDropReason::WelcomeProofMismatch,
		controller.ingest(endpoint(), view(wrong_session.bytes), 2'000U, 1U, true).drop_reason);
	const auto valid = encode_welcome_ack(welcome, canonical);
	EXPECT_EQ(detail::SessionIngressDropReason::WelcomeProofMismatch,
		controller.ingest(endpoint(9U), view(valid.bytes), 2'000U, 1U, true).drop_reason);
	EXPECT_EQ(detail::ProducerSessionProgress::AwaitWelcomeApplied, controller.slot(0U).progress);
}

TEST(TelemetryWp06HandshakeContract, WelcomeProofBeforeReliableDeadlineIsAcceptedAndAtDeadlineIsInert)
{
	for (const auto delta : {protocol::ReliableOrdinaryRetentionUs - 1U,
		protocol::ReliableOrdinaryRetentionUs}) {
		IdentityHarness ids{{{true, 0x1111U}}};
		auto controller = make_controller(ids.allocator);
		const auto request = hello(87U + delta);
		ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
			controller.ingest(endpoint(), view(request.bytes), 1'000U, 0U, false).disposition);
		const auto welcome = pop_output(controller);
		const auto ack = applied_welcome_ack(welcome);
		const auto result = controller.ingest(endpoint(), view(ack.bytes), 1'000U + delta, 0U, false);
		if (delta < protocol::ReliableOrdinaryRetentionUs) {
			EXPECT_EQ(detail::SessionIngressDisposition::WelcomeProofApplied, result.disposition);
			(void)pop_output(controller);
		} else {
			EXPECT_EQ(detail::SessionIngressDisposition::Dropped, result.disposition);
			EXPECT_EQ(detail::SessionIngressDropReason::WelcomeProofExpired, result.drop_reason);
			EXPECT_EQ(detail::ProducerSessionProgress::AwaitWelcomeApplied, controller.slot(0U).progress);
			EXPECT_FALSE(controller.has_output());
		}
	}
}

TEST(TelemetryWp06HandshakeContract, OutOfOrderWelcomeAcksSelectTheExactEndpointAndSessionTuple)
{
	IdentityHarness ids{{{true, 0x1111U}, {true, 0x2222U}}};
	auto controller = make_controller(ids.allocator, nullptr, 2U);
	const auto first_endpoint = endpoint(2U, 42043U);
	const auto second_endpoint = endpoint(3U, 42044U);
	const auto first_hello = hello(9001U);
	const auto second_hello = hello(9002U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(first_endpoint, view(first_hello.bytes), 1'000U, 0U, false).disposition);
	const auto first_welcome = pop_output(controller);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(second_endpoint, view(second_hello.bytes), 1'001U, 0U, false).disposition);
	const auto second_welcome = pop_output(controller);

	const auto second_ack = applied_welcome_ack(second_welcome);
	const auto second_result = controller.ingest(second_endpoint, view(second_ack.bytes), 2'000U, 0U, false);
	ASSERT_EQ(detail::SessionIngressDisposition::WelcomeProofApplied, second_result.disposition)
		<< "ACK lookup must not select the first AwaitWelcomeApplied slot.";
	const auto second_begin = pop_output(controller);
	EXPECT_EQ(second_welcome.datagram.header.session_id, second_begin.datagram.header.session_id);
	EXPECT_EQ(detail::ProducerSessionProgress::AwaitWelcomeApplied, controller.slot(0U).progress);
	EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState, controller.slot(1U).progress);

	const auto first_ack = applied_welcome_ack(first_welcome);
	EXPECT_EQ(detail::SessionIngressDisposition::WelcomeProofApplied,
		controller.ingest(first_endpoint, view(first_ack.bytes), 2'001U, 0U, false).disposition);
}

TEST(TelemetryWp06HandshakeContract, ReliableSessionBeginSurvivesOutputPopAndComposesWithPhase0NackValidation)
{
	IdentityHarness ids{{{true, 0x1111U}}};
	auto controller = make_controller(ids.allocator);
	const auto request = hello(9010U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(endpoint(), view(request.bytes), 1'000U, 0U, false).disposition);
	const auto welcome = pop_output(controller);
	const auto proof = applied_welcome_ack(welcome);
	ASSERT_EQ(detail::SessionIngressDisposition::WelcomeProofApplied,
		controller.ingest(endpoint(), view(proof.bytes), 2'000U, 0U, false).disposition);
	auto begin = pop_output(controller);
	const std::vector<std::uint8_t> retained_payload(begin.datagram.payload.data,
		begin.datagram.payload.data + static_cast<std::ptrdiff_t>(begin.datagram.payload.size));
	const auto nack = missing_fragment_nack(begin, 77U);
	std::fill(begin.storage.begin(), begin.storage.end(), std::uint8_t{0xA5U});

	const auto result = controller.ingest(endpoint(), view(nack.bytes), 3'000U, 0U, false);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued, result.disposition)
		<< "The controller must route NACK through its real ReliableSendWindow.";
	ASSERT_TRUE(controller.has_output());
	const auto retransmission = pop_output(controller);
	EXPECT_EQ(protocol::MessageType::SessionBegin, retransmission.datagram.header.message_type);
	EXPECT_NE(std::uint8_t{0U},
		static_cast<std::uint8_t>(retransmission.datagram.header.flags & protocol::MessageFlagRetransmission));
	EXPECT_EQ(retained_payload,
		(std::vector<std::uint8_t>(retransmission.datagram.payload.data,
			retransmission.datagram.payload.data +
				static_cast<std::ptrdiff_t>(retransmission.datagram.payload.size))))
		<< "The retained logical payload must outlive the popped output buffer.";
}

TEST(TelemetryWp06SecurityContract, PreproofNackAccountsValidatedRxAndQueuedRetransmissionTransactionally)
{
	IdentityHarness ids{{{true, 0x1111U}}};
	auto controller = make_controller(ids.allocator);
	const auto request = hello(9011U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(endpoint(), view(request.bytes), 1'000U, 0U, false).disposition);
	const auto welcome = pop_output(controller);
	const auto before = controller.preproof_account(endpoint());
	const auto nack = missing_fragment_nack(welcome, 78U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(endpoint(), view(nack.bytes), 2'000U, 0U, false).disposition);
	const auto retransmission = pop_output(controller);
	const auto after = controller.preproof_account(endpoint());
	EXPECT_EQ(before.validated_bytes_received + nack.bytes.size(), after.validated_bytes_received);
	EXPECT_EQ(before.bytes_sent + retransmission.storage.size(), after.bytes_sent);
	EXPECT_LE(after.bytes_sent, after.validated_bytes_received * 3U);

	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(endpoint(), view(nack.bytes), 2'001U, 0U, false).disposition);
	const auto busy_account = controller.preproof_account(endpoint());
	const auto busy_usage = controller.owned_usage();
	const auto busy = controller.ingest(endpoint(), view(nack.bytes), 2'002U, 0U, false);
	EXPECT_EQ(detail::SessionIngressDropReason::OutputBusy, busy.drop_reason);
	EXPECT_EQ(busy_account.validated_bytes_received,
		controller.preproof_account(endpoint()).validated_bytes_received);
	EXPECT_EQ(busy_account.bytes_sent, controller.preproof_account(endpoint()).bytes_sent);
	EXPECT_EQ(busy_usage, controller.owned_usage());
}

TEST(TelemetryWp06HandshakeContract, BusyOutputRejectsBeforeIdSlotCacheLedgerAndReplayAccounting)
{
	{
		IdentityHarness ids{{{true, 0x1111U}}};
		auto controller = make_controller(ids.allocator, nullptr, 2U);
		const auto rejected = hello(9020U, protocol::VersionMinorV1_0, protocol::VersionMinorV1_0);
		ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
			controller.ingest(endpoint(), view(rejected.bytes), 1'000U, 0U, false).disposition);
		const auto before = controller.owned_usage();
		const auto before_account = controller.preproof_account(endpoint());
		const auto accepted = hello(9021U);
		const auto result = controller.ingest(endpoint(), view(accepted.bytes), 1'001U, 0U, false);
		EXPECT_EQ(detail::SessionIngressDropReason::OutputBusy, result.drop_reason);
		EXPECT_EQ(0U, ids.random.calls) << "Busy output must be rejected before session-ID allocation.";
		EXPECT_EQ(before, controller.owned_usage());
		EXPECT_EQ(before_account.validated_bytes_received,
			controller.preproof_account(endpoint()).validated_bytes_received);
		EXPECT_EQ(before_account.bytes_sent, controller.preproof_account(endpoint()).bytes_sent);
	}
	{
		IdentityHarness ids{{{true, 0x2222U}}};
		auto controller = make_controller(ids.allocator);
		const auto request = hello(9022U);
		ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
			controller.ingest(endpoint(), view(request.bytes), 1'000U, 0U, false).disposition);
		const auto before = controller.owned_usage();
		const auto before_account = controller.preproof_account(endpoint());
		const auto replay = controller.ingest(endpoint(), view(request.bytes), 1'001U, 0U, false);
		EXPECT_EQ(detail::SessionIngressDropReason::OutputBusy, replay.drop_reason);
		EXPECT_EQ(before, controller.owned_usage());
		EXPECT_EQ(before_account.validated_bytes_received,
			controller.preproof_account(endpoint()).validated_bytes_received)
			<< "A replay becomes accounted only after its cached bytes are queued.";
		EXPECT_EQ(before_account.bytes_sent, controller.preproof_account(endpoint()).bytes_sent);
	}
}

TEST(TelemetryWp06HandshakeContract, SameEndpointNonceLifetimesDoNotEraseOtherOutstandingPreproofAccounting)
{
	IdentityHarness ids{{{true, 0x1111U}, {true, 0x2222U}}};
	auto controller = make_controller(ids.allocator, nullptr, 2U);
	const auto shared = endpoint();
	const auto first = hello(9030U);
	const auto second = hello(9031U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(shared, view(first.bytes), 0U, 0U, false).disposition);
	(void)pop_output(controller);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(shared, view(second.bytes), 1'000'000U, 0U, false).disposition);
	(void)pop_output(controller);
	ASSERT_EQ(2U, controller.handshake_cache_entries());
	ASSERT_EQ(1U, controller.preproof_account_count());

	controller.expire_housekeeping(protocol::HandshakeCacheLifetimeMs * 1000U);
	EXPECT_EQ(1U, controller.handshake_cache_entries());
	EXPECT_EQ(1U, controller.preproof_account_count())
		<< "Expiring nonce 9030 must retain the endpoint ledger contribution for nonce 9031.";
	EXPECT_GT(controller.preproof_account(shared).validated_bytes_received, 0U);
}

TEST(TelemetryWp06HandshakeContract, ClosingOneSameEndpointSessionRetainsTheOtherNoncePreproofLedger)
{
	IdentityHarness ids{{{true, 0x1111U}, {true, 0x2222U}}};
	auto controller = make_controller(ids.allocator, nullptr, 2U);
	const auto shared = endpoint();
	const auto first = hello(9040U);
	const auto second = hello(9041U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(shared, view(first.bytes), 0U, 0U, false).disposition);
	(void)pop_output(controller);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(shared, view(second.bytes), 1U, 0U, false).disposition);
	(void)pop_output(controller);
	ASSERT_TRUE(controller.close_slot(0U, detail::SessionCloseReason::ProtocolError));
	EXPECT_EQ(1U, controller.active_slots());
	EXPECT_EQ(1U, controller.handshake_cache_entries());
	EXPECT_EQ(1U, controller.preproof_account_count());
	EXPECT_GT(controller.preproof_account(shared).validated_bytes_received, 0U);
}

template <typename Controller,
	typename std::enable_if<accepts_phase0_packet_sequence_source<Controller>::value, int>::type = 0>
void expect_packet_sequence_source_controls_presession_and_session_initialization()
{
	IdentityHarness ids{{{true, 0x1111U}}};
	ScriptedRandomSource packet_sequences{{true, 0x12345678U}, {true, 0xABCDEF00U}};
	Controller controller;
	ASSERT_EQ(detail::SessionControllerConfigureResult::Ready,
		Controller::configure(config(), ids.allocator, packet_sequences, 0U, nullptr, controller));
	const auto rejected = hello(9050U, protocol::VersionMinorV1_0, protocol::VersionMinorV1_0);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(endpoint(), view(rejected.bytes), 0U, 0U, false).disposition);
	EXPECT_EQ(0x12345678U, pop_output(controller).datagram.header.packet_sequence);

	const auto accepted = hello(9051U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(endpoint(), view(accepted.bytes), 1U, 0U, false).disposition);
	const auto welcome = pop_output(controller);
	EXPECT_EQ(0xABCDEF00U, welcome.datagram.header.packet_sequence);
	const auto proof = applied_welcome_ack(welcome);
	ASSERT_EQ(detail::SessionIngressDisposition::WelcomeProofApplied,
		controller.ingest(endpoint(), view(proof.bytes), 2U, 0U, false).disposition);
	EXPECT_EQ(0xABCDEF01U, pop_output(controller).datagram.header.packet_sequence);
}

template <typename Controller,
	typename std::enable_if<!accepts_phase0_packet_sequence_source<Controller>::value, int>::type = 0>
void expect_packet_sequence_source_controls_presession_and_session_initialization()
{
}

TEST(TelemetryWp06HandshakeContract, PacketSequencesUseTheInjectedPhase0SourceForPresessionAndSessionChannels)
{
	EXPECT_TRUE(accepts_phase0_packet_sequence_source<detail::SessionController>::value)
		<< "SessionController has no Phase 0 random source for packet_sequence initialization.";
	expect_packet_sequence_source_controls_presession_and_session_initialization<detail::SessionController>();
}

TEST(TelemetryWp06HandshakeContract, ControllerRequiresAnExplicitPacketSequenceEntropySource)
{
	EXPECT_FALSE(accepts_legacy_packet_sequence_fallback<detail::SessionController>::value)
		<< "The public fallback overload produces predictable process-global packet sequences.";
}

template <typename Controller, typename = void>
struct has_wp06_reliability_service_and_transactional_egress : std::false_type {};

template <typename Controller>
struct has_wp06_reliability_service_and_transactional_egress<Controller,
	std::void_t<decltype(std::declval<Controller&>().service_reliability(std::declval<std::uint64_t>())),
		decltype(std::declval<const Controller&>().peek_output(std::declval<detail::SessionControllerOutput&>())),
		decltype(std::declval<Controller&>().complete_output(std::declval<detail::IoStatus>()))>>
	: std::bool_constant<noexcept(std::declval<Controller&>().service_reliability(std::declval<std::uint64_t>())) &&
		noexcept(std::declval<const Controller&>().peek_output(std::declval<detail::SessionControllerOutput&>())) &&
		noexcept(std::declval<Controller&>().complete_output(std::declval<detail::IoStatus>()))> {};

protocol::AckPayload ack_for_target(const DecodedOutput& target, std::uint8_t flags)
{
	protocol::AckPayload payload;
	payload.target_message_id = target.datagram.header.message_id;
	payload.target_message_type = target.datagram.header.message_type;
	payload.ack_flags = flags;
	payload.target_fragment_count = target.datagram.header.fragment_count;
	payload.target_message_crc32 = target.datagram.header.message_crc32;
	return payload;
}

EncodedDatagram nack_for_target(const DecodedOutput& target,
	protocol::NackReason reason,
	std::uint32_t packet_sequence)
{
	std::array<std::uint8_t, 1U> bitmap{{1U}};
	protocol::NackPayload nack;
	nack.target_message_id = target.datagram.header.message_id;
	nack.target_message_type = target.datagram.header.message_type;
	nack.reason = reason;
	nack.target_fragment_count = target.datagram.header.fragment_count;
	nack.target_message_crc32 = target.datagram.header.message_crc32;
	if (reason == protocol::NackReason::MissingFragments) {
		nack.missing_bitmap = {bitmap.data(), bitmap.size()};
	}
	if (reason == protocol::NackReason::UnsupportedMessage) {
		// The canonical encoder permits UnsupportedMessage only for an unknown
		// target type. Encode that valid form, then make the target known so the
		// controller's wire decoder exercises the required incoherent rejection.
		nack.target_message_type = static_cast<protocol::MessageType>(0xfeU);
	}
	std::array<std::uint8_t, protocol::NackPayloadPrefixSize + 1U> payload{};
	std::size_t written = 0U;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::encode_nack_payload(nack, {payload.data(), payload.size()}, written));
	if (reason == protocol::NackReason::UnsupportedMessage) {
		payload[4U] = static_cast<std::uint8_t>(target.datagram.header.message_type);
	}
	protocol::TelemetryDatagramHeader header;
	header.version_minor = protocol::VersionMinorV1_1;
	header.message_type = protocol::MessageType::Nack;
	header.session_id = target.datagram.header.session_id;
	header.packet_sequence = packet_sequence;
	header.sent_time_us = 3'000U;
	header.message_id = packet_sequence;
	header.message_size = static_cast<std::uint32_t>(written);
	header.message_crc32 = protocol::crc32_iso_hdlc({payload.data(), written});
	return encode_datagram(header, {payload.data(), written});
}

EncodedDatagram nack_with_wrong_target_id(const DecodedOutput& target,
	protocol::NackReason reason,
	std::uint32_t packet_sequence)
{
	auto result = nack_for_target(target, reason, packet_sequence);
	put_u32(result.bytes, protocol::HeaderSizeV1, target.datagram.header.message_id + 1U);
	reseal_single_fragment(result.bytes);
	return result;
}

DecodedOutput establish_session_begin(detail::SessionController& controller,
	const protocol::EndpointKey& peer,
	std::uint64_t nonce,
	std::uint64_t hello_time_us,
	std::uint64_t proof_time_us)
{
	const auto request = hello(nonce);
	EXPECT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(peer, view(request.bytes), hello_time_us, 0U, false).disposition);
	const auto welcome = pop_output(controller);
	const auto proof = applied_welcome_ack(welcome);
	EXPECT_EQ(detail::SessionIngressDisposition::WelcomeProofApplied,
		controller.ingest(peer, view(proof.bytes), proof_time_us, 0U, false).disposition);
	return pop_output(controller);
}

template <typename Controller>
void expect_session_begin_ack_lifecycle()
{
	IdentityHarness ids{{{true, 0x1111U}}};
	auto controller = make_controller(ids.allocator);
	const auto begin = establish_session_begin(controller, endpoint(), 10'001U, 1'000U, 2'000U);
	ASSERT_EQ(1U, controller.slot(0U).reliable_items_in_use);
	const auto validated_flags = static_cast<std::uint8_t>(protocol::AckFlag::Validated);
	const auto validated = encode_welcome_ack(begin, ack_for_target(begin, validated_flags));
	const auto first = controller.ingest(endpoint(), view(validated.bytes), 2'100U, 0U, false);
	EXPECT_EQ(detail::SessionIngressDropReason::None, first.drop_reason);
	EXPECT_NE(detail::SessionIngressDisposition::Dropped, first.disposition);
	EXPECT_NE(detail::SessionIngressDisposition::Faulted, first.disposition);
	EXPECT_EQ(1U, controller.slot(0U).reliable_items_in_use);
	EXPECT_EQ(2'100U, controller.slot(0U).heartbeat.last_valid_network_activity_us)
		<< "A fully admitted exact ACK refreshes network activity.";
	EXPECT_FALSE(controller.has_output());

	const auto duplicate = controller.ingest(endpoint(), view(validated.bytes), 2'200U, 0U, false);
	EXPECT_EQ(detail::SessionIngressDropReason::None, duplicate.drop_reason);
	EXPECT_NE(detail::SessionIngressDisposition::Dropped, duplicate.disposition);
	EXPECT_NE(detail::SessionIngressDisposition::Faulted, duplicate.disposition);
	EXPECT_EQ(1U, controller.slot(0U).reliable_items_in_use)
		<< "A duplicate VALIDATED ACK must remain accepted without releasing the exact tuple.";
	EXPECT_EQ(2'200U, controller.slot(0U).heartbeat.last_valid_network_activity_us)
		<< "An exact duplicate VALIDATED ACK remains an admitted reliable response.";
	EXPECT_FALSE(controller.has_output());

	if constexpr (has_wp06_reliability_service_and_transactional_egress<Controller>::value) {
		const auto due = 2'000U + protocol::reliable_retry_delay_us(protocol::ReliableDefaultRtoUs,
			0x4653544c5f52544fULL,
			begin.datagram.header.session_id,
			begin.datagram.header.message_id,
			0U);
		controller.service_reliability(due);
		EXPECT_FALSE(controller.has_output()) << "VALIDATED suppresses retries while awaiting APPLIED.";

		auto forged_payload = ack_for_target(begin, protocol::KnownAckFlags);
		++forged_payload.target_message_id;
		const auto forged = encode_welcome_ack(begin, forged_payload);
		const auto progress_before_forged = controller.slot(0U).progress;
		const auto message_id_before_forged = controller.slot(0U).next_message_id;
		const auto packet_sequence_before_forged = controller.slot(0U).next_packet_sequence;
		const auto usage_before_forged = controller.owned_usage();
		const auto activity_before_forged = controller.slot(0U).heartbeat.last_valid_network_activity_us;
		const auto forged_result = controller.ingest(endpoint(), view(forged.bytes), due + 1U, 0U, false);
		EXPECT_NE(detail::SessionIngressDropReason::None, forged_result.drop_reason);
		EXPECT_EQ(usage_before_forged, controller.owned_usage());
		EXPECT_EQ(progress_before_forged, controller.slot(0U).progress);
		EXPECT_EQ(message_id_before_forged, controller.slot(0U).next_message_id);
		EXPECT_EQ(packet_sequence_before_forged, controller.slot(0U).next_packet_sequence);
		EXPECT_EQ(activity_before_forged, controller.slot(0U).heartbeat.last_valid_network_activity_us)
			<< "A forged ACK is rejected before activity mutation.";
		EXPECT_EQ(1U, controller.slot(0U).reliable_items_in_use);
		EXPECT_FALSE(controller.has_output());

		const auto applied = encode_welcome_ack(begin, ack_for_target(begin, protocol::KnownAckFlags));
		const auto applied_result = controller.ingest(endpoint(), view(applied.bytes), due + 2U, 0U, false);
		EXPECT_EQ(detail::SessionIngressDropReason::None, applied_result.drop_reason);
		EXPECT_NE(detail::SessionIngressDisposition::Dropped, applied_result.disposition);
		EXPECT_NE(detail::SessionIngressDisposition::Faulted, applied_result.disposition);
		EXPECT_EQ(0U, controller.slot(0U).reliable_items_in_use);
		EXPECT_EQ(due + 2U, controller.slot(0U).heartbeat.last_valid_network_activity_us)
			<< "The exact APPLIED ACK refreshes activity before releasing its tuple.";
		EXPECT_FALSE(controller.has_output());

		const auto progress_before_late = controller.slot(0U).progress;
		const auto message_id_before_late = controller.slot(0U).next_message_id;
		const auto packet_sequence_before_late = controller.slot(0U).next_packet_sequence;
		const auto usage_before_late = controller.owned_usage();
		const auto activity_before_late = controller.slot(0U).heartbeat.last_valid_network_activity_us;
		const auto late = controller.ingest(endpoint(), view(applied.bytes), due + 3U, 0U, false);
		EXPECT_NE(detail::SessionIngressDropReason::None, late.drop_reason);
		EXPECT_EQ(usage_before_late, controller.owned_usage());
		EXPECT_EQ(progress_before_late, controller.slot(0U).progress);
		EXPECT_EQ(message_id_before_late, controller.slot(0U).next_message_id);
		EXPECT_EQ(packet_sequence_before_late, controller.slot(0U).next_packet_sequence);
		EXPECT_EQ(activity_before_late, controller.slot(0U).heartbeat.last_valid_network_activity_us)
			<< "An APPLIED ACK becomes late after release and cannot extend the timeout.";
		EXPECT_EQ(0U, controller.slot(0U).reliable_items_in_use);
		EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState, controller.slot(0U).progress);
		EXPECT_FALSE(controller.has_output());
	} else {
		FAIL() << "The missing reliability service prevents the VALIDATED retry-suppression proof.";
	}
}

TEST(TelemetryWp06ReliabilityContract, SessionBeginAckLifecycleIsDuplicateSafeAndMutationAtomic)
{
	EXPECT_TRUE(has_wp06_reliability_service_and_transactional_egress<detail::SessionController>::value);
	expect_session_begin_ack_lifecycle<detail::SessionController>();
}

template <typename Slot, typename = void>
struct exposes_wp06_stale_terminal_policy : std::false_type {};

template <typename Slot>
struct exposes_wp06_stale_terminal_policy<Slot,
	std::void_t<decltype(std::decay_t<decltype(std::declval<Slot>().progress)>::Stale),
		decltype(std::declval<Slot>().has_reliability_terminal_policy),
		decltype(std::declval<Slot>().reliability_terminal_policy)>> : std::true_type {};

template <typename Slot>
void expect_session_begin_terminal_state(const Slot& slot)
{
	if constexpr (exposes_wp06_stale_terminal_policy<Slot>::value) {
		using Progress = std::decay_t<decltype(slot.progress)>;
		EXPECT_EQ(Progress::Stale, slot.progress);
		EXPECT_TRUE(slot.has_reliability_terminal_policy);
		EXPECT_EQ(protocol::ReliableTerminalPolicy::MarkSessionStale, slot.reliability_terminal_policy);
	} else {
		FAIL() << "A terminal SESSION_BEGIN NACK must expose Stale progress and MarkSessionStale reason.";
	}
}

template <typename Slot>
void expect_no_session_begin_terminal_state(const Slot& slot)
{
	if constexpr (exposes_wp06_stale_terminal_policy<Slot>::value) {
		EXPECT_FALSE(slot.has_reliability_terminal_policy);
	}
}

template <typename Controller>
void expect_resource_limit_preserves_original_schedule(Controller& controller, const DecodedOutput& begin)
{
	if constexpr (has_wp06_reliability_service_and_transactional_egress<Controller>::value) {
		const auto first_send_us = begin.datagram.header.sent_time_us;
		const auto due = first_send_us + protocol::reliable_retry_delay_us(protocol::ReliableDefaultRtoUs,
			0x4653544c5f52544fULL,
			begin.datagram.header.session_id,
			begin.datagram.header.message_id,
			0U);
		controller.service_reliability(due - 1U);
		EXPECT_FALSE(controller.has_output()) << "ResourceLimit must not move the original RTO earlier.";
		controller.service_reliability(due);
		ASSERT_TRUE(controller.has_output()) << "ResourceLimit must not reset or postpone the original RTO.";
		const auto retry = pop_output(controller);
		EXPECT_EQ(begin.datagram.header.message_type, retry.datagram.header.message_type);
		EXPECT_EQ(begin.datagram.header.message_id, retry.datagram.header.message_id);
		EXPECT_EQ(begin.datagram.header.fragment_count, retry.datagram.header.fragment_count);
		EXPECT_EQ(begin.datagram.header.message_crc32, retry.datagram.header.message_crc32);
		EXPECT_EQ(begin.datagram.payload.size, retry.datagram.payload.size);
		EXPECT_TRUE(std::equal(begin.datagram.payload.data,
			begin.datagram.payload.data + begin.datagram.payload.size,
			retry.datagram.payload.data));
		EXPECT_NE(begin.datagram.header.packet_sequence, retry.datagram.header.packet_sequence);
		EXPECT_NE(static_cast<std::uint8_t>(0U),
			static_cast<std::uint8_t>(retry.datagram.header.flags & protocol::MessageFlagRetransmission));

		controller.service_reliability(first_send_us + protocol::ReliableOrdinaryRetentionUs);
		EXPECT_FALSE(controller.has_output());
		EXPECT_EQ(0U, controller.slot(0U).reliable_items_in_use)
			<< "ResourceLimit must not extend the immutable retention deadline.";
		expect_session_begin_terminal_state(controller.slot(0U));
	} else {
		FAIL() << "The missing reliability service prevents the ResourceLimit RTO/deadline proof.";
	}
}

TEST(TelemetryWp06ReliabilityContract, NackDecisionsComposeWithoutRequiringAnImmediateRetransmission)
{
	struct Case {
		protocol::NackReason reason;
		bool queues_output;
		bool releases;
		bool accepted;
	};
	for (const auto item : {Case{protocol::NackReason::MissingFragments, true, false, true},
			 Case{protocol::NackReason::BadMessageCrc, true, false, true},
			 Case{protocol::NackReason::BadFragmentLayout, true, false, true},
			 Case{protocol::NackReason::ResourceLimit, false, false, true},
			 Case{protocol::NackReason::UnsupportedMessage, false, false, false}}) {
		SCOPED_TRACE(static_cast<unsigned int>(item.reason));
		IdentityHarness ids{{{true, 0x2222U}}};
		auto controller = make_controller(ids.allocator);
		const auto begin = establish_session_begin(
			controller, endpoint(), 11'000U + static_cast<std::uint8_t>(item.reason), 1'000U, 2'000U);
		const auto nack = nack_for_target(begin, item.reason, 100U + static_cast<std::uint8_t>(item.reason));
		const auto result = controller.ingest(endpoint(), view(nack.bytes), 3'000U, 0U, false);
		EXPECT_EQ(item.accepted ? detail::SessionIngressDropReason::None : detail::SessionIngressDropReason::PayloadInvalid,
			result.drop_reason);
		if (item.accepted) {
			EXPECT_NE(detail::SessionIngressDisposition::Dropped, result.disposition);
			EXPECT_NE(detail::SessionIngressDisposition::Faulted, result.disposition);
		}
		EXPECT_EQ(item.queues_output, controller.has_output());
		EXPECT_EQ(item.releases ? 0U : 1U, controller.slot(0U).reliable_items_in_use);
		if (item.reason == protocol::NackReason::ResourceLimit) {
			expect_resource_limit_preserves_original_schedule(controller, begin);
		}
	}
}

void expect_rejected_nack_preserves_reliability_state(const EncodedDatagram& nack,
	detail::SessionController& controller,
	const DecodedOutput& begin,
	std::uint64_t now_us)
{
	const auto activity_before = controller.slot(0U).heartbeat.last_valid_network_activity_us;
	const auto usage_before = controller.owned_usage();
	const auto progress_before = controller.slot(0U).progress;
	const auto packet_sequence_before = controller.slot(0U).next_packet_sequence;
	const auto message_id_before = controller.slot(0U).next_message_id;
	const auto result = controller.ingest(endpoint(), view(nack.bytes), now_us, 0U, false);
	EXPECT_EQ(detail::SessionIngressDisposition::Dropped, result.disposition);
	EXPECT_EQ(detail::SessionIngressDropReason::PayloadInvalid, result.drop_reason);
	EXPECT_EQ(activity_before, controller.slot(0U).heartbeat.last_valid_network_activity_us);
	EXPECT_EQ(usage_before, controller.owned_usage());
	EXPECT_EQ(progress_before, controller.slot(0U).progress);
	EXPECT_EQ(packet_sequence_before, controller.slot(0U).next_packet_sequence);
	EXPECT_EQ(message_id_before, controller.slot(0U).next_message_id);
	EXPECT_EQ(1U, controller.slot(0U).reliable_items_in_use);
	EXPECT_FALSE(controller.has_output());

	const auto due = begin.datagram.header.sent_time_us +
		protocol::reliable_retry_delay_us(protocol::ReliableDefaultRtoUs,
			0x4653544c5f52544fULL,
			begin.datagram.header.session_id,
			begin.datagram.header.message_id,
			0U);
	controller.service_reliability(due - 1U);
	EXPECT_FALSE(controller.has_output());
	controller.service_reliability(due);
	ASSERT_TRUE(controller.has_output()) << "Rejected NACK input cannot perturb the original retry schedule.";
	const auto retry = pop_output(controller);
	EXPECT_EQ(begin.datagram.header.message_type, retry.datagram.header.message_type);
	EXPECT_EQ(begin.datagram.header.message_id, retry.datagram.header.message_id);
	EXPECT_EQ(begin.datagram.header.message_crc32, retry.datagram.header.message_crc32);
}

void expect_original_retry_tuple_rto_and_deadline(detail::SessionController& controller,
	const DecodedOutput& begin)
{
	const auto due = begin.datagram.header.sent_time_us +
		protocol::reliable_retry_delay_us(protocol::ReliableDefaultRtoUs,
			0x4653544c5f52544fULL,
			begin.datagram.header.session_id,
			begin.datagram.header.message_id,
			0U);
	controller.service_reliability(due - 1U);
	EXPECT_FALSE(controller.has_output()) << "Rate-limited input cannot move the original RTO earlier.";
	controller.service_reliability(due);
	ASSERT_TRUE(controller.has_output()) << "Rate-limited input cannot reset or postpone the original RTO.";
	detail::SessionControllerOutput output;
	ASSERT_TRUE(controller.peek_output(output));
	EXPECT_EQ(endpoint(), output.endpoint);
	const auto retry = pop_output(controller);
	EXPECT_EQ(begin.datagram.header.session_id, retry.datagram.header.session_id);
	EXPECT_EQ(begin.datagram.header.message_type, retry.datagram.header.message_type);
	EXPECT_EQ(begin.datagram.header.message_id, retry.datagram.header.message_id);
	EXPECT_EQ(begin.datagram.header.fragment_count, retry.datagram.header.fragment_count);
	EXPECT_EQ(begin.datagram.header.message_crc32, retry.datagram.header.message_crc32);
	EXPECT_EQ(begin.datagram.payload.size, retry.datagram.payload.size);
	EXPECT_TRUE(std::equal(begin.datagram.payload.data,
		begin.datagram.payload.data + begin.datagram.payload.size,
		retry.datagram.payload.data));
	EXPECT_NE(begin.datagram.header.packet_sequence, retry.datagram.header.packet_sequence);
	EXPECT_NE(static_cast<std::uint8_t>(0U),
		static_cast<std::uint8_t>(retry.datagram.header.flags & protocol::MessageFlagRetransmission));

	const auto deadline = begin.datagram.header.sent_time_us + protocol::ReliableOrdinaryRetentionUs;
	controller.service_reliability(deadline - 1U);
	if (controller.has_output()) {
		const auto later_retry = pop_output(controller);
		EXPECT_EQ(begin.datagram.header.session_id, later_retry.datagram.header.session_id);
		EXPECT_EQ(begin.datagram.header.message_type, later_retry.datagram.header.message_type);
		EXPECT_EQ(begin.datagram.header.message_id, later_retry.datagram.header.message_id);
		EXPECT_EQ(begin.datagram.header.fragment_count, later_retry.datagram.header.fragment_count);
		EXPECT_EQ(begin.datagram.header.message_crc32, later_retry.datagram.header.message_crc32);
		EXPECT_EQ(begin.datagram.payload.size, later_retry.datagram.payload.size);
		EXPECT_TRUE(std::equal(begin.datagram.payload.data,
			begin.datagram.payload.data + begin.datagram.payload.size,
			later_retry.datagram.payload.data));
		EXPECT_NE(static_cast<std::uint8_t>(0U),
			static_cast<std::uint8_t>(later_retry.datagram.header.flags & protocol::MessageFlagRetransmission));
	}
	EXPECT_EQ(1U, controller.slot(0U).reliable_items_in_use);
	EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState, controller.slot(0U).progress);
	controller.service_reliability(deadline);
	EXPECT_FALSE(controller.has_output());
	EXPECT_EQ(0U, controller.slot(0U).reliable_items_in_use)
		<< "Rate-limited input cannot extend the immutable retention deadline.";
	expect_session_begin_terminal_state(controller.slot(0U));
}

TEST(TelemetryWp06ReliabilityContract, RejectedForgedIncoherentAndUncorrelatedNacksAreActivityAtomic)
{
	for (const auto kind : {0U, 1U}) {
		SCOPED_TRACE(kind);
		IdentityHarness ids{{{true, 0x2525U + kind}}};
		auto controller = make_controller(ids.allocator);
		const auto begin = establish_session_begin(controller, endpoint(), 12'500U + kind, 1'000U, 2'000U);
		const auto nack = kind == 0U
			? nack_with_wrong_target_id(begin, protocol::NackReason::MissingFragments, 300U + kind)
			: nack_for_target(begin, protocol::NackReason::UnsupportedMessage, 300U + kind);
		expect_rejected_nack_preserves_reliability_state(nack, controller, begin, 3'000U);
	}
}

TEST(TelemetryWp06ReliabilityContract, RateLimitedAndLateDuplicateNacksCannotExtendNetworkTimeout)
{
	{
		IdentityHarness ids{{{true, 0x2626U}}};
		auto controller = make_controller(ids.allocator);
		const auto begin = establish_session_begin(controller, endpoint(), 12'600U, 1'000U, 2'000U);
		// The WELCOME APPLIED proof already consumed one token from the session's
		// common ACK/NACK bucket. Non-correlated NACKs consume the remainder without
		// validating or otherwise changing the retained SESSION_BEGIN schedule.
		const auto burner = nack_with_wrong_target_id(begin, protocol::NackReason::MissingFragments, 399U);
		const auto activity_before_burners = controller.slot(0U).heartbeat.last_valid_network_activity_us;
		ASSERT_EQ(2'000U, activity_before_burners);
		for (std::uint32_t index = 0U; index + 1U < protocol::AckNackRateLimit.burst_tokens; ++index) {
			const auto rejected = controller.ingest(endpoint(), view(burner.bytes), 3'000U, 0U, false);
			ASSERT_EQ(detail::SessionIngressDisposition::Dropped, rejected.disposition) << index;
			ASSERT_EQ(detail::SessionIngressDropReason::PayloadInvalid, rejected.drop_reason) << index;
		}
		EXPECT_EQ(activity_before_burners, controller.slot(0U).heartbeat.last_valid_network_activity_us)
			<< "Non-correlated burner NACKs must remain activity-atomic while exhausting the bucket.";
		const auto usage_before = controller.owned_usage();
		const auto progress_before = controller.slot(0U).progress;
		const auto packet_sequence_before = controller.slot(0U).next_packet_sequence;
		const auto limited = nack_for_target(begin, protocol::NackReason::MissingFragments, 400U);
		const auto result = controller.ingest(endpoint(), view(limited.bytes), 4'000U, 0U, false);
		EXPECT_EQ(detail::SessionIngressDisposition::Dropped, result.disposition);
		EXPECT_EQ(detail::SessionIngressDropReason::PayloadInvalid, result.drop_reason);
		EXPECT_EQ(activity_before_burners, controller.slot(0U).heartbeat.last_valid_network_activity_us)
			<< "The shared ACK/NACK bucket must deny before any session mutation.";
		EXPECT_EQ(usage_before, controller.owned_usage());
		EXPECT_EQ(progress_before, controller.slot(0U).progress);
		EXPECT_EQ(packet_sequence_before, controller.slot(0U).next_packet_sequence);
		EXPECT_EQ(1U, controller.slot(0U).reliable_items_in_use);
		EXPECT_FALSE(controller.has_output());
		expect_original_retry_tuple_rto_and_deadline(controller, begin);
	}
	{
		IdentityHarness ids{{{true, 0x2727U}}};
		auto controller = make_controller(ids.allocator);
		const auto begin = establish_session_begin(controller, endpoint(), 12'700U, 1'000U, 2'000U);
		const auto terminal = nack_for_target(begin, protocol::NackReason::StaleBaseline, 500U);
		ASSERT_EQ(detail::SessionIngressDropReason::None,
			controller.ingest(endpoint(), view(terminal.bytes), 3'000U, 0U, false).drop_reason);
		ASSERT_EQ(3'000U, controller.slot(0U).heartbeat.last_valid_network_activity_us);
		ASSERT_EQ(detail::ProducerSessionProgress::Stale, controller.slot(0U).progress);
		ASSERT_EQ(0U, controller.slot(0U).reliable_items_in_use);
		const auto usage_before = controller.owned_usage();
		const auto duplicate = controller.ingest(endpoint(), view(terminal.bytes), 4'000U, 0U, false);
		EXPECT_EQ(detail::SessionIngressDisposition::Dropped, duplicate.disposition);
		EXPECT_EQ(detail::SessionIngressDropReason::PayloadInvalid, duplicate.drop_reason);
		EXPECT_EQ(3'000U, controller.slot(0U).heartbeat.last_valid_network_activity_us)
			<< "A terminal NACK is late and uncorrelated once its exact tuple has been released.";
		EXPECT_EQ(usage_before, controller.owned_usage());
		EXPECT_EQ(detail::ProducerSessionProgress::Stale, controller.slot(0U).progress);
		EXPECT_EQ(0U, controller.slot(0U).reliable_items_in_use);
		EXPECT_FALSE(controller.has_output());
	}
}

TEST(TelemetryWp06ReliabilityContract, FullyAdmittedNacksRefreshActivityForRetryWaitAndTerminalOutcomes)
{
	for (const auto reason : {protocol::NackReason::MissingFragments,
			 protocol::NackReason::ResourceLimit,
			 protocol::NackReason::StaleBaseline}) {
		SCOPED_TRACE(static_cast<unsigned int>(reason));
		IdentityHarness ids{{{true, 0x2828U + static_cast<std::uint8_t>(reason)}}};
		auto controller = make_controller(ids.allocator);
		const auto begin = establish_session_begin(
			controller, endpoint(), 12'800U + static_cast<std::uint8_t>(reason), 1'000U, 2'000U);
		const auto nack = nack_for_target(begin, reason, 600U + static_cast<std::uint8_t>(reason));
		const auto result = controller.ingest(endpoint(), view(nack.bytes), 3'000U, 0U, false);
		EXPECT_EQ(detail::SessionIngressDropReason::None, result.drop_reason);
		EXPECT_NE(detail::SessionIngressDisposition::Dropped, result.disposition);
		EXPECT_NE(detail::SessionIngressDisposition::Faulted, result.disposition);
		EXPECT_EQ(3'000U, controller.slot(0U).heartbeat.last_valid_network_activity_us)
			<< "Activity commits only after bucket admission and exact reliable correlation.";
		if (reason == protocol::NackReason::MissingFragments) {
			EXPECT_TRUE(controller.has_output());
			EXPECT_EQ(1U, controller.slot(0U).reliable_items_in_use);
			EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState, controller.slot(0U).progress);
		} else if (reason == protocol::NackReason::ResourceLimit) {
			EXPECT_FALSE(controller.has_output());
			EXPECT_EQ(1U, controller.slot(0U).reliable_items_in_use);
			EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState, controller.slot(0U).progress);
		} else {
			EXPECT_FALSE(controller.has_output());
			EXPECT_EQ(0U, controller.slot(0U).reliable_items_in_use);
			EXPECT_EQ(detail::ProducerSessionProgress::Stale, controller.slot(0U).progress);
		}
	}
}

TEST(TelemetryWp06ReliabilityContract, SessionBeginTerminalNacksExposeStaleReasonAndUnsupportedIsAtomic)
{
	for (const auto reason : {protocol::NackReason::StaleBaseline,
			 protocol::NackReason::SemanticValidationFailed,
			 protocol::NackReason::DeadlineExpired}) {
		SCOPED_TRACE(static_cast<unsigned int>(reason));
		IdentityHarness ids{{{true, 0x2323U}}};
		auto controller = make_controller(ids.allocator);
		const auto begin = establish_session_begin(
			controller, endpoint(), 11'500U + static_cast<std::uint8_t>(reason), 1'000U, 2'000U);
		const auto nack = nack_for_target(begin, reason, 150U + static_cast<std::uint8_t>(reason));
		const auto result = controller.ingest(endpoint(), view(nack.bytes), 3'000U, 0U, false);
		EXPECT_EQ(detail::SessionIngressDropReason::None, result.drop_reason);
		EXPECT_NE(detail::SessionIngressDisposition::Dropped, result.disposition);
		EXPECT_NE(detail::SessionIngressDisposition::Faulted, result.disposition);
		EXPECT_EQ(0U, controller.slot(0U).reliable_items_in_use);
		EXPECT_FALSE(controller.has_output());
		expect_session_begin_terminal_state(controller.slot(0U));
	}

	IdentityHarness ids{{{true, 0x2424U}}};
	auto controller = make_controller(ids.allocator);
	const auto begin = establish_session_begin(controller, endpoint(), 11'999U, 1'000U, 2'000U);
	const auto before = controller.owned_usage();
	const auto progress = controller.slot(0U).progress;
	const auto nack = nack_for_target(begin, protocol::NackReason::UnsupportedMessage, 199U);
	const auto result = controller.ingest(endpoint(), view(nack.bytes), 3'000U, 0U, false);
	EXPECT_EQ(detail::SessionIngressDropReason::PayloadInvalid, result.drop_reason);
	EXPECT_EQ(detail::SessionIngressDisposition::Dropped, result.disposition);
	EXPECT_EQ(before, controller.owned_usage());
	EXPECT_EQ(progress, controller.slot(0U).progress);
	EXPECT_EQ(1U, controller.slot(0U).reliable_items_in_use);
	EXPECT_FALSE(controller.has_output());
	expect_no_session_begin_terminal_state(controller.slot(0U));
}

template <typename Controller>
void expect_output_ownership_survives_unrelated_close()
{
	IdentityHarness ids{{{true, 0x3333U}, {true, 0x3434U}}};
	auto controller = make_controller(ids.allocator, nullptr, 2U);
	const auto first = endpoint(2U, 42043U);
	const auto second = endpoint(3U, 42044U);
	const auto first_begin = establish_session_begin(controller, first, 12'001U, 1'000U, 2'000U);
	(void)establish_session_begin(controller, second, 12'002U, 3'000U, 4'000U);
	const auto nack = nack_for_target(first_begin, protocol::NackReason::MissingFragments, 200U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(first, view(nack.bytes), 5'000U, 0U, false).disposition);
	ASSERT_TRUE(controller.has_output());
	if constexpr (has_wp06_reliability_service_and_transactional_egress<Controller>::value) {
		detail::SessionControllerOutput before;
		ASSERT_TRUE(controller.peek_output(before));
		ASSERT_EQ(first, before.endpoint);
		ASSERT_TRUE(controller.close_slot(1U, detail::SessionCloseReason::MissionDiscontinuity));
		detail::SessionControllerOutput after;
		ASSERT_TRUE(controller.peek_output(after));
		EXPECT_EQ(before.endpoint, after.endpoint);
		EXPECT_EQ(before.size, after.size);
		EXPECT_TRUE(std::equal(before.bytes.begin(),
			before.bytes.begin() + static_cast<std::ptrdiff_t>(before.size),
			after.bytes.begin()))
			<< "Closing client B must preserve client A's queued datagram byte-for-byte.";
		ASSERT_TRUE(controller.close_slot(0U, detail::SessionCloseReason::MissionDiscontinuity));
		EXPECT_FALSE(controller.has_output()) << "Closing owner A must purge A's queued retransmission.";
	} else {
		FAIL() << "Transactional peek is required to prove output ownership across slot cleanup.";
	}
}

TEST(TelemetryWp06ReliabilityContract, QueuedOutputIsPurgedOnlyWhenItsOwningSlotCloses)
{
	EXPECT_TRUE(has_wp06_reliability_service_and_transactional_egress<detail::SessionController>::value);
	expect_output_ownership_survives_unrelated_close<detail::SessionController>();
}

TEST(TelemetryWp06ReliabilityContract, WelcomeWouldBlockAtExactDeadlinePurgesOutputAndClosesOwner)
{
	IdentityHarness ids{{{true, 0x3535U}}};
	auto controller = make_controller(ids.allocator);
	const auto request = hello(12'100U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(endpoint(), view(request.bytes), 1'000U, 0U, false).disposition);
	ASSERT_EQ(1U, controller.slot(0U).reliable_items_in_use);
	detail::SessionControllerOutput before;
	ASSERT_TRUE(controller.peek_output(before));
	controller.complete_output(detail::IoStatus::WouldBlock);
	detail::SessionControllerOutput blocked;
	ASSERT_TRUE(controller.peek_output(blocked));
	ASSERT_EQ(before.endpoint, blocked.endpoint);
	ASSERT_EQ(before.size, blocked.size);
	ASSERT_TRUE(std::equal(before.bytes.begin(),
		before.bytes.begin() + static_cast<std::ptrdiff_t>(before.size),
		blocked.bytes.begin()));

	controller.service_reliability(1'000U + protocol::ReliableOrdinaryRetentionUs);
	EXPECT_FALSE(controller.has_output()) << "An expired WELCOME must never remain sendable after WouldBlock.";
	EXPECT_EQ(0U, controller.active_slots());
	EXPECT_EQ(0U, controller.owned_usage().reliable_items);
	EXPECT_FALSE(controller.owned_usage().output_queued);
}

TEST(TelemetryWp06ReliabilityContract, SessionBeginRetryWouldBlockAtDeadlinePurgesWithoutSequenceConsumption)
{
	IdentityHarness ids{{{true, 0x3636U}}};
	auto controller = make_controller(ids.allocator);
	const auto begin = establish_session_begin(controller, endpoint(), 12'200U, 1'000U, 2'000U);
	const auto nack = nack_for_target(begin, protocol::NackReason::MissingFragments, 250U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(endpoint(), view(nack.bytes), 3'000U, 0U, false).disposition);
	detail::SessionControllerOutput retry;
	ASSERT_TRUE(controller.peek_output(retry));
	protocol::DatagramView retry_view;
	ASSERT_EQ(protocol::ValidationError::None,
		protocol::decode_and_validate_datagram({retry.bytes.data(), retry.size},
			protocol::ProtocolMinorRange{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1},
			retry_view));
	const auto sequence_before_deadline = controller.slot(0U).next_packet_sequence;
	ASSERT_EQ(sequence_before_deadline, retry_view.header.packet_sequence);
	controller.complete_output(detail::IoStatus::WouldBlock);
	detail::SessionControllerOutput blocked;
	ASSERT_TRUE(controller.peek_output(blocked));
	ASSERT_EQ(retry.size, blocked.size);
	ASSERT_TRUE(std::equal(retry.bytes.begin(),
		retry.bytes.begin() + static_cast<std::ptrdiff_t>(retry.size),
		blocked.bytes.begin()));

	controller.service_reliability(2'000U + protocol::ReliableOrdinaryRetentionUs);
	EXPECT_FALSE(controller.has_output()) << "The deadline must preempt a blocked retry owned by the same tuple.";
	EXPECT_EQ(0U, controller.slot(0U).reliable_items_in_use);
	EXPECT_EQ(sequence_before_deadline, controller.slot(0U).next_packet_sequence)
		<< "Neither WouldBlock nor deadline cleanup may commit the pending retry sequence.";
	EXPECT_EQ(1U, controller.active_slots()) << "MarkSessionStale does not close the established slot.";
	expect_session_begin_terminal_state(controller.slot(0U));
}

TEST(TelemetryWp06ReliabilityContract, CrossOwnerBlockedRetrySurvivesWelcomeOwnerDeadlineByteIdentically)
{
	IdentityHarness ids{{{true, 0x3737U}, {true, 0x3838U}}};
	auto controller = make_controller(ids.allocator, nullptr, 2U);
	const auto expiring = endpoint(2U, 42043U);
	const auto blocked_owner = endpoint(3U, 42044U);
	const auto expiring_hello = hello(12'301U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(expiring, view(expiring_hello.bytes), 1'000U, 0U, false).disposition);
	(void)pop_output(controller);
	ASSERT_EQ(detail::ProducerSessionProgress::AwaitWelcomeApplied, controller.slot(0U).progress);
	const auto blocked_begin =
		establish_session_begin(controller, blocked_owner, 12'302U, 2'000U, 3'000U);
	const auto nack = nack_for_target(blocked_begin, protocol::NackReason::MissingFragments, 260U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(blocked_owner, view(nack.bytes), 4'000U, 0U, false).disposition);
	detail::SessionControllerOutput before;
	ASSERT_TRUE(controller.peek_output(before));
	ASSERT_EQ(blocked_owner, before.endpoint);
	controller.complete_output(detail::IoStatus::WouldBlock);
	const auto blocked_sequence = controller.slot(1U).next_packet_sequence;
	const auto blocked_session = controller.slot(1U).session_id;
	ASSERT_EQ(1U, controller.slot(0U).reliable_items_in_use);
	ASSERT_EQ(1U, controller.slot(1U).reliable_items_in_use);

	controller.service_reliability(1'000U + protocol::ReliableOrdinaryRetentionUs);
	EXPECT_EQ(1U, controller.active_slots());
	EXPECT_EQ(detail::ProducerSessionProgress::Empty, controller.slot(0U).progress);
	EXPECT_EQ(0U, controller.slot(0U).reliable_items_in_use);
	EXPECT_EQ(detail::SessionControllerSlot{}.next_packet_sequence, controller.slot(0U).next_packet_sequence)
		<< "CloseSession resets A; it must not expose a committed deadline retry sequence.";
	EXPECT_EQ(blocked_session, controller.slot(1U).session_id);
	EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState, controller.slot(1U).progress);
	EXPECT_EQ(1U, controller.slot(1U).reliable_items_in_use);
	EXPECT_EQ(blocked_sequence, controller.slot(1U).next_packet_sequence);
	expect_no_session_begin_terminal_state(controller.slot(1U));
	detail::SessionControllerOutput after;
	ASSERT_TRUE(controller.peek_output(after));
	EXPECT_EQ(before.endpoint, after.endpoint);
	EXPECT_EQ(before.size, after.size);
	EXPECT_TRUE(std::equal(before.bytes.begin(),
		before.bytes.begin() + static_cast<std::ptrdiff_t>(before.size),
		after.bytes.begin()));
}

TEST(TelemetryWp06ReliabilityContract, CrossOwnerBlockedRetrySurvivesSessionBeginStaleDeadlineByteIdentically)
{
	IdentityHarness ids{{{true, 0x3939U}, {true, 0x3a3aU}}};
	auto controller = make_controller(ids.allocator, nullptr, 2U);
	const auto expiring = endpoint(2U, 42043U);
	const auto blocked_owner = endpoint(3U, 42044U);
	(void)establish_session_begin(controller, expiring, 12'401U, 1'000U, 2'000U);
	const auto blocked_begin =
		establish_session_begin(controller, blocked_owner, 12'402U, 3'000U, 4'000U);
	const auto nack = nack_for_target(blocked_begin, protocol::NackReason::MissingFragments, 270U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(blocked_owner, view(nack.bytes), 5'000U, 0U, false).disposition);
	detail::SessionControllerOutput before;
	ASSERT_TRUE(controller.peek_output(before));
	ASSERT_EQ(blocked_owner, before.endpoint);
	controller.complete_output(detail::IoStatus::WouldBlock);
	const auto expiring_sequence = controller.slot(0U).next_packet_sequence;
	const auto blocked_sequence = controller.slot(1U).next_packet_sequence;
	const auto blocked_session = controller.slot(1U).session_id;
	ASSERT_EQ(1U, controller.slot(0U).reliable_items_in_use);
	ASSERT_EQ(1U, controller.slot(1U).reliable_items_in_use);

	controller.service_reliability(2'000U + protocol::ReliableOrdinaryRetentionUs);
	EXPECT_EQ(2U, controller.active_slots());
	EXPECT_EQ(0U, controller.slot(0U).reliable_items_in_use);
	EXPECT_EQ(expiring_sequence, controller.slot(0U).next_packet_sequence);
	expect_session_begin_terminal_state(controller.slot(0U));
	EXPECT_EQ(blocked_session, controller.slot(1U).session_id);
	EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState, controller.slot(1U).progress);
	EXPECT_EQ(1U, controller.slot(1U).reliable_items_in_use);
	EXPECT_EQ(blocked_sequence, controller.slot(1U).next_packet_sequence);
	expect_no_session_begin_terminal_state(controller.slot(1U));
	detail::SessionControllerOutput after;
	ASSERT_TRUE(controller.peek_output(after));
	EXPECT_EQ(before.endpoint, after.endpoint);
	EXPECT_EQ(before.size, after.size);
	EXPECT_TRUE(std::equal(before.bytes.begin(),
		before.bytes.begin() + static_cast<std::ptrdiff_t>(before.size),
		after.bytes.begin()));
}

template <typename Controller>
void expect_wp06_reliability_service_boundaries_and_egress()
{
	if constexpr (!has_wp06_reliability_service_and_transactional_egress<Controller>::value) {
		FAIL() << "SessionController lacks noexcept service_reliability/peek_output/complete_output seams.";
	} else {
		IdentityHarness ids{{{true, 0x4444U}}};
		auto controller = make_controller(ids.allocator);
		const auto request = hello(13'000U);
		ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
			controller.ingest(endpoint(), view(request.bytes), 1'000U, 0U, false).disposition);
		detail::SessionControllerOutput original;
		ASSERT_TRUE(controller.peek_output(original));
		controller.complete_output(detail::IoStatus::WouldBlock);
		EXPECT_TRUE(controller.has_output());
		detail::SessionControllerOutput blocked;
		ASSERT_TRUE(controller.peek_output(blocked));
		EXPECT_EQ(original.size, blocked.size);
		EXPECT_TRUE(std::equal(original.bytes.begin(), original.bytes.begin() + static_cast<std::ptrdiff_t>(original.size),
			blocked.bytes.begin()));
		controller.complete_output(detail::IoStatus::Complete);
		EXPECT_FALSE(controller.has_output());

		protocol::DatagramView welcome;
		ASSERT_EQ(protocol::ValidationError::None,
			protocol::decode_and_validate_datagram({original.bytes.data(), original.size},
				protocol::ProtocolMinorRange{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1},
				welcome));
		const auto due = 1'000U + protocol::reliable_retry_delay_us(protocol::ReliableDefaultRtoUs,
			0x4653544c5f52544fULL,
			welcome.header.session_id,
			welcome.header.message_id,
			0U);
		controller.service_reliability(due - 1U);
		EXPECT_FALSE(controller.has_output());
		controller.service_reliability(due);
		ASSERT_TRUE(controller.has_output());
		const auto retry = pop_output(controller);
		EXPECT_EQ(welcome.header.message_id, retry.datagram.header.message_id);
		EXPECT_EQ(welcome.header.message_crc32, retry.datagram.header.message_crc32);
		EXPECT_NE(welcome.header.packet_sequence, retry.datagram.header.packet_sequence);
		EXPECT_NE(static_cast<std::uint8_t>(0U),
			static_cast<std::uint8_t>(retry.datagram.header.flags & protocol::MessageFlagRetransmission));

		controller.service_reliability(1'000U + protocol::ReliableOrdinaryRetentionUs);
		EXPECT_EQ(0U, controller.active_slots());
		EXPECT_FALSE(controller.has_output());
	}
}

TEST(TelemetryWp06ReliabilityContract, ServiceRtoDeadlineAndWouldBlockAreBoundedAndTransactional)
{
	EXPECT_TRUE(has_wp06_reliability_service_and_transactional_egress<detail::SessionController>::value);
	expect_wp06_reliability_service_boundaries_and_egress<detail::SessionController>();
}

template <typename Controller>
void expect_due_reliable_clients_make_progress()
{
	if constexpr (has_wp06_reliability_service_and_transactional_egress<Controller>::value) {
		IdentityHarness ids{{{true, 0x5555U}, {true, 0x6666U}}};
		auto controller = make_controller(ids.allocator, nullptr, 2U);
		const auto first = endpoint(2U, 42043U);
		const auto second = endpoint(3U, 42044U);
		for (const auto item : {std::pair{first, 14'001U}, std::pair{second, 14'002U}}) {
			const auto request = hello(item.second);
			ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
				controller.ingest(item.first, view(request.bytes), 1'000U, 0U, false).disposition);
			controller.complete_output(detail::IoStatus::Complete);
		}
		controller.service_reliability(1'000'000U);
		detail::SessionControllerOutput first_retry;
		ASSERT_TRUE(controller.peek_output(first_retry));
		controller.complete_output(detail::IoStatus::Complete);
		controller.service_reliability(1'000'000U);
		detail::SessionControllerOutput second_retry;
		ASSERT_TRUE(controller.peek_output(second_retry));
		EXPECT_NE(first_retry.endpoint, second_retry.endpoint);
	} else {
		FAIL() << "The missing reliability service prevents a bounded multi-client fairness proof.";
	}
}

TEST(TelemetryWp06ReliabilityContract, DueReliableClientsMakeProgressOneDatagramPerServiceCall)
{
	expect_due_reliable_clients_make_progress<detail::SessionController>();
}

} // namespace
