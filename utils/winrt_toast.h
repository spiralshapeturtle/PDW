// utils/winrt_toast.h
// FIX [WinRTToast]: WinRT Toast API — PDW-notificaties via Action Center (Windows 8+)
//   g_bWinRTAvail = false als WinRT niet beschikbaar is (pre-Win8, DLL-fout, init-fout)
//   Caller gebruikt bestaande balloon-fallback wanneer g_bWinRTAvail == false.
#pragma once

extern bool g_bWinRTAvail;

// Laad runtimeobject.dll dynamisch en registreer AUMID in HKCU.
// Retourneert true als WinRT Toast beschikbaar is; stelt g_bWinRTAvail in.
// Aanroepen eenmalig vanuit WinMain vóór InitInstance.
bool WinRTToastInit();

// Toont een WinRT toast-notificatie. Doet niets als g_bWinRTAvail == false.
// title en body zijn ANSI (CP_ACP); worden intern naar wide geconverteerd.
void WinRTToastNotify(const char* title, const char* body);
