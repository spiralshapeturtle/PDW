// FIX [MySQLFeed]: MySQL output feed — mysql_native_password, no external DLLs.

#pragma once
#ifndef MYSQL_H
#define MYSQL_H

// Bitmask for mysql_fields — which optional columns to include in INSERT.
// address and received are always written; these five are individually switchable.
#define MYF_MODE      (1<<0)
#define MYF_MSG_TYPE  (1<<1)
#define MYF_BITRATE   (1<<2)
#define MYF_MESSAGE   (1<<3)
#define MYF_LABEL     (1<<4)
#define MYF_ALL       0x1F

// Message posted to the status window (MysqlSetStatusWnd) via PostMessage.
// wParam = MYS_* constant, lParam = 0 (reserved).
#define WM_MYSQL_STATUS  (WM_USER + 52)

#define MYS_IDLE     0
#define MYS_SENDING  1
#define MYS_OK       2
#define MYS_ERROR    3
#define MYS_DISABLED 4

// Table schema variant — selects CREATE TABLE and INSERT format.
// Classic  = backward compat with meld2mysql.exe (3 columns: capcode/melding/label)
// Extended = all 8 PDW text fields as raw strings (address/time/date/mode/type/bitrate/message/label)
// Optimized= type-correct columns (address CHAR(9), DATETIME received, SMALLINT bitrate, FULLTEXT index)
#define MYSQL_SCHEMA_CLASSIC   0
#define MYSQL_SCHEMA_EXTENDED  1
#define MYSQL_SCHEMA_OPTIMIZED 2

void MysqlInit(void);
void MysqlNotify(const char *capcode, const char *message, const char *label,
                 const char *szTime,  const char *szDate,
                 const char *szMode,  const char *szType, const char *szBitrate,
                 int matchType, const char *labelColor);
/* FIX [MySQLFeed]: FLEX group call support — Optimized schema only.
   Accumulate is called for each subscriber row; FlushGroup stores the group row. */
void MysqlGroupAccumulate(const char *capcode, const char *label,
                          const char *message,
                          const char *szTime,  const char *szDate,
                          const char *szMode,  const char *szType, const char *szBitrate,
                          int matchType, const char *labelColor,
                          int groupbit);
void MysqlFlushGroup(int groupbit);
void MysqlStop(void);
void MysqlDestroy(void);
void MysqlSetStatusWnd(HWND hWnd);

// FIX [ConnTest]: synchronous connection test for the Setup dialog. Connects with its OWN socket
// (never touches the running worker's g_sock/globals), authenticates and checks database access.
// Fills szMsg with a human-readable result either way. Blocks up to ~5 s; call from a user-initiated
// action (the Test button) only. Returns TRUE on a usable connection.
BOOL MysqlTestConnection(const char *host, int port, const char *user, const char *pass,
                         const char *database, char *szMsg, int msgLen);

#endif // MYSQL_H
