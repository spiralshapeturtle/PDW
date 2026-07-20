// HealthSource.cpp -- selectable RX-health source (see HealthSource.h)
// FIX [HealthSource]

#include <windows.h>
#include "Headers/pdw.h"
#include "HealthSource.h"
#include "utils/rxq.h"

extern PROFILE Profile;
extern double  dRX_Quality;

/* FIX [HealthRxqStale]: the penalty EMA only recomputes on decoded frames
** (FLEX valid-BIW / POCSAG message paths, see Rxq_UpdateTimeBased call sites) -
** on dead air NOTHING recomputes it, so it freezes at its last (typically
** healthy) value. Without this check a receiver that dies after a healthy
** period keeps showing a green ~100% and the [RxQualZero] dead-receiver mail
** alert can never fire for this source. After this window without a recompute
** the score reads 0 ("nothing decodable is coming in").
**
** Deliberately implemented HERE and not as a timer-driven Rxq_UpdateTimeBased():
** the EMA also feeds the telnet wire format, whose call-site parity with
** p2kflexDecoder is a recorded design decision ([RxqFrameGate]) - the wire
** score keeps freezing exactly like p2kflex does; only the Health-panel/alert
** interpretation of it decays. Caveat: on networks with legitimate traffic
** gaps longer than this window (sporadic POCSAG) the score dips to 0 between
** transmissions; the mail alert additionally needs nRxQualMinutes consecutive
** low minutes, and the classic needle source is the better pick there. */
#define HEALTH_RXQ_STALE_MS  120000UL	/* 2 min without a decoded frame = dead air */

static int HealthRxqIsStale(void)
{
	unsigned long long last = Rxq_LastUpdateTick();
	if (!Rxq_HasData() || !last) return 0;	/* cold start: HSTAT_IDLE handles it */
	return (GetTickCount64() - last) > HEALTH_RXQ_STALE_MS;
}

double Health_GetScore(void)
{
	if (Profile.nHealthSource == HEALTH_SRC_TELNET)
	{
		if (HealthRxqIsStale()) return 0.0;	// FIX [HealthRxqStale]: dead air, not the frozen EMA
		double q = Rxq_GetEMA();
		if (q < 0.0)   q = 0.0;
		if (q > 100.0) q = 100.0;
		return q;
	}
	return dRX_Quality;
}

int Health_HasData(void)
{
	if (Profile.nHealthSource == HEALTH_SRC_TELNET)
		return Rxq_HasData();
	// Classic source: dRX_Quality sits at exactly 0.0 until the first decode
	// touches CountBiterrors (same convention the RX-Q corner box uses to
	// show "RX-Q" instead of a percentage). But 0.0 is ambiguous: a LIVE
	// receiver decoding pure noise also reads exactly 0.0. Make the flag
	// sticky once any real (non-zero) reading has been seen, so a true 0%
	// period plots as a 0-line in the trend (and shows "0%") instead of
	// being treated as "no data yet".
	static int s_needleSeenData = 0;
	if (dRX_Quality != 0.0) s_needleSeenData = 1;
	return s_needleSeenData;
}

int Health_GetStatus(void)
{
	if (!Health_HasData()) return HSTAT_IDLE;

	double q = Health_GetScore();

	// Red boundary follows the mail-alert threshold so "red" always means
	// "this is mail-alert territory". Clamp to keep a sane band even if the
	// user configured an extreme threshold.
	int red = Profile.nRxQualThreshold;
	if (red < 1)  red = 1;
	if (red > 95) red = 95;

	if (q <  (double)red) return HSTAT_RED;
	if (q >= 96.0)        return HSTAT_GREEN;   // same green bar as the RX-Q corner box
	return HSTAT_ORANGE;
}

const char *Health_SourceName(int src)
{
	// FIX [HealthSourceName]: user-facing name is "Penalty system" - the algorithm is
	// shared with the telnet wire score but works regardless of the telnet server.
	return (src == HEALTH_SRC_TELNET) ? "Penalty system" : "RX needle";
}
