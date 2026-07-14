#pragma once

#include <cstddef>
#include <cstdint>

namespace telemetry::protocol {

constexpr std::uint32_t Magic = 0x4c545346U;
constexpr std::uint8_t VersionMajor = 1;
constexpr std::uint8_t VersionMinor = 0;
constexpr std::size_t HeaderSizeV1 = 68;
constexpr std::size_t MaxDatagramSize = 1200;
constexpr std::size_t MaxFragmentPayload = MaxDatagramSize - HeaderSizeV1;
constexpr std::size_t MaxStateMessageSize = 1'048'576;
constexpr std::size_t MaxVideoMessageSize = 2'097'152;
constexpr std::size_t MaxStateFragments = 1024;
constexpr std::size_t MaxVideoFragments = 2048;
constexpr std::size_t MaxStateReassembliesPerClient = 4;
constexpr std::size_t MaxVideoReassembliesPerClient = 3;
constexpr std::size_t MaxStateReassemblyBytesPerClient = 4'194'304;
constexpr std::size_t MaxVideoReassemblyBytesPerClient = 6'291'456;
constexpr std::size_t MaxStatePartSize = 1'048'576;
constexpr std::size_t MaxTransactionSize = 16'777'216;
constexpr std::size_t MaxTransactionParts = 64;
constexpr std::size_t MaxCandidateTransactionBytesPerClient = 33'554'432;

enum class MessageType : std::uint8_t {
	Invalid = 0,
	Discovery = 1,
	Hello = 2,
	Welcome = 3,
	SessionBegin = 4,
	Manifest = 5,
	FullSnapshot = 6,
	Delta = 7,
	EventBatch = 8,
	Heartbeat = 9,
	Ack = 10,
	Nack = 11,
	ResyncRequest = 12,
	SessionEnd = 13,
	TargetVideoSubscribe = 14,
	TargetVideoConfig = 15,
	TargetVideoFrame = 16,
	TargetVideoKeyframeRequest = 17,
	TargetVideoStop = 18,
	TargetVideoStats = 19,
	CapabilityUpdate = 20,
};

enum MessageFlag : std::uint8_t {
	MessageFlagNone = 0,
	MessageFlagFragmented = 0x01,
	MessageFlagAckRequired = 0x02,
	MessageFlagKeyframe = 0x04,
	MessageFlagVideoIdr = 0x08,
	MessageFlagRetransmission = 0x10,
};
constexpr std::uint8_t KnownMessageFlags = 0x1f;

enum class RecordType : std::uint16_t {
	Invalid = 0,
	SessionState = 1,
	MissionState = 2,
	ClassManifest = 3,
	WeaponManifest = 4,
	EntityLifecycle = 5,
	ShipIdentity = 6,
	FlightState = 7,
	ControlState = 8,
	DamageState = 9,
	ShieldState = 10,
	SubsystemState = 11,
	EnergyState = 12,
	PropulsionState = 13,
	WeaponState = 14,
	LockState = 15,
	TargetState = 16,
	RadarState = 17,
	RadarContacts = 18,
	ThreatState = 19,
	CargoScanState = 20,
	DockingState = 21,
	SupportState = 22,
	NavigationState = 23,
	EffectState = 24,
	CommAssetManifest = 25,
	CommViewState = 26,
	CommViewEvent = 27,
	Events = 28,
};

enum RecordFlag : std::uint8_t {
	RecordFlagNone = 0,
	RecordFlagCreate = 0x01,
	RecordFlagDelete = 0x02,
	RecordFlagPartial = 0x04,
};
constexpr std::uint8_t KnownRecordFlags = 0x07;

enum Capability : std::uint64_t {
	CapabilityNone = 0,
	CapabilityCommViewLocalAssets = 0x0000000000000001ULL,
	CapabilityCommViewAuthoritativeSource = 0x0000000000000002ULL,
	CapabilityTargetVideoH264 = 0x0000000000000004ULL,
	CapabilityTargetVideoRemoteRender = 0x0000000000000008ULL,
	CapabilityDynamicUpdate = 0x0000000000000010ULL,
};
constexpr std::uint64_t KnownCapabilities = 0x1fULL;

enum class ValidationError : std::uint8_t {
	None = 0,
	SourceNotAllowed = 1,
	DatagramTooShort = 2,
	DatagramTooLarge = 3,
	BadMagic = 4,
	UnsupportedMajor = 5,
	UnsupportedMinor = 6,
	BadHeaderSize = 7,
	ReservedHeaderFlag = 8,
	BadDatagramLength = 9,
	BadDatagramCrc = 10,
	UnknownMessageType = 11,
	WrongDirection = 12,
	SessionMismatch = 13,
	EndpointMismatch = 14,
	MessageTooLarge = 15,
	BadFragmentCount = 16,
	BadFragmentIndex = 17,
	BadFragmentOffset = 18,
	BadFragmentSlice = 19,
	ReassemblyQuota = 20,
	InconsistentFragment = 21,
	ReassemblyTimeout = 22,
	BadMessageCrc = 23,
	TruncatedPayload = 24,
	TrailingBytes = 25,
	UnknownRequiredRecord = 26,
	UnsupportedRecordVersion = 27,
	BadRecordLength = 28,
	DuplicateRecord = 29,
	DuplicateItemKey = 30,
	InvalidUtf8 = 31,
	StringTooLong = 32,
	NonFiniteFloat = 33,
	OutOfRange = 34,
	UnknownEnum = 35,
	ReservedFlag = 36,
	InvalidAbsence = 37,
	UnknownEntity = 38,
	StaleBaseline = 39,
	StaleGeneration = 40,
	MissingManifest = 41,
	CapabilityNotNegotiated = 42,
	VisibilityViolation = 43,
	InvalidStateTransition = 44,
	RateLimited = 45,
	ResourceLimit = 46,
	InternalSerializationError = 47,
};

enum class VisibilityMode : std::uint8_t {
	Cockpit = 0,
	TrustedFullState = 1,
};

enum class AckFlag : std::uint8_t {
	Validated = 0x01,
	Applied = 0x02,
};

enum class NackReason : std::uint8_t {
	Invalid = 0,
	MissingFragments = 1,
	BadMessageCrc = 2,
	StaleBaseline = 3,
	BadFragmentLayout = 4,
	ResourceLimit = 5,
	UnsupportedMessage = 6,
	SemanticValidationFailed = 7,
	DeadlineExpired = 8,
};

enum class ResyncReason : std::uint8_t {
	Invalid = 0,
	UnknownBaseline = 1,
	ValidationFailed = 2,
	ReassemblyTimeout = 3,
	SessionStale = 4,
	Manual = 5,
};

constexpr bool is_known_message_type(MessageType type) noexcept {
	return type >= MessageType::Discovery && type <= MessageType::CapabilityUpdate;
}

constexpr bool is_video_message(MessageType type) noexcept {
	return type == MessageType::TargetVideoFrame;
}

constexpr std::size_t max_message_size(MessageType type) noexcept {
	return is_video_message(type) ? MaxVideoMessageSize : MaxStateMessageSize;
}

constexpr std::size_t max_fragment_count(MessageType type) noexcept {
	return is_video_message(type) ? MaxVideoFragments : MaxStateFragments;
}

} // namespace telemetry::protocol
