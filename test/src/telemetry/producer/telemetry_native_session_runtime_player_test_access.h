#pragma once

#include "telemetry_session_controller_player_test_access.h"

#include "telemetry/engine_adapter.h"
#include "telemetry/native_session_runtime.h"
#include "telemetry/session_controller.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace telemetry::detail {

// Test-side declarations let the consumer name the exact public D3 types while
// the RED still compiles before their production definitions exist.
enum class NativePlayerCaptureStatus : std::uint8_t;
struct CurrentPlayerCapture;

struct NativePlayerCaptureProbe {
	bool available = false;
	CaptureResult result{};
	PlayerObservationDto observation{};
};

// Test-only friend seam. All production state remains owned by
// NativeSessionRuntime; this class neither adds a production symbol nor
// duplicates controller mutation logic.
class NativeSessionRuntimePlayerTestAccess final {
  private:
	template <typename Runtime, typename = void>
	struct HasPrivateContract : std::false_type {};

	template <typename Runtime>
	struct HasPrivateContract<Runtime,
		std::void_t<decltype(std::declval<const Runtime&>().m_controller.slot(std::declval<std::size_t>())),
			decltype(std::declval<Runtime&>().apply_collected_player_capture(std::declval<const CaptureResult&>(),
				std::declval<const PlayerObservationDto&>()))>>
		: std::bool_constant<std::is_same_v<decltype(std::declval<Runtime&>().apply_collected_player_capture(
												std::declval<const CaptureResult&>(),
												std::declval<const PlayerObservationDto&>())),
								 NativeSessionTickStatus> &&
							 noexcept(std::declval<Runtime&>().apply_collected_player_capture(
								 std::declval<const CaptureResult&>(),
								 std::declval<const PlayerObservationDto&>()))> {};

	template <typename Runtime>
	static const SessionControllerSlot* slot_impl(const Runtime& runtime, std::size_t index, std::true_type) noexcept
	{
		if (!runtime.m_controller_ready)
			return nullptr;
		return &runtime.m_controller.slot(index);
	}

	template <typename Runtime>
	static const SessionControllerSlot* slot_impl(const Runtime&, std::size_t, std::false_type) noexcept
	{
		return nullptr;
	}

	template <typename Runtime>
	static bool seed_impl(Runtime& runtime, std::size_t index, std::uint64_t last_id, std::true_type) noexcept
	{
		return SessionControllerPlayerTestAccess::seed_last_allocated_entity_id(runtime.m_controller, index, last_id);
	}

	template <typename Runtime>
	static bool seed_impl(Runtime&, std::size_t, std::uint64_t, std::false_type) noexcept
	{
		return false;
	}

	template <typename Runtime>
	static NativeSessionTickStatus inject_impl(Runtime& runtime,
		const CaptureResult& result,
		const PlayerObservationDto& observation,
		std::true_type) noexcept
	{
		if (runtime.m_state != Runtime::State::Started || !runtime.m_controller_ready) {
			return NativeSessionTickStatus::Unavailable;
		}
		return runtime.apply_collected_player_capture(result, observation);
	}

	template <typename Runtime>
	static NativeSessionTickStatus
	inject_impl(Runtime&, const CaptureResult&, const PlayerObservationDto&, std::false_type) noexcept
	{
		return NativeSessionTickStatus::Unavailable;
	}

  public:
	static constexpr bool private_contract_available() noexcept
	{
		return HasPrivateContract<NativeSessionRuntime>::value;
	}

	static const SessionControllerSlot* slot(const NativeSessionRuntime& runtime, std::size_t index) noexcept
	{
		return slot_impl(runtime, index, HasPrivateContract<NativeSessionRuntime>{});
	}

	static bool
	seed_last_allocated_entity_id(NativeSessionRuntime& runtime, std::size_t index, std::uint64_t last_id) noexcept
	{
		return seed_impl(runtime, index, last_id, HasPrivateContract<NativeSessionRuntime>{});
	}

	static NativeSessionTickStatus inject_collected_player_capture(NativeSessionRuntime& runtime,
		const CaptureResult& result,
		const PlayerObservationDto& observation) noexcept
	{
		return inject_impl(runtime, result, observation, HasPrivateContract<NativeSessionRuntime>{});
	}
};

template <typename Runtime, typename = void>
struct HasNativePlayerCapturePublicContract : std::false_type {};

template <typename Status, typename TickStatus, typename Current, typename = void>
struct HasExactNativePlayerCaptureTypes : std::false_type {};

template <typename Status, typename TickStatus, typename Current>
struct HasExactNativePlayerCaptureTypes<Status,
	TickStatus,
	Current,
	std::void_t<decltype(Status::Unavailable),
		decltype(Status::Inactive),
		decltype(Status::NotDue),
		decltype(Status::CapturedValid),
		decltype(Status::CapturedNoPlayer),
		decltype(Status::CapturedInvalidSource),
		decltype(Status::CaptureInvariantFailure),
		decltype(Status::CadenceFailure),
		decltype(Status::Count),
		decltype(TickStatus::PermanentCaptureFailure),
		decltype(std::declval<Current&>().available),
		decltype(std::declval<Current&>().result),
		decltype(std::declval<Current&>().observation)>>
	: std::bool_constant<std::is_enum_v<Status> &&
		std::is_same_v<Status, NativePlayerCaptureStatus> &&
		std::is_same_v<Current, CurrentPlayerCapture> &&
		std::is_same_v<std::underlying_type_t<Status>, std::uint8_t> &&
		static_cast<std::uint8_t>(Status::Unavailable) == 0U &&
		static_cast<std::uint8_t>(Status::Inactive) == 1U &&
		static_cast<std::uint8_t>(Status::NotDue) == 2U &&
		static_cast<std::uint8_t>(Status::CapturedValid) == 3U &&
		static_cast<std::uint8_t>(Status::CapturedNoPlayer) == 4U &&
		static_cast<std::uint8_t>(Status::CapturedInvalidSource) == 5U &&
		static_cast<std::uint8_t>(Status::CaptureInvariantFailure) == 6U &&
		static_cast<std::uint8_t>(Status::CadenceFailure) == 7U &&
		static_cast<std::uint8_t>(Status::Count) == 8U &&
		static_cast<std::uint8_t>(TickStatus::PermanentCaptureFailure) == 3U &&
		std::is_same_v<decltype(Current::available), bool> &&
		std::is_same_v<decltype(Current::result), CaptureResult> &&
		std::is_same_v<decltype(Current::observation), PlayerObservationDto> &&
		std::is_nothrow_default_constructible_v<Current>> {};

template <typename Runtime>
struct HasNativePlayerCapturePublicContract<Runtime,
	std::void_t<decltype(std::declval<Runtime&>().service_tick(std::declval<const NativeSessionTickContext&>(),
					std::declval<const EngineReadView&>())),
		decltype(std::declval<const Runtime&>().last_player_capture_status()),
		decltype(std::declval<const Runtime&>().current_player_capture()),
		decltype(std::declval<const Runtime&>().last_player_materialization())>>
	: std::bool_constant<
		  HasExactNativePlayerCaptureTypes<
			  std::remove_cv_t<std::remove_reference_t<decltype(
				  std::declval<const Runtime&>().last_player_capture_status())>>,
			  NativeSessionTickStatus,
			  std::remove_cv_t<std::remove_reference_t<decltype(
				  std::declval<const Runtime&>().current_player_capture())>>>::value &&
		  std::is_same_v<decltype(std::declval<const Runtime&>().last_player_capture_status()),
			  std::remove_cv_t<std::remove_reference_t<decltype(
				  std::declval<const Runtime&>().last_player_capture_status())>>> &&
		  std::is_same_v<decltype(std::declval<Runtime&>().service_tick(std::declval<const NativeSessionTickContext&>(),
							 std::declval<const EngineReadView&>())),
			  NativeSessionTickStatus> &&
		  std::is_same_v<decltype(std::declval<const Runtime&>().current_player_capture()),
			  const std::remove_reference_t<decltype(
				  std::declval<const Runtime&>().current_player_capture())>&> &&
		  std::is_same_v<decltype(std::declval<const Runtime&>().last_player_materialization()),
			  const SessionPlayerMaterializationResult&> &&
		  noexcept(std::declval<Runtime&>().service_tick(std::declval<const NativeSessionTickContext&>(),
			  std::declval<const EngineReadView&>())) &&
		  noexcept(std::declval<const Runtime&>().last_player_capture_status()) &&
		  noexcept(std::declval<const Runtime&>().current_player_capture()) &&
		  noexcept(std::declval<const Runtime&>().last_player_materialization())> {};

class NativeSessionRuntimePlayerPublicProbe final {
  private:
	template <typename Runtime>
	static NativeSessionTickStatus service_impl(Runtime& runtime,
		const NativeSessionTickContext& context,
		const EngineReadView& view,
		std::true_type) noexcept
	{
		return runtime.service_tick(context, view);
	}

	template <typename Runtime>
	static NativeSessionTickStatus service_impl(Runtime& runtime,
		const NativeSessionTickContext& context,
		const EngineReadView&,
		std::false_type) noexcept
	{
		return runtime.service_tick(context);
	}

	template <typename Runtime>
	static std::uint8_t status_impl(const Runtime& runtime, std::true_type) noexcept
	{
		return static_cast<std::uint8_t>(runtime.last_player_capture_status());
	}

	template <typename Runtime>
	static std::uint8_t status_impl(const Runtime&, std::false_type) noexcept
	{
		return 0U;
	}

	template <typename Runtime>
	static NativePlayerCaptureProbe current_impl(const Runtime& runtime, std::true_type) noexcept
	{
		const auto& current = runtime.current_player_capture();
		return {current.available, current.result, current.observation};
	}

	template <typename Runtime>
	static NativePlayerCaptureProbe current_impl(const Runtime&, std::false_type) noexcept
	{
		return {};
	}

	template <typename Runtime>
	static NativePlayerCaptureProbe declared_default_impl(std::true_type) noexcept
	{
		using DeclaredCurrent = std::remove_cv_t<std::remove_reference_t<decltype(
			std::declval<const Runtime&>().current_player_capture())>>;
		const DeclaredCurrent current{};
		return {current.available, current.result, current.observation};
	}

	template <typename Runtime>
	static NativePlayerCaptureProbe declared_default_impl(std::false_type) noexcept
	{
		return {};
	}

	template <typename Runtime>
	static SessionPlayerMaterializationResult materialization_impl(const Runtime& runtime, std::true_type) noexcept
	{
		return runtime.last_player_materialization();
	}

	template <typename Runtime>
	static SessionPlayerMaterializationResult materialization_impl(const Runtime&, std::false_type) noexcept
	{
		return {};
	}

  public:
	static constexpr bool public_contract_available() noexcept
	{
		return HasNativePlayerCapturePublicContract<NativeSessionRuntime>::value;
	}

	static NativeSessionTickStatus service_tick(NativeSessionRuntime& runtime,
		const NativeSessionTickContext& context,
		const EngineReadView& view) noexcept
	{
		return service_impl(runtime, context, view, HasNativePlayerCapturePublicContract<NativeSessionRuntime>{});
	}

	static std::uint8_t last_status(const NativeSessionRuntime& runtime) noexcept
	{
		return status_impl(runtime, HasNativePlayerCapturePublicContract<NativeSessionRuntime>{});
	}

	static NativePlayerCaptureProbe current(const NativeSessionRuntime& runtime) noexcept
	{
		return current_impl(runtime, HasNativePlayerCapturePublicContract<NativeSessionRuntime>{});
	}

	static NativePlayerCaptureProbe declared_default_current() noexcept
	{
		return declared_default_impl<NativeSessionRuntime>(
			HasNativePlayerCapturePublicContract<NativeSessionRuntime>{});
	}

	static SessionPlayerMaterializationResult materialization(const NativeSessionRuntime& runtime) noexcept
	{
		return materialization_impl(runtime, HasNativePlayerCapturePublicContract<NativeSessionRuntime>{});
	}
};

} // namespace telemetry::detail
