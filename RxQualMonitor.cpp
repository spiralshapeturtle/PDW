// FIX [RxQualAlert]: timer-based RX Quality alert monitor
#include <windows.h>
#include "Headers/pdw.h"
#include "utils/smtp.h"
#include "RxQualMonitor.h"

extern PROFILE Profile;
extern double  dRX_Quality;

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

	// Build subject
	char szSubject[128];
	_snprintf_s(szSubject, sizeof(szSubject), _TRUNCATE,
		"PDW - Low RX Quality Alert (%.0f%%)", dRX_Quality);

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
		"Alert threshold : %d%%\r\n"
		"Recovery level  : %d%%\r\n"
		"Host            : %s\r\n"
		"Time            : %04d-%02d-%02d %02d:%02d:%02d",
		dRX_Quality,
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

	// FIX [RxQualZero]: arm 0%-alerting once the receiver has proven it can produce a healthy
	// reading. Before that we treat 0.0 as "no data yet" (cold start) and do not count it.
	if (dRX_Quality >= (double)Profile.nRxQualRecover)
		s_seenSignal = TRUE;

	// FIX [RxQualZero]: below-threshold now INCLUDES 0% (dead/noise receiver), but only after a
	// healthy reading has been seen — so a live receiver decoding pure noise (0%) finally alerts.
	if (dRX_Quality < (double)Profile.nRxQualThreshold && (s_seenSignal || dRX_Quality > 0.0))
	{
		s_belowCount++;
		if (s_belowCount >= Profile.nRxQualMinutes)
		{
			SendRxQualAlert();
			s_belowCount   = 0;
			s_cooldownLeft = Profile.nRxQualCooldown;
		}
	}
	else if (dRX_Quality >= (double)Profile.nRxQualRecover)
	{
		s_belowCount = 0;
	}
	// Between threshold and recover: hold counter (hysteresis zone — no reset)
}
