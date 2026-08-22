#include "fuzz_common.h"

#include "telemetry/protocol/telemetry_business_records.h"
#include "telemetry/protocol/telemetry_business_state_validation.h"
#include "telemetry/protocol/telemetry_state_messages.h"

using namespace telemetry::protocol;

extern "C" int LLVMFuzzerTestOneInput(
	const std::uint8_t* data, std::size_t size)
{
	if (data == nullptr || size < 4U)
		return 0;

	BusinessStateValidationContext context;
	context.protocol_minor = VersionMinor;
	context.required_manifest_id =
		static_cast<std::uint32_t>(data[0]);
	context.class_manifest_installed = (data[1] & 1U) != 0U;
	context.weapon_manifest_installed = (data[1] & 2U) != 0U;
	context.enforce_negotiated_capabilities = true;
	context.negotiated_capabilities =
		static_cast<std::uint64_t>(data[1] >> 4U);
	BusinessStateImageValidator validator(context);

	const auto record_count = fuzz::read_u16(data, size, 2U);
	const auto records = fuzz::tail(data, size, 4U);
	StateImage image;
	static_cast<void>(decode_business_snapshot_region_validated(
		records, record_count, validator, image));

	DeltaPayload delta;
	if (decode_delta_payload(records, delta) == ValidationError::None) {
		CumulativeStateDelta mutations;
		static_cast<void>(decode_business_delta(
			delta, VersionMinor, mutations));
	}

	RecordEnvelopeIterator iterator(
		records, record_count, RecordFlagPolicy::AllowV1Mutations);
	for (;;) {
		RecordEnvelopeView record;
		bool has_value = false;
		if (iterator.next(record, has_value) != ValidationError::None ||
			!has_value)
			break;
		BusinessRecordMetadata metadata;
		static_cast<void>(validate_business_record(record,
			BusinessRecordContainer::FullSnapshot,
			VersionMinor, metadata));
		StateAtom atom;
		static_cast<void>(decode_business_state_atom(record,
			BusinessRecordContainer::FullSnapshot,
			VersionMinor, atom));
	}
	return 0;
}
