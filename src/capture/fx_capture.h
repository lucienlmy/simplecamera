/*
    Simple Camera — Frame Capture Bridge (Phase 1 PoC)

    The ASI cannot read the GPU back buffer itself; the ReShade addon
    (IgcsConnector.addon64) can. Both DLLs live in the GTA5 process, so we
    talk over a single named shared-memory block: the ASI writes a target
    PNG path and bumps a request counter; the addon notices the change on
    its next present, grabs the frame via ReShade's backend-agnostic
    capture_screenshot(), writes the PNG, and echoes the counter back.

    This is the minimal spike that proves the capture pipeline end to end.
    The real image-sequence renderer builds on the same channel.
*/

#pragma once

#include <cstdint>

// Shared layout — MUST stay byte-identical to the copy in the addon's Main.cpp.
#pragma pack(push, 4)
struct FxCaptureBlock {
  uint32_t magic;       // 'SCFX' (0x53434658) — set by the ASI once mapped
  uint32_t version;     // 12
  uint32_t requestId;   // ASI increments to request a capture
  uint32_t ackId;       // addon echoes requestId once handled
  uint32_t status;      // 0 = ok, 1 = capture failed, 2 = file write failed
  uint32_t width;       // addon writes the captured dimensions
  uint32_t height;
  uint32_t sampleCount; // motion-blur samples for this output frame (1 = none)
  uint32_t sampleIndex; // which sample, 0..sampleCount-1 (addon accumulates;
                        // resets on 0, averages + writes on sampleCount-1)
  uint32_t quality;        // JPEG quality 1..100 (ignored for PNG)
  float highlightBoost;    // 0..~1 — extra highlight lift in linear accumulation
  uint32_t addonHeartbeat; // addon bumps this every present; 0 = addon not loaded
  char outPath[512];       // ASI writes the full destination path; the addon
                           // picks PNG vs JPEG from the .png / .jpg extension
  uint32_t channelOrder;   // 0 = Auto (addon detects the back-buffer format),
                           // 1 = force RGBA (no swap), 2 = force BGRA (swap R/B).
                           // Appended LAST so older ASI/addon pairings keep
                           // every pre-existing field at the same offset.

  // --- autofocus for the depth-of-field session (v7) --------------------
  // Not used by Simple Camera - carried so this struct stays byte-identical
  // to the add-on's copy and RockstarEditorPlus's, which is the only thing
  // keeping all three able to drive the same add-on. Do not reorder or drop.
  uint32_t afEnabled;      // ASI -> addon: 1 while the DoF panel wants autofocus
  float    afPointX;       // focus point across the frame, 0..1
  float    afPointY;       // and down it, 0..1
  uint32_t afResultId;     // ASI -> addon: bumped on every answer written
  uint32_t afStatus;       // 0 = ok, 1 = nothing hit, 2 = no camera
  float    afDistance;     // metres to the hit ALONG THE VIEW AXIS
  float    afTanHalfHFov;  // tan(hfov/2) for the probed frame

  // --- a depth-of-field pass per rendered frame (v8) ---------------------
  // Not used by Simple Camera. Carried so the struct stays byte-identical to
  // the add-on's copy and RockstarEditorPlus's. Do not reorder or drop.
  uint32_t dofSeq;         // ASI -> addon: request one DoF pass
  float    dofShutterMs;   // the renderer owns the shutter in this mode
  uint32_t dofDoneSeq;     // addon -> ASI: echoes dofSeq when done
  uint32_t dofStatus;      // 0 idle, 1 running, 2 done, 3 failed

  // --- the lens, pushed by the renderer (v9) -----------------------------
  // Not used by Simple Camera; carried for layout parity. Do not reorder.
  float    dofBokehSize;
  uint32_t dofQuality;
  uint32_t dofAutofocus;
  float    dofFocusX;
  float    dofFocusY;

  // --- which camera tool owns the interface (v10) ------------------------
  uint32_t asiModuleLo;
  uint32_t asiModuleHi;

  // --- manual focus (v11) -----------------------------------------------
  // Not used by Simple Camera - carried so this struct stays byte-identical
  // to the add-on's copy and RockstarEditorPlus's. Do not reorder or drop.
  float    dofFocusDelta;

  // --- what the lens is doing RIGHT NOW (v12) ----------------------------
  // Published every present so a camera tool can capture the focus a user just
  // dialled by eye. The aperture goes with it because FocusDelta is a
  // DISPARITY, not a distance - it scales with maxBokehSize, so a delta
  // without the aperture it was measured at means nothing.
  float    dofLiveFocusDelta;
  float    dofLiveBokeh;
};
#pragma pack(pop)

// Map (or create) the shared block. Safe to call once at startup.
void FxCapture_Init();

// Request a single (no motion blur) capture of the next presented frame into
// `fullPath`. Returns false if the shared channel isn't available.
bool FxCapture_RequestFrame(const char *fullPath);

// Request one motion-blur sub-sample. The addon resets its accumulator on
// sampleIndex 0, adds each sample, and on sampleIndex == sampleCount-1
// averages and writes the PNG to `fullPath`. Returns false if unavailable.
bool FxCapture_RequestSample(const char *fullPath, int sampleCount,
                             int sampleIndex);

// Convenience for the F10 test trigger: builds an auto-numbered path under a
// "SimpleCamera_Captures" folder next to the ASI and requests it. Writes the
// chosen path into `outPathBuf` (may be null). Returns false if unavailable.
bool FxCapture_CaptureTest(char *outPathBuf, int outPathCap);

// Set the JPEG quality (1..100) used when the output path ends in .jpg/.jpeg.
// Sticky — call once before a render. Ignored for PNG output.
void FxCapture_SetQuality(int quality);

// Set the highlight-boost amount (0..~1) applied during linear-light blur
// accumulation. 0 = plain linear average; higher = brighter highlight streaks.
void FxCapture_SetHighlightBoost(float boost);

// Set the captured channel order: 0 = Auto (addon queries the back-buffer
// format — correct on both vanilla GTA Enhanced [BGRA] and e.g. FiveM [RGBA]),
// 1 = force RGBA, 2 = force BGRA. Sticky; fixes red/blue-inverted output.
void FxCapture_SetChannelOrder(int order);

// True once the shared channel is mapped (the ASI side always maps at startup).
bool FxCapture_Available();

// True if the ReShade capture addon (IgcsConnector) is actually loaded and
// running — detected via its per-frame heartbeat. This is what gates rendering.
bool FxCapture_AddonPresent();

// Create a fresh, auto-numbered output folder ("SimpleCamera_Captures/
// render_NNNN") for an image-sequence render. Writes its full path into
// `outFolder`. Returns false on failure.
bool FxCapture_NewSequenceFolder(char *outFolder, int cap);

// True when the addon has acknowledged the most recent capture request
// (ackId caught up to requestId) — i.e. the last requested frame is written.
bool FxCapture_IsLastDone();
