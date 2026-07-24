/*
        GTA V Free Camera / Photo Mode Plugin
        Camera Sequence Mode — implementation
*/

#include "sequence.h"

#include "camera.h"
#include "keyboard.h"
#include "vehicleclip.h"

#include "external\scripthook_sdk\inc\main.h"
#include "external\scripthook_sdk\inc\natives.h"
#include "external\scripthook_sdk\inc\types.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>

#pragma warning(disable : 4244 4305)

// ============================================================
//  Constants / helpers
// ============================================================

static const float PI_F = 3.14159265358979323846f;
static const float DEG2RAD_F = PI_F / 180.0f;
static const float RAD2DEG_F = 180.0f / PI_F;

static float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// Shortest-arc angle lerp in degrees. Handles wrap-around so that
// going from 170° → -170° travels 20° (through 180), not 340°.
static float lerpAngle(float a, float b, float t) {
  float d = b - a;
  while (d > 180.0f) d -= 360.0f;
  while (d < -180.0f) d += 360.0f;
  return a + d * t;
}

const char *EaseName(EaseType e) {
  switch (e) {
  case EASE_LINEAR: return "Linear";
  case EASE_IN_OUT: return "EaseInOut";
  case EASE_IN:     return "EaseIn";
  case EASE_OUT:    return "EaseOut";
  case EASE_HOLD:   return "Hold";
  }
  return "?";
}

const char *PathName(PathType p) {
  return p == PATH_SPLINE ? "Spline" : "Linear";
}

const char *EffectName(EffectKind k) {
  switch (k) {
  case EFX_SHAKE_ENABLED:     return "Shake On/Off";
  case EFX_SHAKE_PRESET:      return "Shake Preset";
  case EFX_SHAKE_AMP:         return "Shake Amp";
  case EFX_SHAKE_FREQ:        return "Shake Freq";
  case EFX_SHAKE_SPEED_AMP:   return "Speed -> Amp";
  case EFX_SHAKE_SPEED_FREQ:  return "Speed -> Freq";
  case EFX_SHAKE_ROT_WEIGHT:  return "Rotation Weight";
  case EFX_SHAKE_POS_WEIGHT:  return "Position Weight";
  case EFX_SHAKE_STOP_STILL:  return "Stop When Still";
  case EFX_SHAKE_RANDOMIZE:   return "Randomize Pattern";
  default:                    return "(unsupported)";
  }
}

// ============================================================
//  Module state
// ============================================================

int g_SeqHotkeyAdd = VK_F6;
int g_SeqHotkeyPlay = VK_F7;
int g_SeqHotkeyStop = VK_F8;
int g_SeqHotkeyNext = VK_F9;
bool g_SequenceShowMarkers = true;
// Marker / path appearance (overridable via the Appearance menu + INI).
int g_SeqMarkerR = 80, g_SeqMarkerG = 200, g_SeqMarkerB = 80;
float g_SeqMarkerSize = 0.30f;

// Path timestamp interval, shared by the camera-path and vehicle-path time
// labels. Stored as an index into {Off, 0.5s, 1s, 2s, 5s} (so it maps cleanly to
// an Appearance list control); SeqTimeLabelStep() turns it into seconds, 0=off.
int g_SeqTimeLabelMode = 2;
float SeqTimeLabelStep() {
  static const float steps[] = {0.0f, 0.5f, 1.0f, 2.0f, 5.0f};
  int m = g_SeqTimeLabelMode;
  if (m < 0 || m > 4) m = 2;
  return steps[m];
}
int g_SeqPathR = 100, g_SeqPathG = 180, g_SeqPathB = 255;
static int s_EditingPoseIdx = -1;

// Snapshot of the user's shake configuration captured the moment Play
// starts. Effect events mutate the shake globals during playback; on
// Stop / end-of-playback we restore from this snapshot so the user's
// authoring-time shake settings come back unchanged. Without this,
// the camera would keep shaking forever after the sequence ends.
//
// Noise seeds and pattern multipliers are intentionally NOT included —
// the user typically wants any Randomize firing to leave a fresh
// pattern in place.
struct ShakeSnapshot {
  bool enabled;
  int preset;
  float amp, freq;
  float spdAmp, spdFreq;
  float rotW, posW;
  bool stopStill;
};
static ShakeSnapshot s_PreplayShake;
static bool s_HasShakeSnapshot = false;

static void SaveShakeSnapshot() {
  s_PreplayShake.enabled = g_ShakeEnabled;
  s_PreplayShake.preset = g_ShakePreset;
  s_PreplayShake.amp = g_ShakeAmp;
  s_PreplayShake.freq = g_ShakeFreq;
  s_PreplayShake.spdAmp = g_ShakeSpeedAmpCoupling;
  s_PreplayShake.spdFreq = g_ShakeSpeedFreqCoupling;
  s_PreplayShake.rotW = g_ShakeRotWeight;
  s_PreplayShake.posW = g_ShakePosWeight;
  s_PreplayShake.stopStill = g_ShakeStopWhenStill;
  s_HasShakeSnapshot = true;
}

static void RestoreShakeSnapshot() {
  if (!s_HasShakeSnapshot) return;
  g_ShakeEnabled = s_PreplayShake.enabled;
  g_ShakePreset = s_PreplayShake.preset;
  g_ShakeAmp = s_PreplayShake.amp;
  g_ShakeFreq = s_PreplayShake.freq;
  g_ShakeSpeedAmpCoupling = s_PreplayShake.spdAmp;
  g_ShakeSpeedFreqCoupling = s_PreplayShake.spdFreq;
  g_ShakeRotWeight = s_PreplayShake.rotW;
  g_ShakePosWeight = s_PreplayShake.posW;
  g_ShakeStopWhenStill = s_PreplayShake.stopStill;
  s_HasShakeSnapshot = false;
  // Time scale is no longer touched by sequence events — it's owned entirely
  // by the World & Scene slow-motion control (UpdateGlobalEffects), which
  // restores real-time on its own when the slider is back at 1.0.
}

void Sequence_SetEditingPose(int idx) { s_EditingPoseIdx = idx; }
int Sequence_GetEditingPose() { return s_EditingPoseIdx; }

static std::vector<CameraSequence> s_Sequences;
static int s_ActiveIdx = -1;
static bool s_InMode = false;
static bool s_Playing = false;
static bool s_FirstPlayTick = false; // widen event window on the very first tick
static float s_PlaybackTime = 0.0f;
static float s_LastTickTime = 0.0f; // for event cross-detection
static DWORD s_LastFrameTick = 0;

// We forward the playback override to the camera via this flag so
// UpdateFreeCamera (if it runs in some edge case) won't fight us.
// Also used so the sequence mode menu / authoring code knows.
extern bool g_SequencePlaybackActive; // defined in camera.cpp

// Forward decl — defined later, used by Sequence_SetCurrentTime for
// scrub-preview pose application.
static void ApplyPoseAtTime(float t, const CameraSequence *s);

// Forward decl — defined alongside event firing, used by Sequence_Play
// for catch-up when starting playback mid-sequence.
static void ApplyEffectValue(EffectKind kind, float value);

// Forward decls — defined in the persistence section. Each sequence is a single
// merged JSON file (keyframes + events + settings + its vehicle clips).
static void SaveSequenceJson(int idx);
static void LoadActiveSequenceClips();
static void GetSeqJsonPath(const char *seqName, char *outPath, size_t bufSize);

// ============================================================
//  Easing
// ============================================================

static float ApplyEase(EaseType e, float t) {
  t = clampf(t, 0.0f, 1.0f);
  switch (e) {
  case EASE_LINEAR:
    return t;
  case EASE_IN_OUT:
    // Smoothstep (cubic Hermite): 3t^2 - 2t^3
    return t * t * (3.0f - 2.0f * t);
  case EASE_IN:
    // Quadratic ease-in
    return t * t;
  case EASE_OUT:
    // Quadratic ease-out
    return 1.0f - (1.0f - t) * (1.0f - t);
  case EASE_HOLD:
    // Stay at 0 until t reaches 1, then snap
    return t >= 1.0f ? 1.0f : 0.0f;
  }
  return t;
}

// ============================================================
//  Quaternion slerp for rotation interpolation
//  (Uses the same Euler convention as camera.cpp's Quat helpers)
// ============================================================

struct SQuat {
  float w, x, y, z;
};

static SQuat EulerToSQuat(float pitchDeg, float yawDeg, float rollDeg) {
  // Match the convention used in camera.cpp:EulerToQuat — ZXY order
  // (yaw about Z, pitch about X, roll about Y), with yaw negated.
  float halfYaw = -yawDeg * DEG2RAD_F * 0.5f;
  float halfPitch = pitchDeg * DEG2RAD_F * 0.5f;
  float halfRoll = rollDeg * DEG2RAD_F * 0.5f;
  SQuat qYaw   = {cosf(halfYaw),   0.0f,             0.0f,           sinf(halfYaw)};
  SQuat qPitch = {cosf(halfPitch), sinf(halfPitch),  0.0f,           0.0f};
  SQuat qRoll  = {cosf(halfRoll),  0.0f,             sinf(halfRoll), 0.0f};
  // qYaw * qPitch
  SQuat qa;
  qa.w = qYaw.w*qPitch.w - qYaw.x*qPitch.x - qYaw.y*qPitch.y - qYaw.z*qPitch.z;
  qa.x = qYaw.w*qPitch.x + qYaw.x*qPitch.w + qYaw.y*qPitch.z - qYaw.z*qPitch.y;
  qa.y = qYaw.w*qPitch.y - qYaw.x*qPitch.z + qYaw.y*qPitch.w + qYaw.z*qPitch.x;
  qa.z = qYaw.w*qPitch.z + qYaw.x*qPitch.y - qYaw.y*qPitch.x + qYaw.z*qPitch.w;
  // qa * qRoll
  SQuat r;
  r.w = qa.w*qRoll.w - qa.x*qRoll.x - qa.y*qRoll.y - qa.z*qRoll.z;
  r.x = qa.w*qRoll.x + qa.x*qRoll.w + qa.y*qRoll.z - qa.z*qRoll.y;
  r.y = qa.w*qRoll.y - qa.x*qRoll.z + qa.y*qRoll.w + qa.z*qRoll.x;
  r.z = qa.w*qRoll.z + qa.x*qRoll.y - qa.y*qRoll.x + qa.z*qRoll.w;
  return r;
}

static void SQuatToEuler(const SQuat &q, float &pitchOut, float &yawOut,
                         float &rollOut) {
  // Inverse of EulerToSQuat (ZXY). Lifted from camera.cpp:QuatToEuler.
  float m12 = 2.0f * (q.x * q.y - q.z * q.w);
  float m22 = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
  float m31 = 2.0f * (q.x * q.z - q.y * q.w);
  float m32 = 2.0f * (q.y * q.z + q.x * q.w);
  float m33 = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);

  pitchOut = asinf(clampf(m32, -1.0f, 1.0f)) * RAD2DEG_F;
  if (fabsf(m32) < 0.999f) {
    yawOut = atan2f(-m12, m22) * RAD2DEG_F;
    rollOut = atan2f(-m31, m33) * RAD2DEG_F;
  } else {
    yawOut = 0.0f;
    rollOut = 0.0f;
  }
}

// Hamilton product. Same maths as the inline composition in
// EulerToSQuat — extracted here so the entity-rotation-lock paths can
// build rotations by composing pre-built quats without duplicating it.
static SQuat QuatMul(const SQuat &a, const SQuat &b) {
  SQuat r;
  r.w = a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z;
  r.x = a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y;
  r.y = a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x;
  r.z = a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w;
  return r;
}

// Inverse of a UNIT quaternion is its conjugate. Our quats come from
// EulerToSQuat which produces unit-length results, so we can skip the
// /norm step. Used to flip an "object orientation" rotation into the
// world-to-object frame change needed for the rotation-lock maths.
static SQuat QuatConj(const SQuat &q) {
  return {q.w, -q.x, -q.y, -q.z};
}

// Wrap GTA's ENTITY::GET_ENTITY_ROTATION(entity, 2) — returns a Vector3
// with .x = pitch, .y = roll, .z = yaw — into the EulerToSQuat
// convention that the rest of this file uses. Putting the axis re-
// shuffle in one place avoids accidentally mixing them up at the
// (lots of) call sites that need an entity's quat.
static SQuat EntityRotationQuat(int entity) {
  Vector3 r = ENTITY::GET_ENTITY_ROTATION(entity, 2);
  return EulerToSQuat(r.x, r.z, r.y);
}

static SQuat Slerp(SQuat a, SQuat b, float t) {
  float dot = a.w*b.w + a.x*b.x + a.y*b.y + a.z*b.z;
  if (dot < 0.0f) {
    b.w = -b.w; b.x = -b.x; b.y = -b.y; b.z = -b.z;
    dot = -dot;
  }
  if (dot > 0.9995f) {
    // Nearly identical → linear lerp + renormalize
    SQuat r = {a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t,
               a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
    float len = sqrtf(r.w*r.w + r.x*r.x + r.y*r.y + r.z*r.z);
    if (len > 0.00001f) { r.w/=len; r.x/=len; r.y/=len; r.z/=len; }
    return r;
  }
  float theta = acosf(dot);
  float sinTheta = sinf(theta);
  float wa = sinf((1.0f - t) * theta) / sinTheta;
  float wb = sinf(t * theta) / sinTheta;
  return {a.w*wa + b.w*wb, a.x*wa + b.x*wb,
          a.y*wa + b.y*wb, a.z*wa + b.z*wb};
}

// ============================================================
//  Catmull-Rom position spline (kept for reference; superseded by
//  HermiteSpline1D below which handles non-uniform time spacing).
// ============================================================

static float CatmullRom1D(float p0, float p1, float p2, float p3, float t) {
  float t2 = t * t;
  float t3 = t2 * t;
  return 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f*p0 - 5.0f*p1 + 4.0f*p2 - p3) * t2 +
                 (-p0 + 3.0f*p1 - 3.0f*p2 + p3) * t3);
}

// ============================================================
//  Time-aware cubic Hermite spline
// ============================================================
//
// Given 4 control points (p0..p3) at times (t0..t3) and a query time t
// in [t1, t2], compute a smooth interpolated value. The tangents at p1
// and p2 are finite-difference slopes weighted by the actual time
// spacing — so the tangent at p1 reaching one segment FROM behind
// equals the tangent leaving INTO the next segment. Result: velocity
// is C1-continuous across keyframes even when segment durations vary.
//
// Catmull-Rom (uniform parameter) only achieves position continuity
// when all segments have equal duration; with mixed durations the
// camera visibly jerks at keyframes. Hermite with time-weighted slopes
// is the standard fix.
static float HermiteSpline1D(float p0, float p1, float p2, float p3,
                             float t0, float t1, float t2, float t3,
                             float t) {
  float du = t2 - t1;
  if (du < 0.0001f) return p1;
  float dt02 = t2 - t0;
  float dt13 = t3 - t1;
  float m1 = (dt02 > 0.0001f) ? (p2 - p0) / dt02 : 0.0f;
  float m2 = (dt13 > 0.0001f) ? (p3 - p1) / dt13 : 0.0f;
  float u = (t - t1) / du;
  if (u < 0.0f) u = 0.0f;
  if (u > 1.0f) u = 1.0f;
  float u2 = u * u;
  float u3 = u2 * u;
  float h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
  float h10 = u3 - 2.0f * u2 + u;
  float h01 = -2.0f * u3 + 3.0f * u2;
  float h11 = u3 - u2;
  return h00 * p1 + h10 * du * m1 + h01 * p2 + h11 * du * m2;
}

// Unwrap `angle` so it's within ±180° of `reference`. Used so Hermite
// rotation interpolation takes the short way around when angles cross
// the ±180 seam.
static float unwrapAngle(float angle, float reference) {
  while (angle - reference > 180.0f) angle -= 360.0f;
  while (angle - reference < -180.0f) angle += 360.0f;
  return angle;
}

// ============================================================
//  Loop closure — tolerances + detector + cyclic neighbor lookup
// ============================================================

// Tuned by feel: a 1m gap is small enough that the eye doesn't catch
// it at the wrap, 5° is below the threshold most users notice for
// rotation continuity, 2° FOV is roughly one click in the in-game menu.
static const float kLoopPosTolerance = 1.0f;
static const float kLoopRotTolerance = 5.0f;
static const float kLoopFovTolerance = 2.0f;

static bool IsLoopClosedImpl(const CameraSequence *s) {
  if (!s || !s->loop || s->poses.size() < 2) return false;
  const PoseKeyframe &a = s->poses.front();
  const PoseKeyframe &b = s->poses.back();
  float dx = b.posX - a.posX, dy = b.posY - a.posY, dz = b.posZ - a.posZ;
  if (sqrtf(dx*dx + dy*dy + dz*dz) > kLoopPosTolerance) return false;
  // unwrapAngle returns b's angle re-expressed within ±180 of a, so the
  // delta is the shortest-arc difference.
  if (fabsf(unwrapAngle(b.pitch, a.pitch) - a.pitch) > kLoopRotTolerance) return false;
  if (fabsf(unwrapAngle(b.yaw,   a.yaw)   - a.yaw)   > kLoopRotTolerance) return false;
  if (fabsf(unwrapAngle(b.roll,  a.roll)  - a.roll)  > kLoopRotTolerance) return false;
  if (fabsf(b.fov - a.fov) > kLoopFovTolerance) return false;
  return true;
}

bool Sequence_IsLoopClosed() { return IsLoopClosedImpl(Sequence_Active()); }

bool Sequence_GetLoopGap(LoopGap *out) {
  if (!out) return false;
  CameraSequence *s = Sequence_Active();
  if (!s || s->poses.size() < 2) return false;
  const PoseKeyframe &a = s->poses.front();
  const PoseKeyframe &b = s->poses.back();
  float dx = b.posX - a.posX, dy = b.posY - a.posY, dz = b.posZ - a.posZ;
  out->posDist = sqrtf(dx*dx + dy*dy + dz*dz);
  out->pitchDelta = fabsf(unwrapAngle(b.pitch, a.pitch) - a.pitch);
  out->yawDelta   = fabsf(unwrapAngle(b.yaw,   a.yaw)   - a.yaw);
  out->rollDelta  = fabsf(unwrapAngle(b.roll,  a.roll)  - a.roll);
  out->fovDelta   = fabsf(b.fov - a.fov);
  return true;
}

// Resolves a control-point index for the spline tangent calc. For
// loop-closed sequences, indices wrap cyclically over poses[0..N-2]
// (the canonical loop — poses[N-1] is a duplicate of poses[0]) and
// the returned tOffset shifts the control point's time value by
// ±duration so time-aware Hermite slopes stay correct across the seam.
// For non-closed sequences this just clamps to [0, N-1] with zero offset.
struct CtrlRef {
  int idx;
  float tOffset;
};

static CtrlRef ResolveCtrl(int idx, int N, bool closed, float dur) {
  CtrlRef r{idx, 0.0f};
  if (!closed) {
    if (r.idx < 0) r.idx = 0;
    if (r.idx >= N) r.idx = N - 1;
    return r;
  }
  int eff = N - 1; // unique poses in the cycle
  if (eff < 1) { r.idx = 0; return r; }
  while (r.idx < 0)    { r.idx += eff; r.tOffset -= dur; }
  while (r.idx >= eff) { r.idx -= eff; r.tOffset += dur; }
  return r;
}

// Resolves a control-point's pos/rot/fov values. When closed and the
// resolved index lands on the seam duplicate (N-1), substitutes pose[0]'s
// values — that's what makes the wrap bit-exact even if the user hasn't
// hard-snapped the last keyframe yet. Returns a copy by value so the
// caller can read .posX / .pitch / .fov / etc. directly without further
// indirection.
static PoseKeyframe ResolvePose(const CameraSequence *s, int idx,
                                bool closed) {
  int N = (int)s->poses.size();
  PoseKeyframe p = s->poses[idx];
  if (closed && idx == N - 1) {
    const PoseKeyframe &q = s->poses[0];
    p.posX = q.posX; p.posY = q.posY; p.posZ = q.posZ;
    p.pitch = q.pitch; p.yaw = q.yaw; p.roll = q.roll;
    p.fov = q.fov;
    // Mirror the lock state too so the seam stays anchored to the same
    // entity as pose[0]. CloseLoop already syncs these for stored poses,
    // but live cyclic resolution may hit this path before/without a
    // CloseLoop having run, so do it defensively.
    p.entityHandle = q.entityHandle;
    p.localOffsetX = q.localOffsetX;
    p.localOffsetY = q.localOffsetY;
    p.localOffsetZ = q.localOffsetZ;
    p.lockEntPitch = q.lockEntPitch;
    p.lockEntYaw   = q.lockEntYaw;
    p.lockEntRoll  = q.lockEntRoll;
    p.lockEntPosX  = q.lockEntPosX;
    p.lockEntPosY  = q.lockEntPosY;
    p.lockEntPosZ  = q.lockEntPosZ;
  }
  // Entity lock: if this keyframe was authored riding along with an
  // entity that still exists, recompute BOTH its world-space position
  // and rotation from the stored entity-relative state. We mutate the
  // returned copy so all downstream consumers (spline tangent calc,
  // marker preview, linear lerp) treat it uniformly as a world-space
  // pose. The original world-space fields stored on the keyframe stay
  // intact on disk — they're the fallback used when the locked entity
  // is gone (DOES_ENTITY_EXIST returns FALSE for stale or never-existed
  // handles, so this branch simply skips and the stored coords remain).
  if (p.entityHandle != 0 && ENTITY::DOES_ENTITY_EXIST(p.entityHandle)) {
    Vector3 entNowPos = ENTITY::GET_ENTITY_COORDS(p.entityHandle, TRUE);

    // Entity lock has two modes. The critical rule: ROTATION may only turn with
    // the car when the POSITION also orbits it. Rotating the view without
    // orbiting points the camera away from the car (the car drifts out of
    // frame) — so rotation-with-car lives strictly inside the rigid branch.
    if (g_FollowRigidMode) {
      // ---- Full rigid mount (orbit + rotate) ----
      // Position: re-expand the stored local offset by the car's CURRENT
      // rotation (GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS), so the camera swings
      // around the car as it turns — bolted on like a hood cam.
      Vector3 w = invoke<Vector3>(0x1899F328B0E12848, p.entityHandle,
                                  p.localOffsetX, p.localOffsetY,
                                  p.localOffsetZ);
      p.posX = w.x;
      p.posY = w.y;
      p.posZ = w.z;

      // Rotation: add the car's rotation CHANGE since lock to the authored
      // angles. Paired with the orbit above, this keeps the car PRECISELY
      // framed as it turns — the camera's view relative to the car is
      // preserved. Camera and entity share GTA's world Euler convention
      // (rotation order 2), so it's a plain delta, no quaternions.
      // GET_ENTITY_ROTATION order-2: .x = pitch, .y = roll, .z = yaw.
      Vector3 entNowRot = ENTITY::GET_ENTITY_ROTATION(p.entityHandle, 2);
      float dPitch = entNowRot.x - p.lockEntPitch;
      float dRoll  = entNowRot.y - p.lockEntRoll;
      float dYaw   = entNowRot.z - p.lockEntYaw;
      // Shortest-arc each delta so a 179 -> -179 wrap reads as +2, not -358.
      while (dPitch > 180.0f) dPitch -= 360.0f;
      while (dPitch <= -180.0f) dPitch += 360.0f;
      while (dRoll > 180.0f) dRoll -= 360.0f;
      while (dRoll <= -180.0f) dRoll += 360.0f;
      while (dYaw > 180.0f) dYaw -= 360.0f;
      while (dYaw <= -180.0f) dYaw += 360.0f;
      p.pitch += dPitch;
      p.roll  += dRoll;
      p.yaw   += dYaw;
    } else {
      // ---- Translate-only (move with the car, keep world rotation) ----
      // The keyframe rides the car down the road by however far it has
      // TRANSLATED since lock, but KEEPS its authored world pitch/yaw/roll. We
      // deliberately do NOT rotate the view with the car: without an orbit,
      // rotating would swing the camera off the car. "Keyframes only move with
      // the entity." (A look-at "eyesight lock" that keeps the car framed from
      // a fixed spot is a separate planned mode.)
      p.posX += entNowPos.x - p.lockEntPosX;
      p.posY += entNowPos.y - p.lockEntPosY;
      p.posZ += entNowPos.z - p.lockEntPosZ;
    }
  }
  return p;
}

// ============================================================
//  Sequence accessors
// ============================================================

static CameraSequence *EnsureActive() {
  if (s_Sequences.empty()) {
    CameraSequence s;
    s.name = "Default";
    s.loop = false;
    s.playbackSpeed = 1.0f;
    s_Sequences.push_back(s);
    s_ActiveIdx = 0;
  } else if (s_ActiveIdx < 0 || s_ActiveIdx >= (int)s_Sequences.size()) {
    s_ActiveIdx = 0;
  }
  return &s_Sequences[s_ActiveIdx];
}

CameraSequence *Sequence_Active() {
  if (s_Sequences.empty() || s_ActiveIdx < 0 ||
      s_ActiveIdx >= (int)s_Sequences.size())
    return nullptr;
  return &s_Sequences[s_ActiveIdx];
}

int Sequence_ActiveIndex() { return s_ActiveIdx; }
int Sequence_Count() { return (int)s_Sequences.size(); }

CameraSequence *Sequence_At(int index) {
  if (index < 0 || index >= (int)s_Sequences.size())
    return nullptr;
  return &s_Sequences[index];
}

void Sequence_SetActive(int index) {
  if (index >= 0 && index < (int)s_Sequences.size()) {
    if (index == s_ActiveIdx) { Sequence_Stop(); return; }
    // Persist the outgoing sequence (keyframes + its clips) to its JSON, then
    // swap in the new one and load its clips.
    SaveSequenceJson(s_ActiveIdx);
    VehicleClip_DespawnGhost();
    s_ActiveIdx = index;
    Sequence_Stop();
    LoadActiveSequenceClips();
  }
}

void Sequence_New(const char *name) {
  // Persist the current sequence (incl. its clips) before switching away.
  SaveSequenceJson(s_ActiveIdx);
  VehicleClip_DespawnGhost();
  CameraSequence s;
  s.name = (name && *name) ? name : "Untitled";
  s.loop = false;
  s.playbackSpeed = 1.0f;
  s_Sequences.push_back(s);
  s_ActiveIdx = (int)s_Sequences.size() - 1;
  VehicleClip_Clear(); // a brand-new sequence starts with no vehicle clips
  Sequence_Stop();
}

void Sequence_DeleteActive() {
  if (s_ActiveIdx < 0 || s_ActiveIdx >= (int)s_Sequences.size())
    return;
  // Remove the sequence's JSON file from disk too.
  char path[MAX_PATH];
  GetSeqJsonPath(s_Sequences[s_ActiveIdx].name.c_str(), path, sizeof(path));
  DeleteFileA(path);
  VehicleClip_DespawnGhost();
  s_Sequences.erase(s_Sequences.begin() + s_ActiveIdx);
  if (s_Sequences.empty()) {
    s_ActiveIdx = -1;
    EnsureActive();
  } else if (s_ActiveIdx >= (int)s_Sequences.size()) {
    s_ActiveIdx = (int)s_Sequences.size() - 1;
  }
  Sequence_Stop();
  LoadActiveSequenceClips();
}

void Sequence_SortByTime() {
  CameraSequence *s = Sequence_Active();
  if (!s) return;
  std::sort(s->poses.begin(), s->poses.end(),
            [](const PoseKeyframe &a, const PoseKeyframe &b) { return a.t < b.t; });
  std::sort(s->events.begin(), s->events.end(),
            [](const EffectEvent &a, const EffectEvent &b) { return a.t < b.t; });
}

int Sequence_CloseLoop() {
  CameraSequence *s = Sequence_Active();
  if (!s) return -1;
  // Closing a non-looping sequence makes no sense — flip the flag so
  // playback actually wraps. (The user can still toggle Loop back off
  // afterward if they only wanted the value-snap.)
  s->loop = true;
  if (s->poses.empty()) return -1;

  if (s->poses.size() == 1) {
    // Trivial: duplicate the single pose 2s later. The two-pose result
    // is automatically loop-closed by definition.
    PoseKeyframe k = s->poses[0];
    k.t = s->poses[0].t + 2.0f;
    s->poses.push_back(k);
    Sequence_SortByTime();
    return (int)s->poses.size() - 1;
  }

  // If already within tolerance, hard-snap the last pose's values to
  // exactly match the first. This makes the duplicate bit-exact (and is
  // visible to the user — the deltas drop to 0 in the menu).
  if (IsLoopClosedImpl(s)) {
    PoseKeyframe &last = s->poses.back();
    const PoseKeyframe &first = s->poses.front();
    last.posX = first.posX; last.posY = first.posY; last.posZ = first.posZ;
    last.pitch = first.pitch; last.yaw = first.yaw; last.roll = first.roll;
    last.fov = first.fov;
    // Also mirror the entity-lock state so a locked first keyframe's
    // wrap-around to itself stays anchored to the same entity (otherwise
    // the seam would drift relative to a moving vehicle).
    last.entityHandle = first.entityHandle;
    last.localOffsetX = first.localOffsetX;
    last.localOffsetY = first.localOffsetY;
    last.localOffsetZ = first.localOffsetZ;
    last.lockEntPitch = first.lockEntPitch;
    last.lockEntYaw   = first.lockEntYaw;
    last.lockEntRoll  = first.lockEntRoll;
    last.lockEntPosX  = first.lockEntPosX;
    last.lockEntPosY  = first.lockEntPosY;
    last.lockEntPosZ  = first.lockEntPosZ;
    return (int)s->poses.size() - 1;
  }

  // Otherwise append a new closing pose at last.t + average_gap with
  // values copied from pose[0]. The average gap heuristic keeps the new
  // seam segment's duration consistent with the rest of the sequence,
  // so playback speed across the wrap feels natural.
  float lastT = s->poses.back().t;
  float avgGap = 2.0f;
  if (s->poses.size() >= 2) {
    avgGap = (s->poses.back().t - s->poses.front().t) /
             (float)(s->poses.size() - 1);
    if (avgGap < 0.5f) avgGap = 0.5f;
  }
  PoseKeyframe k = s->poses.front();
  k.t = lastT + avgGap;
  // Spline path for the seam keeps the closing arc smooth; Linear ease
  // gives the constant-velocity wrap we want for a continuous loop.
  k.path = PATH_SPLINE;
  k.ease = EASE_LINEAR;
  s->poses.push_back(k);
  Sequence_SortByTime();
  return (int)s->poses.size() - 1;
}

void Sequence_DeletePose(int idx) {
  CameraSequence *s = Sequence_Active();
  if (!s || idx < 0 || idx >= (int)s->poses.size())
    return;
  s->poses.erase(s->poses.begin() + idx);
}

int Sequence_DuplicatePose(int poseIdx) {
  CameraSequence *s = Sequence_Active();
  if (!s || poseIdx < 0 || poseIdx >= (int)s->poses.size()) return -1;
  PoseKeyframe k = s->poses[poseIdx]; // copy everything (pose, ease, lock, ...)

  // Time placement: midpoint to the next-later keyframe (an insert you'll
  // reshape), or +2s past the end when duplicating the last one (a hold).
  float srcT = k.t;
  float nextT = -1.0f;
  for (const auto &p : s->poses)
    if (p.t > srcT + 0.001f && (nextT < 0.0f || p.t < nextT)) nextT = p.t;
  k.t = (nextT >= 0.0f) ? (srcT + nextT) * 0.5f : srcT + 2.0f;

  // Nudge off any coincident keyframe so we don't create a zero-length segment.
  for (const auto &p : s->poses)
    if (fabsf(p.t - k.t) < 0.05f) k.t = p.t + 0.05f;

  s->poses.push_back(k);
  Sequence_SortByTime();
  // Locate the copy post-sort by its (nudged-unique) time.
  for (int i = 0; i < (int)s->poses.size(); ++i)
    if (fabsf(s->poses[i].t - k.t) < 0.001f) return i;
  return -1;
}

int Sequence_DistributeTimes() {
  CameraSequence *s = Sequence_Active();
  if (!s || (int)s->poses.size() < 3) return 0;
  Sequence_SortByTime(); // make first/last meaningful even after hand edits
  int n = (int)s->poses.size();
  float t0 = s->poses.front().t, t1 = s->poses.back().t;
  if (t1 - t0 < 0.01f) return 0;
  for (int i = 1; i < n - 1; ++i)
    s->poses[i].t = t0 + (t1 - t0) * (float)i / (float)(n - 1);
  return n - 2;
}

void Sequence_AddEvent(EffectKind kind, float t, float value, bool ramp) {
  CameraSequence *s = EnsureActive();
  EffectEvent e = {t, kind, value, ramp};
  s->events.push_back(e);
  Sequence_SortByTime();
}

void Sequence_DeleteEvent(int idx) {
  CameraSequence *s = Sequence_Active();
  if (!s || idx < 0 || idx >= (int)s->events.size())
    return;
  s->events.erase(s->events.begin() + idx);
}

// ============================================================
//  Time / duration / scrub
// ============================================================

float Sequence_TotalDuration() {
  CameraSequence *s = Sequence_Active();
  if (!s) return 0.0f;
  float maxT = 0.0f;
  for (const auto &p : s->poses) if (p.t > maxT) maxT = p.t;
  for (const auto &e : s->events) if (e.t > maxT) maxT = e.t;
  // Extend the timeline to cover the vehicle clips too (a delayed / slowed car
  // reaches past the last keyframe — its per-clip offset+speed are folded in by
  // VehicleClip_Duration), so the scrub/playback can reach the whole action.
  float vd = VehicleClip_Duration();
  if (vd > maxT) maxT = vd;
  return maxT;
}

float Sequence_CurrentTime() { return s_PlaybackTime; }

void Sequence_SetCurrentTime(float t) {
  if (t < 0.0f) t = 0.0f;
  float dur = Sequence_TotalDuration();
  if (t > dur) t = dur;
  s_LastTickTime = t;
  s_PlaybackTime = t;
  // Apply the pose at this time immediately so the user sees what's
  // there while scrubbing. Pass dt=0 to suppress shake — a one-shot
  // scrub apply shouldn't animate jitter.
  CameraSequence *s = Sequence_Active();
  if (s && !s->poses.empty()) {
    // Idle scrub: move the recorded vehicles to the scrub time FIRST, so an
    // entity-LOCKED pose resolves its world coord against the vehicle where
    // it will actually be shown — not against its previous playhead pose.
    // (While playing, the tick drives the vehicles itself.)
    if (!s_Playing) VehicleClip_ApplyAtTime(t, false);
    ApplyPoseAtTime(t, s);
    // Idle scrub is a one-shot: route the push through the WORLD-coord path,
    // never the attach path. Attaching would leave free-fly rotation-only
    // (an attached cam ignores SET_CAM_COORD) — and attach+detach within one
    // script frame never takes render effect anyway, so the camera would
    // visibly stay put. Clearing the attach state BEFORE the push makes
    // PushToEngine call SET_CAM_COORD with the resolved locked pose.
    // Playback keeps the native attach (lag-free ride-along) untouched.
    if (!s_Playing) SequenceDetachCamera();
    SequencePushToEngine(0.0f);
  }
}

// Destructively scale the time of EVERY pose keyframe and effect event in the
// active sequence by `factor` (>1 stretches/slows the whole shot, <1 compresses/
// speeds it) — unlike playbackSpeed, this rewrites the keyframe t values. The
// playhead is scaled too so the cursor stays at the same relative position.
void Sequence_ScaleTimes(float factor) {
  if (factor <= 0.0f) return;
  CameraSequence *s = Sequence_Active();
  if (!s) return;
  for (PoseKeyframe &p : s->poses) p.t *= factor;
  for (EffectEvent &e : s->events) e.t *= factor;
  s_PlaybackTime *= factor;
  s_LastTickTime *= factor;
  Sequence_SortByTime();
}

// Translate EVERY keyframe's world position by (dx,dy,dz). The entity-lock world
// anchor (lockEntPos) is shifted by the same delta: for a locked keyframe the
// resolve is `keyframePos + (entityNow - lockEntPos)`, so moving both terms by
// the same delta cancels — the keyframe keeps riding its entity and only genuine
// world-space (unlocked) keyframes relocate. Rigid-locked keyframes resolve from
// the entity + localOffset and are unaffected either way.
void Sequence_TranslateAll(float dx, float dy, float dz) {
  CameraSequence *s = Sequence_Active();
  if (!s) return;
  for (PoseKeyframe &p : s->poses) {
    p.posX += dx; p.posY += dy; p.posZ += dz;
    p.lockEntPosX += dx; p.lockEntPosY += dy; p.lockEntPosZ += dz;
  }
}

bool Sequence_IsPlaying() { return s_Playing; }

void Sequence_Play() {
  CameraSequence *s = Sequence_Active();
  // Allow playback with no camera keyframes as long as there are recorded
  // vehicles to replay (the camera just stays in free-fly).
  if (!s || (s->poses.empty() && !VehicleClip_HasData())) return;
  if (s_PlaybackTime >= Sequence_TotalDuration())
    s_PlaybackTime = 0.0f;

  // Snapshot the user's shake config the moment Play starts (only on
  // first transition into play — Pause+Play preserves the original
  // snapshot so events stay scoped to the sequence).
  if (!s_HasShakeSnapshot) SaveShakeSnapshot();

  // Catch-up: if Play starts past t=0 (user scrubbed forward first), apply
  // every snap event whose time is <= the current scrub position so the
  // engine state matches "what would have happened if we played from 0".
  // Without this, the DoF / shake / weather state at the moment of Play
  // ignores all the events you scrolled past.
  for (const auto &e : s->events) {
    if (e.ramp) continue;
    if (e.t <= s_PlaybackTime) {
      ApplyEffectValue(e.kind, e.value);
    }
  }
  // Apply the visual pose at the current scrub position too. dt=0
  // suppresses shake on this initial push; the first real Sequence_Tick
  // call will start animating shake with the actual frame delta.
  ApplyPoseAtTime(s_PlaybackTime, s);
  if (!s->poses.empty()) SequencePushToEngine(0.0f); // own the camera only with keyframes

  s_LastTickTime = s_PlaybackTime;
  s_FirstPlayTick = true;
  s_Playing = true;
  g_SequencePlaybackActive = true;
}

void Sequence_Stop() {
  s_Playing = false;
  s_PlaybackTime = 0.0f;
  s_LastTickTime = 0.0f;
  g_SequencePlaybackActive = false;
  // Roll back any shake mutations applied by effect events during this
  // playback session so authoring/free-fly state isn't polluted.
  RestoreShakeSnapshot();
}

void Sequence_TogglePlay() {
  if (s_Playing) {
    s_Playing = false;
    g_SequencePlaybackActive = false;
  } else {
    Sequence_Play();
  }
}

void Sequence_JumpToNextPose() {
  CameraSequence *s = Sequence_Active();
  if (!s) return;
  for (const auto &p : s->poses) {
    if (p.t > s_PlaybackTime + 0.001f) {
      Sequence_SetCurrentTime(p.t);
      return;
    }
  }
}

void Sequence_JumpToPrevPose() {
  CameraSequence *s = Sequence_Active();
  if (!s) return;
  float best = -1.0f;
  for (const auto &p : s->poses) {
    if (p.t < s_PlaybackTime - 0.001f && p.t > best)
      best = p.t;
  }
  if (best >= 0.0f) Sequence_SetCurrentTime(best);
  else Sequence_SetCurrentTime(0.0f);
}

int Sequence_ApplyLockToAll(int entityHandle) {
  CameraSequence *s = Sequence_Active();
  if (!s || entityHandle == 0) return 0;
  if (!ENTITY::DOES_ENTITY_EXIST(entityHandle)) return 0;
  // Snapshot the entity's rotation ONCE — every keyframe gets the same
  // lockEntRot. Conceptually: "at this instant, freeze the world<-entity
  // transform and use it as the basis for all keyframes' lock state".
  Vector3 entRot = ENTITY::GET_ENTITY_ROTATION(entityHandle, 2);
  Vector3 entPos = ENTITY::GET_ENTITY_COORDS(entityHandle, TRUE);
  int n = 0;
  for (PoseKeyframe &p : s->poses) {
    // Compute local offset from the keyframe's CURRENT stored world
    // coords. After this call the keyframe rides along with the entity
    // starting from exactly the position the user already authored.
    Vector3 local = invoke<Vector3>(
        0x2274BC1C4885E333, entityHandle, p.posX, p.posY, p.posZ);
    p.entityHandle = entityHandle;
    p.localOffsetX = local.x;
    p.localOffsetY = local.y;
    p.localOffsetZ = local.z;
    p.lockEntPitch = entRot.x;
    p.lockEntRoll  = entRot.y;
    p.lockEntYaw   = entRot.z;
    p.lockEntPosX  = entPos.x;
    p.lockEntPosY  = entPos.y;
    p.lockEntPosZ  = entPos.z;
    ++n;
  }
  return n;
}

int Sequence_ClearAllLocks() {
  CameraSequence *s = Sequence_Active();
  if (!s) return 0;
  int n = 0;
  for (PoseKeyframe &p : s->poses) {
    // Treat "had a handle" OR "has any non-zero offset / lockEntRot" as
    // locked-ish so the count matches the per-pose editor's hints.
    bool wasLocked = p.entityHandle != 0 ||
                     p.localOffsetX != 0.0f || p.localOffsetY != 0.0f ||
                     p.localOffsetZ != 0.0f ||
                     p.lockEntPitch != 0.0f || p.lockEntYaw != 0.0f ||
                     p.lockEntRoll != 0.0f;
    p.entityHandle = 0;
    p.localOffsetX = p.localOffsetY = p.localOffsetZ = 0.0f;
    p.lockEntPitch = p.lockEntYaw = p.lockEntRoll = 0.0f;
    p.lockEntPosX = p.lockEntPosY = p.lockEntPosZ = 0.0f;
    if (wasLocked) ++n;
  }
  return n;
}

int Sequence_LockedPoseCount() {
  CameraSequence *s = Sequence_Active();
  if (!s) return 0;
  int n = 0;
  for (const PoseKeyframe &p : s->poses) {
    if (p.entityHandle != 0 || p.localOffsetX != 0.0f ||
        p.localOffsetY != 0.0f || p.localOffsetZ != 0.0f ||
        p.lockEntPitch != 0.0f || p.lockEntYaw != 0.0f ||
        p.lockEntRoll != 0.0f) ++n;
  }
  return n;
}

void Sequence_CaptureLockForPose(PoseKeyframe &p) {
  p.entityHandle = 0;
  p.localOffsetX = p.localOffsetY = p.localOffsetZ = 0.0f;
  p.lockEntPitch = p.lockEntYaw = p.lockEntRoll = 0.0f;
  p.lockEntPosX = p.lockEntPosY = p.lockEntPosZ = 0.0f;
  // Resolve the current free-cam follow target. Mode 1 (Player follow)
  // and mode 2 (Aimed Entity) both produce a usable keyframe anchor —
  // mode 1 attaches the keyframe to whichever ped the user is
  // controlling (handy for "track me as I walk"), mode 2 to whatever
  // they raycasted / picked.
  int target = 0;
  if (g_FollowMode == 1) {
    target = PLAYER::PLAYER_PED_ID();
  } else if (g_FollowMode == 2 && g_FollowTargetEntity != 0) {
    target = g_FollowTargetEntity;
  }
  if (target == 0 || !ENTITY::DOES_ENTITY_EXIST(target)) return;
  // GET_OFFSET_FROM_ENTITY_GIVEN_WORLD_COORDS — same native Free Camera
  // uses to compute the rigid-attach offset (see camera.cpp:613-617).
  Vector3 local = invoke<Vector3>(0x2274BC1C4885E333, target, p.posX, p.posY,
                                  p.posZ);
  p.entityHandle = target;
  p.localOffsetX = local.x;
  p.localOffsetY = local.y;
  p.localOffsetZ = local.z;
  // Snapshot the entity's world rotation at lock time so ResolvePose can apply
  // the car's rotation CHANGE since lock to the authored angles each frame
  // (plain Euler delta — no quaternions). Stored in the entity's native
  // (pitch, roll, yaw) layout, matching ResolvePose's read-back.
  Vector3 entRot = ENTITY::GET_ENTITY_ROTATION(target, 2);
  p.lockEntPitch = entRot.x;
  p.lockEntRoll  = entRot.y;
  p.lockEntYaw   = entRot.z;
  // Snapshot the entity's world position too, so the non-rigid translate-only
  // follow can offset by the entity's travel since this moment.
  Vector3 entPos = ENTITY::GET_ENTITY_COORDS(target, TRUE);
  p.lockEntPosX = entPos.x;
  p.lockEntPosY = entPos.y;
  p.lockEntPosZ = entPos.z;
}

int Sequence_CapturePoseAtCurrentTime() {
  CameraSequence *s = EnsureActive();
  float px, py, pz, pitch, yaw, roll;
  GetCameraState(px, py, pz, pitch, yaw, roll);

  PoseKeyframe k{};
  // Current timeline end (last keyframe's time).
  float lastT = 0.0f;
  bool hasPoses = !s->poses.empty();
  for (const auto &p : s->poses) if (p.t > lastT) lastT = p.t;

  // Where does the new keyframe land in time?
  //
  // If a vehicle clip already defines the timeline length, drop the keyframe
  // exactly at the playhead — you scrub along the pre-recorded action and place
  // camera poses onto that existing timeline, and we never auto-extend past the
  // vehicle (so an "ending" keyframe doesn't tack on a spurious 2s).
  //
  // Otherwise the timeline is built purely from keyframes: an ENDING keyframe
  // (playhead at/after the end) is placed a fixed 2s after the last one so the
  // timeline grows by 2s and the playhead lands exactly on the new end (a valid
  // scrub position that can't be clamped back). An INSERT between existing
  // keyframes is placed at the playhead, adding no time.
  bool vehDrivesTimeline = VehicleClip_Duration() > 0.01f;
  bool isAppend = !hasPoses || s_PlaybackTime >= lastT - 0.01f;
  if (!vehDrivesTimeline && isAppend && hasPoses)
    k.t = lastT + 2.0f;
  else
    k.t = s_PlaybackTime;

  // Nudge off any coincident keyframe so we don't create a zero-length segment.
  // (An append lands past every existing time, so this only matters for inserts.)
  for (const auto &existing : s->poses) {
    if (fabsf(existing.t - k.t) < 0.05f) k.t = existing.t + 0.05f;
  }

  k.posX = px; k.posY = py; k.posZ = pz;
  k.pitch = pitch; k.yaw = yaw; k.roll = roll;
  k.fov = g_CamFOV;
  // Spline + Linear is the smoothest default: time-aware Hermite path
  // through the surrounding keyframes with constant-velocity progress
  // through each segment. Users who want a different feel can change
  // ease/path in the per-pose editor.
  k.ease = EASE_LINEAR;
  k.path = PATH_SPLINE;
  // If free-cam is currently entity-locked, ride along with that entity
  // during playback too. The world-space pos* values stay populated as a
  // fallback for the case where the entity no longer exists at playback.
  Sequence_CaptureLockForPose(k);
  s->poses.push_back(k);
  Sequence_SortByTime();

  // Move the playhead onto the new keyframe. For an append that's the new end
  // (its time is already 2s past the previous last, so the scrub shows the
  // advance and the timeline grew by 2s); for an insert it stays on the inserted
  // pose. Either way it lands exactly on a real keyframe, so it's a valid scrub
  // position that won't be clamped back.
  s_PlaybackTime = k.t;
  s_LastTickTime = s_PlaybackTime;

  // Find the new pose's index after sorting
  for (int i = 0; i < (int)s->poses.size(); ++i)
    if (fabsf(s->poses[i].t - k.t) < 0.0001f) return i;
  return -1;
}

// ============================================================
//  Playback: pose interpolation
// ============================================================

static void ApplyPoseAtTime(float t, const CameraSequence *s) {
  if (!s) return;

  // Position the recorded vehicle(s) at this timeline instant FIRST, so
  // entity-locked keyframes below resolve against the subject's correct pose.
  // This runs before the no-keyframes check below, so recorded vehicles can be
  // played back on their own (the camera just stays in free-fly).
  // animateWheels while playing/rendering; held still on a plain scrub.
  VehicleClip_ApplyAtTime(t, s_Playing);

  if (s->poses.empty()) return; // vehicle-only playback — no camera pose to drive

  // Single pose: just hold it
  if (s->poses.size() == 1) {
    const PoseKeyframe p = ResolvePose(s, 0, false);
    // If this lone pose is locked to a live entity, route through the
    // attach path too — otherwise we'd see lag while the camera sits
    // on a moving target.
    if (s->poses[0].entityHandle != 0 &&
        ENTITY::DOES_ENTITY_EXIST(s->poses[0].entityHandle)) {
      SetCameraStateFromSequenceLocked(
          s->poses[0].entityHandle, s->poses[0].localOffsetX,
          s->poses[0].localOffsetY, s->poses[0].localOffsetZ, p.pitch,
          p.yaw, p.roll, p.fov);
    } else {
      SetCameraStateFromSequence(p.posX, p.posY, p.posZ, p.pitch, p.yaw,
                                 p.roll, p.fov);
    }
    return;
  }

  // Find the segment [i, i+1] containing t
  int i = 0;
  for (int k = 0; k < (int)s->poses.size() - 1; ++k) {
    if (t >= s->poses[k].t && t <= s->poses[k + 1].t) {
      i = k; break;
    }
    if (k == (int)s->poses.size() - 2 && t > s->poses[k + 1].t) {
      i = k; // past end, clamp at last segment
    }
  }
  if (t < s->poses.front().t) i = 0;

  const int N = (int)s->poses.size();
  const bool closed = IsLoopClosedImpl(s);
  // Cycle period for closed loops: the seam pose's time. (We could
  // alternatively use TotalDuration, but pose[N-1].t is the canonical
  // wrap point since events past it don't make sense in a closed loop.)
  const float dur = closed ? s->poses[N - 1].t : 0.0f;

  // Resolve a and b. When closed and we're on the seam segment
  // [N-2, N-1], pose[N-1]'s values are substituted from pose[0] so the
  // wrap is bit-exact even if the user hasn't manually snapped them.
  const PoseKeyframe a = ResolvePose(s, i, closed);
  const PoseKeyframe b = ResolvePose(s, i + 1, closed);
  float segDur = b.t - a.t;
  float u = (segDur > 0.0001f) ? clampf((t - a.t) / segDur, 0.0f, 1.0f) : 0.0f;
  float te = ApplyEase(b.ease, u);

  float px, py, pz;
  float pitch, yaw, roll;

  if (b.path == PATH_SPLINE && N >= 2) {
    // Time-aware cubic Hermite across 4 control points (i-1, i, i+1, i+2).
    // For non-closed sequences, boundary clamping produces one-sided
    // slopes at start/end. For closed sequences, ResolveCtrl wraps the
    // neighbor indices and returns a time offset (±dur) so the slope
    // calculation sees the cyclic continuation — making velocity C1-
    // continuous across the seam.
    CtrlRef cp0 = ResolveCtrl(i - 1, N, closed, dur);
    CtrlRef cp3 = ResolveCtrl(i + 2, N, closed, dur);
    const PoseKeyframe p0 = ResolvePose(s, cp0.idx, closed);
    const PoseKeyframe p3 = ResolvePose(s, cp3.idx, closed);
    const float p0t = p0.t + cp0.tOffset;
    const float p3t = p3.t + cp3.tOffset;

    // We pass eased time into the spline via a scaled `t` value so the
    // ease shape (Linear / EaseInOut / etc.) reshapes progress through
    // the segment without breaking position continuity at boundaries.
    float tEased = a.t + te * (b.t - a.t);

    px = HermiteSpline1D(p0.posX, a.posX, b.posX, p3.posX,
                         p0t, a.t, b.t, p3t, tEased);
    py = HermiteSpline1D(p0.posY, a.posY, b.posY, p3.posY,
                         p0t, a.t, b.t, p3t, tEased);
    pz = HermiteSpline1D(p0.posZ, a.posZ, b.posZ, p3.posZ,
                         p0t, a.t, b.t, p3t, tEased);

    // Apply the same spline treatment to rotation. Unwrap angles
    // relative to the segment endpoints (a, b) so the spline takes the
    // short way around the ±180° seam instead of swinging through.
    float a_pitch = a.pitch;
    float b_pitch = unwrapAngle(b.pitch, a_pitch);
    float p0_pitch = unwrapAngle(p0.pitch, a_pitch);
    float p3_pitch = unwrapAngle(p3.pitch, b_pitch);

    float a_yaw = a.yaw;
    float b_yaw = unwrapAngle(b.yaw, a_yaw);
    float p0_yaw = unwrapAngle(p0.yaw, a_yaw);
    float p3_yaw = unwrapAngle(p3.yaw, b_yaw);

    float a_roll = a.roll;
    float b_roll = unwrapAngle(b.roll, a_roll);
    float p0_roll = unwrapAngle(p0.roll, a_roll);
    float p3_roll = unwrapAngle(p3.roll, b_roll);

    pitch = HermiteSpline1D(p0_pitch, a_pitch, b_pitch, p3_pitch,
                            p0t, a.t, b.t, p3t, tEased);
    yaw = HermiteSpline1D(p0_yaw, a_yaw, b_yaw, p3_yaw,
                          p0t, a.t, b.t, p3t, tEased);
    roll = HermiteSpline1D(p0_roll, a_roll, b_roll, p3_roll,
                           p0t, a.t, b.t, p3t, tEased);
  } else {
    // Linear path: simple lerp per axis, shortest-arc for rotation.
    px = lerpf(a.posX, b.posX, te);
    py = lerpf(a.posY, b.posY, te);
    pz = lerpf(a.posZ, b.posZ, te);
    pitch = lerpAngle(a.pitch, b.pitch, te);
    yaw   = lerpAngle(a.yaw,   b.yaw,   te);
    roll  = lerpAngle(a.roll,  b.roll,  te);
  }

  float fov = lerpf(a.fov, b.fov, te);

  // Native-attach when the active segment [i, i+1] is fully locked to
  // the same entity. The interpolated world coord we computed via the
  // spline is converted back to the entity's local frame and handed to
  // the camera driver, which uses ATTACH_CAM_TO_ENTITY for that frame.
  // The engine then positions the camera at render time (after physics)
  // instead of from our script-tick world coord — eliminating the one-
  // frame lag that otherwise looks like jitter on a fast-moving vehicle.
  //
  // We require BOTH endpoints locked to the SAME entity. Mixed lock
  // state means the spline crosses a "world<->entity" boundary; in that
  // case world-coord SET_CAM_COORD is still the right path (the lag is
  // unavoidable but the alternative — attach for only part of the
  // segment — would visibly snap).
  int segEntity = s->poses[i].entityHandle;
  if (closed && (i + 1) == N - 1) {
    // Seam segment: pose[N-1] inherits pose[0]'s lock per ResolvePose.
    if (s->poses[0].entityHandle != segEntity) segEntity = 0;
  } else if (i + 1 < N) {
    if (s->poses[i + 1].entityHandle != segEntity) segEntity = 0;
  } else {
    segEntity = 0;
  }
  if (segEntity != 0 && ENTITY::DOES_ENTITY_EXIST(segEntity)) {
    // Convert the spline result back into the entity's local frame so
    // the camera driver can attach with this offset.
    Vector3 local = invoke<Vector3>(0x2274BC1C4885E333, segEntity, px, py, pz);
    SetCameraStateFromSequenceLocked(segEntity, local.x, local.y, local.z,
                                     pitch, yaw, roll, fov);
  } else {
    SetCameraStateFromSequence(px, py, pz, pitch, yaw, roll, fov);
  }
}

// ============================================================
//  Playback: event firing
// ============================================================

static void ApplyEffectValue(EffectKind kind, float value) {
  switch (kind) {
  case EFX_SHAKE_ENABLED:
    g_ShakeEnabled = value >= 0.5f;
    break;
  case EFX_SHAKE_PRESET: {
    int p = (int)(value + 0.5f);
    if (p >= 0 && p <= 4) {
      ApplyShakePreset(p);
      // Auto-enable shake when picking a non-Off preset (otherwise the
      // event silently writes amp/freq but nothing visible happens).
      // Off preset (0) leaves enabled state alone — explicit disable is
      // via SHAKE_ENABLED=0 event.
      if (p > 0) g_ShakeEnabled = true;
    }
    break;
  }
  case EFX_SHAKE_AMP:
    g_ShakeAmp = value;
    break;
  case EFX_SHAKE_FREQ:
    g_ShakeFreq = value;
    break;
  case EFX_SHAKE_SPEED_AMP:
    g_ShakeSpeedAmpCoupling = value;
    break;
  case EFX_SHAKE_SPEED_FREQ:
    g_ShakeSpeedFreqCoupling = value;
    break;
  case EFX_SHAKE_ROT_WEIGHT:
    g_ShakeRotWeight = value;
    break;
  case EFX_SHAKE_POS_WEIGHT:
    g_ShakePosWeight = value;
    break;
  case EFX_SHAKE_STOP_STILL:
    g_ShakeStopWhenStill = value >= 0.5f;
    break;
  case EFX_SHAKE_RANDOMIZE:
    // Fire-once trigger: re-roll the noise seeds. Value is meaningless.
    // If the user marked this event as `ramp` it'll re-roll every frame
    // during the ramp window — that produces a chaotic white-noise look
    // (sometimes desirable). For a single re-roll, use snap mode.
    RandomizeShakePattern();
    break;
  default:
    // Removed kinds (DoF, time, weather, walk mode, world freeze, world
    // speed/slow-motion) are silently ignored if they appear in an old
    // saved sequence. Slow motion now lives in the World & Scene menu.
    break;
  }
}

// For ramp events: find the previous event of the same kind (by index in
// the active sequence's event list). If found, returns its value at
// `out_prevVal` and its time at `out_prevT`; returns true on success.
static bool FindPrevEventOfKind(const CameraSequence *s, int curIdx,
                                EffectKind kind, float &out_prevT,
                                float &out_prevVal) {
  for (int i = curIdx - 1; i >= 0; --i) {
    if (s->events[i].kind == kind) {
      out_prevT = s->events[i].t;
      out_prevVal = s->events[i].value;
      return true;
    }
  }
  return false;
}

// Fire snap events whose t falls in (lastT, nowT]; for ramp events that
// are currently "in flight" (i.e. lastSameKind.t < nowT < this.t), apply
// the lerped value continuously.
// `firstTick` widens the lower bound to include lastT itself, so events
// scheduled exactly at the playback start time (e.g. t=0) fire on the
// very first frame instead of being skipped.
static void DriveEvents(const CameraSequence *s, float lastT, float nowT,
                        bool firstTick) {
  if (!s) return;

  // Pass 1: snap events. Window is (lastT, nowT] in normal flow, but
  // [lastT, nowT] on the first tick so t=0 events fire.
  for (int i = 0; i < (int)s->events.size(); ++i) {
    const EffectEvent &e = s->events[i];
    if (e.ramp) continue;
    bool inWindow = firstTick ? (e.t >= lastT && e.t <= nowT)
                              : (e.t > lastT && e.t <= nowT);
    if (inWindow) {
      ApplyEffectValue(e.kind, e.value);
    }
  }

  // Pass 2: ramp events — for each ramp event, if nowT is within its
  // active span (prevSameKind.t .. e.t], lerp.
  for (int i = 0; i < (int)s->events.size(); ++i) {
    const EffectEvent &e = s->events[i];
    if (!e.ramp) continue;
    float prevT, prevVal;
    if (!FindPrevEventOfKind(s, i, e.kind, prevT, prevVal)) {
      // No predecessor — treat as snap at e.t
      if (e.t > lastT && e.t <= nowT) ApplyEffectValue(e.kind, e.value);
      continue;
    }
    if (nowT >= prevT && nowT <= e.t && e.t > prevT) {
      float u = (nowT - prevT) / (e.t - prevT);
      ApplyEffectValue(e.kind, lerpf(prevVal, e.value, u));
    } else if (nowT > e.t && lastT <= e.t) {
      // Just crossed the end of the ramp — snap to final value
      ApplyEffectValue(e.kind, e.value);
    }
  }
}

// ============================================================
//  Mode lifecycle + per-frame tick
// ============================================================

bool Sequence_IsInMode() { return s_InMode; }

void Sequence_EnterMode() {
  if (s_InMode) return;
  s_InMode = true;
  s_LastFrameTick = 0;
  EnsureActive();
  // Use Free Camera's scripted-camera infrastructure so the view actually
  // gets taken over. We never run UpdateFreeCamera in this mode — the
  // sequence tick drives s_Pos* directly via SetCameraStateFromSequence
  // and pushes to the engine.
  if (!g_FreeCamActive) {
    InitFreeCamera();
  }
  // Drone mode is incompatible with keyframe authoring (it owns its own
  // physics-driven position state). Free-cam's follow mode, on the other
  // hand, is exactly what we want available while authoring — locking the
  // camera to a vehicle / ped while flying around composes naturally with
  // capturing keyframes that ride along with that entity. Sequence_Tick
  // never runs UpdateFreeCamera during playback (only when !s_Playing),
  // so the follow logic can't fight the playback driver.
  g_DroneMode = false;
}

void Sequence_ExitMode() {
  if (!s_InMode) return;
  Sequence_Stop();
  s_InMode = false;
  VehicleClip_DespawnGhost(); // remove a respawned ghost when leaving the mode
  VehicleClip_Release();      // hand any real recorded vehicle back to the game
  if (g_FreeCamActive) {
    DestroyFreeCamera();
  }
}

// ============================================================
//  In-world keyframe visualization
// ============================================================
//
// Draws a sphere at each pose keyframe and a thin polyline between
// consecutive poses so the user can see the camera path. The pose
// closest to the current scrub time is highlighted (larger + brighter)
// to act as a "you are here" cursor.
//
// For PATH_SPLINE segments we tessellate the Catmull-Rom curve so the
// preview line matches what playback will actually do.

// World→screen text helper. Returns silently if the point is offscreen
// or behind the camera (_WORLD3D_TO_SCREEN2D returns FALSE in both cases).
static void DrawText3D(float x, float y, float z, const char *text, int r,
                       int g, int b, int a, float scale = 0.30f) {
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

// Cheap position-only interpolation along a sequence's path. Used for
// placing event labels along the camera's trajectory at the event's
// scheduled time. Linear lerp is fine for label positioning; we don't
// need spline accuracy here.
static void ComputePosAtTime(float t, const CameraSequence *s,
                             float &outX, float &outY, float &outZ) {
  outX = outY = outZ = 0.0f;
  if (!s || s->poses.empty()) return;
  if (s->poses.size() == 1) {
    outX = s->poses[0].posX;
    outY = s->poses[0].posY;
    outZ = s->poses[0].posZ;
    return;
  }
  int i = 0;
  for (int k = 0; k < (int)s->poses.size() - 1; ++k) {
    if (t >= s->poses[k].t && t <= s->poses[k + 1].t) { i = k; break; }
    if (k == (int)s->poses.size() - 2 && t > s->poses[k + 1].t) i = k;
  }
  if (t < s->poses.front().t) i = 0;
  const bool closed = IsLoopClosedImpl(s);
  const PoseKeyframe a = ResolvePose(s, i, closed);
  const PoseKeyframe b = ResolvePose(s, i + 1, closed);
  float segDur = b.t - a.t;
  float u = (segDur > 0.0001f) ? clampf((t - a.t) / segDur, 0.0f, 1.0f) : 0.0f;
  outX = lerpf(a.posX, b.posX, u);
  outY = lerpf(a.posY, b.posY, u);
  outZ = lerpf(a.posZ, b.posZ, u);
}

// Compact, human-readable one-line summary of an event for the in-world
// label. Prefixed with "~" for ramp events so the visual distinguishes
// snap vs. lerp at a glance.
static std::string FormatEventLabel(const EffectEvent &e) {
  char buf[64];
  const char *p = e.ramp ? "~" : "";
  switch (e.kind) {
  case EFX_SHAKE_ENABLED:
    sprintf_s(buf, "%sShake %s", p, e.value >= 0.5f ? "On" : "Off"); break;
  case EFX_SHAKE_PRESET: {
    static const char *names[] = {"Off", "Subtle", "Handheld", "Vehicle",
                                  "Earthquake"};
    int idx = (int)(e.value + 0.5f);
    if (idx < 0) idx = 0; if (idx > 4) idx = 4;
    sprintf_s(buf, "%sPreset:%s", p, names[idx]); break;
  }
  case EFX_SHAKE_AMP:        sprintf_s(buf, "%sAmp:%.2f",     p, e.value); break;
  case EFX_SHAKE_FREQ:       sprintf_s(buf, "%sFreq:%.1f",    p, e.value); break;
  case EFX_SHAKE_SPEED_AMP:  sprintf_s(buf, "%sSpdAmp:%.2f",  p, e.value); break;
  case EFX_SHAKE_SPEED_FREQ: sprintf_s(buf, "%sSpdFreq:%.2f", p, e.value); break;
  case EFX_SHAKE_ROT_WEIGHT: sprintf_s(buf, "%sRotW:%.2f",    p, e.value); break;
  case EFX_SHAKE_POS_WEIGHT: sprintf_s(buf, "%sPosW:%.2f",    p, e.value); break;
  case EFX_SHAKE_STOP_STILL:
    sprintf_s(buf, "%sStopStill %s", p, e.value >= 0.5f ? "On" : "Off"); break;
  case EFX_SHAKE_RANDOMIZE:
    sprintf_s(buf, "%sRandomize!", p); break;
  default: sprintf_s(buf, "%s(legacy)", p); break;
  }
  return buf;
}

static void DrawSequenceMarkers() {
  if (!g_SequenceShowMarkers) return;
  // Recorded-vehicle path first — it's independent of the camera keyframes, so
  // it shows even before any pose has been captured (record car, then author).
  VehicleClip_DrawPath(s_PlaybackTime, s_Playing);
  CameraSequence *s = Sequence_Active();
  if (!s || s->poses.empty()) return;

  const int N = (int)s->poses.size();
  const bool closed = IsLoopClosedImpl(s);

  // Find the pose nearest in time to the scrub cursor
  int currentIdx = 0;
  float bestDt = 1e9f;
  for (int i = 0; i < N; ++i) {
    float d = fabsf(s->poses[i].t - s_PlaybackTime);
    if (d < bestDt) { bestDt = d; currentIdx = i; }
  }
  // Only treat as "current" if reasonably close (within 0.5s); otherwise
  // we're between keyframes and nothing should be highlighted.
  bool hasCurrent = bestDt < 0.5f;

  // Markers — type 28 = sphere. States:
  //  - loop seam (closed && i == 0): cyan, larger — the merged endpoint
  //  - editing (s_EditingPoseIdx == i): bright magenta — the menu's open
  //  - current (closest to scrub): yellow-green — playback "you are here"
  //  - locked to entity (live): teal — rides along with the anchor entity
  //  - normal: dim olive
  // Editing wins over current wins over locked. When the loop is closed,
  // we skip the duplicate last keyframe's marker entirely (it would draw
  // on top of pose[0]) — pose[0] gets the cyan "Loop ⟲" treatment instead.
  for (int i = 0; i < N; ++i) {
    if (closed && i == N - 1) continue; // duplicate of pose[0]; rendered there
    // ResolvePose recomputes the world-space coords from the entity-
    // relative offset when the keyframe is locked and its entity still
    // exists. That way the marker (and the orientation arrow + label
    // anchored to it) ride along with the locked entity, matching what
    // playback will actually do.
    const PoseKeyframe p = ResolvePose(s, i, closed);
    bool isEditing = (i == s_EditingPoseIdx);
    bool isCurrent = hasCurrent &&
                     (i == currentIdx ||
                      // Treat seam dup as the same logical pose as 0 for
                      // highlighting purposes — otherwise "current" would
                      // visibly flicker off pose[0] when scrub lands on
                      // the seam.
                      (closed && i == 0 && currentIdx == N - 1));
    bool isSeam = closed && i == 0;
    bool isLockedLive = p.entityHandle != 0 &&
                        ENTITY::DOES_ENTITY_EXIST(p.entityHandle);
    int r, g, b;
    float size;
    if (isEditing)         { r = 255; g =   0; b = 255; size = 0.55f; }
    else if (isCurrent)    { r = 255; g = 220; b =   0; size = 0.45f; }
    else if (isSeam)       { r =   0; g = 220; b = 255; size = 0.45f; }
    else if (isLockedLive) { r =  60; g = 180; b = 200; size = 0.32f; }
    else                   { r = g_SeqMarkerR; g = g_SeqMarkerG; b = g_SeqMarkerB;
                             size = g_SeqMarkerSize; }
    int a = s_Playing ? 90 : 200;
    if (isEditing) a = 230; // always vivid so the editor target is unmistakable
    if (isSeam && !s_Playing) a = 230;
    GRAPHICS::DRAW_MARKER(28, p.posX, p.posY, p.posZ, 0.0f, 0.0f, 0.0f,
                          0.0f, 0.0f, 0.0f, size, size, size, r, g, b, a,
                          FALSE, FALSE, 2, FALSE, NULL, NULL, FALSE);
    // Small forward-direction indicator: a tiny line from the marker out
    // in the keyframe's facing direction so the user can see orientation
    float yawRad = p.yaw * (3.14159265f / 180.0f);
    float pitchRad = p.pitch * (3.14159265f / 180.0f);
    float dx = -sinf(yawRad) * cosf(pitchRad);
    float dy = cosf(yawRad) * cosf(pitchRad);
    float dz = sinf(pitchRad);
    const float arrowLen = isEditing ? 1.2f : 0.8f;
    GRAPHICS::DRAW_LINE(p.posX, p.posY, p.posZ,
                        p.posX + dx * arrowLen, p.posY + dy * arrowLen,
                        p.posZ + dz * arrowLen, r, g, b, a);
    // Label above the marker. Seam pose gets the "Loop" tag so the user
    // can see at a glance which sequences are closed. Locked poses get a
    // small chain glyph so locks are visible at a glance in the world.
    char poseLabel[48];
    const char *lockTag = isLockedLive ? " [L]" : "";
    if (isSeam) sprintf_s(poseLabel, "KF 0 / Loop%s  %.1fs", lockTag, p.t);
    else        sprintf_s(poseLabel, "KF %d%s  %.1fs", i, lockTag, p.t);
    int la = s_Playing ? 140 : 230;
    DrawText3D(p.posX, p.posY, p.posZ + 0.5f, poseLabel, r, g, b, la, 0.32f);
  }

  // Path preview — line strip between consecutive poses. For spline
  // segments we sample the curve so what you see matches what playback
  // will do. When the loop is closed, the seam segment [N-2, N-1] uses
  // cyclic neighbors (matches ApplyPoseAtTime) so the preview line
  // curves smoothly back to the start.
  const int subdivPerSegment = 16;
  const float dur = closed ? s->poses[N - 1].t : 0.0f;
  for (int i = 0; i + 1 < N; ++i) {
    const PoseKeyframe a = ResolvePose(s, i, closed);
    const PoseKeyframe b = ResolvePose(s, i + 1, closed);
    int alpha = s_Playing ? 80 : 140;
    // Seam segment gets the cyan tint so it visually pops as the loop arc.
    bool seamSegment = closed && (i + 1) == N - 1;
    int lineR = seamSegment ? 0   : g_SeqPathR;
    int lineG = seamSegment ? 220 : g_SeqPathG;
    int lineB = seamSegment ? 255 : g_SeqPathB;

    if (b.path == PATH_SPLINE) {
      CtrlRef cp0 = ResolveCtrl(i - 1, N, closed, dur);
      CtrlRef cp3 = ResolveCtrl(i + 2, N, closed, dur);
      const PoseKeyframe p0 = ResolvePose(s, cp0.idx, closed);
      const PoseKeyframe p3 = ResolvePose(s, cp3.idx, closed);
      const float p0t = p0.t + cp0.tOffset;
      const float p3t = p3.t + cp3.tOffset;
      float prevX = a.posX, prevY = a.posY, prevZ = a.posZ;
      for (int k = 1; k <= subdivPerSegment; ++k) {
        float u = (float)k / (float)subdivPerSegment;
        float tEased = a.t + u * (b.t - a.t);
        float x = HermiteSpline1D(p0.posX, a.posX, b.posX, p3.posX,
                                  p0t, a.t, b.t, p3t, tEased);
        float y = HermiteSpline1D(p0.posY, a.posY, b.posY, p3.posY,
                                  p0t, a.t, b.t, p3t, tEased);
        float z = HermiteSpline1D(p0.posZ, a.posZ, b.posZ, p3.posZ,
                                  p0t, a.t, b.t, p3t, tEased);
        GRAPHICS::DRAW_LINE(prevX, prevY, prevZ, x, y, z,
                            lineR, lineG, lineB, alpha);
        prevX = x; prevY = y; prevZ = z;
      }
    } else {
      GRAPHICS::DRAW_LINE(a.posX, a.posY, a.posZ, b.posX, b.posY, b.posZ,
                          lineR, lineG, lineB, alpha);
    }
  }

  // Timestamp ticks along the camera path (mirrors the vehicle path). Interval
  // is the Appearance "Path Timestamps" setting; skipped where a keyframe already
  // shows its own time so the two don't overlap.
  float tsStep = SeqTimeLabelStep();
  if (tsStep > 0.0f) {
    float dur = s->poses[N - 1].t;
    int ta = s_Playing ? 110 : 180;
    for (float sec = tsStep; sec < dur; sec += tsStep) {
      bool nearKf = false;
      for (int i = 0; i < N; ++i)
        if (fabsf(s->poses[i].t - sec) < 0.2f) { nearKf = true; break; }
      if (nearKf) continue;
      float x, y, z;
      ComputePosAtTime(sec, s, x, y, z);
      char buf[16];
      if (sec == floorf(sec)) sprintf_s(buf, "%ds", (int)sec);
      else                    sprintf_s(buf, "%.1fs", sec);
      DrawText3D(x, y, z + 0.15f, buf, g_SeqPathR, g_SeqPathG, g_SeqPathB, ta,
                 0.24f);
      GRAPHICS::DRAW_LINE(x, y, z, x, y, z + 0.12f, g_SeqPathR, g_SeqPathG,
                          g_SeqPathB, ta);
    }
  }

  // Event text labels — projected onto the path at each event's scheduled
  // time. Stacks of labels at the same time get a small vertical fan so
  // they don't overlap. Color codes the event family.
  float lastT = -1.0f;
  int stackCount = 0;
  for (int i = 0; i < (int)s->events.size(); ++i) {
    const EffectEvent &e = s->events[i];
    float ex, ey, ez;
    ComputePosAtTime(e.t, s, ex, ey, ez);
    if (fabsf(e.t - lastT) < 0.05f) {
      stackCount++;
    } else {
      stackCount = 0;
      lastT = e.t;
    }
    // Warm orange for shake events; bright magenta for the one-shot
    // Randomize trigger so it visually pops; muted grey for any
    // legacy/removed kinds loaded from older sequence files so they're
    // visibly inert.
    int r, g, b;
    switch (e.kind) {
    case EFX_SHAKE_ENABLED:    case EFX_SHAKE_PRESET:
    case EFX_SHAKE_AMP:        case EFX_SHAKE_FREQ:
    case EFX_SHAKE_SPEED_AMP:  case EFX_SHAKE_SPEED_FREQ:
    case EFX_SHAKE_ROT_WEIGHT: case EFX_SHAKE_POS_WEIGHT:
    case EFX_SHAKE_STOP_STILL:
      r = 255; g = 160; b = 80; break;
    case EFX_SHAKE_RANDOMIZE:
      r = 255; g = 80;  b = 220; break;
    default:
      r = 140; g = 140; b = 140; break;
    }
    int a = s_Playing ? 130 : 220;
    std::string label = FormatEventLabel(e);
    // 0.9m above the path is well clear of the KF labels (0.5m offset).
    // Each stacked label adds another 0.3m.
    float zOff = 0.9f + 0.3f * stackCount;
    DrawText3D(ex, ey, ez + zOff, label.c_str(), r, g, b, a, 0.28f);
    // Small downward tick from label to the path point, so the user can
    // see which spot along the path the event fires at.
    GRAPHICS::DRAW_LINE(ex, ey, ez, ex, ey, ez + zOff - 0.1f,
                        r, g, b, a / 2);
  }
}

bool Sequence_TeleportToStart() {
  CameraSequence *s = Sequence_Active();
  if (!s || s->poses.empty()) return false;
  // Jump the flycam to the first keyframe's stored world pose. (Entity-locked
  // keyframes use pos*/rot* as their capture-time fallback, which is exactly the
  // right place to drop the camera.) With Stream Around Camera on, the player
  // ped then follows the cam here each frame and streams the area's LODs in.
  const PoseKeyframe &p = s->poses.front();
  FreeCam_SetPose(p.posX, p.posY, p.posZ, p.pitch, p.yaw, p.roll, p.fov);
  return true;
}

void Sequence_FrameTick() {
  if (!s_InMode) return;
  float dt = GAMEPLAY::GET_FRAME_TIME();
  if (dt <= 0.0f || dt > 0.1f) dt = 0.016f;

  // Bring a loaded clip's ghost vehicle into the world (polls model streaming;
  // cheap no-op once spawned / when there's no pending clip). Done here so it
  // happens even while idle, not only during scrub/playback.
  VehicleClip_EnsureGhost();

  static bool s_PrevTickPlaying = false;
  Sequence_Tick(dt);

  // A locked-segment playback frame leaves the scripted cam ATTACHED to the
  // entity (ATTACH_CAM_TO_ENTITY). When playback stops / pauses / ends, detach
  // it so free-fly authoring can translate the camera again — otherwise
  // SET_CAM_COORD is ignored while attached and the user can only rotate.
  // Edge-detected here so it fires exactly once on the playing->stopped
  // transition no matter where the stop came from (hotkey, menu, natural end),
  // and AFTER Sequence_Tick's final push (which would otherwise re-attach).
  if (s_PrevTickPlaying && !s_Playing) {
    SequenceDetachCamera();
  }
  s_PrevTickPlaying = s_Playing;

  // The sequence only drives the camera when it's playing AND has keyframes.
  // With no keyframes (vehicle-only playback) the camera is left in free-fly.
  CameraSequence *sFt = Sequence_Active();
  bool cameraDriven = s_Playing && sFt && !sFt->poses.empty();

  // Hold the recorded vehicle on its current scrub pose while authoring (not
  // playing), so it stays put where the playhead is as you fly around to frame
  // a keyframe. Velocity off — it's parked, not driving. (While playing, the
  // tick above drives the vehicles instead.)
  if (!s_Playing) VehicleClip_ApplyAtTime(s_PlaybackTime, false);

  // Free-fly the camera whenever the sequence isn't driving it — that's both
  // authoring (not playing) AND vehicle-only playback (playing, no keyframes).
  // UpdateFreeCamera owns input + collision + IGCS + shake handling.
  if (!cameraDriven && g_FreeCamActive) {
    UpdateFreeCamera();
  }
}

void Sequence_Tick(float dt) {
  if (!s_InMode) return;

  // Hotkeys (only respond while in Sequence mode)
  if (IsKeyJustUp(g_SeqHotkeyAdd))  { Sequence_CapturePoseAtCurrentTime(); }
  if (IsKeyJustUp(g_SeqHotkeyPlay)) { Sequence_TogglePlay(); }
  if (IsKeyJustUp(g_SeqHotkeyStop)) { Sequence_Stop(); }
  if (IsKeyJustUp(g_SeqHotkeyNext)) { Sequence_JumpToNextPose(); }

  // Drive playback if running. When NOT playing we leave the camera
  // alone so the user can free-fly via UpdateFreeCamera (the dispatcher
  // in script.cpp calls it for us). Scrub preview is applied on demand
  // by Sequence_SetCurrentTime, not every frame.
  if (s_Playing) {
    CameraSequence *s = Sequence_Active();
    bool hasPoses = s && !s->poses.empty();
    // Keep playing as long as there's something on the timeline: camera
    // keyframes OR recorded vehicles. With vehicles only, the camera is left in
    // free-fly and just the clips replay.
    if (!s || (!hasPoses && !VehicleClip_HasData())) {
      Sequence_Stop();
      return;
    }
    float speed = s->playbackSpeed > 0.0f ? s->playbackSpeed : 1.0f;
    bool firstTick = s_FirstPlayTick;
    s_FirstPlayTick = false;
    bool wrapped = false;
    s_PlaybackTime += dt * speed;
    float dur = Sequence_TotalDuration();
    if (s_PlaybackTime >= dur) {
      if (s->loop && dur > 0.0001f) {
        // Fire any events still pending in the tail (s_LastTickTime, dur] BEFORE
        // wrapping — otherwise resetting s_LastTickTime to 0 collapses the event
        // window and events near the end of the loop are skipped every cycle.
        DriveEvents(s, s_LastTickTime, dur, firstTick);
        s_PlaybackTime = fmodf(s_PlaybackTime, dur);
        s_LastTickTime = 0.0f;
        wrapped = true;
      } else {
        s_PlaybackTime = dur;
        s_Playing = false;
        g_SequencePlaybackActive = false;
        // End of non-looped playback — restore the pre-play shake state
        // so the camera doesn't keep shaking forever in authoring mode.
        RestoreShakeSnapshot();
      }
    }
    ApplyPoseAtTime(s_PlaybackTime, s);
    DriveEvents(s, s_LastTickTime, s_PlaybackTime, firstTick || wrapped);
    s_LastTickTime = s_PlaybackTime;
    if (hasPoses) SequencePushToEngine(dt); // own the camera only with keyframes
  }

  // Always draw keyframe markers + path preview while in Sequence mode
  // (dimmed during playback so they don't dominate the shot).
  DrawSequenceMarkers();
}

// ============================================================
//  Persistence — SimpleCamera_Sequences.ini
// ============================================================

static void GetSequencesIniPath(char *outPath, size_t bufSize) {
  HMODULE hMod = NULL;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     (LPCSTR)GetSequencesIniPath, &hMod);
  GetModuleFileNameA(hMod, outPath, (DWORD)bufSize);
  char *last = strrchr(outPath, '\\');
  if (last)
    strcpy_s(last + 1, bufSize - (last + 1 - outPath),
             "SimpleCamera_Sequences.ini");
}

// Path to a sequence's vehicle-clip JSON sidecar:
//   <plugin dir>\SimpleCamera_Clips\<sanitized name>.json
// The clip is stored per-sequence (named after it) so loading a sequence brings
// back its recorded vehicle. Creates the Clips directory on demand.
static void GetClipPath(const char *seqName, char *outPath, size_t bufSize) {
  char base[MAX_PATH];
  GetSequencesIniPath(base, sizeof(base)); // ...\SimpleCamera_Sequences.ini
  char *last = strrchr(base, '\\');
  if (last) *(last + 1) = '\0'; // -> "...\" (plugin dir)
  char dir[MAX_PATH];
  sprintf_s(dir, "%sSimpleCamera_Clips", base);
  CreateDirectoryA(dir, NULL);
  char safe[128];
  size_t n = 0;
  for (const char *p = seqName; *p && n < sizeof(safe) - 1; ++p) {
    char c = *p;
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ' ';
    safe[n++] = ok ? c : '_';
  }
  safe[n] = '\0';
  sprintf_s(outPath, bufSize, "%s\\%s.json", dir, safe);
}

// Directory holding the per-sequence merged JSON files, i.e.
// "<plugin dir>\SimpleCamera_Sequences".
static void GetSeqDir(char *outDir, size_t bufSize) {
  char base[MAX_PATH];
  GetSequencesIniPath(base, sizeof(base)); // ...\SimpleCamera_Sequences.ini
  char *last = strrchr(base, '\\');
  if (last) *(last + 1) = '\0'; // -> plugin dir
  sprintf_s(outDir, bufSize, "%sSimpleCamera_Sequences", base);
  CreateDirectoryA(outDir, NULL);
}

// Path to a sequence's merged JSON: <seq dir>\<sanitized name>.json
static void GetSeqJsonPath(const char *seqName, char *outPath, size_t bufSize) {
  char dir[MAX_PATH];
  GetSeqDir(dir, sizeof(dir));
  char safe[128];
  size_t n = 0;
  for (const char *p = seqName; *p && n < sizeof(safe) - 1; ++p) {
    char c = *p;
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ' ';
    safe[n++] = ok ? c : '_';
  }
  safe[n] = '\0';
  sprintf_s(outPath, bufSize, "%s\\%s.json", dir, safe);
}

static void ParsePose(const char *line, PoseKeyframe &p) {
  // Format: t=X,x=X,y=X,z=X,pitch=X,yaw=X,roll=X,fov=X,ease=N,path=N
  //         [,locked=1,locX=F,locY=F,locZ=F]    (entity lock fields, optional)
  p = PoseKeyframe{};
  p.ease = EASE_LINEAR;
  p.path = PATH_LINEAR;
  bool hadLockFlag = false;
  char buf[512]; strncpy_s(buf, sizeof(buf), line, _TRUNCATE);
  char *ctx = nullptr;
  for (char *tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(nullptr, ",", &ctx)) {
    char *eq = strchr(tok, '=');
    if (!eq) continue;
    *eq = '\0';
    const char *k = tok;
    const char *v = eq + 1;
    if      (!strcmp(k, "t"))     p.t = (float)atof(v);
    else if (!strcmp(k, "x"))     p.posX = (float)atof(v);
    else if (!strcmp(k, "y"))     p.posY = (float)atof(v);
    else if (!strcmp(k, "z"))     p.posZ = (float)atof(v);
    else if (!strcmp(k, "pitch")) p.pitch = (float)atof(v);
    else if (!strcmp(k, "yaw"))   p.yaw = (float)atof(v);
    else if (!strcmp(k, "roll"))  p.roll = (float)atof(v);
    else if (!strcmp(k, "fov"))   p.fov = (float)atof(v);
    else if (!strcmp(k, "ease"))  p.ease = (EaseType)atoi(v);
    else if (!strcmp(k, "path"))  p.path = (PathType)atoi(v);
    // Entity lock: 'locked' is a flag (1 when the keyframe was authored
    // with a live entity lock). The handle itself is NOT serialized —
    // GTA entity handles are session-scoped, so a stored handle from a
    // previous session would either be invalid OR (worse) collide with
    // an unrelated entity. Always set handle to 0 on load; the local
    // offset + entity rotation snapshot are preserved so the user could
    // manually relock to a similar entity later. While handle=0 the
    // playback path falls through to the world-space fallback (which is
    // the right behavior).
    else if (!strcmp(k, "locked")) hadLockFlag = (atoi(v) != 0);
    else if (!strcmp(k, "locX"))   p.localOffsetX = (float)atof(v);
    else if (!strcmp(k, "locY"))   p.localOffsetY = (float)atof(v);
    else if (!strcmp(k, "locZ"))   p.localOffsetZ = (float)atof(v);
    // Entity rotation at lock time (matches the ENTITY::GET_ENTITY_ROTATION
    // (entity, 2) layout: pitch, roll, yaw).
    else if (!strcmp(k, "lEntP"))  p.lockEntPitch = (float)atof(v);
    else if (!strcmp(k, "lEntY"))  p.lockEntYaw   = (float)atof(v);
    else if (!strcmp(k, "lEntR"))  p.lockEntRoll  = (float)atof(v);
    // Entity world position at lock time (for non-rigid translate-only follow).
    else if (!strcmp(k, "lEpX"))   p.lockEntPosX  = (float)atof(v);
    else if (!strcmp(k, "lEpY"))   p.lockEntPosY  = (float)atof(v);
    else if (!strcmp(k, "lEpZ"))   p.lockEntPosZ  = (float)atof(v);
  }
  // Keep entityHandle at 0 on load regardless of the locked flag — see
  // comment above. If hadLockFlag is true we still cleared the handle;
  // it just means the offset values are meaningful and the menu can
  // surface "Lock to current entity" as a one-click way to re-anchor.
  (void)hadLockFlag;
  p.entityHandle = 0;
}

static void ParseEvent(const char *line, EffectEvent &e) {
  e = EffectEvent{};
  char buf[256]; strncpy_s(buf, sizeof(buf), line, _TRUNCATE);
  char *ctx = nullptr;
  for (char *tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(nullptr, ",", &ctx)) {
    char *eq = strchr(tok, '=');
    if (!eq) continue;
    *eq = '\0';
    const char *k = tok;
    const char *v = eq + 1;
    if      (!strcmp(k, "t"))     e.t = (float)atof(v);
    else if (!strcmp(k, "kind"))  e.kind = (EffectKind)atoi(v);
    else if (!strcmp(k, "value")) e.value = (float)atof(v);
    else if (!strcmp(k, "ramp"))  e.ramp = atoi(v) != 0;
  }
}

// ---- Legacy INI loader (kept only to migrate old saves to JSON) ----
static void LoadAllLegacyIni() {
  char path[MAX_PATH];
  GetSequencesIniPath(path, sizeof(path));
  char names[8192] = {0};
  DWORD got = GetPrivateProfileSectionNamesA(names, sizeof(names), path);
  if (got == 0) return;
  for (const char *p = names; *p; p += strlen(p) + 1) {
    if (strncmp(p, "Sequence:", 9) != 0) continue;
    CameraSequence seq;
    seq.name = p + 9;
    seq.loop = GetPrivateProfileIntA(p, "Loop", 0, path) != 0;
    char speedBuf[32];
    GetPrivateProfileStringA(p, "Speed", "1.0", speedBuf, sizeof(speedBuf), path);
    seq.playbackSpeed = (float)atof(speedBuf);
    int poseCount = GetPrivateProfileIntA(p, "PoseCount", 0, path);
    for (int i = 0; i < poseCount; ++i) {
      char key[32]; sprintf_s(key, "Pose%d", i);
      char buf[512];
      GetPrivateProfileStringA(p, key, "", buf, sizeof(buf), path);
      if (!*buf) continue;
      PoseKeyframe pk; ParsePose(buf, pk);
      seq.poses.push_back(pk);
    }
    int eventCount = GetPrivateProfileIntA(p, "EventCount", 0, path);
    for (int i = 0; i < eventCount; ++i) {
      char key[32]; sprintf_s(key, "Event%d", i);
      char buf[256];
      GetPrivateProfileStringA(p, key, "", buf, sizeof(buf), path);
      if (!*buf) continue;
      EffectEvent ev; ParseEvent(buf, ev);
      seq.events.push_back(ev);
    }
    s_Sequences.push_back(seq);
  }
  if (!s_Sequences.empty()) s_ActiveIdx = 0;
}

// ---- Minimal JSON scanning helpers (sequence side) ----
static const char *SqWs(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
  return p;
}
static const char *SqAfterKey(const char *base, const char *end, const char *key) {
  char pat[40];
  sprintf_s(pat, "\"%s\"", key);
  size_t plen = strlen(pat);
  for (const char *p = base; p + plen <= end; ++p) {
    if (strncmp(p, pat, plen) == 0) {
      p = SqWs(p + plen);
      if (*p == ':') return SqWs(p + 1);
      return SqWs(p);
    }
  }
  return nullptr;
}
static void SqWriteStr(FILE *f, const char *s) { // JSON-escape quotes/backslash
  for (; *s; ++s) {
    if (*s == '"' || *s == '\\') fputc('\\', f);
    fputc(*s, f);
  }
}

static void WritePoseJson(FILE *f, const PoseKeyframe &p, bool last) {
  fprintf(f,
          "    {\"t\":%.3f,\"x\":%.3f,\"y\":%.3f,\"z\":%.3f,\"pitch\":%.3f,"
          "\"yaw\":%.3f,\"roll\":%.3f,\"fov\":%.3f,\"ease\":%d,\"path\":%d,"
          "\"locked\":%d,\"locX\":%.3f,\"locY\":%.3f,\"locZ\":%.3f,"
          "\"lEntP\":%.3f,\"lEntY\":%.3f,\"lEntR\":%.3f,"
          "\"lEpX\":%.3f,\"lEpY\":%.3f,\"lEpZ\":%.3f}%s\n",
          p.t, p.posX, p.posY, p.posZ, p.pitch, p.yaw, p.roll, p.fov,
          (int)p.ease, (int)p.path, p.entityHandle != 0 ? 1 : 0, p.localOffsetX,
          p.localOffsetY, p.localOffsetZ, p.lockEntPitch, p.lockEntYaw,
          p.lockEntRoll, p.lockEntPosX, p.lockEntPosY, p.lockEntPosZ,
          last ? "" : ",");
}

static void WriteEventJson(FILE *f, const EffectEvent &e, bool last) {
  fprintf(f, "    {\"t\":%.3f,\"kind\":%d,\"value\":%.4f,\"ramp\":%d}%s\n", e.t,
          (int)e.kind, e.value, e.ramp ? 1 : 0, last ? "" : ",");
}

static void ParsePoseJson(const char *start, const char *end, PoseKeyframe &p) {
  p = PoseKeyframe{};
  p.ease = EASE_LINEAR;
  p.path = PATH_LINEAR;
  const char *k;
  if ((k = SqAfterKey(start, end, "t")))     p.t = (float)atof(k);
  if ((k = SqAfterKey(start, end, "x")))     p.posX = (float)atof(k);
  if ((k = SqAfterKey(start, end, "y")))     p.posY = (float)atof(k);
  if ((k = SqAfterKey(start, end, "z")))     p.posZ = (float)atof(k);
  if ((k = SqAfterKey(start, end, "pitch"))) p.pitch = (float)atof(k);
  if ((k = SqAfterKey(start, end, "yaw")))   p.yaw = (float)atof(k);
  if ((k = SqAfterKey(start, end, "roll")))  p.roll = (float)atof(k);
  if ((k = SqAfterKey(start, end, "fov")))   p.fov = (float)atof(k);
  if ((k = SqAfterKey(start, end, "ease")))  p.ease = (EaseType)atoi(k);
  if ((k = SqAfterKey(start, end, "path")))  p.path = (PathType)atoi(k);
  if ((k = SqAfterKey(start, end, "locX")))  p.localOffsetX = (float)atof(k);
  if ((k = SqAfterKey(start, end, "locY")))  p.localOffsetY = (float)atof(k);
  if ((k = SqAfterKey(start, end, "locZ")))  p.localOffsetZ = (float)atof(k);
  if ((k = SqAfterKey(start, end, "lEntP"))) p.lockEntPitch = (float)atof(k);
  if ((k = SqAfterKey(start, end, "lEntY"))) p.lockEntYaw = (float)atof(k);
  if ((k = SqAfterKey(start, end, "lEntR"))) p.lockEntRoll = (float)atof(k);
  if ((k = SqAfterKey(start, end, "lEpX")))  p.lockEntPosX = (float)atof(k);
  if ((k = SqAfterKey(start, end, "lEpY")))  p.lockEntPosY = (float)atof(k);
  if ((k = SqAfterKey(start, end, "lEpZ")))  p.lockEntPosZ = (float)atof(k);
  p.entityHandle = 0; // handles are session-scoped — never restored
}

static void ParseEventJson(const char *start, const char *end, EffectEvent &e) {
  e = EffectEvent{};
  const char *k;
  if ((k = SqAfterKey(start, end, "t")))     e.t = (float)atof(k);
  if ((k = SqAfterKey(start, end, "kind")))  e.kind = (EffectKind)atoi(k);
  if ((k = SqAfterKey(start, end, "value"))) e.value = (float)atof(k);
  if ((k = SqAfterKey(start, end, "ramp")))  e.ramp = atoi(k) != 0;
}

// Iterate the flat {...} objects of a named array within [data,end), invoking
// `fn(objStart, objEnd)` for each. Objects must contain no nested braces (poses
// and events are flat scalars), so a plain '}' scan terminates each.
template <typename F>
static void ForEachArrayObject(const char *data, const char *end,
                               const char *arrayKey, F fn) {
  const char *ak = SqAfterKey(data, end, arrayKey);
  if (!ak) return;
  const char *p = strchr(ak, '[');
  if (!p || p >= end) return;
  ++p;
  while (p < end) {
    p = SqWs(p);
    if (*p == ']' || *p == 0) break;
    if (*p != '{') { ++p; continue; }
    const char *objEnd = strchr(p, '}');
    if (!objEnd || objEnd >= end) break;
    fn(p, objEnd);
    p = objEnd + 1;
  }
}

static void ParseSequenceJson(const char *data, int len, CameraSequence &seq) {
  const char *end = data + len;
  seq.playbackSpeed = 1.0f;
  const char *k = SqAfterKey(data, end, "name");
  if (k) {
    k = SqWs(k);
    if (*k == '"') {
      ++k;
      std::string nm;
      while (*k && *k != '"') {
        if (*k == '\\' && k[1]) ++k;
        nm.push_back(*k++);
      }
      seq.name = nm;
    }
  }
  if (seq.name.empty()) seq.name = "Untitled";
  if ((k = SqAfterKey(data, end, "loop"))) seq.loop = atoi(k) != 0;
  if ((k = SqAfterKey(data, end, "playbackSpeed")))
    seq.playbackSpeed = (float)atof(k);
  ForEachArrayObject(data, end, "poses", [&](const char *a, const char *b) {
    PoseKeyframe pk; ParsePoseJson(a, b, pk); seq.poses.push_back(pk);
  });
  ForEachArrayObject(data, end, "events", [&](const char *a, const char *b) {
    EffectEvent ev; ParseEventJson(a, b, ev); seq.events.push_back(ev);
  });
  // The interpolators and event scan assume ascending time; a hand-edited or
  // out-of-order file must not feed them a negative segment duration.
  std::sort(seq.poses.begin(), seq.poses.end(),
            [](const PoseKeyframe &a, const PoseKeyframe &b) { return a.t < b.t; });
  std::sort(seq.events.begin(), seq.events.end(),
            [](const EffectEvent &a, const EffectEvent &b) { return a.t < b.t; });
}

// Write sequence `idx` as a merged JSON file. The clip section uses the runtime
// (singleton) clips, so the caller must ensure those belong to `idx` — true for
// the active sequence and during migration.
static void SaveSequenceJson(int idx) {
  if (idx < 0 || idx >= (int)s_Sequences.size()) return;
  const CameraSequence &s = s_Sequences[idx];
  // Don't persist a completely empty sequence (no keyframes, events or vehicle
  // clips). Camera Sequence mode opens on a fresh scratch sequence; this stops
  // that throwaway from littering the folder with an empty file when you switch
  // away from it. (Every SaveSequenceJson caller saves the sequence whose clips
  // are loaded in the runtime module, so VehicleClip_Count() matches `s`.)
  if (s.poses.empty() && s.events.empty() && VehicleClip_Count() == 0) return;
  char path[MAX_PATH];
  GetSeqJsonPath(s.name.c_str(), path, sizeof(path));
  FILE *f = nullptr;
  if (fopen_s(&f, path, "wb") != 0 || !f) return;
  fprintf(f, "{\n  \"version\": 1,\n  \"name\": \"");
  SqWriteStr(f, s.name.c_str());
  fprintf(f, "\",\n  \"loop\": %d,\n  \"playbackSpeed\": %.4f,\n", s.loop ? 1 : 0,
          s.playbackSpeed);
  fprintf(f, "  \"poses\": [\n");
  for (int i = 0; i < (int)s.poses.size(); ++i)
    WritePoseJson(f, s.poses[i], i + 1 == (int)s.poses.size());
  fprintf(f, "  ],\n  \"events\": [\n");
  for (int i = 0; i < (int)s.events.size(); ++i)
    WriteEventJson(f, s.events[i], i + 1 == (int)s.events.size());
  fprintf(f, "  ],\n");
  VehicleClip_WriteClipsJson(f); // "sampleHz" + "clips":[...] (runtime clips)
  fprintf(f, "}\n");
  fclose(f);
}

// Load the active sequence's embedded clips into the runtime clip module.
static void LoadActiveSequenceClips() {
  CameraSequence *s = Sequence_Active();
  if (!s) { VehicleClip_Clear(); return; }
  char path[MAX_PATH];
  GetSeqJsonPath(s->name.c_str(), path, sizeof(path));
  FILE *f = nullptr;
  if (fopen_s(&f, path, "rb") != 0 || !f) { VehicleClip_Clear(); return; }
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (len <= 0) { fclose(f); VehicleClip_Clear(); return; }
  std::vector<char> buf((size_t)len + 1);
  size_t rd = fread(buf.data(), 1, (size_t)len, f);
  fclose(f);
  buf[rd] = 0;
  if (!VehicleClip_ParseClipsFromBuffer(buf.data(), (int)rd))
    VehicleClip_Clear();
}

// One-time migration: rewrite legacy-INI sequences as merged JSON, pulling each
// one's clips from its old sidecar.
static void MigrateLegacyToJson() {
  for (int i = 0; i < (int)s_Sequences.size(); ++i) {
    char clip[MAX_PATH];
    GetClipPath(s_Sequences[i].name.c_str(), clip, sizeof(clip));
    if (!VehicleClip_LoadFromFile(clip)) VehicleClip_Clear();
    SaveSequenceJson(i);
  }
  s_ActiveIdx = s_Sequences.empty() ? -1 : 0;
  LoadActiveSequenceClips();
}

// Append a fresh, empty sequence and make it the active one (without touching
// the saved sequences already in the list). Used so Camera Sequence mode always
// opens on a blank take instead of auto-loading a previous one.
static void StartFreshActiveSequence() {
  CameraSequence s;
  s.name = "Untitled";
  s.loop = false;
  s.playbackSpeed = 1.0f;
  s_Sequences.push_back(s);
  s_ActiveIdx = (int)s_Sequences.size() - 1;
  Sequence_Stop();
  VehicleClip_Clear(); // a fresh sequence has no vehicle clips / ghosts
}

void Sequence_LoadAll() {
  s_Sequences.clear();
  s_ActiveIdx = -1;

  // Load every per-sequence JSON file from the sequences directory.
  char dir[MAX_PATH];
  GetSeqDir(dir, sizeof(dir));
  char pattern[MAX_PATH];
  sprintf_s(pattern, "%s\\*.json", dir);
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pattern, &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      char full[MAX_PATH];
      sprintf_s(full, "%s\\%s", dir, fd.cFileName);
      FILE *f = nullptr;
      if (fopen_s(&f, full, "rb") != 0 || !f) continue;
      fseek(f, 0, SEEK_END);
      long len = ftell(f);
      fseek(f, 0, SEEK_SET);
      if (len <= 0) { fclose(f); continue; }
      std::vector<char> buf((size_t)len + 1);
      size_t rd = fread(buf.data(), 1, (size_t)len, f);
      fclose(f);
      buf[rd] = 0;
      CameraSequence seq;
      seq.loop = false;
      seq.playbackSpeed = 1.0f;
      ParseSequenceJson(buf.data(), (int)rd, seq);
      s_Sequences.push_back(seq);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
  }

  // If there are no JSON sequences yet, pull in any legacy-INI sequences and
  // rewrite them as JSON so they show up in the list too.
  if (s_Sequences.empty()) {
    LoadAllLegacyIni();
    if (!s_Sequences.empty()) MigrateLegacyToJson();
  }

  // Don't auto-activate a saved sequence. Saved takes stay in the list (pick one
  // from Select Active when you want it), but Camera Sequence mode always opens
  // on a fresh, empty sequence — so a previous take's keyframes and vehicle
  // ghosts never stream in unasked (which was unloading nearby LODs on entry).
  StartFreshActiveSequence();
}

void Sequence_SaveAll() {
  // Only the active sequence can be edited, so persisting it (with its clips) is
  // sufficient; the others were written when last active / on switch-away.
  SaveSequenceJson(s_ActiveIdx);
}
