#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace telemetry::protocol {

static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
              "FSTL 1.1 requires IEEE-754 binary32 floats");
static_assert(std::numeric_limits<std::int8_t>::min() == -128 &&
                  std::numeric_limits<std::int16_t>::min() == -32768 &&
                  std::numeric_limits<std::int32_t>::min() == (-2147483647 - 1) &&
                  std::numeric_limits<std::int64_t>::min() == (-9223372036854775807LL - 1),
              "FSTL 1.1 requires two's-complement signed integers");

struct ByteView {
	const std::uint8_t* data = nullptr;
	std::size_t size = 0;

	constexpr bool empty() const noexcept { return size == 0; }
	constexpr const std::uint8_t* begin() const noexcept { return data; }
	constexpr const std::uint8_t* end() const noexcept { return data == nullptr ? nullptr : data + size; }

	constexpr ByteView subview(std::size_t offset, std::size_t count) const noexcept {
		return data != nullptr && offset <= size && count <= size - offset ? ByteView{data + offset, count} : ByteView{};
	}
};

struct MutableByteView {
	std::uint8_t* data = nullptr;
	std::size_t size = 0;

	constexpr bool empty() const noexcept { return size == 0; }
	constexpr std::uint8_t* begin() const noexcept { return data; }
	constexpr std::uint8_t* end() const noexcept { return data == nullptr ? nullptr : data + size; }

	constexpr operator ByteView() const noexcept { return ByteView{data, size}; }
};

} // namespace telemetry::protocol
