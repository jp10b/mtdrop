import React from "react";
import { AbsoluteFill, useCurrentFrame, useVideoConfig, interpolate } from "remotion";
import { theme } from "./theme";
import { sp, tween } from "./anim";
import { f, T, CAPTIONS, SCREEN, PHONE_SCREEN, PC } from "./timeline";
import { Backdrop, Cursor, Photo, Vignette } from "./parts/common";
import { Monitor, ACCEPT, OPEN_FOLDER, MENU_START } from "./parts/Pc";
import { Phone, PC_TAP } from "./parts/Phone";

const C = theme.colors;

/** Step caption at the top: progress dots, then the sentence word by word with one phrase in the hero colour. */
const Caption: React.FC = () => {
  const frame = useCurrentFrame();
  const { fps } = useVideoConfig();
  const idx = CAPTIONS.reduce((k, c, i) => (frame >= f(c.at) ? i : k), -1);
  if (idx < 0) return null;
  const cap = CAPTIONS[idx];
  const next = CAPTIONS[idx + 1];
  const exitAt = next ? f(next.at) - 8 : f(T.outro);
  const exitP = tween(frame, exitAt, exitAt + 8, 0, 1, theme.ease.in);

  const words = cap.text.split(" ");
  const hl = cap.highlight.split(" ");
  const hlStart = words.findIndex((_, i) => hl.every((h, j) => words[i + j] === h));

  return (
    <div style={{ position: "absolute", left: 0, right: 0, top: 30, display: "flex", flexDirection: "column", alignItems: "center", gap: 14,
      fontFamily: theme.fonts.ui }}>
      <div style={{ display: "flex", gap: 8, opacity: next ? 1 : 1 - exitP }}>
        {CAPTIONS.map((_, i) => {
          const p = sp(frame, f(CAPTIONS[i].at), fps, theme.spring.snappy);
          const active = i === idx;
          const done = i < idx;
          return (
            <div key={i} style={{ height: 6, borderRadius: 3, width: active ? 10 + 22 * p : 10,
              background: active ? C.text : done ? "rgba(243,244,246,0.55)" : "rgba(243,244,246,0.18)" }} />
          );
        })}
      </div>
      <div style={{ display: "flex", alignItems: "center", gap: 14, opacity: 1 - exitP,
        transform: `translateY(${-14 * exitP}px)` }}>
        <div style={{ width: 34, height: 34, borderRadius: 17, border: "1.5px solid rgba(243,244,246,0.35)", color: C.text,
          display: "flex", alignItems: "center", justifyContent: "center", fontSize: 16, fontWeight: 600,
          opacity: sp(frame, f(cap.at), fps, theme.spring.snappy),
          transform: `scale(${interpolate(sp(frame, f(cap.at), fps, theme.spring.bouncy), [0, 1], [0.6, 1])})` }}>{idx + 1}</div>
        <div style={{ display: "flex", flexWrap: "wrap", gap: 9, fontSize: 30, fontWeight: 600, letterSpacing: "-0.02em", color: C.text }}>
          {words.map((w, i) => {
            const p = sp(frame, f(cap.at) + 3 + i * 2, fps, theme.spring.snappy);
            const isHl = i >= hlStart && i < hlStart + hl.length;
            return (
              <span key={i} style={{ display: "inline-block", opacity: p, color: isHl ? C.hero : C.text,
                transform: `translateY(${interpolate(p, [0, 1], [18, 0])}px)` }}>{w}</span>
            );
          })}
        </div>
      </div>
    </div>
  );
};

type Key = { t: number; x: number; y: number };
const sx = (x: number) => SCREEN.x + x;
const sy = (y: number) => SCREEN.y + y;
const rest = { x: sx(300), y: sy(230) };
const PATH: Key[] = [
  { t: 0, x: sx(380), y: sy(190) },
  { t: 0.5, x: sx(380), y: sy(190) },
  { t: 1.1, x: sx(PC.trayIcon.x), y: sy(PC.trayIcon.y) },
  { t: 1.95, x: sx(PC.trayIcon.x), y: sy(PC.trayIcon.y) },
  { t: 2.5, x: sx(MENU_START.x), y: sy(MENU_START.y) },
  { t: 3.0, x: sx(MENU_START.x), y: sy(MENU_START.y) },
  { t: 3.6, ...rest },
  { t: 9.3, ...rest },
  { t: 10.0, x: sx(ACCEPT.x), y: sy(ACCEPT.y) },
  { t: 11.6, x: sx(ACCEPT.x), y: sy(ACCEPT.y) },
  { t: 12.3, x: sx(OPEN_FOLDER.x), y: sy(OPEN_FOLDER.y) },
  { t: 12.9, x: sx(OPEN_FOLDER.x), y: sy(OPEN_FOLDER.y) },
  { t: 13.6, x: sx(560), y: sy(300) },
  { t: 16.9, x: sx(560), y: sy(300) },
  { t: 17.8, x: sx(380), y: sy(190) },
];

const cursorAt = (frame: number) => {
  const k = PATH.findIndex((p, i) => i === PATH.length - 1 || frame < f(PATH[i + 1].t));
  const a = PATH[k];
  const b = PATH[Math.min(k + 1, PATH.length - 1)];
  const x = tween(frame, f(a.t), f(b.t), a.x, b.x, theme.ease.move);
  const y = tween(frame, f(a.t), f(b.t), a.y, b.y, theme.ease.move);
  return { x, y };
};

/** During the upload, a small copy of the photo travels from the phone to the PC along an arc. */
const FlyingPhoto: React.FC = () => {
  const frame = useCurrentFrame();
  const a = f(T.promptOut) + 4;
  const b = f(T.sendDone);
  if (frame < a || frame > b + 6) return null;
  const t = tween(frame, a, b, 0, 1, theme.ease.inOut);
  const from = { x: PHONE_SCREEN.x + PC_TAP.x, y: PHONE_SCREEN.y + PC_TAP.y };
  const to = { x: sx(PC.card.right - 120), y: sy(PC.card.bottom - 70) };
  const x = from.x + (to.x - from.x) * t;
  const y = from.y + (to.y - from.y) * t - Math.sin(Math.PI * t) * 120;
  const o = Math.min(tween(frame, a, a + 5, 0, 1), tween(frame, b, b + 6, 1, 0, theme.ease.in));
  const s = 0.7 + Math.sin(Math.PI * t) * 0.45;
  return (
    <div style={{ position: "absolute", left: x - 40, top: y - 30, width: 80, height: 60, borderRadius: 8, overflow: "hidden",
      opacity: o, transform: `scale(${s}) rotate(${(1 - t) * -8}deg)`, border: "2px solid rgba(255,255,255,0.85)",
      boxShadow: `0 12px 30px rgba(0,0,0,0.5), 0 0 40px ${C.hero}55` }}>
      <Photo start={a} span={b - a} />
    </div>
  );
};

export const HowItWorks: React.FC = () => {
  const frame = useCurrentFrame();
  // Devices stay on screen for the whole loop, so the GIF's first frame (its still preview) is not empty.
  const cur = cursorAt(frame);

  return (
    <AbsoluteFill style={{ background: C.bg }}>
      <Backdrop />
      <AbsoluteFill>
        <Monitor p={1} />
        <Phone p={1} />
        <FlyingPhoto />
        <Caption />
      </AbsoluteFill>
      <Cursor x={cur.x} y={cur.y} opacity={1}
        clicks={[f(T.trayClick), f(T.startClick), f(T.acceptClick), f(T.openClick)]} />
      <Vignette />
    </AbsoluteFill>
  );
};

