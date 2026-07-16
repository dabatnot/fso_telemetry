#include "telemetry/telemetry.h"

#include <gtest/gtest.h>

#include <type_traits>

namespace {

using InitializeFunction = void (*)() noexcept;

static_assert(std::is_same_v<decltype(&telemetry::initialize), InitializeFunction>,
	"The engine seam must remain exactly void telemetry::initialize() noexcept.");

TEST(TelemetryProducerInitialize, PublicApiIsNoexceptAndSafeToCallTwice) {
	EXPECT_NO_THROW(telemetry::initialize());
	EXPECT_NO_THROW(telemetry::initialize());
}

} // namespace
