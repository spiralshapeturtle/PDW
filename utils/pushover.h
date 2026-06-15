/*
** pushover.h -- Pushover (pushover.net) output sink for PDW  (FIX [Pushover])
**
** Mirror of the Telegram sink: WinHTTP worker thread + ring buffer.
** Sends decoded messages to the Pushover Messages API.
*/

#ifndef PDW_PUSHOVER_H
#define PDW_PUSHOVER_H

#include <windows.h>

// FIX [StatusMsgId]: was WM_USER+53, which collided with WM_SQLITE_STATUS (see telegram.h note).
#define WM_PUSHOVER_STATUS   (WM_USER + 55)

#define PUS_IDLE      0
#define PUS_SENDING   1
#define PUS_OK        2
#define PUS_RETRY     3
#define PUS_ERROR     4
#define PUS_DISABLED  5

void PushoverInit(void);
void PushoverShutdown(void);
void PushoverDestroy(void);
void PushoverNotify(const char *capcode, const char *message, const char *label,
                    const char *szTime, const char *szDate,
                    const char *szMode, const char *szType, const char *szBitrate,
                    BOOL isGroup, int groupbit,
                    int jobPriority, const char *jobSound);  // FIX [PushoverPerFilter]: -9/""=use global
// FIX [PoGroupBatch]: emit one accumulated FLEX group call as a single notification.
void PushoverFlushGroup(int groupbit);
void PushoverSetStatusWnd(HWND hWnd);

// Synchronous test send used by the config dialog (GUI thread). Renders a sample page through the
// given Title/Body templates (and html flag) so the test previews the real formatting. TRUE on 2xx.
BOOL PushoverTestSend(const char *appToken, const char *userKey, const char *title, const char *body,
                      BOOL html, int priority, const char *sound, char *errOut, int errLen);  // FIX [PushoverPerFilter]: Test honours priority/sound

#endif /* PDW_PUSHOVER_H */
