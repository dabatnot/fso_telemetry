#pragma once

#include <cstddef>
#include <cstdint>

namespace telemetry::protocol {

struct ByteView {
	const std::uint8_t* data = nullptr;
	std::size_t size = 0;

	constexpr bool empty() const noexcept { return size == 0; }
	constexpr const std::uint8_t* begin() const noexcept { return data; }
	constexpr const std::uint8_t* end() const noexcept { return data + size; }

	constexpr ByteView subview(std::size_t offset, std::size_t count) const noexcept {
		return offset <= size && count <= size - offset ? ByteView{data + offset, count} : ByteView{};
	}
};

struct MutableByteView {
	std::uint8_t* data = nullptr;
	std::size_t size = 0;

	constexpr bool empty() const noexcept { return size == 0; }
	constexpr std::uint8_t* begin() const noexcept { return data; }
	constexpr std::uint8_t* end() const noexcept { return data + size; }

	constexpr operator ByteView() const noexcept { return ByteView{data, size}; }
};

template <typename Container>
ByteView as_bytes(const Container& value) {
	return ByteView{reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

template <typename Container>
MutableByteView as_writable_bytes(Container& value) {
	return MutableByteView{reinterpret_cast<std::uint8_t*>(value.data()), value.size()};
}

} // namespace telemetry::protocol
