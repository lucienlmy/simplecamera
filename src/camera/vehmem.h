/*
        GTA V Free Camera / Photo Mode Plugin
        Vehicle Memory — direct CWheel field access for replay wheel spin

        There is no ScriptHookV native to set a wheel's visual rotation angle, so
        deterministic replay (vehicleclip.cpp) leaves the wheels frozen: the car
        is teleported onto its recorded pose each frame and the physics sim never
        sees it "travel". To roll the wheels we read/write the per-CWheel rotation
        angle directly in game memory.

        The field offsets are NOT hardcoded — they are resolved at runtime by AOB
        pattern-scanning the game module (offsets differ between game builds, and
        hardcoding them would break on every patch). Both the GTA V *Enhanced* and
        *Legacy* builds are supported: the wheel-array pointer/count signature is
        shared, while suspension-compression has a per-build signature and the
        angle/angvel deltas differ. Init() tries each and lets whichever matches
        the running game select the math, so no game-version branching is needed.

        FiveM build: FiveM dislikes raw module pattern scans (its image is managed
        and getScriptHandleBaseAddress writes are unreliable there), so when we
        detect FiveM (g_IsFiveM) we skip the scan entirely and drive the wheels
        through FiveM's CFX wheel natives instead — same public API, different
        backend. The natives used (hash = joaat of the name):
          GET_VEHICLE_NUMBER_OF_WHEELS    0xEDF4B0FC  (Vehicle) -> int
          GET_VEHICLE_WHEEL_Y_ROTATION    0x2EA4AFFE  (Vehicle, wheel) -> float
          SET_VEHICLE_WHEEL_Y_ROTATION    0xC6C2171F  (Vehicle, wheel, radians)
          SET_VEHICLE_WHEEL_ROTATION_SPEED 0x35ED100D (Vehicle, wheel, speed)
        FiveM exposes only GET_VEHICLE_WHEEL_STEERING_ANGLE (no setter), so under
        FiveM SteerAvailable() stays false and steering replay falls back to the
        SET_VEHICLE_STEER_BIAS path in vehicleclip.cpp.

        Resolved offsets (each = *(int32*)(match + k), from the matched disp32):
          Wheels Pointer  3B B7 ? ? ? ? 7D 0D  (fallback 8B 90 ...)   *(m+2) - 8   [both]
          Wheel Count     (same match)                                *(m+2)       [both]
          Susp. Compress  C7 83 ? ? 00 00 00 00 00 00 48 89 D9 ...    *(m+2)       [Enhanced]
                          45 0F 57 ? F3 0F 11 ? ? ? 00 00 F3 0F 5C    *(m+8)       [Legacy]
          Wheel Angle     SuspComp + 0xC (Enhanced) / + 8 (Legacy)  <- visible spin
          Wheel Ang.Vel.  SuspComp + 0x10 (Enhanced) / + 0xC (Legacy)
*/

#pragma once

namespace VehMem {

// Hard cap on wheels we will ever touch (real vehicles top out well under this).
static const int kMaxWheels = 10;

// Resolve the CWheel field offsets by AOB-scanning the game module. Idempotent —
// does the scan once and caches the result. Returns true if all required offsets
// were found. Safe to call every frame.
bool Init();

// True once Init() has successfully resolved the offsets.
bool Available();

// Number of wheels on the vehicle (0 if unavailable / invalid).
int WheelCount(int vehicle);

// Read each wheel's current rotation angle (radians) into out[<= maxCount].
// Returns the number of wheels actually read (0 on failure).
int ReadWheelAngles(int vehicle, float *out, int maxCount);

// Write a rotation angle (radians) to each wheel and zero its angular velocity
// so the sim won't keep advancing the value between our per-frame writes.
// `count` should match the vehicle's wheel count (clamped internally).
// (Memory backend only — FiveM has no "set absolute wheel angle" native, so this
// is a no-op there; use SetWheelRotationSpeed instead.)
void WriteWheelAngles(int vehicle, const float *angles, int count);

// True when wheel spin must be reproduced via angular VELOCITY rather than an
// absolute angle — i.e. the FiveM native backend. FiveM's only spin lever is
// SET_VEHICLE_WHEEL_ROTATION_SPEED (SET_VEHICLE_WHEEL_Y_ROTATION is camber, not
// roll), so replay forces each wheel's rotation speed every frame instead.
bool UsesNativeSpin();

// Force every wheel's spin angular velocity (radians/sec). No-op unless the
// FiveM native backend is active. The game integrates this into the visible
// wheel roll each frame, so a teleported/parked ghost still appears to drive.
void SetWheelRotationSpeed(int vehicle, float radPerSec);

// True once the suspension-compression offset has resolved (memory backend
// only). Replay writes the recorded compression back each frame so a
// collision-off ghost keeps its recorded ride height instead of drooping the
// wheels into the road.
bool SuspAvailable();

// Read / write each wheel's suspension compression. Mirrors the wheel-angle
// calls. Returns the number read (Read) / no-op if unavailable.
int ReadWheelSusp(int vehicle, float *out, int maxCount);
void WriteWheelSusp(int vehicle, const float *comp, int count);

// True once the per-wheel STEERING-angle offset has resolved. Steering can be
// driven by direct memory write (unlike SET_VEHICLE_STEER_BIAS, this works even
// with a driver ped seated).
bool SteerAvailable();

// Read / write each wheel's steering angle (radians, signed). Mirrors the
// rotation-angle calls. Returns the number read (Read) / no-op if unavailable.
int ReadWheelSteer(int vehicle, float *out, int maxCount);
void WriteWheelSteer(int vehicle, const float *angles, int count);

} // namespace VehMem
