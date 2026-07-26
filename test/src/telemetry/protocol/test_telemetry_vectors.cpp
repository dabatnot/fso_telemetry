#include "telemetry/protocol/telemetry_business_records.h"
#include "telemetry/protocol/telemetry_business_state_validation.h"
#include "telemetry/protocol/telemetry_control_messages.h"
#include "telemetry/protocol/telemetry_crc32.h"
#include "telemetry/protocol/telemetry_datagram.h"
#include "telemetry/protocol/telemetry_event_messages.h"
#include "telemetry/protocol/telemetry_reassembler.h"
#include "telemetry/protocol/telemetry_reliability_messages.h"
#include "telemetry/protocol/telemetry_reliable_window.h"
#include "telemetry/protocol/telemetry_replication.h"
#include "telemetry/protocol/telemetry_security.h"
#include "telemetry/protocol/telemetry_sha256.h"
#include "telemetry/protocol/telemetry_session_context.h"
#include "telemetry/protocol/telemetry_specialized_lifecycle.h"
#include "telemetry/protocol/telemetry_specialized_views.h"
#include "telemetry/protocol/telemetry_state_messages.h"

#include <gtest/gtest.h>
#include <jansson.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef FSTL_PROTOCOL_TEST_ASSET_PATH
#error "FSTL_PROTOCOL_TEST_ASSET_PATH must point at the external telemetry protocol test assets"
#endif

namespace {

using namespace telemetry::protocol;

template <typename Container>
ByteView byte_view(const Container& bytes) {
	return ByteView{bytes.empty() ? nullptr
	                              : static_cast<const std::uint8_t*>(static_cast<const void*>(bytes.data())),
	                bytes.size()};
}

std::filesystem::path asset_root() {
	return std::filesystem::path{FSTL_PROTOCOL_TEST_ASSET_PATH};
}

std::vector<std::uint8_t> read_binary(const std::filesystem::path& path) {
	std::ifstream stream(path, std::ios::binary);
	if (!stream) {
		ADD_FAILURE() << "cannot open external telemetry vector " << path.string();
		return {};
	}
	return std::vector<std::uint8_t>{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

std::string read_text(const std::filesystem::path& path) {
	std::ifstream stream(path);
	return std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

// Deliberately independent, test-only projection of the small FSTL 1.1 corpus.
// It reads the wire bytes directly instead of reusing the production business
// record decoder, so the fixed JSON files remain a useful second oracle.
class CanonicalReader {
  public:
	explicit CanonicalReader(ByteView input) : m_data(input.data), m_size(input.size) {}
	std::uint8_t u8() { return take<std::uint8_t>(); }
	std::uint16_t u16() { return take<std::uint16_t>(); }
	std::uint32_t u32() { return take<std::uint32_t>(); }
	std::uint64_t u64() { return take<std::uint64_t>(); }
	float f32() { return take<float>(); }
	std::string bytes(std::size_t count) {
		EXPECT_LE(m_offset + count, m_size);
		if (m_offset + count > m_size) return {};
		std::string result(reinterpret_cast<const char*>(m_data + m_offset), count);
		m_offset += count;
		return result;
	}
	std::string utf8() { return bytes(u16()); }
	ByteView view(std::size_t count) {
		EXPECT_LE(m_offset + count, m_size);
		if (m_offset + count > m_size) return {};
		const ByteView result{m_data + m_offset, count};
		m_offset += count;
		return result;
	}
	std::size_t remaining() const { return m_size - m_offset; }
  private:
	template <typename T> T take() {
		EXPECT_LE(m_offset + sizeof(T), m_size);
		T value{};
		if (m_offset + sizeof(T) <= m_size) std::memcpy(&value, m_data + m_offset, sizeof(T));
		m_offset += std::min(sizeof(T), m_size - m_offset);
		return value;
	}
	const std::uint8_t* m_data = nullptr;
	std::size_t m_size = 0U;
	std::size_t m_offset = 0U;
};

void put(json_t* object, const char* key, json_t* value) { ASSERT_EQ(0, json_object_set_new(object, key, value)); }
json_t* ji(std::uint64_t value) { return json_integer(static_cast<json_int_t>(value)); }
json_t* js(std::uint64_t value) { return json_string(std::to_string(value).c_str()); }
json_t* jr(float value) { return json_real(value == 0.0F ? 0.0 : static_cast<double>(value)); }
json_t* vector_json(CanonicalReader& reader, std::size_t count) {
	auto* array = json_array();
	for (std::size_t i = 0; i < count; ++i) EXPECT_EQ(0, json_array_append_new(array, jr(reader.f32())));
	return array;
}
std::string hex_string(const std::string& bytes) {
	static constexpr char digits[] = "0123456789abcdef";
	std::string result;
	result.reserve(bytes.size() * 2U);
	for (const auto byte : bytes) {
		const auto value = static_cast<unsigned char>(byte);
		result.push_back(digits[value >> 4U]); result.push_back(digits[value & 15U]);
	}
	return result;
}

json_t* canonical_v11_record(CanonicalReader& region) {
	const auto type = region.u16(); const auto version = region.u8(); const auto flags = region.u8();
	const auto length = region.u16(); CanonicalReader reader(region.view(length));
	auto* fields = json_object();
	auto entity_prefix = [&]() { const auto entity = reader.u64(); const auto presence = reader.u64();
		const auto sample = reader.u64(); put(fields, "entity_id", js(entity)); put(fields, "presence", js(presence));
		put(fields, "producer_sample_time_us", js(sample)); return presence; };
	const char* name = "";
	if (type == 1U) {
		name = "SESSION_STATE"; const auto presence = reader.u64(); const auto producer = reader.u64();
		const auto sample = reader.u64(); const auto authority = reader.u8(); const auto visibility = reader.u8();
		const auto phase = reader.u8(); const auto reserved = reader.u8(); const auto generation = reader.u32();
		const auto caps = reader.u64(); const auto coverage = reader.u64(); const auto derived = reader.u64(); const auto exact = reader.u64();
		put(fields,"authority_mode",ji(authority)); put(fields,"event_coverage_exact",js(exact));
		put(fields,"event_coverage_state_derived",js(derived)); put(fields,"negotiated_capabilities",js(caps));
		put(fields,"negotiated_capability_generation",ji(generation));
		if (presence & 1U) { const auto observed = reader.u64(); put(fields,"observed_player_entity_id",js(observed)); }
		put(fields,"presence",js(presence)); put(fields,"producer_id",js(producer)); put(fields,"producer_sample_time_us",js(sample));
		put(fields,"reserved",ji(reserved)); put(fields,"session_phase",ji(phase)); put(fields,"state_domain_coverage",js(coverage));
		put(fields,"visibility_mode",ji(visibility));
	} else if (type == 2U) {
		name="MISSION_STATE"; const auto presence=reader.u64(); const auto generation=reader.u32(); const auto phase=reader.u8();
		const auto paused=reader.u8(); const auto reserved=reader.u16(); const auto compression=reader.f32(); const auto sample=reader.u64();
		put(fields,"mission_generation",ji(generation)); put(fields,"paused",ji(paused)); put(fields,"phase",ji(phase));
		put(fields,"presence",js(presence)); put(fields,"producer_sample_time_us",js(sample)); put(fields,"reserved",ji(reserved));
		put(fields,"time_compression",jr(compression));
	} else if (type == 5U) {
		name="ENTITY_LIFECYCLE"; const auto presence=entity_prefix(); const auto object_type=reader.u8(); const auto phase=reader.u8(); const auto lifecycle=reader.u32();
		put(fields,"lifecycle_flags",ji(lifecycle)); put(fields,"lifecycle_phase",ji(phase)); put(fields,"object_type",ji(object_type));
		if (presence&1U) {
			put(fields,"signature",ji(reader.u32()));
		}
		if (presence&2U) {
			put(fields,"net_signature",ji(reader.u32()));
		}
		if (presence&4U) put(fields,"class_id",ji(reader.u32()));
	} else if (type == 6U) {
		name="SHIP_IDENTITY"; entity_prefix(); const auto class_id=reader.u32(); const auto internal=reader.utf8();
		const auto species=reader.u32(); const auto team=reader.u32(); const auto iff=reader.u32(); const auto role=reader.u16(); const auto radius=reader.f32();
		put(fields,"iff_id",ji(iff)); put(fields,"internal_name",json_string(internal.c_str())); put(fields,"radius",jr(radius));
		put(fields,"role_flags",ji(role)); put(fields,"ship_class_id",ji(class_id)); put(fields,"species_id",ji(species)); put(fields,"team_id",ji(team));
	} else if (type == 7U) {
		name="FLIGHT_STATE"; const auto presence=entity_prefix(); auto* position=vector_json(reader,3); auto* orientation=vector_json(reader,4);
		auto* velocity=vector_json(reader,3); auto* rotational=vector_json(reader,3); const auto radius=reader.f32(); const auto physics=reader.u32();
		put(fields,"orientation_local_to_world",orientation); put(fields,"physics_mode_flags",ji(physics)); put(fields,"position_world",position);
		put(fields,"radius",jr(radius)); put(fields,"rotational_velocity_local",rotational); put(fields,"velocity_world",velocity);
		if (presence&1U) put(fields,"desired_velocity_world",vector_json(reader,3));
	} else if (type == 8U) {
		name="CONTROL_STATE"; entity_prefix(); const auto pitch=reader.f32(); const auto heading=reader.f32();
		const auto bank=reader.f32(); const auto forward=reader.f32(); const auto sideways=reader.f32();
		const auto vertical=reader.f32(); const auto mode=reader.u8(); const auto control_flags=reader.u32();
		put(fields,"bank",jr(bank)); put(fields,"control_flags",ji(control_flags)); put(fields,"control_mode",ji(mode));
		put(fields,"forward",jr(forward)); put(fields,"heading",jr(heading)); put(fields,"pitch",jr(pitch));
		put(fields,"sideways",jr(sideways)); put(fields,"vertical",jr(vertical));
	} else if (type == 9U) {
		name="DAMAGE_STATE"; entity_prefix(); const auto hull=reader.f32(); const auto maximum=reader.f32(); const auto protection=reader.u16();
		put(fields,"dynamic_max_hull",jr(maximum)); put(fields,"hull_strength",jr(hull)); put(fields,"protection_flags",ji(protection));
	} else if (type == 10U) {
		name="SHIELD_STATE"; entity_prefix(); const auto has=reader.u8(); const auto count=reader.u16(); const auto reserved=reader.u16();
		put(fields,"has_shields",ji(has)); put(fields,"reserved",ji(reserved)); put(fields,"segment_count",ji(count));
		put(fields,"segment_current_hits",vector_json(reader,count)); put(fields,"segment_max_hits",vector_json(reader,count));
	} else if (type == 11U) {
		name="SUBSYSTEM_STATE"; const auto entity=reader.u64(); const auto subsystem=reader.u32(); const auto presence=reader.u64(); const auto sample=reader.u64();
		const auto index=reader.u16(); const auto subtype=reader.u8(); const auto current=reader.f32(); const auto maximum=reader.f32(); const auto subsystem_flags=reader.u32();
		put(fields,"canonical_index",ji(index)); put(fields,"current_hits",jr(current)); put(fields,"entity_id",js(entity)); put(fields,"max_hits",jr(maximum));
		put(fields,"presence",js(presence)); put(fields,"producer_sample_time_us",js(sample)); put(fields,"subsystem_flags",ji(subsystem_flags));
		put(fields,"subsystem_id",ji(subsystem)); put(fields,"type",ji(subtype));
	} else if (type == 12U) {
		name="ENERGY_STATE"; entity_prefix(); const auto mode=reader.u8(); const auto shields=reader.u8(); const auto weapons=reader.u8(); const auto engines=reader.u8(); const auto reserved=reader.u8();
		put(fields,"ets_engines_index",ji(engines)); put(fields,"ets_mode",ji(mode)); put(fields,"ets_shields_index",ji(shields));
		put(fields,"ets_weapons_index",ji(weapons)); put(fields,"reserved",ji(reserved));
	} else if (type == 13U) {
		name="PROPULSION_STATE"; entity_prefix(); const auto propulsion=reader.u16(); const auto reserved=reader.u16();
		put(fields,"propulsion_flags",ji(propulsion)); put(fields,"reserved",ji(reserved));
	} else if (type == 14U) {
		name="WEAPON_STATE"; entity_prefix(); const auto primary_count=reader.u16(); const auto secondary_count=reader.u16();
		const auto tertiary_count=reader.u16(); const auto reserved=reader.u16(); const auto current_primary=reader.u32();
		const auto current_secondary=reader.u32(); const auto current_tertiary=reader.u32(); const auto weapon_flags=reader.u32();
		const auto encoded_primary_count=reader.u16(); auto* primary_banks=json_array();
		for (std::uint16_t index=0;index<encoded_primary_count;++index) ADD_FAILURE() << "non-minimal primary bank";
		const auto encoded_secondary_count=reader.u16(); auto* secondary_banks=json_array();
		for (std::uint16_t index=0;index<encoded_secondary_count;++index) ADD_FAILURE() << "non-minimal secondary bank";
		put(fields,"current_primary_bank_id",ji(current_primary)); put(fields,"current_secondary_bank_id",ji(current_secondary));
		put(fields,"current_tertiary_bank_id",ji(current_tertiary)); put(fields,"primary_bank_count",ji(primary_count));
		put(fields,"primary_banks",primary_banks); put(fields,"reserved",ji(reserved));
		put(fields,"secondary_bank_count",ji(secondary_count)); put(fields,"secondary_banks",secondary_banks);
		put(fields,"tertiary_bank_count",ji(tertiary_count)); put(fields,"weapon_flags",ji(weapon_flags));
	} else if (type == 20U) {
		name="CARGO_SCAN_STATE"; entity_prefix(); const auto phase=reader.u8(); const auto disclosure=reader.u8();
		put(fields,"disclosure",ji(disclosure)); put(fields,"scan_phase",ji(phase));
	} else if (type == 21U) {
		name="DOCKING_STATE"; entity_prefix(); const auto phase=reader.u8(); const auto leader=reader.u64();
		const auto count=reader.u16(); auto* relations=json_array();
		for (std::uint16_t index=0;index<count;++index) ADD_FAILURE() << "non-minimal docking relation";
		put(fields,"group_leader_entity_id",js(leader)); put(fields,"phase",ji(phase)); put(fields,"relations",relations);
	} else if (type == 22U) {
		name="SUPPORT_STATE"; entity_prefix(); const auto phase=reader.u8(); const auto support_flags=reader.u8();
		const auto reserved=hex_string(reader.bytes(3));
		put(fields,"phase",ji(phase)); put(fields,"reserved",json_string(reserved.c_str()));
		put(fields,"support_flags",ji(support_flags));
	} else { ADD_FAILURE() << "unhandled FSTL 1.1 record type " << type; }
	EXPECT_EQ(0U, reader.remaining());
	auto* record=json_object(); put(record,"fields",fields); put(record,"kind",json_string("record")); put(record,"recordFlags",ji(flags));
	put(record,"recordLength",ji(length)); put(record,"recordName",json_string(name)); put(record,"recordType",ji(type));
	put(record,"recordVersion",ji(version)); put(record,"schema",json_string("FSTL-1.1")); return record;
}

json_t* canonical_v11_message(MessageType type, std::uint8_t flags, ByteView payload) {
	CanonicalReader reader(payload); auto* fields=json_object(); const char* name="";
	if (type == MessageType::Hello) {
		name="HELLO"; const auto nonce=reader.u64(); const auto t0=reader.u64(); const auto min_major=reader.u8(); const auto max_major=reader.u8();
		const auto min_minor=reader.u8(); const auto max_minor=reader.u8(); const auto visibility=reader.u8(); const auto reserved=hex_string(reader.bytes(3));
		const auto caps=reader.u64(); const auto heartbeat=reader.u16(); const auto ext_length=reader.u16(); const auto ext_count=reader.u16();
		put(fields,"advertised_capabilities",js(caps)); put(fields,"client_nonce",js(nonce)); put(fields,"client_send_t0_us",js(t0));
		put(fields,"extension_count",ji(ext_count)); put(fields,"extensions",json_array()); put(fields,"extensions_length",ji(ext_length));
		put(fields,"max_major",ji(max_major)); put(fields,"max_minor",ji(max_minor)); put(fields,"min_major",ji(min_major)); put(fields,"min_minor",ji(min_minor));
		put(fields,"requested_heartbeat_ms",ji(heartbeat)); put(fields,"requested_visibility_mode",ji(visibility)); put(fields,"reserved",json_string(reserved.c_str()));
	} else if (type == MessageType::Welcome) {
		name="WELCOME"; const auto nonce=reader.u64(); const auto t0=reader.u64(); const auto t1=reader.u64(); const auto t2=reader.u64();
		const auto status=reader.u8(); const auto major=reader.u8(); const auto minor=reader.u8(); const auto visibility=reader.u8();
		const auto producer_caps=reader.u64(); const auto active=reader.u64(); const auto heartbeat=reader.u16(); const auto timeout=reader.u16();
		const auto ext_length=reader.u16(); const auto ext_count=reader.u16(); const auto producer=reader.u64();
		put(fields,"active_capabilities",js(active)); put(fields,"client_nonce",js(nonce)); put(fields,"client_send_t0_us",js(t0));
		put(fields,"extension_count",ji(ext_count)); put(fields,"extensions",json_array()); put(fields,"extensions_length",ji(ext_length));
		put(fields,"heartbeat_interval_ms",ji(heartbeat)); put(fields,"producer_capabilities",js(producer_caps)); put(fields,"producer_id",js(producer));
		put(fields,"producer_receive_t1_us",js(t1)); put(fields,"producer_send_t2_us",js(t2)); put(fields,"reliable_reassembly_timeout_ms",ji(timeout));
		put(fields,"selected_major",ji(major)); put(fields,"selected_minor",ji(minor)); put(fields,"selected_visibility_mode",ji(visibility)); put(fields,"status",ji(status));
	} else if (type == MessageType::FullSnapshot) {
		name="FULL_SNAPSHOT"; const auto snapshot=reader.u32(); const auto part_index=reader.u16(); const auto part_count=reader.u16();
		const auto transaction_size=reader.u32(); const auto sha=hex_string(reader.bytes(32)); const auto sample=reader.u64();
		const auto manifest=reader.u32(); const auto snapshot_flags=reader.u16(); const auto count=reader.u16(); auto* records=json_array();
		for (std::uint16_t i=0;i<count;++i) EXPECT_EQ(0,json_array_append_new(records,canonical_v11_record(reader)));
		put(fields,"part_count",ji(part_count)); put(fields,"part_index",ji(part_index)); put(fields,"producer_sample_time_us",js(sample));
		put(fields,"record_count",ji(count)); put(fields,"records",records); put(fields,"required_manifest_id",ji(manifest));
		put(fields,"snapshot_flags",ji(snapshot_flags)); put(fields,"snapshot_id",ji(snapshot)); put(fields,"transaction_sha256",json_string(sha.c_str()));
		put(fields,"transaction_size",ji(transaction_size));
	} else if (type == MessageType::Delta) {
		name="DELTA"; const auto baseline=reader.u32(); const auto sequence=reader.u32(); const auto sample=reader.u64();
		const auto count=reader.u16(); const auto reserved=reader.u16(); auto* records=json_array();
		for (std::uint16_t i=0;i<count;++i) EXPECT_EQ(0,json_array_append_new(records,canonical_v11_record(reader)));
		put(fields,"baseline_snapshot_id",ji(baseline)); put(fields,"delta_sequence",ji(sequence)); put(fields,"producer_sample_time_us",js(sample));
		put(fields,"record_count",ji(count)); put(fields,"records",records); put(fields,"reserved",ji(reserved));
	} else { ADD_FAILURE() << "unhandled FSTL 1.1 message"; }
	EXPECT_EQ(0U,reader.remaining()); auto* message=json_object(); put(message,"fields",fields); put(message,"kind",json_string("message-payload"));
	put(message,"messageFlags",ji(flags)); put(message,"messageName",json_string(name)); put(message,"messageType",ji(static_cast<std::uint8_t>(type)));
	put(message,"schema",json_string("FSTL-1.1")); return message;
}

const char* stable_error_name(ValidationError error) {
	switch (error) {
	case ValidationError::DuplicateRecord: return "DuplicateRecord";
	case ValidationError::InvalidAbsence: return "InvalidAbsence";
	case ValidationError::InvalidStateTransition: return "InvalidStateTransition";
	case ValidationError::ReservedFlag: return "ReservedFlag";
	case ValidationError::MissingManifest: return "MissingManifest";
	case ValidationError::VisibilityViolation: return "VisibilityViolation";
	default: return "UNMAPPED";
	}
}

ValidationError validate_v11_snapshot_asset(const std::string& name, std::uint8_t minor = VersionMinorV1_1) {
	const auto input = read_binary(asset_root() / "vectors-v1.1" / name / (name + ".bin"));
	FullSnapshotPartPayload payload;
	if (const auto error = decode_full_snapshot_part_payload(byte_view(input), payload);
		error != ValidationError::None) return error;
	std::vector<std::uint8_t> reencoded(input.size());
	std::size_t written = 0U;
	if (const auto error = encode_full_snapshot_part_payload(
		payload, MutableByteView{reencoded.data(), reencoded.size()}, written);
		error != ValidationError::None || written != input.size() || reencoded != input) {
		return error != ValidationError::None ? error : ValidationError::InternalSerializationError;
	}
	BusinessStateValidationContext context;
	context.protocol_minor = minor;
	context.required_manifest_id = payload.required_manifest_id;
	context.class_manifest_installed = payload.required_manifest_id != 0U;
	context.weapon_manifest_installed = context.class_manifest_installed && name == "phase2-complete-ship";
	const std::uint32_t subsystem_id = 2U;
	const BusinessClassCatalogEntry class_catalog{1U, &subsystem_id, 1U};
	if (context.class_manifest_installed) {
		context.class_catalog = &class_catalog;
		context.class_catalog_count = 1U;
	}
	BusinessStateImageValidator validator(context);
	StateImage image;
	return decode_business_snapshot_region_validated(payload.records, payload.record_count, validator, image);
}

std::vector<std::uint8_t> expected_payload(std::size_t size) {
	std::vector<std::uint8_t> result(size);
	for (std::size_t index = 0; index < size; ++index) {
		result[index] = static_cast<std::uint8_t>(0x31U + index * 37U);
	}
	return result;
}

template <typename Payload, typename Decoder, typename Encoder>
ValidationError canonical_payload_roundtrip(const std::vector<std::uint8_t>& input,
	std::vector<std::uint8_t>& encoded,
	Decoder decoder,
	Encoder encoder) {
	Payload payload;
	if (const auto error = decoder(byte_view(input), payload); error != ValidationError::None) {
		return error;
	}
	encoded.assign(input.size(), 0xa5U);
	std::size_t written = 0;
	if (const auto error = encoder(payload, MutableByteView{encoded.data(), encoded.size()}, written);
		error != ValidationError::None) {
		return error;
	}
	return written == input.size() && encoded == input ? ValidationError::None
													 : ValidationError::InternalSerializationError;
}

ValidationError roundtrip_message_fixture(const std::string& name,
	const std::vector<std::uint8_t>& input,
	std::vector<std::uint8_t>& encoded) {
	if (name == "discovery") {
		return canonical_payload_roundtrip<DiscoveryPayload>(
			input, encoded, decode_discovery_payload, encode_discovery_payload);
	}
	if (name == "hello") {
		return canonical_payload_roundtrip<HelloPayload>(input, encoded, decode_hello_payload, encode_hello_payload);
	}
	if (name == "welcome") {
		return canonical_payload_roundtrip<WelcomePayload>(input, encoded, decode_welcome_payload, encode_welcome_payload);
	}
	if (name == "session_begin") {
		return canonical_payload_roundtrip<SessionBeginPayload>(
			input, encoded, decode_session_begin_payload, encode_session_begin_payload);
	}
	if (name == "manifest") {
		return canonical_payload_roundtrip<ManifestPartPayload>(
			input, encoded, decode_manifest_part_payload, encode_manifest_part_payload);
	}
	if (name == "full_snapshot") {
		return canonical_payload_roundtrip<FullSnapshotPartPayload>(
			input, encoded, decode_full_snapshot_part_payload, encode_full_snapshot_part_payload);
	}
	if (name == "delta") {
		return canonical_payload_roundtrip<DeltaPayload>(input, encoded, decode_delta_payload, encode_delta_payload);
	}
	if (name == "event_batch") {
		EventBatchPayload payload;
		if (const auto error = decode_event_batch_payload(byte_view(input), true, payload);
			error != ValidationError::None) {
			return error;
		}
		encoded.assign(input.size(), 0xa5U);
		std::size_t written = 0;
		if (const auto error = encode_event_batch_payload(
				payload, true, MutableByteView{encoded.data(), encoded.size()}, written);
			error != ValidationError::None) {
			return error;
		}
		return written == input.size() && encoded == input ? ValidationError::None
													 : ValidationError::InternalSerializationError;
	}
	if (name == "heartbeat") {
		return canonical_payload_roundtrip<HeartbeatPayload>(
			input, encoded, decode_heartbeat_payload, encode_heartbeat_payload);
	}
	if (name == "ack") {
		return canonical_payload_roundtrip<AckPayload>(input, encoded, decode_ack_payload, encode_ack_payload);
	}
	if (name == "nack") {
		return canonical_payload_roundtrip<NackPayload>(input, encoded, decode_nack_payload, encode_nack_payload);
	}
	if (name == "resync_request") {
		return canonical_payload_roundtrip<ResyncRequestPayload>(
			input, encoded, decode_resync_request_payload, encode_resync_request_payload);
	}
	if (name == "session_end") {
		return canonical_payload_roundtrip<SessionEndPayload>(
			input, encoded, decode_session_end_payload, encode_session_end_payload);
	}
	if (name == "target_video_subscribe") {
		return canonical_payload_roundtrip<TargetVideoSubscribePayload>(
			input, encoded, decode_target_video_subscribe_payload, encode_target_video_subscribe_payload);
	}
	if (name == "target_video_config") {
		return canonical_payload_roundtrip<TargetVideoConfigPayload>(
			input, encoded, decode_target_video_config_payload, encode_target_video_config_payload);
	}
	if (name == "target_video_frame") {
		return canonical_payload_roundtrip<TargetVideoFramePayload>(
			input, encoded, decode_target_video_frame_payload, encode_target_video_frame_payload);
	}
	if (name == "target_video_keyframe_request") {
		return canonical_payload_roundtrip<TargetVideoKeyframeRequestPayload>(input,
			encoded,
			decode_target_video_keyframe_request_payload,
			encode_target_video_keyframe_request_payload);
	}
	if (name == "target_video_stop") {
		return canonical_payload_roundtrip<TargetVideoStopPayload>(
			input, encoded, decode_target_video_stop_payload, encode_target_video_stop_payload);
	}
	if (name == "target_video_stats") {
		return canonical_payload_roundtrip<TargetVideoStatsPayload>(
			input, encoded, decode_target_video_stats_payload, encode_target_video_stats_payload);
	}
	if (name == "capability_update") {
		return canonical_payload_roundtrip<CapabilityUpdatePayload>(
			input, encoded, decode_capability_update_payload, encode_capability_update_payload);
	}
	return ValidationError::UnknownMessageType;
}

BusinessRecordContainer fixture_container(RecordType type) {
	if (type == RecordType::ClassManifest || type == RecordType::WeaponManifest ||
		type == RecordType::CommAssetManifest) {
		return BusinessRecordContainer::Manifest;
	}
	if (type == RecordType::CommViewEvent || type == RecordType::Events) {
		return BusinessRecordContainer::EventBatchReliable;
	}
	return BusinessRecordContainer::FullSnapshot;
}

ValidationError validate_invalid_record_fixture(const std::vector<std::uint8_t>& input) {
	RecordEnvelopeIterator iterator(byte_view(input), 1U, RecordFlagPolicy::AllowV1Mutations);
	RecordEnvelopeView record;
	bool has_value = false;
	if (const auto error = iterator.next(record, has_value); error != ValidationError::None) {
		return error;
	}
	if (!has_value) {
		return ValidationError::UnknownRequiredRecord;
	}
	BusinessRecordMetadata metadata;
	return validate_business_record(
		record, fixture_container(static_cast<RecordType>(record.raw_record_type)), metadata);
}

ValidationError validate_invalid_message_payload_fixture(const std::string& name,
	const std::vector<std::uint8_t>& input) {
	if (name == "fixed_payload_truncated" || name == "fixed_payload_trailing_byte") {
		HeartbeatPayload payload;
		return decode_heartbeat_payload(byte_view(input), payload);
	}
	if (name == "video_encoded_frame_size_mismatch") {
		TargetVideoFramePayload payload;
		return decode_target_video_frame_payload(byte_view(input), payload);
	}
	if (name == "nack_bitmap_tail_bits_set") {
		NackPayload payload;
		return decode_nack_payload(byte_view(input), payload);
	}
	if (name == "duplicate_singleton_record") {
		FullSnapshotPartPayload payload;
		if (const auto error = decode_full_snapshot_part_payload(byte_view(input), payload);
			error != ValidationError::None) {
			return error;
		}
		StateImage snapshot;
		return decode_business_snapshot_region(payload.records, payload.record_count, snapshot);
	}
	if (name == "event_batch_record_count_exceeds_region") {
		EventBatchPayload payload;
		return decode_event_batch_payload(byte_view(input), true, payload);
	}
	return ValidationError::UnknownMessageType;
}

EndpointKey client_endpoint(std::uint8_t last_octet, std::uint16_t port = 42042U) {
	return EndpointKey::from_ipv4({127U, 0U, 0U, last_octet}, port);
}

ValidationError configure_default_limiter(ProtocolRateLimiter& limiter) {
	return ProtocolRateLimiter::configure({}, 0U, limiter);
}

ValidationError configure_video_client(TargetVideoClientLifecycle& lifecycle,
	std::uint64_t target_entity_id,
	std::uint32_t config_generation = 1U) {
	const auto config_bytes = read_binary(
		asset_root() / "vectors" / "valid" / "messages" / "target_video_config" / "target_video_config.bin");
	if (config_bytes.empty()) {
		return ValidationError::InternalSerializationError;
	}
	TargetVideoConfigPayload config;
	if (const auto error = decode_target_video_config_payload(byte_view(config_bytes), config);
		error != ValidationError::None) {
		return error;
	}
	config.config_generation = config_generation;
	lifecycle.reset(true);
	if (lifecycle.begin_subscribe(config.request_id) != VideoClientLifecycleResult::Applied ||
		lifecycle.apply_config(config, target_entity_id) != VideoClientLifecycleResult::Applied ||
		lifecycle.acknowledge_config_applied(config.stream_id, config.config_generation) !=
			VideoClientLifecycleResult::Applied) {
		return ValidationError::InternalSerializationError;
	}
	return ValidationError::None;
}

ValidationError validate_invalid_contextual_message_fixture(const std::string& name,
	const std::vector<std::uint8_t>& input) {
	if (name == "ack_forged_target" || name == "ack_wrong_target_crc") {
		AckPayload ack;
		if (const auto error = decode_ack_payload(byte_view(input), ack); error != ValidationError::None) {
			return error;
		}
		ReliabilityTargetTuple target;
		target.message_id = 100U;
		target.message_type = MessageType::FullSnapshot;
		target.fragment_count = 1U;
		target.message_crc32 = 0x12345678U;
		return validate_ack_target(ack, target);
	}
	if (name == "ack_late_after_retention") {
		AckPayload ack;
		if (const auto error = decode_ack_payload(byte_view(input), ack); error != ValidationError::None) {
			return error;
		}
		ReliableSendWindow window;
		if (const auto error = ReliableSendWindow::configure({}, window); error != ValidationError::None) {
			return error;
		}
		return reliable_response_validation_error(window.acknowledge(42U, client_endpoint(1U), ack, 0U));
	}
	if (name == "ack_wrong_endpoint") {
		AckPayload ack;
		if (const auto error = decode_ack_payload(byte_view(input), ack); error != ValidationError::None) {
			return error;
		}
		TelemetryDatagramHeader header;
		header.message_type = MessageType::Ack;
		header.session_id = 42U;
		TelemetrySessionContext context{LocalEndpointRole::Producer, client_endpoint(1U), 42U};
		return validate_received_datagram_context(header, client_endpoint(2U), context);
	}
	if (name == "client_simulated_producer_command") {
		SessionBeginPayload payload;
		if (const auto error = decode_session_begin_payload(byte_view(input), payload);
			error != ValidationError::None) {
			return error;
		}
		TelemetryDatagramHeader header;
		header.message_type = MessageType::SessionBegin;
		header.session_id = 42U;
		TelemetrySessionContext context{LocalEndpointRole::Producer, client_endpoint(1U), 42U};
		return validate_received_datagram_context(header, client_endpoint(1U), context);
	}
	if (name == "unknown_message_type") {
		if (!input.empty()) {
			return ValidationError::TrailingBytes;
		}
		TelemetryDatagramHeader header;
		header.message_type = static_cast<MessageType>(255U);
		header.session_id = 42U;
		TelemetrySessionContext context{LocalEndpointRole::Producer, client_endpoint(1U), 42U};
		return validate_received_datagram_context(header, client_endpoint(1U), context);
	}
	if (name == "delta_unknown_baseline") {
		DeltaPayload payload;
		if (const auto error = decode_delta_payload(byte_view(input), payload); error != ValidationError::None) {
			return error;
		}
		CumulativeStateDelta delta;
		if (const auto error = decode_business_delta(payload, delta); error != ValidationError::None) {
			return error;
		}
		StateImage baseline;
		if (StateImage::create({}, baseline) != StateImageResult::Created) {
			return ValidationError::InternalSerializationError;
		}
		ProtocolRateLimiter limiter;
		if (const auto error = configure_default_limiter(limiter); error != ValidationError::None) {
			return error;
		}
		ClientReplicationModel client;
		if (client.note_snapshot_candidate(1U, 0U, 0U, 10'000'000U) != SnapshotCandidateResult::Known) {
			return ValidationError::InternalSerializationError;
		}
		ResyncRequestPayload request;
		ClientResyncChannel channel{42U, client_endpoint(1U), limiter, request};
		if (client.commit_snapshot(1U, 0U, baseline, 1U, channel) != SnapshotCommitResult::Committed ||
			client.note_snapshot_candidate(2U, 0U, 2U, 10'000'002U) != SnapshotCandidateResult::Known) {
			return ValidationError::InternalSerializationError;
		}
		return client_delta_validation_error(
			client.receive_delta(delta, 3U, 42U, client_endpoint(1U), limiter, request));
	}
	if (name == "snapshot_missing_manifest") {
		FullSnapshotPartPayload payload;
		if (const auto error = decode_full_snapshot_part_payload(byte_view(input), payload);
			error != ValidationError::None) {
			return error;
		}
		StateImage snapshot;
		if (const auto error = decode_business_snapshot_region(payload.records, payload.record_count, snapshot);
			error != ValidationError::None) {
			return error;
		}
		ProtocolRateLimiter limiter;
		if (const auto error = configure_default_limiter(limiter); error != ValidationError::None) {
			return error;
		}
		ClientReplicationModel client;
		if (client.install_manifest(1U) != ManifestInstallResult::Installed ||
			client.note_snapshot_candidate(payload.snapshot_id,
				payload.required_manifest_id,
				0U,
				10'000'000U) != SnapshotCandidateResult::Known) {
			return ValidationError::InternalSerializationError;
		}
		ResyncRequestPayload request;
		ClientResyncChannel channel{42U, client_endpoint(1U), limiter, request};
		return snapshot_commit_validation_error(client.commit_snapshot(payload.snapshot_id,
			payload.required_manifest_id,
			snapshot,
			1U,
			channel));
	}
	if (name == "video_stale_generation" || name == "video_unknown_stream" ||
		name == "video_unknown_target" || name == "video_old_target_after_change") {
		TargetVideoFramePayload frame;
		if (const auto error = decode_target_video_frame_payload(byte_view(input), frame);
			error != ValidationError::None) {
			return error;
		}
		if (name == "video_unknown_target") {
			return validate_target_video_frame_entity_context(frame, false);
		}
		TargetVideoClientLifecycle lifecycle;
		const auto old_target = name == "video_old_target_after_change";
		const auto target = old_target ? 3U : 2U;
		if (const auto error = configure_video_client(lifecycle, target, old_target ? 2U : 1U);
			error != ValidationError::None) {
			return error;
		}
		return video_lifecycle_validation_error(lifecycle.accept_frame(frame, target, 1U));
	}
	if (name == "video_codec_not_negotiated" || name == "video_profile_not_negotiated" ||
		name == "video_resolution_not_negotiated" || name == "video_bitrate_not_negotiated") {
		TargetVideoConfigPayload config;
		if (const auto error = decode_target_video_config_payload(byte_view(input), config);
			error != ValidationError::None) {
			return error;
		}
		const auto subscribe_bytes = read_binary(asset_root() / "vectors" / "valid" / "messages" /
			"target_video_subscribe" / "target_video_subscribe.bin");
		TargetVideoSubscribePayload subscribe;
		if (const auto error = decode_target_video_subscribe_payload(byte_view(subscribe_bytes), subscribe);
			error != ValidationError::None) {
			return error;
		}
		if (name == "video_bitrate_not_negotiated") {
			subscribe.max_bitrate_kbps = 4000U;
		} else if (name == "video_resolution_not_negotiated") {
			subscribe.max_width = 958U;
			subscribe.preferred_width = 958U;
		}
		return name == "video_codec_not_negotiated"
				   ? validate_target_video_config_for_subscribe(config, subscribe, false)
				   : validate_target_video_config_for_subscribe(config, subscribe);
	}
	return ValidationError::UnknownMessageType;
}

ValidationError validate_invalid_contextual_record_fixture(const std::string& name,
	const std::vector<std::uint8_t>& input) {
	RecordEnvelopeIterator iterator(byte_view(input), 1U, RecordFlagPolicy::AllowV1Mutations);
	RecordEnvelopeView record;
	bool has_value = false;
	if (const auto error = iterator.next(record, has_value); error != ValidationError::None) {
		return error;
	}
	if (!has_value) {
		return ValidationError::TruncatedPayload;
	}
	if (name == "comm_bundle_hash_mismatch") {
		CommAssetManifestPayloadView manifest;
		if (const auto error = decode_comm_asset_manifest_payload(record.payload, manifest);
			error != ValidationError::None) {
			return error;
		}
		CommViewClientLifecycle lifecycle;
		lifecycle.reset_session(true, true, CommNegotiationResult::Accepted);
		Sha256Digest required_hash{};
		required_hash.fill(1U);
		return comm_view_lifecycle_validation_error(
			lifecycle.install_bundle(1U, required_hash, manifest.bundle_hash));
	}
	if (name == "comm_duration_mismatch") {
		CommViewEventPayload event;
		if (const auto error = decode_comm_view_event_payload(record.payload, event);
			error != ValidationError::None) {
			return error;
		}
		CommViewClientLifecycle lifecycle;
		lifecycle.reset_session(true, true, CommNegotiationResult::Accepted);
		if (lifecycle.install_bundle(1U) != CommViewClientLifecycleResult::Applied) {
			return ValidationError::InternalSerializationError;
		}
		CommAssetRuntimeStatus asset;
		asset.asset_id = event.head_asset_id;
		asset.duration_us = 200'000U;
		asset.available = true;
		asset.content_valid = true;
		return comm_view_lifecycle_validation_error(lifecycle.apply_event(event, asset));
	}
	return ValidationError::UnknownRequiredRecord;
}

ReassembledMessage sentinel_message() {
	ReassembledMessage result;
	result.header.message_id = 0xfeedbeefU;
	result.message_class = MessageSizeClass::Video;
	result.payload = {0x5aU};
	return result;
}

void expect_sentinel(const ReassembledMessage& message) {
	EXPECT_EQ(0xfeedbeefU, message.header.message_id);
	EXPECT_EQ(MessageSizeClass::Video, message.message_class);
	EXPECT_EQ((std::vector<std::uint8_t>{0x5aU}), message.payload);
}

struct ValidVectorCase {
	std::size_t message_size;
	std::uint16_t fragment_count;
};

TEST(TelemetryProtocolVectors, EveryExternalMessagePayloadDecodesAndReencodesCanonically) {
	const auto root = asset_root() / "vectors" / "valid" / "messages";
	std::vector<std::filesystem::path> directories;
	for (const auto& entry : std::filesystem::directory_iterator(root)) {
		if (entry.is_directory()) {
			directories.push_back(entry.path());
		}
	}
	std::sort(directories.begin(), directories.end());
	ASSERT_EQ(20U, directories.size());

	for (const auto& directory : directories) {
		const auto name = directory.filename().string();
		SCOPED_TRACE(name);
		const auto input = read_binary(directory / (name + ".bin"));
		ASSERT_FALSE(input.empty());
		std::vector<std::uint8_t> encoded;
		EXPECT_EQ(ValidationError::None, roundtrip_message_fixture(name, input, encoded));
		EXPECT_EQ(input, encoded);
	}
}

TEST(TelemetryProtocolVectors, EveryExternalBusinessRecordDecodesAndReencodesCanonically) {
	const auto root = asset_root() / "vectors" / "valid" / "records";
	std::vector<std::filesystem::path> directories;
	for (const auto& entry : std::filesystem::directory_iterator(root)) {
		if (entry.is_directory()) {
			directories.push_back(entry.path());
		}
	}
	std::sort(directories.begin(), directories.end());
	ASSERT_EQ(28U, directories.size());

	for (const auto& directory : directories) {
		const auto name = directory.filename().string();
		SCOPED_TRACE(name);
		const auto input = read_binary(directory / (name + ".bin"));
		ASSERT_FALSE(input.empty());

		RecordEnvelopeIterator iterator(byte_view(input), 1U, RecordFlagPolicy::AllowV1Mutations);
		RecordEnvelopeView record;
		bool has_value = false;
		ASSERT_EQ(ValidationError::None, iterator.next(record, has_value));
		ASSERT_TRUE(has_value);
		bool trailing = true;
		RecordEnvelopeView ignored;
		ASSERT_EQ(ValidationError::None, iterator.next(ignored, trailing));
		ASSERT_FALSE(trailing);

		BusinessRecordMetadata metadata;
		ASSERT_EQ(ValidationError::None,
			validate_business_record(record,
				fixture_container(static_cast<RecordType>(record.raw_record_type)),
				metadata));
		ASSERT_EQ(record.raw_record_type, static_cast<std::uint16_t>(metadata.type));

		std::vector<std::uint8_t> encoded(input.size(), 0xa5U);
		std::size_t written = 0;
		ASSERT_EQ(ValidationError::None,
			encode_business_record(record,
				fixture_container(metadata.type),
				MutableByteView{encoded.data(), encoded.size()},
				written));
		EXPECT_EQ(input.size(), written);
		EXPECT_EQ(input, encoded);
	}
}

struct InvalidProtocolFixtureCase {
	const char* category;
	const char* name;
	ValidationError expected_error;
};

TEST(TelemetryProtocolVectors, ExternalInvalidPayloadOnlyMessagesMatchTheStableErrorTaxonomy) {
	const std::array<InvalidProtocolFixtureCase, 6> cases{{
		{"fixed_payload_truncated", "fixed_payload_truncated", ValidationError::TruncatedPayload},
		{"fixed_payload_trailing", "fixed_payload_trailing_byte", ValidationError::TrailingBytes},
		{"encoded_frame_size_mismatch", "video_encoded_frame_size_mismatch", ValidationError::TruncatedPayload},
		{"nack_bitmap_incoherent", "nack_bitmap_tail_bits_set", ValidationError::ReservedFlag},
		{"record_duplicate_singleton", "duplicate_singleton_record", ValidationError::DuplicateRecord},
		{"record_count_truncated_region",
			"event_batch_record_count_exceeds_region",
			ValidationError::TruncatedPayload},
	}};
	const auto root = asset_root() / "vectors" / "invalid" / "messages";
	for (const auto& test_case : cases) {
		SCOPED_TRACE(test_case.name);
		const auto input = read_binary(root / test_case.category / (std::string{test_case.name} + ".bin"));
		ASSERT_FALSE(input.empty());
		EXPECT_EQ(test_case.expected_error, validate_invalid_message_payload_fixture(test_case.name, input));
	}
}

TEST(TelemetryProtocolVectors, ExternalInvalidContextualMessagesMatchTheStableErrorTaxonomy) {
	const std::array<InvalidProtocolFixtureCase, 16> cases{{
		{"ack_forged", "ack_forged_target", ValidationError::InvalidStateTransition},
		{"ack_late", "ack_late_after_retention", ValidationError::InvalidStateTransition},
		{"ack_wrong_crc", "ack_wrong_target_crc", ValidationError::InvalidStateTransition},
		{"ack_wrong_endpoint", "ack_wrong_endpoint", ValidationError::EndpointMismatch},
		{"client_simulated_command", "client_simulated_producer_command", ValidationError::WrongDirection},
		{"client_undefined_command", "unknown_message_type", ValidationError::UnknownMessageType},
		{"stale_video_target", "video_old_target_after_change", ValidationError::StaleGeneration},
		{"unknown_baseline", "delta_unknown_baseline", ValidationError::StaleBaseline},
		{"unknown_generation", "video_stale_generation", ValidationError::StaleGeneration},
		{"unknown_manifest", "snapshot_missing_manifest", ValidationError::MissingManifest},
		{"unknown_stream", "video_unknown_stream", ValidationError::InvalidStateTransition},
		{"unknown_target", "video_unknown_target", ValidationError::UnknownEntity},
		{"video_bitrate_not_negotiated", "video_bitrate_not_negotiated", ValidationError::CapabilityNotNegotiated},
		{"video_codec_not_negotiated", "video_codec_not_negotiated", ValidationError::CapabilityNotNegotiated},
		{"video_profile_not_negotiated", "video_profile_not_negotiated", ValidationError::CapabilityNotNegotiated},
		{"video_resolution_not_negotiated",
			"video_resolution_not_negotiated",
			ValidationError::CapabilityNotNegotiated},
	}};
	const auto root = asset_root() / "vectors" / "invalid" / "messages";
	for (const auto& test_case : cases) {
		SCOPED_TRACE(test_case.name);
		const auto input = read_binary(root / test_case.category / (std::string{test_case.name} + ".bin"));
		if (std::string{test_case.name} != "unknown_message_type") {
			ASSERT_FALSE(input.empty());
		}
		EXPECT_EQ(test_case.expected_error, validate_invalid_contextual_message_fixture(test_case.name, input));
	}
}

TEST(TelemetryProtocolVectors, ExternalInvalidRecordFixturesMatchTheStableErrorTaxonomy) {
	const std::array<InvalidProtocolFixtureCase, 18> cases{{
		{"bool_outside_0_1", "bool_outside_closed_domain", ValidationError::OutOfRange},
		{"closed_enum_unknown", "closed_enum_unknown", ValidationError::UnknownEnum},
		{"comm_bundle_inconsistent", "comm_bundle_version_inconsistent", ValidationError::OutOfRange},
		{"invalid_utf8", "invalid_utf8_string", ValidationError::InvalidUtf8},
		{"non_finite_float", "nan_float", ValidationError::NonFiniteFloat},
		{"non_finite_float", "positive_infinity_float", ValidationError::NonFiniteFloat},
		{"non_finite_float", "negative_infinity_float", ValidationError::NonFiniteFloat},
		{"quaternion_non_canonical", "quaternion_non_canonical_sign", ValidationError::OutOfRange},
		{"quaternion_non_finite", "quaternion_non_finite", ValidationError::NonFiniteFloat},
		{"quaternion_non_normalizable", "quaternion_zero_non_normalizable", ValidationError::OutOfRange},
		{"quota_before_allocation", "comm_asset_count_over_quota", ValidationError::ResourceLimit},
		{"record_duplicate_item_key", "duplicate_comm_asset_id", ValidationError::DuplicateItemKey},
		{"record_length_excessive", "record_declared_length_exceeds_input", ValidationError::BadRecordLength},
		{"record_reserved_flag", "reserved_record_flag", ValidationError::ReservedFlag},
		{"record_truncated", "record_truncated_header", ValidationError::TruncatedPayload},
		{"record_invalid_type", "invalid_record_type_zero", ValidationError::OutOfRange},
		{"record_unsupported_version", "unsupported_record_version", ValidationError::UnsupportedRecordVersion},
		{"string_too_long", "string_over_field_limit", ValidationError::StringTooLong},
	}};
	const auto root = asset_root() / "vectors" / "invalid" / "records";
	for (const auto& test_case : cases) {
		SCOPED_TRACE(test_case.name);
		const auto input = read_binary(root / test_case.category / (std::string{test_case.name} + ".bin"));
		ASSERT_FALSE(input.empty());
		EXPECT_EQ(test_case.expected_error, validate_invalid_record_fixture(input));
	}
}

TEST(TelemetryProtocolVectors, ExternalInvalidContextualRecordsMatchTheStableErrorTaxonomy) {
	const std::array<InvalidProtocolFixtureCase, 2> cases{{
		{"comm_bundle_hash_inconsistent", "comm_bundle_hash_mismatch", ValidationError::InvalidStateTransition},
		{"comm_duration_inconsistent", "comm_duration_mismatch", ValidationError::InvalidStateTransition},
	}};
	const auto root = asset_root() / "vectors" / "invalid" / "records";
	for (const auto& test_case : cases) {
		SCOPED_TRACE(test_case.name);
		const auto input = read_binary(root / test_case.category / (std::string{test_case.name} + ".bin"));
		ASSERT_FALSE(input.empty());
		EXPECT_EQ(test_case.expected_error, validate_invalid_contextual_record_fixture(test_case.name, input));
	}
}

TEST(TelemetryProtocolVectors, ExternalCanonicalBoundaryFixturesDecodeAndReassembleExactly) {
	const std::array<ValidVectorCase, 6> cases{{
	    {0U, 1U}, {1U, 1U}, {1132U, 1U}, {1133U, 2U}, {2264U, 2U}, {2265U, 3U},
	}};

	for (const auto& test_case : cases) {
		SCOPED_TRACE(testing::Message() << "external vector size " << test_case.message_size);
		const auto directory = asset_root() / "vectors" / "valid" / "datagrams" / "transport" /
		                       ("transport_" + std::to_string(test_case.message_size) + "_bytes");
		const auto logical = expected_payload(test_case.message_size);
		const auto expected_crc = crc32_iso_hdlc(byte_view(logical));
		TelemetryReassembler reassembler;
		auto completed = sentinel_message();

		for (std::uint16_t index = 0; index < test_case.fragment_count; ++index) {
			const auto filename = (index < 10U ? "00" : index < 100U ? "0" : "") + std::to_string(index) + ".bin";
			const auto encoded = read_binary(directory / filename);
			const auto expected_slice = test_case.message_size == 0U
			                                ? 0U
			                                : std::min<std::size_t>(MaxFragmentPayload,
			                                                        test_case.message_size -
			                                                            static_cast<std::size_t>(index) * MaxFragmentPayload);
			ASSERT_EQ(HeaderSizeV1 + expected_slice, encoded.size());

			DatagramView decoded;
			ASSERT_EQ(ValidationError::None, decode_and_validate_datagram(byte_view(encoded), decoded));
			EXPECT_EQ(test_case.message_size == 0U ? MessageType::Heartbeat : MessageType::Delta,
			          decoded.header.message_type);
			EXPECT_EQ(test_case.fragment_count > 1U ? MessageFlagFragmented : MessageFlagNone,
			          decoded.header.flags);
			EXPECT_EQ(test_case.message_size, decoded.header.message_size);
			EXPECT_EQ(test_case.fragment_count, decoded.header.fragment_count);
			EXPECT_EQ(index, decoded.header.fragment_index);
			EXPECT_EQ(static_cast<std::size_t>(index) * MaxFragmentPayload, decoded.header.fragment_offset);
			EXPECT_EQ(expected_slice, decoded.header.payload_size);
			EXPECT_EQ(expected_slice, decoded.payload.size);
			EXPECT_EQ(expected_crc, decoded.header.message_crc32);
			if (expected_slice != 0U) {
				EXPECT_TRUE(std::equal(decoded.payload.begin(),
				                       decoded.payload.end(),
				                       logical.begin() + static_cast<std::ptrdiff_t>(decoded.header.fragment_offset)));
			}

			const auto expected_result = index + 1U == test_case.fragment_count ? ReassemblyResult::Completed
			                                                                    : ReassemblyResult::Accepted;
			ASSERT_EQ(expected_result, reassembler.ingest(decoded, completed));
			if (expected_result != ReassemblyResult::Completed) {
				expect_sentinel(completed);
			}
		}
		EXPECT_EQ(logical, completed.payload);
		EXPECT_EQ(ValidationError::None, validate_message_crc(completed.header, completed.payload_view()));
		EXPECT_EQ(0U, reassembler.active_reassemblies(MessageSizeClass::State));
		EXPECT_EQ(0U, reassembler.reserved_bytes(MessageSizeClass::State));
	}
}

TEST(TelemetryProtocolVectors, EveryExternalTruncatedHeaderFixtureReturnsDatagramTooShort) {
	const auto root = asset_root() / "vectors" / "invalid" / "datagrams" / "transport";
	for (std::size_t size = 0; size < HeaderSizeV1; ++size) {
		SCOPED_TRACE(testing::Message() << "external truncated header size " << size);
		const auto suffix = size < 10U ? "0" + std::to_string(size) : std::to_string(size);
		const auto encoded = read_binary(root / ("truncated_header_" + suffix) / "000.bin");
		ASSERT_EQ(size, encoded.size());
		DatagramView decoded;
		decoded.header.message_id = 0xfeedbeefU;
		EXPECT_EQ(ValidationError::DatagramTooShort, decode_and_validate_datagram(byte_view(encoded), decoded));
		EXPECT_EQ(0U, decoded.header.message_id);
		EXPECT_EQ(nullptr, decoded.payload.data);
		EXPECT_EQ(0U, decoded.payload.size);
	}
}

struct InvalidDatagramCase {
	const char* name;
	ValidationError expected_error;
};

TEST(TelemetryProtocolVectors, ExternalSingleDatagramMutationsReturnTheirDeclaredErrors) {
	const std::array<InvalidDatagramCase, 8> cases{{
	    {"bad_datagram_crc", ValidationError::BadDatagramCrc},
	    {"reserved_header_flag", ValidationError::ReservedHeaderFlag},
	    {"zero_fragment_count", ValidationError::BadFragmentCount},
	    {"fragment_index_out_of_range", ValidationError::BadFragmentIndex},
	    {"fragment_offset_noncanonical", ValidationError::BadFragmentOffset},
	    {"fragment_slice_noncanonical", ValidationError::BadFragmentSlice},
	    {"trailing_datagram_byte", ValidationError::BadDatagramLength},
	    // The datagram itself is valid; this one is checked after logical reassembly below.
	    {"bad_message_crc", ValidationError::None},
	}};
	const auto root = asset_root() / "vectors" / "invalid" / "datagrams" / "transport";
	for (const auto& test_case : cases) {
		SCOPED_TRACE(test_case.name);
		const auto encoded = read_binary(root / test_case.name / "000.bin");
		DatagramView decoded;
		const auto actual = decode_and_validate_datagram(byte_view(encoded), decoded);
		EXPECT_EQ(test_case.expected_error, actual);
		if (actual != ValidationError::None) {
			EXPECT_EQ(nullptr, decoded.payload.data);
			EXPECT_EQ(0U, decoded.payload.size);
		}

		if (std::string{test_case.name} == "bad_message_crc") {
			TelemetryReassembler reassembler;
			auto completed = sentinel_message();
			ASSERT_EQ(ValidationError::None, actual);
			EXPECT_EQ(ReassemblyResult::MessageCrcMismatch, reassembler.ingest(decoded, completed));
			expect_sentinel(completed);
			EXPECT_EQ(0U, reassembler.active_reassemblies(MessageSizeClass::State));
		}
	}
}

TEST(TelemetryProtocolVectors, ExternalMetadataAndDuplicateContradictionsPurgeReassembly) {
	const std::array<const char*, 2> names{{"inconsistent_fragment_metadata", "contradictory_duplicate_fragment"}};
	const auto root = asset_root() / "vectors" / "invalid" / "datagrams" / "transport";
	for (const auto* name : names) {
		SCOPED_TRACE(name);
		TelemetryReassembler reassembler;
		auto completed = sentinel_message();
		for (std::size_t index = 0; index < 2U; ++index) {
			const auto filename = index == 0U ? "000.bin" : "001.bin";
			const auto encoded = read_binary(root / name / filename);
			DatagramView decoded;
			ASSERT_EQ(ValidationError::None, decode_and_validate_datagram(byte_view(encoded), decoded));
			const auto expected = index == 0U ? ReassemblyResult::Accepted : ReassemblyResult::InconsistentFragment;
			EXPECT_EQ(expected, reassembler.ingest(decoded, completed));
			expect_sentinel(completed);
		}
		EXPECT_EQ(0U, reassembler.active_reassemblies(MessageSizeClass::State));
		EXPECT_EQ(0U, reassembler.reserved_bytes(MessageSizeClass::State));
	}
}

TEST(TelemetryProtocolVectors, Fstl11SnapshotsCrossTheProductionDecoder) {
	EXPECT_EQ(ValidationError::None, validate_v11_snapshot_asset("minimal-no-player"));
	EXPECT_EQ(ValidationError::None, validate_v11_snapshot_asset("minimal-with-player"));
	EXPECT_EQ(ValidationError::InvalidAbsence, validate_v11_snapshot_asset("missing-mission"));
	EXPECT_EQ(ValidationError::MissingManifest, validate_v11_snapshot_asset("phase2-promotion-incomplete"));
	EXPECT_EQ(ValidationError::None, validate_v11_snapshot_asset("phase2-promotion"));
	EXPECT_EQ(ValidationError::None, validate_v11_snapshot_asset("phase2-complete-ship"));
	const std::array<std::pair<const char*, ValidationError>, 12> invalid{{
		{"missing-mission", ValidationError::InvalidAbsence},
		{"missing-lifecycle", ValidationError::InvalidAbsence},
		{"missing-flight", ValidationError::InvalidAbsence},
		{"minor-zero-reserved-bit", ValidationError::ReservedFlag},
		{"phase2-promotion-incomplete", ValidationError::MissingManifest},
		{"duplicate-flight-record", ValidationError::DuplicateRecord},
		{"observed-player-id-mismatch", ValidationError::InvalidAbsence},
		{"player-lifecycle-non-ship", ValidationError::InvalidStateTransition},
		{"unexpected-ship-identity", ValidationError::InvalidAbsence},
		{"flight-presence-not-covered", ValidationError::InvalidAbsence},
		{"non-solo-authority", ValidationError::InvalidStateTransition},
		{"non-cockpit-visibility", ValidationError::VisibilityViolation},
	}};
	for (const auto& test_case : invalid) {
		SCOPED_TRACE(test_case.first);
		const auto actual = validate_v11_snapshot_asset(test_case.first,
			std::string{test_case.first} == "minor-zero-reserved-bit" ? VersionMinorV1_0 : VersionMinorV1_1);
		EXPECT_EQ(test_case.second, actual);
		const auto metadata = read_text(asset_root() / "vectors-v1.1" / test_case.first /
			(std::string{test_case.first} + ".json"));
		const auto expected_name = std::string{"\""} + stable_error_name(actual) + "\"";
		EXPECT_NE(std::string::npos, metadata.find("\"expectedValidationErrorName\": " + expected_name));
		EXPECT_NE(std::string::npos, metadata.find("\"expectedValidationError\": " +
			std::to_string(static_cast<unsigned>(actual))));
		EXPECT_NE(std::string::npos, metadata.find("\"valid\": false"));
	}
}

TEST(TelemetryProtocolVectors, MissingCascadeOwnerKeepsBadRecordLengthInFrozenFstl10) {
	auto input = read_binary(asset_root() / "vectors-v1.1" / "missing-lifecycle" / "missing-lifecycle.bin");
	ASSERT_GT(input.size(), 113U);
	// SESSION_STATE is the first envelope: record region 60 + envelope 6 + coverage offset 40.
	std::fill(input.begin() + 106, input.begin() + 114, 0U);
	input[106] = static_cast<std::uint8_t>(StateDomainCoverageBitCoreShip);
	Sha256Digest digest{};
	ASSERT_TRUE(sha256(ByteView{input.data() + FullSnapshotPartPayloadPrefixSize,
		input.size() - FullSnapshotPartPayloadPrefixSize}, digest));
	std::copy(digest.begin(), digest.end(), input.begin() + 12);
	FullSnapshotPartPayload payload;
	ASSERT_EQ(ValidationError::None, decode_full_snapshot_part_payload(byte_view(input), payload));
	BusinessStateValidationContext context;
	context.protocol_minor = VersionMinorV1_0;
	context.class_manifest_installed = true;
	BusinessStateImageValidator validator(context);
	StateImage image;
	EXPECT_EQ(ValidationError::BadRecordLength,
		decode_business_snapshot_region_validated(payload.records, payload.record_count, validator, image));
}

TEST(TelemetryProtocolVectors, Fstl11NegotiationAndDeltaCorpusCrossesTheProductionDecoder) {
	const auto root = asset_root() / "vectors-v1.1";
	std::vector<std::vector<std::uint8_t>> decoded_storage;
	auto decode = [&](const char* name, ProtocolMinorRange range, MessageType type) {
		decoded_storage.emplace_back(read_binary(root / name / (std::string{name} + ".bin")));
		const auto& encoded = decoded_storage.back();
		DatagramView view;
		EXPECT_EQ(ValidationError::None, decode_and_validate_datagram(byte_view(encoded), range, view));
		EXPECT_EQ(type, view.header.message_type);
		return view;
	};
	const auto peer = EndpointKey::from_ipv4({{127U, 0U, 0U, 1U}}, DefaultTelemetryPort);
	TelemetryOperationalConfig config;
	config.enabled = true;
	auto ingress = [&](const char* name, LocalEndpointRole role, ProtocolMinorRange range) {
		const auto encoded = read_binary(root / name / (std::string{name} + ".bin"));
		TelemetrySessionContext context;
		context.local_role = role;
		context.peer_endpoint = peer;
		context.accepted_minors = range;
		TelemetryIngressCounters counters;
		DatagramView view;
		EXPECT_EQ(ValidationError::None, decode_and_validate_ingress_datagram(
			config, peer, byte_view(encoded), context, view, counters));
		EXPECT_EQ(0U, view.header.frame_id);
		EXPECT_EQ(0, view.header.mission_time_us);
		auto invalid = view.header;
		invalid.frame_id = 1U;
		EXPECT_EQ(ValidationError::OutOfRange, validate_received_datagram_context(invalid, peer, context));
		invalid = view.header;
		invalid.mission_time_us = 1;
		EXPECT_EQ(ValidationError::OutOfRange, validate_received_datagram_context(invalid, peer, context));
		return std::make_pair(view.header, context);
	};

	const auto hello11 = decode("hello-minor-one-only", FrozenV1_0MinorRange, MessageType::Hello);
	HelloPayload hello;
	ASSERT_EQ(ValidationError::None, decode_hello_payload(hello11.payload, hello));
	EXPECT_EQ(VersionMinorV1_1, hello.min_minor);
	EXPECT_EQ(VersionMinorV1_1, hello.max_minor);
	EXPECT_EQ(VersionMinorV1_0, hello11.header.version_minor);
	ingress("hello-minor-one-only", LocalEndpointRole::Producer, FrozenV1_0MinorRange);

	const auto welcome11 = decode("welcome-accepted-minor-one", Phase1ProducerMinorRange, MessageType::Welcome);
	WelcomePayload accepted;
	ASSERT_EQ(ValidationError::None, decode_welcome_payload(welcome11.payload, accepted));
	EXPECT_EQ(WelcomeStatus::Accepted, accepted.status);
	EXPECT_EQ(VersionMinorV1_1, accepted.selected_minor);
	EXPECT_NE(0U, welcome11.header.session_id);
	const auto accepted_ingress = ingress("welcome-accepted-minor-one", LocalEndpointRole::Client,
		ProtocolMinorRange{VersionMinorV1_0, VersionMinorV1_1});
	EXPECT_EQ(ValidationError::None,
		validate_welcome_logical_context(accepted_ingress.first, accepted, accepted_ingress.second));

	const auto hello10 = decode("hello-minor-zero-only", FrozenV1_0MinorRange, MessageType::Hello);
	ASSERT_EQ(ValidationError::None, decode_hello_payload(hello10.payload, hello));
	EXPECT_EQ(VersionMinorV1_0, hello.min_minor);
	EXPECT_EQ(VersionMinorV1_0, hello.max_minor);
	ingress("hello-minor-zero-only", LocalEndpointRole::Producer, FrozenV1_0MinorRange);

	const auto rejected = decode("welcome-unsupported-version", FrozenV1_0MinorRange, MessageType::Welcome);
	WelcomePayload unsupported;
	ASSERT_EQ(ValidationError::None, decode_welcome_payload(rejected.payload, unsupported));
	EXPECT_EQ(WelcomeStatus::UnsupportedVersion, unsupported.status);
	EXPECT_EQ(0U, rejected.header.session_id);
	EXPECT_EQ(VersionMinorV1_0, rejected.header.version_minor);
	const auto rejected_ingress = ingress(
		"welcome-unsupported-version", LocalEndpointRole::Client, FrozenV1_0MinorRange);
	EXPECT_EQ(ValidationError::None,
		validate_welcome_logical_context(rejected_ingress.first, unsupported, rejected_ingress.second));

	auto verify_delta = [&](const char* name, std::uint32_t sequence) {
		const auto delta_view = decode(name, Phase1ProducerMinorRange, MessageType::Delta);
		DeltaPayload delta;
		ASSERT_EQ(ValidationError::None, decode_delta_payload(delta_view.payload, delta));
		EXPECT_EQ(1U, delta.baseline_snapshot_id);
		EXPECT_EQ(sequence, delta.delta_sequence);
		EXPECT_EQ(4U, delta.record_count);
		CumulativeStateDelta business_delta;
		EXPECT_EQ(ValidationError::None, decode_business_delta(delta, VersionMinorV1_1, business_delta));
		std::vector<std::uint8_t> reencoded(delta_view.payload.size);
		std::size_t written = 0U;
		ASSERT_EQ(ValidationError::None,
			encode_delta_payload(delta, MutableByteView{reencoded.data(), reencoded.size()}, written));
		EXPECT_EQ(delta_view.payload.size, written);
		EXPECT_TRUE(std::equal(reencoded.begin(), reencoded.end(), delta_view.payload.begin()));
	};
	verify_delta("delta-player-kinematics-cumulative", 2U);
	verify_delta("delta-player-return-baseline", 3U);
}

TEST(TelemetryProtocolVectors, EveryValidFstl11VectorMatchesTheFixedCanonicalJsonInCpp) {
	struct Case { const char* name; MessageType type; bool datagram; };
	const std::array<Case, 10> cases{{
		{"minimal-no-player", MessageType::FullSnapshot, false},
		{"minimal-with-player", MessageType::FullSnapshot, false},
		{"phase2-promotion", MessageType::FullSnapshot, false},
		{"phase2-complete-ship", MessageType::FullSnapshot, false},
		{"hello-minor-one-only", MessageType::Hello, true},
		{"hello-minor-zero-only", MessageType::Hello, true},
		{"welcome-accepted-minor-one", MessageType::Welcome, true},
		{"welcome-unsupported-version", MessageType::Welcome, true},
		{"delta-player-kinematics-cumulative", MessageType::Delta, true},
		{"delta-player-return-baseline", MessageType::Delta, true},
	}};
	for (const auto& test_case : cases) {
		SCOPED_TRACE(test_case.name);
		const auto encoded = read_binary(asset_root() / "vectors-v1.1" / test_case.name /
			(std::string{test_case.name} + ".bin"));
		ASSERT_FALSE(encoded.empty());
		ByteView payload = byte_view(encoded); std::uint8_t flags = 0U;
		DatagramView datagram;
		if (test_case.datagram) {
			const auto range = std::string{test_case.name} == "welcome-accepted-minor-one" ||
				(std::string{test_case.name} == "delta-player-kinematics-cumulative" ||
				 std::string{test_case.name} == "delta-player-return-baseline")
				? Phase1ProducerMinorRange : FrozenV1_0MinorRange;
			ASSERT_EQ(ValidationError::None, decode_and_validate_datagram(byte_view(encoded), range, datagram));
			ASSERT_EQ(test_case.type, datagram.header.message_type);
			payload = datagram.payload; flags = datagram.header.flags;
		}
		json_t* actual = canonical_v11_message(test_case.type, flags, payload);
		json_error_t json_error{};
		json_t* expected = json_load_file((asset_root() / "expected-v1.1" /
			(std::string{test_case.name} + ".json")).string().c_str(), JSON_REJECT_DUPLICATES, &json_error);
		ASSERT_NE(nullptr, expected) << json_error.text;
		if (!json_equal(actual, expected)) {
			char* actual_text = json_dumps(actual, JSON_INDENT(2) | JSON_SORT_KEYS);
			char* expected_text = json_dumps(expected, JSON_INDENT(2) | JSON_SORT_KEYS);
			ADD_FAILURE() << "canonical JSON mismatch\nactual:\n" << actual_text << "\nexpected:\n" << expected_text;
			free(actual_text); free(expected_text);
		}
		json_decref(actual); json_decref(expected);
	}
}

} // namespace
