#pragma once

#include "telemetry/protocol/telemetry_protocol_types.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace telemetry::protocol {

class PacketReader {
  public:
	PacketReader() noexcept = default;
	explicit PacketReader(ByteView input) noexcept;

	bool read_u8(std::uint8_t& value) noexcept;
	bool read_i8(std::int8_t& value) noexcept;
	bool read_u16(std::uint16_t& value) noexcept;
	bool read_i16(std::int16_t& value) noexcept;
	bool read_u32(std::uint32_t& value) noexcept;
	bool read_i32(std::int32_t& value) noexcept;
	bool read_u64(std::uint64_t& value) noexcept;
	bool read_i64(std::int64_t& value) noexcept;
	bool read_f32(float& value) noexcept;
	bool read_bytes(std::size_t count, ByteView& value) noexcept;
	bool read_utf8(std::size_t field_limit, std::string_view& value, bool allow_nul = false) noexcept;
	bool subreader(std::size_t count, PacketReader& value) noexcept;
	bool skip(std::size_t count) noexcept;

	std::size_t consumed() const noexcept { return m_position; }
	std::size_t remaining() const noexcept;
	bool at_end() const noexcept { return m_ok && remaining() == 0; }
	bool ok() const noexcept { return m_ok; }
	ByteView unread() const noexcept { return ByteView{m_input.data + m_position, remaining()}; }

  private:
	bool reserve(std::size_t count) noexcept;
	bool fail() noexcept;

	ByteView m_input;
	std::size_t m_position = 0;
	bool m_ok = true;
};

} // namespace telemetry::protocol
