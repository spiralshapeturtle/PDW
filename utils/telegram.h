/*
** telegram.h -- Telegram Bot API output sink for PDW (FIX [Telegram])
**
** Mirror of the webhook sink: WinHTTP worker thread + ring buffer.
** Sends decoded messages to one or more chat_id's via the Bot API sendMessage method.
*/

#ifndef PDW_TELEGRAM_H
#define PDW_TELEGRAM_H

#include <windows.h>

// Status message posted to the config dialog via PostMessage.
// wParam = TGS_* constant, lParam = HTTP status code (on TGS_OK) or retry attempt (on TGS_RETRY).
// FIX [StatusMsgId]: was WM_USER+52, which collided with WM_MYSQL_STATUS. Each feed posts only to
// its own dialog HWND so it never misdelivered in practice, but the duplicate is a landmine if a
// window ever handles two feeds' status. Moved to a unique id (webhook 50/mqtt 51/mysql 52/sqlite 53).
#define WM_TELEGRAM_STATUS   (WM_USER + 54)

#define TGS_IDLE      0
#define TGS_SENDING   1
#define TGS_OK        2
#define TGS_RETRY     3
#define TGS_ERROR     4
#define TGS_DISABLED  5

void TelegramInit(void);
void TelegramShutdown(void);
void TelegramDestroy(void);   // process-exit teardown (keeps g_cs; OS reclaims it) - FIX [TgCsTeardown]
void TelegramNotify(const char *capcode, const char *message, const char *label,
                    const char *szTime, const char *szDate,
                    const char *szMode, const char *szType, const char *szBitrate,
                    BOOL isGroup, int groupbit);
// FIX [TgGroupBatch]: emit one accumulated FLEX group call as a single message.
void TelegramFlushGroup(int groupbit);
void TelegramSetStatusWnd(HWND hWnd);

// Synchronous helpers used by the config dialog (run on the GUI thread).
// TelegramTestSend: renders a sample page through the given Title/Body templates (parse_mode=HTML) and
// sends it to each chat_id, so the test previews the real formatting. Returns TRUE on HTTP 2xx.
BOOL TelegramTestSend(const char *token, const char *chatids, const char *title, const char *body,
                      char *errOut, int errLen);
// TelegramDiscoverChatId: calls getUpdates, fills chatOut with the most recent chat_id (+name), TRUE if found.
BOOL TelegramDiscoverChatId(const char *token, char *chatOut, int chatLen, char *errOut, int errLen);

#endif /* PDW_TELEGRAM_H */
