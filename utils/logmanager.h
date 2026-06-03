// logmanager.h — Central log manager for PDW.
//
// Single write path for all subsystem logs: debug, telnet, MQTT, webhook,
// MySQL, SQLite, SMTP.  Optional ring-buffer mode reduces SSD write cycles.
//
// Message logs (monitor/filter) are NOT routed here — Misc.cpp writes those
// directly so the user-configurable column layout is not disturbed.
//
// Thread-safe: callers on any thread, flush worker on its own thread.

#pragma once
#ifndef PDW_LOGMANAGER_H
#define PDW_LOGMANAGER_H

#include <windows.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Log categories — each maps to a distinct output file.
// ---------------------------------------------------------------------------
enum LogCat : uint32_t {
    LC_DEBUG    = 1u << 0,   // {YYMMDD}_pdw_debug.log
    LC_TELNET   = 1u << 1,   // {YYMMDD}_telnet_server.log
    LC_WIRE     = 1u << 2,   // {YYMMDD}_telnet_traffic.log
    LC_MQTT     = 1u << 3,   // {YYMMDD}_mqtt.log
    LC_WEBHOOK  = 1u << 4,   // {YYMMDD}_webhook.log
    LC_MYSQL    = 1u << 5,   // {YYMMDD}_mysql.log  (daily rotation)
    LC_SQLITE   = 1u << 6,   // {YYMMDD}_sqlite.log
    LC_SMTP     = 1u << 7,   // {YYMMDD}_mail.log
    LC_ALL      = 0x000000FFu
};

// Maximum length of a single formatted log line (timestamp + tag + message).
#define LM_LINE_MAX 1024

// ---------------------------------------------------------------------------
class LogManager {
public:
    static LogManager& Get();

    // Call once at startup before any Write().
    //   path         — log directory (Profile.LogfilePath or szPath fallback)
    //   enableMask   — bitmask of LogCat values initially enabled
    //   monthNumber  — Profile.MonthNumber: 0 = abbreviated month (JAN/FEB/…),
    //                  1 = numeric month (01/02/…)
    //   bufEnabled   — true = ring-buffer mode (reduces disk writes)
    //   flushMs      — flush interval in ms when bufEnabled (e.g. 2000)
    //   bufSlots     — ring-buffer capacity in entries (e.g. 256)
    void Init(const char* path, uint32_t enableMask, int monthNumber,
              bool bufEnabled, DWORD flushMs, int bufSlots);

    // Re-apply settings after user changes the log dialog.
    // Flushes the current buffer before switching.
    void Reconfigure(const char* path, uint32_t enableMask, int monthNumber,
                     bool bufEnabled, DWORD flushMs, int bufSlots);

    // Flush remaining entries and stop the worker thread.  Safe to call multiple times.
    void Shutdown();

    // Write a log line with ISO timestamp: "YYYY-MM-DD HH:MM:SS message\n"
    // Category tag is NOT included in the line (keeps existing log file format).
    void Write(LogCat cat, const char* fmt, ...);

    // Write a pre-formatted line exactly as supplied (caller must include '\n').
    // Used for wire-format lines that already carry their own content structure.
    void WriteRaw(LogCat cat, const char* line, int len = -1);

    // Write a pre-formatted line to an explicit file path (no timestamp added).
    // Used by monitor/filter/sepfiles/groupcall logs where format and path are
    // fully determined by the caller.  Respects the ring-buffer if enabled.
    void WriteLineTo(const char* path, const char* line, int len = -1);

    // Enable / disable a category without a full Reconfigure.
    void SetEnabled(LogCat cat, bool on);
    bool IsEnabled(LogCat cat) const;

    // Force an immediate flush (e.g. before writing a critical event on WM_CLOSE).
    void Flush();

private:
    LogManager();
    ~LogManager();
    LogManager(const LogManager&) = delete;
    LogManager& operator=(const LogManager&) = delete;

    struct Entry {
        char path[MAX_PATH];     // destination file path (computed at write time)
        char line[LM_LINE_MAX];  // formatted line including '\n'
        int  lineLen;
    };

    // Ring buffer state (protected by m_cs)
    Entry*   m_buf;
    int      m_slots;
    int      m_head;
    int      m_tail;
    int      m_count;

    // Drain buffer — only accessed by the worker thread
    Entry*   m_drain;

    char     m_path[MAX_PATH];
    uint32_t m_enableMask;
    int      m_monthNumber;
    bool     m_bufEnabled;
    DWORD    m_flushMs;

    CRITICAL_SECTION m_cs;
    HANDLE   m_hEvent;          // auto-reset, signaled when entries are pushed
    HANDLE   m_hThread;
    volatile bool m_stop;
    bool     m_initialized;

    // Push a pre-built line + path into the ring buffer (or write directly).
    void Emit(const char* path, const char* line, int len);

    // Build the file path for a category using the given time.
    void BuildPath(char* out, int outSize, LogCat cat, const SYSTEMTIME& st) const;

    // Drain all buffered entries to disk.  Must be called only from the worker thread.
    void DrainAll();

    // Write a batch of entries to disk (opens/closes file per unique path group).
    void WriteEntries(Entry* entries, int count);


    static DWORD WINAPI WorkerProc(LPVOID param);
    void WorkerLoop();
};

// ---------------------------------------------------------------------------
// Drop-in replacement macros for existing subsystem log calls.
// Replace: WriteLog("fmt", args)  with  PDW_MQTTLOG("fmt", args)  etc.
// ---------------------------------------------------------------------------
#define PDW_DLOG(fmt, ...)       LogManager::Get().Write(LC_DEBUG,   fmt, ##__VA_ARGS__)
#define PDW_TSLOG(fmt, ...)      LogManager::Get().Write(LC_TELNET,  fmt, ##__VA_ARGS__)
#define PDW_WIRELOG(line)        LogManager::Get().WriteRaw(LC_WIRE, (line))
#define PDW_MQTTLOG(fmt, ...)    LogManager::Get().Write(LC_MQTT,    fmt, ##__VA_ARGS__)
#define PDW_WEBHOOKLOG(fmt, ...) LogManager::Get().Write(LC_WEBHOOK, fmt, ##__VA_ARGS__)
#define PDW_MYSQLLOG(fmt, ...)   LogManager::Get().Write(LC_MYSQL,   fmt, ##__VA_ARGS__)
#define PDW_SQLITELOG(fmt, ...)  LogManager::Get().Write(LC_SQLITE,  fmt, ##__VA_ARGS__)
#define PDW_SMTPLOG(fmt, ...)    LogManager::Get().Write(LC_SMTP,    fmt, ##__VA_ARGS__)

#endif // PDW_LOGMANAGER_H
