// Single source of truth for colours, easings and springs. Never inline these in components.
import { Easing } from "remotion";
import { loadFont } from "@remotion/google-fonts/Inter";

const { fontFamily: inter } = loadFont("normal", { weights: ["400", "500", "600", "700"], subsets: ["latin"] });

export const theme = {
  colors: {
    bg: "#0B1020",
    bgAlt: "#141B33",
    hero: "#60CDFF", // Windows dark-mode accent, as in the app's Accept button
    heroText: "#000000",
    text: "#F3F4F6",
    textDim: "#9CA3AF",
    // the app's card theme (CardTheme.cs, dark)
    card: "#2B2B2B",
    edge: "#4D4D4D",
    cardText: "#F3F3F3",
    muted: "#A6A6A6",
    button: "#3C3C3C",
    on: "#009688", // tray icon while receiving (App.xaml.cs)
    off: "#828282",
    // devices
    bezel: "#0E0F13",
    bezelEdge: "#2A2D36",
    taskbar: "rgba(28,28,32,0.92)",
    menu: "#2C2C2C",
    menuHover: "#3A3A3A",
    sheet: "#1C1C1E",
    cell: "#2C2C2E",
    separator: "#3A3A3C",
  },
  fonts: {
    ui: inter,
    windows: `"Segoe UI Variable Text", "Segoe UI", ${inter}`,
  },
  ease: {
    out: Easing.bezier(0.16, 1, 0.3, 1),
    inOut: Easing.bezier(0.83, 0, 0.17, 1),
    move: Easing.bezier(0.45, 0, 0.2, 1), // cursor travel
    in: Easing.bezier(0.7, 0, 0.84, 0),
  },
  spring: {
    snappy: { damping: 14, stiffness: 160, mass: 0.6 },
    smooth: { damping: 20, stiffness: 90, mass: 1 },
    bouncy: { damping: 11, stiffness: 170, mass: 0.7 },
  },
} as const;
