#include "telemetry/protocol/packet_reader.h"
#include "telemetry/protocol/packet_writer.h"
#include "telemetry/protocol/telemetry_crc32.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace {

using namespace telemetry::protocol;

template <typename Container>
ByteView byte_view(const Container& value) {
	return ByteView{static_cast<const std::uint8_t*>(static_cast<const void*>(value.data())), value.size()};
}

template <typename Container>
MutableByteView writable_view(Container& value) {
	return MutableByteView{static_cast<std::uint8_t*>(static_cast<void*>(value.data())), value.size()};
}

template <std::size_t Size>
void expect_bytes(ByteView actual, const std::array<std::uint8_t, Size>& expected) {
	ASSERT_EQ(expected.size(), actual.size);
	for (std::size_t i = 0; i < expected.size(); ++i) {
		EXPECT_EQ(expected[i], actual.data[i]) << "byte offset " << i;
	}
}

template <typename Value, std::size_t Count, typename Write, typename Read>
void expect_fixed_width_contract(const char* name,
                                 const std::array<Value, Count>& values,
                                 Write write,
                                 Read read) {
	static_assert(Count > 0, "at least one value is required");
	constexpr auto Width = sizeof(Value);
	SCOPED_TRACE(name);

	for (const auto value : values) {
		std::array<std::uint8_t, Width> storage{};
		PacketWriter writer(writable_view(storage));
		ASSERT_TRUE((writer.*write)(value));
		EXPECT_EQ(Width, writer.size());
		EXPECT_EQ(0U, writer.remaining());

		PacketReader reader(writer.written());
		Value decoded = values.front();
		ASSERT_TRUE((reader.*read)(decoded));
		EXPECT_EQ(value, decoded);
		EXPECT_TRUE(reader.at_end());
	}

	std::array<std::uint8_t, Width> short_storage{};
	short_storage.fill(0xa5U);
	const auto canary = short_storage;
	PacketWriter short_writer(MutableByteView{short_storage.data(), Width - 1U});
	EXPECT_FALSE((short_writer.*write)(values.back()));
	EXPECT_FALSE(short_writer.ok());
	EXPECT_EQ(0U, short_writer.size());
	EXPECT_EQ(canary, short_storage);

	std::array<std::uint8_t, Width> encoded{};
	PacketWriter complete_writer(writable_view(encoded));
	ASSERT_TRUE((complete_writer.*write)(values.back()));
	PacketReader short_reader(ByteView{encoded.data(), Width - 1U});
	Value unchanged = values.front();
	EXPECT_FALSE((short_reader.*read)(unchanged));
	EXPECT_FALSE(short_reader.ok());
	EXPECT_EQ(values.front(), unchanged);
}

TEST(TelemetryProtocolPacketIo, IntegerExtremaUseExactWidthsAndRejectShortBuffers) {
	expect_fixed_width_contract("u8",
	                            std::array<std::uint8_t, 2>{std::numeric_limits<std::uint8_t>::min(),
	                                                        std::numeric_limits<std::uint8_t>::max()},
	                            &PacketWriter::write_u8,
	                            &PacketReader::read_u8);
	expect_fixed_width_contract("i8",
	                            std::array<std::int8_t, 2>{std::numeric_limits<std::int8_t>::min(),
	                                                       std::numeric_limits<std::int8_t>::max()},
	                            &PacketWriter::write_i8,
	                            &PacketReader::read_i8);
	expect_fixed_width_contract("u16",
	                            std::array<std::uint16_t, 2>{std::numeric_limits<std::uint16_t>::min(),
	                                                         std::numeric_limits<std::uint16_t>::max()},
	                            &PacketWriter::write_u16,
	                            &PacketReader::read_u16);
	expect_fixed_width_contract("i16",
	                            std::array<std::int16_t, 2>{std::numeric_limits<std::int16_t>::min(),
	                                                        std::numeric_limits<std::int16_t>::max()},
	                            &PacketWriter::write_i16,
	                            &PacketReader::read_i16);
	expect_fixed_width_contract("u32",
	                            std::array<std::uint32_t, 2>{std::numeric_limits<std::uint32_t>::min(),
	                                                         std::numeric_limits<std::uint32_t>::max()},
	                            &PacketWriter::write_u32,
	                            &PacketReader::read_u32);
	expect_fixed_width_contract("i32",
	                            std::array<std::int32_t, 2>{std::numeric_limits<std::int32_t>::min(),
	                                                        std::numeric_limits<std::int32_t>::max()},
	                            &PacketWriter::write_i32,
	                            &PacketReader::read_i32);
	expect_fixed_width_contract("u64",
	                            std::array<std::uint64_t, 2>{std::numeric_limits<std::uint64_t>::min(),
	                                                         std::numeric_limits<std::uint64_t>::max()},
	                            &PacketWriter::write_u64,
	                            &PacketReader::read_u64);
	expect_fixed_width_contract("i64",
	                            std::array<std::int64_t, 2>{std::numeric_limits<std::int64_t>::min(),
	                                                        std::numeric_limits<std::int64_t>::max()},
	                            &PacketWriter::write_i64,
	                            &PacketReader::read_i64);
}

TEST(TelemetryProtocolPacketIo, Bool8IsClosedAndUsesOneByteCapacity) {
	expect_fixed_width_contract("bool8",
	                            std::array<bool, 2>{false, true},
	                            &PacketWriter::write_bool8,
	                            &PacketReader::read_bool8);

	std::array<std::uint8_t, 2> storage{};
	PacketWriter writer(writable_view(storage));
	ASSERT_TRUE(writer.write_bool8(false));
	ASSERT_TRUE(writer.write_bool8(true));
	EXPECT_EQ((std::array<std::uint8_t, 2>{0U, 1U}), storage);

	PacketReader valid_reader(byte_view(storage));
	bool first = true;
	bool second = false;
	ASSERT_TRUE(valid_reader.read_bool8(first));
	ASSERT_TRUE(valid_reader.read_bool8(second));
	EXPECT_FALSE(first);
	EXPECT_TRUE(second);

	for (unsigned int raw = 2; raw <= std::numeric_limits<std::uint8_t>::max(); ++raw) {
		SCOPED_TRACE(testing::Message() << "raw bool8 " << raw);
		const auto byte = static_cast<std::uint8_t>(raw);
		PacketReader invalid_reader(ByteView{&byte, 1});
		const bool sentinel = (raw & 1U) != 0;
		bool decoded = sentinel;
		EXPECT_FALSE(invalid_reader.read_bool8(decoded));
		EXPECT_FALSE(invalid_reader.ok());
		EXPECT_EQ(sentinel, decoded);
	}
}

TEST(TelemetryProtocolPacketIo, FiniteFloatExtremaUseExactWidthAndRejectShortBuffers) {
	const auto subnormal = std::numeric_limits<float>::denorm_min();
	ASSERT_GT(subnormal, 0.0F);
	ASSERT_EQ(FP_SUBNORMAL, std::fpclassify(subnormal));
	expect_fixed_width_contract("f32",
	                            std::array<float, 2>{subnormal, std::numeric_limits<float>::max()},
	                            &PacketWriter::write_f32,
	                            &PacketReader::read_f32);
}

TEST(TelemetryProtocolPacketIo, ByteStringsRoundTripAndHonorExactCapacity) {
	const std::array<std::uint8_t, 8> value{0x00, 0x01, 0x7f, 0x80, 0xfe, 0xff, 'F', 'S'};
	std::array<std::uint8_t, value.size()> storage{};
	PacketWriter writer(writable_view(storage));
	ASSERT_TRUE(writer.write_bytes(byte_view(value)));
	EXPECT_EQ(value, storage);
	EXPECT_EQ(0U, writer.remaining());

	PacketReader reader(writer.written());
	ByteView decoded;
	ASSERT_TRUE(reader.read_bytes(value.size(), decoded));
	expect_bytes(decoded, value);
	EXPECT_TRUE(reader.at_end());

	std::array<std::uint8_t, value.size()> short_storage{};
	short_storage.fill(0xa5U);
	const auto canary = short_storage;
	PacketWriter short_writer(MutableByteView{short_storage.data(), short_storage.size() - 1U});
	EXPECT_FALSE(short_writer.write_bytes(byte_view(value)));
	EXPECT_EQ(canary, short_storage);

	PacketReader short_reader(ByteView{storage.data(), storage.size() - 1U});
	const std::uint8_t sentinel = 0x5aU;
	ByteView unchanged{&sentinel, 1};
	EXPECT_FALSE(short_reader.read_bytes(value.size(), unchanged));
	EXPECT_EQ(&sentinel, unchanged.data);
	EXPECT_EQ(1U, unchanged.size);
}

TEST(TelemetryProtocolPacketIo, WritesCanonicalLittleEndianScalars) {
	std::array<std::uint8_t, 40> storage{};
	PacketWriter writer(writable_view(storage));

	ASSERT_TRUE(writer.write_u8(0xabU));
	ASSERT_TRUE(writer.write_i8(-2));
	ASSERT_TRUE(writer.write_u16(0x1234U));
	ASSERT_TRUE(writer.write_i16(-2));
	ASSERT_TRUE(writer.write_u32(0x89abcdefU));
	ASSERT_TRUE(writer.write_i32(-2));
	ASSERT_TRUE(writer.write_u64(0x0123456789abcdefULL));
	ASSERT_TRUE(writer.write_i64(-2));
	ASSERT_TRUE(writer.write_f32(1.0F));
	ASSERT_TRUE(writer.write_f32(-0.0F));
	ASSERT_TRUE(writer.write_zeroes(2));

	const std::array<std::uint8_t, 40> expected{
		0xab,
		0xfe,
		0x34,
		0x12,
		0xfe,
		0xff,
		0xef,
		0xcd,
		0xab,
		0x89,
		0xfe,
		0xff,
		0xff,
		0xff,
		0xef,
		0xcd,
		0xab,
		0x89,
		0x67,
		0x45,
		0x23,
		0x01,
		0xfe,
		0xff,
		0xff,
		0xff,
		0xff,
		0xff,
		0xff,
		0xff,
		0x00,
		0x00,
		0x80,
		0x3f,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
	};

	EXPECT_TRUE(writer.ok());
	EXPECT_EQ(expected.size(), writer.size());
	EXPECT_EQ(0U, writer.remaining());
	expect_bytes(writer.written(), expected);
}

TEST(TelemetryProtocolPacketIo, ReadsCanonicalLittleEndianScalars) {
	const std::array<std::uint8_t, 38> input{
		0xab, 0xfe, 0x34, 0x12, 0xfe, 0xff, 0xef, 0xcd, 0xab, 0x89, 0xfe, 0xff, 0xff,
		0xff, 0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01, 0xfe, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x80,
	};
	PacketReader reader(byte_view(input));

	std::uint8_t u8 = 0;
	std::int8_t i8 = 0;
	std::uint16_t u16 = 0;
	std::int16_t i16 = 0;
	std::uint32_t u32 = 0;
	std::int32_t i32 = 0;
	std::uint64_t u64 = 0;
	std::int64_t i64 = 0;
	float one = 0.0F;
	float zero = 1.0F;

	ASSERT_TRUE(reader.read_u8(u8));
	ASSERT_TRUE(reader.read_i8(i8));
	ASSERT_TRUE(reader.read_u16(u16));
	ASSERT_TRUE(reader.read_i16(i16));
	ASSERT_TRUE(reader.read_u32(u32));
	ASSERT_TRUE(reader.read_i32(i32));
	ASSERT_TRUE(reader.read_u64(u64));
	ASSERT_TRUE(reader.read_i64(i64));
	ASSERT_TRUE(reader.read_f32(one));
	ASSERT_TRUE(reader.read_f32(zero));

	EXPECT_EQ(0xabU, u8);
	EXPECT_EQ(-2, i8);
	EXPECT_EQ(0x1234U, u16);
	EXPECT_EQ(-2, i16);
	EXPECT_EQ(0x89abcdefU, u32);
	EXPECT_EQ(-2, i32);
	EXPECT_EQ(0x0123456789abcdefULL, u64);
	EXPECT_EQ(-2, i64);
	EXPECT_FLOAT_EQ(1.0F, one);
	EXPECT_FLOAT_EQ(0.0F, zero);
	EXPECT_FALSE(std::signbit(zero));
	EXPECT_TRUE(reader.ok());
	EXPECT_TRUE(reader.at_end());
	EXPECT_EQ(input.size(), reader.consumed());
}

TEST(TelemetryProtocolPacketIo, WriterFailureIsBoundedAndSticky) {
	std::array<std::uint8_t, 4> storage{0xaa, 0xbb, 0xcc, 0xdd};
	PacketWriter writer(writable_view(storage));

	ASSERT_TRUE(writer.write_u32(0x12345678U));
	const auto complete = storage;
	EXPECT_FALSE(writer.write_u8(0xffU));
	EXPECT_FALSE(writer.ok());
	EXPECT_EQ(4U, writer.size());
	EXPECT_EQ(0U, writer.remaining());
	EXPECT_EQ(complete, storage);

	EXPECT_FALSE(writer.write_zeroes(0));
	EXPECT_FALSE(writer.write_u8(0U));
	EXPECT_EQ(complete, storage);
}

TEST(TelemetryProtocolPacketIo, WriterCanRollbackAValidCheckpoint) {
	std::array<std::uint8_t, 4> storage{};
	PacketWriter writer(writable_view(storage));

	const auto checkpoint = writer.checkpoint();
	ASSERT_TRUE(writer.write_u16(0x1234U));
	ASSERT_TRUE(writer.rollback(checkpoint));
	ASSERT_TRUE(writer.write_u8(0x7fU));

	EXPECT_TRUE(writer.ok());
	EXPECT_EQ(1U, writer.size());
	EXPECT_EQ(0x7fU, storage[0]);
}

TEST(TelemetryProtocolPacketIo, ReaderFailureDoesNotPublishAValueAndIsSticky) {
	const std::array<std::uint8_t, 2> input{0x34, 0x12};
	PacketReader reader(byte_view(input));
	std::uint32_t value = 0xfeedbeefU;

	EXPECT_FALSE(reader.read_u32(value));
	EXPECT_EQ(0xfeedbeefU, value);
	EXPECT_FALSE(reader.ok());
	EXPECT_EQ(0U, reader.consumed());

	std::uint8_t byte = 0x5aU;
	EXPECT_FALSE(reader.read_u8(byte));
	EXPECT_EQ(0x5aU, byte);
}

TEST(TelemetryProtocolPacketIo, SubreaderIsBoundedToItsDeclaredSlice) {
	const std::array<std::uint8_t, 4> input{1, 2, 3, 4};
	PacketReader reader(byte_view(input));
	std::uint8_t first = 0;
	ASSERT_TRUE(reader.read_u8(first));

	PacketReader child;
	ASSERT_TRUE(reader.subreader(2, child));
	EXPECT_EQ(1U, reader.remaining());

	std::uint16_t child_value = 0;
	ASSERT_TRUE(child.read_u16(child_value));
	EXPECT_EQ(0x0302U, child_value);
	EXPECT_TRUE(child.at_end());

	std::uint8_t last = 0;
	ASSERT_TRUE(reader.read_u8(last));
	EXPECT_EQ(1U, first);
	EXPECT_EQ(4U, last);
	EXPECT_TRUE(reader.at_end());
}

TEST(TelemetryProtocolPacketIo, EmptyViewSupportsZeroLengthReadsAndSubreaders) {
	PacketReader reader(ByteView{});
	const std::uint8_t sentinel = 0x5aU;
	ByteView bytes{&sentinel, 1};
	ASSERT_TRUE(reader.read_bytes(0, bytes));
	EXPECT_EQ(nullptr, bytes.data);
	EXPECT_EQ(0U, bytes.size);
	EXPECT_TRUE(reader.at_end());

	PacketReader child(ByteView{nullptr, 1});
	ASSERT_TRUE(reader.subreader(0, child));
	EXPECT_TRUE(child.ok());
	EXPECT_TRUE(child.at_end());
	EXPECT_EQ(nullptr, child.unread().data);
	EXPECT_TRUE(reader.at_end());
}

TEST(TelemetryProtocolPacketIo, WriterRejectsNonFiniteFloatsWithoutWriting) {
	const std::array<float, 3> invalid{
		std::numeric_limits<float>::quiet_NaN(),
		std::numeric_limits<float>::infinity(),
		-std::numeric_limits<float>::infinity(),
	};

	for (const auto value : invalid) {
		std::array<std::uint8_t, 4> storage{0xa5, 0xa5, 0xa5, 0xa5};
		PacketWriter writer(writable_view(storage));
		EXPECT_FALSE(writer.write_f32(value));
		EXPECT_FALSE(writer.ok());
		EXPECT_EQ(0U, writer.size());
		EXPECT_EQ((std::array<std::uint8_t, 4>{0xa5, 0xa5, 0xa5, 0xa5}), storage);
	}
}

TEST(TelemetryProtocolPacketIo, ReaderRejectsNonFiniteFloatsWithoutPublishingThem) {
	const std::array<std::array<std::uint8_t, 4>, 3> invalid{{
		{{0x00, 0x00, 0x80, 0x7f}},
		{{0x00, 0x00, 0xc0, 0x7f}},
		{{0x00, 0x00, 0x80, 0xff}},
	}};

	for (const auto& bytes : invalid) {
		PacketReader reader(byte_view(bytes));
		float value = 42.0F;
		EXPECT_FALSE(reader.read_f32(value));
		EXPECT_FLOAT_EQ(42.0F, value);
		EXPECT_FALSE(reader.ok());
		EXPECT_EQ(bytes.size(), reader.consumed());
	}
}

TEST(TelemetryProtocolPacketIo, Utf8RoundTripUsesByteLength) {
	const std::string value = "FSO " "\xc3\xa9" " " "\xe2\x82\xac" " " "\xf0\x9f\x9a\x80";
	ASSERT_EQ(15U, value.size());
	std::array<std::uint8_t, 17> storage{};
	PacketWriter writer(writable_view(storage));

	ASSERT_TRUE(writer.write_utf8(value, value.size()));
	EXPECT_EQ(value.size() + 2U, writer.size());
	EXPECT_EQ(15U, storage[0]);
	EXPECT_EQ(0U, storage[1]);

	PacketReader reader(writer.written());
	std::string_view decoded;
	ASSERT_TRUE(reader.read_utf8(value.size(), decoded));
	EXPECT_EQ(std::string_view(value), decoded);
	EXPECT_TRUE(reader.at_end());

	std::array<std::uint8_t, 16> short_storage{};
	short_storage.fill(0xa5U);
	const auto canary = short_storage;
	PacketWriter short_writer(writable_view(short_storage));
	EXPECT_FALSE(short_writer.write_utf8(value, value.size()));
	EXPECT_EQ(canary, short_storage);
}

TEST(TelemetryProtocolPacketIo, Utf8ValidatorRejectsMalformedSequencesAndDefaultNul) {
	const std::array<std::string, 5> invalid{
		std::string("\xc2", 1),
		std::string("\x80", 1),
		std::string("\xc0\x80", 2),
		std::string("\xed\xa0\x80", 3),
		std::string("\xf4\x90\x80\x80", 4),
	};

	for (std::size_t i = 0; i < invalid.size(); ++i) {
		SCOPED_TRACE(testing::Message() << "invalid UTF-8 case " << i);
		EXPECT_FALSE(is_valid_utf8(invalid[i]));
		std::array<std::uint8_t, 16> storage{};
		PacketWriter writer(writable_view(storage));
		EXPECT_FALSE(writer.write_utf8(invalid[i], invalid[i].size()));
		EXPECT_EQ(0U, writer.size());
	}

	const std::string embedded_nul("A\0B", 3);
	EXPECT_FALSE(is_valid_utf8(embedded_nul));
	EXPECT_TRUE(is_valid_utf8(embedded_nul, true));
}

TEST(TelemetryProtocolPacketIo, Utf8WriterEnforcesFieldLimitAndNulPolicy) {
	std::array<std::uint8_t, 16> limited_storage{};
	PacketWriter limited(writable_view(limited_storage));
	EXPECT_FALSE(limited.write_utf8("four", 3));
	EXPECT_EQ(0U, limited.size());

	const std::string embedded_nul("A\0B", 3);
	std::array<std::uint8_t, 16> storage{};
	PacketWriter writer(writable_view(storage));
	ASSERT_TRUE(writer.write_utf8(embedded_nul, embedded_nul.size(), true));

	PacketReader default_reader(writer.written());
	std::string_view rejected = "unchanged";
	EXPECT_FALSE(default_reader.read_utf8(embedded_nul.size(), rejected));
	EXPECT_EQ("unchanged", rejected);

	PacketReader allowed_reader(writer.written());
	std::string_view accepted;
	ASSERT_TRUE(allowed_reader.read_utf8(embedded_nul.size(), accepted, true));
	EXPECT_EQ(std::string_view(embedded_nul), accepted);
}

TEST(TelemetryProtocolPacketIo, Utf8ReaderRejectsTruncatedOversizedAndMalformedFields) {
	{
		const std::array<std::uint8_t, 4> truncated{3, 0, 'a', 'b'};
		PacketReader reader(byte_view(truncated));
		std::string_view output = "unchanged";
		EXPECT_FALSE(reader.read_utf8(3, output));
		EXPECT_EQ("unchanged", output);
	}
	{
		const std::array<std::uint8_t, 6> oversized{4, 0, 't', 'e', 's', 't'};
		PacketReader reader(byte_view(oversized));
		std::string_view output = "unchanged";
		EXPECT_FALSE(reader.read_utf8(3, output));
		EXPECT_EQ("unchanged", output);
	}
	{
		const std::array<std::uint8_t, 4> malformed{2, 0, 0xc0, 0x80};
		PacketReader reader(byte_view(malformed));
		std::string_view output = "unchanged";
		EXPECT_FALSE(reader.read_utf8(2, output));
		EXPECT_EQ("unchanged", output);
	}
}

TEST(TelemetryProtocolPacketIo, Crc32MatchesIsoHdlcChecks) {
	const std::array<std::uint8_t, 0> empty{};
	EXPECT_EQ(0x00000000U, crc32_iso_hdlc(byte_view(empty)));

	const std::string check = "123456789";
	const ByteView check_bytes{
		static_cast<const std::uint8_t*>(static_cast<const void*>(check.data())), check.size()};
	EXPECT_EQ(0xcbf43926U, crc32_iso_hdlc(check_bytes));

	Crc32IsoHdlc incremental;
	incremental.update(check_bytes.subview(0, 4));
	incremental.update(check_bytes.subview(4, check.size() - 4));
	EXPECT_EQ(0xcbf43926U, incremental.value());

	const ByteView invalid{nullptr, 1};
	Crc32IsoHdlc rejected;
	EXPECT_FALSE(rejected.update(invalid));
	EXPECT_FALSE(rejected.ok());
	EXPECT_EQ(0U, rejected.value());
	EXPECT_FALSE(rejected.update(ByteView{}));

	std::uint32_t unchanged = 0xfeedbeefU;
	EXPECT_FALSE(crc32_iso_hdlc(invalid, unchanged));
	EXPECT_EQ(0xfeedbeefU, unchanged);
	EXPECT_EQ(0U, crc32_iso_hdlc(invalid));
}

} // namespace
