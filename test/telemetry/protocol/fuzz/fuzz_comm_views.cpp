#include "fuzz_common.h"

#include "telemetry/protocol/telemetry_specialized_views.h"

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
	std::array<std::uint8_t, 128> output{};
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
	switch (data[0]) {
	case 0:
	case static_cast<std::uint8_t>(RecordType::CommAssetManifest):
		static_cast<void>(validate_comm_asset_manifest_record_payload(payload));
		break;
	case 1:
		decode_and_roundtrip<CommBundleOffer>(payload, decode_comm_bundle_offer, encode_comm_bundle_offer);
		break;
	case 2:
		decode_and_roundtrip<CommBundleSelection>(payload,
			decode_comm_bundle_selection,
			encode_comm_bundle_selection);
		break;
	case 3:
	case static_cast<std::uint8_t>(RecordType::CommViewState):
		decode_and_roundtrip<CommViewStatePayload>(payload,
			decode_comm_view_state_payload,
			encode_comm_view_state_payload);
		break;
	case 4:
	case static_cast<std::uint8_t>(RecordType::CommViewEvent):
		decode_and_roundtrip<CommViewEventPayload>(payload,
			decode_comm_view_event_payload,
			encode_comm_view_event_payload);
		break;
	case 5:
	default:
		static_cast<void>(validate_portable_bundle_path(payload));
		static_cast<void>(validate_comm_asset_entry_payload(payload));
		break;
	}
	return 0;
}
