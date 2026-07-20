/*
** feedstatus.h -- central "last known outcome" store for the output feeds
**
** FIX [FeedStatus]: every output feed reports its worker status only via
** PostMessage to its own config dialog (and only while that dialog is open).
** The toolbar Health panel needs a pollable, GUI-thread-readable snapshot of
** each feed's last real outcome. This module is that snapshot: one
** InterlockedExchange'd LONG per feed, written from each feed's PostStatus()
** (worker thread) and read lock-free from the GUI thread.
**
** FeedStatus_Set() takes the feed's OWN raw status enum (WHS_ / MHS_ / MYS_ /
** SQS_ / TGS_ / PUS_ / TSS_ values) and normalizes it here, so each feed only
** adds a single self-describing line to its PostStatus(). SMTP has no status
** enum; it passes the normalized FS_ values directly (identity mapping).
**
** FIX [FeedDotLatch]: the stored state is a LATCHED outcome, not the raw
** report. A feed that is continuously failing cycles SENDING -> RETRY ->
** ERROR on every message, which made the Health-panel dot strobe
** green/orange/red. Two rules stop that:
**   - BUSY (a send in progress) is activity, not an outcome: it never
**     changes the displayed state, so a broken feed no longer flashes green
**     at the start of every attempt;
**   - RETRY never downgrades ERROR: once red, only a real success (OK)
**     clears the dot back to green.
**
** FIX [FeedLastError]: alongside the state this module keeps a short
** "last problem" text + the local time of the last state change, written by
** the feeds via FeedStatus_SetDetail() right before an ERROR/RETRY report
** and shown in the Health-panel tooltips. Detail writes take a private
** critical section (nothing else shares it; the hot read path
** FeedStatus_Get stays lock-free).
**
** FIX [FeedTransitionLog]: every latched state CHANGE is written to the
** central LogManager (LC_HEALTH -> {date}_health.log), so "the dot was red
** last night" is diagnosable after the fact. Steady states log nothing.
*/
#ifndef PDW_FEEDSTATUS_H
#define PDW_FEEDSTATUS_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Feed identifiers */
#define FEED_SMTP     0
#define FEED_WEBHOOK  1
#define FEED_TELEGRAM 2
#define FEED_PUSHOVER 3
#define FEED_MQTT     4
#define FEED_MYSQL    5
#define FEED_SQLITE   6
#define FEED_TELNET   7
#define FEED_COUNT    8

/* Normalized health states (returned by FeedStatus_Get).
** FS_UNKNOWN = nothing reported since startup: the feed is idle/unproven.
** Idle reports do NOT overwrite a previous real outcome - the store keeps
** the last OK/RETRY/ERROR so the panel shows "last known outcome".
** FS_BUSY exists on the input side only; the latched state never holds it
** (see [FeedDotLatch] above). */
#define FS_UNKNOWN 0
#define FS_OK      1
#define FS_BUSY    2
#define FS_RETRY   3
#define FS_ERROR   4

/* Snapshot for tooltips/diagnostics (FIX [FeedLastError]). */
typedef struct {
	int        state;          /* latched FS_* value                        */
	SYSTEMTIME stSince;        /* local time of the last state change       */
	BOOL       bSinceValid;    /* FALSE until the first real report         */
	char       szDetail[128];  /* last problem text, "" when none           */
} FEEDSTATUS_INFO;

/* rawStatus = the feed's own status enum value (see mapping in feedstatus.cpp).
** Thread-safe; callable from any thread. */
void FeedStatus_Set(int feed, int rawStatus);

/* Store a short "last problem" description (printf-style). Call right before
** the ERROR/RETRY PostStatus so the transition log line can include it.
** Cleared automatically on the next FS_OK. Never put secrets in here.
** Thread-safe; callable from any thread. */
void FeedStatus_SetDetail(int feed, const char *fmt, ...);

/* Normalized (latched) FS_* value. Thread-safe lock-free read. */
int  FeedStatus_Get(int feed);

/* Full snapshot incl. since-time and last problem text. GUI-thread use. */
void FeedStatus_GetInfo(int feed, FEEDSTATUS_INFO *out);

/* Short label for the Health panel ("SM", "WH", ...). Static string. */
const char *FeedStatus_Tag(int feed);

/* Full feed name for tooltips/logs ("SMTP e-mail", ...). Static string. */
const char *FeedStatus_Name(int feed);

#ifdef __cplusplus
}
#endif
#endif /* PDW_FEEDSTATUS_H */
