// feedstatus.cpp -- central "last known outcome" store for the output feeds
// FIX [FeedStatus] / [FeedDotLatch] / [FeedLastError] / [FeedTransitionLog]:
// see feedstatus.h for the design rationale.

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "feedstatus.h"
#include "logmanager.h"     // FIX [FeedTransitionLog]: PDW_HEALTHLOG

// Raw feed enums, needed only for the normalization mapping below.
#include "webhook.h"        // WHS_*
#include "mqtt.h"           // MHS_*
#include "mysql.h"          // MYS_*
#include "sqlite_feed.h"    // SQS_*
#include "telegram.h"       // TGS_*
#include "pushover.h"       // PUS_*
#include "telnet_server.h"  // TSS_*

static volatile LONG g_feedState[FEED_COUNT] = { 0 };  // latched FS_* values (lock-free read path)

// FIX [FeedLastError]: detail text + since-time per feed, written from the feed
// worker threads, read by the GUI thread (tooltips). Guarded by a private CS;
// initialized from a static constructor, which runs before main() and thus
// before any feed worker thread exists.
static struct FsDetailStore {
	CRITICAL_SECTION cs;
	SYSTEMTIME stSince[FEED_COUNT];
	BOOL       bSinceValid[FEED_COUNT];
	char       szDetail[FEED_COUNT][128];
	FsDetailStore() { InitializeCriticalSection(&cs); memset(stSince, 0, sizeof(stSince)); memset(bSinceValid, 0, sizeof(bSinceValid)); memset(szDetail, 0, sizeof(szDetail)); }
} g_det;

// Map a feed's own status enum onto the normalized FS_* scale.
// Returns -1 for "ignore this report" (idle/disabled reports must not
// overwrite the last real outcome; DISABLED entries are hidden by the
// Health panel via the Profile enable flag anyway).
static int NormalizeStatus(int feed, int raw)
{
	switch (feed)
	{
		case FEED_WEBHOOK:   // WHS_: IDLE 0, SENDING 1, OK 2, RETRY 3, ERROR 4, DISABLED 5
		case FEED_TELEGRAM:  // TGS_: same layout
		case FEED_PUSHOVER:  // PUS_: same layout
		case FEED_MQTT:      // MHS_: same layout
		switch (raw)
		{
			case 1:  return FS_BUSY;
			case 2:  return FS_OK;
			case 3:  return FS_RETRY;
			case 4:  return FS_ERROR;
			default: return -1;          // IDLE / DISABLED: keep last outcome
		}

		case FEED_MYSQL:     // MYS_: IDLE 0, SENDING 1, OK 2, ERROR 3, DISABLED 4
		case FEED_SQLITE:    // SQS_: IDLE 0, WRITING 1, OK 2, ERROR 3, DISABLED 4
		switch (raw)
		{
			case 1:  return FS_BUSY;
			case 2:  return FS_OK;
			case 3:  return FS_ERROR;
			default: return -1;
		}

		case FEED_TELNET:    // TSS_: DISABLED 0, LISTENING 1, ERROR 2
		switch (raw)
		{
			case TSS_LISTENING: return FS_OK;
			case TSS_ERROR:     return FS_ERROR;
			default:            return FS_UNKNOWN; // server stopped: back to unproven
		}

		case FEED_SMTP:      // no own enum: callers pass FS_* directly
		return (raw >= FS_UNKNOWN && raw <= FS_ERROR) ? raw : -1;
	}
	return -1;
}

// FIX [FeedTransitionLog]: readable state names for the health log.
static const char *FsStateName(int fs)
{
	switch (fs)
	{
		case FS_OK:    return "OK";
		case FS_RETRY: return "RETRYING";
		case FS_ERROR: return "FAILED";
	}
	return "idle";
}

void FeedStatus_Set(int feed, int rawStatus)
{
	if (feed < 0 || feed >= FEED_COUNT) return;
	int fs = NormalizeStatus(feed, rawStatus);
	if (fs < 0) return;

	// FIX [FeedDotLatch]: latch the DISPLAYED outcome.
	//  - BUSY is a send in progress, not an outcome: keep showing the last
	//    outcome instead of flashing a broken feed green on every attempt.
	//  - RETRY never downgrades ERROR: once red, only a real OK clears it.
	//
	// FIX [FeedStatusCas]: apply that latch as one atomic compare-and-swap instead of
	// a plain read (InterlockedCompareExchange-with-itself) followed by an unconditional
	// InterlockedExchange. Most feeds have a single worker, but three of them also report
	// from the GUI thread via the queue-full drop path (MqttFlushGroup / WebhookFlushGroup /
	// the Telegram+Pushover EnqueueJob drops), so two threads CAN write the same feed. The
	// split read/write let one clobber the other's decision and break exactly the invariant
	// this latch exists for: worker reports RETRY and GUI reports ERROR, both read cur=OK,
	// ERROR is written first, RETRY overwrites it - a red dot downgraded to orange. The two
	// writers are correlated, not independent: the GUI only hits the drop path when the
	// worker is already failing to drain the queue, i.e. precisely during the outage the
	// dot has to report honestly. The CAS loop re-reads and re-applies the latch rules
	// against the state it actually lost to, so no report can erase another's outcome.
	int cur, newState;
	for (;;)
	{
		cur      = (int)InterlockedCompareExchange(&g_feedState[feed], 0, 0);
		newState = cur;
		switch (fs)
		{
			case FS_BUSY:  break;                                            // keep last outcome
			case FS_RETRY: newState = (cur == FS_ERROR) ? FS_ERROR : FS_RETRY; break;
			default:       newState = fs; break;                             // OK / ERROR / UNKNOWN
		}
		if (newState == cur) return;                                         // nothing to change
		if ((int)InterlockedCompareExchange(&g_feedState[feed], (LONG)newState, (LONG)cur) == cur)
			break;                          // won the swap: cur is the state we really replaced
		/* lost it - another thread moved the state; re-latch against the new value */
	}

	// FIX [FeedTransitionLog]: snapshot the detail + stamp the since-time
	// under the CS, log OUTSIDE it (LogManager direct mode does disk I/O).
	// FIX [FeedStatusCas]: this now runs AFTER the state is committed rather than before.
	// Either order leaves a one-tick window where a GUI reader pairs a state with the other
	// field's previous value; committing first is the better half of that trade, because the
	// state is what the latch protects and the log line below can then only describe a
	// transition that definitely happened (it reports the value the CAS truly replaced).
	char det[128];
	EnterCriticalSection(&g_det.cs);
	GetLocalTime(&g_det.stSince[feed]);
	g_det.bSinceValid[feed] = TRUE;
	if (newState == FS_OK || newState == FS_UNKNOWN)
		g_det.szDetail[feed][0] = '\0';                                  // problem resolved/reset
	strcpy(det, g_det.szDetail[feed]);
	LeaveCriticalSection(&g_det.cs);

	// FIX [HealthLogNoIdleOk]: "idle -> OK" (unproven feed's first successful
	// contact) is not a failure and not a recovery - it clutters the health
	// log with routine noise (e.g. MQTT's proactive idle-reconnect can hit
	// this once a minute). Every other transition still logs, including
	// recoveries (RETRYING/FAILED -> OK), since those are what "what went
	// wrong overnight" diagnosis actually needs.
	if (cur == FS_UNKNOWN && newState == FS_OK) return;

	PDW_HEALTHLOG("%s (%s): %s -> %s%s%s",
	              FeedStatus_Tag(feed), FeedStatus_Name(feed),
	              FsStateName(cur), FsStateName(newState),
	              det[0] ? " - " : "", det);
}

// FIX [FeedLastError]: store the "last problem" text shown in the Health-panel
// tooltip and appended to the transition log line. Call before PostStatus().
void FeedStatus_SetDetail(int feed, const char *fmt, ...)
{
	if (feed < 0 || feed >= FEED_COUNT || !fmt) return;

	char buf[128];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
	va_end(ap);
	buf[sizeof(buf) - 1] = '\0';

	EnterCriticalSection(&g_det.cs);
	strcpy(g_det.szDetail[feed], buf);
	LeaveCriticalSection(&g_det.cs);
}

int FeedStatus_Get(int feed)
{
	if (feed < 0 || feed >= FEED_COUNT) return FS_UNKNOWN;
	// Atomic read (compare-exchange with itself); returns the current FS_* value.
	return (int)InterlockedCompareExchange(&g_feedState[feed], 0, 0);
}

void FeedStatus_GetInfo(int feed, FEEDSTATUS_INFO *out)
{
	if (!out) return;
	memset(out, 0, sizeof(*out));
	if (feed < 0 || feed >= FEED_COUNT) return;

	out->state = FeedStatus_Get(feed);
	EnterCriticalSection(&g_det.cs);
	out->stSince     = g_det.stSince[feed];
	out->bSinceValid = g_det.bSinceValid[feed];
	strcpy(out->szDetail, g_det.szDetail[feed]);
	LeaveCriticalSection(&g_det.cs);
}

const char *FeedStatus_Tag(int feed)
{
	switch (feed)
	{
		case FEED_SMTP:     return "SM";
		case FEED_WEBHOOK:  return "WH";
		case FEED_TELEGRAM: return "TG";
		case FEED_PUSHOVER: return "PO";
		case FEED_MQTT:     return "MQ";
		case FEED_MYSQL:    return "MY";
		case FEED_SQLITE:   return "SQ";
		case FEED_TELNET:   return "TS";
	}
	return "?";
}

const char *FeedStatus_Name(int feed)
{
	switch (feed)
	{
		case FEED_SMTP:     return "SMTP e-mail";
		case FEED_WEBHOOK:  return "Webhook";
		case FEED_TELEGRAM: return "Telegram";
		case FEED_PUSHOVER: return "Pushover";
		case FEED_MQTT:     return "MQTT";
		case FEED_MYSQL:    return "MySQL";
		case FEED_SQLITE:   return "SQLite";
		case FEED_TELNET:   return "Telnet server";
	}
	return "?";
}
