#include "radar_manifest.h"

#include "telemetry/phase2_manifest_builder.h"
#include "telemetry/protocol/packet_writer.h"
#include "telemetry/protocol/telemetry_business_records.h"
#include "telemetry/protocol/telemetry_manifest_metadata.h"
#include "telemetry/protocol/telemetry_protocol_constants.h"
#include "telemetry/protocol/telemetry_records.h"
#include "telemetry/protocol/telemetry_transaction.h"

#include <QTest>

#include <array>
#include <memory>
#include <vector>

using namespace simpit::radar;
namespace protocol = telemetry::protocol;

namespace {

std::vector<std::uint8_t> classRecord(
    std::uint32_t generation, std::uint32_t classId, std::uint32_t iconId)
{
    std::array<std::uint8_t, 256> payload{};
    protocol::PacketWriter writer({payload.data(), payload.size()});
    const auto presence = iconId == 0U
        ? protocol::ClassManifestPresenceFlagNone
        : protocol::ClassManifestPresenceFlagRadarIcon;
    writer.write_u32(generation);
    writer.write_u32(classId);
    writer.write_u64(presence);
    writer.write_utf8("Test Ship", 255);
    writer.write_u32(0);
    writer.write_u32(1);
    writer.write_f32(1.0F);
    writer.write_f32(0.0F);
    writer.write_f32(0.0F);
    writer.write_f32(0.0F);
    if (iconId != 0U) writer.write_u32(iconId);
    if (!writer.ok()) return {};

    const protocol::RecordEnvelopeView envelope{
        static_cast<std::uint16_t>(protocol::RecordType::ClassManifest),
        1, protocol::RecordFlagNone, writer.written()};
    std::array<std::uint8_t, 512> encoded{};
    std::size_t written = 0;
    if (protocol::encode_business_record(
            envelope, protocol::BusinessRecordContainer::Manifest,
            protocol::VersionMinorV1_1, {encoded.data(), encoded.size()}, written) !=
        protocol::ValidationError::None) {
        return {};
    }
    return {encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(written)};
}

std::vector<std::uint8_t> weaponRecord(
    std::uint32_t generation, std::uint32_t classId, protocol::WeaponSubtype subtype,
    std::uint64_t lockTimeUs = 0U)
{
    std::array<std::uint8_t, 256> payload{};
    protocol::PacketWriter writer({payload.data(), payload.size()});
    writer.write_u32(generation);
    writer.write_u32(classId);
    writer.write_u64(lockTimeUs == 0U ? protocol::WeaponManifestPresenceFlagNone
                                     : protocol::WeaponManifestPresenceFlagLock);
    writer.write_utf8("Test Weapon", 255);
    writer.write_u8(static_cast<std::uint8_t>(subtype));
    writer.write_u64(0);
    writer.write_f32(100.0F);
    writer.write_f32(1.0F);
    writer.write_f32(0.0F);
    writer.write_u64(1);
    if (lockTimeUs != 0U) {
        writer.write_u64(lockTimeUs);
        writer.write_f32(0.5F);
    }
    writer.write_f32(0.0F);
    if (!writer.ok()) return {};

    const protocol::RecordEnvelopeView envelope{
        static_cast<std::uint16_t>(protocol::RecordType::WeaponManifest),
        1, protocol::RecordFlagNone, writer.written()};
    std::array<std::uint8_t, 512> encoded{};
    std::size_t written = 0;
    if (protocol::encode_business_record(
            envelope, protocol::BusinessRecordContainer::Manifest,
            protocol::VersionMinorV1_1, {encoded.data(), encoded.size()}, written) !=
        protocol::ValidationError::None) {
        return {};
    }
    return {encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(written)};
}

protocol::CompletedTransaction transaction(
    std::uint32_t generation,
    std::initializer_list<std::vector<std::uint8_t>> parts)
{
    protocol::CompletedTransaction result;
    result.message_type = protocol::MessageType::Manifest;
    result.transaction_id = generation;
    result.kind_or_flags = static_cast<std::uint16_t>(protocol::ManifestKind::FullRequired);
    std::uint32_t messageId = 1;
    for (const auto& records : parts) {
        protocol::CompletedTransactionPart part;
        part.message_id = messageId++;
        part.record_count = 1;
        part.records = records;
        result.parts.push_back(std::move(part));
    }
    return result;
}

} // namespace

class RadarManifestTests final : public QObject {
    Q_OBJECT
private slots:
    void mapsCanonicalShipTypes()
    {
        const std::array<const char*, 17> names{
            "Navbuoy", "Sentry Gun", "Escape Pod", "Cargo", "Support",
            "Fighter", "Bomber", "Transport", "Freighter", "AWACS",
            "Gas Miner", "Cruiser", "Corvette", "Capital", "Super Cap",
            "Drydock", "Knossos Device"};
        for (std::uint32_t index = 0; index < names.size(); ++index)
            QCOMPARE(telemetry::radar_icon_id_for_ship_type(names[index]), index + 1U);
        QCOMPARE(telemetry::radar_icon_id_for_ship_type("  fIgHtEr\t"), 6U);
        QCOMPARE(telemetry::radar_icon_id_for_ship_type("Modded Flagship"), 0U);
    }

    void installsFragmentedCatalogAtomically()
    {
        const auto ship = classRecord(77, 10, 6);
        const auto weapon = weaponRecord(77, 20, protocol::WeaponSubtype::Missile, 2'500'000U);
        QVERIFY(!ship.empty());
        QVERIFY(!weapon.empty());
        auto input = transaction(77, {ship, weapon});
        std::shared_ptr<const RadarManifestCatalog> catalog;
        QString error;
        QVERIFY2(buildRadarManifestCatalog(input, catalog, &error), qPrintable(error));
        QCOMPARE(catalog->manifestId, 77U);
        QCOMPARE(catalog->shipRadarIconIds.at(10), 6U);
        QCOMPARE(catalog->weapons.at(20).subtype, protocol::WeaponSubtype::Missile);
        QVERIFY(catalog->weapons.at(20).hasLock);
        QCOMPARE(catalog->weapons.at(20).nominalLockTimeUs, std::uint64_t{2'500'000U});

        const auto active = catalog;
        auto invalid = transaction(78, {classRecord(78, 10, 6), classRecord(78, 10, 7)});
        QVERIFY(!buildRadarManifestCatalog(invalid, catalog, &error));
        QCOMPARE(catalog, active);
        QCOMPARE(catalog->manifestId, 77U);
    }

    void producerSerializesOnlyKnownRadarIconAndFingerprintsIt()
    {
        const auto build = [](const char* typeName) {
            struct Result {
                std::uint32_t iconId = 0;
                bool hasWireIcon = false;
                protocol::Sha256Digest fingerprint{};
            } result;
            auto source = std::make_unique<telemetry::Phase2ManifestSource>();
            source->ship_classes[0].source_key = 41;
            source->ship_classes[0].name = "Ulysses";
            source->ship_classes[0].model_mass = 100.0F;
            source->ship_classes[0].effective_mass = 100.0F;
            source->ship_classes[0].ship_type_index = 9;
            source->ship_class_count = 1;
            source->referenced_ship_class_keys[0] = 41;
            source->referenced_ship_class_count = 1;
            source->auxiliary_entries[0].registry = telemetry::AuxiliaryRegistry::ShipType;
            source->auxiliary_entries[0].engine_index = 9;
            source->auxiliary_entries[0].name = typeName;
            source->auxiliary_entry_count = 1;
            std::vector<std::uint8_t> arena(2U * protocol::MaxTransactionSize);
            telemetry::Phase2ManifestStorage storage({arena.data(), arena.size()});
            auto slot = std::make_unique<telemetry::Phase2ManifestSlot>(storage);
            if (slot->rebuild(*source) != telemetry::Phase2ManifestError::None) return result;
            const auto& candidate = slot->staged_candidate();
            result.iconId = candidate.class_records[0].radar_icon_id;
            result.fingerprint = candidate.catalog_fingerprint;
            protocol::RecordEnvelopeIterator iterator(
                candidate.parts[0].records, candidate.parts[0].record_count,
                protocol::RecordFlagPolicy::RequireNone);
            protocol::RecordEnvelopeView record;
            bool hasValue = false;
            if (iterator.next(record, hasValue) == protocol::ValidationError::None && hasValue) {
                protocol::ClassManifestRadarMetadata metadata;
                if (protocol::decode_class_manifest_radar_metadata(
                        record, protocol::VersionMinorV1_1, metadata) ==
                    protocol::ValidationError::None) {
                    result.hasWireIcon = metadata.has_radar_icon;
                }
            }
            return result;
        };

        const auto fighter = build("Fighter");
        const auto bomber = build("Bomber");
        const auto modded = build("Modded Flagship");
        QCOMPARE(fighter.iconId, 6U);
        QVERIFY(fighter.hasWireIcon);
        QCOMPARE(bomber.iconId, 7U);
        QVERIFY(bomber.hasWireIcon);
        QCOMPARE(modded.iconId, 0U);
        QVERIFY(!modded.hasWireIcon);
        QVERIFY(fighter.fingerprint != bomber.fingerprint);
        QVERIFY(fighter.fingerprint != modded.fingerprint);
    }

    void replacesGenerationAndPreservesUnknownIds()
    {
        std::shared_ptr<const RadarManifestCatalog> catalog;
        QString error;
        auto first = transaction(12, {classRecord(12, 1, 6)});
        QVERIFY(buildRadarManifestCatalog(first, catalog, &error));
        auto replacement = transaction(13, {classRecord(13, 1, 9001)});
        QVERIFY(buildRadarManifestCatalog(replacement, catalog, &error));
        QCOMPARE(catalog->manifestId, 13U);
        QCOMPARE(catalog->shipRadarIconIds.at(1), 9001U);
    }

    void rejectsMissingOrMismatchedManifest()
    {
        std::shared_ptr<const RadarManifestCatalog> catalog;
        QString error;
        protocol::CompletedTransaction missing;
        QVERIFY(!buildRadarManifestCatalog(missing, catalog, &error));
        auto mismatch = transaction(42, {classRecord(41, 1, 6)});
        QVERIFY(!buildRadarManifestCatalog(mismatch, catalog, &error));
        QVERIFY(!catalog);
    }
};

QTEST_APPLESS_MAIN(RadarManifestTests)
#include "radar_manifest_tests.moc"
