export interface AttitudeAngles {
  headingDeg: number;
  pitchDeg: number;
  rollDeg: number;
  quaternion: [number, number, number, number];
}

export interface TrajectoryMarker {
  yawDeg: number;
  pitchDeg: number;
  driftDeg: number;
  xPercent: number;
  yPercent: number;
}

const RAD_TO_DEG = 180 / Math.PI;
const EPSILON = 1e-6;

function finiteVector(value: unknown, length: number): number[] | null {
  if (!Array.isArray(value) || value.length !== length) return null;
  const result = value.map(Number);
  return result.every(Number.isFinite) ? result : null;
}

export function normalizeQuaternion(value: unknown): [number, number, number, number] | null {
  const components = finiteVector(value, 4);
  if (!components) return null;
  const norm = Math.hypot(...components);
  if (!Number.isFinite(norm) || norm < EPSILON) return null;
  const normalized = components.map((component) => component / norm) as [number, number, number, number];
  if (normalized[0] < 0) return normalized.map((component) => -component) as [number, number, number, number];
  return normalized;
}

export function rotateVector(
  quaternion: [number, number, number, number],
  vector: [number, number, number]
): [number, number, number] {
  const [w, x, y, z] = quaternion;
  const [vx, vy, vz] = vector;
  const tx = 2 * (y * vz - z * vy);
  const ty = 2 * (z * vx - x * vz);
  const tz = 2 * (x * vy - y * vx);
  return [
    vx + w * tx + (y * tz - z * ty),
    vy + w * ty + (z * tx - x * tz),
    vz + w * tz + (x * ty - y * tx)
  ];
}

function normalizedDegrees(value: number): number {
  const normalized = ((value + 180) % 360 + 360) % 360 - 180;
  return Object.is(normalized, -0) ? 0 : normalized;
}

export function attitudeFromQuaternion(value: unknown): AttitudeAngles | null {
  const quaternion = normalizeQuaternion(value);
  if (!quaternion) return null;
  const forward = rotateVector(quaternion, [0, 0, 1]);
  const right = rotateVector(quaternion, [1, 0, 0]);
  const up = rotateVector(quaternion, [0, 1, 0]);
  const pitch = Math.asin(Math.max(-1, Math.min(1, forward[1])));
  const heading = Math.atan2(forward[0], forward[2]);
  const roll = Math.atan2(right[1], up[1]);
  return {
    headingDeg: normalizedDegrees(heading * RAD_TO_DEG),
    pitchDeg: pitch * RAD_TO_DEG,
    rollDeg: normalizedDegrees(roll * RAD_TO_DEG),
    quaternion
  };
}

export function trajectoryFromLocalVelocity(value: unknown, fieldOfViewDeg = 45): TrajectoryMarker | null {
  const velocity = finiteVector(value, 3);
  if (!velocity) return null;
  const [x, y, z] = velocity;
  const speed = Math.hypot(x, y, z);
  if (speed < EPSILON) return null;
  const yaw = Math.atan2(x, z) * RAD_TO_DEG;
  const pitch = Math.atan2(y, Math.hypot(x, z)) * RAD_TO_DEG;
  const drift = Math.acos(Math.max(-1, Math.min(1, z / speed))) * RAD_TO_DEG;
  return {
    yawDeg: normalizedDegrees(yaw),
    pitchDeg: pitch,
    driftDeg: drift,
    xPercent: Math.max(-100, Math.min(100, (yaw / fieldOfViewDeg) * 100)),
    yPercent: Math.max(-100, Math.min(100, (-pitch / fieldOfViewDeg) * 100))
  };
}
