#pragma once

#include "telemetry/protocol/telemetry_protocol_types.h"

#include <cstddef>
#include <cstdint>

namespace telemetry::protocol::fuzz {

inline ByteView bytes(const std::uint8_t* data, std::size_t size) noexcept
{
	return ByteView{size == 0 ? nullptr : data, size};
}

inline std::uint16_t read_u16(const std::uint8_t* data, std::size_t size, std::size_t offset) noexcept
{
	if (data == nullptr || offset > size || size - offset < 2U) {
		return 0;
	}
	return static_cast<std::uint16_t>(data[offset]) |
		   static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset + 1U]) << 8U);
}

inline ByteView tail(const std::uint8_t* data, std::size_t size, std::size_t offset) noexcept
{
	if (data == nullptr || offset > size) {
		return ByteView{};
	}
	return ByteView{offset == size ? nullptr : data + offset, size - offset};
}

} // namespace telemetry::protocol::fuzz
