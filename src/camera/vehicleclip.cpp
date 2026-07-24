/*
        GTA V Free Camera / Photo Mode Plugin
        Vehicle Clip — deterministic multi-vehicle path record / replay
        See vehicleclip.h for the rationale and contract.

        Multiple vehicles can be recorded onto one shared timeline (t=0 = start
        of the take). Recording a new vehicle replays all previously-recorded
        ones as ghosts around the player, so traffic / chases can be choreographed
        layer by layer. On playback every clip replays together in sync.
*/

#include "vehicleclip.h"
#include "camera.h" // ScriptHookV natives + free-cam suspend/resume
#include "vehmem.h" // direct CWheel access for replay wheel spin

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma warning(disable : 4244 4305)

// Defined in sequence.cpp — the shared path-timestamp interval in seconds
// (0 = off), driven by the Appearance "Path Timestamps" setting.
float SeqTimeLabelStep();

// Mod slots 0..kModSlots-1 captured for visual replication. Slots 17..22 are
// toggle mods (turbo / tire smoke / xenon / etc.), the rest are indexed mods.
static const int kModSlots = 49;
static bool IsToggleSlot(int m) { return m >= 17 && m <= 22; }

// Snapshot of a recorded vehicle's appearance so a loaded clip can respawn a
// faithful ghost (model + colors + mods).
struct VehVisual {
  int model;
  int primary, secondary, pearl, wheelColor;
  int customPrim, cpR, cpG, cpB; // customPrim/Sec are 0/1 flags
  int customSec, csR, csG, csB;
  int windowTint, wheelType, livery, plateIdx;
  char plate[16];
  unsigned toggleMask;       // bit m set => toggle slot m is on
  int mods[kModSlots];       // indexed mod value per slot (-1 = none)
  int modVar[kModSlots];     // custom-tire / variation flag per slot (0/1)
  bool valid;
};

// Snapshot of the driver ped's model + outfit so a ghost can seat a matching
// driver. Components 0..11 (drawable/texture/palette) + props 0..2 (index/tex).
static const int kPedComps = 12;
static const int kPedProps = 3;
struct PedVisual {
  int model;
  int compDraw[kPedComps], compTex[kPedComps], compPal[kPedComps];
  int propIdx[kPedProps], propTex[kPedProps];
  bool valid;
};

// One recorded frame of vehicle state.
struct ClipSample {
  float t;                 // seconds from clip start
  float px, py, pz;        // world position
  float qx, qy, qz, qw;    // world rotation (GTA quaternion)
  float vx, vy, vz;        // linear velocity (m/s)
  int   nWheels;           // wheels captured (0 if memory access unavailable)
  float wheel[VehMem::kMaxWheels]; // per-wheel rotation angle (radians)
  float steer[VehMem::kMaxWheels]; // per-wheel steering angle (radians, signed)
  float susp[VehMem::kMaxWheels];  // per-wheel suspension compression
};

// One recorded vehicle: its samples, appearance, and (during replay) a spawned
// ghost vehicle the samples are played back onto.
struct VehClip {
  std::vector<ClipSample> samples;
  VehVisual visual;
  PedVisual ped;      // recorded driver (valid=false if none captured)
  VehicleClipSettings st; // per-clip editable settings (speed/offset/lights/...)
  int ghost;          // spawned ghost handle (0 = none)
  int driver;         // spawned driver ped handle (0 = none)
  bool ghostPending;  // wants a ghost spawned for replay
  bool collisionOff;  // we currently hold the ghost (collision disabled)
  bool hasSteer;      // per-wheel steering angles were recorded (memory-driven)
  bool hasSusp;       // per-wheel suspension compression was recorded — replay
                      // writes it back so the collision-off ghost keeps its
                      // recorded ride height (no wheels drooping into the road)
  float steerSmoothed; // low-passed fallback visual steer (no recorded steer)
};

// Per-clip settings initialized from the current globals (which act as the
// "defaults for new clips") so a fresh recording inherits the menu toggles.
static VehicleClipSettings DefaultClipSettings() {
  VehicleClipSettings s{};
  s.enabled = true;
  s.speed = 1.0f;
  s.offset = 0.0f;
  s.lights = 0; // Auto
  s.engineOn = true;
  s.siren = false;
  s.showDriver = g_VehicleClipShowDriver;
  s.collision = g_VehicleClipGhostCollision;
  s.godmode = g_VehicleClipGhostGodmode;
  s.label[0] = '\0';
  return s;
}

// ---- Module state ----
static std::vector<VehClip> s_clips; // all banked vehicle clips (shared timeline)
static bool s_enabled = true;        // replay allowed when data exists

// Recording-in-progress (a single new clip, committed to s_clips on stop).
static bool s_recording = false;
static int s_recVehicle = 0;     // live player-driven car being recorded
static float s_recTime = 0.0f;   // running clip time while recording
static float s_lastSampleT = 0.0f;
static std::vector<ClipSample> s_recSamples;
static VehVisual s_recVisual = {};
static PedVisual s_recPed = {};  // driver ped captured at record start
static bool s_recHasSteer = false; // steering offset was available at record
static bool s_recHasSusp = false;  // suspension offset was available at record

// Hard cap so a forgotten recording can't grow without bound (120 s).
static const float kMaxRecordSeconds = 120.0f;

// Recording sample rate (Hz); 0 = every frame. Customizable from the menu.
int g_VehicleClipSampleHz = 30;

// Visual front-wheel steering strength multiplier for replay. Steering is now
// replayed from the real recorded per-wheel angles, so 1.0 = exact; the knob is
// just optional artistic exaggeration (and scales the fallback for old clips).
float g_VehicleClipSteerGain = 1.0f;

// Seat a recorded driver ped inside each ghost on replay.
bool g_VehicleClipShowDriver = true;

// Keep replayed ghosts' collision on (show impacts) instead of off (smooth).
bool g_VehicleClipGhostCollision = false;

// Keep replayed ghosts pristine (invincible + auto-repair) to hide damage.
bool g_VehicleClipGhostGodmode = true;

static void CaptureVisual(int veh, VehVisual &out); // defined below
static void ApplyVisual(int veh, const VehVisual &v);
static void CapturePedVisual(int ped, PedVisual &out);
static void ApplyPedVisual(int ped, const PedVisual &v);
static void EnsureDriverFor(VehClip &c);
static void DespawnGhostFor(VehClip &c);

// ============================================================
//  Helpers
// ============================================================

static bool VehicleValid(int veh) {
  return veh != 0 && ENTITY::DOES_ENTITY_EXIST(veh) &&
         ENTITY::IS_ENTITY_A_VEHICLE(veh);
}

static bool PedValid(int ped) {
  return ped != 0 && ENTITY::DOES_ENTITY_EXIST(ped) &&
         ENTITY::IS_ENTITY_A_PED(ped);
}

static ClipSample SampleVehicle(int veh, float t) {
  ClipSample s{};
  s.t = t;
  Vector3 p = ENTITY::GET_ENTITY_COORDS(veh, TRUE);
  s.px = p.x; s.py = p.y; s.pz = p.z;
  ENTITY::GET_ENTITY_QUATERNION(veh, &s.qx, &s.qy, &s.qz, &s.qw);
  Vector3 v = ENTITY::GET_ENTITY_VELOCITY(veh);
  s.vx = v.x; s.vy = v.y; s.vz = v.z;
  // Capture the live per-wheel rotation angle so replay can reproduce the spin
  // exactly (collision is off during playback, so the sim won't roll them).
  s.nWheels = VehMem::Available()
                  ? VehMem::ReadWheelAngles(veh, s.wheel, VehMem::kMaxWheels)
                  : 0;
  // Per-wheel steering angle (signed) — replayed via memory so it survives a
  // seated driver, which SET_VEHICLE_STEER_BIAS does not.
  if (VehMem::SteerAvailable())
    VehMem::ReadWheelSteer(veh, s.steer, VehMem::kMaxWheels);
  // Per-wheel suspension compression — replayed so the collision-off ghost
  // keeps the recorded ride height instead of drooping wheels into the road.
  if (VehMem::SuspAvailable())
    VehMem::ReadWheelSusp(veh, s.susp, VehMem::kMaxWheels);
  return s;
}

// Yaw (heading, degrees) from a GTA quaternion (GET_ENTITY_ROTATION order-2).
static float QuatYawDeg(float qx, float qy, float qz, float qw) {
  const float RAD2DEG = 57.2957795f;
  return atan2f(2.0f * (qw * qz + qx * qy),
                1.0f - 2.0f * (qy * qy + qz * qz)) * RAD2DEG;
}

static void ApplyVisualSteer(int veh, float steerBias) {
  if (steerBias < -1.0f) steerBias = -1.0f; else if (steerBias > 1.0f) steerBias = 1.0f;
  // STEER_UNLOCK_BIAS lets the value we set HOLD instead of the game snapping the
  // front wheels back to centre — without it SET_VEHICLE_STEER_BIAS is invisible
  // on a driverless ghost. Both are ScriptHookV SDK natives (no CFX extra), so
  // this works under FiveM where the per-wheel steer memory write isn't available.
  VEHICLE::STEER_UNLOCK_BIAS(veh, TRUE);
  VEHICLE::SET_VEHICLE_STEER_BIAS(veh, steerBias);
}

// Normalized lerp of two quaternions (shortest arc).
static void NlerpQuat(const ClipSample &a, const ClipSample &b, float u,
                      float &ox, float &oy, float &oz, float &ow) {
  float dot = a.qx * b.qx + a.qy * b.qy + a.qz * b.qz + a.qw * b.qw;
  float s = (dot < 0.0f) ? -1.0f : 1.0f; // flip b to the near hemisphere
  ox = a.qx + u * (s * b.qx - a.qx);
  oy = a.qy + u * (s * b.qy - a.qy);
  oz = a.qz + u * (s * b.qz - a.qz);
  ow = a.qw + u * (s * b.qw - a.qw);
  float len = sqrtf(ox * ox + oy * oy + oz * oz + ow * ow);
  if (len > 1e-6f) { ox /= len; oy /= len; oz /= len; ow /= len; }
  else { ox = oy = oz = 0.0f; ow = 1.0f; }
}

// Largest sample index whose time is <= t (binary search). 0 if t precedes the
// first sample.
static int LowerSampleIn(const std::vector<ClipSample> &s, float t) {
  int lo = 0, hi = (int)s.size() - 1;
  if (hi <= 0) return 0;
  if (t <= s[0].t) return 0;
  if (t >= s[hi].t) return hi;
  while (lo < hi) {
    int mid = (lo + hi + 1) / 2;
    if (s[mid].t <= t) lo = mid;
    else hi = mid - 1;
  }
  return lo;
}

// Interpolated world position along a sample list at clip time t.
static void PathPosIn(const std::vector<ClipSample> &s, float t, float &ox,
                      float &oy, float &oz) {
  int i = LowerSampleIn(s, t);
  const ClipSample &a = s[i];
  ox = a.px; oy = a.py; oz = a.pz;
  if (i + 1 < (int)s.size()) {
    const ClipSample &b = s[i + 1];
    float span = b.t - a.t;
    float u = (span > 1e-5f) ? (t - a.t) / span : 0.0f;
    if (u < 0.0f) u = 0.0f; else if (u > 1.0f) u = 1.0f;
    ox = a.px + u * (b.px - a.px);
    oy = a.py + u * (b.py - a.py);
    oz = a.pz + u * (b.pz - a.pz);
  }
}

static float ClipDuration(const std::vector<ClipSample> &s) {
  return s.empty() ? 0.0f : s.back().t;
}

// Interpolated heading (yaw, degrees) along a sample list at clip time t.
static float ClipYawAt(const std::vector<ClipSample> &S, float t) {
  int i = LowerSampleIn(S, t);
  const ClipSample &a = S[i];
  float qx = a.qx, qy = a.qy, qz = a.qz, qw = a.qw;
  if (i + 1 < (int)S.size()) {
    const ClipSample &b = S[i + 1];
    float span = b.t - a.t;
    float u = (span > 1e-5f) ? (t - a.t) / span : 0.0f;
    if (u < 0.0f) u = 0.0f; else if (u > 1.0f) u = 1.0f;
    NlerpQuat(a, b, u, qx, qy, qz, qw);
  }
  return QuatYawDeg(qx, qy, qz, qw);
}

// Visual front-wheel steer at clip time t, estimated from the heading turn rate
// over a time WINDOW centered on t (not the single sample-segment). The window
// averages out per-sample noise and is continuous in t, so the steering eases
// through turns instead of stepping at each sample. ~60 deg/s of yaw -> full lock.
static float ClipSteerAt(const std::vector<ClipSample> &S, float t) {
  const float h = 0.10f; // half-window seconds (small enough to keep peak turn)
  float lo = t - h, hi = t + h;
  float t0 = S.front().t, t1 = S.back().t;
  if (lo < t0) lo = t0;
  if (hi > t1) hi = t1;
  float denom = hi - lo;
  if (denom < 1e-3f) return 0.0f;
  float dyaw = ClipYawAt(S, hi) - ClipYawAt(S, lo);
  while (dyaw > 180.0f) dyaw -= 360.0f;
  while (dyaw <= -180.0f) dyaw += 360.0f;
  // ~40 deg/s of yaw -> full lock at gain 1.0; the gain scales how pronounced
  // the visible wheel turn is through corners.
  float steer = (dyaw / denom) / 40.0f * g_VehicleClipSteerGain;
  if (steer < -1.0f) steer = -1.0f; else if (steer > 1.0f) steer = 1.0f;
  return steer;
}

// ============================================================
//  Per-clip ghost lifecycle + replay
// ============================================================

// Delete a clip's driver ped, if any.
static void DeleteDriver(VehClip &c) {
  if (c.driver != 0) {
    if (PedValid(c.driver)) {
      int p = c.driver;
      ENTITY::SET_ENTITY_AS_MISSION_ENTITY(p, TRUE, TRUE);
      PED::DELETE_PED(&p);
    }
    c.driver = 0;
  }
}

// Spawn / despawn this clip's driver to match the Show-Driver toggle. Called
// once a ghost exists (and re-checked each frame, so the toggle is live).
static void EnsureDriverFor(VehClip &c) {
  bool ghostOk = c.ghost != 0 && VehicleValid(c.ghost);
  bool want = ghostOk && c.st.showDriver && c.ped.valid && c.ped.model != 0;
  bool have = c.driver != 0 && PedValid(c.driver);
  if (!want) { if (c.driver != 0) DeleteDriver(c); return; }
  if (have) return;

  int model = c.ped.model;
  if (!STREAMING::IS_MODEL_IN_CDIMAGE(model)) { c.ped.valid = false; return; }
  STREAMING::REQUEST_MODEL(model);
  if (!STREAMING::HAS_MODEL_LOADED(model)) return; // poll again next frame

  int ped = PED::CREATE_PED_INSIDE_VEHICLE(c.ghost, 4 /*CIVMALE*/, model,
                                           -1 /*driver seat*/, FALSE, FALSE);
  STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED(model);
  if (ped == 0) return;
  ApplyPedVisual(ped, c.ped);
  ENTITY::SET_ENTITY_AS_MISSION_ENTITY(ped, TRUE, TRUE);
  ENTITY::SET_ENTITY_INVINCIBLE(ped, TRUE);
  PED::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(ped, TRUE); // sit still, don't react
  PED::SET_PED_CAN_BE_DRAGGED_OUT(ped, FALSE);
  c.driver = ped;
}

static void EnsureGhostFor(VehClip &c) {
  if (!c.st.enabled) { if (c.ghost != 0) DespawnGhostFor(c); return; } // muted
  if (c.ghost != 0 && VehicleValid(c.ghost)) { EnsureDriverFor(c); return; }
  c.ghost = 0;
  DeleteDriver(c); // ghost gone -> its driver (if any) is invalid too
  if (!c.ghostPending || c.samples.size() < 2 || c.visual.model == 0) return;

  int model = c.visual.model;
  if (!STREAMING::IS_MODEL_IN_CDIMAGE(model) ||
      !STREAMING::IS_MODEL_A_VEHICLE(model)) {
    c.ghostPending = false;
    return;
  }
  STREAMING::REQUEST_MODEL(model);
  if (!STREAMING::HAS_MODEL_LOADED(model)) return; // poll again next frame

  const ClipSample &s0 = c.samples[0];
  int veh = VEHICLE::CREATE_VEHICLE(model, s0.px, s0.py, s0.pz, 0.0f, FALSE, FALSE);
  STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED(model);
  if (veh == 0) { c.ghostPending = false; return; }

  ApplyVisual(veh, c.visual);
  ENTITY::SET_ENTITY_AS_MISSION_ENTITY(veh, TRUE, TRUE);
  VEHICLE::SET_VEHICLE_IS_CONSIDERED_BY_PLAYER(veh, FALSE);
  ENTITY::SET_ENTITY_QUATERNION(veh, s0.qx, s0.qy, s0.qz, s0.qw);
  c.ghost = veh;
  c.collisionOff = false;
  c.ghostPending = false;
  VehMem::Init();
  EnsureDriverFor(c); // seat the recorded driver if enabled
}

static void ReleaseClip(VehClip &c) {
  if (c.collisionOff && c.ghost != 0 && VehicleValid(c.ghost)) {
    ENTITY::SET_ENTITY_VELOCITY(c.ghost, 0.0f, 0.0f, 0.0f);
    ENTITY::SET_ENTITY_COLLISION(c.ghost, TRUE, TRUE);
  }
  c.collisionOff = false;
}

static void DespawnGhostFor(VehClip &c) {
  DeleteDriver(c); // remove the driver before the car it sits in
  if (c.ghost != 0) {
    ReleaseClip(c);
    if (VehicleValid(c.ghost)) {
      int v = c.ghost;
      ENTITY::SET_ENTITY_AS_MISSION_ENTITY(v, TRUE, TRUE);
      ENTITY::DELETE_ENTITY(&v);
    }
    c.ghost = 0;
  }
  c.collisionOff = false;
  // Re-arm so the ghost respawns next time we're in Sequence mode. (This despawn
  // is temporary — leaving the mode, switching sequences — not a permanent
  // removal; VehicleClip_Clear/DeleteClip drop the clip data entirely instead.)
  c.ghostPending = (c.samples.size() >= 2 && c.visual.model != 0);
}

// Keep a ghost pristine (or release it) per the godmode toggle. Applied every
// replay frame so it survives collisions: invincible + no visible damage + body
// deformation fixed + full health each frame = damage never shows.
static void ApplyGhostGodmode(int veh, bool on) {
  if (on) {
    ENTITY::SET_ENTITY_INVINCIBLE(veh, TRUE);
    ENTITY::SET_ENTITY_CAN_BE_DAMAGED(veh, FALSE);
    VEHICLE::SET_VEHICLE_CAN_BE_VISIBLY_DAMAGED(veh, FALSE);
    VEHICLE::SET_VEHICLE_STRONG(veh, TRUE);
    VEHICLE::SET_VEHICLE_TYRES_CAN_BURST(veh, FALSE);
    VEHICLE::SET_VEHICLE_WHEELS_CAN_BREAK(veh, FALSE);
    VEHICLE::SET_VEHICLE_BODY_HEALTH(veh, 1000.0f);
    VEHICLE::SET_VEHICLE_ENGINE_HEALTH(veh, 1000.0f);
    VEHICLE::SET_VEHICLE_PETROL_TANK_HEALTH(veh, 1000.0f);
    VEHICLE::SET_VEHICLE_DEFORMATION_FIXED(veh); // smooth out any dents (auto-repair)
  } else {
    ENTITY::SET_ENTITY_INVINCIBLE(veh, FALSE);
    ENTITY::SET_ENTITY_CAN_BE_DAMAGED(veh, TRUE);
    VEHICLE::SET_VEHICLE_CAN_BE_VISIBLY_DAMAGED(veh, TRUE);
  }
}

// Drive one clip's ghost to clip time t. Spawns the ghost on demand.
static void ApplyClipAtTime(VehClip &c, float t, bool animateWheels) {
  EnsureGhostFor(c);
  int veh = c.ghost;
  if (!c.st.enabled || !VehicleValid(veh) || c.samples.size() < 2) return;

  // Per-clip time remap: start `offset` seconds into the timeline, then run at
  // `speed`x. Before it starts, hold at the first sample.
  float localT = (t - c.st.offset) * c.st.speed;
  if (localT < 0.0f) localT = 0.0f;
  t = localT; // everything below samples the clip at the remapped time

  // Collision (per-clip): off = smooth deterministic teleport; on = shove
  // objects it passes through (impacts). Honored live.
  if (c.st.collision) {
    if (c.collisionOff) {
      ENTITY::SET_ENTITY_COLLISION(veh, TRUE, TRUE);
      c.collisionOff = false;
    }
  } else if (!c.collisionOff) {
    ENTITY::SET_ENTITY_COLLISION(veh, FALSE, TRUE);
    c.collisionOff = true;
  }

  // Godmode (per-clip): keep the ghost undamaged (or allow damage when off).
  ApplyGhostGodmode(veh, c.st.godmode);

  // Lights / engine / siren (per-clip), reasserted each frame. lights: 0 Auto,
  // 1 On, 2 Off, 3 On+FullBeam → SET_VEHICLE_LIGHTS state 0 reset / 1 off / 2 on.
  int lstate = (c.st.lights == 2) ? 1 : (c.st.lights == 1 || c.st.lights == 3) ? 2 : 0;
  VEHICLE::SET_VEHICLE_LIGHTS(veh, lstate);
  VEHICLE::SET_VEHICLE_FULLBEAM(veh, c.st.lights == 3 ? TRUE : FALSE);
  VEHICLE::SET_VEHICLE_ENGINE_ON(veh, c.st.engineOn ? TRUE : FALSE, TRUE, TRUE);
  VEHICLE::SET_VEHICLE_SIREN(veh, c.st.siren ? TRUE : FALSE);

  const std::vector<ClipSample> &S = c.samples;
  int i = LowerSampleIn(S, t);
  const ClipSample &a = S[i];
  float px = a.px, py = a.py, pz = a.pz;
  float qx = a.qx, qy = a.qy, qz = a.qz, qw = a.qw;
  float vx = a.vx, vy = a.vy, vz = a.vz;
  int nWheels = a.nWheels;
  float wheelAng[VehMem::kMaxWheels];
  float steerArr[VehMem::kMaxWheels];
  float suspArr[VehMem::kMaxWheels];
  for (int w = 0; w < nWheels; ++w) {
    wheelAng[w] = a.wheel[w];
    steerArr[w] = a.steer[w];
    suspArr[w] = a.susp[w];
  }

  if (i + 1 < (int)S.size()) {
    const ClipSample &b = S[i + 1];
    float span = b.t - a.t;
    float u = (span > 1e-5f) ? (t - a.t) / span : 0.0f;
    if (u < 0.0f) u = 0.0f; else if (u > 1.0f) u = 1.0f;
    px = a.px + u * (b.px - a.px);
    py = a.py + u * (b.py - a.py);
    pz = a.pz + u * (b.pz - a.pz);
    NlerpQuat(a, b, u, qx, qy, qz, qw);
    vx = a.vx + u * (b.vx - a.vx);
    vy = a.vy + u * (b.vy - a.vy);
    vz = a.vz + u * (b.vz - a.vz);
    const float kPi = 3.14159265f;
    int nw = (a.nWheels < b.nWheels) ? a.nWheels : b.nWheels;
    for (int w = 0; w < nw; ++w) {
      float d = b.wheel[w] - a.wheel[w];
      while (d > kPi) d -= 2.0f * kPi;
      while (d <= -kPi) d += 2.0f * kPi;
      wheelAng[w] = a.wheel[w] + u * d;
      steerArr[w] = a.steer[w] + u * (b.steer[w] - a.steer[w]); // plain lerp
      suspArr[w] = a.susp[w] + u * (b.susp[w] - a.susp[w]);     // plain lerp
    }
    nWheels = nw;
  }

  ENTITY::SET_ENTITY_COORDS_NO_OFFSET(veh, px, py, pz, FALSE, FALSE, FALSE);
  ENTITY::SET_ENTITY_QUATERNION(veh, qx, qy, qz, qw);
  ENTITY::SET_ENTITY_VELOCITY(veh, animateWheels ? vx : 0.0f,
                              animateWheels ? vy : 0.0f,
                              animateWheels ? vz : 0.0f);

  // Wheel spin.
  if (VehMem::UsesNativeSpin()) {
    // FiveM native backend: there is no native to set an absolute wheel angle
    // (SET_VEHICLE_WHEEL_Y_ROTATION is camber), so reproduce the roll by forcing
    // each wheel's angular VELOCITY = forward speed / wheel radius. The game then
    // integrates that into the visible spin every frame, so even a teleported
    // ghost looks like it's driving. Gated by animateWheels (0 on a paused scrub).
    float omega = 0.0f;
    if (animateWheels) {
      // Forward (local +Y) axis in world space from the replayed quaternion.
      float fwx = 2.0f * (qx * qy - qz * qw);
      float fwy = 1.0f - 2.0f * (qx * qx + qz * qz);
      float fwz = 2.0f * (qy * qz + qx * qw);
      float fwdSpeed = vx * fwx + vy * fwy + vz * fwz; // m/s, signed (− = reverse)
      const float kWheelRadius = 0.34f; // m — visual approximation (avg car tyre)
      omega = fwdSpeed / kWheelRadius;
    }
    VehMem::SetWheelRotationSpeed(veh, omega);
  } else if (nWheels > 0 && VehMem::Available()) {
    // Memory backend (singleplayer): write the recorded roll angle to each CWheel.
    VehMem::WriteWheelAngles(veh, wheelAng, nWheels);
  }

  // Suspension compression: the ghost runs with collision off, so physics never
  // resolves wheel-ground contact and the wheels droop to full extension —
  // sinking visibly into the road. Writing the recorded compression back each
  // frame restores the exact recorded stance. (Old clips without recorded
  // suspension keep the previous behavior.)
  if (c.hasSusp && nWheels > 0 && VehMem::SuspAvailable())
    VehMem::WriteWheelSusp(veh, suspArr, nWheels);

  // Steering. Preferred path = write the recorded per-wheel steer angle to
  // memory (works even with a seated driver, which SET_VEHICLE_STEER_BIAS does
  // not, and is exact). g_VehicleClipSteerGain scales the visible amount.
  if (c.hasSteer && nWheels > 0 && VehMem::SteerAvailable()) {
    float out[VehMem::kMaxWheels];
    for (int w = 0; w < nWheels; ++w) {
      float s = steerArr[w] * g_VehicleClipSteerGain;
      if (s < -1.2f) s = -1.2f; else if (s > 1.2f) s = 1.2f; // sane radians cap
      out[w] = s;
    }
    VehMem::WriteWheelSteer(veh, out, nWheels);
  } else if (nWheels > 0 && VehMem::SteerAvailable()) {
    // Older clip without recorded steer: synthesize from the path turn rate and
    // write to the front wheels (indices 0/1) via memory.
    float ang = ClipSteerAt(S, t) * 0.7f * g_VehicleClipSteerGain; // -> radians
    if (ang < -1.0f) ang = -1.0f; else if (ang > 1.0f) ang = 1.0f;
    float out[VehMem::kMaxWheels];
    for (int w = 0; w < nWheels; ++w) out[w] = (w < 2) ? ang : 0.0f;
    VehMem::WriteWheelSteer(veh, out, nWheels);
  } else {
    // Last resort (no memory steer offset): steer bias — only visible on a
    // driverless ghost, but better than nothing.
    ApplyVisualSteer(veh, ClipSteerAt(S, t));
  }
}

// ============================================================
//  Recording
// ============================================================

bool VehicleClip_StartRecording() {
  int ped = PLAYER::PLAYER_PED_ID();
  int veh = PED::GET_VEHICLE_PED_IS_IN(ped, FALSE);
  if (!VehicleValid(veh)) return false;
  // Don't record a ghost vehicle (the player must drive a different car).
  for (const VehClip &c : s_clips)
    if (c.ghost == veh) return false;

  VehMem::Init();
  s_recHasSteer = VehMem::SteerAvailable(); // record per-wheel steer if we can
  s_recHasSusp = VehMem::SuspAvailable();   // record per-wheel suspension too
  s_recSamples.clear();
  CaptureVisual(veh, s_recVisual);
  CapturePedVisual(ped, s_recPed); // remember the driver (model + outfit)
  s_recVehicle = veh;
  s_recTime = 0.0f;
  s_lastSampleT = 0.0f;
  s_recording = true;
  s_recSamples.push_back(SampleVehicle(veh, 0.0f)); // anchor at t=0

  // Make every existing clip replay around the player while recording.
  for (VehClip &c : s_clips)
    if (c.ghost == 0) c.ghostPending = true;

  FreeCam_SuspendForDrive();
  return true;
}

void VehicleClip_StopRecording() {
  if (!s_recording) return;
  s_recording = false;
  if (s_recSamples.size() >= 2) {
    VehClip c{};
    c.samples = s_recSamples;
    c.visual = s_recVisual;
    c.ped = s_recPed;
    c.st = DefaultClipSettings();
    c.ghost = 0;
    c.driver = 0;
    c.ghostPending = true; // replay as a ghost from here on
    c.collisionOff = false;
    c.hasSteer = s_recHasSteer;
    c.hasSusp = s_recHasSusp;
    s_clips.push_back(c);
  }

  // Put the player's car (and the player in it) back NEXT TO the start of the
  // path they just drove — out of the middle of the scene and lined up for
  // another take. One lane to the RIGHT of the recorded starting pose, not ON
  // it: the ghost replays from exactly the first sample, so parking there
  // would drop the player's car inside the ghost.
  if (!s_recSamples.empty() && VehicleValid(s_recVehicle)) {
    const ClipSample &s0 = s_recSamples.front();
    // Right vector of the recorded start orientation (first column of the
    // quaternion's rotation matrix; GTA: X = right, Y = forward, Z = up).
    float rx = 1.0f - 2.0f * (s0.qy * s0.qy + s0.qz * s0.qz);
    float ry = 2.0f * (s0.qx * s0.qy + s0.qw * s0.qz);
    float rz = 2.0f * (s0.qx * s0.qz - s0.qw * s0.qy);
    const float kSideOffset = 4.0f; // ~one lane over
    ENTITY::SET_ENTITY_COORDS_NO_OFFSET(
        s_recVehicle, s0.px + rx * kSideOffset, s0.py + ry * kSideOffset,
        s0.pz + rz * kSideOffset, FALSE, FALSE, FALSE);
    ENTITY::SET_ENTITY_QUATERNION(s_recVehicle, s0.qx, s0.qy, s0.qz, s0.qw);
    ENTITY::SET_ENTITY_VELOCITY(s_recVehicle, 0.0f, 0.0f, 0.0f);
  }

  s_recSamples.clear();
  FreeCam_ResumeAfterDrive();
}

bool VehicleClip_IsRecording() { return s_recording; }

void VehicleClip_RecordTick() {
  if (!s_recording) return;
  if (!VehicleValid(s_recVehicle)) { VehicleClip_StopRecording(); return; }
  float dt = GAMEPLAY::GET_FRAME_TIME();
  if (dt <= 0.0f || dt > 0.1f) dt = 0.016f;
  s_recTime += dt;
  // Decimate to the configured rate (0 = every frame).
  float minInterval = (g_VehicleClipSampleHz > 0) ? (1.0f / g_VehicleClipSampleHz) : 0.0f;
  if (s_recTime - s_lastSampleT >= minInterval - 1e-6f) {
    s_recSamples.push_back(SampleVehicle(s_recVehicle, s_recTime));
    s_lastSampleT = s_recTime;
  }
  // Replay every previously-recorded clip around the player on the shared clock.
  // (The main loop skips the sequence tick while recording, so we drive them.)
  if (s_enabled)
    for (VehClip &c : s_clips) ApplyClipAtTime(c, s_recTime, true);
  if (s_recTime >= kMaxRecordSeconds) VehicleClip_StopRecording();
}

void VehicleClip_DrawRecordingBanner() {
  if (!s_recording) return;
  char buf[128];
  sprintf_s(buf,
            "~r~REC~s~ #%d  %.1fs   Drive - press the Menu key to stop",
            (int)s_clips.size() + 1, s_recTime);
  UI::SET_TEXT_FONT(0);
  UI::SET_TEXT_SCALE(0.5f, 0.5f);
  UI::SET_TEXT_COLOUR(255, 255, 255, 255);
  UI::SET_TEXT_CENTRE(1);
  UI::SET_TEXT_DROPSHADOW(0, 0, 0, 0, 0);
  UI::SET_TEXT_EDGE(1, 0, 0, 0, 205);
  UI::_SET_TEXT_ENTRY((char *)"STRING");
  UI::_ADD_TEXT_COMPONENT_STRING((char *)buf);
  UI::_DRAW_TEXT(0.5f, 0.045f);
}

// ============================================================
//  Replay (driven off the sequence timeline)
// ============================================================

void VehicleClip_EnsureGhost() {
  if (s_recording) return;
  for (VehClip &c : s_clips) EnsureGhostFor(c);
}

void VehicleClip_ApplyAtTime(float t, bool animateWheels) {
  if (!s_enabled || s_recording) return;
  for (VehClip &c : s_clips) ApplyClipAtTime(c, t, animateWheels);
}

void VehicleClip_Release() {
  for (VehClip &c : s_clips) ReleaseClip(c);
}

void VehicleClip_DespawnGhost() {
  for (VehClip &c : s_clips) DespawnGhostFor(c);
}

// ============================================================
//  In-world path preview
// ============================================================

static void PathText3D(float x, float y, float z, const char *text, int r,
                       int g, int b, int a, float scale) {
  float sx, sy;
  if (!GRAPHICS::_WORLD3D_TO_SCREEN2D(x, y, z, &sx, &sy)) return;
  UI::SET_TEXT_FONT(0);
  UI::SET_TEXT_SCALE(0.0f, scale);
  UI::SET_TEXT_COLOUR(r, g, b, a);
  UI::SET_TEXT_CENTRE(1);
  UI::SET_TEXT_DROPSHADOW(2, 0, 0, 0, 200);
  UI::SET_TEXT_EDGE(1, 0, 0, 0, 220);
  UI::_SET_TEXT_ENTRY((char *)"STRING");
  UI::_ADD_TEXT_COMPONENT_STRING((LPSTR)text);
  UI::_DRAW_TEXT(sx, sy);
}

// Per-clip path color palette (distinct hues so layered vehicles are readable).
static void ClipColor(int idx, int &r, int &g, int &b) {
  static const int pal[][3] = {
      {255, 140, 0},  {0, 200, 255},  {255, 80, 220}, {120, 220, 80},
      {240, 220, 60}, {170, 100, 255}, {255, 90, 90},  {90, 255, 170}};
  const int n = (int)(sizeof(pal) / sizeof(pal[0]));
  const int *c = pal[((idx % n) + n) % n];
  r = c[0]; g = c[1]; b = c[2];
}

static void DrawOneClipPath(const VehClip &clip, float currentTime, bool playing,
                            bool labelTimes, int r, int g, int b) {
  const std::vector<ClipSample> &S = clip.samples;
  const int n = (int)S.size();
  if (n < 2) return;
  const float dur = S[n - 1].t;
  const int baseA = playing ? 90 : 200;

  // Distance-decimated polyline.
  const float kMinSeg = 0.5f;
  float lastX = S[0].px, lastY = S[0].py, lastZ = S[0].pz;
  for (int i = 1; i < n; ++i) {
    const ClipSample &p = S[i];
    float dx = p.px - lastX, dy = p.py - lastY, dz = p.pz - lastZ;
    bool isLast = (i == n - 1);
    if (!isLast && (dx * dx + dy * dy + dz * dz) < kMinSeg * kMinSeg) continue;
    GRAPHICS::DRAW_LINE(lastX, lastY, lastZ, p.px, p.py, p.pz, r, g, b, baseA);
    lastX = p.px; lastY = p.py; lastZ = p.pz;
  }

  // Start (green) / end (red) markers.
  GRAPHICS::DRAW_MARKER(28, S[0].px, S[0].py, S[0].pz, 0, 0, 0, 0, 0, 0, 0.3f,
                        0.3f, 0.3f, 60, 220, 60, baseA, FALSE, FALSE, 2, FALSE,
                        NULL, NULL, FALSE);
  GRAPHICS::DRAW_MARKER(28, S[n - 1].px, S[n - 1].py, S[n - 1].pz, 0, 0, 0, 0, 0,
                        0, 0.3f, 0.3f, 0.3f, 220, 60, 60, baseA, FALSE, FALSE, 2,
                        FALSE, NULL, NULL, FALSE);

  // Per-second timestamp labels along the path, drawn for every clip (colored to
  // match it). Labels read SHARED-TIMELINE time — honoring this clip's offset and
  // speed — so the same number on two vehicles marks the same instant, which is
  // exactly what you want when syncing layered clips.
  float tsStep = SeqTimeLabelStep();
  if (labelTimes && tsStep > 0.0f) {
    float sp = (clip.st.speed > 0.0001f) ? clip.st.speed : 1.0f;
    float tlDur = dur / sp; // this clip's length measured on the timeline
    const int la = playing ? 140 : 230;
    for (float sec = tsStep; sec < clip.st.offset + tlDur; sec += tsStep) {
      float localT = (sec - clip.st.offset) * sp; // timeline sec -> path
      if (localT < 0.0f || localT > dur) continue;
      float x, y, z;
      PathPosIn(S, localT, x, y, z);
      char buf[16];
      if (sec == floorf(sec)) sprintf_s(buf, "%ds", (int)sec);
      else                    sprintf_s(buf, "%.1fs", sec);
      PathText3D(x, y, z + 0.35f, buf, r, g, b, la, 0.28f);
      GRAPHICS::DRAW_LINE(x, y, z, x, y, z + 0.25f, r, g, b, baseA);
    }
  }

  // Direction arrows (capped ~30) along the path.
  float arrowStep = dur / 30.0f;
  if (arrowStep < 1.0f) arrowStep = 1.0f;
  for (float ta = arrowStep; ta < dur; ta += arrowStep) {
    int i = LowerSampleIn(S, ta);
    const ClipSample &p = S[i];
    float vx = p.vx, vy = p.vy, vz = p.vz;
    float vlen = sqrtf(vx * vx + vy * vy + vz * vz);
    if (vlen < 0.1f) continue;
    vx /= vlen; vy /= vlen; vz /= vlen;
    float x, y, z;
    PathPosIn(S, ta, x, y, z);
    const float aLen = 0.9f, barb = 0.3f;
    float tipX = x + vx * aLen, tipY = y + vy * aLen, tipZ = z + vz * aLen;
    GRAPHICS::DRAW_LINE(x, y, z, tipX, tipY, tipZ, r, g, b, baseA);
    float perpX = -vy, perpY = vx;
    GRAPHICS::DRAW_LINE(tipX, tipY, tipZ, tipX - vx * barb + perpX * barb,
                        tipY - vy * barb + perpY * barb, tipZ - vz * barb, r, g,
                        b, baseA);
    GRAPHICS::DRAW_LINE(tipX, tipY, tipZ, tipX - vx * barb - perpX * barb,
                        tipY - vy * barb - perpY * barb, tipZ - vz * barb, r, g,
                        b, baseA);
  }

  // Playhead: a bright sphere where the car actually is at the current timeline
  // time, honoring this clip's offset/speed remap so it matches replay.
  float sp = (clip.st.speed > 0.0001f) ? clip.st.speed : 1.0f;
  float localT = (currentTime - clip.st.offset) * sp;
  if (localT < 0.0f) localT = 0.0f;
  if (localT <= dur + 0.001f) {
    float x, y, z;
    PathPosIn(S, localT, x, y, z);
    int hr = (r + 255) / 2, hg = (g + 255) / 2, hb = (b + 255) / 2; // brighten
    GRAPHICS::DRAW_MARKER(28, x, y, z, 0, 0, 0, 0, 0, 0, 0.45f, 0.45f, 0.45f, hr,
                          hg, hb, playing ? 120 : 230, FALSE, FALSE, 2, FALSE,
                          NULL, NULL, FALSE);
  }
}

void VehicleClip_DrawPath(float currentTime, bool playing) {
  for (int i = 0; i < (int)s_clips.size(); ++i) {
    int r, g, b;
    ClipColor(i, r, g, b);
    DrawOneClipPath(s_clips[i], currentTime, playing, /*labelTimes=*/true, r,
                    g, b);
  }
}

// ============================================================
//  Vehicle appearance capture / apply (for ghost respawn)
// ============================================================

static void CaptureVisual(int veh, VehVisual &v) {
  memset(&v, 0, sizeof(v));
  v.model = ENTITY::GET_ENTITY_MODEL(veh);
  VEHICLE::GET_VEHICLE_COLOURS(veh, &v.primary, &v.secondary);
  VEHICLE::GET_VEHICLE_EXTRA_COLOURS(veh, &v.pearl, &v.wheelColor);
  v.customPrim = VEHICLE::GET_IS_VEHICLE_PRIMARY_COLOUR_CUSTOM(veh) ? 1 : 0;
  if (v.customPrim)
    VEHICLE::GET_VEHICLE_CUSTOM_PRIMARY_COLOUR(veh, &v.cpR, &v.cpG, &v.cpB);
  v.customSec = VEHICLE::GET_IS_VEHICLE_SECONDARY_COLOUR_CUSTOM(veh) ? 1 : 0;
  if (v.customSec)
    VEHICLE::GET_VEHICLE_CUSTOM_SECONDARY_COLOUR(veh, &v.csR, &v.csG, &v.csB);
  v.windowTint = VEHICLE::GET_VEHICLE_WINDOW_TINT(veh);
  v.wheelType = VEHICLE::GET_VEHICLE_WHEEL_TYPE(veh);
  v.livery = VEHICLE::GET_VEHICLE_LIVERY(veh);
  v.plateIdx = VEHICLE::GET_VEHICLE_NUMBER_PLATE_TEXT_INDEX(veh);
  const char *pt = VEHICLE::GET_VEHICLE_NUMBER_PLATE_TEXT(veh);
  if (pt) strncpy_s(v.plate, sizeof(v.plate), pt, _TRUNCATE);
  VEHICLE::SET_VEHICLE_MOD_KIT(veh, 0);
  for (int m = 0; m < kModSlots; ++m) {
    if (IsToggleSlot(m)) {
      v.mods[m] = -1;
      if (VEHICLE::IS_TOGGLE_MOD_ON(veh, m)) v.toggleMask |= (1u << m);
    } else {
      v.mods[m] = VEHICLE::GET_VEHICLE_MOD(veh, m);
      v.modVar[m] = VEHICLE::GET_VEHICLE_MOD_VARIATION(veh, m) ? 1 : 0;
    }
  }
  v.valid = true;
}

static void ApplyVisual(int veh, const VehVisual &v) {
  if (!v.valid) return;
  VEHICLE::SET_VEHICLE_MOD_KIT(veh, 0);
  VEHICLE::SET_VEHICLE_COLOURS(veh, v.primary, v.secondary);
  VEHICLE::SET_VEHICLE_EXTRA_COLOURS(veh, v.pearl, v.wheelColor);
  if (v.customPrim)
    VEHICLE::SET_VEHICLE_CUSTOM_PRIMARY_COLOUR(veh, v.cpR, v.cpG, v.cpB);
  if (v.customSec)
    VEHICLE::SET_VEHICLE_CUSTOM_SECONDARY_COLOUR(veh, v.csR, v.csG, v.csB);
  VEHICLE::SET_VEHICLE_WINDOW_TINT(veh, v.windowTint);
  VEHICLE::SET_VEHICLE_WHEEL_TYPE(veh, v.wheelType);
  if (v.livery >= 0) VEHICLE::SET_VEHICLE_LIVERY(veh, v.livery);
  VEHICLE::SET_VEHICLE_NUMBER_PLATE_TEXT_INDEX(veh, v.plateIdx);
  if (v.plate[0]) {
    char pbuf[16]; // native takes a non-const char*
    strncpy_s(pbuf, sizeof(pbuf), v.plate, _TRUNCATE);
    VEHICLE::SET_VEHICLE_NUMBER_PLATE_TEXT(veh, pbuf);
  }
  for (int m = 0; m < kModSlots; ++m) {
    if (IsToggleSlot(m))
      VEHICLE::TOGGLE_VEHICLE_MOD(veh, m, (v.toggleMask >> m) & 1u);
    else
      VEHICLE::SET_VEHICLE_MOD(veh, m, v.mods[m], v.modVar[m]);
  }
}

static void CapturePedVisual(int ped, PedVisual &v) {
  memset(&v, 0, sizeof(v));
  if (!PedValid(ped)) return;
  v.model = ENTITY::GET_ENTITY_MODEL(ped);
  for (int i = 0; i < kPedComps; ++i) {
    v.compDraw[i] = PED::GET_PED_DRAWABLE_VARIATION(ped, i);
    v.compTex[i] = PED::GET_PED_TEXTURE_VARIATION(ped, i);
    v.compPal[i] = PED::GET_PED_PALETTE_VARIATION(ped, i);
  }
  for (int i = 0; i < kPedProps; ++i) {
    v.propIdx[i] = PED::GET_PED_PROP_INDEX(ped, i);
    v.propTex[i] = PED::GET_PED_PROP_TEXTURE_INDEX(ped, i);
  }
  v.valid = (v.model != 0);
}

static void ApplyPedVisual(int ped, const PedVisual &v) {
  if (!v.valid) return;
  for (int i = 0; i < kPedComps; ++i)
    PED::SET_PED_COMPONENT_VARIATION(ped, i, v.compDraw[i], v.compTex[i],
                                     v.compPal[i]);
  for (int i = 0; i < kPedProps; ++i) {
    if (v.propIdx[i] >= 0)
      PED::SET_PED_PROP_INDEX(ped, i, v.propIdx[i], v.propTex[i], TRUE);
    else
      PED::CLEAR_PED_PROP(ped, i);
  }
}

// ============================================================
//  Persistence — JSON clip sidecar (array of clips)
// ============================================================

// Make a user-supplied string safe to embed in a JSON double-quoted value.
// The reader is a simple scan-until-quote loop that does NOT decode escape
// sequences, so rather than emit \" / \\ (which it would mis-parse) we replace
// the structural characters with harmless look-alikes and drop control chars.
// This keeps the document valid even for a label like: He said "go"\stop.
static std::string JsonSanitize(const char *s) {
  std::string out;
  if (!s) return out;
  for (; *s; ++s) {
    unsigned char c = (unsigned char)*s;
    if (c == '"') out += '\'';
    else if (c == '\\') out += '/';
    else if (c >= 0x20) out += (char)c;
    // control chars (< 0x20) are dropped
  }
  return out;
}

static void WriteVisual(FILE *f, const VehVisual &v) {
  fprintf(f, "    \"model\": %d,\n", v.model);
  fprintf(f, "    \"visual\": {\n");
  fprintf(f, "      \"primary\": %d, \"secondary\": %d, \"pearl\": %d, "
             "\"wheelColor\": %d,\n",
          v.primary, v.secondary, v.pearl, v.wheelColor);
  fprintf(f, "      \"customPrim\": %d, \"cp\": [%d,%d,%d],\n", v.customPrim,
          v.cpR, v.cpG, v.cpB);
  fprintf(f, "      \"customSec\": %d, \"cs\": [%d,%d,%d],\n", v.customSec, v.csR,
          v.csG, v.csB);
  fprintf(f, "      \"windowTint\": %d, \"wheelType\": %d, \"livery\": %d,\n",
          v.windowTint, v.wheelType, v.livery);
  fprintf(f, "      \"plateIdx\": %d, \"plate\": \"%s\",\n", v.plateIdx,
          JsonSanitize(v.plate).c_str());
  fprintf(f, "      \"toggleMask\": %u,\n", v.toggleMask);
  fprintf(f, "      \"mods\": [");
  for (int m = 0; m < kModSlots; ++m) fprintf(f, "%s%d", m ? "," : "", v.mods[m]);
  fprintf(f, "],\n");
  fprintf(f, "      \"modVar\": [");
  for (int m = 0; m < kModSlots; ++m) fprintf(f, "%s%d", m ? "," : "", v.modVar[m]);
  fprintf(f, "]\n    },\n");
}

static void WritePed(FILE *f, const PedVisual &p) {
  fprintf(f, "    \"pedModel\": %d,\n", p.model);
  fprintf(f, "    \"pedComp\": [");
  for (int i = 0; i < kPedComps; ++i)
    fprintf(f, "%s%d,%d,%d", i ? "," : "", p.compDraw[i], p.compTex[i],
            p.compPal[i]);
  fprintf(f, "],\n");
  fprintf(f, "    \"pedProp\": [");
  for (int i = 0; i < kPedProps; ++i)
    fprintf(f, "%s%d,%d", i ? "," : "", p.propIdx[i], p.propTex[i]);
  fprintf(f, "],\n");
}

static void WriteSamples(FILE *f, const std::vector<ClipSample> &S) {
  fprintf(f, "    \"samples\": [\n");
  for (size_t i = 0; i < S.size(); ++i) {
    const ClipSample &s = S[i];
    fprintf(f,
            "      {\"t\":%.4f,\"p\":[%.3f,%.3f,%.3f],\"q\":[%.5f,%.5f,%.5f,%.5f"
            "],\"v\":[%.3f,%.3f,%.3f],\"nw\":%d,\"w\":[",
            s.t, s.px, s.py, s.pz, s.qx, s.qy, s.qz, s.qw, s.vx, s.vy, s.vz,
            s.nWheels);
    for (int w = 0; w < s.nWheels; ++w)
      fprintf(f, "%s%.4f", w ? "," : "", s.wheel[w]);
    fprintf(f, "],\"s\":[");
    for (int w = 0; w < s.nWheels; ++w)
      fprintf(f, "%s%.4f", w ? "," : "", s.steer[w]);
    fprintf(f, "],\"sc\":[");
    for (int w = 0; w < s.nWheels; ++w)
      fprintf(f, "%s%.4f", w ? "," : "", s.susp[w]);
    fprintf(f, "]}%s\n", (i + 1 < S.size()) ? "," : "");
  }
  fprintf(f, "    ]\n");
}

// Write the active clips as a `"sampleHz"` line + `"clips":[...]` array into an
// already-open file, no enclosing braces. Shared by the standalone sidecar
// writer and the merged-sequence writer.
void VehicleClip_WriteClipsJson(FILE *f) {
  int nSave = 0;
  for (const VehClip &c : s_clips) if (c.samples.size() >= 2) ++nSave;
  fprintf(f, "  \"sampleHz\": %d,\n", g_VehicleClipSampleHz);
  fprintf(f, "  \"clips\": [\n");
  int written = 0;
  for (const VehClip &c : s_clips) {
    if (c.samples.size() < 2) continue;
    fprintf(f, "  {\n");
    fprintf(f, "    \"hasSteer\": %d,\n", c.hasSteer ? 1 : 0);
    fprintf(f, "    \"hasSusp\": %d,\n", c.hasSusp ? 1 : 0);
    fprintf(f, "    \"settings\": {\"enabled\":%d,\"speed\":%.4f,\"offset\":%.4f,"
               "\"lights\":%d,\"engine\":%d,\"siren\":%d,\"driver\":%d,"
               "\"collision\":%d,\"godmode\":%d,\"label\":\"%s\"},\n",
            c.st.enabled ? 1 : 0, c.st.speed, c.st.offset, c.st.lights,
            c.st.engineOn ? 1 : 0, c.st.siren ? 1 : 0, c.st.showDriver ? 1 : 0,
            c.st.collision ? 1 : 0, c.st.godmode ? 1 : 0,
            JsonSanitize(c.st.label).c_str());
    WriteVisual(f, c.visual);
    WritePed(f, c.ped);
    WriteSamples(f, c.samples);
    fprintf(f, "  }%s\n", (++written < nSave) ? "," : "");
  }
  fprintf(f, "  ]\n");
}

bool VehicleClip_SaveToFile(const char *path) {
  int nSave = 0;
  for (const VehClip &c : s_clips) if (c.samples.size() >= 2) ++nSave;
  if (nSave == 0) return false;
  FILE *f = nullptr;
  if (fopen_s(&f, path, "wb") != 0 || !f) return false;
  fprintf(f, "{\n  \"version\": 2,\n");
  VehicleClip_WriteClipsJson(f);
  fprintf(f, "}\n");
  fclose(f);
  return true;
}

// --- tiny JSON scanning helpers (only what our fixed schema needs) ---
static const char *JWs(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
  return p;
}
static const char *AfterKey(const char *base, const char *end, const char *key) {
  char pat[48];
  sprintf_s(pat, "\"%s\"", key);
  size_t plen = strlen(pat);
  for (const char *p = base; p + plen <= end; ++p) {
    if (strncmp(p, pat, plen) == 0) {
      p = JWs(p + plen);
      if (*p == ':') return JWs(p + 1);
      return JWs(p);
    }
  }
  return nullptr;
}
static void ParseFloatArray(const char *p, float *out, int max, int *count) {
  *count = 0;
  if (!p) return;
  p = JWs(p);
  if (*p != '[') return;
  ++p;
  for (;;) {
    p = JWs(p);
    if (*p == ']' || *p == 0) break;
    char *e = nullptr;
    double d = strtod(p, &e);
    if (e == p) break;
    if (*count < max) out[*count] = (float)d;
    ++(*count);
    p = JWs(e);
    if (*p == ',') ++p;
    else break;
  }
}
// Return the '}' matching the '{' at p (depth-aware, string-aware), or null.
static const char *MatchBrace(const char *p) {
  int depth = 0;
  bool inStr = false;
  for (; *p; ++p) {
    char c = *p;
    if (inStr) {
      if (c == '\\' && p[1]) ++p;
      else if (c == '"') inStr = false;
      continue;
    }
    if (c == '"') inStr = true;
    else if (c == '{') ++depth;
    else if (c == '}') { if (--depth == 0) return p; }
  }
  return nullptr;
}

// Parse one clip object spanning [start, end) (end points at its closing '}')
// and append it to s_clips.
static void ParseClipObject(const char *start, const char *end) {
  VehClip c{};
  c.ghost = 0;
  c.collisionOff = false;
  VehVisual &v = c.visual;

  const char *samplesKey = nullptr;
  for (const char *p = start; p + 9 <= end; ++p)
    if (strncmp(p, "\"samples\"", 9) == 0) { samplesKey = p; break; }
  const char *headerEnd = samplesKey ? samplesKey : end;

  const char *k;
  if ((k = AfterKey(start, headerEnd, "model"))) v.model = atoi(k);
  if ((k = AfterKey(start, headerEnd, "primary"))) v.primary = atoi(k);
  if ((k = AfterKey(start, headerEnd, "secondary"))) v.secondary = atoi(k);
  if ((k = AfterKey(start, headerEnd, "pearl"))) v.pearl = atoi(k);
  if ((k = AfterKey(start, headerEnd, "wheelColor"))) v.wheelColor = atoi(k);
  if ((k = AfterKey(start, headerEnd, "customPrim"))) v.customPrim = atoi(k);
  if ((k = AfterKey(start, headerEnd, "customSec"))) v.customSec = atoi(k);
  if ((k = AfterKey(start, headerEnd, "windowTint"))) v.windowTint = atoi(k);
  if ((k = AfterKey(start, headerEnd, "wheelType"))) v.wheelType = atoi(k);
  if ((k = AfterKey(start, headerEnd, "livery"))) v.livery = atoi(k);
  if ((k = AfterKey(start, headerEnd, "plateIdx"))) v.plateIdx = atoi(k);
  if ((k = AfterKey(start, headerEnd, "toggleMask")))
    v.toggleMask = (unsigned)strtoul(k, nullptr, 10);
  float tmp[4];
  int cnt;
  if ((k = AfterKey(start, headerEnd, "cp"))) {
    ParseFloatArray(k, tmp, 3, &cnt);
    if (cnt >= 3) { v.cpR = (int)tmp[0]; v.cpG = (int)tmp[1]; v.cpB = (int)tmp[2]; }
  }
  if ((k = AfterKey(start, headerEnd, "cs"))) {
    ParseFloatArray(k, tmp, 3, &cnt);
    if (cnt >= 3) { v.csR = (int)tmp[0]; v.csG = (int)tmp[1]; v.csB = (int)tmp[2]; }
  }
  if ((k = AfterKey(start, headerEnd, "plate"))) {
    k = JWs(k);
    if (*k == '"') {
      ++k;
      int n = 0;
      while (*k && *k != '"' && n < (int)sizeof(v.plate) - 1) v.plate[n++] = *k++;
      v.plate[n] = 0;
    }
  }
  float modsF[kModSlots];
  if ((k = AfterKey(start, headerEnd, "mods"))) {
    ParseFloatArray(k, modsF, kModSlots, &cnt);
    for (int m = 0; m < kModSlots; ++m) v.mods[m] = (m < cnt) ? (int)modsF[m] : -1;
  }
  if ((k = AfterKey(start, headerEnd, "modVar"))) {
    ParseFloatArray(k, modsF, kModSlots, &cnt);
    for (int m = 0; m < kModSlots; ++m) v.modVar[m] = (m < cnt) ? (int)modsF[m] : 0;
  }
  v.valid = (v.model != 0);

  // Driver ped (optional — absent in older clips / clips with no driver).
  PedVisual &pv = c.ped;
  if ((k = AfterKey(start, headerEnd, "pedModel"))) pv.model = atoi(k);
  float pedComp[kPedComps * 3];
  if ((k = AfterKey(start, headerEnd, "pedComp"))) {
    ParseFloatArray(k, pedComp, kPedComps * 3, &cnt);
    for (int i = 0; i < kPedComps; ++i) {
      pv.compDraw[i] = (i * 3 + 0 < cnt) ? (int)pedComp[i * 3 + 0] : 0;
      pv.compTex[i] = (i * 3 + 1 < cnt) ? (int)pedComp[i * 3 + 1] : 0;
      pv.compPal[i] = (i * 3 + 2 < cnt) ? (int)pedComp[i * 3 + 2] : 0;
    }
  }
  float pedProp[kPedProps * 2];
  if ((k = AfterKey(start, headerEnd, "pedProp"))) {
    ParseFloatArray(k, pedProp, kPedProps * 2, &cnt);
    for (int i = 0; i < kPedProps; ++i) {
      pv.propIdx[i] = (i * 2 + 0 < cnt) ? (int)pedProp[i * 2 + 0] : -1;
      pv.propTex[i] = (i * 2 + 1 < cnt) ? (int)pedProp[i * 2 + 1] : 0;
    }
  }
  pv.valid = (pv.model != 0);

  if ((k = AfterKey(start, headerEnd, "hasSteer"))) c.hasSteer = atoi(k) != 0;
  if ((k = AfterKey(start, headerEnd, "hasSusp"))) c.hasSusp = atoi(k) != 0;

  // Per-clip settings (optional — older clips default from the globals).
  c.st = DefaultClipSettings();
  if ((k = AfterKey(start, headerEnd, "enabled")))   c.st.enabled = atoi(k) != 0;
  if ((k = AfterKey(start, headerEnd, "speed")))     c.st.speed = (float)atof(k);
  if ((k = AfterKey(start, headerEnd, "offset")))    c.st.offset = (float)atof(k);
  if ((k = AfterKey(start, headerEnd, "lights")))    c.st.lights = atoi(k);
  if ((k = AfterKey(start, headerEnd, "engine")))    c.st.engineOn = atoi(k) != 0;
  if ((k = AfterKey(start, headerEnd, "siren")))     c.st.siren = atoi(k) != 0;
  if ((k = AfterKey(start, headerEnd, "driver")))    c.st.showDriver = atoi(k) != 0;
  if ((k = AfterKey(start, headerEnd, "collision"))) c.st.collision = atoi(k) != 0;
  if ((k = AfterKey(start, headerEnd, "godmode")))   c.st.godmode = atoi(k) != 0;
  if (c.st.speed < 0.05f) c.st.speed = 1.0f; // guard against bad/old values
  if ((k = AfterKey(start, headerEnd, "label"))) {
    k = JWs(k);
    if (*k == '"') {
      ++k;
      int n = 0;
      while (*k && *k != '"' && n < (int)sizeof(c.st.label) - 1) c.st.label[n++] = *k++;
      c.st.label[n] = 0;
    }
  }

  // Samples: each {...} object inside the clip's samples array (no nested
  // braces in a sample, so a plain '}' scan terminates each one).
  if (samplesKey) {
    const char *p = strchr(samplesKey, '[');
    if (p && p < end) {
      ++p;
      while (p < end) {
        p = JWs(p);
        if (*p == ']' || *p == 0 || p >= end) break;
        if (*p != '{') { ++p; continue; }
        const char *objEnd = strchr(p, '}');
        if (!objEnd || objEnd >= end) break;
        ClipSample s{};
        const char *kk;
        if ((kk = AfterKey(p, objEnd, "t"))) s.t = (float)atof(kk);
        if ((kk = AfterKey(p, objEnd, "p"))) {
          ParseFloatArray(kk, tmp, 3, &cnt);
          if (cnt >= 3) { s.px = tmp[0]; s.py = tmp[1]; s.pz = tmp[2]; }
        }
        float q[4];
        if ((kk = AfterKey(p, objEnd, "q"))) {
          ParseFloatArray(kk, q, 4, &cnt);
          if (cnt >= 4) { s.qx = q[0]; s.qy = q[1]; s.qz = q[2]; s.qw = q[3]; }
        }
        if ((kk = AfterKey(p, objEnd, "v"))) {
          ParseFloatArray(kk, tmp, 3, &cnt);
          if (cnt >= 3) { s.vx = tmp[0]; s.vy = tmp[1]; s.vz = tmp[2]; }
        }
        if ((kk = AfterKey(p, objEnd, "nw"))) {
          s.nWheels = atoi(kk);
          // Clamp unconditionally: a malformed sample can carry "nw" without a
          // matching "w" array, and nWheels later drives fixed-size stack arrays
          // (wheel[]/steer[] are only kMaxWheels) in ApplyClipAtTime.
          if (s.nWheels < 0) s.nWheels = 0;
          if (s.nWheels > VehMem::kMaxWheels) s.nWheels = VehMem::kMaxWheels;
        }
        if ((kk = AfterKey(p, objEnd, "w"))) {
          ParseFloatArray(kk, s.wheel, VehMem::kMaxWheels, &cnt);
          if (s.nWheels > cnt) s.nWheels = cnt;
          if (s.nWheels > VehMem::kMaxWheels) s.nWheels = VehMem::kMaxWheels;
        }
        if ((kk = AfterKey(p, objEnd, "s"))) {
          float st[VehMem::kMaxWheels];
          int sc;
          ParseFloatArray(kk, st, VehMem::kMaxWheels, &sc);
          for (int w = 0; w < VehMem::kMaxWheels; ++w)
            s.steer[w] = (w < sc) ? st[w] : 0.0f;
        }
        if ((kk = AfterKey(p, objEnd, "sc"))) {
          float sp[VehMem::kMaxWheels];
          int spc;
          ParseFloatArray(kk, sp, VehMem::kMaxWheels, &spc);
          for (int w = 0; w < VehMem::kMaxWheels; ++w)
            s.susp[w] = (w < spc) ? sp[w] : 0.0f;
        }
        c.samples.push_back(s);
        p = objEnd + 1;
      }
    }
  }

  if (c.samples.size() >= 2) {
    c.ghostPending = (v.model != 0);
    s_clips.push_back(c);
  }
}

// Parse the `"clips"` array (or a v1 single-clip document) out of an in-memory
// buffer, replacing the runtime clips. Also used for the embedded clips inside a
// merged sequence JSON.
bool VehicleClip_ParseClipsFromBuffer(const char *data, int len) {
  const char *end = data + len;
  VehicleClip_Clear();

  const char *k = AfterKey(data, end, "sampleHz");
  if (k) g_VehicleClipSampleHz = atoi(k);

  const char *clipsKey = strstr(data, "\"clips\"");
  if (clipsKey) {
    const char *p = strchr(clipsKey, '[');
    if (p) {
      ++p;
      while (p < end) {
        p = JWs(p);
        if (*p == ']' || *p == 0) break;
        if (*p != '{') { ++p; continue; }
        const char *objEnd = MatchBrace(p);
        if (!objEnd) break;
        ParseClipObject(p, objEnd);
        p = objEnd + 1;
      }
    }
  } else if (strstr(data, "\"samples\"")) {
    // v1 backward-compat: a single clip at the document root.
    ParseClipObject(data, end);
  }
  return !s_clips.empty();
}

bool VehicleClip_LoadFromFile(const char *path) {
  FILE *f = nullptr;
  if (fopen_s(&f, path, "rb") != 0 || !f) return false;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (len <= 0) { fclose(f); return false; }
  std::vector<char> buf((size_t)len + 1);
  size_t rd = fread(buf.data(), 1, (size_t)len, f);
  fclose(f);
  buf[rd] = 0;
  return VehicleClip_ParseClipsFromBuffer(buf.data(), (int)rd);
}

// ============================================================
//  State / data
// ============================================================

bool VehicleClip_HasData() {
  for (const VehClip &c : s_clips) if (c.samples.size() >= 2) return true;
  return false;
}

float VehicleClip_Duration() {
  float maxD = s_recording ? s_recTime : 0.0f;
  for (const VehClip &c : s_clips) {
    if (!c.st.enabled) continue;
    float sp = (c.st.speed > 0.0001f) ? c.st.speed : 1.0f;
    float d = c.st.offset + ClipDuration(c.samples) / sp; // offset + remapped len
    if (d > maxD) maxD = d;
  }
  return maxD;
}

bool VehicleClip_GetSettings(int index, VehicleClipSettings *out) {
  if (!out || index < 0 || index >= (int)s_clips.size()) return false;
  *out = s_clips[index].st;
  return true;
}

void VehicleClip_SetSettings(int index, const VehicleClipSettings *in) {
  if (!in || index < 0 || index >= (int)s_clips.size()) return;
  s_clips[index].st = *in;
  // Guard speed like the load path does — a 0 (or negative) multiplier freezes
  // the ghost at its first sample forever (localT = (t-offset)*speed == 0).
  if (s_clips[index].st.speed < 0.05f) s_clips[index].st.speed = 1.0f;
}

void VehicleClip_GetLabel(int index, char *out, int outLen) {
  if (!out || outLen <= 0) return;
  out[0] = '\0';
  if (index < 0 || index >= (int)s_clips.size()) return;
  const VehClip &c = s_clips[index];
  if (c.st.label[0]) {
    strncpy_s(out, outLen, c.st.label, _TRUNCATE);
    return;
  }
  // No custom label: default to the recorded vehicle's model name ("SULTAN"),
  // numbered so several clips of the same car stay distinguishable.
  const char *nm = nullptr;
  if (c.visual.model != 0) {
    nm = VEHICLE::GET_DISPLAY_NAME_FROM_VEHICLE_MODEL((Hash)c.visual.model);
    // The native answers "CARNOTFOUND" for hashes it can't resolve.
    if (nm && (!nm[0] || strcmp(nm, "CARNOTFOUND") == 0)) nm = nullptr;
  }
  if (nm) sprintf_s(out, outLen, "%d. %s", index + 1, nm);
  else sprintf_s(out, outLen, "Vehicle %d", index + 1);
}

int VehicleClip_SampleCount() {
  int n = (int)s_recSamples.size();
  for (const VehClip &c : s_clips) n += (int)c.samples.size();
  return n;
}

int VehicleClip_Count() { return (int)s_clips.size(); }

void VehicleClip_DeleteClip(int index) {
  if (index < 0 || index >= (int)s_clips.size()) return;
  DespawnGhostFor(s_clips[index]);
  s_clips.erase(s_clips.begin() + index);
}

void VehicleClip_Clear() {
  VehicleClip_DespawnGhost();
  s_clips.clear();
  s_recording = false;
  s_recSamples.clear();
  s_recVehicle = 0;
  s_recTime = 0.0f;
  s_lastSampleT = 0.0f;
}

bool VehicleClip_Enabled() { return s_enabled; }

void VehicleClip_SetEnabled(bool on) {
  s_enabled = on;
  if (!on) VehicleClip_Release();
}

int VehicleClip_VehicleHandle() {
  for (const VehClip &c : s_clips)
    if (c.ghost != 0 && VehicleValid(c.ghost)) return c.ghost;
  return VehicleValid(s_recVehicle) ? s_recVehicle : 0;
}

int VehicleClip_GhostAt(int index) {
  if (index < 0 || index >= (int)s_clips.size()) return 0;
  const VehClip &c = s_clips[index];
  return (c.ghost != 0 && VehicleValid(c.ghost)) ? c.ghost : 0;
}

bool VehicleClip_OwnsEntity(int handle) {
  if (handle == 0) return false;
  if (s_recVehicle != 0 && handle == s_recVehicle) return true;
  for (const VehClip &c : s_clips)
    if (handle == c.ghost || handle == c.driver) return true;
  return false;
}
