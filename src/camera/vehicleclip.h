/*
        GTA V Free Camera / Photo Mode Plugin
        Vehicle Clip — deterministic vehicle path record / replay

        GTA's world isn't deterministic: drive a car and it won't reproduce the
        same path next time, so camera keyframes placed against a live-driven
        vehicle would drift on every replay and render. This module records the
        target vehicle's transform per frame into a clip, then forces the vehicle
        back onto that exact path as a function of the camera-sequence timeline.

        Once a clip exists, the vehicle's pose at sequence time `t` is a pure
        function of `t` — identical on every scrub, preview and offline render —
        which is what makes a clean, syncable camera animation possible. Entity-
        locked keyframes (camera.cpp / sequence.cpp) then line up frame-for-frame
        because the subject's transform at each `t` is now known and repeatable.

        v1 records position + world rotation (quaternion) + linear velocity.
        Position/rotation are forced each frame (deterministic); the velocity is
        re-applied during playback so wheels spin / suspension settles naturally.
        Steering / per-wheel state is a later refinement.
*/

#pragma once

#include <cstdio> // FILE* for the merged-sequence JSON writer

// ---- Recording ----

// Begin recording the vehicle the player is currently driving as a NEW clip
// (previously-recorded clips are kept and replay as ghosts around the player so
// multi-vehicle scenes can be choreographed on a shared timeline). The free
// camera is suspended for the duration (player gets the wheel + gameplay view)
// and restored on Stop. Returns false if the player isn't in a drivable vehicle
// (or is sitting in one of the ghost vehicles).
bool VehicleClip_StartRecording();

// Stop recording (keeps whatever was captured) and bring the free camera back.
void VehicleClip_StopRecording();

bool VehicleClip_IsRecording();

// Per-frame sampler — call every frame from the main loop. Samples the vehicle
// while recording; a no-op otherwise. Uses game frame time.
void VehicleClip_RecordTick();

// Centered "● REC … drive, press Menu key to stop" banner. Draw each frame while
// recording so the player knows they're driving a take. No-op when idle.
void VehicleClip_DrawRecordingBanner();

// ---- Replay (driven off the sequence timeline) ----

// Force the recorded vehicle onto its clip pose at clip time `t` (seconds from
// the start of the clip). `animateWheels` re-applies the recorded velocity so
// wheels spin during playback; pass false while merely holding a scrub position.
// No-op when disabled, recording, without data, or if the vehicle is gone.
void VehicleClip_ApplyAtTime(float t, bool animateWheels);

// Release the vehicle from clip control (restores collision). Call when leaving
// Sequence mode, disabling replay, or clearing the clip.
void VehicleClip_Release();

// ---- In-world path preview ----

// Draw the recorded vehicle path in the world: a polyline through the captured
// samples, per-second timestamp labels, start (green) / end (red) markers,
// travel-direction arrows, and a "playhead" sphere at clip time `currentTime`
// (so the user can line camera keyframes up against the car's motion). No-op
// without recorded data. `playing` dims the overlay during playback. Call each
// frame while authoring; respects the same Show-Markers gate as the keyframes.
void VehicleClip_DrawPath(float currentTime, bool playing);

// ---- Persistence + ghost respawn (per-sequence JSON sidecar) ----

// Sample rate (Hz) used while recording. Playback interpolates, so a lower rate
// shrinks the saved clip with little visible cost. 0 = capture every frame.
// Customizable in the menu; persisted with the rest of the settings.
extern int g_VehicleClipSampleHz;

// Replay front-wheel steering strength multiplier (1.0 = neutral). Higher makes
// the wheels turn more visibly through corners. Customizable in the menu.
extern float g_VehicleClipSteerGain;

// When true, each ghost vehicle gets a driver ped matching the one recorded at
// capture time (model + outfit). Toggle live in the menu; persisted.
extern bool g_VehicleClipShowDriver;

// When true, replayed ghost vehicles keep collision ON, so they physically
// shove/knock objects (props, other cars) along the recorded path — useful for
// showing impacts. Default off (collision off = perfectly smooth deterministic
// replay). Toggle live; persisted.
extern bool g_VehicleClipGhostCollision;

// When true, replayed ghost vehicles are invincible and auto-repair every frame
// (deformation fixed, full health, no visible damage) — keeps the ghost pristine
// even with collision on. Default on. Toggle live; persisted.
extern bool g_VehicleClipGhostGodmode;

// Save / load the active clip (per-frame samples + the recorded vehicle's
// model, colors and mods) to a JSON file. Save returns false without data.
// Load replaces the in-memory clip and remembers the vehicle's appearance; the
// recorded vehicle is respawned lazily as a plugin-owned "ghost" on the next
// EnsureGhost/replay tick (no live vehicle is needed at load time). Returns
// success.
bool VehicleClip_SaveToFile(const char *path);
bool VehicleClip_LoadFromFile(const char *path);

// Merged-sequence JSON helpers: write the active clips as a `"sampleHz"` +
// `"clips":[...]` section into an already-open sequence file (no enclosing
// braces — the sequence writer wraps it), and parse the `"clips"` array back out
// of an in-memory sequence-JSON buffer (replaces the runtime clips).
void VehicleClip_WriteClipsJson(FILE *f);
bool VehicleClip_ParseClipsFromBuffer(const char *data, int len);

// Spawn the ghost vehicle for a loaded clip if one is pending and not yet
// present (polls model streaming, so safe to call every frame). No-op when the
// recorded vehicle is a real, player-provided one or no clip is loaded.
void VehicleClip_EnsureGhost();

// Delete the plugin-owned ghost vehicle, if any. Call when leaving Sequence
// mode, switching sequences, or clearing. No-op for a real recorded vehicle.
void VehicleClip_DespawnGhost();

// ---- Per-clip settings ----

// Editable per-vehicle settings, saved with the sequence. `lights`: 0=Auto,
// 1=On, 2=Off, 3=On+FullBeam. `speed` is a playback multiplier (>0). `offset`
// is seconds before the clip starts on the shared timeline.
struct VehicleClipSettings {
  bool enabled;
  float speed;
  float offset;
  int lights;
  bool engineOn;
  bool siren;
  bool showDriver;
  bool collision;
  bool godmode;
  char label[32];
};

// Read / write a clip's settings by index. Get returns false on a bad index.
bool VehicleClip_GetSettings(int index, VehicleClipSettings *out);
void VehicleClip_SetSettings(int index, const VehicleClipSettings *in);

// Default label for a clip (its user label, or "Vehicle N" / model name).
void VehicleClip_GetLabel(int index, char *out, int outLen);

// ---- State / data ----

bool  VehicleClip_HasData();      // at least one clip with >= 2 samples
float VehicleClip_Duration();     // longest clip length in seconds (or rec time)
int   VehicleClip_SampleCount();  // total samples across all clips
int   VehicleClip_Count();        // number of recorded vehicle clips
void  VehicleClip_DeleteClip(int index); // delete one clip + despawn its ghost
void  VehicleClip_Clear();        // drop all clips and release the vehicles

bool  VehicleClip_Enabled();      // replay on/off (independent of having data)
void  VehicleClip_SetEnabled(bool on);

int   VehicleClip_VehicleHandle(); // recorded vehicle handle (0 if none / gone)
int   VehicleClip_GhostAt(int index); // clip's spawned ghost handle (0 if none)
// True if the handle is an entity this module owns (the recorded vehicle, a
// replay ghost, or a ghost's driver) — lets World "Clear Vehicles/Peds" spare it.
bool  VehicleClip_OwnsEntity(int handle);
