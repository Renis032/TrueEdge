# TrueEdge

Moves the mouse between monitors where they **physically** meet instead of where their pixel edges happen to line up. Inspired by [LittleBigMouse](https://github.com/mgth/littlebigmouse), which only runs on Windows. This version is for Linux/X11 (built and tested on KDE Plasma, Ubuntu 20.04).

On this machine, DP-1 is a portrait 24" (1200×1920) and DP-3 is a landscape 24" (1920×1080, placed 420 px lower). Without trueedged:
- the cursor sticks at the right edge of DP-1 above y=420 and below y=1500;
- a crossing keeps the pixel row, not the physical height.

trueedged fixes both.

## How it works

- `trueedged` (C, ~500 lines) reads monitors from RandR. It takes physical sizes from EDID and swaps them for rotated outputs.
- It puts an XFixes **pointer barrier** on every pixel edge shared by two monitors, so X never crosses on its own.
- It listens to **XInput2 raw motion**. When the cursor sits on a monitor edge and the mouse keeps pushing outwards, it does three things:
  1. converts the exit point to millimetres;
  2. carries it straight across to the monitor that is really there, crossing bezels and gaps (or, with `jump 1`, to the nearest one when nothing is straight across);
  3. warps the cursor to the matching pixel.
- It reloads on SIGHUP and when the RandR layout changes (hotplug, rotation, `xrandr`).
- Only one instance runs per session (lock in `$XDG_RUNTIME_DIR/trueedged.lock`).

`trueedge-config` (PyQt5) is the layout editor. Drag the monitors to match your desk: edges snap, and Shift places freely. Fix the sizes if the EDID is wrong (measure the visible area with a ruler), then **Save & apply**.

### Aligning with the rulers

**Show rulers** puts a yellow ruler along every screen edge. Every monitor uses the same millimetre coordinates from the layout: ticks every mm, a number every cm, and red every 10 cm.

- **Position:** look across a bezel. If the marks on both sides continue in a straight line (20 meets 20), the layout matches your desk. If they don't, drag the monitor or nudge Left/Top in the editor until they do. The rulers update live.
- **Save & apply:** writes the layout, reloads trueedged, and moves the real screens with `xrandr --pos` so their pixel arrangement matches it. Neighbours keep touching in pixels and are offset by their mm offset along the shared edge. Windows, wallpaper and screenshots then line up across the bezel too.
- **Size:** hold a real ruler against the screen. If 10 cm on screen isn't 10 cm, fix Width/Height.

## Use

```sh
make                 # needs libx11-dev libxi-dev libxrandr-dev libxfixes-dev
./trueedged &        # run the daemon
./trueedge-config    # arrange monitors, Save & apply (trueedged reloads)
make install         # ~/.local/bin + KDE autostart (~/.config/autostart/trueedged.desktop)
```

Layout file: `~/.config/trueedge/layout.conf`

```
jump 1                                # nothing straight across: go to the nearest monitor
monitor DP-1 0.0 0.0 324.0 519.0      # name  left top width height (mm)
monitor DP-3 324.0 115.1 527.0 296.0
```

Monitors missing from the file get a default position next to their pixel neighbour.

Debugging without touching the cursor:

```sh
./trueedged --print [--defaults]    # layout in use / as detected
./trueedged --test 1199 100 3 0     # where a move of (+3, 0) from (1199, 100) lands -> "1200 420"
```

## Not supported yet

- Per-monitor pointer speed / DPI scaling. Both monitors here are about 93 DPI, so it isn't needed.
- Wayland. trueedged needs an X11 session, because Wayland doesn't let clients warp the global pointer.
- Corner-only (diagonal) crossings and wrap-around.
