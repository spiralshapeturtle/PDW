// FIX [RxQualAlert]: timer-based RX Quality alert monitor
// FIX [HealthSource]: reads the selectable health source (Health_GetScore) instead of the
// hardcoded dRX_Quality global, so the alert follows whichever source (classic RX needle
// score or telnet penalty EMA) the user selected on the Health panel. Thresholds,
// hysteresis and cooldown semantics are unchanged - only the input feed moved.
#include <windows.h>
#include "Headers/pdw.h"
#include "utils/smtp.h"
#include "RxQualMonitor.h"
#include "HealthSource.h"

extern PROFILE Profile;

static int  s_belowCount   = 0;
static int  s_cooldownLeft = 0;
// FIX [RxQualZero]: gate 0%-alerting on having first seen a healthy reading. dRX_Quality is a
// global that sits at 0.0 until the first decode (cold start) but ALSO reads 0 when the receiver
// is live yet decoding pure noise (nErrors==nErrorChecks in CountBiterrors). Without this flag the
// old "> 0.0" guard silently ignored the dead/noise case — the single most important alert. We arm
// 0%-alerting only once quality has reached the recover level, so cold start never false-fires.
static BOOL s_seenSignal   = FALSE;

static void SendRxQualAlert()
{
	if (Profile.szMailHost[0] == '\0')     return;
	if (Profile.szRxQualMailTo[0] == '\0') return;

	double dQuality = Health_GetScore();	// FIX [HealthSource]: active source, not hardcoded dRX_Quality

	// Build subject
	char szSubject[128];
	_snprintf_s(szSubject, sizeof(szSubject), _TRUNCATE,
		"PDW - Low RX Quality Alert (%.0f%%)", dQuality);

	// Build body
	char szComputer[64] = {0};
	DWORD dwSz = sizeof(szComputer);
	GetComputerNameA(szComputer, &dwSz);
	SYSTEMTIME st;
	GetLocalTime(&st);

	char szBody[512];
	_snprintf_s(szBody, sizeof(szBody), _TRUNCATE,
		"RX Quality has been below threshold for the required time.\r\n"
		"\r\n"
		"Current quality : %.1f%%\r\n"
		"Quality source  : %s\r\n"
		"Alert threshold : %d%%\r\n"
		"Recovery level  : %d%%\r\n"
		"Host            : %s\r\n"
		"Time            : %04d-%02d-%02d %02d:%02d:%02d",
		dQuality,
		Health_SourceName(Profile.nHealthSource),	// FIX [HealthSource]
		Profile.nRxQualThreshold,
		Profile.nRxQualRecover,
		szComputer,
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

	// FIX [RxQualAlert]: QueueAlertMail embeds the recipient in the queue slot so the
	// worker thread uses it regardless of what mail.to points to when it sends.
	// No MailInit redirect/restore needed — the normal SMTP worker handles the send.
	QueueAlertMail(Profile.szRxQualMailTo, szSubject, szBody);
}

void RxQualMonitor_Reset()
{
	s_belowCount   = 0;
	s_cooldownLeft = 0;
	s_seenSignal   = FALSE;	// FIX [RxQualZero]
}

void RxQualMonitor_OnTimer()
{
	if (!Profile.bRxQualAlertEnabled)
	{
		RxQualMonitor_Reset();
		return;
	}
	if (Profile.szMailHost[0] == '\0') return;

	if (s_cooldownLeft > 0) { s_cooldownLeft--; return; }

	// FIX [HealthSource]: one snapshot of the active source per tick.
	// For the telnet-penalty source, cold start reads 99.5 (unmeasured) instead of 0.0; the
	// gate below keeps cold start silent for both sources. It must NOT fire once s_seenSignal
	// is armed: with the needle source a live receiver decoding pure noise reads 0.0 again
	// (Health_HasData()==FALSE) and that is exactly the dead/noise case [RxQualZero] must count.
	double dQuality = Health_GetScore();
	if (!Health_HasData() && !s_seenSignal) return;	// no measurement yet (cold start)

	// FIX [RxQualZero]: arm 0%-alerting once the receiver has proven it can produce a healthy
	// reading. Before that we treat 0.0 as "no data yet" (cold start) and do not count it.
	if (dQuality >= (double)Profile.nRxQualRecover)
		s_seenSignal = TRUE;

	// FIX [RxQualZero]: below-threshold now INCLUDES 0% (dead/noise receiver), but only after a
	// healthy reading has been seen — so a live receiver decoding pure noise (0%) finally alerts.
	if (dQuality < (double)Profile.nRxQualThreshold && (s_seenSignal || dQuality > 0.0))
	{
		s_belowCount++;
		if (s_belowCount >= Profile.nRxQualMinutes)
		{
			SendRxQualAlert();
			s_belowCount   = 0;
			s_cooldownLeft = Profile.nRxQualCooldown;
		}
	}
	else if (dQuality >= (double)Profile.nRxQualRecover)
	{
		s_belowCount = 0;
	}
	// Between threshold and recover: hold counter (hysteresis zone — no reset)
}

// ---------------------------------------------------------------------------
// FIX [ComLinkAlert]: serial-input (COM) link-lost alert.
// The RX-quality alert above watches DECODE quality; on a COM/Moxa dropout the
// needle-source score simply freezes at its last value (dRX_Quality is only
// recomputed on decoded data, Misc.cpp CountBiterrors) so a total loss of serial
// data is invisible to it. This alert instead watches the physical link state
// (Rs232LinkState(): 2 = receiving, 1 = open-but-no-data/stalled, 0 = not open)
// and mails when the link stops receiving for nComLinkMinutes consecutive minutes.
// Runs off the same 60 s RXQUAL_TIMER tick, so one tick == one minute.
// ---------------------------------------------------------------------------
EXTERN_C int Rs232LinkState(void);	// utils/rs232.cpp (C linkage, matches rs232.h; avoids pulling slicer.h/rs232.h here)

static int s_comBelow    = 0;
static int s_comCooldown = 0;

static void SendComLinkAlert(int link)
{
	if (Profile.szMailHost[0]     == '\0') return;
	if (Profile.szRxQualMailTo[0] == '\0') return;

	char szComputer[64] = {0};
	DWORD dwSz = sizeof(szComputer);
	GetComputerNameA(szComputer, &dwSz);
	SYSTEMTIME st;
	GetLocalTime(&st);

	char szSubject[128];
	_snprintf_s(szSubject, sizeof(szSubject), _TRUNCATE,
		"PDW - COM Link Alert (no serial data)");

	char szBody[512];
	_snprintf_s(szBody, sizeof(szBody), _TRUNCATE,
		"The serial input (COM port) has stopped receiving data.\r\n"
		"\r\n"
		"Link state      : %s\r\n"
		"No data for     : %d minute(s)\r\n"
		"Host            : %s\r\n"
		"Time            : %04d-%02d-%02d %02d:%02d:%02d",
		(link == 0) ? "port not open" : "open, but no data coming in",
		Profile.nComLinkMinutes,
		szComputer,
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

	QueueAlertMail(Profile.szRxQualMailTo, szSubject, szBody);
}

void ComLinkMonitor_Reset()
{
	s_comBelow    = 0;
	s_comCooldown = 0;
}

void ComLinkMonitor_OnTimer()
{
	// Only meaningful when serial input is actually the source. If the COM port
	// is disabled (sound-card input, or no input configured) there is no link to
	// watch, so stay silent and keep the counters armed for a later enable.
	if (!Profile.bComLinkAlertEnabled || !Profile.comPortEnabled)
	{
		ComLinkMonitor_Reset();
		return;
	}
	if (Profile.szMailHost[0] == '\0') return;

	if (s_comCooldown > 0) { s_comCooldown--; return; }

	int link = Rs232LinkState();	// 2 = receiving, 1 = stalled/open-no-data, 0 = not open
	if (link != 2)					// anything but "receiving" counts as the link being down
	{
		s_comBelow++;
		if (s_comBelow >= Profile.nComLinkMinutes)
		{
			SendComLinkAlert(link);
			s_comBelow    = 0;
			s_comCooldown = Profile.nComLinkCooldown;
		}
	}
	else
	{
		s_comBelow = 0;	// link recovered - re-arm
	}
}
