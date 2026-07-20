// FIX [RxQualAlert]: dialog procedure for RX Quality Alert settings
#include <windows.h>
#include "Headers/pdw.h"
#include "Headers/Resource.h"
#include "RxQualAlertDlg.h"
#include "RxQualMonitor.h"
#include <string>
#include <sstream>
#include <vector>

extern PROFILE Profile;

// FIX [ComLinkAlert]: two independent alerts share one recipient list. The
// recipient box is enabled whenever EITHER alert is on; each alert's own fields
// follow only its own checkbox. All gated behind a configured SMTP host.
static void RefreshEnable(HWND hDlg)
{
	BOOL bSmtp = (Profile.szMailHost[0] != '\0');
	BOOL bRx   = bSmtp && (IsDlgButtonChecked(hDlg, IDC_RXQA_EN)     == BST_CHECKED);
	BOOL bCom  = bSmtp && (IsDlgButtonChecked(hDlg, IDC_RXQA_COM_EN) == BST_CHECKED);

	EnableWindow(GetDlgItem(hDlg, IDC_RXQA_MAILTO),   bRx || bCom);	// shared recipient

	EnableWindow(GetDlgItem(hDlg, IDC_RXQA_THR),      bRx);
	EnableWindow(GetDlgItem(hDlg, IDC_RXQA_REC),      bRx);
	EnableWindow(GetDlgItem(hDlg, IDC_RXQA_MIN),      bRx);
	EnableWindow(GetDlgItem(hDlg, IDC_RXQA_COOL),     bRx);

	EnableWindow(GetDlgItem(hDlg, IDC_RXQA_COM_MIN),  bCom);
	EnableWindow(GetDlgItem(hDlg, IDC_RXQA_COM_COOL), bCom);
}

BOOL FAR PASCAL RxQualAlertDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam) // FIX [RxQualAlert]
{
	char szBuf[32];

	switch (uMsg)
	{
	case WM_INITDIALOG:
	{
		CheckDlgButton(hDlg, IDC_RXQA_EN, Profile.bRxQualAlertEnabled ? BST_CHECKED : BST_UNCHECKED);
		SetDlgItemText(hDlg, IDC_RXQA_MAILTO, Profile.szRxQualMailTo);
		sprintf(szBuf, "%d", Profile.nRxQualThreshold); SetDlgItemText(hDlg, IDC_RXQA_THR,  szBuf);
		sprintf(szBuf, "%d", Profile.nRxQualRecover);   SetDlgItemText(hDlg, IDC_RXQA_REC,  szBuf);
		sprintf(szBuf, "%d", Profile.nRxQualMinutes);   SetDlgItemText(hDlg, IDC_RXQA_MIN,  szBuf);
		sprintf(szBuf, "%d", Profile.nRxQualCooldown);  SetDlgItemText(hDlg, IDC_RXQA_COOL, szBuf);

		// FIX [HealthSparkThreshold]: display option, not an alert - never disabled/gated on SMTP
		CheckDlgButton(hDlg, IDC_RXQA_THRLINE, Profile.nHealthThreshLine ? BST_CHECKED : BST_UNCHECKED);

		// FIX [ComLinkAlert]: COM link-lost alert fields
		CheckDlgButton(hDlg, IDC_RXQA_COM_EN, Profile.bComLinkAlertEnabled ? BST_CHECKED : BST_UNCHECKED);
		sprintf(szBuf, "%d", Profile.nComLinkMinutes);  SetDlgItemText(hDlg, IDC_RXQA_COM_MIN,  szBuf);
		sprintf(szBuf, "%d", Profile.nComLinkCooldown); SetDlgItemText(hDlg, IDC_RXQA_COM_COOL, szBuf);

		// Show or hide SMTP warning depending on whether a mail host is configured
		bool bHasSmtp = (Profile.szMailHost[0] != '\0');
		ShowWindow(GetDlgItem(hDlg, IDC_RXQA_WARN), bHasSmtp ? SW_HIDE : SW_SHOW);
		if (!bHasSmtp)
		{
			CheckDlgButton(hDlg, IDC_RXQA_EN, BST_UNCHECKED);
			CheckDlgButton(hDlg, IDC_RXQA_COM_EN, BST_UNCHECKED);
			EnableWindow(GetDlgItem(hDlg, IDC_RXQA_EN), FALSE);
			EnableWindow(GetDlgItem(hDlg, IDC_RXQA_COM_EN), FALSE);
		}
		RefreshEnable(hDlg);
		CenterWindow(hDlg);
		return TRUE;
	}

	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDC_RXQA_EN:
		case IDC_RXQA_COM_EN:		// FIX [ComLinkAlert]
			if (HIWORD(wParam) == BN_CLICKED)
				RefreshEnable(hDlg);
			break;

		case IDOK:
		{
			BOOL bRxEn  = IsDlgButtonChecked(hDlg, IDC_RXQA_EN)     == BST_CHECKED;
			BOOL bComEn = IsDlgButtonChecked(hDlg, IDC_RXQA_COM_EN) == BST_CHECKED;

			GetDlgItemText(hDlg, IDC_RXQA_THR,  szBuf, sizeof(szBuf)); int thr  = atoi(szBuf);
			GetDlgItemText(hDlg, IDC_RXQA_REC,  szBuf, sizeof(szBuf)); int rec  = atoi(szBuf);
			GetDlgItemText(hDlg, IDC_RXQA_MIN,  szBuf, sizeof(szBuf)); int mins = atoi(szBuf);
			GetDlgItemText(hDlg, IDC_RXQA_COOL, szBuf, sizeof(szBuf)); int cool = atoi(szBuf);

			if (thr < 1 || thr > 99)
			{
				MessageBox(hDlg, "Alert threshold must be between 1 and 99.", "System Alerts", MB_ICONEXCLAMATION | MB_OK);
				SetFocus(GetDlgItem(hDlg, IDC_RXQA_THR));
				return TRUE;
			}
			if (rec <= thr || rec > 99)
			{
				MessageBox(hDlg, "Recovery threshold must be higher than alert threshold and at most 99.", "System Alerts", MB_ICONEXCLAMATION | MB_OK);
				SetFocus(GetDlgItem(hDlg, IDC_RXQA_REC));
				return TRUE;
			}
			if (mins < 1 || mins > 120)
			{
				MessageBox(hDlg, "Minutes below threshold must be between 1 and 120.", "System Alerts", MB_ICONEXCLAMATION | MB_OK);
				SetFocus(GetDlgItem(hDlg, IDC_RXQA_MIN));
				return TRUE;
			}
			if (cool < 1 || cool > 1440)
			{
				MessageBox(hDlg, "Cooldown must be between 1 and 1440 minutes.", "System Alerts", MB_ICONEXCLAMATION | MB_OK);
				SetFocus(GetDlgItem(hDlg, IDC_RXQA_COOL));
				return TRUE;
			}

			// FIX [ComLinkAlert]: COM link-lost alert fields
			GetDlgItemText(hDlg, IDC_RXQA_COM_MIN,  szBuf, sizeof(szBuf)); int comMin  = atoi(szBuf);
			GetDlgItemText(hDlg, IDC_RXQA_COM_COOL, szBuf, sizeof(szBuf)); int comCool = atoi(szBuf);
			if (comMin < 1 || comMin > 1440)
			{
				MessageBox(hDlg, "COM: minutes without data must be between 1 and 1440.", "System Alerts", MB_ICONEXCLAMATION | MB_OK);
				SetFocus(GetDlgItem(hDlg, IDC_RXQA_COM_MIN));
				return TRUE;
			}
			if (comCool < 1 || comCool > 1440)
			{
				MessageBox(hDlg, "COM: cooldown must be between 1 and 1440 minutes.", "System Alerts", MB_ICONEXCLAMATION | MB_OK);
				SetFocus(GetDlgItem(hDlg, IDC_RXQA_COM_COOL));
				return TRUE;
			}
			// A recipient is required if either alert is on (both mail to the same list).
			if (bRxEn || bComEn)
			{
				char szCheck[512];
				GetDlgItemText(hDlg, IDC_RXQA_MAILTO, szCheck, sizeof(szCheck));
				if (szCheck[0] == '\0')
				{
					MessageBox(hDlg, "Enter at least one mail recipient for the enabled alert(s).", "System Alerts", MB_ICONEXCLAMATION | MB_OK);
					SetFocus(GetDlgItem(hDlg, IDC_RXQA_MAILTO));
					return TRUE;
				}
			}

			// Validate recipient addresses
			char szMailTo[512];
			GetDlgItemText(hDlg, IDC_RXQA_MAILTO, szMailTo, sizeof(szMailTo));

			std::string bad;
			std::istringstream ss(szMailTo);
			std::string token;
			while (std::getline(ss, token, ';'))
			{
				// trim whitespace
				size_t s = token.find_first_not_of(" \t\r\n");
				size_t e = token.find_last_not_of(" \t\r\n");
				if (s == std::string::npos) continue; // skip empty tokens
				std::string addr = token.substr(s, e - s + 1);
				if (addr.find('@') == std::string::npos || addr.find('.') == std::string::npos)
				{
					if (!bad.empty()) bad += "\n";
					bad += addr;
				}
			}
			if (!bad.empty())
			{
				std::string msg = "The following addresses appear invalid:\n" + bad;
				MessageBox(hDlg, msg.c_str(), "System Alerts", MB_ICONEXCLAMATION | MB_OK);
				SetFocus(GetDlgItem(hDlg, IDC_RXQA_MAILTO));
				return TRUE;
			}

			Profile.bRxQualAlertEnabled = bRxEn;
			strncpy(Profile.szRxQualMailTo, szMailTo, sizeof(Profile.szRxQualMailTo) - 1);
			Profile.szRxQualMailTo[sizeof(Profile.szRxQualMailTo) - 1] = '\0';
			Profile.nRxQualThreshold = thr;
			Profile.nRxQualRecover   = rec;
			Profile.nRxQualMinutes   = mins;
			Profile.nRxQualCooldown  = cool;

			// FIX [HealthSparkThreshold]: save the trend-graph threshold-line option
			Profile.nHealthThreshLine = (IsDlgButtonChecked(hDlg, IDC_RXQA_THRLINE) == BST_CHECKED) ? 1 : 0;

			// FIX [ComLinkAlert]: save COM link-lost alert settings
			Profile.bComLinkAlertEnabled = bComEn;
			Profile.nComLinkMinutes      = comMin;
			Profile.nComLinkCooldown     = comCool;

			WriteSettings();
			RxQualMonitor_Reset();
			ComLinkMonitor_Reset();	// FIX [ComLinkAlert]
			EndDialog(hDlg, IDOK);
			break;
		}

		case IDCANCEL:
			EndDialog(hDlg, IDCANCEL);
			break;
		}
		break;
	}
	return FALSE;
}
