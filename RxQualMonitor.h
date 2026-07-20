#pragma once
// FIX [RxQualAlert]: timer-based RX Quality alert monitor
#include <windows.h>

void RxQualMonitor_OnTimer();  // call from WM_TIMER every 60 seconds
void RxQualMonitor_Reset();    // call on profile change or app start

// FIX [ComLinkAlert]: serial-input (COM) link-lost alert, same 60 s timer tick.
void ComLinkMonitor_OnTimer(); // call from WM_TIMER every 60 seconds
void ComLinkMonitor_Reset();   // call on profile change or app start
