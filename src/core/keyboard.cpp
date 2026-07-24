/*
        GTA V Free Camera / Photo Mode Plugin
        Keyboard Input Handler
*/

#include "keyboard.h"
#include "TextInput.h" // forward keystrokes to the menu's inline value editor

const int KEYS_SIZE = 256; // virtual-key codes span 0x00..0xFF (256 values)

struct {
  DWORD time;
  BOOL isWithAlt;
  BOOL wasDownBefore;
  BOOL isUpNow;
} keyStates[KEYS_SIZE];

void OnKeyboardMessage(DWORD key, WORD repeats, BYTE scanCode, BOOL isExtended,
                       BOOL isWithAlt, BOOL wasDownBefore, BOOL isUpNow) {
  // Route to the menu's focused text field (no-op unless one is focused), so
  // you can type exact values in-menu without the game's on-screen keyboard.
  gtam::TextInput::FeedGlobal(key, scanCode, isExtended, isUpNow, isWithAlt);

  if (key < KEYS_SIZE) {
    keyStates[key].time = GetTickCount();
    keyStates[key].isWithAlt = isWithAlt;
    keyStates[key].wasDownBefore = wasDownBefore;
    keyStates[key].isUpNow = isUpNow;
  }
}

const int NOW_PERIOD = 100, MAX_DOWN = 5000; // ms

bool IsKeyDown(DWORD key) {
  return (key < KEYS_SIZE)
             ? ((GetTickCount() < keyStates[key].time + MAX_DOWN) &&
                !keyStates[key].isUpNow)
             : false;
}

bool IsKeyJustUp(DWORD key, bool exclusive) {
  bool b = (key < KEYS_SIZE)
               ? (GetTickCount() < keyStates[key].time + NOW_PERIOD &&
                  keyStates[key].isUpNow)
               : false;
  if (b && exclusive)
    ResetKeyState(key);
  return b;
}

void ResetKeyState(DWORD key) {
  if (key < KEYS_SIZE)
    memset(&keyStates[key], 0, sizeof(keyStates[0]));
}
