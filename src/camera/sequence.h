/*
        GTA V Free Camera / Photo Mode Plugin
        Camera Sequence Mode

        Keyframe-driven camera animation. Authors a list of camera poses
        (PoseKeyframe) over a timeline and plays them back with configurable
        easing, optional spline interpolation, and a parallel track of
        EffectEvents (DoF, shake, time, weather) that fire on schedule.

        Lives alongside Free Camera mode; the top-level mode dispatcher
        decides which one drives the camera each frame.
*/

#pragma once

#include <string>
#include <vector>

// ============================================================
//  Public types
// ============================================================

enum EaseType {
  EASE_LINEAR = 0,
  EASE_IN_OUT = 1,
  EASE_IN = 2,
  EASE_OUT = 3,
  EASE_HOLD = 4, // freeze on previous keyframe until reaching this t
};

enum PathType {
  PATH_LINEAR = 0,
  PATH_SPLINE = 1, // Catmull-Rom across neighboring poses
};

enum EffectKind {
  EFX_SHAKE_ENABLED = 0, // value = 0 or 1
  EFX_SHAKE_PRESET = 1,        // value = 0..4 (preset index); auto-enables shake
  EFX_SHAKE_AMP = 2,           // float
  EFX_SHAKE_FREQ = 3,          // Hz
  EFX_SHAKE_SPEED_AMP = 4,     // speed→amplitude coupling (0..2)
  EFX_SHAKE_SPEED_FREQ = 5,    // speed→frequency coupling (0..2)
  EFX_SHAKE_ROT_WEIGHT = 6,    // rotation contribution (0..2)
  EFX_SHAKE_POS_WEIGHT = 7,    // position contribution (0..2)
  EFX_SHAKE_STOP_STILL = 8,    // stop-when-still toggle (0/1)
  EFX_SHAKE_RANDOMIZE = 9,     // fire = re-roll noise pattern (value ignored)
  // Note: enum values are stable IDs. Sequences saved with earlier
  // builds that used 4..11 for DOF / time / weather / walk / world-freeze
  // kinds now collide with the new shake kinds — open those events in
  // the editor and either delete them or set them to the right kind.
  // Kind 10 ("World Speed" / slow-motion) was REMOVED — slow motion now
  // lives solely in the World & Scene menu (g_WorldTimeScale) so the two
  // can't fight over SET_TIME_SCALE. Old saves with kind 10 load inert.
  EFX_COUNT = 10,
};

const char *EaseName(EaseType e);
const char *PathName(PathType p);
const char *EffectName(EffectKind k);

struct PoseKeyframe {
  float t;
  float posX, posY, posZ;
  float pitch, yaw, roll;
  float fov;
  EaseType ease;
  PathType path;
  // Entity lock (mirrors Free Camera's follow-target system).
  //
  // When `entityHandle != 0` AND the entity still exists at playback time,
  // the world-space `pos*` fields are treated as a fallback and the real
  // playback position is computed each frame as
  // GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(entityHandle, localOffset*).
  // That makes the keyframe ride along with a moving vehicle or ped just
  // like the free-cam's rigid follow mode.
  //
  // Handles are session-scoped — on INI load we always reset entityHandle
  // to 0 (a stored handle from a previous session would either be invalid
  // or, worse, collide with an unrelated entity). The local offset is
  // still persisted so the user could re-lock to the same kind of entity
  // mid-session without losing the captured relative position.
  //
  // Rotation lock works the same way as position lock:
  //
  //   - At lock time we snapshot the entity's WORLD rotation into
  //     lockEntPitch/Yaw/Roll. The keyframe's own pitch/yaw/roll keep
  //     their world meaning (the camera orientation at capture).
  //   - At playback ResolvePose computes:
  //         localQ = inverse(lockEntQ) * camWorldQ_at_capture
  //         worldQ_now = entQ_now * localQ
  //     and writes the recomposed world Euler back into the returned
  //     PoseKeyframe copy. The spline / linear interpolator downstream
  //     never has to know rotation is locked.
  //
  // Storing world-at-capture (rather than baked local Eulers) keeps the
  // menu's pitch/yaw/roll display intuitive — it always shows where
  // the camera was actually looking — and clearing the lock leaves
  // those fields meaningful without any reverse-transform.
  int entityHandle;
  float localOffsetX, localOffsetY, localOffsetZ;
  float lockEntPitch, lockEntYaw, lockEntRoll;
  // Entity's WORLD position at lock time. Used by the non-rigid (translate-
  // only) follow in ResolvePose to move the keyframe by the entity's
  // translation since lock without orbiting it. Captured alongside
  // lockEntPitch/Yaw/Roll; mirrored on loop-closure the same way.
  float lockEntPosX, lockEntPosY, lockEntPosZ;
};

struct EffectEvent {
  float t;
  EffectKind kind;
  float value;
  bool ramp; // if true, lerp from previous event of same kind across [prev.t, this.t]
};

struct CameraSequence {
  std::string name;
  std::vector<PoseKeyframe> poses;
  std::vector<EffectEvent> events;
  bool loop;
  float playbackSpeed; // 1.0 = real-time
};

// ============================================================
//  Mode lifecycle
// ============================================================

// Called when the user enters Camera Sequence mode via the picker.
// Sets up the scripted camera; if no sequence exists yet, creates an empty one.
void Sequence_EnterMode();

// Called when leaving Camera Sequence mode.
// Stops playback, tears down the scripted camera.
void Sequence_ExitMode();

// Are we currently in Camera Sequence mode? (Set/cleared by Enter/ExitMode.)
bool Sequence_IsInMode();

// ============================================================
//  Per-frame tick
// ============================================================

// Called every frame from the main loop while in Sequence mode. Handles
// hotkeys, advances playback, applies pose + fires events, drives the
// scripted camera.
void Sequence_Tick(float dt);

// One-call per-frame driver for Sequence mode. Computes dt, runs
// Sequence_Tick, and when not playing also runs UpdateFreeCamera so the
// user can free-fly while authoring. Safe to call from either the main
// script loop OR a menu's inner draw loop — these contexts are mutually
// exclusive (ScriptHookV's WAIT(0) inside a menu does NOT re-enter main).
void Sequence_FrameTick();

// Is playback currently active (running, not paused)?
bool Sequence_IsPlaying();

// Total duration of the active sequence (max of last pose t and last event t).
float Sequence_TotalDuration();

// Current scrub / playback position in seconds.
float Sequence_CurrentTime();
void Sequence_SetCurrentTime(float t);

// ============================================================
//  Authoring / playback control
// ============================================================

void Sequence_Play();
void Sequence_Stop();
void Sequence_TogglePlay();
void Sequence_JumpToNextPose();
void Sequence_JumpToPrevPose();

// Move the flycam to the active sequence's first keyframe (so you don't have to
// fly across the map). Returns false if there's no active sequence / no poses.
bool Sequence_TeleportToStart();

// Capture current camera world pose into a new PoseKeyframe at the
// current scrub time (or at sequence end if no scrub). Returns the
// index of the inserted keyframe in the active sequence's pose list.
int Sequence_CapturePoseAtCurrentTime();

// Capture the current free-cam entity-lock state (g_FollowMode == 2 +
// g_FollowTargetEntity) into the keyframe, given its world-space
// position. If no entity is currently locked, clears the keyframe's
// lock fields. Used by capture / recapture paths so the keyframe's
// "ride along with this entity" state matches what the camera is
// doing right now. Only the "Aimed Entity" follow mode counts —
// Player follow is intentionally ignored (locking keyframes to the
// controlled ped is rarely what the user wants).
void Sequence_CaptureLockForPose(PoseKeyframe &p);

// Batch operations on every keyframe of the active sequence.
//
// ApplyLockToAll: take an entity handle (typically g_FollowTargetEntity)
// and re-anchor every keyframe to it. The entity-local offset is computed
// per keyframe from its stored world-space coords, so each keyframe keeps
// its original world position at the moment of binding — the entity then
// drags them along during playback. Pass 0 to no-op. Returns the number
// of keyframes updated.
//
// ClearAllLocks: drop the lock from every keyframe in the active
// sequence. World-space coords are left intact, so playback continues
// to work with the stored positions. Returns the number cleared.
//
// Both are intended for the Sequence menu's "Follow & Entity Lock"
// submenu — they let the user bulk-re-anchor a previously-locked
// sequence after loading from disk (where handles are deliberately
// dropped), or wipe locks in one click before saving a "world-space
// only" version.
int Sequence_ApplyLockToAll(int entityHandle);
int Sequence_ClearAllLocks();

// Count of keyframes in the active sequence that currently have an
// entity-lock binding (live or authored-but-stale). Useful for the
// follow submenu's status display.
int Sequence_LockedPoseCount();

// Active sequence accessors (the one being edited/played)
CameraSequence *Sequence_Active();
int Sequence_ActiveIndex();
int Sequence_Count();
CameraSequence *Sequence_At(int index);
void Sequence_SetActive(int index);
void Sequence_New(const char *name);
void Sequence_DeleteActive();

// Pose / event mutations on the active sequence
void Sequence_DeletePose(int poseIdx);
void Sequence_AddEvent(EffectKind kind, float t, float value, bool ramp);
void Sequence_DeleteEvent(int eventIdx);
void Sequence_SortByTime(); // re-sort both arrays after time edits

// Duplicate keyframe `poseIdx`: the copy lands at the midpoint to the next
// keyframe (shaping a segment), or +2s past the end when duplicating the last
// one (extending the shot with a hold). Returns the copy's post-sort index,
// or -1 on failure.
int Sequence_DuplicatePose(int poseIdx);

// Re-space all middle keyframes evenly between the first and last one's times
// (constant-speed pass). First/last keep their t; events are untouched.
// Returns how many keyframes were re-timed (0 when fewer than 3 poses).
int Sequence_DistributeTimes();

// Scale the time of all pose keyframes + effect events by `factor` (>1 = slower/
// longer, <1 = faster/shorter). Rewrites the keyframe t values in place.
void Sequence_ScaleTimes(float factor);

// Translate every pose keyframe's world position by (dx,dy,dz) metres, relocating
// the whole shot without re-authoring it (e.g. re-using a loop at a new spot on
// the map). The entity-lock world anchor is shifted by the same delta so locked
// keyframes keep riding their entity and only world-space (unlocked) keyframes
// actually move. Rotation / FOV / timing are untouched.
void Sequence_TranslateAll(float dx, float dy, float dz);

// ============================================================
//  Persistence (SimpleCamera_Sequences.ini)
// ============================================================

void Sequence_LoadAll();
void Sequence_SaveAll();

// ============================================================
//  Loop closure
// ============================================================
//
// A sequence is "loop-closed" when its `loop` flag is true AND its
// first and last keyframes are close enough (position, rotation, FOV)
// that the wrap from t=duration back to t=0 should be seamless. When
// closed, the spline tangents at the seam wrap cyclically — predecessor
// of pose[0] reads pose[N-2] with a time offset of −duration, successor
// of pose[N-1] reads pose[1] with +duration — and the last segment's
// endpoint values are substituted from pose[0] so position lines up
// exactly. Velocity is C1-continuous across the wrap.
//
// Tolerances are hardcoded: 1 m position, 5° rotation, 2° FOV.

bool Sequence_IsLoopClosed();

struct LoopGap {
  float posDist;    // meters
  float pitchDelta; // degrees, abs, shortest-arc
  float yawDelta;
  float rollDelta;
  float fovDelta;
};
// Diagnostic gap between first and last keyframe. Returns false if the
// active sequence has fewer than 2 poses.
bool Sequence_GetLoopGap(LoopGap *out);

// Authoring action. If the active sequence has only one pose, clones
// it at t+2s. If the last pose is already within tolerance of the
// first, hard-snaps the last pose's values to exactly equal pose[0]
// (so the duplicate is bit-exact). Otherwise appends a new closing
// pose at end+avg_gap with values copied from pose[0]. Sets loop=true.
// Returns the resulting closing-pose index, or -1 if no active sequence.
int Sequence_CloseLoop();

// ============================================================
//  Hotkeys (read by Sequence_Tick)
// ============================================================

extern int g_SeqHotkeyAdd;     // default VK_F6
extern int g_SeqHotkeyPlay;    // default VK_F7
extern int g_SeqHotkeyStop;    // default VK_F8
extern int g_SeqHotkeyNext;    // default VK_F9

// Toggle for the in-world keyframe markers + path preview. Turn off
// when capturing a video / clip so the spheres and lines don't show up
// in the recording.
extern bool g_SequenceShowMarkers;

// Appearance of the in-world keyframe markers + path preview (customizable in
// the menu's Appearance page, persisted to SimpleCamera.ini). These tint the
// "normal" keyframe marker + path line; the special states (editing/current/
// loop-seam/locked) keep their distinct semantic colors.
extern int g_SeqMarkerR, g_SeqMarkerG, g_SeqMarkerB; // normal keyframe color
extern float g_SeqMarkerSize;                         // normal keyframe size
extern int g_SeqTimeLabelMode; // path timestamp interval (index; see .cpp)
float SeqTimeLabelStep();       // interval in seconds (0 = labels off)
extern int g_SeqPathR, g_SeqPathG, g_SeqPathB;        // path line color

// Track which pose keyframe (if any) is currently open in the per-pose
// editor menu, so its in-world marker can be highlighted distinctly.
// -1 = nothing being edited.
void Sequence_SetEditingPose(int idx);
int Sequence_GetEditingPose();
