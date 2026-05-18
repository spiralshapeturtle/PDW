// utils/winrt_toast.cpp
// FIX [WinRTToast]: WinRT Toast API implementatie voor PDW
//   Alle runtimeobject.dll-symbolen worden via LoadLibrary/GetProcAddress geladen
//   zodat PDW normaal opstart op Windows 7 en ouder (dll ontbreekt daar).

#ifndef STRICT
#define STRICT 1
#endif
#include <windows.h>
#include <stdio.h>   // FIX [WinRTToast]: swprintf_s declaratie
#include <wchar.h>   // FIX [WinRTToast]: wcslen / wide-char declaraties

// WRL ComPtr — pure templates, geen runtimeobject.lib-afhankelijkheid
#include <wrl/client.h>

// WinRT ABI interface-definities (alleen typedefs/vtables, geen #pragma comment-libs)
#include <windows.ui.notifications.h>
#include <windows.data.xml.dom.h>
#include <activation.h>

#include "..\utils\winrt_toast.h"

using Microsoft::WRL::ComPtr;
using namespace ABI::Windows::UI::Notifications;
using namespace ABI::Windows::Data::Xml::Dom;

bool g_bWinRTAvail = false;

// FIX [WinRTToast]: functiepointers voor runtimeobject.dll (dynamisch geladen)
typedef HRESULT (WINAPI *PFN_RoInitialize)          (DWORD initType);
typedef HRESULT (WINAPI *PFN_RoGetActivationFactory)(HSTRING classId, REFIID iid, void** factory);
typedef HRESULT (WINAPI *PFN_WindowsCreateString)   (const wchar_t* src, UINT32 len, HSTRING* out);
typedef HRESULT (WINAPI *PFN_WindowsDeleteString)   (HSTRING hs);

static PFN_RoInitialize           s_pfnRoInitialize           = nullptr;
static PFN_RoGetActivationFactory s_pfnRoGetActivationFactory = nullptr;
static PFN_WindowsCreateString    s_pfnWindowsCreateString    = nullptr;
static PFN_WindowsDeleteString    s_pfnWindowsDeleteString    = nullptr;
static HMODULE                    s_hRoRuntime                = nullptr;

static HSTRING MakeHStr(const wchar_t* str)
{
    HSTRING hs = nullptr;
    if (s_pfnWindowsCreateString && str)
        s_pfnWindowsCreateString(str, (UINT32)wcslen(str), &hs);
    return hs;
}

static void FreeHStr(HSTRING hs)
{
    if (s_pfnWindowsDeleteString && hs)
        s_pfnWindowsDeleteString(hs);
}

// FIX [WinRTToast]: escapen van & < > " voor inbedding in toast-XML
static void XmlEscape(const wchar_t* src, wchar_t* dst, int dstLen)
{
    int di = 0;
    for (int si = 0; src[si] && di < dstLen - 7; si++)
    {
        wchar_t c = src[si];
        if      (c == L'&') { dst[di++]=L'&';dst[di++]=L'a';dst[di++]=L'm';dst[di++]=L'p';dst[di++]=L';'; }
        else if (c == L'<') { dst[di++]=L'&';dst[di++]=L'l';dst[di++]=L't';dst[di++]=L';'; }
        else if (c == L'>') { dst[di++]=L'&';dst[di++]=L'g';dst[di++]=L't';dst[di++]=L';'; }
        else if (c == L'"') { dst[di++]=L'&';dst[di++]=L'q';dst[di++]=L'u';dst[di++]=L'o';dst[di++]=L't';dst[di++]=L';'; }
        else                { dst[di++] = c; }
    }
    dst[di] = L'\0';
}

bool WinRTToastInit()
{
    // FIX [WinRTToast]: dynamisch laden — XP/Win7 hebben geen runtimeobject.dll
    s_hRoRuntime = LoadLibraryW(L"runtimeobject.dll");
    if (!s_hRoRuntime) return false;

    s_pfnRoInitialize           = (PFN_RoInitialize)          GetProcAddress(s_hRoRuntime, "RoInitialize");
    s_pfnRoGetActivationFactory = (PFN_RoGetActivationFactory)GetProcAddress(s_hRoRuntime, "RoGetActivationFactory");
    s_pfnWindowsCreateString    = (PFN_WindowsCreateString)   GetProcAddress(s_hRoRuntime, "WindowsCreateString");
    s_pfnWindowsDeleteString    = (PFN_WindowsDeleteString)   GetProcAddress(s_hRoRuntime, "WindowsDeleteString");

    if (!s_pfnRoInitialize || !s_pfnRoGetActivationFactory ||
        !s_pfnWindowsCreateString || !s_pfnWindowsDeleteString)
    {
        FreeLibrary(s_hRoRuntime);
        s_hRoRuntime = nullptr;
        return false;
    }

    // FIX [WinRTToast]: RO_INIT_MULTITHREADED=1; 0x80010106=al als STA geïnitialiseerd (acceptabel)
    HRESULT hr = s_pfnRoInitialize(1 /*RO_INIT_MULTITHREADED*/);
    if (FAILED(hr) && hr != (HRESULT)0x80010106)
    {
        FreeLibrary(s_hRoRuntime);
        s_hRoRuntime = nullptr;
        return false;
    }

    // FIX [WinRTToast]: AUMID registreren in HKCU — geen admin vereist; Windows toont DisplayName in Action Center
    wchar_t szExe[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, szExe, _countof(szExe));

    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
            L"Software\\Classes\\AppUserModelId\\PDW.PagingDecoder",
            0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS
        && hKey)
    {
        static const wchar_t szName[] = L"PDW";
        RegSetValueExW(hKey, L"DisplayName", 0, REG_SZ,
                       (const BYTE*)szName, (DWORD)sizeof(szName));
        wchar_t szIcon[MAX_PATH + 4] = {0};
        swprintf_s(szIcon, _countof(szIcon), L"%s,0", szExe);
        RegSetValueExW(hKey, L"IconUri", 0, REG_SZ,
                       (const BYTE*)szIcon, (DWORD)((wcslen(szIcon) + 1) * sizeof(wchar_t)));
        // FIX [WinRTToast]: ShowInSettings=1 zodat PDW als app verschijnt in Windows Settings → Notificaties
        DWORD dwShow = 1;
        RegSetValueExW(hKey, L"ShowInSettings", 0, REG_DWORD,
                       (const BYTE*)&dwShow, sizeof(dwShow));
        RegCloseKey(hKey);
    }

    // FIX [WinRTToast]: CRITICAL — claim de AUMID voor het lopende proces zodat Windows toasts
    //   correct kan associëren met de registry-metadata en ze in Action Center kan persisteren.
    //   Zonder deze call wordt de toast banner wel getoond, maar verdwijnt direct uit Action Center
    //   omdat de proces-AUMID (heuristisch op exe-pad) niet matched met PDW.PagingDecoder.
    //   shell32.dll is altijd geladen; SetCurrentProcessExplicitAppUserModelID bestaat sinds Win7
    //   — voor Vista/XP-veiligheid via GetProcAddress geladen.
    typedef HRESULT (WINAPI *PFN_SetCurrentProcessExplicitAppUserModelID)(PCWSTR);
    HMODULE hShell32 = GetModuleHandleW(L"shell32.dll");
    if (hShell32)
    {
        PFN_SetCurrentProcessExplicitAppUserModelID pfnSetAumid =
            (PFN_SetCurrentProcessExplicitAppUserModelID)GetProcAddress(hShell32, "SetCurrentProcessExplicitAppUserModelID");
        if (pfnSetAumid) pfnSetAumid(L"PDW.PagingDecoder");
    }

    // FIX [WinRTToast]: factory-test — valideert dat ToastNotificationManager aanwezig en werkzaam is
    HSTRING hstrMgr = MakeHStr(L"Windows.UI.Notifications.ToastNotificationManager");
    ComPtr<IToastNotificationManagerStatics> pMgr;
    hr = s_pfnRoGetActivationFactory(hstrMgr, IID_PPV_ARGS(&pMgr));
    FreeHStr(hstrMgr);

    if (FAILED(hr) || !pMgr)
    {
        FreeLibrary(s_hRoRuntime);
        s_hRoRuntime = nullptr;
        return false;
    }

    g_bWinRTAvail = true;
    return true;
}

void WinRTToastNotify(const char* title, const char* body)
{
    if (!g_bWinRTAvail) return;

    // FIX [WinRTToast]: CP_ACP → wide; lege-string guard
    wchar_t wTitle[256]  = {0};
    wchar_t wBody [2048] = {0};
    MultiByteToWideChar(CP_ACP, 0, title ? title : "", -1, wTitle, _countof(wTitle) - 1);
    MultiByteToWideChar(CP_ACP, 0, body  ? body  : "", -1, wBody,  _countof(wBody)  - 1);

    wchar_t wTitleEsc[512]  = {0};
    wchar_t wBodyEsc [4096] = {0};
    XmlEscape(wTitle, wTitleEsc, _countof(wTitleEsc));
    XmlEscape(wBody,  wBodyEsc,  _countof(wBodyEsc));

    wchar_t szXml[5120] = {0};
    swprintf_s(szXml, _countof(szXml),
        L"<toast>"
        L"<visual><binding template=\"ToastGeneric\">"
        L"<text>%s</text>"
        L"<text>%s</text>"
        L"</binding></visual>"
        L"</toast>",
        wTitleEsc, wBodyEsc);

    HRESULT hr;

    // FIX [WinRTToast]: IXmlDocument aanmaken via IActivationFactory
    HSTRING hstrXmlDocClass = MakeHStr(L"Windows.Data.Xml.Dom.XmlDocument");
    ComPtr<IActivationFactory> pXmlDocFactory;
    hr = s_pfnRoGetActivationFactory(hstrXmlDocClass, IID_PPV_ARGS(&pXmlDocFactory));
    FreeHStr(hstrXmlDocClass);
    if (FAILED(hr) || !pXmlDocFactory) return;

    ComPtr<IInspectable> pXmlDocInsp;
    if (FAILED(pXmlDocFactory->ActivateInstance(&pXmlDocInsp)) || !pXmlDocInsp) return;

    ComPtr<IXmlDocumentIO> pXmlDocIO;
    if (FAILED(pXmlDocInsp.As(&pXmlDocIO)) || !pXmlDocIO) return;

    HSTRING hstrXml = MakeHStr(szXml);
    hr = pXmlDocIO->LoadXml(hstrXml);
    FreeHStr(hstrXml);
    if (FAILED(hr)) return;

    ComPtr<IXmlDocument> pXmlDoc;
    if (FAILED(pXmlDocInsp.As(&pXmlDoc)) || !pXmlDoc) return;

    // FIX [WinRTToast]: ToastNotifier aanmaken voor AUMID PDW.PagingDecoder
    HSTRING hstrMgr = MakeHStr(L"Windows.UI.Notifications.ToastNotificationManager");
    ComPtr<IToastNotificationManagerStatics> pMgr;
    hr = s_pfnRoGetActivationFactory(hstrMgr, IID_PPV_ARGS(&pMgr));
    FreeHStr(hstrMgr);
    if (FAILED(hr) || !pMgr) return;

    HSTRING hstrAumid = MakeHStr(L"PDW.PagingDecoder");
    ComPtr<IToastNotifier> pNotifier;
    hr = pMgr->CreateToastNotifierWithId(hstrAumid, &pNotifier);
    FreeHStr(hstrAumid);
    if (FAILED(hr) || !pNotifier) return;

    // FIX [WinRTToast]: ToastNotification aanmaken en weergeven
    HSTRING hstrNotifClass = MakeHStr(L"Windows.UI.Notifications.ToastNotification");
    ComPtr<IToastNotificationFactory> pNotifFactory;
    hr = s_pfnRoGetActivationFactory(hstrNotifClass, IID_PPV_ARGS(&pNotifFactory));
    FreeHStr(hstrNotifClass);
    if (FAILED(hr) || !pNotifFactory) return;

    ComPtr<IToastNotification> pNotif;
    if (FAILED(pNotifFactory->CreateToastNotification(pXmlDoc.Get(), &pNotif)) || !pNotif) return;

    pNotifier->Show(pNotif.Get());
}
