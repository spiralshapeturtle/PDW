/***************************************************
 * Filename : Ostype.cpp
 * Purpose  : OS version detection via RtlGetVersion() — bypasses the
 *            AppCompat shim that GetVersionEx() uses since Windows 8.1,
 *            which would return fabricated values (always "6.2") without
 *            a SupportedOS manifest GUID.
 *********************************************************************/

#include <windows.h>
#include <winternl.h>
#include <tchar.h>

#include "debug.h"
#include "ostype.h"

OSVERSIONINFO  OSVersionInfo = { sizeof(OSVERSIONINFO), 0 };
TCHAR          szOSType[128];
int            nOSType;

static void FillVersionViaRtl(DWORD& major, DWORD& minor, DWORD& build, WORD& product)
{
    major = minor = build = 0;
    product = VER_NT_WORKSTATION;

    typedef NTSTATUS (WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (!hNtdll) return;

    RtlGetVersionPtr fn = reinterpret_cast<RtlGetVersionPtr>(
        GetProcAddress(hNtdll, "RtlGetVersion"));
    if (!fn) return;

    OSVERSIONINFOEXW osvi = {};
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    if (fn(reinterpret_cast<PRTL_OSVERSIONINFOW>(&osvi)) == 0 /* STATUS_SUCCESS */) {
        major   = osvi.dwMajorVersion;
        minor   = osvi.dwMinorVersion;
        build   = osvi.dwBuildNumber;
        product = osvi.wProductType;
    }
}

int GetOSType(TCHAR* szOS)
{
    nOSType = OS_UNKNOWN;

    DWORD major, minor, build;
    WORD  product;
    FillVersionViaRtl(major, minor, build, product);

    // Propagate to the global OSVERSIONINFO so the IS_OSWINME macro in
    // OSTYPE.H keeps working (it reads dwMajorVersion / dwMinorVersion directly).
    OSVersionInfo.dwMajorVersion = major;
    OSVersionInfo.dwMinorVersion = minor;
    OSVersionInfo.dwBuildNumber  = build;
    OSVersionInfo.dwPlatformId   = VER_PLATFORM_WIN32_NT;

    bool bServer = (product != VER_NT_WORKSTATION);

    if (major == 10 && minor == 0) {
        nOSType = OS_WINXP;
        if (bServer) {
            if      (build >= 20348) wsprintf(szOSType, TEXT("Windows Server 2022 (Build %lu)"), build);
            else if (build >= 17763) wsprintf(szOSType, TEXT("Windows Server 2019 (Build %lu)"), build);
            else if (build >= 14393) wsprintf(szOSType, TEXT("Windows Server 2016 (Build %lu)"), build);
            else                     wsprintf(szOSType, TEXT("Windows Server 2012 R2+ (Build %lu)"), build);
        } else {
            if (build >= 22000) wsprintf(szOSType, TEXT("Windows 11 (Build %lu)"), build);
            else                wsprintf(szOSType, TEXT("Windows 10 (Build %lu)"), build);
        }
    } else if (major == 6 && minor == 3) {
        nOSType = OS_WINXP;
        wsprintf(szOSType, bServer ? TEXT("Windows Server 2012 R2 (Build %lu)") : TEXT("Windows 8.1 (Build %lu)"), build);
    } else if (major == 6 && minor == 2) {
        nOSType = OS_WINXP;
        wsprintf(szOSType, bServer ? TEXT("Windows Server 2012 (Build %lu)") : TEXT("Windows 8 (Build %lu)"), build);
    } else if (major == 6 && minor == 1) {
        nOSType = OS_WINXP;
        wsprintf(szOSType, bServer ? TEXT("Windows Server 2008 R2 (Build %lu)") : TEXT("Windows 7 (Build %lu)"), build);
    } else if (major == 6 && minor == 0) {
        nOSType = OS_WINXP;
        wsprintf(szOSType, bServer ? TEXT("Windows Server 2008 (Build %lu)") : TEXT("Windows Vista (Build %lu)"), build);
    } else if (major == 5 && minor == 2) {
        nOSType = OS_WIN2000;
        wsprintf(szOSType, TEXT("Windows Server 2003 (Build %lu)"), build);
    } else if (major == 5 && minor == 1) {
        nOSType = OS_WINXP;
        wsprintf(szOSType, TEXT("Windows XP (Build %lu)"), build);
    } else if (major == 5 && minor == 0) {
        nOSType = OS_WIN2000;
        wsprintf(szOSType, TEXT("Windows 2000 (Build %lu)"), build);
    } else if (major > 0) {
        nOSType = OS_WINXP;
        wsprintf(szOSType, TEXT("Windows %lu.%lu (Build %lu)"), major, minor, build);
    } else {
        nOSType = OS_UNKNOWN;
        wsprintf(szOSType, TEXT("Unknown OS"));
    }

    if (szOS) _tcscpy(szOS, szOSType);

    OUTPUTDEBUGMSG((TEXT("OS Type %d String = >%s<"), nOSType, szOSType));
    return nOSType;
}
