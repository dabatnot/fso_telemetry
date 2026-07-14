#include "telemetry/protocol/packet_writer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace telemetry::protocol {

namespace {

bool validate_utf8_code_point(const unsigned char*& current, const unsigned char* end, bool allow_nul) noexcept {
	const auto lead = *current++;
	if (lead <= 0x7f) {
		return allow_nul || lead != 0;
	}

	std::uint32_t code_point = 0;
	std::size_t continuation_count = 0;
	std::uint32_t minimum = 0;
	if (lead >= 0xc2 && lead <= 0xdf) {
		code_point = lead & 0x1fU;
		continuation_count = 1;
		minimum = 0x80;
	} else if (lead >= 0xe0 && lead <= 0xef) {
		code_point = lead & 0x0fU;
		continuation_count = 2;
		minimum = 0x800;
	} else if (lead >= 0xf0 && lead <= 0xf4) {
		code_point = lead & 0x07U;
		continuation_count = 3;
		minimum = 0x10000;
	} else {
		return false;
	}

	if (static_cast<std::size_t>(end - current) < continuation_count) {
		return false;
	}
	for (std::size_t i = 0; i < continuation_count; ++i) {
		const auto byte = *current++;
		if ((byte & 0xc0U) != 0x80U) {
			return false;
		}
		code_point = (code_point << 6U) | (byte & 0x3fU);
	}

	return code_point >= minimum && code_point <= 0x10ffffU &&
	       !(code_point >= 0xd800U && code_point <= 0xdfffU) && (allow_nul || code_point != 0);
}

} // namespace

bool is_valid_utf8(std::string_view value, bool allow_nul) noexcept {
	if (value.empty()) {
		return true;
	}
	const auto* current = static_cast<const unsigned char*>(static_cast<const void*>(value.data()));
	const auto* end = current + value.size();
	while (current != end) {
		if (!validate_utf8_code_point(current, end, allow_nul)) {
			return false;
		}
	}
	return true;
}

PacketWriter::PacketWriter(MutableByteView output) noexcept : m_output(output) {
	if (output.size != 0 && output.data == nullptr) {
		m_ok = false;
	}
}

std::size_t PacketWriter::remaining() const noexcept {
	return m_position <= m_output.size ? m_output.size - m_position : 0;
}

bool PacketWriter::fail() noexcept {
	m_ok = false;
	return false;
}

bool PacketWriter::reserve(std::size_t count) noexcept {
	if (!m_ok || count > remaining()) {
		return fail();
	}
	return true;
}

bool PacketWriter::rollback(std::size_t checkpoint) noexcept {
	if (!m_ok || checkpoint > m_position) {
		return fail();
	}
	m_position = checkpoint;
	return true;
}

bool PacketWriter::write_u8(std::uint8_t value) noexcept {
	if (!reserve(1)) {
		return false;
	}
	m_output.data[m_position++] = value;
	return true;
}

bool PacketWriter::write_i8(std::int8_t value) noexcept {
	return write_u8(static_cast<std::uint8_t>(value));
}

bool PacketWriter::write_u16(std::uint16_t value) noexcept {
	if (!reserve(2)) {
		return false;
	}
	m_output.data[m_position++] = static_cast<std::uint8_t>(value);
	m_output.data[m_position++] = static_cast<std::uint8_t>(value >> 8U);
	return true;
}

bool PacketWriter::write_i16(std::int16_t value) noexcept {
	return write_u16(static_cast<std::uint16_t>(value));
}

bool PacketWriter::write_u32(std::uint32_t value) noexcept {
	if (!reserve(4)) {
		return false;
	}
	for (unsigned int shift = 0; shift < 32; shift += 8) {
		m_output.data[m_position++] = static_cast<std::uint8_t>(value >> shift);
	}
	return true;
}

bool PacketWriter::write_i32(std::int32_t value) noexcept {
	return write_u32(static_cast<std::uint32_t>(value));
}

bool PacketWriter::write_u64(std::uint64_t value) noexcept {
	if (!reserve(8)) {
		return false;
	}
	for (unsigned int shift = 0; shift < 64; shift += 8) {
		m_output.data[m_position++] = static_cast<std::uint8_t>(value >> shift);
	}
	return true;
}

bool PacketWriter::write_i64(std::int64_t value) noexcept {
	return write_u64(static_cast<std::uint64_t>(value));
}

bool PacketWriter::write_f32(float value) noexcept {
	if (!std::isfinite(value)) {
		return fail();
	}
	if (value == 0.0f) {
		value = 0.0f;
	}
	std::uint32_t bits = 0;
	static_assert(sizeof(bits) == sizeof(value), "FSTL requires IEEE-754 binary32 floats");
	std::memcpy(&bits, &value, sizeof(bits));
	return write_u32(bits);
}

bool PacketWriter::write_bool8(bool value) noexcept {
	return write_u8(value ? 1U : 0U);
}

bool PacketWriter::write_bytes(ByteView value) noexcept {
	if (value.size != 0 && value.data == nullptr) {
		return fail();
	}
	if (!reserve(value.size)) {
		return false;
	}
	if (value.size != 0) {
		std::memcpy(m_output.data + m_position, value.data, value.size);
		m_position += value.size;
	}
	return true;
}

bool PacketWriter::write_utf8(std::string_view value, std::size_t field_limit, bool allow_nul) noexcept {
	if (value.size() > field_limit || value.size() > std::numeric_limits<std::uint16_t>::max() ||
	    !is_valid_utf8(value, allow_nul)) {
		return fail();
	}
	if (remaining() < sizeof(std::uint16_t) || value.size() > remaining() - sizeof(std::uint16_t)) {
		return fail();
	}
	return write_u16(static_cast<std::uint16_t>(value.size())) &&
	       write_bytes(ByteView{static_cast<const std::uint8_t*>(static_cast<const void*>(value.data())), value.size()});
}

bool PacketWriter::write_zeroes(std::size_t count) noexcept {
	if (!reserve(count)) {
		return false;
	}
	if (count != 0) {
		std::fill_n(m_output.data + m_position, count, std::uint8_t{0});
	}
	m_position += count;
	return true;
}

} // namespace telemetry::protocol
