// Every moment of the story, in seconds. Components convert with f(): no frame numbers anywhere else.
export const FPS = 30;
export const DURATION_S = 18;
export const f = (seconds: number) => Math.round(seconds * FPS);

export const T = {
  devicesIn: 0.05,

  // 1. PC: tray menu -> Start receiving
  step1: 0.35,
  trayHover: 1.15,
  trayClick: 1.65,
  menuOpen: 1.72,
  startClick: 2.65,
  receivingOn: 2.72,
  tipOut: 4.05,

  // 2. iPhone: AirDrop -> Everyone for 10 Minutes
  step2: 4.0,
  settingsIn: 4.2,
  everyoneTap: 5.25,
  settingsOut: 6.55,

  // 3. iPhone: share the photo, tap the PC
  step3: 6.6,
  shareTap: 6.85,
  shareIn: 7.0,
  pcTap: 8.0,

  // 4. PC: Accept
  step4: 8.8,
  promptIn: 8.9,
  acceptClick: 10.2,
  promptOut: 10.3,
  sendDone: 11.2,

  // 5. PC: the photo is in Downloads
  step5: 11.05,
  noticeIn: 11.3,
  openClick: 12.5,
  noticeOut: 12.6,
  folderIn: 12.7,
  tileIn: 13.05,

  sessionEnd: 16.2, // the session ends a few seconds after the transfer
  outro: 16.9,
} as const;

export const CAPTIONS: { at: number; text: string; highlight: string }[] = [
  { at: T.step1, text: "Click Start receiving in the mtdrop tray menu", highlight: "Start receiving" },
  { at: T.step2, text: "On the iPhone, set AirDrop to Everyone for 10 Minutes", highlight: "Everyone for 10 Minutes" },
  { at: T.step3, text: "Share a photo and tap your PC", highlight: "tap your PC" },
  { at: T.step4, text: "Accept the transfer on the PC", highlight: "Accept" },
  { at: T.step5, text: "The photo is in your Downloads folder", highlight: "Downloads" },
];

// Layout (px, composition coordinates)
export const MONITOR = { x: 60, y: 118, w: 720, h: 440, bezel: 14 };
export const SCREEN = {
  x: MONITOR.x + MONITOR.bezel,
  y: MONITOR.y + MONITOR.bezel,
  w: MONITOR.w - 2 * MONITOR.bezel,
  h: MONITOR.h - 2 * MONITOR.bezel,
};
export const TASKBAR_H = 36;
export const PHONE = { x: 912, y: 108, w: 270, h: 548, bezel: 10 };
export const PHONE_SCREEN = {
  x: PHONE.x + PHONE.bezel,
  y: PHONE.y + PHONE.bezel,
  w: PHONE.w - 2 * PHONE.bezel,
  h: PHONE.h - 2 * PHONE.bezel,
};

// Positions inside the PC screen (screen coordinates)
const trayY = SCREEN.h - TASKBAR_H / 2;
export const PC = {
  trayIcon: { x: SCREEN.w - 104, y: trayY },
  menu: { right: SCREEN.w - 60, bottom: SCREEN.h - TASKBAR_H - 8, w: 262 },
  card: { right: SCREEN.w - 14, bottom: SCREEN.h - TASKBAR_H - 12, w: 372 },
  folder: { x: 92, y: 40, w: 470, h: 290 },
};

// Positions inside the phone screen (screen coordinates)
export const IPHONE = {
  settingsTop: 252, // top of the AirDrop sheet
  rowH: 46,
  shareTop: 236, // top of the share sheet
};
