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
** the maximum trend window (60 min). Negative = "no measurement" gap.
** ---------------------------------------------------------------------------*/
#define HP_HIST_MAX   3600
#define HP_SPARK_GAP  -1.0f

static float s_hist[HP_HIST_MAX];
static int   s_histHead  = 0;      /* next write position */
static int   s_histCount = 0;

/* Cached GDI objects (GUI thread only). Recreated when DPI/display changes. */
static HDC     s_hdcMem   = NULL;
static HBITMAP s_hbmMem   = NULL;
static HBITMAP s_hbmMemOld= NULL;
static int     s_memW = 0, s_memH = 0;
static HPEN    s_penGreen = NULL, s_penOrange = NULL, s_penRed = NULL, s_penGray = NULL;
/* FIX [HealthSparkColor]: the trend line is drawn in status colours (green/orange/red
** per segment) with a bolder stroke so a healthy period clearly reads as a green line
** (was a thin neutral gray that disappeared against the band). */
static HPEN    s_penSparkG = NULL, s_penSparkO = NULL, s_penSparkR = NULL;
/* FIX [HealthSparkThreshold]: dotted marker at the mail-alert level in the sparkline.
** FIX [HealthDotShapes]: ring pen (retry) + white bar pen (error) for the status dots. */
static HPEN    s_penThresh = NULL, s_penRingO = NULL, s_penWhiteBar = NULL;
static HBRUSH  s_brGreen  = NULL, s_brOrange = NULL, s_brRed = NULL, s_brGray = NULL;
static UINT    s_gdiDpi   = 0;     /* DPI the pens were created for */

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
#define HP_TIP_SLOTS          (3 + FEED_COUNT)   /* 0=score, 1=spark, 2=COM, 3+i=feed i */
#define HP_TIP_TIMER_ID       0xE210             /* far above PDW's timer ids 101..107 */
#define HP_TIP_POLL_MS        150
#define HP_TIP_DWELL_TICKS    3                  /* ~450 ms hover before showing */
#define HP_TIP_AUTOPOP_TICKS  40                 /* ~6 s visible, then hide until re-hover */
static HWND     s_hTip = NULL;                   /* our own tracking tooltip */
static BOOL     s_tipToolAdded = FALSE;
static UINT_PTR s_tipTimer = 0;
static char     s_tipText[HP_TIP_SLOTS][224];    /* per-entry text, filled during Draw */
static RECT     s_tipRect[HP_TIP_SLOTS];         /* main-window client coords; empty = off */
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

/* Palette (ORANGE/RED still match rgbColor[] in Gfx.cpp). FIX [HealthGreenLighter]:
** GREEN is intentionally lighter/softer than rgbColor[]'s message-text GREEN (0,128,0) -
** that dark a green read as too "heavy" for a status dot/score that's on screen
** constantly; the message-color palette itself is untouched (Rob). */
#define HP_RGB_GREEN   RGB(40, 167, 69)
#define HP_RGB_ORANGE  RGB(255, 165, 0)
#define HP_RGB_RED     RGB(255, 0, 0)
#define HP_RGB_GRAY    RGB(138, 138, 138)

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
	if (s_penRingO)  { DeleteObject(s_penRingO);  s_penRingO  = NULL; }
	if (s_penWhiteBar) { DeleteObject(s_penWhiteBar); s_penWhiteBar = NULL; }
	if (s_penGreen)  { DeleteObject(s_penGreen);  s_penGreen  = NULL; }
	if (s_penOrange) { DeleteObject(s_penOrange); s_penOrange = NULL; }
	if (s_penRed)    { DeleteObject(s_penRed);    s_penRed    = NULL; }
	if (s_penGray)   { DeleteObject(s_penGray);   s_penGray   = NULL; }
	if (s_brGreen)   { DeleteObject(s_brGreen);   s_brGreen   = NULL; }
	if (s_brOrange)  { DeleteObject(s_brOrange);  s_brOrange  = NULL; }
	if (s_brRed)     { DeleteObject(s_brRed);     s_brRed     = NULL; }
	if (s_brGray)    { DeleteObject(s_brGray);    s_brGray    = NULL; }

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
	s_penThresh = CreatePen(PS_DOT, 1, HP_RGB_GRAY);
	// FIX [HealthDotShapes]: ring stroke matches the spark stroke; the white bar
	// stays thin so the red disc keeps reading as red
	s_penRingO   = CreatePen(PS_SOLID, pwSpark, HP_RGB_ORANGE);
	s_penWhiteBar = CreatePen(PS_SOLID, pw, RGB(255, 255, 255));
	s_penGreen  = CreatePen(PS_SOLID, pw, HP_RGB_GREEN);
	s_penOrange = CreatePen(PS_SOLID, pw, HP_RGB_ORANGE);
	s_penRed    = CreatePen(PS_SOLID, pw, HP_RGB_RED);
	s_penGray   = CreatePen(PS_SOLID, pw, HP_RGB_GRAY);
	s_brGreen   = CreateSolidBrush(HP_RGB_GREEN);
	s_brOrange  = CreateSolidBrush(HP_RGB_ORANGE);
	s_brRed     = CreateSolidBrush(HP_RGB_RED);
	s_brGray    = CreateSolidBrush(HP_RGB_GRAY);
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
		HMONITOR hMon = MonitorFromPoint(ptScr, MONITOR_DEFAULTTONEAREST);
		if (hMon && GetMonitorInfo(hMon, &mi))
		{
			if (tx + tw > mi.rcWork.right)    tx = mi.rcWork.right - tw - Scale(2); /* never off the right */
			if (tx < mi.rcWork.left)          tx = mi.rcWork.left;
			if (ty + th > mi.rcWork.bottom)   ty = ptScr.y - th - Scale(8);
			if (ty < mi.rcWork.top)           ty = mi.rcWork.top;
		}
		// Diagnostic (silent unless the debug channel is enabled): confirms the
		// measured width now tracks the current entry, not the previous one.
		PDW_DLOG("HealthTip slot=%d pt=%d,%d tw=%d th=%d tx=%d ty=%d workR=%d",
		         slot, ptScr.x, ptScr.y, tw, th, tx, ty,
		         (hMon ? (int)mi.rcWork.right : -1));
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
** store - the tooltip control is never touched from the draw path. */
static void TipSet(HWND hMain, int slot, const RECT *rcMain, const char *text)
{
	(void)hMain;
	if (slot < 0 || slot >= HP_TIP_SLOTS) return;
	_snprintf(s_tipText[slot], sizeof(s_tipText[slot]) - 1, "%s", text ? text : "");
	s_tipText[slot][sizeof(s_tipText[slot]) - 1] = '\0';
	s_tipRect[slot] = *rcMain;
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
	}
	TipHide();
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
	int avail = right - left;

	/* Measure content (against the main-window DC with the shared fonts). */
	HDC hdc = GetDC(hwnd);
	int wScore = TextW(hdc, pdw_font[FONT_RXQUAL], "100%");
	int pad    = Scale(4);
	int dot    = Scale(8);	// FIX [HealthDotSize]: was Scale(7, too small), then Scale(10, too big) - 8 is the sweet spot (Rob)
	int gapDot = Scale(3);
	int gapEnt = Scale(6);

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
	int fixed = pad + wScore + pad;                 /* score block */
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

/* One solid status dot. */
static void DrawDot(HDC hdc, int x, int cy, int d, HPEN pen, HBRUSH fill)
{
	SelectObject(hdc, pen);
	SelectObject(hdc, fill);
	Ellipse(hdc, x, cy - d / 2, x + d, cy - d / 2 + d);
}

/* FIX [HealthDotShapes]: status dots are no longer colour-only - RETRY draws as
** a RING (hollow) and ERROR as a solid disc with a light "minus" bar, so the
** three states stay distinguishable for colour-blind users. Used by the feed
** dots and the COM link dot alike. sev: 0 = ok, 1 = retry/stalled, 2 = error.
** FIX [HealthDotGreenStart]: an enabled feed with no outcome yet (FS_UNKNOWN) is
** shown SOLID green - "configured and no known problem" should read healthy from
** the first second, because the per-message feeds (Telegram, Pushover, webhook,
** SMTP) can legitimately go days before their first delivery. A problem changes
** the dot within seconds of the first failing attempt anyway. */
static void DrawStatusDot(HDC hdc, int x, int cy, int d, int sev)
{
	switch (sev)
	{
		case 1:  /* retry: orange ring, band background shows through the centre */
		SelectObject(hdc, s_penRingO);
		SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
		Ellipse(hdc, x, cy - d / 2, x + d, cy - d / 2 + d);
		break;

		case 2:  /* error: solid red disc with a white "minus" bar */
		DrawDot(hdc, x, cy, d, s_penRed, s_brRed);
		{
			int inset = d / 3;
			SelectObject(hdc, s_penWhiteBar);
			MoveToEx(hdc, x + inset, cy, NULL);
			LineTo(hdc, x + d - inset, cy);
		}
		break;

		default: /* ok / unknown: solid green */
		DrawDot(hdc, x, cy, d, s_penGreen, s_brGreen);
		break;
	}
}

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

	// FIX [HealthSparkThreshold]: OPTIONAL dotted marker line at the mail-alert
	// threshold (System Alerts dialog / [HealthPanel] ThresholdLine, default off),
	// so the trend reads against the alarm level instead of colour flips only.
	// Drawn first; the data line paints over it. Skipped when it would sit on the
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

	/* Newest sample at the right edge. One column = windowSec/iw seconds,
	** averaged; gaps (no measurement) break the polyline. The line is drawn in
	** status colours per segment (FIX [HealthSparkColor]) - a healthy history
	** is a solid green line, degradation shows as orange/red stretches. */
	BOOL penDown = FALSE;
	HPEN curPen  = NULL;
	int  col;
	for (col = 0; col < iw; col++)
	{
		int aFrom = (int)(( (double)(iw - 1 - col)     * windowSec) / iw);
		int aTo   = (int)(( (double)(iw - col)         * windowSec) / iw);
		if (aTo <= aFrom) aTo = aFrom + 1;

		double sum = 0.0; int n = 0, a;
		for (a = aFrom; a < aTo; a++)
		{
			float v = HistAt(a);
			if (v >= 0.0f) { sum += v; n++; }
		}
		if (!n) { penDown = FALSE; continue; }

		double avg = sum / n;
		if (avg < 0.0)   avg = 0.0;
		if (avg > 100.0) avg = 100.0;

		HPEN segPen = (avg < (double)red) ? s_penSparkR
		            : (avg >= 96.0)       ? s_penSparkG
		                                  : s_penSparkO;
		if (segPen != curPen) { SelectObject(hdc, segPen); curPen = segPen; }	/* keeps the current position */

		int py = iy + (int)(((100.0 - avg) * (ih - 1)) / 100.0 + 0.5);
		int px = ix + col;

		if (penDown) LineTo(hdc, px, py);
		else         MoveToEx(hdc, px, py, NULL);
		penDown = TRUE;
	}
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
	int dot    = Scale(8);	// FIX [HealthDotSize]: was Scale(7, too small), then Scale(10, too big) - 8 is the sweet spot (Rob)
	int gapDot = Scale(3);
	int gapEnt = Scale(6);
	int cy     = h / 2;
	int x      = pad;

	SetBkMode(mem, TRANSPARENT);

	/* FIX [HealthPanelTips3]: hover regions collected while drawing (panel-relative,
	** converted to main-window coords with the lo.rc offset; pure data capture,
	** the tooltip control itself is only driven from the poll timer). */
	char szTip[224];
	RECT rcTip;
#define HP_TIPRECT(x0, x1) { rcTip.left = lo.rc.left + (x0); rcTip.top = lo.rc.top; \
	                         rcTip.right = lo.rc.left + (x1); rcTip.bottom = lo.rc.bottom; }

	/* --- active health score ------------------------------------------- */
	{
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

		_snprintf(szTip, sizeof(szTip) - 1, "Health score%s - source: %s",
		          (hstat == HSTAT_IDLE) ? " (no data yet)" : "",
		          Health_SourceName(Profile.nHealthSource));
		szTip[sizeof(szTip) - 1] = '\0';
		HP_TIPRECT(x, x + wScore);
		TipSet(hwnd, 0, &rcTip, szTip);

		x += wScore + pad;
	}

	/* --- sparkline ------------------------------------------------------ */
	if (lo.sparkW > 0)
	{
		int sy = Scale(3);
		DrawSparkline(mem, x, sy, lo.sparkW, h - 2 * sy);

		if (Profile.nHealthThreshLine)
			_snprintf(szTip, sizeof(szTip) - 1, "Health trend - last %d minute%s (dotted line = alert level %d%%)",
			          Profile.nHealthSparkMin, Profile.nHealthSparkMin == 1 ? "" : "s", Profile.nRxQualThreshold);
		else
			_snprintf(szTip, sizeof(szTip) - 1, "Health trend - last %d minute%s (green = healthy, orange = degraded, red = alert level)",
			          Profile.nHealthSparkMin, Profile.nHealthSparkMin == 1 ? "" : "s");
		szTip[sizeof(szTip) - 1] = '\0';
		HP_TIPRECT(x, x + lo.sparkW);
		TipSet(hwnd, 1, &rcTip, szTip);

		x += lo.sparkW + pad;
	}
	else
	{
		RECT rcOff = { 0, 0, 0, 0 };
		TipSet(hwnd, 1, &rcOff, "");	/* sparkline dropped: park its hover region */
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

		_snprintf(szTip, sizeof(szTip) - 1, "Serial input (COM port): %s",
		          (link == 2) ? "open and receiving" : (link == 1) ? "open, but no data coming in" : "not open");
		szTip[sizeof(szTip) - 1] = '\0';
		HP_TIPRECT(xEntry, x);
		TipSet(hwnd, 2, &rcTip, szTip);

		x += gapEnt;
	}
	else
	{
		RECT rcOff = { 0, 0, 0, 0 };
		TipSet(hwnd, 2, &rcOff, "");
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
			HP_TIPRECT(xEntry, x);
			TipSet(hwnd, 3 + i, &rcTip, szTip);

			x += gapEnt;
		}
		/* park hover regions of feeds that are no longer enabled */
		for (i = lo.nFeeds; i < FEED_COUNT; i++)
		{
			RECT rcOff = { 0, 0, 0, 0 };
			TipSet(hwnd, 3 + i, &rcOff, "");
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
	LineTo(hdc, lo.rc.right + 1, g_cyToolbar);

	ReleaseDC(hwnd, hdc);

	s_lastRect  = lo.rc;
	s_lastValid = TRUE;
}

/* ---------------------------------------------------------------------------
** Context menu (right-click on the panel area, forwarded from the toolbar's
** NM_RCLICK in PDW.cpp). Returns TRUE when the click was on the panel strip.
** ---------------------------------------------------------------------------*/
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
	AppendMenu(hSpark, MF_STRING | (mins == 1  ? MF_CHECKED : 0), HPM_SPARK_1,  "1 minute");
	AppendMenu(hSpark, MF_STRING | (mins == 5  ? MF_CHECKED : 0), HPM_SPARK_5,  "5 minutes");
	AppendMenu(hSpark, MF_STRING | (mins == 15 ? MF_CHECKED : 0), HPM_SPARK_15, "15 minutes");
	AppendMenu(hSpark, MF_STRING | (mins == 60 ? MF_CHECKED : 0), HPM_SPARK_60, "60 minutes");
	AppendMenu(hMenu, MF_POPUP | MF_STRING, (UINT_PTR)hSpark, "Trend window");
	AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
	AppendMenu(hMenu, MF_STRING, HPM_TOGGLE_SHOW, Profile.nHealthPanelVisible ? "Hide health panel" : "Show health panel");

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

		case HPM_SPARK_1:  Profile.nHealthSparkMin = 1;  bChanged = TRUE; break;
		case HPM_SPARK_5:  Profile.nHealthSparkMin = 5;  bChanged = TRUE; break;
		case HPM_SPARK_15: Profile.nHealthSparkMin = 15; bChanged = TRUE; break;
		case HPM_SPARK_60: Profile.nHealthSparkMin = 60; bChanged = TRUE; break;

		case HPM_TOGGLE_SHOW:
		Profile.nHealthPanelVisible = !Profile.nHealthPanelVisible;
		bChanged = TRUE;
		break;
	}

	if (bChanged)
	{
		WriteSettings();
		if (cmd == HPM_TOGGLE_SHOW)
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
	if (s_penRingO)  { DeleteObject(s_penRingO);  s_penRingO  = NULL; }
	if (s_penWhiteBar) { DeleteObject(s_penWhiteBar); s_penWhiteBar = NULL; }
	if (s_penGreen)  { DeleteObject(s_penGreen);  s_penGreen  = NULL; }
	if (s_penOrange) { DeleteObject(s_penOrange); s_penOrange = NULL; }
	if (s_penRed)    { DeleteObject(s_penRed);    s_penRed    = NULL; }
	if (s_penGray)   { DeleteObject(s_penGray);   s_penGray   = NULL; }
	if (s_brGreen)   { DeleteObject(s_brGreen);   s_brGreen   = NULL; }
	if (s_brOrange)  { DeleteObject(s_brOrange);  s_brOrange  = NULL; }
	if (s_brRed)     { DeleteObject(s_brRed);     s_brRed     = NULL; }
	if (s_brGray)    { DeleteObject(s_brGray);    s_brGray    = NULL; }
	s_gdiDpi = 0;
}
