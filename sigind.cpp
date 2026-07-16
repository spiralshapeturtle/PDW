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

#define MAX_SI_POS        20	// 0-12. Max positions available to signal indicator.
#define AUDIO_POINT_VALUE 2	// Used for working out samples per signal
										// position.e.g. sample=20/10, position=2.

BOOL old_rect_flg=FALSE;
BOOL got_sigind=FALSE;
HDC hdcMemory=NULL;

// FIX [DpiScale]: DPI-geschaalde naaldpennen — aangemaakt in InitSigIndPens() na g_dpi bekend is.
HPEN hPenNeedleRed   = NULL;
HPEN hPenNeedleWhite = NULL;

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

int sip[21][4] =	{//from: x,  y, To: x,  y
							{18, 20,  6, 14},
//							{18, 20,  7, 13},
							{18, 20,  8, 12},
							{18, 20,  9, 11},
							{18, 20, 10, 10},
							{18, 20, 11,  9},
							{18, 20, 12,  9},
							{18, 20, 13,  8},
							{18, 20, 15,  8},
							{18, 20, 16,  7},
							{18, 20, 17,  7},
							{18, 20, 19,  7},
							{18, 20, 20,  7},
							{18, 20, 22,  8},
							{18, 20, 23,  8},
							{18, 20, 24,  9},
							{18, 20, 25,  9},
							{18, 20, 26, 10},
							{18, 20, 27, 11},
							{18, 20, 28, 12},
							{18, 20, 29, 13},
							{18, 20, 30, 14},
					};


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
	LoadSigInd(hThisInstance);	// reloads + GetObject(bms) + sets got_sigind
}

// Free bitmap object
void FreeSigInd(void)
{
	if (got_sigind) DeleteObject(hbm_sigind);
	if (hPenNeedleWhite) { DeleteObject(hPenNeedleWhite); hPenNeedleWhite = NULL; }	// FIX [DpiScale]
	if (hPenNeedleRed)   { DeleteObject(hPenNeedleRed);   hPenNeedleRed   = NULL; }	// FIX [DpiScale]
}

// FIX [DpiScale]: maak pennen aan met DPI-proportionele dikte (1px op 96dpi, 2px op 150%+).
// Aanroepen na g_dpi bekend is (WM_CREATE), niet in LoadSigInd (die loopt vóór WM_CREATE).
void InitSigIndPens(void)
{
	int pw = Scale(1);
	if (hPenNeedleWhite) { DeleteObject(hPenNeedleWhite); hPenNeedleWhite = NULL; }
	if (hPenNeedleRed)   { DeleteObject(hPenNeedleRed);   hPenNeedleRed   = NULL; }
	hPenNeedleWhite = CreatePen(PS_SOLID, pw, RGB(255, 255, 255));
	hPenNeedleRed   = CreatePen(PS_SOLID, pw, RGB(255,   0,   0));
}

// Draw signal indicator on toolbar
void DrawSigInd(HWND hwnd)
{
	HDC hdc;
	RECT r;
	int x=Scale(5),bw=Scale(bms.bmWidth),bh=Scale(bms.bmHeight);	// FIX [DpiScale]: geschaalde sigind
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

		// need black background for bitmap
		SelectObject(hdc,black_brush);
		Rectangle(hdc,r.right-(bw+x),y,r.right-(x-1),bh+y+1);	// FIX [DpiScale]

		// Keep record of bitmaps current location
		sig_rect.left	= r.right-(bw+x);	// FIX [DpiScale]
		sig_rect.top	= y;
		sig_rect.bottom	= bh + y;	// FIX [DpiScale]
		sig_rect.right	= r.right-(x-1);

		// draw bitmap
		if (hdcMemory = CreateCompatibleDC(hdc))
		{
			SelectObject(hdcMemory, hbm_sigind);
			SetStretchBltMode(hdc, COLORONCOLOR);	// FIX [DpiScale]
			StretchBlt(hdc,sig_rect.left,sig_rect.top, bw, bh, hdcMemory, 0, 0, bms.bmWidth, bms.bmHeight, SRCPAINT);
			DeleteDC(hdcMemory);
		}

		// FIX [SigindDividerClip]: proactively restore the toolbar/title-bar divider segment
		// under the gauge. Only the PANE1 path draws this dark-gray line (at g_cyToolbar,
		// full width); the hover (WM_NOTIFY) and per-second (PANERXQUAL) repaints never do.
		// Redrawing it here lets an already-broken line self-heal on the next meter repaint
		// instead of waiting for a full WM_PAINT. The box now stays above g_cyToolbar (see
		// the y clamp above), so this line sits cleanly just below the gauge, matching PANE1.
		SelectObject(hdc, SysPEN[DARKGRAY]);
		MoveToEx(hdc, sig_rect.left, g_cyToolbar, NULL);
		LineTo(hdc, r.right, g_cyToolbar);

		ReleaseDC(hwnd,hdc);
		show_sigind(0, 0);	// show sigind needle
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


// Draw signal indicator needle.
// Draw needle at new_pos.
// old_pos is used to erase previous line.
void show_sigind(int new_pos,int old_pos)
{
	HDC hdc;
	int x,y;

	hdc = GetDC(ghWnd);

	// erase old line.
	SelectObject(hdc, hPenNeedleWhite ? hPenNeedleWhite : SysPEN[WHITE]);	// FIX [DpiScale]: geschaalde pen
	x = sig_rect.left+Scale(sip[old_pos][0]);	// FIX [DpiScale]
	y = sig_rect.top+Scale(sip[old_pos][1]);
	MoveToEx(hdc,x,y,NULL);

	x = sig_rect.left+Scale(sip[old_pos][2]);
	y = sig_rect.top+Scale(sip[old_pos][3]);
	LineTo(hdc,x,y);

	// Draw new line.
	SelectObject(hdc, hPenNeedleRed ? hPenNeedleRed : SysPEN[RED]);		// FIX [DpiScale]: geschaalde pen
	x = sig_rect.left+Scale(sip[new_pos][0]);	// FIX [DpiScale]
	y = sig_rect.top+Scale(sip[new_pos][1]);
	MoveToEx(hdc,x,y,NULL);

	x = sig_rect.left+Scale(sip[new_pos][2]);
	y = sig_rect.top+Scale(sip[new_pos][3]);
	LineTo(hdc,x,y);

	ReleaseDC(ghWnd,hdc);
}

