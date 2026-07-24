/*
        GTA V Free Camera / Photo Mode Plugin
        Shared menu infrastructure.

        The in-game UI now lives in scmenu.cpp (on the gtam framework). What
        remains here is the shared, non-UI plumbing that scmenu / script / the
        camera & sequence systems still rely on: INI settings load/save/reset,
        the on-screen status-text helper, the menu hotkey + controller combos,
        and the offline "Render to Images" worker (ProcessRenderToImages) plus
        its render-settings globals.
*/

#pragma once

#include "external\scripthook_sdk\inc\enums.h"
#include "external\scripthook_sdk\inc\main.h"
#include "external\scripthook_sdk\inc\natives.h"
#include "external\scripthook_sdk\inc\types.h"

#include <string>

// Settings
void LoadSettings();
void SaveSettings();
void ResetSettingsToDefaults(); // restore every tunable to its factory default

// Menu toggle key
bool IsMenuTogglePressed();
// Clear the menu key's edge state so the same release isn't acted on twice.
void ConsumeMenuToggle();

// Controller combos (no keyboard on a pad). Edge-triggered, poll once per frame.
bool IsControllerMenuCombo(); // LB + RB  -> open the menu
bool IsControllerExitCombo(); // LB + B   -> exit Free Camera

void MenuBeep();
void SetStatusText(std::string str, DWORD time = 2500);
void UpdateStatusText();

// ---- Render-to-image settings (shared with the new framework render menu) ----
extern float g_RenderFps;
extern int g_RenderFlushFrames;
extern int g_RenderBlurSamples;
extern int g_RenderFormat;
extern int g_RenderJpegQuality;
extern float g_RenderHighlightBoost;
extern int g_RenderChannelOrder;
// Optional render time range (seconds). Render only [start, end] of the
// sequence; end <= 0 (or <= start) means "to the end". Lets you re-render just a
// section. Frame numbering still starts at 0.
extern float g_RenderRangeStart;
extern float g_RenderRangeEnd;

// Run the deterministic offline render (blocks until the sequence is written or
// cancelled). Needs the ReShade + IgcsConnector capture addon.
void ProcessRenderToImages();
