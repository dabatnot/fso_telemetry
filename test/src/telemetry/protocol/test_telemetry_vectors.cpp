#include "telemetry/protocol/telemetry_crc32.h"
#include "telemetry/protocol/telemetry_datagram.h"
#include "telemetry/protocol/telemetry_reassembler.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
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

std::vector<std::uint8_t> expected_payload(std::size_t size) {
	std::vector<std::uint8_t> result(size);
	for (std::size_t index = 0; index < size; ++index) {
		result[index] = static_cast<std::uint8_t>(0x31U + index * 37U);
	}
	return result;
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

} // namespace
