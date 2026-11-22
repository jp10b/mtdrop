import React from "react";
import { useCurrentFrame, useVideoConfig, interpolate } from "remotion";
import { theme } from "../theme";
import { presence, sp, tween } from "../anim";
import { f, T, MONITOR, SCREEN, TASKBAR_H, PC } from "../timeline";
import { Photo, TrayGlyph } from "./common";

const C = theme.colors;
const win: React.CSSProperties = { fontFamily: theme.fonts.windows, color: C.cardText };

/** Card in the style of the app's PromptWindow / NoticeWindow (dark theme). */
const Card: React.FC<{ p: number; children: React.ReactNode }> = ({ p, children }) => (
  <div style={{
    ...win, position: "absolute", right: SCREEN.w - PC.card.right, bottom: SCREEN.h - PC.card.bottom, width: PC.card.w,
    background: C.card, border: `1px solid ${C.edge}`, borderRadius: 8, padding: "14px 16px",
    boxShadow: "0 10px 28px rgba(0,0,0,0.45)", opacity: p,
    transform: `translateY(${interpolate(p, [0, 1], [24, 0])}px) scale(${interpolate(p, [0, 1], [0.96, 1])})`,
  }}>{children}</div>
);

const Btn: React.FC<{ label: string; accent?: boolean; w: number }> = ({ label, accent, w }) => (
  <div style={{
    width: w, height: 32, borderRadius: 4, display: "flex", alignItems: "center", justifyContent: "center",
    fontSize: 15, fontWeight: accent ? 600 : 400,
    background: accent ? C.hero : C.button, color: accent ? C.heroText : C.cardText,
  }}>{label}</div>
);

// Button centres, screen coordinates (cards are right/bottom anchored: 16 px padding, 14 px bottom padding)
const BTN_Y = PC.card.bottom - 14 - 16;
export const ACCEPT = { x: PC.card.right - 16 - 48, y: BTN_Y };
export const OPEN_FOLDER = { x: PC.card.right - 16 - 80 - 8 - 60, y: BTN_Y };
export const MENU_START = { x: PC.menu.right - PC.menu.w + 100, y: PC.menu.bottom - 4 - 34 * 3 - 9 + 17 };

export const PcScreen: React.FC = () => {
  const frame = useCurrentFrame();
  const { fps } = useVideoConfig();

  const menuP = presence(frame, fps, f(T.menuOpen), f(T.startClick) + 2);
  const tooltipP = presence(frame, fps, f(T.trayHover), f(T.trayClick) - 2);
  const on = frame >= f(T.receivingOn) && frame < f(T.sessionEnd);
  const tipP = presence(frame, fps, f(T.receivingOn) + 3, f(T.tipOut));
  const promptP = presence(frame, fps, f(T.promptIn), f(T.promptOut));
  const noticeP = presence(frame, fps, f(T.noticeIn), f(T.noticeOut));
  const folderP = presence(frame, fps, f(T.folderIn), f(T.outro), theme.spring.smooth);
  const tileP = sp(frame, f(T.tileIn), fps, theme.spring.bouncy);
  const iconPop = on ? 1 + 0.25 * Math.max(0, 1 - (frame - f(T.receivingOn)) / 8) : 1;
  const countdown = 30 - Math.floor(Math.max(0, frame - f(T.promptIn)) / fps);

  const items = ["Start receiving (2 min)", "Send files to an iPhone...", "Exit"];

  return (
    <div style={{ position: "absolute", left: SCREEN.x, top: SCREEN.y, width: SCREEN.w, height: SCREEN.h,
      overflow: "hidden", borderRadius: 4,
      background: "radial-gradient(120% 90% at 30% 20%, #26408B 0%, #142352 45%, #0B1433 100%)" }}>
      {/* wallpaper bloom */}
      <div style={{ position: "absolute", left: 210, top: 40, width: 300, height: 300, borderRadius: "50%",
        background: "radial-gradient(circle, rgba(96,205,255,0.25), transparent 65%)" }} />

      {/* Downloads folder window */}
      {folderP > 0.001 && (
        <div style={{ ...win, position: "absolute", left: PC.folder.x, top: PC.folder.y, width: PC.folder.w, height: PC.folder.h,
          background: "#202020", border: `1px solid ${C.edge}`, borderRadius: 8, overflow: "hidden",
          boxShadow: "0 18px 40px rgba(0,0,0,0.5)", opacity: folderP,
          transform: `translateY(${interpolate(folderP, [0, 1], [30, 0])}px) scale(${interpolate(folderP, [0, 1], [0.94, 1])})` }}>
          <div style={{ height: 32, display: "flex", alignItems: "center", padding: "0 12px", fontSize: 12, background: "#2B2B2B" }}>
            <span style={{ flex: 1 }}>Downloads</span>
            <span style={{ color: C.muted, letterSpacing: 14 }}>– ▢ ✕</span>
          </div>
          <div style={{ height: 30, display: "flex", alignItems: "center", padding: "0 12px", fontSize: 12, color: C.muted,
            borderBottom: `1px solid ${C.separator}` }}>This PC  ›  Downloads</div>
          <div style={{ display: "flex", gap: 14, padding: 16 }}>
            {[0, 1].map((i) => (
              <div key={i} style={{ width: 96, display: "flex", flexDirection: "column", alignItems: "center", gap: 6, opacity: 0.55 }}>
                <div style={{ width: 62, height: 76, borderRadius: 4, background: "#3A3A3A", border: "1px solid #4A4A4A" }} />
                <div style={{ height: 6, width: 64, borderRadius: 3, background: "#4A4A4A" }} />
              </div>
            ))}
            <div style={{ width: 128, padding: 6, borderRadius: 6, display: "flex", flexDirection: "column", alignItems: "center", gap: 6,
              background: `rgba(96,205,255,${0.16 * tileP})`, border: `1px solid rgba(96,205,255,${0.5 * tileP})`,
              opacity: tileP, transform: `scale(${interpolate(tileP, [0, 1], [0.6, 1])})` }}>
              <div style={{ position: "relative", width: 112, height: 84, borderRadius: 4, overflow: "hidden" }}>
                <Photo start={f(T.tileIn)} span={f(4)} zoomFrom={1.05} zoomTo={1.12} />
              </div>
              <div style={{ fontSize: 12 }}>IMG_2048.jpg</div>
            </div>
          </div>
        </div>
      )}

      {/* hover tooltip over the tray icon */}
      {tooltipP > 0.001 && (
        <div style={{ ...win, position: "absolute", left: PC.trayIcon.x - 34, bottom: TASKBAR_H + 8, padding: "4px 10px", fontSize: 14,
          background: C.menu, border: "1px solid #3F3F3F", borderRadius: 6, opacity: tooltipP,
          transform: `translateY(${interpolate(tooltipP, [0, 1], [6, 0])}px)` }}>mtdrop</div>
      )}

      {/* tray menu */}
      {menuP > 0.001 && (
        <div style={{ ...win, position: "absolute", right: SCREEN.w - PC.menu.right, bottom: SCREEN.h - PC.menu.bottom, width: PC.menu.w,
          background: C.menu, border: "1px solid #3F3F3F", borderRadius: 8, padding: 4, fontSize: 15,
          boxShadow: "0 10px 24px rgba(0,0,0,0.45)", opacity: menuP, transformOrigin: "80% 100%",
          transform: `translateY(${interpolate(menuP, [0, 1], [10, 0])}px) scale(${interpolate(menuP, [0, 1], [0.95, 1])})` }}>
          {items.map((it, i) => {
            const hover = i === 0 ? tween(frame, f(T.startClick) - 14, f(T.startClick) - 8, 0, 1) : 0;
            return (
              <React.Fragment key={it}>
                {i === 2 && <div style={{ height: 1, background: "#454545", margin: "4px 8px" }} />}
                <div style={{ height: 34, display: "flex", alignItems: "center", padding: "0 12px", borderRadius: 4,
                  background: `rgba(255,255,255,${0.08 * hover})` }}>{it}</div>
              </React.Fragment>
            );
          })}
        </div>
      )}

      {/* "receiving" tip */}
      {tipP > 0.001 && (
        <Card p={tipP}>
          <div style={{ display: "flex", alignItems: "center", gap: 10 }}>
            <TrayGlyph color={C.on} size={24} />
            <div>
              <div style={{ fontSize: 17, fontWeight: 600 }}>Receiving</div>
              <div style={{ fontSize: 14, color: C.muted, marginTop: 3 }}>Your PC is visible to iPhones nearby for 2 min</div>
            </div>
          </div>
        </Card>
      )}

      {/* Accept / Decline prompt (PromptWindow.xaml) */}
      {promptP > 0.001 && (
        <Card p={promptP}>
          <div style={{ fontSize: 17 }}><b style={{ fontWeight: 600 }}>iPhone</b> wants to send you a file</div>
          <div style={{ fontSize: 15, marginTop: 5 }}>IMG_2048.jpg, 2.4 MB</div>
          <div style={{ fontSize: 13, marginTop: 5, color: C.muted }}>Sender name is not verified</div>
          <div style={{ fontSize: 13, marginTop: 10, color: C.muted }}>Declines automatically in {countdown} s</div>
          <div style={{ display: "flex", justifyContent: "flex-end", gap: 8, marginTop: 10 }}>
            <Btn label="Decline" w={96} />
            <Btn label="Accept" w={96} accent />
          </div>
        </Card>
      )}

      {/* Notice after the transfer (NoticeWindow.xaml) */}
      {noticeP > 0.001 && (
        <Card p={noticeP}>
          <div style={{ fontSize: 17, fontWeight: 600 }}>Received from iPhone</div>
          <div style={{ fontSize: 15, marginTop: 5 }}>IMG_2048.jpg saved to Downloads</div>
          <div style={{ display: "flex", justifyContent: "flex-end", gap: 8, marginTop: 12 }}>
            <Btn label="Open folder" w={120} accent />
            <Btn label="Close" w={80} />
          </div>
        </Card>
      )}

      {/* taskbar */}
      <div style={{ ...win, position: "absolute", left: 0, right: 0, bottom: 0, height: TASKBAR_H, background: C.taskbar,
        borderTop: "1px solid rgba(255,255,255,0.06)", display: "flex", alignItems: "center", justifyContent: "center", gap: 10 }}>
        {[0, 1, 2, 3, 4].map((i) => (
          <div key={i} style={{ width: 20, height: 20, borderRadius: 5,
            background: i === 0 ? "linear-gradient(135deg,#4CC2FF,#0067C0)" : "rgba(255,255,255,0.16)" }} />
        ))}
        <div style={{ position: "absolute", left: PC.trayIcon.x - 10, top: TASKBAR_H / 2 - 10, width: 20, height: 20,
          display: "flex", alignItems: "center", justifyContent: "center", transform: `scale(${iconPop})` }}>
          <TrayGlyph color={on ? C.on : C.off} size={18} />
        </div>
        <svg width="16" height="16" viewBox="0 0 16 16" style={{ position: "absolute", left: PC.trayIcon.x + 18, top: TASKBAR_H / 2 - 8, opacity: 0.85 }}>
          <path d="M1 6 A10 10 0 0 1 15 6" stroke="#ddd" strokeWidth="1.6" fill="none" />
          <path d="M3.5 8.8 A6.5 6.5 0 0 1 12.5 8.8" stroke="#ddd" strokeWidth="1.6" fill="none" />
          <circle cx="8" cy="12" r="1.6" fill="#ddd" />
        </svg>
        <div style={{ position: "absolute", right: 12, fontSize: 11, textAlign: "right", lineHeight: 1.25, color: "#E6E6E6" }}>
          14:30<br />22.11.2026
        </div>
      </div>
    </div>
  );
};

/** Monitor bezel and stand; the screen is drawn on top by PcScreen. */
export const Monitor: React.FC<{ p: number }> = ({ p }) => (
  <div style={{ position: "absolute", inset: 0, opacity: p,
    transform: `translateY(${interpolate(p, [0, 1], [30, 0])}px) scale(${interpolate(p, [0, 1], [0.96, 1])})`,
    transformOrigin: `${MONITOR.x + MONITOR.w / 2}px ${MONITOR.y + MONITOR.h / 2}px` }}>
    <div style={{ position: "absolute", left: MONITOR.x + MONITOR.w / 2 - 40, top: MONITOR.y + MONITOR.h - 4, width: 80, height: 70,
      background: "linear-gradient(180deg,#22252E,#15171D)" }} />
    <div style={{ position: "absolute", left: MONITOR.x + MONITOR.w / 2 - 130, top: MONITOR.y + MONITOR.h + 62, width: 260, height: 12,
      borderRadius: 6, background: "#1B1E26", boxShadow: "0 12px 30px rgba(0,0,0,0.5)" }} />
    <div style={{ position: "absolute", left: MONITOR.x, top: MONITOR.y, width: MONITOR.w, height: MONITOR.h, borderRadius: 14,
      background: theme.colors.bezel, border: `1px solid ${theme.colors.bezelEdge}`, boxShadow: "0 30px 70px -20px rgba(0,0,0,0.7)" }} />
    <PcScreen />
  </div>
);
