#pragma once

#include "telemetry/protocol/telemetry_protocol_types.h"

#include <cstdint>

namespace telemetry::protocol {

class Crc32IsoHdlc {
  public:
	Crc32IsoHdlc() noexcept = default;

	bool update(ByteView bytes) noexcept;
	bool ok() const noexcept { return m_ok; }
	std::uint32_t value() const noexcept { return m_ok ? m_crc ^ 0xffffffffU : 0U; }

  private:
	std::uint32_t m_crc = 0xffffffffU;
	bool m_ok = true;
};

bool crc32_iso_hdlc(ByteView bytes, std::uint32_t& value) noexcept;
std::uint32_t crc32_iso_hdlc(ByteView bytes) noexcept;

} // namespace telemetry::protocol
