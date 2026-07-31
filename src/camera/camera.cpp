/*
        GTA V Free Camera / Photo Mode Plugin
        Camera System Implementation

        6-DOF free camera with smooth keyboard + mouse + controller input,
        depth of field integration, and time/weather overrides.
*/

#include "camera.h"
#include "igcs_bridge.h"
#include "keyboard.h"
#include "vehicleclip.h" // recorded-vehicle ghost handle for streaming focus
#include <cmath>
#include <cstdio>

#pragma warning(disable : 4244 4305)

// ============================================================
//  Constants
// ============================================================

static const float PI = 3.14159265358979323846f;
static const float DEG2RAD = PI / 180.0f;

// Controller input IDs (GTA V control enums)
static const int INPUT_LOOK_LR = 1;           // Right stick X
static const int INPUT_LOOK_UD = 2;           // Right stick Y
static const int INPUT_MOVE_LR = 30;          // Left stick X
static const int INPUT_MOVE_UD = 31;          // Left stick Y
static const int INPUT_ATTACK = 24;           // RT
static const int INPUT_AIM = 25;              // LT
static const int INPUT_FRONTEND_LB = 205;     // LB
static const int INPUT_FRONTEND_RB = 206;     // RB
static const int INPUT_FRONTEND_UP = 188;     // D-pad Up
static const int INPUT_FRONTEND_DOWN = 187;   // D-pad Down
static const int INPUT_FRONTEND_ACCEPT = 201; // A / Enter
static const int INPUT_FRONTEND_CANCEL = 202; // B / Backspace

// ============================================================
//  Global State
// ============================================================

bool g_FreeCamActive = false;
float g_CamSpeed = 1.0f;
float g_CamSensitivity = 1.0f;
float g_CamFOV = 50.0f;
float g_CamRoll = 0.0f;
float g_RollSpeed = 1.0f;
float g_ZoomSpeed = 1.0f;
bool g_LockCamera = false;
bool g_EnablePlayerMovement = false;
bool g_CamCollision = false;
bool g_LockHeight = false;
bool g_RotationEngine = false;

// Follow Entity Mode
int g_FollowMode = 0; // 0 = None, 1 = Player, 2 = Aimed Entity
int g_FollowTargetEntity = 0;
bool g_FollowRigidMode = false;

// Drone Camera Mode
bool g_DroneMode = false;
float g_DroneDrag = 3.0f;
float g_DroneAcceleration = 15.0f;
float g_DroneGravity = 0.0f;
float g_DroneBanking = 15.0f;
float g_DroneRotSmoothing = 8.0f;
float g_DroneFovSmoothing = 5.0f;

// Procedural Camera Shake
bool g_ShakeEnabled = false;
int g_ShakePreset = 0;              // Off
float g_ShakeAmp = 0.0f;
float g_ShakeFreq = 4.0f;
float g_ShakeSpeedAmpCoupling = 1.0f;
float g_ShakeSpeedFreqCoupling = 0.5f;
float g_ShakeRotWeight = 1.0f;
float g_ShakePosWeight = 1.0f;
float g_ShakeSpeedRefMax = 30.0f;   // reference max speed (m/s) for normalization
bool g_ShakeStopWhenStill = false;

// Walk Mode
bool g_WalkMode = false;
float g_WalkHeight = 1.7f; // ~5'7" eye level

// Auto Drive
bool g_AutoDriveEnabled = false;
int g_AutoDriveMode = 0;        // 0 = Go To Waypoint, 1 = Drive Anywhere
float g_AutoDriveSpeed = 20.0f; // m/s (~72 km/h)
int g_AutoDriveStyleIndex = 0;

// Depth of Field
bool g_DoFEnabled = false;
bool g_DoFAutofocus = false;
float g_DoFFocusDist = 10.0f;
float g_DoFMaxNearInFocus = 0.5f;
float g_DoFMaxFarInFocus = 5.0f;

// Time & Weather
bool g_TimePaused = false;
int g_TimeHour = 12;
int g_TimeMinute = 0;
int g_Weather1Index = 0;
int g_Weather2Index = 1;
float g_WeatherBlend = 0.0f;
bool g_BlendWeatherActive = false;
int g_TimelapseMode = 0;

const char *g_WeatherNames[] = {
    "CLEAR",     "EXTRASUNNY", "CLOUDS",  "OVERCAST", "RAIN",
    "CLEARING",  "THUNDER",    "SMOG",    "FOGGY",    "XMAS",
    "SNOWLIGHT", "BLIZZARD",   "NEUTRAL", "SNOW"};
const int g_WeatherCount = sizeof(g_WeatherNames) / sizeof(g_WeatherNames[0]);

bool g_IsFiveM = false;

// Mode dispatcher: -1 = unset (picker), 0 = Free Camera, 1 = Camera Sequence
int g_CameraMode = -1;
bool g_SequencePlaybackActive = false;

// ---- Misc ----
bool g_HideHUD = false;
bool g_DisableVehicleShake = false;
bool g_HidePlayer = false;
bool g_RememberCamPosition = false;
bool g_StreamAroundCamera = true;
bool g_FreezeWorld = false;     // Pause Game (SET_GAME_PAUSED)
bool g_FreezeEntities = false;  // Freeze All Entities (camera stays live)
float g_WorldTimeScale = 1.0f;
bool g_RenderActive = false;    // true while ProcessRenderToImages owns the time
                                // scale; suspends World & Scene slow-motion so the
                                // two don't fight over SET_TIME_SCALE.
bool g_ShowInfoOverlay = false;
bool g_ShowLockedEntityMarker = true;
bool g_ClearVehicles = false; // World & Scene: keep the map clear of ambient vehicles
bool g_ClearPeds = false;     // World & Scene: keep the map clear of ambient peds

// ============================================================
//  Functions
// ============================================================

// Detect the FiveM host. Gates which clock/weather natives are legal and
// whether VehMem may use the CFX wheel natives, so a wrong answer here is
// expensive.
//
// Test MODULES LOADED IN THE GAME PROCESS, never the process name: FiveM runs
// the game in a subprocess called FiveM_b<build>_GTAProcess.exe, so a check for
// "FiveM.exe" is always NULL there.
void DetectFiveM() {
  g_IsFiveM = (GetModuleHandleA("citizen-game-main.dll") != NULL ||
               GetModuleHandleA("asi-five.dll") != NULL ||
               GetModuleHandleA("scripting-gta.dll") != NULL ||
               GetModuleHandleA("CitizenGame.dll") != NULL);
}

void SetClockTime(int hour, int minute, int second) {
  if (g_IsFiveM) {
    // We use multiple natives for FiveM to ensure the time change sticks,
    // especially in Story Mode or when other scripts/sync might interfere.
    // 1. Standard SET_CLOCK_TIME (0x47C3B5848C3E45D8)
    invoke<Void>(0x47C3B5848C3E45D8, hour, minute, second);
    // 2. ADVANCE_CLOCK_TIME_TO (0xC8CA9670B9D83B3B)
    invoke<Void>(0xC8CA9670B9D83B3B, hour, minute, second);
    // 3. NETWORK_OVERRIDE_CLOCK_TIME (0xE679E3E06E363892)
    invoke<Void>(0xE679E3E06E363892, hour, minute, second);
  } else {
    TIME::SET_CLOCK_TIME(hour, minute, second);
  }
}

void SetWeatherTransition(Hash w1, Hash w2, float blend) {
  if (g_IsFiveM) {
    // FiveM Native: _SET_WEATHER_TYPE_TRANSITION(weatherType1, weatherType2,
    // percentWeather2)
    // Hash: 0x578C752848ECFA0C
    invoke<Void>(0x578C752848ECFA0C, w1, w2, blend);
  } else {
    // SP native hash for _SET_WEATHER_TYPE_TRANSITION is the same,
    // but we use the explicit call to match the pattern or existing script calls.
    invoke<Void>(0x578C752848ECFA0C, w1, w2, blend);
  }
}

// ---- Internal camera state ----

static Cam s_Cam = 0;
static float s_PosX = 0.0f, s_PosY = 0.0f, s_PosZ = 0.0f;
static float s_Pitch = 0.0f, s_Yaw = 0.0f; // degrees (heading)
static float s_IgcsOffsetX = 0.0f, s_IgcsOffsetY = 0.0f, s_IgcsOffsetZ = 0.0f;
static float s_IgcsPitchOffset = 0.0f, s_IgcsYawOffset = 0.0f,
             s_IgcsRollOffset = 0.0f;
static Ped s_FrozenPed = 0;
static bool s_HasSavedPosition = false;
// Stream-around-camera: the player ped's real position (saved on entry) and
// whether we've been relocating it to the camera, so we can put it back.
static float s_PlayerHomeX = 0, s_PlayerHomeY = 0, s_PlayerHomeZ = 0;
static bool s_PlayerMoved = false;
static bool s_FocusSet = false;     // SET_FOCUS_ENTITY active (in-vehicle case)
static bool s_StreamActive = false; // currently anchoring streaming to the cam
static Entity s_RotationAnchor = 0;

// Rigid-follow attach state. File-scoped (not function-local statics) so
// DestroyFreeCamera can reset them on teardown — otherwise a stale "already
// attached" flag survives into the next free-cam session and the freshly
// created s_Cam is never attached to the follow target.
static bool s_IsAttached = false;
static int s_AttachedEntity = 0;

// Drone physics state
static float s_DroneVelX = 0.0f;
static float s_DroneVelY = 0.0f;
static float s_DroneVelZ = 0.0f;
static float s_DroneYawRate = 0.0f;   // degrees/sec
static float s_DronePitchRate = 0.0f; // degrees/sec
static float s_DroneRollRate = 0.0f;  // degrees/sec
static float s_DroneTargetRoll = 0.0f;

// Procedural shake state
static float s_ShakePhase = 0.0f;
static bool s_HasPrevPos = false;
static float s_PrevPosX = 0.0f, s_PrevPosY = 0.0f, s_PrevPosZ = 0.0f;
static float s_ShakeSeeds[6] = {17.31f, 41.07f, 73.59f, 11.27f, 53.83f, 97.41f};
// Per-axis frequency multipliers. Incommensurate ratios so axes drift
// independently and the global pattern never re-syncs. Randomized in
// RandomizeShakePattern() to make each free-cam session feel different.
static float s_ShakeAxisFreqMul[6] = {1.000f, 1.073f, 0.927f, 1.131f,
                                      0.893f, 1.211f};
// Smoothed motion factor (0..1) used by Stop-When-Still
static float s_ShakeMotionFactor = 1.0f;

// ============================================================
//  Quaternion Support for Acrobatic Mode
// ============================================================

struct Quat {
  float w, x, y, z;
};

static Quat MultiplyQuat(const Quat &q1, const Quat &q2) {
  return {q1.w * q2.w - q1.x * q2.x - q1.y * q2.y - q1.z * q2.z,
          q1.w * q2.x + q1.x * q2.w + q1.y * q2.z - q1.z * q2.y,
          q1.w * q2.y - q1.x * q2.z + q1.y * q2.w + q1.z * q2.x,
          q1.w * q2.z + q1.x * q2.y - q1.y * q2.x + q1.z * q2.w};
}

static Quat AxisAngleToQuat(float axisX, float axisY, float axisZ,
                            float angleRad) {
  float halfAngle = angleRad * 0.5f;
  float s = sinf(halfAngle);
  return {cosf(halfAngle), axisX * s, axisY * s, axisZ * s};
}

static Quat NormalizeQuat(const Quat &q) {
  float len = sqrtf(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
  if (len > 0.00001f) {
    return {q.w / len, q.x / len, q.y / len, q.z / len};
  }
  return {1.0f, 0.0f, 0.0f, 0.0f};
}

static float s_LastYaw = 0.0f;
static float s_LastRoll = 0.0f;

static void QuatToEuler(const Quat &q, float &pitchOut, float &yawOut,
                        float &rollOut) {
  // Convert Quaternion to Rotation Matrix elements
  float m11 = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
  float m12 = 2.0f * (q.x * q.y - q.z * q.w);
  float m13 = 2.0f * (q.x * q.z + q.y * q.w);

  float m21 = 2.0f * (q.x * q.y + q.z * q.w);
  float m22 = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
  float m23 = 2.0f * (q.y * q.z - q.x * q.w);

  float m31 = 2.0f * (q.x * q.z - q.y * q.w);
  float m32 = 2.0f * (q.y * q.z + q.x * q.w);
  float m33 = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);

  // GTA V Rotation Order 2 (ZXY equivalent)
  pitchOut = asinf(max(-1.0f, min(1.0f, m32))) * (180.0f / PI);

  // Check for Gimbal Lock (Pitch near +/- 90)
  if (abs(m32) < 0.999f) {
    yawOut = atan2f(-m12, m22) * (180.0f / PI);
    rollOut = atan2f(-m31, m33) * (180.0f / PI);

    // Store safe orientations
    s_LastYaw = yawOut;
    s_LastRoll = rollOut;
  } else {

    yawOut = atan2f(m21, m11) * (180.0f / PI);

    if (m32 < 0) {
      yawOut -= s_LastRoll;
    } else {
      yawOut += s_LastRoll;
    }

    // Smooth boundary clamp
    // Restricting Roll manually prevents the "Screen Flip" structural snap
    rollOut = s_LastRoll;
  }
}

static Quat EulerToQuat(float pitchDeg, float yawDeg, float rollDeg) {
  Quat qYaw = AxisAngleToQuat(0, 0, 1, -yawDeg * DEG2RAD);
  Quat qPitch = AxisAngleToQuat(1, 0, 0, pitchDeg * DEG2RAD);
  Quat qRoll = AxisAngleToQuat(0, 1, 0, rollDeg * DEG2RAD);

  Quat qCombined = MultiplyQuat(qYaw, qPitch);
  return MultiplyQuat(qCombined, qRoll);
}

static Quat s_AcroMatrix = {1.0f, 0.0f, 0.0f, 0.0f};

// ============================================================
//  Procedural Camera Shake
// ============================================================

// 3-octave summed-sine value noise, sampled at parameter t (already
// scaled by frequency in caller). seed shifts the phase per axis so
// different DOFs don't move in lockstep. Output range ~ [-1, +1].
static float ShakeNoise1D(float t, float seed) {
  float n = 0.0f;
  n += sinf(t + seed) * 0.5714f;             // octave 0, weight 4/7
  n += sinf(t * 2.03f + seed * 1.7f) * 0.2857f; // octave 1, weight 2/7
  n += sinf(t * 4.11f + seed * 2.3f) * 0.1429f; // octave 2, weight 1/7
  return n;
}

// Preset table: BaseAmp, BaseFreq, SpeedAmpCoupling, SpeedFreqCoupling
struct ShakePresetData {
  float amp;
  float freq;
  float spdAmp;
  float spdFreq;
};
static const ShakePresetData kShakePresets[5] = {
    {0.00f,  4.0f, 0.0f, 0.0f}, // 0 Off
    {0.15f,  2.5f, 0.5f, 0.3f}, // 1 Subtle
    {0.40f,  4.0f, 1.0f, 0.5f}, // 2 Handheld
    {0.70f,  6.0f, 1.5f, 1.0f}, // 3 Vehicle
    {1.50f, 10.0f, 0.0f, 0.0f}, // 4 Earthquake
};

void ApplyShakePreset(int preset) {
  if (preset < 0 || preset > 4)
    return;
  const ShakePresetData &p = kShakePresets[preset];
  g_ShakeAmp = p.amp;
  g_ShakeFreq = p.freq;
  g_ShakeSpeedAmpCoupling = p.spdAmp;
  g_ShakeSpeedFreqCoupling = p.spdFreq;
  g_ShakePreset = preset;
}

// Re-roll the noise seeds and per-axis frequency multipliers. Called
// automatically when free-cam starts (so each session feels different)
// and on demand from the Camera Effects menu's "Randomize Pattern" row.
void RandomizeShakePattern() {
  for (int i = 0; i < 6; ++i) {
    // Phase seed: large range so axes never align by accident
    s_ShakeSeeds[i] = (rand() / (float)RAND_MAX) * 1000.0f;
    // Frequency multiplier in [0.80, 1.25]. Keeps every axis close to the
    // user-chosen base frequency while ensuring no two axes share a rational
    // ratio, so the composite pattern is effectively non-repeating.
    s_ShakeAxisFreqMul[i] = 0.80f + (rand() / (float)RAND_MAX) * 0.45f;
  }
  // Reset phase so the new pattern starts from a clean t=0 rather than
  // continuing wherever the previous pattern was sitting.
  s_ShakePhase = 0.0f;
}

// Compute 6 shake offsets for this frame. Advances phase by dt * effective
// frequency so shake speed is framerate-independent.
static void ComputeShakeOffsets(float dt, float &dx, float &dy, float &dz,
                                float &dPitch, float &dYaw, float &dRoll) {
  dx = dy = dz = dPitch = dYaw = dRoll = 0.0f;

  if (!g_ShakeEnabled || g_ShakeAmp <= 0.0001f)
    return;

  // Estimate current camera speed (m/s)
  float speed = 0.0f;
  if (g_DroneMode) {
    speed = sqrtf(s_DroneVelX * s_DroneVelX + s_DroneVelY * s_DroneVelY +
                  s_DroneVelZ * s_DroneVelZ);
  } else if (s_HasPrevPos && dt > 0.0001f) {
    float ddx = s_PosX - s_PrevPosX;
    float ddy = s_PosY - s_PrevPosY;
    float ddz = s_PosZ - s_PrevPosZ;
    speed = sqrtf(ddx * ddx + ddy * ddy + ddz * ddz) / dt;
  }

  float speedNorm = speed / (g_ShakeSpeedRefMax > 0.0f ? g_ShakeSpeedRefMax : 30.0f);
  if (speedNorm > 1.0f) speedNorm = 1.0f;
  if (speedNorm < 0.0f) speedNorm = 0.0f;

  float effectiveFreq = g_ShakeFreq *
                        (1.0f + g_ShakeSpeedFreqCoupling * speedNorm);
  float effectiveAmp = g_ShakeAmp *
                       (1.0f + g_ShakeSpeedAmpCoupling * speedNorm);

  // Motion gate: when "Stop When Still" is on, fade the shake out the
  // moment translation drops below a small threshold. Rotation does NOT
  // count as motion — looking around with a still body shouldn't shake.
  // Hysteresis via exponential smoothing prevents popping.
  if (g_ShakeStopWhenStill) {
    const float MOTION_THRESHOLD = 0.05f; // m/s
    float target = (speed > MOTION_THRESHOLD) ? 1.0f : 0.0f;
    // Fast attack (~80 ms) when motion starts, slow release (~250 ms) when
    // it stops, so the shake feels responsive but settles gracefully.
    float rate = (target > s_ShakeMotionFactor) ? 12.0f : 4.0f;
    float k = 1.0f - expf(-dt * rate);
    s_ShakeMotionFactor += (target - s_ShakeMotionFactor) * k;
  } else {
    s_ShakeMotionFactor = 1.0f;
  }

  if (s_ShakeMotionFactor < 0.001f)
    return; // Fully gated — skip the trig work entirely

  // Advance phase. The 2*PI converts Hz into radians/sec for sin().
  s_ShakePhase += dt * effectiveFreq * 2.0f * PI;
  // Wrap to avoid float-precision loss over long sessions
  const float TWO_PI = 2.0f * PI;
  if (s_ShakePhase > 1000.0f * TWO_PI)
    s_ShakePhase -= 1000.0f * TWO_PI;

  // Sample each axis at its own phase rate. Per-axis frequency
  // multipliers are incommensurate, so the 6 channels never re-sync —
  // the macro pattern is effectively non-repeating.
  float nx  = ShakeNoise1D(s_ShakePhase * s_ShakeAxisFreqMul[0], s_ShakeSeeds[0]);
  float ny  = ShakeNoise1D(s_ShakePhase * s_ShakeAxisFreqMul[1], s_ShakeSeeds[1]);
  float nz  = ShakeNoise1D(s_ShakePhase * s_ShakeAxisFreqMul[2], s_ShakeSeeds[2]);
  float np  = ShakeNoise1D(s_ShakePhase * s_ShakeAxisFreqMul[3], s_ShakeSeeds[3]);
  float nyw = ShakeNoise1D(s_ShakePhase * s_ShakeAxisFreqMul[4], s_ShakeSeeds[4]);
  float nr  = ShakeNoise1D(s_ShakePhase * s_ShakeAxisFreqMul[5], s_ShakeSeeds[5]);

  // Scale into world units. Position uses 5 cm per amp unit; rotation
  // uses 1 degree per amp unit (asymmetric because rotation reads
  // visually stronger than translation at the same magnitude).
  const float POS_SCALE = 0.05f;
  const float ROT_SCALE = 1.0f;

  float posMag = effectiveAmp * POS_SCALE * g_ShakePosWeight * s_ShakeMotionFactor;
  float rotMag = effectiveAmp * ROT_SCALE * g_ShakeRotWeight * s_ShakeMotionFactor;

  dx = nx * posMag;
  dy = ny * posMag;
  dz = nz * posMag;
  dPitch = np * rotMag;
  dYaw = nyw * rotMag;
  dRoll = nr * rotMag;
}

// ============================================================
//  Init / Destroy
// ============================================================

void InitFreeCamera() {
  if (g_FreeCamActive)
    return;

  // If we have a saved position and the user wants to remember it, reuse it
  if (!(g_RememberCamPosition && s_HasSavedPosition)) {
    // Grab current gameplay camera state as starting point
    Vector3 gcPos = CAM::GET_GAMEPLAY_CAM_COORD();
    Vector3 gcRot = CAM::GET_GAMEPLAY_CAM_ROT(2);

    s_PosX = gcPos.x;
    s_PosY = gcPos.y;
    s_PosZ = gcPos.z;
    s_Pitch = gcRot.x;
    s_Yaw = gcRot.z;
    g_CamRoll = 0.0f;
    g_CamFOV = CAM::GET_GAMEPLAY_CAM_FOV();

    // Initialize the Acro quaternion based on the Gameplay Cam Euler memory
    s_AcroMatrix = EulerToQuat(s_Pitch, s_Yaw, g_CamRoll);
  } else {
    // If remembering previous position, sync the AcroMatrix to our saved Eulers
    s_AcroMatrix = EulerToQuat(s_Pitch, s_Yaw, g_CamRoll);
  }

  // Create the scripted camera
  s_Cam = CAM::CREATE_CAM((char *)"DEFAULT_SCRIPTED_CAMERA", TRUE);
  CAM::SET_CAM_COORD(s_Cam, s_PosX, s_PosY, s_PosZ);
  CAM::SET_CAM_ROT(s_Cam, s_Pitch, g_CamRoll, s_Yaw, 2);
  CAM::SET_CAM_FOV(s_Cam, g_CamFOV);
  CAM::SET_CAM_ACTIVE(s_Cam, TRUE);
  CAM::RENDER_SCRIPT_CAMS(TRUE, FALSE, 0, TRUE, FALSE);

  // Freeze the player ped (unless player movement is enabled)
  Ped playerPed = PLAYER::PLAYER_PED_ID();
  s_FrozenPed = playerPed;
  // Remember where the player really is, so "stream around camera" can put them
  // back on exit instead of stranding them wherever the camera ended.
  Vector3 home = ENTITY::GET_ENTITY_COORDS(playerPed, TRUE);
  s_PlayerHomeX = home.x; s_PlayerHomeY = home.y; s_PlayerHomeZ = home.z;
  s_PlayerMoved = false;
  s_StreamActive = false;
  if (!g_EnablePlayerMovement) {
    ENTITY::FREEZE_ENTITY_POSITION(playerPed, TRUE);
    ENTITY::SET_ENTITY_COLLISION(playerPed, FALSE, FALSE);
  }

  // Auto-hide HUD and Player when entering freecam
  g_HideHUD = true;
  g_HidePlayer = true;

  // Fresh shake pattern for this session so the same preset doesn't
  // produce the exact same motion across multiple toggles.
  RandomizeShakePattern();
  s_HasPrevPos = false;
  s_ShakeMotionFactor = g_ShakeStopWhenStill ? 0.0f : 1.0f;

  g_FreeCamActive = true;
}

// Quick action — recall the flycam to just above the player ped. Handy when
// you've flown far across the map and want to get back to the action. Keeps
// the current look direction; only the position jumps.
void SnapCameraToPlayer() {
  if (!g_FreeCamActive)
    return;
  Ped ped = PLAYER::PLAYER_PED_ID();
  Vector3 p = ENTITY::GET_ENTITY_COORDS(ped, TRUE);
  // If stream-around-camera has parked the ped at the camera, its live coords
  // are useless — snap to the player's real (home) position instead.
  if (s_PlayerMoved) { p.x = s_PlayerHomeX; p.y = s_PlayerHomeY; p.z = s_PlayerHomeZ; }
  s_PosX = p.x;
  s_PosY = p.y;
  s_PosZ = p.z + 2.0f;
  // Don't let the big position jump spike the speed-coupled shake for a frame.
  s_HasPrevPos = false;
}

// Place the flycam at an explicit world pose. Used by "Teleport to Sequence" to
// jump straight to a sequence's first keyframe instead of flying across the map.
// Sets position, orientation and FOV; the quaternion (acrobatic) engine matrix
// is reseated so a roll-through-zenith session continues cleanly from here.
void FreeCam_SetPose(float x, float y, float z, float pitch, float yaw,
                     float roll, float fov) {
  if (!g_FreeCamActive)
    return;
  s_PosX = x;
  s_PosY = y;
  s_PosZ = z;
  s_Pitch = pitch;
  s_Yaw = yaw;
  g_CamRoll = roll;
  if (fov > 1.0f && fov < 130.0f)
    g_CamFOV = fov;
  s_AcroMatrix = EulerToQuat(s_Pitch, s_Yaw, g_CamRoll);
  // Don't let the big position jump spike the speed-coupled shake for a frame.
  s_HasPrevPos = false;
}

// Keep the world streaming/LOD centered on the camera rather than the player's
// real position. The strongest lever is the player ped itself (the streamer
// prioritizes around it), so when the ped is frozen and on foot we teleport it
// to the camera each frame (it's hidden, collision-off, so it's invisible and
// inert). When that's not possible (player driving / movement enabled) we fall
// back to the streaming focus + HD area at the camera. Called every frame while
// a scripted cam is up; paired with StreamFollowClear() on teardown.
static void StreamFollowClear(); // defined below

static void StreamFollowTick() {
  if (s_Cam == 0) return;
  // Only commandeer the player as a streaming anchor when it's HIDDEN (not in
  // the shot). The instant the player is unhidden — i.e. you're filming your own
  // character — leave it where it is so it stays in frame; the game already
  // streams detail around a visible player. Undo any prior anchoring once.
  bool want = g_StreamAroundCamera && g_HidePlayer;
  if (!want) {
    if (s_StreamActive) { StreamFollowClear(); s_StreamActive = false; }
    return;
  }
  s_StreamActive = true;
  // Primary anchor: when the player ped is frozen and ON FOOT, teleport it to
  // the camera so the world streams/LODs (and HD textures) around the shot. It's
  // hidden + collision-off, so it's invisible and inert. This is the most
  // accurate (it targets the exact camera position) and uses only SDK natives.
  Ped ped = s_FrozenPed;
  bool onFoot = ENTITY::DOES_ENTITY_EXIST(ped) && ENTITY::IS_ENTITY_A_PED(ped) &&
                PED::GET_VEHICLE_PED_IS_IN(ped, FALSE) == 0;
  if (!g_EnablePlayerMovement && onFoot) {
    if (s_FocusSet) { STREAMING::CLEAR_FOCUS(); s_FocusSet = false; } // switched modes
    ENTITY::SET_ENTITY_COORDS_NO_OFFSET(ped, s_PosX, s_PosY, s_PosZ, FALSE, FALSE,
                                        FALSE);
    s_PlayerMoved = true;
    return;
  }
  // Fallback when the ped can't be relocated (player sitting in a vehicle, which
  // is the common case while filming a recorded clip): point the streaming FOCUS
  // at the recorded-vehicle ghost — the subject of the shot — so detail streams
  // around it. SET_FOCUS_ENTITY is a real SDK native (unlike SET_FOCUS_POS_*).
  int subject = VehicleClip_VehicleHandle();
  if (subject != 0 && ENTITY::DOES_ENTITY_EXIST(subject)) {
    STREAMING::SET_FOCUS_ENTITY(subject);
    s_FocusSet = true;
  }
}

// Undo StreamFollowTick: clear focus/HD overrides and put the player back where
// they really were. Call on camera teardown (before the ped is unfrozen).
static void StreamFollowClear() {
  if (s_FocusSet) {
    STREAMING::CLEAR_FOCUS();
    s_FocusSet = false;
  }
  if (s_PlayerMoved && ENTITY::DOES_ENTITY_EXIST(s_FrozenPed)) {
    ENTITY::SET_ENTITY_COORDS_NO_OFFSET(s_FrozenPed, s_PlayerHomeX, s_PlayerHomeY,
                                        s_PlayerHomeZ, FALSE, FALSE, FALSE);
  }
  s_PlayerMoved = false;
  s_StreamActive = false;
}

// Quick action — zero the camera roll ("level the horizon"). Works in both the
// Euler and Acrobatic engines: s_Pitch / s_Yaw are kept live in both (the acro
// path writes them back via QuatToEuler each frame), so rebuilding the acro
// quaternion with roll = 0 levels the shot without changing pitch or heading.
void LevelCameraHorizon() {
  g_CamRoll = 0.0f;
  s_AcroMatrix = EulerToQuat(s_Pitch, s_Yaw, 0.0f);
}

// Freeze (or release) every dynamic entity in the world. SET_TIME_SCALE(0)
// alone leaves a bit of residual creep — physics settling, vehicles rolling
// to a stop, peds finishing a step. Pinning each entity's position with
// FREEZE_ENTITY_POSITION removes that, giving a near-total freeze while the
// camera and rendering stay live (unlike SET_GAME_PAUSED, which halts those
// too). The player ped is skipped — free-cam manages its own freeze state.
static void FreezeWorldEntities(bool freeze) {
  const int kMax = 1024;
  int pool[kMax];
  Ped player = PLAYER::PLAYER_PED_ID();

  int n = worldGetAllVehicles(pool, kMax);
  for (int i = 0; i < n; i++)
    if (ENTITY::DOES_ENTITY_EXIST(pool[i]))
      ENTITY::FREEZE_ENTITY_POSITION(pool[i], freeze);

  n = worldGetAllPeds(pool, kMax);
  for (int i = 0; i < n; i++)
    if (pool[i] != player && ENTITY::DOES_ENTITY_EXIST(pool[i]))
      ENTITY::FREEZE_ENTITY_POSITION(pool[i], freeze);

  n = worldGetAllObjects(pool, kMax);
  for (int i = 0; i < n; i++)
    if (ENTITY::DOES_ENTITY_EXIST(pool[i]))
      ENTITY::FREEZE_ENTITY_POSITION(pool[i], freeze);
}

void DestroyFreeCamera() {
  if (!g_FreeCamActive)
    return;

  // Stop any AI driving task before we restore/refreeze the player ped, so the
  // car isn't left mid-route with a frozen driver. Idempotent — safe to call
  // even when Auto Drive was never engaged.
  AutoDrive_Stop();

  // Stop streaming around the camera and put the player back at their real
  // position before unfreezing — otherwise they'd be stranded at the camera.
  StreamFollowClear();

  // Restore player
  if (ENTITY::DOES_ENTITY_EXIST(s_FrozenPed)) {
    ENTITY::FREEZE_ENTITY_POSITION(s_FrozenPed, FALSE);
    ENTITY::SET_ENTITY_COLLISION(s_FrozenPed, TRUE, FALSE);
  }

  // Auto-disable HUD/Player hiding when exiting freecam
  g_HideHUD = false;
  g_HidePlayer = false;

  // Detach from any follow target and clear the attach flags so a later
  // free-cam session re-attaches its fresh camera instead of assuming it's
  // already bound (the flags are file-scoped and persist otherwise).
  if (s_IsAttached && s_Cam != 0)
    invoke<Void>(0xA2FABBE87F4BAD82, s_Cam, FALSE); // DETACH_CAM
  s_IsAttached = false;
  s_AttachedEntity = 0;

  // Delete the quaternion-mode rotation anchor prop. Without this it leaks an
  // (invisible, frozen) object into the world every session, and its stale
  // non-zero handle would suppress re-creation/re-attach next time.
  if (s_RotationAnchor != 0) {
    if (ENTITY::DOES_ENTITY_EXIST(s_RotationAnchor))
      ENTITY::DELETE_ENTITY(&s_RotationAnchor);
    s_RotationAnchor = 0;
  }

  // Destroy script camera and restore gameplay camera
  CAM::RENDER_SCRIPT_CAMS(FALSE, FALSE, 0, TRUE, FALSE);
  CAM::SET_CAM_ACTIVE(s_Cam, FALSE);
  CAM::DESTROY_CAM(s_Cam, FALSE);
  s_Cam = 0;

  // Mark that we now have a saved position for next time
  s_HasSavedPosition = true;

  // Leaving freecam: lift any freeze/slow-mo so the world resumes normally.
  if (g_FreezeWorld) {
    GAMEPLAY::SET_GAME_PAUSED(FALSE);
    g_FreezeWorld = false;
  }
  if (g_FreezeEntities) {
    FreezeWorldEntities(false);
    g_FreezeEntities = false;
  }
  GAMEPLAY::SET_TIME_SCALE(1.0f);
  g_WorldTimeScale = 1.0f;

  // Reset lock/movement flags
  g_LockCamera = false;
  g_EnablePlayerMovement = false;
  g_CamCollision = false;

  g_FreeCamActive = false;
}

// ============================================================
//  Suspend / resume for "drive the car yourself" (vehicle clip recording)
// ============================================================

static bool s_DriveSuspended = false;
static bool s_DrivePrevHideHUD = false;
static bool s_DrivePrevHidePlayer = false;

void FreeCam_SuspendForDrive() {
  if (!g_FreeCamActive || s_DriveSuspended) return;
  s_DriveSuspended = true;

  // Hand the view back to the gameplay camera (the free-cam scripted cam is left
  // alive and untouched, so Resume snaps straight back to the same shot).
  CAM::SET_CAM_ACTIVE(s_Cam, FALSE);
  CAM::RENDER_SCRIPT_CAMS(FALSE, FALSE, 0, TRUE, FALSE);

  // Release the player ped so it can actually drive.
  if (ENTITY::DOES_ENTITY_EXIST(s_FrozenPed)) {
    ENTITY::FREEZE_ENTITY_POSITION(s_FrozenPed, FALSE);
    ENTITY::SET_ENTITY_COLLISION(s_FrozenPed, TRUE, TRUE);
  }

  // Show the player + HUD while driving (restored on resume).
  s_DrivePrevHideHUD = g_HideHUD;
  s_DrivePrevHidePlayer = g_HidePlayer;
  g_HideHUD = false;
  g_HidePlayer = false;
}

void FreeCam_ResumeAfterDrive() {
  if (!s_DriveSuspended) return;
  s_DriveSuspended = false;

  // Re-freeze the ped to match the free-cam contract (unless player movement is
  // explicitly allowed), then re-activate the scripted camera at its saved pose.
  if (!g_EnablePlayerMovement && ENTITY::DOES_ENTITY_EXIST(s_FrozenPed)) {
    ENTITY::FREEZE_ENTITY_POSITION(s_FrozenPed, TRUE);
    ENTITY::SET_ENTITY_COLLISION(s_FrozenPed, FALSE, FALSE);
  }
  if (s_Cam != 0) {
    CAM::SET_CAM_ACTIVE(s_Cam, TRUE);
    CAM::RENDER_SCRIPT_CAMS(TRUE, FALSE, 0, TRUE, FALSE);
  }
  g_HideHUD = s_DrivePrevHideHUD;
  g_HidePlayer = s_DrivePrevHidePlayer;
}

bool FreeCam_IsSuspended() { return s_DriveSuspended; }

// ============================================================
//  Info Overlay Drawing
// ============================================================

void DrawOverlayLine(const char *text, float y) {
  UI::SET_TEXT_FONT(0);
  UI::SET_TEXT_SCALE(0.0f, 0.30f);
  UI::SET_TEXT_COLOUR(220, 220, 220, 220);
  UI::SET_TEXT_CENTRE(0);
  UI::SET_TEXT_DROPSHADOW(2, 0, 0, 0, 200);
  UI::SET_TEXT_EDGE(1, 0, 0, 0, 200);
  UI::SET_TEXT_RIGHT_JUSTIFY(1);
  UI::SET_TEXT_WRAP(0.0f, 0.995f);
  UI::_SET_TEXT_ENTRY((char *)"STRING");
  UI::_ADD_TEXT_COMPONENT_STRING((LPSTR)text);
  UI::_DRAW_TEXT(0.995f, y);
}

void DrawInfoOverlay() {
  char buf[128];
  float y = 0.005f;
  float lineH = 0.022f;

  // Show camera position if freecam is active, otherwise show player position
  if (g_FreeCamActive) {
    snprintf(buf, sizeof(buf), "Pos: %.2f, %.2f, %.2f", s_PosX, s_PosY, s_PosZ);
  } else {
    Ped playerPed = PLAYER::PLAYER_PED_ID();
    Vector3 pPos = ENTITY::GET_ENTITY_COORDS(playerPed, TRUE);
    snprintf(buf, sizeof(buf), "Pos: %.2f, %.2f, %.2f", pPos.x, pPos.y, pPos.z);
  }
  DrawOverlayLine(buf, y);
  y += lineH;

  snprintf(buf, sizeof(buf), "Rot: P %.1f  Y %.1f  R %.1f", s_Pitch, s_Yaw,
           g_CamRoll);
  DrawOverlayLine(buf, y);
  y += lineH;

  snprintf(buf, sizeof(buf), "FOV: %.1f   Speed: %.1f", g_CamFOV, g_CamSpeed);
  DrawOverlayLine(buf, y);
  y += lineH;

  if (g_DoFEnabled) {
    snprintf(buf, sizeof(buf), "DoF: %.1f  Near: %.1f  Far: %.1f",
             g_DoFFocusDist, g_DoFMaxNearInFocus, g_DoFMaxFarInFocus);
    DrawOverlayLine(buf, y);
    y += lineH;
  }

  if (g_FreezeWorld) {
    DrawOverlayLine("~b~WORLD FROZEN", y);
    y += lineH;
  }
}

// ============================================================
//  Per-Frame Update
// ============================================================

void UpdateFreeCamera() {
  if (!g_FreeCamActive)
    return;

  // Use a real wall-clock delta when the world is frozen (GET_FRAME_TIME
  // returns 0) OR slowed (GET_FRAME_TIME is scaled by the time scale, which
  // would drag the camera down with the world). For cinematic slow-mo we want
  // the world slow but the camera still flying at full, consistent speed.
  static DWORD s_LastFrameTick = 0;
  DWORD now = GetTickCount();
  float dt;
  if (g_FreezeWorld || g_WorldTimeScale < 0.999f) {
    dt = (s_LastFrameTick > 0) ? (now - s_LastFrameTick) / 1000.0f : 0.016f;
  } else {
    dt = GAMEPLAY::GET_FRAME_TIME();
  }
  s_LastFrameTick = now;
  if (dt <= 0.0f || dt > 0.1f)
    dt = 0.016f;

  // --- Follow Entity Delta Tracking ---
  static Vector3 s_LastTargetPos = {0, 0, 0};
  static Vector3 s_LastTargetRot = {0, 0, 0};
  // s_IsAttached / s_AttachedEntity are now file-scoped (see top of file) so
  // teardown can clear them; s_LocalOffset stays local (recomputed on attach).
  static Vector3 s_LocalOffset = {0, 0, 0};

  int targetHandle = 0;

  if (g_FollowMode == 1) {
    targetHandle = PLAYER::PLAYER_PED_ID();
  } else if (g_FollowMode == 2 && g_FollowTargetEntity != 0) {
    targetHandle = g_FollowTargetEntity;
  }

  if (targetHandle != 0 && ENTITY::DOES_ENTITY_EXIST(targetHandle)) {
    Vector3 curTargetPos = ENTITY::GET_ENTITY_COORDS(targetHandle, TRUE);
    Vector3 curTargetRot = ENTITY::GET_ENTITY_ROTATION(targetHandle, 2);

    if (g_FollowRigidMode) {
      if (!s_IsAttached || s_AttachedEntity != targetHandle) {
        s_LocalOffset = invoke<Vector3>(
            0x2274BC1C4885E333, targetHandle, s_PosX, s_PosY,
            s_PosZ); // GET_OFFSET_FROM_ENTITY_GIVEN_WORLD_COORDS
        invoke<Void>(0xFEDB7D269E8C60E3, s_Cam, targetHandle, s_LocalOffset.x,
                     s_LocalOffset.y, s_LocalOffset.z,
                     TRUE); // ATTACH_CAM_TO_ENTITY
        s_IsAttached = true;
        s_AttachedEntity = targetHandle;
      }

      // Sync script variables to the current attached world position so flying
      // and collisions work natively
      Vector3 attachedPos = invoke<Vector3>(
          0x1899F328B0E12848, targetHandle, s_LocalOffset.x, s_LocalOffset.y,
          s_LocalOffset.z); // GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS
      s_PosX = attachedPos.x;
      s_PosY = attachedPos.y;
      s_PosZ = attachedPos.z;

      if (s_LastTargetRot.x != 0 || s_LastTargetRot.y != 0 ||
          s_LastTargetRot.z != 0) {
        float dPitch = curTargetRot.x - s_LastTargetRot.x;
        float dRoll = curTargetRot.y - s_LastTargetRot.y;
        float dYaw = curTargetRot.z - s_LastTargetRot.z;

        while (dPitch > 180.0f)
          dPitch -= 360.0f;
        while (dPitch <= -180.0f)
          dPitch += 360.0f;

        while (dRoll > 180.0f)
          dRoll -= 360.0f;
        while (dRoll <= -180.0f)
          dRoll += 360.0f;

        while (dYaw > 180.0f)
          dYaw -= 360.0f;
        while (dYaw <= -180.0f)
          dYaw += 360.0f;

        // Apply rotation deltas
        s_Pitch += dPitch;
        g_CamRoll += dRoll;
        s_Yaw += dYaw;
      }
    } else {
      if (s_IsAttached) {
        invoke<Void>(0xA2FABBE87F4BAD82, s_Cam, FALSE); // DETACH_CAM
        s_IsAttached = false;
        s_AttachedEntity = 0;
        CAM::SET_CAM_COORD(s_Cam, s_PosX, s_PosY, s_PosZ);
      }

      // If we already have a valid last pos, apply the delta
      if (s_LastTargetPos.x != 0 || s_LastTargetPos.y != 0 ||
          s_LastTargetPos.z != 0) {
        s_PosX += (curTargetPos.x - s_LastTargetPos.x);
        s_PosY += (curTargetPos.y - s_LastTargetPos.y);
        s_PosZ += (curTargetPos.z - s_LastTargetPos.z);
      }
    }

    s_LastTargetPos = curTargetPos;
    s_LastTargetRot = curTargetRot;

    if (g_ShowLockedEntityMarker && g_FollowMode == 2) {
      GRAPHICS::DRAW_MARKER(0, curTargetPos.x, curTargetPos.y,
                            curTargetPos.z + 1.25f, 0, 0, 0, 0, 0, 0, 0.4f,
                            0.4f, 0.4f, 0, 255, 0, 200, TRUE, TRUE, 2, FALSE,
                            NULL, NULL, FALSE);
    }
  } else {
    s_LastTargetPos = {0, 0, 0};
    s_LastTargetRot = {0, 0, 0};
    if (s_IsAttached) {
      invoke<Void>(0xA2FABBE87F4BAD82, s_Cam, FALSE); // DETACH_CAM
      s_IsAttached = false;
      s_AttachedEntity = 0;
      CAM::SET_CAM_COORD(s_Cam, s_PosX, s_PosY, s_PosZ);
    }
  }

  // Always disable character wheel (interferes with FOV D-pad controls)
  CONTROLS::DISABLE_CONTROL_ACTION(0, 19, TRUE);  // INPUT_CHARACTER_WHEEL
  CONTROLS::DISABLE_CONTROL_ACTION(0, 166, TRUE); // INPUT_SELECT_CHARACTER_MICHAEL
  CONTROLS::DISABLE_CONTROL_ACTION(0, 167, TRUE); // INPUT_SELECT_CHARACTER_FRANKLIN
  CONTROLS::DISABLE_CONTROL_ACTION(0, 168, TRUE); // INPUT_SELECT_CHARACTER_TREVOR
  CONTROLS::DISABLE_CONTROL_ACTION(0, 169, TRUE); // INPUT_SELECT_CHARACTER_MULTIPLAYER

  if (!g_EnablePlayerMovement) {

    // Player Movement & Actions
    CONTROLS::DISABLE_CONTROL_ACTION(0, 30, TRUE); // INPUT_MOVE_LR
    CONTROLS::DISABLE_CONTROL_ACTION(0, 31, TRUE); // INPUT_MOVE_UD
    CONTROLS::DISABLE_CONTROL_ACTION(0, 32, TRUE); // INPUT_MOVE_UP_ONLY
    CONTROLS::DISABLE_CONTROL_ACTION(0, 33, TRUE); // INPUT_MOVE_DOWN_ONLY
    CONTROLS::DISABLE_CONTROL_ACTION(0, 34, TRUE); // INPUT_MOVE_LEFT_ONLY
    CONTROLS::DISABLE_CONTROL_ACTION(0, 35, TRUE); // INPUT_MOVE_RIGHT_ONLY
    CONTROLS::DISABLE_CONTROL_ACTION(0, 21, TRUE); // INPUT_SPRINT
    CONTROLS::DISABLE_CONTROL_ACTION(0, 22, TRUE); // INPUT_JUMP
    CONTROLS::DISABLE_CONTROL_ACTION(0, 36, TRUE); // INPUT_DUCK

    // Look / Aim
    CONTROLS::DISABLE_CONTROL_ACTION(0, 1, TRUE);  // INPUT_LOOK_LR
    CONTROLS::DISABLE_CONTROL_ACTION(0, 2, TRUE);  // INPUT_LOOK_UD
    CONTROLS::DISABLE_CONTROL_ACTION(0, 25, TRUE); // INPUT_AIM
    CONTROLS::DISABLE_CONTROL_ACTION(0, 26, TRUE); // INPUT_LOOK_BEHIND

    // Phone (prevent scroll/up from opening phone)
    CONTROLS::DISABLE_CONTROL_ACTION(0, 27, TRUE);  // INPUT_PHONE
    CONTROLS::DISABLE_CONTROL_ACTION(0, 172, TRUE); // INPUT_PHONE_UP
    CONTROLS::DISABLE_CONTROL_ACTION(0, 173, TRUE); // INPUT_PHONE_DOWN
    CONTROLS::DISABLE_CONTROL_ACTION(0, 174, TRUE); // INPUT_PHONE_LEFT
    CONTROLS::DISABLE_CONTROL_ACTION(0, 175, TRUE); // INPUT_PHONE_RIGHT

    // Combat & Weapons
    CONTROLS::DISABLE_CONTROL_ACTION(0, 24, TRUE);  // INPUT_ATTACK
    CONTROLS::DISABLE_CONTROL_ACTION(0, 257, TRUE); // INPUT_ATTACK2
    CONTROLS::DISABLE_CONTROL_ACTION(0, 140, TRUE); // INPUT_MELEE_ATTACK_LIGHT
    CONTROLS::DISABLE_CONTROL_ACTION(0, 141, TRUE); // INPUT_MELEE_ATTACK_HEAVY
    CONTROLS::DISABLE_CONTROL_ACTION(0, 142,
                                     TRUE); // INPUT_MELEE_ATTACK_ALTERNATE
    CONTROLS::DISABLE_CONTROL_ACTION(0, 143, TRUE); // INPUT_MELEE_BLOCK
    CONTROLS::DISABLE_CONTROL_ACTION(0, 37, TRUE);  // INPUT_SELECT_WEAPON
    CONTROLS::DISABLE_CONTROL_ACTION(0, 14, TRUE);  // INPUT_WEAPON_WHEEL_NEXT
    CONTROLS::DISABLE_CONTROL_ACTION(0, 15, TRUE);  // INPUT_WEAPON_WHEEL_PREV
    CONTROLS::DISABLE_CONTROL_ACTION(0, 45, TRUE);  // INPUT_RELOAD
    CONTROLS::DISABLE_CONTROL_ACTION(0, 44, TRUE);  // INPUT_COVER
    CONTROLS::DISABLE_CONTROL_ACTION(0, 114, TRUE); // INPUT_VEH_FLY_ATTACK

    // Vehicle Driving
    CONTROLS::DISABLE_CONTROL_ACTION(0, 59, TRUE); // INPUT_VEH_MOVE_LR
    CONTROLS::DISABLE_CONTROL_ACTION(0, 60, TRUE); // INPUT_VEH_MOVE_UD
    CONTROLS::DISABLE_CONTROL_ACTION(0, 71, TRUE); // INPUT_VEH_ACCELERATE
    CONTROLS::DISABLE_CONTROL_ACTION(0, 72, TRUE); // INPUT_VEH_BRAKE
    CONTROLS::DISABLE_CONTROL_ACTION(0, 76, TRUE); // INPUT_VEH_HANDBRAKE
    CONTROLS::DISABLE_CONTROL_ACTION(0, 86, TRUE); // INPUT_VEH_HORN
    CONTROLS::DISABLE_CONTROL_ACTION(0, 68, TRUE); // INPUT_VEH_AIM
    CONTROLS::DISABLE_CONTROL_ACTION(0, 69, TRUE); // INPUT_VEH_ATTACK
    CONTROLS::DISABLE_CONTROL_ACTION(0, 70, TRUE); // INPUT_VEH_ATTACK2
    CONTROLS::DISABLE_CONTROL_ACTION(0, 79, TRUE); // INPUT_VEH_LOOK_BEHIND
  }

  // ---- User input (disabled when IGCS has camera locked or camera is locked)
  // ----
  if (!IGCS_IsSessionActive() && !g_LockCamera) {

    // ---- Rotation (Mouse + Right Stick) ----

    // Mouse: raw axis input (-1..1 per frame, usually small)
    float mouseX = PAD::GET_DISABLED_CONTROL_NORMAL(0, INPUT_LOOK_LR);
    float mouseY = PAD::GET_DISABLED_CONTROL_NORMAL(0, INPUT_LOOK_UD);

    float rotSpeed = g_CamSensitivity * 4.0f;

    if (g_DroneMode) {
      // Drone: smooth rotation via angular velocity interpolation
      float targetYawRate = -mouseX * rotSpeed * 60.0f; // degrees/sec
      float targetPitchRate = -mouseY * rotSpeed * 60.0f;
      float smoothFactor = 1.0f - expf(-g_DroneRotSmoothing * dt);
      s_DroneYawRate += (targetYawRate - s_DroneYawRate) * smoothFactor;
      s_DronePitchRate += (targetPitchRate - s_DronePitchRate) * smoothFactor;

      if (!g_RotationEngine) {
        s_Yaw += s_DroneYawRate * dt;
        s_Pitch += s_DronePitchRate * dt;
      }
    } else {
      if (!g_RotationEngine) {
        s_Yaw -= mouseX * rotSpeed;
        s_Pitch -= mouseY * rotSpeed;
      }
    }

    // Protect Euler angles from Gimbal Lock if Quaternion Engine is OFF
    if (!g_RotationEngine) {
      if (s_Pitch > 89.0f)
        s_Pitch = 89.0f;
      if (s_Pitch < -89.0f)
        s_Pitch = -89.0f;
    }

    // ---- Roll (Q/E keys, LB/RB controller) ----

    float rollDelta = 0.0f;
    if (IsKeyDown(0x51))
      rollDelta -= 45.0f * dt; // Q
    if (IsKeyDown(0x45))
      rollDelta += 45.0f * dt; // E

    // Controller bumpers
    if (PAD::IS_DISABLED_CONTROL_PRESSED(0, INPUT_FRONTEND_LB))
      rollDelta -= 45.0f * dt;
    if (PAD::IS_DISABLED_CONTROL_PRESSED(0, INPUT_FRONTEND_RB))
      rollDelta += 45.0f * dt;

    rollDelta *= g_RollSpeed;

    if (g_DroneMode && !g_RotationEngine) {
      // Auto-banking: camera tilts into yaw turns
      s_DroneTargetRoll = -s_DroneYawRate * (g_DroneBanking / 60.0f);
      // Add manual roll on top
      g_CamRoll += rollDelta;
      // Blend toward auto-bank target
      float bankBlend = 1.0f - expf(-4.0f * dt);
      g_CamRoll += (s_DroneTargetRoll - g_CamRoll) * bankBlend;
    } else {
      if (!g_RotationEngine) {
        g_CamRoll += rollDelta;
      }
    }

    // Euler Angle Roll Wrapping
    if (!g_RotationEngine) {
      if (g_CamRoll > 180.0f)
        g_CamRoll -= 360.0f;
      if (g_CamRoll < -180.0f)
        g_CamRoll += 360.0f;
    }

    // ---- Quaternion Processing ----
    if (g_RotationEngine) {
      // Calculate angular deltas
      float pitchDelta =
          g_DroneMode ? (s_DronePitchRate * dt) : (-mouseY * rotSpeed);
      float yawDelta =
          g_DroneMode ? (s_DroneYawRate * dt) : (-mouseX * rotSpeed);

      float finalRollDelta = rollDelta;

      if (g_DroneMode) {
        // Drone Roll Smoothing
        float targetRollRate = rollDelta * 60.0f; // Scale input per sec
        float smoothFactor = 1.0f - expf(-g_DroneRotSmoothing * dt);
        s_DroneRollRate += (targetRollRate - s_DroneRollRate) * smoothFactor;
        finalRollDelta = s_DroneRollRate * dt;
      }

      // Apply relative rotations to the Quaternion Matrix sequentially (Yaw,
      // Pitch, Roll)
      Quat qYaw = AxisAngleToQuat(0, 0, 1, yawDelta * DEG2RAD);
      // We pitch relative to our current matrix
      Quat qPitch = AxisAngleToQuat(1, 0, 0, pitchDelta * DEG2RAD);
      Quat qRoll = AxisAngleToQuat(
          0, 1, 0,
          finalRollDelta * DEG2RAD); // Roll around locally transformed Y

      // Update the mastery tracking Quat
      s_AcroMatrix = MultiplyQuat(s_AcroMatrix, qYaw);
      s_AcroMatrix = MultiplyQuat(s_AcroMatrix, qPitch);
      s_AcroMatrix = MultiplyQuat(s_AcroMatrix, qRoll);

      // Prevent Floating-Point Dimensional Collapse over Time
      s_AcroMatrix = NormalizeQuat(s_AcroMatrix);

      // Unpack updated Quat back to Euler angles strictly to feed the GTA
      // Engine
      QuatToEuler(s_AcroMatrix, s_Pitch, s_Yaw, g_CamRoll);
    } else {
      // Keep Quat synchronized with Euler for seamless transitioning back to
      // Acro
      s_AcroMatrix = EulerToQuat(s_Pitch, s_Yaw, g_CamRoll);
    }

    // ---- FOV / Zoom (Mouse Wheel: +/- keys, D-Pad up/down) ----

    float zoomRate = g_ZoomSpeed * 30.0f; // 1.0 = 30 deg/sec (original speed)

    // Desired zoom rate from input (deg/sec): negative = zoom in, positive = zoom out
    float targetFovRate = 0.0f;

    // Keyboard zoom
    if (IsKeyDown(VK_OEM_PLUS) || IsKeyDown(VK_ADD))
      targetFovRate -= zoomRate;
    if (IsKeyDown(VK_OEM_MINUS) || IsKeyDown(VK_SUBTRACT))
      targetFovRate += zoomRate;

    // Mouse scroll for speed (241 = scroll up, 242 = scroll down, mouse-only)
    if (PAD::IS_DISABLED_CONTROL_PRESSED(0, 241)) {
      g_CamSpeed += 10.0f * dt;
      if (g_CamSpeed > 20.0f)
        g_CamSpeed = 20.0f;
    }
    if (PAD::IS_DISABLED_CONTROL_PRESSED(0, 242)) {
      g_CamSpeed -= 10.0f * dt;
      if (g_CamSpeed < 0.1f)
        g_CamSpeed = 0.1f;
    }

    // Controller D-pad up/down for zoom — but NOT while a menu is open, since
    // the D-Pad is the menu's navigation there and would otherwise zoom the
    // FOV every time you move the cursor.
    if (!g_MenuOpen) {
      if (PAD::IS_DISABLED_CONTROL_PRESSED(0, INPUT_FRONTEND_UP))
        targetFovRate -= zoomRate;
      if (PAD::IS_DISABLED_CONTROL_PRESSED(0, INPUT_FRONTEND_DOWN))
        targetFovRate += zoomRate;
    }

    // Apply FOV — velocity-based inertia in drone mode, instant in standard
    static float s_DroneFovRate = 0.0f;
    if (g_DroneMode && g_DroneFovSmoothing > 0.0f) {
      // Smooth the zoom velocity (same pattern as drone rotation smoothing)
      float smoothFactor = 1.0f - expf(-g_DroneFovSmoothing * dt);
      s_DroneFovRate += (targetFovRate - s_DroneFovRate) * smoothFactor;
      g_CamFOV += s_DroneFovRate * dt;
    } else {
      g_CamFOV += targetFovRate * dt;
      s_DroneFovRate = 0.0f;
    }

    // Clamp FOV
    if (g_CamFOV < 5.0f)
      g_CamFOV = 5.0f;
    if (g_CamFOV > 130.0f)
      g_CamFOV = 130.0f;

    // ---- Translation (WASD + Space/Ctrl + Left stick + LT/RT) ----

    float moveForward = 0.0f;
    float moveRight = 0.0f;
    float moveUp = 0.0f;

    float stickX = PAD::GET_DISABLED_CONTROL_NORMAL(0, INPUT_MOVE_LR);
    float stickY = PAD::GET_DISABLED_CONTROL_NORMAL(0, INPUT_MOVE_UD);

    moveRight += stickX;
    moveForward -= stickY; // stick Y is inverted

    // Controller triggers for vertical movement
    float triggerRT = PAD::GET_DISABLED_CONTROL_NORMAL(0, INPUT_ATTACK);
    float triggerLT = PAD::GET_DISABLED_CONTROL_NORMAL(0, INPUT_AIM);

    if (moveUp == 0.0f) {
      moveUp += triggerRT;
      moveUp -= triggerLT;
    }

    // Up/Down Keyboard (Space/Ctrl)
    if (IsKeyDown(VK_SPACE))
      moveUp += 1.0f;
    if (IsKeyDown(VK_CONTROL))
      moveUp -= 1.0f;

    // Left Shift = speed boost (turbo); Alt = precision (slow). Ctrl is taken
    // by move-down, so Alt is the free modifier for fine framing moves.
    float speedMult = 1.0f;
    if (IsKeyDown(VK_SHIFT))
      speedMult = 3.0f;
    else if (IsKeyDown(VK_MENU))
      speedMult = 0.25f;

    // Normalize movement vector so diagonal flight isn't mathematically faster
    float moveLen = sqrtf(moveForward * moveForward + moveRight * moveRight +
                          moveUp * moveUp);
    if (moveLen > 1.0f) {
      moveForward /= moveLen;
      moveRight /= moveLen;
      moveUp /= moveLen;
    }

    // Calculate world-space displacement
    float fwdX, fwdY, fwdZ;
    float rightX, rightY, rightZ;
    float upX, upY, upZ;

    if (g_RotationEngine) {
      // True 6-DOF Movement Array (Extracted directly from the Quaternion
      // Matrix)
      Quat q = s_AcroMatrix;

      // Local Forward Vector (Rotated +Y Axis)
      fwdX = 2.0f * (q.x * q.y - q.w * q.z);
      fwdY = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
      fwdZ = 2.0f * (q.y * q.z + q.w * q.x);

      // Local Right Vector (Rotated +X Axis)
      rightX = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
      rightY = 2.0f * (q.x * q.y + q.w * q.z);
      rightZ = 2.0f * (q.x * q.z - q.w * q.y);

      // Local Up Vector (Rotated +Z Axis)
      upX = 2.0f * (q.x * q.z + q.w * q.y);
      upY = 2.0f * (q.y * q.z - q.w * q.x);
      upZ = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    } else {
      // Basic 2.5D Eulerian Movement Array
      float yawRad = s_Yaw * DEG2RAD;
      float pitchRad = s_Pitch * DEG2RAD;

      float cosYaw = cosf(yawRad);
      float sinYaw = sinf(yawRad);
      float cosPitch = cosf(pitchRad);
      float sinPitch = sinf(pitchRad);

      // Forward vector (direction camera is looking)
      fwdX = -sinYaw * cosPitch;
      fwdY = cosYaw * cosPitch;
      fwdZ = sinPitch;

      // Right vector (perpendicular to forward on XY plane)
      rightX = cosYaw;
      rightY = sinYaw;
      rightZ = 0.0f;

      // Up is always world up for simplicity (no roll-based movement natively)
      upX = 0.0f;
      upY = 0.0f;
      upZ = 1.0f;
    }

    // Lock Height: Flatten the movement direction vectors to purely horizontal
    if (g_LockHeight) {
      fwdZ = 0.0f;
      float fwdLen = sqrtf(fwdX * fwdX + fwdY * fwdY);
      if (fwdLen > 0.001f) {
        fwdX /= fwdLen;
        fwdY /= fwdLen;
      }

      rightZ = 0.0f;
      float rightLen = sqrtf(rightX * rightX + rightY * rightY);
      if (rightLen > 0.001f) {
        rightX /= rightLen;
        rightY /= rightLen;
      }
    }

    if (g_DroneMode) {

      float accel = g_DroneAcceleration * g_CamSpeed * speedMult * 2.0f;

      // Acceleration from input in world space
      float accelX =
          (fwdX * moveForward + rightX * moveRight + upX * moveUp) * accel;
      float accelY =
          (fwdY * moveForward + rightY * moveRight + upY * moveUp) * accel;
      float accelZ =
          (fwdZ * moveForward + rightZ * moveRight + upZ * moveUp) * accel;

      // Apply acceleration
      s_DroneVelX += accelX * dt;
      s_DroneVelY += accelY * dt;
      s_DroneVelZ += accelZ * dt;

      // Apply gravity (only when not actively thrusting upward)
      s_DroneVelZ -= g_DroneGravity * dt;

      // Apply drag (exponential decay for smooth deceleration)
      float dragFactor = expf(-g_DroneDrag * dt);
      s_DroneVelX *= dragFactor;
      s_DroneVelY *= dragFactor;
      s_DroneVelZ *= dragFactor;

      // Update position from velocity
      float moveX = s_DroneVelX * dt;
      float moveY = s_DroneVelY * dt;
      float moveZ = s_DroneVelZ * dt;

      float newX = s_PosX + moveX;
      float newY = s_PosY + moveY;
      float newZ = s_PosZ + moveZ;

      if (g_CamCollision) {
        float radius = 0.5f;
        int iterations = 3;

        while (iterations-- > 0) {
          float moveDist = sqrt(moveX * moveX + moveY * moveY + moveZ * moveZ);
          if (moveDist < 0.001f)
            break; // Reached destination

          int handle = invoke<int>(0x28579D1B8F8AAC80, s_PosX, s_PosY, s_PosZ,
                                   s_PosX + moveX, s_PosY + moveY,
                                   s_PosZ + moveZ, radius, -1, s_FrozenPed, 7);
          int hit = 0;
          Vector3 hitCoords, surfNormal;
          int entHit = 0;
          invoke<int>(0x3D87450E15D98694, handle, &hit, &hitCoords, &surfNormal,
                      &entHit);

          if (!hit || (surfNormal.x == 0.0f && surfNormal.y == 0.0f &&
                       surfNormal.z == 0.0f)) {
            s_PosX += moveX;
            s_PosY += moveY;
            s_PosZ += moveZ;
            break;
          }

          // Push the safe starting position out of the hit plane strictly
          float distA = (s_PosX - hitCoords.x) * surfNormal.x +
                        (s_PosY - hitCoords.y) * surfNormal.y +
                        (s_PosZ - hitCoords.z) * surfNormal.z;
          if (distA < radius) {
            float pushOutA = radius - distA + 0.0001f;
            s_PosX += surfNormal.x * pushOutA;
            s_PosY += surfNormal.y * pushOutA;
            s_PosZ += surfNormal.z * pushOutA;
          }

          // Push the intended destination out of the hit plane
          float tgtX = s_PosX + moveX;
          float tgtY = s_PosY + moveY;
          float tgtZ = s_PosZ + moveZ;
          float distToPlane = (tgtX - hitCoords.x) * surfNormal.x +
                              (tgtY - hitCoords.y) * surfNormal.y +
                              (tgtZ - hitCoords.z) * surfNormal.z;
          if (distToPlane < radius) {
            float pushOut = radius - distToPlane + 0.0001f;
            tgtX += surfNormal.x * pushOut;
            tgtY += surfNormal.y * pushOut;
            tgtZ += surfNormal.z * pushOut;
          }

          // Re-calculate the remaining movement vector
          moveX = tgtX - s_PosX;
          moveY = tgtY - s_PosY;
          moveZ = tgtZ - s_PosZ;

          // Dampen drone velocity into the wall
          float dotVel = s_DroneVelX * surfNormal.x +
                         s_DroneVelY * surfNormal.y +
                         s_DroneVelZ * surfNormal.z;
          if (dotVel < 0) {
            s_DroneVelX -= dotVel * surfNormal.x;
            s_DroneVelY -= dotVel * surfNormal.y;
            s_DroneVelZ -= dotVel * surfNormal.z;
          }
        }

        newX = s_PosX;
        newY = s_PosY;
        newZ = s_PosZ;
      }
      s_PosX = newX;
      s_PosY = newY;
      s_PosZ = newZ;
    } else {

      float speed = g_CamSpeed * speedMult * dt * 40.0f;
      float moveX =
          (fwdX * moveForward + rightX * moveRight + upX * moveUp) * speed;
      float moveY =
          (fwdY * moveForward + rightY * moveRight + upY * moveUp) * speed;
      float moveZ =
          (fwdZ * moveForward + rightZ * moveRight + upZ * moveUp) * speed;

      float newX = s_PosX + moveX;
      float newY = s_PosY + moveY;
      float newZ = s_PosZ + moveZ;

      if (g_CamCollision) {
        float radius = 0.5f;
        int iterations = 3;

        while (iterations-- > 0) {
          float moveDist = sqrt(moveX * moveX + moveY * moveY + moveZ * moveZ);
          if (moveDist < 0.001f)
            break; // Reached destination

          int handle = invoke<int>(0x28579D1B8F8AAC80, s_PosX, s_PosY, s_PosZ,
                                   s_PosX + moveX, s_PosY + moveY,
                                   s_PosZ + moveZ, radius, -1, s_FrozenPed, 7);
          int hit = 0;
          Vector3 hitCoords, surfNormal;
          int entHit = 0;
          invoke<int>(0x3D87450E15D98694, handle, &hit, &hitCoords, &surfNormal,
                      &entHit);

          if (!hit || (surfNormal.x == 0.0f && surfNormal.y == 0.0f &&
                       surfNormal.z == 0.0f)) {
            s_PosX += moveX;
            s_PosY += moveY;
            s_PosZ += moveZ;
            break;
          }

          // Push the safe starting position out of the hit plane strictly
          float distA = (s_PosX - hitCoords.x) * surfNormal.x +
                        (s_PosY - hitCoords.y) * surfNormal.y +
                        (s_PosZ - hitCoords.z) * surfNormal.z;
          if (distA < radius) {
            float pushOutA = radius - distA + 0.0001f;
            s_PosX += surfNormal.x * pushOutA;
            s_PosY += surfNormal.y * pushOutA;
            s_PosZ += surfNormal.z * pushOutA;
          }

          // Push the intended destination out of the hit plane
          float tgtX = s_PosX + moveX;
          float tgtY = s_PosY + moveY;
          float tgtZ = s_PosZ + moveZ;
          float distToPlane = (tgtX - hitCoords.x) * surfNormal.x +
                              (tgtY - hitCoords.y) * surfNormal.y +
                              (tgtZ - hitCoords.z) * surfNormal.z;
          if (distToPlane < radius) {
            float pushOut = radius - distToPlane + 0.0001f;
            tgtX += surfNormal.x * pushOut;
            tgtY += surfNormal.y * pushOut;
            tgtZ += surfNormal.z * pushOut;
          }

          // Re-calculate the remaining movement vector
          moveX = tgtX - s_PosX;
          moveY = tgtY - s_PosY;
          moveZ = tgtZ - s_PosZ;
        }

        newX = s_PosX;
        newY = s_PosY;
        newZ = s_PosZ;
      }
      s_PosX = newX;
      s_PosY = newY;
      s_PosZ = newZ;
    }

    // ---- Walk Mode: snap to ground at a fixed eye height ----
    // Raycast straight down from well above the current XY to find ground,
    // then place the camera at groundZ + g_WalkHeight. Done after all
    // movement/collision so it overrides drone gravity and lock-altitude
    // produces a true ground-follow behaviour rather than a flat plane.
    if (g_WalkMode) {
      float groundZ = 0.0f;
      // Start probe well above the current camera position so the ray clears
      // any roof / overhang the camera might have flown into. 200m is enough
      // for tall GTA structures.
      if (GAMEPLAY::GET_GROUND_Z_FOR_3D_COORD(s_PosX, s_PosY,
                                              s_PosZ + 200.0f, &groundZ,
                                              FALSE)) {
        s_PosZ = groundZ + g_WalkHeight;
        // Kill drone Z velocity so gravity can't fight the snap next frame.
        s_DroneVelZ = 0.0f;
      }
    }

  } // end IGCS session guard

  // Check if the script updated s_PosX/Y/Z manually and sync the attachment
  // offset if so
  if (s_IsAttached && targetHandle != 0) {
    Vector3 attachedPos =
        invoke<Vector3>(0x1899F328B0E12848, targetHandle, s_LocalOffset.x,
                        s_LocalOffset.y, s_LocalOffset.z);
    if (abs(s_PosX - attachedPos.x) > 0.001f ||
        abs(s_PosY - attachedPos.y) > 0.001f ||
        abs(s_PosZ - attachedPos.z) > 0.001f) {
      s_LocalOffset = invoke<Vector3>(0x2274BC1C4885E333, targetHandle, s_PosX,
                                      s_PosY, s_PosZ);
      invoke<Void>(0xFEDB7D269E8C60E3, s_Cam, targetHandle, s_LocalOffset.x,
                   s_LocalOffset.y, s_LocalOffset.z, TRUE);
    }
  }

  // ---- Procedural shake ----
  // Computed once per frame from the (pre-shake) position, then added at
  // every SET_CAM_COORD / SET_CAM_ROT site below so IGCS offsets and shake
  // compose cleanly.
  float shakeDx, shakeDy, shakeDz, shakePitchD, shakeYawD, shakeRollD;
  ComputeShakeOffsets(dt, shakeDx, shakeDy, shakeDz, shakePitchD, shakeYawD,
                      shakeRollD);

  // Snapshot non-shaken position for next-frame speed estimation
  s_PrevPosX = s_PosX;
  s_PrevPosY = s_PosY;
  s_PrevPosZ = s_PosZ;
  s_HasPrevPos = true;

  // ---- Apply to camera ----

  if (g_RotationEngine && !s_IsAttached && !IGCS_IsSessionActive()) {
    // Quaternion Entity Anchor Architecture
    if (s_RotationAnchor == 0) {
      Hash anchorModel = 0x19B81B74; // "prop_paper_bag"

      // Asynchronously request the model into memory
      if (!STREAMING::HAS_MODEL_LOADED(anchorModel)) {
        STREAMING::REQUEST_MODEL(anchorModel);
      }

      if (STREAMING::HAS_MODEL_LOADED(anchorModel)) {
        s_RotationAnchor = OBJECT::CREATE_OBJECT_NO_OFFSET(
            anchorModel, s_PosX, s_PosY, s_PosZ, FALSE, FALSE, FALSE);
        if (s_RotationAnchor != 0) {
          ENTITY::SET_ENTITY_VISIBLE(s_RotationAnchor, FALSE, FALSE);
          ENTITY::FREEZE_ENTITY_POSITION(s_RotationAnchor, TRUE);
          ENTITY::SET_ENTITY_COLLISION(s_RotationAnchor, FALSE, FALSE);
          CAM::ATTACH_CAM_TO_ENTITY(s_Cam, s_RotationAnchor, 0.0f, 0.0f, 0.0f,
                                    TRUE);
        }
      }
    }

    if (s_RotationAnchor != 0) {
      // Teleport the invisible anchor to the calculated camera flight
      // coordinates (plus IGCS offset if any, plus shake)
      ENTITY::SET_ENTITY_COORDS_NO_OFFSET(
          s_RotationAnchor, s_PosX + s_IgcsOffsetX + shakeDx,
          s_PosY + s_IgcsOffsetY + shakeDy,
          s_PosZ + s_IgcsOffsetZ + shakeDz, FALSE, FALSE, FALSE);

      // Natively apply the raw Quaternion Matrix to the Anchor (bypassing
      // SET_CAM_ROT's Euler limits natively)
      invoke<Void>(0x77B21BE7AC540F07, s_RotationAnchor, s_AcroMatrix.x,
                   s_AcroMatrix.y, s_AcroMatrix.z, s_AcroMatrix.w);

      // Strip any residual Euler drift from the camera itself so it identically
      // inherits the Anchor's exact rotation (plus shake)
      CAM::SET_CAM_ROT(s_Cam, 0.0f + s_IgcsPitchOffset + shakePitchD,
                       0.0f + s_IgcsRollOffset + shakeRollD,
                       0.0f + s_IgcsYawOffset + shakeYawD, 2);
    } else {
      // Model is still streaming in, temporarily fallback to Basic Eulerian
      // rendering to prevent rendering/movement freezing
      CAM::SET_CAM_COORD(s_Cam, s_PosX + s_IgcsOffsetX + shakeDx,
                         s_PosY + s_IgcsOffsetY + shakeDy,
                         s_PosZ + s_IgcsOffsetZ + shakeDz);
      CAM::SET_CAM_ROT(s_Cam, s_Pitch + s_IgcsPitchOffset + shakePitchD,
                       g_CamRoll + s_IgcsRollOffset + shakeRollD,
                       s_Yaw + s_IgcsYawOffset + shakeYawD, 2);
    }
  } else {
    // Standard Eulerian Flight (or Rigid Mode)
    if (s_RotationAnchor != 0) {
      if (!s_IsAttached || IGCS_IsSessionActive()) {
        // Only detach if Rigid Mode didn't recently hijack the attachment
        // Or temporarily detach if IGCS is running to bypass forced local
        // offsets
        CAM::DETACH_CAM(s_Cam);
      }
      if (ENTITY::DOES_ENTITY_EXIST(s_RotationAnchor)) {
        ENTITY::DELETE_ENTITY(&s_RotationAnchor);
      }
      s_RotationAnchor = 0;
    }

    static bool s_IgcsTemporarilyOffset = false;

    if (IGCS_IsSessionActive()) {
      if (s_IsAttached) {
        // Translate the camera's intended world position into a temporary local offset for the entity
        Vector3 tempLocalOffset = invoke<Vector3>(0x2274BC1C4885E333, targetHandle,
                                        s_PosX + s_IgcsOffsetX + shakeDx,
                                        s_PosY + s_IgcsOffsetY + shakeDy,
                                        s_PosZ + s_IgcsOffsetZ + shakeDz);
        // Force the engine to hold the camera natively on the vehicle at this specific DoF pixel offset
        invoke<Void>(0xFEDB7D269E8C60E3, s_Cam, targetHandle, tempLocalOffset.x,
                     tempLocalOffset.y, tempLocalOffset.z, TRUE);
        s_IgcsTemporarilyOffset = true;
      } else {
        CAM::SET_CAM_COORD(s_Cam, s_PosX + s_IgcsOffsetX + shakeDx,
                           s_PosY + s_IgcsOffsetY + shakeDy,
                           s_PosZ + s_IgcsOffsetZ + shakeDz);
      }
    } else {
      if (s_IsAttached && s_IgcsTemporarilyOffset) {
        // Session ended! Revert the ATTACH offset back to the clean s_LocalOffset tracking lock.
        invoke<Void>(0xFEDB7D269E8C60E3, s_Cam, targetHandle, s_LocalOffset.x,
                     s_LocalOffset.y, s_LocalOffset.z, TRUE); // ATTACH_CAM_TO_ENTITY
        s_IgcsTemporarilyOffset = false;
      } else if (!s_IsAttached) {
        CAM::SET_CAM_COORD(s_Cam, s_PosX + shakeDx, s_PosY + shakeDy,
                           s_PosZ + shakeDz);
      }
    }

    // Apply Standard Native Euler tracking (Allows Gimbal clipping limits
    // natively)
    CAM::SET_CAM_ROT(s_Cam, s_Pitch + s_IgcsPitchOffset + shakePitchD,
                     g_CamRoll + s_IgcsRollOffset + shakeRollD,
                     s_Yaw + s_IgcsYawOffset + shakeYawD, 2);
  }

  // Set FOV (real-time) via hash because SDK version is mismatched
  invoke<Void>(0xB13C14F66A00D047, s_Cam, g_CamFOV);

  // ---- Depth of Field ----

  if (g_DoFEnabled) {
    CAM::SET_USE_HI_DOF();

    if (g_DoFAutofocus) {
      // Calculate forward vector from yaw/pitch
      float yawRad = s_Yaw * DEG2RAD;
      float pitchRad = s_Pitch * DEG2RAD;

      float dirX = -sin(yawRad) * cos(pitchRad);
      float dirY = cos(yawRad) * cos(pitchRad);
      float dirZ = sin(pitchRad);

      // Raycast end coordinates
      float endX = s_PosX + dirX * 1000.0f;
      float endY = s_PosY + dirY * 1000.0f;
      float endZ = s_PosZ + dirZ * 1000.0f;

      // START_EXPENSIVE_SYNCHRONOUS_SHAPE_TEST_LOS_PROBE (flags: -1 = intersect
      // everything, p8 = 7). Ignore the frozen ped only while it's ON FOOT
      // (it can be parked at the camera by Stream Around Camera and would
      // self-hit) — the ignore extends to attached entities, so ignoring a
      // ped seated in a car would make autofocus look straight through the
      // player's own vehicle and focus the background instead.
      int afIgnore = (ENTITY::DOES_ENTITY_EXIST(s_FrozenPed) &&
                      PED::IS_PED_IN_ANY_VEHICLE(s_FrozenPed, FALSE))
                         ? 0
                         : s_FrozenPed;
      int handle = invoke<int>(0x377906D8A31E5586, s_PosX, s_PosY, s_PosZ, endX,
                               endY, endZ, -1, afIgnore, 7);

      int hit = 0;
      Vector3 endCoords, surfaceNormal;
      int entityHit = 0;

      // GET_SHAPE_TEST_RESULT
      int result = invoke<int>(0x3D87450E15D98694, handle, &hit, &endCoords,
                               &surfaceNormal, &entityHit);

      if (result == 2 && hit != 0) {
        // Calculate distance
        float dx = endCoords.x - s_PosX;
        float dy = endCoords.y - s_PosY;
        float dz = endCoords.z - s_PosZ;
        g_DoFFocusDist = sqrt(dx * dx + dy * dy + dz * dz);
      } else {
        g_DoFFocusDist = 1000.0f; // Default if nothing hit
      }
    }

    // Use standard Depth of Field planes since Shallow DoF hashes are missing
    float nearFocus = g_DoFFocusDist;
    float nearBlur = nearFocus - g_DoFMaxNearInFocus;
    float farFocus = nearFocus + g_DoFMaxFarInFocus;
    float farBlur = farFocus + 15.0f;

    if (nearBlur < 0.0f)
      nearBlur = 0.0f;

    CAM::SET_CAM_NEAR_DOF(s_Cam, nearBlur);
    CAM::SET_CAM_FAR_DOF(s_Cam, farBlur);

    CAM::SET_CAM_DOF_PLANES(s_Cam, nearBlur, nearFocus, farFocus, farBlur);
  } else if (g_FreeCamActive || IGCS_IsConnected()) {
    // Force disable game's automatic DoF by explicitly taking control and
    // setting 0 blur
    CAM::SET_USE_HI_DOF();

    // Near planes: 0, Far planes: 10000
    CAM::SET_CAM_NEAR_DOF(s_Cam, 0.0f);
    CAM::SET_CAM_FAR_DOF(s_Cam, 10000.0f);
    CAM::SET_CAM_DOF_PLANES(s_Cam, 0.0f, 0.0f, 10000.0f, 10000.0f);

    // SET_CAM_DOF_STRENGTH to 0
    invoke<Void>(0x5EE29B4D7D5DF897, s_Cam, 0.0f);
  } else {
    // Freecam is OFF and IGCS is disconnected.
    // Explicitly release script control over DoF so the game's default dynamic
    // DoF works again. SET_CAM_USE_SHALLOW_DOF_MODE
    invoke<Void>(0x16A96863A17552BB, s_Cam, FALSE);
  }

  StreamFollowTick(); // keep streaming/LOD on the shot, not the player
}

// ============================================================
//  Auto Drive
// ============================================================
//
// The AI drives the player's current land vehicle while the free camera flies
// independently. Two modes: "Go To Waypoint" tasks the car to the map pin and
// re-tasks if the pin moves; "Drive Anywhere" wanders the road network. Written
// from scratch against the documented GTA natives (no third-party code).

// Standard GTA V driving-style bitflags.
struct AutoDriveStyleDef {
  const char *name;
  int value;
};
static const AutoDriveStyleDef kAutoDriveStyles[] = {
    {"Normal", 786603},
    {"Rushed", 1074528293},
    {"Avoid Traffic", 786468},
    {"Ignore Lights", 2883621},
    {"Sometimes Overtake", 5},
    {"Avoid Traffic A Lot", 6},
};
const char *g_AutoDriveStyleNames[] = {
    "Normal",      "Rushed",            "Avoid Traffic",
    "Ignore Lights", "Sometimes Overtake", "Avoid Traffic A Lot"};
const int g_AutoDriveStyleCount =
    sizeof(g_AutoDriveStyleNames) / sizeof(g_AutoDriveStyleNames[0]);

int AutoDriveStyleValue(int index) {
  if (index < 0 || index >= g_AutoDriveStyleCount)
    index = 0;
  return kAutoDriveStyles[index].value;
}

// Live-task tracking. We only (re)issue the driving task when something
// material changes — re-tasking every frame makes the car stutter and brake.
static bool s_AutoDriveTasked = false;
static int s_AutoDriveVeh = 0;
static float s_AutoDriveDestX = 0.0f, s_AutoDriveDestY = 0.0f,
             s_AutoDriveDestZ = 0.0f;
static float s_AutoDriveLastSpeed = -1.0f;
static int s_AutoDriveLastStyle = -1;
static int s_AutoDriveLastMode = -1;

void AutoDrive_Stop() {
  Ped ped = PLAYER::PLAYER_PED_ID();
  if (s_AutoDriveTasked && ENTITY::DOES_ENTITY_EXIST(ped))
    AI::CLEAR_PED_TASKS(ped);

  // Restore the free-cam freeze state we lifted so the AI could drive — match
  // the "Allow Player to Move" toggle so we don't leave the ped in a state the
  // rest of the mod doesn't expect.
  if (g_FreeCamActive && !g_EnablePlayerMovement &&
      ENTITY::DOES_ENTITY_EXIST(ped)) {
    ENTITY::FREEZE_ENTITY_POSITION(ped, TRUE);
    ENTITY::SET_ENTITY_COLLISION(ped, FALSE, FALSE);
  }

  g_AutoDriveEnabled = false;
  s_AutoDriveTasked = false;
  s_AutoDriveVeh = 0;
  s_AutoDriveLastSpeed = -1.0f;
  s_AutoDriveLastStyle = -1;
  s_AutoDriveLastMode = -1;
}

void UpdateAutoDrive() {
  if (!g_AutoDriveEnabled)
    return;

  Ped ped = PLAYER::PLAYER_PED_ID();
  if (!ENTITY::DOES_ENTITY_EXIST(ped) ||
      !PED::IS_PED_IN_ANY_VEHICLE(ped, FALSE)) {
    // Nothing to drive (player on foot) — drop any stale task and idle.
    if (s_AutoDriveTasked && ENTITY::DOES_ENTITY_EXIST(ped))
      AI::CLEAR_PED_TASKS(ped);
    s_AutoDriveTasked = false;
    s_AutoDriveVeh = 0;
    return;
  }

  Vehicle veh = PED::GET_VEHICLE_PED_IS_IN(ped, FALSE);
  if (veh == 0 || !ENTITY::DOES_ENTITY_EXIST(veh))
    return;

  // The free camera freezes the player ped on entry; a frozen driver can't
  // drive. Keep it free + collidable while Auto Drive owns the car.
  ENTITY::FREEZE_ENTITY_POSITION(ped, FALSE);
  ENTITY::SET_ENTITY_COLLISION(ped, TRUE, FALSE);

  int style = AutoDriveStyleValue(g_AutoDriveStyleIndex);
  float speed = g_AutoDriveSpeed;

  bool needRetask = !s_AutoDriveTasked || veh != s_AutoDriveVeh ||
                    speed != s_AutoDriveLastSpeed ||
                    style != s_AutoDriveLastStyle ||
                    g_AutoDriveMode != s_AutoDriveLastMode;

  if (g_AutoDriveMode == 0) {
    // Go To Waypoint
    if (!UI::IS_WAYPOINT_ACTIVE()) {
      // No pin set — stop driving and wait for one. Re-tasks automatically
      // once the user drops a waypoint.
      if (s_AutoDriveTasked) {
        AI::CLEAR_PED_TASKS(ped);
        s_AutoDriveTasked = false;
      }
      return;
    }
    Vector3 dest = UI::GET_BLIP_COORDS(UI::GET_FIRST_BLIP_INFO_ID(8)); // 8 = waypoint
    // Re-task if the pin moved more than ~5 m (squared distance > 25).
    float dx = dest.x - s_AutoDriveDestX;
    float dy = dest.y - s_AutoDriveDestY;
    float dz = dest.z - s_AutoDriveDestZ;
    if (dx * dx + dy * dy + dz * dz > 25.0f)
      needRetask = true;
    if (needRetask) {
      AI::TASK_VEHICLE_DRIVE_TO_COORD_LONGRANGE(ped, veh, dest.x, dest.y,
                                                dest.z, speed, style, 7.0f);
      s_AutoDriveDestX = dest.x;
      s_AutoDriveDestY = dest.y;
      s_AutoDriveDestZ = dest.z;
    }
  } else {
    // Drive Anywhere (wander the road network)
    if (needRetask)
      AI::TASK_VEHICLE_DRIVE_WANDER(ped, veh, speed, style);
  }

  s_AutoDriveTasked = true;
  s_AutoDriveVeh = veh;
  s_AutoDriveLastSpeed = speed;
  s_AutoDriveLastStyle = style;
  s_AutoDriveLastMode = g_AutoDriveMode;
}

// ============================================================
//  Time & Weather Update
// ============================================================

void UpdateTimeWeather() {
  if (g_TimePaused) {
    TIME::PAUSE_CLOCK(TRUE);
    SetClockTime(g_TimeHour, g_TimeMinute, 0);
  } else {
    TIME::PAUSE_CLOCK(FALSE);
  }

  // Timelapse logic
  if (g_TimelapseMode > 0) {
    static DWORD lastLapseTick = GetTickCount();
    DWORD currentTick = GetTickCount();

    DWORD interval = 100; // 0 = Off. 1 = Slow.
    if (g_TimelapseMode == 2)
      interval = 25; // Medium
    if (g_TimelapseMode == 3)
      interval = 5; // Fast

    if (currentTick - lastLapseTick >= interval) {
      g_TimeMinute++;
      if (g_TimeMinute > 59) {
        g_TimeMinute = 0;
        g_TimeHour = (g_TimeHour + 1) % 24;
      }
      SetClockTime(g_TimeHour, g_TimeMinute, 0);
      lastLapseTick = currentTick;
    }
  }

  // Weather Blending logic
  if (g_BlendWeatherActive && g_WeatherBlend > 0.0f) {
    // Clamp before indexing g_WeatherNames[] — a stray index here reads a bad
    // char* and passes garbage to GET_HASH_KEY.
    int i1 = (g_Weather1Index < 0 || g_Weather1Index >= g_WeatherCount) ? 0 : g_Weather1Index;
    int i2 = (g_Weather2Index < 0 || g_Weather2Index >= g_WeatherCount) ? 0 : g_Weather2Index;
    Hash w1 = GAMEPLAY::GET_HASH_KEY((char *)g_WeatherNames[i1]);
    Hash w2 = GAMEPLAY::GET_HASH_KEY((char *)g_WeatherNames[i2]);
    SetWeatherTransition(w1, w2, g_WeatherBlend);
  }
}

// ============================================================
//  Global Effects (run every frame regardless of freecam)
// ============================================================

// World & Scene: when "Clear Vehicles" / "Clear Peds" are on, keep the map empty
// of ambient traffic and pedestrians — both by zeroing the spawn density every
// frame (so nothing new appears) AND by deleting whatever already exists. Always
// spared: the player ped, the player's vehicle (current or last), the free-cam
// follow target, and any tool-owned entity (recorded clip, replay ghost, or a
// ghost's driver). Other players (IS_PED_A_PLAYER) are never touched.
void UpdateWorldPopulation() {
  // The density multipliers below are THIS_FRAME (they auto-reset every frame),
  // but the population BUDGET is sticky — left at 0 it permanently blocks new
  // spawns even after the toggle is switched off. Edge-detect the off-transition
  // and restore the game's normal full budget (3) once so traffic/peds return.
  static bool prevVeh = false, prevPed = false;
  if (prevVeh && !g_ClearVehicles) STREAMING::SET_VEHICLE_POPULATION_BUDGET(3);
  if (prevPed && !g_ClearPeds) STREAMING::SET_PED_POPULATION_BUDGET(3);
  prevVeh = g_ClearVehicles;
  prevPed = g_ClearPeds;

  if (!g_ClearVehicles && !g_ClearPeds) return;

  const Ped player = PLAYER::PLAYER_PED_ID();

  if (g_ClearVehicles) {
    VEHICLE::SET_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);
    VEHICLE::SET_RANDOM_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);
    VEHICLE::SET_PARKED_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);
    STREAMING::SET_VEHICLE_POPULATION_BUDGET(0);

    Vehicle keep = PED::GET_VEHICLE_PED_IS_IN(player, FALSE);
    if (keep == 0) keep = PED::GET_VEHICLE_PED_IS_IN(player, TRUE); // last car on foot

    int veh[1024];
    int n = worldGetAllVehicles(veh, 1024);
    for (int i = 0; i < n; ++i) {
      Vehicle v = veh[i];
      if (v == 0 || v == keep) continue;
      if (g_FollowTargetEntity != 0 && v == g_FollowTargetEntity) continue;
      if (VehicleClip_OwnsEntity(v)) continue;
      if (!ENTITY::DOES_ENTITY_EXIST(v)) continue;
      ENTITY::SET_ENTITY_AS_MISSION_ENTITY(v, TRUE, TRUE);
      VEHICLE::DELETE_VEHICLE(&v);
    }
  }

  if (g_ClearPeds) {
    PED::SET_PED_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);
    PED::SET_SCENARIO_PED_DENSITY_MULTIPLIER_THIS_FRAME(0.0f, 0.0f);
    STREAMING::SET_PED_POPULATION_BUDGET(0);

    int ped[1024];
    int n = worldGetAllPeds(ped, 1024);
    for (int i = 0; i < n; ++i) {
      Ped p = ped[i];
      if (p == 0 || p == player) continue;
      if (PED::IS_PED_A_PLAYER(p)) continue; // never delete player peds (MP safety)
      if (g_FollowTargetEntity != 0 && p == g_FollowTargetEntity) continue;
      if (VehicleClip_OwnsEntity(p)) continue;
      if (!ENTITY::DOES_ENTITY_EXIST(p)) continue;
      ENTITY::SET_ENTITY_AS_MISSION_ENTITY(p, TRUE, TRUE);
      PED::DELETE_PED(&p);
    }
  }
}

void UpdateGlobalEffects() {
  // Three independent world-speed controls:
  //   - Pause Game (g_FreezeWorld): a true full halt via SET_GAME_PAUSED —
  //     stops EVERYTHING including the camera, audio and rendering. Total
  //     freeze; you can't fly while it's on. SET_THIS_SCRIPT_CAN_BE_PAUSED
  //     keeps our script thread alive so the toggle can be lifted.
  //   - Freeze All Entities (g_FreezeEntities): pins every ped/vehicle/object
  //     in place but leaves time scale normal, so the camera, audio and
  //     particles keep running — a "frozen actors, live camera" freeze.
  //     Re-applied every frame to catch entities that stream in.
  //   - Slow motion (g_WorldTimeScale): SET_TIME_SCALE(0.1..1.0). Works in both
  //     Free Camera and Sequence mode (the only owner of time scale now — the
  //     old per-sequence World Speed event was removed). Suspended while an
  //     image-sequence render is running, since the render drives its own
  //     capture time scale. Statics restore a 1.0 scale exactly once.
  static bool s_GamePaused = false;
  static bool s_EntitiesFrozen = false;
  static bool s_SlowMoApplied = false;

  // --- Pause Game (full halt) ---
  if (g_FreezeWorld) {
    GAMEPLAY::SET_THIS_SCRIPT_CAN_BE_PAUSED(FALSE);
    GAMEPLAY::SET_GAME_PAUSED(TRUE);
    s_GamePaused = true;
  } else if (s_GamePaused) {
    GAMEPLAY::SET_GAME_PAUSED(FALSE);
    s_GamePaused = false;
  }

  // --- Freeze All Entities (camera stays live) ---
  if (g_FreezeEntities) {
    FreezeWorldEntities(true); // every frame, to catch newly streamed entities
    s_EntitiesFrozen = true;
  } else if (s_EntitiesFrozen) {
    FreezeWorldEntities(false);
    s_EntitiesFrozen = false;
  }

  // --- Slow motion ---
  // Applies whenever the slider is below 1.0 — in any camera mode AND in normal
  // gameplay (no camera active), so it's a global world-speed control. Skipped
  // during a render so the renderer's capture time scale isn't overwritten; the
  // render restores real-time itself, and this re-asserts the slider once it
  // finishes. Restored to 1.0 exactly once when the slider returns to full.
  if (!g_FreezeWorld && !g_RenderActive && g_WorldTimeScale < 0.999f) {
    GAMEPLAY::SET_TIME_SCALE(g_WorldTimeScale);
    s_SlowMoApplied = true;
  } else if (s_SlowMoApplied) {
    GAMEPLAY::SET_TIME_SCALE(1.0f);
    s_SlowMoApplied = false;
  }

  // Info Overlay
  if (g_ShowInfoOverlay) {
    DrawInfoOverlay();
  }

  // Hide HUD
  if (g_HideHUD) {
    UI::HIDE_HUD_AND_RADAR_THIS_FRAME();
  }

  // Hide Player
  static bool s_PlayerWasHiddenByUs = false;
  Ped playerPed = PLAYER::PLAYER_PED_ID();
  if (ENTITY::DOES_ENTITY_EXIST(playerPed)) {
    if (g_HidePlayer) {
      ENTITY::SET_ENTITY_VISIBLE(playerPed, FALSE, FALSE);
      s_PlayerWasHiddenByUs = true;
    } else if (s_PlayerWasHiddenByUs) {
      ENTITY::SET_ENTITY_VISIBLE(playerPed, TRUE, FALSE);
      s_PlayerWasHiddenByUs = false;
    }
  }

  // Vehicle Shake Disabler
  if (g_DisableVehicleShake) {
    invoke<Void>(0x84FD40F56075E816, 0.0f);
  }
}

// ============================================================
//  IGCS Connector Helpers
// ============================================================

void IGCS_ResetOffsets() {
  s_IgcsOffsetX = 0.0f;
  s_IgcsOffsetY = 0.0f;
  s_IgcsOffsetZ = 0.0f;
  s_IgcsPitchOffset = 0.0f;
  s_IgcsYawOffset = 0.0f;
  s_IgcsRollOffset = 0.0f;
}

void IGCS_ShiftCamera(float rightStep, float upStep, float forwardStep,
                      float pitchDelta, float yawDelta, float rollDelta) {
  if (!g_FreeCamActive)
    return;

  // Use core rotation + IGCS offset rotation
  float curPitch = s_Pitch + s_IgcsPitchOffset;
  float curYaw = s_Yaw + s_IgcsYawOffset;
  float curRoll = g_CamRoll + s_IgcsRollOffset;

  // Compute direction vectors from current yaw/pitch/roll
  float yawRad = curYaw * (3.14159265f / 180.0f);
  float pitchRad = curPitch * (3.14159265f / 180.0f);
  float rollRad = curRoll * (3.14159265f / 180.0f);

  float cp = cosf(pitchRad), sp = sinf(pitchRad);
  float cy = cosf(yawRad), sy = sinf(yawRad);
  float cr = cosf(rollRad), sr = sinf(rollRad);

  // Forward
  float fwdX = -sy * cp;
  float fwdY = cy * cp;
  float fwdZ = sp;

  // Right (accounts for roll)
  float rightX = cy * cr + sy * sp * sr;
  float rightY = sy * cr - cy * sp * sr;
  float rightZ = cp * sr;

  // Up (cross product: forward x right, accounts for pitch and roll)
  float camUpX = -cy * sr + sy * sp * cr;
  float camUpY = -sy * sr - cy * sp * cr;
  float camUpZ = cp * cr;

  if (IGCS_IsSessionActive()) {
    s_IgcsOffsetX += rightX * rightStep + camUpX * upStep + fwdX * forwardStep;
    s_IgcsOffsetY += rightY * rightStep + camUpY * upStep + fwdY * forwardStep;
    s_IgcsOffsetZ += rightZ * rightStep + camUpZ * upStep + fwdZ * forwardStep;
    s_IgcsPitchOffset += pitchDelta;
    s_IgcsYawOffset += yawDelta;
    s_IgcsRollOffset += rollDelta;
  } else {
    s_PosX += rightX * rightStep + camUpX * upStep + fwdX * forwardStep;
    s_PosY += rightY * rightStep + camUpY * upStep + fwdY * forwardStep;
    s_PosZ += rightZ * rightStep + camUpZ * upStep + fwdZ * forwardStep;
    s_Pitch += pitchDelta;
    s_Yaw += yawDelta;
    g_CamRoll += rollDelta;
  }
}

void IGCS_SetCameraPosition(float posX, float posY, float posZ, float pitch,
                            float yaw, float roll) {
  if (IGCS_IsSessionActive()) {
    s_IgcsOffsetX = posX - s_PosX;
    s_IgcsOffsetY = posY - s_PosY;
    s_IgcsOffsetZ = posZ - s_PosZ;
    s_IgcsPitchOffset = pitch - s_Pitch;
    s_IgcsYawOffset = yaw - s_Yaw;
    s_IgcsRollOffset = roll - g_CamRoll;
  } else {
    s_PosX = posX;
    s_PosY = posY;
    s_PosZ = posZ;
    s_Pitch = pitch;
    s_Yaw = yaw;
    g_CamRoll = roll;
  }
}

void GetCameraState(float &posX, float &posY, float &posZ, float &pitch,
                    float &yaw, float &roll) {
  posX = s_PosX;
  posY = s_PosY;
  posZ = s_PosZ;
  pitch = s_Pitch;
  yaw = s_Yaw;
  roll = g_CamRoll;
}

// ============================================================
//  Sequence-mode helpers
// ============================================================

// State that lets SequencePushToEngine decide between native attach and
// script-driven SET_CAM_COORD. Set by SetCameraStateFromSequenceLocked
// (active) and cleared by SetCameraStateFromSequence (inactive). Kept
// module-private so the sequence module only touches these via the
// two API entry points.
static bool s_SequenceAttachActive = false;
static int s_SequenceAttachEntity = 0;
static float s_SequenceAttachOffX = 0.0f;
static float s_SequenceAttachOffY = 0.0f;
static float s_SequenceAttachOffZ = 0.0f;

void SetCameraStateFromSequence(float posX, float posY, float posZ,
                                float pitch, float yaw, float roll,
                                float fov) {
  s_PosX = posX;
  s_PosY = posY;
  s_PosZ = posZ;
  s_Pitch = pitch;
  s_Yaw = yaw;
  g_CamRoll = roll;
  g_CamFOV = fov;
  // Mark attachment as inactive so SequencePushToEngine routes through
  // the world-coord (detach + SET_CAM_COORD) path. Any leftover handle
  // from a prior locked-segment frame stops being honored.
  s_SequenceAttachActive = false;
  s_SequenceAttachEntity = 0;
}

void SetCameraStateFromSequenceLocked(int entity, float localOffsetX,
                                      float localOffsetY, float localOffsetZ,
                                      float pitch, float yaw, float roll,
                                      float fov) {
  // Resolve a world coord too — used as fallback if SequencePushToEngine
  // ends up in the unattached path (e.g. entity died between this call
  // and the push), and so things like shake speed-tracking still have
  // a fresh value to derive velocity from.
  Vector3 w = invoke<Vector3>(0x1899F328B0E12848, entity, localOffsetX,
                              localOffsetY, localOffsetZ);
  s_PosX = w.x;
  s_PosY = w.y;
  s_PosZ = w.z;
  s_Pitch = pitch;
  s_Yaw = yaw;
  g_CamRoll = roll;
  g_CamFOV = fov;
  s_SequenceAttachActive = true;
  s_SequenceAttachEntity = entity;
  s_SequenceAttachOffX = localOffsetX;
  s_SequenceAttachOffY = localOffsetY;
  s_SequenceAttachOffZ = localOffsetZ;
}

void SequencePushToEngine(float dt) {
  // Sequence mode shares the Free Camera scripted-cam handle (s_Cam),
  // initialized by InitFreeCamera() during Sequence_EnterMode. If for
  // some reason the cam isn't up, bail.
  if (s_Cam == 0)
    return;

  // Two routing modes based on whether the current playback segment is
  // anchored to an entity:
  //
  //   Attach path (s_SequenceAttachActive): when both endpoints of the
  //   current spline segment are locked to the same entity, the sequence
  //   driver computed an entity-local offset for this frame. We bind
  //   the camera to that entity via ATTACH_CAM_TO_ENTITY — the engine
  //   then positions the camera relative to the entity at RENDER time,
  //   AFTER physics has finalized. This is the only way to avoid the
  //   one-frame lag between the entity's physics update and a script-
  //   driven SET_CAM_COORD (which always reads stale entity coords).
  //   Without this, riding-along-with-a-moving-vehicle looks shaky.
  //
  //   Detach path (default): clear any native attachment left over from
  //   prior free-cam rigid follow or from a previous locked segment, so
  //   SET_CAM_COORD below has authority. DETACH_CAM is idempotent —
  //   harmless when the cam isn't actually attached.
  bool useAttach = s_SequenceAttachActive && s_SequenceAttachEntity != 0 &&
                   ENTITY::DOES_ENTITY_EXIST(s_SequenceAttachEntity);
  if (useAttach) {
    invoke<Void>(0xFEDB7D269E8C60E3, s_Cam, s_SequenceAttachEntity,
                 s_SequenceAttachOffX, s_SequenceAttachOffY,
                 s_SequenceAttachOffZ, TRUE); // ATTACH_CAM_TO_ENTITY
  } else {
    invoke<Void>(0xA2FABBE87F4BAD82, s_Cam, FALSE); // DETACH_CAM
  }

  // Procedural shake during playback: ComputeShakeOffsets is normally
  // called by UpdateFreeCamera, which doesn't run while Sequence is
  // playing. Call it directly here so any SHAKE_* effect events that
  // toggled g_ShakeEnabled actually produce visible jitter on top of
  // the playback pose. Also keep s_PrevPos in sync so speed-coupled
  // shake works (speed = position derivative due to playback motion).
  float shakeDx = 0, shakeDy = 0, shakeDz = 0;
  float shakePitchD = 0, shakeYawD = 0, shakeRollD = 0;
  if (dt > 0.0f) {
    ComputeShakeOffsets(dt, shakeDx, shakeDy, shakeDz, shakePitchD,
                        shakeYawD, shakeRollD);
    s_PrevPosX = s_PosX;
    s_PrevPosY = s_PosY;
    s_PrevPosZ = s_PosZ;
    s_HasPrevPos = true;
  }

  // Skip SET_CAM_COORD when natively attached — the engine owns position
  // in that mode and the call would be ignored. As a consequence, the
  // position component of procedural shake is also a no-op for locked
  // segments; this is the right trade-off because the lag-free entity
  // follow it buys is more important than position jitter on a shot
  // that's already deliberately rigid to a moving vehicle. Rotation and
  // FOV are always pushed — both work whether attached or not.
  if (!useAttach) {
    CAM::SET_CAM_COORD(s_Cam, s_PosX + shakeDx, s_PosY + shakeDy,
                       s_PosZ + shakeDz);
  }
  CAM::SET_CAM_ROT(s_Cam, s_Pitch + shakePitchD,
                   g_CamRoll + shakeRollD, s_Yaw + shakeYawD, 2);
  invoke<Void>(0xB13C14F66A00D047, s_Cam, g_CamFOV); // SET_CAM_FOV (real-time)
  StreamFollowTick(); // keep streaming/LOD on the shot, not the player
}

// Detach the scripted cam from any entity a locked sequence segment bound it
// to via ATTACH_CAM_TO_ENTITY. Must run when playback stops — otherwise the
// cam stays glued to the entity and free-fly authoring can't TRANSLATE it
// (SET_CAM_COORD is ignored while attached); you'd only be able to rotate.
// Idempotent: DETACH_CAM is harmless when the cam isn't actually attached.
void SequenceDetachCamera() {
  if (s_Cam != 0)
    invoke<Void>(0xA2FABBE87F4BAD82, s_Cam, FALSE); // DETACH_CAM
  s_SequenceAttachActive = false;
  s_SequenceAttachEntity = 0;
}
