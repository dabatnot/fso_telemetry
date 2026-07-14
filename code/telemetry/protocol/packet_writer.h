#pragma once

#include "telemetry/protocol/telemetry_protocol_types.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace telemetry::protocol {

class PacketWriter {
  public:
	explicit PacketWriter(MutableByteView output) noexcept;

	bool write_u8(std::uint8_t value) noexcept;
	bool write_i8(std::int8_t value) noexcept;
	bool write_u16(std::uint16_t value) noexcept;
	bool write_i16(std::int16_t value) noexcept;
	bool write_u32(std::uint32_t value) noexcept;
	bool write_i32(std::int32_t value) noexcept;
	bool write_u64(std::uint64_t value) noexcept;
	bool write_i64(std::int64_t value) noexcept;
	bool write_f32(float value) noexcept;
	bool write_bytes(ByteView value) noexcept;
	bool write_utf8(std::string_view value, std::size_t field_limit, bool allow_nul = false) noexcept;
	bool write_zeroes(std::size_t count) noexcept;

	std::size_t size() const noexcept { return m_position; }
	std::size_t remaining() const noexcept;
	bool ok() const noexcept { return m_ok; }
	std::size_t checkpoint() const noexcept { return m_position; }
	bool rollback(std::size_t checkpoint) noexcept;
	ByteView written() const noexcept { return ByteView{m_output.data, m_position}; }

  private:
	bool reserve(std::size_t count) noexcept;
	bool fail() noexcept;

	MutableByteView m_output;
	std::size_t m_position = 0;
	bool m_ok = true;
};

bool is_valid_utf8(std::string_view value, bool allow_nul = false) noexcept;

} // namespace telemetry::protocol
