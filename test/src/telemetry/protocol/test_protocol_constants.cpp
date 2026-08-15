#include "telemetry/protocol/telemetry_protocol_constants.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <type_traits>

namespace {

using namespace telemetry::protocol;

template <typename Enum, std::size_t Size>
void expect_sequential_registry(const std::array<Enum, Size>& values) {
	for (std::size_t i = 0; i < values.size(); ++i) {
		EXPECT_EQ(i, static_cast<std::uint64_t>(values[i])) << "registry index " << i;
	}
}

static_assert(std::is_same<std::underlying_type<MessageType>::type, std::uint8_t>::value,
	"MessageType must remain a u8");
static_assert(std::is_same<std::underlying_type<RecordType>::type, std::uint16_t>::value,
	"RecordType must remain a u16");
static_assert(std::is_same<std::underlying_type<ValidationError>::type, std::uint8_t>::value,
	"ValidationError must remain a u8");

TEST(TelemetryProtocolConstants, FreezesWireIdentityAndResourceBounds) {
	EXPECT_EQ(0x4c545346U, Magic);
	EXPECT_EQ(1U, VersionMajor);
	EXPECT_EQ(0U, VersionMinor);
	EXPECT_EQ(68U, HeaderSizeV1);
	EXPECT_EQ(1200U, MaxDatagramSize);
	EXPECT_EQ(1132U, MaxFragmentPayload);
	EXPECT_EQ(1'048'576U, MaxStateMessageSize);
	EXPECT_EQ(2'097'152U, MaxVideoMessageSize);
	EXPECT_EQ(1024U, MaxStateFragments);
	EXPECT_EQ(2048U, MaxVideoFragments);
	EXPECT_EQ(4U, MaxStateReassembliesPerClient);
	EXPECT_EQ(3U, MaxVideoReassembliesPerClient);
	EXPECT_EQ(4'194'304U, MaxStateReassemblyBytesPerClient);
	EXPECT_EQ(6'291'456U, MaxVideoReassemblyBytesPerClient);
	EXPECT_EQ(1'048'576U, MaxStatePartSize);
	EXPECT_EQ(16'777'216U, MaxTransactionSize);
	EXPECT_EQ(64U, MaxTransactionParts);
	EXPECT_EQ(2U, MaxCandidateTransactionsPerClient);
	EXPECT_EQ(33'554'432U, MaxCandidateTransactionBytesPerClient);
	EXPECT_EQ(10'000U, TransactionAssemblyTimeoutMs);
}

TEST(TelemetryProtocolConstants, FreezesMessageTypeRegistry) {
	const std::array<MessageType, 21> values{
		MessageType::Invalid,
		MessageType::Discovery,
		MessageType::Hello,
		MessageType::Welcome,
		MessageType::SessionBegin,
		MessageType::Manifest,
		MessageType::FullSnapshot,
		MessageType::Delta,
		MessageType::EventBatch,
		MessageType::Heartbeat,
		MessageType::Ack,
		MessageType::Nack,
		MessageType::ResyncRequest,
		MessageType::SessionEnd,
		MessageType::TargetVideoSubscribe,
		MessageType::TargetVideoConfig,
		MessageType::TargetVideoFrame,
		MessageType::TargetVideoKeyframeRequest,
		MessageType::TargetVideoStop,
		MessageType::TargetVideoStats,
		MessageType::CapabilityUpdate,
	};
	expect_sequential_registry(values);

	EXPECT_FALSE(is_known_message_type(MessageType::Invalid));
	for (std::size_t i = 1; i < values.size(); ++i) {
		EXPECT_TRUE(is_known_message_type(values[i])) << "message type " << i;
	}
	EXPECT_FALSE(is_known_message_type(static_cast<MessageType>(21)));
}

TEST(TelemetryProtocolConstants, FreezesMessageFlagsAndPerClassLimits) {
	EXPECT_EQ(0x00U, MessageFlagNone);
	EXPECT_EQ(0x01U, MessageFlagFragmented);
	EXPECT_EQ(0x02U, MessageFlagAckRequired);
	EXPECT_EQ(0x04U, MessageFlagKeyframe);
	EXPECT_EQ(0x08U, MessageFlagVideoIdr);
	EXPECT_EQ(0x10U, MessageFlagRetransmission);
	EXPECT_EQ(0x1fU, KnownMessageFlags);

	EXPECT_TRUE(is_video_message(MessageType::TargetVideoFrame));
	EXPECT_FALSE(is_video_message(MessageType::TargetVideoConfig));
	EXPECT_EQ(MaxVideoMessageSize, max_message_size(MessageType::TargetVideoFrame));
	EXPECT_EQ(MaxVideoFragments, max_fragment_count(MessageType::TargetVideoFrame));
	EXPECT_EQ(MaxStateMessageSize, max_message_size(MessageType::FullSnapshot));
	EXPECT_EQ(MaxStateFragments, max_fragment_count(MessageType::FullSnapshot));
	EXPECT_EQ(0U, max_message_size(MessageType::Invalid));
	EXPECT_EQ(0U, max_fragment_count(MessageType::Invalid));
	const auto unknown = static_cast<MessageType>(0xffU);
	EXPECT_EQ(0U, max_message_size(unknown));
	EXPECT_EQ(0U, max_fragment_count(unknown));
}

TEST(TelemetryProtocolConstants, FreezesRecordTypeRegistry) {
	const std::array<RecordType, 30> values{
		RecordType::Invalid,
		RecordType::SessionState,
		RecordType::MissionState,
		RecordType::ClassManifest,
		RecordType::WeaponManifest,
		RecordType::EntityLifecycle,
		RecordType::ShipIdentity,
		RecordType::FlightState,
		RecordType::ControlState,
		RecordType::DamageState,
		RecordType::ShieldState,
		RecordType::SubsystemState,
		RecordType::EnergyState,
		RecordType::PropulsionState,
		RecordType::WeaponState,
		RecordType::LockState,
		RecordType::TargetState,
		RecordType::RadarState,
		RecordType::RadarContacts,
		RecordType::ThreatState,
		RecordType::CargoScanState,
		RecordType::DockingState,
		RecordType::SupportState,
		RecordType::NavigationState,
		RecordType::EffectState,
		RecordType::CommAssetManifest,
		RecordType::CommViewState,
		RecordType::CommViewEvent,
		RecordType::Events,
		RecordType::HudAlertState,
	};
	expect_sequential_registry(values);

	EXPECT_EQ(0x00U, RecordFlagNone);
	EXPECT_EQ(0x01U, RecordFlagCreate);
	EXPECT_EQ(0x02U, RecordFlagDelete);
	EXPECT_EQ(0x04U, RecordFlagPartial);
	EXPECT_EQ(0x07U, KnownRecordFlags);
}

TEST(TelemetryProtocolConstants, FreezesPhase3HudAlertContract)
{
	EXPECT_EQ(29U, static_cast<std::uint16_t>(RecordType::HudAlertState));
	EXPECT_EQ(30U, FirstReservedRecordType);
	EXPECT_EQ(0x1ULL, HudAlertStatePresenceFlagActiveWarning);
	EXPECT_EQ(0U,
		static_cast<std::uint8_t>(HudAlertMissileLockState::None));
	EXPECT_EQ(1U,
		static_cast<std::uint8_t>(HudAlertMissileLockState::Attempt));
	EXPECT_EQ(2U,
		static_cast<std::uint8_t>(HudAlertMissileLockState::Acquired));
	EXPECT_EQ(1U, static_cast<std::uint8_t>(HudAlertWarningKind::Launch));
	EXPECT_EQ(7U, static_cast<std::uint8_t>(HudAlertWarningKind::Other));
}

TEST(TelemetryProtocolConstants, FreezesPhase3VersionFourRadarVisualContract) {
	EXPECT_EQ(0U, static_cast<std::uint8_t>(RadarBlipType::JumpNode));
	EXPECT_EQ(1U, static_cast<std::uint8_t>(RadarBlipType::NavbuoyCargo));
	EXPECT_EQ(2U, static_cast<std::uint8_t>(RadarBlipType::Bomb));
	EXPECT_EQ(3U, static_cast<std::uint8_t>(RadarBlipType::WarpingShip));
	EXPECT_EQ(4U, static_cast<std::uint8_t>(RadarBlipType::TaggedShip));
	EXPECT_EQ(5U, static_cast<std::uint8_t>(RadarBlipType::NormalShip));
	EXPECT_EQ(0x0000000000010000ULL, TargetStatePresenceFlagHudTargetColor);
	EXPECT_EQ(0x0000000000020000ULL, TargetStatePresenceFlagHudTargetSubsystemLabel);
	EXPECT_EQ(0x0000000000040000ULL, TargetStatePresenceFlagHudLockSubsystemLabel);
	EXPECT_EQ(0x0000000000080000ULL, TargetStatePresenceFlagHudTargetStrength);
	EXPECT_EQ(0x00000000000fffffULL, KnownTargetStatePresenceFlags);
	EXPECT_EQ(0x0000000000000080ULL, RadarContactsPresenceFlagRadarVisual);
	EXPECT_EQ(0x00000000000000ffULL, KnownRadarContactsPresenceFlags);
}

TEST(TelemetryProtocolConstants, FreezesCapabilitiesAndControlRegistries) {
	EXPECT_EQ(0x00ULL, CapabilityNone);
	EXPECT_EQ(0x01ULL, CapabilityCommViewLocalAssets);
	EXPECT_EQ(0x02ULL, CapabilityCommViewAuthoritativeSource);
	EXPECT_EQ(0x04ULL, CapabilityTargetVideoH264);
	EXPECT_EQ(0x08ULL, CapabilityTargetVideoRemoteRender);
	EXPECT_EQ(0x10ULL, CapabilityDynamicUpdate);
	EXPECT_EQ(0x1fULL, KnownCapabilities);

	EXPECT_EQ(0U, static_cast<std::uint8_t>(VisibilityMode::Cockpit));
	EXPECT_EQ(0x01U, static_cast<std::uint8_t>(AckFlag::Validated));
	EXPECT_EQ(0x02U, static_cast<std::uint8_t>(AckFlag::Applied));

	const std::array<NackReason, 9> nack_reasons{
		NackReason::Invalid,
		NackReason::MissingFragments,
		NackReason::BadMessageCrc,
		NackReason::StaleBaseline,
		NackReason::BadFragmentLayout,
		NackReason::ResourceLimit,
		NackReason::UnsupportedMessage,
		NackReason::SemanticValidationFailed,
		NackReason::DeadlineExpired,
	};
	expect_sequential_registry(nack_reasons);

	const std::array<ResyncReason, 6> resync_reasons{
		ResyncReason::Invalid,
		ResyncReason::UnknownBaseline,
		ResyncReason::ValidationFailed,
		ResyncReason::ReassemblyTimeout,
		ResyncReason::SessionStale,
		ResyncReason::Manual,
	};
	expect_sequential_registry(resync_reasons);
}

TEST(TelemetryProtocolConstants, FreezesValidationErrorRegistry) {
	const std::array<ValidationError, 48> values{
		ValidationError::None,
		ValidationError::SourceNotAllowed,
		ValidationError::DatagramTooShort,
		ValidationError::DatagramTooLarge,
		ValidationError::BadMagic,
		ValidationError::UnsupportedMajor,
		ValidationError::UnsupportedMinor,
		ValidationError::BadHeaderSize,
		ValidationError::ReservedHeaderFlag,
		ValidationError::BadDatagramLength,
		ValidationError::BadDatagramCrc,
		ValidationError::UnknownMessageType,
		ValidationError::WrongDirection,
		ValidationError::SessionMismatch,
		ValidationError::EndpointMismatch,
		ValidationError::MessageTooLarge,
		ValidationError::BadFragmentCount,
		ValidationError::BadFragmentIndex,
		ValidationError::BadFragmentOffset,
		ValidationError::BadFragmentSlice,
		ValidationError::ReassemblyQuota,
		ValidationError::InconsistentFragment,
		ValidationError::ReassemblyTimeout,
		ValidationError::BadMessageCrc,
		ValidationError::TruncatedPayload,
		ValidationError::TrailingBytes,
		ValidationError::UnknownRequiredRecord,
		ValidationError::UnsupportedRecordVersion,
		ValidationError::BadRecordLength,
		ValidationError::DuplicateRecord,
		ValidationError::DuplicateItemKey,
		ValidationError::InvalidUtf8,
		ValidationError::StringTooLong,
		ValidationError::NonFiniteFloat,
		ValidationError::OutOfRange,
		ValidationError::UnknownEnum,
		ValidationError::ReservedFlag,
		ValidationError::InvalidAbsence,
		ValidationError::UnknownEntity,
		ValidationError::StaleBaseline,
		ValidationError::StaleGeneration,
		ValidationError::MissingManifest,
		ValidationError::CapabilityNotNegotiated,
		ValidationError::VisibilityViolation,
		ValidationError::InvalidStateTransition,
		ValidationError::RateLimited,
		ValidationError::ResourceLimit,
		ValidationError::InternalSerializationError,
	};
	expect_sequential_registry(values);
}

} // namespace
