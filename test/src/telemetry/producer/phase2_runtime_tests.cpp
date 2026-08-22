#include "telemetry/phase2_runtime.h"
#include "telemetry/phase2_session_transition.h"
#include "telemetry/phase1_state_image.h"
#include "telemetry/logging.h"
#include "telemetry/protocol/telemetry_control_messages.h"
#include "telemetry/protocol/telemetry_crc32.h"
#include "telemetry/protocol/telemetry_datagram.h"
#include "telemetry/protocol/telemetry_reliability_messages.h"
#include "telemetry/protocol/telemetry_reassembler.h"
#include "telemetry/session_controller.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

namespace {

namespace detail = telemetry::detail;
namespace protocol = telemetry::protocol;

TEST(TelemetryPhase2Runtime, DeliveredLogsExposeManifestAndCaptureCause)
{
	detail::TelemetryStructuredLog log;
	std::array<char, detail::TelemetryLogLineCapacity> line{};

	log.phase2_manifest(0U,
		detail::TelemetryLogEvent::Phase2ManifestInstalled,
		7U, 33U, 2U, 2048U, 123U);
	auto snapshot = log.snapshot();
	ASSERT_EQ(1U, snapshot.count);
	ASSERT_TRUE(detail::format_telemetry_log_record(
		snapshot.records[0], line));
	const std::string_view manifest{line.data()};
	EXPECT_NE(std::string_view::npos, manifest.find("event=14"));
	EXPECT_NE(std::string_view::npos, manifest.find("generation=7"));
	EXPECT_NE(std::string_view::npos, manifest.find("records=33"));
	EXPECT_NE(std::string_view::npos, manifest.find("parts=2"));
	EXPECT_NE(std::string_view::npos, manifest.find("bytes=2048"));
	EXPECT_NE(std::string_view::npos, manifest.find("duration_us=123"));

	log.phase2_source_rejected(
		detail::TelemetryPhase2Block::DamageShield,
		detail::TelemetryPhase2CaptureFailure::NonFinite,
		1'000'000U);
	snapshot = log.snapshot();
	ASSERT_EQ(2U, snapshot.count);
	ASSERT_TRUE(detail::format_telemetry_log_record(
		snapshot.records[1], line));
	const std::string_view rejection{line.data()};
	EXPECT_NE(std::string_view::npos, rejection.find("event=19"));
	EXPECT_NE(std::string_view::npos, rejection.find("p2_block=3"));
	EXPECT_NE(std::string_view::npos,
		rejection.find("p2_capture_failure=1"));
	EXPECT_NE(std::string_view::npos, rejection.find("value=1"));

	detail::TelemetryLogRecord maximum{};
	maximum.platform_code = std::numeric_limits<std::uint32_t>::max();
	maximum.local_generation = std::numeric_limits<std::uint32_t>::max();
	maximum.record_count = std::numeric_limits<std::uint32_t>::max();
	maximum.part_count = std::numeric_limits<std::uint16_t>::max();
	maximum.bytes = std::numeric_limits<std::uint64_t>::max();
	maximum.duration_us = std::numeric_limits<std::uint64_t>::max();
	maximum.value = std::numeric_limits<std::uint64_t>::max();
	maximum.limit = std::numeric_limits<std::uint64_t>::max();
	maximum.high_water = std::numeric_limits<std::uint64_t>::max();
	maximum.drops.fill(std::numeric_limits<std::uint64_t>::max());
	ASSERT_TRUE(detail::format_telemetry_log_record(maximum, line));
	EXPECT_LT(std::string_view{line.data()}.size(),
		detail::TelemetryLogLineCapacity);
}

protocol::Sha256Digest digest(std::uint8_t value)
{
	protocol::Sha256Digest result{};
	result[0] = value;
	return result;
}

template <typename Container>
protocol::ByteView view(const Container& bytes)
{
	return {reinterpret_cast<const std::uint8_t*>(bytes.data()),
		bytes.size()};
}

template <typename Container>
protocol::MutableByteView mutable_view(Container& bytes)
{
	return {reinterpret_cast<std::uint8_t*>(bytes.data()),
		bytes.size()};
}

protocol::EndpointKey endpoint(
	std::uint8_t last_octet, std::uint16_t port)
{
	return protocol::EndpointKey::from_ipv4(
		{127U, 0U, 0U, last_octet}, port);
}

struct ScriptedRandomSource final : detail::RandomSource {
	struct Draw {
		bool succeeds = false;
		std::uint64_t value = 0U;
	};
	std::vector<Draw> draws;
	std::size_t cursor = 0U;

	explicit ScriptedRandomSource(
		std::initializer_list<Draw> values) : draws(values)
	{
	}

	bool next_u64(std::uint64_t& output) noexcept override
	{
		if (cursor >= draws.size())
			return false;
		const auto draw = draws[cursor++];
		output = draw.value;
		return draw.succeeds;
	}
};

struct ControllerIdentity {
	ScriptedRandomSource random{{{true, 0x1111U},
		{true, 0x2222U}}};
	detail::SessionIdRegistry registry;
	detail::SessionIdAllocator allocator;

	ControllerIdentity() : allocator(random, registry)
	{
		EXPECT_TRUE(registry.allocate_storage());
	}
};

struct PacketSequences final : detail::RandomSource {
	std::uint64_t next = 0x10203040U;
	bool next_u64(std::uint64_t& output) noexcept override
	{
		output = next++;
		return true;
	}
};

std::vector<std::uint8_t> hello(std::uint64_t nonce)
{
	protocol::HelloPayload payload;
	payload.client_nonce = nonce;
	payload.client_send_t0_us = 1'000U;
	payload.min_major = protocol::VersionMajor;
	payload.max_major = protocol::VersionMajor;
	payload.min_minor = protocol::VersionMinor;
	payload.max_minor = protocol::VersionMinor;
	payload.requested_visibility_mode =
		protocol::VisibilityMode::Cockpit;
	payload.requested_heartbeat_ms = 1'000U;
	std::array<std::uint8_t,
		protocol::HelloPayloadPrefixSize> encoded_payload{};
	std::size_t payload_size = 0U;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::encode_hello_payload(payload,
			mutable_view(encoded_payload), payload_size));

	protocol::TelemetryDatagramHeader header;
	header.version_minor = protocol::VersionMinor;
	header.message_type = protocol::MessageType::Hello;
	header.packet_sequence = 1U;
	header.sent_time_us = payload.client_send_t0_us;
	header.message_id = 1U;
	header.message_size =
		static_cast<std::uint32_t>(payload_size);
	header.message_crc32 = protocol::crc32_iso_hdlc(
		{encoded_payload.data(), payload_size});
	std::vector<std::uint8_t> encoded(
		protocol::HeaderSizeV1 + payload_size);
	std::size_t written = 0U;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::encode_datagram(header,
			{protocol::VersionMinor,
				protocol::VersionMinor},
			{encoded_payload.data(), payload_size},
			mutable_view(encoded), written));
	EXPECT_EQ(encoded.size(), written);
	return encoded;
}

detail::SessionController make_controller(
	ControllerIdentity& identity)
{
	detail::SessionControllerConfig config;
	config.max_clients = 2U;
	config.producer_id = 0x1020304050607080ULL;
	config.security.enabled = true;
	config.security.port = 42042U;
	config.security.bind_mode =
		protocol::NetworkBindMode::LoopbackOnly;
	config.security.resources.max_clients = 2U;
	config.security.resources.global_state_reassembly_bytes =
		2U * protocol::MaxStateReassemblyBytesPerClient;
	static PacketSequences sequences;
	detail::SessionController controller;
	EXPECT_EQ(detail::SessionControllerConfigureResult::Ready,
		detail::SessionController::configure(config,
			identity.allocator, sequences, 0U, nullptr,
			controller));
	return controller;
}

protocol::DatagramView decode_output(
	const detail::SessionControllerOutput& output)
{
	protocol::DatagramView decoded;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::decode_and_validate_datagram(
			{output.bytes.data(), output.size},
			{protocol::VersionMinor,
				protocol::VersionMinor}, decoded));
	return decoded;
}

std::vector<std::uint8_t> applied_ack(
	const protocol::DatagramView& target,
	std::uint32_t packet_sequence,
	std::uint8_t ack_flags = protocol::KnownAckFlags)
{
	protocol::AckPayload ack;
	ack.target_message_id = target.header.message_id;
	ack.target_message_type = target.header.message_type;
	ack.ack_flags = ack_flags;
	ack.target_fragment_count =
		target.header.fragment_count;
	ack.target_message_crc32 =
		target.header.message_crc32;
	std::array<std::uint8_t,
		protocol::AckPayloadSize> payload{};
	std::size_t payload_size = 0U;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::encode_ack_payload(
			ack, mutable_view(payload), payload_size));
	protocol::TelemetryDatagramHeader header;
	header.version_minor = protocol::VersionMinor;
	header.message_type = protocol::MessageType::Ack;
	header.session_id = target.header.session_id;
	header.packet_sequence = packet_sequence;
	header.sent_time_us = 2'000U;
	header.message_id = packet_sequence;
	header.message_size =
		static_cast<std::uint32_t>(payload_size);
	header.message_crc32 = protocol::crc32_iso_hdlc(
		{payload.data(), payload_size});
	std::vector<std::uint8_t> encoded(
		protocol::HeaderSizeV1 + payload_size);
	std::size_t written = 0U;
	EXPECT_EQ(protocol::ValidationError::None,
		protocol::encode_datagram(header,
			{protocol::VersionMinor,
				protocol::VersionMinor},
			{payload.data(), payload_size},
			mutable_view(encoded), written));
	return encoded;
}

void establish_ready(detail::SessionController& controller,
	const protocol::EndpointKey& peer, std::uint64_t nonce,
	std::uint64_t now_us, std::size_t expected_slot)
{
	const auto request = hello(nonce);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(peer, view(request), now_us, 1U,
			true).disposition);
	detail::SessionControllerOutput welcome;
	ASSERT_TRUE(controller.pop_output(welcome));
	const auto welcome_view = decode_output(welcome);
	const auto ack = applied_ack(welcome_view,
		static_cast<std::uint32_t>(nonce));
	ASSERT_EQ(detail::SessionIngressDisposition::WelcomeProofApplied,
		controller.ingest(peer, view(ack), now_us + 1U, 1U,
			true).disposition);
	detail::SessionControllerOutput session_begin;
	ASSERT_TRUE(controller.pop_output(session_begin));
	ASSERT_EQ(protocol::MessageType::SessionBegin,
		decode_output(session_begin).header.message_type);
	ASSERT_EQ(detail::ProducerSessionProgress::ReadyForState,
		controller.slot(expected_slot).progress);
}

protocol::StateImage one_atom_image(std::uint8_t value)
{
	protocol::StateAtom atom;
	atom.key.record_type = 1U;
	atom.value = {value};
	protocol::StateImage image;
	EXPECT_EQ(protocol::StateImageResult::Created,
		protocol::StateImage::create(
			std::vector<protocol::StateAtom>{atom}, image));
	return image;
}

protocol::StateImage delta_compatible_image(
	float player_x, std::uint64_t sample_time_us)
{
	detail::Phase1StateImageInput input{};
	input.producer_id = 0x1020304050607080ULL;
	input.negotiated_capability_generation = 3U;
	input.mission.producer_sample_time_us = sample_time_us;
	input.mission.mission_generation = 1U;
	input.mission.phase = protocol::MissionPhase::Active;
	input.mission.time_compression = 1.0F;
	input.player_capture = {
		detail::CaptureStatus::Valid,
		detail::CaptureReason::None};
	input.player.entity_id = 42U;
	input.player.value.producer_sample_time_us = sample_time_us;
	input.player.value.position_world =
		{player_x, 2.0F, 3.0F};
	input.player.value.orientation_local_to_world =
		{1.0F, 0.0F, 0.0F, 0.0F};
	input.player.value.radius = 1.0F;
	protocol::StateImage image;
	EXPECT_EQ(detail::Phase1StateImageBuildStatus::Created,
		detail::build_phase1_state_image(input, image));
	return image;
}

protocol::StateImage multipart_runtime_image(bool mutate_first_player)
{
	const auto base =
		delta_compatible_image(4.0F, 3'000U);
	const auto changed =
		delta_compatible_image(5.0F, 3'001U);
	const auto base_flight = std::find_if(
		base.records().begin(), base.records().end(),
		[](const auto& atom) {
			return atom.key.record_type ==
				static_cast<std::uint16_t>(
					protocol::RecordType::FlightState);
		});
	const auto changed_flight = std::find_if(
		changed.records().begin(), changed.records().end(),
		[](const auto& atom) {
			return atom.key.record_type ==
				static_cast<std::uint16_t>(
					protocol::RecordType::FlightState);
		});
	const auto base_lifecycle = std::find_if(
		base.records().begin(), base.records().end(),
		[](const auto& atom) {
			return atom.key.record_type ==
				static_cast<std::uint16_t>(
					protocol::RecordType::EntityLifecycle);
		});
	if (base_flight == base.records().end() ||
		base_lifecycle == base.records().end() ||
		changed_flight == changed.records().end())
		return {};
	std::vector<protocol::StateAtom> atoms;
	atoms.reserve(20'000U);
	for (std::uint64_t index = 1U; index <= 10'000U; ++index) {
		auto lifecycle = *base_lifecycle;
		auto flight = *base_flight;
		if (mutate_first_player && index == 1U)
			flight.value = changed_flight->value;
		for (auto* atom : {&lifecycle, &flight}) {
			atom->key.identity.assign(sizeof(index), 0U);
			for (std::size_t byte = 0U;
				 byte < sizeof(index); ++byte) {
				atom->key.identity[byte] =
					static_cast<std::uint8_t>(
						index >> (byte * 8U));
				atom->value[byte] =
				static_cast<std::uint8_t>(index >> (byte * 8U));
			}
		}
		flight.cascade_owner = lifecycle.key;
		atoms.push_back(std::move(lifecycle));
		atoms.push_back(std::move(flight));
	}
	protocol::StateImage image;
	EXPECT_EQ(protocol::StateImageResult::Created,
		protocol::StateImage::create(std::move(atoms), image));
	EXPECT_GT(image.encoded_snapshot_records_size(),
		detail::Phase2ReplicationPartBytes);
	return image;
}

struct ManifestPair {
	std::unique_ptr<std::vector<std::uint8_t>> active;
	std::unique_ptr<std::vector<std::uint8_t>> staged;
	std::unique_ptr<telemetry::Phase2ManifestStorage> storage;
	std::unique_ptr<telemetry::Phase2ManifestSlot> slot;
	std::unique_ptr<telemetry::Phase2ManifestSource> source;
	std::unique_ptr<telemetry::Phase2ManifestCandidate> first;
	std::unique_ptr<telemetry::Phase2ManifestCandidate> second;

	bool initialize()
	{
		active = std::make_unique<std::vector<std::uint8_t>>(
			protocol::MaxTransactionSize);
		staged = std::make_unique<std::vector<std::uint8_t>>(
			protocol::MaxTransactionSize);
		storage = std::make_unique<telemetry::Phase2ManifestStorage>(
			protocol::MutableByteView{active->data(), active->size()},
			protocol::MutableByteView{staged->data(), staged->size()});
		slot = std::make_unique<telemetry::Phase2ManifestSlot>(*storage);
		source = std::make_unique<telemetry::Phase2ManifestSource>();
		auto& ship_class = source->ship_classes[0];
		ship_class.source_key = 1U;
		ship_class.name = "p2-tst-026-class-n";
		ship_class.model_mass = 100.0F;
		ship_class.density_provenance = 1.0F;
		ship_class.effective_mass = 100.0F;
		source->ship_class_count = 1U;
		source->referenced_ship_class_keys[0] = 1U;
		source->referenced_ship_class_count = 1U;
		source->player_instance_signature = 1U;
		if (slot->rebuild(*source) !=
			telemetry::Phase2ManifestError::None)
			return false;
		first = std::make_unique<telemetry::Phase2ManifestCandidate>(
			slot->staged_candidate());
		if (slot->on_manifest_applied(first->manifest_id) !=
				telemetry::Phase2ManifestError::None ||
			slot->on_dependent_snapshot_applied(
				1U, first->manifest_id) !=
				telemetry::Phase2ManifestError::None)
			return false;
		ship_class.name = "p2-tst-026-class-n-plus-one";
		if (slot->rebuild(*source) !=
			telemetry::Phase2ManifestError::None)
			return false;
		second = std::make_unique<telemetry::Phase2ManifestCandidate>(
			slot->staged_candidate());
		return first->manifest_id == 1U && second->manifest_id == 2U;
	}
};

TEST(Phase2Runtime,
	TST026ControllerManifestStageIsTransactionalBehindSnapshotCandidate)
{
	ControllerIdentity identity;
	auto controller = make_controller(identity);
	const auto peer = endpoint(2U, 42043U);
	establish_ready(controller, peer, 0x260U, 1'000U, 0U);

	auto manifests = std::make_unique<ManifestPair>();
	ASSERT_TRUE(manifests->initialize());
	const auto& first = *manifests->first;
	ASSERT_EQ(detail::Phase2RuntimeResult::ManifestRequired,
		controller.stage_phase2_manifest(0U, first, 2'000U));
	ASSERT_EQ(1U, controller.service_phase2_manifest_egress(
		0U, 2'001U));
	detail::SessionControllerOutput manifest_output;
	ASSERT_TRUE(controller.pop_output(manifest_output));
	const auto manifest_view = decode_output(manifest_output);
	ASSERT_EQ(protocol::MessageType::Manifest,
		manifest_view.header.message_type);
	const auto manifest_ack = applied_ack(manifest_view, 0x261U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(peer, view(manifest_ack), 2'002U, 1U,
			true).disposition);

	const auto initial_image = one_atom_image(0x22U);
	ASSERT_TRUE(controller.begin_phase2_snapshot(0U, initial_image,
		detail::Phase2RuntimeSnapshotCause::Initial, 3'000U));
	ASSERT_EQ(1U, controller.service_initial_snapshot_egress(
		1U, 3'001U));
	detail::SessionControllerOutput initial_output;
	ASSERT_TRUE(controller.pop_output(initial_output));
	const auto initial_view = decode_output(initial_output);
	ASSERT_EQ(protocol::MessageType::FullSnapshot,
		initial_view.header.message_type);
	const auto initial_ack = applied_ack(initial_view, 0x262U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(peer, view(initial_ack), 3'002U, 1U,
			true).disposition);
	ASSERT_EQ(1U,
		controller.slot(0U).phase2_runtime.manifest_state().active_id);
	ASSERT_EQ(0U,
		controller.slot(0U).phase2_runtime.manifest_state().staged_id);

	ASSERT_TRUE(controller.begin_phase2_snapshot(0U, initial_image,
		detail::Phase2RuntimeSnapshotCause::Periodic, 4'000U));
	const auto active_before =
		controller.slot(0U).phase2_runtime.manifest_state().active_id;
	const auto staged_before =
		controller.slot(0U).phase2_runtime.manifest_state().staged_id;
	const auto required_before =
		controller.slot(0U).required_manifest_id;
	const auto candidate_snapshot_before =
		controller.slot(0U).snapshot.candidate_snapshot_id();

	const auto& second = *manifests->second;
	ASSERT_EQ(detail::Phase2RuntimeResult::ManifestRequired,
		controller.slot(0U).phase2_runtime.preview_stage_manifest(
			second.manifest_id, second.catalog_fingerprint));
	ASSERT_EQ(detail::Phase2RuntimeResult::CandidateBusy,
		controller.stage_phase2_manifest(0U, second, 4'001U));
	EXPECT_EQ(active_before,
		controller.slot(0U).phase2_runtime.manifest_state().active_id);
	EXPECT_EQ(staged_before,
		controller.slot(0U).phase2_runtime.manifest_state().staged_id);
	EXPECT_EQ(required_before,
		controller.slot(0U).required_manifest_id);
	EXPECT_EQ(candidate_snapshot_before,
		controller.slot(0U).snapshot.candidate_snapshot_id());
	EXPECT_EQ(detail::Phase2RuntimeResult::ManifestRequired,
		controller.slot(0U).phase2_runtime.preview_stage_manifest(
			second.manifest_id, second.catalog_fingerprint))
		<< "CandidateBusy must not consume the catalog generation.";

	ASSERT_EQ(1U, controller.service_initial_snapshot_egress(
		1U, 4'002U));
	detail::SessionControllerOutput replacement_output;
	ASSERT_TRUE(controller.pop_output(replacement_output));
	const auto replacement_view = decode_output(replacement_output);
	ASSERT_EQ(protocol::MessageType::FullSnapshot,
		replacement_view.header.message_type);
	const auto replacement_ack = applied_ack(replacement_view, 0x263U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(peer, view(replacement_ack), 4'003U, 1U,
			true).disposition);

	ASSERT_EQ(detail::Phase2RuntimeResult::ManifestRequired,
		controller.stage_phase2_manifest(0U, second, 4'004U));
	EXPECT_TRUE(controller.slot(0U).snapshot_egress.has_candidate());
	EXPECT_EQ(protocol::MessageType::Manifest,
		controller.slot(0U).snapshot_egress.candidate_message_type());
	EXPECT_EQ(2U,
		controller.slot(0U).phase2_runtime.manifest_state().staged_id);
	EXPECT_EQ(detail::Phase2RuntimeResult::NoChange,
		controller.stage_phase2_manifest(0U, second, 4'005U));
	ASSERT_EQ(1U, controller.service_phase2_manifest_egress(
		0U, 4'006U));
	detail::SessionControllerOutput retry_output;
	ASSERT_TRUE(controller.pop_output(retry_output));
	EXPECT_EQ(protocol::MessageType::Manifest,
		decode_output(retry_output).header.message_type);
}

TEST(Phase2Runtime,
	CockpitManifestAppliedProofOrdersAndUnlocksTheDependentSnapshot)
{
	ControllerIdentity identity;
	auto controller = make_controller(identity);
	const auto peer = endpoint(2U, 42043U);
	establish_ready(controller, peer, 0x268U, 1'000U, 0U);
	const auto session_id = controller.slot(0U).session_id;

	auto manifests = std::make_unique<ManifestPair>();
	ASSERT_TRUE(manifests->initialize());
	const auto& manifest = *manifests->first;
	ASSERT_EQ(detail::Phase2RuntimeResult::ManifestRequired,
		controller.stage_phase2_manifest(
			0U, manifest, 2'000U));
	EXPECT_FALSE(controller.slot(0U).required_manifest_applied);
	EXPECT_FALSE(controller.begin_phase2_snapshot(
		0U, one_atom_image(0x31U),
		detail::Phase2RuntimeSnapshotCause::Initial, 2'001U));

	ASSERT_EQ(1U, controller.service_phase2_manifest_egress(
		0U, 2'002U));
	detail::SessionControllerOutput manifest_output;
	ASSERT_TRUE(controller.pop_output(manifest_output));
	const auto manifest_view = decode_output(manifest_output);
	ASSERT_EQ(protocol::MessageType::Manifest,
		manifest_view.header.message_type);
	protocol::ManifestPartPayload manifest_part;
	ASSERT_EQ(protocol::ValidationError::None,
		protocol::decode_manifest_part_payload(
			manifest_view.payload, manifest_part));
	EXPECT_EQ(manifest.manifest_id, manifest_part.manifest_id);

	const auto validated = applied_ack(
		manifest_view, 0x269U,
		static_cast<std::uint8_t>(
			protocol::AckFlag::Validated));
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(peer, view(validated), 2'003U, 1U,
			true).disposition);
	EXPECT_FALSE(controller.slot(0U).required_manifest_applied);
	EXPECT_TRUE(controller.slot(0U).snapshot_egress.has_candidate());
	EXPECT_EQ(protocol::MessageType::Manifest,
		controller.slot(0U).snapshot_egress.candidate_message_type());
	EXPECT_FALSE(controller.begin_phase2_snapshot(
		0U, one_atom_image(0x32U),
		detail::Phase2RuntimeSnapshotCause::Initial, 2'004U));

	const auto applied =
		applied_ack(manifest_view, 0x26aU);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(peer, view(applied), 2'005U, 1U,
			true).disposition);
	EXPECT_TRUE(controller.slot(0U).required_manifest_applied);
	EXPECT_FALSE(controller.slot(0U).snapshot_egress.has_candidate());

	const auto image = one_atom_image(0x33U);
	ASSERT_TRUE(controller.begin_phase2_snapshot(
		0U, image,
		detail::Phase2RuntimeSnapshotCause::Initial, 2'006U));
	ASSERT_EQ(1U, controller.service_initial_snapshot_egress(
		1U, 2'007U));
	detail::SessionControllerOutput snapshot_output;
	ASSERT_TRUE(controller.pop_output(snapshot_output));
	const auto snapshot_view = decode_output(snapshot_output);
	ASSERT_EQ(protocol::MessageType::FullSnapshot,
		snapshot_view.header.message_type);
	protocol::FullSnapshotPartPayload snapshot_part;
	ASSERT_EQ(protocol::ValidationError::None,
		protocol::decode_full_snapshot_part_payload(
			snapshot_view.payload, snapshot_part));
	EXPECT_EQ(manifest.manifest_id,
		snapshot_part.required_manifest_id);

	const auto snapshot_ack =
		applied_ack(snapshot_view, 0x26bU);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(peer, view(snapshot_ack), 2'008U, 1U,
			true).disposition);
	EXPECT_EQ(session_id, controller.slot(0U).session_id);
	EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState,
		controller.slot(0U).progress);
	EXPECT_FALSE(controller.slot(0U).fault_session_end_pending);
	EXPECT_TRUE(controller.slot(0U).snapshot.has_active_baseline());
	EXPECT_FALSE(controller.slot(0U).snapshot.has_candidate());
}

TEST(Phase2Runtime,
	ResyncMultipartReplacementConsumesOneGlobalMessageIdPerPart)
{
	ControllerIdentity identity;
	auto controller = make_controller(identity);
	const auto peer = endpoint(2U, 42043U);
	establish_ready(controller, peer, 0x270U, 1'000U, 0U);

	auto manifests = std::make_unique<ManifestPair>();
	ASSERT_TRUE(manifests->initialize());
	ASSERT_EQ(detail::Phase2RuntimeResult::ManifestRequired,
		controller.stage_phase2_manifest(
			0U, *manifests->first, 1'500U));
	ASSERT_EQ(1U, controller.service_phase2_manifest_egress(
		0U, 1'501U));
	detail::SessionControllerOutput manifest_output;
	ASSERT_TRUE(controller.pop_output(manifest_output));
	const auto manifest_ack = applied_ack(
		decode_output(manifest_output), 0x271U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(peer, view(manifest_ack), 1'502U, 1U,
			true).disposition);

	ASSERT_TRUE(controller.begin_phase2_snapshot(
		0U, delta_compatible_image(1.0F, 2'000U),
		detail::Phase2RuntimeSnapshotCause::Initial, 2'000U));
	ASSERT_EQ(1U, controller.service_initial_snapshot_egress(
		1U, 2'001U));
	detail::SessionControllerOutput initial_output;
	ASSERT_TRUE(controller.pop_output(initial_output));
	const auto initial_ack = applied_ack(
		decode_output(initial_output), 0x272U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(peer, view(initial_ack), 2'002U, 1U,
			true).disposition);
	ASSERT_TRUE(controller.slot(0U).snapshot.has_active_baseline());
	ASSERT_TRUE(controller.begin_phase2_snapshot(
		0U, delta_compatible_image(1.0F, 2'010U),
		detail::Phase2RuntimeSnapshotCause::Periodic, 2'010U));
	ASSERT_EQ(1U, controller.service_initial_snapshot_egress(
		1U, 2'011U));
	detail::SessionControllerOutput settled_output;
	ASSERT_TRUE(controller.pop_output(settled_output));
	const auto settled_ack = applied_ack(
		decode_output(settled_output), 0x2721U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(peer, view(settled_ack), 2'012U, 1U,
			true).disposition);
	ASSERT_EQ(controller.slot(0U).snapshot.active_snapshot_id(),
		controller.slot(0U).phase2_runtime.active_snapshot_id());

	std::uint32_t last_delta_message_id = 0U;
	for (std::uint8_t value = 1U; value <= 3U; ++value) {
		ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
			controller.replace_current_state(
				0U, delta_compatible_image(
					1.0F + value,
					2'100U + value)));
		ASSERT_TRUE(controller.queue_cumulative_delta(
			0U, 2'100U + value));
		ASSERT_TRUE(controller.slot(0U).delta_egress.has_delta());
		ASSERT_FALSE(controller.has_output());
		ASSERT_EQ(1U, controller.service_delta_egress(
			1U, 2'200U + value));
		detail::SessionControllerOutput delta_output;
		ASSERT_TRUE(controller.pop_output(delta_output));
		const auto delta = decode_output(delta_output);
		ASSERT_EQ(protocol::MessageType::Delta,
			delta.header.message_type);
		EXPECT_GT(delta.header.message_id,
			last_delta_message_id);
		last_delta_message_id = delta.header.message_id;
	}

	const auto replacement_image = multipart_runtime_image(false);
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		controller.replace_current_state(0U, replacement_image));
	protocol::ResyncRequestPayload request{
		1U, protocol::ResyncReason::UnknownBaseline,
		protocol::ResyncRequestFlagRequireFullSnapshot,
		controller.slot(0U).snapshot.active_snapshot_id(),
		0U, 3'000U};
	std::array<std::uint8_t,
		protocol::ResyncRequestPayloadSize> request_payload{};
	std::size_t request_payload_size = 0U;
	ASSERT_EQ(protocol::ValidationError::None,
		protocol::encode_resync_request_payload(request,
			mutable_view(request_payload),
			request_payload_size));
	protocol::TelemetryDatagramHeader request_header;
	request_header.version_minor = protocol::VersionMinor;
	request_header.message_type =
		protocol::MessageType::ResyncRequest;
	request_header.flags = protocol::MessageFlagAckRequired;
	request_header.session_id =
		controller.slot(0U).session_id;
	request_header.packet_sequence = 0x273U;
	request_header.sent_time_us = 3'000U;
	request_header.message_id = 0x273U;
	request_header.fragment_count = 1U;
	request_header.message_size =
		static_cast<std::uint32_t>(request_payload_size);
	request_header.message_crc32 =
		protocol::crc32_iso_hdlc(
			{request_payload.data(), request_payload_size});
	std::vector<std::uint8_t> encoded_request(
		protocol::HeaderSizeV1 + request_payload_size);
	std::size_t encoded_request_size = 0U;
	ASSERT_EQ(protocol::ValidationError::None,
		protocol::encode_datagram(request_header,
			{protocol::VersionMinor,
			 protocol::VersionMinor},
			{request_payload.data(), request_payload_size},
			mutable_view(encoded_request),
			encoded_request_size));
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(peer, view(encoded_request),
			3'000U, 1U, true).disposition);
	ASSERT_TRUE(controller.queue_cumulative_delta(0U, 3'001U, 3'001U));
	ASSERT_EQ(detail::Phase2RuntimeSnapshotCause::Resync,
		controller.slot(0U).phase2_runtime.last_started_snapshot_cause());
	detail::SessionControllerOutput resync_ack_output;
	ASSERT_TRUE(controller.pop_output(resync_ack_output));
	const auto resync_ack = decode_output(resync_ack_output);
	ASSERT_EQ(protocol::MessageType::Ack,
		resync_ack.header.message_type);
	const auto ack_message_id = resync_ack.header.message_id;
	EXPECT_GT(ack_message_id, last_delta_message_id);

	protocol::TelemetryReassembler reassembler;
	std::vector<protocol::TelemetryDatagramHeader> part_targets;
	std::uint16_t expected_part_count = 0U;
	std::size_t completed_parts = 0U;
	for (std::size_t iteration = 0U; iteration < 20'000U;
		 ++iteration) {
		if (controller.service_initial_snapshot_egress(
				1U, 3'001U + iteration) == 0U)
			break;
		detail::SessionControllerOutput output;
		ASSERT_TRUE(controller.pop_output(output));
		const auto fragment = decode_output(output);
		ASSERT_EQ(protocol::MessageType::FullSnapshot,
			fragment.header.message_type);
		protocol::ReassembledMessage completed;
		const auto result =
			reassembler.ingest(fragment, completed);
		ASSERT_NE(protocol::ReassemblyResult::InconsistentFragment,
			result);
		ASSERT_TRUE(result == protocol::ReassemblyResult::Accepted ||
			result == protocol::ReassemblyResult::Duplicate ||
			result == protocol::ReassemblyResult::Completed);
		if (result != protocol::ReassemblyResult::Completed)
			continue;
		protocol::FullSnapshotPartPayload part;
		ASSERT_EQ(protocol::ValidationError::None,
			protocol::decode_full_snapshot_part_payload(
				completed.payload_view(), part));
		if (expected_part_count == 0U)
			expected_part_count = part.part_count;
		ASSERT_EQ(expected_part_count, part.part_count);
		ASSERT_EQ(completed_parts, part.part_index);
		part_targets.push_back(completed.header);
		++completed_parts;
	}
	ASSERT_GT(expected_part_count, 1U);
	ASSERT_EQ(expected_part_count, completed_parts);
	ASSERT_EQ(expected_part_count, part_targets.size());
	const auto& candidate_parts =
		controller.slot(0U).snapshot_egress.candidate_parts();
	ASSERT_EQ(part_targets.size(), candidate_parts.size());
	ASSERT_EQ(part_targets.size(),
		controller.slot(0U).snapshot_egress.retained_item_count());
	auto runtime_probe = controller.slot(0U).phase2_runtime;
	ASSERT_EQ(detail::Phase2RuntimeResult::Applied,
		runtime_probe.on_snapshot_applied(
			controller.slot(0U).snapshot.candidate_snapshot_id(),
			controller.slot(0U).required_manifest_id));
	const auto prior_snapshot_id =
		controller.slot(0U).snapshot.active_snapshot_id();
	const auto replacement_snapshot_id =
		controller.slot(0U).snapshot.candidate_snapshot_id();
	for (std::size_t index = 0U;
		 index < part_targets.size(); ++index) {
		EXPECT_EQ(ack_message_id + 1U + index,
			part_targets[index].message_id);
		EXPECT_EQ(candidate_parts[index].target.message_id,
			part_targets[index].message_id);
		EXPECT_EQ(candidate_parts[index].target.fragment_count,
			part_targets[index].fragment_count);
		EXPECT_EQ(candidate_parts[index].target.message_crc32,
			part_targets[index].message_crc32);
	}

	for (std::size_t index = 0U;
		 index < part_targets.size(); ++index) {
		SCOPED_TRACE(index);
		const protocol::DatagramView target{
			part_targets[index], {}};
		const auto ack = applied_ack(target,
			static_cast<std::uint32_t>(0x280U + index));
		const auto ack_result = controller.ingest(
			peer, view(ack),
			1'000'000U + index * 10'000U,
			1U, true);
		EXPECT_NE(detail::SessionIngressDisposition::Dropped,
			ack_result.disposition)
			<< static_cast<unsigned>(ack_result.drop_reason);
		EXPECT_EQ(part_targets.size() - index - 1U,
			controller.slot(0U).snapshot_egress.retained_item_count());
		if (index + 1U < part_targets.size()) {
			EXPECT_EQ(prior_snapshot_id,
				controller.slot(0U).snapshot.active_snapshot_id());
			EXPECT_EQ(prior_snapshot_id,
				controller.slot(0U).phase2_runtime.active_snapshot_id());
		}
	}
	ASSERT_TRUE(controller.slot(0U).snapshot.has_active_baseline());
	ASSERT_EQ(replacement_snapshot_id,
		controller.slot(0U).snapshot.active_snapshot_id());
	ASSERT_EQ(replacement_snapshot_id,
		controller.slot(0U).phase2_runtime.active_snapshot_id());
	ASSERT_EQ(protocol::ProducerBaselineResult::Applied,
		controller.replace_current_state(
			0U, multipart_runtime_image(true)));
	ASSERT_TRUE(controller.queue_cumulative_delta(0U, 1'500'000U));
	ASSERT_EQ(1U, controller.service_delta_egress(
		1U, 1'500'001U));
	detail::SessionControllerOutput next_delta_output;
	ASSERT_TRUE(controller.pop_output(next_delta_output));
	const auto next_delta = decode_output(next_delta_output);
	ASSERT_EQ(protocol::MessageType::Delta,
		next_delta.header.message_type);
	EXPECT_GT(next_delta.header.message_id,
		part_targets.back().message_id);
}

TEST(Phase2Runtime,
	StaleSessionAcceptsValidatedResyncAndStartsRecovery)
{
	ControllerIdentity identity;
	auto controller = make_controller(identity);
	const auto peer = endpoint(2U, 42043U);
	establish_ready(controller, peer, 0x4f0U, 1'000U, 0U);

	auto manifests = std::make_unique<ManifestPair>();
	ASSERT_TRUE(manifests->initialize());
	ASSERT_EQ(detail::Phase2RuntimeResult::ManifestRequired,
		controller.stage_phase2_manifest(
			0U, *manifests->first, 1'500U));
	ASSERT_EQ(1U, controller.service_phase2_manifest_egress(
		0U, 1'501U));
	detail::SessionControllerOutput manifest_output;
	ASSERT_TRUE(controller.pop_output(manifest_output));
	const auto manifest_ack = applied_ack(
		decode_output(manifest_output), 0x4f1U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(peer, view(manifest_ack), 1'502U, 1U,
			true).disposition);

	ASSERT_TRUE(controller.begin_phase2_snapshot(
		0U, delta_compatible_image(1.0F, 2'000U),
		detail::Phase2RuntimeSnapshotCause::Initial, 2'000U));
	ASSERT_EQ(1U, controller.service_initial_snapshot_egress(
		1U, 2'001U));
	detail::SessionControllerOutput initial_output;
	ASSERT_TRUE(controller.pop_output(initial_output));
	const auto initial_view = decode_output(initial_output);
	const auto initial_ack = applied_ack(initial_view, 0x4f2U);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		controller.ingest(peer, view(initial_ack), 2'002U, 1U,
			true).disposition);
	ASSERT_TRUE(controller.slot(0U).snapshot.has_active_baseline());

	const auto stale_at =
		controller.slot(0U).heartbeat.last_valid_clock_response_us +
		controller.slot(0U).heartbeat.stale_timeout_us;
	controller.service_timeouts(stale_at);
	ASSERT_EQ(detail::ProducerSessionProgress::Stale,
		controller.slot(0U).progress);

	protocol::ResyncRequestPayload request{
		1U, protocol::ResyncReason::SessionStale,
		protocol::ResyncRequestFlagRequireFullSnapshot,
		controller.slot(0U).snapshot.active_snapshot_id(),
		0U, stale_at};
	std::array<std::uint8_t,
		protocol::ResyncRequestPayloadSize> request_payload{};
	std::size_t request_payload_size = 0U;
	ASSERT_EQ(protocol::ValidationError::None,
		protocol::encode_resync_request_payload(request,
			mutable_view(request_payload), request_payload_size));
	protocol::TelemetryDatagramHeader request_header;
	request_header.version_minor = protocol::VersionMinor;
	request_header.message_type = protocol::MessageType::ResyncRequest;
	request_header.flags = protocol::MessageFlagAckRequired;
	request_header.session_id = controller.slot(0U).session_id;
	request_header.packet_sequence = 0x4f3U;
	request_header.sent_time_us = stale_at;
	request_header.message_id = 0x4f3U;
	request_header.fragment_count = 1U;
	request_header.message_size =
		static_cast<std::uint32_t>(request_payload_size);
	request_header.message_crc32 = protocol::crc32_iso_hdlc(
		{request_payload.data(), request_payload_size});
	std::vector<std::uint8_t> encoded_request(
		protocol::HeaderSizeV1 + request_payload_size);
	std::size_t encoded_request_size = 0U;
	ASSERT_EQ(protocol::ValidationError::None,
		protocol::encode_datagram(request_header,
			{protocol::VersionMinor,
			 protocol::VersionMinor},
			{request_payload.data(), request_payload_size},
			mutable_view(encoded_request), encoded_request_size));

	const auto result = controller.ingest(
		peer, view(encoded_request), stale_at + 1U, 1U, true);
	ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
		result.disposition);
	ASSERT_TRUE(result.has_phase2_resync);
	EXPECT_EQ(protocol::ProducerResyncResult::AcceptedNewCandidate,
		result.phase2_resync_result);
	EXPECT_EQ(0U, result.phase2_resync_slot);
	EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState,
		controller.slot(0U).progress);
	ASSERT_TRUE(controller.queue_cumulative_delta(
		0U, stale_at + 2U, stale_at + 2U));
	EXPECT_EQ(detail::Phase2RuntimeSnapshotCause::Resync,
		controller.slot(0U).phase2_runtime.last_started_snapshot_cause());

	detail::SessionControllerOutput ack_output;
	ASSERT_TRUE(controller.pop_output(ack_output));
	const auto ack_view = decode_output(ack_output);
	ASSERT_EQ(protocol::MessageType::Ack,
		ack_view.header.message_type);
	protocol::AckPayload ack{};
	ASSERT_EQ(protocol::ValidationError::None,
		protocol::decode_ack_payload(ack_view.payload, ack));
	EXPECT_EQ(protocol::MessageType::ResyncRequest,
		ack.target_message_type);
	EXPECT_EQ(request_header.message_id, ack.target_message_id);
	EXPECT_EQ(static_cast<std::uint8_t>(protocol::AckFlag::Validated),
		ack.ack_flags);
}

TEST(Phase2Runtime,
	TST050StaleSessionContinuesFullSnapshotReliabilityToTerminalPolicy)
{
	ControllerIdentity identity;
	auto controller = make_controller(identity);
	const auto peer = endpoint(2U, 42043U);
	establish_ready(controller, peer, 0x500U, 1'000U, 0U);

	constexpr std::uint64_t snapshot_started_us = 2'000U;
	ASSERT_TRUE(controller.begin_initial_snapshot(
		0U, one_atom_image(0x50U), snapshot_started_us));
	ASSERT_EQ(1U, controller.service_initial_snapshot_egress(
		1U, snapshot_started_us + 1U));
	detail::SessionControllerOutput initial_output;
	ASSERT_TRUE(controller.pop_output(initial_output));
	const auto initial_view = decode_output(initial_output);
	ASSERT_EQ(protocol::MessageType::FullSnapshot,
		initial_view.header.message_type);
	EXPECT_EQ(0U, static_cast<std::uint8_t>(
		initial_view.header.flags &
		protocol::MessageFlagRetransmission));
	ASSERT_TRUE(controller.slot(0U).snapshot.has_candidate());
	ASSERT_TRUE(controller.slot(0U).snapshot_egress.has_candidate());

	const auto stale_at =
		controller.slot(0U).heartbeat.last_valid_clock_response_us +
		controller.slot(0U).heartbeat.stale_timeout_us;
	controller.service_timeouts(stale_at);
	ASSERT_EQ(detail::ProducerSessionProgress::Stale,
		controller.slot(0U).progress);
	ASSERT_TRUE(controller.slot(0U).snapshot_egress.has_candidate());

	controller.service_periodic(stale_at);
	detail::SessionControllerOutput heartbeat_output;
	ASSERT_TRUE(controller.pop_output(heartbeat_output));
	EXPECT_EQ(protocol::MessageType::Heartbeat,
		decode_output(heartbeat_output).header.message_type);
	EXPECT_EQ(detail::ProducerSessionProgress::Stale,
		controller.slot(0U).progress)
		<< "Heartbeat traffic alone cannot restore ReadyForState.";

	const auto retry_at = stale_at + 1U;
	ASSERT_LT(retry_at - snapshot_started_us,
		protocol::ReliableTransactionRetentionUs);
	controller.service_reliability(retry_at);
	detail::SessionControllerOutput retry_output;
	ASSERT_TRUE(controller.pop_output(retry_output));
	const auto retry_view = decode_output(retry_output);
	EXPECT_EQ(protocol::MessageType::FullSnapshot,
		retry_view.header.message_type);
	EXPECT_NE(0U, static_cast<std::uint8_t>(
		retry_view.header.flags &
		protocol::MessageFlagRetransmission));
	EXPECT_EQ(detail::ProducerSessionProgress::Stale,
		controller.slot(0U).progress);
	EXPECT_TRUE(controller.slot(0U).snapshot_egress.has_candidate());

	controller.service_reliability(
		snapshot_started_us +
		protocol::ReliableTransactionRetentionUs + 1U);
	EXPECT_EQ(detail::ProducerSessionProgress::Empty,
		controller.slot(0U).progress);
	EXPECT_FALSE(controller.slot(0U).snapshot.has_candidate());
	EXPECT_FALSE(controller.slot(0U).snapshot_egress.has_candidate());
}

TEST(Phase2Runtime,
	TST051InitialSnapshotAckDeadlineBoundaryIsTerminalAndLateAckIsIgnored)
{
	const auto run_case = [](std::uint64_t ack_offset_us,
							  bool expect_promotion) {
		ControllerIdentity identity;
		auto controller = make_controller(identity);
		const auto peer = endpoint(2U, 42043U);
		establish_ready(controller, peer, 0x510U, 1'000U, 0U);

		auto manifests = std::make_unique<ManifestPair>();
		ASSERT_TRUE(manifests->initialize());
		ASSERT_EQ(detail::Phase2RuntimeResult::ManifestRequired,
			controller.stage_phase2_manifest(
				0U, *manifests->first, 1'500U));
		ASSERT_EQ(1U, controller.service_phase2_manifest_egress(
			0U, 1'501U));
		detail::SessionControllerOutput manifest_output;
		ASSERT_TRUE(controller.pop_output(manifest_output));
		const auto manifest_ack = applied_ack(
			decode_output(manifest_output), 0x511U);
		ASSERT_EQ(detail::SessionIngressDisposition::ResponseQueued,
			controller.ingest(peer, view(manifest_ack), 1'502U, 1U,
				true).disposition);

		constexpr std::uint64_t snapshot_started_us = 2'000U;
		ASSERT_TRUE(controller.begin_phase2_snapshot(
			0U, one_atom_image(0x51U),
			detail::Phase2RuntimeSnapshotCause::Initial,
			snapshot_started_us));
		ASSERT_EQ(1U, controller.service_initial_snapshot_egress(
			1U, snapshot_started_us + 1U));
		detail::SessionControllerOutput output;
		ASSERT_TRUE(controller.pop_output(output));
		const auto output_view = decode_output(output);
		const auto ack = applied_ack(output_view, 0x512U);
		const auto deadline_us = snapshot_started_us +
			protocol::ReliableTransactionRetentionUs;
		const auto ack_at_us = deadline_us + ack_offset_us;

		const auto disposition = controller.ingest(
			peer, view(ack), ack_at_us, 1U, true).disposition;
		if (expect_promotion) {
			EXPECT_EQ(detail::SessionIngressDisposition::ResponseQueued,
				disposition);
			EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState,
				controller.slot(0U).progress);
			EXPECT_TRUE(controller.slot(0U).snapshot.has_active_baseline());
			return;
		}

		EXPECT_NE(detail::SessionIngressDisposition::ResponseQueued,
			disposition);
		controller.service_reliability(ack_at_us);
		EXPECT_EQ(detail::ProducerSessionProgress::Empty,
			controller.slot(0U).progress);
		EXPECT_FALSE(controller.slot(0U).snapshot.has_active_baseline());
		EXPECT_FALSE(controller.slot(0U).snapshot.has_candidate());
		EXPECT_NE(detail::SessionIngressDisposition::ResponseQueued,
			controller.ingest(peer, view(ack), ack_at_us + 200U, 1U,
				true).disposition);
	};

	run_case(static_cast<std::uint64_t>(-1), true);
	run_case(0U, false);
	run_case(200U, false);
}

TEST(Phase2Runtime, TST060BeforeDuringAndAfterLoadMaterializesOnlyCockpitState)
{
	detail::Phase2RuntimeSlot slot;
	std::array<detail::Phase2CaptureLocalKey, 1U> keys{{{41U}}};
	std::array<telemetry::Phase2Wp05SubjectBinding, 1U> bindings{};

	EXPECT_FALSE(slot.configure(
		telemetry::detail::Phase2Wp07EpisodeLatches::SessionCapacity));
	EXPECT_EQ(detail::Phase2RuntimeResult::InvalidInput,
		slot.reconcile_closure(
			keys.data(), keys.size(), bindings.data(), bindings.size()));
	EXPECT_FALSE(slot.configure(
		telemetry::detail::Phase2Wp07EpisodeLatches::SessionCapacity));

	telemetry::CockpitProducerEligibility eligibility{};
	ASSERT_EQ(telemetry::CockpitProducerEligibilityError::None,
		telemetry::validate_cockpit_sensor_producer(eligibility));
	ASSERT_TRUE(slot.configure(0U));
	EXPECT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		slot.reconcile_closure(
			keys.data(), keys.size(), bindings.data(), bindings.size()));
	EXPECT_NE(0U, bindings[0].entity_id);
}

TEST(Phase2Runtime, TST058SlotsOwnIndependentIdsCandidatesAndResyncLatches)
{
	std::array<detail::Phase2RuntimeSlot, 2U> slots;
	for (std::size_t index = 0U; index < slots.size(); ++index)
		ASSERT_TRUE(slots[index].configure(index));
	const std::array<detail::Phase2CaptureLocalKey, 2U> keys{{
		{41U}, {42U}}};
	std::array<telemetry::Phase2Wp05SubjectBinding, 2U> left{};
	std::array<telemetry::Phase2Wp05SubjectBinding, 2U> right{};
	ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		slots[0].reconcile_closure(
			keys.data(), keys.size(), left.data(), left.size()));
	ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		slots[1].reconcile_closure(
			keys.data(), keys.size(), right.data(), right.size()));
	EXPECT_EQ(left[0].capture_key.value,
		right[0].capture_key.value);
	EXPECT_EQ(left[0].entity_id, right[0].entity_id)
		<< "Public IDs may coincide across isolated session namespaces.";

	ASSERT_EQ(detail::Phase2RuntimeResult::ManifestRequired,
		slots[0].stage_manifest(11U, digest(0x11U)));
	ASSERT_EQ(detail::Phase2RuntimeResult::ManifestRequired,
		slots[1].stage_manifest(21U, digest(0x21U)));
	ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		slots[0].on_manifest_applied(11U));
	ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		slots[1].on_manifest_applied(21U));
	detail::Phase2RuntimeSnapshotPlan slow;
	ASSERT_EQ(detail::Phase2RuntimeResult::Applied,
		slots[0].next_snapshot_plan(slow));
	ASSERT_EQ(detail::Phase2RuntimeResult::Applied,
		slots[0].on_snapshot_started(slow));
	EXPECT_EQ(0U, slots[1].active_snapshot_id());
	EXPECT_EQ(0U, slots[0].active_snapshot_id());
	EXPECT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		slots[0].request_snapshot(
			detail::Phase2RuntimeSnapshotCause::Resync));
	EXPECT_NE(detail::Phase2RuntimeSnapshotCause::Resync,
		slots[1].pending_snapshot_cause());
	EXPECT_NE(slots[0].manifest_state().staged_id,
		slots[1].manifest_state().staged_id);
	EXPECT_NE(&slots[0].support_latches(),
		&slots[1].support_latches());
}

TEST(Phase2Runtime,
	TST043GlobalFactsDrainOnceFanOutAtomicallyAndAckOnlyTheExactCandidateGeneration)
{
	detail::reset_phase2_mission_observation_state();
	telemetry::OnShipCleanup(101U,
		detail::ShipCleanupMode::Destroyed);
	telemetry::OnSupportTransition(101U, 202U, 7U,
		detail::SupportTransitionReason::Complete, 10U);
	telemetry::OnSupportTransition(101U, 202U, 7U,
		detail::SupportTransitionReason::End, 11U);
	EXPECT_EQ(1U, detail::phase2_cleanup_ring_depth());
	EXPECT_EQ(2U, detail::phase2_support_ring_depth())
		<< "The global ring preserves both accepted transitions before per-slot coalescing.";

	detail::Phase2CaptureDiagnostics accepted{};
	accepted.source_count = 2U;
	accepted.source_signatures[0] = 101U;
	accepted.source_signatures[1] = 202U;
	detail::Phase2Wp07GlobalEventBatch batch{};
	ASSERT_EQ(detail::Phase2Wp07DrainStatus::Drained,
		detail::prepare_phase2_global_events(accepted, batch));
	ASSERT_EQ(1U, batch.cleanup.count);
	ASSERT_EQ(2U, batch.support_count);
	EXPECT_EQ(1U, batch.cleanup.intents[0].object_signature);
	EXPECT_EQ(1U, batch.support[0].assisted_signature);
	EXPECT_EQ(2U, batch.support[0].support_signature);
	EXPECT_EQ(detail::SupportTransitionReason::Complete,
		batch.support[0].reason);
	EXPECT_EQ(1U, detail::phase2_cleanup_ring_depth());
	EXPECT_EQ(2U, detail::phase2_support_ring_depth())
		<< "Prepare is non-destructive until every target accepts.";

	ControllerIdentity identity;
	auto controller = std::make_unique<detail::SessionController>(
		make_controller(identity));
	establish_ready(*controller, endpoint(2U, 42043U),
		0x501U, 1'000U, 0U);
	establish_ready(*controller, endpoint(3U, 42044U),
		0x502U, 2'000U, 1U);
	const std::array<detail::Phase2CaptureLocalKey, 2U> closure{{
		{batch.cleanup.intents[0].object_signature},
		{accepted.source_signatures[0]}}};
	for (std::size_t slot = 0U; slot < 2U; ++slot) {
		std::array<telemetry::Phase2Wp05SubjectBinding, 2U> bindings{};
		ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
			controller->reconcile_phase2_closure(slot,
				closure.data(), closure.size(),
				bindings.data(), bindings.size()));
	}
	detail::Phase2GlobalFanoutResult fanout{};
	ASSERT_EQ(detail::Phase2RuntimeResult::Applied,
		controller->apply_phase2_global_events_transaction(batch, fanout));
	EXPECT_EQ(2U, fanout.targeted_count);
	for (std::size_t slot = 0U; slot < 2U; ++slot) {
		EXPECT_TRUE(fanout.targeted[slot]);
		EXPECT_EQ(detail::Phase2RuntimeSnapshotCause::Initial,
			fanout.cause_after[slot])
			<< "The already-pending initial snapshot remains the primary cause.";
		const auto* latches =
			controller->slot(slot).phase2_runtime.support_latches().pending(
				slot, 1U)
			? &controller->slot(slot).phase2_runtime.support_latches()
			: nullptr;
		ASSERT_NE(nullptr, latches);
		EXPECT_EQ(7U, latches->value(slot, 1U).episode_sequence);
		EXPECT_EQ(1U,
			controller->slot(slot).phase2_runtime.cleanup_batch().count);
		EXPECT_EQ(2U,
			controller->slot(slot).phase2_runtime.pending_support_count());
	}
	ASSERT_TRUE(detail::commit_phase2_global_events(batch));
	EXPECT_EQ(0U, detail::phase2_cleanup_ring_depth());
	EXPECT_EQ(0U, detail::phase2_support_ring_depth());
	EXPECT_FALSE(detail::commit_phase2_global_events(batch))
		<< "The same global drain cannot be committed twice.";

	auto slots = std::make_unique<
		std::array<detail::Phase2RuntimeSlot, 2U>>();
	for (std::size_t slot = 0U; slot < slots->size(); ++slot) {
		ASSERT_TRUE((*slots)[slot].configure(slot));
		std::array<telemetry::Phase2Wp05SubjectBinding, 2U> bindings{};
		ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
			(*slots)[slot].reconcile_closure(
				closure.data(), closure.size(),
				bindings.data(), bindings.size()));
		ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
			(*slots)[slot].apply_global_events(batch));
		ASSERT_EQ(detail::Phase2RuntimeResult::ManifestRequired,
			(*slots)[slot].stage_manifest(
				static_cast<std::uint32_t>(slot + 1U),
				digest(static_cast<std::uint8_t>(slot + 1U))));
		ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
			(*slots)[slot].on_manifest_applied(
				static_cast<std::uint32_t>(slot + 1U)));
	}
	std::array<detail::Phase2RuntimeSnapshotPlan, 2U> candidates{};
	for (std::size_t slot = 0U; slot < slots->size(); ++slot) {
		ASSERT_EQ(detail::Phase2RuntimeResult::Applied,
			(*slots)[slot].next_snapshot_plan(candidates[slot]));
		ASSERT_EQ(detail::Phase2RuntimeResult::Applied,
			(*slots)[slot].on_snapshot_started(candidates[slot]));
	}
	auto successor = batch;
	successor.cleanup = {};
	successor.support_count = 1U;
	successor.support[0].episode_sequence = 8U;
	successor.support[0].reason =
		detail::SupportTransitionReason::Broken;
	ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		(*slots)[0].apply_global_events(successor));
	ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		(*slots)[1].apply_global_events(successor));
	EXPECT_EQ(3U, (*slots)[0].pending_support_count());
	EXPECT_EQ(3U, (*slots)[1].pending_support_count());

	ASSERT_EQ(detail::Phase2RuntimeResult::Applied,
		(*slots)[0].on_snapshot_applied(candidates[0].snapshot_id, 1U));
	EXPECT_EQ(1U, (*slots)[0].pending_support_count())
		<< "APPLIED removes only facts frozen into this slot's candidate.";
	EXPECT_EQ(1U, (*slots)[0].manifest_state().active_id);
	EXPECT_EQ(0U, (*slots)[1].manifest_state().active_id)
		<< "The slow slot keeps its independent candidate until its ACK.";
	EXPECT_EQ(8U,
		(*slots)[0].support_latches().value(0U, 1U).episode_sequence)
		<< "APPLIED clears only the episode copied into the immutable candidate.";
	EXPECT_EQ(8U,
		(*slots)[1].support_latches().value(1U, 1U).episode_sequence);
	EXPECT_EQ(detail::Phase2RuntimeResult::Stale,
		(*slots)[1].on_snapshot_applied(
			candidates[1].snapshot_id, 1U));
	EXPECT_EQ(3U, (*slots)[1].pending_support_count())
		<< "A divergent ACK cannot drain either the candidate or its successor.";
	ASSERT_EQ(detail::Phase2RuntimeResult::Applied,
		(*slots)[1].on_snapshot_applied(
			candidates[1].snapshot_id, 2U));
	EXPECT_EQ(1U, (*slots)[1].pending_support_count());
	EXPECT_EQ(2U, (*slots)[1].manifest_state().active_id);
	EXPECT_EQ(8U,
		(*slots)[1].support_latches().value(1U, 1U).episode_sequence);

	ControllerIdentity full_identity;
	auto full = std::make_unique<detail::SessionController>(
		make_controller(full_identity));
	establish_ready(*full, endpoint(4U, 42045U),
		0x503U, 3'000U, 0U);
	establish_ready(*full, endpoint(5U, 42046U),
		0x504U, 4'000U, 1U);
	auto* slow_latches = full->phase2_support_latches(1U);
	ASSERT_NE(nullptr, slow_latches);
	for (std::uint32_t entry = 1U; entry <= 64U; ++entry)
		ASSERT_TRUE(slow_latches->latch(1U,
			{100U + entry, 200U, entry,
			 detail::SupportTransitionReason::Complete, entry}));
	detail::Phase2Wp07GlobalEventBatch overflow{};
	overflow.support_count = 1U;
	overflow.support[0] = {999U, 200U, 65U,
		detail::SupportTransitionReason::Complete, 65U};
	detail::Phase2GlobalFanoutResult rejected{};
	EXPECT_EQ(detail::Phase2RuntimeResult::CapacityExceeded,
		full->apply_phase2_global_events_transaction(
			overflow, rejected));
	EXPECT_FALSE(full->slot(0U).phase2_runtime.support_latches()
		.pending(0U, 999U))
		<< "Fanout failure leaves every earlier target unchanged.";
	EXPECT_EQ(0U, full->slot(0U).phase2_runtime.pending_support_count());
	EXPECT_EQ(0U, full->slot(0U).phase2_runtime.cleanup_batch().count);
	EXPECT_EQ(64U,
		full->slot(1U).phase2_runtime.support_latches().size(1U));

	detail::Phase2Wp07GlobalEventBatch unknown{};
	unknown.cleanup.count = 1U;
	unknown.cleanup.intents[0] = batch.cleanup.intents[0];
	unknown.cleanup.intents[0].object_signature = 999U;
	const auto before_cleanup =
		controller->slot(0U).phase2_runtime.cleanup_batch().count;
	const auto before_support =
		controller->slot(0U).phase2_runtime.pending_support_count();
	detail::Phase2GlobalFanoutResult invalid{};
	EXPECT_EQ(detail::Phase2RuntimeResult::InvalidInput,
		controller->apply_phase2_global_events_transaction(
			unknown, invalid));
	EXPECT_EQ(before_cleanup,
		controller->slot(0U).phase2_runtime.cleanup_batch().count);
	EXPECT_EQ(before_support,
		controller->slot(0U).phase2_runtime.pending_support_count())
		<< "An unknown closure signature rejects the whole fanout.";

	detail::reset_phase2_mission_observation_state();
}

TEST(Phase2Runtime, TST059KeepsNAndNPlusOneAndCoalescesNPlusTwo)
{
	detail::Phase2RuntimeSlot slot;
	ASSERT_TRUE(slot.configure(0U));
	ASSERT_EQ(detail::Phase2RuntimeResult::ManifestRequired,
		slot.stage_manifest(1U, digest(1U)));
	ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		slot.on_manifest_applied(1U));
	detail::Phase2RuntimeSnapshotPlan first;
	ASSERT_EQ(detail::Phase2RuntimeResult::Applied,
		slot.next_snapshot_plan(first));
	ASSERT_EQ(detail::Phase2RuntimeResult::Applied,
		slot.on_snapshot_started(first));
	ASSERT_EQ(detail::Phase2RuntimeResult::Applied,
		slot.on_snapshot_applied(first.snapshot_id, 1U));

	ASSERT_EQ(detail::Phase2RuntimeResult::ManifestRequired,
		slot.stage_manifest(2U, digest(2U)));
	ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		slot.on_manifest_applied(2U));
	EXPECT_EQ(detail::Phase2RuntimeResult::CandidateBusy,
		slot.stage_manifest(3U, digest(3U)));
	EXPECT_TRUE(slot.manifest_state().rebuild_intent);
	EXPECT_EQ(1U, slot.manifest_state().active_id);
	EXPECT_EQ(2U, slot.manifest_state().staged_id);
	EXPECT_EQ(detail::Phase2RuntimeResult::NoChange,
		slot.on_snapshot_applied(first.snapshot_id, 1U))
		<< "A late ACK for N is idempotent and cannot promote N+1.";

	detail::Phase2RuntimeSnapshotPlan second;
	ASSERT_EQ(detail::Phase2RuntimeResult::Applied,
		slot.next_snapshot_plan(second));
	ASSERT_EQ(2U, second.required_manifest_id);
	ASSERT_EQ(detail::Phase2RuntimeResult::Applied,
		slot.on_snapshot_started(second));
	ASSERT_EQ(detail::Phase2RuntimeResult::Applied,
		slot.on_snapshot_applied(second.snapshot_id, 2U));
	EXPECT_EQ(2U, slot.manifest_state().active_id);
	EXPECT_EQ(0U, slot.manifest_state().staged_id);
	EXPECT_TRUE(slot.consume_rebuild_intent());
	EXPECT_FALSE(slot.consume_rebuild_intent());
	ASSERT_EQ(detail::Phase2RuntimeResult::ManifestRequired,
		slot.stage_manifest(3U, digest(3U)));
	EXPECT_EQ(2U, slot.manifest_state().active_id);
	EXPECT_EQ(3U, slot.manifest_state().staged_id);
}

TEST(Phase2Runtime,
	TST043SupportCoalescingReportsAllThreeClosedReasonsPerTargetExactlyOnce)
{
	ControllerIdentity identity;
	auto controller = std::make_unique<detail::SessionController>(
		make_controller(identity));
	establish_ready(*controller, endpoint(6U, 42046U),
		0x601U, 1'000U, 0U);
	establish_ready(*controller, endpoint(7U, 42047U),
		0x602U, 2'000U, 1U);

	auto apply = [&](const detail::SupportTransitionFact& fact) {
		detail::Phase2Wp07GlobalEventBatch batch{};
		batch.support[0] = fact;
		batch.support_count = 1U;
		detail::Phase2GlobalFanoutResult result{};
		EXPECT_EQ(detail::Phase2RuntimeResult::Applied,
			controller->apply_phase2_global_events_transaction(
				batch, result));
		EXPECT_EQ(2U, result.targeted_count);
		return result;
	};

	const auto initial = apply(
		{101U, 201U, 10U,
			detail::SupportTransitionReason::Complete, 10U});
	for (std::size_t slot = 0U; slot < 2U; ++slot)
		EXPECT_EQ((std::array<std::uint8_t, 3U>{}),
			initial.support_coalesced[slot]);

	const auto complete_then_end = apply(
		{101U, 201U, 10U,
			detail::SupportTransitionReason::End, 11U});
	const auto newer_episode = apply(
		{101U, 201U, 11U,
			detail::SupportTransitionReason::Broken, 12U});
	const auto stale_generation = apply(
		{101U, 201U, 9U,
			detail::SupportTransitionReason::Abort, 13U});
	for (std::size_t slot = 0U; slot < 2U; ++slot) {
		EXPECT_EQ((std::array<std::uint8_t, 3U>{{1U, 0U, 0U}}),
			complete_then_end.support_coalesced[slot]);
		EXPECT_EQ((std::array<std::uint8_t, 3U>{{0U, 1U, 0U}}),
			newer_episode.support_coalesced[slot]);
		EXPECT_EQ((std::array<std::uint8_t, 3U>{{0U, 0U, 1U}}),
			stale_generation.support_coalesced[slot]);
		EXPECT_EQ(11U, controller->slot(slot).phase2_runtime
			.support_latches().value(slot, 101U).episode_sequence);
	}
}

TEST(Phase2Runtime,
	TST059RebuildIntentHasOneFalseToTrueEdgeUntilItIsConsumed)
{
	detail::Phase2RuntimeSlot slot;
	ASSERT_TRUE(slot.configure(0U));
	ASSERT_EQ(detail::Phase2RuntimeResult::ManifestRequired,
		slot.stage_manifest(1U, digest(1U)));
	EXPECT_FALSE(slot.manifest_state().rebuild_intent);
	ASSERT_EQ(detail::Phase2RuntimeResult::CandidateBusy,
		slot.stage_manifest(2U, digest(2U)));
	EXPECT_TRUE(slot.manifest_state().rebuild_intent);
	ASSERT_EQ(detail::Phase2RuntimeResult::CandidateBusy,
		slot.stage_manifest(2U, digest(2U)));
	EXPECT_TRUE(slot.manifest_state().rebuild_intent)
		<< "Retries preserve one pending edge; they do not create new edges.";
	EXPECT_TRUE(slot.consume_rebuild_intent());
	EXPECT_FALSE(slot.consume_rebuild_intent());
}

TEST(Phase2Runtime,
	CockpitNoPlayerClosureKeepsSessionAllocatorHighWaterAcrossRespawn)
{
	detail::Phase2RuntimeSlot slot;
	ASSERT_TRUE(slot.configure(0U));
	const std::array<detail::Phase2CaptureLocalKey, 1U>
		initial_keys{{{41U}}};
	std::array<telemetry::Phase2Wp05SubjectBinding, 1U>
		initial_bindings{};
	ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		slot.reconcile_closure(initial_keys.data(),
			initial_keys.size(), initial_bindings.data(),
			initial_bindings.size()));
	const auto initial_entity_id =
		initial_bindings[0].entity_id;
	ASSERT_NE(0U, initial_entity_id);

	EXPECT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		slot.reconcile_closure(nullptr, 0U, nullptr, 0U))
		<< "A canonical NoPlayer observation closes the complete-ship "
			   "closure without resetting the session.";
	EXPECT_EQ(detail::Phase2RuntimeResult::NoChange,
		slot.reconcile_closure(nullptr, 0U, nullptr, 0U))
		<< "The empty closure is stable while the player is absent.";

	const std::array<detail::Phase2CaptureLocalKey, 1U>
		respawn_keys{{{42U}}};
	std::array<telemetry::Phase2Wp05SubjectBinding, 1U>
		respawn_bindings{};
	ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		slot.reconcile_closure(respawn_keys.data(),
			respawn_keys.size(), respawn_bindings.data(),
			respawn_bindings.size()));
	EXPECT_GT(respawn_bindings[0].entity_id,
		initial_entity_id)
		<< "A same-session respawn receives a new entity id after the "
			   "NoPlayer closure.";
}

TEST(Phase2Runtime,
	TST046LifecycleQueueIsDurableBoundedFencedAndAckScopedThroughRespawn)
{
	auto slot = std::make_unique<detail::Phase2RuntimeSlot>();
	ASSERT_TRUE(slot->configure(0U));
	std::array<detail::Phase2CaptureLocalKey,
		detail::MaximumPhase2ObservationShips> keys{};
	std::array<telemetry::Phase2Wp05SubjectBinding,
		detail::MaximumPhase2ObservationShips> bindings{};
	for (std::size_t index = 0U; index < keys.size(); ++index)
		keys[index].value = static_cast<std::uint32_t>(index + 1U);
	ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		slot->reconcile_closure(keys.data(), keys.size(),
			bindings.data(), bindings.size()));

	auto observation =
		std::make_unique<detail::Phase2ObservationDto>();
	observation->ships.resize(keys.size());
	for (std::size_t index = 0U; index < keys.size(); ++index) {
		observation->ships[index].capture_key = keys[index];
		observation->ships[index].lifecycle.sample_time_us = 100U;
		observation->ships[index].lifecycle.state =
			detail::ShipLifecycleState::Present;
	}
	const std::array<detail::Phase2RuntimeLifecycleEventKind, 5U>
		kinds{{
			detail::Phase2RuntimeLifecycleEventKind::Appeared,
			detail::Phase2RuntimeLifecycleEventKind::Disabled,
			detail::Phase2RuntimeLifecycleEventKind::DyingStarted,
			detail::Phase2RuntimeLifecycleEventKind::Destroyed,
			detail::Phase2RuntimeLifecycleEventKind::Disappeared}};
	for (std::size_t transition = 0U;
		 transition < kinds.size(); ++transition) {
		for (auto& ship : observation->ships) {
			ship.lifecycle.sample_time_us =
				static_cast<std::uint64_t>(100U + transition);
			if (transition == 1U)
				ship.lifecycle.lifecycle_flags =
					protocol::EntityLifecycleFlagDisabled;
			if (transition == 2U)
				ship.lifecycle.state =
					detail::ShipLifecycleState::Dying;
			if (transition == 3U)
				ship.lifecycle.state =
					detail::ShipLifecycleState::Destroyed;
			if (transition == 4U)
				ship.lifecycle.state =
					detail::ShipLifecycleState::Removed;
		}
		ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
			slot->observe_lifecycle(*observation, bindings.data(),
				bindings.size()));
	}
	ASSERT_EQ(detail::Phase2RuntimeSlot::LifecycleCapacity,
		slot->pending_lifecycle_count());
	for (std::size_t transition = 0U;
		 transition < kinds.size(); ++transition)
		for (std::size_t entity = 0U; entity < keys.size(); ++entity) {
			const auto offset = transition * keys.size() + entity;
			const auto& fact = slot->pending_lifecycle(offset);
			EXPECT_EQ(offset + 1U, fact.event_id);
			EXPECT_EQ(kinds[transition], fact.kind);
			EXPECT_EQ(bindings[entity].entity_id, fact.entity_id);
			EXPECT_EQ(keys[entity].value, fact.source_signature);
			EXPECT_EQ(0U, fact.dependency_snapshot_id);
		}

	detail::Phase2Wp07GlobalEventBatch overflow{};
	overflow.cleanup.count = 1U;
	overflow.cleanup.intents[0].object_signature = keys[0].value;
	overflow.cleanup.intents[0].event_ready = true;
	overflow.cleanup.intents[0].fence_ready = true;
	overflow.cleanup.intents[0].new_entity_id_request = true;
	overflow.cleanup.intents[0].event_order = 1U;
	overflow.cleanup.intents[0].purge_order = 2U;
	overflow.cleanup.intents[0].replacement_entity_id = 65U;
	EXPECT_EQ(detail::Phase2RuntimeResult::CapacityExceeded,
		slot->apply_global_events(overflow));
	EXPECT_EQ(detail::Phase2RuntimeSlot::LifecycleCapacity,
		slot->pending_lifecycle_count());
	EXPECT_EQ(0U, slot->cleanup_batch().count);

	ASSERT_EQ(detail::Phase2RuntimeResult::ManifestRequired,
		slot->stage_manifest(1U, digest(1U)));
	ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		slot->on_manifest_applied(1U));
	const std::array<std::uint64_t,
		detail::Phase2RuntimeSlot::BlockCount> samples{{
			101U, 102U, 103U, 104U, 105U, 106U, 107U, 108U}};
	ASSERT_TRUE(slot->set_current_block_samples(samples));
	detail::Phase2RuntimeSnapshotPlan initial;
	ASSERT_EQ(detail::Phase2RuntimeResult::Applied,
		slot->next_snapshot_plan(initial));
	ASSERT_EQ(detail::Phase2RuntimeResult::Applied,
		slot->on_snapshot_started(initial));
	EXPECT_EQ(detail::Phase2RuntimeSlot::LifecycleCapacity,
		slot->pending_lifecycle_count());
	EXPECT_EQ(detail::Phase2RuntimeResult::Stale,
		slot->on_snapshot_applied(initial.snapshot_id + 1U,
			initial.required_manifest_id));
	EXPECT_EQ(detail::Phase2RuntimeSlot::LifecycleCapacity,
		slot->pending_lifecycle_count());
	ASSERT_EQ(detail::Phase2RuntimeResult::Applied,
		slot->on_snapshot_applied(initial.snapshot_id,
			initial.required_manifest_id));
	EXPECT_EQ(0U, slot->pending_lifecycle_count());
	for (std::size_t block = 0U; block < samples.size(); ++block)
		EXPECT_EQ(samples[block], slot->active_block_sample(block));

	ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		slot->apply_global_events(overflow));
	ASSERT_EQ(1U, slot->pending_lifecycle_count());
	const auto cleanup_fact = slot->pending_lifecycle(0U);
	EXPECT_EQ(321U, cleanup_fact.event_id);
	EXPECT_EQ(initial.snapshot_id,
		cleanup_fact.dependency_snapshot_id);
	EXPECT_EQ(detail::Phase2RuntimeLifecycleEventKind::Destroyed,
		cleanup_fact.kind);
	EXPECT_TRUE(overflow.cleanup.intents[0].event_ready);
	EXPECT_TRUE(overflow.cleanup.intents[0].fence_ready);
	EXPECT_LT(overflow.cleanup.intents[0].event_order,
		overflow.cleanup.intents[0].purge_order);

	detail::Phase2RuntimeSnapshotPlan cleanup_candidate;
	ASSERT_EQ(detail::Phase2RuntimeResult::Applied,
		slot->next_snapshot_plan(cleanup_candidate));
	ASSERT_EQ(detail::Phase2RuntimeSnapshotCause::Lifecycle,
		cleanup_candidate.cause);
	ASSERT_EQ(detail::Phase2RuntimeResult::Applied,
		slot->on_snapshot_started(cleanup_candidate));
	EXPECT_EQ(detail::Phase2RuntimeResult::Stale,
		slot->on_snapshot_applied(cleanup_candidate.snapshot_id,
			cleanup_candidate.required_manifest_id + 1U));
	EXPECT_EQ(1U, slot->pending_lifecycle_count());
	ASSERT_EQ(detail::Phase2RuntimeResult::Applied,
		slot->on_snapshot_applied(cleanup_candidate.snapshot_id,
			cleanup_candidate.required_manifest_id));
	EXPECT_EQ(0U, slot->pending_lifecycle_count());

	const auto first_entity = bindings[0].entity_id;
	ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		slot->reconcile_closure(keys.data() + 1U, keys.size() - 1U,
			bindings.data() + 1U, bindings.size() - 1U));
	ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		slot->reconcile_closure(keys.data(), keys.size(),
			bindings.data(), bindings.size()));
	EXPECT_GT(bindings[0].entity_id, first_entity)
		<< "Respawn after cleanup receives a fresh session-scoped entity id.";

	slot->reset();
	EXPECT_EQ(0U, slot->pending_lifecycle_count());
	EXPECT_EQ(0U, slot->pending_support_count());
	EXPECT_EQ(0U, slot->cleanup_batch().count);
	EXPECT_EQ(0U, slot->started_snapshot_sequence());
	EXPECT_EQ(detail::Phase2RuntimeSnapshotCause::None,
		slot->last_started_snapshot_cause());
	for (std::size_t block = 0U; block < samples.size(); ++block)
		EXPECT_EQ(0U, slot->active_block_sample(block));
}

TEST(Phase2Runtime,
	TST051ForcedReasonsIncrementOnlyAtCandidateStartAndSamplesOnlyAtApplied)
{
	detail::Phase2RuntimeSlot slot;
	ASSERT_TRUE(slot.configure(0U));
	const std::array<detail::Phase2CaptureLocalKey, 1U> keys{{{41U}}};
	std::array<telemetry::Phase2Wp05SubjectBinding, 1U> bindings{};
	ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		slot.reconcile_closure(keys.data(), keys.size(),
			bindings.data(), bindings.size()));
	ASSERT_EQ(detail::Phase2RuntimeResult::ManifestRequired,
		slot.stage_manifest(1U, digest(1U)));
	ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		slot.on_manifest_applied(1U));
	std::array<std::uint64_t,
		detail::Phase2RuntimeSlot::BlockCount> samples{{
			11U, 12U, 13U, 14U, 15U, 16U, 17U, 18U}};
	ASSERT_TRUE(slot.set_current_block_samples(samples));
	detail::Phase2RuntimeSnapshotPlan initial;
	ASSERT_EQ(detail::Phase2RuntimeResult::Applied,
		slot.next_snapshot_plan(initial));
	EXPECT_EQ(0U, slot.started_snapshot_sequence());
	ASSERT_EQ(detail::Phase2RuntimeResult::Applied,
		slot.on_snapshot_started(initial));
	EXPECT_EQ(1U, slot.started_snapshot_sequence());
	EXPECT_EQ(detail::Phase2RuntimeSnapshotCause::Initial,
		slot.last_started_snapshot_cause());
	for (std::size_t block = 0U; block < samples.size(); ++block)
		EXPECT_EQ(0U, slot.active_block_sample(block));
	ASSERT_EQ(detail::Phase2RuntimeResult::Applied,
		slot.on_snapshot_applied(initial.snapshot_id,
			initial.required_manifest_id));
	for (std::size_t block = 0U; block < samples.size(); ++block)
		EXPECT_EQ(samples[block], slot.active_block_sample(block));

	const std::array<detail::Phase2RuntimeSnapshotCause, 6U> causes{{
		detail::Phase2RuntimeSnapshotCause::Periodic,
		detail::Phase2RuntimeSnapshotCause::Topology,
		detail::Phase2RuntimeSnapshotCause::Catalog,
		detail::Phase2RuntimeSnapshotCause::Lifecycle,
		detail::Phase2RuntimeSnapshotCause::SupportTerminal,
		detail::Phase2RuntimeSnapshotCause::Resync}};
	for (const auto cause : causes) {
		for (auto& sample : samples) ++sample;
		ASSERT_TRUE(slot.set_current_block_samples(samples));
		ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
			slot.request_snapshot(cause));
		ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
			slot.request_snapshot(cause));
		detail::Phase2RuntimeSnapshotPlan candidate;
		ASSERT_EQ(detail::Phase2RuntimeResult::Applied,
			slot.next_snapshot_plan(candidate));
		const auto before = slot.started_snapshot_sequence();
		EXPECT_EQ(cause, candidate.cause);
		EXPECT_EQ(before, slot.started_snapshot_sequence());
		ASSERT_EQ(detail::Phase2RuntimeResult::Applied,
			slot.on_snapshot_started(candidate));
		EXPECT_EQ(before + 1U, slot.started_snapshot_sequence());
		EXPECT_EQ(cause, slot.last_started_snapshot_cause());
		EXPECT_EQ(detail::Phase2RuntimeResult::InvalidInput,
			slot.on_snapshot_started(candidate));
		EXPECT_EQ(before + 1U, slot.started_snapshot_sequence());
		for (std::size_t block = 0U; block < samples.size(); ++block)
			EXPECT_NE(samples[block], slot.active_block_sample(block));
		EXPECT_EQ(detail::Phase2RuntimeResult::Stale,
			slot.on_snapshot_applied(candidate.snapshot_id + 1U,
				candidate.required_manifest_id));
		ASSERT_EQ(detail::Phase2RuntimeResult::Applied,
			slot.on_snapshot_applied(candidate.snapshot_id,
				candidate.required_manifest_id));
		for (std::size_t block = 0U; block < samples.size(); ++block)
			EXPECT_EQ(samples[block], slot.active_block_sample(block));
	}
}

TEST(Phase2Runtime, TST067MissionExitPurgesAnInFlightSnapshotAndRejectsEveryLateCompletion)
{
	detail::Phase2RuntimeSlot slot;
	ASSERT_TRUE(slot.configure(0U));
	ASSERT_EQ(detail::Phase2RuntimeResult::ManifestRequired,
		slot.stage_manifest(1U, digest(1U)));
	ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		slot.on_manifest_applied(1U));
	detail::Phase2RuntimeSnapshotPlan candidate;
	ASSERT_EQ(detail::Phase2RuntimeResult::Applied,
		slot.next_snapshot_plan(candidate));
	ASSERT_EQ(detail::Phase2RuntimeResult::Applied,
		slot.on_snapshot_started(candidate));

	slot.reset();
	EXPECT_EQ(0U, slot.active_snapshot_id());
	EXPECT_EQ(0U, slot.closure_size());
	EXPECT_EQ(0U, slot.manifest_state().active_id);
	EXPECT_EQ(0U, slot.manifest_state().staged_id);
	EXPECT_EQ(detail::Phase2RuntimeResult::InvalidInput,
		slot.on_snapshot_applied(
			candidate.snapshot_id, candidate.required_manifest_id));
	EXPECT_EQ(detail::Phase2RuntimeResult::InvalidInput,
		slot.on_snapshot_abandoned(candidate.snapshot_id));
}

TEST(Phase2Runtime, TST068MissionRelaunchCreatesFreshSessionAndEntityNamespacesWithoutResidue)
{
	ControllerIdentity identity;
	auto controller = make_controller(identity);
	const auto first_endpoint = endpoint(2U, 42043U);
	establish_ready(controller, first_endpoint,
		0x401U, 1'000U, 0U);
	const auto first_session = controller.slot(0U).session_id;
	ASSERT_NE(0U, first_session);
	std::array<detail::Phase2CaptureLocalKey, 1U> keys{{{77U}}};
	std::array<telemetry::Phase2Wp05SubjectBinding, 1U> first_binding{};
	ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		controller.reconcile_phase2_closure(0U, keys.data(), keys.size(),
			first_binding.data(), first_binding.size()));
	ASSERT_NE(0U, first_binding[0].entity_id);

	controller.purge_all(detail::SessionCloseReason::MissionDiscontinuity);
	EXPECT_EQ(0U, controller.active_slots());
	EXPECT_FALSE(controller.has_output());
	const auto second_endpoint = endpoint(3U, 42044U);
	establish_ready(controller, second_endpoint,
		0x402U, 2'000U, 0U);
	const auto second_session = controller.slot(0U).session_id;
	EXPECT_NE(first_session, second_session);
	std::array<telemetry::Phase2Wp05SubjectBinding, 1U> second_binding{};
	ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		controller.reconcile_phase2_closure(0U, keys.data(), keys.size(),
			second_binding.data(), second_binding.size()));
	EXPECT_EQ(first_binding[0].entity_id, second_binding[0].entity_id)
		<< "Entity IDs restart only inside the new session namespace.";
}

TEST(Phase2Runtime, TST069SupportOrDockedSignatureReentryNeverReactivatesTheOldEntity)
{
	detail::Phase2RuntimeSlot slot;
	ASSERT_TRUE(slot.configure(0U));
	const std::array<detail::Phase2CaptureLocalKey, 2U> initial{{
		{41U}, {77U}}};
	std::array<telemetry::Phase2Wp05SubjectBinding, 2U> bindings{};
	ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		slot.reconcile_closure(initial.data(), initial.size(),
			bindings.data(), bindings.size()));
	const auto old_related_id = bindings[1].entity_id;

	const std::array<detail::Phase2CaptureLocalKey, 1U> player{{{41U}}};
	ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		slot.reconcile_closure(player.data(), player.size(),
			bindings.data(), bindings.size()));
	EXPECT_EQ(1U, slot.closure_size());

	ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		slot.reconcile_closure(initial.data(), initial.size(),
			bindings.data(), bindings.size()));
	EXPECT_GT(bindings[1].entity_id, old_related_id);
	EXPECT_NE(bindings[1].entity_id, old_related_id);
}

TEST(Phase2Runtime, CockpitClosureAdoptsSensorIdentityAcrossRemovalAndReentry)
{
	detail::Phase2RuntimeSlot slot;
	ASSERT_TRUE(slot.configure(0U));
	const std::array<detail::Phase2CaptureLocalKey, 2U> first_signatures{{
		{4101U}, {7701U}}};
	const std::array<detail::Phase2CaptureLocalKey, 2U> first_binding_keys{{
		{1U}, {2U}}};
	const std::array<std::uint64_t, 2U> first_public_ids{{1U, 2U}};
	std::array<telemetry::Phase2Wp05SubjectBinding, 2U> bindings{};
	ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		slot.reconcile_closure_with_public_ids(first_signatures.data(),
			first_binding_keys.data(), first_public_ids.data(),
			first_signatures.size(), bindings.data(), bindings.size()));
	EXPECT_EQ(1U, bindings[0].capture_key.value);
	EXPECT_EQ(2U, bindings[1].capture_key.value);
	EXPECT_EQ(2U, bindings[1].entity_id);

	ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		slot.reconcile_closure_with_public_ids(first_signatures.data(),
			first_binding_keys.data(), first_public_ids.data(), 1U,
			bindings.data(), bindings.size()));

	// The replacement already existed as a sensor-only radar identity before
	// it joined the cockpit closure. Its authoritative public ID must be
	// adopted instead of allocating the closure-local key "2" again.
	const std::array<detail::Phase2CaptureLocalKey, 2U> replacement_signatures{{
		{4101U}, {8801U}}};
	const std::array<std::uint64_t, 2U> replacement_public_ids{{1U, 21U}};
	ASSERT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		slot.reconcile_closure_with_public_ids(replacement_signatures.data(),
			first_binding_keys.data(), replacement_public_ids.data(),
			replacement_signatures.size(), bindings.data(), bindings.size()));
	EXPECT_EQ(2U, bindings[1].capture_key.value);
	EXPECT_EQ(21U, bindings[1].entity_id);
	auto observation = std::make_unique<detail::Phase2ObservationDto>();
	observation->ships.resize(2U);
	observation->ships[0].capture_key = first_binding_keys[0];
	observation->ships[1].capture_key = first_binding_keys[1];
	observation->ships[0].lifecycle.state =
		detail::ShipLifecycleState::Present;
	observation->ships[1].lifecycle.state =
		detail::ShipLifecycleState::Present;
	observation->ships[0].lifecycle.sample_time_us = 100U;
	observation->ships[1].lifecycle.sample_time_us = 100U;
	EXPECT_EQ(detail::Phase2RuntimeResult::SnapshotRequired,
		slot.observe_lifecycle(*observation, bindings.data(), bindings.size()));
}

TEST(Phase2Runtime, TST018FaultsOnlyTargetAndPreservesOtherExposedOutput)
{
	ControllerIdentity identity;
	auto controller = make_controller(identity);
	const auto first_endpoint = endpoint(2U, 42043U);
	const auto second_endpoint = endpoint(3U, 42044U);
	establish_ready(controller, first_endpoint,
		0x101U, 1'000U, 0U);
	establish_ready(controller, second_endpoint,
		0x202U, 2'000U, 1U);
	ASSERT_TRUE(controller.begin_initial_snapshot(
		1U, one_atom_image(0x22U), 3'000U));
	ASSERT_EQ(1U, controller.service_initial_snapshot_egress(
		1U, 3'001U));
	detail::SessionControllerOutput exposed_other;
	ASSERT_TRUE(controller.peek_output(exposed_other));
	ASSERT_EQ(second_endpoint, exposed_other.endpoint);
	const auto exposed_other_view = decode_output(exposed_other);
	ASSERT_EQ(protocol::MessageType::FullSnapshot,
		exposed_other_view.header.message_type);
	const auto other_message_id =
		exposed_other_view.header.message_id;
	const auto other_message_crc =
		exposed_other_view.header.message_crc32;
	const auto other_bytes = exposed_other.bytes;
	const auto other_size = exposed_other.size;

	const auto result =
		controller.reject_cockpit_coverage_mutation_for_slot(
			0U,
			telemetry::CockpitCoverageMutationSource::CapabilityUpdate);
	EXPECT_EQ(protocol::ValidationError::InvalidStateTransition,
		result.error);
	ASSERT_EQ(protocol::SessionEndReason::ProtocolError,
		result.session_end_reason);
	ASSERT_EQ(protocol::SessionEndFlagReconnectAllowed,
		result.session_end_flags);
	ASSERT_EQ(telemetry::Phase2SessionSlotState::FaultedSession,
		result.slot_state);
	EXPECT_EQ(detail::ProducerSessionProgress::FaultedSession,
		controller.slot(0U).progress);
	EXPECT_EQ(detail::ProducerSessionProgress::ReadyForState,
		controller.slot(1U).progress);
	EXPECT_FALSE(controller.slot(0U).snapshot.has_active_baseline());
	EXPECT_FALSE(controller.slot(0U).snapshot.has_candidate());
	EXPECT_TRUE(controller.slot(0U).snapshot.current_state().empty());
	EXPECT_FALSE(controller.slot(0U).delta_egress.has_delta());
	EXPECT_EQ(0U,
		controller.slot(0U).phase2_runtime.active_snapshot_id());

	detail::SessionControllerOutput preserved_other;
	ASSERT_TRUE(controller.peek_output(preserved_other));
	EXPECT_EQ(second_endpoint, preserved_other.endpoint);
	ASSERT_EQ(other_size, preserved_other.size);
	EXPECT_EQ(other_bytes, preserved_other.bytes);
	ASSERT_TRUE(controller.pop_output(preserved_other));

	controller.service_reliability(2'001U);
	detail::SessionControllerOutput session_end_output;
	ASSERT_TRUE(controller.peek_output(session_end_output));
	EXPECT_EQ(first_endpoint, session_end_output.endpoint);
	const auto session_end_view = decode_output(session_end_output);
	EXPECT_EQ(protocol::MessageType::SessionEnd,
		session_end_view.header.message_type);
	EXPECT_NE(0U, static_cast<std::uint8_t>(
		session_end_view.header.flags &
		protocol::MessageFlagAckRequired));
	protocol::SessionEndPayload end;
	ASSERT_EQ(protocol::ValidationError::None,
		protocol::decode_session_end_payload(
			session_end_view.payload, end));
	EXPECT_EQ(protocol::SessionEndReason::ProtocolError,
		end.reason);
	EXPECT_EQ(protocol::SessionEndFlagReconnectAllowed,
		end.end_flags);
	const auto retained_message_id =
		session_end_view.header.message_id;
	const auto retained_crc =
		session_end_view.header.message_crc32;
	ASSERT_TRUE(controller.pop_output(session_end_output));
	EXPECT_EQ(1U, controller.slot(0U).reliable_items_in_use);

	bool saw_other_snapshot_retry = false;
	bool saw_session_end_retry = false;
	for (std::size_t attempt = 0U; attempt < 4U &&
		 !saw_session_end_retry; ++attempt) {
		controller.service_reliability(
			2'000'000U + attempt);
		detail::SessionControllerOutput retransmission;
		ASSERT_TRUE(controller.peek_output(retransmission));
		const auto retransmission_view =
			decode_output(retransmission);
		EXPECT_NE(0U, static_cast<std::uint8_t>(
			retransmission_view.header.flags &
			protocol::MessageFlagRetransmission));
		if (retransmission.endpoint == second_endpoint) {
			EXPECT_EQ(protocol::MessageType::FullSnapshot,
				retransmission_view.header.message_type);
			EXPECT_EQ(other_message_id,
				retransmission_view.header.message_id);
			EXPECT_EQ(other_message_crc,
				retransmission_view.header.message_crc32);
			saw_other_snapshot_retry = true;
		} else {
			EXPECT_EQ(first_endpoint,
				retransmission.endpoint);
			EXPECT_EQ(protocol::MessageType::SessionEnd,
				retransmission_view.header.message_type);
			EXPECT_EQ(retained_message_id,
				retransmission_view.header.message_id);
			EXPECT_EQ(retained_crc,
				retransmission_view.header.message_crc32);
			saw_session_end_retry = true;
		}
		ASSERT_TRUE(controller.pop_output(retransmission));
	}
	EXPECT_TRUE(saw_other_snapshot_retry)
		<< "The unrelated reliable snapshot remains intact and due.";
	EXPECT_TRUE(saw_session_end_retry)
		<< "The fault SESSION_END must make progress behind other due work.";
}

TEST(Phase2Runtime, TST018SameSlotExposedOutputIsReplacedBySessionEnd)
{
	ControllerIdentity identity;
	auto controller = make_controller(identity);
	const auto target_endpoint = endpoint(2U, 42043U);
	establish_ready(controller, target_endpoint,
		0x303U, 1'000U, 0U);
	ASSERT_TRUE(controller.begin_initial_snapshot(
		0U, one_atom_image(0x33U), 2'000U));
	ASSERT_EQ(1U, controller.service_initial_snapshot_egress(
		1U, 2'001U));
	detail::SessionControllerOutput welcome;
	ASSERT_TRUE(controller.peek_output(welcome));
	ASSERT_EQ(protocol::MessageType::FullSnapshot,
		decode_output(welcome).header.message_type);

	const auto result =
		controller.reject_cockpit_coverage_mutation_for_slot(
			0U, telemetry::CockpitCoverageMutationSource::Delta);
	ASSERT_EQ(protocol::ValidationError::InvalidStateTransition,
		result.error);
	ASSERT_EQ(detail::ProducerSessionProgress::FaultedSession,
		controller.slot(0U).progress);
	detail::SessionControllerOutput replacement;
	ASSERT_TRUE(controller.peek_output(replacement));
	EXPECT_EQ(target_endpoint, replacement.endpoint);
	const auto replacement_view = decode_output(replacement);
	EXPECT_EQ(protocol::MessageType::SessionEnd,
		replacement_view.header.message_type);
	protocol::SessionEndPayload end;
	ASSERT_EQ(protocol::ValidationError::None,
		protocol::decode_session_end_payload(
			replacement_view.payload, end));
	EXPECT_EQ(protocol::SessionEndReason::ProtocolError,
		end.reason);
	EXPECT_EQ(protocol::SessionEndFlagReconnectAllowed,
		end.end_flags);
}

} // namespace
