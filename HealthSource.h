/*
** HealthSource.h -- selectable RX-health source
**
** FIX [HealthSource]: PDW has TWO independent RX-quality scores:
**   1. the classic dRX_Quality (Misc.cpp CountBiterrors, running bit-error
**      ratio) shown in the RX-Q corner box - fairly lenient;
**   2. the p2kflex penalty EMA (utils/rxq.cpp, bucket history + asymmetric
**      "instant drop, slow recovery" EMA) - until now telnet-wire-only, but
**      computed unconditionally on the decode path even with the telnet
**      server disabled.
**
** This module exposes ONE normalized 0..100 score from whichever source the
** user selected (Profile.nHealthSource), consumed by BOTH the toolbar Health
** panel and the RX-quality mail alert (RxQualMonitor.cpp) - so the alert
** always follows the same number the panel shows.
**
** Deliberately a pure toggle, NOT telnet-primary-with-fallback: both sources
** are always computed, so there is no real "unavailable" state to fall back
** on, and silently switching sources would make the mail alert fire against
** a different formula than the user configured. Default = classic needle
** score (backward compatible with existing alert threshold tuning).
*/
#ifndef PDW_HEALTHSOURCE_H
#define PDW_HEALTHSOURCE_H

#define HEALTH_SRC_NEEDLE 0  /* classic dRX_Quality (CountBiterrors)   */
#define HEALTH_SRC_TELNET 1  /* p2kflex penalty EMA (utils/rxq.cpp)    */

/* Normalized status for the Health panel colour coding. */
#define HSTAT_IDLE   0   /* no data seen yet from the active source    */
#define HSTAT_GREEN  1   /* score >= 96 (same bar as the RX-Q corner)  */
#define HSTAT_ORANGE 2   /* below green, above the mail-alert threshold */
#define HSTAT_RED    3   /* below the mail-alert threshold             */

double      Health_GetScore(void);       /* active source, 0..100            */
int         Health_HasData(void);        /* 0 until the source really measured */
int         Health_GetStatus(void);      /* HSTAT_* from score + thresholds  */
const char *Health_SourceName(int src);  /* short static name for UI/mail   */

#endif /* PDW_HEALTHSOURCE_H */
