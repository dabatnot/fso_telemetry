#include "fuzz_common.h"

#include "telemetry/protocol/packet_reader.h"
#include "telemetry/protocol/packet_writer.h"

#include <algorithm>
#include <cstdlib>
#include <string_view>

using namespace telemetry::protocol;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
	PacketReader reader(fuzz::bytes(data, size));
	const auto operation_count = std::min<std::size_t>(size, 512U);
	for (std::size_t index = 0; index < operation_count && reader.ok(); ++index) {
		const auto operation = static_cast<unsigned int>(data[index] % 14U);
		const auto requested = static_cast<std::size_t>(fuzz::read_u16(data, size, index + 1U));
		switch (operation) {
		case 0: {
			std::uint8_t value = 0;
			reader.read_u8(value);
			break;
		}
		case 1: {
			std::int8_t value = 0;
			reader.read_i8(value);
			break;
		}
		case 2: {
			std::uint16_t value = 0;
			reader.read_u16(value);
			break;
		}
		case 3: {
			std::int16_t value = 0;
			reader.read_i16(value);
			break;
		}
		case 4: {
			std::uint32_t value = 0;
			reader.read_u32(value);
			break;
		}
		case 5: {
			std::int32_t value = 0;
			reader.read_i32(value);
			break;
		}
		case 6: {
			std::uint64_t value = 0;
			reader.read_u64(value);
			break;
		}
		case 7: {
			std::int64_t value = 0;
			reader.read_i64(value);
			break;
		}
		case 8: {
			float value = 0.0F;
			reader.read_f32(value);
			break;
		}
		case 9: {
			bool value = false;
			reader.read_bool8(value);
			break;
		}
		case 10: {
			ByteView value;
			reader.read_bytes(requested, value);
			break;
		}
		case 11: {
			std::string_view value;
			reader.read_utf8(requested, value, (data[index] & 0x80U) != 0);
			break;
		}
		case 12: {
			PacketReader nested;
			if (reader.subreader(requested, nested)) {
				std::uint64_t value = 0;
				nested.read_u64(value);
			}
			break;
		}
		case 13:
		default:
			reader.skip(requested);
			break;
		}
	}

	if (!reader.ok()) {
		std::uint8_t canary = 0;
		if (reader.read_u8(canary) || reader.skip(0)) {
			std::abort();
		}
	}

	const auto input = fuzz::bytes(data, size);
	static constexpr char EmptyText = '\0';
	const auto* text_data = input.empty() ? &EmptyText : static_cast<const char*>(static_cast<const void*>(input.data));
	const std::string_view text(text_data, input.size);
	static_cast<void>(is_valid_utf8(text, false));
	static_cast<void>(is_valid_utf8(text, true));
	return 0;
}
