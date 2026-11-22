# How-it-works animation

Source of `docs/media/how-it-works.gif` and `.mp4`: an 18-second loop that shows receiving a photo
(tray menu → *Start receiving*, iPhone AirDrop → *Everyone for 10 Minutes*, share, *Accept*, Downloads).
Built with [Remotion](https://www.remotion.dev/); the PC cards copy the tray app's own layout and colours
(`app/Mtdrop.App/PromptWindow.xaml`, `NoticeWindow.xaml`, `CardTheme.cs`). The photo is drawn, not a real picture.

```
npm install
npm run studio     # preview and edit
npm run render     # out/how-it-works.mp4
ffmpeg -i out/how-it-works.mp4 -vf "fps=15,scale=800:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=128:stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle" -loop 0 ../../docs/media/how-it-works.gif
```

Timing lives in `src/timeline.ts`, colours and easings in `src/theme.ts`.
Remotion is free for individuals and small teams; see its licence before using it in a company.
