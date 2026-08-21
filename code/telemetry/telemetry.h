#pragma once

namespace telemetry {

void initialize() noexcept;
void mission_pause_changed(bool paused) noexcept;
void modal_loop_update() noexcept;

} // namespace telemetry
