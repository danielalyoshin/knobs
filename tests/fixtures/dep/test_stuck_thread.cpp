// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>

#include <atomic>

namespace {
std::atomic<DWORD> stuck_thread = 0;
}

extern "C" __declspec(dllexport) void BlockThisThreadOnExit() {
  stuck_thread = GetCurrentThreadId();
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, void*) {
  if (reason == DLL_THREAD_DETACH && GetCurrentThreadId() == stuck_thread) Sleep(INFINITE);
  return TRUE;
}
