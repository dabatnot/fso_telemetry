#pragma once

#include "telemetry/native_session_runtime.h"
#include "telemetry/runtime_adapter.h"

#include <type_traits>
#include <utility>

namespace telemetry::detail {

// Production D4 owns the definition.  Keeping this declaration incomplete lets
// the test-first executable report one focused RED failure instead of failing to
// compile while the tracker-approved seam is absent.
class RuntimeAdapterPlayerTestAccess;

template <typename Access, typename = void>
struct HasRuntimeAdapterPlayerTestAccess : std::false_type {};

template <typename Access>
struct HasRuntimeAdapterPlayerTestAccess<Access,
	std::void_t<decltype(Access::service_tick(std::declval<NativeSessionRuntime*>(),
				std::declval<const RuntimeTickContext&>())),
		decltype(Access::map_tick_status(std::declval<NativeSessionTickStatus>())),
		decltype(Access::stop_collection(std::declval<NativeSessionRuntime*>())),
		decltype(Access::invalidate_mission_state_and_entities(std::declval<NativeSessionRuntime*>())),
		decltype(Access::close_sessions_and_stores(std::declval<NativeSessionRuntime*>())),
		decltype(Access::stop_transport(std::declval<NativeSessionRuntime*>()))>>
	: std::bool_constant<
		  std::is_same_v<decltype(Access::service_tick(std::declval<NativeSessionRuntime*>(),
							 std::declval<const RuntimeTickContext&>())),
			  RuntimeTickStatus> &&
		  std::is_same_v<decltype(Access::map_tick_status(std::declval<NativeSessionTickStatus>())),
			  RuntimeTickStatus> &&
		  std::is_same_v<decltype(Access::stop_collection(std::declval<NativeSessionRuntime*>())), void> &&
		  std::is_same_v<decltype(Access::invalidate_mission_state_and_entities(
			  std::declval<NativeSessionRuntime*>())), void> &&
		  std::is_same_v<decltype(Access::close_sessions_and_stores(std::declval<NativeSessionRuntime*>())),
			  void> &&
		  std::is_same_v<decltype(Access::stop_transport(std::declval<NativeSessionRuntime*>())), void> &&
		  noexcept(Access::service_tick(std::declval<NativeSessionRuntime*>(),
			  std::declval<const RuntimeTickContext&>())) &&
		  noexcept(Access::map_tick_status(std::declval<NativeSessionTickStatus>())) &&
		  noexcept(Access::stop_collection(std::declval<NativeSessionRuntime*>())) &&
		  noexcept(Access::invalidate_mission_state_and_entities(std::declval<NativeSessionRuntime*>())) &&
		  noexcept(Access::close_sessions_and_stores(std::declval<NativeSessionRuntime*>())) &&
		  noexcept(Access::stop_transport(std::declval<NativeSessionRuntime*>()))> {};

template <typename Runtime, typename = void>
struct HasLegacyNativeOneArgumentTick : std::false_type {};

template <typename Runtime>
struct HasLegacyNativeOneArgumentTick<Runtime,
	std::void_t<decltype(std::declval<Runtime&>().service_tick(
		std::declval<const NativeSessionTickContext&>()))>> : std::true_type {};

template <typename TickStatus, typename TerminalReason, typename = void>
struct HasD4ClosedRuntimeEnums : std::false_type {};

template <typename TickStatus, typename TerminalReason>
struct HasD4ClosedRuntimeEnums<TickStatus,
	TerminalReason,
	std::void_t<decltype(TickStatus::PermanentCaptureFailure),
		decltype(TerminalReason::CaptureFailure)>>
	: std::bool_constant<static_cast<std::uint8_t>(TickStatus::Complete) == 0U &&
		  static_cast<std::uint8_t>(TickStatus::Unavailable) == 1U &&
		  static_cast<std::uint8_t>(TickStatus::PermanentTransportFailure) == 2U &&
		  static_cast<std::uint8_t>(TickStatus::PermanentCaptureFailure) == 3U &&
		  static_cast<std::uint8_t>(TerminalReason::MissionGenerationOverflow) == 13U &&
		  static_cast<std::uint8_t>(TerminalReason::CaptureFailure) == 14U> {};

template <typename Access, typename = void>
struct HasRuntimeAdapterDiagnosticContract : std::false_type {};

template <typename Access>
struct HasRuntimeAdapterDiagnosticContract<Access,
	std::void_t<decltype(Access::startup_diagnostic_message(
		std::declval<RuntimeTerminalReason>()))>>
	: std::bool_constant<std::is_same_v<decltype(Access::startup_diagnostic_message(
											  std::declval<RuntimeTerminalReason>())),
			const char*> &&
		  noexcept(Access::startup_diagnostic_message(std::declval<RuntimeTerminalReason>()))> {};

class RuntimeAdapterPlayerPublicProbe final {
  public:
	static constexpr bool contract_available() noexcept
	{
		return HasRuntimeAdapterPlayerTestAccess<RuntimeAdapterPlayerTestAccess>::value;
	}
	static constexpr bool diagnostic_contract_available() noexcept
	{
		return HasRuntimeAdapterDiagnosticContract<RuntimeAdapterPlayerTestAccess>::value;
	}

	static RuntimeTickStatus service_tick(NativeSessionRuntime* runtime,
		const RuntimeTickContext& context) noexcept
	{
		return service_tick_impl<RuntimeAdapterPlayerTestAccess>(runtime, context);
	}
	static RuntimeTickStatus map_tick_status(NativeSessionTickStatus status) noexcept
	{
		return map_impl<RuntimeAdapterPlayerTestAccess>(status);
	}
	static void stop_collection(NativeSessionRuntime* runtime) noexcept
	{
		stop_impl<RuntimeAdapterPlayerTestAccess>(runtime);
	}
	static void invalidate_mission_state_and_entities(NativeSessionRuntime* runtime) noexcept
	{
		invalidate_impl<RuntimeAdapterPlayerTestAccess>(runtime);
	}
	static void close_sessions_and_stores(NativeSessionRuntime* runtime) noexcept
	{
		close_impl<RuntimeAdapterPlayerTestAccess>(runtime);
	}
	static void stop_transport(NativeSessionRuntime* runtime) noexcept
	{
		transport_impl<RuntimeAdapterPlayerTestAccess>(runtime);
	}
	static const char* startup_diagnostic_message(RuntimeTerminalReason reason) noexcept
	{
		return diagnostic_impl<RuntimeAdapterPlayerTestAccess>(reason);
	}

  private:
	template <typename Access>
	static RuntimeTickStatus service_tick_impl(NativeSessionRuntime* runtime,
		const RuntimeTickContext& context) noexcept
	{
		if constexpr (HasRuntimeAdapterPlayerTestAccess<Access>::value) {
			return Access::service_tick(runtime, context);
		}
		return RuntimeTickStatus::Unavailable;
	}
	template <typename Access>
	static RuntimeTickStatus map_impl(NativeSessionTickStatus status) noexcept
	{
		if constexpr (HasRuntimeAdapterPlayerTestAccess<Access>::value) {
			return Access::map_tick_status(status);
		}
		return RuntimeTickStatus::Unavailable;
	}
	template <typename Access>
	static void stop_impl(NativeSessionRuntime* runtime) noexcept
	{
		if constexpr (HasRuntimeAdapterPlayerTestAccess<Access>::value) Access::stop_collection(runtime);
	}
	template <typename Access>
	static void invalidate_impl(NativeSessionRuntime* runtime) noexcept
	{
		if constexpr (HasRuntimeAdapterPlayerTestAccess<Access>::value) {
			Access::invalidate_mission_state_and_entities(runtime);
		}
	}
	template <typename Access>
	static void close_impl(NativeSessionRuntime* runtime) noexcept
	{
		if constexpr (HasRuntimeAdapterPlayerTestAccess<Access>::value) Access::close_sessions_and_stores(runtime);
	}
	template <typename Access>
	static void transport_impl(NativeSessionRuntime* runtime) noexcept
	{
		if constexpr (HasRuntimeAdapterPlayerTestAccess<Access>::value) Access::stop_transport(runtime);
	}
	template <typename Access>
	static const char* diagnostic_impl(RuntimeTerminalReason reason) noexcept
	{
		if constexpr (HasRuntimeAdapterDiagnosticContract<Access>::value) {
			return Access::startup_diagnostic_message(reason);
		}
		return nullptr;
	}
};

} // namespace telemetry::detail
