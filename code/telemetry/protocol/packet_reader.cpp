#include "telemetry/protocol/packet_reader.h"

#include "telemetry/protocol/packet_writer.h"

#include <cmath>
#include <cstring>

namespace telemetry::protocol {

PacketReader::PacketReader(ByteView input) noexcept : m_input(input) {
	if (input.size != 0 && input.data == nullptr) {
		m_ok = false;
	}
}

std::size_t PacketReader::remaining() const noexcept {
	return m_position <= m_input.size ? m_input.size - m_position : 0;
}

bool PacketReader::fail() noexcept {
	m_ok = false;
	return false;
}

bool PacketReader::reserve(std::size_t count) noexcept {
	if (!m_ok || count > remaining()) {
		return fail();
	}
	return true;
}

bool PacketReader::read_u8(std::uint8_t& value) noexcept {
	if (!reserve(1)) {
		return false;
	}
	value = m_input.data[m_position++];
	return true;
}

bool PacketReader::read_i8(std::int8_t& value) noexcept {
	std::uint8_t raw = 0;
	if (!read_u8(raw)) {
		return false;
	}
	value = static_cast<std::int8_t>(raw);
	return true;
}

bool PacketReader::read_u16(std::uint16_t& value) noexcept {
	if (!reserve(2)) {
		return false;
	}
	const auto* bytes = m_input.data + m_position;
	value = static_cast<std::uint16_t>(bytes[0]) | (static_cast<std::uint16_t>(bytes[1]) << 8U);
	m_position += 2;
	return true;
}

bool PacketReader::read_i16(std::int16_t& value) noexcept {
	std::uint16_t raw = 0;
	if (!read_u16(raw)) {
		return false;
	}
	value = static_cast<std::int16_t>(raw);
	return true;
}

bool PacketReader::read_u32(std::uint32_t& value) noexcept {
	if (!reserve(4)) {
		return false;
	}
	const auto* bytes = m_input.data + m_position;
	value = static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
	        (static_cast<std::uint32_t>(bytes[2]) << 16U) | (static_cast<std::uint32_t>(bytes[3]) << 24U);
	m_position += 4;
	return true;
}

bool PacketReader::read_i32(std::int32_t& value) noexcept {
	std::uint32_t raw = 0;
	if (!read_u32(raw)) {
		return false;
	}
	value = static_cast<std::int32_t>(raw);
	return true;
}

bool PacketReader::read_u64(std::uint64_t& value) noexcept {
	if (!reserve(8)) {
		return false;
	}
	value = 0;
	for (unsigned int shift = 0; shift < 64; shift += 8) {
		value |= static_cast<std::uint64_t>(m_input.data[m_position++]) << shift;
	}
	return true;
}

bool PacketReader::read_i64(std::int64_t& value) noexcept {
	std::uint64_t raw = 0;
	if (!read_u64(raw)) {
		return false;
	}
	value = static_cast<std::int64_t>(raw);
	return true;
}

bool PacketReader::read_f32(float& value) noexcept {
	std::uint32_t bits = 0;
	if (!read_u32(bits)) {
		return false;
	}
	float candidate = 0.0f;
	static_assert(sizeof(bits) == sizeof(candidate), "FSTL requires IEEE-754 binary32 floats");
	std::memcpy(&candidate, &bits, sizeof(candidate));
	if (!std::isfinite(candidate)) {
		return fail();
	}
	value = candidate == 0.0f ? 0.0f : candidate;
	return true;
}

bool PacketReader::read_bytes(std::size_t count, ByteView& value) noexcept {
	if (!reserve(count)) {
		return false;
	}
	value = ByteView{m_input.data + m_position, count};
	m_position += count;
	return true;
}

bool PacketReader::read_utf8(std::size_t field_limit, std::string_view& value, bool allow_nul) noexcept {
	std::uint16_t length = 0;
	if (!read_u16(length) || length > field_limit || length > remaining()) {
		return fail();
	}
	const std::string_view candidate(reinterpret_cast<const char*>(m_input.data + m_position), length);
	if (!is_valid_utf8(candidate, allow_nul)) {
		return fail();
	}
	m_position += length;
	value = candidate;
	return true;
}

bool PacketReader::subreader(std::size_t count, PacketReader& value) noexcept {
	ByteView bytes;
	if (!read_bytes(count, bytes)) {
		return false;
	}
	value = PacketReader(bytes);
	return true;
}

bool PacketReader::skip(std::size_t count) noexcept {
	if (!reserve(count)) {
		return false;
	}
	m_position += count;
	return true;
}

} // namespace telemetry::protocol
