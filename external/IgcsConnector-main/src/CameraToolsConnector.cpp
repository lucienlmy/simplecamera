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
#include "stdafx.h"
#include "CameraToolsConnector.h"
#include "OverlayControl.h"
#include <cstring>
#include <cstdio>


namespace
{
	// Tools that export the IGCS interface as a SIDE FEATURE.
	//
	// Not a blacklist - a tie-break. Several mods export these entry points, and
	// the scan below used to take whichever the loader happened to place first.
	// When that landed on one of these, a depth-of-field session drove ITS
	// camera while a different tool was asking for the frames: sessions started,
	// passes completed, and every frame came out identical because the camera
	// never moved where the accumulator thought it had. Nothing reported an
	// error, because every step that gets checked did work.
	//
	// So one of these is used only when it is the ONLY tool present, which keeps
	// it working exactly as before for anyone running it on its own.
	bool isSecondaryCameraTool(HMODULE moduleHandle)
	{
		char path[MAX_PATH]{};
		if(0 == GetModuleFileNameA(moduleHandle, path, MAX_PATH))
		{
			return false;
		}
		const char* name = strrchr(path, '\\');
		name = name ? name + 1 : path;

		static const char* const secondary[] = { "NVE.asi" };
		for(const char* s : secondary)
		{
			if(0 == _stricmp(name, s))
			{
				return true;
			}
		}
		return false;
	}
}


void CameraToolsConnector::connectToCameraTools()
{
	// Enumerate all modules in the process and check if they export a defined function (IGCS_StartScreenshotSession). If so, we map to the known functions.
	const HANDLE processHandle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, GetCurrentProcessId());
	if(nullptr == processHandle)
	{
		return;
	}
	HMODULE modules[512];
	DWORD cbNeeded;
	if(EnumProcessModules(processHandle, modules, sizeof(modules), &cbNeeded))
	{
		// Two passes rather than first-past-the-post - see isSecondaryCameraTool.
		HMODULE preferred = nullptr;
		HMODULE fallback  = nullptr;

		for(int i = 0; i < cbNeeded / sizeof(HMODULE); i++)
		{
			const HMODULE moduleHandle = modules[i];
			if(nullptr == GetProcAddress(moduleHandle, "IGCS_StartScreenshotSession"))
			{
				// doesn't export it, not an IGCS camera tools dll
				continue;
			}
			if(isSecondaryCameraTool(moduleHandle))
			{
				if(nullptr == fallback) fallback = moduleHandle;
				continue;
			}
			if(nullptr == preferred) preferred = moduleHandle;
		}

		const HMODULE chosen = preferred ? preferred : fallback;
		if(nullptr != chosen)
		{
			connectToModule(chosen);

			// Said out loud, because "which tool is driving" is invisible
			// otherwise and is exactly what goes wrong in a modded game.
			char path[MAX_PATH]{};
			GetModuleFileNameA(chosen, path, MAX_PATH);
			char msg[MAX_PATH + 96];
			snprintf(msg, sizeof(msg), "Camera tools bound to %s%s", path,
				(preferred && fallback) ? " (another tool also offers the interface)" : "");
			OverlayControl::addNotification(msg);
		}
	}
	CloseHandle(processHandle);
	OverlayControl::addNotification(cameraToolsConnected() ? "Camera tools connected" : "No camera tools found");
}


bool CameraToolsConnector::connectToModule(HMODULE moduleHandle)
{
	if(nullptr == moduleHandle)
	{
		return false;
	}
	const auto start = (IGCS_StartScreenshotSession)GetProcAddress(moduleHandle, "IGCS_StartScreenshotSession");
	if(nullptr == start)
	{
		return false;
	}

	_igcs_StartScreenshotSessionFunc   = start;
	_igcs_EndScreenshotSessionFunc     = (IGCS_EndScreenshotSession)GetProcAddress(moduleHandle, "IGCS_EndScreenshotSession");
	_igcs_MoveCameraPanoramaFunc       = (IGCS_MoveCameraPanorama)GetProcAddress(moduleHandle, "IGCS_MoveCameraPanorama");
	_igcs_MoveCameraMultishotFunc      = (IGCS_MoveCameraMultishot)GetProcAddress(moduleHandle, "IGCS_MoveCameraMultishot");
	_igcs_MoveCameraMultishotTimedFunc = (IGCS_MoveCameraMultishotTimed)GetProcAddress(moduleHandle, "IGCS_MoveCameraMultishotTimed");
	_igcs_QuerySampleReadyFunc         = (IGCS_QuerySampleReady)GetProcAddress(moduleHandle, "IGCS_QuerySampleReady");
	return true;
}


ScreenshotSessionStartReturnCode CameraToolsConnector::startScreenshotSession(uint8_t type)
{
	if(!cameraToolsConnected())
	{
		return ScreenshotSessionStartReturnCode::Error_CameraFeatureNotAvailable;
	}
	return _igcs_StartScreenshotSessionFunc(type);
}


void CameraToolsConnector::moveCameraPanorama(float stepAngle)
{
	if(!cameraToolsConnected())
	{
		return;
	}
	_igcs_MoveCameraPanoramaFunc(stepAngle);
}


void CameraToolsConnector::moveCameraMultishot(float stepLeftRight, float stepUpDown, float fovDegrees, bool fromStartPosition)
{
	if(!cameraToolsConnected())
	{
		return;
	}
	_igcs_MoveCameraMultishotFunc(stepLeftRight, stepUpDown, fovDegrees, fromStartPosition);
}


void CameraToolsConnector::moveCameraMultishotTimed(float stepLeftRight, float stepUpDown, float fovDegrees, bool fromStartPosition, float timeOffsetMs)
{
	if(!cameraToolsConnected())
	{
		return;
	}
	if(nullptr == _igcs_MoveCameraMultishotTimedFunc)
	{
		// Tools can't step time. Take the plain step so the aperture walk still
		// happens - a session without the shutter is the old behaviour, which is
		// a perfectly good result rather than a failure.
		_igcs_MoveCameraMultishotFunc(stepLeftRight, stepUpDown, fovDegrees, fromStartPosition);
		return;
	}
	_igcs_MoveCameraMultishotTimedFunc(stepLeftRight, stepUpDown, fovDegrees, fromStartPosition, timeOffsetMs);
}


bool CameraToolsConnector::querySampleReady()
{
	if(nullptr == _igcs_QuerySampleReadyFunc)
	{
		// No query available: never make the caller wait on an answer nobody is
		// going to give. The frame-wait counter alone then governs, exactly as
		// it did before this extension existed.
		return true;
	}
	return 0 != _igcs_QuerySampleReadyFunc();
}


void CameraToolsConnector::endScreenshotSession()
{
	if(!cameraToolsConnected())
	{
		return;
	}
	_igcs_EndScreenshotSessionFunc();
}

