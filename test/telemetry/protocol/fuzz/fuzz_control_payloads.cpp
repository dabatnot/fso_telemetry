#include "fuzz_common.h"

#include "telemetry/protocol/telemetry_control_messages.h"
#include "telemetry/protocol/telemetry_reliability_messages.h"

#include <array>

using namespace telemetry::protocol;

namespace {

template <typename Payload, typename Decoder, typename Encoder>
void decode_and_roundtrip(ByteView input, Decoder decoder, Encoder encoder)
{
	Payload payload;
	if (decoder(input, payload) != ValidationError::None) {
		return;
	}
	std::array<std::uint8_t, 2048> output{};
	std::size_t written = 0;
	static_cast<void>(encoder(payload, MutableByteView{output.data(), output.size()}, written));
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
	if (data == nullptr || size == 0) {
		return 0;
	}
	const auto payload = fuzz::tail(data, size, 1U);
	switch (static_cast<MessageType>(data[0])) {
	case MessageType::Discovery:
		decode_and_roundtrip<DiscoveryPayload>(payload, decode_discovery_payload, encode_discovery_payload);
		break;
	case MessageType::Hello:
		decode_and_roundtrip<HelloPayload>(payload, decode_hello_payload, encode_hello_payload);
		break;
	case MessageType::Welcome:
		decode_and_roundtrip<WelcomePayload>(payload, decode_welcome_payload, encode_welcome_payload);
		break;
	case MessageType::SessionBegin:
		decode_and_roundtrip<SessionBeginPayload>(payload, decode_session_begin_payload, encode_session_begin_payload);
		break;
	case MessageType::Heartbeat:
		decode_and_roundtrip<HeartbeatPayload>(payload, decode_heartbeat_payload, encode_heartbeat_payload);
		break;
	case MessageType::Ack:
		decode_and_roundtrip<AckPayload>(payload, decode_ack_payload, encode_ack_payload);
		break;
	case MessageType::Nack:
		decode_and_roundtrip<NackPayload>(payload, decode_nack_payload, encode_nack_payload);
		break;
	case MessageType::ResyncRequest:
		decode_and_roundtrip<ResyncRequestPayload>(payload,
			decode_resync_request_payload,
			encode_resync_request_payload);
		break;
	case MessageType::SessionEnd:
		decode_and_roundtrip<SessionEndPayload>(payload, decode_session_end_payload, encode_session_end_payload);
		break;
	case MessageType::CapabilityUpdate:
		decode_and_roundtrip<CapabilityUpdatePayload>(payload,
			decode_capability_update_payload,
			encode_capability_update_payload);
		break;
	default: {
		const auto count = fuzz::read_u16(data, size, 1U);
		static_cast<void>(validate_capability_extensions(fuzz::tail(data, size, 3U), count));
		if (validate_missing_fragment_bitmap(payload, count) == ValidationError::None) {
			MissingFragmentIterator iterator(payload, count);
			for (;;) {
				std::uint16_t fragment_index = 0;
				bool has_value = false;
				if (iterator.next(fragment_index, has_value) != ValidationError::None || !has_value) {
					break;
				}
			}
		}
		break;
	}
	}
	return 0;
}
