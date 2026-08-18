/*
        GTA V Free Camera / Photo Mode Plugin
        Shared menu infrastructure (the UI itself lives in scmenu.cpp).

        Contains only the non-UI plumbing that survives the move to the gtam
        framework: INI settings (load/save/reset), the on-screen status text,
        the menu hotkey + controller combos, MenuBeep, a couple of small input
        helpers (GetMenuButtons / DisableMenuPhoneControls) used by the render
        worker, and the offline "Render to Images" pipeline.
*/

#include "menu.h"
#include "camera.h"
#include "fx_capture.h"
#include "keyboard.h"
#include "scmenu.h" // menu-appearance globals (position/scale/accent)
#include "sequence.h"
#include "vehicleclip.h" // g_VehicleClipSampleHz

#include <cstdio>
#include <string>

#pragma warning(disable : 4244 4305)

// ============================================================
//  Status Text
// ============================================================

static std::string s_StatusText;
static DWORD s_StatusTextMaxTicks = 0;

// Set while any configuration menu is open (see camera.h). UpdateFreeCamera
// reads this to suppress menu-shared controller input (D-Pad zoom).
bool g_MenuOpen = false;

void SetStatusText(std::string str, DWORD time) {
  s_StatusText = str;
  s_StatusTextMaxTicks = GetTickCount() + time;
}

void UpdateStatusText() {
  if (GetTickCount() < s_StatusTextMaxTicks) {
    UI::SET_TEXT_FONT(0);
    UI::SET_TEXT_SCALE(0.55, 0.55);
    UI::SET_TEXT_COLOUR(255, 255, 255, 255);
    UI::SET_TEXT_WRAP(0.0, 1.0);
    UI::SET_TEXT_CENTRE(1);
    UI::SET_TEXT_DROPSHADOW(0, 0, 0, 0, 0);
    UI::SET_TEXT_EDGE(1, 0, 0, 0, 205);
    UI::_SET_TEXT_ENTRY((char *)"STRING");
    UI::_ADD_TEXT_COMPONENT_STRING((LPSTR)s_StatusText.c_str());
    UI::_DRAW_TEXT(0.5, 0.5);
  }
}

// ============================================================
//  Settings
// ============================================================

static int g_MenuKey = VK_F5; // default

// Resolve the full path to SimpleCamera.ini sitting next to the ASI. Single
// source of truth for both load and save so they can never disagree.
static void GetIniPath(char *path, size_t cap) {
  HMODULE hMod = NULL;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     (LPCSTR)GetIniPath, &hMod);
  GetModuleFileNameA(hMod, path, (DWORD)cap);
  char *last = strrchr(path, '\\');
  if (last)
    strcpy_s(last + 1, cap - (last + 1 - path), "SimpleCamera.ini");
}

// GetPrivateProfileInt stores ints only; floats need string round-tripping.
static float IniReadFloat(const char *section, const char *key, float def,
                          const char *path) {
  char buf[64], defbuf[64];
  sprintf_s(defbuf, "%g", def);
  GetPrivateProfileStringA(section, key, defbuf, buf, sizeof(buf), path);
  return (float)atof(buf);
}

static bool IniReadBool(const char *section, const char *key, bool def,
                        const char *path) {
  return GetPrivateProfileIntA(section, key, def ? 1 : 0, path) != 0;
}

void LoadSettings() {
  char path[MAX_PATH];
  GetIniPath(path, MAX_PATH);

  // ---- Controls ----
  g_MenuKey = GetPrivateProfileIntA("Controls", "MenuKey", VK_F5, path);
  // Camera Sequence hotkeys вЂ” inert outside Sequence mode.
  g_SeqHotkeyAdd = GetPrivateProfileIntA("Controls", "SequenceAddKey", VK_F6, path);
  g_SeqHotkeyPlay = GetPrivateProfileIntA("Controls", "SequencePlayKey", VK_F7, path);
  g_SeqHotkeyStop = GetPrivateProfileIntA("Controls", "SequenceStopKey", VK_F8, path);
  g_SeqHotkeyNext = GetPrivateProfileIntA("Controls", "SequenceNextKey", VK_F9, path);

  // ---- Camera ---- (defaults match the global initializers in camera.cpp)
  g_CamSpeed = IniReadFloat("Camera", "CamSpeed", g_CamSpeed, path);
  g_CamSensitivity = IniReadFloat("Camera", "CamSensitivity", g_CamSensitivity, path);
  g_CamFOV = IniReadFloat("Camera", "CamFOV", g_CamFOV, path);
  g_ZoomSpeed = IniReadFloat("Camera", "ZoomSpeed", g_ZoomSpeed, path);
  g_RollSpeed = IniReadFloat("Camera", "RollSpeed", g_RollSpeed, path);
  g_CamCollision = IniReadBool("Camera", "Collision", g_CamCollision, path);
  g_LockHeight = IniReadBool("Camera", "LockHeight", g_LockHeight, path);
  g_RotationEngine = IniReadBool("Camera", "AcrobaticRotation", g_RotationEngine, path);
  g_WalkMode = IniReadBool("Camera", "WalkMode", g_WalkMode, path);
  g_WalkHeight = IniReadFloat("Camera", "WalkHeight", g_WalkHeight, path);
  g_FollowRigidMode = IniReadBool("Camera", "FollowRigidMode", g_FollowRigidMode, path);

  // ---- Auto Drive ---- (mode/speed/style persist; the enabled flag is
  // transient вЂ” it's never auto-engaged on load)
  g_AutoDriveMode = GetPrivateProfileIntA("AutoDrive", "Mode", g_AutoDriveMode, path);
  if (g_AutoDriveMode < 0 || g_AutoDriveMode > 1) g_AutoDriveMode = 0;
  g_AutoDriveSpeed = IniReadFloat("AutoDrive", "Speed", g_AutoDriveSpeed, path);
  g_AutoDriveStyleIndex = GetPrivateProfileIntA("AutoDrive", "StyleIndex", g_AutoDriveStyleIndex, path);
  if (g_AutoDriveStyleIndex < 0 || g_AutoDriveStyleIndex >= g_AutoDriveStyleCount)
    g_AutoDriveStyleIndex = 0;

  // ---- Drone ----
  g_DroneMode = IniReadBool("Drone", "Enabled", g_DroneMode, path);
  g_DroneDrag = IniReadFloat("Drone", "Drag", g_DroneDrag, path);
  g_DroneAcceleration = IniReadFloat("Drone", "Acceleration", g_DroneAcceleration, path);
  g_DroneGravity = IniReadFloat("Drone", "Gravity", g_DroneGravity, path);
  g_DroneBanking = IniReadFloat("Drone", "Banking", g_DroneBanking, path);
  g_DroneRotSmoothing = IniReadFloat("Drone", "RotSmoothing", g_DroneRotSmoothing, path);
  g_DroneFovSmoothing = IniReadFloat("Drone", "FovSmoothing", g_DroneFovSmoothing, path);

  // ---- Shake ----
  g_ShakeEnabled = IniReadBool("Shake", "Enabled", g_ShakeEnabled, path);
  g_ShakePreset = GetPrivateProfileIntA("Shake", "Preset", g_ShakePreset, path);
  if (g_ShakePreset < 0 || g_ShakePreset > 5) g_ShakePreset = 0;
  g_ShakeAmp = IniReadFloat("Shake", "Amplitude", g_ShakeAmp, path);
  g_ShakeFreq = IniReadFloat("Shake", "Frequency", g_ShakeFreq, path);
  g_ShakeSpeedAmpCoupling = IniReadFloat("Shake", "SpeedAmpCoupling", g_ShakeSpeedAmpCoupling, path);
  g_ShakeSpeedFreqCoupling = IniReadFloat("Shake", "SpeedFreqCoupling", g_ShakeSpeedFreqCoupling, path);
  g_ShakeRotWeight = IniReadFloat("Shake", "RotWeight", g_ShakeRotWeight, path);
  g_ShakePosWeight = IniReadFloat("Shake", "PosWeight", g_ShakePosWeight, path);
  g_ShakeStopWhenStill = IniReadBool("Shake", "StopWhenStill", g_ShakeStopWhenStill, path);

  // ---- Depth of Field ----
  g_DoFEnabled = IniReadBool("DoF", "Enabled", g_DoFEnabled, path);
  g_DoFAutofocus = IniReadBool("DoF", "Autofocus", g_DoFAutofocus, path);
  g_DoFFocusDist = IniReadFloat("DoF", "FocusDist", g_DoFFocusDist, path);
  g_DoFMaxNearInFocus = IniReadFloat("DoF", "NearRange", g_DoFMaxNearInFocus, path);
  g_DoFMaxFarInFocus = IniReadFloat("DoF", "FarRange", g_DoFMaxFarInFocus, path);

  // ---- Misc ----
  g_HideHUD = IniReadBool("Misc", "HideHUD", g_HideHUD, path);
  g_HidePlayer = IniReadBool("Misc", "HidePlayer", g_HidePlayer, path);
  g_RememberCamPosition = IniReadBool("Misc", "RememberCamPosition", g_RememberCamPosition, path);
  g_StreamAroundCamera = IniReadBool("Misc", "StreamAroundCamera", g_StreamAroundCamera, path);
  g_ShowInfoOverlay = IniReadBool("Misc", "ShowInfoOverlay", g_ShowInfoOverlay, path);
  g_DisableVehicleShake = IniReadBool("Misc", "DisableVehicleShake", g_DisableVehicleShake, path);
  g_ClearVehicles = IniReadBool("Misc", "ClearVehicles", g_ClearVehicles, path);
  g_ClearPeds = IniReadBool("Misc", "ClearPeds", g_ClearPeds, path);
  g_ShowLockedEntityMarker = IniReadBool("Misc", "ShowLockedEntityMarker", g_ShowLockedEntityMarker, path);

  // ---- Appearance: menu look (UI) ----
  g_MenuPosX = IniReadFloat("UI", "MenuPosX", g_MenuPosX, path);
  g_MenuPosY = IniReadFloat("UI", "MenuPosY", g_MenuPosY, path);
  g_MenuScale = IniReadFloat("UI", "MenuScale", g_MenuScale, path);
  g_MenuAccentR = GetPrivateProfileIntA("UI", "AccentR", g_MenuAccentR, path);
  g_MenuAccentG = GetPrivateProfileIntA("UI", "AccentG", g_MenuAccentG, path);
  g_MenuAccentB = GetPrivateProfileIntA("UI", "AccentB", g_MenuAccentB, path);
  g_MenuBgR = GetPrivateProfileIntA("UI", "BgR", g_MenuBgR, path);
  g_MenuBgG = GetPrivateProfileIntA("UI", "BgG", g_MenuBgG, path);
  g_MenuBgB = GetPrivateProfileIntA("UI", "BgB", g_MenuBgB, path);
  g_MenuTextR = GetPrivateProfileIntA("UI", "TextR", g_MenuTextR, path);
  g_MenuTextG = GetPrivateProfileIntA("UI", "TextG", g_MenuTextG, path);
  g_MenuTextB = GetPrivateProfileIntA("UI", "TextB", g_MenuTextB, path);
  g_MenuSelR = GetPrivateProfileIntA("UI", "SelR", g_MenuSelR, path);
  g_MenuSelG = GetPrivateProfileIntA("UI", "SelG", g_MenuSelG, path);
  g_MenuSelB = GetPrivateProfileIntA("UI", "SelB", g_MenuSelB, path);
  g_MenuSelTextR = GetPrivateProfileIntA("UI", "SelTextR", g_MenuSelTextR, path);
  g_MenuSelTextG = GetPrivateProfileIntA("UI", "SelTextG", g_MenuSelTextG, path);
  g_MenuSelTextB = GetPrivateProfileIntA("UI", "SelTextB", g_MenuSelTextB, path);

  // ---- Appearance: sequence keyframe markers + path ----
  g_SeqMarkerSize = IniReadFloat("Sequence", "MarkerSize", g_SeqMarkerSize, path);
  g_SeqMarkerR = GetPrivateProfileIntA("Sequence", "MarkerR", g_SeqMarkerR, path);
  g_SeqMarkerG = GetPrivateProfileIntA("Sequence", "MarkerG", g_SeqMarkerG, path);
  g_SeqMarkerB = GetPrivateProfileIntA("Sequence", "MarkerB", g_SeqMarkerB, path);
  g_SeqPathR = GetPrivateProfileIntA("Sequence", "PathR", g_SeqPathR, path);
  g_SeqPathG = GetPrivateProfileIntA("Sequence", "PathG", g_SeqPathG, path);
  g_SeqPathB = GetPrivateProfileIntA("Sequence", "PathB", g_SeqPathB, path);
  g_SeqTimeLabelMode = GetPrivateProfileIntA("Sequence", "TimeLabelMode", g_SeqTimeLabelMode, path);
  if (g_SeqTimeLabelMode < 0 || g_SeqTimeLabelMode > 4) g_SeqTimeLabelMode = 2;
  g_VehicleClipSampleHz =
      GetPrivateProfileIntA("Sequence", "ClipSampleHz", g_VehicleClipSampleHz, path);
  g_VehicleClipSteerGain =
      IniReadFloat("Sequence", "ClipSteerGain", g_VehicleClipSteerGain, path);
  g_VehicleClipShowDriver =
      GetPrivateProfileIntA("Sequence", "ClipShowDriver",
                            g_VehicleClipShowDriver ? 1 : 0, path) != 0;
  g_VehicleClipGhostCollision =
      GetPrivateProfileIntA("Sequence", "ClipGhostCollision",
                            g_VehicleClipGhostCollision ? 1 : 0, path) != 0;
  g_VehicleClipGhostGodmode =
      GetPrivateProfileIntA("Sequence", "ClipGhostGodmode",
                            g_VehicleClipGhostGodmode ? 1 : 0, path) != 0;
}

// Persist all tunable settings to the INI. Called from the Misc menu's
// "Save Settings to INI" row. Transient state (active mode, follow target,
// live time/weather, freeze) is intentionally NOT saved.
void SaveSettings() {
  char path[MAX_PATH];
  GetIniPath(path, MAX_PATH);
  char buf[64];
  auto wf = [&](const char *sec, const char *key, float v) {
    sprintf_s(buf, "%g", v);
    WritePrivateProfileStringA(sec, key, buf, path);
  };
  auto wb = [&](const char *sec, const char *key, bool v) {
    WritePrivateProfileStringA(sec, key, v ? "1" : "0", path);
  };
  auto wi = [&](const char *sec, const char *key, int v) {
    sprintf_s(buf, "%d", v);
    WritePrivateProfileStringA(sec, key, buf, path);
  };

  wf("Camera", "CamSpeed", g_CamSpeed);
  wf("Camera", "CamSensitivity", g_CamSensitivity);
  wf("Camera", "CamFOV", g_CamFOV);
  wf("Camera", "ZoomSpeed", g_ZoomSpeed);
  wf("Camera", "RollSpeed", g_RollSpeed);
  wb("Camera", "Collision", g_CamCollision);
  wb("Camera", "LockHeight", g_LockHeight);
  wb("Camera", "AcrobaticRotation", g_RotationEngine);
  wb("Camera", "WalkMode", g_WalkMode);
  wf("Camera", "WalkHeight", g_WalkHeight);
  wb("Camera", "FollowRigidMode", g_FollowRigidMode);

  wi("AutoDrive", "Mode", g_AutoDriveMode);
  wf("AutoDrive", "Speed", g_AutoDriveSpeed);
  wi("AutoDrive", "StyleIndex", g_AutoDriveStyleIndex);

  wb("Drone", "Enabled", g_DroneMode);
  wf("Drone", "Drag", g_DroneDrag);
  wf("Drone", "Acceleration", g_DroneAcceleration);
  wf("Drone", "Gravity", g_DroneGravity);
  wf("Drone", "Banking", g_DroneBanking);
  wf("Drone", "RotSmoothing", g_DroneRotSmoothing);
  wf("Drone", "FovSmoothing", g_DroneFovSmoothing);

  wb("Shake", "Enabled", g_ShakeEnabled);
  wi("Shake", "Preset", g_ShakePreset);
  wf("Shake", "Amplitude", g_ShakeAmp);
  wf("Shake", "Frequency", g_ShakeFreq);
  wf("Shake", "SpeedAmpCoupling", g_ShakeSpeedAmpCoupling);
  wf("Shake", "SpeedFreqCoupling", g_ShakeSpeedFreqCoupling);
  wf("Shake", "RotWeight", g_ShakeRotWeight);
  wf("Shake", "PosWeight", g_ShakePosWeight);
  wb("Shake", "StopWhenStill", g_ShakeStopWhenStill);

  wb("DoF", "Enabled", g_DoFEnabled);
  wb("DoF", "Autofocus", g_DoFAutofocus);
  wf("DoF", "FocusDist", g_DoFFocusDist);
  wf("DoF", "NearRange", g_DoFMaxNearInFocus);
  wf("DoF", "FarRange", g_DoFMaxFarInFocus);

  wb("Misc", "HideHUD", g_HideHUD);
  wb("Misc", "HidePlayer", g_HidePlayer);
  wb("Misc", "RememberCamPosition", g_RememberCamPosition);
  wb("Misc", "StreamAroundCamera", g_StreamAroundCamera);
  wb("Misc", "ShowInfoOverlay", g_ShowInfoOverlay);
  wb("Misc", "DisableVehicleShake", g_DisableVehicleShake);
  wb("Misc", "ClearVehicles", g_ClearVehicles);
  wb("Misc", "ClearPeds", g_ClearPeds);
  wb("Misc", "ShowLockedEntityMarker", g_ShowLockedEntityMarker);

  wf("UI", "MenuPosX", g_MenuPosX);
  wf("UI", "MenuPosY", g_MenuPosY);
  wf("UI", "MenuScale", g_MenuScale);
  wi("UI", "AccentR", g_MenuAccentR);
  wi("UI", "AccentG", g_MenuAccentG);
  wi("UI", "AccentB", g_MenuAccentB);
  wi("UI", "BgR", g_MenuBgR);
  wi("UI", "BgG", g_MenuBgG);
  wi("UI", "BgB", g_MenuBgB);
  wi("UI", "TextR", g_MenuTextR);
  wi("UI", "TextG", g_MenuTextG);
  wi("UI", "TextB", g_MenuTextB);
  wi("UI", "SelR", g_MenuSelR);
  wi("UI", "SelG", g_MenuSelG);
  wi("UI", "SelB", g_MenuSelB);
  wi("UI", "SelTextR", g_MenuSelTextR);
  wi("UI", "SelTextG", g_MenuSelTextG);
  wi("UI", "SelTextB", g_MenuSelTextB);

  wf("Sequence", "MarkerSize", g_SeqMarkerSize);
  wi("Sequence", "MarkerR", g_SeqMarkerR);
  wi("Sequence", "MarkerG", g_SeqMarkerG);
  wi("Sequence", "MarkerB", g_SeqMarkerB);
  wi("Sequence", "PathR", g_SeqPathR);
  wi("Sequence", "PathG", g_SeqPathG);
  wi("Sequence", "PathB", g_SeqPathB);
  wi("Sequence", "TimeLabelMode", g_SeqTimeLabelMode);
  wi("Sequence", "ClipSampleHz", g_VehicleClipSampleHz);
  wf("Sequence", "ClipSteerGain", g_VehicleClipSteerGain);
  wi("Sequence", "ClipShowDriver", g_VehicleClipShowDriver ? 1 : 0);
  wi("Sequence", "ClipGhostCollision", g_VehicleClipGhostCollision ? 1 : 0);
  wi("Sequence", "ClipGhostGodmode", g_VehicleClipGhostGodmode ? 1 : 0);
}

// Restore every persisted tunable to its factory default вЂ” the same values as
// the global initializers in camera.cpp. Does NOT write the INI (the user can
// Save afterward if they want the reset persisted) and leaves transient state
// (active mode, follow target, live time/weather) untouched.
void ResetSettingsToDefaults() {
  g_CamSpeed = 1.0f;
  g_CamSensitivity = 1.0f;
  g_CamFOV = 50.0f;
  g_CamRoll = 0.0f;
  g_RollSpeed = 1.0f;
  g_ZoomSpeed = 1.0f;
  g_CamCollision = false;
  g_LockHeight = false;
  g_RotationEngine = false;
  g_WalkMode = false;
  g_WalkHeight = 1.7f;
  g_FollowRigidMode = false;

  g_AutoDriveMode = 0;
  g_AutoDriveSpeed = 20.0f;
  g_AutoDriveStyleIndex = 0;

  g_DroneMode = false;
  g_DroneDrag = 3.0f;
  g_DroneAcceleration = 15.0f;
  g_DroneGravity = 0.0f;
  g_DroneBanking = 15.0f;
  g_DroneRotSmoothing = 8.0f;
  g_DroneFovSmoothing = 5.0f;

  g_ShakeEnabled = false;
  g_ShakePreset = 0;
  g_ShakeAmp = 0.0f;
  g_ShakeFreq = 4.0f;
  g_ShakeSpeedAmpCoupling = 1.0f;
  g_ShakeSpeedFreqCoupling = 0.5f;
  g_ShakeRotWeight = 1.0f;
  g_ShakePosWeight = 1.0f;
  g_ShakeStopWhenStill = false;

  g_DoFEnabled = false;
  g_DoFAutofocus = false;
  g_DoFFocusDist = 10.0f;
  g_DoFMaxNearInFocus = 0.5f;
  g_DoFMaxFarInFocus = 5.0f;

  g_RememberCamPosition = false;
  g_StreamAroundCamera = true;
  g_ShowInfoOverlay = false;
  g_DisableVehicleShake = false;
  g_ClearVehicles = false;
  g_ClearPeds = false;
  g_ShowLockedEntityMarker = true;

  g_MenuPosX = 0.025f;
  g_MenuPosY = 0.07f;
  g_MenuScale = 1.0f;
  g_MenuAccentR = 60;
  g_MenuAccentG = 200;
  g_MenuAccentB = 220;
  g_MenuBgR = 17; g_MenuBgG = 19; g_MenuBgB = 24;
  g_MenuTextR = 205; g_MenuTextG = 210; g_MenuTextB = 218;
  g_MenuSelR = 30; g_MenuSelG = 80; g_MenuSelB = 95;
  g_MenuSelTextR = 240; g_MenuSelTextG = 245; g_MenuSelTextB = 250;

  g_SeqMarkerSize = 0.30f;
  g_SeqMarkerR = 80;
  g_SeqMarkerG = 200;
  g_SeqMarkerB = 80;
  g_SeqPathR = 100;
  g_SeqPathG = 180;
  g_SeqPathB = 255;
  g_SeqTimeLabelMode = 2; // "1s"
  g_VehicleClipSampleHz = 30;
  g_VehicleClipSteerGain = 1.0f;
  g_VehicleClipShowDriver = true;
  g_VehicleClipGhostCollision = false;
  g_VehicleClipGhostGodmode = true;
}

// ============================================================
//  Menu Toggle
// ============================================================

bool IsMenuTogglePressed() { return IsKeyJustUp(g_MenuKey); }

// Clear the menu key's "just up" state so a single release can't be consumed
// twice in consecutive frames (e.g. the press that stops a vehicle-clip take
// shouldn't also re-open the menu on the next frame).
void ConsumeMenuToggle() { ResetKeyState(g_MenuKey); }

// True when the player's most recent input came from a gamepad (not keyboard +
// mouse). The frontend controls our pad combos read are ALSO bound to keyboard
// keys (INPUT_FRONTEND_LB/RB 205/206 -> Q/E, CANCEL 202 -> Esc), so without
// this gate those combos fire for keyboard users — pressing Q+E would open the
// menu. _GET_LAST_INPUT_METHOD(2) is true for keyboard & mouse.
static bool IsUsingGamepad() { return !CONTROLS::_GET_LAST_INPUT_METHOD(2); }

// Controller has no F5, so LB + RB opens the menu. Edge-triggered (one bumper
// held, the other just pressed) so holding both вЂ” or rolling with a single
// bumper вЂ” doesn't retrigger every frame. 205 = INPUT_FRONTEND_LB, 206 = RB.
// Gamepad-only: those controls double as keyboard Q/E (see IsUsingGamepad).
bool IsControllerMenuCombo() {
  if (!IsUsingGamepad()) return false;
  bool lb = PAD::IS_DISABLED_CONTROL_PRESSED(0, 205);
  bool rb = PAD::IS_DISABLED_CONTROL_PRESSED(0, 206);
  bool lbJust = PAD::IS_DISABLED_CONTROL_JUST_PRESSED(0, 205);
  bool rbJust = PAD::IS_DISABLED_CONTROL_JUST_PRESSED(0, 206);
  return (lb && rbJust) || (rb && lbJust);
}

// LB + B leaves Free Camera back to the mode picker without opening the menu.
// Edge-triggered on B (202 = INPUT_FRONTEND_CANCEL) while LB is held.
// Gamepad-only for the same reason (205/202 also map to keyboard Q/Esc).
bool IsControllerExitCombo() {
  if (!IsUsingGamepad()) return false;
  return PAD::IS_DISABLED_CONTROL_PRESSED(0, 205) &&
         PAD::IS_DISABLED_CONTROL_JUST_PRESSED(0, 202);
}


void MenuBeep() {
  AUDIO::PLAY_SOUND_FRONTEND(-1, (char *)"NAV_UP_DOWN",
                             (char *)"HUD_FRONTEND_DEFAULT_SOUNDSET", 0);
}

// ============================================================
//  Button State (Keyboard + Controller)
// ============================================================

static void GetMenuButtons(bool *select, bool *back, bool *up, bool *down,
                           bool *left, bool *right) {
  if (select)
    *select = IsKeyDown(VK_NUMPAD5) || IsKeyDown(VK_RETURN) ||
              PAD::IS_DISABLED_CONTROL_JUST_PRESSED(0, 201);
  if (back)
    *back = IsKeyDown(VK_NUMPAD0) || IsKeyDown(VK_BACK) ||
            IsMenuTogglePressed() ||
            PAD::IS_DISABLED_CONTROL_JUST_PRESSED(0, 202);
  if (up)
    *up = IsKeyDown(VK_NUMPAD8) || IsKeyDown(VK_UP) ||
          PAD::IS_DISABLED_CONTROL_PRESSED(0, 188);
  if (down)
    *down = IsKeyDown(VK_NUMPAD2) || IsKeyDown(VK_DOWN) ||
            PAD::IS_DISABLED_CONTROL_PRESSED(0, 187);
  if (right)
    *right = IsKeyDown(VK_NUMPAD6) || IsKeyDown(VK_RIGHT) ||
             PAD::IS_DISABLED_CONTROL_PRESSED(0, 190);
  if (left)
    *left = IsKeyDown(VK_NUMPAD4) || IsKeyDown(VK_LEFT) ||
            PAD::IS_DISABLED_CONTROL_PRESSED(0, 189);
}

// ============================================================
//  Shared per-frame + control helpers
// ============================================================

// Every menu suppresses the same phone / character-wheel controls so the
// player can't dismiss the menu sideways. Defined once here; every submenu
// calls it at the top of its draw loop.
static void DisableMenuPhoneControls() {
  CONTROLS::DISABLE_CONTROL_ACTION(0, 27, TRUE);   // INPUT_PHONE
  CONTROLS::DISABLE_CONTROL_ACTION(0, 172, TRUE);  // INPUT_PHONE_UP
  CONTROLS::DISABLE_CONTROL_ACTION(0, 173, TRUE);  // INPUT_PHONE_DOWN
  CONTROLS::DISABLE_CONTROL_ACTION(0, 174, TRUE);  // INPUT_PHONE_LEFT
  CONTROLS::DISABLE_CONTROL_ACTION(0, 175, TRUE);  // INPUT_PHONE_RIGHT
  CONTROLS::DISABLE_CONTROL_ACTION(0, 19, TRUE);   // INPUT_CHARACTER_WHEEL
  CONTROLS::DISABLE_CONTROL_ACTION(0, 166, TRUE);  // INPUT_SELECT_CHARACTER_MICHAEL
  CONTROLS::DISABLE_CONTROL_ACTION(0, 167, TRUE);  // INPUT_SELECT_CHARACTER_FRANKLIN
  CONTROLS::DISABLE_CONTROL_ACTION(0, 168, TRUE);  // INPUT_SELECT_CHARACTER_TREVOR
  CONTROLS::DISABLE_CONTROL_ACTION(0, 169, TRUE);  // INPUT_SELECT_CHARACTER_MULTIPLAYER
}

// ============================================================
//  Sequence в†’ Render to Image Sequence (Phase 2)
// ============================================================
//
// Deterministic offline render (Synced): the sequence PLAYS at a slow time
// scale so the camera, world, shake and effect events all advance on one game
// clock and stay in lockstep. We grab a frame each time playback crosses the
// next 1/fps mark вЂ” taking as long as each frame needs, independent of real
// FPS вЂ” so world motion matches camera speed exactly. The result is a numbered
// PNG sequence ready for ffmpeg.

// Render settings вЂ” non-static so the new framework menu (scmenu.cpp) can bind
// to the same state the classic render menu and ProcessRenderToImages use.
// Declared extern in menu.h.
float g_RenderFps = 60.0f;     // output frame rate
int g_RenderFlushFrames = 3;   // clean frames after the banner, before grabbing
int g_RenderBlurSamples = 1;   // motion-blur sub-samples per frame (1 = off)
int g_RenderFormat = 0;        // 0 = PNG (lossless), 1 = JPEG (lightweight)
int g_RenderJpegQuality = 90;  // JPEG quality 1..100
// (The former g_RenderSlowmo fixed-time-scale override is gone: the capture
// time scale is always AUTO-managed inside ProcessRenderToImages.)
float g_RenderHighlightBoost = 0.3f; // 0..0.99 вЂ” highlight lift in blur accumulation
float g_RenderRangeStart = 0.0f; // render range start (s); 0 = sequence start
float g_RenderRangeEnd = 0.0f;   // render range end (s); <=0 / <=start = to end

// Centered progress banner. Only drawn on settle frames вЂ” never on the frame
// the addon actually captures, so it can't bleed into the output.
static void DrawRenderProgress(int done, int total, const char *folder) {
  char buf[160];
  sprintf_s(buf, "RENDERING  %d / %d   (Backspace = cancel)", done, total);
  UI::SET_TEXT_FONT(0);
  UI::SET_TEXT_SCALE(0.6f, 0.6f);
  UI::SET_TEXT_COLOUR(255, 255, 255, 255);
  UI::SET_TEXT_WRAP(0.0, 1.0);
  UI::SET_TEXT_CENTRE(1);
  UI::SET_TEXT_DROPSHADOW(0, 0, 0, 0, 0);
  UI::SET_TEXT_EDGE(1, 0, 0, 0, 205);
  UI::_SET_TEXT_ENTRY((char *)"STRING");
  UI::_ADD_TEXT_COMPONENT_STRING((LPSTR)buf);
  UI::_DRAW_TEXT(0.5f, 0.45f);
}

void ProcessRenderToImages() {
  CameraSequence *s = Sequence_Active();
  if (!s || (int)s->poses.size() < 2) {
    SetStatusText("Need at least 2 keyframes to render");
    return;
  }
  if (!FxCapture_AddonPresent()) {
    SetStatusText("Image rendering needs ReShade + IgcsConnector addon. "
                  "Install both (see README), then restart.");
    return;
  }

  float dur = Sequence_TotalDuration();
  // Honor the sequence "Speed": it scales how fast the shot plays, so a render
  // advances the timeline by `speed/fps` per output frame and produces
  // `duration/speed * fps` frames. Speed > 1 = faster/shorter; < 1 = slower/
  // longer. Works in both modes (Synced stays in sync because camera + world
  // both advance by the same per-frame sequence step).
  float renderSpeed = (s->playbackSpeed > 0.0001f) ? s->playbackSpeed : 1.0f;
  const float stepT = renderSpeed / g_RenderFps; // sequence-time per output frame
  // Optional time range: render only [rangeStart, rangeEnd]. end<=0 or <=start
  // means "to the end". Lets you re-render just a section of the sequence.
  float rangeStart = g_RenderRangeStart;
  if (rangeStart < 0.0f) rangeStart = 0.0f;
  if (rangeStart > dur) rangeStart = dur;
  float rangeEnd = (g_RenderRangeEnd > 0.0001f) ? g_RenderRangeEnd : dur;
  if (rangeEnd > dur) rangeEnd = dur;
  if (rangeEnd <= rangeStart) { rangeStart = 0.0f; rangeEnd = dur; } // bad range -> full
  float rangeLen = rangeEnd - rangeStart;
  int total = (int)(rangeLen / renderSpeed * g_RenderFps + 0.5f);
  if (total < 1) {
    SetStatusText("Sequence too short to render");
    return;
  }

  char folder[MAX_PATH];
  if (!FxCapture_NewSequenceFolder(folder, sizeof(folder))) {
    SetStatusText("Couldn't create output folder");
    return;
  }

  // Save state we override during the render, restore on exit.
  bool prevFreezeEnt = g_FreezeEntities;
  bool prevHideHUD = g_HideHUD;
  bool prevOverlay = g_ShowInfoOverlay;
  bool prevMarkers = g_SequenceShowMarkers;
  float prevTime = Sequence_CurrentTime();
  if (Sequence_IsPlaying()) Sequence_Stop();

  // Take ownership of the time scale for the whole render so the World & Scene
  // slow-motion control (UpdateGlobalEffects) doesn't fight the capture time
  // scale. Cleared in the restore block, after which the slider re-asserts.
  g_RenderActive = true;

  // Hide the keyframe markers / path preview вЂ” Sequence_FrameTick (used by the
  // synced path) redraws them every frame, so they'd be baked into the output.
  g_SequenceShowMarkers = false;

  // Do NOT freeze entities вЂ” that pins positions but leaves wind-driven
  // vegetation, cloth and particles running at full speed (they'd smear in
  // motion blur and flicker between frames). Instead hold a low time scale so
  // ALL physics slow uniformly; the camera is still scrubbed exactly each
  // frame, independent of time scale.
  g_FreezeEntities = false;
  g_HideHUD = true;          // keep the HUD out of the frames
  g_ShowInfoOverlay = false; // and our debug overlay
  SetStatusText("", 0);      // clear any lingering status so it can't be captured

  FxCapture_SetQuality(g_RenderJpegQuality);
  FxCapture_SetHighlightBoost(g_RenderHighlightBoost);
  const char *ext = (g_RenderFormat == 1) ? "jpg" : "png";

  bool cancelled = false;
  bool addonFail = false;
  int progDone = 0;

  {
    // ============================================================
    //  Synced (live world) вЂ” playback-driven. The sequence PLAYS at a slow
    //  time scale, so the camera, world, shake and effect events all advance
    //  on the SAME game clock and stay in lockstep. We grab a frame each time
    //  playback crosses the next 1/fps mark в†’ world motion matches camera
    //  speed exactly. Motion blur (when enabled) accumulates M consecutive
    //  live frames per output frame, so it blurs the WHOLE moving scene.
    // ============================================================
    int syncSamples = g_RenderBlurSamples;
    if (syncSamples < 1) syncSamples = 1;

    // No-blur capture is a time INSTANT: the playhead must sit ON its 1/fps
    // grid mark when the addon grabs the frame. Any sequence time consumed
    // between crossing the mark and the actual capture present (banner, flush,
    // addon ack latency, PNG encode) shifts that frame's true time by a
    // VARIABLE amount — and frame-to-frame variance in that shift is visible
    // as speed ramps / judder in the output. So without blur we freeze the
    // playhead (near-zero time scale) for the whole work block and advance
    // ONLY in the catch-up loop, creeping the final approach so each capture
    // lands within a couple percent of its grid mark. With blur the work
    // block MUST stay live (the samples need world motion between them), so
    // that path keeps the AUTO budget controller as before.
    const bool instantCapture = (syncSamples == 1);
    const float kFrozenScale = 0.001f; // ~frozen (0.0 risks engine clamping)

    // Per-output-frame game-time budget = the sequence-time step between output
    // frames (speed-scaled). If the capture work for one output frame consumes
    // MORE than this, playback overshoots the next grid mark and the world runs
    // fast. AUTO tunes the time scale to keep the work inside the budget.
    const float budget = stepT;
    // curSlowmo is the live time scale stepDyn asserts; AUTO adapts it per
    // frame. Time scale is ALWAYS auto-managed — the fixed "World Slow-mo"
    // override was removed from the UI: any hand-set value could only match
    // what AUTO picks or break sync, so it was pure foot-gun.
    float curSlowmo;
    if (instantCapture) {
      // Playhead is frozen during the work block, so the time scale's only
      // job is catch-up granularity: aim for ~12 advance frames per output
      // frame (at ~60 real fps), with a 4x finer creep on final approach.
      // The budget controller below is skipped — consumed is ~0 by design.
      curSlowmo = stepT * 5.0f;
      if (curSlowmo < 0.005f) curSlowmo = 0.005f;
      if (curSlowmo > 0.5f) curSlowmo = 0.5f;
    } else {
      // Initial estimate from fps x work (assumes ~30 real fps; it
      // self-corrects within a few frames from measured consumption anyway).
      int workFrames = 2 + g_RenderFlushFrames + syncSamples;
      curSlowmo = budget / ((float)workFrames * 0.033f) * 0.7f;
      if (curSlowmo < 0.005f) curSlowmo = 0.005f;
      if (curSlowmo > 0.2f) curSlowmo = 0.2f;
    }

    float savedSpeed = s->playbackSpeed;
    bool savedLoop = s->loop;
    s->playbackSpeed = 1.0f; // 1:1 so output frame i maps to sequence time i/fps
    s->loop = false; // a render is ONE pass — a loop wrap would yank the
                     // playhead backward and force the catch-up loop to replay
                     // nearly the whole sequence to re-reach its target
    Sequence_SetCurrentTime(rangeStart); // start at the range's beginning
    Sequence_Play(); // Play snapshots/restores shake config itself

    // Assert the capture time scale BEFORE the first tick — SET_TIME_SCALE
    // affects the NEXT frame's dt, so without this the first FrameTick would
    // consume one full-speed frame and push frame 0 off its grid mark.
    GAMEPLAY::SET_TIME_SCALE(instantCapture ? kFrozenScale : curSlowmo);

    // `scale` is the time scale asserted for the frame this call waits out —
    // curSlowmo while advancing toward the next grid mark, kFrozenScale during
    // the no-blur work block so the playhead stays pinned on the mark.
    auto stepDyn = [&](bool banner, float scale) {
      DisableMenuPhoneControls();
      if (Sequence_IsInMode()) Sequence_FrameTick(); // camera+world+events+shake on one clock
      UpdateTimeWeather();
      UpdateGlobalEffects();
      GAMEPLAY::SET_TIME_SCALE(scale); // reassert capture time scale
      if (banner) DrawRenderProgress(progDone, total, folder);
      WAIT(0);
    };
    // Time scale for the banner/flush/capture block of each output frame.
    // (A function, not a snapshot — AUTO retunes curSlowmo between frames.)
    auto workScale = [&] { return instantCapture ? kFrozenScale : curSlowmo; };

    int cap = 0;
    while (cap < total && !cancelled && !addonFail) {
      progDone = cap;
      float target = rangeStart + (float)cap * stepT; // sequence time for this frame
      int safety = 0;
      float lastAdv = 0.0f; // sequence time the previous advance frame covered
      while (Sequence_CurrentTime() < target && !cancelled) {
        // Final-approach creep (no-blur only): once the remaining distance is
        // within a few frame-steps, advance 4x finer so we exit this loop —
        // and therefore capture — within ~2% of the grid mark instead of
        // overshooting by up to a full step. (SET_TIME_SCALE affects the NEXT
        // frame's dt, hence the 3x lookahead margin.)
        float remain = target - Sequence_CurrentTime();
        float scale = curSlowmo;
        if (instantCapture && lastAdv > 0.0f && remain < lastAdv * 3.0f)
          scale = curSlowmo * 0.25f;
        float t0 = Sequence_CurrentTime();
        stepDyn(true, scale);
        lastAdv = Sequence_CurrentTime() - t0;
        bool bBack;
        GetMenuButtons(NULL, &bBack, NULL, NULL, NULL, NULL);
        if (bBack) cancelled = true;
        if (!Sequence_IsPlaying()) break; // playback hit the end
        if (++safety > 20000) break;      // hard safety net
      }
      if (cancelled) break;

      // Snapshot the playhead the instant we've reached this frame's grid mark.
      // Everything below (banner + flush + capture samples) runs with playback
      // STILL LIVE, so each of those frames also advances the playhead. AUTO
      // must budget that ENTIRE block of work — not just the capture samples —
      // otherwise the banner/flush overshoot leaks past the grid mark every
      // frame, the absolute-target catch-up loop above gets skipped, the
      // overshoot compounds, and the whole sequence plays several times too
      // fast (camera animation finishing long before the last output frame).
      float tWorkStart = Sequence_CurrentTime();

      // Guarantee a couple of banner frames so progress is always visible вЂ”
      // the advance loop above is skipped when the per-frame blur work has
      // already overshot the grid mark.
      for (int k = 0; k < 2 && !cancelled; ++k) {
        stepDyn(true, workScale());
        bool bBack;
        GetMenuButtons(NULL, &bBack, NULL, NULL, NULL, NULL);
        if (bBack) cancelled = true;
      }
      if (cancelled) break;
      // Clean frames so the banner clears the pipeline before grabbing.
      for (int k = 0; k < g_RenderFlushFrames; ++k) stepDyn(false, workScale());

      char path[MAX_PATH];
      sprintf_s(path, "%s\\frame_%06d.%s", folder, cap, ext);

      // Accumulate `syncSamples` consecutive live frames. The world advances a
      // little between each (slowed playback), so the addon's linear average
      // becomes true full-scene motion blur. 1 = no blur.
      //
      // The banner is suppressed here (it would bake into the captured samples),
      // so this is the long stretch where progress is invisible. Poll Backspace
      // every sample so a high sample count can still be cancelled promptly.
      for (int j = 0; j < syncSamples && !addonFail && !cancelled; ++j) {
        FxCapture_RequestSample(path, syncSamples, j);
        int guard = 0;
        while (!FxCapture_IsLastDone() && guard < 300) { stepDyn(false, workScale()); ++guard; }
        if (guard >= 300) addonFail = true;
        bool bBack;
        GetMenuButtons(NULL, &bBack, NULL, NULL, NULL, NULL);
        if (bBack) cancelled = true;
      }
      if (cancelled) break;

      // AUTO: nudge the time scale so ALL of this frame's post-grid-mark work
      // (banner + flush + capture samples) lands inside the budget. Keeping it
      // under stepT means the playhead overshoots the next grid mark by less
      // than one output step, so the catch-up loop above re-engages every frame
      // and the playhead can't run away. Too much consumed -> slow down; plenty
      // of headroom -> speed up (don't waste wall-clock). Self-correcting.
      // Skipped in instant-capture mode: the work block runs frozen there, so
      // consumed is ~0 by design and the controller would just ramp curSlowmo
      // to its cap, wrecking the catch-up granularity chosen above.
      if (!instantCapture) {
        float consumed = Sequence_CurrentTime() - tWorkStart;
        if (consumed > budget * 0.85f) curSlowmo *= 0.7f;
        else if (consumed < budget * 0.40f) curSlowmo *= 1.15f;
        if (curSlowmo < 0.003f) curSlowmo = 0.003f;
        if (curSlowmo > 0.5f) curSlowmo = 0.5f;
      }
      ++cap;
    }

    Sequence_Stop();
    s->playbackSpeed = savedSpeed;
    s->loop = savedLoop;
  }

  // Restore.
  GAMEPLAY::SET_TIME_SCALE(1.0f);
  g_RenderActive = false; // hand the time scale back to World & Scene slow-motion
  g_FreezeEntities = prevFreezeEnt;
  g_HideHUD = prevHideHUD;
  g_ShowInfoOverlay = prevOverlay;
  g_SequenceShowMarkers = prevMarkers;
  Sequence_SetCurrentTime(prevTime);

  if (addonFail) {
    SetStatusText("Capture addon not responding вЂ” aborted");
  } else if (cancelled) {
    SetStatusText("Render cancelled");
  } else {
    char msg[96];
    sprintf_s(msg, "Render complete: %d frames", total);
    SetStatusText(msg);
  }
}
