# Simple Camera

Free camera and cinematic photo mode for GTA V. Singleplayer and FiveM.

- **Free camera** — 6-DOF flight, Euler or quaternion rotation, momentum-based
  Drone mode, Walk mode, optional collision, follow / entity lock, Auto Drive.
- **Scene control** — depth of field, procedural shake, time of day, weather,
  slow motion, world freeze, HUD and player hiding.
- **Camera Sequence** — keyframed moves with easing and spline paths, an effect
  event track, entity lock, multi-vehicle recording on a shared timeline, and
  offline render to an image sequence with motion blur.

Every menu option carries an in-game tooltip. This file covers setup, the
workflows, and the parts that are not self-evident from the menu.

---

## Requirements

| | |
|---|---|
| GTA V | the build your ScriptHookV supports (Legacy). Singleplayer or FiveM |
| [ScriptHookV](http://www.dev-c.com/gtav/scripthookv/) | required |
| ReShade + IgcsConnector | **rendering, F10 capture and IGCS DOF only** — must be the ReShade build with add-on support |

### FiveM

Supported. Same build, `plugins\` folder, no separate download.
`SimpleCamera.log` reports `FiveM=yes`.

Two differences, neither specific to this mod:

- A **server can override camera, time and weather** from its own scripts and
  will win.
- **Follow/aim targeting and "stream around camera" do nothing** — FiveM's
  ScriptHookV shim stubs out `worldGetAllVehicles/Peds/Objects`, so anything
  enumerating world entities has nothing to read.

Everything else, including vehicle-clip wheel rotation, behaves as in
singleplayer.

---

## Installation

1. Install **ScriptHookV** (`dinput8.dll` + `ScriptHookV.dll` in the GTA V
   folder).
2. Copy `SimpleCamera.asi` next to `GTA5.exe`, or into `plugins\` for FiveM.
3. Launch. Press **F5**.

`SimpleCamera.ini`, `SimpleCamera_Sequences.ini` and `SimpleCamera_Captures\`
are created next to the `.asi` on demand. The ini is optional — without it the
mod uses built-in defaults and writes the file when you save.

---

## Controls

**Free camera — keyboard**

| | |
|---|---|
| Menu | **F5** |
| Move | **W A S D**, **Space / Ctrl** up-down |
| Look / roll | **Mouse**, **Q / E** |
| FOV | **+ / −** |
| Fly speed | **Mouse wheel** |
| Turbo / precision | **Shift** / **Alt** |

**Free camera — controller**

| | |
|---|---|
| Menu | **LB + RB** |
| Exit flycam | **LB + B** |
| Move / look | **Left stick** / **Right stick** |
| Up / down | **RT / LT** |
| Roll | **LB / RB** |
| FOV | **D-Pad up / down** |

**Menu** — arrows or D-Pad to move and adjust, **Enter**/**A** to select,
**Backspace**/**B** to go back.

**Sequence hotkeys** — **F6** capture pose, **F7** play, **F8** stop, **F9** next
pose.

The menu key and all four sequence hotkeys are configurable in the ini.

---

## Menu

**Camera Mode** is the top row: **Off**, **Free Camera**, **Camera Sequence**.
Selecting a mode engages it immediately and it stays active when the menu closes.

The camera keeps running while the menu is open — compose with it up, then close
it for a clean frame. Closing the menu does **not** exit the flycam; set
**Camera Mode → Off** for that.

| Mode | Submenus |
|---|---|
| Free Camera | Movement, Lens, Depth of Field, Camera Effects, World & Scene, Misc |
| Camera Sequence | playback, Pose Keyframes, Effect Events, Vehicle Clip, Follow & Entity Lock, Sequences, Render to Images |

**Auto Drive** (World & Scene, both modes) has the AI drive the car you are
sitting in while the camera flies free. Waypoint or free-roam, with speed and
driving style. Land vehicles only; leaving the camera mode stops the car.

---

## Workflows

### Still photo

Frame in Free Camera, hide what you do not want, then capture with **F10**
(requires the ReShade add-on) or your usual screenshot key. **Save position on
exit** in Misc lets you leave and resume the same spot.

### Cinematic move

1. **Camera Mode → Camera Sequence.** An empty sequence is created.
2. Fly to each beat and press **F6** to capture a keyframe.
3. **Pose Keyframes…** — set each keyframe's time, easing (**Ease-In-Out** for
   smooth starts and stops, **Hold** to pause on a pose) and path type
   (**Spline** for flowing curves, **Linear** for straight dolly moves).
4. **Effect Events…** *(optional)* — schedule shake or World Speed changes along
   the timeline. Mark an event **ramp** to ease between values.
5. **Follow & Entity Lock… → Apply Lock to All** *(optional)* — lock the move to
   a vehicle. Rigid Mode off makes keyframes travel with the car; on makes them
   orbit it.
6. **F7** plays, **F8** stops, **F9** steps. **Close Loop** reports the gap
   between first and last keyframe for a seamless repeat.
7. Turn **Show Markers** off so keyframe spheres and the path line stay out of
   the footage.
8. **Save All to INI** persists sequences between sessions.

### Sequence to video

Build and save a sequence, enable the **IgcsDOF** technique in ReShade, then
**Render to Images… → Start Render**. Frames land in
`SimpleCamera_Captures\render_NNNN\`. Assemble at the same FPS:

```
ffmpeg -framerate 30 -i frame_%06d.png -c:v libx264 -crf 16 -preset slow \
  -pix_fmt yuv420p -color_range tv -colorspace bt709 -movflags +faststart out.mp4
```

The colour flags keep contrast matching the rendered frames.

### IGCS depth of field

Real optical bokeh — the add-on nudges the camera around a lens aperture and
blends the results. **Single hero stills, not video.** Free Camera mode only.

1. **Frame the shot precisely.** The camera cannot move once the session starts.
   No need to freeze the scene: the world is automatically slowed to 1% for the
   duration. Do **not** use Pause Game — it freezes the camera and breaks the
   session.
2. **Turn Simple Camera's own Depth of Field off** so the two do not fight, and
   **disable the game's TAA** — the multi-frame blend does its own
   anti-aliasing and TAA ghosting smears the result.
3. ReShade → confirm **IgcsDOF** is enabled → **Add-ons → IGCS Connector →
   Start depth-of-field session**. Flight input locks for the duration.
4. **Focus delta** until the focal plane lands on the subject; **Show magnifier**
   to confirm on fine detail. **Max bokeh size** sets blur strength.
5. *(Optional)* shape the bokeh — circular or aperture-shaped, highlight
   boost/gamma, anamorphic, fringing, cat-eye. More rings = smoother, slower.
6. Start with **frames in flight = 1**, **frames to wait = 0**. If the in-focus
   area comes out soft, cancel and raise frames in flight to 2–3.
7. **Start render**, then **PrintScreen to save** — *then* End session. Ending
   discards the blended image, so screenshot first.

Manual: <https://opm.fransbouma.com/igcsdof.htm>

### Techniques

**Drone mode** gives weighty momentum-based motion with auto-banking.
**Walk mode** pins the camera to eye height. **Acrobatic (quaternion)** rotation
removes the gimbal limit for FPV-style rolls. **Procedural shake** adds life to
static shots and can couple to camera speed. **Info Overlay** shows live
position, rotation and FOV.

---

## Rendering

Camera Sequence renders to a numbered image folder.

**Requires ReShade with add-on support, the IgcsConnector add-on, and the
`IgcsDOF` technique enabled in the ReShade menu** — capture is routed through
that shader, so leaving it off produces black or missing frames. The menu warns
if the add-on is absent.

**Render to Images…** sets output FPS, flush frames, motion blur samples (up to
128), highlight boost, format (PNG or JPEG + quality), optional world slow-mo,
and colour channel order. Leave channels on **Auto**; force RGBA or BGRA only if
frames come out with red and blue swapped.

**How it works.** The sequence genuinely *plays* at a slow time scale, so camera,
world, shake and effect events all advance on the same game clock. A frame is
grabbed each time playback crosses the next 1/fps mark, so world motion matches
camera speed exactly — traffic, pedestrians, physics, water and particles all
render correctly. Motion blur accumulates consecutive live frames, so it blurs
the whole moving scene rather than just the camera move.

---

## ReShade / IGCS Connector

Simple Camera implements the IGCS Connector protocol, so the add-on can read the
camera's live position, rotation and FOV and drive its own features. While a
session is active the add-on owns the camera, so the two never fight.

That same channel powers F10 capture, the image-sequence renderer and IGCS depth
of field. All three need the **IgcsDOF** technique enabled. No core free-camera
feature needs ReShade.

The add-on and shader are by **Frans Bouma (Otis_Inf)**.

---

## Configuration

`SimpleCamera.ini`, next to the `.asi`. Key codes are Windows
[virtual-key codes](https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes).

```ini
[Controls]
MenuKey=116          ; F5
SequenceAddKey=117   ; F6  capture pose
SequencePlayKey=118  ; F7  play
SequenceStopKey=119  ; F8  stop
SequenceNextKey=120  ; F9  next pose
```

Camera, Drone, Shake, DoF and Misc settings are written under their own sections
by **Save Settings to INI** in the Misc menu. **Reset to Defaults** restores
factory values; save afterwards to persist.

---

## Building

Visual Studio 2022, toolset v143, x64 Release. Static CRT — an ASI does not
choose which runtime its host has already loaded. The ScriptHookV SDK (headers +
`ScriptHookV.lib`) is expected under `external/scripthook_sdk/`.

Open `FreeCameraPlugin.sln` and build. Output: `bin/Release/SimpleCamera.asi`.

| File | |
|---|---|
| `core/main.cpp` | DLL entry; registers the script and keyboard handler |
| `core/script.cpp` | per-frame loop and mode dispatch |
| `camera/camera.cpp` | 6-DOF camera, rotation engines, drone physics, shake, DoF, time/weather, follow/lock |
| `camera/sequence.cpp` | keyframe sequences, easing and splines, effect events, persistence |
| `camera/vehicleclip.cpp` | multi-vehicle recording and replay |
| `camera/vehmem.cpp` | direct CWheel access for replay wheel rotation |
| `menu/`, `gui/` | in-game menu, ini load/save, input |
| `capture/fx_capture.cpp` | shared-memory bridge to the ReShade add-on |
| `capture/igcs_bridge.cpp` | IGCS Connector protocol |

---

## Troubleshooting

| Symptom | Cause |
|---|---|
| F5 does nothing | ScriptHookV missing or out of date for your game build, `.asi` in the wrong folder, or `MenuKey` bound elsewhere |
| "Needs ReShade + IGCS addon" on Start Render | Rendering and F10 capture need ReShade with add-on support plus IgcsConnector. Core free-cam works without them |
| Render produces black or empty frames | The **IgcsDOF** technique is not enabled in the ReShade menu. Capture routes through it |
| Camera clips through walls | Enable **World Collision** in Movement — off by default so flight stays unobstructed |
| Time/weather does not stick in FiveM | The mod uses the network override natives, but a server that actively re-syncs will win |
| Acrobatic rotation stutters on the first frames | It briefly falls back to standard rotation while a small anchor prop streams in. Normal |

---

## Credits

Author: **crxhvrd**. Built on **ScriptHookV** by Alexander Blade. Capture and
IGCS interop via the **IgcsConnector** ReShade add-on by Frans Bouma.
