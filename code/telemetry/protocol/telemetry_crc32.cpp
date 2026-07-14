#include "telemetry/protocol/telemetry_crc32.h"

#include <array>

namespace telemetry::protocol {

namespace {

constexpr std::array<std::uint32_t, 256> make_crc_table() noexcept {
	std::array<std::uint32_t, 256> table{};
	for (std::uint32_t i = 0; i < table.size(); ++i) {
		auto value = i;
		for (int bit = 0; bit < 8; ++bit) {
			value = (value >> 1U) ^ ((value & 1U) != 0 ? 0xedb88320U : 0U);
		}
		table[i] = value;
	}
	return table;
}

constexpr auto CrcTable = make_crc_table();

} // namespace

bool Crc32IsoHdlc::update(ByteView bytes) noexcept {
	if (!m_ok || (bytes.size != 0 && bytes.data == nullptr)) {
		m_ok = false;
		return false;
	}
	for (std::size_t i = 0; i < bytes.size; ++i) {
		m_crc = (m_crc >> 8U) ^ CrcTable[(m_crc ^ bytes.data[i]) & 0xffU];
	}
	return true;
}

bool crc32_iso_hdlc(ByteView bytes, std::uint32_t& value) noexcept {
	Crc32IsoHdlc crc;
	if (!crc.update(bytes)) {
		return false;
	}
	value = crc.value();
	return true;
}

std::uint32_t crc32_iso_hdlc(ByteView bytes) noexcept {
	std::uint32_t value = 0;
	crc32_iso_hdlc(bytes, value);
	return value;
}

} // namespace telemetry::protocol
