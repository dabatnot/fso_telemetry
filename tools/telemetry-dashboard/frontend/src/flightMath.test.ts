import { describe, expect, it } from "vitest";
import { attitudeFromQuaternion, normalizeQuaternion, trajectoryFromLocalVelocity } from "./flightMath";

const close = (actual: number, expected: number) => expect(actual).toBeCloseTo(expected, 5);
const rotation = (axis: "x" | "y" | "z", degrees: number): [number, number, number, number] => {
  const half = degrees * Math.PI / 360;
  const sine = Math.sin(half);
  return [
    Math.cos(half),
    axis === "x" ? sine : 0,
    axis === "y" ? sine : 0,
    axis === "z" ? sine : 0
  ];
};

describe("attitude quaternion projection", () => {
  it("projects identity without rotation", () => {
    const result = attitudeFromQuaternion([1, 0, 0, 0])!;
    close(result.headingDeg, 0);
    close(result.pitchDeg, 0);
    close(result.rollDeg, 0);
  });

  it("projects known heading, pitch and roll rotations", () => {
    close(attitudeFromQuaternion(rotation("y", 45))!.headingDeg, 45);
    close(attitudeFromQuaternion(rotation("x", 30))!.pitchDeg, -30);
    close(attitudeFromQuaternion(rotation("z", 35))!.rollDeg, 35);
  });

  it("normalizes finite quaternions and rejects invalid input", () => {
    expect(normalizeQuaternion([2, 0, 0, 0])).toEqual([1, 0, 0, 0]);
    expect(normalizeQuaternion([0, 0, 0, 0])).toBeNull();
    expect(normalizeQuaternion([1, 0, Number.NaN, 0])).toBeNull();
    expect(normalizeQuaternion([1, 0, 0])).toBeNull();
  });

  it("remains finite near the pitch singularity", () => {
    const result = attitudeFromQuaternion(rotation("x", -89.999))!;
    expect(Number.isFinite(result.headingDeg)).toBe(true);
    expect(Number.isFinite(result.rollDeg)).toBe(true);
    close(result.pitchDeg, 89.999);
  });
});

describe("local trajectory marker", () => {
  it("separates flight path from the nose during drift", () => {
    const result = trajectoryFromLocalVelocity([10, 0, 10])!;
    close(result.yawDeg, 45);
    close(result.pitchDeg, 0);
    close(result.driftDeg, 45);
    close(result.xPercent, 100);
  });

  it("projects vertical drift and rejects a quasi-zero velocity", () => {
    const result = trajectoryFromLocalVelocity([0, 10, 10])!;
    close(result.pitchDeg, 45);
    close(result.yPercent, -100);
    expect(trajectoryFromLocalVelocity([0, 0, 0])).toBeNull();
  });
});
