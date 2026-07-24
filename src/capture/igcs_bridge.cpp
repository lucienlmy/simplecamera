/*
    IGCS Connector Bridge for Simple Camera Plugin

    See igcs_bridge.h for protocol description.
*/

#include "igcs_bridge.h"
#include "camera.h"
#include <Psapi.h>

#pragma warning(disable : 4244 4305)

// ============================================================
//  Constants
// ============================================================

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEG2RAD_IGCS (M_PI / 180.0)

// ============================================================
//  IGCS Connector function pointers (we discover these)
// ============================================================

typedef bool (*ConnectFromCameraToolsFunc)();
typedef LPBYTE (*GetDataFromCameraToolsBufferFunc)();

static ConnectFromCameraToolsFunc s_connectFunc = nullptr;
static GetDataFromCameraToolsBufferFunc s_getBufferFunc = nullptr;
static CameraToolsData *s_cameraToolsData = nullptr;
static bool s_connected = false;

// ============================================================
//  Screenshot session state
// ============================================================

static bool s_screenshotSessionActive = false;
static float s_savedPosX, s_savedPosY, s_savedPosZ;
static float s_savedPitch, s_savedYaw, s_savedRoll;
static float s_savedFOV;
static bool s_savedFreezeWorld = false;
static float s_savedTimeScale = 1.0f;

// ============================================================
//  Euler to Quaternion conversion
// ============================================================

static void EulerToQuaternion(float pitchDeg, float yawDeg, float rollDeg,
                              float &qx, float &qy, float &qz, float &qw) {
  // Convert to radians and half-angles
  float hp = (float)(pitchDeg * DEG2RAD_IGCS * 0.5);
  float hy = (float)(yawDeg * DEG2RAD_IGCS * 0.5);
  float hr = (float)(rollDeg * DEG2RAD_IGCS * 0.5);

  float sp = sinf(hp), cp = cosf(hp);
  float sy = sinf(hy), cy = cosf(hy);
  float sr = sinf(hr), cr = cosf(hr);

  // ZYX order (yaw * pitch * roll)
  qw = cy * cp * cr + sy * sp * sr;
  qx = cy * sp * cr + sy * cp * sr;
  qy = sy * cp * cr - cy * sp * sr;
  qz = cy * cp * sr - sy * sp * cr;
}

// ============================================================
//  Rotation matrix vectors from Euler angles
// ============================================================

static void EulerToRotationVectors(float pitchDeg, float yawDeg, float rollDeg,
                                   float *fwd, float *right, float *up) {
  float p = (float)(pitchDeg * DEG2RAD_IGCS);
  float y = (float)(yawDeg * DEG2RAD_IGCS);
  float r = (float)(rollDeg * DEG2RAD_IGCS);

  float cp = cosf(p), sp = sinf(p);
  float cy = cosf(y), sy = sinf(y);
  float cr = cosf(r), sr = sinf(r);

  // Forward vector (GTA V: -sin(yaw)*cos(pitch), cos(yaw)*cos(pitch),
  // sin(pitch))
  fwd[0] = -sy * cp;
  fwd[1] = cy * cp;
  fwd[2] = sp;

  // Right vector
  right[0] = cy * cr + sy * sp * sr;
  right[1] = sy * cr - cy * sp * sr;
  right[2] = cp * sr;

  // Up vector
  up[0] = -cy * sr + sy * sp * cr;
  up[1] = -sy * sr - cy * sp * cr;
  up[2] = cp * cr;
}

// ============================================================
//  DLL Exports (IGCS Connector discovers these via GetProcAddress)
// ============================================================

extern "C" __declspec(dllexport) int IGCS_StartScreenshotSession(uint8_t type) {
  if (!g_FreeCamActive)
    return 1; // Error: camera not enabled

  if (s_screenshotSessionActive)
    return 3; // Error: session already active

  // Save camera state at session start
  float px, py, pz, pitch, yaw, roll;
  GetCameraState(px, py, pz, pitch, yaw, roll);
  s_savedPosX = px;
  s_savedPosY = py;
  s_savedPosZ = pz;
  s_savedPitch = pitch;
  s_savedYaw = yaw;
  s_savedRoll = roll;
  s_savedFOV = g_CamFOV;

  // Keep the scene near-still for the duration of the DoF session WITHOUT
  // freezing the camera. IGCS DoF works by nudging the camera to many small
  // offsets and blending the frames, so the camera + rendering MUST stay live
  // — Pause Game (SET_GAME_PAUSED) would freeze the camera too and break the
  // session. Freezing entities doesn't help either: it pins positions but
  // animations keep playing. Slowing time to 1% gets the best of both: peds /
  // vehicles / cloth / particles barely move between samples, while the camera
  // and ReShade keep running normally. Restored in EndScreenshotSession.
  s_savedFreezeWorld = g_FreezeWorld;
  g_FreezeWorld = false; // never Pause Game during a session — it stalls the cam
  s_savedTimeScale = g_WorldTimeScale;
  g_WorldTimeScale = 0.01f; // 1% — UpdateGlobalEffects applies SET_TIME_SCALE

  IGCS_ResetOffsets();
  s_screenshotSessionActive = true;
  return 0; // AllOk
}

extern "C" __declspec(dllexport) void IGCS_EndScreenshotSession() {
  if (s_screenshotSessionActive) {
    // Restore camera to session start position (relative to moving target)
    IGCS_ResetOffsets();
    g_CamFOV = s_savedFOV;

    // Restore the pre-session world-speed state (we slowed time to 1% on
    // start; UpdateGlobalEffects will reapply or clear the time scale).
    g_FreezeWorld = s_savedFreezeWorld;
    g_WorldTimeScale = s_savedTimeScale;
    if (!g_FreezeWorld && g_WorldTimeScale >= 0.999f) {
      GAMEPLAY::SET_TIME_SCALE(1.0f);
    }
  }
  IGCS_ResetOffsets();
  s_screenshotSessionActive = false;
}

extern "C" __declspec(dllexport) void IGCS_MoveCameraPanorama(float stepAngle) {
  if (!g_FreeCamActive || !s_screenshotSessionActive)
    return;

  // stepAngle is in radians, convert to degrees and shift yaw
  IGCS_ShiftCamera(0, 0, 0, 0, (float)(stepAngle * 180.0 / M_PI), 0);
}

extern "C" __declspec(dllexport) void
IGCS_MoveCameraMultishot(float stepLeftRight, float stepUpDown,
                         float fovDegrees, bool fromStartPosition) {
  if (!g_FreeCamActive || !s_screenshotSessionActive)
    return;

  if (fromStartPosition) {
    // Reset to saved start position first, then apply offset.
    // By zeroing the IGCS offsets, we instantly snap back to the plugin's base
    // tracking coordinates!
    IGCS_ResetOffsets();
  }

  // Move camera by the specified amounts along camera-relative right/up
  IGCS_ShiftCamera(stepLeftRight, stepUpDown, 0, 0, 0, 0);

  if (fovDegrees > 0.0f) {
    g_CamFOV = fovDegrees;
  }
}

// ============================================================
//  Discovery: find IGCS Connector in loaded modules
// ============================================================

void IGCS_TryConnect() {
  if (s_connected)
    return;

  HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                FALSE, GetCurrentProcessId());
  if (!hProcess)
    return;

  HMODULE modules[512];
  DWORD cbNeeded;
  if (EnumProcessModules(hProcess, modules, sizeof(modules), &cbNeeded)) {
    // cbNeeded is the number of bytes REQUIRED, which can exceed our buffer when
    // the process has more than 512 modules loaded — clamp so we never index
    // past modules[].
    DWORD filled = cbNeeded < sizeof(modules) ? cbNeeded : (DWORD)sizeof(modules);
    for (DWORD i = 0; i < filled / sizeof(HMODULE); i++) {
      auto connectFunc = (ConnectFromCameraToolsFunc)GetProcAddress(
          modules[i], "connectFromCameraTools");
      if (!connectFunc)
        continue;

      auto getBufferFunc = (GetDataFromCameraToolsBufferFunc)GetProcAddress(
          modules[i], "getDataFromCameraToolsBuffer");
      if (!getBufferFunc)
        continue;

      // Found the IGCS Connector!
      s_connectFunc = connectFunc;
      s_getBufferFunc = getBufferFunc;

      // Call connect to allocate the shared buffer
      if (s_connectFunc()) {
        LPBYTE buffer = s_getBufferFunc();
        if (buffer) {
          s_cameraToolsData = (CameraToolsData *)buffer;
          s_connected = true;
        }
      }
      break;
    }
  }
  CloseHandle(hProcess);
}

// ============================================================
//  Write camera state to shared buffer
// ============================================================

void IGCS_UpdateData(float posX, float posY, float posZ, float pitchDeg,
                     float yawDeg, float rollDeg, float fov,
                     bool cameraEnabled) {
  if (!s_connected || !s_cameraToolsData)
    return;

  s_cameraToolsData->cameraEnabled = cameraEnabled ? 1 : 0;
  s_cameraToolsData->cameraMovementLocked = s_screenshotSessionActive ? 1 : 0;
  s_cameraToolsData->fov = fov;

  s_cameraToolsData->coordinates.set(posX, posY, posZ);

  // Quaternion
  float qx, qy, qz, qw;
  EulerToQuaternion(pitchDeg, yawDeg, rollDeg, qx, qy, qz, qw);
  s_cameraToolsData->lookQuaternion.set(qx, qy, qz, qw);

  // Rotation matrix vectors
  float fwd[3], right[3], up[3];
  EulerToRotationVectors(pitchDeg, yawDeg, rollDeg, fwd, right, up);
  s_cameraToolsData->rotationMatrixForwardVector.set(fwd[0], fwd[1], fwd[2]);
  s_cameraToolsData->rotationMatrixRightVector.set(right[0], right[1],
                                                   right[2]);
  s_cameraToolsData->rotationMatrixUpVector.set(up[0], up[1], up[2]);

  // Euler angles in radians (as IGCS expects)
  s_cameraToolsData->pitch = (float)(pitchDeg * DEG2RAD_IGCS);
  s_cameraToolsData->yaw = (float)(yawDeg * DEG2RAD_IGCS);
  s_cameraToolsData->roll = (float)(rollDeg * DEG2RAD_IGCS);
}

bool IGCS_IsConnected() { return s_connected; }
bool IGCS_IsSessionActive() { return s_screenshotSessionActive; }
