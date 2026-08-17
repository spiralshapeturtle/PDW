// HealthPanel.cpp -- compact "Health" status panel on the toolbar band
// FIX [HealthPanel]: see HealthPanel.h for the design overview.

#ifndef STRICT
#define STRICT 1
#endif

#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>

#include "Headers/resource.h"
#include "Headers/pdw.h"
#include "Headers/gfx.h"
#include "Headers/initapp.h"
#include "Headers/sigind.h"		// FIX [HealthPanelCorner]: DrawSigInd for the classic-corner swap-back
#include "HealthPanel.h"
#include "HealthSource.h"
#include "RxQualMonitor.h"
#include "utils/feedstatus.h"
#include "utils/logmanager.h"	// FIX [FeedTransitionLog]: PDW_HEALTHLOG for COM link transitions
#include "Headers/slicer.h"		// SLICER_IN_STR/SLICER_OUT_STR types used by rs232.h prototypes
#include "utils/rs232.h"

extern PROFILE Profile;

/* ---------------------------------------------------------------------------
** Trend history: one sample per second, sampled by HealthPanel_OnSecond()
** (SECOND_TIMER, GUI thread - single-threaded access only). Ring sized for
** the maximum trend window. Negative = "no measurement" gap.
** FIX [HealthSparkLong]: ring grown from 3600 (1 h) to 28800 (8 h) so the trend
** can look back over a full night. Static float[] = 28800*4 = 112 KB, GUI-thread
** only. Overnight dips are preserved by the min/max band in DrawSparkline (a plain
** per-column average washed short drops out; the band keeps the worst case visible).
** ---------------------------------------------------------------------------*/
#define HP_HIST_MAX   28800
#define HP_SPARK_GAP  -1.0f

static float s_hist[HP_HIST_MAX];
static int   s_histHead  = 0;      /* next write position */
static int   s_histCount = 0;

/* Cached GDI objects (GUI thread only). Recreated when DPI/display changes. */
static HDC     s_hdcMem   = NULL;
static HBITMAP s_hbmMem   = NULL;
static HBITMAP s_hbmMemOld= NULL;
static int     s_memW = 0, s_memH = 0;
/* FIX [HealthSparkColor]: the trend line is drawn in status colours (green/orange/red
** per segment) with a bolder stroke so a healthy period clearly reads as a green line
** (was a thin neutral gray that disappeared against the band). */
static HPEN    s_penSparkG = NULL, s_penSparkO = NULL, s_penSparkR = NULL;
/* FIX [HealthSparkThreshold]: dotted marker at the mail-alert level in the sparkline. */
static HPEN    s_penThresh = NULL;
/* FIX [HealthSparkFill]: light-tint pens for the area fill under the trend line. */
static HPEN    s_penFillG = NULL, s_penFillO = NULL, s_penFillR = NULL;
/* Status-colour brushes: still used by the rollup accent bar (ComputeRollup path).
** FIX [HealthDotAA]: the dot pens/brushes are gone - the dots now build their own
** oversampled figure (DrawDotFigure) with locally created GDI objects. */
static HBRUSH  s_brGreen  = NULL, s_brOrange = NULL, s_brRed = NULL;
static UINT    s_gdiDpi   = 0;     /* DPI the pens were created for */

/* FIX [HealthDotAA]: oversample scratch buffer for antialiased status dots (see
** DrawStatusDot). Sized d*HP_DOT_SS square; rebuilt only when that edge changes. */
#define HP_DOT_SS 4
static HDC     s_hdcDot    = NULL;
static HBITMAP s_hbmDot    = NULL, s_hbmDotOld = NULL;
static int     s_dotSS     = 0;    /* current oversampled buffer edge, 0 = none */

/* FIX [HealthPanelTips3]: hover tooltips over the panel entries, third design.
** Take 1 ([HealthPanelTipsDisabled]) put a second TTF_SUBCLASS hook on the
** toolbar and crashed; take 2 ([HealthPanelTips2]) shared the toolbar's OWN
** tooltip control (TB_GETTOOLTIPS) and ALSO produced crashes (0xc000041d) on
** the production instance. This version shares NOTHING with comctl32's toolbar
** machinery: one self-owned TRACKING tooltip window (owner = the main window),
** one TTF_TRACK tool, driven entirely by our own low-frequency poll timer
** (GetCursorPos + PtInRect against rects captured during Draw). No subclassing,
** no foreign control, no per-draw messages - the tooltip is only touched when
** the hover state actually changes. */
/* Slots: 0=score, 1=spark, 2=COM, 3+i=feed i, and the rollup summary dot last.
** FIX [HealthRollup]: rollup appended as the final slot so the existing 0..2 and
** 3+i feed indices are unchanged. FIX [HealthClickConfig]: s_slotCmd[] carries the
** WM_COMMAND id a left-click on that slot posts (0 = not clickable). */
#define HP_ROLLUP_SLOT        (3 + FEED_COUNT)
#define HP_TIP_SLOTS          (4 + FEED_COUNT)   /* 0=score, 1=spark, 2=COM, 3+i=feed, last=rollup */
#define HP_TIP_TIMER_ID       0xE210             /* far above PDW's timer ids 101..107 */
#define HP_TIP_POLL_MS        150
#define HP_TIP_DWELL_TICKS    3                  /* ~450 ms hover before showing */
#define HP_TIP_AUTOPOP_TICKS  40                 /* ~6 s visible, then hide until re-hover */
static HWND     s_hTip = NULL;                   /* our own tracking tooltip */
static BOOL     s_tipToolAdded = FALSE;
static UINT_PTR s_tipTimer = 0;
static char     s_tipText[HP_TIP_SLOTS][224];    /* per-entry text, filled during Draw */
static RECT     s_tipRect[HP_TIP_SLOTS];         /* main-window client coords; empty = off */
static UINT     s_slotCmd[HP_TIP_SLOTS];         /* FIX [HealthClickConfig]: WM_COMMAND on left-click, 0 = none */
static char     s_trackText[224];                /* buffer registered with the control */
static int      s_tipSlot = -1;                  /* slot currently shown (-1 = hidden) */
static int      s_hoverSlot = -1;                /* slot currently under the cursor */
static int      s_tipDwell = 0;
static int      s_tipShownTicks = 0;
static int      s_tipMutedSlot = -1;             /* auto-popped; stay quiet until re-hover */
static int      s_tipLastW = 120;                /* last measured bubble width (provisional placement) */

/* Last painted footprint (main-window client coords), for self-erase. */
static RECT s_lastRect;
static BOOL s_lastValid = FALSE;

/* FIX [HealthPanelCorner]: cached fit state for HealthPanel_Active(). Updated by
** every ComputeLayout(); computed lazily on first use so the needle gate works
** before the first paint. */
static BOOL s_lastFits  = FALSE;
static BOOL s_fitsValid = FALSE;

/* Context-menu command ids - local to TrackPopupMenu(TPM_RETURNCMD), never
** routed through WM_COMMAND, so no Resource.h registration is needed. */
#define HPM_SRC_NEEDLE   1
#define HPM_SRC_TELNET   2
#define HPM_SPARK_1      3
#define HPM_SPARK_5      4
#define HPM_SPARK_15     5
#define HPM_SPARK_60     6
#define HPM_TOGGLE_SHOW  7
#define HPM_TOGGLE_NEEDLE 8	// FIX [HealthNeedleCombo]: panel + classic needle in its old corner slot
#define HPM_SPARK_240    9	// FIX [HealthSparkLong]: 4-hour trend window
#define HPM_SPARK_480    10	// FIX [HealthSparkLong]: 8-hour trend window (see the night)

/* FIX [HealthStatusTriad]: one coherent status triad, tuned for a status panel
** that sits permanently on the light-gray (COLOR_3DFACE) toolbar band and where
** a fault MUST read instantly. The three come from one design family (deep,
** saturated, a step back from the pure primaries) so they harmonise on gray,
** yet RED stays loud:
**   - GREEN darkened/saturated vs the earlier (40,167,69): that read too light
**     on the "100%" score TEXT against the gray band. This green keeps a healthy
**     dot but gives the numeral real contrast (Rob).
**   - RED a strong, minimal-blue red (not a soft crimson): errors have to shout.
**   - ORANGE a clear orange, distinct from both green and red.
** The message-text palette in Gfx.cpp (rgbColor[]) is untouched; these are
** panel-local only, and the panel replaces the classic RX-Q corner while active
** so there is no side-by-side clash with rgbColor[]'s pure RED. */
#define HP_RGB_GREEN   RGB( 21, 128,  61)
#define HP_RGB_ORANGE  RGB(249, 115,  22)
#define HP_RGB_RED     RGB(220,  38,  38)
#define HP_RGB_GRAY    RGB(138, 138, 138)
#define HP_RGB_GRAYLT  RGB(170, 170, 170)	/* threshold marker: lighter than the data line */
/* FIX [HealthSparkFill]: light tints of the triad for the soft area fill under
** the trend line, so trend direction reads at a glance without drowning the line. */
#define HP_RGB_FILLG   RGB(198, 224, 205)
#define HP_RGB_FILLO   RGB(250, 226, 193)
#define HP_RGB_FILLR   RGB(244, 203, 203)

/* ---------------------------------------------------------------------------
** Layout
** ---------------------------------------------------------------------------*/
typedef struct {
	RECT rc;            /* panel box, main-window client coords */
	int  sparkW;        /* sparkline width in px, 0 = dropped   */
	BOOL showLabels;    /* feed tags next to the dots           */
	BOOL fits;          /* FALSE = not enough room, don't draw  */
	int  feeds[FEED_COUNT];
	int  nFeeds;        /* enabled feeds, in display order      */
	BOOL showCom;       /* COM dot (serial input enabled)       */
} HPLAYOUT;

static void EnsureGdiObjects(void)
{
	if (s_gdiDpi == g_dpi && s_penSparkG) return;

	/* DPI changed (or first use): pens have Scale()d width, rebuild them. */
	if (s_penSparkG) { DeleteObject(s_penSparkG); s_penSparkG = NULL; }
	if (s_penSparkO) { DeleteObject(s_penSparkO); s_penSparkO = NULL; }
	if (s_penSparkR) { DeleteObject(s_penSparkR); s_penSparkR = NULL; }
	if (s_penThresh) { DeleteObject(s_penThresh); s_penThresh = NULL; }
	if (s_penFillG)  { DeleteObject(s_penFillG);  s_penFillG  = NULL; }	// FIX [HealthSparkFill]
	if (s_penFillO)  { DeleteObject(s_penFillO);  s_penFillO  = NULL; }
	if (s_penFillR)  { DeleteObject(s_penFillR);  s_penFillR  = NULL; }
	if (s_brGreen)   { DeleteObject(s_brGreen);   s_brGreen   = NULL; }
	if (s_brOrange)  { DeleteObject(s_brOrange);  s_brOrange  = NULL; }
	if (s_brRed)     { DeleteObject(s_brRed);     s_brRed     = NULL; }

	int pw = Scale(1);
	if (pw < 1) pw = 1;
	// FIX [HealthSparkColor]: bolder stroke for the trend line so it reads at a glance
	int pwSpark = Scale(2);
	if (pwSpark < 2) pwSpark = 2;
	s_penSparkG = CreatePen(PS_SOLID, pwSpark, HP_RGB_GREEN);
	s_penSparkO = CreatePen(PS_SOLID, pwSpark, HP_RGB_ORANGE);
	s_penSparkR = CreatePen(PS_SOLID, pwSpark, HP_RGB_RED);
	// FIX [HealthSparkThreshold]: PS_DOT only dots with width 1 on classic GDI.
	// Gray, not red (Rob): a neutral marker, the data line carries the colours.
	// FIX [HealthStatusTriad]: lighter gray than the data line so "reference level"
	// clearly reads as secondary to the measured trend.
	s_penThresh = CreatePen(PS_DOT, 1, HP_RGB_GRAYLT);
	// FIX [HealthSparkFill]: 1px light-tint pens draw the soft area fill under the trend.
	s_penFillG  = CreatePen(PS_SOLID, pw, HP_RGB_FILLG);
	s_penFillO  = CreatePen(PS_SOLID, pw, HP_RGB_FILLO);
	s_penFillR  = CreatePen(PS_SOLID, pw, HP_RGB_FILLR);
	// FIX [HealthDotAA]: status-dot pens are no longer cached here - the dots build
	// their own oversampled figure with locally created GDI objects (DrawDotFigure).
	// Only the rollup accent bar still needs the solid status brushes.
	s_brGreen   = CreateSolidBrush(HP_RGB_GREEN);
	s_brOrange  = CreateSolidBrush(HP_RGB_ORANGE);
	s_brRed     = CreateSolidBrush(HP_RGB_RED);
	s_gdiDpi    = g_dpi;
}

static void FreeMemDC(void)
{
	if (s_hdcMem)
	{
		if (s_hbmMemOld) SelectObject(s_hdcMem, s_hbmMemOld);
		DeleteDC(s_hdcMem);
		s_hdcMem = NULL;
	}
	if (s_hbmMem) { DeleteObject(s_hbmMem); s_hbmMem = NULL; }
	s_hbmMemOld = NULL;
	s_memW = s_memH = 0;

	/* FIX [HealthDotAA]: tear down the dot oversample buffer alongside the back-buffer. */
	if (s_hdcDot)
	{
		if (s_hbmDotOld) SelectObject(s_hdcDot, s_hbmDotOld);
		DeleteDC(s_hdcDot);
		s_hdcDot = NULL;
	}
	if (s_hbmDot) { DeleteObject(s_hbmDot); s_hbmDot = NULL; }
	s_hbmDotOld = NULL;
	s_dotSS = 0;
}

/* ---------------------------------------------------------------------------
** FIX [HealthPanelTips3]: hover tooltips (self-owned tracking tooltip)
** ---------------------------------------------------------------------------*/

/* Fill the track tool's TOOLINFO. Our control has exactly one tool. */
static void TipToolInfo(TOOLINFO *ti)
{
	memset(ti, 0, sizeof(*ti));
	ti->cbSize   = sizeof(TOOLINFO);
	ti->uFlags   = TTF_TRACK | TTF_ABSOLUTE;
	ti->hwnd     = ghWnd;
	ti->uId      = 1;
	ti->lpszText = s_trackText;
}

/* Hide the tracking tip (no-op when already hidden). */
static void TipHide(void)
{
	if (s_hTip && s_tipToolAdded && s_tipSlot >= 0)
	{
		TOOLINFO ti;
		TipToolInfo(&ti);
		SendMessage(s_hTip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
	}
	s_tipSlot = -1;
	s_tipShownTicks = 0;
}

/* Poll tick (TIMERPROC, GUI thread): hit-test the cursor against the rects
** captured during the last Draw and show/hide the tracking tip accordingly.
** The tooltip control is only messaged on actual state changes. */
static void CALLBACK TipTimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
	(void)hwnd; (void)uMsg; (void)idEvent; (void)dwTime;
	if (!s_hTip || !s_tipToolAdded) return;

	int   slot = -1;
	POINT ptScr;
	GetCursorPos(&ptScr);

	if (Profile.nHealthPanelVisible && s_lastValid && HealthPanel_Active())
	{
		/* only when the pointer really rests on OUR band (the toolbar child covers
		** the panel area) - never through an overlapping foreign window */
		HWND hUnder = WindowFromPoint(ptScr);
		if (hUnder == hToolbar || hUnder == ghWnd)
		{
			POINT pt = ptScr;
			ScreenToClient(ghWnd, &pt);
			int i;
			for (i = 0; i < HP_TIP_SLOTS; i++)
			{
				if (s_tipText[i][0] && PtInRect(&s_tipRect[i], pt)) { slot = i; break; }
			}
		}
	}

	if (slot < 0)                       /* not over any entry */
	{
		TipHide();
		s_hoverSlot = -1;
		s_tipDwell = 0;
		s_tipMutedSlot = -1;
		return;
	}

	if (slot != s_hoverSlot)            /* moved onto a different entry */
	{
		s_hoverSlot = slot;
		s_tipDwell  = 0;
		if (s_tipSlot >= 0 && s_tipSlot != slot) TipHide();
		if (s_tipMutedSlot != slot) s_tipMutedSlot = -1;
		return;
	}

	if (slot == s_tipMutedSlot) return; /* auto-popped; wait until the cursor leaves */

	if (s_tipSlot == slot)              /* currently visible on this entry */
	{
		if (++s_tipShownTicks >= HP_TIP_AUTOPOP_TICKS)
		{
			TipHide();
			s_tipMutedSlot = slot;
		}
		return;
	}

	if (++s_tipDwell < HP_TIP_DWELL_TICKS) return;

	/* show: refresh text (entries change state live), position near the cursor */
	_snprintf(s_trackText, sizeof(s_trackText) - 1, "%s", s_tipText[slot]);
	s_trackText[sizeof(s_trackText) - 1] = '\0';
	{
		TOOLINFO ti;
		TipToolInfo(&ti);
		SendMessage(s_hTip, TTM_UPDATETIPTEXT, 0, (LPARAM)&ti);

		// FIX [HealthPanelTipPos2]: the tip still clipped off the right edge on the
		// rightmost feed dots. Root cause: comctl32 recomputes the bubble size only when
		// the tip is activated/shown, NOT on TTM_UPDATETIPTEXT. The earlier
		// [HealthPanelTipPos] measured with TTM_GETBUBBLESIZE right AFTER updating the
		// text but BEFORE activating, so it read the PREVIOUS entry's width (and 0 on the
		// first show). With TTF_ABSOLUTE the left edge is pinned at tx = cursor - tw - 8;
		// when the real bubble was wider than that stale tw, its right edge overshot the
		// cursor and ran off-screen - worst on the rightmost, longest-text dots.
		// Fix: ACTIVATE first (forces the layout), THEN measure, THEN position. A
		// provisional placement (last known width) precedes the activate so the one-shot
		// show never flashes at the right edge, and a right-edge clamp is the final net.
		int ty = ptScr.y + Scale(18);
		int txProv = ptScr.x - s_tipLastW - Scale(8);
		SendMessage(s_hTip, TTM_TRACKPOSITION, 0, MAKELPARAM(txProv, ty));
		SendMessage(s_hTip, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);

		DWORD bub = (DWORD)SendMessage(s_hTip, TTM_GETBUBBLESIZE, 0, (LPARAM)&ti);
		int   tw  = LOWORD(bub), th = HIWORD(bub);
		if (tw > 0) s_tipLastW = tw;          /* remember for the next provisional show */
		int   tx  = ptScr.x - tw - Scale(8);
		MONITORINFO mi;
		mi.cbSize = sizeof(mi);
		// FIX [HealthTipMonInfoUninit]: workR must come from a branch that really filled
		// mi. The diagnostic below used to read mi.rcWork.right whenever hMon was non-NULL,
		// but mi is only populated when GetMonitorInfo ALSO succeeds - only cbSize is
		// initialized here. A non-NULL hMon with a failing GetMonitorInfo (display being
		// swapped out under RDP / monitor detached between the two calls) therefore read an
		// uninitialized stack int into the log line. Capture it inside the success branch.
		int workR = -1;
		HMONITOR hMon = MonitorFromPoint(ptScr, MONITOR_DEFAULTTONEAREST);
		if (hMon && GetMonitorInfo(hMon, &mi))
		{
			if (tx + tw > mi.rcWork.right)    tx = mi.rcWork.right - tw - Scale(2); /* never off the right */
			if (tx < mi.rcWork.left)          tx = mi.rcWork.left;
			if (ty + th > mi.rcWork.bottom)   ty = ptScr.y - th - Scale(8);
			if (ty < mi.rcWork.top)           ty = mi.rcWork.top;
			workR = (int)mi.rcWork.right;
		}
		// Diagnostic (silent unless the debug channel is enabled): confirms the
		// measured width now tracks the current entry, not the previous one.
		PDW_DLOG("HealthTip slot=%d pt=%d,%d tw=%d th=%d tx=%d ty=%d workR=%d",
		         slot, ptScr.x, ptScr.y, tw, th, tx, ty, workR);
		SendMessage(s_hTip, TTM_TRACKPOSITION, 0, MAKELPARAM(tx, ty));
	}
	s_tipSlot = slot;
	s_tipShownTicks = 0;
}

static void TipEnsure(void)
{
	if (!ghWnd) return;
	if (!s_tipTimer)
		s_tipTimer = SetTimer(ghWnd, HP_TIP_TIMER_ID, HP_TIP_POLL_MS, TipTimerProc);
	if (s_hTip) return;

	s_hTip = CreateWindowEx(WS_EX_TOPMOST, TOOLTIPS_CLASS, NULL,
	                        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
	                        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
	                        ghWnd, NULL, GetModuleHandle(NULL), NULL);
	if (!s_hTip) return;

	TOOLINFO ti;
	TipToolInfo(&ti);
	s_trackText[0] = '\0';
	s_tipToolAdded = (BOOL)SendMessage(s_hTip, TTM_ADDTOOL, 0, (LPARAM)&ti);
	SendMessage(s_hTip, TTM_SETMAXTIPWIDTH, 0, 400);	/* wrap long feed-error texts */
}

/* Capture one hover region. rcMain = main-window client coords. Pure data
** store - the tooltip control is never touched from the draw path.
** FIX [HealthClickConfig]: cmd = WM_COMMAND id posted on a left-click on this
** slot (0 = not clickable). */
static void TipSet(HWND hMain, int slot, const RECT *rcMain, const char *text, UINT cmd)
{
	(void)hMain;
	if (slot < 0 || slot >= HP_TIP_SLOTS) return;
	_snprintf(s_tipText[slot], sizeof(s_tipText[slot]) - 1, "%s", text ? text : "");
	s_tipText[slot][sizeof(s_tipText[slot]) - 1] = '\0';
	s_tipRect[slot] = *rcMain;
	s_slotCmd[slot] = cmd;
}

/* Panel hidden/unfit: clear all hover regions and hide any visible tip. */
static void TipParkAll(HWND hMain)
{
	int slot;
	(void)hMain;
	for (slot = 0; slot < HP_TIP_SLOTS; slot++)
	{
		s_tipRect[slot].left = s_tipRect[slot].top = 0;
		s_tipRect[slot].right = s_tipRect[slot].bottom = 0;
		s_slotCmd[slot] = 0;	// FIX [HealthClickConfig]
	}
	TipHide();

	// FIX [HealthTipTimerHide]: stop the hover poll timer while the panel is hidden
	// or does not fit. TipEnsure() recreates it on the next draw when the panel is
	// active again. Previously the 150 ms timer, once created, kept firing (a
	// GetCursorPos every tick) until shutdown even with the panel hidden - harmless
	// but a needless steady-state wakeup on an otherwise idle machine.
	if (s_tipTimer) { if (ghWnd) KillTimer(ghWnd, HP_TIP_TIMER_ID); s_tipTimer = 0; }
}

/* FIX [HealthUptimeTip]: local time of "ago" seconds ago, as a SYSTEMTIME. FILETIME
** arithmetic so it stays correct across midnight/month ends without pulling in <time.h>
** (same approach as HP_FormatAgoClock further down, which formats rather than returns). */
static BOOL HP_SystemTimeAgo(unsigned long long ago, SYSTEMTIME *out)
{
	SYSTEMTIME stNow;  GetLocalTime(&stNow);
	FILETIME   ft;     if (!SystemTimeToFileTime(&stNow, &ft)) return FALSE;
	ULARGE_INTEGER u;  u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
	ULONGLONG back = (ULONGLONG)ago * 10000000ULL;	/* 100 ns units */
	u.QuadPart = (u.QuadPart > back) ? (u.QuadPart - back) : 0;
	ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
	return FileTimeToSystemTime(&ft, out);
}

/* "since 14:02" / "since 18-07 14:02" helper for the feed tooltips. */
static void FmtSince(const SYSTEMTIME *st, char *out, int cb)
{
	SYSTEMTIME now;
	GetLocalTime(&now);
	if (st->wYear == now.wYear && st->wMonth == now.wMonth && st->wDay == now.wDay)
		_snprintf(out, cb - 1, "%02d:%02d", st->wHour, st->wMinute);
	else
		_snprintf(out, cb - 1, "%02d-%02d %02d:%02d", st->wDay, st->wMonth, st->wHour, st->wMinute);
	out[cb - 1] = '\0';
}

/* FIX [FeedLastError]: feed tooltip = latched state + since-time + last problem
** text from feedstatus.cpp, so a red/orange dot explains itself on hover. */
static void BuildFeedTip(int feed, char *out, int cb)
{
	FEEDSTATUS_INFO fi;
	char szSince[40] = "";

	FeedStatus_GetInfo(feed, &fi);
	if (fi.bSinceValid) FmtSince(&fi.stSince, szSince, sizeof(szSince));

	switch (fi.state)
	{
		case FS_OK:
		_snprintf(out, cb - 1, "%s: OK%s%s (last delivery/connection successful)",
		          FeedStatus_Name(feed), szSince[0] ? " since " : "", szSince);
		break;

		case FS_RETRY:
		_snprintf(out, cb - 1, "%s: retrying%s%s%s%s",
		          FeedStatus_Name(feed), szSince[0] ? " since " : "", szSince,
		          fi.szDetail[0] ? " - " : " (temporary problem)", fi.szDetail);
		break;

		case FS_ERROR:
		_snprintf(out, cb - 1, "%s: FAILED%s%s%s%s",
		          FeedStatus_Name(feed), szSince[0] ? " since " : "", szSince,
		          fi.szDetail[0] ? " - " : " (last delivery/connection failed)", fi.szDetail);
		break;

		default:
		_snprintf(out, cb - 1, "%s: enabled - nothing delivered yet", FeedStatus_Name(feed));
		break;
	}
	out[cb - 1] = '\0';
}

/* Which feeds get an entry: only feeds enabled in the Profile, so the strip
** stays compact ("compiled in" is not enough - everything is always compiled
** in; enabled is the real signal that the user cares about this feed). */
static void CollectFeeds(HPLAYOUT *lo)
{
	lo->nFeeds = 0;
	if (Profile.SMTP)                lo->feeds[lo->nFeeds++] = FEED_SMTP;
	if (Profile.webhookEnabled)      lo->feeds[lo->nFeeds++] = FEED_WEBHOOK;
	if (Profile.telegramEnabled)     lo->feeds[lo->nFeeds++] = FEED_TELEGRAM;
	if (Profile.pushoverEnabled)     lo->feeds[lo->nFeeds++] = FEED_PUSHOVER;
	if (Profile.mqttEnabled)         lo->feeds[lo->nFeeds++] = FEED_MQTT;
	if (Profile.mysql_enabled)       lo->feeds[lo->nFeeds++] = FEED_MYSQL;
	if (Profile.sqlite_enabled)      lo->feeds[lo->nFeeds++] = FEED_SQLITE;
	if (Profile.telnetServerEnabled) lo->feeds[lo->nFeeds++] = FEED_TELNET;
	lo->showCom = Profile.comPortEnabled ? TRUE : FALSE;
}

/* FIX [HealthClickConfig]: which config dialog a left-click on an entry opens.
** Mirrors the Interface menu WM_COMMAND ids handled in PDW.cpp. */
static UINT FeedMenuCmd(int feed)
{
	switch (feed)
	{
		case FEED_SMTP:     return IDM_MAIL;
		case FEED_WEBHOOK:  return IDM_WEBHOOK;
		case FEED_TELEGRAM: return IDM_TELEGRAM;
		case FEED_PUSHOVER: return IDM_PUSHOVER;
		case FEED_MQTT:     return IDM_MQTT;
		case FEED_MYSQL:    return IDM_MYSQL;
		case FEED_SQLITE:   return IDM_SQLITE;
		case FEED_TELNET:   return IDM_TELNETSERVER;
	}
	return 0;
}

/* FIX [HealthRollup]: severity of a single "overall" summary dot = the worst of
** the RX health, the COM input link and every enabled feed (0 ok, 1 degraded,
** 2 fault). Also fills a short reason for the tooltip. This is the one light to
** glance at: green only when the whole chain (input -> decode -> every feed) is
** healthy. It intentionally folds in the RX score too, so the summary is red
** whenever anything - signal OR plumbing - is wrong. */
static int ComputeRollup(const HPLAYOUT *lo, char *why, int cbWhy)
{
	int worst = 0, nErr = 0, nWarn = 0, i;

	int hs = Health_GetStatus();
	if (hs == HSTAT_RED)         { worst = 2; nErr++; }
	else if (hs == HSTAT_ORANGE) { if (worst < 1) worst = 1; nWarn++; }

	if (lo->showCom)
	{
		int link = Rs232LinkState();
		int sev  = (link == 2) ? 0 : (link == 1) ? 1 : 2;
		if (sev == 2) { worst = 2; nErr++; }
		else if (sev == 1) { if (worst < 1) worst = 1; nWarn++; }
	}

	for (i = 0; i < lo->nFeeds; i++)
	{
		int fs = FeedStatus_Get(lo->feeds[i]);
		if (fs == FS_ERROR) { worst = 2; nErr++; }
		else if (fs == FS_RETRY) { if (worst < 1) worst = 1; nWarn++; }
	}

	if (why && cbWhy > 0)
	{
		/* FIX [HealthUptimeTip]: PDW is a 24/7 decoder, so "how long has it been up"
		** belongs with the one status light you hover anyway. Second line of the rollup
		** bubble = "Up 3d 04:12 - since 15-08 09:03", no F12 dialog needed. The start
		** stamp is reconstructed from the uptime (FmtSince drops the date when PDW was
		** started today, exactly like the feed tooltips). */
		unsigned long long up = PdwUptimeSeconds();
		char szUp[40] = "", szSince[40] = "";
		SYSTEMTIME stStart;

		PdwFormatUptime(up, 0, szUp, sizeof(szUp));
		if (HP_SystemTimeAgo(up, &stStart)) FmtSince(&stStart, szSince, sizeof(szSince));

		if (worst == 0)
			_snprintf(why, cbWhy - 1, "Overall status: all OK (RX, input and every enabled feed healthy)\r\nUp %s%s%s",
			          szUp, szSince[0] ? " - since " : "", szSince);
		else
			_snprintf(why, cbWhy - 1, "Overall status: %d fault%s, %d warning%s - see the score and dots\r\nUp %s%s%s",
			          nErr, nErr == 1 ? "" : "s", nWarn, nWarn == 1 ? "" : "s",
			          szUp, szSince[0] ? " - since " : "", szSince);
		why[cbWhy - 1] = '\0';
	}
	return worst;
}

/* Measure text with a given font against the MAIN window DC. */
static int TextW(HDC hdc, HFONT font, const char *s)
{
	SIZE sz = { 0, 0 };
	HFONT old = (HFONT)SelectObject(hdc, font);
	GetTextExtentPoint32(hdc, s, (int)strlen(s), &sz);
	SelectObject(hdc, old);
	return sz.cx;
}

/* Compute the panel footprint. Also used by the right-click hit test while
** the panel is hidden (the rect is where the panel would be). */
static void ComputeLayout(HWND hwnd, HPLAYOUT *lo)
{
	RECT r, rcBtn;
	int  i;

	memset(lo, 0, sizeof(*lo));
	memset(&r, 0, sizeof(r));

	// FIX [HealthPanelCorner]: this can now be reached from the high-frequency needle
	// gate (HealthPanel_Active) very early in startup - fail safe to "no panel" until
	// the main window and its client rect really exist.
	if (!hwnd || !GetClientRect(hwnd, &r) || r.right <= 0 || g_cyToolbar <= 0)
	{
		lo->fits = FALSE; s_lastFits = FALSE;	/* leave s_fitsValid FALSE: retry next call */
		return;
	}

	CollectFeeds(lo);

	/* Vertical: same box height + band clamps as the signal meter box. */
	int boxH = Scale(24);
	int y = (g_cyToolbar - boxH) / 2;
	if (y < Scale(1)) y = Scale(1);
	if (y + boxH > g_cyToolbar - 1) y = g_cyToolbar - 1 - boxH;
	if (y < 0) { y = 0; boxH = g_cyToolbar - 1; }	/* band shorter than the box: shrink */
	if (boxH < Scale(12)) { lo->fits = FALSE; s_lastFits = FALSE; s_fitsValid = TRUE; return; }	// FIX [HealthPanelCorner]

	/* Horizontal free span: last toolbar item .. RX-Q warning square. */
	int left = 0;
	if (hToolbar)
	{
		int nBtns = (int)SendMessage(hToolbar, TB_BUTTONCOUNT, 0, 0);
		if (nBtns > 0 && SendMessage(hToolbar, TB_GETITEMRECT, nBtns - 1, (LPARAM)&rcBtn))
		{
			POINT pt = { rcBtn.right, 0 };
			MapWindowPoints(hToolbar, hwnd, &pt, 1);
			left = pt.x;
		}
	}
	left += Scale(8);
	// FIX [HealthPanelCorner]: the panel REPLACES the classic corner (needle + RX-Q box +
	// warning square) while active, so it may run to the same right margin the needle used.
	int right = r.right - Scale(5);
	// FIX [HealthNeedleCombo]: in the combined layout the classic RX needle keeps its old
	// far-right slot; pull the panel's right edge in so the two never overlap. The reserve
	// mirrors sigind.cpp's gauge footprint (SIGIND_LOGW=32 + its Scale(5) right margin) plus
	// a small gap before the panel frame - same "local mirror of another file's fixed corner
	// geometry" pattern as Gfx.cpp's Scale(46) RX-Q box.
	if (Profile.nHealthShowNeedle) right -= Scale(32) + Scale(5) + Scale(8);
	int avail = right - left;

	/* Measure content (against the main-window DC with the shared fonts). */
	HDC hdc = GetDC(hwnd);
	int wScore = TextW(hdc, pdw_font[FONT_RXQUAL], "100%");
	int pad    = Scale(4);
	int dot    = Scale(9);	// FIX [HealthDotSize]: 7=too small, 10=too big WITH inner icons; after [HealthDotNoBar] bumped 8->9 (Rob wanted 1-2px larger). Keep in sync with the draw pass.
	int gapDot = Scale(3);
	int gapEnt = Scale(6);
	// FIX [HealthScoreLabel]: small "RX" caption so the bare percentage is
	// unmistakably the RX-health score.
	int wRx  = TextW(hdc, pdw_font[FONT_LABELS], "RX");
	// FIX [HealthRollupBar]: left-edge accent bar (overall status) + its leading pad.
	int accW = (lo->showCom || lo->nFeeds > 0) ? Scale(4) : 0;
	int lead = accW ? (accW + pad) : pad;

	int wDots = 0, wDotsLbl = 0;
	if (lo->showCom)
	{
		wDots    += dot + gapEnt;
		wDotsLbl += dot + gapDot + TextW(hdc, pdw_font[FONT_LABELS], "COM") + gapEnt;
	}
	for (i = 0; i < lo->nFeeds; i++)
	{
		wDots    += dot + gapEnt;
		wDotsLbl += dot + gapDot + TextW(hdc, pdw_font[FONT_LABELS], FeedStatus_Tag(lo->feeds[i])) + gapEnt;
	}
	ReleaseDC(hwnd, hdc);

	int sparkMin = Scale(30), sparkMax = Scale(96);	/* FIX [HealthPanelCorner]: corner space freed up */

	/* Tier 1: labels + sparkline. Tier 2: dots only + sparkline.
	** Tier 3: dots only, no sparkline. Otherwise: hide. */
	// FIX [HealthRollupBar]/[HealthScoreLabel]: accent-bar lead + "RX" caption + score block
	int fixed = lead + wRx + gapDot + wScore + pad;
	int tail  = pad;                                /* right padding */

	lo->showLabels = TRUE;
	int need = fixed + sparkMin + pad + wDotsLbl + tail;
	if (need > avail)
	{
		lo->showLabels = FALSE;
		need = fixed + sparkMin + pad + wDots + tail;
	}
	if (need > avail)
	{
		lo->sparkW = 0;
		need = fixed + wDots + tail;
		if (need > avail) { lo->fits = FALSE; s_lastFits = FALSE; s_fitsValid = TRUE; return; }	// FIX [HealthPanelCorner]
	}
	else
	{
		int extra = avail - need;
		lo->sparkW = sparkMin + (extra > (sparkMax - sparkMin) ? (sparkMax - sparkMin) : extra);
		need += lo->sparkW - sparkMin;
	}

	lo->rc.left   = right - need;
	lo->rc.right  = right;
	lo->rc.top    = y;
	lo->rc.bottom = y + boxH;
	lo->fits      = TRUE;
	s_lastFits    = TRUE;	// FIX [HealthPanelCorner]
	s_fitsValid   = TRUE;
}

// FIX [HealthPanelCorner]: see HealthPanel.h. Reads cached fit state; computes it
// once lazily so the needle gate is correct even before the first panel paint.
BOOL HealthPanel_Active(void)
{
	if (!Profile.nHealthPanelVisible) return FALSE;
	if (!s_fitsValid)
	{
		HPLAYOUT lo;
		ComputeLayout(ghWnd, &lo);
	}
	return s_lastFits;
}

// FIX [HealthNeedleCombo]: see HealthPanel.h. The needle is suppressed only when the
// panel owns the corner alone; in the combined layout (nHealthShowNeedle) the panel
// makes room on its right (ComputeLayout) and the needle keeps its old slot.
BOOL HealthPanel_SuppressNeedle(void)
{
	return (HealthPanel_Active() && !Profile.nHealthShowNeedle) ? TRUE : FALSE;
}

// FIX [HealthComboRxqLeak]: force a FRESH fit evaluation for the current client
// width. HealthPanel_Active() otherwise returns the fit cached by the last panel
// Draw (ComputeLayout). Within a single DrawPaneLabels pass the classic PANE1
// header width and the PANERXQUAL "100%"-box gate read that cache BEFORE PANEHEALTH's
// own Draw refreshes it, so a FALSE->TRUE fit transition (e.g. maximizing from a
// narrower size where the combined-layout panel did not fit, or startup before the
// toolbar is laid out) made those classic pieces use the STALE "does not fit" value:
// PANERXQUAL then painted the green RX-Q "100%" box under the combined-layout needle
// for one frame, and it lingered until the next full repaint. Calling this at the top
// of DrawPaneLabels makes every HealthPanel_Active() read in that pass current and
// consistent, so the stray "100%" is never drawn while the panel actually fits.
void HealthPanel_RefreshActive(void)
{
	if (!Profile.nHealthPanelVisible) return;
	HPLAYOUT lo;
	ComputeLayout(ghWnd, &lo);
}

// FIX [HealthFitSizeInvalidate]: drop the cached fit decision on a window-size change
// so the next HealthPanel_Active() re-evaluates for the NEW client width instead of
// trusting the value cached at the previous size. Pairs with [HealthComboRxqLeak]:
// keeps a pre-maximize "does not fit" (classic-corner) verdict from carrying into the
// post-maximize layout and briefly drawing the classic RX-Q "100%" under the needle.
// Covers every HealthPanel_Active() reader, including the ones that run before the
// first DrawPaneLabels of the new size (e.g. DrawSigInd's SuppressNeedle gate).
void HealthPanel_OnSize(void)
{
	s_fitsValid = FALSE;
}

/* ---------------------------------------------------------------------------
** Trend sampling (1 Hz, GUI thread)
** ---------------------------------------------------------------------------*/
void HealthPanel_OnSecond(void)
{
	float v = Health_HasData() ? (float)Health_GetScore() : HP_SPARK_GAP;
	s_hist[s_histHead] = v;
	s_histHead = (s_histHead + 1) % HP_HIST_MAX;
	if (s_histCount < HP_HIST_MAX) s_histCount++;

	// FIX [FeedTransitionLog]: COM link state changes go to the health log, like
	// the feed transitions in feedstatus.cpp (1 Hz GUI tick; flips are rare, steady
	// states log nothing). The first observation is stored silently so startup does
	// not produce a fake "transition". Runs regardless of panel visibility.
	if (Profile.comPortEnabled)
	{
		static int s_lastLink = -1;
		int link = Rs232LinkState();
		if (s_lastLink >= 0 && link != s_lastLink)
		{
			static const char *kLink[3] = { "not open", "open, no data", "receiving" };
			PDW_HEALTHLOG("COM (serial input): %s -> %s",
			              kLink[(s_lastLink >= 0 && s_lastLink <= 2) ? s_lastLink : 0],
			              kLink[(link       >= 0 && link       <= 2) ? link       : 0]);
		}
		s_lastLink = link;
	}
}

/* Fetch the sample from "ago" seconds back; returns HP_SPARK_GAP when absent. */
static float HistAt(int ago)
{
	if (ago >= s_histCount) return HP_SPARK_GAP;
	int idx = s_histHead - 1 - ago;
	while (idx < 0) idx += HP_HIST_MAX;
	return s_hist[idx];
}

/* ---------------------------------------------------------------------------
** Painting
** ---------------------------------------------------------------------------*/
static void EraseFootprint(HWND hwnd, const RECT *rc)
{
	HDC hdc = GetDC(hwnd);
	SelectObject(hdc, null_pen);
	SelectObject(hdc, lgray_brush);
	Rectangle(hdc, rc->left, rc->top, rc->right + 1, rc->bottom + 1);
	/* Keep the band divider continuous under the erased span (mirrors
	** [SigindDividerClip]/[RxqSquareDividerClip]). */
	SelectObject(hdc, SysPEN[DARKGRAY]);
	MoveToEx(hdc, rc->left, g_cyToolbar, NULL);
	LineTo(hdc, rc->right + 1, g_cyToolbar);
	ReleaseDC(hwnd, hdc);
}

static COLORREF StatusColor(int hstat)
{
	switch (hstat)
	{
		case HSTAT_GREEN:  return HP_RGB_GREEN;
		case HSTAT_ORANGE: return HP_RGB_ORANGE;
		case HSTAT_RED:    return HP_RGB_RED;
	}
	return RGB(0, 0, 0);
}

/* FIX [HealthDotAA]: draw one status-dot figure at diameter d into the top-left
** of `dc`, with stroke widths multiplied by `scale`. Factored out so the exact
** same geometry serves both the oversampled AA path (scale = HP_DOT_SS, into the
** scratch buffer) and the non-AA fallback (scale = 1, straight onto the panel).
** sev: 0 = ok (solid green), 1 = retry (orange ring), 2 = error (solid red disc). */
static void DrawDotFigure(HDC dc, int ox, int oy, int d, int sev, int scale)
{
	int pwR = Scale(2) * scale; if (pwR < 2) pwR = 2;   /* retry ring stroke   */

	switch (sev)
	{
		case 1:  /* retry: orange ring, background shows through the centre */
		{
			HPEN   pen = CreatePen(PS_SOLID, pwR, HP_RGB_ORANGE);
			HGDIOBJ op = SelectObject(dc, pen);
			HGDIOBJ ob = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
			int in = pwR / 2;   /* inset so the whole stroke stays inside the box */
			Ellipse(dc, ox + in, oy + in, ox + d - in, oy + d - in);
			SelectObject(dc, ob);
			SelectObject(dc, op);
			DeleteObject(pen);
			break;
		}

		case 2:  /* error: solid red disc (no inner mark) */
		{
			/* FIX [HealthDotNoBar]: dropped the white "minus" bar. A red disc with a
			** horizontal white bar reads as the universal "no-entry / disabled / off"
			** glyph, so an errored feed looked switched-off rather than faulty (Rob).
			** Colour-blind separation is preserved by shape on the OTHER state: retry
			** is a hollow ring (see case 1), ok/error are solid discs of distinct hue,
			** and the hover tooltip spells the state out. `pw` is now unused here. */
			HBRUSH br = CreateSolidBrush(HP_RGB_RED);
			HGDIOBJ op = SelectObject(dc, null_pen);
			HGDIOBJ ob = SelectObject(dc, br);
			Ellipse(dc, ox, oy, ox + d, oy + d);
			SelectObject(dc, ob);
			SelectObject(dc, op);
			DeleteObject(br);
			break;
		}

		default: /* ok / unknown: solid green */
		{
			HBRUSH br = CreateSolidBrush(HP_RGB_GREEN);
			HGDIOBJ op = SelectObject(dc, null_pen);
			HGDIOBJ ob = SelectObject(dc, br);
			Ellipse(dc, ox, oy, ox + d, oy + d);
			SelectObject(dc, ob);
			SelectObject(dc, op);
			DeleteObject(br);
			break;
		}
	}
}

/* FIX [HealthDotShapes]: status dots are no longer colour-only - RETRY draws as
** a RING (hollow) and ERROR as a solid disc with a light "minus" bar, so the
** three states stay distinguishable for colour-blind users. Used by the feed
** dots and the COM link dot alike. sev: 0 = ok, 1 = retry/stalled, 2 = error.
** FIX [HealthDotGreenStart]: an enabled feed with no outcome yet (FS_UNKNOWN) is
** shown SOLID green - "configured and no known problem" should read healthy from
** the first second, because the per-message feeds (Telegram, Pushover, webhook,
** SMTP) can legitimately go days before their first delivery. A problem changes
** the dot within seconds of the first failing attempt anyway.
** FIX [HealthDotAA]: GDI Ellipse() is not antialiased; at ~8px a filled circle
** rasterizes to an angular "cog/torx" outline, and the white bar on the red error
** disc amplified the notched look (the white centre "fell away"). Render the dot
** 4x oversampled into a scratch buffer pre-filled with the panel background, then
** HALFTONE-downscale - the same supersample trick the sigind gauge and toolbar
** icons already use. The edge pixels blend the dot colour into COLOR_3DFACE,
** giving a clean round dot at any DPI. Falls back to the direct (non-AA) path if
** the scratch buffer can't be built (GDI-handle pressure). */
static void DrawStatusDot(HDC hdc, int x, int cy, int d, int sev)
{
	int S  = d * HP_DOT_SS;
	/* FIX [HealthDotVAlign]: nudge the dot down ~1px. Geometrically it is centred on
	** cy (same as the label text), but the label glyphs carry their visual mass above
	** the cell centre, so a dot on cy reads as sitting slightly high next to the tag.
	** Scale(1) so it tracks DPI; tune the multiplier if a future font shifts this. */
	int top = cy - d / 2 + Scale(1);

	/* (Re)build the oversample buffer only when its edge changes (DPI/size). */
	if (!s_hdcDot || s_dotSS != S)
	{
		if (s_hdcDot)
		{
			if (s_hbmDotOld) SelectObject(s_hdcDot, s_hbmDotOld);
			DeleteDC(s_hdcDot); s_hdcDot = NULL;
		}
		if (s_hbmDot) { DeleteObject(s_hbmDot); s_hbmDot = NULL; }
		s_hbmDotOld = NULL; s_dotSS = 0;

		s_hdcDot = CreateCompatibleDC(hdc);
		if (s_hdcDot)
		{
			s_hbmDot = CreateCompatibleBitmap(hdc, S, S);
			if (!s_hbmDot) { DeleteDC(s_hdcDot); s_hdcDot = NULL; }
			else { s_hbmDotOld = (HBITMAP)SelectObject(s_hdcDot, s_hbmDot); s_dotSS = S; }
		}
	}

	if (!s_hdcDot || s_dotSS != S)
	{
		/* Fallback: direct (jaggy) draw so a GDI failure still shows a dot. */
		DrawDotFigure(hdc, x, top, d, sev, 1);
		return;
	}

	/* Prime the scratch buffer with the panel background so the AA edge blends
	** to exactly the colour the dot sits on. */
	{
		HBRUSH bg = CreateSolidBrush(GetSysColor(COLOR_3DFACE));
		RECT   rc = { 0, 0, S, S };
		FillRect(s_hdcDot, &rc, bg);
		DeleteObject(bg);
	}

	DrawDotFigure(s_hdcDot, 0, 0, S, sev, HP_DOT_SS);

	SetStretchBltMode(hdc, HALFTONE);
	SetBrushOrgEx(hdc, 0, 0, NULL);
	StretchBlt(hdc, x, top, d, d, s_hdcDot, 0, 0, S, S, SRCCOPY);
}

/* FIX [HealthSparkLong]: worst dip in the drawn window, published for the tooltip.
** s_lastSparkMinPct = -1 means "no measurement in the window". s_lastSparkMinAgo is
** how many seconds back it occurred (for the "at HH:MM" annotation). GUI thread only. */
static int s_lastSparkMinPct = -1;
static int s_lastSparkMinAgo = 0;

static void DrawSparkline(HDC hdc, int x, int y, int w, int h)
{
	int windowSec = Profile.nHealthSparkMin * 60;
	if (windowSec < 60)   windowSec = 60;
	if (windowSec > HP_HIST_MAX) windowSec = HP_HIST_MAX;

	/* Sunken frame, classic toolbar etch: dark top/left, white bottom/right. */
	SelectObject(hdc, null_pen);
	SelectObject(hdc, lgray_brush);
	Rectangle(hdc, x, y, x + w + 1, y + h + 1);
	SelectObject(hdc, SysPEN[DARKGRAY]);
	MoveToEx(hdc, x + w, y, NULL); LineTo(hdc, x, y); LineTo(hdc, x, y + h);
	SelectObject(hdc, SysPEN[WHITE]);
	LineTo(hdc, x + w, y + h); LineTo(hdc, x + w, y - 1);

	int ix = x + 2, iy = y + 2, iw = w - 4, ih = h - 4;
	if (iw < 8 || ih < 4) return;

	// FIX [HealthSparkColor]: red boundary follows the mail-alert threshold, same
	// mapping as Health_GetStatus(), so the line colour matches the score colour.
	int red = Profile.nRxQualThreshold;
	if (red < 1)  red = 1;
	if (red > 95) red = 95;

	/* One column = windowSec/iw seconds. Newest sample at the right edge. For each
	** column we keep BOTH the average (the trend line + its colour) AND the minimum
	** (the worst RX in that slice). Gaps (no measurement) break the polyline.
	** FIX [HealthSparkLong]: a plain per-column average hides short RX drops once one
	** pixel spans minutes (e.g. an 8-hour window). The minimum is drawn as a band that
	** hangs down from the average line to the worst value, so a night-time dip stays
	** visible as a coloured spike even when the averaged line stays high. Computed once
	** into small stack arrays so the draw passes below render in the right order. */
#define HP_SPARK_MAXCOLS 512
	if (iw > HP_SPARK_MAXCOLS) iw = HP_SPARK_MAXCOLS;
	static int  cPy   [HP_SPARK_MAXCOLS];	/* GUI thread only - safe as static */
	static int  cPyMin[HP_SPARK_MAXCOLS];	/* y of the column minimum (the band foot) */
	static char cIdx  [HP_SPARK_MAXCOLS];	/* avg status: 0 green, 1 orange, 2 red    */
	static char cIdxMn[HP_SPARK_MAXCOLS];	/* min status: colours the band            */
	static char cOn   [HP_SPARK_MAXCOLS];	/* 1 = has a measurement                   */
	int col;
	double gMin = 101.0; int gMinAgo = 0;	/* worst dip across the whole window, for the tooltip */
	for (col = 0; col < iw; col++)
	{
		int aFrom = (int)(( (double)(iw - 1 - col)     * windowSec) / iw);
		int aTo   = (int)(( (double)(iw - col)         * windowSec) / iw);
		if (aTo <= aFrom) aTo = aFrom + 1;

		double sum = 0.0, mn = 101.0; int n = 0, a;
		for (a = aFrom; a < aTo; a++)
		{
			float v = HistAt(a);
			if (v >= 0.0f)
			{
				sum += v; n++;
				if (v < mn) mn = v;
				if (v < gMin) { gMin = v; gMinAgo = a; }	/* track the window's worst + when */
			}
		}
		if (!n) { cOn[col] = 0; continue; }

		double avg = sum / n;
		if (avg < 0.0)   avg = 0.0;
		if (avg > 100.0) avg = 100.0;
		if (mn  < 0.0)   mn  = 0.0;
		if (mn  > 100.0) mn  = 100.0;

		cIdx  [col] = (char)((avg < (double)red) ? 2 : (avg >= 96.0) ? 0 : 1);
		cIdxMn[col] = (char)((mn  < (double)red) ? 2 : (mn  >= 96.0) ? 0 : 1);
		cPy   [col] = iy + (int)(((100.0 - avg) * (ih - 1)) / 100.0 + 0.5);
		cPyMin[col] = iy + (int)(((100.0 - mn ) * (ih - 1)) / 100.0 + 0.5);
		cOn   [col] = 1;
	}

	/* Publish the window's worst dip for the tooltip. */
	s_lastSparkMinPct = (gMin <= 100.0) ? (int)(gMin + 0.5) : -1;
	s_lastSparkMinAgo = gMinAgo;

	// FIX [HealthSparkFill]: soft area fill under the line first, in the light tint of
	// each column's AVERAGE status - painted down to the column MINIMUM so the whole
	// worst-case envelope carries the area-chart look. Threshold + line stay crisp on top.
	{
		HPEN fillPens[3] = { s_penFillG, s_penFillO, s_penFillR };
		HPEN curFill = NULL;
		for (col = 0; col < iw; col++)
		{
			if (!cOn[col]) continue;
			HPEN fp = fillPens[(int)cIdx[col]];
			if (fp != curFill) { SelectObject(hdc, fp); curFill = fp; }
			int px = ix + col;
			MoveToEx(hdc, px, cPyMin[col] + 1, NULL);
			LineTo(hdc, px, iy + ih);
		}
	}

	// FIX [HealthSparkLong]: the min band - a bold, saturated column from the average
	// line down to the worst value in that slice, coloured by the MIN status. Drawn only
	// where the dip is at least ~2px deep so steady periods stay a clean line. This is
	// what makes an overnight RX crash visible: a green averaged line with a red spike
	// stabbing downward exactly where reception collapsed.
	{
		HPEN bandPens[3] = { s_penSparkG, s_penSparkO, s_penSparkR };
		HPEN curBand = NULL;
		for (col = 0; col < iw; col++)
		{
			if (!cOn[col]) continue;
			if (cPyMin[col] - cPy[col] < 2) continue;	/* no meaningful excursion */
			HPEN bp = bandPens[(int)cIdxMn[col]];
			if (bp != curBand) { SelectObject(hdc, bp); curBand = bp; }
			int px = ix + col;
			MoveToEx(hdc, px, cPy[col], NULL);
			LineTo(hdc, px, cPyMin[col] + 1);
		}
	}

	// FIX [HealthSparkThreshold]: OPTIONAL dotted marker line at the mail-alert
	// threshold (System Alerts dialog / [HealthPanel] ThresholdLine, default off),
	// so the trend reads against the alarm level instead of colour flips only.
	// Drawn over the fill, under the data line. Skipped when it would sit on the
	// frame edge (threshold near 0/100).
	if (Profile.nHealthThreshLine)
	{
		int ty = iy + (int)(((100.0 - (double)red) * (ih - 1)) / 100.0 + 0.5);
		if (ty > iy && ty < iy + ih - 1)
		{
			SelectObject(hdc, s_penThresh);
			MoveToEx(hdc, ix, ty, NULL);
			LineTo(hdc, ix + iw, ty);
		}
	}

	/* Bold status line on top (FIX [HealthSparkColor]) - a healthy history is a
	** solid green line, degradation shows as orange/red stretches. Each segment
	** is drawn explicitly from the previous point so it never depends on the GDI
	** current position (the fill pass moved it around). */
	{
		HPEN segPens[3] = { s_penSparkG, s_penSparkO, s_penSparkR };
		HPEN curPen  = NULL;
		BOOL havePrev = FALSE;
		int  prevPx = 0, prevPy = 0;
		for (col = 0; col < iw; col++)
		{
			if (!cOn[col]) { havePrev = FALSE; continue; }
			HPEN sp = segPens[(int)cIdx[col]];
			if (sp != curPen) { SelectObject(hdc, sp); curPen = sp; }
			int px = ix + col, py = cPy[col];
			if (havePrev) { MoveToEx(hdc, prevPx, prevPy, NULL); LineTo(hdc, px, py); }
			else          { MoveToEx(hdc, px, py, NULL); LineTo(hdc, px + 1, py); }	/* lone point: 1px stub so it shows */
			prevPx = px; prevPy = py; havePrev = TRUE;
		}
	}
#undef HP_SPARK_MAXCOLS
}

/* FIX [HealthSparkLong]: format the wall-clock time "ago" seconds before now as HH:MM
** (local time), for the trend tooltip's "lowest X% at HH:MM" annotation. Uses FILETIME
** arithmetic so it stays correct across midnight without pulling in <time.h>. */
static void HP_FormatAgoClock(int ago, char *buf, size_t n)
{
	SYSTEMTIME stNow;  GetLocalTime(&stNow);
	FILETIME   ft;     SystemTimeToFileTime(&stNow, &ft);
	ULARGE_INTEGER u;  u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
	ULONGLONG back = (ULONGLONG)(ago > 0 ? ago : 0) * 10000000ULL;	/* 100 ns units */
	u.QuadPart = (u.QuadPart > back) ? (u.QuadPart - back) : 0;
	ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
	SYSTEMTIME stThen;
	if (FileTimeToSystemTime(&ft, &stThen))
		_snprintf(buf, n - 1, "%02d:%02d", stThen.wHour, stThen.wMinute);
	else
		_snprintf(buf, n - 1, "??:??");
	buf[n - 1] = '\0';
}

void HealthPanel_Draw(HWND hwnd)
{
	HPLAYOUT lo;

	if (!Profile.nHealthPanelVisible)
	{
		if (s_lastValid) { EraseFootprint(hwnd, &s_lastRect); s_lastValid = FALSE; }
		TipParkAll(hwnd);	// FIX [HealthPanelTips3]: clear hover regions + hide tip
		return;
	}

	ComputeLayout(hwnd, &lo);
	if (!lo.fits)
	{
		if (s_lastValid) { EraseFootprint(hwnd, &s_lastRect); s_lastValid = FALSE; }
		TipParkAll(hwnd);	// FIX [HealthPanelTips3]
		return;
	}

	EnsureGdiObjects();
	TipEnsure();	// FIX [HealthPanelTips3]

	int w = lo.rc.right - lo.rc.left;
	int h = lo.rc.bottom - lo.rc.top;

	HDC hdc = GetDC(hwnd);

	/* (Re)create the cached back-buffer when the size changes. */
	if (!s_hdcMem || s_memW != w || s_memH != h)
	{
		FreeMemDC();
		s_hdcMem = CreateCompatibleDC(hdc);
		if (s_hdcMem)
		{
			s_hbmMem = CreateCompatibleBitmap(hdc, w, h);
			if (!s_hbmMem) { DeleteDC(s_hdcMem); s_hdcMem = NULL; }
			else { s_hbmMemOld = (HBITMAP)SelectObject(s_hdcMem, s_hbmMem); s_memW = w; s_memH = h; }
		}
	}
	if (!s_hdcMem)
	{
		// FIX [HealthPanelFitsFail]: back-buffer creation failed (GDI-handle pressure /
		// display churn). ComputeLayout() above already published fits=TRUE, which
		// suppresses the classic corner (needle + RX-Q box) via HealthPanel_Active() -
		// with no panel drawn either, the corner would stay EMPTY for as long as the
		// failure persists. Withdraw the fits claim so the classic corner renders on
		// the next repaint; the back-buffer is retried on every draw anyway.
		s_lastFits = FALSE;
		ReleaseDC(hwnd, hdc);
		if (s_lastValid) { EraseFootprint(hwnd, &s_lastRect); s_lastValid = FALSE; }
		return;
	}

	HDC mem = s_hdcMem;

	/* Background + etched frame (sunken, like the sparkline frame but around
	** the whole panel it reads as a quiet inset strip on the button band). */
	SelectObject(mem, null_pen);
	SelectObject(mem, lgray_brush);
	Rectangle(mem, 0, 0, w + 1, h + 1);
	SelectObject(mem, SysPEN[DARKGRAY]);
	MoveToEx(mem, w - 1, 0, NULL); LineTo(mem, 0, 0); LineTo(mem, 0, h - 1);
	SelectObject(mem, SysPEN[WHITE]);
	LineTo(mem, w - 1, h - 1); LineTo(mem, w - 1, -1);

	int pad    = Scale(4);
	int dot    = Scale(9);	// FIX [HealthDotSize]: 7=too small, 10=too big WITH inner icons; after [HealthDotNoBar] bumped 8->9 (Rob wanted 1-2px larger). Keep in sync with the measure pass.
	int gapDot = Scale(3);
	int gapEnt = Scale(6);
	int cy     = h / 2;
	// FIX [HealthRollupBar]: left-edge accent bar carries the overall status (see below).
	int accW   = (lo.showCom || lo.nFeeds > 0) ? Scale(4) : 0;
	int x      = accW ? (accW + pad) : pad;

	SetBkMode(mem, TRANSPARENT);

	/* FIX [HealthPanelTips3]: hover regions collected while drawing (panel-relative,
	** converted to main-window coords with the lo.rc offset; pure data capture,
	** the tooltip control itself is only driven from the poll timer). */
	char szTip[224];
	RECT rcTip;
#define HP_TIPRECT(x0, x1) { rcTip.left = lo.rc.left + (x0); rcTip.top = lo.rc.top; \
	                         rcTip.right = lo.rc.left + (x1); rcTip.bottom = lo.rc.bottom; }

	/* --- overall-status accent bar (FIX [HealthRollupBar]) --------------
	** A full-height colour strip on the panel's LEFT EDGE, just inside the
	** etched frame. It carries the worst of everything (RX / COM / feeds) as
	** the "one light to glance at", but being part of the panel edge - not an
	** inline dot next to the number - a red bar reads as "the strip has a
	** problem", never as "the RX score is red". Replaces the earlier inline
	** rollup dot ([HealthRollup]), which sat too close to the RX value. */
	if (accW)
	{
		int rsev = ComputeRollup(&lo, szTip, sizeof(szTip));
		HBRUSH br = (rsev == 2) ? s_brRed : (rsev == 1) ? s_brOrange : s_brGreen;
		SelectObject(mem, null_pen);
		SelectObject(mem, br);
		Rectangle(mem, 1, 1, 1 + accW, h - 1);
		/* hover region = the bar plus its trailing pad, so it is easy to hit
		** without ever overlapping the RX score's own region */
		HP_TIPRECT(0, accW + pad);
		TipSet(hwnd, HP_ROLLUP_SLOT, &rcTip, szTip, 0);
	}
	else
	{
		RECT rcOff = { 0, 0, 0, 0 };
		TipSet(hwnd, HP_ROLLUP_SLOT, &rcOff, "", 0);
	}

	/* --- active health score (with a small "RX" caption) --------------- */
	{
		int xEntry = x;

		/* "RX" caption so the bare percentage is unmistakably the RX score
		** (FIX [HealthScoreLabel]). */
		HFONT oldL = (HFONT)SelectObject(mem, pdw_font[FONT_LABELS]);
		SetTextColor(mem, RGB(0, 0, 0));
		SIZE szl; GetTextExtentPoint32(mem, "RX", 2, &szl);
		TextOut(mem, x, cy - szl.cy / 2, "RX", 2);
		x += szl.cx + gapDot;
		SelectObject(mem, oldL);

		char szScore[8];
		int  hstat = Health_GetStatus();
		if (hstat == HSTAT_IDLE) strcpy(szScore, "--%");
		else
		{
			int v = (int)(Health_GetScore() + 0.5);
			if (v > 100) v = 100;
			if (v < 0)   v = 0;
			sprintf(szScore, "%d%%", v);
		}
		HFONT oldF = (HFONT)SelectObject(mem, pdw_font[FONT_RXQUAL]);
		SIZE sz; GetTextExtentPoint32(mem, szScore, (int)strlen(szScore), &sz);
		SetTextColor(mem, StatusColor(hstat));
		TextOut(mem, x, cy - sz.cy / 2, szScore, (int)strlen(szScore));
		SelectObject(mem, oldF);
		/* reserve the full "100%" width so the sparkline doesn't shift */
		int wScore = TextW(mem, pdw_font[FONT_RXQUAL], "100%");

		// FIX [HealthClickConfig]: click the score to open the RX-quality alert config
		_snprintf(szTip, sizeof(szTip) - 1, "Health score%s - source: %s (click to open alert settings)",
		          (hstat == HSTAT_IDLE) ? " (no data yet)" : "",
		          Health_SourceName(Profile.nHealthSource));
		szTip[sizeof(szTip) - 1] = '\0';
		HP_TIPRECT(xEntry, x + wScore);
		TipSet(hwnd, 0, &rcTip, szTip, IDM_RXQUAL_ALERT);

		x += wScore + pad;
	}

	/* --- sparkline ------------------------------------------------------ */
	if (lo.sparkW > 0)
	{
		int sy = Scale(3);
		DrawSparkline(mem, x, sy, lo.sparkW, h - 2 * sy);

		// FIX [HealthSparkLong]: window label reads in hours for the long windows, and
		// the worst dip in the window is appended so the tooltip answers "when did RX
		// crash?" without having to read the tiny graph pixel-by-pixel.
		char szWin[32];
		int wm = Profile.nHealthSparkMin;
		if (wm >= 60 && (wm % 60) == 0)
			_snprintf(szWin, sizeof(szWin) - 1, "%d hour%s", wm / 60, (wm / 60) == 1 ? "" : "s");
		else
			_snprintf(szWin, sizeof(szWin) - 1, "%d minute%s", wm, wm == 1 ? "" : "s");
		szWin[sizeof(szWin) - 1] = '\0';

		char szMin[64];
		if (s_lastSparkMinPct >= 0 && s_lastSparkMinPct < 100)
		{
			char szClk[8];
			HP_FormatAgoClock(s_lastSparkMinAgo, szClk, sizeof(szClk));
			_snprintf(szMin, sizeof(szMin) - 1, " - lowest %d%% at %s", s_lastSparkMinPct, szClk);
			szMin[sizeof(szMin) - 1] = '\0';
		}
		else
			szMin[0] = '\0';

		if (Profile.nHealthThreshLine)
			_snprintf(szTip, sizeof(szTip) - 1, "Health trend - last %s (line = average, band = worst dip; dotted = alert level %d%%)%s",
			          szWin, Profile.nRxQualThreshold, szMin);
		else
			_snprintf(szTip, sizeof(szTip) - 1, "Health trend - last %s (line = average, band = worst dip)%s",
			          szWin, szMin);
		szTip[sizeof(szTip) - 1] = '\0';
		HP_TIPRECT(x, x + lo.sparkW);
		TipSet(hwnd, 1, &rcTip, szTip, 0);

		x += lo.sparkW + pad;
	}
	else
	{
		RECT rcOff = { 0, 0, 0, 0 };
		TipSet(hwnd, 1, &rcOff, "", 0);	/* sparkline dropped: park its hover region */
	}

	/* --- COM link dot ---------------------------------------------------- */
	HFONT oldF = (HFONT)SelectObject(mem, pdw_font[FONT_LABELS]);
	SetTextColor(mem, RGB(0, 0, 0));
	if (lo.showCom)
	{
		int xEntry = x;
		int  link = Rs232LinkState();
		// FIX [HealthDotShapes]: same shape coding as the feed dots (ring = stalled, bar = down)
		DrawStatusDot(mem, x, cy, dot, (link == 2) ? 0 : (link == 1) ? 1 : 2);
		x += dot;
		if (lo.showLabels)
		{
			SIZE sz; GetTextExtentPoint32(mem, "COM", 3, &sz);
			TextOut(mem, x + gapDot, cy - sz.cy / 2, "COM", 3);
			x += gapDot + sz.cx;
		}

		_snprintf(szTip, sizeof(szTip) - 1, "Serial input (COM port): %s (click for interface setup)",
		          (link == 2) ? "open and receiving" : (link == 1) ? "open, but no data coming in" : "not open");
		szTip[sizeof(szTip) - 1] = '\0';
		HP_TIPRECT(xEntry, x);
		TipSet(hwnd, 2, &rcTip, szTip, IDM_INTERFACE);	// FIX [HealthClickConfig]

		x += gapEnt;
	}
	else
	{
		RECT rcOff = { 0, 0, 0, 0 };
		TipSet(hwnd, 2, &rcOff, "", 0);
	}

	/* --- feed dots -------------------------------------------------------- */
	{
		int i;
		for (i = 0; i < lo.nFeeds; i++)
		{
			int xEntry = x;
			int    fs = FeedStatus_Get(lo.feeds[i]);
			// FIX [HealthDotShapes]: ring = retry, disc+bar = error, solid = ok
			DrawStatusDot(mem, x, cy, dot, (fs == FS_ERROR) ? 2 : (fs == FS_RETRY) ? 1 : 0);
			x += dot;
			if (lo.showLabels)
			{
				const char *tag = FeedStatus_Tag(lo.feeds[i]);
				SIZE sz; GetTextExtentPoint32(mem, tag, (int)strlen(tag), &sz);
				TextOut(mem, x + gapDot, cy - sz.cy / 2, tag, (int)strlen(tag));
				x += gapDot + sz.cx;
			}

			BuildFeedTip(lo.feeds[i], szTip, sizeof(szTip));	// FIX [FeedLastError]: state + since + last problem
			// FIX [HealthClickConfig]: append the click hint (BuildFeedTip left room)
			{
				int n = (int)strlen(szTip);
				_snprintf(szTip + n, sizeof(szTip) - 1 - n, " (click to open settings)");
				szTip[sizeof(szTip) - 1] = '\0';
			}
			HP_TIPRECT(xEntry, x);
			TipSet(hwnd, 3 + i, &rcTip, szTip, FeedMenuCmd(lo.feeds[i]));	// FIX [HealthClickConfig]

			x += gapEnt;
		}
		/* park hover regions of feeds that are no longer enabled */
		for (i = lo.nFeeds; i < FEED_COUNT; i++)
		{
			RECT rcOff = { 0, 0, 0, 0 };
			TipSet(hwnd, 3 + i, &rcOff, "", 0);
		}
	}
	SelectObject(mem, oldF);
#undef HP_TIPRECT

	/* --- present ---------------------------------------------------------- */
	if (s_lastValid && (s_lastRect.left != lo.rc.left || s_lastRect.top != lo.rc.top ||
	                    s_lastRect.right != lo.rc.right || s_lastRect.bottom != lo.rc.bottom))
	{
		EraseFootprint(hwnd, &s_lastRect);
	}
	BitBlt(hdc, lo.rc.left, lo.rc.top, w, h, mem, 0, 0, SRCCOPY);

	/* Keep the band divider continuous under the panel's x-span, mirroring
	** DrawSigInd / [RxqSquareDividerClip] (only the PANE1 path draws the full
	** line; hover and per-second repaints never do). */
	SelectObject(hdc, SysPEN[DARKGRAY]);
	MoveToEx(hdc, lo.rc.left, g_cyToolbar, NULL);
	if (Profile.nHealthShowNeedle)
	{
		// FIX [HealthNeedleCombo]: combined layout leaves a gap between the panel's right
		// edge and the needle's far-right slot. Carry the divider across it to the window
		// edge so no sub-gap survives between this span and DrawSigInd's own segment redraw.
		RECT rcCl; GetClientRect(hwnd, &rcCl);
		LineTo(hdc, rcCl.right, g_cyToolbar);
	}
	else
		LineTo(hdc, lo.rc.right + 1, g_cyToolbar);

	ReleaseDC(hwnd, hdc);

	s_lastRect  = lo.rc;
	s_lastValid = TRUE;
}

/* ---------------------------------------------------------------------------
** Context menu (right-click on the panel area, forwarded from the toolbar's
** NM_RCLICK in PDW.cpp). Returns TRUE when the click was on the panel strip.
** ---------------------------------------------------------------------------*/
/* FIX [HealthClickConfig]: left-click on a panel entry opens its config dialog.
** Hit-tests the hover rects captured during the last Draw (main-window client
** coords, same space as the forwarded click) and posts the slot's WM_COMMAND.
** Returns TRUE when a clickable entry was hit (caller suppresses default). */
BOOL HealthPanel_OnToolbarLClick(HWND hMain, POINT ptMainClient)
{
	int i;
	if (!Profile.nHealthPanelVisible || !s_lastValid || !HealthPanel_Active())
		return FALSE;
	for (i = 0; i < HP_TIP_SLOTS; i++)
	{
		if (s_slotCmd[i] && s_tipText[i][0] && PtInRect(&s_tipRect[i], ptMainClient))
		{
			PostMessage(hMain, WM_COMMAND, MAKEWPARAM(s_slotCmd[i], 0), 0);
			return TRUE;
		}
	}
	return FALSE;
}

BOOL HealthPanel_OnToolbarRClick(HWND hMain, POINT ptMainClient)
{
	HPLAYOUT lo;
	ComputeLayout(hMain, &lo);
	if (!lo.fits) return FALSE;

	/* Hit-test against the panel strip; when hidden, the same strip re-shows it. */
	RECT hit = lo.rc;
	if (!PtInRect(&hit, ptMainClient)) return FALSE;

	HMENU hMenu = CreatePopupMenu();
	HMENU hSpark = CreatePopupMenu();
	if (!hMenu || !hSpark)
	{
		if (hMenu)  DestroyMenu(hMenu);
		if (hSpark) DestroyMenu(hSpark);
		return FALSE;
	}

	int src = Profile.nHealthSource;
	AppendMenu(hMenu, MF_STRING | (src == HEALTH_SRC_NEEDLE ? MF_CHECKED : 0), HPM_SRC_NEEDLE, "Health source: RX needle (classic)");
	AppendMenu(hMenu, MF_STRING | (src == HEALTH_SRC_TELNET ? MF_CHECKED : 0), HPM_SRC_TELNET, "Health source: Penalty system");	// FIX [HealthSourceName]: was "Telnet penalty" - confusing, the score has nothing to do with the telnet server being on/off
	AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);

	int mins = Profile.nHealthSparkMin;
	AppendMenu(hSpark, MF_STRING | (mins == 1   ? MF_CHECKED : 0), HPM_SPARK_1,   "1 minute");
	AppendMenu(hSpark, MF_STRING | (mins == 5   ? MF_CHECKED : 0), HPM_SPARK_5,   "5 minutes");
	AppendMenu(hSpark, MF_STRING | (mins == 15  ? MF_CHECKED : 0), HPM_SPARK_15,  "15 minutes");
	AppendMenu(hSpark, MF_STRING | (mins == 60  ? MF_CHECKED : 0), HPM_SPARK_60,  "60 minutes");
	// FIX [HealthSparkLong]: long windows to review the past night; the min/max band
	// keeps short RX dips visible even when one pixel spans several minutes.
	AppendMenu(hSpark, MF_STRING | (mins == 240 ? MF_CHECKED : 0), HPM_SPARK_240, "4 hours");
	AppendMenu(hSpark, MF_STRING | (mins == 480 ? MF_CHECKED : 0), HPM_SPARK_480, "8 hours");
	AppendMenu(hMenu, MF_POPUP | MF_STRING, (UINT_PTR)hSpark, "Trend window");
	AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
	AppendMenu(hMenu, MF_STRING, HPM_TOGGLE_SHOW, Profile.nHealthPanelVisible ? "Hide health panel" : "Show health panel");
	// FIX [HealthNeedleCombo]: third layout - the panel PLUS the classic RX needle back in
	// its old far-right corner slot (the RX % number stays on the panel, the needle sits to
	// its right). Checkable; picking it while the panel is hidden also shows the panel, since
	// the needle companion has no meaning on its own.
	AppendMenu(hMenu, MF_STRING | (Profile.nHealthShowNeedle ? MF_CHECKED : 0), HPM_TOGGLE_NEEDLE, "Show RX needle alongside");

	POINT ptScreen;
	GetCursorPos(&ptScreen);
	int cmd = (int)TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
	                              ptScreen.x, ptScreen.y, 0, hMain, NULL);
	DestroyMenu(hMenu);	/* also destroys the submenu */

	BOOL bChanged = FALSE;
	switch (cmd)
	{
		case HPM_SRC_NEEDLE:
		case HPM_SRC_TELNET:
		{
			int newSrc = (cmd == HPM_SRC_TELNET) ? HEALTH_SRC_TELNET : HEALTH_SRC_NEEDLE;
			if (newSrc != Profile.nHealthSource)
			{
				Profile.nHealthSource = newSrc;
				// FIX [HealthSource]: the mail alert reads the same source; re-arm its
				// cold-start/hysteresis state and clear the trend (mixing two formulas
				// in one sparkline reads as a fake signal change).
				RxQualMonitor_Reset();
				s_histHead = s_histCount = 0;
				bChanged = TRUE;
			}
			break;
		}

		case HPM_SPARK_1:   Profile.nHealthSparkMin = 1;   bChanged = TRUE; break;
		case HPM_SPARK_5:   Profile.nHealthSparkMin = 5;   bChanged = TRUE; break;
		case HPM_SPARK_15:  Profile.nHealthSparkMin = 15;  bChanged = TRUE; break;
		case HPM_SPARK_60:  Profile.nHealthSparkMin = 60;  bChanged = TRUE; break;
		case HPM_SPARK_240: Profile.nHealthSparkMin = 240; bChanged = TRUE; break;	// FIX [HealthSparkLong]
		case HPM_SPARK_480: Profile.nHealthSparkMin = 480; bChanged = TRUE; break;	// FIX [HealthSparkLong]

		case HPM_TOGGLE_SHOW:
		Profile.nHealthPanelVisible = !Profile.nHealthPanelVisible;
		bChanged = TRUE;
		break;

		case HPM_TOGGLE_NEEDLE:
		// FIX [HealthNeedleCombo]: flip the combined layout. Turning it on while the panel
		// is hidden also shows the panel - the needle companion is meaningless on its own.
		Profile.nHealthShowNeedle = !Profile.nHealthShowNeedle;
		if (Profile.nHealthShowNeedle && !Profile.nHealthPanelVisible)
			Profile.nHealthPanelVisible = 1;
		bChanged = TRUE;
		break;
	}

	if (bChanged)
	{
		WriteSettings();
		// FIX [HealthNeedleCombo]: HPM_TOGGLE_NEEDLE also changes which corner pieces draw
		// (needle in/out) and the panel width, so it needs the same full corner swap.
		if (cmd == HPM_TOGGLE_SHOW || cmd == HPM_TOGGLE_NEEDLE)
		{
			// FIX [HealthPanelCorner]: full corner swap. Synchronously repaint the toolbar
			// band first (erases leftovers of whichever layout just went away - needle box,
			// warning square, or panel pixels), THEN draw the now-active layout. Both
			// classic pieces gate themselves on HealthPanel_Active(), so the calls below
			// are safe in either direction.
			if (hToolbar) RedrawWindow(hToolbar, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
		}
		HealthPanel_Draw(hMain);	/* immediate visual feedback (also erases when hidden) */
		DrawSigInd(hMain);			/* classic meter back - no-op while the panel is active */
		DrawTitleBarGfx(hMain);		/* PANE1 header width + RX-Q box follow the new state */
	}
	return TRUE;
}

/* ---------------------------------------------------------------------------
** Lifecycle
** ---------------------------------------------------------------------------*/
void HealthPanel_OnDisplayChange(void)
{
	/* Display driver swap (RDP) invalidates the compatible bitmap; a DPI
	** change invalidates the Scale()d pen widths. Drop both; they are
	** rebuilt lazily on the next draw. Mirrors [DisplayBitmapReload]. */
	FreeMemDC();
	s_gdiDpi = 0;
}

void HealthPanel_Free(void)
{
	FreeMemDC();
	// FIX [HealthPanelTips3]: our own timer + tooltip window. If the main window
	// is already gone the owned tooltip was destroyed with it (IsWindow guard).
	if (s_tipTimer) { if (ghWnd) KillTimer(ghWnd, HP_TIP_TIMER_ID); s_tipTimer = 0; }
	if (s_hTip)
	{
		if (IsWindow(s_hTip)) DestroyWindow(s_hTip);
		s_hTip = NULL;
	}
	s_tipToolAdded = FALSE;
	s_tipSlot = s_hoverSlot = s_tipMutedSlot = -1;
	s_tipDwell = s_tipShownTicks = 0;
	if (s_penSparkG) { DeleteObject(s_penSparkG); s_penSparkG = NULL; }
	if (s_penSparkO) { DeleteObject(s_penSparkO); s_penSparkO = NULL; }
	if (s_penSparkR) { DeleteObject(s_penSparkR); s_penSparkR = NULL; }
	if (s_penThresh) { DeleteObject(s_penThresh); s_penThresh = NULL; }
	if (s_penFillG)  { DeleteObject(s_penFillG);  s_penFillG  = NULL; }	// FIX [HealthSparkFill]
	if (s_penFillO)  { DeleteObject(s_penFillO);  s_penFillO  = NULL; }
	if (s_penFillR)  { DeleteObject(s_penFillR);  s_penFillR  = NULL; }
	if (s_brGreen)   { DeleteObject(s_brGreen);   s_brGreen   = NULL; }
	if (s_brOrange)  { DeleteObject(s_brOrange);  s_brOrange  = NULL; }
	if (s_brRed)     { DeleteObject(s_brRed);     s_brRed     = NULL; }
	s_gdiDpi = 0;
}
