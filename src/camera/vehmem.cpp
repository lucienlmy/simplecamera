/*
        GTA V Free Camera / Photo Mode Plugin
        Vehicle Memory — see vehmem.h for the rationale and offset table.

        ONE code path, two backends, chosen at RUNTIME:
          * memory — CWheel offsets resolved by AOB-scanning the live game
            module, then wheel fields read/written directly. Needs both the
            offsets and a way to turn a script handle into a CVehicle*.
          * natives — FiveM's CFX wheel natives. Only legal under FiveM: those
            hashes do not exist in a singleplayer host and calling an
            unregistered native is fatal.

        ONE build serves both hosts, singleplayer and FiveM. Two things make
        that work and both are easy to undo by accident:

          1. no static import of getScriptHandleBaseAddress — FiveM does not
             export it. See ResolveShvHandleFunc below.
          2. FindPattern must not walk base..SizeOfImage unguarded. See
             ResolveModuleText.
*/

#include "vehmem.h"

#include <windows.h>
#include <cstdint>
#include <cstring>

#include "camera.h" // g_IsFiveM + invoke<> / Hash / Void (ScriptHookV natives)
#include "log.h"

namespace {

// FiveM CFX wheel natives (hash = joaat of the registered name). Only ever
// invoked when we've decided we're on the FiveM backend, so the "unknown native
// = fatal" rule for the vanilla ScriptHookV path is never hit. See vehmem.h.
const Hash FM_GET_NUM_WHEELS  = 0xEDF4B0FC; // GET_VEHICLE_NUMBER_OF_WHEELS
const Hash FM_SET_WHEEL_SPEED = 0x35ED100D; // SET_VEHICLE_WHEEL_ROTATION_SPEED
// NOTE: SET_VEHICLE_WHEEL_Y_ROTATION (0xC6C2171F) is CAMBER, not wheel spin
// (it's the VStancer camber native), so it is intentionally NOT used here.

// True once Init() decides we should use the natives above instead of memory.
// Purely a runtime decision now, off the back of g_IsFiveM and what the scan
// managed to resolve.
bool g_fivem = false;

bool g_initDone = false;
bool g_ok = false;

// ============================================================
//  AOB module scan + raw CWheel memory access.
//
//  Runs under FiveM too — Menyoo scans the whole image there and writes to game
//  memory. The scanner is not what FiveM objects to; see ResolveShvHandleFunc.
// ============================================================

// ---- Resolved field offsets (0 = unresolved) ----
int g_wheelsPtrOff = 0;  // CVehicle: pointer to CWheel*[] array
int g_wheelCntOff = 0;   // CVehicle: wheel count
int g_wheelAngOff = 0;   // CWheel: visible rotation angle (radians)
int g_wheelVelOff = 0;   // CWheel: rotation angular velocity
int g_wheelSteerOff = 0; // CWheel: steering angle (radians, signed)
int g_wheelSuspOff = 0;  // CWheel: suspension compression (ride height)

// ---- Executable regions of the game module ----
//
// A LIST, not one range, and that is the bug fix.
//
// This used to scan base .. base+SizeOfImage as one flat span with a raw
// dereference per byte. SizeOfImage is the VIRTUAL extent: it spans section
// gaps and anything the loader left PAGE_NOACCESS, so walking it touches
// memory that cannot be read, and the first such byte is an access violation.
//
// Singleplayer hid it. FindPattern returns the moment it matches, and the
// CWheel signatures match part-way into the first code section, so the walk
// never reached a bad page. A signature that MISSES walks the entire image —
// and under FiveM the game is a different pinned build, where these signatures
// are exactly the ones most likely to miss.
struct ScanRegion {
  const uint8_t *base;
  size_t         size;
};
ScanRegion g_regions[16]{};
int        g_regionCount = 0;

// Only EXECUTE sections, and only ones the OS confirms are readable right now.
// Both halves matter: the section flags say what the image asked for, and
// VirtualQuery says what the process actually has - and under a managed image
// those disagree.
bool ResolveModuleText() {
  g_regionCount = 0;

  HMODULE mod = GetModuleHandleA(nullptr); // main game executable
  if (!mod) return false;
  auto base = reinterpret_cast<const uint8_t *>(mod);
  auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

  // Every executable section, not a single ".text" by name: GTA5_Enhanced.exe
  // has more than one, and its code is decrypted only in memory, so the live
  // image is the only thing worth matching against.
  auto sec = IMAGE_FIRST_SECTION(nt);
  const int count = nt->FileHeader.NumberOfSections;

  for (int i = 0; i < count && g_regionCount < 16; ++i, ++sec) {
    if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
    if (!(sec->Characteristics & IMAGE_SCN_MEM_READ))    continue;
    if (sec->Misc.VirtualSize == 0) continue;

    const uint8_t *secBase = base + sec->VirtualAddress;
    size_t         left    = sec->Misc.VirtualSize;

    // Walk the section in VirtualQuery runs and keep only the committed,
    // readable, non-guard ones. A section can be split into several such runs
    // once something has re-protected part of it, which is normal in a process
    // hosting a JIT or a patcher - and is the FiveM case.
    while (left > 0 && g_regionCount < 16) {
      MEMORY_BASIC_INFORMATION mbi{};
      if (VirtualQuery(secBase, &mbi, sizeof(mbi)) != sizeof(mbi)) break;

      size_t run = (size_t)((const uint8_t *)mbi.BaseAddress + mbi.RegionSize - secBase);
      if (run > left) run = left;
      if (run == 0) break;

      const DWORD prot = mbi.Protect & 0xFF;
      const bool readable =
          mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
          (prot == PAGE_READONLY || prot == PAGE_READWRITE ||
           prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READ ||
           prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY);

      if (readable) {
        g_regions[g_regionCount].base = secBase;
        g_regions[g_regionCount].size = run;
        ++g_regionCount;
      }

      secBase += run;
      left    -= run;
    }
  }
  return g_regionCount > 0;
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

// Find the first occurrence of `pat` across the readable executable regions.
// Returns a pointer to the match, or nullptr if not found.
//
// A match may not straddle two regions. That is correct rather than a
// limitation: the regions are separated precisely because something in between
// is not readable, so a "match" spanning the gap would be reading memory that
// is not there.
const uint8_t *FindPattern(const char *pat) {
  if (g_regionCount == 0) return nullptr;
  uint8_t bytes[64];
  bool mask[64];
  int len = ParsePattern(pat, bytes, mask, 64);
  if (len == 0) return nullptr;

  for (int r = 0; r < g_regionCount; ++r) {
    const uint8_t *rb = g_regions[r].base;
    const size_t   rs = g_regions[r].size;
    if ((size_t)len > rs) continue;

    const uint8_t *end = rb + (rs - len);
    for (const uint8_t *p = rb; p <= end; ++p) {
      int i = 0;
      for (; i < len; ++i) {
        if (mask[i] && p[i] != bytes[i]) break;
      }
      if (i == len) return p;
    }
  }
  return nullptr;
}

// Read the disp32 embedded `at` bytes into the matched instruction.
int Disp32At(const uint8_t *match, int at) {
  int v;
  memcpy(&v, match + at, sizeof(v));
  return v;
}

// ------------------------------------------------------------
//  Handle -> entity pointer, resolved from the image ourselves.
//
//  This is the piece that lets the memory backend exist at all under FiveM.
//  ScriptHookV's getScriptHandleBaseAddress does the same job, but FiveM's
//  shim does not export it (see ResolveShvHandleFunc below). Menyoo has never
//  imported it - it resolves the game's own function by pattern - and that is
//  what is copied here.
//
//  The function is a pool lookup: handle >> 8 is the slot index, the low byte
//  is a generation counter checked against the pool's flag array, and the
//  entity pointer is read out of the slot. It answers 0 for a stale handle,
//  so callers get the same "no entity" contract as before.
// ------------------------------------------------------------
using EntityAddrFn = uint64_t(__fastcall *)(int handle);
EntityAddrFn g_entityAddrFunc = nullptr;

void ResolveEntityAddrFunc() {
  g_entityAddrFunc = nullptr;

  // Enhanced. Verified against GTA5_Enhanced.exe: the pattern hits exactly
  // once, and the callee at the resolved rel32 is the pool lookup described
  // above (index = handle >> 8, generation byte compared, slot dereferenced).
  //     41 8B 4C 1C ?? | E8 <rel32>
  //     ^ mov ecx,[r12+rbx+imm8]   ^ call GetBaseFromGuid
  if (const uint8_t *m = FindPattern("41 8B 4C 1C ? E8")) {
    const uint8_t *target = m + 10 + Disp32At(m, 6);
    g_entityAddrFunc = reinterpret_cast<EntityAddrFn>(const_cast<uint8_t *>(target));
    return;
  }

  // Legacy. Anchored on a CALL SITE, not on the resolver's own body.
  //
  // This is the whole lesson, and it is Menyoo's: a function BODY is reshaped by
  // the optimiser on every game build, while the code AROUND a call to it is far
  // more stable. The body pattern this used to carry was cut from a decompile of
  // build 3889 and matched nothing at all on 3751 - not the full pattern, not a
  // relaxed head, and no structural hunt for the shift/size-compare/generation-
  // byte shape found a candidate either. The call site below matches BOTH builds,
  // exactly once each, unchanged.
  //
  // An earlier comment here claimed Menyoo's pattern "resolves to an unrelated
  // function that takes no handle". That was wrong, and worth recording as such:
  // it resolves to a thin TYPE-CHECKED WRAPPER around the very function that was
  // later derived by hand. Byte-identical on 3751 and 3889 apart from its two
  // displacements:
  //     40 53 48 83 EC 20     push rbx; sub rsp,20
  //     E8 <rel32>            call fwScriptGuid::GetBaseFromGuid    <- +0x06
  //     48 8B D8 48 85 C0     mov rbx,rax; test rax,rax
  //     74 1A / 4C 8B 00      jz; mov r8,[rax]
  //     48 8D 15 <rel32>      lea rdx,<type descriptor>
  //     48 8B C8 41 FF 50 28  mov rcx,rax; call [r8+0x28]           ; type test
  //
  // We follow the INNER call rather than using the wrapper Menyoo uses: the
  // wrapper answers 0 for anything failing its type test, and on 3889 the inner
  // target is precisely the function verified by decompilation (index = guid>>8,
  // generation byte compared, slot dereferenced). So this is corroborated from
  // two independent directions rather than trusted because it is unique.
  //
  // The wrapper prologue is opcode-checked before the hop. A unique match is not
  // a correct match, and this pointer gets CALLED - a build that reshapes the
  // wrapper must fail clean rather than hand us something else entirely.
  if (const uint8_t *m =
          FindPattern("E8 ? ? ? ? 48 8B D8 48 85 C0 74 2E 48 83 3D")) {
    const uint8_t *wrapper = m + 5 + Disp32At(m, 1);
    static const uint8_t kWrapperPrologue[] = {
        0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0xE8 }; // push rbx; sub rsp,20; call
    if (memcmp(wrapper, kWrapperPrologue, sizeof(kWrapperPrologue)) == 0) {
      // inner call: E8 at wrapper+6, rel32 at +7, next instruction at +11.
      g_entityAddrFunc = reinterpret_cast<EntityAddrFn>(
          const_cast<uint8_t *>(wrapper + 11 + Disp32At(wrapper, 7)));
      return;
    }
  }

  // Fallback: the 3889 body pattern. Kept because it was verified against a
  // decompile on that build, so it is a second opinion where it does match; it
  // is simply too build-specific to lead with.
  if (const uint8_t *m = FindPattern("83 F9 FF 74 ? 8B D1 C1 FA 08 85 D2 78 ? "
                                     "4C 8B 05 ? ? ? ? 41 3B 50 10")) {
    g_entityAddrFunc = reinterpret_cast<EntityAddrFn>(const_cast<uint8_t *>(m));
  }
}

// ------------------------------------------------------------
//  ScriptHookV's own resolver, looked up at RUNTIME. Second route, used where
//  the pattern above has not been derived (Legacy).
//
//  DO NOT CALL getScriptHandleBaseAddress DIRECTLY - the static import is what
//  this exists to avoid. FiveM's scripthookv shim does not export it, and an
//  import naming a symbol the host lacks makes the .asi fail to load, which
//  FiveM treats as fatal. GetProcAddress just yields nullptr there.
// ------------------------------------------------------------
using ShvHandleFn = uint8_t *(*)(int handle);
ShvHandleFn g_shvHandleFunc = nullptr;

void ResolveShvHandleFunc() {
  // Case-insensitive, so this matches however the host spells the module.
  const HMODULE shv = GetModuleHandleA("ScriptHookV.dll");
  g_shvHandleFunc =
      shv ? reinterpret_cast<ShvHandleFn>(
                GetProcAddress(shv, "?getScriptHandleBaseAddress@@YAPEAEH@Z"))
          : nullptr;
}

// One entry point for both routes, so no caller has to know which is live.
uint64_t EntityAddress(int handle) {
  if (handle == 0) return 0;
  if (g_entityAddrFunc) return g_entityAddrFunc(handle);
  // The SDK returns BYTE*, so this needs the cast rather than an implicit
  // conversion.
  if (g_shvHandleFunc) return reinterpret_cast<uint64_t>(g_shvHandleFunc(handle));
  return 0;
}

} // namespace

namespace VehMem {

// The scan proper. Split out so Init() can put a guard around it: this reads
// raw game memory chosen by pattern, and a fault here must degrade to "no
// memory backend" rather than take the process down with it.
static void ScanOffsets() {
  if (!ResolveModuleText()) return;

  ResolveEntityAddrFunc();

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
}

bool Init() {
  if (g_initDone) return g_ok;
  g_initDone = true;

  Log("vehmem: init (FiveM=%s)", g_IsFiveM ? "yes" : "no");

  // ScriptHookV's export, if this host has it. Outside the __try: GetProcAddress
  // cannot fault, and the result is needed even when the scan below does.
  ResolveShvHandleFunc();

  // The scan runs under FiveM too. It used to be skipped entirely there; see
  // the header comment on why that was the wrong fix.
  //
  // __try, not because a fault is expected - ResolveModuleText only hands
  // FindPattern committed readable pages - but because the whole point of this
  // change is that a scan under an unfamiliar image must never be able to kill
  // the game. If it does fault we lose the memory backend and keep the mod.
  __try {
    ScanOffsets();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Log("vehmem: SCAN FAULTED (0x%08lX) - falling back to natives",
        GetExceptionCode());
    g_wheelsPtrOff = g_wheelCntOff = g_wheelAngOff = 0;
    g_wheelVelOff = g_wheelSteerOff = g_wheelSuspOff = 0;
    g_entityAddrFunc = nullptr;
  }

  Log("vehmem: regions=%d entityFn=%p shvFn=%p wheelsPtr=0x%X count=0x%X "
      "ang=0x%X vel=0x%X steer=0x%X susp=0x%X",
      g_regionCount, (void *)g_entityAddrFunc, (void *)g_shvHandleFunc,
      g_wheelsPtrOff, g_wheelCntOff, g_wheelAngOff, g_wheelVelOff,
      g_wheelSteerOff, g_wheelSuspOff);

  // Which backend we can actually drive.
  //
  // The memory path needs BOTH the CWheel offsets and a way to turn a script
  // handle into a CVehicle*. Under FiveM the second half is the constraint:
  // getScriptHandleBaseAddress is not exported there, so until the Legacy
  // entity-address pattern is derived FiveM still falls back to natives - but
  // that is a resolved FACT, reported in the log line above as two pointers,
  // rather than an assumption compiled into the binary.
  const bool haveOffsets = (g_wheelsPtrOff != 0 && g_wheelCntOff != 0 &&
                            g_wheelAngOff != 0);
  const bool haveHandleRoute =
      (g_entityAddrFunc != nullptr) || (g_shvHandleFunc != nullptr);
  const bool underFiveM = g_IsFiveM;

  if (haveOffsets && haveHandleRoute) {
    g_fivem = false;              // memory
    g_ok    = true;
  } else if (underFiveM) {
    // CFX wheel natives. ONLY legal here: those hashes do not exist in a
    // singleplayer host, and calling an unregistered native is fatal - which
    // is why a failed scan outside FiveM must disable the feature rather than
    // fall through to this.
    g_fivem = true;
    g_ok    = true;
  } else {
    g_fivem = false;
    g_ok    = false;              // no backend; wheel replay is simply off
  }

  Log("vehmem: backend=%s (ok=%d)",
      g_ok ? (g_fivem ? "natives" : "memory") : "none", g_ok ? 1 : 0);
  return g_ok;
}

bool Available() { return g_ok; }

// Returns the CVehicle base, or nullptr if memory access isn't ready.
//
// Gated on !g_fivem as well as g_ok: on the natives backend the offsets are
// unresolved, and reading base+0 would be a wild pointer rather than a miss.
static uint8_t *VehBase(int vehicle) {
  if (!g_ok || g_fivem || vehicle == 0) return nullptr;
  return reinterpret_cast<uint8_t *>(EntityAddress(vehicle));
}

// Resolve the CWheel* for wheel `i`, or nullptr. Validates the array pointer.
static uint8_t *WheelPtr(uint8_t *base, int i) {
  uint64_t arr = *reinterpret_cast<uint64_t *>(base + g_wheelsPtrOff);
  if (arr == 0) return nullptr;
  uint64_t w = *reinterpret_cast<uint64_t *>(arr + (uint64_t)i * 8);
  return reinterpret_cast<uint8_t *>(w);
}

int WheelCount(int vehicle) {
  if (vehicle == 0) return 0;
  if (g_fivem) {
    int n = invoke<int>(FM_GET_NUM_WHEELS, vehicle);
    if (n < 0 || n > kMaxWheels) return 0;
    return n;
  }
  uint8_t *base = VehBase(vehicle);
  if (!base) return 0;
  int n = *reinterpret_cast<uint8_t *>(base + g_wheelCntOff); // stored as a byte
  if (n < 0 || n > kMaxWheels) return 0; // sanity guard against a bad offset
  return n;
}

int ReadWheelAngles(int vehicle, float *out, int maxCount) {
  if (g_fivem) {
    // FiveM reproduces spin from the replayed forward velocity (no absolute
    // wheel-angle native exists), so per-wheel angle capture is unused — skip it.
    (void)vehicle; (void)out; (void)maxCount;
    return 0;
  }
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
}

// All four availability tests below are now runtime, not compile-time. The
// FiveM build used to hardcode `false` because the memory path was not
// compiled into it; it is now, so what these report is whatever the scan
// actually resolved on the host we are running on.
bool SteerAvailable() { return g_ok && !g_fivem && g_wheelSteerOff != 0; }

int ReadWheelSteer(int vehicle, float *out, int maxCount) {
  uint8_t *base = VehBase(vehicle);
  if (!base || g_wheelSteerOff == 0) return 0;
  int n = WheelCount(vehicle);
  if (n > maxCount) n = maxCount;
  for (int i = 0; i < n; ++i) {
    uint8_t *w = WheelPtr(base, i);
    out[i] = w ? *reinterpret_cast<float *>(w + g_wheelSteerOff) : 0.0f;
  }
  return n;
}

void WriteWheelSteer(int vehicle, const float *angles, int count) {
  uint8_t *base = VehBase(vehicle);
  if (!base || g_wheelSteerOff == 0) return;
  int n = WheelCount(vehicle);
  if (count < n) n = count;
  for (int i = 0; i < n; ++i) {
    uint8_t *w = WheelPtr(base, i);
    if (w) *reinterpret_cast<float *>(w + g_wheelSteerOff) = angles[i];
  }
}

bool SuspAvailable() { return g_ok && !g_fivem && g_wheelSuspOff != 0; }

int ReadWheelSusp(int vehicle, float *out, int maxCount) {
  uint8_t *base = VehBase(vehicle);
  if (!base || g_wheelSuspOff == 0) return 0;
  int n = WheelCount(vehicle);
  if (n > maxCount) n = maxCount;
  for (int i = 0; i < n; ++i) {
    uint8_t *w = WheelPtr(base, i);
    out[i] = w ? *reinterpret_cast<float *>(w + g_wheelSuspOff) : 0.0f;
  }
  return n;
}

void WriteWheelSusp(int vehicle, const float *comp, int count) {
  uint8_t *base = VehBase(vehicle);
  if (!base || g_wheelSuspOff == 0) return;
  int n = WheelCount(vehicle);
  if (count < n) n = count;
  for (int i = 0; i < n; ++i) {
    uint8_t *w = WheelPtr(base, i);
    if (w) *reinterpret_cast<float *>(w + g_wheelSuspOff) = comp[i];
  }
}

void WriteWheelAngles(int vehicle, const float *angles, int count) {
  if (g_fivem) {
    // FiveM has no native to set a wheel's absolute roll angle
    // (SET_VEHICLE_WHEEL_Y_ROTATION is camber). Spin is reproduced via
    // SetWheelRotationSpeed instead, so this is a no-op on the native backend.
    (void)vehicle; (void)angles; (void)count;
    return;
  }
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
