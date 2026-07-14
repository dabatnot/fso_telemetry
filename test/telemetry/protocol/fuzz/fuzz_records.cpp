#include "fuzz_common.h"

#include "telemetry/protocol/telemetry_business_records.h"
#include "telemetry/protocol/telemetry_records.h"

using namespace telemetry::protocol;

namespace {

BusinessRecordContainer container_from(std::uint8_t value) noexcept
{
	return static_cast<BusinessRecordContainer>(1U + value % 5U);
}

RecordFlagPolicy flag_policy(BusinessRecordContainer container) noexcept
{
	return container == BusinessRecordContainer::Manifest || container == BusinessRecordContainer::FullSnapshot
			   ? RecordFlagPolicy::RequireNone
			   : RecordFlagPolicy::AllowV1Mutations;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
	if (data == nullptr || size == 0) {
		return 0;
	}

	if ((data[0] & 1U) == 0) {
		if (size < 4U) {
			return 0;
		}
		const auto container = container_from(data[1]);
		const auto record_count = fuzz::read_u16(data, size, 2U);
		const auto records = fuzz::tail(data, size, 4U);
		static_cast<void>(validate_record_region(records, record_count, flag_policy(container)));

		RecordEnvelopeIterator iterator(records, record_count, flag_policy(container));
		for (;;) {
			RecordEnvelopeView record;
			bool has_value = false;
			if (iterator.next(record, has_value) != ValidationError::None || !has_value) {
				break;
			}
			BusinessRecordMetadata metadata;
			static_cast<void>(validate_business_record(record, container, metadata));
		}
		return 0;
	}

	if (size < 7U) {
		return 0;
	}
	RecordEnvelopeView record;
	record.raw_record_type = fuzz::read_u16(data, size, 1U);
	record.record_version = data[3];
	record.record_flags = data[4];
	record.payload = fuzz::tail(data, size, 7U);
	const auto container = container_from(data[5]);
	BusinessRecordMetadata metadata;
	static_cast<void>(business_record_metadata(record.raw_record_type, metadata));
	static_cast<void>(validate_business_record(record, container, metadata));

	StateAtom atom;
	static_cast<void>(decode_business_state_atom(record, container, atom));
	return 0;
}
