///////////////////////////////////////////////////////////////////////
//
// Part of IGCS Connector, an add on for Reshade 5+ which allows you
// to connect IGCS built camera tools with reshade to exchange data and control
// from Reshade.
// 
// (c) Frans 'Otis_Inf' Bouma.
//
// All rights reserved.
// https://github.com/FransBouma/IgcsConnector
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met :
//
//  * Redistributions of source code must retain the above copyright notice, this
//	  list of conditions and the following disclaimer.
//
//  * Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and / or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
/////////////////////////////////////////////////////////////////////////

#define IMGUI_DISABLE_INCLUDE_IMCONFIG_H

#include "stdafx.h"
#include <imgui.h>
#include <reshade.hpp>
#include <iomanip>
#include <ios>
#include <Psapi.h>
#include <sstream>
#include <string>

#include "CameraToolsData.h"
#include "CDataFile.h"
#include "DepthOfFieldController.h"
#include "ScreenshotController.h"
#include "ScreenshotSettings.h"
#include "OverlayControl.h"
#include "ReshadeStateController.h"
#include "ThreadSafeQueue.h"
#include "WorkItem.h"
#include "fpng.h"
#include "std_image_write.h" // declarations only; impl lives in ScreenshotController.cpp
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace reshade::api;

// ============================================================
//  Simple Camera — Frame Capture Bridge (Phase 1 PoC)
//
//  The Simple Camera ASI requests frame grabs over a named shared-memory
//  block (both DLLs live in GTA5.exe). On each present we check the request
//  counter; if it changed, grab the back buffer via ReShade's backend-
//  agnostic capture_screenshot(), pack RGBA->RGB and write a PNG with fpng
//  (same path ScreenshotController uses), then echo the counter back.
//  Layout MUST match fx_capture.h in the ASI project.
// ============================================================
#pragma pack(push, 4)
struct SC_FxCaptureBlock
{
	uint32_t magic;       // 'SCFX' (0x53434658)
	uint32_t version;
	uint32_t requestId;   // ASI -> addon
	uint32_t ackId;       // addon -> ASI
	uint32_t status;      // 0 ok, 1 capture failed, 2 write failed
	uint32_t width;
	uint32_t height;
	uint32_t sampleCount;   // motion-blur samples (1 = none)
	uint32_t sampleIndex;   // 0..sampleCount-1
	uint32_t quality;       // JPEG quality 1..100 (ignored for PNG)
	float    highlightBoost; // 0..~1 — extra highlight lift in linear accumulation
	uint32_t addonHeartbeat; // we bump this every present so the ASI knows we're loaded
	char outPath[512];
	uint32_t channelOrder;  // 0 = Auto (detect back-buffer format), 1 = force RGBA,
	                        // 2 = force BGRA. Appended LAST so mismatched ASI/addon
	                        // versions keep all earlier fields at the same offset.

	// --- autofocus for the depth-of-field session (v7) --------------------
	// Appended for the same reason. We publish what we want focused; the ASI
	// answers with a measured distance and the lens angle. We keep maxBokehSize
	// on this side and do the conversion here, so dragging it re-derives focus
	// immediately instead of waiting for a round trip.
	uint32_t afEnabled;     // us -> ASI: 1 while the DoF panel wants autofocus
	float    afPointX;      // us -> ASI: focus point across the frame, 0..1
	float    afPointY;      // us -> ASI: and down it, 0..1

	uint32_t afResultId;    // ASI -> us: bumped on every answer written
	uint32_t afStatus;      // ASI -> us: 0 = ok, 1 = nothing hit, 2 = no camera
	float    afDistance;    // ASI -> us: metres to the hit ALONG THE VIEW AXIS
	float    afTanHalfHFov; // ASI -> us: tan(hfov/2) for the probed frame

	// --- a depth-of-field pass per rendered frame (v8) ---------------------
	// The renderer asks us for one accumulated frame instead of accumulating
	// itself. Our finished pass is left ON SCREEN, so its ordinary capture
	// request grabs the result - no new path or buffer on either side.
	uint32_t dofSeq;        // ASI -> us: bumped to ask for one DoF pass
	float    dofShutterMs;  // ASI -> us: the renderer owns the shutter here
	uint32_t dofDoneSeq;    // us -> ASI: echoes dofSeq once the image is up
	uint32_t dofStatus;     // us -> ASI: 0 idle, 1 running, 2 done, 3 failed

	// --- the lens, pushed by the renderer (v9) -----------------------------
	// Read once when a pass starts, never polled: the panel stays authoritative
	// at every other moment. Shape settings stay ours - they are judged by eye.
	float    dofBokehSize;  // aperture diameter
	uint32_t dofQuality;    // ring count
	uint32_t dofAutofocus;  // 1 = measure focus in the world each frame
	float    dofFocusX;     // where to measure, 0..1 across
	float    dofFocusY;     // and down

	// --- which camera tool owns the interface (v10) ------------------------
	// We bind to the first module exporting the IGCS entry points, and more than
	// one mod exports them. The ASI driving a render names itself here so the
	// tie is decided by who is rendering rather than by load order.
	uint32_t asiModuleLo;   // ASI -> us: HMODULE to bind to, 0 = no preference
	uint32_t asiModuleHi;
};
#pragma pack(pop)

static HANDLE g_scFxMapHandle = nullptr;
static SC_FxCaptureBlock* g_scFxBlock = nullptr;
static uint32_t g_scFxLastSeen = 0;
static bool g_scFxFpngInit = false;

// Motion-blur accumulation buffer (CPU). We accumulate in LINEAR light (not
// sRGB) so highlights keep their energy and streak bright instead of being
// averaged down to grey — optionally with an extra highlight lift. Floats so
// the boosted linear values don't clip during summation.
static std::vector<float> g_scFxAccum;
static uint32_t g_scFxAccumW = 0;
static uint32_t g_scFxAccumH = 0;

// sRGB(0..255) -> linear LUT, built once.
static float g_scFxSrgb2Lin[256];
static bool g_scFxLutReady = false;
static void sc_fxBuildLut()
{
	for (int i = 0; i < 256; ++i)
	{
		float c = i / 255.0f;
		g_scFxSrgb2Lin[i] = (c <= 0.04045f) ? (c / 12.92f)
		                                    : powf((c + 0.055f) / 1.055f, 2.4f);
	}
	g_scFxLutReady = true;
}

// linear (0..1+) -> sRGB 8-bit.
static inline uint8_t sc_fxLin2Srgb8(float lin)
{
	if (lin <= 0.0f) return 0;
	if (lin >= 1.0f) return 255;
	float s = (lin <= 0.0031308f) ? (lin * 12.92f)
	                              : (1.055f * powf(lin, 1.0f / 2.4f) - 0.055f);
	int v = (int)(s * 255.0f + 0.5f);
	return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// True if `path` ends (case-insensitively) with `.jpg` or `.jpeg`.
static bool sc_fxIsJpegPath(const char* path)
{
	const size_t n = strlen(path);
	if (n >= 4 && _stricmp(path + n - 4, ".jpg") == 0) return true;
	if (n >= 5 && _stricmp(path + n - 5, ".jpeg") == 0) return true;
	return false;
}

// Write tight RGB to `path`, picking the encoder from the extension: JPEG via
// stb (with `quality`) for .jpg/.jpeg, otherwise PNG via fpng. Returns 0 on
// success, 2 on failure.
static uint32_t sc_fxWriteImage(const char* path, const uint8_t* rgb, uint32_t w, uint32_t h, uint32_t quality)
{
	if (sc_fxIsJpegPath(path))
	{
		int q = (int)quality;
		if (q < 1) q = 1;
		if (q > 100) q = 100;
		return stbi_write_jpg(path, (int)w, (int)h, 3, rgb, q) != 0 ? 0u : 2u;
	}

	if (!g_scFxFpngInit)
	{
		fpng::fpng_init();
		g_scFxFpngInit = true;
	}
	std::vector<uint8_t> encoded;
	if (!fpng::fpng_encode_image_to_memory(rgb, w, h, 3, encoded))
	{
		return 2;
	}
	FILE* f = nullptr;
	if (fopen_s(&f, path, "wb") != 0 || f == nullptr)
	{
		return 2;
	}
	fwrite(encoded.data(), encoded.size(), 1, f);
	fclose(f);
	return 0;
}

static void sc_fxCaptureTick(effect_runtime* runtime)
{
	// Lazily map the shared block (the ASI may map it first, or we do).
	if (g_scFxBlock == nullptr)
	{
		g_scFxMapHandle = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
		                                     0, sizeof(SC_FxCaptureBlock), "Local\\SimpleCameraFxCapture");
		if (g_scFxMapHandle == nullptr)
		{
			return;
		}
		g_scFxBlock = reinterpret_cast<SC_FxCaptureBlock*>(
			MapViewOfFile(g_scFxMapHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SC_FxCaptureBlock)));
		if (g_scFxBlock == nullptr)
		{
			return;
		}
		// Don't fire a stale request that predates us mapping in.
		g_scFxLastSeen = g_scFxBlock->requestId;
	}

	if (g_scFxBlock->magic != 0x53434658u)
	{
		return; // ASI side hasn't stamped the header yet
	}

	// Heartbeat so the ASI can tell this addon is loaded (gates rendering).
	g_scFxBlock->addonHeartbeat++;

	const uint32_t req = g_scFxBlock->requestId;
	if (req == g_scFxLastSeen)
	{
		return; // nothing requested
	}
	g_scFxLastSeen = req;

	const uint32_t sampleCount = (g_scFxBlock->sampleCount < 1) ? 1 : g_scFxBlock->sampleCount;
	const uint32_t sampleIndex = g_scFxBlock->sampleIndex;

	uint32_t width = 0, height = 0;
	runtime->get_screenshot_width_and_height(&width, &height);
	if (width == 0 || height == 0)
	{
		g_scFxBlock->status = 1;
		g_scFxBlock->ackId = req;
		return;
	}

	// The back buffer FORMAT decides two separate things - how many bytes
	// capture_screenshot is going to write, and what order the channels arrive
	// in - so it is read once, before anything is sized.
	reshade::api::format bbFormat = reshade::api::format::unknown;
	if (reshade::api::device *const dev = runtime->get_device())
	{
		const reshade::api::resource bb = runtime->get_current_back_buffer();
		if (bb != 0)
		{
			// format_to_default_typed first: ReShade promotes x8 to a8 before
			// storing _back_buffer_format, and a raw compare would miss b8g8r8x8.
			bbFormat = reshade::api::format_to_default_typed(
				dev->get_resource_desc(bb).texture.format, 0);
		}
	}

	// SIZE THE BUFFER BY THE FORMAT, not by a hardcoded four.
	//
	// The API contract is explicit: "Pointer to an array of width * height * bpp
	// bytes ... where bpp is the number of bytes per pixel of the BACK BUFFER
	// FORMAT". A fixed 4 is only correct while the swapchain is 8-bit.
	//
	// On an HDR swapchain it is not. r16g16b16a16_float is EIGHT bytes per pixel,
	// so a width*height*4 buffer is overrun by exactly its own length - a heap
	// corruption that depends on the user's display settings and would surface
	// as a crash somewhere else entirely. Enabling HDR is all it takes.
	const uint32_t bpp = reshade::api::format_row_pitch(bbFormat, 1);

	// Say what was detected, once. The whole channel-order question is decided
	// by this number and it is otherwise invisible - a swapped render and a
	// correct one differ by nothing a log would normally show. It also answers
	// "do the DX11 and DX12 builds differ" by observation instead of by
	// argument, which is the only way that question stays answered.
	{
		static reshade::api::format loggedFormat = reshade::api::format::unknown;
		static bool everLogged = false;
		if (!everLogged || bbFormat != loggedFormat)
		{
			everLogged = true;
			loggedFormat = bbFormat;
			char msg[192];
			snprintf(msg, sizeof(msg),
				"FxCapture: back buffer format %u, %u byte(s)/pixel - capture reads %s",
				static_cast<uint32_t>(bbFormat), bpp,
				(bbFormat == reshade::api::format::b8g8r8a8_unorm ||
				 bbFormat == reshade::api::format::b8g8r8x8_unorm) ? "BGRA"
				: (bbFormat == reshade::api::format::r10g10b10a2_unorm ||
				   bbFormat == reshade::api::format::b10g10r10a2_unorm) ? "packed 10-bit"
				: (bpp == 4) ? "RGBA" : "an unsupported layout");
			reshade::log::message(reshade::log::level::info, msg);
		}
	}

	if (bpp == 0)
	{
		static bool warnedFormat = false;
		if (!warnedFormat)
		{
			warnedFormat = true;
			reshade::log::message(reshade::log::level::error,
				"FxCapture: could not determine the back buffer format; capture disabled.");
		}
		g_scFxBlock->status = 1;
		g_scFxBlock->ackId = req;
		return;
	}

	std::vector<uint8_t> shot(static_cast<size_t>(width) * height * bpp);
	runtime->capture_screenshot(shot.data());

	// Does capture_screenshot() hand back RGBA or BGRA? It depends on the
	// SWAPCHAIN, and it is knowable rather than guessable.
	//
	// The addon API returns the back buffer's OWN channel order. Its header says
	// so - "bpp is the number of bytes per pixel of the back buffer format" - and
	// runtime.cpp shows why:
	//
	//     bool capture_screenshot(void *pixels) final {
	//         return get_texture_data(..., _back_buffer_format);
	//     }
	//
	// get_texture_data only converts when asked for a DIFFERENT output format:
	//
	//     if (quantization_format == intermediate_format)
	//         -> straight copy, no conversion
	//     if (quantization_format == api::format::r8g8b8a8_unorm)
	//         case api::format::b8g8r8a8_unorm:
	//             // Format is BGRA, but output should be RGBA, so flip channels
	//
	// capture_screenshot passes the back buffer format as the quantization
	// format, so on a BGRA swapchain the two are equal, the first branch wins,
	// and the bytes arrive BGRA. ReShade's own screenshots are correct because
	// they pass r8g8b8a8_unorm explicitly and therefore DO hit the flip.
	//
	// A previous version of this file concluded the opposite - that the flip
	// always happens - from reading that `case` without the `if` wrapping it,
	// and hardcoded "never swap". That is correct only on RGBA swapchains. On
	// GTA V Enhanced it produced a measurable red/blue swap: comparing a render
	// against a ReShade screenshot of the same scene, the render's R-G matched
	// the screenshot's B-G and vice versa, and R-B came out +4.01 where an
	// unswapped capture would have given -3.78.
	//
	// So: read the format. format_to_default_typed first, because ReShade
	// promotes x8 to a8 before storing _back_buffer_format (runtime.cpp:384) and
	// a raw comparison would miss b8g8r8x8.
	bool swapRB = false;
	const uint32_t orderMode = g_scFxBlock->channelOrder;
	if (orderMode == 1)
	{
		swapRB = false;  // forced RGBA
	}
	else if (orderMode == 2)
	{
		swapRB = true;   // forced BGRA
	}
	else
	{
		swapRB = (bbFormat == reshade::api::format::b8g8r8a8_unorm ||
		          bbFormat == reshade::api::format::b8g8r8x8_unorm ||
		          bbFormat == reshade::api::format::b10g10r10a2_unorm);
	}

	// Everything below reads four bytes per pixel, one 8-bit channel each.
	//
	// A 10-bit swapchain is also four bytes per pixel but is NOT that layout -
	// it is three packed 10-bit bitfields plus two bits of alpha, and
	// capture_screenshot hands it over verbatim because it quantises to the back
	// buffer's own format. Reading byte 0 as red there is not a channel swap,
	// it is the bottom eight bits of red mixed with nothing. Unpack it into the
	// layout the rest of the function expects, in place - the sizes match, so
	// this costs one pass and no allocation.
	const bool tenBit = (bbFormat == reshade::api::format::r10g10b10a2_unorm ||
	                     bbFormat == reshade::api::format::b10g10r10a2_unorm);
	if (tenBit)
	{
		for (uint32_t i = 0; i < width * height; ++i)
		{
			uint32_t v = 0;
			std::memcpy(&v, &shot[4 * i], sizeof(v));
			// >> 2 takes the 10-bit range (0-1023) to 8-bit (0-255), the same
			// reduction ReShade applies on this path.
			const uint8_t c0 = static_cast<uint8_t>(( v         & 0x3FFu) >> 2);
			const uint8_t c1 = static_cast<uint8_t>(((v >> 10)  & 0x3FFu) >> 2);
			const uint8_t c2 = static_cast<uint8_t>(((v >> 20)  & 0x3FFu) >> 2);
			// Which end holds red is the same question swapRB already answers,
			// so a forced order override keeps working here too.
			shot[4 * i + 0] = swapRB ? c2 : c0;
			shot[4 * i + 1] = c1;
			shot[4 * i + 2] = swapRB ? c0 : c2;
			shot[4 * i + 3] = 0xFF;
		}
	}
	else if (bpp != 4)
	{
		// A float or 16-bit-per-channel back buffer - an HDR swapchain. There is
		// no honest 8-bit answer without tonemapping it, and inventing one here
		// would silently change what the renderer produces. Refuse loudly.
		static bool warnedHdr = false;
		if (!warnedHdr)
		{
			warnedHdr = true;
			reshade::log::message(reshade::log::level::error,
				"FxCapture: the back buffer is not an 8-bit format (HDR swapchain?); "
				"capture is disabled. Render in SDR, or turn HDR off for the render.");
		}
		g_scFxBlock->status = 1;
		g_scFxBlock->ackId = req;
		return;
	}

	// After the unpack above the data is plain RGBA, so the swap is spent.
	const uint32_t ri = (swapRB && !tenBit) ? 2u : 0u; // byte index of red
	const uint32_t bi = (swapRB && !tenBit) ? 0u : 2u; // byte index of blue

	uint32_t status = 0;

	if (sampleCount <= 1)
	{
		// Single capture: pack down to tight RGB in the detected order, write.
		for (uint32_t i = 0; i < width * height; ++i)
		{
			const uint8_t r = shot[4 * i + ri];
			const uint8_t g = shot[4 * i + 1];
			const uint8_t b = shot[4 * i + bi];
			shot[3 * i + 0] = r;
			shot[3 * i + 1] = g;
			shot[3 * i + 2] = b;
		}
		status = sc_fxWriteImage(g_scFxBlock->outPath, shot.data(), width, height, g_scFxBlock->quality);
	}
	else
	{
		// Motion-blur accumulation in LINEAR light, with optional highlight
		// boost (AccentuateWhites-style: y = x / (1.001 - b*x)). Reset on the
		// first sample (or if the size changed).
		if (!g_scFxLutReady) sc_fxBuildLut();
		const float b = (g_scFxBlock->highlightBoost < 0.0f) ? 0.0f
		              : (g_scFxBlock->highlightBoost > 0.99f) ? 0.99f
		                                                      : g_scFxBlock->highlightBoost;
		const size_t count = static_cast<size_t>(width) * height * 3;
		if (sampleIndex == 0 || g_scFxAccumW != width || g_scFxAccumH != height)
		{
			g_scFxAccum.assign(count, 0.0f);
			g_scFxAccumW = width;
			g_scFxAccumH = height;
		}
		for (uint32_t i = 0; i < width * height; ++i)
		{
			float r = g_scFxSrgb2Lin[shot[4 * i + ri]];
			float g = g_scFxSrgb2Lin[shot[4 * i + 1]];
			float bl = g_scFxSrgb2Lin[shot[4 * i + bi]];
			if (b > 0.0f)
			{
				r  = r  / (1.001f - b * r);
				g  = g  / (1.001f - b * g);
				bl = bl / (1.001f - b * bl);
			}
			g_scFxAccum[3 * i + 0] += r;
			g_scFxAccum[3 * i + 1] += g;
			g_scFxAccum[3 * i + 2] += bl;
		}

		if (sampleIndex >= sampleCount - 1)
		{
			// Final sample: average in linear, undo the highlight boost, then
			// encode back to sRGB.
			std::vector<uint8_t> out(count);
			const float inv = 1.0f / (float)sampleCount;
			for (size_t k = 0; k < count; ++k)
			{
				float v = g_scFxAccum[k] * inv;
				if (b > 0.0f) v = v / (1.001f + b * v); // inverse of the lift
				out[k] = sc_fxLin2Srgb8(v);
			}
			status = sc_fxWriteImage(g_scFxBlock->outPath, out.data(), width, height, g_scFxBlock->quality);
		}
	}

	g_scFxBlock->width = width;
	g_scFxBlock->height = height;
	g_scFxBlock->status = status;
	g_scFxBlock->ackId = req;
}

// externs for reshade
extern "C" __declspec(dllexport) const char *NAME = "IGCS Connector";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Add-on which allows you to connect Reshade with IGCS built camera tools to exchange data and issue commands.";

// externs for IGCS
extern "C" __declspec(dllexport) bool connectFromCameraTools();
extern "C" __declspec(dllexport) LPBYTE getDataFromCameraToolsBuffer();
extern "C" __declspec(dllexport) void addCameraPath();
extern "C" __declspec(dllexport) void appendStateSnapshotAfterSnapshotOnPath(int pathIndex, int indexToAppendAfter);
extern "C" __declspec(dllexport) void appendStateSnapshotToPath(int pathIndex);
extern "C" __declspec(dllexport) void clearPaths();
extern "C" __declspec(dllexport) void insertStateSnapshotBeforeSnapshotOnPath(int pathIndex, int indexToInsertBefore);
extern "C" __declspec(dllexport) void removeCameraPath(int pathIndex);
extern "C" __declspec(dllexport) void removeStateSnapshotFromPath(int pathIndex, int stateIndex);
extern "C" __declspec(dllexport) void setReshadeStateInterpolated(int pathIndex, int fromStateIndex, int toStateIndex, float interpolationFactor);
extern "C" __declspec(dllexport) void setReshadeState(int pathIndex, int stateIndex);
extern "C" __declspec(dllexport) void updateStateSnapshotOnPath(int pathIndex, int stateIndex);

#define SETTINGS_FILE_NAME "IgcsConnector.ini"

static LPBYTE g_dataFromCameraToolsBuffer = nullptr;		// 8192 bytes buffer
static CameraToolsConnector g_cameraToolsConnector;
static ScreenshotSettings g_screenshotSettings;
static ScreenshotController g_screenshotController(g_cameraToolsConnector);
static DepthOfFieldController g_depthOfFieldController(g_cameraToolsConnector);
static ReshadeStateController g_reshadeStateController;
static IGCS::ThreadSafeQueue<WorkItem> g_presentWorkQueue;
static bool g_recordReshadeState = true;

/// <summary>
/// Entry point for IGCS camera tools. Call this to initialize the buffers. Obtain the buffers using the getDataFrom/ToCameraToolsBuffer functions
/// </summary>
/// <returns>true if the allocation went OK, false otherwise. Returns true as well if this function was already called.</returns>
bool connectFromCameraTools()
{
	// Allocate buffer only if not already allocated
	if(nullptr == g_dataFromCameraToolsBuffer)
	{
		// malloc 8K buffers
		g_dataFromCameraToolsBuffer = (LPBYTE)calloc(8 * 1024, 1);
	}

	// Always (re)connect to the camera tools to get fresh function pointers.
	// This permits tools that can unload/load their dll again to work properly.
	g_cameraToolsConnector.connectToCameraTools();

	return g_dataFromCameraToolsBuffer!=nullptr;
}


/// <summary>
/// Gets the pointer to the buffer (8KB) for data to this addon from the camera tools.
/// </summary>
/// <returns>valid pointer or nullptr if connectFromCameraTools hasn't been called yet</returns>
LPBYTE getDataFromCameraToolsBuffer()
{
	return g_dataFromCameraToolsBuffer;
}


/// <summary>
/// Clears all contained camera paths
/// </summary>
void clearPaths()
{
	g_reshadeStateController.clearPaths();
}


/// <summary>
/// Adds a camera path to the reshade state controller
/// </summary>
void addCameraPath()
{
	if(!g_recordReshadeState)
	{
		return;
	}
	g_reshadeStateController.addCameraPath();
}


/// <summary>
/// Removes the path with the index specified
/// </summary>
/// <param name="pathIndex"></param>
void removeCameraPath(int pathIndex)
{
	g_reshadeStateController.removeCameraPath(pathIndex);
}


/// <summary>
/// Appends the current state to the path with the index specified
/// </summary>
/// <param name="pathIndex"></param>
void appendStateSnapshotToPath(int pathIndex)
{
	if(!g_recordReshadeState)
	{
		return;
	}

	// done deferred.
	g_presentWorkQueue.push({ [pathIndex](effect_runtime* lambdaRuntime) {g_reshadeStateController.appendStateSnapshotToPath(pathIndex, lambdaRuntime); } });
}


/// <summary>
/// Inserts the current state to the path with the index specified before the snapshot with the index specified. 
/// </summary>
/// <param name="pathIndex"></param>
/// <param name="indexToInsertBefore"></param>
void insertStateSnapshotBeforeSnapshotOnPath(int pathIndex, int indexToInsertBefore)
{
	if(!g_recordReshadeState)
	{
		return;
	}
	// done deferred
	g_presentWorkQueue.push({ [pathIndex, indexToInsertBefore](effect_runtime* lambdaRuntime) {g_reshadeStateController.insertStateSnapshotBeforeSnapshotOnPath(pathIndex, indexToInsertBefore, lambdaRuntime); } });
}


/// <summary>
/// Appends the current state to the path with the index specified after the snapshot with the index specified
/// </summary>
/// <param name="pathIndex"></param>
/// <param name="indexToAppendAfter"></param>
void appendStateSnapshotAfterSnapshotOnPath(int pathIndex, int indexToAppendAfter)
{
	if(!g_recordReshadeState)
	{
		return;
	}
	// done deferred
	g_presentWorkQueue.push({ [pathIndex, indexToAppendAfter](effect_runtime* lambdaRuntime) {g_reshadeStateController.appendStateSnapshotAfterSnapshotOnPath(pathIndex, indexToAppendAfter, lambdaRuntime); } });
}


/// <summary>
/// Update the state at offset stateIndex on path with index pathIndex to the current state
/// </summary>
/// <param name="pathIndex"></param>
/// <param name="stateIndex"></param>
void updateStateSnapshotOnPath(int pathIndex, int stateIndex)
{
	if(!g_recordReshadeState)
	{
		return;
	}
	// done deferred.
	g_presentWorkQueue.push({ [pathIndex, stateIndex](effect_runtime* lambdaRuntime) {g_reshadeStateController.updateStateSnapshotOnPath(pathIndex, stateIndex, lambdaRuntime); } });
}


/// <summary>
/// Removes the state at index stateIndex from the path at index pathIndex
/// </summary>
/// <param name="pathIndex"></param>
/// <param name="stateIndex"></param>
void removeStateSnapshotFromPath(int pathIndex, int stateIndex)
{
	g_reshadeStateController.removeStateSnapshotFromPath(pathIndex, stateIndex);
}


/// <summary>
/// Sets reshade's effect state to the interpolation of the two states at offset fromStateIndex and toStateIndex on path with index pathIndex, using the interpolation factor specified. 
/// </summary>
/// <param name="pathIndex"></param>
/// <param name="fromStateIndex"></param>
/// <param name="toStateIndex"></param>
/// <param name="interpolationFactor">if 0.0 the state will be fromState, if 1.0 the state will be toState, any value between 0 and 1 will be an interpolation using lerp of fromState and toState</param>
void setReshadeStateInterpolated(int pathIndex, int fromStateIndex, int toStateIndex, float interpolationFactor)
{
	if(!g_recordReshadeState)
	{
		return;
	}

	// done deferred
	g_presentWorkQueue.push({ [pathIndex, fromStateIndex, toStateIndex, interpolationFactor](effect_runtime* lambdaRuntime)
	{
		g_reshadeStateController.setReshadeState(pathIndex, fromStateIndex, toStateIndex, interpolationFactor, lambdaRuntime);
	} });
}


/// <summary>
/// Sets reshade's effect state to the state on index stateIndex on path with index pathIndex
/// </summary>
/// <param name="pathIndex"></param>
/// <param name="stateIndex"></param>
void setReshadeState(int pathIndex, int stateIndex)
{
	if(!g_recordReshadeState)
	{
		return;
	}

	// done deferred
	g_presentWorkQueue.push({ [pathIndex, stateIndex](effect_runtime* lambdaRuntime) {g_reshadeStateController.setReshadeState(pathIndex, stateIndex, lambdaRuntime); } });
}



void handleWorkQueue(effect_runtime* runtime)
{
	for(;;)
	{
		auto topElement = g_presentWorkQueue.pop();
		if(!topElement.has_value())
		{
			break;
		}
		auto workItem = topElement.value();
		workItem.perform(runtime);
	}
}


// Autofocus exchange over the same shared block as frame capture.
//
// Separate from sc_fxCaptureTick, and not folded into it, for two reasons. That
// function returns early when no capture is pending - and during a depth-of-field
// setup pass nothing is being captured, which is exactly when focus matters. And
// it is defined above g_depthOfFieldController, so it cannot see the controller
// at all.
//
// We publish what we want focused; the connected tool answers when it has
// measured. It bumps afResultId rather than us diffing the distance, so a
// measurement that repeats the previous number still counts as a fresh answer -
// which is what a stationary subject looks like.
static void sc_fxAutofocusTick(effect_runtime* runtime)
{
	if (nullptr == g_scFxBlock || g_scFxBlock->magic != 0x53434658u)
	{
		return;
	}

	// Publish the frame size every present, not only after a capture.
	//
	// These used to be written in the capture path alone, which is fine for a
	// render - something has always been captured by the time anyone reads them.
	// A depth-of-field session captures NOTHING while it is being set up, so the
	// other side had no aspect ratio at exactly the moment it needed one to turn
	// a vertical field of view into a horizontal one, and answered "no camera".
	// Same value either way; this just makes it available before the first frame
	// is grabbed rather than after.
	{
		uint32_t w = 0, h = 0;
		runtime->get_screenshot_width_and_height(&w, &h);
		if (w > 0 && h > 0)
		{
			g_scFxBlock->width  = w;
			g_scFxBlock->height = h;
		}
	}

	const bool wantAf = g_depthOfFieldController.getAutofocusEnabled() &&
	                    DepthOfFieldControllerState::Setup == g_depthOfFieldController.getState();

	g_scFxBlock->afEnabled = wantAf ? 1u : 0u;
	g_scFxBlock->afPointX  = g_depthOfFieldController.getAutofocusPointX();
	g_scFxBlock->afPointY  = g_depthOfFieldController.getAutofocusPointY();

	static uint32_t s_afLastResult = 0;
	const uint32_t afResult = g_scFxBlock->afResultId;

	if (!wantAf)
	{
		// Track the counter while idle, or switching autofocus back on replays
		// one stale measurement from whenever it was last used.
		s_afLastResult = afResult;
		return;
	}
	if (afResult != s_afLastResult)
	{
		s_afLastResult = afResult;
		g_depthOfFieldController.applyAutofocusMeasurement(runtime,
			g_scFxBlock->afDistance, g_scFxBlock->afTanHalfHFov,
			(int)g_scFxBlock->afStatus);
	}
}


// One accumulated depth-of-field frame, driven by the renderer instead of by a
// person clicking through this panel.
//
// The panel's own flow is: start a session, dial the focus in by eye, press
// render, wait, look at the result. Every one of those steps is a human, and
// that is the only reason a sequence was never possible. Autofocus removed the
// one that mattered - Setup no longer needs anyone - so the rest is just calling
// the same three functions in order.
//
// The finished image is deliberately LEFT ON SCREEN in the Done state. The
// renderer then takes an ordinary capture, which is what makes this cost no new
// file path, buffer or image format anywhere.
bool requiredTechniqueEnabled(const std::string& effectName, const std::string& techniqueName, effect_runtime* runtime);

static void sc_fxDofTick(effect_runtime* runtime)
{
	if (nullptr == g_scFxBlock || g_scFxBlock->magic != 0x53434658u)
	{
		return;
	}

	enum class Phase { Idle, WaitSetup, WaitRender, Delivered };
	static Phase    s_phase   = Phase::Idle;
	static uint32_t s_seenSeq = 0;
	static uint32_t s_waited  = 0;
	static bool     s_lensApplied = false;
	static uint32_t s_afSeqAtStart = 0;

	const uint32_t seq = g_scFxBlock->dofSeq;

	// 0 means the renderer wants no session at all. Also the way it says "the
	// render is over" and the way it cancels, so one path tears down.
	if (seq == 0)
	{
		if (s_phase != Phase::Idle)
		{
			g_depthOfFieldController.endSession(runtime);
			s_phase = Phase::Idle;
			g_scFxBlock->dofStatus = 0;
		}
		s_seenSeq = 0;
		return;
	}

	// A new request. Anything still up belongs to the previous frame - end it,
	// which is also what releases the image the renderer has by now captured.
	//
	// Keyed on the CONTROLLER's state rather than our own phase, so it also
	// clears a session the user started by hand and left open. Otherwise the
	// first frame of a render would try to open a second session on top of it,
	// be refused, and abort the whole render - with the cause being something
	// the user did several minutes earlier in a different window.
	if (seq != s_seenSeq)
	{
		s_seenSeq = seq;
		s_waited  = 0;
		s_lensApplied = false;
		if (DepthOfFieldControllerState::Off != g_depthOfFieldController.getState())
		{
			g_depthOfFieldController.endSession(runtime);
		}
		// Bind to the tool that is actually driving this render.
		//
		// We normally take the first loaded module exporting the IGCS entry
		// points, and in a modded game that is decided by load order - NVE
		// exports them too, so a session could end up driving ITS camera while
		// RockstarEditorPlus is the one asking for frames. Nothing downstream
		// notices: the session starts, the passes complete, and every frame is
		// identical because the camera never moved where we thought it did.
		//
		// The renderer names its own module in the block, so obey that while it
		// is driving. Only for render-driven passes; a session someone starts
		// from this panel keeps whatever the scan found.
		{
			const uint64_t h = ((uint64_t)g_scFxBlock->asiModuleHi << 32) |
			                    (uint64_t)g_scFxBlock->asiModuleLo;
			if (h != 0 && !g_cameraToolsConnector.connectToModule((HMODULE)h))
			{
				reshade::log::message(reshade::log::level::warning,
					"DoF: the renderer named a module that does not export the IGCS "
					"interface - staying with whatever was found by scanning.");
			}
		}

		// The shader has to be ON, and this is the only place that checks.
		//
		// The panel's own "Start depth-of-field session" button is gated on it
		// and says so; the render path called startSession directly and did not.
		// IgcsDof.fx IS the accumulation - the add-on only moves the camera and
		// writes uniforms into it - so without the technique enabled a render
		// completes normally and produces plain frames with no depth of field in
		// them at all. Nothing else in the chain notices, because every step it
		// checks did work.
		if (!requiredTechniqueEnabled("IgcsDof.fx", "IgcsDOF", runtime))
		{
			reshade::log::message(reshade::log::level::error,
				"DoF: the IgcsDOF technique is not enabled, so nothing would accumulate "
				"and the render would produce plain frames. Enable 'IgcsDOF' on ReShade's "
				"Home tab and drag it to the bottom of the list, then render again.");
			g_scFxBlock->dofStatus = 3;
			s_phase = Phase::Idle;
			return;
		}

		g_depthOfFieldController.setShutterMs(g_scFxBlock->dofShutterMs);
		g_depthOfFieldController.startSession(runtime);
		s_phase = (DepthOfFieldControllerState::Setup == g_depthOfFieldController.getState() ||
		           DepthOfFieldControllerState::Start == g_depthOfFieldController.getState())
			? Phase::WaitSetup : Phase::Idle;
		g_scFxBlock->dofStatus = (s_phase == Phase::WaitSetup) ? 1u : 3u;
		return;
	}

	switch (s_phase)
	{
		case Phase::WaitSetup:
		{
			++s_waited;
			if (DepthOfFieldControllerState::Setup != g_depthOfFieldController.getState())
			{
				// Still starting, or it failed outright.
				if (s_waited > 600)
				{
					g_scFxBlock->dofStatus = 3;
					s_phase = Phase::Idle;
				}
				break;
			}

			// Apply the lens the renderer pushed, now that we are in Setup.
			//
			// Here and not at the request, because setMaxBokehSize refuses
			// outside Setup - it rescales the focus delta as it goes, which is
			// only meaningful once a session exists. Applied every pass rather
			// than once, so changing the aperture between renders takes without
			// anyone having to restart anything.
			if (!s_lensApplied)
			{
				s_lensApplied = true;
				g_depthOfFieldController.setQuality((int)g_scFxBlock->dofQuality);

				// The renderer's highlight boost, pointed at OUR accumulator.
				//
				// It normally applies while the renderer averages its own
				// captures - which does not happen here, since a pass arrives
				// already accumulated and is grabbed in one shot. The job is
				// identical though (keep speculars bright instead of averaging
				// them down to grey) and so is the range, so the setting is
				// routed rather than disabled. The GAMMA that goes with it stays
				// in this panel: it is a curve, and curves are judged by eye.
				g_depthOfFieldController.setHighlightBoostFactor(g_scFxBlock->highlightBoost);
				g_depthOfFieldController.setAutofocusEnabled(g_scFxBlock->dofAutofocus != 0);
				// Applied BEFORE the point, because the point is only meaningful
				// once autofocus owns the focus - and before maxBokehSize, whose
				// setter rescales the focus delta the measurement is about to
				// replace.
				g_depthOfFieldController.setAutofocusPoint(g_scFxBlock->dofFocusX,
				                                           g_scFxBlock->dofFocusY);
				g_depthOfFieldController.setMaxBokehSize(runtime, g_scFxBlock->dofBokehSize);

				// Remember which answer was current, so the wait below can tell a
				// FRESH measurement from the one this frame inherited.
				s_afSeqAtStart = g_scFxBlock->afResultId;

				// Report what was received, and what the controller made of it.
				//
				// setMaxBokehSize refuses outside Setup and clamps inside it, so
				// "the renderer sent 0.12" and "the lens is 0.12" are different
				// claims - and only the second one is what gets rendered.
				char msg[256];
				snprintf(msg, sizeof(msg),
					"DoF: renderer set aperture %.3f (lens now %.3f), %u rings, "
					"autofocus %s at %.2f,%.2f, highlight %.2f",
					g_scFxBlock->dofBokehSize, g_depthOfFieldController.getMaxBokehSize(),
					g_scFxBlock->dofQuality,
					g_scFxBlock->dofAutofocus ? "on" : "off",
					g_scFxBlock->dofFocusX, g_scFxBlock->dofFocusY,
					g_scFxBlock->highlightBoost);
				reshade::log::message(reshade::log::level::info, msg);
			}

			// Give autofocus a measurement before committing the frame. Without
			// this the first frame of a render focuses on whatever the last
			// session left behind, which is usually the wrong subject and always
			// the wrong distance.
			const bool wantAf = g_depthOfFieldController.getAutofocusEnabled();

			// Wait for an answer newer than this pass, not for a good status.
			//
			// Status is sticky - it keeps the last pass's value - so testing it
			// let every frame after the first commit immediately on the PREVIOUS
			// frame's focus. On a static shot that is invisible; on a moving one
			// focus trails the subject by a frame forever.
			//
			// Keyed on the answer counter rather than the status so a measurement
			// that finds nothing still counts as an answer. The controller holds
			// its last good focus in that case, which is the right response to a
			// focus point that briefly crosses the sky - far better than waiting
			// out the timeout on every frame of a render.
			const bool afSettled = !wantAf || (g_scFxBlock->afResultId != s_afSeqAtStart);

			// Trace the Setup phase for the first few passes of a render.
			//
			// This is where focus is decided, and a pass that commits before the
			// measurement lands renders with whatever the previous session left
			// behind - which looks exactly like autofocus not working at all.
			// Only the first passes, so a long render is not flooded.
			{
				static int traced = 0;
				if (traced < 12)
				{
					++traced;
					char m[224];
					snprintf(m, sizeof(m),
						"DoF setup: waited %u, afWanted %d, afStatus %d, delta %.5f, "
						"dist %.2f -> %s",
						s_waited, wantAf ? 1 : 0,
						g_depthOfFieldController.getAutofocusStatus(),
						g_depthOfFieldController.getXFocusDelta(),
						g_depthOfFieldController.getAutofocusDistance(),
						((afSettled && s_waited >= 3) || s_waited > 600) ? "COMMIT" : "wait");
					reshade::log::message(reshade::log::level::info, m);
				}
			}

			if ((afSettled && s_waited >= 3) || s_waited > 600)
			{
				g_depthOfFieldController.startRender(runtime);
				s_phase = Phase::WaitRender;
				s_waited = 0;
			}
			break;
		}

		case Phase::WaitRender:
			if (DepthOfFieldControllerState::Done == g_depthOfFieldController.getState())
			{
				// The image is up. Say so and then do nothing - the session is
				// held open on purpose, because ending it here would clear the
				// very frame the renderer is about to capture.
				g_scFxBlock->dofStatus  = 2;
				g_scFxBlock->dofDoneSeq = s_seenSeq;
				s_phase = Phase::Delivered;
			}
			else if (DepthOfFieldControllerState::Rendering != g_depthOfFieldController.getState())
			{
				g_scFxBlock->dofStatus = 3;   // cancelled or fell over
				s_phase = Phase::Idle;
			}
			break;

		default:
			break;
	}
}


static void onReshadePresent(effect_runtime* runtime)
{
	g_screenshotController.presentCalled();

	// Simple Camera frame-capture requests.
	sc_fxCaptureTick(runtime);
	sc_fxAutofocusTick(runtime);
	sc_fxDofTick(runtime);

	// handle our work.
	handleWorkQueue(runtime);
}


static void onReshadeOverlay(effect_runtime* runtime)
{
	// first let the screenshot controller grab screenshots
	g_screenshotController.reshadeEffectsRendered(runtime);

	// then we'll render our own overlays if needed
	OverlayControl::renderOverlay();
	g_depthOfFieldController.renderOverlay();		// if it has something to display it can do that here
}


static void showHelpMarker(const char* desc)
{
	ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered())
	{
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(450.0f);
		ImGui::TextUnformatted(desc);
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}
}


static void startScreenshotSession(bool isTestRun)
{
	g_screenshotController.configure(g_screenshotSettings.screenshotFolder, g_screenshotSettings.numberOfFramesToWaitBetweenSteps, (ScreenshotFiletype)g_screenshotSettings.screenshotFileType);
	const auto cameraData = (CameraToolsData*)g_dataFromCameraToolsBuffer;
	switch(g_screenshotSettings.typeOfScreenshot)
	{
	case (int)ScreenshotType::HorizontalPanorama:
		g_screenshotController.startHorizontalPanoramaShot(g_screenshotSettings.pano_totalAngleDegrees, g_screenshotSettings.pano_overlapPercentagePerShot, cameraData->fov, isTestRun);
		break;
	case (int)ScreenshotType::MultiShot:
		g_screenshotController.startLightfieldShot(g_screenshotSettings.lightField_distanceBetweenShots, g_screenshotSettings.lightField_numberOfShotsToTake, isTestRun);
		break;
#ifdef _DEBUG
	case (int)ScreenshotType::DebugGrid:
		g_screenshotController.startDebugGridShot();
		break;
#endif
	}
}


void loadGeneralSettingsFromIniFile(CDataFile& iniFile)
{
	if(iniFile.GetValue("RecordReshadeState", "General").length() > 0)
	{
		g_recordReshadeState = iniFile.GetBool("RecordReshadeState", "General");
	}

	// more settings here
}


void saveGeneralSettingsToIniFile(CDataFile& iniFile)
{
	iniFile.SetBool("RecordReshadeState", g_recordReshadeState, "", "General");

	// more settings here
}


void loadIniFile()
{
	CDataFile iniFile;
	if(!iniFile.Load(SETTINGS_FILE_NAME))
	{
		// not there
		return;
	}

	g_depthOfFieldController.loadIniFileData(iniFile);
	loadGeneralSettingsFromIniFile(iniFile);
}



void saveIniFile()
{
	CDataFile iniFile;
	g_depthOfFieldController.saveIniFileData(iniFile);

	saveGeneralSettingsToIniFile(iniFile);

	iniFile.SetFileName(SETTINGS_FILE_NAME);
	iniFile.Save();
}


bool requiredTechniqueEnabled(const std::string& effectName, const std::string& techniqueName , effect_runtime* runtime)
{
	const auto techniqueHandle = runtime->find_technique(effectName.c_str(), techniqueName.c_str());
	if(techniqueHandle.handle <= 0)
	{
		return false;
	}

	return runtime->get_technique_state(techniqueHandle);
}


/// Based on Reshade's functions doing the same thing but simplified as we only need it for ints
///	Replaces controls like ImGui::DragInt("Quality", &quality, 1, 1, 100);
///	Returns true if changed, false otherwise.
static bool intDragWithButtons(const char* label, int* value, int speed, int min, int max, const char* toolTip=nullptr)
{
	const float button_size = ImGui::GetFrameHeight();
	const float button_spacing = ImGui::GetStyle().ItemInnerSpacing.x;

	ImGui::BeginGroup();
	ImGui::PushID(label);

	ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - (2 * (button_spacing + button_size)));
	bool toReturn = ImGui::DragInt("##v", value, speed, min, max);
	if(nullptr!=toolTip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
	{
		ImGui::SetTooltip(toolTip);
	}
	ImGui::SameLine(0, button_spacing);
	if(ImGui::Button("<", ImVec2(button_size, 0)))
	{
		*value -= speed;
		toReturn = true;
	}
	ImGui::SameLine(0, button_spacing);
	if(ImGui::Button(">", ImVec2(button_size, 0)))
	{
		*value += speed;
		toReturn = true;
	}

	ImGui::PopID();
	ImGui::SameLine(0, button_spacing);
	ImGui::TextUnformatted(label);
	ImGui::EndGroup();
	return toReturn;
}


static void displaySettings(reshade::api::effect_runtime* runtime)
{
	ImGui::AlignTextToFramePadding();
	const auto cameraData = (CameraToolsData*)g_dataFromCameraToolsBuffer;
	if(ImGui::CollapsingHeader("Screenshot features"))
	{
		if(g_cameraToolsConnector.cameraToolsConnected() && nullptr != cameraData)
		{
			switch(g_screenshotController.getState())
			{
				case ScreenshotControllerState::Off:
					{
						ImGui::PushItemWidth(ImGui::GetWindowWidth() * 0.5f);
						ImGui::AlignTextToFramePadding();
						ImGui::InputText("Screenshot output directory", g_screenshotSettings.screenshotFolder, 256);
						ImGui::SliderInt("Number of frames to wait between steps", &g_screenshotSettings.numberOfFramesToWaitBetweenSteps, 1, 100);
#ifdef _DEBUG
						ImGui::Combo("Multi-screenshot type", &g_screenshotSettings.typeOfScreenshot, "Horizontal panorama\0Lightfield\0DEBUG: Grid\0");
#else
						ImGui::Combo("Multi-screenshot type", &g_screenshotSettings.typeOfScreenshot, "Horizontal panorama\0Lightfield\0\0");
#endif
						ImGui::Combo("File type", &g_screenshotSettings.screenshotFileType, "Bmp\0Jpeg\0Png\0\0");
						switch(g_screenshotSettings.typeOfScreenshot)
						{
							case (int)ScreenshotType::HorizontalPanorama:
								ImGui::SliderFloat("Total field of view in panorama (in degrees)", &g_screenshotSettings.pano_totalAngleDegrees, 30.0f, 360.0f, "%.1f");
								ImGui::SliderFloat("Percentage of overlap between shots", &g_screenshotSettings.pano_overlapPercentagePerShot, 0.1f, 99.0f, "%.1f");
								break;
							case (int)ScreenshotType::MultiShot:
								ImGui::SliderFloat("Distance between Lightfield shots", &g_screenshotSettings.lightField_distanceBetweenShots, 0.0f, 5.0f, "%.3f");
								ImGui::SliderInt("Number of shots to take", &g_screenshotSettings.lightField_numberOfShotsToTake, 0, 60);
								break;
								// others: ignore.
						}
						ImGui::PopItemWidth();
						if(cameraData->cameraEnabled)
						{
							if(ImGui::Button("Start screenshot session"))
							{
								startScreenshotSession(false);
							}
							ImGui::SameLine();
							if(ImGui::Button("Start test run"))
							{
								startScreenshotSession(true);
							}
						}
						else
						{
							ImGui::Text("Camera disabled so no screenshot session can be started");
						}
					}
					break;
				case ScreenshotControllerState::InSession:
					{
						if(ImGui::Button("Cancel session"))
						{
							g_screenshotController.cancelSession();
						}
					}
					break;
				case ScreenshotControllerState::Canceling:
					ImGui::Text("Cancelling session...");
					break;
				case ScreenshotControllerState::SavingShots:
					ImGui::Text("Saving shots...");
					break;
			}
		}
		else
		{
			ImGui::Text("Camera tools not available");
		}
	}

	ImGui::AlignTextToFramePadding();
	if(ImGui::CollapsingHeader("Depth of Field control", ImGuiTreeNodeFlags_DefaultOpen) && nullptr != g_dataFromCameraToolsBuffer)
	{
		// start: button with 'Start session'
		// step 1: set value A and B and show Render button
		// step 2: rendering: show 'Done' and 'Cancel' buttons
		// end: go back to the start step.
		if(g_cameraToolsConnector.cameraToolsConnected() && nullptr!=cameraData)
		{
			switch(g_depthOfFieldController.getState())
			{
				case DepthOfFieldControllerState::Off:
					{
						// always pass the state to the shader, so it'll reset to 'OFF' if the user accidentally quit the game without clicking done
						g_depthOfFieldController.writeVariableStateToShader(runtime);

						if(cameraData->cameraEnabled)
						{
							if(requiredTechniqueEnabled("IgcsDof.fx", "IgcsDOF", runtime))
							{
								if(ImGui::Button("Start depth-of-field session"))
								{
									g_depthOfFieldController.startSession(runtime);
								}
							}
							else
							{
								ImGui::TextWrapped("On the ReShade 'Home' tab, please enable 'IgcsDOF' and place it at the bottom of the list by dragging it there.");
							}
						}
						else
						{
							ImGui::TextWrapped("The camera is currently disabled so no depth-of-field session can be started");
						}
					}
					break;
				case DepthOfFieldControllerState::Setup:
					{
						if(cameraData->cameraEnabled)
						{
							ImGui::PushItemWidth(ImGui::GetWindowWidth() * 0.40f);
							ImGui::AlignTextToFramePadding();

							ImGui::SeparatorText("Focusing");
							float maxBokehSize = g_depthOfFieldController.getMaxBokehSize();
							bool changed = ImGui::DragFloat("Max. bokeh size", &maxBokehSize, 0.001f, 0.001f, 10.0f, "%.3f");
							if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
							{
								ImGui::SetTooltip("Use this value to define the maximum bokeh size.");
							}
							if(changed)
							{
								g_depthOfFieldController.setMaxBokehSize(runtime, maxBokehSize);
							}

							// Autofocus. The connected tool measures what is actually in
							// front of the lens and we convert that to a focus delta, so
							// the two images align on the subject without anyone dragging.
							bool autofocus = g_depthOfFieldController.getAutofocusEnabled();
							if(ImGui::Checkbox("Autofocus", &autofocus))
							{
								g_depthOfFieldController.setAutofocusEnabled(autofocus);
							}
							if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
							{
								ImGui::SetTooltip("Focus on whatever the focus point below is over, measured in the world.\nNeeds a connected tool that supports it; without one this does nothing.\n\nTurning it off leaves the focus where it was, so you can fine-tune by hand.");
							}

							if(autofocus)
							{
								float afPoint[2] = { g_depthOfFieldController.getAutofocusPointX(),
								                     g_depthOfFieldController.getAutofocusPointY() };
								if(ImGui::DragFloat2("Focus point", afPoint, 0.002f, 0.0f, 1.0f, "%.3f"))
								{
									g_depthOfFieldController.setAutofocusPoint(afPoint[0], afPoint[1]);
								}
								if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
								{
									ImGui::SetTooltip("Where in the frame to focus. 0,0 is the top left, 1,1 the bottom right,\nso 0.5, 0.5 is the centre.\n\nPut it on the part you want sharp - on a face rather than the middle\nof a body, or the plane lands somewhere behind the eyes.");
								}

								switch(g_depthOfFieldController.getAutofocusStatus())
								{
									case 0:
										ImGui::Text("Focused at %.2f m", g_depthOfFieldController.getAutofocusDistance());
										break;
									case 1:
										ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
											"Nothing under the focus point - holding %.2f m",
											g_depthOfFieldController.getAutofocusDistance());
										break;
									default:
										ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "No tool is measuring");
										break;
								}
							}

							float focusDelta = g_depthOfFieldController.getXFocusDelta();
							// Read-only while autofocus owns it: dragging would be
							// overwritten on the next measurement, which reads as the
							// slider being broken rather than as being driven.
							ImGui::BeginDisabled(autofocus);
							changed = ImGui::DragFloat("Focus delta X", &focusDelta, 0.00005f, -1.0f, 1.0f, "%.5f");
							ImGui::EndDisabled();
							if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
							{
								ImGui::SetTooltip("Use this value to align the two images\non the spot you want to have in focus");
							}
							if(changed && !autofocus)
							{
								g_depthOfFieldController.setXFocusDelta(runtime, focusDelta);
							}
							// Fast and the shutter cannot be combined - see
							// effectiveFrameWaitType. Shown as forced rather than
							// silently overridden, or the combo would say Fast while
							// Classic ran and the render time would make no sense.
							const bool shutterForcesClassic = g_depthOfFieldController.shutterInUse();
							int frameWaitType = (int)(shutterForcesClassic
								? DepthOfFieldFrameWaitType::Classic
								: g_depthOfFieldController.getFrameWaitType());

							ImGui::BeginDisabled(shutterForcesClassic);
							changed = ImGui::Combo("Frame wait type", &frameWaitType, "Fast\0Classic (slower)\0\0");
							ImGui::EndDisabled();
							if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
							{
								ImGui::SetTooltip("Fast is using a system which renders at full frame rate.\nYou need to pick the number of frames to wait value that gives a sharp focus area.\n\nClassic is slower, it uses the number of frames to wait to delay the next frame\nso the higher the value, the slower the rendering. Classic is more reliable to have sharp focus areas.");
							}
							if(changed && !shutterForcesClassic)
							{
								g_depthOfFieldController.setFrameWaitType((DepthOfFieldFrameWaitType)frameWaitType);
							}
							if(shutterForcesClassic)
							{
								ImGui::TextWrapped("Classic is required while the shutter is above 0: each sample is a "
								                   "different moment in time, and Fast grabs the frame before the game "
								                   "has moved to it, which smears the result instead of blurring it.");
							}
							std::string toolTipText = "";
							switch(frameWaitType)
							{
								case (int)DepthOfFieldFrameWaitType::Fast:
									toolTipText = "Use this value to define the blend delay.\nUsually 1 or 2. For engines with a long render pipeline you might to\nneed to increase this value. Increasing this value doesn't increase the render time.";
									break;
								case (int)DepthOfFieldFrameWaitType::Classic:
									toolTipText = "Use this value to specify a delay during blending a frame.\nUsually 1 or higher but if the engine uses a lot of temporal effects\nyou might need to increase this value.\nIncreasing this value will increase the render time.";

									break;
							}
							int numberOfFramesToWaitPerFrame = g_depthOfFieldController.getNumberOfFramesToWaitPerFrame();
							changed = intDragWithButtons("Number of frames to wait per frame", &numberOfFramesToWaitPerFrame, 1, 1, 20, toolTipText.c_str());
							if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
							{
								ImGui::SetTooltip(toolTipText.c_str());
							}
							if(changed)
							{
								g_depthOfFieldController.setNumberOfFramesToWaitPerFrame(numberOfFramesToWaitPerFrame);
							}

							// Shutter. Only offered when the connected camera tools can
							// actually step the clock - a control that silently does
							// nothing is worse than an absent one, and the tools that
							// can do this are the exception rather than the rule.
							if(g_depthOfFieldController.supportsShutter())
							{
								ImGui::SeparatorText("Shutter");
								float shutterMs = g_depthOfFieldController.getShutterMs();
								if(ImGui::DragFloat("Shutter (ms)", &shutterMs, 0.1f, 0.0f, 500.0f, "%.1f"))
								{
									g_depthOfFieldController.setShutterMs(shutterMs);
								}
								if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
								{
									ImGui::SetTooltip(
										"How much SCENE TIME this accumulation covers.\n\n"
										"0 freezes the clock: every sample is the same instant from a\n"
										"different point on the lens, so only the aperture varies. That is\n"
										"the classic behaviour and the right answer for a locked-off shot.\n\n"
										"Above 0 the camera tools advance the game's own clock per sample,\n"
										"so one pass produces depth of field AND motion blur. 16.7 is a\n"
										"1/60s exposure, 33.3 is 1/30s.\n\n"
										"Sample times are stratified across the interval and shuffled so\n"
										"they do not correlate with the aperture rings - without that a\n"
										"moving subject smears radially instead of evenly.\n\n"
										"Costs no extra samples. A long shutter over few samples is what\n"
										"discrete ghosting looks like, so raise quality with it.");
								}
								if(shutterMs > 0.0f)
								{
									const int steps = g_depthOfFieldController.getTotalNumberOfStepsToTake();
									ImGui::TextDisabled("%d samples over %.1f ms = %.2f ms apart",
										steps, shutterMs, steps > 0 ? shutterMs / (float)steps : 0.0f);
								}
							}

							ImGui::SeparatorText("Magnifier");
							auto& magnifierSettings = g_depthOfFieldController.getMagnifierSettings();
							ImGui::Checkbox("Show magnifier", &magnifierSettings.ShowMagnifier);
							ImGui::DragFloat("Magnification factor", &magnifierSettings.MagnificationFactor, 1.0f, 1.0f, 10.0f, "%.0f");

							float tempValues[2] = { magnifierSettings.WidthMagnifierArea, magnifierSettings.HeightMagnifierArea };
							changed = ImGui::DragFloat2("Magnifier area size", tempValues, 0.001f, 0.01f, 1.0f);
							if(changed)
							{
								magnifierSettings.WidthMagnifierArea = tempValues[0];
								magnifierSettings.HeightMagnifierArea = tempValues[1];
							}
							tempValues[0] = magnifierSettings.XMagnifierLocation;
							tempValues[1] = magnifierSettings.YMagnifierLocation;
							changed = ImGui::DragFloat2("Magnifier location", tempValues, 0.001f, 0.01f, 1.0f);
							if(changed)
							{
								magnifierSettings.XMagnifierLocation = tempValues[0];
								magnifierSettings.YMagnifierLocation = tempValues[1];
							}

							ImGui::SeparatorText("Bokeh setup");
							int blurType = (int)g_depthOfFieldController.getBlurType();
							changed = ImGui::Combo("Blur type", &blurType, "Aperture shaped\0Circular\0\0");
							if(changed)
							{
								g_depthOfFieldController.setBlurType((DepthOfFieldBlurType)blurType);
							}

							int quality = g_depthOfFieldController.getQuality();
							changed = intDragWithButtons("Quality", &quality, 1, 1, 100);
							if(changed)
							{
								g_depthOfFieldController.setQuality(quality);
							}
							switch((DepthOfFieldBlurType)blurType)
							{
								case DepthOfFieldBlurType::ApertureShape:
									{
										bool shapeSettingsChanged = false;
										auto& shapeSettings = g_depthOfFieldController.getApertureShapeSettings();
										shapeSettingsChanged |= intDragWithButtons("Number of vertices", &shapeSettings.NumberOfVertices, 1, 3, 10);
										shapeSettingsChanged |= ImGui::DragFloat("Rounding factor", &shapeSettings.RoundFactor, 0.001f, 0.0f, 1.0f);
										shapeSettingsChanged |= ImGui::DragFloat("Rotation angle", &shapeSettings.RotationAngle, 0.001f, 0.0f, 1.0f);		// multiplier to 2PI.

										if(shapeSettingsChanged)
										{
											g_depthOfFieldController.invalidateShapePoints();
										}
									}
									break;
								case DepthOfFieldBlurType::Circular:
									{
										int numberOfPointsInnermostCircle = g_depthOfFieldController.getNumberOfPointsInnermostRing();
										changed = intDragWithButtons("Number of points of innermost ring", &numberOfPointsInnermostCircle, 1, 1, 100);
										if(changed)
										{
											g_depthOfFieldController.setNumberOfPointsInnermostRing(numberOfPointsInnermostCircle);
										}
									}
									break;
							}
							float ringAngleOffset = g_depthOfFieldController.getRingAngleOffset();
							changed = ImGui::DragFloat("Ring angle offset", &ringAngleOffset, 0.001f, -0.015f, 0.015f);
							if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
							{
								ImGui::SetTooltip("This offset lets you rotate rings relative\nto each other to avoid the common grid pattern with lower\namount of rings.");
							}
							if(changed)
							{
								g_depthOfFieldController.setRingAngleOffset(ringAngleOffset);
							}
							float anamorphicFactor = g_depthOfFieldController.getAnamorphicFactor();
							changed = ImGui::DragFloat("Anamorphic factor", &anamorphicFactor, 0.001f, 0.01f, 1.0f);
							if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
							{
								ImGui::SetTooltip("Mainly meant for circular shapes\nAt 1.0 it gives perfect round bokehs,\n at a lower value it gives vertical ellipses.");
							}
							if(changed)
							{
								g_depthOfFieldController.setAnamorphicFactor(anamorphicFactor);
							}							

							float sphericalAberrationDimFactor = g_depthOfFieldController.getSphericalAberrationDimFactor();
							changed = ImGui::DragFloat("Spherical aberration dim factor", &sphericalAberrationDimFactor, 0.001f, 0.0f, 1.0f);
							if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
							{
								ImGui::SetTooltip("The factor for how much dimming center pixels in bokeh highlights will receive.");
							}
							if(changed)
							{
								g_depthOfFieldController.setSphericalAberrationDimFactor(sphericalAberrationDimFactor);
							}

							float fringeIntensity = g_depthOfFieldController.getFringeIntensity();
							changed = ImGui::DragFloat("Fringe intensity", &fringeIntensity, 0.001f, 0.0f, 1.0f);
							if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
							{
								ImGui::SetTooltip("Intensity of bokeh outline.\nUsing a value close to 1.0 could lead to having your screen go black\nduring the render phase. This is normal.");
							}
							if(changed)
							{
								g_depthOfFieldController.setFringeIntensity(fringeIntensity);
							}

							float fringeWidth = g_depthOfFieldController.getFringeWidth();
							changed = ImGui::DragFloat("Fringe width", &fringeWidth, 0.001f, 0.0f, 1.0f);
							if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
							{
								ImGui::SetTooltip("Width of bokeh outline");
							}
							if(changed)
							{
								g_depthOfFieldController.setFringeWidth(fringeWidth);
							}
							int caType = (int)g_depthOfFieldController.getCAType();
							changed = ImGui::Combo("Chromatic aberration type", &caType, "Red-Green-Blue\0Red-Green\0Red-Blue\0Blue-Green\0\0");
							if(changed)
							{
								g_depthOfFieldController.setCAType((DepthOfFieldCAType)caType);
							}
							float caStrength = g_depthOfFieldController.getCAStrength();
							changed = ImGui::DragFloat("Chromatic aberration strength", &caStrength, 0.000f, 0.0f, 1.0f);
							if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
							{
								ImGui::SetTooltip("The strength of the chromatic aberration on the edges of the bokeh highlights");
							}
							if(changed)
							{
								g_depthOfFieldController.setCAStrength(caStrength);
							}
							float caWidth = g_depthOfFieldController.getCAWidth();
							changed = ImGui::DragFloat("Chromatic aberration width", &caWidth, 0.001f, 0.0f, 1.0f);
							if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
							{
								ImGui::SetTooltip("Width of outer edge of the bokeh highlight\nto which chromatic aberration is applied.");
							}
							if(changed)
							{
								g_depthOfFieldController.setCAWidth(caWidth);
							}
							float catEyeBokehIntensity = g_depthOfFieldController.getCatEyeBokehIntensity();
							changed = ImGui::DragFloat("Cateye bokeh intensity", &catEyeBokehIntensity, 0.001f, -1.0f, 1.0f);
							if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
							{
								ImGui::SetTooltip("The intensity of the cat-eye effect in the bokeh.\nNegative values cut the bokeh shapes from the inside\nPositive values cut the bokeh shapes from the outside\nZero means no cateye bokeh.");
							}
							if(changed)
							{
								g_depthOfFieldController.setCatEyeBokehIntensity(catEyeBokehIntensity);
							}
							float catEyeRadiusStart = g_depthOfFieldController.getCatEyeRadiusStart();
							changed = ImGui::DragFloat("Cateye bokeh radius start", &catEyeRadiusStart, 0.001f, 0.0f, 1.0f);
							if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
							{
								ImGui::SetTooltip("The distance from the center of the screen where the cateye bokeh effect has to start.\nThis value has to be smaller than the Cateye bokeh radius end value.");
							}
							if(changed)
							{
								g_depthOfFieldController.setCatEyeRadiusStart(catEyeRadiusStart);
							}
							float catEyeRadiusEnd = g_depthOfFieldController.getCatEyeRadiusEnd();
							changed = ImGui::DragFloat("Cateye bokeh radius end", &catEyeRadiusEnd, 0.001f, 0.0f, 1.0f);
							if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
							{
								ImGui::SetTooltip("The distance from the center of the screen where the cateye bokeh effect has to end.\nThis value has to be bigger than the Cateye bokeh radius start value.");
							}
							if(changed)
							{
								g_depthOfFieldController.setCatEyeRadiusEnd(catEyeRadiusEnd);
							}
							bool addCatEyeVignette = g_depthOfFieldController.getAddCatEyeVignette();
							changed = ImGui::Checkbox("Add a vignette darkening to the cateye bokeh", &addCatEyeVignette);
							if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
							{
								ImGui::SetTooltip("If checked, a vignette is applied to the cateye bokeh effect.\nHas no effect if cateye bokeh intensity is (close to) 0.");
							}
							if(changed)
							{
								g_depthOfFieldController.setAddCatEyeVignette(addCatEyeVignette);
							}

							int renderOrder = (int)g_depthOfFieldController.getRenderOrder();
							changed = ImGui::Combo("Render order", &renderOrder, "Inner to outer ring\0Outer to inner ring\0Random\0\0");
							if(changed)
							{
								g_depthOfFieldController.setRenderOrder((DepthOfFieldRenderOrder)renderOrder);
							}

							float highlightBoostFactor = g_depthOfFieldController.getHighlightBoostFactor();
							changed = ImGui::DragFloat("Highlight boost factor", &highlightBoostFactor, 0.001f, 0.0f, 1.0f, "%.3f");
							if(changed)
							{
								g_depthOfFieldController.setHighlightBoostFactor(highlightBoostFactor);
							}
							float highlightGammaFactor = g_depthOfFieldController.getHighlightGammaFactor();
							changed = ImGui::DragFloat("Highlight gamma factor", &highlightGammaFactor, 0.001f, 0.1f, 5.0f, "%.3f");
							if(changed)
							{
								g_depthOfFieldController.setHighlightGammaFactor(highlightGammaFactor);
							}

							// show the shape canvas
							ImGui::Text("Blur shape. Number of shots to take: %d", g_depthOfFieldController.getTotalNumberOfStepsToTake());
							ImGui::InvisibleButton("canvas", ImVec2(250.0f, 250.0f), ImGuiButtonFlags_None);
							const ImVec2 topLeftCoords = ImGui::GetItemRectMin();
							const ImVec2 bottomRightCoords = ImGui::GetItemRectMax();
							ImDrawList* drawList = ImGui::GetWindowDrawList();
							drawList->AddRectFilled(topLeftCoords, bottomRightCoords, IM_COL32(50, 50, 50, 255));
							drawList->AddRect(topLeftCoords, bottomRightCoords, IM_COL32(255, 255, 255, 255));
							g_depthOfFieldController.drawShape(drawList, topLeftCoords, 250.0f);

							ImGui::Separator();
							bool showProgressBarAsOverlay = g_depthOfFieldController.getShowProgressBarAsOverlay();
							changed = ImGui::Checkbox("Show progress bar as overlay", &showProgressBarAsOverlay);
							if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
							{
								ImGui::SetTooltip("If checked, the progress bar will show in the top left corner\notherwise the progress bar will be shown here in the Reshade overlay.");
							}
							if(changed)
							{
								g_depthOfFieldController.setShowProgressBarAsOverlay(showProgressBarAsOverlay);
							}
							if(ImGui::Button("Start render"))
							{
								g_depthOfFieldController.startRender(runtime);
							}
							ImGui::SameLine();
							if(ImGui::Button("Cancel"))
							{
								g_depthOfFieldController.endSession(runtime);
							}
#if _DEBUG
							if(ImGui::CollapsingHeader("Debug"))
							{
								float debugVal1 = g_depthOfFieldController.getDebugVal1();
								changed = ImGui::DragFloat("Debug val1", &debugVal1, 0.001f, -2.0f, 2.0f, "%.3f");
								if(changed)
								{
									g_depthOfFieldController.setDebugVal1(debugVal1);
								}
								float debugVal2 = g_depthOfFieldController.getDebugVal2();
								changed = ImGui::DragFloat("Debug val2", &debugVal2, 0.001f, -20.0f, 20.0f, "%.3f");
								if(changed)
								{
									g_depthOfFieldController.setDebugVal2(debugVal2);
								}
								bool debugBool1 = g_depthOfFieldController.getDebugBool1();
								changed = ImGui::Checkbox("Debug bool 1", &debugBool1);
								if(changed)
								{
									g_depthOfFieldController.setDebugBool1(debugBool1);
								}
								bool debugBool2 = g_depthOfFieldController.getDebugBool2();
								changed = ImGui::Checkbox("Debug bool 2", &debugBool2);
								if(changed)
								{
									g_depthOfFieldController.setDebugBool2(debugBool2);
								}
							}
#endif
							ImGui::PopItemWidth();
						}
						else
						{
							g_depthOfFieldController.endSession(runtime);
						}
					}
					break;
				case DepthOfFieldControllerState::Rendering:
					{
						if(!g_depthOfFieldController.getShowProgressBarAsOverlay())
						{
							g_depthOfFieldController.renderProgressBar();
						}

						const bool isPaused = g_depthOfFieldController.getRenderPaused();
						if(isPaused)
						{
							ImGui::Text("Rendering (Paused), please wait...");
							if(ImGui::Button("Unpause rendering"))
							{
								g_depthOfFieldController.setRenderPaused(false);
							}
						}
						else
						{
							ImGui::Text("Rendering, please wait...");
							if(ImGui::Button("Pause rendering"))
							{
								g_depthOfFieldController.setRenderPaused(true);
							}
						}
						ImGui::SameLine();
						if(ImGui::Button("Cancel"))
						{
							g_depthOfFieldController.endSession(runtime);
						}
					}
					break;
				case DepthOfFieldControllerState::Done:
					ImGui::Text("Done. You can now take a screenshot.\n\n");
					ImGui::Text("Click 'End session' to end this session.\nThis will remove the rendering result.");
					if(ImGui::Button("End session"))
					{
						g_depthOfFieldController.endSession(runtime);
						saveIniFile();
					}
					break;
				case DepthOfFieldControllerState::Cancelling:
					ImGui::Text("Cancelling session...");
					break;
			}
		}
	}

	ImGui::AlignTextToFramePadding();
	if(ImGui::CollapsingHeader("Camera tools info"))
	{
		if(nullptr == g_dataFromCameraToolsBuffer)
		{
			ImGui::Text("Camera data not available");
		}
		else
		{
			// display camera info
			std::ostringstream stringStream;
			stringStream << std::fixed << std::setprecision(2) << cameraData->fov;
			const std::string fovAsString = stringStream.str();
			ImGui::Text(cameraData->cameraEnabled ? "Camera enabled" : "Camera disabled");
			ImGui::Text(cameraData->cameraMovementLocked ? "Camera movement locked" : "Camera movement unlocked");
			ImGui::InputText("FoV (degrees)", (char*)fovAsString.c_str(), fovAsString.length(), ImGuiInputTextFlags_ReadOnly);
			ImGui::InputFloat3("Camera coordinates", cameraData->coordinates.values, "%.4f", ImGuiInputTextFlags_ReadOnly);
			ImGui::InputFloat4("Camera look quaternion", cameraData->lookQuaternion.values, "%.3f", ImGuiInputTextFlags_ReadOnly);
			ImGui::InputFloat3("Rotation matrix Right", cameraData->rotationMatrixRightVector.values, "%.3f", ImGuiInputTextFlags_ReadOnly);
			ImGui::InputFloat3("Rotation matrix Up", cameraData->rotationMatrixUpVector.values, "%.3f", ImGuiInputTextFlags_ReadOnly);
			ImGui::InputFloat3("Rotation matrix Forward", cameraData->rotationMatrixForwardVector.values, "%.3f", ImGuiInputTextFlags_ReadOnly);
			ImGui::InputFloat("Pitch (radians)", &cameraData->pitch, ImGuiInputTextFlags_ReadOnly);
			ImGui::InputFloat("Yaw (radians)", &cameraData->yaw, ImGuiInputTextFlags_ReadOnly);
			ImGui::InputFloat("Roll (radians)", &cameraData->roll, ImGuiInputTextFlags_ReadOnly);
		}
	}
	ImGui::AlignTextToFramePadding();
	if(ImGui::CollapsingHeader("Camera path info"))
	{
		if(nullptr==g_dataFromCameraToolsBuffer)
		{
			ImGui::Text("Camera path info not available");
		}
		else
		{
			ImGui::Checkbox("Record ReShade state with camera nodes", &g_recordReshadeState);
			ImGui::Text("Number of saved ReShade states per path:");

			const auto numberOfPaths = g_reshadeStateController.numberOfPaths();
			if(numberOfPaths<=0)
			{
				ImGui::Text("None.");
			}
			else
			{
				for(int i = 0; i < numberOfPaths; i++)
				{
					// path no's are starting at 0 but for display purposes we start at 1.
					ImGui::Text("Path: %d. # of saved Reshade states: %d.", (i + 1), g_reshadeStateController.numberOfSnapshotsOnPath(i));
				}
			}
		}
	}
}


void sendCameraToolsDataToUniforms(effect_runtime* runtime)
{
	// the following source variables are defined with their types:
	// IGCS_cameraDataAvailable			bool
	// IGCS_cameraEnabled				bool
	// IGCS_cameraMovementLocked		bool
	// IGCS_cameraFoV					float		degrees
	// IGCS_cameraWorldPosition			float3
	// IGCS_cameraOrientation			float4		quaternion (x,y,z,w)
	// IGCS_cameraViewMatrix4x4			float4x4
	// IGCS_cameraProjectionMatrix4x4LH	float4x4	calculated from fov + aspect ratio + near of 0.1 and far of 10000.0, using left handed row major DirectX math
	// IGCS_cameraUp					float3		up vector of 3x3 part of view matrix
	// IGCS_cameraRight					float3		right vector of 3x3 part of view matrix
 	// IGCS_cameraForward				float3		forward vector of 3x3 part of view matrix
	// IGCS_cameraRotationPitch			float		radians
	// IGCS_cameraRotationYaw			float		radians
	// IGCS_cameraRotationRoll			float		radians
	const auto cameraData = (CameraToolsData*)g_dataFromCameraToolsBuffer;
	if(nullptr==cameraData)
	{
		runtime->enumerate_uniform_variables(nullptr, [](effect_runtime* runtime, effect_uniform_variable variable)
		{
			char source[32];
			if(runtime->get_annotation_string_from_uniform_variable(variable, "source", source) && std::strcmp(source, "IGCS_cameraDataAvailable") == 0)
			{
				runtime->set_uniform_value_bool(variable, false);
			}
		});
		return;
	}

	runtime->enumerate_uniform_variables(nullptr, [&cameraData](effect_runtime *runtime, effect_uniform_variable variable) 
	{
		char source[64];
		runtime->get_annotation_string_from_uniform_variable(variable, "source", source);
		if(std::strcmp(source, "IGCS_cameraDataAvailable") == 0)
		{
			runtime->set_uniform_value_bool(variable, true);
		}
		else if(std::strcmp(source, "IGCS_cameraEnabled") == 0)
		{
			runtime->set_uniform_value_bool(variable, cameraData->cameraEnabled);
		}
		else if(std::strcmp(source, "IGCS_cameraMovementLocked") == 0)
		{
			runtime->set_uniform_value_bool(variable, cameraData->cameraMovementLocked);
		}
		else if(std::strcmp(source, "IGCS_cameraFoV") == 0)
		{
			runtime->set_uniform_value_float(variable, cameraData->fov);
		}
		else if(std::strcmp(source, "IGCS_cameraWorldPosition") == 0)
		{
			runtime->set_uniform_value_float(variable, cameraData->coordinates.x(), cameraData->coordinates.y(), cameraData->coordinates.z());
		}
		else if(std::strcmp(source, "IGCS_cameraOrientation") == 0)
		{
			runtime->set_uniform_value_float(variable, cameraData->lookQuaternion.x(), cameraData->lookQuaternion.y(), cameraData->lookQuaternion.z(), cameraData->lookQuaternion.w());
		}
		else if(std::strcmp(source, "IGCS_cameraViewMatrix4x4") == 0)
		{
			const auto matrixAsFlatVector = cameraData->lookQuaternion.toFlatVector();
			runtime->set_uniform_value_float(variable, &matrixAsFlatVector[0], 16, 0);
		}
		else if(std::strcmp(source, "IGCS_cameraProjectionMatrix4x4LH") == 0)
		{
			const auto device = runtime->get_device();
			if(nullptr!=device && cameraData->fov>0)
			{
				const auto currentBackBuffer = runtime->get_current_back_buffer();
				if(currentBackBuffer.handle>0)
				{
					const auto description = device->get_resource_desc(currentBackBuffer);
					const auto width = static_cast<float>(description.texture.width);
					auto height = static_cast<float>(description.texture.height);
					if(height<DirectX::g_XMEpsilon.f[0])
					{
						height = 0.1;
					}
					const auto aspectRatio = width / height;
					const auto projectionMatrixLH = DirectX::XMMatrixPerspectiveFovLH(IGCS::Utils::degreesToRadians(cameraData->fov), aspectRatio, 0.1f, 10000.0f);
					const auto projectionMatrixAsFlatVector = IGCS::Utils::XMFloat4x4ToFlatVector(projectionMatrixLH);
					runtime->set_uniform_value_float(variable, &projectionMatrixAsFlatVector[0], 16, 0);
				}
			}
		}
		else if(std::strcmp(source, "IGCS_cameraRotationPitch") == 0)
		{
			runtime->set_uniform_value_float(variable, cameraData->pitch);
		}
		else if(std::strcmp(source, "IGCS_cameraRotationYaw") == 0)
		{
			runtime->set_uniform_value_float(variable, cameraData->yaw);
		}
		else if(std::strcmp(source, "IGCS_cameraRotationRoll") == 0)
		{
			runtime->set_uniform_value_float(variable, cameraData->roll);
		}
		else if(std::strcmp(source, "IGCS_cameraUp") == 0)
		{
			runtime->set_uniform_value_float(variable, cameraData->rotationMatrixUpVector.x(), cameraData->rotationMatrixUpVector.y(), cameraData->rotationMatrixUpVector.z());
		}
		else if(std::strcmp(source, "IGCS_cameraRight") == 0)
		{
			runtime->set_uniform_value_float(variable, cameraData->rotationMatrixRightVector.x(), cameraData->rotationMatrixRightVector.y(), cameraData->rotationMatrixRightVector.z());
		}
		else if(std::strcmp(source, "IGCS_cameraForward") == 0)
		{
			runtime->set_uniform_value_float(variable, cameraData->rotationMatrixForwardVector.x(), cameraData->rotationMatrixForwardVector.y(), cameraData->rotationMatrixForwardVector.z());
		}
		// add more here. 
	});
}


void onReshadeReloadEffects(effect_runtime* runtime)
{
	// This call can be made in various scenarios, but they have either one of 2 characteristics: 1) there are 0 effects or 2) there are effects but they're changing.
	// We can safely ignore the first one, as that's the one originating from the call to destroy_effects. All the other scenarios are from update_effects which is
	// called in on_present and will end up raising the event in multiple scenarios.
	g_reshadeStateController.migrateContainedHandles(runtime);
	g_depthOfFieldController.migrateReshadeState(runtime);
}

void onReshadeBeginEffects(effect_runtime* runtime, command_list* cmd_list, resource_view rtv, resource_view rtv_srgb)
{
	sendCameraToolsDataToUniforms(runtime);
	g_depthOfFieldController.reshadeBeginEffectsCalled(runtime);
}


void onReshadeFinishEffects(effect_runtime* runtime, command_list* cmd_list, resource_view rtv, resource_view rtv_srgb)
{
	g_depthOfFieldController.reshadeFinishEffectsCalled(runtime);
}


BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		if (!reshade::register_addon(hModule))
		{
			return FALSE;
		}
		reshade::register_event<reshade::addon_event::reshade_present>(onReshadePresent);
		reshade::register_event<reshade::addon_event::reshade_overlay>(onReshadeOverlay);
		reshade::register_event<reshade::addon_event::reshade_begin_effects>(onReshadeBeginEffects);
		reshade::register_event<reshade::addon_event::reshade_finish_effects>(onReshadeFinishEffects);
		reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(onReshadeReloadEffects);
		reshade::register_overlay(nullptr, &displaySettings);
		loadIniFile();
		break;
	case DLL_PROCESS_DETACH:
		reshade::unregister_event<reshade::addon_event::reshade_present>(onReshadePresent);
		reshade::unregister_event<reshade::addon_event::reshade_overlay>(onReshadeOverlay);
		reshade::unregister_event<reshade::addon_event::reshade_begin_effects>(onReshadeBeginEffects);
		reshade::unregister_event<reshade::addon_event::reshade_finish_effects>(onReshadeFinishEffects);
		reshade::unregister_event<reshade::addon_event::reshade_reloaded_effects>(onReshadeReloadEffects);
		reshade::unregister_overlay(nullptr, &displaySettings);
		reshade::unregister_addon(hModule);
		if(nullptr!=g_dataFromCameraToolsBuffer)
		{
			free(g_dataFromCameraToolsBuffer);
		}
		saveIniFile();
		break;
	}

	return TRUE;
}
