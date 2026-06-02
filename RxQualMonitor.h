#pragma once
// FIX [RxQualAlert]: timer-based RX Quality alert monitor
#include <windows.h>

void RxQualMonitor_OnTimer();  // call from WM_TIMER every 60 seconds
void RxQualMonitor_Reset();    // call on profile change or app start
