#include "fuzz_common.h"

#include "telemetry/protocol/telemetry_datagram.h"
#include "telemetry/protocol/telemetry_reassembler.h"

#include <algorithm>

using namespace telemetry::protocol;

namespace {

void ingest_candidate(ByteView datagram, TelemetryReassembler& reassembler)
{
	DatagramView fragment;
	if (decode_and_validate_datagram(datagram, fragment) != ValidationError::None) {
		TelemetryDatagramHeader header;
		if (decode_datagram_header(datagram, header) != ValidationError::None || header.header_size != HeaderSizeV1 ||
			header.payload_size > MaxFragmentPayload || datagram.size < HeaderSizeV1 ||
			header.payload_size > datagram.size - HeaderSizeV1) {
			return;
		}
		fragment.header = header;
		fragment.payload = datagram.subview(HeaderSizeV1, header.payload_size);
	}

	ReassembledMessage completed;
	const auto result = reassembler.ingest(fragment, completed);
	if (result == ReassemblyResult::Completed) {
		static_cast<void>(validate_message_crc(completed.header, completed.payload_view()));
	}
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
	TelemetryReassembler reassembler;
	std::size_t offset = 0;
	std::size_t fragment_count = 0;
	while (offset <= size && size - offset >= 2U && fragment_count < MaxVideoFragments) {
		const auto length = static_cast<std::size_t>(fuzz::read_u16(data, size, offset));
		offset += 2U;
		if (length == 0 || length > size - offset) {
			break;
		}
		ingest_candidate(fuzz::bytes(data + offset, length), reassembler);
		offset += length;
		++fragment_count;
	}

	if (fragment_count == 0) {
		ingest_candidate(fuzz::bytes(data, size), reassembler);
	}
	reassembler.clear();
	return 0;
}
