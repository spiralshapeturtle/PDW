//
// Sigind.cpp
//
// This file contains functions for displaying/updating
// the signal indicator.
//
#ifndef STRICT
#define STRICT 1
#endif

#include <windows.h>
#include <commctrl.h>
#include <mmsystem.h>
#include <stdio.h>
#include <commdlg.h>
#include <string.h>
#include <time.h>


#include "headers\resource.h"
#include "headers\PDW.h"
#include "headers\slicer.h"
#include "headers\toolbar.h"
#include "headers\gfx.h"
#include "headers\initapp.h"
#include "headers\sigind.h"
#include "HealthPanel.h"	// FIX [HealthPanelCorner]: panel replaces the meter while active

#define MAX_SI_POS        20	// 0-20. Max positions available to signal indicator.
#define AUDIO_POINT_VALUE 2	// Used for working out samples per signal
										// position.e.g. sample=20/10, position=2.

// FIX [SigindFlatGauge]: logical (96 dpi) footprint of the meter on the toolbar. The face
// bitmap (GFX\sigind.bmp) is rendered at 4x (128x96) and HALFTONE-downscaled into this box,
// so the toolbar layout is unchanged while the gauge stays crisp at any DPI. The needle
// coordinate table (sip[]) below is expressed in this same 32x24 logical space.
#define SIGIND_LOGW 32
#define SIGIND_LOGH 24

BOOL old_rect_flg=FALSE;
BOOL got_sigind=FALSE;

// FIX [DpiScale]/[SigindFlatGauge]: DPI-scaled needle objects - created in InitSigIndPens()
// once g_dpi is known.
HPEN   hPenNeedleRed   = NULL;	// red needle
HBRUSH hbrNeedleHub    = NULL;	// red pivot hub

// FIX [SigindFlatGauge]: cached off-screen back-buffer. The needle is redrawn MANY times a
// second from the decode/audio path, and it overlaps the arc. Overpainting the old needle
// (the previous approach) nibbled the arc thinner/grainier on every sweep. Instead we keep a
// clean pre-scaled copy of the face (hdcGaugeFace) and, per update, blit it into a compositing
// buffer (hdcGaugeBB), draw the needle on top, and present the buffer in one BitBlt. The arc
// is copied fresh every frame, so it stays crisp forever and there is no flicker. Buffers are
// (re)built lazily when the DPI-scaled size changes and torn down on DPI/display change.
HDC     hdcGaugeFace = NULL;	// holds the clean, pre-scaled meter face
HDC     hdcGaugeBB   = NULL;	// compositing back-buffer (face + needle)
HBITMAP hbmGaugeFace = NULL, hbmGaugeFaceOld = NULL;
HBITMAP hbmGaugeBB   = NULL, hbmGaugeBBOld   = NULL;
int     gaugeBBW = 0, gaugeBBH = 0;	// size the buffers were built for

HBITMAP hbm_sigind=NULL;
BITMAP bms;
RECT old_rect;     // used for redrawing signal indicator bitmap
RECT sig_rect;     // used for holding current position of sigind bitmap

// Keep track of signal indicator.
int si_index=0;
int si_old_index=0;
int delay_cnt=0;
int si_low_hover=0;
int si_hi_hover=0;

// Points used for drawing signal indicator.

// FIX [SigindFlatGauge]: needle endpoints on a TRUE circular arc (pivot 16,18, swept
// 153deg->27deg over the top) so the sweep matches the arc face. Index 0 = resting (up-left,
// as shown on the toolbar), index 20 = full (up-right). Coordinates are in the 32x24 logical
// space (SIGIND_LOGW x SIGIND_LOGH); show_sigind() applies Scale() + the meter offset.
// FIX [SigindWiggleVisibility]: testers reported the at-rest jitter (noise detected, no message
// - UpdateSigInd bouncing the needle around the low indices) was harder to see than on the
// classic meter. Two regressions caused that: (1) the redesigned needle got shorter (tip radius
// ~9) and (2) its low-index steps were evenly compressed, so index 0->1 moved only ~1px. Restore
// the classic character by porting the original hand-tuned angles (bf7cc64) onto the new pivot at
// radius 10 (fills the enlarged arc): a deliberate wider gap between rest(0) and 1 makes the
// needle visibly SNAP out of rest (0->1 is now ~2.2px, was ~1px), matching the old feel. The arc
// face and UpdateSigInd dynamics are unchanged; this is geometry only.
int sip[21][4] =	{//from: x,  y, To: x,  y
							{16, 18,  7, 14},
							{16, 18,  8, 12},
							{16, 18,  9, 11},
							{16, 18, 10, 10},
							{16, 18, 11, 10},
							{16, 18, 11,  9},
							{16, 18, 12,  9},
							{16, 18, 13,  8},
							{16, 18, 14,  8},
							{16, 18, 15,  8},
							{16, 18, 17,  8},
							{16, 18, 18,  8},
							{16, 18, 19,  9},
							{16, 18, 20,  9},
							{16, 18, 21,  9},
							{16, 18, 21, 10},
							{16, 18, 22, 10},
							{16, 18, 23, 11},
							{16, 18, 24, 12},
							{16, 18, 24, 13},
							{16, 18, 25, 14},
					};

// FIX [SigindFlatGauge]: tear down the cached back-buffer (on DPI/display change or shutdown).
void FreeGaugeBuffers(void)
{
	if (hdcGaugeFace) { if (hbmGaugeFaceOld) SelectObject(hdcGaugeFace, hbmGaugeFaceOld); DeleteDC(hdcGaugeFace); hdcGaugeFace = NULL; }
	if (hdcGaugeBB)   { if (hbmGaugeBBOld)   SelectObject(hdcGaugeBB,   hbmGaugeBBOld);   DeleteDC(hdcGaugeBB);   hdcGaugeBB   = NULL; }
	if (hbmGaugeFace) { DeleteObject(hbmGaugeFace); hbmGaugeFace = NULL; }
	if (hbmGaugeBB)   { DeleteObject(hbmGaugeBB);   hbmGaugeBB   = NULL; }
	hbmGaugeFaceOld = hbmGaugeBBOld = NULL;
	gaugeBBW = gaugeBBH = 0;
}

// FIX [SigindFlatGauge]: (re)build the back-buffer at bw x bh and render the clean, HALFTONE-
// downscaled meter face into hdcGaugeFace ONCE. Returns FALSE on any GDI failure.
static BOOL BuildGaugeBuffers(HDC hdcRef, int bw, int bh)
{
	HDC srcdc;

	FreeGaugeBuffers();
	if (bw < 1 || bh < 1 || !got_sigind) return FALSE;

	hdcGaugeFace = CreateCompatibleDC(hdcRef);
	hdcGaugeBB   = CreateCompatibleDC(hdcRef);
	hbmGaugeFace = CreateCompatibleBitmap(hdcRef, bw, bh);
	hbmGaugeBB   = CreateCompatibleBitmap(hdcRef, bw, bh);
	if (!hdcGaugeFace || !hdcGaugeBB || !hbmGaugeFace || !hbmGaugeBB) { FreeGaugeBuffers(); return FALSE; }

	hbmGaugeFaceOld = (HBITMAP)SelectObject(hdcGaugeFace, hbmGaugeFace);
	hbmGaugeBBOld   = (HBITMAP)SelectObject(hdcGaugeBB,   hbmGaugeBB);

	// downscale the hi-res face into the clean-face buffer (once, HALFTONE = high quality)
	if (srcdc = CreateCompatibleDC(hdcRef))
	{
		HBITMAP oldsrc = (HBITMAP)SelectObject(srcdc, hbm_sigind);
		SetStretchBltMode(hdcGaugeFace, HALFTONE);
		SetBrushOrgEx(hdcGaugeFace, 0, 0, NULL);
		StretchBlt(hdcGaugeFace, 0, 0, bw, bh, srcdc, 0, 0, bms.bmWidth, bms.bmHeight, SRCCOPY);
		SelectObject(srcdc, oldsrc);
		DeleteDC(srcdc);
	}
	else
	{
		// FIX [GaugeSrcdcFail]: without this, a failed CreateCompatibleDC skipped the
		// face StretchBlt but the size was still cached and TRUE returned - PaintGauge
		// then presented (and kept presenting) an uninitialized bitmap as the meter
		// face. Tear down and report failure so the next paint retries the build.
		FreeGaugeBuffers();
		return FALSE;
	}
	gaugeBBW = bw; gaugeBBH = bh;
	return TRUE;
}

// FIX [SigindFlatGauge]: paint the complete gauge (clean face + needle at pos + hub) via the
// back-buffer in a single BitBlt. The arc is copied fresh from hdcGaugeFace every call, so the
// moving needle can never damage it, and the single present is flicker-free.
void PaintGauge(int pos)
{
	int bw = Scale(SIGIND_LOGW), bh = Scale(SIGIND_LOGH);
	int fx, fy, tx, ty, hr;
	HDC hdc;

	// FIX [HealthPanelCorner]: the Health panel replaces the meter while active; this is
	// the high-frequency needle path (decode/audio), so the gate is two static reads.
	if (HealthPanel_Active()) return;
	if (!got_sigind) return;
	if (pos < 0) pos = 0; else if (pos > MAX_SI_POS) pos = MAX_SI_POS;
	if (!(hdc = GetDC(ghWnd))) return;

	if (!hdcGaugeBB || gaugeBBW != bw || gaugeBBH != bh)
	{
		if (!BuildGaugeBuffers(hdc, bw, bh)) { ReleaseDC(ghWnd, hdc); return; }
	}

	// clean face -> compositing buffer
	BitBlt(hdcGaugeBB, 0, 0, bw, bh, hdcGaugeFace, 0, 0, SRCCOPY);

	// red needle on top
	SelectObject(hdcGaugeBB, hPenNeedleRed ? hPenNeedleRed : SysPEN[RED]);
	fx = Scale(sip[pos][0]); fy = Scale(sip[pos][1]);
	tx = Scale(sip[pos][2]); ty = Scale(sip[pos][3]);
	MoveToEx(hdcGaugeBB, fx, fy, NULL);
	LineTo(hdcGaugeBB, tx, ty);

	// red pivot hub
	hr = Scale(2); if (hr < 1) hr = 1;
	SelectObject(hdcGaugeBB, null_pen);
	if (hbrNeedleHub) SelectObject(hdcGaugeBB, hbrNeedleHub);
	Ellipse(hdcGaugeBB, fx - hr, fy - hr, fx + hr + 1, fy + hr + 1);

	// present in one blit
	BitBlt(hdc, sig_rect.left, sig_rect.top, bw, bh, hdcGaugeBB, 0, 0, SRCCOPY);
	ReleaseDC(ghWnd, hdc);
}


// Get signal indicator bitmap resource
BOOL LoadSigInd(HINSTANCE hThisInstance)
{
	if (!(got_sigind))
	{
		if (hbm_sigind = LoadBitmap(hThisInstance,MAKEINTRESOURCE((WORD)IDS_SIGIND)))
		{
			if (GetObject(hbm_sigind, sizeof(bms), &bms)) got_sigind=TRUE;
			else DeleteObject(hbm_sigind);
		}
	}
	return(got_sigind);
}

// FIX [DisplayBitmapReload]: hbm_sigind is a LoadBitmap DDB (device-dependent bitmap). An RDP
// connect/disconnect swaps the display driver and invalidates the DDB, so from then on the meter
// face StretchBlt silently draws nothing/garbage - and a window resize does NOT heal it (it just
// re-blits the dead DDB); only a restart, which reloads the bitmap for the new device, fixed it.
// Recreate the bitmap for the CURRENT display on WM_DISPLAYCHANGE. Same reason [ToolbarHiResIcons]
// moved the toolbar buttons off LoadBitmap.
void ReloadSigInd(HINSTANCE hThisInstance)
{
	if (got_sigind || hbm_sigind) { DeleteObject(hbm_sigind); hbm_sigind = NULL; got_sigind = FALSE; }
	FreeGaugeBuffers();	// FIX [SigindFlatGauge]: drop the cached face (built from the old DDB); rebuilt on next paint
	LoadSigInd(hThisInstance);	// reloads + GetObject(bms) + sets got_sigind
}

// Free bitmap object
void FreeSigInd(void)
{
	if (got_sigind) DeleteObject(hbm_sigind);
	FreeGaugeBuffers();	// FIX [SigindFlatGauge]
	if (hPenNeedleRed)   { DeleteObject(hPenNeedleRed);   hPenNeedleRed   = NULL; }	// FIX [DpiScale]
	if (hbrNeedleHub)    { DeleteObject(hbrNeedleHub);    hbrNeedleHub    = NULL; }	// FIX [SigindFlatGauge]
}

// FIX [DpiScale]: maak pennen aan met DPI-proportionele dikte (1px op 96dpi, 2px op 150%+).
// Aanroepen na g_dpi bekend is (WM_CREATE), niet in LoadSigInd (die loopt vóór WM_CREATE).
void InitSigIndPens(void)
{
	int pw = Scale(2);	// FIX [SigindFlatGauge]: slightly bolder needle (was Scale(1)) to match the new face
	if (pw < 1) pw = 1;
	// FIX [SigindPenFreeOrder]: tear down the back-buffer FIRST, then delete the pen/hub. PaintGauge
	// leaves hPenNeedleRed and hbrNeedleHub selected in hdcGaugeBB; DeleteObject on a still-selected
	// object fails and leaks. FreeGaugeBuffers() deletes hdcGaugeBB (deselecting them) so the deletes
	// below succeed. Mirrors FreeSigInd's order. Also forces a rebuild at the new size on next paint.
	FreeGaugeBuffers();
	if (hPenNeedleRed) { DeleteObject(hPenNeedleRed); hPenNeedleRed = NULL; }
	if (hbrNeedleHub)  { DeleteObject(hbrNeedleHub);  hbrNeedleHub  = NULL; }
	// FIX [SigindFlatGauge]: red needle (224,0,0) matching the redesigned icon accent.
	hPenNeedleRed = CreatePen(PS_SOLID, pw, RGB(224, 0, 0));
	hbrNeedleHub  = CreateSolidBrush(RGB(224, 0, 0));
}

// Draw signal indicator on toolbar
void DrawSigInd(HWND hwnd)
{
	HDC hdc;
	RECT r;
	// FIX [HealthPanelCorner]: while the Health panel is active it replaces this meter
	// (the panel paints over the same right-corner span). Skip so the two never fight.
	if (HealthPanel_Active()) return;
	// FIX [SigindFlatGauge]: the drawn footprint is the FIXED logical size (SIGIND_LOGW x
	// SIGIND_LOGH), not the face bitmap's pixel size - the bitmap is now a 4x hi-res source
	// that gets downscaled into this box, so the toolbar layout stays exactly as before.
	int x=Scale(5),bw=Scale(SIGIND_LOGW),bh=Scale(SIGIND_LOGH);	// FIX [DpiScale]: geschaalde sigind
	// FIX [SigindBandAlign]: center the meter vertically in the ACTUAL toolbar band
	// (g_cyToolbar) instead of a fixed top offset. y = Scale(4) was tuned for the toolbar
	// height at startup; when the toolbar re-autosizes shorter to its themed steady-state
	// (e.g. after a WM_THEMECHANGED or a COM-port-loss-triggered repaint), the divider/RX-Q
	// box (anchored to g_cyToolbar) moved up while the meter stayed at the fixed offset, so
	// the meter box overflowed past the divider into the title-bar band. Same anchor = no drift.
	int y = (g_cyToolbar - bh) / 2;
	if (y < Scale(1)) y = Scale(1);
	// FIX [SigindDividerClip]: keep the black meter box STRICTLY ABOVE the g_cyToolbar
	// divider row. The centered y above, floor-clamped, was fine at startup (tall toolbar)
	// but when the toolbar re-autosizes to its shorter themed steady-state the box bottom
	// (bh+y, the Rectangle fill runs to bh+y since bottom=bh+y+1 is exclusive) reached or
	// crossed g_cyToolbar, so the black fill erased the divider line under the gauge. Only
	// the PANE1 path draws that full-width divider; the hover (WM_NOTIFY) and per-second
	// (PANERXQUAL) repaints never redraw it, so the gap stuck (the line "stopped" under the
	// gauge). Clamp y down so the fill stays clear of the divider row.
	if (y + bh > g_cyToolbar - 1) y = g_cyToolbar - 1 - bh;
	if (y < 0) y = 0;
	si_index=0;  // this is used by UpdateSigInd().
	extern double dRX_Quality;

	// FIX [SigindReloadRetry]: if the reload after a display change failed (LoadBitmap
	// during display-driver churn / GDI pressure), got_sigind stayed FALSE and the
	// meter vanished until the next WM_DISPLAYCHANGE or a restart. Cheap self-heal:
	// retry the load on the next repaint.
	if (!got_sigind) LoadSigInd(ghInstance);

	if (got_sigind)
	{
		hdc = GetDC(hwnd);
		SelectObject(hdc,null_pen);

		if (old_rect_flg)
		{  // erase last display of bitmap
			SelectObject(hdc,lgray_brush);
			Rectangle(hdc,old_rect.right-(bw+x),y,old_rect.right-(x-1),bh+y+1);	// FIX [DpiScale]
		}

		GetClientRect(hwnd, &r);
		GetClientRect(hwnd, &old_rect);
		old_rect_flg=TRUE;

		// Keep record of bitmap's current location
		sig_rect.left	= r.right-(bw+x);	// FIX [DpiScale]
		sig_rect.top	= y;
		sig_rect.bottom	= bh + y;	// FIX [DpiScale]
		sig_rect.right	= r.right-(x-1);

		// FIX [SigindFlatGauge]: proactively restore the toolbar/title-bar divider segment
		// under the gauge. Only the PANE1 path draws this dark-gray line (at g_cyToolbar,
		// full width); the hover (WM_NOTIFY) and per-second (PANERXQUAL) repaints never do.
		// Redrawing it here lets an already-broken line self-heal on the next meter repaint
		// instead of waiting for a full WM_PAINT. The gauge box stays above g_cyToolbar (see
		// the y clamp above), so this line sits cleanly just below the gauge, matching PANE1.
		SelectObject(hdc, SysPEN[DARKGRAY]);
		MoveToEx(hdc, sig_rect.left, g_cyToolbar, NULL);
		LineTo(hdc, r.right, g_cyToolbar);

		ReleaseDC(hwnd,hdc);
		// FIX [SigindFlatGauge]: the face + needle are now painted together via the cached
		// back-buffer (PaintGauge), so the arc is redrawn fresh every frame and the moving
		// needle can never nibble it. sig_rect (set above) tells PaintGauge where to blit.
		PaintGauge(si_index);
	}
}

// Update signal indicator on toolbar.
// Draws a new line on signal indicator,
// removing old line first.
void UpdateSigInd(int direction_flg)
{
	si_old_index = si_index;

	if (old_rect_flg)
	{
		if (direction_flg)	// Move indictor right 1
		{
			si_low_hover=0;
			si_index ? si_index+=2 : si_index++;

			if (si_index > MAX_SI_POS)
			{
				si_hi_hover++;
				si_index=MAX_SI_POS;
				return;
			}
	 	}
		else
		{							// Move indictor left 1
			if (si_low_hover)
			{
				if (si_low_hover==1)
				{
					show_sigind(1, 0);
				}
				if (si_low_hover > 1)
				{
					si_low_hover=0;
					show_sigind(0, 1);
				}
			}
			si_hi_hover=0;
			si_index--;

			if (si_index < 0)
			{
				si_low_hover++;
				si_index=0;
				return;
			}
		}
		show_sigind(si_index, si_old_index);
	}
}


// Draw signal indicator needle at new_pos.
// FIX [SigindFlatGauge]: the whole gauge (clean face + needle + hub) is now composited in the
// back-buffer and presented in one blit, so the arc stays crisp and there is no flicker. The
// old_pos parameter is no longer needed (nothing is erased by overpainting) but is kept so the
// existing call sites in UpdateSigInd() stay unchanged.
void show_sigind(int new_pos,int old_pos)
{
	(void)old_pos;
	PaintGauge(new_pos);
}

