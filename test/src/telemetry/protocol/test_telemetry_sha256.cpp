#include "telemetry/protocol/telemetry_sha256.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace telemetry::protocol;

ByteView byte_view(const std::string& bytes) {
	return ByteView{reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
}

template <typename Container>
ByteView byte_view(const Container& bytes) {
	return ByteView{static_cast<const std::uint8_t*>(static_cast<const void*>(bytes.data())), bytes.size()};
}

Sha256Digest digest_from_hex(const char* hex) {
	Sha256Digest result{};
	const auto hex_digit = [](char value) -> std::uint8_t {
		if (value >= '0' && value <= '9') {
			return static_cast<std::uint8_t>(value - '0');
		}
		return static_cast<std::uint8_t>(value - 'a' + 10);
	};
	for (std::size_t index = 0; index < result.size(); ++index) {
		result[index] = static_cast<std::uint8_t>((hex_digit(hex[index * 2]) << 4U) | hex_digit(hex[index * 2 + 1]));
	}
	return result;
}

void expect_digest(ByteView input, const char* expected_hex) {
	Sha256Digest actual{};
	ASSERT_TRUE(sha256(input, actual));
	EXPECT_EQ(digest_from_hex(expected_hex), actual);
}

TEST(TelemetryProtocolSha256, MatchesPublishedEmptyAbcAndNistMultiBlockVectors) {
	expect_digest(ByteView{}, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	expect_digest(byte_view(std::string{"abc"}),
	              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	expect_digest(byte_view(std::string{"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"}),
	              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(TelemetryProtocolSha256, MatchesMillionAAndFragmentedUpdatesAcrossBlockBoundaries) {
	const std::string million_a(1'000'000, 'a');
	const auto expected = digest_from_hex("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

	Sha256Digest one_shot{};
	ASSERT_TRUE(sha256(byte_view(million_a), one_shot));
	EXPECT_EQ(expected, one_shot);

	Sha256 fragmented;
	const std::array<std::size_t, 9> chunk_sizes{{1U, 63U, 64U, 65U, 7U, 4096U, 31U, 8191U, 257U}};
	std::size_t offset = 0;
	std::size_t chunk_index = 0;
	while (offset < million_a.size()) {
		const auto count = std::min(chunk_sizes[chunk_index % chunk_sizes.size()], million_a.size() - offset);
		ASSERT_TRUE(fragmented.update(ByteView{
		    reinterpret_cast<const std::uint8_t*>(million_a.data() + static_cast<std::ptrdiff_t>(offset)), count}));
		ASSERT_TRUE(fragmented.update(ByteView{}));
		offset += count;
		++chunk_index;
	}
	Sha256Digest actual{};
	ASSERT_TRUE(fragmented.finalize(actual));
	EXPECT_EQ(expected, actual);
}

TEST(TelemetryProtocolSha256, FinalizeIsIdempotentAndUpdateAfterFinalizeFailsClosed) {
	Sha256 calculator;
	ASSERT_TRUE(calculator.update(byte_view(std::string{"abc"})));
	Sha256Digest first{};
	Sha256Digest second{};
	ASSERT_TRUE(calculator.finalize(first));
	ASSERT_TRUE(calculator.finalized());
	ASSERT_TRUE(calculator.ok());
	ASSERT_TRUE(calculator.finalize(second));
	EXPECT_EQ(first, second);

	const std::array<std::uint8_t, 1> extra{{0x42U}};
	EXPECT_FALSE(calculator.update(byte_view(extra)));
	EXPECT_FALSE(calculator.ok());
	Sha256Digest rejected;
	rejected.fill(0xffU);
	EXPECT_FALSE(calculator.finalize(rejected));
	EXPECT_EQ(Sha256Digest{}, rejected);
}

TEST(TelemetryProtocolSha256, InvalidViewPoisonsStateAndResetRestoresFreshHasher) {
	Sha256 calculator;
	EXPECT_FALSE(calculator.update(ByteView{nullptr, 1U}));
	EXPECT_FALSE(calculator.ok());
	Sha256Digest rejected;
	rejected.fill(0xffU);
	EXPECT_FALSE(calculator.finalize(rejected));
	EXPECT_EQ(Sha256Digest{}, rejected);

	calculator.reset();
	EXPECT_TRUE(calculator.ok());
	EXPECT_FALSE(calculator.finalized());
	ASSERT_TRUE(calculator.update(byte_view(std::string{"abc"})));
	Sha256Digest actual{};
	ASSERT_TRUE(calculator.finalize(actual));
	EXPECT_EQ(digest_from_hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
	          actual);

	Sha256Digest one_shot;
	one_shot.fill(0xffU);
	EXPECT_FALSE(sha256(ByteView{nullptr, 1U}, one_shot));
	EXPECT_EQ(Sha256Digest{}, one_shot);
}

} // namespace
