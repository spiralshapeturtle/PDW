// debuglog.cpp — Live debug log for PDW.
//
// DebugLog() is the legacy entry point called throughout Flex.cpp, Misc.cpp, etc.
// It now delegates to LogManager (LC_DEBUG), which handles buffering, file paths,
// and thread safety centrally.
//
// DebugLogInit / DebugLogShutdown keep the same signature so PDW.cpp does not
// need changes.  The internal CRITICAL_SECTION is retained only for the cycle
// tracker (DebugLogNotifyFrameChange / DebugLogGetCycle), which is unrelated to I/O.

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

#include "../Headers/pdw.h"
#include "debuglog.h"
#include "logmanager.h"

extern PROFILE Profile;

static CRITICAL_SECTION g_cycleCs;
static BOOL             g_cycleInitialized = FALSE;

static int s_lastFrame    = -1;
static int s_currentCycle = 0;

void DebugLogInit(void)
{
    if (g_cycleInitialized) return;
    InitializeCriticalSection(&g_cycleCs);
    g_cycleInitialized = TRUE;
}

void DebugLogShutdown(void)
{
    if (!g_cycleInitialized) return;
    DeleteCriticalSection(&g_cycleCs);
    g_cycleInitialized = FALSE;
}

void DebugLogNotifyFrameChange(int currentFrame)
{
    if (!g_cycleInitialized) return;
    if (currentFrame < 0 || currentFrame > 127) return;

    EnterCriticalSection(&g_cycleCs);
    if (s_lastFrame >= 0 && currentFrame < s_lastFrame && (s_lastFrame - currentFrame) > 64)
        s_currentCycle = (s_currentCycle + 1) % 15;
    s_lastFrame = currentFrame;
    LeaveCriticalSection(&g_cycleCs);
}

int DebugLogGetCycle(void)
{
    int cycle;
    if (!g_cycleInitialized) return 0;
    EnterCriticalSection(&g_cycleCs);
    cycle = s_currentCycle;
    LeaveCriticalSection(&g_cycleCs);
    return cycle;
}

void DebugLog(const char *fmt, ...)
{
    if (!Profile.bDebugLog) return;

    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);

    LogManager::Get().Write(LC_DEBUG, "%s", msg);
}
