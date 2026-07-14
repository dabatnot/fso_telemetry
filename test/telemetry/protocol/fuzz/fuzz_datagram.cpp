#include "fuzz_common.h"

#include "telemetry/protocol/telemetry_crc32.h"
#include "telemetry/protocol/telemetry_datagram.h"

#include <array>

using namespace telemetry::protocol;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
	const auto input = fuzz::bytes(data, size);
	static_cast<void>(crc32_iso_hdlc(input));

	TelemetryDatagramHeader raw_header;
	if (decode_datagram_header(input, raw_header) == ValidationError::None) {
		static_cast<void>(validate_fragment_layout(raw_header));
		std::uint16_t count = 0;
		static_cast<void>(expected_fragment_count(raw_header.message_type, raw_header.message_size, count));
	}

	DatagramView envelope;
	if (decode_and_validate_datagram_envelope(input, envelope) == ValidationError::None) {
		std::uint32_t crc = 0;
		static_cast<void>(calculate_datagram_crc(envelope.header, envelope.payload, crc));
		static_cast<void>(validate_fragment_layout(envelope.header));
	}

	DatagramView decoded;
	if (decode_and_validate_datagram(input, decoded) == ValidationError::None) {
		std::array<std::uint8_t, MaxDatagramSize> encoded{};
		std::size_t written = 0;
		if (encode_datagram(decoded.header,
				decoded.payload,
				MutableByteView{encoded.data(), encoded.size()},
				written) == ValidationError::None) {
			DatagramView roundtrip;
			static_cast<void>(decode_and_validate_datagram(ByteView{encoded.data(), written}, roundtrip));
		}
	}
	return 0;
}
