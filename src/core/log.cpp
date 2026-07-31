/*
        GTA V Free Camera / Photo Mode Plugin
        Minimal append-only log — see log.h.
*/

#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

// Same trick fx_capture / LoadSettings use: resolve the folder from an address
// inside THIS module, not from the process, so the log lands beside the .asi
// rather than beside the game (which is a different folder under FiveM).
void ModuleDir(char *out, size_t cap) {
  HMODULE hMod = nullptr;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     (LPCSTR)&ModuleDir, &hMod);
  out[0] = '\0';
  GetModuleFileNameA(hMod, out, (DWORD)cap);
  char *last = strrchr(out, '\\');
  if (last) *last = '\0';
}

bool  s_ready = false;
char  s_path[MAX_PATH]{};

// One process-wide lock. The scan runs on the script fiber and the capture
// bridge on another thread, and two interleaved fprintf's produce a line that
// reads as corruption rather than as two events.
CRITICAL_SECTION s_lock;
bool             s_lockReady = false;

} // namespace

void Log(const char *fmt, ...) {
  if (!s_lockReady) {
    // Racy in principle on the very first call from two threads at once; the
    // worst case is a second initialise of a critical section, which is
    // harmless here and cheaper than a static initialiser that would run
    // under the loader lock.
    InitializeCriticalSection(&s_lock);
    s_lockReady = true;
  }
  EnterCriticalSection(&s_lock);

  if (!s_ready) {
    char dir[MAX_PATH];
    ModuleDir(dir, sizeof(dir));
    snprintf(s_path, sizeof(s_path), "%s\\SimpleCamera.log", dir);

    // Truncate once per process, so a log is always the CURRENT run. An
    // appended one from five launches ago is worse than none when the
    // question is "what happened this time".
    FILE *f = nullptr;
    if (fopen_s(&f, s_path, "w") == 0 && f) {
      SYSTEMTIME st;
      GetLocalTime(&st);
      fprintf(f, "SimpleCamera log - %04d-%02d-%02d %02d:%02d:%02d\n",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
      fclose(f);
    }
    s_ready = true;
  }

  FILE *f = nullptr;
  if (fopen_s(&f, s_path, "a") == 0 && f) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    // Closed rather than kept open: the process may be about to die, and a
    // buffered line that never reaches disk is exactly the line worth having.
    fclose(f);
  }

  LeaveCriticalSection(&s_lock);
}
