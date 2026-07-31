# Simple Camera — Free Cam & Cinematic Photo Mode for GTA V

A free-camera and photo-mode plugin for Grand Theft Auto V, built on ScriptHookV.
Detach the camera from your character and fly anywhere to frame screenshots and
cinematic shots.

For motion, switch to **Camera Sequence** mode to author smooth keyframed camera
moves, then play them back live or render them out to an image sequence for video.

Works in **Story Mode** and is **FiveM-aware** (time/weather use the correct
network natives when running under FiveM).

---

## Table of Contents

- [Features](#features)
- [Requirements](#requirements)
- [Installation](#installation)
- [Quick Start](#quick-start)
- [Controls](#controls)
- [The Menu](#the-menu)
- [Workflows](#workflows)
  - [A. Photo Mode — capturing a still](#a-photo-mode--capturing-a-still)
  - [B. Following / filming a moving subject](#b-following--filming-a-moving-subject)
  - [C. Cinematic camera move (sequence)](#c-cinematic-camera-move-sequence)
  - [D. From sequence to finished video](#d-from-sequence-to-finished-video)
  - [E. IGCS Depth of Field (cinematic bokeh)](#e-igcs-depth-of-field-cinematic-bokeh)
  - [Tips & techniques](#tips--techniques)
- [Rendering an Image Sequence](#rendering-an-image-sequence)
- [ReShade / IGCS Connector](#reshade--igcs-connector)
- [Configuration File](#configuration-file)
- [Building from Source](#building-from-source)
- [Troubleshooting](#troubleshooting)
- [Credits & License](#credits--license)

---

## Features

- **Free Camera** — 6-DOF flight (keyboard / mouse / controller) with Euler or
  Quaternion "Acrobatic" rotation, a momentum-based **Drone** mode, **Walk** mode,
  optional collision, **Follow / Entity Lock**, and **Auto Drive** for filming a
  self-driving car.
- **Photo & scene control** — depth of field, procedural camera shake, time-of-day
  and weather, slow motion and world freeze, and HUD / player hiding.
- **Camera Sequence** — keyframed camera moves with easing and spline paths, an
  effect-event track, entity lock, **multi-vehicle recording** synced to the
  timeline, and offline **render to an image sequence** with motion blur.
- **Quality of life** — full controller support, INI-persisted settings, and
  sequences that save and reload automatically.

Every option has an in-game tooltip explaining what it does, so the menu is
self-documenting — this README sticks to setup and the bigger-picture workflows.

---

## Requirements

- **Grand Theft Auto V** (the build supported by your ScriptHookV — i.e. the
  classic/legacy version of the game). Singleplayer or **FiveM**.
- **[ScriptHookV](http://www.dev-c.com/gtav/scripthookv/)** by Alexander Blade.
- *(Optional, only for rendering / advanced photo tools)* **ReShade with add-on
  support** and the **IgcsConnector** add-on.

### FiveM

Supported. Drop `SimpleCamera.asi` into FiveM's `plugins\` folder — same build as
singleplayer, no separate download. `SimpleCamera.log` reports `FiveM=yes` when
it detects the host.

Two things behave differently there, neither of them a bug in the mod:

- **A server can override the camera, time and weather from its own scripts**,
  and it will win. That is multiplayer, not this mod.
- **Follow/aim targeting and "stream around camera" do nothing.** FiveM's
  ScriptHookV shim stubs out `worldGetAllVehicles/Peds/Objects`, so anything that
  enumerates world entities has nothing to work with.

Everything else — free camera, sequences, vehicle clips with wheel rotation,
menu, capture — works as it does in singleplayer.

---

## Installation

1. Install **ScriptHookV** if you don't already have it. This places
   `dinput8.dll` and `ScriptHookV.dll` in your GTA V folder (the folder that
   contains `GTA5.exe`).
2. Copy **`SimpleCamera.asi`** into that same GTA V root folder.
3. *(Optional)* Copy **`SimpleCamera.ini`** alongside it if you want to change
   the menu key or pre-set defaults. If it's missing, the mod just uses built-in
   defaults and writes the file when you save.
4. Launch the game. Press **F5** to open the menu.

> The `.ini`, `SimpleCamera_Sequences.ini`, and any rendered frames
> (`SimpleCamera_Captures/`) are all created next to the `.asi`.

---

## Quick Start

1. Press **F5** to open the menu, then set the top row, **Camera Mode**, to
   **Free Camera**.
2. The flycam engages immediately (HUD and your character auto-hide).
3. Fly with **WASD**, look with the **mouse**, raise/lower with **Space / Ctrl**.
4. Press **F5** again any time to open the settings menu and tweak the shot.
5. Close the menu and you're composing. Press **F10** to save a clean frame
   (requires the ReShade add-on — see below), or use your normal screenshot key.
6. To leave the flycam, open the menu and set **Camera Mode → Off**.

---

## Controls

### Free Camera — Keyboard & Mouse

| Action | Key |
| --- | --- |
| Open / close menu | **F5** |
| Move forward / left / back / right | **W / A / S / D** |
| Move up / down | **Space / Ctrl** |
| Look | **Mouse** |
| Roll left / right | **Q / E** |
| Zoom in / out (FOV) | **+ / -** (also numpad +/-) |
| Adjust fly speed | **Mouse wheel up / down** |
| Speed boost (turbo) | **Hold Shift** |
| Precision / slow | **Hold Alt** |

### Free Camera — Controller

| Action | Button |
| --- | --- |
| Open menu | **LB + RB** |
| Exit flycam (camera off) | **LB + B** |
| Move | **Left stick** |
| Look | **Right stick** |
| Up / down | **RT / LT** |
| Roll | **LB / RB** |
| Zoom (FOV) | **D-Pad Up / Down** |

### Menu navigation

| Action | Keyboard | Controller |
| --- | --- | --- |
| Move / adjust | **Arrow keys** | **D-Pad** |
| Select / toggle | **Enter** | **A** |
| Back | **Backspace** | **B** |

### Camera Sequence hotkeys (in Sequence mode)

| Action | Key |
| --- | --- |
| Capture current pose as a keyframe | **F6** |
| Play | **F7** |
| Stop | **F8** |
| Jump to next pose | **F9** |

> The menu key (F5) and the four sequence hotkeys are all configurable in the
> INI.

---

## The Menu

Press **F5** to open. The top row, **Camera Mode**, switches between **Off**,
**Free Camera**, and **Camera Sequence** — selecting a mode engages it
immediately, and it stays active when you close the menu. Set it to **Off** to
put the camera away.

Each submenu is documented by its own in-game tooltips; in brief:

- **Free Camera** — *Movement* (speed, rotation style, drone / walk, follow
  target), *Lens*, *Depth of Field*, *Camera Effects* (shake), *World & Scene*,
  and *Misc*.
- **Camera Sequence** — playback controls, *Pose Keyframes*, *Effect Events*,
  *Vehicle Clip* recording, *Follow & Entity Lock*, *Sequences*, and *Render to
  Images*.

> To leave either mode, set the top **Camera Mode** row to **Off** (Camera
> Sequence can also switch straight to **Free Camera**).

### Auto Drive

Found under **World & Scene** in both modes. The AI drives the car you're
sitting in while the camera flies free — ideal for filming a moving vehicle
without a second player.

- **Destination: Go To Waypoint** — set a marker on the map; the car drives
  there and re-routes if you move the marker.
- **Destination: Drive Anywhere** — the car wanders the road network endlessly.
- **Speed** (km/h) and **Driving Style** (Normal, Rushed, Avoid Traffic, Ignore
  Lights, …) tune how it drives.

Get into a car first, open **World & Scene → Auto Drive…**, set a waypoint (for
waypoint mode), and toggle **Enabled**. Land vehicles only. Leaving the
camera/sequence mode automatically stops the car.

---

## Workflows

Everything in Simple Camera serves one of two end goals: **a still photo** or
**a moving shot**. The recipes below walk each one start to finish, and show how
the features combine.

The golden rule: **F5 opens the menu, and the camera keeps living while the menu
is open** — so you compose with the menu up, then close it (F5 / Backspace) for a
clean frame. Closing the menu does *not* exit the flycam; set **Camera Mode →
Off** for that.

### A. Photo Mode — capturing a still

1. **Enter.** Press **F5** and set **Camera Mode → Free Camera**. The flycam
   engages and the HUD + your character auto-hide.
2. **Get to the spot.** Fly with **WASD** + **Space/Ctrl**, look with the mouse.
   Hold **Shift** to cover ground fast, **Alt** for slow, precise framing. Mouse
   wheel changes the base fly speed. If you keep clipping into geometry, turn on
   **Movement → World Collision**.
3. **Frame the shot.** Set the focal length in **Lens settings → Lens Zoom (FOV)**
   (low FOV = telephoto/compressed, high FOV = wide). Use **Lens Tilt (Roll)** for
   a dutch angle, or **Misc → Level Horizon** to snap roll back to 0.
4. **Light the scene.** Open **World & Scene → Time & Weather**: set the **Time of
   Day** (and **Pause Time** so it doesn't drift), pick a **Weather**, or blend
   two weathers for in-between skies.
5. **Freeze the moment.** To catch fast action, use **World & Scene → Freeze All
   Entities** (peds/vehicles stop, camera + particles stay live) or **Slow
   Motion**. **Pause Game** halts everything (you can't fly while it's on).
6. **Add depth.** Turn on **Depth of Field**. Use **Auto-Focus** to lock onto
   whatever's under the center of the screen, or set **Manual Focus Dist.** and
   the near/far ranges yourself.
7. **Capture.** Close the menu for a clean frame, then take the shot with your
   normal screenshot key, ReShade, or **F10** (saves a PNG to
   `SimpleCamera_Captures/`, requires the ReShade add-on).
8. **Leave.** Open the menu and set **Camera Mode → Off** (or controller
   **LB+B**) to return the camera to the player.

### B. Following / filming a moving subject

Two ways to keep a moving car/ped in your shot:

- **Make the subject drive itself — Auto Drive.** Get in a car, drop a **map
  waypoint** (or use *Drive Anywhere*), then **World & Scene → Auto Drive →
  Enabled**. The AI drives; you're free to fly the camera around it. Tune
  **Speed** and **Driving Style**.
- **Lock the camera to the subject — Follow / Entity Lock.** In **Movement →
  Follow Target**, choose **Player** or **Aimed Entity** (aim at a vehicle/ped and
  lock on). The camera then rides along:
  - **Rigid Mode off** — the camera holds a fixed *world* orientation but moves
    with the subject's position.
  - **Rigid Mode on** — the camera is bolted on (orbits + rotates with the
    subject), like a hood cam.

Combine them: enable **Auto Drive** so a car drives a route, then **lock** the
camera to that car for a hands-free tracking shot.

### C. Cinematic camera move (sequence)

1. **Enter.** Press **F5** and set **Camera Mode → Camera Sequence**. An empty
   sequence is created; you can free-fly while the menu is open.
2. **Lay down keyframes.** Fly to your opening pose and press **F6** (*Capture
   Current Pose*). Fly to the next position/angle/zoom, press **F6** again.
   Repeat for each "beat" of the move.
3. **Shape the motion.** Open **Pose Keyframes…** to adjust each keyframe's
   **time**, **easing** (**Ease-In-Out** for smooth starts/stops, **Hold** to
   pause on a pose) and **path type** (**Spline** for flowing curves through your
   points, **Linear** for straight dolly moves).
4. **Automate effects (optional).** In **Effect Events…**, schedule changes along
   the timeline — turn **shake** on/off or change its strength, or ramp **World
   Speed** for a slow-mo beat. Mark an event **ramp** to ease between values.
5. **Lock to a subject (optional).** To make the whole move ride with a vehicle:
   lock onto it in Free Camera first (Follow Target → Aimed Entity), then in the
   Sequence menu use **Follow & Entity Lock… → Apply Lock to All**. With Rigid
   Mode off the keyframes just travel with the car; with it on they orbit it.
6. **Preview & refine.** **F7** plays, **F8** stops, **F9** jumps to the next
   pose. Tune **Speed** and **Loop**. For a seamless repeat, use **Close Loop**
   (it reports the gap between the first and last keyframe).
7. **Clean up for capture.** Turn **Show Markers** off so the keyframe spheres
   and path line don't appear in the footage.
8. **Save.** **Save All to INI** persists your sequences to
   `SimpleCamera_Sequences.ini` between sessions.

### D. From sequence to finished video

1. Build and save a sequence (Workflow C).
2. **Enable the `IgcsDOF` technique in the ReShade menu** (required — see
   [Rendering an Image Sequence](#rendering-an-image-sequence)).
3. **Render to Images…** → set FPS / motion blur / format → **Start Render**.
   Frames land in `SimpleCamera_Captures/render_NNNN/`.
4. Assemble the frames into a video at the **same FPS** in your editor, or with
   ffmpeg:
   ```
   ffmpeg -framerate 30 -i frame_%06d.png -c:v libx264 -crf 16 -preset slow \
     -pix_fmt yuv420p -color_range tv -colorspace bt709 -movflags +faststart out.mp4
   ```
   (The color flags keep the contrast matching the rendered frames — see
   Troubleshooting if your video looks more contrasty than the stills.)

### E. IGCS Depth of Field (cinematic bokeh)

**IGCS Depth of Field** (IgcsDof, by Frans Bouma / Otis_Inf) is a far higher-
quality depth of field than the in-game one. Instead of a post-process blur, it
nudges the camera to many tiny offsets and blends the results, simulating real
lens optics — so you get true, photographic bokeh with proper highlights. Simple
Camera acts as the "camera tools" it drives. This is for a **single hero still**,
not video.

> Prerequisites: ReShade (add-on support) + the **IgcsConnector** add-on, with
> the **`IgcsDof.fx`** shader present, exactly as for
> [rendering](#rendering-an-image-sequence). It only works in **Free Camera**
> mode (the IGCS link is disabled in Camera Sequence mode).

1. **Frame the shot in Free Camera.** Compose precisely — once the DoF session
   starts you can't move the camera. You **don't** need to freeze the scene
   yourself: Simple Camera **automatically slows the world to 1%** for the
   duration of the session, so peds/vehicles/cloth/particles barely move between
   samples while the camera and rendering stay live. (Don't use **Pause Game** —
   it freezes the camera too and breaks the session; **Freeze All Entities**
   doesn't help either, since animations keep playing.)
2. **Avoid conflicts.** Turn **Simple Camera's own Depth of Field OFF** (Depth of
   field → Depth of Field) so the two don't fight, and **disable the game's
   TAA/anti-aliasing** — the multi-frame blend does its own anti-aliasing and TAA
   ghosting will smear the result.
3. **Open ReShade** (Home), confirm the **`IgcsDOF`** technique is enabled, then
   go to the **Add-ons** tab → **IGCS Connector** → **Start depth-of-field
   session**. Simple Camera hands camera control to the add-on (your normal
   flight input is locked for the duration — that's expected).
4. **Set focus.** Drag **Focus delta** until the in-focus plane lands on your
   subject; enable **Show magnifier** and check a fine detail (e.g. the eyes) to
   confirm it's razor-sharp. Set **Max bokeh size** for how strong the blur is.
5. **Shape the bokeh (optional).** Pick circular (points-in-innermost-ring +
   number-of-rings) or aperture-shaped (vertices / rounding / rotation), and
   taste-tune **Highlight boost/gamma**, anamorphic factor, fringing, cat-eye,
   etc. More rings = smoother bokeh but many more frames to render.
6. **Tune frame timing if needed.** Start with **frames in flight = 1** and
   **frames to wait = 0**. If the *in-focus* area comes out soft/blurry, cancel,
   raise **frames in flight** to 2–3 (engines with long render pipelines need
   more), and try again.
7. **Render.** Click **Start render** and let the blend finish (watch the
   progress bar).
8. **Save, then end.** Press **PrintScreen** to save the result via ReShade —
   **then** click **End session**. Ending the session discards the blended image,
   so screenshot *first*.

Full tool manual & controls: <https://opm.fransbouma.com/igcsdof.htm>

### Tips & techniques

- **Drone mode** (Movement → Movement Style) gives weighty, momentum-based motion
  with auto-banking — great for organic fly-throughs and hand-held feel.
- **Walk mode** pins the camera to a fixed eye height above the ground for
  natural walking/character-height shots.
- **Acrobatic (Quaternion) rotation** (Movement → Rotation Style) removes the
  gimbal limit so you can roll and pitch freely for FPV-style moves.
- **Procedural shake** (Camera Effects) adds life to otherwise-static shots; the
  speed-coupling makes it react to how fast the camera is moving.
- **Save position on exit** (Misc) lets you toggle the flycam off and back on
  without losing your spot.
- **Info Overlay** (World & Scene) shows live position/rotation/FOV — handy for
  matching or noting a shot.

---

## Rendering an Image Sequence

Camera Sequence mode can render your move out to a numbered folder of image
files — ideal for assembling a clean, stutter-free clip in a video editor.

1. **Requires ReShade (with add-on support) + the IgcsConnector add-on** (see
   next section). The menu shows a warning if the add-on isn't detected.
2. **Enable the `IgcsDOF` technique** (the `IgcsDof.fx` shader) in the ReShade
   in-game menu — the renderer's frame capture goes through it, so rendering
   produces no/black frames if it's left off. (Copy `IgcsDof.fx` into your
   ReShade shaders folder if it isn't already there, then tick **IgcsDOF** in
   the ReShade menu.)
3. In the Sequence menu open **Render to Images…** and set:
   - **Output FPS** and **Flush Frames** (extra clean frames before each grab).
   - **Motion Blur** samples (up to **128**) and **Highlight Boost** for
     cinematic blur.
   - **Format** (PNG lossless or JPEG + quality) and optional **World Slow-mo**.
   - **Color Channels** — leave on **Auto**. If rendered frames come out with
     red/blue swapped (orange sky, blue road lines), force **RGBA** or **BGRA**
     here; some setups (e.g. FiveM) use a different buffer format than vanilla.
4. Choose **Start Render**. Frames are written to
   `SimpleCamera_Captures/render_NNNN/` next to the `.asi`.
5. Import the image sequence into your editor at the same FPS.

> Press **F10** at any time in Free Camera for a quick single-frame capture test
> (also via the ReShade add-on).

### How the render works

The renderer keeps the **world alive and in sync** with the camera. The sequence
actually **plays** at a slow time scale, so the camera, the world, shake and
effect events all advance on the **same game clock** and stay in lockstep. A
frame is grabbed each time playback crosses the next 1/fps mark, so world motion
matches camera speed exactly — moving traffic, pedestrians, your Auto-Drive car,
physics, water and particle effects all render correctly.

- Motion blur accumulates several **consecutive live frames** per output frame
  (set by **Motion Blur** samples, up to **128**), so it blurs the **whole
  moving scene**, not just the camera move. **Highlight Boost** lifts the
  brightness of blur streaks.
- **World Slow-mo** set to **Auto** lets the renderer tune the time scale each
  frame to keep capture in sync; or pin a fixed value.

---

## ReShade / IGCS Connector

Simple Camera implements the **IGCS Connector** protocol, so ReShade's
IgcsConnector add-on can read the camera's live position/rotation/FOV and drive
its advanced photo features (e.g. multi-shot / high-resolution captures and DoF
sessions). When such a session is active, Simple Camera hands control of the
camera to the add-on so the two never fight.

The same shared channel is what powers **F10 single-frame capture**, the
**image-sequence renderer**, and **IGCS Depth of Field** (the multi-frame optical
bokeh tool — see [Workflow E](#e-igcs-depth-of-field-cinematic-bokeh)). For any of
these you must also **enable the `IgcsDOF` technique (`IgcsDof.fx`) in the ReShade
in-game menu** — capture is routed through that shader. None of the core
free-camera features need ReShade — it's only required for the capture/render and
IGCS tooling.

> Credit: the IGCS Connector add-on and IgcsDof shader are by **Frans Bouma
> (Otis_Inf)**. Full DoF manual: <https://opm.fransbouma.com/igcsdof.htm>.

---

## Configuration File

`SimpleCamera.ini` (next to the `.asi`) holds the menu key and all saved
tunables. Key codes use Windows
[virtual-key codes](https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes).

```ini
[Controls]
; Menu toggle key (default: F5 = 116)
MenuKey=116
; Camera Sequence hotkeys
SequenceAddKey=117   ; F6 — capture pose
SequencePlayKey=118  ; F7 — play
SequenceStopKey=119  ; F8 — stop
SequenceNextKey=120  ; F9 — next pose
```

Camera, Drone, Shake, DoF, and Misc settings are written under their own
sections when you choose **Save Settings to INI** in the Misc menu. **Reset to
Defaults** restores the factory values (save afterward to persist).

---

## Building from Source

- **Visual Studio 2022** (toolset **v143**), **x64 / Release**.
- The ScriptHookV SDK (headers + `ScriptHookV.lib`) is expected under
  `external/scripthook_sdk/`.
- Open `FreeCameraPlugin.sln` and build. Output is **`SimpleCamera.asi`** in
  `bin/Release/`.

Source layout:

| File | Responsibility |
| --- | --- |
| `main.cpp` | DLL entry; registers the script + keyboard handler with ScriptHookV. |
| `script.cpp` | Main per-frame loop and mode dispatch. |
| `camera.cpp` / `.h` | The 6-DOF camera, rotation engines, drone physics, shake, DoF, time/weather, follow/lock. |
| `menu.cpp` / `.h` | The in-game menu, INI load/save, input. |
| `sequence.cpp` / `.h` | Keyframe sequences, easing/splines, effect events, persistence. |
| `fx_capture.cpp` / `.h` | Shared-memory bridge to the ReShade capture add-on. |
| `igcs_bridge.cpp` / `.h` | IGCS Connector protocol implementation. |
| `keyboard.cpp` / `.h` | Async keyboard state tracking. |

---

## Troubleshooting

- **Menu won't open / nothing happens on F5** — Make sure ScriptHookV is
  installed and up to date for your game version, and that `SimpleCamera.asi` is
  in the GTA V root folder. Confirm `MenuKey` in the INI isn't bound to
  something else.
- **"Needs ReShade + IGCS addon" on Start Render** — The renderer and F10
  capture need ReShade (add-on support) plus the IgcsConnector add-on loaded.
  Core free-cam features work without them.
- **Render runs but the frames are black / empty** — The `IgcsDOF` technique
  (the `IgcsDof.fx` shader) must be **enabled in the ReShade in-game menu** —
  capture goes through it. Tick it on and render again.
- **Camera clips through walls** — Enable **World Collision** in the Movement
  menu (it's off by default so flight stays unobstructed).
- **Time/weather changes don't stick in FiveM** — The mod auto-detects FiveM and
  uses the network override natives; if a server actively re-syncs time/weather,
  it may override the mod.
- **Acrobatic (Quaternion) rotation needs a model to stream in** — On the very
  first frames it briefly falls back to standard rotation while a tiny invisible
  anchor prop loads; this is normal.

---

## Credits & License

- **Author:** crxhvrd
- Built on **ScriptHookV** by Alexander Blade.
- Photo-capture / IGCS interop via the **IgcsConnector** ReShade add-on.

Released under the **MIT License** — see [`LICENSE`](LICENSE).
