import React from "react";
import { Composition } from "remotion";
import { HowItWorks } from "./HowItWorks";
import { FPS, DURATION_S } from "./timeline";

export const Root: React.FC = () => (
  <Composition
    id="HowItWorks"
    component={HowItWorks}
    durationInFrames={Math.round(DURATION_S * FPS)}
    fps={FPS}
    width={1280}
    height={720}
  />
);
