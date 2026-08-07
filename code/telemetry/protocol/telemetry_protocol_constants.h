#pragma once

#include <cstddef>
#include <cstdint>

namespace telemetry::protocol {

constexpr std::uint32_t Magic = 0x4c545346U;
constexpr std::uint8_t VersionMajor = 1;
constexpr std::uint8_t VersionMinorV1_0 = 0;
constexpr std::uint8_t VersionMinorV1_1 = 1;
// FSTL 1.2 appends the authoritative standard-radar projection inputs to
// RADAR_CONTACTS.  Earlier minors retain their frozen record layout.
constexpr std::uint8_t VersionMinorV1_2 = 2;
// Keep the historical default pinned to the frozen FSTL 1.0 contract. Code
// which emits or validates a negotiated 1.1 session must opt in explicitly.
constexpr std::uint8_t VersionMinor = VersionMinorV1_0;
constexpr std::uint8_t LatestSupportedVersionMinor = VersionMinorV1_2;

struct ProtocolMinorRange {
	std::uint8_t minimum = VersionMinor;
	std::uint8_t maximum = VersionMinor;
};

constexpr ProtocolMinorRange FrozenV1_0MinorRange{VersionMinorV1_0, VersionMinorV1_0};
constexpr ProtocolMinorRange Phase1ProducerMinorRange{VersionMinorV1_1, VersionMinorV1_1};

enum class ProtocolMinorNegotiationResult : std::uint8_t {
	Selected = 0,
	NoIntersection = 1,
	InvalidRange = 2,
};
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
constexpr std::size_t MaxCandidateTransactionsPerClient = 2;
constexpr std::size_t MaxCandidateTransactionBytesPerClient = 33'554'432;
constexpr std::uint32_t TransactionAssemblyTimeoutMs = 10'000;

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
constexpr std::uint8_t FirstReservedMessageType = 21;

enum MessageFlag : std::uint8_t {
	MessageFlagNone = 0,
	MessageFlagFragmented = 0x01,
	MessageFlagAckRequired = 0x02,
	MessageFlagKeyframe = 0x04,
	MessageFlagVideoIdr = 0x08,
	MessageFlagRetransmission = 0x10,
};
constexpr std::uint8_t KnownMessageFlags = 0x1f;
constexpr std::uint8_t ReservedMessageFlags = 0xe0;

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
constexpr std::uint16_t FirstReservedRecordType = 29;

enum RecordFlag : std::uint8_t {
	RecordFlagNone = 0,
	RecordFlagCreate = 0x01,
	RecordFlagDelete = 0x02,
	RecordFlagPartial = 0x04,
};
constexpr std::uint8_t KnownRecordFlags = 0x07;
constexpr std::uint8_t ReservedRecordFlags = 0xf8;
constexpr bool RecordFlagPartialAllowedV1 = false;

enum Capability : std::uint64_t {
	CapabilityNone = 0,
	CapabilityCommViewLocalAssets = 0x0000000000000001ULL,
	CapabilityCommViewAuthoritativeSource = 0x0000000000000002ULL,
	CapabilityTargetVideoH264 = 0x0000000000000004ULL,
	CapabilityTargetVideoRemoteRender = 0x0000000000000008ULL,
	CapabilityUpdate = 0x0000000000000010ULL,
};
constexpr Capability CapabilityDynamicUpdate = CapabilityUpdate;
constexpr std::uint64_t KnownCapabilities = 0x000000000000001fULL;
constexpr std::uint64_t ReservedCapabilities = 0xffffffffffffffe0ULL;
constexpr std::uint8_t FirstReservedCapabilityBit = 5;

enum class CapabilityExtensionType : std::uint16_t {
	Invalid = 0,
	CommViewNegotiation = 1,
	TargetVideoNegotiation = 2,
};
constexpr std::uint16_t FirstReservedCapabilityExtensionType = 3;
constexpr bool TargetVideoNegotiationReservedNonEmittable = true;

constexpr bool is_emittable_capability_extension_type(CapabilityExtensionType type) noexcept {
	return type == CapabilityExtensionType::CommViewNegotiation;
}

enum class WelcomeStatus : std::uint8_t {
	Accepted = 0,
	UnsupportedVersion = 1,
	Unauthorized = 2,
	Busy = 3,
	InvalidCapabilities = 4,
};

enum SessionBeginFlag : std::uint32_t {
	SessionBeginFlagNone = 0,
	SessionBeginFlagReadOnly = 0x00000001U,
	SessionBeginFlagMissionActive = 0x00000002U,
	SessionBeginFlagManifestRequired = 0x00000004U,
};
constexpr std::uint32_t KnownSessionBeginFlags = 0x00000007U;
constexpr std::uint32_t ReservedSessionBeginFlags = 0xfffffff8U;

enum class HeartbeatKind : std::uint8_t {
	Request = 1,
	Response = 2,
};

enum ResyncRequestFlag : std::uint8_t {
	ResyncRequestFlagNone = 0,
	ResyncRequestFlagRequireManifest = 0x01,
	ResyncRequestFlagRequireFullSnapshot = 0x02,
};
constexpr std::uint8_t KnownResyncRequestFlags = 0x03;
constexpr std::uint8_t ReservedResyncRequestFlags = 0xfc;

enum class SessionEndReason : std::uint8_t {
	Normal = 1,
	ProducerShutdown = 2,
	MissionEnded = 3,
	Restart = 4,
	ProtocolError = 5,
	Timeout = 6,
};

enum SessionEndFlag : std::uint8_t {
	SessionEndFlagNone = 0,
	SessionEndFlagReconnectAllowed = 0x01,
};
constexpr std::uint8_t KnownSessionEndFlags = 0x01;
constexpr std::uint8_t ReservedSessionEndFlags = 0xfe;

enum class CapabilityUpdateReason : std::uint8_t {
	Invalid = 0,
	RuntimeAvailability = 1,
	PeerRequest = 2,
	ConfigurationChange = 3,
	ErrorRecovery = 4,
};

enum class ManifestKind : std::uint16_t {
	FullRequired = 1,
};

enum SnapshotFlag : std::uint16_t {
	SnapshotFlagNone = 0,
	SnapshotFlagInitial = 0x0001,
	SnapshotFlagPeriodicKeyframe = 0x0002,
	SnapshotFlagResync = 0x0004,
};
constexpr std::uint16_t KnownSnapshotFlags = 0x0007;
constexpr std::uint16_t ReservedSnapshotFlags = 0xfff8;

enum class EventDeliveryClass : std::uint8_t {
	Replaceable = 1,
	Reliable = 2,
};

enum class AuthorityMode : std::uint8_t {
	Solo = 0,
	MultiplayerClient = 1,
	MultiplayerMaster = 2,
};

enum class SessionPhase : std::uint8_t {
	Starting = 0,
	Synchronizing = 1,
	Live = 2,
	Ending = 3,
};

enum class MissionPhase : std::uint8_t {
	None = 0,
	Loading = 1,
	Active = 2,
	Ending = 3,
	Ended = 4,
};

enum class ObjectType : std::uint8_t {
	Unknown = 0,
	Ship = 1,
	Weapon = 2,
	Asteroid = 3,
	Debris = 4,
	JumpNode = 5,
	Waypoint = 6,
	Fireball = 7,
	Other = 8,
};

enum class LifecyclePhase : std::uint8_t {
	Spawning = 0,
	Active = 1,
	Departing = 2,
	Dying = 3,
	Destroyed = 4,
	Removed = 5,
};

enum class TransitMode : std::uint8_t {
	None = 0,
	Warp = 1,
	Dockbay = 2,
	Scripted = 3,
};

enum class ControlMode : std::uint8_t {
	Unknown = 0,
	Ship = 1,
	View = 2,
	FlightCursor = 3,
	Autopilot = 4,
};

enum class WeaponSubtype : std::uint8_t {
	Unknown = 0,
	Primary = 1,
	Missile = 2,
	Beam = 3,
	Countermeasure = 4,
	Special = 5,
};

enum class SubsystemType : std::uint8_t {
	Unknown = 0,
	Engine = 1,
	Turret = 2,
	Radar = 3,
	Navigation = 4,
	Communication = 5,
	Weapons = 6,
	Sensors = 7,
	Reactor = 8,
	Maneuvering = 9,
	Fighterbay = 10,
	Cargo = 11,
	Awacs = 12,
	Other = 13,
};

enum class AnimationState : std::uint8_t {
	None = 0,
	Stopped = 1,
	Moving = 2,
	Looping = 3,
	Locked = 4,
};

enum class EtsMode : std::uint8_t {
	Absent = 0,
	Available = 1,
	Locked = 2,
};

enum class WeaponFamily : std::uint8_t {
	Primary = 0,
	Secondary = 1,
	Tertiary = 2,
	Turret = 3,
	None = 0xff,
};

enum class ValueTrend : std::uint8_t {
	Unknown = 0,
	Decreasing = 1,
	Stable = 2,
	Increasing = 3,
};

enum class RadarMode : std::uint8_t {
	Short = 0,
	Long = 1,
	Infinite = 2,
	Custom = 3,
};

enum class SensorState : std::uint8_t {
	Offline = 0,
	Degraded = 1,
	Online = 2,
};

enum class RadarVisibility : std::uint8_t {
	NotVisible = 0,
	Visible = 1,
	Distorted = 2,
};

enum class RadarCategory : std::uint8_t {
	Unknown = 0,
	Ship = 1,
	Weapon = 2,
	Navigation = 3,
	JumpNode = 4,
	Asteroid = 5,
	Debris = 6,
	Other = 7,
};

enum class ThreatLevel : std::uint8_t {
	None = 0,
	Dumbfire = 1,
	LockAttempt = 2,
	LockAcquired = 3,
};

enum class GuidanceType : std::uint8_t {
	None = 0,
	Heat = 1,
	Aspect = 2,
	Homing = 3,
	Swarm = 4,
	Scripted = 5,
};

enum class ScanPhase : std::uint8_t {
	NotScannable = 0,
	Idle = 1,
	Scanning = 2,
	Completed = 3,
};

enum class DisclosureState : std::uint8_t {
	Hidden = 0,
	Revealed = 1,
};

enum class DockingPhase : std::uint8_t {
	None = 0,
	Approach = 1,
	Docking = 2,
	Docked = 3,
	Undocking = 4,
};

enum class SupportPhase : std::uint8_t {
	None = 0,
	Requested = 1,
	Approaching = 2,
	Docking = 3,
	Repairing = 4,
	Rearming = 5,
	Obstructed = 6,
	Aborted = 7,
};

enum class NavPointType : std::uint8_t {
	Position = 0,
	Entity = 1,
	Waypoint = 2,
};

enum class AutopilotState : std::uint8_t {
	Disengaged = 0,
	Available = 1,
	Engaged = 2,
	Refused = 3,
};

enum class AutopilotRefusal : std::uint8_t {
	None = 0,
	NoValidNav = 1,
	TooClose = 2,
	Hostiles = 3,
	EngineDisabled = 4,
	ScriptRestricted = 5,
	Unknown = 6,
};

enum class EventReasonCode : std::uint16_t {
	None = 0,
	Completed = 1,
	Interrupted = 2,
	Replaced = 3,
	Scripted = 4,
	Destroyed = 5,
	MissionChange = 6,
	SessionStop = 7,
	Aborted = 8,
	Unknown = 9,
};

enum class EventKind : std::uint16_t {
	EntityAppeared = 1,
	EntityDepartureStarted = 2,
	EntityDisappeared = 3,
	WeaponFired = 4,
	BeamStarted = 5,
	BeamEnded = 6,
	RemoteDetonation = 7,
	CountermeasureLaunched = 8,
	ShieldImpact = 9,
	HullImpact = 10,
	ShieldSegmentDepleted = 11,
	ShieldSegmentRestored = 12,
	SubsystemDamaged = 13,
	SubsystemPerturbed = 14,
	SubsystemRestored = 15,
	SubsystemDestroyed = 16,
	ShipDisabled = 17,
	ShipDyingStarted = 18,
	EntityDestroyed = 19,
	TargetChanged = 20,
	ScanCompleted = 21,
	CargoRevealed = 22,
	DockingStarted = 23,
	Docked = 24,
	UndockingStarted = 25,
	Undocked = 26,
	WarpStarted = 27,
	WarpEnded = 28,
	MissionStarted = 29,
	MissionEnded = 30,
	SessionEnded = 31,
	AfterburnerStarted = 32,
	AfterburnerStopped = 33,
};

enum EntityLifecycleFlag : std::uint32_t {
	EntityLifecycleFlagNone = 0,
	EntityLifecycleFlagDying = 0x00000001U,
	EntityLifecycleFlagDisabled = 0x00000002U,
	EntityLifecycleFlagExploded = 0x00000004U,
	EntityLifecycleFlagShouldBeDead = 0x00000008U,
	EntityLifecycleFlagBomb = 0x00000010U,
};
constexpr std::uint32_t KnownEntityLifecycleFlags = 0x0000001fU;
constexpr std::uint32_t ReservedEntityLifecycleFlags = 0xffffffe0U;

enum ShipRoleFlag : std::uint16_t {
	ShipRoleFlagNone = 0,
	ShipRoleFlagPlayer = 0x0001,
	ShipRoleFlagAi = 0x0002,
	ShipRoleFlagSupport = 0x0004,
	ShipRoleFlagMissionObject = 0x0008,
};
constexpr std::uint16_t KnownShipRoleFlags = 0x000f;
constexpr std::uint16_t ReservedShipRoleFlags = 0xfff0;

enum SensorVisibilityFlag : std::uint16_t {
	SensorVisibilityFlagNone = 0,
	SensorVisibilityFlagStealth = 0x0001,
	SensorVisibilityFlagCloaked = 0x0002,
	SensorVisibilityFlagSensorVisible = 0x0004,
	SensorVisibilityFlagTagged = 0x0008,
};
constexpr std::uint16_t KnownSensorVisibilityFlags = 0x000f;
constexpr std::uint16_t ReservedSensorVisibilityFlags = 0xfff0;

enum ProtectionFlag : std::uint16_t {
	ProtectionFlagNone = 0,
	ProtectionFlagInvulnerable = 0x0001,
	ProtectionFlagProtected = 0x0002,
	ProtectionFlagGuardian = 0x0004,
};
constexpr std::uint16_t KnownProtectionFlags = 0x0007;
constexpr std::uint16_t ReservedProtectionFlags = 0xfff8;

enum PhysicsModeFlag : std::uint32_t {
	PhysicsModeFlagNone = 0,
	PhysicsModeFlagAfterburner = 0x00000001U,
	PhysicsModeFlagBooster = 0x00000002U,
	PhysicsModeFlagGlideActive = 0x00000004U,
	PhysicsModeFlagGlideForced = 0x00000008U,
	PhysicsModeFlagNewtonianDamping = 0x00000010U,
	PhysicsModeFlagLateralTranslation = 0x00000020U,
	PhysicsModeFlagWarpIn = 0x00000040U,
	PhysicsModeFlagWarpOut = 0x00000080U,
	PhysicsModeFlagScripted = 0x00000100U,
	PhysicsModeFlagShockwave = 0x00000200U,
	PhysicsModeFlagImmobile = 0x00000400U,
	PhysicsModeFlagOrientationLocked = 0x00000800U,
};
constexpr std::uint32_t KnownPhysicsModeFlags = 0x00000fffU;
constexpr std::uint32_t ReservedPhysicsModeFlags = 0xfffff000U;

enum ControlFlag : std::uint32_t {
	ControlFlagNone = 0,
	ControlFlagMatchSpeed = 0x00000001U,
	ControlFlagAutoTarget = 0x00000002U,
	ControlFlagAutoMatchSpeed = 0x00000004U,
	ControlFlagPrimaryLinked = 0x00000008U,
	ControlFlagSecondaryDouble = 0x00000010U,
	ControlFlagAfterburnerRequested = 0x00000020U,
};
constexpr std::uint32_t KnownControlFlags = 0x0000003fU;
constexpr std::uint32_t ReservedControlFlags = 0xffffffc0U;

enum SubsystemFlag : std::uint32_t {
	SubsystemFlagNone = 0,
	SubsystemFlagPerturbed = 0x00000001U,
	SubsystemFlagTargetable = 0x00000002U,
	SubsystemFlagVisible = 0x00000004U,
	SubsystemFlagRevealed = 0x00000008U,
	SubsystemFlagGuardian = 0x00000010U,
	SubsystemFlagMovementLocked = 0x00000020U,
	SubsystemFlagBeamFree = 0x00000040U,
	SubsystemFlagBeamLocked = 0x00000080U,
};
constexpr std::uint32_t KnownSubsystemFlags = 0x000000ffU;
constexpr std::uint32_t ReservedSubsystemFlags = 0xffffff00U;

enum WeaponGlobalFlag : std::uint32_t {
	WeaponGlobalFlagNone = 0,
	WeaponGlobalFlagPrimaryLinked = 0x00000001U,
	WeaponGlobalFlagSecondaryDouble = 0x00000002U,
	WeaponGlobalFlagPrimaryTriggerHeld = 0x00000004U,
	WeaponGlobalFlagSecondaryTriggerHeld = 0x00000008U,
	WeaponGlobalFlagPrimaryLocked = 0x00000010U,
	WeaponGlobalFlagSecondaryLocked = 0x00000020U,
	WeaponGlobalFlagTargetingLaser = 0x00000040U,
	WeaponGlobalFlagRemoteDetonatorsActive = 0x00000080U,
	WeaponGlobalFlagBeamFree = 0x00000100U,
	WeaponGlobalFlagBeamLocked = 0x00000200U,
};
constexpr std::uint32_t KnownWeaponGlobalFlags = 0x000003ffU;
constexpr std::uint32_t ReservedWeaponGlobalFlags = 0xfffffc00U;

enum WeaponClassFlag : std::uint64_t {
	WeaponClassFlagNone = 0,
	WeaponClassFlagBomb = 0x0001ULL,
	WeaponClassFlagBallistic = 0x0002ULL,
	WeaponClassFlagAmmoless = 0x0004ULL,
	WeaponClassFlagBeam = 0x0008ULL,
	WeaponClassFlagSwarm = 0x0010ULL,
	WeaponClassFlagCountermeasure = 0x0020ULL,
	WeaponClassFlagHoming = 0x0040ULL,
	WeaponClassFlagRemoteDetonatable = 0x0080ULL,
};
constexpr std::uint64_t KnownWeaponClassFlags = 0x00ffULL;
constexpr std::uint64_t ReservedWeaponClassFlags = 0xffffffffffffff00ULL;

enum WeaponEffectFlag : std::uint32_t {
	WeaponEffectFlagNone = 0,
	WeaponEffectFlagShockwave = 0x0001U,
	WeaponEffectFlagEmp = 0x0002U,
	WeaponEffectFlagTag = 0x0004U,
	WeaponEffectFlagShieldPiercing = 0x0008U,
	WeaponEffectFlagSpawnsChildren = 0x0010U,
};
constexpr std::uint32_t KnownWeaponEffectFlags = 0x001fU;
constexpr std::uint32_t ReservedWeaponEffectFlags = 0xffffffe0U;

enum ClassSubsystemStaticFlag : std::uint32_t {
	ClassSubsystemStaticFlagNone = 0,
	ClassSubsystemStaticFlagTargetable = 0x0001U,
	ClassSubsystemStaticFlagVisibleByDefault = 0x0002U,
	ClassSubsystemStaticFlagScannableCargo = 0x0004U,
	ClassSubsystemStaticFlagRotates = 0x0008U,
	ClassSubsystemStaticFlagTranslates = 0x0010U,
	ClassSubsystemStaticFlagTurret = 0x0020U,
	ClassSubsystemStaticFlagAwacs = 0x0040U,
};
constexpr std::uint32_t KnownClassSubsystemStaticFlags = 0x007fU;
constexpr std::uint32_t ReservedClassSubsystemStaticFlags = 0xffffff80U;

enum ContactFlag : std::uint32_t {
	ContactFlagNone = 0,
	ContactFlagBright = 0x00000001U,
	ContactFlagCurrentTarget = 0x00000002U,
	ContactFlagStealth = 0x00000004U,
	ContactFlagTagged = 0x00000008U,
	ContactFlagWarp = 0x00000010U,
	ContactFlagBomb = 0x00000020U,
	ContactFlagHoming = 0x00000040U,
	ContactFlagThreat = 0x00000080U,
};
constexpr std::uint32_t KnownContactFlags = 0x000000ffU;
constexpr std::uint32_t ReservedContactFlags = 0xffffff00U;

enum EffectFlag : std::uint32_t {
	EffectFlagNone = 0,
	EffectFlagCloaked = 0x00000001U,
	EffectFlagStealth = 0x00000002U,
	EffectFlagElectricArcs = 0x00000004U,
	EffectFlagSparks = 0x00000008U,
	EffectFlagWarpVisual = 0x00000010U,
	EffectFlagDeathRoll = 0x00000020U,
	EffectFlagAmmoWarning = 0x00000040U,
	EffectFlagTargetingLaser = 0x00000080U,
};
constexpr std::uint32_t KnownEffectFlags = 0x000000ffU;
constexpr std::uint32_t ReservedEffectFlags = 0xffffff00U;

enum EventFamilyBit : std::uint64_t {
	EventFamilyBitNone = 0,
	EventFamilyBitEntity = 0x0001ULL,
	EventFamilyBitWeapon = 0x0002ULL,
	EventFamilyBitDamage = 0x0004ULL,
	EventFamilyBitTarget = 0x0008ULL,
	EventFamilyBitCargoScan = 0x0010ULL,
	EventFamilyBitDockingSupport = 0x0020ULL,
	EventFamilyBitWarp = 0x0040ULL,
	EventFamilyBitMissionSession = 0x0080ULL,
	EventFamilyBitControl = 0x0100ULL,
	EventFamilyBitCommunication = 0x0200ULL,
};
constexpr std::uint64_t KnownEventFamilyBits = 0x03ffULL;
constexpr std::uint64_t ReservedEventFamilyBits = 0xfffffffffffffc00ULL;

enum StateDomainCoverageBit : std::uint64_t {
	StateDomainCoverageBitNone = 0,
	StateDomainCoverageBitCoreShip = 0x0001ULL,
	StateDomainCoverageBitControlInputs = 0x0002ULL,
	StateDomainCoverageBitPrediction = 0x0004ULL,
	StateDomainCoverageBitRadarSensors = 0x0008ULL,
	StateDomainCoverageBitAllEntities = 0x0010ULL,
	StateDomainCoverageBitLowFrequencyEffects = 0x0020ULL,
	StateDomainCoverageBitTargeting = 0x0040ULL,
	StateDomainCoverageBitWeapons = 0x0080ULL,
	StateDomainCoverageBitCargoDockSupport = 0x0100ULL,
	StateDomainCoverageBitNavigation = 0x0200ULL,
	StateDomainCoverageBitPlayerKinematics = 0x0400ULL,
};
constexpr std::uint64_t KnownStateDomainCoverageBitsV1_0 = 0x03ffULL;
constexpr std::uint64_t KnownStateDomainCoverageBitsV1_1 = 0x07ffULL;
// The unqualified aliases remain the frozen 1.0 view. This prevents code that
// has no negotiated-version context from treating the 1.1 bit as valid.
constexpr std::uint64_t KnownStateDomainCoverageBits = KnownStateDomainCoverageBitsV1_0;
constexpr std::uint64_t ReservedStateDomainCoverageBits = 0xfffffffffffffc00ULL;
constexpr std::uint64_t ReservedStateDomainCoverageBitsV1_1 = 0xfffffffffffff800ULL;

constexpr bool is_supported_version_minor(std::uint8_t minor) noexcept
{
	return minor == VersionMinorV1_0 || minor == VersionMinorV1_1 ||
		minor == VersionMinorV1_2;
}

constexpr std::uint64_t known_state_domain_coverage_bits(std::uint8_t minor) noexcept
{
	return minor == VersionMinorV1_0 ? KnownStateDomainCoverageBitsV1_0 :
		minor == VersionMinorV1_1 ? KnownStateDomainCoverageBitsV1_1 : 0U;
}

enum PropulsionFlag : std::uint16_t {
	PropulsionFlagNone = 0,
	PropulsionFlagAfterburnerAvailable = 0x0001,
	PropulsionFlagAfterburnerLocked = 0x0002,
	PropulsionFlagAfterburnerActive = 0x0004,
	PropulsionFlagAfterburnerRequested = 0x0008,
	PropulsionFlagBoosterActive = 0x0010,
	PropulsionFlagGlideActive = 0x0020,
	PropulsionFlagGlideForced = 0x0040,
	PropulsionFlagRcsActive = 0x0080,
};
constexpr std::uint16_t KnownPropulsionFlags = 0x00ff;
constexpr std::uint16_t ReservedPropulsionFlags = 0xff00;

enum CountermeasureStateFlag : std::uint16_t {
	CountermeasureStateFlagNone = 0,
	CountermeasureStateFlagAvailable = 0x0001,
	CountermeasureStateFlagLocked = 0x0002,
};
constexpr std::uint16_t KnownCountermeasureStateFlags = 0x0003;
constexpr std::uint16_t ReservedCountermeasureStateFlags = 0xfffc;

enum ScanValidityFlag : std::uint8_t {
	ScanValidityFlagNone = 0,
	ScanValidityFlagInRange = 0x01,
	ScanValidityFlagInAngle = 0x02,
	ScanValidityFlagLineOfSight = 0x04,
};
constexpr std::uint8_t KnownScanValidityFlags = 0x07;
constexpr std::uint8_t ReservedScanValidityFlags = 0xf8;

enum SupportFlag : std::uint8_t {
	SupportFlagNone = 0,
	SupportFlagAwaitingRepair = 0x01,
	SupportFlagBeingRepaired = 0x02,
	SupportFlagRepairingOther = 0x04,
};
constexpr std::uint8_t KnownSupportFlags = 0x07;
constexpr std::uint8_t ReservedSupportFlags = 0xf8;

enum NavPointFlag : std::uint8_t {
	NavPointFlagNone = 0,
	NavPointFlagHidden = 0x01,
	NavPointFlagNoAccess = 0x02,
	NavPointFlagVisited = 0x04,
};
constexpr std::uint8_t KnownNavPointFlags = 0x07;
constexpr std::uint8_t ReservedNavPointFlags = 0xf8;

enum EventFlag : std::uint16_t {
	EventFlagNone = 0,
	EventFlagReconstructible = 0x0001,
	EventFlagExactCapture = 0x0002,
	EventFlagReliable = 0x0004,
};
constexpr std::uint16_t KnownEventFlags = 0x0007;
constexpr std::uint16_t ReservedEventFlags = 0xfff8;

// Top-level RecordType presence bitmaps.
enum SessionStatePresenceFlag : std::uint64_t {
	SessionStatePresenceFlagNone = 0,
	SessionStatePresenceFlagObservedPlayer = 0x0000000000000001ULL,
};
constexpr std::uint64_t KnownSessionStatePresenceFlags = 0x0000000000000001ULL;
constexpr std::uint64_t ReservedSessionStatePresenceFlags = 0xfffffffffffffffeULL;

enum MissionStatePresenceFlag : std::uint64_t {
	MissionStatePresenceFlagNone = 0,
	MissionStatePresenceFlagMissionName = 0x0000000000000001ULL,
};
constexpr std::uint64_t KnownMissionStatePresenceFlags = 0x0000000000000001ULL;
constexpr std::uint64_t ReservedMissionStatePresenceFlags = 0xfffffffffffffffeULL;

enum ClassManifestPresenceFlag : std::uint64_t {
	ClassManifestPresenceFlagNone = 0,
	ClassManifestPresenceFlagInertia = 0x0000000000000001ULL,
	ClassManifestPresenceFlagDamping = 0x0000000000000002ULL,
	ClassManifestPresenceFlagMotion = 0x0000000000000004ULL,
	ClassManifestPresenceFlagHull = 0x0000000000000008ULL,
	ClassManifestPresenceFlagShield = 0x0000000000000010ULL,
	ClassManifestPresenceFlagEnergy = 0x0000000000000020ULL,
	ClassManifestPresenceFlagAfterburner = 0x0000000000000040ULL,
	ClassManifestPresenceFlagCountermeasure = 0x0000000000000080ULL,
	ClassManifestPresenceFlagBanks = 0x0000000000000100ULL,
	ClassManifestPresenceFlagSubsystems = 0x0000000000000200ULL,
	ClassManifestPresenceFlagScan = 0x0000000000000400ULL,
	ClassManifestPresenceFlagGlide = 0x0000000000000800ULL,
	ClassManifestPresenceFlagAutoaim = 0x0000000000001000ULL,
	ClassManifestPresenceFlagRadarIcon = 0x0000000000002000ULL,
};
constexpr std::uint64_t KnownClassManifestPresenceFlags = 0x0000000000003fffULL;
constexpr std::uint64_t ReservedClassManifestPresenceFlags = 0xffffffffffffc000ULL;

enum WeaponManifestPresenceFlag : std::uint64_t {
	WeaponManifestPresenceFlagNone = 0,
	WeaponManifestPresenceFlagTitle = 0x0000000000000001ULL,
	WeaponManifestPresenceFlagAcceleration = 0x0000000000000002ULL,
	WeaponManifestPresenceFlagRanges = 0x0000000000000004ULL,
	WeaponManifestPresenceFlagFire = 0x0000000000000008ULL,
	WeaponManifestPresenceFlagDamage = 0x0000000000000010ULL,
	WeaponManifestPresenceFlagGuidance = 0x0000000000000020ULL,
	WeaponManifestPresenceFlagLock = 0x0000000000000040ULL,
	WeaponManifestPresenceFlagCargoRearm = 0x0000000000000080ULL,
	WeaponManifestPresenceFlagBurst = 0x0000000000000100ULL,
	WeaponManifestPresenceFlagSwarm = 0x0000000000000200ULL,
	WeaponManifestPresenceFlagCountermeasure = 0x0000000000000400ULL,
};
constexpr std::uint64_t KnownWeaponManifestPresenceFlags = 0x00000000000007ffULL;
constexpr std::uint64_t ReservedWeaponManifestPresenceFlags = 0xfffffffffffff800ULL;

enum EntityLifecyclePresenceFlag : std::uint64_t {
	EntityLifecyclePresenceFlagNone = 0,
	EntityLifecyclePresenceFlagSignature = 0x0000000000000001ULL,
	EntityLifecyclePresenceFlagNetSignature = 0x0000000000000002ULL,
	EntityLifecyclePresenceFlagClassReference = 0x0000000000000004ULL,
	EntityLifecyclePresenceFlagParent = 0x0000000000000008ULL,
	EntityLifecyclePresenceFlagArrivalMode = 0x0000000000000010ULL,
	EntityLifecyclePresenceFlagDepartureMode = 0x0000000000000020ULL,
	EntityLifecyclePresenceFlagNonShipNames = 0x0000000000000040ULL,
	EntityLifecyclePresenceFlagNonShipTeamIff = 0x0000000000000080ULL,
	EntityLifecyclePresenceFlagNonShipRadius = 0x0000000000000100ULL,
};
constexpr std::uint64_t KnownEntityLifecyclePresenceFlags = 0x00000000000001ffULL;
constexpr std::uint64_t ReservedEntityLifecyclePresenceFlags = 0xfffffffffffffe00ULL;

enum ShipIdentityPresenceFlag : std::uint64_t {
	ShipIdentityPresenceFlagNone = 0,
	ShipIdentityPresenceFlagDisplayName = 0x0000000000000001ULL,
	ShipIdentityPresenceFlagCallsign = 0x0000000000000002ULL,
	ShipIdentityPresenceFlagWing = 0x0000000000000004ULL,
	ShipIdentityPresenceFlagLogicalSize = 0x0000000000000008ULL,
	ShipIdentityPresenceFlagSensorVisibility = 0x0000000000000010ULL,
};
constexpr std::uint64_t KnownShipIdentityPresenceFlags = 0x000000000000001fULL;
constexpr std::uint64_t ReservedShipIdentityPresenceFlags = 0xffffffffffffffe0ULL;

enum FlightStatePresenceFlag : std::uint64_t {
	FlightStatePresenceFlagNone = 0,
	FlightStatePresenceFlagDesiredVel = 0x0000000000000001ULL,
	FlightStatePresenceFlagDesiredRotvel = 0x0000000000000002ULL,
	FlightStatePresenceFlagPrevRampVel = 0x0000000000000004ULL,
	FlightStatePresenceFlagVelocityCaps = 0x0000000000000008ULL,
	FlightStatePresenceFlagRotationCaps = 0x0000000000000010ULL,
	FlightStatePresenceFlagRearCap = 0x0000000000000020ULL,
	FlightStatePresenceFlagGlideCaps = 0x0000000000000040ULL,
	FlightStatePresenceFlagGravity = 0x0000000000000080ULL,
	FlightStatePresenceFlagTimeConstants = 0x0000000000000100ULL,
	FlightStatePresenceFlagRotdamp = 0x0000000000000200ULL,
	FlightStatePresenceFlagSideSlip = 0x0000000000000400ULL,
	FlightStatePresenceFlagCosmeticThrust = 0x0000000000000800ULL,
};
constexpr std::uint64_t KnownFlightStatePresenceFlags = 0x0000000000000fffULL;
constexpr std::uint64_t ReservedFlightStatePresenceFlags = 0xfffffffffffff000ULL;

enum ControlStatePresenceFlag : std::uint64_t {
	ControlStatePresenceFlagNone = 0,
	ControlStatePresenceFlagCruise = 0x0000000000000001ULL,
	ControlStatePresenceFlagRequestCounters = 0x0000000000000002ULL,
	ControlStatePresenceFlagFlightCursor = 0x0000000000000004ULL,
};
constexpr std::uint64_t KnownControlStatePresenceFlags = 0x0000000000000007ULL;
constexpr std::uint64_t ReservedControlStatePresenceFlags = 0xfffffffffffffff8ULL;

enum DamageStatePresenceFlag : std::uint64_t {
	DamageStatePresenceFlagNone = 0,
	DamageStatePresenceFlagSimHull = 0x0000000000000001ULL,
	DamageStatePresenceFlagArmor = 0x0000000000000002ULL,
	DamageStatePresenceFlagGuardian = 0x0000000000000004ULL,
	DamageStatePresenceFlagCumulativeDamage = 0x0000000000000008ULL,
	DamageStatePresenceFlagLastDamage = 0x0000000000000010ULL,
	DamageStatePresenceFlagContributors = 0x0000000000000020ULL,
};
constexpr std::uint64_t KnownDamageStatePresenceFlags = 0x000000000000003fULL;
constexpr std::uint64_t ReservedDamageStatePresenceFlags = 0xffffffffffffffc0ULL;

enum ShieldStatePresenceFlag : std::uint64_t {
	ShieldStatePresenceFlagNone = 0,
	ShieldStatePresenceFlagRechargeMax = 0x0000000000000001ULL,
	ShieldStatePresenceFlagRegenRate = 0x0000000000000002ULL,
	ShieldStatePresenceFlagDeferredTransfer = 0x0000000000000004ULL,
};
constexpr std::uint64_t KnownShieldStatePresenceFlags = 0x0000000000000007ULL;
constexpr std::uint64_t ReservedShieldStatePresenceFlags = 0xfffffffffffffff8ULL;

enum SubsystemStatePresenceFlag : std::uint64_t {
	SubsystemStatePresenceFlagNone = 0,
	SubsystemStatePresenceFlagNameOverrides = 0x0000000000000001ULL,
	SubsystemStatePresenceFlagArmor = 0x0000000000000002ULL,
	SubsystemStatePresenceFlagPerturbation = 0x0000000000000004ULL,
	SubsystemStatePresenceFlagAnimatedTransform = 0x0000000000000008ULL,
	SubsystemStatePresenceFlagAnimations = 0x0000000000000010ULL,
	SubsystemStatePresenceFlagLocalCargo = 0x0000000000000020ULL,
	SubsystemStatePresenceFlagTypeAggregate = 0x0000000000000040ULL,
	SubsystemStatePresenceFlagTurret = 0x0000000000000080ULL,
};
constexpr std::uint64_t KnownSubsystemStatePresenceFlags = 0x00000000000000ffULL;
constexpr std::uint64_t ReservedSubsystemStatePresenceFlags = 0xffffffffffffff00ULL;

enum EnergyStatePresenceFlag : std::uint64_t {
	EnergyStatePresenceFlagNone = 0,
	EnergyStatePresenceFlagWeaponEnergy = 0x0000000000000001ULL,
	EnergyStatePresenceFlagRegeneration = 0x0000000000000002ULL,
	EnergyStatePresenceFlagDeferredTransfers = 0x0000000000000004ULL,
	EnergyStatePresenceFlagEngineResult = 0x0000000000000008ULL,
	EnergyStatePresenceFlagPowerOutput = 0x0000000000000010ULL,
	EnergyStatePresenceFlagEngineIntegrity = 0x0000000000000020ULL,
};
constexpr std::uint64_t KnownEnergyStatePresenceFlags = 0x000000000000003fULL;
constexpr std::uint64_t ReservedEnergyStatePresenceFlags = 0xffffffffffffffc0ULL;

enum PropulsionStatePresenceFlag : std::uint64_t {
	PropulsionStatePresenceFlagNone = 0,
	PropulsionStatePresenceFlagFuel = 0x0000000000000001ULL,
	PropulsionStatePresenceFlagConsumption = 0x0000000000000002ULL,
	PropulsionStatePresenceFlagEngagement = 0x0000000000000004ULL,
	PropulsionStatePresenceFlagDynamics = 0x0000000000000008ULL,
	PropulsionStatePresenceFlagEngineWash = 0x0000000000000010ULL,
	PropulsionStatePresenceFlagRcs = 0x0000000000000020ULL,
};
constexpr std::uint64_t KnownPropulsionStatePresenceFlags = 0x000000000000003fULL;
constexpr std::uint64_t ReservedPropulsionStatePresenceFlags = 0xffffffffffffffc0ULL;

enum WeaponStatePresenceFlag : std::uint64_t {
	WeaponStatePresenceFlagNone = 0,
	WeaponStatePresenceFlagPreviousPrimary = 0x0000000000000001ULL,
	WeaponStatePresenceFlagPreviousSecondary = 0x0000000000000002ULL,
	WeaponStatePresenceFlagTargetingLaser = 0x0000000000000004ULL,
	WeaponStatePresenceFlagSwarm = 0x0000000000000008ULL,
	WeaponStatePresenceFlagRemoteDetonation = 0x0000000000000010ULL,
	WeaponStatePresenceFlagPerBurstRotation = 0x0000000000000020ULL,
	WeaponStatePresenceFlagTertiary = 0x0000000000000040ULL,
	WeaponStatePresenceFlagCountermeasure = 0x0000000000000080ULL,
};
constexpr std::uint64_t KnownWeaponStatePresenceFlags = 0x00000000000000ffULL;
constexpr std::uint64_t ReservedWeaponStatePresenceFlags = 0xffffffffffffff00ULL;

enum LockStatePresenceFlag : std::uint64_t {
	LockStatePresenceFlagNone = 0,
};
constexpr std::uint64_t KnownLockStatePresenceFlags = 0;
constexpr std::uint64_t ReservedLockStatePresenceFlags = 0xffffffffffffffffULL;

enum TargetStatePresenceFlag : std::uint64_t {
	TargetStatePresenceFlagNone = 0,
	TargetStatePresenceFlagPreviousTarget = 0x0000000000000001ULL,
	TargetStatePresenceFlagRevealedIdentity = 0x0000000000000002ULL,
	TargetStatePresenceFlagTimeOnTarget = 0x0000000000000004ULL,
	TargetStatePresenceFlagTargetSubsystem = 0x0000000000000008ULL,
	TargetStatePresenceFlagLockSubsystem = 0x0000000000000010ULL,
	TargetStatePresenceFlagLastStealthObservation = 0x0000000000000020ULL,
	TargetStatePresenceFlagDistanceTrend = 0x0000000000000040ULL,
	TargetStatePresenceFlagSpeedTrend = 0x0000000000000080ULL,
	TargetStatePresenceFlagInCone = 0x0000000000000100ULL,
	TargetStatePresenceFlagLead = 0x0000000000000200ULL,
	TargetStatePresenceFlagAttacker = 0x0000000000000400ULL,
	TargetStatePresenceFlagDangerousWeapon = 0x0000000000000800ULL,
	TargetStatePresenceFlagNearestLocked = 0x0000000000001000ULL,
	TargetStatePresenceFlagExactHudDistance = 0x0000000000002000ULL,
	TargetStatePresenceFlagExactHudSpeed = 0x0000000000004000ULL,
};
constexpr std::uint64_t KnownTargetStatePresenceFlags = 0x0000000000007fffULL;
constexpr std::uint64_t ReservedTargetStatePresenceFlags = 0xffffffffffff8000ULL;

enum RadarStatePresenceFlag : std::uint64_t {
	RadarStatePresenceFlagNone = 0,
	RadarStatePresenceFlagBrightRange = 0x0000000000000001ULL,
	RadarStatePresenceFlagPrimitiveRange = 0x0000000000000002ULL,
	RadarStatePresenceFlagAwacs = 0x0000000000000004ULL,
	RadarStatePresenceFlagEmp = 0x0000000000000008ULL,
	RadarStatePresenceFlagJamming = 0x0000000000000010ULL,
	RadarStatePresenceFlagVisibilityTimes = 0x0000000000000020ULL,
};
constexpr std::uint64_t KnownRadarStatePresenceFlags = 0x000000000000003fULL;
constexpr std::uint64_t ReservedRadarStatePresenceFlags = 0xffffffffffffffc0ULL;

enum RadarContactsPresenceFlag : std::uint64_t {
	RadarContactsPresenceFlagNone = 0,
	RadarContactsPresenceFlagIconSize = 0x0000000000000001ULL,
	RadarContactsPresenceFlagRevealedName = 0x0000000000000002ULL,
	RadarContactsPresenceFlagRevealedClass = 0x0000000000000004ULL,
	RadarContactsPresenceFlagRevealedTeamIff = 0x0000000000000008ULL,
	RadarContactsPresenceFlagDetectionTimes = 0x0000000000000010ULL,
	RadarContactsPresenceFlagConfidence = 0x0000000000000020ULL,
};
constexpr std::uint64_t KnownRadarContactsPresenceFlags = 0x000000000000003fULL;
constexpr std::uint64_t ReservedRadarContactsPresenceFlags = 0xffffffffffffffc0ULL;

enum ThreatStatePresenceFlag : std::uint64_t {
	ThreatStatePresenceFlagNone = 0,
	ThreatStatePresenceFlagNearestAttacker = 0x0000000000000001ULL,
	ThreatStatePresenceFlagDangerousWeapon = 0x0000000000000002ULL,
	ThreatStatePresenceFlagNearestHoming = 0x0000000000000004ULL,
};
constexpr std::uint64_t KnownThreatStatePresenceFlags = 0x0000000000000007ULL;
constexpr std::uint64_t ReservedThreatStatePresenceFlags = 0xfffffffffffffff8ULL;

enum CargoScanStatePresenceFlag : std::uint64_t {
	CargoScanStatePresenceFlagNone = 0,
	CargoScanStatePresenceFlagTarget = 0x0000000000000001ULL,
	CargoScanStatePresenceFlagSubsystem = 0x0000000000000002ULL,
	CargoScanStatePresenceFlagTiming = 0x0000000000000004ULL,
	CargoScanStatePresenceFlagValidity = 0x0000000000000008ULL,
	CargoScanStatePresenceFlagCargoText = 0x0000000000000010ULL,
};
constexpr std::uint64_t KnownCargoScanStatePresenceFlags = 0x000000000000001fULL;
constexpr std::uint64_t ReservedCargoScanStatePresenceFlags = 0xffffffffffffffe0ULL;

enum DockingStatePresenceFlag : std::uint64_t {
	DockingStatePresenceFlagNone = 0,
};
constexpr std::uint64_t KnownDockingStatePresenceFlags = 0;
constexpr std::uint64_t ReservedDockingStatePresenceFlags = 0xffffffffffffffffULL;

enum SupportStatePresenceFlag : std::uint64_t {
	SupportStatePresenceFlagNone = 0,
	SupportStatePresenceFlagSupportEntity = 0x0000000000000001ULL,
};
constexpr std::uint64_t KnownSupportStatePresenceFlags = 0x0000000000000001ULL;
constexpr std::uint64_t ReservedSupportStatePresenceFlags = 0xfffffffffffffffeULL;

enum NavigationStatePresenceFlag : std::uint64_t {
	NavigationStatePresenceFlagNone = 0,
	NavigationStatePresenceFlagCurrentNavpoint = 0x0000000000000001ULL,
	NavigationStatePresenceFlagAutopilotRefusal = 0x0000000000000002ULL,
	NavigationStatePresenceFlagWaypointRoute = 0x0000000000000004ULL,
};
constexpr std::uint64_t KnownNavigationStatePresenceFlags = 0x0000000000000007ULL;
constexpr std::uint64_t ReservedNavigationStatePresenceFlags = 0xfffffffffffffff8ULL;

enum EffectStatePresenceFlag : std::uint64_t {
	EffectStatePresenceFlagNone = 0,
	EffectStatePresenceFlagEmpVisual = 0x0000000000000001ULL,
	EffectStatePresenceFlagTags = 0x0000000000000002ULL,
	EffectStatePresenceFlagRcs = 0x0000000000000004ULL,
	EffectStatePresenceFlagBayDoors = 0x0000000000000008ULL,
	EffectStatePresenceFlagGlowBanks = 0x0000000000000010ULL,
	EffectStatePresenceFlagThrusters = 0x0000000000000020ULL,
	EffectStatePresenceFlagTeamColors = 0x0000000000000040ULL,
	EffectStatePresenceFlagAutoaim = 0x0000000000000080ULL,
	EffectStatePresenceFlagTargetingLaserVisual = 0x0000000000000100ULL,
};
constexpr std::uint64_t KnownEffectStatePresenceFlags = 0x00000000000001ffULL;
constexpr std::uint64_t ReservedEffectStatePresenceFlags = 0xfffffffffffffe00ULL;

// Nested record-item presence bitmaps.
enum ClassBankPresenceFlag : std::uint16_t {
	ClassBankPresenceFlagNone = 0,
	ClassBankPresenceFlagCapacity = 0x0001,
	ClassBankPresenceFlagWeaponClass = 0x0002,
};
constexpr std::uint16_t KnownClassBankPresenceFlags = 0x0003;
constexpr std::uint16_t ReservedClassBankPresenceFlags = 0xfffc;

enum ClassSubsystemPresenceFlag : std::uint16_t {
	ClassSubsystemPresenceFlagNone = 0,
	ClassSubsystemPresenceFlagAltName = 0x0001,
	ClassSubsystemPresenceFlagHudName = 0x0002,
	ClassSubsystemPresenceFlagOrientation = 0x0004,
	ClassSubsystemPresenceFlagArmor = 0x0008,
};
constexpr std::uint16_t KnownClassSubsystemPresenceFlags = 0x000f;
constexpr std::uint16_t ReservedClassSubsystemPresenceFlags = 0xfff0;

enum SubsystemAnimationPresenceFlag : std::uint16_t {
	SubsystemAnimationPresenceFlagNone = 0,
	SubsystemAnimationPresenceFlagAngle = 0x0001,
	SubsystemAnimationPresenceFlagTranslation = 0x0002,
	SubsystemAnimationPresenceFlagTarget = 0x0004,
};
constexpr std::uint16_t KnownSubsystemAnimationPresenceFlags = 0x0007;
constexpr std::uint16_t ReservedSubsystemAnimationPresenceFlags = 0xfff8;

enum TurretStatePresenceFlag : std::uint32_t {
	TurretStatePresenceFlagNone = 0,
	TurretStatePresenceFlagTargetSubsystem = 0x00000001U,
	TurretStatePresenceFlagAimPoint = 0x00000002U,
	TurretStatePresenceFlagNextFirePoint = 0x00000004U,
	TurretStatePresenceFlagCooldown = 0x00000008U,
	TurretStatePresenceFlagTimeInRange = 0x00000010U,
	TurretStatePresenceFlagOptimalRange = 0x00000020U,
	TurretStatePresenceFlagTargetPriority = 0x00000040U,
	TurretStatePresenceFlagInaccuracy = 0x00000080U,
	TurretStatePresenceFlagRateMultiplier = 0x00000100U,
	TurretStatePresenceFlagAnimation = 0x00000200U,
	TurretStatePresenceFlagBanks = 0x00000400U,
	TurretStatePresenceFlagSwarm = 0x00000800U,
	TurretStatePresenceFlagAwacs = 0x00001000U,
};
constexpr std::uint32_t KnownTurretStatePresenceFlags = 0x00001fffU;
constexpr std::uint32_t ReservedTurretStatePresenceFlags = 0xffffe000U;

enum TurretBankPresenceFlag : std::uint16_t {
	TurretBankPresenceFlagNone = 0,
	TurretBankPresenceFlagAmmo = 0x0001,
};
constexpr std::uint16_t KnownTurretBankPresenceFlags = 0x0001;
constexpr std::uint16_t ReservedTurretBankPresenceFlags = 0xfffe;

enum PrimaryBankPresenceFlag : std::uint16_t {
	PrimaryBankPresenceFlagNone = 0,
	PrimaryBankPresenceFlagBallisticAmmo = 0x0001,
	PrimaryBankPresenceFlagRearm = 0x0002,
	PrimaryBankPresenceFlagBurst = 0x0004,
	PrimaryBankPresenceFlagSubstitution = 0x0008,
	PrimaryBankPresenceFlagAnimation = 0x0010,
	PrimaryBankPresenceFlagFofCooldown = 0x0020,
};
constexpr std::uint16_t KnownPrimaryBankPresenceFlags = 0x003f;
constexpr std::uint16_t ReservedPrimaryBankPresenceFlags = 0xffc0;

enum SecondaryBankPresenceFlag : std::uint16_t {
	SecondaryBankPresenceFlagNone = 0,
	SecondaryBankPresenceFlagAmmo = 0x0001,
	SecondaryBankPresenceFlagRearm = 0x0002,
	SecondaryBankPresenceFlagBurst = 0x0004,
	SecondaryBankPresenceFlagSubstitution = 0x0008,
	SecondaryBankPresenceFlagAnimation = 0x0010,
};
constexpr std::uint16_t KnownSecondaryBankPresenceFlags = 0x001f;
constexpr std::uint16_t ReservedSecondaryBankPresenceFlags = 0xffe0;

enum CountermeasureStatePresenceFlag : std::uint16_t {
	CountermeasureStatePresenceFlagNone = 0,
	CountermeasureStatePresenceFlagClass = 0x0001,
};
constexpr std::uint16_t KnownCountermeasureStatePresenceFlags = 0x0001;
constexpr std::uint16_t ReservedCountermeasureStatePresenceFlags = 0xfffe;

enum LockItemPresenceFlag : std::uint16_t {
	LockItemPresenceFlagNone = 0,
	LockItemPresenceFlagSubsystem = 0x0001,
	LockItemPresenceFlagLockAttempt = 0x0002,
};
constexpr std::uint16_t KnownLockItemPresenceFlags = 0x0003;
constexpr std::uint16_t ReservedLockItemPresenceFlags = 0xfffc;

enum IncomingMissilePresenceFlag : std::uint16_t {
	IncomingMissilePresenceFlagNone = 0,
	IncomingMissilePresenceFlagHomingSubsystem = 0x0001,
};
constexpr std::uint16_t KnownIncomingMissilePresenceFlags = 0x0001;
constexpr std::uint16_t ReservedIncomingMissilePresenceFlags = 0xfffe;

enum NavPointPresenceFlag : std::uint16_t {
	NavPointPresenceFlagNone = 0,
	NavPointPresenceFlagEntityLink = 0x0001,
	NavPointPresenceFlagWaypointLink = 0x0002,
};
constexpr std::uint16_t KnownNavPointPresenceFlags = 0x0003;
constexpr std::uint16_t ReservedNavPointPresenceFlags = 0xfffc;

enum EventItemPresenceFlag : std::uint32_t {
	EventItemPresenceFlagNone = 0,
	EventItemPresenceFlagActor = 0x00000001U,
	EventItemPresenceFlagTarget = 0x00000002U,
	EventItemPresenceFlagSubsystem = 0x00000004U,
	EventItemPresenceFlagWeaponClass = 0x00000008U,
	EventItemPresenceFlagBank = 0x00000010U,
	EventItemPresenceFlagFirePoint = 0x00000020U,
	EventItemPresenceFlagProjectiles = 0x00000040U,
	EventItemPresenceFlagPosition = 0x00000080U,
	EventItemPresenceFlagNormal = 0x00000100U,
	EventItemPresenceFlagAmount = 0x00000200U,
	EventItemPresenceFlagShieldSegment = 0x00000400U,
	EventItemPresenceFlagBurst = 0x00000800U,
	EventItemPresenceFlagCargoText = 0x00001000U,
	EventItemPresenceFlagReasonCode = 0x00002000U,
	EventItemPresenceFlagPreviousTarget = 0x00004000U,
	EventItemPresenceFlagTransitMode = 0x00008000U,
};
constexpr std::uint32_t KnownEventItemPresenceFlags = 0x0000ffffU;
constexpr std::uint32_t ReservedEventItemPresenceFlags = 0xffff0000U;


enum class CommNegotiationResult : std::uint8_t {
	NotRequested = 0,
	Accepted = 1,
	SourceUnavailable = 2,
	BundleAbsent = 3,
	BundleVersionMismatch = 4,
	BundleHashMismatch = 5,
	NoCommonFormat = 6,
	ManifestInvalid = 7,
};

enum class SourceFormat : std::uint8_t {
	Invalid = 0,
	Ani = 1,
	Eff = 2,
	Apng = 3,
	StaticImage = 4,
};

enum class DeliveredFormat : std::uint8_t {
	Invalid = 0,
	Ani = 1,
	Eff = 2,
	Apng = 3,
	WebmNoAudio = 4,
	Rgba8Atlas = 5,
	Png = 6,
};

enum DeliveredFormatBit : std::uint32_t {
	DeliveredFormatBitNone = 0,
	DeliveredFormatBitAni = 0x00000001U,
	DeliveredFormatBitEff = 0x00000002U,
	DeliveredFormatBitApng = 0x00000004U,
	DeliveredFormatBitWebmNoAudio = 0x00000008U,
	DeliveredFormatBitRgba8Atlas = 0x00000010U,
	DeliveredFormatBitPng = 0x00000020U,
};
constexpr std::uint32_t KnownDeliveredFormatBits = 0x0000003fU;
constexpr std::uint32_t ReservedDeliveredFormatBits = 0xffffffc0U;

enum class AlphaMode : std::uint8_t {
	None = 0,
	Straight = 1,
	Premultiplied = 2,
};

enum class AssetTimingMode : std::uint8_t {
	FormatIntrinsic = 0,
	Constant = 1,
	PerFrame = 2,
};

enum ManifestFlag : std::uint16_t {
	ManifestFlagNone = 0,
	ManifestFlagFrameAsset = 0x0001,
	ManifestFlagPlaceholderAsset = 0x0002,
};
constexpr std::uint16_t KnownManifestFlags = 0x0003;
constexpr std::uint16_t ReservedManifestFlags = 0xfffc;

enum class CommEventKind : std::uint8_t {
	Start = 1,
	Stop = 2,
};

enum class CommPlaybackMode : std::uint8_t {
	Once = 0,
	Loop = 1,
};

enum class CommColorMode : std::uint8_t {
	HudTint = 0,
	FullColor = 1,
};

enum class CommStopReason : std::uint8_t {
	None = 0,
	Completed = 1,
	Interrupted = 2,
	Replaced = 3,
	HudDisabled = 4,
	MissionChanged = 5,
	SessionStopped = 6,
};

enum class VideoConfigResult : std::uint8_t {
	Accepted = 0,
	UnsupportedCapability = 1,
	InvalidRequest = 2,
	NoCommonProfile = 3,
	NoCommonLevel = 4,
	NoCommonRenderProfile = 5,
	RequiredOverlaysMissing = 6,
	ResourceLimit = 7,
	TemporarilyUnavailable = 8,
};

enum class Codec : std::uint8_t {
	Invalid = 0,
	H264AnnexB = 1,
};

enum class H264Profile : std::uint8_t {
	ConstrainedBaseline = 66,
	Main = 77,
	High = 100,
};

enum H264ProfileBit : std::uint32_t {
	H264ProfileBitNone = 0,
	H264ProfileBitConstrainedBaseline = 0x00000001U,
	H264ProfileBitMain = 0x00000002U,
	H264ProfileBitHigh = 0x00000004U,
};
constexpr std::uint32_t KnownH264ProfileBits = 0x00000007U;
constexpr std::uint32_t ReservedH264ProfileBits = 0xfffffff8U;

enum class H264Level : std::uint8_t {
	Level3_1 = 31,
	Level3_2 = 32,
	Level4_0 = 40,
	Level4_1 = 41,
	Level4_2 = 42,
	Level5_0 = 50,
	Level5_1 = 51,
	Level5_2 = 52,
};

enum H264LevelBit : std::uint32_t {
	H264LevelBitNone = 0,
	H264LevelBitLevel3_1 = 0x00000001U,
	H264LevelBitLevel3_2 = 0x00000002U,
	H264LevelBitLevel4_0 = 0x00000004U,
	H264LevelBitLevel4_1 = 0x00000008U,
	H264LevelBitLevel4_2 = 0x00000010U,
	H264LevelBitLevel5_0 = 0x00000020U,
	H264LevelBitLevel5_1 = 0x00000040U,
	H264LevelBitLevel5_2 = 0x00000080U,
};
constexpr std::uint32_t KnownH264LevelBits = 0x000000ffU;
constexpr std::uint32_t ReservedH264LevelBits = 0xffffff00U;

enum class PixelFormat : std::uint8_t {
	Invalid = 0,
	Yuv420p8 = 1,
};

enum class RenderProfile : std::uint8_t {
	Invalid = 0,
	MfdHigh = 1,
	HudExact = 2,
};

enum RenderProfileBit : std::uint32_t {
	RenderProfileBitNone = 0,
	RenderProfileBitMfdHigh = 0x00000001U,
	RenderProfileBitHudExact = 0x00000002U,
};
constexpr std::uint32_t KnownRenderProfileBits = 0x00000003U;
constexpr std::uint32_t ReservedRenderProfileBits = 0xfffffffcU;

enum class OverlayMode : std::uint8_t {
	Invalid = 0,
	Client = 1,
};

enum OverlayCapabilityBit : std::uint32_t {
	OverlayCapabilityBitNone = 0,
	OverlayCapabilityBitTargetIdentity = 0x00000001U,
	OverlayCapabilityBitDistanceAndSpeed = 0x00000002U,
	OverlayCapabilityBitHullAndSubsystem = 0x00000004U,
	OverlayCapabilityBitIffAndBrackets = 0x00000008U,
	OverlayCapabilityBitAuxiliaryGauges = 0x00000010U,
};
constexpr std::uint32_t KnownOverlayCapabilityBits = 0x0000001fU;
constexpr std::uint32_t ReservedOverlayCapabilityBits = 0xffffffe0U;
constexpr std::uint32_t RequiredOverlayCapabilityBits = 0x0000000fU;

enum class RecoveryMode : std::uint8_t {
	Invalid = 0,
	IdrSelectiveRetransmit = 1,
};

enum VideoFrameFlag : std::uint16_t {
	VideoFrameFlagNone = 0,
	VideoFrameFlagIdr = 0x0001,
	VideoFrameFlagDiscontinuity = 0x0002,
	VideoFrameFlagTargetChanged = 0x0004,
};
constexpr std::uint16_t KnownVideoFrameFlags = 0x0007;
constexpr std::uint16_t ReservedVideoFrameFlags = 0xfff8;

enum class VideoKeyframeReason : std::uint8_t {
	PacketLoss = 1,
	DecoderError = 2,
	LateJoin = 3,
	ConfigChanged = 4,
	IdrRecoveryExpired = 5,
};

enum class VideoStopReason : std::uint8_t {
	ClientUnsubscribe = 1,
	NoTarget = 2,
	UnsupportedTarget = 3,
	MissionChanged = 4,
	SessionStopped = 5,
	CapabilityWithdrawn = 6,
	EncoderFailed = 7,
	RendererUnavailable = 8,
	ResourceLimit = 9,
};

enum VideoStopFlag : std::uint8_t {
	VideoStopFlagNone = 0,
	VideoStopFlagConfirmation = 0x01,
};
constexpr std::uint8_t KnownVideoStopFlags = 0x01;
constexpr std::uint8_t ReservedVideoStopFlags = 0xfe;

enum VideoStatsFlag : std::uint16_t {
	VideoStatsFlagNone = 0,
	VideoStatsFlagDecoderStalled = 0x0001,
	VideoStatsFlagDisplayStale = 0x0002,
	VideoStatsFlagClientOverloaded = 0x0004,
};
constexpr std::uint16_t KnownVideoStatsFlags = 0x0007;
constexpr std::uint16_t ReservedVideoStatsFlags = 0xfff8;

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
constexpr std::uint8_t KnownAckFlags = 0x03;
constexpr std::uint8_t ReservedAckFlags = 0xfc;

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

// Closed bitmaps reject unknown bits by default. These six negotiated bitmaps
// are explicitly extensible and ignore unknown bits on receipt.
template <typename Bitmap>
constexpr bool ignores_unknown_bitmap_bits(Bitmap) noexcept {
	return false;
}

constexpr bool ignores_unknown_bitmap_bits(Capability) noexcept {
	return true;
}

constexpr bool ignores_unknown_bitmap_bits(DeliveredFormatBit) noexcept {
	return true;
}

constexpr bool ignores_unknown_bitmap_bits(H264ProfileBit) noexcept {
	return true;
}

constexpr bool ignores_unknown_bitmap_bits(H264LevelBit) noexcept {
	return true;
}

constexpr bool ignores_unknown_bitmap_bits(RenderProfileBit) noexcept {
	return true;
}

constexpr bool ignores_unknown_bitmap_bits(OverlayCapabilityBit) noexcept {
	return true;
}

template <typename Bitmap>
constexpr bool rejects_unknown_bitmap_bits(Bitmap bitmap) noexcept {
	return !ignores_unknown_bitmap_bits(bitmap);
}

constexpr bool is_known_message_type(MessageType type) noexcept {
	return type >= MessageType::Discovery && type <= MessageType::CapabilityUpdate;
}

constexpr bool is_video_message(MessageType type) noexcept {
	return type == MessageType::TargetVideoFrame;
}

constexpr std::size_t max_message_size(MessageType type) noexcept {
	return !is_known_message_type(type) ? 0 : (is_video_message(type) ? MaxVideoMessageSize : MaxStateMessageSize);
}

constexpr std::size_t max_fragment_count(MessageType type) noexcept {
	return !is_known_message_type(type) ? 0 : (is_video_message(type) ? MaxVideoFragments : MaxStateFragments);
}

} // namespace telemetry::protocol
