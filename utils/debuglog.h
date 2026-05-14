#ifndef PDW_DEBUGLOG_H
#define PDW_DEBUGLOG_H

#ifdef __cplusplus
extern "C" {
#endif

void DebugLogInit(void);
void DebugLogShutdown(void);

// Append a timestamped line to pdw_debug.log when Profile.bDebugLog is enabled.
// Format: "YYYY-MM-DD <Day> HH:MM:SS | <fmt-output>\n"
void DebugLog(const char *fmt, ...);

// Track FLEX cycle by detecting frame-number wraparound (127 -> 0).
// Call once per showframe() entry with the current frame number.
void DebugLogNotifyFrameChange(int currentFrame);

// Returns the current cycle counter (0-14, wraps).
int  DebugLogGetCycle(void);

#ifdef __cplusplus
}
#endif

#endif
