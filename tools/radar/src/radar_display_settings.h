#pragma once

namespace simpit::radar {

struct RadarDisplaySettings final {
    bool targetCallout = true;
    bool targetStrength = true;
    bool lead = true;
    bool lock = true;
    bool subsystems = true;
    bool edgeThreats = true;
    bool sensorEffects = true;
    bool motionVectors = false;
    bool trails = false;

    friend bool operator==(const RadarDisplaySettings&, const RadarDisplaySettings&) = default;
};

} // namespace simpit::radar
