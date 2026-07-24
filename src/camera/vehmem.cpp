/*
        GTA V Free Camera / Photo Mode Plugin
        Vehicle Memory — see vehmem.h for the rationale and offset table.

        Two build flavours share this file:
          * Default (singleplayer, Enhanced + Legacy): resolves CWheel offsets by
            AOB-scanning the live game module, then reads/writes wheel fields
            directly.
          * BUILD_FIVEM (the FiveM .asi): every byte of the module scan and raw
            memory access below is compiled OUT — FiveM crashes if a loaded .asi
            even contains that machinery — and the wheels are driven purely
            through FiveM's CFX wheel natives.
*/

#include "vehmem.h"

#include <windows.h>
#include <cstdint>
#include <cstring>

#include "main.h"   // getScriptHandleBaseAddress (ScriptHookV)
#include "camera.h" // g_IsFiveM + invoke<> / Hash / Void (ScriptHookV natives)

namespace {

// FiveM CFX wheel natives (hash = joaat of the registered name). Only ever
// invoked when we've decided we're on the FiveM backend, so the "unknown native
// = fatal" rule for the vanilla ScriptHookV path is never hit. See vehmem.h.
const Hash FM_GET_NUM_WHEELS  = 0xEDF4B0FC; // GET_VEHICLE_NUMBER_OF_WHEELS
const Hash FM_SET_WHEEL_SPEED = 0x35ED100D; // SET_VEHICLE_WHEEL_ROTATION_SPEED
// NOTE: SET_VEHICLE_WHEEL_Y_ROTATION (0xC6C2171F) is CAMBER, not wheel spin
// (it's the VStancer camber native), so it is intentionally NOT used here.

// True once Init() decides we should use the natives above instead of memory.
// In the FiveM build this is the only backend; in the default build it tracks
// the g_IsFiveM runtime detection.
bool g_fivem = false;

bool g_initDone = false;
bool g_ok = false;

#ifndef BUILD_FIVEM
// ============================================================
//  Singleplayer-only: AOB module scan + raw CWheel memory access.
//  None of this is compiled into the FiveM build.
// ============================================================

// ---- Resolved field offsets (0 = unresolved) ----
int g_wheelsPtrOff = 0;  // CVehicle: pointer to CWheel*[] array
int g_wheelCntOff = 0;   // CVehicle: wheel count
int g_wheelAngOff = 0;   // CWheel: visible rotation angle (radians)
int g_wheelVelOff = 0;   // CWheel: rotation angular velocity
int g_wheelSteerOff = 0; // CWheel: steering angle (radians, signed)
int g_wheelSuspOff = 0;  // CWheel: suspension compression (ride height)

// ---- Game module range (the .text section we scan) ----
const uint8_t *g_scanBase = nullptr;
size_t g_scanSize = 0;

bool ResolveModuleText() {
  HMODULE mod = GetModuleHandleA(nullptr); // main game executable
  if (!mod) return false;
  auto base = reinterpret_cast<const uint8_t *>(mod);
  auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

  // Scan the whole module image. GTA5_Enhanced.exe has more than one executable
  // section, and its on-disk code is protected (decrypted only in memory), so we
  // match against the live image exactly like ScriptHookV-based tools do rather
  // than picking a single ".text" by name.
  g_scanBase = base;
  g_scanSize = nt->OptionalHeader.SizeOfImage;
  return true;
}

// Parse a space-separated AOB string ("3B B7 ? ? ? ? 7D 0D") into bytes + mask.
// A '?' token is a wildcard (mask = false). Returns token count.
int ParsePattern(const char *pat, uint8_t *bytes, bool *mask, int cap) {
  int n = 0;
  for (const char *p = pat; *p && n < cap;) {
    if (*p == ' ') { ++p; continue; }
    if (*p == '?') {
      bytes[n] = 0;
      mask[n] = false;
      ++n;
      ++p;
      if (*p == '?') ++p; // tolerate "??"
    } else {
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      int hi = hex(p[0]);
      int lo = (hi >= 0) ? hex(p[1]) : -1;
      if (hi < 0 || lo < 0) { ++p; continue; }
      bytes[n] = (uint8_t)((hi << 4) | lo);
      mask[n] = true;
      ++n;
      p += 2;
    }
  }
  return n;
}

// Find the first occurrence of `pat` in the scanned module range. Returns a
// pointer to the match, or nullptr if not found.
const uint8_t *FindPattern(const char *pat) {
  if (!g_scanBase || g_scanSize == 0) return nullptr;
  uint8_t bytes[64];
  bool mask[64];
  int len = ParsePattern(pat, bytes, mask, 64);
  if (len == 0 || (size_t)len > g_scanSize) return nullptr;

  const uint8_t *end = g_scanBase + (g_scanSize - len);
  for (const uint8_t *p = g_scanBase; p <= end; ++p) {
    int i = 0;
    for (; i < len; ++i) {
      if (mask[i] && p[i] != bytes[i]) break;
    }
    if (i == len) return p;
  }
  return nullptr;
}

// Read the disp32 embedded `at` bytes into the matched instruction.
int Disp32At(const uint8_t *match, int at) {
  int v;
  memcpy(&v, match + at, sizeof(v));
  return v;
}
#endif // !BUILD_FIVEM

} // namespace

namespace VehMem {

bool Init() {
  if (g_initDone) return g_ok;
  g_initDone = true;

#ifdef BUILD_FIVEM
  // FiveM build: natives are the only backend. No scan, no memory access.
  g_fivem = true;
  return (g_ok = true);
#else
  // FiveM (running the singleplayer .asi under FiveM): skip the module pattern
  // scan and route every call through the CFX wheel natives. The steering offset
  // is left unresolved because FiveM has no steer-angle SETTER native, so
  // SteerAvailable() reports false and replay uses the steer-bias fallback.
  if (g_IsFiveM) {
    g_fivem = true;
    return (g_ok = true);
  }

  // Bail on a wholly unidentified build so we never read a bogus offset; the
  // signatures below cover the known Enhanced and Legacy builds.
  if (getGameVersion() == VER_UNK) return (g_ok = false);
  if (!ResolveModuleText()) return (g_ok = false);

  // CVehicle: wheels pointer + wheel count share one match. Primary signature
  // with a longer fallback for builds where it doesn't hit. Both store the
  // wheel-count disp32 at +2; the pointer sits 8 bytes before it.
  const uint8_t *mw = FindPattern("3B B7 ? ? ? ? 7D 0D");
  if (!mw)
    mw = FindPattern("8B 90 ? ? 00 00 4C 8B ? ? ? 00 00 48 8B 40 20 48 8B 80 "
                     "B0 00 00 00 4C 8B ? F3 0F 11 44 24 30");
  if (mw) {
    g_wheelsPtrOff = Disp32At(mw, 2) - 8;
    g_wheelCntOff = Disp32At(mw, 2);
  }

  // CWheel: suspension compression; angle / angular velocity derive from it.
  // The signature AND the angle/angvel deltas differ between the Enhanced and
  // Legacy builds, so we try each in turn and let whichever matches the running
  // game pick the math — no getGameVersion() branching needed, and it adapts on
  // its own if only one signature survives a future patch.
  if (const uint8_t *m =
          FindPattern("C7 83 ? ? 00 00 00 00 00 00 48 89 D9 48 8D 54 24 30")) {
    // Enhanced: SuspComp = the C7 83 store's disp32 (+2); angle/angvel at +0xC/+0x10.
    int suspComp = Disp32At(m, 2);
    g_wheelSuspOff = suspComp;
    g_wheelAngOff = suspComp + 0xC;
    g_wheelVelOff = suspComp + 0x10;
  } else if (const uint8_t *m =
                 FindPattern("45 0F 57 ? F3 0F 11 ? ? ? 00 00 F3 0F 5C")) {
    // Legacy: SuspComp = the F3 0F 11 store's disp32 (+8); angle/angvel at +8/+0xC.
    int suspComp = Disp32At(m, 8);
    g_wheelSuspOff = suspComp;
    g_wheelAngOff = suspComp + 8;
    g_wheelVelOff = suspComp + 0xC;
  }

  // CWheel: steering angle (signed radians). Used for driver-immune visual
  // steering (SET_VEHICLE_STEER_BIAS only steers EMPTY vehicles and is flaky on
  // spawned ghosts). Try the Enhanced signature first, then the Legacy one.
  if (const uint8_t *m = FindPattern("0F 11 81 ? ? 00 00 C7 81 ? ? 00 00 00 00 "
                                     "00 00 48 8B 99 ? ? 00 00")) {
    // Enhanced: Traction = the C7 81 store's disp32 (+3); steer is +0x14 past it.
    g_wheelSteerOff = Disp32At(m, 3) + 0x14;
  } else {
    // Legacy: steer angle = the ucomiss operand's disp32 (+3) directly.
    const uint8_t *ml = FindPattern("0F 2F ? ? ? 00 00 0F 97 C0 EB ? D1 ?");
    if (!ml) ml = FindPattern("0F 2F ? ? ? 00 00 0F 97 C0 EB DA");
    if (ml) g_wheelSteerOff = Disp32At(ml, 3);
  }

  g_ok = (g_wheelsPtrOff != 0 && g_wheelCntOff != 0 && g_wheelAngOff != 0);
  return g_ok;
#endif // BUILD_FIVEM
}

bool Available() { return g_ok; }

#ifndef BUILD_FIVEM
// Returns the CVehicle base, or nullptr if memory access isn't ready.
static uint8_t *VehBase(int vehicle) {
  if (!g_ok || vehicle == 0) return nullptr;
  return reinterpret_cast<uint8_t *>(getScriptHandleBaseAddress(vehicle));
}

// Resolve the CWheel* for wheel `i`, or nullptr. Validates the array pointer.
static uint8_t *WheelPtr(uint8_t *base, int i) {
  uint64_t arr = *reinterpret_cast<uint64_t *>(base + g_wheelsPtrOff);
  if (arr == 0) return nullptr;
  uint64_t w = *reinterpret_cast<uint64_t *>(arr + (uint64_t)i * 8);
  return reinterpret_cast<uint8_t *>(w);
}
#endif // !BUILD_FIVEM

int WheelCount(int vehicle) {
  if (vehicle == 0) return 0;
  if (g_fivem) {
    int n = invoke<int>(FM_GET_NUM_WHEELS, vehicle);
    if (n < 0 || n > kMaxWheels) return 0;
    return n;
  }
#ifndef BUILD_FIVEM
  uint8_t *base = VehBase(vehicle);
  if (!base) return 0;
  int n = *reinterpret_cast<uint8_t *>(base + g_wheelCntOff); // stored as a byte
  if (n < 0 || n > kMaxWheels) return 0; // sanity guard against a bad offset
  return n;
#else
  return 0; // unreachable in the FiveM build (g_fivem is always true)
#endif
}

int ReadWheelAngles(int vehicle, float *out, int maxCount) {
  if (g_fivem) {
    // FiveM reproduces spin from the replayed forward velocity (no absolute
    // wheel-angle native exists), so per-wheel angle capture is unused — skip it.
    (void)vehicle; (void)out; (void)maxCount;
    return 0;
  }
#ifndef BUILD_FIVEM
  uint8_t *base = VehBase(vehicle);
  if (!base) return 0;
  int n = WheelCount(vehicle);
  if (n > maxCount) n = maxCount;
  int read = 0;
  for (int i = 0; i < n; ++i) {
    uint8_t *w = WheelPtr(base, i);
    out[i] = w ? *reinterpret_cast<float *>(w + g_wheelAngOff) : 0.0f;
    ++read;
  }
  return read;
#else
  return 0; // unreachable in the FiveM build
#endif
}

bool SteerAvailable() {
#ifdef BUILD_FIVEM
  return false; // FiveM exposes only a steering-angle GETTER, no setter
#else
  return g_ok && g_wheelSteerOff != 0;
#endif
}

int ReadWheelSteer(int vehicle, float *out, int maxCount) {
#ifndef BUILD_FIVEM
  uint8_t *base = VehBase(vehicle);
  if (!base || g_wheelSteerOff == 0) return 0;
  int n = WheelCount(vehicle);
  if (n > maxCount) n = maxCount;
  for (int i = 0; i < n; ++i) {
    uint8_t *w = WheelPtr(base, i);
    out[i] = w ? *reinterpret_cast<float *>(w + g_wheelSteerOff) : 0.0f;
  }
  return n;
#else
  (void)vehicle; (void)out; (void)maxCount;
  return 0; // no steer backend under FiveM (SteerAvailable() == false)
#endif
}

void WriteWheelSteer(int vehicle, const float *angles, int count) {
#ifndef BUILD_FIVEM
  uint8_t *base = VehBase(vehicle);
  if (!base || g_wheelSteerOff == 0) return;
  int n = WheelCount(vehicle);
  if (count < n) n = count;
  for (int i = 0; i < n; ++i) {
    uint8_t *w = WheelPtr(base, i);
    if (w) *reinterpret_cast<float *>(w + g_wheelSteerOff) = angles[i];
  }
#else
  (void)vehicle; (void)angles; (void)count; // no steer backend under FiveM
#endif
}

bool SuspAvailable() {
#ifdef BUILD_FIVEM
  return false; // no suspension-compression native under FiveM
#else
  return g_ok && !g_fivem && g_wheelSuspOff != 0;
#endif
}

int ReadWheelSusp(int vehicle, float *out, int maxCount) {
#ifndef BUILD_FIVEM
  uint8_t *base = VehBase(vehicle);
  if (!base || g_wheelSuspOff == 0 || g_fivem) return 0;
  int n = WheelCount(vehicle);
  if (n > maxCount) n = maxCount;
  for (int i = 0; i < n; ++i) {
    uint8_t *w = WheelPtr(base, i);
    out[i] = w ? *reinterpret_cast<float *>(w + g_wheelSuspOff) : 0.0f;
  }
  return n;
#else
  (void)vehicle; (void)out; (void)maxCount;
  return 0;
#endif
}

void WriteWheelSusp(int vehicle, const float *comp, int count) {
#ifndef BUILD_FIVEM
  uint8_t *base = VehBase(vehicle);
  if (!base || g_wheelSuspOff == 0 || g_fivem) return;
  int n = WheelCount(vehicle);
  if (count < n) n = count;
  for (int i = 0; i < n; ++i) {
    uint8_t *w = WheelPtr(base, i);
    if (w) *reinterpret_cast<float *>(w + g_wheelSuspOff) = comp[i];
  }
#else
  (void)vehicle; (void)comp; (void)count;
#endif
}

void WriteWheelAngles(int vehicle, const float *angles, int count) {
  if (g_fivem) {
    // FiveM has no native to set a wheel's absolute roll angle
    // (SET_VEHICLE_WHEEL_Y_ROTATION is camber). Spin is reproduced via
    // SetWheelRotationSpeed instead, so this is a no-op on the native backend.
    (void)vehicle; (void)angles; (void)count;
    return;
  }
#ifndef BUILD_FIVEM
  uint8_t *base = VehBase(vehicle);
  if (!base) return;
  int n = WheelCount(vehicle);
  if (count < n) n = count;
  for (int i = 0; i < n; ++i) {
    uint8_t *w = WheelPtr(base, i);
    if (!w) continue;
    *reinterpret_cast<float *>(w + g_wheelAngOff) = angles[i];
    if (g_wheelVelOff) *reinterpret_cast<float *>(w + g_wheelVelOff) = 0.0f;
  }
#endif
}

bool UsesNativeSpin() { return g_fivem; }

void SetWheelRotationSpeed(int vehicle, float radPerSec) {
  if (!g_fivem || vehicle == 0) return; // native backend only
  // Loop a fixed upper bound rather than trusting GET_VEHICLE_NUMBER_OF_WHEELS
  // (so spin still works even if that native is unreachable / returns 0). The
  // CFX setter validates the wheel index internally and ignores out-of-range.
  for (int i = 0; i < kMaxWheels; ++i)
    invoke<Void>(FM_SET_WHEEL_SPEED, vehicle, i, radPerSec);
}

} // namespace VehMem
