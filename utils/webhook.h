#ifndef PDW_WEBHOOK_H
#define PDW_WEBHOOK_H

#define WEBHOOK_URL_LEN     512

// Message sent to the status window (WebhookSetStatusWnd) via PostMessage.
// wParam = WHS_* constant, lParam = HTTP status code (on WHS_OK) or retry attempt (on WHS_RETRY).
#define WM_WEBHOOK_STATUS   (WM_USER + 50)

#define WHS_IDLE      0
#define WHS_SENDING   1
#define WHS_OK        2
#define WHS_RETRY     3
#define WHS_ERROR     4
#define WHS_DISABLED  5

void WebhookInit(void);
void WebhookShutdown(void);
void WebhookDestroy(void); // FIX [L3]: full teardown incl. DeleteCriticalSection
void WebhookNotify(const char *capcode, const char *message, const char *label,
                   const char *szTime, const char *szDate,
                   const char *szMode, const char *szType, const char *szBitrate,
                   BOOL isGroup, int groupbit);
void WebhookFlushGroup(int groupbit);
void WebhookSetStatusWnd(HWND hWnd);

#endif /* PDW_WEBHOOK_H */
