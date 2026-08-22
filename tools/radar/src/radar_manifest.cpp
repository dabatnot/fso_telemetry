#include "radar_manifest.h"

#include "telemetry/protocol/telemetry_manifest_metadata.h"
#include "telemetry/protocol/telemetry_records.h"
#include "telemetry/protocol/telemetry_transaction.h"

namespace simpit::radar {
namespace protocol = telemetry::protocol;

bool buildRadarManifestCatalog(
    const protocol::CompletedTransaction& transaction,
    std::shared_ptr<const RadarManifestCatalog>& output,
    QString* error)
{
    const auto fail = [error](const QString& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (transaction.message_type != protocol::MessageType::Manifest ||
        transaction.transaction_id == 0 ||
        transaction.kind_or_flags != static_cast<std::uint16_t>(protocol::ManifestKind::FullRequired)) {
        return fail(QStringLiteral("Non-canonical MANIFEST transaction"));
    }

    auto candidate = std::make_shared<RadarManifestCatalog>();
    candidate->manifestId = transaction.transaction_id;
    for (const auto& part : transaction.parts) {
        protocol::RecordEnvelopeIterator iterator(
            part.records_view(), part.record_count, protocol::RecordFlagPolicy::RequireNone);
        for (;;) {
            protocol::RecordEnvelopeView record;
            bool hasValue = false;
            if (iterator.next(record, hasValue) != protocol::ValidationError::None)
                return fail(QStringLiteral("Invalid MANIFEST envelope"));
            if (!hasValue) break;

            if (record.raw_record_type ==
                static_cast<std::uint16_t>(protocol::RecordType::ClassManifest)) {
                protocol::ClassManifestRadarMetadata metadata;
                if (protocol::decode_class_manifest_radar_metadata(
                        record, protocol::VersionMinor, metadata) !=
                        protocol::ValidationError::None ||
                    metadata.manifest_generation != transaction.transaction_id ||
                    metadata.class_id == 0 ||
                    !candidate->shipRadarIconIds.emplace(
                        metadata.class_id,
                        metadata.has_radar_icon ? metadata.radar_icon_id : 0U).second) {
                    return fail(QStringLiteral("Invalid or duplicate radar MANIFEST class"));
                }
            } else if (record.raw_record_type ==
                       static_cast<std::uint16_t>(protocol::RecordType::WeaponManifest)) {
                protocol::WeaponManifestRadarMetadata metadata;
                if (protocol::decode_weapon_manifest_radar_metadata(
                        record, protocol::VersionMinor, metadata) !=
                        protocol::ValidationError::None ||
                    metadata.manifest_generation != transaction.transaction_id ||
                    metadata.weapon_class_id == 0 ||
                    !candidate->weapons.emplace(metadata.weapon_class_id,
                        RadarWeaponMetadata{metadata.subtype, metadata.weapon_flags,
                            metadata.nominal_lock_time_us, metadata.has_lock}).second) {
                    return fail(QStringLiteral("Invalid or duplicate radar MANIFEST weapon"));
                }
            } else {
                return fail(QStringLiteral("Unexpected MANIFEST record"));
            }
        }
    }
    output = std::move(candidate);
    if (error != nullptr) error->clear();
    return true;
}

} // namespace simpit::radar
