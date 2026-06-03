// FIX [SqliteFeed]: SQLite output feed -- amalgamation (sqlite3.c), no external DLL.
// Mirrors the MySQL feed pattern (utils/mysql.cpp): decoder-thread -> ring buffer
// (CRITICAL_SECTION + event) -> worker-thread -> prepared INSERT into a local db file.
// Schema = MySQL "Optimized" 1-to-1 (capcode as TEXT so ASTRID leading zeros are preserved).

#pragma once
#ifndef SQLITE_FEED_H
#define SQLITE_FEED_H

// Bitmask for sqlite_fields -- which optional columns are included in the INSERT.
// ontvangen + capcode are always written; these five are individually switchable.
// Same bit positions as MYF_* (utils/mysql.h) for consistency.
#define SQF_MODE      (1<<0)
#define SQF_MSG_TYPE  (1<<1)
#define SQF_BITRATE   (1<<2)
#define SQF_MESSAGE   (1<<3)
#define SQF_LABEL     (1<<4)
#define SQF_ALL       0x1F

// Message posted to the status window (SqliteSetStatusWnd) via PostMessage.
// wParam = SQS_* constant, lParam = 0 (reserved).
#define WM_SQLITE_STATUS  (WM_USER + 53)

#define SQS_IDLE     0
#define SQS_WRITING  1
#define SQS_OK       2
#define SQS_ERROR    3
#define SQS_DISABLED 4

void SqliteInit(void);
void SqliteNotify(const char *capcode, const char *message, const char *label,
                  const char *szTime,  const char *szDate,
                  const char *szMode,  const char *szType, const char *szBitrate,
                  int matchType, const char *labelColor);
/* FLEX group call: call Accumulate for each subscriber row, then FlushGroup for the group row. */
void SqliteGroupAccumulate(const char *capcode, const char *label,
                           const char *message,
                           const char *szTime,  const char *szDate,
                           const char *szMode,  const char *szType, const char *szBitrate,
                           int matchType, const char *labelColor,
                           int groupbit);
void SqliteFlushGroup(int groupbit);
void SqliteStop(void);
void SqliteDestroy(void);
void SqliteSetStatusWnd(HWND hWnd);

// FIX [ConnTest]: synchronous test for the Setup dialog. Opens (or creates) the given db file
// with its OWN connection (never touches the running worker's globals), creates/checks the schema
// and closes. Fills szMsg with a human-readable result. Call only from a user action (Test button).
// Returns TRUE on success.
BOOL SqliteTestConnection(const char *path, const char *table, char *szMsg, int msgLen);

#endif // SQLITE_FEED_H
