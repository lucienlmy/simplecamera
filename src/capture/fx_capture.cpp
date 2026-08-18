/*
    Simple Camera — Frame Capture Bridge (Phase 1 PoC)
    See fx_capture.h for the protocol.
*/

#include "fx_capture.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

#pragma warning(disable : 4244 4305)

static const char *kMappingName = "Local\\SimpleCameraFxCapture";
static const uint32_t kMagic = 0x53434658; // 'SCFX'

static HANDLE s_mapHandle = nullptr;
static FxCaptureBlock *s_block = nullptr;

// Resolve the folder the ASI lives in (same trick LoadSettings uses).
static void GetModuleDir(char *out, size_t cap) {
  HMODULE hMod = nullptr;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     (LPCSTR)GetModuleDir, &hMod);
  GetModuleFileNameA(hMod, out, (DWORD)cap);
  char *last = strrchr(out, '\\');
  if (last)
    *last = '\0'; // strip filename, leaving the directory
}

void FxCapture_Init() {
  if (s_block)
    return;

  SetLastError(0);
  s_mapHandle = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                   0, sizeof(FxCaptureBlock), kMappingName);
  if (!s_mapHandle)
    return;
  bool alreadyExisted = (GetLastError() == ERROR_ALREADY_EXISTS);

  s_block = (FxCaptureBlock *)MapViewOfFile(s_mapHandle, FILE_MAP_ALL_ACCESS, 0,
                                            0, sizeof(FxCaptureBlock));
  if (!s_block) {
    // Don't leak the mapping handle on the failure path — a later re-init would
    // overwrite s_mapHandle and orphan this one.
    CloseHandle(s_mapHandle);
    s_mapHandle = nullptr;
    return;
  }

  if (!alreadyExisted) {
    // We created it — zero everything and stamp the header.
    memset(s_block, 0, sizeof(FxCaptureBlock));
  }
  // Stamp header regardless so the addon knows the ASI side is live.
  s_block->magic = kMagic;
  s_block->version = 6;
  if (s_block->quality == 0)
    s_block->quality = 90; // sensible JPEG default
}

void FxCapture_SetQuality(int quality) {
  if (!s_block)
    return;
  if (quality < 1) quality = 1;
  if (quality > 100) quality = 100;
  s_block->quality = (uint32_t)quality;
}

void FxCapture_SetHighlightBoost(float boost) {
  if (!s_block)
    return;
  if (boost < 0.0f) boost = 0.0f;
  if (boost > 0.99f) boost = 0.99f;
  s_block->highlightBoost = boost;
}

bool FxCapture_RequestFrame(const char *fullPath) {
  return FxCapture_RequestSample(fullPath, 1, 0);
}

bool FxCapture_RequestSample(const char *fullPath, int sampleCount,
                             int sampleIndex) {
  if (!s_block || !fullPath)
    return false;
  strncpy_s(s_block->outPath, sizeof(s_block->outPath), fullPath, _TRUNCATE);
  s_block->sampleCount = (sampleCount < 1) ? 1 : (uint32_t)sampleCount;
  s_block->sampleIndex = (uint32_t)sampleIndex;
  s_block->status = 0;
  // Bump last so all fields are written before the addon sees the request.
  s_block->requestId += 1;
  return true;
}

bool FxCapture_CaptureTest(char *outPathBuf, int outPathCap) {
  if (!s_block)
    return false;

  char dir[MAX_PATH];
  GetModuleDir(dir, sizeof(dir));

  char folder[MAX_PATH];
  sprintf_s(folder, "%s\\SimpleCamera_Captures", dir);
  CreateDirectoryA(folder, nullptr); // ok if it already exists

  static int s_counter = 1;
  char path[MAX_PATH];
  sprintf_s(path, "%s\\capture_%04d.png", folder, s_counter++);

  if (outPathBuf && outPathCap > 0)
    strncpy_s(outPathBuf, outPathCap, path, _TRUNCATE);

  return FxCapture_RequestFrame(path);
}

bool FxCapture_Available() { return s_block != nullptr; }

bool FxCapture_AddonPresent() {
  return s_block != nullptr && s_block->addonHeartbeat != 0;
}

bool FxCapture_IsLastDone() {
  return !s_block || s_block->ackId == s_block->requestId;
}

bool FxCapture_NewSequenceFolder(char *outFolder, int cap) {
  if (!s_block)
    return false;

  char dir[MAX_PATH];
  GetModuleDir(dir, sizeof(dir));

  char base[MAX_PATH];
  sprintf_s(base, "%s\\SimpleCamera_Captures", dir);
  CreateDirectoryA(base, nullptr);

  // Find the first render_NNNN folder that doesn't exist yet, then create it.
  for (int n = 1; n < 10000; ++n) {
    char folder[MAX_PATH];
    sprintf_s(folder, "%s\\render_%04d", base, n);
    if (GetFileAttributesA(folder) == INVALID_FILE_ATTRIBUTES) {
      if (CreateDirectoryA(folder, nullptr)) {
        if (outFolder && cap > 0)
          strncpy_s(outFolder, cap, folder, _TRUNCATE);
        return true;
      }
    }
  }
  return false;
}
