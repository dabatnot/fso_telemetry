#pragma once

#include "telemetry/protocol/telemetry_protocol_constants.h"

#include <QString>

#include <cstdint>
#include <memory>
#include <unordered_map>

namespace telemetry::protocol {
struct CompletedTransaction;
}

namespace simpit::radar {

struct RadarWeaponMetadata {
    telemetry::protocol::WeaponSubtype subtype = telemetry::protocol::WeaponSubtype::Unknown;
    std::uint64_t flags = 0;
};

struct RadarManifestCatalog final {
    std::uint32_t manifestId = 0;
    std::unordered_map<std::uint32_t, std::uint32_t> shipRadarIconIds;
    std::unordered_map<std::uint32_t, RadarWeaponMetadata> weapons;
};

bool buildRadarManifestCatalog(
    const telemetry::protocol::CompletedTransaction& transaction,
    std::shared_ptr<const RadarManifestCatalog>& output,
    QString* error = nullptr);

} // namespace simpit::radar
