#pragma once

#include "telemetry/protocol/telemetry_protocol_types.h"

#include <cstdint>

namespace telemetry::protocol {

class Crc32IsoHdlc {
  public:
	Crc32IsoHdlc() noexcept = default;

	void update(ByteView bytes) noexcept;
	std::uint32_t value() const noexcept { return m_crc ^ 0xffffffffU; }

  private:
	std::uint32_t m_crc = 0xffffffffU;
};

std::uint32_t crc32_iso_hdlc(ByteView bytes) noexcept;

} // namespace telemetry::protocol
