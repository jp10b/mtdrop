import React from "react";
import { AbsoluteFill, useCurrentFrame, useVideoConfig } from "remotion";
import { theme } from "../theme";
import { sp, tween } from "../anim";

/** The photo that travels from the iPhone to the PC: drawn, not a real picture. Slow Ken Burns. */
export const Photo: React.FC<{ zoomFrom?: number; zoomTo?: number; start?: number; span?: number; loop?: boolean }> = ({
  zoomFrom = 1, zoomTo = 1.08, start = 0, span = 300, loop = false,
}) => {
  const frame = useCurrentFrame();
  const { durationInFrames } = useVideoConfig();
  // loop: a slow zoom in and out that returns to its start at the end of the composition
  const wave = (1 - Math.cos((2 * Math.PI * frame) / durationInFrames)) / 2;
  const z = loop ? 1.02 + 0.08 * wave : tween(frame, start, start + span, zoomFrom, zoomTo, theme.ease.inOut);
  const pan = loop ? -10 * wave : tween(frame, start, start + span, 0, -10, theme.ease.inOut);
  return (
    <div style={{ position: "absolute", inset: 0, overflow: "hidden" }}>
      <svg viewBox="0 0 400 300" preserveAspectRatio="xMidYMid slice"
        style={{ width: "100%", height: "100%", transform: `scale(${z}) translateX(${pan}px)` }}>
        <defs>
          <linearGradient id="sky" x1="0" y1="0" x2="0" y2="1">
            <stop offset="0" stopColor="#3B2F6B" />
            <stop offset="0.45" stopColor="#C9637A" />
            <stop offset="0.75" stopColor="#F6A55E" />
          </linearGradient>
          <linearGradient id="lake" x1="0" y1="0" x2="0" y2="1">
            <stop offset="0" stopColor="#E98B66" />
            <stop offset="1" stopColor="#2A2350" />
          </linearGradient>
          <radialGradient id="sun">
            <stop offset="0" stopColor="#FFF4D6" />
            <stop offset="0.6" stopColor="#FFD38A" />
            <stop offset="1" stopColor="#FFD38A" stopOpacity="0" />
          </radialGradient>
        </defs>
        <rect width="400" height="300" fill="url(#sky)" />
        <circle cx="250" cy="178" r="70" fill="url(#sun)" />
        <circle cx="250" cy="178" r="26" fill="#FFF1CF" />
        <polygon points="0,200 60,140 110,180 170,110 240,190 300,130 360,170 400,150 400,220 0,220" fill="#5B3F78" />
        <polygon points="0,215 50,175 120,205 190,160 260,210 330,170 400,200 400,230 0,230" fill="#3A2B5C" />
        <rect y="222" width="400" height="78" fill="url(#lake)" />
        <rect x="236" y="226" width="28" height="3" rx="1.5" fill="#FFE7B8" opacity="0.8" />
        <rect x="228" y="236" width="44" height="2.5" rx="1.25" fill="#FFE7B8" opacity="0.55" />
        <rect x="240" y="246" width="20" height="2" rx="1" fill="#FFE7B8" opacity="0.4" />
      </svg>
    </div>
  );
};

/** mtdrop's tray icon (App.xaml.cs MakeIcon): two arcs and a dot. */
export const TrayGlyph: React.FC<{ color: string; size?: number }> = ({ color, size = 18 }) => (
  <svg width={size} height={size} viewBox="0 0 32 32">
    <path d="M 4.2 11.4 A 13 13 0 0 1 27.8 11.4" fill="none" stroke={color} strokeWidth="3.2" strokeLinecap="round" />
    <path d="M 10 13.6 A 7 7 0 0 1 22 13.6" fill="none" stroke={color} strokeWidth="3.2" strokeLinecap="round" />
    <circle cx="16" cy="22" r="3" fill={color} />
  </svg>
);

/** Windows arrow cursor with a click ripple. Position in composition coordinates. */
export const Cursor: React.FC<{ x: number; y: number; clicks: number[]; opacity: number }> = ({ x, y, clicks, opacity }) => {
  const frame = useCurrentFrame();
  const { fps } = useVideoConfig();
  const press = clicks.reduce((m, c) => Math.max(m, tween(frame, c - 2, c, 0, 1) * tween(frame, c + 1, c + 6, 1, 0)), 0);
  return (
    <div style={{ position: "absolute", left: 0, top: 0, opacity, pointerEvents: "none" }}>
      {clicks.map((c) => {
        const p = sp(frame, c, fps, theme.spring.smooth);
        const o = tween(frame, c, c + Math.round(fps * 0.45), 0.9, 0);
        if (frame < c) return null;
        return (
          <div key={c} style={{
            position: "absolute", left: x - 22, top: y - 22, width: 44, height: 44, borderRadius: 22,
            border: `2px solid ${theme.colors.hero}`, opacity: o, transform: `scale(${0.3 + p * 0.9})`,
          }} />
        );
      })}
      <svg width="22" height="30" viewBox="0 0 22 30"
        style={{ position: "absolute", left: x - 2, top: y - 1, transform: `scale(${1 - press * 0.12})`, transformOrigin: "2px 1px",
          filter: "drop-shadow(0 2px 3px rgba(0,0,0,0.45))" }}>
        <path d="M2 1 L2 24 L7.5 18.6 L11.6 27.6 L15.4 25.9 L11.3 17.2 L19 17.2 Z" fill="#FFFFFF" stroke="#000000" strokeWidth="1.4" strokeLinejoin="round" />
      </svg>
    </div>
  );
};

/** A finger tap on the phone: a soft disc that grows and fades. */
export const Tap: React.FC<{ x: number; y: number; at: number }> = ({ x, y, at }) => {
  const frame = useCurrentFrame();
  const { fps } = useVideoConfig();
  if (frame < at - 4) return null;
  const inP = tween(frame, at - 4, at, 0, 1);
  const outP = tween(frame, at + 3, at + Math.round(fps * 0.4), 1, 0, theme.ease.in);
  const grow = sp(frame, at, fps, theme.spring.smooth);
  return (
    <div style={{
      position: "absolute", left: x - 26, top: y - 26, width: 52, height: 52, borderRadius: 26,
      background: "rgba(255,255,255,0.35)", border: "2px solid rgba(255,255,255,0.75)",
      opacity: Math.min(inP, outP), transform: `scale(${0.7 + grow * 0.35})`,
    }} />
  );
};

/** Background: deep gradient with two soft colour fields (static: the GIF stays small). */
export const Backdrop: React.FC = () => (
  <AbsoluteFill style={{ background: `linear-gradient(160deg, ${theme.colors.bgAlt} 0%, ${theme.colors.bg} 60%)` }}>
    <div style={{ position: "absolute", width: 900, height: 900, left: -260, top: -420, borderRadius: "50%",
      background: `radial-gradient(circle, ${theme.colors.hero}22, transparent 62%)` }} />
    <div style={{ position: "absolute", width: 800, height: 800, right: -300, bottom: -420, borderRadius: "50%",
      background: "radial-gradient(circle, #C9637A1c, transparent 64%)" }} />
  </AbsoluteFill>
);

export const Vignette: React.FC = () => (
  <AbsoluteFill style={{ pointerEvents: "none",
    background: "radial-gradient(ellipse at center, transparent 60%, rgba(0,0,0,0.35) 100%)" }} />
);
