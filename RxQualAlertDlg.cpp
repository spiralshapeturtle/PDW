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

static void SetControlsEnabled(HWND hDlg, BOOL bEnable)
{
	EnableWindow(GetDlgItem(hDlg, IDC_RXQA_MAILTO), bEnable);
	EnableWindow(GetDlgItem(hDlg, IDC_RXQA_THR),    bEnable);
	EnableWindow(GetDlgItem(hDlg, IDC_RXQA_REC),    bEnable);
	EnableWindow(GetDlgItem(hDlg, IDC_RXQA_MIN),    bEnable);
	EnableWindow(GetDlgItem(hDlg, IDC_RXQA_COOL),   bEnable);
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

		// Show or hide SMTP warning depending on whether a mail host is configured
		bool bHasSmtp = (Profile.szMailHost[0] != '\0');
		ShowWindow(GetDlgItem(hDlg, IDC_RXQA_WARN), bHasSmtp ? SW_HIDE : SW_SHOW);
		if (!bHasSmtp)
		{
			CheckDlgButton(hDlg, IDC_RXQA_EN, BST_UNCHECKED);
			EnableWindow(GetDlgItem(hDlg, IDC_RXQA_EN), FALSE);
		}
		SetControlsEnabled(hDlg, bHasSmtp && Profile.bRxQualAlertEnabled ? TRUE : FALSE);
		CenterWindow(hDlg);
		return TRUE;
	}

	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDC_RXQA_EN:
			if (HIWORD(wParam) == BN_CLICKED)
			{
				BOOL bChecked = IsDlgButtonChecked(hDlg, IDC_RXQA_EN) == BST_CHECKED;
				SetControlsEnabled(hDlg, bChecked);
			}
			break;

		case IDOK:
		{
			GetDlgItemText(hDlg, IDC_RXQA_THR,  szBuf, sizeof(szBuf)); int thr  = atoi(szBuf);
			GetDlgItemText(hDlg, IDC_RXQA_REC,  szBuf, sizeof(szBuf)); int rec  = atoi(szBuf);
			GetDlgItemText(hDlg, IDC_RXQA_MIN,  szBuf, sizeof(szBuf)); int mins = atoi(szBuf);
			GetDlgItemText(hDlg, IDC_RXQA_COOL, szBuf, sizeof(szBuf)); int cool = atoi(szBuf);

			if (thr < 1 || thr > 99)
			{
				MessageBox(hDlg, "Alert threshold must be between 1 and 99.", "RX Quality Alert", MB_ICONEXCLAMATION | MB_OK);
				SetFocus(GetDlgItem(hDlg, IDC_RXQA_THR));
				return TRUE;
			}
			if (rec <= thr || rec > 99)
			{
				MessageBox(hDlg, "Recovery threshold must be higher than alert threshold and at most 99.", "RX Quality Alert", MB_ICONEXCLAMATION | MB_OK);
				SetFocus(GetDlgItem(hDlg, IDC_RXQA_REC));
				return TRUE;
			}
			if (mins < 1 || mins > 120)
			{
				MessageBox(hDlg, "Minutes below threshold must be between 1 and 120.", "RX Quality Alert", MB_ICONEXCLAMATION | MB_OK);
				SetFocus(GetDlgItem(hDlg, IDC_RXQA_MIN));
				return TRUE;
			}
			if (cool < 1 || cool > 1440)
			{
				MessageBox(hDlg, "Cooldown must be between 1 and 1440 minutes.", "RX Quality Alert", MB_ICONEXCLAMATION | MB_OK);
				SetFocus(GetDlgItem(hDlg, IDC_RXQA_COOL));
				return TRUE;
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
				MessageBox(hDlg, msg.c_str(), "RX Quality Alert", MB_ICONEXCLAMATION | MB_OK);
				SetFocus(GetDlgItem(hDlg, IDC_RXQA_MAILTO));
				return TRUE;
			}

			Profile.bRxQualAlertEnabled = IsDlgButtonChecked(hDlg, IDC_RXQA_EN) == BST_CHECKED;
			strncpy(Profile.szRxQualMailTo, szMailTo, sizeof(Profile.szRxQualMailTo) - 1);
			Profile.szRxQualMailTo[sizeof(Profile.szRxQualMailTo) - 1] = '\0';
			Profile.nRxQualThreshold = thr;
			Profile.nRxQualRecover   = rec;
			Profile.nRxQualMinutes   = mins;
			Profile.nRxQualCooldown  = cool;

			WriteSettings();
			RxQualMonitor_Reset();
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
