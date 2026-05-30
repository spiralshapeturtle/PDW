//
// Toolbar.cpp
//
// Functions to allocate the toolbar/buttons and button bitmaps
// Function to set toolbar tooltips
//

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
#include "headers\gfx.h"		// FIX [DpiScale]: TITLE_BAR_SIZE
#include "headers\initapp.h"	// FIX [DpiScale]: Scale(), g_dpi, g_cyToolbar/g_cyTopBand


#define NUM_TB_BUTTONS 18   // 14 buttons and 5 seperators
#define NUM_TB_IMAGES  13   // 14 button images
// #define TOOLBAR_DEBUG  1

TBBUTTON toolbar_btns[NUM_TB_BUTTONS];  // Toolbar buttons
HBITMAP tb_btn_image[NUM_TB_IMAGES];    // Bitmap Handles to Toolbar images

// FIX [DpiScale]: DPI-geschaalde imagelist met de (gestretchte) 18x18 knop-bitmaps.
HIMAGELIST hToolImageList = NULL;

// FIX [DpiScale]: stretch een bron-bitmap naar w x h (DDB, schermcompatibel).
static HBITMAP ScaleToolBitmap(HDC hdcRef, HBITMAP src, int w, int h)
{
	BITMAP bm;
	if (!src || !GetObject(src, sizeof(bm), &bm)) return NULL;

	HDC sdc = CreateCompatibleDC(hdcRef);
	HDC ddc = CreateCompatibleDC(hdcRef);
	HBITMAP dst = CreateCompatibleBitmap(hdcRef, w, h);
	HBITMAP os  = (HBITMAP)SelectObject(sdc, src);
	HBITMAP od  = (HBITMAP)SelectObject(ddc, dst);

	SetStretchBltMode(ddc, COLORONCOLOR);
	StretchBlt(ddc, 0, 0, w, h, sdc, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);

	SelectObject(sdc, os);
	SelectObject(ddc, od);
	DeleteDC(sdc);
	DeleteDC(ddc);
	return dst;
}

// FIX [DpiScale]: bouw de imagelist op iconSize uit de reeds geladen tb_btn_image[].
// Maskerkleur = hoekpixel (0,0): de 4bpp-knoppen hebben een vlakke grijze
// COLOR_BTNFACE-achtergrond, die zo transparant wordt en de knop-face doorlaat.
static void BuildToolImageList(int iconSize)
{
	if (hToolImageList) { ImageList_Destroy(hToolImageList); hToolImageList = NULL; }

	hToolImageList = ImageList_Create(iconSize, iconSize, ILC_COLOR32 | ILC_MASK, NUM_TB_IMAGES, 0);
	if (!hToolImageList) return;

	HDC hdcScreen = GetDC(NULL);
	for (int i = 0; i < NUM_TB_IMAGES; i++)
	{
		HBITMAP scaled = ScaleToolBitmap(hdcScreen, tb_btn_image[i], iconSize, iconSize);
		if (!scaled) continue;

		HDC mdc = CreateCompatibleDC(hdcScreen);
		HBITMAP old = (HBITMAP)SelectObject(mdc, scaled);
		COLORREF mask = GetPixel(mdc, 0, 0);
		SelectObject(mdc, old);
		DeleteDC(mdc);

		ImageList_AddMasked(hToolImageList, scaled, mask);
		DeleteObject(scaled);
	}
	ReleaseDC(NULL, hdcScreen);
}


// Allocate toolbar and return its window handle
HWND ShowMakeToolBar(HWND parent_hwnd,HINSTANCE hThisInstance)
{
	HWND tbhwnd=NULL;

#ifdef TOOLBAR_DEBUG
 HDC hdc;
#endif

	for(int i=0; i<NUM_TB_IMAGES; i++) tb_btn_image[i]=0; // Ensure only images allocated are freed.

	InitCommonControls();               // Ensure dll is loaded for CreateToolbarEx.

	if(GetToolBarImages(hThisInstance)) // Load bitmap button images
	{
		SetToolBarButtons();            // Set type/state for each button

		int iconSize = Scale(18);       // FIX [DpiScale]: 18 px basis -> 18/23/27/36 bij 100/125/150/200%

		// Create Toolbar without buttons. Add these later
		tbhwnd = CreateToolbarEx(parent_hwnd,
							     WS_CHILD|WS_VISIBLE|WS_DLGFRAME|TBSTYLE_TOOLTIPS,
								 IDW_TOOL_BAR,
								 0,
								 NULL,
								 0,
								 0,
								 0,
								 iconSize,
								 iconSize,
								 iconSize,
								 iconSize,
								 sizeof(TBBUTTON));

		if(!(tbhwnd))
		{                         // Got tool bar?
			FreeToolBarImages(hThisInstance);  // error? - free any allocated objects
			return(NULL);
		}

		// FIX [DpiScale]: geef de toolbar een DPI-geschaalde 32-bit imagelist en
		// expliciete bitmap-/knopmaten i.p.v. de oude 18x18 defaults.
		BuildToolImageList(iconSize);
		if (hToolImageList)
			SendMessage(tbhwnd, TB_SETIMAGELIST, 0, (LPARAM)hToolImageList);
		SendMessage(tbhwnd, TB_SETBITMAPSIZE, 0, MAKELPARAM(iconSize, iconSize));
		SendMessage(tbhwnd, TB_SETBUTTONSIZE, 0, MAKELPARAM(iconSize + Scale(7), iconSize + Scale(7)));

		Add_TB_ButtonsBitmaps(tbhwnd,hThisInstance); // Add rest of buttons.

		// FIX [DpiScale]: forceer autosize en lees de ECHTE toolbar-hoogte terug;
		// hier wordt de pane-layout aan gekoppeld (g_cyTopBand), niet aan een vaste maat.
		SendMessage(tbhwnd, TB_AUTOSIZE, 0, 0);
		{
			// FIX [DpiScale]: onderkant in client-coords (niet de window-hoogte); zie PdwUpdateToolbarMetrics.
			RECT rcTb; GetWindowRect(tbhwnd, &rcTb);
			POINT ptBottom; ptBottom.x = rcTb.left; ptBottom.y = rcTb.bottom;
			ScreenToClient(parent_hwnd, &ptBottom);
			g_cyToolbar = ptBottom.y;
			g_cyTopBand = g_cyToolbar + Scale(TITLE_BAR_SIZE) + Scale(2);
		}
	}

#ifdef TOOLBAR_DEBUG
	hdc = GetDC(parent_hwnd);

	if(tbhwnd == NULL) TextOut(hdc, 0, 70, "NOT Allocated!", 14);
	else TextOut(hdc, 0, 70, "OK!!", 4);// UpdateWindow(hToolbar);

	ReleaseDC(parent_hwnd, hdc);

#endif

	return(tbhwnd);
}


// Load toolbar bitmap images
// Start from TOOL_BAR_BTN0
//
BOOL GetToolBarImages(HINSTANCE hThisInstance)
{
	WORD btn_index = (WORD)IDT_TOOLBAR_BTN0;

	for(int i=0; i<NUM_TB_IMAGES; i++)
	{
		if(!(tb_btn_image[i] = LoadBitmap(hThisInstance,MAKEINTRESOURCE((WORD)btn_index))))
		{
			FreeToolBarImages(hThisInstance);
			return(FALSE);
		}
		btn_index++;
	}
	return(TRUE);
}

// Free Toolbar Bitmap images
void FreeToolBarImages(HINSTANCE hThisInstance)
{
	for(int i=0;i < NUM_TB_IMAGES;i++) if (tb_btn_image[i]) DeleteObject(tb_btn_image[i]);
	if (hToolImageList) { ImageList_Destroy(hToolImageList); hToolImageList = NULL; } // FIX [DpiScale]
}


// Set info for each toolbar button.
void SetToolBarButtons(void)
{
	WORD btn_index =(WORD)IDT_TOOLBAR_BTN0;

	for (int i=0, x=0; i<NUM_TB_BUTTONS; i++)
	{
		toolbar_btns[i].fsState=(BYTE)(TBSTATE_ENABLED);
		toolbar_btns[i].dwData=0;
		toolbar_btns[i].iString=0;

		switch(i)
		{   // workout where to put seperators.
			case 0:
			// 1 = open/close log file
			case 2:
			// 3,4,5,6,7 = copy/upper/lower/save/print window panes
			case 8:
			// 9,10 = options/filter
			case 11:
			// 12,13 = statistics/pause
			case 14:
			// 15,16 = help/clear screen

			toolbar_btns[i].iBitmap = 0;
			toolbar_btns[i].fsStyle=(BYTE)TBSTYLE_SEP;
			toolbar_btns[i].idCommand = 0;
			break;

			default:  // Actual button
			toolbar_btns[i].fsStyle=(BYTE)TBSTYLE_BUTTON;
			toolbar_btns[i].iBitmap = x++;
			toolbar_btns[i].idCommand = btn_index++;
		}
	}
}

// Add all toolbar bitmaps then add all toolbar buttons
void Add_TB_ButtonsBitmaps(HWND tbar_hwnd,HINSTANCE hThisInstance)
{
	// FIX [DpiScale]: de knopafbeeldingen komen nu uit de DPI-geschaalde imagelist
	// (TB_SETIMAGELIST in ShowMakeToolBar); de oude per-knop TB_ADDBITMAP vervalt.
	(void)hThisInstance;
	SendMessage(tbar_hwnd,(UINT)TB_ADDBUTTONS,(WPARAM)NUM_TB_BUTTONS,(LPARAM)toolbar_btns);
}

// used to ensure toolbar stays correct size.
void TB_AutoSize(HWND hTbar)
{
	SendMessage(hTbar, (UINT)TB_AUTOSIZE, 0, 0);
}

// FIX [DpiScale]: hermeet de toolbar en herbereken de pane-bovenrand (roep aan vanuit WM_SIZE).
// Belangrijk: gebruik de ONDERKANT van de toolbar in client-coords van de parent, NIET de
// window-hoogte. Met WS_DLGFRAME ligt de toolbar-top boven y=0 (bv. -3), dus de hoogte
// (bv. 62) overschat waar de toolbar werkelijk eindigt (client-y 59) -> dat gaf de zwarte balk.
void PdwUpdateToolbarMetrics(void)
{
	if (!hToolbar) return;

	SendMessage(hToolbar, (UINT)TB_AUTOSIZE, 0, 0);

	RECT rcTb; GetWindowRect(hToolbar, &rcTb);
	POINT ptBottom; ptBottom.x = rcTb.left; ptBottom.y = rcTb.bottom;
	ScreenToClient(GetParent(hToolbar), &ptBottom);

	g_cyToolbar = ptBottom.y;	// onderkant toolbar in client-coords van de parent
	g_cyTopBand = g_cyToolbar + Scale(TITLE_BAR_SIZE) + Scale(2);
}


// Called when parent window receives a WM_NOTIFTY message.
// Sets the toolbar Tool Tip text if it's a TTN_NEEDTEXT message.
void SetToolTXT(HINSTANCE hThisInstance, LPARAM lParam)
{
	LPTOOLTIPTEXT new_txt;		// Used for setting tool-tip text for toolbar.

	new_txt = (LPTOOLTIPTEXT)lParam;
	new_txt->hinst = hThisInstance;

	switch(new_txt->hdr.idFrom)	// Set text, based on which control the cursor is currently on
	{
		case IDT_TOOLBAR_BTN0:
		new_txt->lpszText = "Logfile";
		break;

		case IDT_TOOLBAR_BTN1:
		new_txt->lpszText = "Copy Selection";
		break;

		case IDT_TOOLBAR_BTN2:
		new_txt->lpszText = "Copy Monitor Window";
		break;

		case IDT_TOOLBAR_BTN3:
		new_txt->lpszText = "Copy Filter Window";
		break;

		case IDT_TOOLBAR_BTN4:
		new_txt->lpszText = "Save Clipboard";
		break;

		case IDT_TOOLBAR_BTN5:
		new_txt->lpszText = "Print Clipboard";
		break;

		case IDT_TOOLBAR_BTN6:
		new_txt->lpszText = "Options";
		break;

		case IDT_TOOLBAR_BTN7:
		new_txt->lpszText = "Filters";
		break;

		case IDT_TOOLBAR_BTN8:
		new_txt->lpszText = "Statistics";
		break;

		case IDT_TOOLBAR_BTN9:
		new_txt->lpszText = "Pause";
		break;

		case IDT_TOOLBAR_BTN10:
		new_txt->lpszText = "Help";
		break;

		case IDT_TOOLBAR_BTN11:
		new_txt->lpszText = "Clear Screen";
		break;

		case IDT_TOOLBAR_BTN12:
		new_txt->lpszText = "Change Mode";
		break;

//		default:
//		new_txt->lpszText = "*";
//		break;
	}
}
