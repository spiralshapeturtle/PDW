/*
** HealthPanel.h -- compact "Health" status panel on the toolbar band
**
** FIX [HealthPanel]: parent-drawn panel between the toolbar icon buttons and
** the signal meter / RX-Q corner, showing:
**   - the active health score (HealthSource.h) as a colour-coded percentage,
**   - a sparkline of that score over a configurable trend window,
**   - a COM-port link dot (utils/rs232.cpp Rs232LinkState),
**   - one status dot + tag per ENABLED output feed (utils/feedstatus.h).
**
** Drawn exactly like the existing corner graphics (sigind.cpp / the RX-Q box):
** plain GDI onto the main window's DC over the toolbar band, composed in a
** cached memory DC and presented with a single BitBlt (flicker-free), all
** geometry through the Scale() DPI helper. Repainted from the same three
** paths that heal the corner: WM_PAINT, WM_NOTIFY hover bursts, SECOND_TIMER.
*/
#ifndef PDW_HEALTHPANEL_H
#define PDW_HEALTHPANEL_H

#include <windows.h>

void HealthPanel_Draw(HWND hwnd);        /* paint (no-op when hidden or no room)      */
void HealthPanel_OnSecond(void);         /* 1 Hz: sample active score into trend ring */

/* FIX [HealthPanelCorner]: TRUE when the panel is visible AND currently fits.
** While active the panel REPLACES the classic right corner: DrawSigInd/
** PaintGauge (needle) and the PANERXQUAL block (RX-Q % box + warning square)
** early-out on this, and the PANE1 header runs to the window edge. When the
** panel is hidden - or the window is too narrow for it - the classic corner
** renders exactly as before. Cheap (two static reads), safe to call from the
** high-frequency needle path. */
BOOL HealthPanel_Active(void);
BOOL HealthPanel_OnToolbarRClick(HWND hMain, POINT ptMainClient); /* context menu     */
BOOL HealthPanel_OnToolbarLClick(HWND hMain, POINT ptMainClient); /* FIX [HealthClickConfig]: open feed config */
void HealthPanel_OnDisplayChange(void);  /* drop cached DDB/pens after display/DPI change */
void HealthPanel_Free(void);             /* delete all cached GDI objects             */

#endif /* PDW_HEALTHPANEL_H */
