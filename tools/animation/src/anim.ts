import { interpolate, spring, EasingFunction } from "remotion";
import { theme } from "./theme";

/** Clamped, eased interpolation between two frames. */
export const tween = (frame: number, from: number, to: number, a: number, b: number,
  easing: EasingFunction = theme.ease.out) =>
  to <= from ? (frame < from ? a : b)
    : interpolate(frame, [from, to], [a, b], { easing, extrapolateLeft: "clamp", extrapolateRight: "clamp" });

/** Spring that starts at `at` (frames). */
export const sp = (frame: number, at: number, fps: number,
  config: { damping: number; stiffness: number; mass: number } = theme.spring.smooth) =>
  spring({ frame: frame - at, fps, config });

/** 1 while `frame` is inside [enter, exit), with a spring entrance and a faster eased exit. */
export const presence = (frame: number, fps: number, enter: number, exit: number,
  config: { damping: number; stiffness: number; mass: number } = theme.spring.snappy) => {
  const inP = sp(frame, enter, fps, config);
  const outP = tween(frame, exit, exit + Math.round(fps * 0.25), 1, 0, theme.ease.in);
  return Math.min(inP, outP);
};
