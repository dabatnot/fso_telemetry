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

void Crc32IsoHdlc::update(ByteView bytes) noexcept {
	for (std::size_t i = 0; i < bytes.size; ++i) {
		m_crc = (m_crc >> 8U) ^ CrcTable[(m_crc ^ bytes.data[i]) & 0xffU];
	}
}

std::uint32_t crc32_iso_hdlc(ByteView bytes) noexcept {
	Crc32IsoHdlc crc;
	crc.update(bytes);
	return crc.value();
}

} // namespace telemetry::protocol
