// debuglog.cpp — file-based live debug log for PDW.
//
// When Profile.bDebugLog is enabled, DebugLog() appends timestamped lines to
// pdw_debug.log in Profile.LogfilePath (fallback: szPath). Designed for live
// monitoring with `tail -f` — fflush() after every write.
//
// Thread-safe via critical section (FLEX/POCSAG decoders run on the sound thread,
// menu toggle runs on main thread).

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

#include "../Headers/pdw.h"
#include "debuglog.h"

extern PROFILE Profile;
extern char    szPath[];

static CRITICAL_SECTION g_dbgCs;
static BOOL             g_dbgInitialized = FALSE;

// Cycle tracking — PDW does not maintain iCurrentCycle natively. We detect
// wraparound (large backward jump in frame number) and increment a local cycle
// counter. The absolute cycle value is meaningless across PDW restarts; only
// transitions are useful for log correlation.
static int s_lastFrame    = -1;
static int s_currentCycle = 0;

static const char *kDayAbbr[7] = { "Zo", "Ma", "Di", "Wo", "Do", "Vr", "Za" };

void DebugLogInit(void)
{
	if (g_dbgInitialized) return;
	InitializeCriticalSection(&g_dbgCs);
	g_dbgInitialized = TRUE;
}

void DebugLogShutdown(void)
{
	if (!g_dbgInitialized) return;
	DeleteCriticalSection(&g_dbgCs);
	g_dbgInitialized = FALSE;
}

void DebugLogNotifyFrameChange(int currentFrame)
{
	if (!g_dbgInitialized) return;
	if (currentFrame < 0 || currentFrame > 127) return;

	EnterCriticalSection(&g_dbgCs);
	if (s_lastFrame >= 0 && currentFrame < s_lastFrame && (s_lastFrame - currentFrame) > 64)
	{
		s_currentCycle = (s_currentCycle + 1) % 15;
	}
	s_lastFrame = currentFrame;
	LeaveCriticalSection(&g_dbgCs);
}

int DebugLogGetCycle(void)
{
	int cycle;
	if (!g_dbgInitialized) return 0;
	EnterCriticalSection(&g_dbgCs);
	cycle = s_currentCycle;
	LeaveCriticalSection(&g_dbgCs);
	return cycle;
}

static const char *kMonthAbbr[12] = {
	"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
	"JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
};

void DebugLog(const char *fmt, ...)
{
	if (!Profile.bDebugLog) return;
	if (!g_dbgInitialized)  return;

	char szPathLog[MAX_PATH];
	const char *root = (Profile.LogfilePath[0]) ? Profile.LogfilePath : szPath;
	if (!root || !root[0]) return;

	EnterCriticalSection(&g_dbgCs);

	SYSTEMTIME st;
	GetLocalTime(&st);

	// Daily-rotated filename — date prefix for chronological sorting in Explorer.
	if (Profile.MonthNumber)
	{
		_snprintf(szPathLog, sizeof(szPathLog) - 1, "%s\\%02d%02d%02d_pdw_debug.log",
			root, st.wYear % 100, st.wMonth, st.wDay);
	}
	else
	{
		const char *mon = (st.wMonth >= 1 && st.wMonth <= 12) ? kMonthAbbr[st.wMonth - 1] : "???";
		_snprintf(szPathLog, sizeof(szPathLog) - 1, "%s\\%02d%s%02d_pdw_debug.log",
			root, st.wYear % 100, mon, st.wDay);
	}
	szPathLog[sizeof(szPathLog) - 1] = '\0';

	FILE *f = fopen(szPathLog, "a");
	if (f)
	{
		const char *day = (st.wDayOfWeek <= 6) ? kDayAbbr[st.wDayOfWeek] : "??";

		fprintf(f, "%04d-%02d-%02d %s %02d:%02d:%02d | ",
			st.wYear, st.wMonth, st.wDay, day,
			st.wHour, st.wMinute, st.wSecond);

		va_list args;
		va_start(args, fmt);
		vfprintf(f, fmt, args);
		va_end(args);

		fputc('\n', f);
		fflush(f);
		fclose(f);
	}

	LeaveCriticalSection(&g_dbgCs);
}
