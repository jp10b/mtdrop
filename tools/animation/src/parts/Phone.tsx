import React from "react";
import { useCurrentFrame, useVideoConfig, interpolate } from "remotion";
import { theme } from "../theme";
import { sp, tween } from "../anim";
import { f, T, PHONE, PHONE_SCREEN, IPHONE } from "../timeline";
import { Photo, Tap } from "./common";

const C = theme.colors;
const W = PHONE_SCREEN.w;
const H = PHONE_SCREEN.h;
const ui: React.CSSProperties = { fontFamily: theme.fonts.ui, color: "#FFFFFF" };

const OPTIONS = ["Receiving Off", "Contacts Only", "Everyone for 10 Minutes"];
const optionY = (i: number) => IPHONE.settingsTop + 64 + i * IPHONE.rowH + IPHONE.rowH / 2;
export const EVERYONE_TAP = { x: W - 34, y: optionY(2) };
const SHARE_BTN = { x: 34, y: H - 50 };
const BUBBLE = { x: 54, y: IPHONE.shareTop + 132 }; // centre of the PC's bubble in the share sheet
export const PC_TAP = BUBBLE;

/** Sheet sliding up from the bottom of the phone screen, faster on the way down. */
const sheetY = (frame: number, fps: number, enter: number, exit: number | null, height: number) => {
  const inP = sp(frame, enter, fps, theme.spring.smooth);
  const out = exit === null ? 0 : tween(frame, exit, exit + Math.round(fps * 0.25), 0, 1, theme.ease.in);
  return interpolate(inP, [0, 1], [height, 0]) + out * height;
};

const Laptop: React.FC = () => (
  <svg width="30" height="22" viewBox="0 0 30 22">
    <rect x="4" y="1" width="22" height="15" rx="2" fill="none" stroke="#FFFFFF" strokeWidth="2" />
    <rect x="1" y="17.5" width="28" height="3" rx="1.5" fill="#FFFFFF" />
  </svg>
);

export const PhoneScreen: React.FC = () => {
  const frame = useCurrentFrame();
  const { fps } = useVideoConfig();

  const settingsH = H - IPHONE.settingsTop;
  const shareH = H - IPHONE.shareTop;
  const sY = sheetY(frame, fps, f(T.settingsIn), f(T.settingsOut), settingsH);
  const shY = sheetY(frame, fps, f(T.shareIn), f(T.outro), shareH);
  const dim = Math.max(
    tween(frame, f(T.settingsIn), f(T.settingsIn) + 10, 0, 0.45) * tween(frame, f(T.settingsOut), f(T.settingsOut) + 8, 1, 0),
    tween(frame, f(T.shareIn), f(T.shareIn) + 10, 0, 0.45) * tween(frame, f(T.outro), f(T.outro) + 8, 1, 0),
  );

  // check mark moves from "Contacts Only" to "Everyone for 10 Minutes"
  const moved = sp(frame, f(T.everyoneTap) + 2, fps, theme.spring.snappy);
  const checkY = interpolate(moved, [0, 1], [optionY(1), optionY(2)]);
  const rowFlash = tween(frame, f(T.everyoneTap) - 2, f(T.everyoneTap) + 2, 0, 1) * tween(frame, f(T.everyoneTap) + 6, f(T.everyoneTap) + 16, 1, 0);

  // PC bubble: waiting, then a progress ring, then "Sent"
  const ring = tween(frame, f(T.promptOut), f(T.sendDone), 0, 1, theme.ease.inOut);
  const status = frame < f(T.pcTap) ? "" : frame < f(T.promptOut) ? "Waiting…" : frame < f(T.sendDone) ? "Sending…" : "Sent";
  const statusP = sp(frame, frame < f(T.promptOut) ? f(T.pcTap) : frame < f(T.sendDone) ? f(T.promptOut) : f(T.sendDone), fps, theme.spring.snappy);
  const bubbleBreath = frame >= f(T.pcTap) && frame < f(T.promptOut) ? 1 + Math.sin((frame - f(T.pcTap)) / 5) * 0.03 : 1;
  const sentPop = frame >= f(T.sendDone) ? 1 + 0.12 * Math.max(0, 1 - (frame - f(T.sendDone)) / 8) : 1;

  return (
    <div style={{ ...ui, position: "absolute", left: PHONE_SCREEN.x, top: PHONE_SCREEN.y, width: W, height: H, borderRadius: 34, overflow: "hidden", background: "#000" }}>
      {/* the photo, full screen */}
      <div style={{ position: "absolute", left: 0, right: 0, top: 70, height: 300 }}>
        <Photo loop />
      </div>
      {/* Photos-style header and toolbar */}
      <div style={{ position: "absolute", left: 0, right: 0, top: 44, textAlign: "center" }}>
        <div style={{ fontSize: 14, fontWeight: 600 }}>Today</div>
        <div style={{ fontSize: 11, color: "#8E8E93" }}>14:28</div>
      </div>
      <div style={{ position: "absolute", left: 0, right: 0, bottom: 0, height: 84, display: "flex", alignItems: "flex-start",
        justifyContent: "space-between", padding: "22px 22px 0" }}>
        <svg width="24" height="26" viewBox="0 0 24 26">
          <path d="M12 2 V15 M7 6.5 L12 1.5 L17 6.5" fill="none" stroke={C.hero} strokeWidth="2" strokeLinecap="round" strokeLinejoin="round" />
          <path d="M8 10 H4 V24 H20 V10 H16" fill="none" stroke={C.hero} strokeWidth="2" strokeLinejoin="round" />
        </svg>
        <svg width="22" height="22" viewBox="0 0 22 22"><path d="M11 19 C4 14 1.5 10.5 1.5 7 A4.5 4.5 0 0 1 11 4.5 A4.5 4.5 0 0 1 20.5 7 C20.5 10.5 18 14 11 19 Z" fill="none" stroke="#fff" strokeWidth="1.8" /></svg>
        <svg width="22" height="22" viewBox="0 0 22 22"><circle cx="11" cy="11" r="9" fill="none" stroke="#fff" strokeWidth="1.8" /><path d="M11 10 V16 M11 6.5 V7" stroke="#fff" strokeWidth="2" strokeLinecap="round" /></svg>
        <svg width="20" height="22" viewBox="0 0 20 22"><path d="M2 5 H18 M7 5 V2.5 H13 V5 M4 5 L5.2 20 H14.8 L16 5" fill="none" stroke="#fff" strokeWidth="1.8" strokeLinejoin="round" /></svg>
      </div>
      <div style={{ position: "absolute", inset: 0, background: `rgba(0,0,0,${dim})` }} />

      {/* status bar + island */}
      <div style={{ position: "absolute", left: 24, top: 14, fontSize: 13, fontWeight: 600 }}>9:41</div>
      <div style={{ position: "absolute", left: W / 2 - 44, top: 10, width: 88, height: 25, borderRadius: 13, background: "#000" }} />
      <div style={{ position: "absolute", right: 22, top: 17, width: 22, height: 10, borderRadius: 3, border: "1.5px solid #fff", opacity: 0.9 }}>
        <div style={{ position: "absolute", left: 1.5, top: 1.5, width: 13, height: 4, borderRadius: 1, background: "#fff" }} />
      </div>

      {/* AirDrop setting sheet */}
      {frame >= f(T.settingsIn) - 1 && frame < f(T.settingsOut) + 12 && (
        <div style={{ position: "absolute", left: 0, right: 0, top: IPHONE.settingsTop, height: settingsH, background: C.sheet,
          borderRadius: "18px 18px 0 0", transform: `translateY(${sY}px)` }}>
          <div style={{ width: 36, height: 5, borderRadius: 3, background: "#5A5A5E", margin: "8px auto 0" }} />
          <div style={{ textAlign: "center", fontSize: 16, fontWeight: 600, marginTop: 14 }}>AirDrop</div>
          <div style={{ position: "absolute", left: 14, right: 14, top: 64, borderRadius: 12, background: C.cell, overflow: "hidden" }}>
            {OPTIONS.map((o, i) => (
              <div key={o} style={{ height: IPHONE.rowH, display: "flex", alignItems: "center", padding: "0 14px", fontSize: 14,
                borderTop: i ? `1px solid ${C.separator}` : "none", background: i === 2 ? `rgba(255,255,255,${0.12 * rowFlash})` : "transparent" }}>
                {o}
              </div>
            ))}
          </div>
          <svg width="16" height="14" viewBox="0 0 16 14" style={{ position: "absolute", right: 30, top: checkY - IPHONE.settingsTop - 7 }}>
            <path d="M1.5 7.5 L6 12 L14.5 1.5" fill="none" stroke={C.hero} strokeWidth="2.4" strokeLinecap="round" strokeLinejoin="round" />
          </svg>
        </div>
      )}

      {/* share sheet */}
      {frame >= f(T.shareIn) - 1 && frame < f(T.outro) + 12 && (
        <div style={{ position: "absolute", left: 0, right: 0, top: IPHONE.shareTop, height: shareH, background: C.sheet,
          borderRadius: "18px 18px 0 0", transform: `translateY(${shY}px)` }}>
          <div style={{ display: "flex", alignItems: "center", gap: 10, padding: "14px 16px 12px" }}>
            <div style={{ position: "relative", width: 40, height: 40, borderRadius: 8, overflow: "hidden" }}>
              <Photo start={0} span={f(1)} />
            </div>
            <div>
              <div style={{ fontSize: 14, fontWeight: 600 }}>1 Photo Selected</div>
              <div style={{ fontSize: 11, color: "#8E8E93", marginTop: 2 }}>Share with</div>
            </div>
          </div>
          <div style={{ height: 1, background: C.separator, margin: "0 16px" }} />
          <div style={{ fontSize: 12, color: "#8E8E93", padding: "12px 16px 0" }}>AirDrop</div>

          {/* the PC's bubble */}
          <div style={{ position: "absolute", left: BUBBLE.x - 30, top: BUBBLE.y - IPHONE.shareTop - 30, width: 60, height: 60,
            transform: `scale(${bubbleBreath * sentPop})` }}>
            <div style={{ position: "absolute", inset: 0, borderRadius: 30, background: "linear-gradient(160deg,#5A5F6B,#3A3E48)",
              display: "flex", alignItems: "center", justifyContent: "center" }}>
              <Laptop />
            </div>
            {frame >= f(T.promptOut) && (
              <svg width="66" height="66" viewBox="0 0 66 66" style={{ position: "absolute", left: -3, top: -3, transform: "rotate(-90deg)" }}>
                <circle cx="33" cy="33" r="31" fill="none" stroke={C.hero} strokeWidth="3"
                  strokeDasharray={`${2 * Math.PI * 31}`} strokeDashoffset={`${2 * Math.PI * 31 * (1 - ring)}`} strokeLinecap="round" />
              </svg>
            )}
          </div>
          <div style={{ position: "absolute", left: BUBBLE.x - 46, width: 92, top: BUBBLE.y - IPHONE.shareTop + 38, textAlign: "center" }}>
            <div style={{ fontSize: 11, fontWeight: 600 }}>DESKTOP-PC</div>
            <div style={{ fontSize: 10, color: status === "Sent" ? C.hero : "#8E8E93", marginTop: 2, height: 12,
              opacity: status ? statusP : 0, transform: `translateY(${interpolate(statusP, [0, 1], [4, 0])}px)` }}>{status}</div>
          </div>
          {/* a few blank app icons for context */}
          <div style={{ position: "absolute", left: 16, right: 16, top: 230, display: "flex", justifyContent: "space-between" }}>
            {[0, 1, 2, 3].map((i) => (
              <div key={i} style={{ width: 46, height: 46, borderRadius: 11, background: C.cell }} />
            ))}
          </div>
        </div>
      )}

      <Tap x={EVERYONE_TAP.x} y={EVERYONE_TAP.y} at={f(T.everyoneTap)} />
      <Tap x={SHARE_BTN.x} y={SHARE_BTN.y} at={f(T.shareTap)} />
      <Tap x={PC_TAP.x} y={PC_TAP.y} at={f(T.pcTap)} />
    </div>
  );
};

/** Phone body; the screen is drawn on top by PhoneScreen. */
export const Phone: React.FC<{ p: number }> = ({ p }) => {
  const frame = useCurrentFrame();
  const { durationInFrames } = useVideoConfig();
  const float = Math.sin((2 * Math.PI * 3 * frame) / durationInFrames) * 2.5; // idle breathing, whole cycles per loop
  return (
    <div style={{ position: "absolute", inset: 0, opacity: p,
      transform: `translateY(${interpolate(p, [0, 1], [40, 0]) + float}px) scale(${interpolate(p, [0, 1], [0.94, 1])})`,
      transformOrigin: `${PHONE.x + PHONE.w / 2}px ${PHONE.y + PHONE.h / 2}px` }}>
      <div style={{ position: "absolute", left: PHONE.x, top: PHONE.y, width: PHONE.w, height: PHONE.h, borderRadius: 44,
        background: C.bezel, border: `2px solid ${C.bezelEdge}`, boxShadow: "0 40px 80px -24px rgba(0,0,0,0.75)" }} />
      <PhoneScreen />
    </div>
  );
};
