/*
        Simple Camera — new GUI menu implementation. See scmenu.h.

        Builds the Free Camera menu tree on the gtam::NativeMenu framework using
        the Spotlight (dark + cyan) theme, binding every row to the existing
        camera.h globals and actions. Nothing here duplicates camera logic — it's
        purely a new front-end over the same state the classic menu drives.
*/

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "scmenu.h"
#include "camera.h"
#include "fx_capture.h" // FxCapture_AddonPresent
#include "menu.h"     // SetStatusText, SaveSettings, Reset, render globals + ProcessRenderToImages
#include "sequence.h" // Camera Sequence data model + API
#include "vehicleclip.h" // record/replay a vehicle path along the sequence timeline

#include "NativeMenu.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

// ============================================================
//  Menu objects (host-owned, alive for the process lifetime)
// ============================================================

static gtam::MenuController g_Ctrl;
// The single unified root. A "Camera Mode" row at the top switches Off / Free
// Camera / Camera Sequence; the rest of the page is rebuilt to match the mode.
static gtam::Menu g_Root("SIMPLE CAMERA", "Free Camera");
static gtam::Menu g_Movement("SIMPLE CAMERA", "Movement");
static gtam::Menu g_Drone("SIMPLE CAMERA", "Drone Settings");
static gtam::Menu g_Follow("SIMPLE CAMERA", "Follow Target");
static gtam::Menu g_Lens("SIMPLE CAMERA", "Lens");
static gtam::Menu g_DoF("SIMPLE CAMERA", "Depth of Field");
static gtam::Menu g_Effects("SIMPLE CAMERA", "Camera Effects");
static gtam::Menu g_World("SIMPLE CAMERA", "World & Scene");
static gtam::Menu g_Time("SIMPLE CAMERA", "Time & Weather");
static gtam::Menu g_AutoDrive("SIMPLE CAMERA", "Auto Drive");
static gtam::Menu g_Misc("SIMPLE CAMERA", "Misc");
static gtam::Menu g_Appearance("SIMPLE CAMERA", "Appearance");
static gtam::Menu g_ApLayout("SIMPLE CAMERA", "Menu Layout");
static gtam::Menu g_ApColors("SIMPLE CAMERA", "Menu Colours");
static gtam::Menu g_ApMarkers("SIMPLE CAMERA", "Sequence Markers");

// Menu appearance state (persisted via Load/SaveSettings; see scmenu.h).
// Defaults mirror the Spotlight theme so the out-of-box look is unchanged.
float g_MenuPosX = 0.025f, g_MenuPosY = 0.07f, g_MenuScale = 1.0f;
int g_MenuAccentR = 60, g_MenuAccentG = 200, g_MenuAccentB = 220;
int g_MenuBgR = 17, g_MenuBgG = 19, g_MenuBgB = 24;
int g_MenuTextR = 205, g_MenuTextG = 210, g_MenuTextB = 218;
int g_MenuSelR = 30, g_MenuSelG = 80, g_MenuSelB = 95;
int g_MenuSelTextR = 240, g_MenuSelTextG = 245, g_MenuSelTextB = 250;

// ---- Camera Sequence menus ----
static gtam::Menu g_SeqPoses("CAMERA SEQUENCE", "Pose Keyframes");
static gtam::Menu g_SeqPoseEdit("CAMERA SEQUENCE", "Keyframe");
static gtam::Menu g_SeqEvents("CAMERA SEQUENCE", "Effect Events");
static gtam::Menu g_SeqEventEdit("CAMERA SEQUENCE", "Effect Event");
static gtam::Menu g_SeqFollow("CAMERA SEQUENCE", "Follow & Entity Lock");
static gtam::Menu g_SeqList("CAMERA SEQUENCE", "Sequences");
static gtam::Menu g_SeqPlayback("CAMERA SEQUENCE", "Playback");
static gtam::Menu g_SeqRender("CAMERA SEQUENCE", "Render to Images");
static gtam::Menu g_SeqVehicle("CAMERA SEQUENCE", "Vehicle Clip");
static gtam::Menu g_SeqVehicleList("CAMERA SEQUENCE", "Vehicles");
static gtam::Menu g_SeqVehicleEdit("CAMERA SEQUENCE", "Vehicle");
static gtam::Menu g_SeqMoveAll("CAMERA SEQUENCE", "Move All Keyframes");

// Sequence editing mirrors. The framework binds raw pointers, but pose/event
// vectors can reallocate (capture/add/sort), so editors bind to these stable
// mirrors and we push them to the live element by index each frame.
static float s_scrub = 0.0f;     // root scrub time (synced to playback)
static float s_totalDur = 0.0f;  // pose-list "Total Duration" (scales times)
// "Move All Keyframes" live offset (metres). The bound value shows the running
// offset since the page was opened; s_moveLast* tracks the last-applied value so
// each adjuster nudge translates only the delta. Reset to 0 on page push.
static float s_moveOffX = 0.0f, s_moveOffY = 0.0f, s_moveOffZ = 0.0f;
static float s_moveLastX = 0.0f, s_moveLastY = 0.0f, s_moveLastZ = 0.0f;
static int s_editPose = -1;
static int s_editEvent = -1;
static struct { float t, posX, posY, posZ, pitch, yaw, roll, fov; int easeI, pathI; } s_pm;
static struct { float t, value; int kindI; bool ramp; } s_em;
// Workflow helpers: live keyframe preview + fast timeline navigation.
static decltype(s_pm) s_pmPrev;      // change detect: re-preview edited keyframe
static int s_kfNav = 1;              // root "Go to Keyframe" stepper (1-based)
static int s_poseListHeaderRows = 0; // rows before the first keyframe row
static int s_poseListPrevSel = -1;   // selection edge detect for browse preview
static int s_editVeh = -1;                 // clip index open in the vehicle editor
static VehicleClipSettings s_vm{};         // vehicle editor mirror (bound by menu)
static bool s_seqPopRequested = false; // deferred Back() (delete-from-editor)
static bool s_seqCloseRequested = false; // deferred full close (start a take)
static DWORD s_seqDelArmed = 0;        // 2-press confirm for Delete Active
static DWORD s_seqReloadArmed = 0;     // 2-press confirm for Reload from Disk
// Index of the "Value" row inside g_SeqEventEdit.items (resolved each frame).
// Storing an index rather than a MenuItem* is deliberate: g_SeqEventEdit.items
// is a std::vector that reallocates as later rows are appended, which would
// leave a cached pointer dangling.
static int s_EventValueIdx = -1;

// Render menu: the FPS / blur settings are non-linear preset cycles, so they
// bind to mirror indices synced from the live (float/int) globals on entry.
static const float kRenderFps[] = {24, 25, 30, 48, 50, 60, 120, 240};
static const int kRenderFpsCount = 8;
static const int kRenderBlur[] = {1, 2, 4, 8, 16, 32, 64, 128};
static const int kRenderBlurCount = 8;
static int s_fpsIdx = 5;  // -> 60
static int s_blurIdx = 0; // -> 1 (Off)

// Unified-root "Camera Mode" mirror: 0 = Off, 1 = Free Camera, 2 = Sequence.
// Synced from the live camera state each frame; changing it applies the switch.
static int s_mode = 0;

static bool g_Built = false;

// ============================================================
//  Helpers
// ============================================================

// Fire a ray from the camera and return the entity under the crosshair (0 if
// none). Mirrors the classic menu's RaycastEntityFromCamera so "Lock Aimed
// Entity" works the same way.
static int RaycastEntityFromCamera() {
  float px, py, pz, pitch, yaw, roll;
  GetCameraState(px, py, pz, pitch, yaw, roll);
  float yawRad = yaw * 0.0174532925f;
  float pitchRad = pitch * 0.0174532925f;
  float dx = -sinf(yawRad) * cosf(pitchRad);
  float dy = cosf(yawRad) * cosf(pitchRad);
  float dz = sinf(pitchRad);
  float ex = px + dx * 1000.0f, ey = py + dy * 1000.0f, ez = pz + dz * 1000.0f;
  // Ignore the player ped so the ray can't self-hit when the hidden ped is
  // parked at the camera (Stream Around Camera) — but ONLY while on foot.
  // The shape test extends the ignore to everything the ignored entity is
  // attached to, so ignoring a ped who's sitting in a car silently made the
  // player's own vehicle un-hittable. In a vehicle the ped is at the car,
  // not at the camera, so there's nothing to self-hit — ignore nothing.
  Ped player = PLAYER::PLAYER_PED_ID();
  int ignore = PED::IS_PED_IN_ANY_VEHICLE(player, FALSE) ? 0 : player;
  int ray = invoke<int>(0x377906D8A31E5586, px, py, pz, ex, ey, ez, 30,
                        ignore, 7);
  int hit = 0, ent = 0;
  Vector3 a{}, b{};
  invoke<int>(0x3D87450E15D98694, ray, &hit, &a, &b, &ent);
  if (hit && ent != 0 && ENTITY::DOES_ENTITY_EXIST(ent)) {
    // A hit on a seated ped resolves to their vehicle: option 7 ignores glass,
    // so aiming "at a car" can clip the driver through the windshield — the
    // car is what the user meant to lock.
    if (ENTITY::IS_ENTITY_A_PED(ent) && PED::IS_PED_IN_ANY_VEHICLE(ent, FALSE)) {
      int v = PED::GET_VEHICLE_PED_IS_IN(ent, FALSE);
      if (v != 0 && ENTITY::DOES_ENTITY_EXIST(v)) return v;
    }
    if (ENTITY::IS_ENTITY_A_PED(ent) || ENTITY::IS_ENTITY_A_VEHICLE(ent) ||
        ENTITY::IS_ENTITY_AN_OBJECT(ent))
      return ent;
  }
  // Replay ghosts run with collision DISABLED, so the shape test passes
  // straight through them and aiming at a recorded vehicle never locked.
  // Fall back to a ray-proximity test against each spawned ghost: pick the
  // nearest one whose center lies within a few meters of the aim ray.
  int bestGhost = 0;
  float bestT = 1e9f;
  for (int i = 0; i < VehicleClip_Count(); ++i) {
    int g = VehicleClip_GhostAt(i);
    if (g == 0) continue;
    Vector3 gp = ENTITY::GET_ENTITY_COORDS(g, TRUE);
    float vx = gp.x - px, vy = gp.y - py, vz = gp.z - pz;
    float tproj = vx * dx + vy * dy + vz * dz; // dir is unit-length
    if (tproj < 0.5f || tproj > 1000.0f) continue; // behind us / out of range
    float cx = px + dx * tproj - gp.x;
    float cy = py + dy * tproj - gp.y;
    float cz = pz + dz * tproj - gp.z;
    float perp2 = cx * cx + cy * cy + cz * cz;
    if (perp2 < 3.0f * 3.0f && tproj < bestT) { bestT = tproj; bestGhost = g; }
  }
  if (bestGhost != 0) return bestGhost;
  return 0;
}

// Weather / driving-style / effect option lists, built once from the tables.
static std::vector<std::string> g_WeatherOpts;
static std::vector<std::string> g_StyleOpts;
static std::vector<std::string> g_EffectOpts;

static void BuildOptionLists() {
  g_WeatherOpts.clear();
  for (int i = 0; i < g_WeatherCount; ++i)
    g_WeatherOpts.push_back(g_WeatherNames[i]);
  g_StyleOpts.clear();
  for (int i = 0; i < g_AutoDriveStyleCount; ++i)
    g_StyleOpts.push_back(g_AutoDriveStyleNames[i]);
}

// ============================================================
//  Tree construction
// ============================================================

// Build the menu theme from the Spotlight preset + Simple Camera's tweaks +
// the user's accent colour. Called at startup and whenever an accent slider
// changes, so the whole menu recolours live from one RGB.
static void ApplyMenuTheme() {
  gtam::MenuTheme t = gtam::MenuTheme::Spotlight();
  t.maxVisibleRows = 12;
  t.valuePill = false; // no translucent box behind adjustable values
  // Clean sans-serif title (GTA font 0) instead of the cursive House Script.
  t.titleFont = 0;
  t.titleScale = 0.50f;
  t.titleHeightPx = 54.0f;
  gtam::Color ac(g_MenuAccentR, g_MenuAccentG, g_MenuAccentB, 255);
  gtam::Color bg(g_MenuBgR, g_MenuBgG, g_MenuBgB);
  gtam::Color tx(g_MenuTextR, g_MenuTextG, g_MenuTextB, 255);

  // Accent chrome (left bar, selected outline, scrollbar, checkbox fill).
  t.accent = ac;
  t.rowSelectedOutline = ac;
  t.scrollThumb = ac;
  t.checkboxFill = ac;

  // Background family — recolour every panel surface, preserving each one's
  // original translucency so the layered look survives.
  t.titleBg = gtam::Color(bg.r, bg.g, bg.b, 245);
  t.rowBg = gtam::Color(bg.r, bg.g, bg.b, 230);
  t.subtitleBg = gtam::Color(bg.r, bg.g, bg.b, 245);
  t.descBg = gtam::Color(bg.r, bg.g, bg.b, 240);
  t.footerBg = gtam::Color(bg.r, bg.g, bg.b, 240);

  // Text family — captions/values/title/subtitle/description; dimmed variants
  // for separators, disabled rows and the footer.
  t.titleText = tx;
  t.rowText = tx;
  t.subtitleText = tx;
  t.descText = tx;
  t.separatorText = gtam::Color((int)(tx.r * 0.62f), (int)(tx.g * 0.62f), (int)(tx.b * 0.62f), 255);
  t.disabledText = gtam::Color((int)(tx.r * 0.50f), (int)(tx.g * 0.50f), (int)(tx.b * 0.50f), 255);
  t.footerText = gtam::Color((int)(tx.r * 0.78f), (int)(tx.g * 0.78f), (int)(tx.b * 0.78f), 255);

  // Selected row.
  t.rowSelectedBg = gtam::Color(g_MenuSelR, g_MenuSelG, g_MenuSelB, 235);
  t.rowSelectedText = gtam::Color(g_MenuSelTextR, g_MenuSelTextG, g_MenuSelTextB, 255);

  g_Ctrl.SetTheme(t);
}

static void BuildTree() {
  BuildOptionLists();

  ApplyMenuTheme();
  g_Ctrl.SetPosition(g_MenuPosX, g_MenuPosY);
  g_Ctrl.SetScale(g_MenuScale);

  // Reopening returns to the row we closed on (via F5 toggle or Backspace at
  // the root) instead of jumping back to the top.
  g_Ctrl.SetRememberRootCursor(true);

  g_Ctrl.SetFooterText("");

  // The unified root's rows are built per-mode in RebuildRoot(); the leaf
  // submenus below are built once here.

  // ---- Movement ----
  g_Movement.AddFloat("Camera Speed", &g_CamSpeed, 0.1f, 50.0f, 0.1f, 1, nullptr,
                      "How fast the camera flies through the world. Higher moves quicker.");
  g_Movement.AddFloat("Look Sensitivity", &g_CamSensitivity, 0.1f, 5.0f, 0.1f, 1, nullptr,
                      "How fast the camera turns when you move the mouse or stick.");
  g_Movement.AddFloat("Zoom Speed", &g_ZoomSpeed, 0.1f, 10.0f, 0.1f, 1, nullptr,
                      "How quickly the lens zooms (changes FOV) when you scroll.");
  g_Movement.AddFloat("Roll Speed", &g_RollSpeed, 0.1f, 10.0f, 0.1f, 1, nullptr,
                      "How fast the camera tilts side-to-side (Dutch angle).");
  g_Movement.AddToggle("World Collision", &g_CamCollision, nullptr,
                       "Camera collides with the world.");
  g_Movement.AddToggle("Lock Altitude", &g_LockHeight, nullptr,
                       "Freeze the Z axis while flying.");
  g_Movement.AddToggle("Walk Mode", &g_WalkMode, nullptr,
                       "Follow the terrain at a fixed eye height.");
  g_Movement.AddFloat("Walk Height (m)", &g_WalkHeight, 0.1f, 50.0f, 0.1f, 2,
                      nullptr, "Eye height above ground in Walk Mode.");
  g_Movement.AddToggle("Acrobatic Rotation", &g_RotationEngine, nullptr,
                       "Quaternion engine: full 360 roll, no gimbal lock.");
  g_Movement.AddSubmenu("Drone Settings", &g_Drone,
                        "Momentum-based flight tuning.");
  g_Movement.AddSubmenu("Follow Target", &g_Follow,
                        "Track the player or an aimed entity.");

  // ---- Drone ----
  g_Drone.AddToggle("Drone Mode", &g_DroneMode, nullptr,
                    "Momentum/inertia flight instead of direct movement.");
  g_Drone.AddFloat("Drag", &g_DroneDrag, 0.0f, 20.0f, 0.5f, 1, nullptr,
                   "Air resistance. Higher stops the camera sooner after you let go.");
  g_Drone.AddFloat("Acceleration", &g_DroneAcceleration, 1.0f, 50.0f, 1.0f, 1, nullptr,
                   "How hard the camera thrusts. Higher reaches top speed faster.");
  g_Drone.AddFloat("Gravity", &g_DroneGravity, 0.0f, 20.0f, 0.5f, 1, nullptr,
                   "Constant downward pull when not thrusting. 0 = weightless / floaty.");
  g_Drone.AddFloat("Banking", &g_DroneBanking, 0.0f, 45.0f, 1.0f, 1, nullptr,
                   "How much the camera auto-rolls into turns, in degrees. 0 = stays level.");
  g_Drone.AddFloat("Rotation Smoothing", &g_DroneRotSmoothing, 0.0f, 20.0f, 0.5f, 1, nullptr,
                   "Turn responsiveness. Higher = snappier, lower = heavier and laggier.");
  g_Drone.AddFloat("FOV Smoothing", &g_DroneFovSmoothing, 0.0f, 20.0f, 0.5f, 1, nullptr,
                   "How smoothly the zoom eases in drone mode. Higher = softer.");

  // ---- Follow ----
  g_Follow.AddList("Follow Mode", &g_FollowMode, {"None", "Player", "Aimed Entity"},
                   [](int m) { if (m != 2) g_FollowTargetEntity = 0; },
                   "What the camera tracks.");
  g_Follow.AddToggle("Rigid Mode", &g_FollowRigidMode, nullptr,
                     "Inherit the target's rotation, not just position.");
  g_Follow.AddToggle("Show Locked Marker", &g_ShowLockedEntityMarker, nullptr,
                     "Draw a marker on the entity the camera is locked to.");
  g_Follow.AddButton("Lock Aimed Entity", [] {
    int e = RaycastEntityFromCamera();
    if (e) {
      g_FollowTargetEntity = e;
      g_FollowMode = 2;
      SetStatusText("Locked onto entity");
    } else {
      SetStatusText("No entity found. Aim closer.");
    }
  }, "Point the camera at a ped/vehicle/object and lock onto it.");
  g_Follow.AddButton("Unlock Entity", [] {
    g_FollowTargetEntity = 0;
    SetStatusText("Entity unlocked");
  }, "Release the currently locked entity.");

  // ---- Lens ----
  g_Lens.AddFloat("Field of View", &g_CamFOV, 5.0f, 130.0f, 1.0f, 0, nullptr,
                  "Lens zoom in degrees. Lower = more zoom.");
  g_Lens.AddFloat("Lens Roll", &g_CamRoll, -180.0f, 180.0f, 1.0f, 0, nullptr,
                  "Dutch-angle tilt in degrees.");

  // ---- Depth of Field ----
  g_DoF.AddToggle("Depth of Field", &g_DoFEnabled, nullptr,
                  "Blur everything except what's in focus (cinematic bokeh).");
  g_DoF.AddToggle("Auto-Focus", &g_DoFAutofocus, nullptr,
                  "Focus on whatever is at the centre of the screen.");
  g_DoF.AddFloat("Manual Focus Dist.", &g_DoFFocusDist, 0.5f, 500.0f, 1.0f, 1, nullptr,
                 "Distance (m) the lens focuses at when Auto-Focus is off.");
  g_DoF.AddFloat("Near Focus Range", &g_DoFMaxNearInFocus, 0.0f, 50.0f, 0.1f, 1, nullptr,
                 "How far in FRONT of the focus point stays sharp (metres).");
  g_DoF.AddFloat("Far Focus Range", &g_DoFMaxFarInFocus, 0.0f, 50.0f, 0.1f, 1, nullptr,
                 "How far BEHIND the focus point stays sharp (metres).");

  // ---- Camera Effects (shake) ----
  // Editing any numeric shake row marks the preset Custom (index 5), matching
  // the classic menu's MarkShakeCustom behaviour.
  auto markCustom = [](float) { g_ShakePreset = 5; };
  g_Effects.AddToggle("Enabled", &g_ShakeEnabled, nullptr,
                      "Turn the procedural handheld camera shake on or off.");
  g_Effects.AddList("Preset", &g_ShakePreset,
                    {"Off", "Subtle", "Handheld", "Vehicle", "Earthquake", "Custom"},
                    [](int idx) { if (idx >= 0 && idx < 5) ApplyShakePreset(idx); },
                    "Pick a ready-made feel; editing a value below switches to Custom.");
  g_Effects.AddFloat("Base Amplitude", &g_ShakeAmp, 0.0f, 3.0f, 0.05f, 2, markCustom,
                     "Overall strength of the shake. 0 = none.");
  g_Effects.AddFloat("Base Frequency (Hz)", &g_ShakeFreq, 0.05f, 20.0f, 0.1f, 2, markCustom,
                     "How fast the shake oscillates. Higher = jittery, lower = slow sway.");
  g_Effects.AddFloat("Speed -> Amplitude", &g_ShakeSpeedAmpCoupling, 0.0f, 2.0f, 0.1f, 2, markCustom,
                     "How much faster camera movement increases the shake strength.");
  g_Effects.AddFloat("Speed -> Frequency", &g_ShakeSpeedFreqCoupling, 0.0f, 2.0f, 0.1f, 2, markCustom,
                     "How much faster camera movement speeds up the shake.");
  g_Effects.AddFloat("Rotation Weight", &g_ShakeRotWeight, 0.0f, 2.0f, 0.1f, 2, markCustom,
                     "How much the shake rotates the camera (vs. shifting it).");
  g_Effects.AddFloat("Position Weight", &g_ShakePosWeight, 0.0f, 2.0f, 0.1f, 2, markCustom,
                     "How much the shake shifts the camera (vs. rotating it).");
  g_Effects.AddToggle("Stop When Still", &g_ShakeStopWhenStill, nullptr,
                      "Fade the shake out when the camera isn't moving.");
  g_Effects.AddButton("Randomize Pattern", [] {
    RandomizeShakePattern();
    SetStatusText("Shake pattern randomized");
  }, "Re-roll the random noise so the shake feels different.");

  // ---- World & Scene ----
  g_World.AddSubmenu("Time & Weather", &g_Time, "Clock, time-lapse, weather.");
  g_World.AddSubmenu("Auto Drive", &g_AutoDrive,
                     "Let the AI drive your car while you film.");
  g_World.AddToggle("Hide Game HUD", &g_HideHUD, nullptr,
                    "Hide the game's HUD (radar, health, weapon) for clean shots.");
  g_World.AddToggle("Hide Player Character", &g_HidePlayer, nullptr,
                    "Make your character invisible while filming.");
  g_World.AddToggle("Stream Around Camera", &g_StreamAroundCamera, nullptr,
                    "Keep the world's detail (LOD, HD textures, props) loaded "
                    "around the camera instead of your character. Fixes low "
                    "detail / pop-in when filming far from the player. Only "
                    "acts while the player is HIDDEN; unhide the player to film "
                    "your own character and it stays in place.");
  g_World.AddToggle("Clear Vehicles", &g_ClearVehicles, nullptr,
                    "Empties the map of traffic and keeps it empty: deletes all "
                    "ambient vehicles and stops new ones spawning. Spares your "
                    "own vehicle and any recorded / ghost vehicles.");
  g_World.AddToggle("Clear Peds", &g_ClearPeds, nullptr,
                    "Empties the map of pedestrians and keeps it empty: deletes "
                    "all ambient peds and stops new ones spawning. Spares you and "
                    "any recorded ghost's driver.");
  g_World.AddToggle("Disable Vehicle Shake", &g_DisableVehicleShake, nullptr,
                    "Removes the engine-induced body jitter vehicles have while idling "
                    "and driving, so cars sit dead still for clean shots.");
  g_World.AddToggle("Show Info Overlay", &g_ShowInfoOverlay, nullptr,
                    "Show a debug overlay with the camera's position and FOV.");
  g_World.AddSeparator("");
  g_World.AddButton("Save Settings to INI", [] {
    SaveSettings();
    SetStatusText("Settings saved to SimpleCamera.ini");
  }, "Write all current settings to SimpleCamera.ini so they persist.");
  g_World.AddButton("Reset to Defaults", [] {
    ResetSettingsToDefaults();
    // Re-apply the appearance globals to the live menu.
    ApplyMenuTheme();
    g_Ctrl.SetPosition(g_MenuPosX, g_MenuPosY);
    g_Ctrl.SetScale(g_MenuScale);
    SetStatusText("Settings reset to defaults");
  }, "Restore every tunable to its factory value.");

  // ---- Time & Weather ----
  g_Time.AddToggle("Pause Time of Day", &g_TimePaused, nullptr,
                   "Freeze the in-game clock so the sun and lighting stop moving.");
  g_Time.AddInt("Hour", &g_TimeHour, 0, 23, 1, [](int) {
    if (!g_TimePaused) SetClockTime(g_TimeHour, g_TimeMinute, 0);
  }, "Set the hour of the in-game clock (0-23).");
  g_Time.AddInt("Minute", &g_TimeMinute, 0, 59, 1, [](int) {
    if (!g_TimePaused) SetClockTime(g_TimeHour, g_TimeMinute, 0);
  }, "Set the minute of the in-game clock (0-59).");
  g_Time.AddList("Time-lapse Speed", &g_TimelapseMode,
                 {"Off", "Slow", "Medium", "Fast"}, nullptr,
                 "Auto-advance the clock for time-lapse shots. Off = normal speed.");
  g_Time.AddList("Primary Weather", &g_Weather1Index, g_WeatherOpts, nullptr,
                 "The main weather type, applied when you press Apply Weather.");
  g_Time.AddList("Secondary Weather", &g_Weather2Index, g_WeatherOpts, nullptr,
                 "A second weather to blend toward (use the Weather Blend slider).");
  g_Time.AddFloat("Weather Blend", &g_WeatherBlend, 0.0f, 1.0f, 0.05f, 2, nullptr,
                  "Mix between primary and secondary weather. 0% = primary only.");
  g_Time.AddButton("Apply Weather", [] {
    if (g_WeatherBlend <= 0.0f) {
      g_BlendWeatherActive = false;
      GAMEPLAY::SET_WEATHER_TYPE_NOW_PERSIST((char *)g_WeatherNames[g_Weather1Index]);
      SetStatusText(std::string("Weather: ") + g_WeatherNames[g_Weather1Index]);
    } else {
      g_BlendWeatherActive = true;
      SetStatusText(std::string("Blended: ") + g_WeatherNames[g_Weather1Index] +
                    " | " + g_WeatherNames[g_Weather2Index]);
    }
  }, "Commit the chosen weather (or blend) to the world.");
  g_Time.AddToggle("Pause Game (full freeze)", &g_FreezeWorld, nullptr,
                   "Pause the entire world - everything halts (overrides Slow Motion).");
  g_Time.AddToggle("Freeze All Entities", &g_FreezeEntities, nullptr,
                   "Freeze peds and vehicles in place while the camera stays live.");
  g_Time.AddFloat("Slow Motion", &g_WorldTimeScale, 0.01f, 1.0f, 0.01f, 2, nullptr,
                  "World time scale for slow-motion. 1.00 = real time, lower = slower. "
                  "Works in both Free Camera and Sequence modes; auto-suspended while "
                  "an image-sequence render runs (which uses its own slow-mo).");

  // ---- Auto Drive ----
  g_AutoDrive.AddToggle("Enabled", &g_AutoDriveEnabled, [](bool on) {
    if (!on) AutoDrive_Stop();
  }, "AI drives your current land vehicle.");
  g_AutoDrive.AddList("Destination", &g_AutoDriveMode,
                      {"Go To Waypoint", "Drive Anywhere"}, nullptr,
                      "Drive to your map waypoint, or wander the roads freely.");
  g_AutoDrive.AddFloat("Speed (m/s)", &g_AutoDriveSpeed, 1.0f, 100.0f, 1.0f, 0, nullptr,
                       "Target driving speed for the AI, in metres per second.");
  g_AutoDrive.AddList("Driving Style", &g_AutoDriveStyleIndex, g_StyleOpts, nullptr,
                      "How the AI drives - cautious, normal, or rushed/aggressive.");

  // ---- Misc ----
  g_Misc.AddToggle("Save Position on Exit", &g_RememberCamPosition, nullptr,
                   "Remember the camera's spot so it reopens where you left it.");
  g_Misc.AddToggle("Lock Camera Position", &g_LockCamera, nullptr,
                   "Freeze the camera completely - all movement and rotation "
                   "input is ignored until you turn this off.");
  g_Misc.AddToggle("Allow Player to Move", &g_EnablePlayerMovement, [](bool on) {
    Ped ped = PLAYER::PLAYER_PED_ID();
    ENTITY::FREEZE_ENTITY_POSITION(ped, on ? FALSE : TRUE);
    ENTITY::SET_ENTITY_COLLISION(ped, on ? TRUE : FALSE, FALSE);
  }, "Let your character walk around while the camera is active.");
  g_Misc.AddButton("Snap Camera to Player", [] {
    SnapCameraToPlayer();
    SetStatusText("Camera snapped to player");
  }, "Jump the camera back to your character's position.");
  g_Misc.AddButton("Level Horizon (reset roll)", [] {
    LevelCameraHorizon();
    SetStatusText("Horizon leveled (roll reset)");
  }, "Reset any side-tilt (Dutch angle) back to level.");

  // ---- Appearance: hub of focused sub-pages ----
  g_Appearance.AddSubmenu("Menu Layout", &g_ApLayout,
                          "Where the menu sits and how big it is.");
  g_Appearance.AddSubmenu("Menu Colours", &g_ApColors,
                          "Accent, background, text and selected-row colours.");
  g_Appearance.AddSubmenu("Sequence Markers", &g_ApMarkers,
                          "Keyframe and camera-path appearance in Camera Sequence.");
  g_Appearance.AddSeparator("");
  g_Appearance.AddButton("Save Appearance to INI", [] {
    SaveSettings();
    SetStatusText("Appearance saved to SimpleCamera.ini");
  }, "Persist these appearance settings so they load next session.");

  // ---- Menu Layout ----
  g_ApLayout.AddFloat("Menu Position X", &g_MenuPosX, 0.0f, 0.9f, 0.005f, 3,
                      [](float) { g_Ctrl.SetPosition(g_MenuPosX, g_MenuPosY); },
                      "Horizontal position of the menu (fraction of screen width).");
  g_ApLayout.AddFloat("Menu Position Y", &g_MenuPosY, 0.0f, 0.85f, 0.005f, 3,
                      [](float) { g_Ctrl.SetPosition(g_MenuPosX, g_MenuPosY); },
                      "Vertical position of the menu (fraction of screen height).");
  g_ApLayout.AddFloat("Menu Scale", &g_MenuScale, 0.6f, 2.0f, 0.05f, 2,
                      [](float) { g_Ctrl.SetScale(g_MenuScale); },
                      "Overall menu size multiplier.");

  // ---- Menu Colours ---- (every slider recolours the menu live)
  auto recolor = [](int) { ApplyMenuTheme(); };
  g_ApColors.AddSeparator("Accent (bars, outline, scrollbar)");
  g_ApColors.AddInt("Accent Red", &g_MenuAccentR, 0, 255, 5, recolor, "Accent colour red channel.");
  g_ApColors.AddInt("Accent Green", &g_MenuAccentG, 0, 255, 5, recolor, "Accent colour green channel.");
  g_ApColors.AddInt("Accent Blue", &g_MenuAccentB, 0, 255, 5, recolor, "Accent colour blue channel.");
  g_ApColors.AddSeparator("Background (panel)");
  g_ApColors.AddInt("Bg Red", &g_MenuBgR, 0, 255, 5, recolor, "Panel background red channel.");
  g_ApColors.AddInt("Bg Green", &g_MenuBgG, 0, 255, 5, recolor, "Panel background green channel.");
  g_ApColors.AddInt("Bg Blue", &g_MenuBgB, 0, 255, 5, recolor, "Panel background blue channel.");
  g_ApColors.AddSeparator("Text");
  g_ApColors.AddInt("Text Red", &g_MenuTextR, 0, 255, 5, recolor, "Menu text red channel.");
  g_ApColors.AddInt("Text Green", &g_MenuTextG, 0, 255, 5, recolor, "Menu text green channel.");
  g_ApColors.AddInt("Text Blue", &g_MenuTextB, 0, 255, 5, recolor, "Menu text blue channel.");
  g_ApColors.AddSeparator("Selected Row");
  g_ApColors.AddInt("Selected Bg Red", &g_MenuSelR, 0, 255, 5, recolor, "Highlighted row fill red channel.");
  g_ApColors.AddInt("Selected Bg Green", &g_MenuSelG, 0, 255, 5, recolor, "Highlighted row fill green channel.");
  g_ApColors.AddInt("Selected Bg Blue", &g_MenuSelB, 0, 255, 5, recolor, "Highlighted row fill blue channel.");
  g_ApColors.AddSeparator("Selected Text");
  g_ApColors.AddInt("Selected Text Red", &g_MenuSelTextR, 0, 255, 5, recolor, "Highlighted row text red channel.");
  g_ApColors.AddInt("Selected Text Green", &g_MenuSelTextG, 0, 255, 5, recolor, "Highlighted row text green channel.");
  g_ApColors.AddInt("Selected Text Blue", &g_MenuSelTextB, 0, 255, 5, recolor, "Highlighted row text blue channel.");

  // ---- Sequence Markers ----
  g_ApMarkers.AddFloat("Keyframe Size", &g_SeqMarkerSize, 0.1f, 1.0f, 0.05f, 2, nullptr,
                       "Size of the in-world keyframe spheres in Camera Sequence.");
  g_ApMarkers.AddInt("Keyframe Red", &g_SeqMarkerR, 0, 255, 5, nullptr, "Normal keyframe marker red channel.");
  g_ApMarkers.AddInt("Keyframe Green", &g_SeqMarkerG, 0, 255, 5, nullptr, "Normal keyframe marker green channel.");
  g_ApMarkers.AddInt("Keyframe Blue", &g_SeqMarkerB, 0, 255, 5, nullptr, "Normal keyframe marker blue channel.");
  g_ApMarkers.AddInt("Path Red", &g_SeqPathR, 0, 255, 5, nullptr, "Camera path line red channel.");
  g_ApMarkers.AddInt("Path Green", &g_SeqPathG, 0, 255, 5, nullptr, "Camera path line green channel.");
  g_ApMarkers.AddInt("Path Blue", &g_SeqPathB, 0, 255, 5, nullptr, "Camera path line blue channel.");
  g_ApMarkers.AddList("Path Timestamps", &g_SeqTimeLabelMode,
                      {"Off", "0.5s", "1s", "2s", "5s"}, nullptr,
                      "How often to label time along the camera and vehicle "
                      "paths in the world. Off hides the labels.");
}

// ============================================================
//  Camera Sequence — reorganized onto the new framework
// ============================================================
//
// The classic Sequence menu was one flat 18-row screen. Here it's regrouped:
// the few common actions (Capture / Play / Stop / Scrub) sit on the root, and
// everything else is a focused submenu. Variable-length lists (poses, events,
// sequences) are rebuilt each frame they're shown; per-item editors bind to
// stable mirror structs (s_pm / s_em) and push edits to the live element by
// index, so a vector reallocation (capture/add/sort) can never dangle a bound
// pointer.

static float SeqEventValueStep(int kindI) {
  switch (kindI) {
  case EFX_SHAKE_ENABLED:
  case EFX_SHAKE_PRESET:
  case EFX_SHAKE_STOP_STILL:
  case EFX_SHAKE_RANDOMIZE:
    return 1.0f; // discrete kinds step by whole units
  default:
    return 0.05f;
  }
}

// ---- Pose editor mirror <-> live keyframe ----
static void LoadPoseMirror() {
  CameraSequence *s = Sequence_Active();
  if (!s || s_editPose < 0 || s_editPose >= (int)s->poses.size()) return;
  const PoseKeyframe &p = s->poses[s_editPose];
  s_pm.t = p.t; s_pm.posX = p.posX; s_pm.posY = p.posY; s_pm.posZ = p.posZ;
  s_pm.pitch = p.pitch; s_pm.yaw = p.yaw; s_pm.roll = p.roll; s_pm.fov = p.fov;
  s_pm.easeI = (int)p.ease; s_pm.pathI = (int)p.path;
}
static void WritePoseMirror() {
  CameraSequence *s = Sequence_Active();
  if (!s || s_editPose < 0 || s_editPose >= (int)s->poses.size()) {
    s_seqPopRequested = true; // pose vanished under us — leave the editor
    return;
  }
  PoseKeyframe &p = s->poses[s_editPose];
  p.t = s_pm.t; p.posX = s_pm.posX; p.posY = s_pm.posY; p.posZ = s_pm.posZ;
  p.pitch = s_pm.pitch; p.yaw = s_pm.yaw; p.roll = s_pm.roll; p.fov = s_pm.fov;
  p.ease = (EaseType)s_pm.easeI; p.path = (PathType)s_pm.pathI;
}
static std::string PoseLockLabel() {
  CameraSequence *s = Sequence_Active();
  if (!s || s_editPose < 0 || s_editPose >= (int)s->poses.size()) return "None";
  const PoseKeyframe &p = s->poses[s_editPose];
  if (p.entityHandle != 0)
    return ENTITY::DOES_ENTITY_EXIST(p.entityHandle) ? "Locked" : "Locked (gone)";
  if (p.localOffsetX || p.localOffsetY || p.localOffsetZ || p.lockEntPitch ||
      p.lockEntYaw || p.lockEntRoll)
    return "Authored locked";
  return "None";
}
static void TogglePoseLock() {
  CameraSequence *s = Sequence_Active();
  if (!s || s_editPose < 0 || s_editPose >= (int)s->poses.size()) return;
  PoseKeyframe &p = s->poses[s_editPose];
  if (p.entityHandle != 0) {
    p.entityHandle = 0;
    p.localOffsetX = p.localOffsetY = p.localOffsetZ = 0.0f;
    p.lockEntPitch = p.lockEntYaw = p.lockEntRoll = 0.0f;
    SetStatusText("Entity lock cleared");
  } else if ((g_FollowMode == 1) ||
             (g_FollowMode == 2 && g_FollowTargetEntity != 0 &&
              ENTITY::DOES_ENTITY_EXIST(g_FollowTargetEntity))) {
    Sequence_CaptureLockForPose(p);
    SetStatusText(p.entityHandle != 0 ? "Locked to free-cam target" : "Lock failed");
  } else {
    // No target picked yet — instead of a dead-end error, grab whatever the
    // camera is aimed at right now so this button works on its own, without a
    // prior trip to the Entity Lock page.
    int e = RaycastEntityFromCamera();
    if (e != 0 && ENTITY::DOES_ENTITY_EXIST(e)) {
      g_FollowTargetEntity = e;
      g_FollowMode = 2;
      Sequence_CaptureLockForPose(p);
      SetStatusText(p.entityHandle != 0 ? "Locked to the aimed entity"
                                        : "Lock failed");
    } else {
      p.localOffsetX = p.localOffsetY = p.localOffsetZ = 0.0f;
      p.lockEntPitch = p.lockEntYaw = p.lockEntRoll = 0.0f;
      SetStatusText("Aim the camera at a ped/vehicle, then press again");
    }
  }
}
static void RecapturePose() {
  CameraSequence *s = Sequence_Active();
  if (!s || s_editPose < 0 || s_editPose >= (int)s->poses.size()) return;
  PoseKeyframe &p = s->poses[s_editPose];
  float px, py, pz, pi, ya, ro;
  GetCameraState(px, py, pz, pi, ya, ro);
  p.posX = px; p.posY = py; p.posZ = pz;
  p.pitch = pi; p.yaw = ya; p.roll = ro; p.fov = g_CamFOV;
  Sequence_CaptureLockForPose(p);
  LoadPoseMirror();
  SetStatusText("Pose recaptured from live camera");
}
static void DeleteEditPose() {
  if (s_editPose >= 0) {
    Sequence_DeletePose(s_editPose);
    SetStatusText("Keyframe deleted");
  }
  s_editPose = -1;
  s_seqPopRequested = true;
}

// ---- Event editor mirror <-> live event ----
static void LoadEventMirror() {
  CameraSequence *s = Sequence_Active();
  if (!s || s_editEvent < 0 || s_editEvent >= (int)s->events.size()) return;
  const EffectEvent &e = s->events[s_editEvent];
  s_em.t = e.t; s_em.value = e.value; s_em.kindI = (int)e.kind; s_em.ramp = e.ramp;
}
static void WriteEventMirror() {
  CameraSequence *s = Sequence_Active();
  if (!s || s_editEvent < 0 || s_editEvent >= (int)s->events.size()) {
    s_seqPopRequested = true;
    return;
  }
  EffectEvent &e = s->events[s_editEvent];
  e.t = s_em.t; e.value = s_em.value; e.kind = (EffectKind)s_em.kindI; e.ramp = s_em.ramp;
}
static void DeleteEditEvent() {
  if (s_editEvent >= 0) {
    Sequence_DeleteEvent(s_editEvent);
    SetStatusText("Event deleted");
  }
  s_editEvent = -1;
  s_seqPopRequested = true;
}
static void AddEventAndEdit() {
  float t = Sequence_CurrentTime();
  Sequence_AddEvent(EFX_SHAKE_ENABLED, t, 1.0f, false);
  CameraSequence *s = Sequence_Active();
  int idx = -1;
  if (s)
    for (int i = 0; i < (int)s->events.size(); ++i) {
      const EffectEvent &e = s->events[i];
      if (e.kind == EFX_SHAKE_ENABLED && fabsf(e.t - t) < 0.001f &&
          e.value == 1.0f && !e.ramp) { idx = i; break; }
    }
  if (idx >= 0) { s_editEvent = idx; g_Ctrl.Push(&g_SeqEventEdit); }
  else SetStatusText("Event added");
}

static void BakeLockedPoses() {
  CameraSequence *s = Sequence_Active();
  int n = 0;
  if (s)
    for (PoseKeyframe &p : s->poses)
      if (p.entityHandle != 0 && ENTITY::DOES_ENTITY_EXIST(p.entityHandle)) {
        Vector3 w = invoke<Vector3>(0x1899F328B0E12848, p.entityHandle,
                                    p.localOffsetX, p.localOffsetY, p.localOffsetZ);
        p.posX = w.x; p.posY = w.y; p.posZ = w.z; ++n;
      }
  char b[64];
  sprintf_s(b, "Baked %d locked poses to world coords", n);
  SetStatusText(b);
}

// Hover marker on the entity under the crosshair while no target is locked
// yet — so the user sees what "Lock Aimed Entity" will grab. Drawn in any
// follow mode: picking a target is the page's step 1, mode is set on lock.
static void DrawSeqHoverMarker() {
  if (g_FollowMode == 1) return;                            // player is the target
  if (g_FollowMode == 2 && g_FollowTargetEntity != 0) return; // already locked
  int e = RaycastEntityFromCamera();
  if (e == 0 || !ENTITY::DOES_ENTITY_EXIST(e)) return;
  Vector3 p = ENTITY::GET_ENTITY_COORDS(e, TRUE);
  GRAPHICS::DRAW_MARKER(0, p.x, p.y, p.z + 1.25f, 0, 0, 0, 0, 0, 0, 0.4f, 0.4f,
                        0.4f, 255, 255, 255, 200, TRUE, TRUE, 2, FALSE, NULL,
                        NULL, FALSE);
}

// The free-cam's current lock target, resolved the same way keyframe locking
// resolves it (mode 1 = the player ped, mode 2 = the raycast/picked entity).
static int SeqLockTarget() {
  int t = 0;
  if (g_FollowMode == 1) t = PLAYER::PLAYER_PED_ID();
  else if (g_FollowMode == 2) t = g_FollowTargetEntity;
  if (t == 0 || !ENTITY::DOES_ENTITY_EXIST(t)) return 0;
  return t;
}

// One-line description of the lock target for status rows:
// "SULTAN, 42m" / "Player, 8m" / "Ped, 12m" / "" when none.
static std::string SeqLockTargetLabel() {
  int t = SeqLockTarget();
  if (t == 0) return std::string();
  float x, y, z, pi, ya, ro;
  GetCameraState(x, y, z, pi, ya, ro);
  Vector3 p = ENTITY::GET_ENTITY_COORDS(t, TRUE);
  float d = sqrtf((p.x - x) * (p.x - x) + (p.y - y) * (p.y - y) +
                  (p.z - z) * (p.z - z));
  char b[64];
  if (ENTITY::IS_ENTITY_A_VEHICLE(t)) {
    const char *nm =
        VEHICLE::GET_DISPLAY_NAME_FROM_VEHICLE_MODEL(ENTITY::GET_ENTITY_MODEL(t));
    sprintf_s(b, "%s, %.0fm", (nm && nm[0]) ? nm : "Vehicle", d);
  } else if (ENTITY::IS_ENTITY_A_PED(t)) {
    sprintf_s(b, "%s, %.0fm", (g_FollowMode == 1) ? "Player" : "Ped", d);
  } else {
    sprintf_s(b, "Object, %.0fm", d);
  }
  return std::string(b);
}

// ---- Dynamic list (re)builders ----
static void RebuildPoseList() {
  int sel = g_SeqPoses.selected, scroll = g_SeqPoses.scrollOffset;
  g_SeqPoses.items.clear();
  g_SeqPoses.AddButton("+ Capture Pose at camera", [] {
    int i = Sequence_CapturePoseAtCurrentTime();
    if (i >= 0) SetStatusText("Pose captured");
  }, "Add a keyframe at the camera's current position and angle.").rightLabel = "F6";
  CameraSequence *s = Sequence_Active();
  int n = s ? (int)s->poses.size() : 0;
  if (n >= 1)
    g_SeqPoses.AddFloat("Total Duration (s)", &s_totalDur, 0.1f, 100000.0f, 0.1f,
                        2, [](float v) {
                          float cur = Sequence_TotalDuration();
                          if (cur > 0.01f && v > 0.01f) Sequence_ScaleTimes(v / cur);
                        }, "Stretch or compress every keyframe + event time at once.");
  if (n >= 1)
    g_SeqPoses.AddButton("Move All Keyframes...", [] { g_Ctrl.Push(&g_SeqMoveAll); },
                         "Shift every keyframe's position together to relocate the whole "
                         "shot to a new spot on the map without re-authoring it.");
  if (n >= 3)
    g_SeqPoses.AddButton("Distribute Times Evenly", [] {
      int c = Sequence_DistributeTimes();
      char b[48];
      sprintf_s(b, "Re-timed %d keyframe%s evenly", c, c == 1 ? "" : "s");
      SetStatusText(b);
    }, "Re-space all middle keyframes evenly between the first and last one's "
       "times - a constant-speed pass. Positions are untouched.");
  // Everything above this line is a header row; keyframe rows start here.
  // Recorded so the browse-preview in SeqMenu_FrameSync can map the list
  // selection back to a keyframe index without hardcoding row counts.
  s_poseListHeaderRows = (int)g_SeqPoses.items.size();
  // Playhead cursor: mark the keyframe at/just before the current scrub time.
  int curKf = -1;
  if (s) {
    float ph = Sequence_CurrentTime();
    for (int i = 0; i < n; ++i)
      if (s->poses[i].t <= ph + 0.001f) curKf = i;
  }
  for (int i = 0; i < n; ++i) {
    bool locked = s->poses[i].entityHandle != 0;
    char label[40];
    sprintf_s(label, "%sKeyframe %d%s", (i == curKf) ? "> " : "", i,
              locked ? "  [L]" : "");
    g_SeqPoses.AddButton(label, [i] { s_editPose = i; g_Ctrl.Push(&g_SeqPoseEdit); },
                         "Open this keyframe to edit its pose, timing, easing and lock. "
                         "\"> \" marks the playhead's keyframe; [L] = entity-locked. "
                         "Moving the selection previews each keyframe through the camera.")
        .valueGetter = [i] {
          CameraSequence *s2 = Sequence_Active();
          if (!s2 || i >= (int)s2->poses.size()) return std::string();
          const PoseKeyframe &p = s2->poses[i];
          char b[96];
          sprintf_s(b, "t=%.2f fov=%.0f %s/%s", p.t, p.fov, EaseName(p.ease),
                    PathName(p.path));
          return std::string(b);
        };
  }
  int cnt = (int)g_SeqPoses.items.size();
  if (sel >= cnt) sel = cnt - 1; if (sel < 0) sel = 0;
  g_SeqPoses.selected = sel; g_SeqPoses.scrollOffset = scroll;
}
static void RebuildEventList() {
  int sel = g_SeqEvents.selected, scroll = g_SeqEvents.scrollOffset;
  g_SeqEvents.items.clear();
  g_SeqEvents.AddButton("+ Add event at current time", [] { AddEventAndEdit(); },
                        "Create a new timed effect change at the playhead.");
  CameraSequence *s = Sequence_Active();
  int n = s ? (int)s->events.size() : 0;
  for (int i = 0; i < n; ++i) {
    char label[24]; sprintf_s(label, "Event %d", i);
    g_SeqEvents.AddButton(label, [i] { s_editEvent = i; g_Ctrl.Push(&g_SeqEventEdit); },
                          "Open this event to change its effect, value and timing.")
        .valueGetter = [i] {
          CameraSequence *s2 = Sequence_Active();
          if (!s2 || i >= (int)s2->events.size()) return std::string();
          const EffectEvent &e = s2->events[i];
          char b[96];
          sprintf_s(b, "t=%.2f %s=%.2f %s", e.t, EffectName(e.kind), e.value,
                    e.ramp ? "ramp" : "snap");
          return std::string(b);
        };
  }
  int cnt = (int)g_SeqEvents.items.size();
  if (sel >= cnt) sel = cnt - 1; if (sel < 0) sel = 0;
  g_SeqEvents.selected = sel; g_SeqEvents.scrollOffset = scroll;
}
static void RebuildSeqList() {
  int sel = g_SeqList.selected, scroll = g_SeqList.scrollOffset;
  g_SeqList.items.clear();
  g_SeqList.AddButton("Teleport to Sequence", [] {
    if (Sequence_TeleportToStart())
      SetStatusText("Camera moved to first keyframe");
    else
      SetStatusText("No keyframes in this sequence");
  }, "Jump the flycam to this sequence's first keyframe so you don't have to fly "
     "across the map. With Stream Around Camera on, the area's LODs stream in "
     "around the new position.");
  g_SeqList.AddButton("New Sequence", [] {
    Sequence_New("Untitled");
    SetStatusText("New sequence created");
  }, "Create a new, empty sequence and make it active.");
  g_SeqList.AddButton("Delete Active", [] {
    DWORD now = GetTickCount();
    if (now < s_seqDelArmed) {
      Sequence_DeleteActive(); s_seqDelArmed = 0; SetStatusText("Sequence deleted");
    } else {
      s_seqDelArmed = now + 3000; SetStatusText("Press again to confirm delete");
    }
  }, "Delete the current sequence. Press twice within 3s to confirm.")
      .valueGetter = [] { return std::string(GetTickCount() < s_seqDelArmed ? "Confirm?" : ""); };
  g_SeqList.AddButton("Save Sequence", [] {
    Sequence_SaveAll(); SetStatusText("Sequence saved");
  }, "Save the current sequence (keyframes, events and its vehicle clips) to "
     "its JSON file in SimpleCamera_Sequences\\. Other sequences are saved "
     "automatically when you switch away from them.");
  g_SeqList.AddButton("Reload from Disk", [] {
    DWORD now = GetTickCount();
    if (now < s_seqReloadArmed) {
      Sequence_Stop(); Sequence_LoadAll(); s_seqReloadArmed = 0;
      SetStatusText("Sequences reloaded from disk");
    } else {
      s_seqReloadArmed = now + 3000;
      SetStatusText("Press again to discard unsaved changes and reload");
    }
  }, "Re-read all sequences from SimpleCamera_Sequences\\ on disk, discarding "
     "any unsaved in-memory changes. Useful after hand-editing a JSON. Press "
     "twice within 3s to confirm.")
      .valueGetter = [] {
        return std::string(GetTickCount() < s_seqReloadArmed ? "Confirm?" : "");
      };
  g_SeqList.AddSeparator("Select Active");
  int n = Sequence_Count();
  for (int i = 0; i < n; ++i) {
    CameraSequence *cs = Sequence_At(i);
    std::string nm = cs ? cs->name : std::string("?");
    g_SeqList.AddButton(nm, [i] { Sequence_SetActive(i); SetStatusText("Active sequence set"); },
                        "Make this the active sequence to edit and play.")
        .valueGetter = [i] { return std::string(i == Sequence_ActiveIndex() ? "Active" : ""); };
  }
  int cnt = (int)g_SeqList.items.size();
  if (sel >= cnt) sel = cnt - 1; if (sel < 0) sel = 0;
  g_SeqList.selected = sel; g_SeqList.scrollOffset = scroll;
}
// Entity Lock page — rebuilt every frame it's shown so the status rows are
// live. Laid out as a guided two-step flow (pick a target -> lock keyframes)
// because the old flat button pile hid the ordering dependency and most users
// never discovered how the feature works.
static void RebuildSeqFollow() {
  int sel = g_SeqFollow.selected, scroll = g_SeqFollow.scrollOffset;
  g_SeqFollow.items.clear();

  std::string tgt = SeqLockTargetLabel();
  bool hasTarget = !tgt.empty();
  int lockedKf = Sequence_LockedPoseCount();
  CameraSequence *s = Sequence_Active();
  int kfN = s ? (int)s->poses.size() : 0;

  // What the feature does, in plain words (labels are skipped by navigation).
  g_SeqFollow.AddLabel("Anchor the camera path to a moving ped or");
  g_SeqFollow.AddLabel("vehicle: author around it parked, then it rides.");

  g_SeqFollow.AddSeparator("Status");
  g_SeqFollow.AddLabel(hasTarget ? (std::string("Target:  ") + tgt)
                                 : std::string("Target:  none - pick one below"));
  {
    char b[48];
    sprintf_s(b, "Locked keyframes:  %d / %d", lockedKf, kfN);
    g_SeqFollow.AddLabel(b);
  }

  g_SeqFollow.AddSeparator("Step 1 - Pick a Target");
  g_SeqFollow.AddButton("Lock Aimed Entity", [] {
    int e = RaycastEntityFromCamera();
    if (e) { g_FollowTargetEntity = e; g_FollowMode = 2; SetStatusText("Target locked"); }
    else SetStatusText("Nothing under the crosshair - aim closer");
  }, "Aim the center of the screen at a ped/vehicle/object and press. The "
     "white marker in the world shows what will be grabbed.");
  g_SeqFollow.AddButton("Lock Nearest Vehicle", [] {
    float x, y, z, pi, ya, ro;
    GetCameraState(x, y, z, pi, ya, ro);
    int v = VEHICLE::GET_CLOSEST_VEHICLE(x, y, z, 30.0f, 0, 70);
    if (v && ENTITY::DOES_ENTITY_EXIST(v)) { g_FollowTargetEntity = v; g_FollowMode = 2; SetStatusText("Nearest vehicle locked"); }
    else SetStatusText("No vehicle within 30m");
  }, "Lock the closest vehicle - works at close range where the raycast can fail.");
  g_SeqFollow.AddButton("Lock Player's Vehicle", [] {
    Ped p = PLAYER::PLAYER_PED_ID();
    int v = PED::IS_PED_IN_ANY_VEHICLE(p, FALSE) ? PED::GET_VEHICLE_PED_IS_IN(p, FALSE) : 0;
    if (v && ENTITY::DOES_ENTITY_EXIST(v)) { g_FollowTargetEntity = v; g_FollowMode = 2; SetStatusText("Player's vehicle locked"); }
    else SetStatusText("Player isn't in a vehicle");
  }, "Lock onto the car the player is currently sitting in.");
  g_SeqFollow.AddButton("Use the Player", [] {
    g_FollowMode = 1;
    SetStatusText("Player set as target");
  }, "Use the player ped as the target (\"track me as I walk\").");
  if (hasTarget)
    g_SeqFollow.AddButton("Release Target", [] {
      g_FollowTargetEntity = 0;
      if (g_FollowMode == 1) g_FollowMode = 0;
      SetStatusText("Target released");
    }, "Drop the current target. Keyframes already locked keep their lock.");

  g_SeqFollow.AddSeparator("Step 2 - Lock Keyframes to It");
  g_SeqFollow.AddLabel("New keyframes auto-lock while a target is set.");
  g_SeqFollow.AddButton("Lock All Keyframes to Target", [] {
    int t = SeqLockTarget();
    if (!t) { SetStatusText("Pick a target first (Step 1)"); return; }
    int n = Sequence_ApplyLockToAll(t);
    char b[64]; sprintf_s(b, "Locked %d keyframes to target", n); SetStatusText(b);
  }, "Anchor every keyframe to the target: each stores its offset from the "
     "entity, so the whole camera move rides along when it drives off.")
      .valueGetter = [] {
        char b[16];
        CameraSequence *s2 = Sequence_Active();
        sprintf_s(b, "%d/%d", Sequence_LockedPoseCount(),
                  s2 ? (int)s2->poses.size() : 0);
        return std::string(b);
      };
  g_SeqFollow.AddButton("Clear All Keyframe Locks", [] {
    int n = Sequence_ClearAllLocks();
    char b[64]; sprintf_s(b, "Cleared lock from %d keyframes", n); SetStatusText(b);
  }, "Remove entity locks from every keyframe. They keep their current world "
     "positions and stop following the entity.");
  g_SeqFollow.AddButton("Bake Locked Poses to World", [] { BakeLockedPoses(); },
                        "Advanced: rewrite each locked keyframe's world position to where "
                        "its entity is RIGHT NOW (locks stay). Use to re-base a shot after "
                        "moving the vehicle.");

  g_SeqFollow.AddSeparator("Live Camera Follow (free-fly)");
  g_SeqFollow.AddList("Follow Mode", &g_FollowMode,
                      {"None", "Player", "Aimed Entity"},
                      [](int m) { if (m != 2) g_FollowTargetEntity = 0; },
                      "What the free-fly camera tracks while authoring: nothing, the "
                      "player, or the locked entity. Also selects the lock target above.");
  g_SeqFollow.AddToggle("Rigid Mode", &g_FollowRigidMode, nullptr,
                        "Free-fly follow inherits the target's rotation, not just its position.");
  g_SeqFollow.AddToggle("Show Marker", &g_ShowLockedEntityMarker, nullptr,
                        "Draw a marker on the locked entity.");

  int cnt = (int)g_SeqFollow.items.size();
  if (sel >= cnt) sel = cnt - 1; if (sel < 0) sel = 0;
  g_SeqFollow.selected = sel; g_SeqFollow.scrollOffset = scroll;
}

static void BuildSeqPlayback() {
  g_SeqPlayback.items.clear();
  CameraSequence *s = Sequence_Active();
  if (!s) { g_SeqPlayback.AddLabel("No active sequence"); g_SeqPlayback.selected = 0; return; }
  g_SeqPlayback.AddToggle("Loop", &s->loop, nullptr,
                          "Repeat the sequence from the start when it reaches the end.");
  g_SeqPlayback.AddFloat("Playback Speed", &s->playbackSpeed, 0.05f, 8.0f, 0.05f, 2, nullptr,
                         "Playback speed multiplier. 2.0 plays twice as fast, 0.5 half.");
  g_SeqPlayback.AddButton("Close Loop", [] {
    int i = Sequence_CloseLoop();
    if (i >= 0) SetStatusText(Sequence_IsLoopClosed() ? "Loop closed" : "Closing keyframe added");
  }, "Add/snap a closing keyframe so a looped shot wraps seamlessly.")
      .valueGetter = [] {
    LoopGap g;
    if (Sequence_GetLoopGap(&g)) {
      char b[48];
      sprintf_s(b, "d=%.2fm/%.0fdeg", g.posDist, g.pitchDelta + g.yawDelta + g.rollDelta);
      return std::string(b);
    }
    return std::string("Press");
  };
  g_SeqPlayback.AddToggle("Show Markers", &g_SequenceShowMarkers, nullptr,
                          "Show the keyframe spheres and path preview in the world "
                          "(hide them before recording).");
  g_SeqPlayback.selected = 0;
}

// Snap the render preset mirrors to the live globals (called on entry).
static void SyncRenderMirrors() {
  int bi = 0; float bd = 1e9f;
  for (int i = 0; i < kRenderFpsCount; ++i) {
    float d = fabsf(kRenderFps[i] - g_RenderFps);
    if (d < bd) { bd = d; bi = i; }
  }
  s_fpsIdx = bi;
  int bj = 0, bb = 1 << 30;
  for (int i = 0; i < kRenderBlurCount; ++i) {
    int d = kRenderBlur[i] - g_RenderBlurSamples; if (d < 0) d = -d;
    if (d < bb) { bb = d; bj = i; }
  }
  s_blurIdx = bj;
}

// Rebuilt each frame it's shown so options that don't apply right now
// (blur off, PNG) gray out + read "n/a" instead of letting you tweak settings
// that have no effect.
static void RebuildSeqRender() {
  int sel = g_SeqRender.selected, scroll = g_SeqRender.scrollOffset;
  g_SeqRender.items.clear();

  const bool blurOn = g_RenderBlurSamples > 1;
  const bool jpeg = g_RenderFormat == 1;
  const float rdur = Sequence_TotalDuration();

  g_SeqRender.AddList("Output FPS", &s_fpsIdx,
                      {"24", "25", "30", "48", "50", "60", "120", "240"},
                      [](int i) { g_RenderFps = kRenderFps[i]; },
                      "Frame rate of the exported sequence (sets the total frame count).")
      .valueGetter = [] {
        char b[48];
        // Frame count over the active render range (whole sequence if no range).
        float d = Sequence_TotalDuration();
        float st = g_RenderRangeStart < 0 ? 0 : (g_RenderRangeStart > d ? d : g_RenderRangeStart);
        float en = (g_RenderRangeEnd > 0.0001f && g_RenderRangeEnd <= d) ? g_RenderRangeEnd : d;
        if (en <= st) { st = 0; en = d; }
        int frames = (int)((en - st) * g_RenderFps + 0.5f);
        sprintf_s(b, "%d fps - %d fr", (int)g_RenderFps, frames);
        return std::string(b);
      };

  // Optional render range — render only part of the sequence.
  g_SeqRender.AddFloat("Start Time (s)", &g_RenderRangeStart, 0.0f,
                       rdur > 0.1f ? rdur : 0.1f, 0.5f, 2, nullptr,
                       "Render from this point in the sequence (0 = start). Lets "
                       "you re-render just a section.");
  g_SeqRender.AddFloat("End Time (s)", &g_RenderRangeEnd, 0.0f,
                       rdur > 0.1f ? rdur : 0.1f, 0.5f, 2, nullptr,
                       "Render up to this point. 0 (or <= Start) = render to the "
                       "end of the sequence.")
      .valueGetter = [] {
        if (g_RenderRangeEnd <= 0.0001f) return std::string("to end");
        char b[16]; sprintf_s(b, "%.1fs", g_RenderRangeEnd); return std::string(b);
      };

  g_SeqRender.AddInt("Flush Frames", &g_RenderFlushFrames, 0, 20, 1, nullptr,
                     "Extra clean frames after the progress banner clears, before each grab.");
  g_SeqRender.AddList("Motion Blur", &s_blurIdx,
                      {"Off", "2", "4", "8", "16", "32", "64", "128"},
                      [](int i) { g_RenderBlurSamples = kRenderBlur[i]; },
                      "Sub-samples blended per output frame (Off = sharp). Each sample "
                      "is a live frame captured in slow motion, so the game renders "
                      "'samples' frames for every output frame. Needed in-game FPS is "
                      "about samples x output-fps x slow-mo (see the 'Needs >=' line "
                      "below). More samples = smoother blur but needs more FPS; if your "
                      "FPS is under that, the world overshoots and the blur/sync breaks. "
                      "Render time scales with samples, NOT slow-mo. Requires the "
                      "IgcsDof.fx shader enabled in ReShade.");
  {
    auto &it = g_SeqRender.AddFloat("Highlight Boost", &g_RenderHighlightBoost, 0.0f, 0.95f, 0.05f, 2, nullptr,
        "Extra brightness lift on blur streaks. Only used when Motion Blur is on.");
    it.enabled = blurOn;
    it.valueGetter = [] {
      if (g_RenderBlurSamples <= 1) return std::string("n/a");
      char b[8]; sprintf_s(b, "%d%%", (int)(g_RenderHighlightBoost * 100.0f + 0.5f));
      return std::string(b);
    };
  }
  g_SeqRender.AddList("Format", &g_RenderFormat, {"PNG (lossless)", "JPEG"}, nullptr,
                      "Image format: PNG is lossless/large, JPEG is smaller/lossy.");
  {
    auto &it = g_SeqRender.AddInt("JPEG Quality", &g_RenderJpegQuality, 10, 100, 5, nullptr,
        "JPEG quality 10-100. Only used when Format is JPEG.");
    it.enabled = jpeg;
    it.valueGetter = [] {
      if (g_RenderFormat != 1) return std::string("n/a");
      char b[8]; sprintf_s(b, "%d", g_RenderJpegQuality); return std::string(b);
    };
  }
  // (The old "World Slow-mo" override slider was removed: capture time scale
  // is always AUTO now — the self-tuning controller always picks a safe value,
  // and every hand-set fixed scale could only match it or break sync.)

  // Estimated in-game FPS needed for these settings. The render plays in slow
  // motion and accumulates `samples` consecutive live frames per output frame
  // (+ flush + a little overhead); each rendered frame advances game time by
  // slowmo/gameFps, and that work must fit the per-output-frame time budget
  // (renderSpeed / outFps). Solving for gameFps:
  //   needFps = (samples + flush + overhead) * slowmo * outFps / renderSpeed
  // Below this, slow-mo can't compensate, so the world overshoots and the blur /
  // sync breaks. (Slow-mo can't go below ~1% without the game misbehaving.)
  {
    CameraSequence *sq = Sequence_Active();
    float speed = (sq && sq->playbackSpeed > 0.0001f) ? sq->playbackSpeed : 1.0f;
    int samples = g_RenderBlurSamples > 1 ? g_RenderBlurSamples : 1;
    if (samples <= 1) {
      // No blur = instant capture: the playhead is frozen during each frame's
      // capture work, so sync can't break — in-game FPS only affects how long
      // the render takes.
      g_SeqRender.AddLabel("No blur: any in-game FPS stays in sync");
    } else {
      int workFrames = samples + g_RenderFlushFrames + 4; // 2 banner + ~advance
      const float slowmo = 0.003f; // AUTO's floor — time scale is always AUTO
      float needFps = (float)workFrames * slowmo * g_RenderFps / speed;

      float ft = GAMEPLAY::GET_FRAME_TIME();
      float curFps = (ft > 0.0001f) ? (1.0f / ft) : 0.0f;

      char info[96];
      sprintf_s(info, "Needs >= %.0f in-game FPS (Auto slow-mo)", needFps);
      g_SeqRender.AddLabel(info);
      if (curFps > 1.0f) {
        char cur[64];
        bool low = curFps < needFps * 0.95f;
        sprintf_s(cur, "Your FPS now: %.0f%s", curFps,
                  low ? "  - TOO LOW, blur may break" : "  - OK");
        g_SeqRender.AddLabel(cur);
      }
    }
  }

  g_SeqRender.AddButton("Start Render", [] {
    if (FxCapture_AddonPresent()) ProcessRenderToImages();
    else SetStatusText("Rendering needs ReShade + IgcsConnector addon");
  }, "Render the sequence to a numbered image folder. Needs the ReShade + "
     "IgcsConnector capture addon.")
      .valueGetter = [] {
        return std::string(FxCapture_AddonPresent() ? "Press Enter" : "Addon missing");
      };

  int cnt = (int)g_SeqRender.items.size();
  if (sel >= cnt) sel = cnt - 1;
  if (sel < 0) sel = 0;
  g_SeqRender.selected = sel;
  g_SeqRender.scrollOffset = scroll;
}

// Rebuilt each frame it's shown so the record status / clip length / button
// labels track the live recording state.
static void RebuildSeqVehicle() {
  int sel = g_SeqVehicle.selected, scroll = g_SeqVehicle.scrollOffset;
  g_SeqVehicle.items.clear();

  const bool rec = VehicleClip_IsRecording();
  const bool hasData = VehicleClip_HasData();
  const int clipCount = VehicleClip_Count();

  char info[96];
  if (rec)
    sprintf_s(info, "Recording vehicle #%d... %.1fs", clipCount + 1,
              VehicleClip_Duration());
  else if (clipCount > 0)
    sprintf_s(info, "%d vehicle%s,  %.1fs", clipCount, clipCount == 1 ? "" : "s",
              VehicleClip_Duration());
  else
    sprintf_s(info, "No vehicles recorded");
  g_SeqVehicle.AddLabel(info);

  {
    auto &it = g_SeqVehicle.AddButton(
        clipCount > 0 ? "Record Another Vehicle" : "Record Vehicle Path",
        [] {
          if (VehicleClip_StartRecording()) {
            s_seqCloseRequested = true; // hide the menu so the player can drive
            SetStatusText("Recording - drive, press the Menu key to stop");
          } else {
            SetStatusText("Get into a (non-ghost) vehicle first, then record");
          }
        },
        "Hands you the wheel: the free camera drops, you drive the take, and the "
        "Menu key stops it and brings the camera back. Any vehicles already "
        "recorded replay around you so you can choreograph against them. Each "
        "take shares the same timeline (t=0). (You must be sitting in a vehicle "
        "that isn't one of the ghosts.)");
    it.enabled = !rec;
    it.valueGetter = [] { return std::string("Press Enter"); };
  }

  {
    auto &it = g_SeqVehicle.AddButton(
        "Replay On Timeline",
        [] { VehicleClip_SetEnabled(!VehicleClip_Enabled()); },
        "When on, scrubbing/playing the sequence drives the recorded vehicle "
        "along its path so the camera stays in sync with it.");
    it.enabled = hasData && !rec;
    it.valueGetter = [] {
      return std::string(VehicleClip_Enabled() ? "On" : "Off");
    };
  }

  {
    auto &it = g_SeqVehicle.AddButton(
        "Delete Last Vehicle",
        [] {
          int n = VehicleClip_Count();
          if (n > 0) {
            VehicleClip_DeleteClip(n - 1);
            SetStatusText("Deleted last recorded vehicle");
          }
        },
        "Remove the most recently recorded vehicle (and its ghost), keeping the "
        "earlier ones.");
    it.enabled = clipCount > 0 && !rec;
  }

  {
    auto &it = g_SeqVehicle.AddButton(
        "Clear All Vehicles",
        [] { VehicleClip_Clear(); SetStatusText("All vehicle clips cleared"); },
        "Delete every recorded vehicle path and remove their ghosts.");
    it.enabled = hasData && !rec;
  }

  {
    auto &it = g_SeqVehicle.AddSubmenu(
        "Per-Vehicle Settings", &g_SeqVehicleList,
        "Tune each recorded vehicle on its own: playback speed, start-time "
        "offset, lights, engine, siren, driver, collision and godmode.");
    it.enabled = clipCount > 0;
    it.valueGetter = [] {
      char b[16]; sprintf_s(b, "%d", VehicleClip_Count()); return std::string(b);
    };
  }

  {
    auto &it = g_SeqVehicle.AddInt(
        "Sample Rate (Hz)", &g_VehicleClipSampleHz, 0, 120, 10, nullptr,
        "How many vehicle states per second to record. Playback interpolates, "
        "so a lower rate shrinks the saved clip with little visible cost. "
        "0 = capture every frame (largest, most exact).");
    it.enabled = !rec;
    it.valueGetter = [] {
      if (g_VehicleClipSampleHz <= 0) return std::string("Every frame");
      char b[16]; sprintf_s(b, "%d Hz", g_VehicleClipSampleHz); return std::string(b);
    };
  }

  g_SeqVehicle.AddFloat(
      "Steering Strength", &g_VehicleClipSteerGain, 0.0f, 3.0f, 0.25f, 2, nullptr,
      "Steering is replayed from the real recorded wheel angles, so 1.0 is "
      "exact. Raise it only for an exaggerated/cinematic wheel turn, lower for "
      "subtler, or 0 to disable. (Also scales steering on older clips recorded "
      "before per-wheel steer capture.)");

  g_SeqVehicle.AddSeparator("Defaults for newly recorded vehicles");

  g_SeqVehicle.AddToggle(
      "Default: Show Driver", &g_VehicleClipShowDriver, nullptr,
      "Default for NEW recordings (each vehicle can override in Per-Vehicle "
      "Settings): seat the recorded driver ped inside the car.");

  g_SeqVehicle.AddToggle(
      "Default: Collision", &g_VehicleClipGhostCollision, nullptr,
      "Default for NEW recordings (override per-vehicle): keep collision ON so "
      "the car shoves objects along its path. Off = smooth deterministic replay.");

  g_SeqVehicle.AddToggle(
      "Default: Godmode", &g_VehicleClipGhostGodmode, nullptr,
      "Default for NEW recordings (override per-vehicle): keep the car pristine "
      "(invincible + auto-repair, no damage).");

  g_SeqVehicle.AddLabel("Clips save/load automatically with the sequence.");

  int cnt = (int)g_SeqVehicle.items.size();
  if (sel >= cnt) sel = cnt - 1;
  if (sel < 0) sel = 0;
  g_SeqVehicle.selected = sel;
  g_SeqVehicle.scrollOffset = scroll;
}

// ---- Per-vehicle editor (mirror <-> live clip settings) ----
static void LoadVehMirror() {
  if (!VehicleClip_GetSettings(s_editVeh, &s_vm)) {
    s_vm = VehicleClipSettings{};
    s_vm.enabled = true; s_vm.speed = 1.0f;
  }
}
static void WriteVehMirror() {
  VehicleClipSettings probe;
  if (!VehicleClip_GetSettings(s_editVeh, &probe)) { // clip vanished under us
    s_seqPopRequested = true;
    return;
  }
  if (s_vm.speed < 0.05f) s_vm.speed = 0.05f; // guard the divisor
  VehicleClip_SetSettings(s_editVeh, &s_vm);
}
static void DeleteEditVeh() {
  if (s_editVeh >= 0) {
    VehicleClip_DeleteClip(s_editVeh);
    s_editVeh = -1;
    SetStatusText("Vehicle deleted");
  }
  s_seqPopRequested = true; // leave the editor (deferred)
}

// Rebuilt each frame it's shown: one row per recorded vehicle -> its editor.
static void RebuildSeqVehicleList() {
  int sel = g_SeqVehicleList.selected, scroll = g_SeqVehicleList.scrollOffset;
  g_SeqVehicleList.items.clear();
  int n = VehicleClip_Count();
  if (n == 0) g_SeqVehicleList.AddLabel("No vehicles recorded yet.");
  for (int i = 0; i < n; ++i) {
    char nm[40];
    VehicleClip_GetLabel(i, nm, sizeof(nm));
    VehicleClipSettings st{};
    VehicleClip_GetSettings(i, &st);
    char label[64];
    sprintf_s(label, "%s%s", nm, st.enabled ? "" : "  (off)");
    auto &it = g_SeqVehicleList.AddButton(
        label, [i] { s_editVeh = i; g_Ctrl.Push(&g_SeqVehicleEdit); },
        "Open this vehicle's settings (speed, offset, lights, engine, siren, "
        "driver, collision, godmode).");
    it.valueGetter = [i] {
      VehicleClipSettings s{};
      if (!VehicleClip_GetSettings(i, &s)) return std::string("");
      char b[24];
      if (s.offset < -0.01f || s.offset > 0.01f)
        sprintf_s(b, "%gx %+.1fs", s.speed, s.offset); // %+ shows the sign
      else sprintf_s(b, "%gx", s.speed);
      return std::string(b);
    };
  }
  int cnt = (int)g_SeqVehicleList.items.size();
  if (sel >= cnt) sel = cnt - 1;
  if (sel < 0) sel = 0;
  g_SeqVehicleList.selected = sel;
  g_SeqVehicleList.scrollOffset = scroll;
}

static void BuildSeqTree() {
  g_EffectOpts.clear();
  for (int k = 0; k < EFX_COUNT; ++k) g_EffectOpts.push_back(EffectName((EffectKind)k));

  // The sequence root rows now live in the unified root (RebuildRoot); only the
  // editors / lists / follow / render sub-pages are built here.

  // ---- Keyframe list: re-arm the browse preview on entry so the row the
  // cursor lands on previews immediately (see SeqMenu_FrameSync).
  g_SeqPoses.onPush = [] { s_poseListPrevSel = -1; };

  // ---- Pose editor (mirror-bound) ----
  g_SeqPoseEdit.onPush = [] { LoadPoseMirror(); };
  g_SeqPoseEdit.onPop = [] { Sequence_SortByTime(); };
  g_SeqPoseEdit.AddFloat("Time (s)", &s_pm.t, 0.0f, 100000.0f, 0.1f, 2, nullptr,
                         "When this keyframe happens on the timeline, in seconds.");
  g_SeqPoseEdit.AddFloat("Pos X", &s_pm.posX, -1000000.0f, 1000000.0f, 0.5f, 2, nullptr,
                         "World X position of the camera at this keyframe.");
  g_SeqPoseEdit.AddFloat("Pos Y", &s_pm.posY, -1000000.0f, 1000000.0f, 0.5f, 2, nullptr,
                         "World Y position of the camera at this keyframe.");
  g_SeqPoseEdit.AddFloat("Pos Z", &s_pm.posZ, -1000000.0f, 1000000.0f, 0.5f, 2, nullptr,
                         "World Z (height) of the camera at this keyframe.");
  g_SeqPoseEdit.AddFloat("Pitch", &s_pm.pitch, -360.0f, 360.0f, 1.0f, 2, nullptr,
                         "Up/down look angle at this keyframe (degrees).");
  g_SeqPoseEdit.AddFloat("Yaw", &s_pm.yaw, -360.0f, 360.0f, 1.0f, 2, nullptr,
                         "Left/right look angle at this keyframe (degrees).");
  g_SeqPoseEdit.AddFloat("Roll", &s_pm.roll, -360.0f, 360.0f, 1.0f, 2, nullptr,
                         "Side tilt (Dutch angle) at this keyframe (degrees).");
  g_SeqPoseEdit.AddFloat("FOV", &s_pm.fov, 5.0f, 130.0f, 1.0f, 1, nullptr,
                         "Lens zoom at this keyframe. Lower = more zoom.");
  g_SeqPoseEdit.AddList("Ease", &s_pm.easeI, {"Linear", "In-Out", "In", "Out", "Hold"},
                        nullptr,
                        "How motion eases around this keyframe. In-Out = smooth start & stop.");
  g_SeqPoseEdit.AddList("Path", &s_pm.pathI, {"Linear", "Spline"}, nullptr,
                        "Straight line to the next keyframe, or a smooth Spline curve.");
  g_SeqPoseEdit.AddButton("Entity Lock", [] { TogglePoseLock(); },
                          "Anchor this keyframe to a moving ped/vehicle so it rides along "
                          "when the entity moves. Uses the target picked on the Entity Lock "
                          "page - or, with none set, grabs whatever the camera is aimed at. "
                          "Press again to clear.")
      .valueGetter = [] { return PoseLockLabel(); };
  g_SeqPoseEdit.AddButton("Recapture from live", [] { RecapturePose(); },
                          "Overwrite this keyframe with the camera's current pose.");
  g_SeqPoseEdit.AddButton("Duplicate Keyframe", [] {
    int ni = Sequence_DuplicatePose(s_editPose);
    if (ni >= 0) {
      s_editPose = ni;
      LoadPoseMirror();
      SetStatusText("Duplicated - now editing the copy");
    }
  }, "Copy this keyframe. The copy lands midway to the next keyframe (or +2s "
     "past the end when this is the last one) and opens here for retiming.");
  g_SeqPoseEdit.AddButton("Delete Keyframe", [] { DeleteEditPose(); },
                          "Remove this keyframe from the sequence.");

  // ---- Move All Keyframes (bulk translate the whole shot) ----
  // Reset the running offset each time the page opens so the adjusters start at 0
  // (the previous translation is already baked into the keyframes).
  g_SeqMoveAll.onPush = [] {
    s_moveOffX = s_moveOffY = s_moveOffZ = 0.0f;
    s_moveLastX = s_moveLastY = s_moveLastZ = 0.0f;
  };
  g_SeqMoveAll.AddButton("Move to Camera", [] {
    CameraSequence *s = Sequence_Active();
    if (!s || s->poses.empty()) { SetStatusText("No keyframes to move"); return; }
    float cx, cy, cz, pi, ya, ro;
    GetCameraState(cx, cy, cz, pi, ya, ro);
    const PoseKeyframe &k0 = s->poses[0];
    Sequence_TranslateAll(cx - k0.posX, cy - k0.posY, cz - k0.posZ);
    s_moveOffX = s_moveOffY = s_moveOffZ = 0.0f;
    s_moveLastX = s_moveLastY = s_moveLastZ = 0.0f;
    SetStatusText("Sequence moved — keyframe 0 placed at camera");
  }, "Relocate the whole sequence so its FIRST keyframe sits at the current camera "
     "position, keeping the shot's shape. Ideal for re-using a loop at a new location.");
  g_SeqMoveAll.AddFloat("Move X / East (m)", &s_moveOffX, -1000000.0f, 1000000.0f, 0.5f, 2,
                        [](float v) { Sequence_TranslateAll(v - s_moveLastX, 0.0f, 0.0f); s_moveLastX = v; },
                        "Nudge every keyframe along world X: + east / - west. Resets on re-open.");
  g_SeqMoveAll.AddFloat("Move Y / North (m)", &s_moveOffY, -1000000.0f, 1000000.0f, 0.5f, 2,
                        [](float v) { Sequence_TranslateAll(0.0f, v - s_moveLastY, 0.0f); s_moveLastY = v; },
                        "Nudge every keyframe along world Y: + north / - south.");
  g_SeqMoveAll.AddFloat("Move Z / Up (m)", &s_moveOffZ, -1000000.0f, 1000000.0f, 0.5f, 2,
                        [](float v) { Sequence_TranslateAll(0.0f, 0.0f, v - s_moveLastZ); s_moveLastZ = v; },
                        "Nudge every keyframe along world Z: + up / - down.");

  // ---- Event editor (mirror-bound) ----
  g_SeqEventEdit.onPush = [] { LoadEventMirror(); };
  g_SeqEventEdit.onPop = [] { Sequence_SortByTime(); };
  g_SeqEventEdit.AddFloat("Time (s)", &s_em.t, 0.0f, 100000.0f, 0.1f, 2, nullptr,
                          "When this effect change fires on the timeline.");
  g_SeqEventEdit.AddList("Effect", &s_em.kindI, g_EffectOpts, nullptr,
                         "Which property this event changes (shake, world speed, etc.).");
  s_EventValueIdx = (int)g_SeqEventEdit.items.size();
  g_SeqEventEdit.AddFloat("Value", &s_em.value, -100000.0f,
                          100000.0f, 0.05f, 3, nullptr,
                          "The value applied for the chosen effect.");
  g_SeqEventEdit.AddToggle("Ramp (lerp from prev)", &s_em.ramp, nullptr,
                           "Snap to the value instantly, or smoothly ramp from the previous event.");
  g_SeqEventEdit.AddButton("Delete Event", [] { DeleteEditEvent(); },
                           "Remove this effect event.");

  // ---- Per-vehicle editor (mirror-bound) ----
  g_SeqVehicleEdit.onPush = [] { LoadVehMirror(); };
  g_SeqVehicleEdit.AddToggle("Enabled", &s_vm.enabled, nullptr,
      "Replay this vehicle. Off = muted (kept but not shown/despawned).");
  g_SeqVehicleEdit.AddFloat("Playback Speed", &s_vm.speed, 0.1f, 4.0f, 0.05f, 2, nullptr,
      "Speed multiplier for this car's motion (1.0 = as recorded, 2 = twice as fast). "
      "Reshapes how long it takes on the timeline.");
  g_SeqVehicleEdit.AddFloat("Time Offset (s)", &s_vm.offset, -100000.0f, 100000.0f, 0.1f, 2, nullptr,
      "Shift this car on the timeline. Positive = starts later (holds at its "
      "first frame until then). Negative = starts already in motion (skips the "
      "first seconds of its recording, so it's mid-drive at t=0).");
  g_SeqVehicleEdit.AddList("Lights", &s_vm.lights, {"Auto", "On", "Off", "On + Full Beam"},
      nullptr, "Force this car's head/tail lights: Auto (game default), On, Off, "
               "or On with high beams.");
  g_SeqVehicleEdit.AddToggle("Engine Running", &s_vm.engineOn, nullptr,
      "Engine on (idle anim, exhaust, working lights) or off (dead car).");
  g_SeqVehicleEdit.AddToggle("Siren", &s_vm.siren, nullptr,
      "Emergency siren + flashing lights (police / ambulance / fire trucks).");
  g_SeqVehicleEdit.AddToggle("Show Driver", &s_vm.showDriver, nullptr,
      "Seat the recorded driver ped inside this car.");
  g_SeqVehicleEdit.AddToggle("Collision", &s_vm.collision, nullptr,
      "Keep collision on so this car shoves objects it passes through (impacts).");
  g_SeqVehicleEdit.AddToggle("Godmode", &s_vm.godmode, nullptr,
      "Keep this car pristine: invincible + auto-repair (no dents, full health).");
  g_SeqVehicleEdit.AddButton("Delete This Vehicle", [] { DeleteEditVeh(); },
      "Remove this recorded vehicle from the sequence.");

  // ---- Follow & Entity Lock: rebuilt on entry + every frame it's shown
  // (RebuildSeqFollow) so the target / locked-keyframe status rows are live.
  g_SeqFollow.onPush = [] { RebuildSeqFollow(); };

  // ---- Sequences + Playback + Render are (re)built on entry ----
  g_SeqList.onPush = [] { RebuildSeqList(); };
  g_SeqPlayback.onPush = [] { BuildSeqPlayback(); };
  g_SeqRender.onPush = [] { SyncRenderMirrors(); RebuildSeqRender(); };
  g_SeqVehicle.onPush = [] { RebuildSeqVehicle(); };
}

// ============================================================
//  Unified root — Camera Mode switcher + per-mode content
// ============================================================

// The live mode, derived from the camera state (not a stored flag).
static int CurrentMode() {
  if (Sequence_IsInMode()) return 2;
  if (g_FreeCamActive) return 1;
  return 0;
}

static const char *ModeName(int m) {
  return m == 2 ? "Camera Sequence" : m == 1 ? "Free Camera" : "Off";
}

// Switch to mode `m` (0 Off, 1 Free Camera, 2 Sequence), tearing down whatever's
// active first. This is the ONE place mode changes — no scattered exit buttons.
static void ApplyMode(int m) {
  int cur = CurrentMode();
  if (m == cur) return;
  // Leave the current mode.
  if (cur == 2) { Sequence_ExitMode(); g_CameraMode = -1; }
  else if (cur == 1) { DestroyFreeCamera(); }
  // Enter the new one.
  if (m == 1) {
    g_CameraMode = 0;
    InitFreeCamera();
    SetStatusText("Free Camera on");
  } else if (m == 2) {
    g_CameraMode = 1;
    Sequence_EnterMode();
    SetStatusText("Camera Sequence on");
  } else {
    g_CameraMode = -1;
    SetStatusText("Camera off");
  }
}

// Rebuild the root's rows to match the active mode (called each frame it's
// shown). Row 0 is always the Camera Mode switcher.
static void RebuildRoot() {
  int sel = g_Root.selected, scroll = g_Root.scrollOffset;
  g_Root.items.clear();

  int mode = CurrentMode();
  g_Root.subtitle = ModeName(mode);

  g_Root.AddList("Camera Mode", &s_mode, {"Off", "Free Camera", "Camera Sequence"},
                 [](int m) { ApplyMode(m); },
                 "Switch the active camera mode.");

  if (mode == 1) {
    // ---- Free Camera ----
    g_Root.AddSeparator("");
    g_Root.AddSubmenu("Movement", &g_Movement,
                      "Speed, sensitivity, collision, drone, follow.");
    g_Root.AddSubmenu("Lens", &g_Lens, "Field of view and roll.");
    g_Root.AddSubmenu("Depth of Field", &g_DoF, "Focus and bokeh range.");
    g_Root.AddSubmenu("Camera Effects", &g_Effects, "Procedural handheld shake.");
    g_Root.AddSubmenu("Misc", &g_Misc, "Player linkage and quick actions.");
    g_Root.AddSubmenu("World & Scene", &g_World,
                      "Time, weather, auto-drive, HUD, player.");
    g_Root.AddSubmenu("Appearance", &g_Appearance,
                      "Menu position, scale, accent colour, and sequence marker/path look.");
    g_Root.AddSeparator("");
    g_Root.AddButton("Save Settings", [] {
      SaveSettings();
      SetStatusText("Settings saved to SimpleCamera.ini");
    }, "Write the current setup to SimpleCamera.ini.");
  } else if (mode == 2) {
    // ---- Camera Sequence ----
    g_Root.AddSeparator("");
    g_Root.AddButton("Capture Pose", [] {
      int i = Sequence_CapturePoseAtCurrentTime();
      if (i >= 0) SetStatusText("Pose captured");
    }, "Add a keyframe at the camera's current pose (hotkey F6).").valueGetter = [] {
      CameraSequence *s = Sequence_Active();
      char b[24]; sprintf_s(b, "F6 - %d kf", s ? (int)s->poses.size() : 0);
      return std::string(b);
    };
    g_Root.AddButton("Play / Pause", [] { Sequence_TogglePlay(); },
                     "Start or pause playback of the sequence (hotkey F7).")
        .valueGetter = [] { return std::string(Sequence_IsPlaying() ? "Playing" : "Paused"); };
    g_Root.AddButton("Stop", [] { Sequence_Stop(); SetStatusText("Stopped"); },
                     "Stop playback and rewind to the start (hotkey F8).");
    float dur = Sequence_TotalDuration();
    g_Root.AddFloat("Scrub Time (s)", &s_scrub, 0.0f, dur > 0.1f ? dur : 0.1f, 0.1f, 2,
                    [](float v) { Sequence_SetCurrentTime(v); },
                    "Move the playhead through the sequence by hand to preview a moment.");
    {
      // Keyframe stepper: scroll to jump the playhead (and camera) straight to
      // the previous / next keyframe — the fastest way to walk through a shot.
      CameraSequence *sq = Sequence_Active();
      int kfN = sq ? (int)sq->poses.size() : 0;
      if (kfN > 0) {
        // Mirror = 1-based index of the keyframe at/just before the playhead
        // (recomputed every rebuild, so it always tracks scrub/playback).
        float ph = Sequence_CurrentTime();
        int at = 0;
        for (int i = 0; i < kfN; ++i)
          if (sq->poses[i].t <= ph + 0.001f) at = i;
        s_kfNav = at + 1;
        g_Root.AddInt("Go to Keyframe", &s_kfNav, 1, kfN, 1, [](int v) {
          CameraSequence *s2 = Sequence_Active();
          if (!s2 || s2->poses.empty()) return;
          int i = v - 1;
          if (i < 0) i = 0;
          if (i >= (int)s2->poses.size()) i = (int)s2->poses.size() - 1;
          Sequence_SetCurrentTime(s2->poses[i].t);
        }, "Step the playhead keyframe by keyframe. The camera snaps to each "
           "one, so this doubles as instant teleport along the shot.")
            .valueGetter = [] {
              CameraSequence *s2 = Sequence_Active();
              if (!s2 || s2->poses.empty()) return std::string();
              int i = s_kfNav - 1;
              if (i < 0) i = 0;
              if (i >= (int)s2->poses.size()) i = (int)s2->poses.size() - 1;
              char b[32];
              sprintf_s(b, "%d/%d @ %.2fs", i + 1, (int)s2->poses.size(),
                        s2->poses[i].t);
              return std::string(b);
            };
      }
    }
    g_Root.AddSeparator("");
    g_Root.AddSubmenu("Pose Keyframes", &g_SeqPoses,
                      "The camera poses that make up the shot. Add, edit and delete keyframes.")
        .valueGetter = [] {
          CameraSequence *s = Sequence_Active();
          char b[16]; sprintf_s(b, "%d", s ? (int)s->poses.size() : 0);
          return std::string(b);
        };
    g_Root.AddSubmenu("Effect Events", &g_SeqEvents,
                      "Timed effect changes along the timeline (shake events).")
        .valueGetter = [] {
          CameraSequence *s = Sequence_Active();
          char b[16]; sprintf_s(b, "%d", s ? (int)s->events.size() : 0);
          return std::string(b);
        };
    g_Root.AddSubmenu("Playback Settings", &g_SeqPlayback,
                      "Loop, playback speed, loop-closing and in-world markers.");
    g_Root.AddSubmenu("Entity Lock (Follow)", &g_SeqFollow,
                      "Anchor the shot to a moving ped/vehicle: author the camera path "
                      "around it parked, and the whole path rides along when it moves. "
                      "Two steps inside: pick a target, lock the keyframes.")
        .valueGetter = [] {
          int locked = Sequence_LockedPoseCount();
          bool tgt = (g_FollowMode == 1) ||
                     (g_FollowMode == 2 && g_FollowTargetEntity != 0 &&
                      ENTITY::DOES_ENTITY_EXIST(g_FollowTargetEntity));
          if (locked > 0) {
            char b[24];
            sprintf_s(b, "%d kf locked", locked);
            return std::string(b);
          }
          if (tgt) return std::string("target set");
          return std::string("off");
        };
    g_Root.AddSubmenu("Vehicle Clip", &g_SeqVehicle,
                      "Record a vehicle's drive and replay it in sync with the "
                      "timeline so the camera lines up frame-for-frame.")
        .valueGetter = [] {
          if (VehicleClip_IsRecording()) return std::string("REC");
          int n = VehicleClip_Count();
          if (n > 0) {
            char b[32]; sprintf_s(b, "%dx %.1fs %s", n, VehicleClip_Duration(),
                                  VehicleClip_Enabled() ? "On" : "Off");
            return std::string(b);
          }
          return std::string("empty");
        };
    g_Root.AddSubmenu("Sequences", &g_SeqList,
                      "Create, pick, delete and save your sequences.")
        .valueGetter = [] {
          CameraSequence *s = Sequence_Active();
          char b[48];
          // snprintf, not sprintf_s: a long (user/JSON-supplied) sequence name
          // would trip sprintf_s's bounds check and abort; snprintf truncates.
          snprintf(b, sizeof(b), "%s [%d/%d]", s ? s->name.c_str() : "(none)",
                   Sequence_ActiveIndex() + 1, Sequence_Count());
          return std::string(b);
        };
    g_Root.AddSubmenu("Render to Images", &g_SeqRender,
                      "Export the sequence as a numbered image sequence (needs ReShade addon).");
    g_Root.AddSubmenu("World & Scene", &g_World,
                      "Time, weather, HUD and player visibility.");
    g_Root.AddSubmenu("Appearance", &g_Appearance,
                      "Menu position, scale, accent colour, and sequence marker/path look.");
  } else {
    // ---- Off ---- (no camera engaged, but world/menu config is still useful)
    g_Root.AddSeparator("");
    g_Root.AddSubmenu("World & Scene", &g_World,
                      "Time, weather, HUD and player visibility.");
    g_Root.AddSubmenu("Appearance", &g_Appearance,
                      "Menu position, scale, accent colour, and sequence marker/path look.");
  }

  int cnt = (int)g_Root.items.size();
  if (sel >= cnt) sel = cnt - 1;
  if (sel < 0) sel = 0;
  g_Root.selected = sel;
  g_Root.scrollOffset = scroll;
}

// Per-frame sequence upkeep: keep live lists/mirrors in step with the data.
// Called from SCMenu_Update BEFORE the controller's Update so input acts on the
// freshly-rebuilt items.
static void SeqMenu_FrameSync() {
  gtam::Menu *cur = g_Ctrl.Current();

  // Mode mirror + always-visible mode indicator in the footer.
  s_mode = CurrentMode();
  g_Ctrl.SetFooterText(std::string("Mode: ") + ModeName(s_mode));

  // Root scrub mirrors the live playhead.
  s_scrub = Sequence_CurrentTime();

  if (cur == &g_Root) { RebuildRoot(); }
  else if (cur == &g_SeqPoses) {
    s_totalDur = Sequence_TotalDuration();
    RebuildPoseList();
    // Browse preview: landing the selection on a keyframe row scrubs the
    // playhead there, so scrolling the list flips through the shot visually.
    int kfIdx = g_SeqPoses.selected - s_poseListHeaderRows;
    if (!Sequence_IsPlaying() && kfIdx >= 0 &&
        g_SeqPoses.selected != s_poseListPrevSel) {
      CameraSequence *sq = Sequence_Active();
      if (sq && kfIdx < (int)sq->poses.size())
        Sequence_SetCurrentTime(sq->poses[kfIdx].t);
    }
    s_poseListPrevSel = g_SeqPoses.selected;
  }
  else if (cur == &g_SeqEvents) { RebuildEventList(); }
  else if (cur == &g_SeqList) { RebuildSeqList(); }
  else if (cur == &g_SeqRender) { RebuildSeqRender(); }
  else if (cur == &g_SeqVehicle) { RebuildSeqVehicle(); }
  else if (cur == &g_SeqVehicleList) { RebuildSeqVehicleList(); }
  else if (cur == &g_SeqVehicleEdit) {
    WriteVehMirror();
    // Context header: which recorded vehicle is open ("SULTAN", custom label, ...).
    char nm[40];
    VehicleClip_GetLabel(s_editVeh, nm, sizeof(nm));
    if (nm[0]) g_SeqVehicleEdit.subtitle = nm;
  }
  else if (cur == &g_SeqPoseEdit) {
    WritePoseMirror();
    // Context header: which keyframe is open, out of how many.
    CameraSequence *sq = Sequence_Active();
    if (sq && s_editPose >= 0 && s_editPose < (int)sq->poses.size()) {
      char b[48];
      sprintf_s(b, "KEYFRAME %d / %d", s_editPose + 1, (int)sq->poses.size());
      g_SeqPoseEdit.subtitle = b;
    }
    // Live preview: the moment any value is nudged (time, pos, rot, fov, ease),
    // re-apply the pose at the keyframe's time so the edit is visible through
    // the camera instantly. Value-change–gated, so free-flying to compare or
    // recapture stays untouched. (Also fires once on open — the stale s_pmPrev
    // differs — which doubles as preview-on-open.)
    if (memcmp(&s_pm, &s_pmPrev, sizeof(s_pm)) != 0) {
      if (!Sequence_IsPlaying()) Sequence_SetCurrentTime(s_pm.t);
      memcpy(&s_pmPrev, &s_pm, sizeof(s_pm));
    }
  }
  else if (cur == &g_SeqEventEdit) {
    WriteEventMirror();
    // Context header, same as the pose editor.
    CameraSequence *sq = Sequence_Active();
    if (sq && s_editEvent >= 0 && s_editEvent < (int)sq->events.size()) {
      char b[32];
      sprintf_s(b, "EVENT %d / %d", s_editEvent + 1, (int)sq->events.size());
      g_SeqEventEdit.subtitle = b;
    }
    if (s_EventValueIdx >= 0 && s_EventValueIdx < (int)g_SeqEventEdit.items.size())
      g_SeqEventEdit.items[s_EventValueIdx].fStep = SeqEventValueStep(s_em.kindI);
  } else if (cur == &g_SeqFollow) {
    RebuildSeqFollow();
    DrawSeqHoverMarker();
  }
}

// ============================================================
//  Public API
// ============================================================

void SCMenu_Init() {
  if (g_Built) return;
  BuildTree();
  BuildSeqTree();
  g_Built = true;
}

void SCMenu_Toggle() {
  if (!g_Built) SCMenu_Init();
  g_Ctrl.Toggle(&g_Root); // single unified menu (mode switcher at the top)
}

bool SCMenu_IsOpen() { return g_Ctrl.IsOpen(); }

void SCMenu_Update() {
  if (!g_Built) return;

  // When the Time & Weather page first comes into view, sync the clock fields
  // to the live game time (matches the classic menu's open-time refresh).
  static gtam::Menu *s_prev = nullptr;
  gtam::Menu *cur = g_Ctrl.Current();
  if (cur == &g_Time && s_prev != &g_Time && !g_TimePaused) {
    g_TimeHour = TIME::GET_CLOCK_HOURS();
    g_TimeMinute = TIME::GET_CLOCK_MINUTES();
  }
  s_prev = cur;

  // Keep sequence lists / editors in step with the live data before input.
  SeqMenu_FrameSync();

  bool visible = g_Ctrl.Update();

  // Deferred Back() requested from an editor (e.g. Delete) — done after Update
  // so we don't mutate the stack mid-frame.
  if (s_seqPopRequested) {
    s_seqPopRequested = false;
    g_Ctrl.Back();
  }
  // Deferred full close (e.g. starting a vehicle-clip take so the player can
  // drive) — also after Update to avoid mutating the stack mid-frame.
  if (s_seqCloseRequested) {
    s_seqCloseRequested = false;
    g_Ctrl.Close();
    visible = false;
  }

  // Tell the free camera a menu is up so it suppresses input it shares with
  // menu navigation (same contract the classic menu uses).
  g_MenuOpen = visible;
}
