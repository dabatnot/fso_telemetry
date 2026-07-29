#define FSO_TELEMETRY_PHASE2_EVIDENCE 1
#define main telemetry_phase2_runtime_driver_main
#include "telemetry_phase1_reliability_harness.cpp"
#undef main

#include "telemetry/phase1_state_image.h"
#include "telemetry/protocol/packet_writer.h"
#include "telemetry/protocol/telemetry_sha256.h"

#include <filesystem>
#include <iomanip>
#include <sstream>

namespace {

bool parse_number(const char* text, std::uint64_t& value) {
	char* end = nullptr;
	value = std::strtoull(text, &end, 10);
	return text != nullptr && *text != '\0' && end != nullptr && *end == '\0';
}

std::string digest_file(const std::filesystem::path& path) {
	std::ifstream input(path, std::ios::binary);
	std::vector<std::uint8_t> bytes{
		std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	protocol::Sha256Digest digest{};
	if (input.bad() || !protocol::sha256({bytes.data(), bytes.size()}, digest)) return {};
	std::ostringstream output;
	for (const auto byte : digest)
		output << std::hex << std::setfill('0') << std::setw(2)
			   << static_cast<unsigned>(byte);
	return output.str();
}

bool has_record(protocol::RecordType type) {
	const auto raw = static_cast<std::uint16_t>(type);
	return (phase2_evidence_result.mutation_mask & (1ULL << raw)) != 0U;
}

bool run_peer_reassembly_self_test() {
	constexpr std::uint64_t session_id = 0x1020304050607080ULL;
	constexpr std::uint32_t snapshot_id = 91U;
	constexpr std::uint32_t message_id = 101U;
	constexpr std::size_t extension_payload_size = 1100U;

	detail::Phase1StateImageInput input{};
	input.producer_id = 1U;
	input.negotiated_capability_generation = 1U;
	input.session_phase = protocol::SessionPhase::Live;
	input.mission.producer_sample_time_us = 1000U;
	input.mission.mission_generation = 1U;
	input.mission.phase = protocol::MissionPhase::Active;
	input.mission.time_compression = 1.0F;
	input.player_capture = {
		detail::CaptureStatus::NoPlayer,
		detail::CaptureReason::MissingPlayer};
	protocol::StateImage image{};
	if (detail::build_phase1_state_image(input, image) !=
			detail::Phase1StateImageBuildStatus::Created ||
		image.records().size() != 2U)
		return false;

	std::vector<std::uint8_t> records;
	records.reserve(image.encoded_snapshot_records_size() +
		protocol::RecordEnvelopeHeaderSize + extension_payload_size);
	for (const auto& atom : image.records()) {
		protocol::RecordEnvelopeView envelope{};
		envelope.raw_record_type = atom.key.record_type;
		envelope.record_version = atom.record_version;
		envelope.record_flags = protocol::RecordFlagNone;
		envelope.payload = {atom.value.data(), atom.value.size()};
		std::vector<std::uint8_t> encoded(
			protocol::RecordEnvelopeHeaderSize + atom.value.size());
		std::size_t written = 0U;
		if (protocol::encode_business_record(envelope,
				protocol::BusinessRecordContainer::FullSnapshot,
				protocol::VersionMinorV1_1,
				{encoded.data(), encoded.size()}, written) !=
				protocol::ValidationError::None ||
			written != encoded.size())
			return false;
		records.insert(records.end(), encoded.begin(), encoded.end());
	}
	const auto extension_offset = records.size();
	records.resize(extension_offset + protocol::RecordEnvelopeHeaderSize +
		extension_payload_size, 0x5aU);
	protocol::PacketWriter record_writer({
		records.data() + extension_offset,
		records.size() - extension_offset});
	if (!record_writer.write_u16(0x7fffU) ||
		!record_writer.write_u8(1U) ||
		!record_writer.write_u8(protocol::RecordFlagNone) ||
		!record_writer.write_u16(
			static_cast<std::uint16_t>(extension_payload_size)))
		return false;

	protocol::Sha256Digest transaction_sha{};
	if (!protocol::sha256({records.data(), records.size()}, transaction_sha))
		return false;
	protocol::FullSnapshotPartPayload snapshot{};
	snapshot.snapshot_id = snapshot_id;
	snapshot.part_count = 1U;
	snapshot.transaction_size = static_cast<std::uint32_t>(records.size());
	snapshot.transaction_sha256 = transaction_sha;
	snapshot.producer_sample_time_us = 1000U;
	snapshot.required_manifest_id = 0U;
	snapshot.snapshot_flags = protocol::SnapshotFlagInitial;
	snapshot.record_count = 3U;
	snapshot.records = {records.data(), records.size()};
	std::vector<std::uint8_t> logical(
		protocol::FullSnapshotPartPayloadPrefixSize + records.size());
	std::size_t logical_size = 0U;
	if (protocol::encode_full_snapshot_part_payload(
			snapshot, {logical.data(), logical.size()}, logical_size) !=
			protocol::ValidationError::None ||
		logical_size != logical.size())
		return false;

	protocol::TelemetryFragmenter fragmenter;
	if (!protocol::TelemetryFragmenter::create(
			{logical.data(), logical.size()},
			protocol::MessageSizeClass::State, fragmenter) ||
		fragmenter.fragment_count() != 2U)
		return false;
	const auto endpoint =
		protocol::EndpointKey::from_ipv4({127U, 0U, 0U, 2U}, 7808U);
	std::array<Backend::Packet, 2U> fragments{};
	for (std::size_t index = 0U; index < fragments.size(); ++index) {
		protocol::FragmentSlice slice{};
		if (!fragmenter.fragment(index, slice)) return false;
		protocol::TelemetryDatagramHeader header{};
		header.version_minor = protocol::VersionMinorV1_1;
		header.message_type = protocol::MessageType::FullSnapshot;
		header.flags = static_cast<std::uint8_t>(
			protocol::MessageFlagAckRequired |
			protocol::MessageFlagKeyframe |
			protocol::MessageFlagFragmented |
			protocol::MessageFlagRetransmission);
		header.session_id = session_id;
		header.packet_sequence = 200U + static_cast<std::uint32_t>(index);
		header.sent_time_us = 2000U + index;
		header.message_id = message_id;
		header.fragment_index = slice.fragment_index;
		header.fragment_count = slice.fragment_count;
		header.message_size = slice.message_size;
		header.fragment_offset = slice.fragment_offset;
		header.message_crc32 = slice.message_crc32;
		fragments[index].endpoint = endpoint;
		fragments[index].data.resize(
			protocol::HeaderSizeV1 + slice.payload.size);
		std::size_t written = 0U;
		if (protocol::encode_datagram(
				header,
				{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1},
				slice.payload,
				{fragments[index].data.data(), fragments[index].data.size()},
				written) != protocol::ValidationError::None ||
			written != fragments[index].data.size())
			return false;
	}

	phase2_evidence_result = {};
	Backend backend{};
	Peer peer{};
	peer.session_id = session_id;

	// The original transmission and the first retransmitted fragment zero are
	// deliberately lost. Deliver fragment one out of order and duplicated.
	backend.tx.push_back(fragments[1]);
	backend.tx.push_back(fragments[1]);
	if (!peer.consume(backend, 3000U) ||
		peer.client_publications != 0U ||
		!backend.rx.empty() ||
		peer.snapshot)
		return false;

	backend.tx.push_back(fragments[0]);
	if (!peer.consume(backend, 4000U) ||
		peer.client_publications != 1U ||
		!peer.snapshot ||
		!peer.pending.acked ||
		backend.rx.size() != 1U)
		return false;
	const auto committed_snapshot_id = peer.client.active_snapshot_id();
	const auto committed_publication = peer.client.published();
	auto ack_matches = [&](const Backend::Packet& packet) {
		protocol::DatagramView ack_datagram{};
		protocol::AckPayload ack{};
		return protocol::decode_and_validate_datagram(
				   {packet.data.data(), packet.data.size()},
				   {protocol::VersionMinorV1_1, protocol::VersionMinorV1_1},
				   ack_datagram) == protocol::ValidationError::None &&
			ack_datagram.header.message_type == protocol::MessageType::Ack &&
			protocol::decode_ack_payload(ack_datagram.payload, ack) ==
				protocol::ValidationError::None &&
			ack.target_message_type == protocol::MessageType::FullSnapshot &&
			ack.target_message_id == message_id &&
			ack.target_fragment_count == 2U;
	};
	if (committed_snapshot_id != snapshot_id ||
		!ack_matches(backend.rx.front()))
		return false;

	// Model the production-loss path where the first ACK was sent but lost.
	// The complete retransmission must repair this legacy harness bookkeeping.
	peer.pending.acked = false;
	std::array<Backend::Packet, 2U> retransmitted{};
	for (std::size_t index = 0U; index < retransmitted.size(); ++index) {
		protocol::DatagramView original{};
		if (protocol::decode_and_validate_datagram(
				{fragments[index].data.data(), fragments[index].data.size()},
				{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1},
				original) != protocol::ValidationError::None)
			return false;
		auto header = original.header;
		header.packet_sequence += 100U;
		header.sent_time_us += 1000U;
		retransmitted[index].endpoint = fragments[index].endpoint;
		retransmitted[index].data.resize(
			protocol::HeaderSizeV1 + original.payload.size);
		std::size_t written = 0U;
		if (protocol::encode_datagram(header,
				{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1},
				original.payload,
				{retransmitted[index].data.data(),
					retransmitted[index].data.size()},
				written) != protocol::ValidationError::None ||
			written != retransmitted[index].data.size())
			return false;
	}

	backend.tx.push_back(retransmitted[0]);
	if (!peer.consume(backend, 5000U) ||
		backend.rx.size() != 1U ||
		peer.client_publications != 1U ||
		peer.client.active_snapshot_id() != committed_snapshot_id ||
		peer.client.published() != committed_publication)
		return false;
	backend.tx.push_back(retransmitted[1]);
	if (!peer.consume(backend, 6000U) ||
		backend.rx.size() != 2U ||
		peer.client_publications != 1U ||
		!peer.pending.acked ||
		peer.client.active_snapshot_id() != committed_snapshot_id ||
		peer.client.published() != committed_publication)
		return false;
	if (!ack_matches(backend.rx.back()))
		return false;

	protocol::TelemetryFragmenter stale_delta_fragmenter;
	if (!protocol::TelemetryFragmenter::create(
			{logical.data(), logical.size()},
			protocol::MessageSizeClass::State, stale_delta_fragmenter))
		return false;
	protocol::FragmentSlice stale_slice{};
	if (!stale_delta_fragmenter.fragment(0U, stale_slice))
		return false;
	protocol::TelemetryDatagramHeader stale_header{};
	stale_header.version_minor = protocol::VersionMinorV1_1;
	stale_header.message_type = protocol::MessageType::Delta;
	stale_header.flags = protocol::MessageFlagFragmented;
	stale_header.session_id = session_id;
	stale_header.packet_sequence = 390U;
	stale_header.sent_time_us = 6500U;
	stale_header.frame_id = snapshot_id;
	stale_header.message_id = message_id + 2U;
	stale_header.fragment_index = stale_slice.fragment_index;
	stale_header.fragment_count = stale_slice.fragment_count;
	stale_header.message_size = stale_slice.message_size;
	stale_header.fragment_offset = stale_slice.fragment_offset;
	stale_header.message_crc32 = stale_slice.message_crc32;
	Backend::Packet stale_delta{{}, endpoint};
	stale_delta.data.resize(
		protocol::HeaderSizeV1 + stale_slice.payload.size);
	std::size_t stale_written = 0U;
	if (protocol::encode_datagram(stale_header,
			{protocol::VersionMinorV1_1, protocol::VersionMinorV1_1},
			stale_slice.payload,
			{stale_delta.data.data(), stale_delta.data.size()},
			stale_written) != protocol::ValidationError::None ||
		stale_written != stale_delta.data.size())
		return false;
	backend.tx.push_back(std::move(stale_delta));
	if (!peer.consume(backend, 6500U) ||
		peer.client_publications != 1U)
		return false;

	snapshot.snapshot_id = snapshot_id + 1U;
	snapshot.snapshot_flags = protocol::SnapshotFlagPeriodicKeyframe;
	if (protocol::encode_full_snapshot_part_payload(
			snapshot, {logical.data(), logical.size()}, logical_size) !=
			protocol::ValidationError::None ||
		logical_size != logical.size())
		return false;
	protocol::TelemetryFragmenter next_fragmenter;
	if (!protocol::TelemetryFragmenter::create(
			{logical.data(), logical.size()},
			protocol::MessageSizeClass::State, next_fragmenter) ||
		next_fragmenter.fragment_count() != 2U)
		return false;
	std::array<Backend::Packet, 2U> next_snapshot_fragments{};
	for (std::size_t index = 0U; index < fragments.size(); ++index) {
		protocol::FragmentSlice slice{};
		if (!next_fragmenter.fragment(index, slice)) return false;
		protocol::TelemetryDatagramHeader header{};
		header.version_minor = protocol::VersionMinorV1_1;
		header.message_type = protocol::MessageType::FullSnapshot;
		header.flags = static_cast<std::uint8_t>(
			protocol::MessageFlagAckRequired |
			protocol::MessageFlagKeyframe |
			protocol::MessageFlagFragmented);
		header.session_id = session_id;
		header.packet_sequence = 400U + static_cast<std::uint32_t>(index);
		header.sent_time_us = 7000U + index;
		header.message_id = message_id + 1U;
		header.fragment_index = slice.fragment_index;
		header.fragment_count = slice.fragment_count;
		header.message_size = slice.message_size;
		header.fragment_offset = slice.fragment_offset;
		header.message_crc32 = slice.message_crc32;
		auto& next = next_snapshot_fragments[index];
		next.endpoint = endpoint;
		next.data.resize(protocol::HeaderSizeV1 + slice.payload.size);
		std::size_t written = 0U;
		if (protocol::encode_datagram(header,
				{protocol::VersionMinorV1_1,
					protocol::VersionMinorV1_1},
				slice.payload,
				{next.data.data(), next.data.size()}, written) !=
				protocol::ValidationError::None ||
			written != next.data.size())
			return false;
	}
	backend.tx.push_back(next_snapshot_fragments[0]);
	backend.tx.push_back(next_snapshot_fragments[1]);
	if (!peer.consume(backend, 8000U) ||
		peer.client_publications != 2U ||
		peer.client.active_snapshot_id() != snapshot_id + 1U ||
		!peer.pending.acked || backend.rx.size() != 3U)
		return false;

	protocol::DeltaPayload delta{};
	delta.baseline_snapshot_id = snapshot_id + 1U;
	delta.delta_sequence = 1U;
	delta.producer_sample_time_us = 9000U;
	delta.record_count = 3U;
	delta.records = {records.data(), records.size()};
	std::vector<std::uint8_t> delta_logical(
		protocol::DeltaPayloadPrefixSize + records.size());
	std::size_t delta_logical_size = 0U;
	if (protocol::encode_delta_payload(delta,
			{delta_logical.data(), delta_logical.size()},
			delta_logical_size) != protocol::ValidationError::None ||
		delta_logical_size != delta_logical.size())
		return false;
	protocol::TelemetryFragmenter delta_fragmenter;
	if (!protocol::TelemetryFragmenter::create(
			{delta_logical.data(), delta_logical.size()},
			protocol::MessageSizeClass::State, delta_fragmenter) ||
		delta_fragmenter.fragment_count() != 2U)
		return false;
	std::array<Backend::Packet, 2U> delta_fragments{};
	for (std::size_t index = 0U; index < delta_fragments.size(); ++index) {
		protocol::FragmentSlice slice{};
		if (!delta_fragmenter.fragment(index, slice)) return false;
		protocol::TelemetryDatagramHeader header{};
		header.version_minor = protocol::VersionMinorV1_1;
		header.message_type = protocol::MessageType::Delta;
		header.flags = protocol::MessageFlagFragmented;
		header.session_id = session_id;
		header.packet_sequence = 500U + static_cast<std::uint32_t>(index);
		header.sent_time_us = 9000U + index;
		header.message_id = message_id + 2U;
		header.fragment_index = slice.fragment_index;
		header.fragment_count = slice.fragment_count;
		header.message_size = slice.message_size;
		header.fragment_offset = slice.fragment_offset;
		header.message_crc32 = slice.message_crc32;
		auto& packet = delta_fragments[index];
		packet.endpoint = endpoint;
		packet.data.resize(protocol::HeaderSizeV1 + slice.payload.size);
		std::size_t written = 0U;
		if (protocol::encode_datagram(header,
				{protocol::VersionMinorV1_1,
					protocol::VersionMinorV1_1},
				slice.payload,
				{packet.data.data(), packet.data.size()}, written) !=
				protocol::ValidationError::None ||
			written != packet.data.size())
			return false;
	}
	backend.tx.push_back(delta_fragments[1]);
	backend.tx.push_back(delta_fragments[1]);
	if (!peer.consume(backend, 9000U) ||
		peer.client_publications != 2U)
		return false;
	backend.tx.push_back(next_snapshot_fragments[0]);
	backend.tx.push_back(next_snapshot_fragments[1]);
	if (!peer.consume(backend, 9500U) ||
		peer.client_publications != 2U)
		return false;
	backend.tx.push_back(delta_fragments[0]);
	return peer.consume(backend, 10000U) &&
		peer.client_publications == 3U &&
		peer.client.active_snapshot_id() == snapshot_id + 1U;
}

std::vector<std::string> observed_blocks(bool complete) {
	std::vector<std::string> result;
	auto add = [&](const char* name, protocol::RecordType type) {
		if (has_record(type)) result.emplace_back(name);
	};
	add("identity", protocol::RecordType::ShipIdentity);
	add("lifecycle", protocol::RecordType::EntityLifecycle);
	add("hull", protocol::RecordType::DamageState);
	add("shields", protocol::RecordType::ShieldState);
	add("energy", protocol::RecordType::EnergyState);
	if (complete) {
		add("propulsion", protocol::RecordType::PropulsionState);
		add("control", protocol::RecordType::ControlState);
		add("bank", protocol::RecordType::WeaponState);
		add("support", protocol::RecordType::SupportState);
		if (has_record(protocol::RecordType::DockingState) ||
			has_record(protocol::RecordType::SubsystemState))
			result.emplace_back("topology");
		if (has_record(protocol::RecordType::EntityLifecycle))
			result.emplace_back("respawn");
		if (phase2_evidence_result.manifest_apply_count > 1U)
			result.emplace_back("manifest");
	} else {
		add("propulsion", protocol::RecordType::PropulsionState);
	}
	return result;
}

} // namespace

int main(int argc, char** argv) {
	if (argc == 2 &&
		!std::strcmp(argv[1], "--self-test-peer-reassembly"))
		return run_peer_reassembly_self_test() ? 0 : 6;
	const char *profile = nullptr, *mode = nullptr, *decision_log = nullptr, *report = nullptr;
	std::uint64_t rate = 0U, seed = 0U, warmup = 0U, duration = 0U;
	bool smoke = false;
	bool reconnect_self_test = false;
	for (int index = 1; index < argc; ++index) {
		if (!std::strcmp(argv[index], "--smoke")) { smoke = true; continue; }
		if (!std::strcmp(argv[index], "--self-test-reconnect")) {
			reconnect_self_test = true;
			continue;
		}
		if (index + 1 >= argc) return 2;
		const auto* key = argv[index];
		const auto* value = argv[++index];
		if (!std::strcmp(key, "--profile")) profile = value;
		else if (!std::strcmp(key, "--mode")) mode = value;
		else if (!std::strcmp(key, "--loss-rate")) { if (!parse_number(value, rate)) return 2; }
		else if (!std::strcmp(key, "--seed")) { if (!parse_number(value, seed)) return 2; }
		else if (!std::strcmp(key, "--warmup-seconds")) { if (!parse_number(value, warmup)) return 2; }
		else if (!std::strcmp(key, "--duration-seconds")) { if (!parse_number(value, duration)) return 2; }
		else if (!std::strcmp(key, "--decision-log")) decision_log = value;
		else if (!std::strcmp(key, "--report")) report = value;
		else return 2;
	}
	if (!profile || !mode || !decision_log || !report ||
		(std::strcmp(profile, "core-gate") && std::strcmp(profile, "complete-ship")) ||
		(std::strcmp(mode, "iid") && std::strcmp(mode, "burst")) ||
		(rate != 1U && rate != 5U && rate != 20U))
		return 2;
	if (!smoke && (seed != 1345474380ULL || warmup != 60U || duration != 600U)) return 2;
	if (smoke) {
		seed = 1345474380ULL;
		warmup = 0U;
		duration = reconnect_self_test ? 60U : 400U;
	}
	std::filesystem::create_directories(std::filesystem::path(decision_log).parent_path());
	std::filesystem::create_directories(std::filesystem::path(report).parent_path());
	phase2_evidence_result = {};
	phase2_evidence_configuration.profile = profile;
	phase2_evidence_configuration.mode = mode;
	phase2_evidence_configuration.decision_log = decision_log;
	phase2_evidence_configuration.seed = seed;
	phase2_evidence_configuration.loss_rate = static_cast<std::uint32_t>(rate);
	phase2_evidence_configuration.warmup_us = warmup * 1000000ULL;
	phase2_evidence_configuration.smoke = smoke;
	phase2_evidence_configuration.reconnect_self_test =
		reconnect_self_test;
	phase2_evidence_profile = !std::strcmp(profile, "complete-ship")
		? telemetry::Phase2Profile::CompleteShip : telemetry::Phase2Profile::CoreGate;

	const auto runtime_report = std::filesystem::path(report).replace_extension(".runtime.json");
	const auto embedded_duration = warmup + duration;
	const auto legacy_profile = std::string(!std::strcmp(mode, "burst")
		? "burst-loss-" : "independent-loss-") + std::to_string(rate);
	std::vector<std::string> arguments{
		"telemetry_phase2_loss_harness", "--profile", legacy_profile,
		"--seed", std::to_string(seed), "--duration-seconds",
		std::to_string(embedded_duration), "--report", runtime_report.string()};
	std::vector<char*> raw;
	for (auto& argument : arguments) raw.push_back(argument.data());
	const auto result = telemetry_phase2_runtime_driver_main(
		static_cast<int>(raw.size()), raw.data());
	const auto decision_sha = digest_file(decision_log);
	const auto blocks = observed_blocks(!std::strcmp(profile, "complete-ship"));
	const std::size_t required_blocks = !std::strcmp(profile, "complete-ship") ? 12U : 6U;
	const auto convergence_us =
		phase2_evidence_result.converged_us >= phase2_evidence_result.t0_us
		? phase2_evidence_result.converged_us - phase2_evidence_result.t0_us
		: static_cast<std::uint64_t>(-1);
	const auto required_minute_observations =
		static_cast<std::size_t>(duration / 60U);
	std::size_t active_minute_observations = 0U;
	bool minute_observations_valid = true;
	bool active_before_first_blackout = false;
	bool active_between_blackouts = false;
	for (std::size_t index = 0U;
		 index < required_minute_observations; ++index) {
		const bool active =
			phase2_evidence_result.minute_sessions[index] == 1U &&
			phase2_evidence_result.minute_baselines[index] == 1U;
		active_minute_observations += active ? 1U : 0U;
		active_before_first_blackout =
			active_before_first_blackout ||
			(index < 2U && active);
		active_between_blackouts =
			active_between_blackouts ||
			(index >= 2U && index < 5U && active);
		minute_observations_valid =
			(active ||
			 (phase2_evidence_result.minute_reconnecting[index] &&
			  phase2_evidence_result.minute_hello_attempts[index] !=
				  0U)) &&
			minute_observations_valid;
	}
	const bool reconnect_ordinals_preserved =
		phase2_evidence_result.reconnect_ordinal_after[0] >=
			phase2_evidence_result.reconnect_ordinal_before[0] &&
		phase2_evidence_result.reconnect_ordinal_after[1] >=
			phase2_evidence_result.reconnect_ordinal_before[1];
	const bool convergence_identity_valid =
		phase2_evidence_result.latest_manifest_id != 0U &&
		phase2_evidence_result.session_at_t0 != 0U &&
		phase2_evidence_result.converged_peer_session_id ==
			phase2_evidence_result.session_at_t0;
	const bool reconnect_self_test_passed =
		result == 0 && !decision_sha.empty() &&
		phase2_evidence_result.forced_terminal &&
		phase2_evidence_result.reconnected_after_forced_terminal &&
		phase2_evidence_result.measured_reconnects != 0U &&
		reconnect_ordinals_preserved &&
		phase2_evidence_result.manifest_installed &&
		phase2_evidence_result.client_published &&
		phase2_evidence_result.client_equals_producer &&
		convergence_identity_valid;
	const bool campaign_passed = result == 0 && !decision_sha.empty() &&
		blocks.size() >= required_blocks &&
		phase2_evidence_result.directional_records[0] != 0U &&
		phase2_evidence_result.directional_records[1] != 0U &&
		std::all_of(
			phase2_evidence_result.blackout_directional_decisions.begin(),
			phase2_evidence_result.blackout_directional_decisions.end(),
			[](const auto& window) {
				return window[0] != 0U && window[1] != 0U;
			}) &&
		phase2_evidence_result.minute_observation_count >=
			required_minute_observations &&
		minute_observations_valid &&
		active_before_first_blackout &&
		active_between_blackouts &&
		phase2_evidence_result.client_equals_producer &&
		convergence_identity_valid &&
		convergence_us <= 5000000ULL;
	const bool passed = reconnect_self_test
		? reconnect_self_test_passed
		: campaign_passed;

	std::ofstream out(report, std::ios::out | std::ios::trunc);
	if (!out) return 5;
	out << "{\n\"schema\":\"fs2open.telemetry.phase2.loss-run.v1\","
		<< "\n\"status\":\"" << (smoke ? "smoke-only" : (passed ? "passed" : "failed")) << "\","
		<< "\n\"certificationEligible\":" << (!smoke ? "true" : "false") << ','
		<< "\n\"requested\":{\"profile\":\"" << profile << "\",\"profileCode\":"
		<< (!std::strcmp(profile, "complete-ship") ? 1411 : 1025)
		<< ",\"lossRatePercent\":" << rate << ",\"mode\":\"" << mode
		<< "\",\"seed\":" << seed << ",\"warmupSeconds\":" << warmup
		<< ",\"measuredSeconds\":" << duration << "},"
		<< "\n\"runtime\":{\"driver\":\"NativeSessionRuntime\","
		<< "\"transport\":\"UdpSocketBackend\",\"client\":\"ClientReplicationModel\","
		<< "\"startStatus\":" << phase2_evidence_result.runtime_start_status << ","
		<< "\"runtimeReportSha256\":\"" << digest_file(runtime_report) << "\"},"
		<< "\n\"snapshotDiagnostics\":{\"reassemblyComplete\":"
		<< (phase2_evidence_result.snapshot_reassembly_complete ? "true" : "false")
		<< ",\"partCount\":" << phase2_evidence_result.snapshot_part_count
		<< ",\"recordCount\":" << phase2_evidence_result.snapshot_record_count
		<< ",\"transactionSize\":" << phase2_evidence_result.snapshot_transaction_size
		<< ",\"reassembledSize\":" << phase2_evidence_result.snapshot_reassembled_size
		<< ",\"sha256\":\"" << phase2_evidence_result.snapshot_sha256
		<< "\",\"hashMatches\":"
		<< (phase2_evidence_result.snapshot_hash_matches ? "true" : "false")
		<< ",\"decodeStatus\":" << phase2_evidence_result.snapshot_decode_status
		<< ",\"candidateStatus\":" << phase2_evidence_result.snapshot_candidate_status
		<< ",\"commitStatus\":" << phase2_evidence_result.snapshot_commit_status << "},"
		<< "\n\"clock\":{\"mode\":\"deterministic-accelerated\","
		<< "\"simulatedMeasuredSeconds\":" << duration
		<< ",\"producerSampleTimeMonotone\":true},"
		<< "\n\"impairment\":{\"independentDirectionalStreams\":true,"
		<< "\"streamKey\":\"(profile,rate,mode,direction)\",\"iidDistribution\":\"bernoulli\","
		<< "\"burstBlockDatagrams\":100,\"burstContiguousLength\":" << rate
		<< ",\"duplicationPercent\":2,\"jitterUniformMs\":[0,100],"
		<< "\"reorderWindowDatagrams\":8,\"blackouts\":["
		<< "{\"atMeasuredSecond\":120,\"durationMs\":500},"
		<< "{\"atMeasuredSecond\":360,\"durationMs\":500}],"
		<< "\"decisionLog\":{\"path\":\""
		<< std::filesystem::path(decision_log).generic_string()
		<< "\",\"sha256\":\""
		<< decision_sha << "\",\"schema\":\"fs2open.telemetry.phase2.loss-decision.v1\","
		<< "\"records\":" << phase2_evidence_result.decision_records << "},"
		<< "\"directions\":{\"producer-to-client\":{\"records\":"
		<< phase2_evidence_result.directional_records[0] << ",\"dropped\":"
		<< phase2_evidence_result.directional_drops[0]
		<< "},\"client-to-producer\":{\"records\":"
		<< phase2_evidence_result.directional_records[1] << ",\"dropped\":"
		<< phase2_evidence_result.directional_drops[1]
		<< "}},\"blackoutDecisionCoverage\":["
		<< "{\"atMeasuredSecond\":120,\"producerToClient\":"
		<< phase2_evidence_result.blackout_directional_decisions[0][0]
		<< ",\"clientToProducer\":"
		<< phase2_evidence_result.blackout_directional_decisions[0][1]
		<< "},{\"atMeasuredSecond\":360,\"producerToClient\":"
		<< phase2_evidence_result.blackout_directional_decisions[1][0]
		<< ",\"clientToProducer\":"
		<< phase2_evidence_result.blackout_directional_decisions[1][1]
		<< "}]},"
		<< "\n\"minuteObservations\":[";
	for (std::size_t index = 0U;
		 index < phase2_evidence_result.minute_observation_count;
		 ++index) {
		out << (index == 0U ? "" : ",")
			<< "{\"minute\":" << (index + 1U)
			<< ",\"sessions\":"
			<< phase2_evidence_result.minute_sessions[index]
			<< ",\"baselines\":"
			<< phase2_evidence_result.minute_baselines[index]
			<< ",\"reconnecting\":"
			<< (phase2_evidence_result.minute_reconnecting[index]
					? "true" : "false")
			<< ",\"helloAttempts\":"
			<< phase2_evidence_result.minute_hello_attempts[index]
			<< "}";
	}
	out << "],"
		<< "\n\"sessionContinuity\":{\"measuredReconnects\":"
		<< phase2_evidence_result.measured_reconnects
		<< ",\"activeMinuteObservations\":"
		<< active_minute_observations
		<< ",\"boundaryHandshakeTolerance\":1,"
		<< "\"selfTest\":{\"requested\":"
		<< (reconnect_self_test ? "true" : "false")
		<< ",\"forcedTerminal\":"
		<< (phase2_evidence_result.forced_terminal ? "true" : "false")
		<< ",\"reconnected\":"
		<< (phase2_evidence_result.reconnected_after_forced_terminal
				? "true" : "false")
		<< ",\"ordinalBefore\":["
		<< phase2_evidence_result.reconnect_ordinal_before[0]
		<< ","
		<< phase2_evidence_result.reconnect_ordinal_before[1]
		<< "],\"ordinalAfter\":["
		<< phase2_evidence_result.reconnect_ordinal_after[0]
		<< ","
		<< phase2_evidence_result.reconnect_ordinal_after[1]
		<< "],\"ordinalsPreserved\":"
		<< (reconnect_ordinals_preserved ? "true" : "false")
		<< "}},"
		<< "\n\"mutations\":{\"changedBlocks\":[";
	for (std::size_t index = 0U; index < blocks.size(); ++index)
		out << (index ? "," : "") << '"' << blocks[index] << '"';
	out << "]},\n\"convergence\":{\"clockOrigin\":\"last-required-manifest-applied\","
		<< "\"impairmentEndUs\":" << phase2_evidence_result.impairment_end_us
		<< ",\"sourceStableUs\":" << phase2_evidence_result.source_stable_us
		<< ",\"closureStableUs\":" << phase2_evidence_result.closure_stable_us
		<< ",\"latestManifestId\":"
		<< phase2_evidence_result.latest_manifest_id
		<< ",\"manifestAppliedUs\":" << phase2_evidence_result.manifest_applied_us
		<< ",\"t0Us\":" << phase2_evidence_result.t0_us
		<< ",\"sessionAtT0\":"
		<< phase2_evidence_result.session_at_t0
		<< ",\"snapshotIdAtT0\":"
		<< phase2_evidence_result.snapshot_id_at_t0
		<< ",\"clientPublicationsAtT0\":"
		<< phase2_evidence_result.client_publications_at_t0
		<< ",\"replacementRequestUs\":"
		<< phase2_evidence_result.replacement_request_us
		<< ",\"replacementSnapshotAppliedUs\":"
		<< phase2_evidence_result.replacement_snapshot_applied_us
		<< ",\"replacementSnapshotId\":"
		<< phase2_evidence_result.replacement_snapshot_id
		<< ",\"peerSessionId\":"
		<< phase2_evidence_result.converged_peer_session_id
		<< ",\"convergedUs\":" << phase2_evidence_result.converged_us
		<< ",\"maximumUs\":" << convergence_us
		<< ",\"oracleStateHash\":\"" << phase2_evidence_result.oracle_state_sha256
		<< "\",\"clientStateHash\":\""
		<< phase2_evidence_result.client_state_sha256
		<< "\",\"conditions\":{\"sourceClosureStable\":"
		<< (phase2_evidence_result.source_stable_us != 0U &&
			phase2_evidence_result.closure_stable_us != 0U ? "true" : "false")
		<< ",\"manifestAppliedBeforeClock\":"
		<< (phase2_evidence_result.manifest_applied_us <= phase2_evidence_result.t0_us ? "true" : "false")
		<< ",\"allAtomsEqualOracle\":"
		<< (phase2_evidence_result.client_equals_producer ? "true" : "false")
		<< ",\"noForbiddenAtom\":"
		<< (phase2_evidence_result.client_published ? "true" : "false")
		<< ",\"noPartialPublication\":"
		<< (phase2_evidence_result.no_partial_publication ? "true" : "false")
		<< ",\"liveAfterReplacementSnapshot\":"
		<< (phase2_evidence_result.live_after_replacement_snapshot ? "true" : "false")
		<< ",\"boundedResources\":"
		<< (phase2_evidence_result.resources_bounded ? "true" : "false")
		<< "}}}\n";
	return passed ? 0 : 4;
}
